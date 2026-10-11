// Touhou 18: Unconnected Marketeers v1.00a

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou18.h"

using namespace Touhou18;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004CF418;

int __fastcall th18_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th18 runs on the same family of engine as th17, but it is one generation further along: the
// block that draws a frame is recognisably the same idea, laid out slightly differently. It sits
// in the frame function of whatever scene is running:
//
//     0x471B87  mov eax, [0x4CCDF8]       the device
//     0x471B8F  CALL [ecx+0xA4]           BeginScene
//     0x471B95  FUN_0047E6E0              reset the sprite queue
//     0x471B9A  mov ecx, [0x51F65C]       the sprite manager
//     0x471BA0  mov [0x4CD918], 0FFh      the flag the block sets to 0xFF
//     0x471BAA  FUN_0047E730              draw the sprite queue (ECX = the sprite manager)
//     0x471BB6  mov [0x4CD918], 0
//     0x471BC5  CALL [ecx+0xE4]           SetRenderState(D3DRS_FOGENABLE, 0)
//     0x471BCB  FUN_00401420              render every object (they fill the sprite queue)
//     0x471BD6  FUN_0047E730              draw the sprite queue
//     0x471BE7  CALL [ecx+0x104]          SetTexture(0, NULL)
//     0x471BF5  CALL [ecx+0xA8]           EndScene
//
// Two things differ from th17's block: there is no "prepare the render" call into the scene object
// (the game sets D3DRS_FOGENABLE itself, which is what that call used to do), and the sprite queue
// is drawn twice - once before the objects are rendered and once after. The first of those draws
// an empty queue (the reset just emptied it), so it does nothing, but the extra presentation
// reproduces the block as it is rather than as it is meant.
//
// The camera is laid out differently too. There is no call that takes a camera object: the frame
// function loads the render context out of 0x4CD5DC and hands the device the viewport at its
// +0x10C, and copies the two floats that place the sprite queue on screen (context +0x104/+0x108,
// integers turned into floats) into the sprite manager's +0xD8/+0xDC. No view or projection
// transform is set anywhere in these frame functions, but what the scene is drawn with is taken
// from the device all the same, so an extra presentation can't be the thing that changes it.
//
// The block appears three times, once for the game scene and once for each of the two menu scenes:
// the renders are at 0x471BCB, 0x472F21 and 0x47307A, and the presentations - the game's own
// Present call - at 0x472D2D, 0x472F6C and 0x473139. Each present site is laid out identically:
// the device is loaded into eax, then the arguments are pushed and the present is called through
// the device's vtable, 14 bytes in all after the load. All five of those arguments are pushed
// inside those 14 bytes, so skipping the setup leaves the stack the way the game expects and the
// hook's return value lands in eax where the game's own code reads it.
//
// zun's engine separates the game's logic from its rendering, so the same state can be drawn again
// by running the render block again - that's what the extra presentations are. The game's own
// presentation is the first one, and the extra ones are spread evenly over the rest of the frame,
// right where the game would otherwise sit and wait for the next one, so they land on the display's
// refresh evenly.
//
// The projection of the drawn objects - moving them forward in time before the extra presentations
// - comes with stage 1. With stage 0 the extra presentations draw the state the game is already
// in, which is the "moved forward by zero frames" case, and is what the infrastructure - the
// pacing, the re-rendering, the overlays of the user's tools, the diagnostics - is validated with.

// The plain calls the render block makes into the game
static auto th18_reset_sprite_queue = (void(*)())0x0047E6E0;
static auto th18_draw_sprite_queue = (void(__fastcall*)(void*))0x0047E730; // ECX = the sprite manager
static auto th18_render_objects = (void(*)())0x00401420;                   // renders every object

static void** const th18_sprite_manager = (void**)0x0051F65C;
// The dword the render block sets to 0xFF (and back to 0) around its first draw of the queue
static DWORD* const th18_render_flag = (DWORD*)0x004CD918;

// The two floats the frame function copies into the sprite manager to place the queue on screen
// (the sprite manager gets them at +0xD8/+0xDC)
static const size_t th18_sprite_queue = 0xD8;
static float th18_sprite_queue_saved[2];

