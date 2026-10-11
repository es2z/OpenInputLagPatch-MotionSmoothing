// Touhou 11: Subterranean Animism v1.00a

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou11.h"

using namespace Touhou11;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004A8EB8;

int __stdcall th11_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager at 0x4A8D68, which FUN_00408390 allocates as
// a single 0x46D678 byte block (the manager the game itself is, since the stage build stores it
// in that global) and FUN_00408450 then fills in: it loads bullet.anm into manager+0x46D674 (the
// four bytes of the block that aren't part of the pool), points manager+0x10 at the pool, which
// starts at manager+0x64 and holds 2001 bullets of 0x910 bytes each, and registers the manager's
// two tasks - the update one (thunk 0x408EC0) and the draw one (thunk 0x408EF0). Every bullet
// carries an animation object of its own at +0x08.
//
// The manager's update (FUN_00408D00) runs the bullets and rebuilds the six in-play lists at
// manager+0x14 - one head and one tail per column, so manager+0x14..0x28 are the heads and
// manager+0x2C..0x40 the tails, exactly the layout th12 uses. It walks the pool (2000 slots,
// FUN_00408D00 from manager+0x64 with the state at +0x4B2 deciding) and, for every bullet that is
// in play, runs it through FUN_00408900 and threads it onto the list of the column the bullet
// keeps as a float at +0x4CC; the bullet's link to the next one of its column is its own +0x4B8,
// and the manager counts the ones in play at +0x5C. The manager's draw (FUN_00408E00 and
// FUN_00408E20, which the draw thunk calls once per column) walks those same lists, which makes
// them exactly the bullets that are drawn - so that is the list walked here, and it is read after
// the game's own update ran, which is the case by the time the frame is presented.
//
// A bullet moves by its velocity: FUN_00408900 adds the three floats at +0x448/+0x44C/+0x450 to
// the position at +0x43C/+0x440/+0x444, scaled by the global 0x4A7948 (1.0 unless the game slows
// everything down because the screen is full enough), once per frame. It moves at half of that
// while its state at +0x4B2 is 2 or 3 (0 is free, 1 is a bullet in flight; the half is the double
// at 0x497E60, which is 0.5), and not at all in any other state. The states are what 0x408Fxx
// switches on inside FUN_00408900, and the two half-speed paths are the same code as the full
// speed one with that one factor more.
//
// The drawn sprites follow on their own: the draw pass copies this position into the sprite fields
// at +0x3E4/+0x3E8/+0x3EC (adding the play field's own origin, the doubles at 0x497EE8, 0x497EA0
// and 0x497EE0) and hands the animation object at +0x08 to the sprite queue (0x451EF0), so moving
// the position is enough - the same rule as in th12 to th15.

static void** const th11_bullet_manager = (void**)0x004A8D68;
static const size_t th11_bullet_lists = 0x14;      // in the manager: the first column's head
static const size_t th11_bullet_columns = 6;       // ...and how many columns there are
static const size_t th11_bullet_next = 0x4B8;      // in a bullet: the next one of its column
static const size_t th11_bullet_position = 0x43C;  // in a bullet: its position, 3 floats
static const size_t th11_bullet_velocity = 0x448;  // in a bullet: what it moves by, 3 floats
static const size_t th11_bullet_state = 0x4B2;     // in a bullet: what it's doing, 16 bits
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th11_time_scale = (float*)0x004A7948;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th11_frames_per_render = (unsigned char*)0x004C3466;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th11_pause_object = (void**)0x004A8E88;
static const size_t th11_pause_flags = 0x60;

// The pool holds 2001 bullets, so the lists of the ones in play can't be longer
static const size_t th11_bullets_saved_max = 2001;
struct Th11SavedBullet {
	void* bullet;
	float position[3];
};
static Th11SavedBullet th11_saved_bullets[th11_bullets_saved_max];
static size_t th11_saved_bullets_count;

