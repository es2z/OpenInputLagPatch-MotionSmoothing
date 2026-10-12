// Touhou 19: Unfinished Dream of All Living Ghost v1.10c

#include <stdio.h>
#include <string.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou19.h"

using namespace Touhou19;

// th19 is built with ASLR enabled, unlike the older games, so the addresses from the
// disassembly (which all assume the default 0x400000 image base) have to be rebased on the
// base the executable actually got loaded at
static DWORD rebase(DWORD address) {
	static DWORD image_base = (DWORD)GetModuleHandleW(nullptr);
	return image_base + (address - 0x400000);
}

// Unlike the older games, th19 doesn't just run its logic once per rendered frame: each update
// function advances the engine's clock by one frame at a time and only runs the game's logic
// (and renders) once that clock says a frame has passed. How long a frame is comes from a
// hardcoded double holding the frame rate (0x005AA2C0 is 60.0), which is read as
// 1.0 / frame_rate wherever it's needed - that's why the game kept running at 60fps no matter
// what the frame limiter was set to.
//
// Pointing those reads at the limiter's frame rate instead moves the engine's clock (and with
// it the game's speed) to GameFPS, the same thing thprac's FPS option does for th11 and up.
// Each address below is the 4 byte displacement of a MOVSD/DIVSD that reads 0x005AA2C0, so the
// patch only changes which double the instruction reads.
//
// The game's own frame limiter (0x004B6240) reads the same constant, but it's disabled below,
// so it doesn't have to be patched.
static const DWORD fps_references[] = {
	0x004B5C9B, // Sleep based pacer of the update path at 0x004B5C70
	0x004B5E8E, // Sleep based pacer of the update path at 0x004B5D70
	0x004B6090, // frame stepping of the update path at 0x004B5FE0
	0x004B650D, // frame stepping of the update path at 0x004B64E0
	0x004B7639, // frame stepping of the main loop's own catch up path
};

static void patch_fps_reference(DWORD address) {
	DWORD location = rebase(address);

	// Only patch if the instruction still reads the engine's frame rate: an unknown build
	// shouldn't get random data written into the middle of its code. The address in the
	// instruction is an absolute one, so it gets relocated along with the executable and has
	// to be rebased before it can be compared
	DWORD frame_rate = rebase(0x005AA2C0);
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
double th19_frame_wait_ms = 0.0;
void th19_frame_wait_impl();

// Set once the game hands its frame pacing over to the limiter (see the frame interpolation
// below). Until it is, the game is running a frame function that paces itself, which is what
// happens while it presents with vsync on - and then the patch's pacing never gets to run.
static bool th19_limiter_ticked = false;

void __declspec(naked) th19_frame_wait() {
	__asm {
		movsd qword ptr [th19_frame_wait_ms], xmm0
		jmp th19_frame_wait_impl
	}
}

void th19_frame_wait_impl() {
	th19_limiter_ticked = true;
	Limiter::TickUntil(th19_frame_wait_ms / 1000.0);
}

static void patch_frame_wait(DWORD address) {
	BYTE* sequence = (BYTE*)rebase(address);

	// push 1 / call dword ptr [Sleep]
	if (sequence[0] != 0x6A || sequence[1] != 0x01 || sequence[2] != 0xFF || sequence[3] != 0x15) {
		printf("Frame wait at 0x%x doesn't look like a known one, skipping!\n", address);
		return;
	}

	BYTE patch[8] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90 };
	*(DWORD*)(patch + 1) = (DWORD)&th19_frame_wait - ((DWORD)sequence + 5);
	patch_bytes(sequence, patch, sizeof(patch));
}

// thprac changes the frame rate of games like this one by pointing the engine's clock at a
// variable of its own (0x004B6090, the spot the update path the game uses reads). If that
// happens the game and the limiter would disagree about how long a frame is, so the operand is
// taken back - but the value the other tool put there is followed, since tools like thprac are
// what the player configures in game and their frame rate wins over the configured GameFPS.
static double* external_fps = nullptr;