// What the scene is drawn with, taken from the device in the render hook (the frame functions set
// the viewport themselves and the menus set their own, so an extra presentation has to put back
// whatever the scene the game just drew was using)
static D3DVIEWPORT9 th18_scene_viewport = {};
static D3DMATRIX th18_scene_view = {};
static D3DMATRIX th18_scene_projection = {};
static bool th18_scene_transforms = false;

// Where the user's tool builds its frame and where it draws it. thprac's th18 module starts its
// frame (its NewFrame, its UI code, and its EndFrame) from a hook on the return of the scene
// update at 0x4013F5, and draws the overlay it built from a hook on the return of the object
// render at 0x401510. The extra presentations render the scene again, which erases the overlay the
// game's own pass drew, so the tool has to draw it again for every one of them (see tool_state.h -
// its frame state is found at runtime and set again there, which draws the overlay without running
// any of the tool's own code).
static unsigned char* const th18_tool_frame_address = (unsigned char*)0x004013F5;
static unsigned char* const th18_tool_draw_address = (unsigned char*)0x00401510;

// Remembers the viewport, the transforms and the two queue floats the scene is about to be drawn
// with, then lets the game's own render run exactly as it always did
static void th18_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr) {
		device->GetViewport(&th18_scene_viewport);
		th18_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th18_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th18_scene_projection));
	}

	void* sprites = *th18_sprite_manager;
	if (sprites != nullptr) {
		th18_sprite_queue_saved[0] = *(float*)((char*)sprites + th18_sprite_queue);
		th18_sprite_queue_saved[1] = *(float*)((char*)sprites + th18_sprite_queue + 4);
	}

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th18_tool_frame_address, th18_tool_draw_address);

	th18_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
//
// "-1" picks the largest multiple of the game's frame rate that still fits the display refresh
// (a 190hz display and a 60fps game give 3), a positive value is that multiple, and the
// "*<frame rate>" form is that frame rate no matter what the game's own is.
static double th18_present_rate() {
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
static const unsigned int th18_diag_frames_max = 300;
static const unsigned int th18_diag_presents_max = 8;
struct Th18DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th18_diag_presents_max];      // when each presentation happened
};
static Th18DiagFrame th18_diag_frames[th18_diag_frames_max];
static unsigned int th18_diag_count;
static bool th18_diag_key_down;

static LARGE_INTEGER th18_perf_freq;

