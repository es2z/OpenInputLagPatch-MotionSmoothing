// Touhou 20: Fossilized Wonders v1.00c

#include <stdio.h>
#include <string.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou20.h"

using namespace Touhou20;

// th20 is built with ASLR enabled, like th19, so the addresses from the disassembly (which all
// assume the default 0x400000 image base) have to be rebased onto the base the executable
// actually got loaded at
static DWORD rebase(DWORD address) {
	static DWORD image_base = (DWORD)GetModuleHandleW(nullptr);
	return image_base + (address - 0x400000);
}

// Like th19, th20 doesn't run its logic once per rendered frame: each update function advances
// the engine's own clock by one frame at a time and only runs the game's logic once that clock
// says a frame has passed. How long a frame is comes from a hardcoded 60fps (0x0056C470 holds
// 60.0), which is read as 1.0 / frame_rate, so disabling the frame limiter alone leaves the
// game running at its original speed no matter what GameFPS is set to.
//
// The addresses below are the 4 byte displacements of the MOVSD/DIVSD instructions that read
// 0x0056C470, so patching them only changes which double the instruction reads. thprac does the
// same thing for th20 (it patches 0x00419EAF, the one in the update path the game actually
// uses).
//
// The game's own frame limiter (0x004193E0) reads the constant as well, but it's disabled
// below, so it doesn't have to be patched.
static const DWORD fps_references[] = {
	0x00419A9C, // Sleep based pacer of the update path at 0x00419A50
	0x00419AD2, // the same pacer, second use of the constant
	0x00419C5A, // frame stepping of the update path at 0x00419C20
	0x00419EAF, // frame stepping of the update path at 0x00419DE0
};

static void patch_fps_reference(DWORD address) {
	DWORD location = rebase(address);

	// Only patch if the instruction still reads the engine's frame rate: an unknown build
	// shouldn't get random data written into the middle of its code. The address in the
	// instruction is an absolute one, so it gets relocated along with the executable and has
	// to be rebased before it can be compared
	DWORD frame_rate = rebase(0x0056C470);
	if (*(DWORD*)location != frame_rate) {
		printf("Frame rate reference at 0x%x doesn't look like a known one (found 0x%x), skipping!\n",
			address, *(DWORD*)location);
		return;
	}

	DWORD value = (DWORD)&Limiter::game_fps;
	patch_bytes(location, &value, sizeof(value));
}

// The game paces its frames itself: once its own clock is more than 1.5ms ahead of the current
// time, it sleeps a single millisecond before it gets any work done. That wakeup is far too
// imprecise to pace a frame with (Sleep(1) rarely returns after exactly one millisecond, so the
// frames end up a millisecond or two out of place), so the call is redirected into the patch's
// limiter instead.
//
// The wait uses exactly what the game itself worked out: right before it calls Sleep, the code
// has the time left until its clock says the next frame is due, multiplied by 1000, sitting in
// XMM0 (that's the "more than 1.5ms" it compared just before). Waiting for that means the limiter
// ends up in phase with the game's clock by construction - no schedule of its own that could
// drift - and the game's clock is the thing that decides how fast the game runs anyway.
//
// This replaces a "push 1 / call Sleep", so the argument isn't pushed anymore and the hook takes
// none.
double th20_frame_wait_ms = 0.0;
void th20_frame_wait_impl();

// Set once the game hands its frame pacing over to the limiter (see the frame interpolation
// below). Until it is, the game is running a frame function that paces itself, which is what
// happens while it presents with vsync on - and then the patch's pacing never gets to run.
static bool th20_limiter_ticked = false;

void __declspec(naked) th20_frame_wait() {
	__asm {
		movsd qword ptr [th20_frame_wait_ms], xmm0
		jmp th20_frame_wait_impl
	}
}

void th20_frame_wait_impl() {
	th20_limiter_ticked = true;
	Limiter::TickUntil(th20_frame_wait_ms / 1000.0);
}