static UINT th19_external_game_fps() {
	DWORD* operand = (DWORD*)rebase(0x004B6090);

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
static double th19_external_rate() {
	return external_fps != nullptr ? *external_fps : 0.0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th19's engine is three years younger than th18's, but a frame is still laid out the same way:
// something updates every object, a block draws them, and the frame is presented once. The main
// loop (FUN_004B6E50) runs one of two frame functions for every iteration, and both of them hold
// the whole frame - the object update, the render block, the wait for the game's own clock, the
// present and the frame rate bookkeeping. FUN_004B5FE0 is the one it runs normally, and
// FUN_004B5D70 the one it runs when 0x62BC58 is 1 and 0x62BDDC is 0.
//
// The render block is the same idea as th18's, in the same order:
//
//   CALL [ecx+0xA4]           BeginScene
//   FUN_004CA3C0              reset the sprite queue
//   mov [0x62C70C], 0FFh      the flag the block sets to 0xFF
//   FUN_004CA360              draw the sprite queue (ECX = [0x5D19BC])
//   mov [0x62C70C], 0
//   CALL [ecx+0xE4]           SetRenderState(D3DRS_FOGENABLE, 0)
//   FUN_004D71A0              render every object (they fill the sprite queue)
//   FUN_004CA360              draw the sprite queue
//   CALL [ecx+0x104]          SetTexture(0, NULL)
//   CALL [ecx+0xA8]           EndScene
//
// so an extra presentation is the same thing it is in th18: run that block again. What is not
// there any more is the camera - the frame functions set no transform at all and the queue is
// placed by the coordinates the objects are queued with. What the extra presentations do have to
// put back is whatever the scene was drawn with, and that is read off the device in the render
// hook below (the menus do change the viewport).
//
// The block sits in four places, and two of them are dead code: nothing in the executable
// references FUN_004B5C70, the small function at 0x4B5F80 or the function at 0x4B64E0 (a scan of
// the whole image finds no reference to any of their entry points), and the block in the main
// loop's own frame stepping path is behind a flag that is only ever cleared (bit 0x80 of
// 0x62EE7C). They are patched anyway - taking them over costs nothing, and a path the patch
// doesn't know about still runs the way it always did if it is ever taken.
//
// The presents are laid out identically at all five call sites, 20 bytes each:
//
//   8B 0D 48 BB 62 00   mov ecx, [0x62BB48]     the device  <- the hook goes over these 6 bytes
//   6A 00 6A 00 6A 00   push 0 x3
//   8B 01               mov eax, [ecx]
//   6A 00               push 0
//   51                  push ecx
//   FF 50 44            call [eax+44h]          Present, 20 bytes after the load
//
// Every one of the five arguments is pushed inside those 20 bytes, so skipping the whole setup
// leaves the stack the way the game expects, and the hook's return value lands in eax - which is
// where the code right after the call reads it, to notice a lost device.

// The plain calls the render block makes into the game
static auto th19_reset_sprite_queue = (void(*)())rebase(0x004CA3C0);
static auto th19_draw_sprite_queue = (void(__fastcall*)(void*))rebase(0x004CA360); // ECX = the sprite manager
static auto th19_render_objects = (void(*)())rebase(0x004D71A0);

// The sprite (animation) manager whose queue the render block resets and draws
static void** th19_sprite_manager = (void**)rebase(0x005D19BC);
// The dword the render block sets to 0xFF (and back to 0) around its first draw of the queue
static DWORD* th19_render_flag = (DWORD*)rebase(0x0062C70C);

// What the scene is drawn with, taken off the device in the render hook (the frame functions never
// set it, but the menus do, so an extra presentation has to put back whatever the scene that was
// just drawn used)
static D3DVIEWPORT9 th19_scene_viewport = {};
static D3DMATRIX th19_scene_view = {};
static D3DMATRIX th19_scene_projection = {};
static bool th19_scene_transforms = false;

// Where the user's tool builds its frame and where it draws it. thprac's th19 module starts its
// frame (its NewFrame, its UI code and its EndFrame) from a hook on the return of the scene update
// at 0x4D715F - the `ret` the update walk 0x4D7000 ends with - and draws the overlay it built from
// a hook on the return of the object render at 0x4D72C0, the `ret` the render walk 0x4D71A0 ends
// with. The extra presentations draw the scene again, which erases the overlay the game's own pass
// drew, so the tool has to draw it again for every one of them (see tool_state.h).
static unsigned char* th19_tool_frame_address = (unsigned char*)rebase(0x004D715F);
static unsigned char* th19_tool_draw_address = (unsigned char*)rebase(0x004D72C0);

// Remembers what the scene is about to be drawn with, then lets the game's own render run exactly
// as it always did
static void th19_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr) {
		device->GetViewport(&th19_scene_viewport);
		th19_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th19_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th19_scene_projection));
	}

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th19_tool_frame_address, th19_tool_draw_address);

	th19_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
