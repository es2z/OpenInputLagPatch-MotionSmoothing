// Touhou 13: Ten Desires v1.00c

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou13.h"

using namespace Touhou13;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004C22C8;

int __stdcall th13_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th13 draws a game frame the way th14 and th15 do, with a block that sits in the frame function of
// the scene that's running:
//
//     0x45CFD6  mov eax, [0x4DC6A8]       the device
//     0x45CFDD  [vtable+0xA4]             BeginScene
//     0x45CFE6  FUN_00467960              reset the sprite queue
//     0x45CFEB  mov edi, 0x4DC6A0         the render context, for the call below
//     0x45CFF0  [0x4DCF30] = 0xFF
//     0x45CFFA  FUN_0043B080              prepare the render context (its object in EDI)
//     0x45CFFF  FUN_00470C30              render every object (they fill the sprite queue)
//     0x45D004  mov esi, [0x4DC688]       the sprite queue's owner, for the call below
//     0x45D00A  FUN_004679A0              draw the sprite queue (its object in ESI)
//     0x45D00F  [vtable+0x104]            SetTexture(0, NULL)
//     0x45D023  [vtable+0xA8]             EndScene
//
// and presents later in the same frame function (0x45D207 and the two blocks after it, one per
// menu scene), each a 5 byte load of the device followed by the arguments being pushed and the
// present called.
//
// Two things are different from th14 and th15, both the game's own doing:
//
//   - two of the helpers take their object in a register instead of on the stack, which is how the
//     compiler that built this game left them, so they can't be reached through a function pointer
//     - the trampolines below are what put the object where each one expects it.
//   - the second present site zeroes esi as part of pushing its arguments, and the code after it
//     expects that, so that site is hooked through a trampoline that does the same.
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

// The plain calls the render block makes into the game
static auto th13_reset_sprite_queue = (void(*)())0x00467960;
static auto th13_render_objects = (int(*)())0x00470C30;
static void* const th13_prepare_render_address = (void*)0x0043B080;
static void* const th13_draw_sprite_queue_address = (void*)0x004679A0;