static void patch_frame_wait(DWORD address) {
	BYTE* sequence = (BYTE*)rebase(address);

	// push 1 / call dword ptr [Sleep]
	if (sequence[0] != 0x6A || sequence[1] != 0x01 || sequence[2] != 0xFF || sequence[3] != 0x15) {
		printf("Frame wait at 0x%x doesn't look like a known one, skipping!\n", address);
		return;
	}

	BYTE patch[8] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90 };
	*(DWORD*)(patch + 1) = (DWORD)&th20_frame_wait - ((DWORD)sequence + 5);
	patch_bytes(sequence, patch, sizeof(patch));
}

// thprac changes the frame rate of games like this one by pointing the engine's clock at a
// variable of its own. If that happens, the game and the limiter would disagree about how long
// a frame is - and the replay speed control would lose the clock it drives. So the operand is
// taken back, but the value the other tool put there is followed: tools like thprac are what
// the player configures in game, so their frame rate wins over the configured GameFPS.
static double* external_fps = nullptr;

static UINT th20_external_game_fps() {
	DWORD* operand = (DWORD*)rebase(0x00419EAF);

	if (*operand != (DWORD)&Limiter::game_fps) {
		double* candidate = (double*)*operand;
		if (*candidate > 1.0 && *candidate < 10000.0)
			external_fps = candidate;

		DWORD value = (DWORD)&Limiter::game_fps;
		patch_bytes(operand, &value, sizeof(value));
	}

	if (external_fps == nullptr)
		return 0;

	// thprac writes this whenever its framerate option is applied, so it has to be read back
	// every time instead of being copied once
	double rate = *external_fps;
	if (rate < 1.0 || rate > 10000.0)
		return 0;
	return (UINT)(rate + 0.5);
}

// The frame rate the external tool (thprac) currently has its option set to, for the diagnostics
static double th20_external_rate() {
	return external_fps != nullptr ? *external_fps : 0.0;
}

// Takes over the game's render block and its presents (defined below, after the comment that
// explains what they are)
static void th20_install_interpolation();

void th20_install_patches() {
	{
		// Make the game's frame limiter return immediately, exactly like on th19 - it's called
		// from the update paths that render, and our limiter (ticked from the wait the game
		// itself would sleep in, at 0x00419E66) takes over its job
		BYTE patch[] = { 0xC3 };
		patch_bytes(rebase(0x004193E0), patch, sizeof(patch));
	}
	{
		// The engine starts reading its frame rate long before the limiter is ticked for the
		// first time, so start it off with the configured value
		Limiter::game_fps = (double)Config::GameFPS;

		// Follow thprac (or any other tool) if it takes the engine's frame rate over
		Limiter::external_game_fps = th20_external_game_fps;
	}
	{
		// Run the engine's clock at GameFPS instead of its hardcoded 60
		for (DWORD reference : fps_references)
			patch_fps_reference(reference);

		// Let the limiter do the waiting for the frames instead of the game's own Sleep(1)
		patch_frame_wait(0x00419E66);
	}
	if (Config::D3D9Ex) {
		// Redirect the Direct3DCreate9 call (0x0041C335 calls the d3d9.dll import thunk).
		// The IAT is being hooked as well, but thcrap hooks Direct3DCreate9 in the same place,
		// so this makes sure our hook is the one that gets called
		patch_call(rebase(0x0041C335), Direct3DCreate9_hook);
	}
	if (Config::Interpolation || Config::InterpolationFPS)
		th20_install_interpolation();
}

// The game runs replay playback as its own app state, which is what this checks before it lets
// the replay keys change the framerate: a replay playing back is state 2 with the replay file
// object loaded, the other states are the menus, a normal game and so on.
static const int replay_state = 2;