//
// "-1" picks the largest multiple of the game's frame rate that still fits the display refresh
// (a 190hz display and a 60fps game give 3), a positive value is that multiple, and the
// "*<frame rate>" form is that frame rate no matter what the game's own is.
static double th19_present_rate() {
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
static const unsigned int th19_diag_frames_max = 300;
static const unsigned int th19_diag_presents_max = 8;
struct Th19DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th19_diag_presents_max];      // when each presentation happened
};
static Th19DiagFrame th19_diag_frames[th19_diag_frames_max];
static unsigned int th19_diag_count;
static bool th19_diag_key_down;

static LARGE_INTEGER th19_perf_freq;

static void th19_diag_dump() {
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

	unsigned int count = th19_diag_count < th19_diag_frames_max ? th19_diag_count : th19_diag_frames_max;
	unsigned int start = th19_diag_count < th19_diag_frames_max ? 0 : th19_diag_count % th19_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th19_present_rate());
	// 0x80000000 is D3DPRESENT_INTERVAL_IMMEDIATE, which is what the game uses only while its
	// input latency setting is the fastest one - at any other setting it presents with vsync on
	// and paces itself, and the extra presentations end up waiting for the display
	fprintf(file, "presentation interval: 0x%x, limiter ticked: %s\n",
		*(DWORD*)rebase(0x0062BC58), th19_limiter_ticked ? "yes" : "no");
	fprintf(file, "configured game frame rate: %u, thprac's frame rate: %.2f\n",
		Config::GameFPS, th19_external_rate());
	fprintf(file, "frames recorded: %u\n", count);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th19DiagFrame& frame = th19_diag_frames[(start + i) % th19_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th19_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th19_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th19_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the render block the game runs, once more, so the state the
// game is in is drawn and presented again. The game's own presentation is the first one of a frame
// and the extras are spread over the rest of it, right where the game would otherwise sit and wait
// for the next one, so they land on the display's refresh evenly.
static void th19_extra_presentation() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// Put back whatever the scene was drawn with (see th19_render_hook)
	if (th19_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th19_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th19_scene_projection);
	}
	if (th19_scene_viewport.Width != 0)
		device->SetViewport(&th19_scene_viewport);

	// The block itself, in the order the game runs it
	th19_reset_sprite_queue();
	*th19_render_flag = 0xFF;
	th19_draw_sprite_queue(*th19_sprite_manager);
	*th19_render_flag = 0;
	device->SetRenderState(D3DRS_FOGENABLE, 0);

	// The queue was just reset, so the objects have to be drawn again to fill it - this is where
	// the projection of stage 1 will go, right before the render
	ToolState::ReadyToDraw();
	th19_render_objects();
	th19_draw_sprite_queue(*th19_sprite_manager);

	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras are presented with the device's own Present, without the hooks the tools put in
	// front of it (see d3d9_present_bypassing_hooks)
	d3d9_present_bypassing_hooks();
	overlay_mark_presentation();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