// The two helpers that want their object in a register (see the top of this file). Both registers
// are callee-saved, so the value the caller had in them is put back before returning - leaving
// them holding the object would hand the caller something else than what it had (the game's own
// call sites simply never used those registers again, which is why the game gets away with it).
static void __declspec(naked) th13_prepare_render() {
	__asm {
		push edi
		mov edi, 0x004DC6A0
		call dword ptr [th13_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th13_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x004DC688]
		call dword ptr [th13_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// Where the user's tool builds its frame and where it draws it. thprac's th13 module starts its
// frame from a hook on the return of the scene update at 0x470C04 and draws the overlay it built
// from a hook on the return of the object render at 0x470D27 (see tool_state.h).
static unsigned char* const th13_tool_frame_address = (unsigned char*)0x00470C04;
static unsigned char* const th13_tool_draw_address = (unsigned char*)0x00470D27;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in
static D3DVIEWPORT9 th13_scene_viewport = {};

// The device the game draws with. The present sites load it from the game's own global, so that is
// the device that has to present the frame - not the one the patch's D3D9 hook captured, which is
// the same object while that hook is in place and nothing at all if the game got its device in a
// way the hook doesn't see. Reporting a failed present to the game ends it (its device-lost
// handling takes over), so the device the game itself would have presented with comes first.
static IDirect3DDevice9* th13_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004DC6A8;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th13_extra_presentation for what it's for)
static void th13_render_hook() {
	IDirect3DDevice9* device = th13_device();
	if (device != nullptr)
		device->GetViewport(&th13_scene_viewport);

	ToolState::WatchFrameReady(th13_tool_frame_address, th13_tool_draw_address);

	th13_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
static double th13_present_rate() {
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
// (what the last presentation projected, see the bullets below)
static unsigned int th13_bullets_in_play;
static unsigned int th13_bullets_moved;

static const unsigned int th13_diag_frames_max = 300;
static const unsigned int th13_diag_presents_max = 8;
struct Th13DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th13_diag_presents_max];
};
static Th13DiagFrame th13_diag_frames[th13_diag_frames_max];
static unsigned int th13_diag_count;
static bool th13_diag_key_down;

static LARGE_INTEGER th13_perf_freq;

static void th13_diag_dump() {
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

	unsigned int count = th13_diag_count < th13_diag_frames_max ? th13_diag_count : th13_diag_frames_max;
	unsigned int start = th13_diag_count < th13_diag_frames_max ? 0 : th13_diag_count % th13_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th13_present_rate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th13_bullets_in_play, th13_bullets_moved);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th13DiagFrame& frame = th13_diag_frames[(start + i) % th13_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th13_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th13_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th13_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager, which is created when a stage starts (0x40D920
// allocates it and 0x40D170 fills it in, loading bullet.anm for it) and released when it ends;
// 0x4C2174 holds it and is zero while no stage is running. The manager is the same object th14 has,
// one engine generation older: a pool of 2001 bullets of 0x135C bytes each, allocated up front and
// threaded onto a free list, plus a second list of the bullets that are in play.
//
// A bullet that is in play is entered into the second list (whose head is at manager+0x84) with a
// node that is part of the bullet itself, and the game's own update walks it that way (0x40E360):
// the node is at bullet+0x10, the node's first word is the bullet, and the node's +4 is the next
// node.
//
// A bullet keeps three floats of position (+0xB70) and three floats of velocity (+0xB7C). The
// game's own update of one bullet (0x40DB10) moves the position by the velocity once per frame,
// through the slowdown factor it applies to everything while the screen is full (0x4C0A28), and it
// moves a bullet that is waiting for its sub-pattern to start by half of that: the state at
// +0xBBE is 1 for a bullet in play, 2/3/5 for one that is starting or finishing - where the game
// itself multiplies by the double at 0x4AE8B8, which is 0.5 - and 4 or anything else doesn't move.
//
// The drawn sprites follow on their own: the bullet draw pass (0x40E4D0) copies this position into
// the animation objects the bullet carries before handing them to the sprite queue (0x46A700), so
// moving the position is enough.

static void** const th13_bullet_manager = (void**)0x004C2174;
static const size_t th13_bullet_list_head = 0x84; // in it: the first node of the bullets in play
static const size_t th13_bullet_list_next = 0x04; // in a node: the next node
static const size_t th13_bullet_position = 0xB70; // in a bullet: its position, 3 floats
static const size_t th13_bullet_velocity = 0xB7C; // in a bullet: what it moves by, 3 floats
static const size_t th13_bullet_state = 0xBBE;    // in a bullet: what it's doing, 16 bits
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th13_time_scale = (float*)0x004C0A28;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th13_frames_per_render = (unsigned char*)0x004DC890;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th13_pause_object = (void**)0x004C2194;
static const size_t th13_pause_flags = 0x60;

// The pool holds 2001 bullets, so the list of the ones in play can't be longer
static const size_t th13_bullets_saved_max = 2001;
struct Th13SavedBullet {
	void* bullet;
	float position[3];
};
static Th13SavedBullet th13_saved_bullets[th13_bullets_saved_max];
static size_t th13_saved_bullets_count;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either. 0x40E780 and 0x40E7B0 are the game's two bullet tasks.
static bool th13_bullets_held() {
	void* pause = *th13_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th13_pause_flags);
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0;
}

// How much of a frame's movement a bullet in this state gets (0x40DB10)
static float th13_bullet_step_factor(unsigned short state) {
	switch (state) {
	case 1:
		return 1.0f;
	case 2:
	case 3:
	case 5:
		return 0.5f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames and remembers what it changed
static void th13_bullets_advance(float t) {
	th13_saved_bullets_count = 0;
	th13_bullets_in_play = 0;
	th13_bullets_moved = 0;

	void* manager = *th13_bullet_manager;
	if (manager == nullptr || th13_bullets_held())
		return;

	unsigned int frames_per_render = *th13_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th13_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* node = *(void**)((char*)manager + th13_bullet_list_head);
	for (size_t guard = 0; node != nullptr && guard < th13_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th13_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th13_bullets_in_play;
		const float factor = th13_bullet_step_factor(*(unsigned short*)((char*)bullet + th13_bullet_state));
		if (factor != 0.0f && th13_saved_bullets_count < th13_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th13_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th13_bullet_velocity);
			Th13SavedBullet& saved = th13_saved_bullets[th13_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th13_bullets_moved;
		}

		node = next;
	}
}

// Puts the positions of every bullet th13_bullets_advance moved back
static void th13_bullets_undo() {
	for (size_t i = 0; i < th13_saved_bullets_count; ++i) {
		const Th13SavedBullet& saved = th13_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th13_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th13_saved_bullets_count = 0;
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the bullets projected t frames ahead of it. The
// tool's frame is readied again, so the object render below draws its overlay into this presentation
// the same way it does in the game's own one.
static void th13_extra_presentation(IDirect3DDevice9* device, float t) {
	device->BeginScene();
	if (th13_scene_viewport.Width != 0)
		device->SetViewport(&th13_scene_viewport);
	th13_reset_sprite_queue();
	*(DWORD*)0x004DCF30 = 0xFF;
	th13_prepare_render();
	ToolState::ReadyToDraw();
	// The bullets are drawn where they will be t frames from now; the render bakes the positions
	// into the sprite queue, so they can go back right after it
	th13_bullets_advance(t);
	th13_render_objects();
	th13_bullets_undo();
	th13_draw_sprite_queue();
	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras go through the device's own Present, without the hooks a tool put in front of it
	// (see d3d9_present_bypassing_hooks). If the patch's D3D9 hook isn't on this device there is no
	// saved original to use, and the game's own present call is what is left.
	if (d3d9_hooked_device() == device)
		d3d9_present_bypassing_hooks();
	else
		device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
static HRESULT th13_present_hook() {
	IDirect3DDevice9* device = th13_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th13_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th13_diag_presents_max];
	present_time[0] = base;

	double rate = th13_present_rate();
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		static double presentations_owed = 0.0;
		presentations_owed += rate / Limiter::game_fps - 1.0;
		unsigned int extras = 0;
		while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
			presentations_owed -= 1.0;
			++extras;
		}

		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && frame_wait.QuadPart > 0) {
			double spacing = (double)frame_wait.QuadPart / (double)(extras + 1);
			if (spacing >= 1.0) {
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

					if (presents < th13_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along
					th13_extra_presentation(device, (float)k / (float)(extras + 1));
				}
			}
		}
	}

	{
		Th13DiagFrame& frame = th13_diag_frames[th13_diag_count % th13_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th13_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th13_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th13_diag_key_down) {
			th13_diag_key_down = true;
			th13_diag_dump();
		}
	} else {
		th13_diag_key_down = false;
	}

	return result;
}

// The second present site zeroes esi among the arguments it pushes, and the code after it expects
// that, so it goes through this instead of straight to the hook
static void __declspec(naked) th13_present_hook_keeping_esi() {
	__asm {
		call th13_present_hook
		xor esi, esi
		ret
	}
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax. The three sites lay their arguments out over a different
// number of bytes, hence the size.
static void th13_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th13_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x5B };
		patch_bytes(0x0045D334, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0045C5D7, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0045C5EB, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x0045C607, th13_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0045C42F, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x0045C434, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB, 0x1D };
		patch_bytes(0x00448E6F, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th13_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x0045CFFF, 0x0045D452, 0x0045D637 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th13_render_hook);
				++render_patched;
			} else {
				printf("th13 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the three frame functions. Each site starts by loading the
		// device (5 bytes, exactly what a hooked call needs) and then pushes its arguments and
		// calls Present over the number of bytes below, which are skipped over so the code after
		// them runs with the result the hook left in eax.
		static const DWORD present_sites[] = { 0x0045D207, 0x0045D4B6, 0x0045D709 };
		static const size_t present_setup_size[] = { 12, 14, 12 };
		unsigned int present_patched = 0;
		for (size_t i = 0; i < sizeof(present_sites) / sizeof(present_sites[0]); ++i) {
			BYTE* present_site = (BYTE*)present_sites[i];
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004DC6A8) {
				// The middle one also zeroes esi among its arguments (see the trampoline)
				patch_call(present_sites[i], i == 1 ? (void*)th13_present_hook_keeping_esi : (void*)th13_present_hook);
				th13_skip_present_setup(present_sites[i] + 5, present_setup_size[i]);
				++present_patched;
			} else {
				printf("th13 present call at 0x%x doesn't look like a known one, skipping!\n", present_sites[i]);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th14 and th15
		overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th13_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004E49F0;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