FPSTarget th20_replay_callback() {
	if (*(int*)rebase(0x005C6128) != replay_state || *(DWORD*)rebase(0x005C60FC) == 0)
		return FPSTarget::Game;

	// While a replay plays back, the input state holds the recorded inputs, which is exactly
	// what the game's own fast forward and slow down go by: the shoot key skips ahead (like on
	// th17 and th18, holding ctrl does nothing in these games) and the focus key slows down
	auto input = *(DWORD*)rebase(0x005B88B4);
	if (input & InputState::Focus)
		return FPSTarget::ReplaySlow;
	else if (input & InputState::Shoot)
		return FPSTarget::ReplaySkip;
	return FPSTarget::Game;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th20 runs on the engine th19 does, so a frame is laid out the same way: the game draws a sprite
// queue, updates every object, draws the objects into the queue and presents the result once. The
// main loop is FUN_0041E7D0, and the frame function it runs is FUN_00419DE0 (the one it picks at
// 0x41EBA3); when bit 2 of [0x5B87E8] is set it runs FUN_00419C20 instead (0x41EB4F), and when
// [0x5C4E58] is 1 and [0x5C4F84] is 0 it runs FUN_0041A030 (0x41EB83). All three hold the same
// frame with a different tail, and all three are taken over below.
//
// A frame walks the game's objects twice. The update walk FUN_00412810 (`mov ecx, [0x5B66D8h]` at
// 0x419EE2; its `ret` is at 0x412A72) moves them, and the render walk FUN_00412AA0 (called at
// 0x419F91; its `ret` is at 0x412CEA) draws them into the sprite queue. Those two `ret`s are what
// thprac's th20 module hooks with its two EHOOK_DY calls (0x12A72 and 0x12CEA, the same addresses
// without the image base): it builds its overlay frame from the first and draws it from the
// second, so they are where the tool's frame is watched from (see tool_state.h).
//
// The render block sits right after the update walk in every frame function
// (0x419F4E..0x419FE3 in FUN_00419DE0, 0x419CF8..0x419D8D in FUN_00419C20 and 0x41A0C9..0x41A15E
// in FUN_0041A030) and is always the same:
//
//   mov ecx, 5C4D40h / call 00412730      the renderer, whose +8 is the D3D9 device (thprac's
//                                         D3D_DEVICE is 0x1C4D48, i.e. that same +8)
//   CALL [device+0xA4]                    BeginScene
//   mov ecx, [005C0028h] / call 00445A40  reset the sprite queue
//   mov dword [005C5AF8h], 0FFh           the flag the block raises
//   mov ecx, 5C4D40h / call 004DDA60      flush the queue if one is pending, then
//                                         SetRenderState(D3DRS_FOGENABLE, 0)
//   mov ecx, [005B66D8h] / call 00412AA0  draw every object, which fills the queue
//   mov ecx, [005C0028h] / call 004455C0  draw the sprite queue
//   CALL [device+0x104]                   SetTexture(0, NULL)
//   CALL [device+0xA8]                    EndScene
//
// so an extra presentation is that block run again, exactly the way th19's extras run th19's
// block. Nothing in the frame functions sets a transform (the menus do, so whatever the scene was
// drawn with is read off the device in the render hook below and put back for every extra
// presentation), and the queue is drawn at the coordinates the objects are queued with, so there
// is no pair of floats to put back the way th18 had.
//
// The presents are laid out identically at all three call sites, 39 bytes each:
//
//   B9 40 4D 5C 00     mov ecx, 5C4D40h    <- the hook goes over these 5 bytes
//   E8 ..              call 00412730       the renderer, which returns the device
//   89 45 ??           mov [ebp+?], eax
//   8B 4? ??           mov ?, [ebp+?]
//   8B 0?              mov ?, [?]          the device's vtable
//   8B 4? 44           mov ?, [?+44h]      Present
//   89 4D/55 ??        mov [ebp+?], ?
//   6A 00 x4           push 0 x4
//   8B 4? ??           mov ?, [ebp+?]
//   50/51/52           push ?
//   FF 55 ??           call [ebp+?]
//
// Every one of the five arguments is pushed inside those 39 bytes, so skipping the whole setup
// leaves the stack the way the game expects, and the hook's return value lands in eax - which is
// exactly where the `test eax, eax` right after the call reads it, to notice a lost device.

// The plain calls the render block makes into the game
static auto th20_reset_sprite_queue = (void(__fastcall*)(void*))rebase(0x00445A40); // ECX = the sprite manager
static auto th20_draw_sprite_queue = (void(__fastcall*)(void*))rebase(0x004455C0);  // ECX = the sprite manager
static auto th20_flush_sprite_queue = (void(__fastcall*)(void*))rebase(0x004DDA60); // ECX = the renderer (0x5C4D40)
static auto th20_render_objects = (void(__fastcall*)(void*))rebase(0x00412AA0);     // ECX = the scene ([0x5B66D8])

// The manager whose queue the block resets and draws, the renderer it asks for the device, the
// scene the render walk draws, and the dword the block raises
static void** th20_sprite_manager = (void**)rebase(0x005C0028);
static void* th20_renderer = (void*)rebase(0x005C4D40);
static void** th20_scene = (void**)rebase(0x005B66D8);
static DWORD* th20_render_flag = (DWORD*)rebase(0x005C5AF8);

// What the scene is drawn with, taken off the device in the render hook (the frame functions never
// set it, but the menus do, so an extra presentation has to put back whatever the scene that was
// just drawn used)
static D3DVIEWPORT9 th20_scene_viewport = {};
static D3DMATRIX th20_scene_view = {};
static D3DMATRIX th20_scene_projection = {};
static bool th20_scene_transforms = false;

// Where the user's tool builds its frame and where it draws it (see the comment above)
static unsigned char* th20_tool_frame_address = (unsigned char*)rebase(0x00412A72);
static unsigned char* th20_tool_draw_address = (unsigned char*)rebase(0x00412CEA);

// The device the frame is drawn with. That is the one the patch hooked, and the game's own device
// is the same object (the renderer's +8, the pointer thprac's D3D_DEVICE names) - so if the
// patch's own D3D9 hook isn't in (Config::D3D9Ex is off), the game's is used instead and the frame
// is still presented, just without the extra presentations.
static IDirect3DDevice9* th20_device() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr && th20_renderer != nullptr)
		device = *(IDirect3DDevice9**)((char*)th20_renderer + 8);
	return device;
}