// This is what the game's own present call runs instead of: the call is replaced with a call to
// this, and the present the game sets up after it (pushing the arguments and calling the device)
// is skipped over, so the game ends up right after its own present call again, with the result in
// eax where its code expects it.
static HRESULT th19_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th19_perf_freq);
	}

	// Follow whatever frame rate thprac's option is asking for, from here rather than from the
	// limiter: th19's own frame function only ticks the limiter while its clock says the next
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
		DWORD interval = *(DWORD*)rebase(0x0062BC58);
		if (!th19_limiter_ticked || interval != 0x80000000) {
			printf("th19 is presenting with vsync (presentation interval 0x%x, limiter ticked: %s):\n"
				"set the game's input latency setting to the fastest one, or the frame interpolation has no pacing to follow!\n",
				interval, th19_limiter_ticked ? "yes" : "no");
		}
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// Where the game's own part of this frame started, so the extras can tell whether they still
	// fit before the next frame is due. The limiter's own FrameWork() can't be used here: th19 is
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
	LARGE_INTEGER present_time[th19_diag_presents_max];
	present_time[0] = base;

	double rate = th19_present_rate();
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

					if (presents < th19_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					th19_extra_presentation();
				}
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th19DiagFrame& frame = th19_diag_frames[th19_diag_count % th19_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th19_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th19_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th19_diag_key_down) {
			th19_diag_key_down = true;
			th19_diag_dump();
		}
	} else {
		th19_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the device load, the arguments and the
// call itself, 20 bytes in all (see th19_install_patches)
static const size_t th19_present_setup_size = 20;

// Takes over one of the game's Present calls. The device load at its start is 6 bytes, one byte
// more than the hook's call needs, so the hook goes over those 6 bytes (and a nop), and the 14
// bytes of setup that follow it are jumped over - all five arguments are pushed inside those 14
// bytes, so skipping them leaves the stack the way the game expects.
static bool th19_hook_present(DWORD address) {
	BYTE* site = (BYTE*)rebase(address);
	DWORD device = rebase(0x0062BB48);

	// mov ecx, dword ptr ds:[0062BB48h] : 8B 0D <address>
	if (site[0] != 0x8B || site[1] != 0x0D || *(DWORD*)(site + 2) != device) {
		printf("th19 present call at 0x%x doesn't look like a known one, skipping!\n", address);
		return false;
	}
	// push 0 x3 / mov eax, [ecx] / push 0 / push ecx / call [eax+44h]
	static const BYTE expected[14] = {
		0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00,
		0x8B, 0x01,
		0x6A, 0x00,
		0x51,
		0xFF, 0x50, 0x44,
	};
	if (memcmp(site + 6, expected, sizeof(expected)) != 0) {
		printf("th19 present call at 0x%x doesn't look like a known one, skipping!\n", address);
		return false;
	}

	{
		// The hook's call, and a nop where the device load used to end
		BYTE patch[6] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90 };
		*(DWORD*)(patch + 1) = (DWORD)&th19_present_hook - ((DWORD)site + 5);
		patch_bytes(site, patch, sizeof(patch));
	}
	{
		// Jump over the setup and over the present call itself, so the code after it runs with
		// the result the hook left in eax
		BYTE patch[14];
		patch[0] = 0xE9;
		*(DWORD*)(patch + 1) = ((DWORD)site + th19_present_setup_size) - ((DWORD)site + 6 + 5);
		for (size_t i = 5; i < sizeof(patch); ++i)
			patch[i] = 0x90;
		patch_bytes(site + 6, patch, sizeof(patch));
	}
	return true;
}

void th19_install_patches() {
	{
		// Make the game's frame limiter return immediately. It is called from more than one
		// code path, and which one the game actually uses is hard to pin down, so disabling
		// the function itself covers all of them. Our limiter takes over its job.
		BYTE patch[] = { 0xC3 };
		patch_bytes(rebase(0x004B6240), patch, sizeof(patch));
	}
	{
		// The engine starts reading its frame rate long before the limiter is ticked for the
		// first time, so start it off with the configured value
		Limiter::game_fps = (double)Config::GameFPS;

		// Follow thprac (or any other tool) if it takes the engine's frame rate over
		Limiter::external_game_fps = th19_external_game_fps;
	}
	{
		// Run the engine's clock at GameFPS instead of its hardcoded 60
		for (DWORD reference : fps_references)
			patch_fps_reference(reference);

		// Let the limiter do the waiting for the frames instead of the game's own Sleep(1)
		patch_frame_wait(0x004B605C);
	}
	if (Config::D3D9Ex) {
		// Redirect the Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(rebase(0x004B71A2), Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(rebase(0x004B71A7), patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember what the scene is drawn with (see
		// th19_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x004B5E32, 0x004B6189, 0x004B65E5, 0x004B76EB };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)rebase(address);
			// call 004D71A0
			if (render_call[0] == 0xE8 &&
				*(DWORD*)(render_call + 1) == rebase(0x004D71A0) - (rebase(address) + 5)) {
				patch_call(rebase(address), (void*)th19_render_hook);
				++render_patched;
			} else {
				printf("th19 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the frame functions (see th19_hook_present)
		static const DWORD present_sites[] = {
			0x004B5EFF, // FUN_004B5D70
			0x004B61C4, // FUN_004B5FE0
			0x004B5D0C, // FUN_004B5C70 (dead code)
			0x004B5F86, // the function at 0x4B5F80 (dead code)
			0x004B642E, // FUN_004B6240 (the limiter, disabled above)
		};
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			if (th19_hook_present(address))
				++present_patched;
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The overlay shows the frame rate the frames are actually presented at, which only says
		// something (and is only higher than the game's own rate) with the interpolation on - so
		// it is only shown when that and the overlay are both on
		overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
		// ...and it goes right after the line the patch itself draws in the bottom left corner,
		// rather than at the bottom right corner of the window where it would sit on top of the
		// frame rate and slowdown readout thprac draws there (see overlay.h)
		overlay_set_present_rate_follow_text(true);
	}
}