static void th18_diag_dump() {
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

	unsigned int count = th18_diag_count < th18_diag_frames_max ? th18_diag_count : th18_diag_frames_max;
	unsigned int start = th18_diag_count < th18_diag_frames_max ? 0 : th18_diag_count % th18_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th18_present_rate());
	fprintf(file, "frames recorded: %u\n", count);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th18DiagFrame& frame = th18_diag_frames[(start + i) % th18_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th18_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th18_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th18_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the drawn objects projected t frames ahead of
// it (stage 1) - t = 0 is the state the game's own presentation showed, and the presentations of
// one frame reach t = 1, which is where the game's next one takes over. The tool's frame is readied
// again, so the object render below draws its overlay into this presentation the same way it does
// in the game's own one.
static void th18_extra_presentation(float t) {
	(void)t; // stage 0 draws the state the game is already in

	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// The game's own pass set these before this point and the menus change them, so put back
	// whatever the scene was drawn with (see th18_render_hook)
	if (th18_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th18_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th18_scene_projection);
	}
	if (th18_scene_viewport.Width != 0)
		device->SetViewport(&th18_scene_viewport);

	// The block itself, in the order the game runs it
	th18_reset_sprite_queue();
	*th18_render_flag = 0xFF;
	th18_draw_sprite_queue(*th18_sprite_manager);
	*th18_render_flag = 0;
	device->SetRenderState(D3DRS_FOGENABLE, 0);

	// The two floats that place the queue on screen have to be the ones the game's own pass left,
	// since the menus write their own and an extra presentation runs after the whole frame
	void* sprites = *th18_sprite_manager;
	if (sprites != nullptr) {
		*(float*)((char*)sprites + th18_sprite_queue) = th18_sprite_queue_saved[0];
		*(float*)((char*)sprites + th18_sprite_queue + 4) = th18_sprite_queue_saved[1];
	}

	ToolState::ReadyToDraw();
	th18_render_objects();
	th18_draw_sprite_queue(*th18_sprite_manager);
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
static HRESULT th18_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th18_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

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
	LARGE_INTEGER present_time[th18_diag_presents_max];
	present_time[0] = base;

	double rate = th18_present_rate();
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
		// fraction is carried over to the next frame, so a rate that isn't a multiple of the game's
		// frame rate still averages out ("*144" with 60fps logic presents 2 and 3 alternately).
		static double presentations_owed = 0.0;
		presentations_owed += rate / Limiter::game_fps - 1.0;
		unsigned int extras = 0;
		while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
			presentations_owed -= 1.0;
			++extras;
		}

		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && frame_wait.QuadPart > 0) {
			// The game's own present is the first presentation of the frame; the extras go after
			// it, one every this many performance counter ticks, evenly spread over the rest of
			// the frame. Keeping the presentations evenly spaced matters: each one draws the state
			// a little further along, so an uneven spacing is an uneven looking motion.
			double spacing = (double)frame_wait.QuadPart / (double)(extras + 1);
			if (spacing >= 1.0) {
				// The presentations can't run past the end of the frame either - the limiter's
				// schedule is absolute, so a frame that overruns is a frame the game falls behind
				// on. An extra presentation costs about as much as the game's own pass did, so if
				// the last ones wouldn't fit before the next frame starts they're dropped (the
				// frame is then presented fewer times, but never late).
				LARGE_INTEGER frame_start = Limiter::FrameStart();
				LARGE_INTEGER frame_work = Limiter::FrameWork();
				bool has_deadline = frame_start.QuadPart > 0 && frame_work.QuadPart > 0;
				LARGE_INTEGER deadline;
				deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

				for (unsigned int k = 1; k <= extras; ++k) {
					LARGE_INTEGER target;
					target.QuadPart = base.QuadPart + (LONGLONG)(spacing * (double)k + 0.5);
					if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
						continue;

					Limiter::WaitUntil(target);

					if (presents < th18_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along (stage 1)
					th18_extra_presentation((float)k / (float)(extras + 1));
				}
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th18DiagFrame& frame = th18_diag_frames[th18_diag_count % th18_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th18_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th18_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th18_diag_key_down) {
			th18_diag_key_down = true;
			th18_diag_dump();
		}
	} else {
		th18_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the arguments are pushed and the device
// is called, 14 bytes in all (see th18_install_patches)
static const size_t th18_present_setup_size = 14;

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax
static void th18_skip_present_setup(DWORD address) {
	BYTE patch[th18_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th18_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th18_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th18_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x53 };
		patch_bytes(0x00472E25, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xE9, 0xB2, 0x01, 0x00, 0x00, 0x90 };
		patch_bytes(0x00471A9E, patch, sizeof(patch));
	}
	{
		// Hook window update
		patch_call(0x00471C5A, th18_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00471634, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00471639, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0x90, 0x90 };
		patch_bytes(0x00461DD3, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport, the transforms and the two
		// queue floats the scene is drawn with (see th18_render_hook); the render itself runs
		// exactly the way it always did
		static const DWORD render_calls[] = { 0x00471BCB, 0x00472F21, 0x0047307A };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th18_render_hook);
				++render_patched;
			} else {
				printf("th18 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the three frame functions. The instruction that loads the
		// device for the present is 5 bytes, which is exactly what the hook's call needs, and the
		// 14 bytes after it (the arguments and the present call itself) are skipped over - the hook
		// presents with the same device and arguments the game would have used.
		static const DWORD present_sites[] = { 0x00472D1D, 0x00472F5C, 0x00473129 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004CCDF8) {
				patch_call(address, (void*)th18_present_hook);
				th18_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th18 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
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

FPSTarget th18_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004CA210;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & InputState::Shoot) // LCTRL isn't read at all in this game
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