// Remembers what the scene is about to be drawn with, then lets the game's own render run exactly
// as it always did
static void __fastcall th20_render_hook(void* scene) {
	IDirect3DDevice9* device = th20_device();
	if (device != nullptr) {
		device->GetViewport(&th20_scene_viewport);
		th20_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th20_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th20_scene_projection));
	}

	// The tool's frame is built by now (it runs from the update walk's return, before this) but not
	// drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th20_tool_frame_address, th20_tool_draw_address);

	th20_render_objects(scene);
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
//
// "-1" picks the largest multiple of the game's frame rate that still fits the display refresh
// (a 190hz display and a 60fps game give 3), a positive value is that multiple, and the
// "*<frame rate>" form is that frame rate no matter what the game's own is.
static double th20_present_rate() {
	if (Config::InterpolationFPS)
		return (double)Config::InterpolationFPS;

	int multiplier = Config::Interpolation;
	if (multiplier < 0) {
		UINT display = Limiter::DisplayRefresh();
		UINT logic = (UINT)(Limiter::game_fps + 0.5);
		if (display == 0 || logic == 0)
			return 0.0;
		multiplier = (int)(display / logic);
	}
	if (multiplier < 2)
		return 0.0;
	return Limiter::game_fps * (double)multiplier;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th20_diag_frames_max = 300;
static const unsigned int th20_diag_presents_max = 8;
struct Th20DiagFrame {
	LARGE_INTEGER base;                                 // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th20_diag_presents_max]; // when each presentation happened
};
static Th20DiagFrame th20_diag_frames[th20_diag_frames_max];
static unsigned int th20_diag_count;
static bool th20_diag_key_down;

static LARGE_INTEGER th20_perf_freq;

// How many of the render calls and presents were taken over, for the diagnostics (the patch's own
// console messages don't reach the user with the debug console off, and this is what says whether
// the patterns still matched)
static unsigned int th20_render_patched;
static const unsigned int th20_render_sites = 3;
static unsigned int th20_present_patched;
static const unsigned int th20_present_sites = 3;

static void th20_diag_dump() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash)
		slash[1] = L'\0';
	wcscat_s(path, L"oilp_diag.txt");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
		return;

	unsigned int count = th20_diag_count < th20_diag_frames_max ? th20_diag_count : th20_diag_frames_max;
	unsigned int start = th20_diag_count < th20_diag_frames_max ? 0 : th20_diag_count % th20_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th20_present_rate());
	// The game hands its frame pacing to the limiter from the wait it would sleep in (see
	// th20_frame_wait), and that only happens while its own clock still says the next frame is
	// ahead of it - which it does not while the game presents with vsync on and the display is
	// what paces it. The input latency setting is what decides between the two: 0x41C535 reads it
	// and picks D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000) only for the fastest setting, 3.
	fprintf(file, "input latency setting: %u (the fastest is 3), limiter ticked: %s\n",
		*(unsigned char*)rebase(0x005C4F89), th20_limiter_ticked ? "yes" : "no");
	fprintf(file, "configured game frame rate: %u, thprac's frame rate: %.2f\n",
		Config::GameFPS, th20_external_rate());
	fprintf(file, "taken over: %u of %u render call(s), %u of %u present call(s)\n",
		th20_render_patched, th20_render_sites, th20_present_patched, th20_present_sites);
	fprintf(file, "frames recorded: %u\n", count);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th20DiagFrame& frame = th20_diag_frames[(start + i) % th20_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th20_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th20_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th20_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the render block the game runs, once more, so the state the
// game is in is drawn and presented again. The game's own presentation is the first one of a frame
// and the extras are spread over the rest of it, right where the game would otherwise sit and wait
// for the next one, so they land on the display's refresh evenly.
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed.
static void th20_extra_presentation(float t) {
	IDirect3DDevice9* device = th20_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// Put back whatever the scene was drawn with (see th20_render_hook)
	if (th20_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th20_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th20_scene_projection);
	}
	if (th20_scene_viewport.Width != 0)
		device->SetViewport(&th20_scene_viewport);

	// The block itself, in the order the game runs it
	th20_reset_sprite_queue(*th20_sprite_manager);
	*th20_render_flag = 0xFF;
	th20_flush_sprite_queue(th20_renderer);

	// The reset above emptied the queue, exactly the way the game's own block empties it right
	// before it draws the objects into it - so the objects are drawn again here to fill it
	ToolState::ReadyToDraw();
	th20_render_objects(*th20_scene);
	th20_draw_sprite_queue(*th20_sprite_manager);

	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras are presented with the device's own Present, without the hooks the tools put in
	// front of it (see d3d9_present_bypassing_hooks) - unless the patch didn't hook D3D9 at all,
	// and then there is only the device's own Present to use
	if (d3d9_hooked_device() != nullptr)
		d3d9_present_bypassing_hooks();
	else
		device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
// This is what the game's own present call runs instead of: the call is replaced with a call to
// this, and the present the game sets up after it (loading the device, pushing the arguments and
// calling it) is skipped over, so the game ends up right after its own present call again, with
// the result in eax where its code expects it.
static HRESULT th20_present_hook() {
	IDirect3DDevice9* device = th20_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th20_perf_freq);
	}

	// Follow whatever frame rate thprac's option is asking for, from here rather than from the
	// limiter: th20's own frame function only ticks the limiter while its clock says the next
	// frame is still ahead of it, so a game that has been pushed to a frame rate it can't keep up
	// with would never look at thprac's value again - bringing the frame rate back down would do
	// nothing, and the game would stay at the rate it could not keep up with (see
	// Limiter::PollExternalGameFPS)
	Limiter::PollExternalGameFPS();

	// The frame rate this game runs at, and the pacing that goes with it, are the game's own: it
	// presents with vsync off (and paces itself through the limiter) only while its input latency
	// setting is the fastest one. Any other setting has it present with vsync on, and then the
	// presents throttle everything - including the extra presentations, which would be waiting for
	// the display each time - so the game would visibly slow down instead of getting smoother.
	static bool pacing_checked = false;
	if (!pacing_checked) {
		pacing_checked = true;
		unsigned char latency = *(unsigned char*)rebase(0x005C4F89);
		if (!th20_limiter_ticked || latency != 3) {
			printf("th20 is presenting with vsync (input latency setting %u, limiter ticked: %s):\n"
				"set the game's input latency setting to the fastest one, or the frame interpolation has no pacing to follow!\n",
				latency, th20_limiter_ticked ? "yes" : "no");
		}
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// Where the game's own part of this frame started, so the extras can tell whether they still
	// fit before the next frame is due. The limiter's own FrameWork() can't be used here: th20 is
	// paced through TickUntil, which doesn't have the game report where its frame ended, so it
	// would count the previous frame's extra presentations as this frame's work and drop them.
	LARGE_INTEGER frame_start = Limiter::FrameStart();
	LARGE_INTEGER game_work;
	game_work.QuadPart = base.QuadPart - frame_start.QuadPart;

	// The game's own presentation of this frame, through the device it drew with, so that the
	// hooks the user's tools have on it still see it
	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();

	// The game's own work of this frame ends here as far as the overlay is concerned - everything
	// below is the patch presenting the same frame again
	Limiter::MarkFrameWorkEnd();

	// The tool has drawn its frame by now: the other half of watching for its frame state
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th20_diag_presents_max];
	present_time[0] = base;

	double rate = th20_present_rate();
	// Nothing more to present if the presentation that just happened failed (a lost device, for
	// example) - the game's own code right after this handles that
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		// How many presentations this frame gets. The game's own present is one of them, and the
		// fraction is carried over to the next frame, so a rate that isn't a multiple of the
		// game's frame rate still averages out ("*144" with 60fps logic presents 2 and 3
		// alternately).
		static double presentations_owed = 0.0;
		presentations_owed += rate / Limiter::game_fps - 1.0;
		unsigned int extras = 0;
		while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
			presentations_owed -= 1.0;
			++extras;
		}

		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && frame_wait.QuadPart > 0) {
			// In the steady state the game's own presents are one frame apart, so spreading the
			// extras this far apart from the one that just happened puts them evenly between this
			// frame and the next. Evenly spaced presentations are what keeps the motion looking
			// even, since each one draws the state a little further along.
			double spacing = (double)frame_wait.QuadPart / (double)(extras + 1);
			if (spacing >= 1.0) {
				// The presentations can't run past the end of the frame either - the game's own
				// clock is what decides when the next frame is due, and a frame that overruns it
				// makes the game's catch up run twice. An extra presentation costs about as much
				// as the game's own pass did, so if the last ones wouldn't fit before the next
				// frame starts they're dropped (the frame is then presented fewer times, but
				// never late).
				bool has_deadline = frame_start.QuadPart > 0 && game_work.QuadPart > 0;
				LARGE_INTEGER deadline;
				deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

				for (unsigned int k = 1; k <= extras; ++k) {
					LARGE_INTEGER target;
					target.QuadPart = base.QuadPart + (LONGLONG)(spacing * (double)k + 0.5);
					if (has_deadline && target.QuadPart + game_work.QuadPart > deadline.QuadPart)
						continue;

					Limiter::WaitUntil(target);

					if (presents < th20_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along
					th20_extra_presentation((float)k / (float)(extras + 1));
				}
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th20DiagFrame& frame = th20_diag_frames[th20_diag_count % th20_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th20_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th20_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th20_diag_key_down) {
			th20_diag_key_down = true;
			th20_diag_dump();
		}
	} else {
		th20_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the renderer load, the device it returns,
// the arguments and the call itself, 39 bytes in all (see the comment above)
static const size_t th20_present_setup_size = 39;

// Takes over one of the game's Present calls. The renderer load at its start is 5 bytes, which is
// what the hook's call needs, so the hook goes over those 5 bytes, and the 34 bytes of setup that
// follow are jumped over - all five arguments are pushed inside them, so skipping them leaves the
// stack the way the game expects.
static bool th20_hook_present(DWORD address) {
	BYTE* site = (BYTE*)rebase(address);
	DWORD renderer = rebase(0x005C4D40);

	// mov ecx, 005C4D40h : B9 40 4D 5C 00
	if (site[0] != 0xB9 || *(DWORD*)(site + 1) != renderer ||
		// call 00412730 : E8 <rel32>
		site[5] != 0xE8 || *(DWORD*)(site + 6) != rebase(0x00412730) - (rebase(address) + 10) ||
		// mov [ebp+?], eax / mov ?, [ebp+?] / mov ?, [?] / mov ?, [?+44h] / mov [ebp+?], ?
		site[10] != 0x89 || site[11] != 0x45 ||
		site[13] != 0x8B || (site[14] & 0xC7) != 0x45 || site[15] != site[12] ||
		site[16] != 0x8B ||
		site[18] != 0x8B || site[20] != 0x44 ||
		site[21] != 0x89 || (site[22] != 0x4D && site[22] != 0x55) ||
		// push 0 x4
		site[24] != 0x6A || site[25] != 0x00 || site[26] != 0x6A || site[27] != 0x00 ||
		site[28] != 0x6A || site[29] != 0x00 || site[30] != 0x6A || site[31] != 0x00 ||
		// mov ?, [ebp+?] / push ? / call [ebp+?]
		site[32] != 0x8B || (site[33] & 0xC7) != 0x45 || site[34] != site[12] ||
		(site[35] != 0x50 && site[35] != 0x51 && site[35] != 0x52 && site[35] != 0x53) ||
		site[36] != 0xFF || site[37] != 0x55 || site[38] != site[23]) {
		printf("th20 present call at 0x%x doesn't look like a known one, skipping!\n", address);
		return false;
	}

	{
		// The hook's call
		BYTE patch[5] = { 0xE8, 0x00, 0x00, 0x00, 0x00 };
		*(DWORD*)(patch + 1) = (DWORD)&th20_present_hook - ((DWORD)site + 5);
		patch_bytes(site, patch, sizeof(patch));
	}
	{
		// Jump over the setup and over the present call itself, so the code after it runs with
		// the result the hook left in eax
		BYTE patch[34];
		patch[0] = 0xE9;
		*(DWORD*)(patch + 1) = ((DWORD)site + th20_present_setup_size) - ((DWORD)site + 10);
		for (size_t i = 5; i < sizeof(patch); ++i)
			patch[i] = 0x90;
		patch_bytes(site + 5, patch, sizeof(patch));
	}
	return true;
}

// Takes over the object render (only to watch what the scene is drawn with and where the tool's
// frame is) and the three presents
static void th20_install_interpolation() {
	// Take over the object render, only to remember what the scene is drawn with (see
	// th20_render_hook); the render itself runs exactly the way it always did
	static const DWORD render_calls[] = {
		0x00419D3B, // FUN_00419C20
		0x00419F91, // FUN_00419DE0
		0x0041A10C, // FUN_0041A030
	};
	for (DWORD address : render_calls) {
		BYTE* render_call = (BYTE*)rebase(address);
		// call 00412AA0
		if (render_call[0] == 0xE8 &&
			*(DWORD*)(render_call + 1) == rebase(0x00412AA0) - (rebase(address) + 5)) {
			patch_call(rebase(address), (void*)th20_render_hook);
			++th20_render_patched;
		} else {
			printf("th20 render call at 0x%x doesn't look like a known one, skipping!\n", address);
		}
	}

	// Take over the presents in the frame functions (see th20_hook_present)
	static const DWORD present_sites[] = {
		0x00419666, // FUN_004193E0
		0x004199B2, // FUN_004199A0
		0x00419B2E, // FUN_00419A50
	};
	for (DWORD address : present_sites) {
		if (th20_hook_present(address))
			++th20_present_patched;
	}
	printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
		th20_render_patched, th20_present_patched,
		Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

	// The overlay shows the frame rate the frames are actually presented at, which only says
	// something (and is only higher than the game's own rate) with the interpolation on - so it is
	// only shown when that and the overlay are both on
	overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
	// ...and it goes right after the line the patch itself draws in the bottom left corner, rather
	// than at the bottom right corner of the window where it would sit on top of the frame rate and
	// slowdown readout thprac draws there (see overlay.h)
	overlay_set_present_rate_follow_text(true);
}