// What the last game frame had in play and how much of it was projected forward, for the
// diagnostics dump below
static unsigned int th11_bullets_in_play;
static unsigned int th11_bullets_moved;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either. 0x408EC0 and 0x408EF0 are the game's two bullet
// tasks, and this is the same test each of them makes - plus the one the pool walk makes per
// bullet (a held picture leaves the bullets in their lists but doesn't run them).
static bool th11_bullets_held() {
	void* pause = *th11_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th11_pause_flags);
	if (((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0)
		return true;
	return (flags & 2) != 0 && (flags & 0x400) != 0;
}

// How much of a frame's movement a bullet in this state gets (FUN_00408900)
static float th11_bullet_step_factor(unsigned short state) {
	switch (state) {
	case 1:
		return 1.0f;
	case 2:
	case 3:
		return 0.5f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames and remembers what it changed
static void th11_bullets_advance(float t) {
	th11_saved_bullets_count = 0;
	th11_bullets_in_play = 0;
	th11_bullets_moved = 0;

	void* manager = *th11_bullet_manager;
	if (manager == nullptr || th11_bullets_held())
		return;

	unsigned int frames_per_render = *th11_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th11_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	for (size_t column = 0; column < th11_bullet_columns; ++column) {
		void* bullet = *(void**)((char*)manager + th11_bullet_lists + column * 4);
		for (size_t guard = 0; bullet != nullptr && guard < th11_bullets_saved_max; ++guard) {
			void* next = *(void**)((char*)bullet + th11_bullet_next);

			++th11_bullets_in_play;
			const float factor = th11_bullet_step_factor(*(unsigned short*)((char*)bullet + th11_bullet_state));
			if (factor != 0.0f && th11_saved_bullets_count < th11_bullets_saved_max) {
				float* position = (float*)((char*)bullet + th11_bullet_position);
				const float* velocity = (const float*)((char*)bullet + th11_bullet_velocity);
				Th11SavedBullet& saved = th11_saved_bullets[th11_saved_bullets_count++];
				saved.bullet = bullet;

				const float bullet_step = step * factor;
				for (int axis = 0; axis < 3; ++axis) {
					saved.position[axis] = position[axis];
					position[axis] += velocity[axis] * bullet_step;
				}
				++th11_bullets_moved;
			}

			bullet = next;
		}
	}
}

// Puts the positions of every bullet th11_bullets_advance moved back
static void th11_bullets_undo() {
	for (size_t i = 0; i < th11_saved_bullets_count; ++i) {
		const Th11SavedBullet& saved = th11_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th11_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th11_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th11 draws a game frame the way th12 to th15 do. The block that does it sits in three frame
// functions, one per kind of scene the game can be in, and all three are the same:
//
//     0x446152  mov eax, [0x4C3288]       the device
//     0x446159  [ecx+0xA4]                BeginScene
//     0x446162  [0x4C3268]                reset the sprite queue, inlined here
//     0x44617F  mov edi, 0x4C3280         the render context, for the call below
//     0x446184  [0x4C3C10] = 0xFF
//     0x44618E  FUN_0042A300              prepare the render context (its object in EDI)
//     0x446193  FUN_00456E10              render every object (they fill the sprite queue)
//     0x446198  mov esi, [0x4C3268]       the sprite queue's owner, for the call below
//     0x44619E  FUN_0044FD10              draw the sprite queue (its object in ESI)
//     0x4461AA  [ecx+0x104]               SetTexture(0, NULL)
//     0x4461BC  [ecx+0xA8]                EndScene
//
// The other two blocks are at 0x446530 and 0x4466EF, with the same instructions at different
// addresses (in FUN_00446420, the window update, and FUN_00446650). Each frame function then calls
// a helper of its own right after its EndScene, and that is where it presents: 0x446382 in
// FUN_004461F0, 0x4465D6 in FUN_004465D0 and 0x4467F9 in FUN_00446790. Every one of them is a 5
// byte load of the device followed by the arguments being pushed and the present called over the
// 16 bytes after it.
//
// The sprite queue is what the game's own sprites are queued in before they are drawn in one batch:
// its owner is [0x4C3268], the queue itself is a buffer inside it at +0x435624 with the number of
// queued sprites at +0x435620 and two pointers to its ends at +0x7B5624/+0x7B5628 (FUN_0044FCE0
// resets it, FUN_0044FD10 draws it and resets it again). The frame function inlines the four stores
// the reset does instead of calling it, and the extra presentations below do the same through the
// function, which is the same four stores.
//
// Two of the calls take their object in a register instead of on the stack, which is how the
// compiler that built this game left them, so they can't be reached through a function pointer -
// the trampolines below are what put the object where each one expects it. Both registers are
// callee-saved and are put back before returning: leaving them holding the object hands the caller
// something other than what it had, which is what broke the th13 version of this code (it crashed
// in the SetTexture right after the sprite queue was drawn).
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

static void* const th11_prepare_render_address = (void*)0x0042A300;
static void* const th11_draw_sprite_queue_address = (void*)0x0044FD10;

static void __declspec(naked) th11_prepare_render() {
	__asm {
		push edi
		mov edi, 0x004C3280
		call dword ptr [th11_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th11_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x004C3268]
		call dword ptr [th11_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// The plain calls the render block makes into the game
static auto th11_reset_sprite_queue = (void(*)())0x0044FCE0;
static auto th11_render_objects = (int(*)())0x00456E10;

// Where the user's tool builds its frame and where it draws it. thprac's th11 module starts its
// frame from a hook on the return of the scene update at 0x456DEB and draws the overlay it built
// from a hook on the return of the object render at 0x456F12 (see tool_state.h); both are the ret
// of the walk the render block above calls into.
static unsigned char* const th11_tool_frame_address = (unsigned char*)0x00456DEB;
static unsigned char* const th11_tool_draw_address = (unsigned char*)0x00456F12;

// What the scene is drawn with, taken from the frame the game's own render ran in. Each of the
// three frame functions, right before it renders, makes its 2D context the current one (0x4C359C
// into [0x4C37CC]), sets the viewport from it (FUN_00446080 at 0x4460D6 to 0x4460F1) and calls
// FUN_0042A810 on it, which builds the view and projection matrices in the context (at +0x4C and
// +0x8C) and hands them to SetTransform, and copies the two values at +0xE8/+0xEC of it into the
// sprite manager (at +0xB0/+0xB4) - those are what places the sprite queue on screen. So what an
// extra presentation has to put back is whatever the game's frame left: the user's tool draws its
// overlay with a viewport, a projection and a screen placement of its own, and an extra
// presentation runs after that.
static D3DVIEWPORT9 th11_scene_viewport = {};
static D3DMATRIX th11_scene_view = {};
static D3DMATRIX th11_scene_projection = {};
static bool th11_scene_transforms = false;
static void* const th11_render_context = (void*)0x004C37CC;  // the current 2D context
static void* const th11_sprite_manager = (void*)0x004C3268;
static DWORD th11_scene_context;
static DWORD th11_scene_sprite_offset[2];
static const size_t th11_sprite_screen_offset = 0xB0;        // in the sprite manager

// The device the game draws with. The present sites load it from the game's own global, so that is
// the device that has to present the frame - not the one the patch's D3D9 hook captured, which is
// the same object while that hook is in place and nothing at all if the game got its device in a
// way the hook doesn't see. Reporting a failed present to the game ends it (its device-lost
// handling takes over), so the device the game itself would have presented with comes first.
static IDirect3DDevice9* th11_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004C3288;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// The menus draw their text through a list of records the game keeps in the text object it holds at
// [0x4A8D58]: the game's own printf-for-text call appends a record (0x130 bytes each, at +0x87C -
// FUN_004014E0 does that, and FUN_004015A0 is the vsprintf wrapper in front of it) and bumps the
// frame's count of them (+0x1847C), and the drawing then walks the records that count covers
// (FUN_00401670, whose loop at 0x401C56 compares its index with that count). The list is worked
// over once per frame - FUN_004014A0, the compaction that drops the records whose timer ran out, is
// part of what the update walk does, which an extra presentation does not run - so an extra frame
// that draws the same screen while the list still holds the records the game's own frame appended
// draws that text a second time on top of itself, which the alpha blending these screens use turns
// into text that looks a little denser than the game's own frame shows.
//
// So the count the game's own run of the walk started from is kept here and put back before every
// extra presentation: each one then appends its own records from the start of the list, exactly the
// way the game's own frame did.
static const size_t th11_text_record_count = 0x1847C;
static void* const th11_text_object = (void*)0x004A8D58;
static unsigned int th11_text_records;

static unsigned int th11_text_count() {
	void* text = *(void**)th11_text_object;
	return text != nullptr ? *(unsigned int*)((char*)text + th11_text_record_count) : 0;
}

static void th11_text_records_save() {
	th11_text_records = th11_text_count();
}

static void th11_text_records_restore() {
	void* text = *(void**)th11_text_object;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th11_text_record_count) = th11_text_records;
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th11_extra_presentation for what it's for)
static void th11_render_hook() {
	IDirect3DDevice9* device = th11_device();
	if (device != nullptr) {
		device->GetViewport(&th11_scene_viewport);
		th11_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th11_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th11_scene_projection));
	}
	th11_scene_context = *(DWORD*)th11_render_context;
	void* manager = *(void**)th11_sprite_manager;
	if (manager != nullptr) {
		th11_scene_sprite_offset[0] = *(DWORD*)((char*)manager + th11_sprite_screen_offset);
		th11_scene_sprite_offset[1] = *(DWORD*)((char*)manager + th11_sprite_screen_offset + 4);
	}

	ToolState::WatchFrameReady(th11_tool_frame_address, th11_tool_draw_address);

	// What the game's own run of the walk finds the text list at, for the extra presentations to put
	// back (see th11_text_record_count)
	th11_text_records_save();

	th11_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
static double th11_present_rate() {
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
static const unsigned int th11_diag_frames_max = 300;
static const unsigned int th11_diag_presents_max = 8;
struct Th11DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th11_diag_presents_max];
};
static Th11DiagFrame th11_diag_frames[th11_diag_frames_max];
static unsigned int th11_diag_count;
static bool th11_diag_key_down;

static LARGE_INTEGER th11_perf_freq;

static void th11_diag_dump() {
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

	unsigned int count = th11_diag_count < th11_diag_frames_max ? th11_diag_count : th11_diag_frames_max;
	unsigned int start = th11_diag_count < th11_diag_frames_max ? 0 : th11_diag_count % th11_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th11_present_rate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th11_bullets_in_play, th11_bullets_moved);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th11DiagFrame& frame = th11_diag_frames[(start + i) % th11_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th11_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th11_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th11_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again. The tool's frame is readied again, so the object
// render below draws its overlay into this presentation the same way it does in the game's own one.
static void th11_extra_presentation(IDirect3DDevice9* device, float t) {
	device->BeginScene();
	if (th11_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th11_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th11_scene_projection);
	}
	if (th11_scene_viewport.Width != 0)
		device->SetViewport(&th11_scene_viewport);
	// The 2D context the game's own frame rendered with, and the screen placement it left in the
	// sprite queue (see th11_scene_viewport)
	*(DWORD*)th11_render_context = th11_scene_context;
	{
		void* manager = *(void**)th11_sprite_manager;
		if (manager != nullptr) {
			*(DWORD*)((char*)manager + th11_sprite_screen_offset) = th11_scene_sprite_offset[0];
			*(DWORD*)((char*)manager + th11_sprite_screen_offset + 4) = th11_scene_sprite_offset[1];
		}
	}
	th11_reset_sprite_queue();
	*(DWORD*)0x004C3C10 = 0xFF;
	th11_prepare_render();
	ToolState::ReadyToDraw();
	// The text list goes back to how the game's own run of the walk found it, so this presentation
	// draws the same text that one drew (see th11_text_record_count above)
	th11_text_records_restore();
	// Everything is drawn where it will be t frames from now; the render bakes those positions into
	// what it draws, so they can all go back right after it
	th11_bullets_advance(t);
	th11_render_objects();
	th11_bullets_undo();
	th11_draw_sprite_queue();
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
static HRESULT th11_present_hook() {
	IDirect3DDevice9* device = th11_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th11_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th11_diag_presents_max];
	present_time[0] = base;

	double rate = th11_present_rate();
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

					if (presents < th11_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along
					th11_extra_presentation(device, (float)k / (float)(extras + 1));
				}
			}
		}
	}

	{
		Th11DiagFrame& frame = th11_diag_frames[th11_diag_count % th11_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th11_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th11_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th11_diag_key_down) {
			th11_diag_key_down = true;
			th11_diag_dump();
		}
	} else {
		th11_diag_key_down = false;
	}

	return result;
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax.
static void th11_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th11_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x43 };
		patch_bytes(0x00446454, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00445877, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0044588B, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x004458A7, th11_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0044570E, Direct3DCreate9_hook);
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00436D5F, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th11_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x00446193, 0x00446571, 0x00446730 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th11_render_hook);
				++render_patched;
			} else {
				printf("th11 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the three frame functions' helpers. Each site starts by loading
		// the device (5 bytes, exactly what a hooked call needs) and then pushes its arguments and
		// calls Present over the 16 bytes below, which are skipped over so the code after them runs
		// with the result the hook left in eax.
		static const DWORD present_sites[] = { 0x00446382, 0x004465D6, 0x004467F9 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004C3288) {
				patch_call(address, (void*)th11_present_hook);
				th11_skip_present_setup(address + 5, 16);
				++present_patched;
			} else {
				printf("th11 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th12 to th15
		overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th11_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004C92A8;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
