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

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// th19 is a 1v1 game, so it doesn't have one bullet manager: it has one per side, and the side
// table they are kept in is at 0x5D1A60 with a stride of 0x3C - side 0's manager is the dword
// there, side 1's the one 0x3C later (the table's own layout was read out of the constructor
// FUN_00530AE0, which stores the object it builds at table + 0x14, and out of the bullet
// manager's constructor FUN_004F6470, which stores itself at +0). The object is a pool of 2001
// bullets of 0x1088 bytes, plus two tasks (the update at 0x25 and the draw at 0x29).
//
// A bullet keeps its position at +0x650 (three floats) and its velocity at +0x65C (three), and
// the game moves it the way th18's bullets move: position += velocity * the global scale. The
// code that does it is at 0x4F5D4E, inside the per bullet update FUN_004F5680:
//
//     0x4F5D4E  test [bullet+0x10], 200h      a held bullet is not moved (and not drawn)
//     0x4F5D55  jne  0x4F5E64
//     0x4F5D5B  movss xmm2, [0x5C6418]        the scale the game applies to every movement
//     0x4F5D69  mulss xmm0, [bullet+0x65C]    velocity.x
//     0x4F5D71  mulss xmm1, [bullet+0x660]    velocity.y
//     0x4F5D79  mulss xmm2, [bullet+0x664]    velocity.z
//     0x4F5D81  addss xmm0, [bullet+0x650]    + position.x
//     0x4F5D89  addss xmm1, [bullet+0x654]    + position.y
//     0x4F5D91  addss xmm2, [bullet+0x658]    + position.z
//
// with the velocity itself coming out of the bullet's speed and angle (FUN_004F08A0:
// cos([bullet+0x670]) * the speed into +0x65C, sin(...) into +0x660).
//
// Two things about the scale: the game doesn't necessarily move a bullet by the global scale as
// it stands - before it moves one whose state (+0x1044) isn't 2, 3 or 4 it copies two of that
// bullet's own factors into the global ([bullet+0x1038] * [bullet+0x1024], see the top of
// FUN_004F5680), so the scale a bullet moves by is the one it carries. The global dword is a
// scratch the sub-functions read, not the per frame setting th18's is, so it isn't read here.
// The state 2/3/4 bullets keep whatever the scale already was; they are moved with 1 (the value
// the per bullet code leaves behind), which is what the other games' half stepping amounts to
// being absent here.
//
// The bullets in play are the ones on the six layer chains the update walk (FUN_004F4C30) builds
// every frame: the heads are at manager+0x14 (the tails at +0x2C), one chain per layer, threaded
// through bullet+0x6B8, and the layer a bullet belongs to is at +0x6C0. That walk is the only
// thing that moves bullets - the wrapper that also moves them, FUN_004F55C0, isn't called from
// anywhere in this build - and the extra presentations only run the render walk, so a bullet is
// never stepped twice by them.
//
// The sprites are the other half: the update walk copies a bullet's position into the sprite
// object it found for the bullet's id at +0x64C, and it is that copy the draw walk puts on
// screen. Since the update walk isn't replayed, an extra presentation has to move the copy as
// well as the bullet. The sprite is looked up here rather than through the game's own lookup
// (FUN_004CB8B0, whose array branch is reproduced): slot = [0x5D19BC] + 0x754 + (id & 0xFFFF) *
// 0x64C, with the id at +0x4D8, the in use byte at +0x644 and the position at +0x614.

// The side table the bullet managers are kept in, and how far apart its entries are
static char* th19_side_table = (char*)rebase(0x005D1A60);
static const size_t th19_side_stride = 0x3C;

// In a bullet manager: the six layer chains the update walk builds (heads, then tails), and in a
// bullet: where it is, where it is going, and what it is doing
static const size_t th19_bullet_layers = 6;
static const size_t th19_bullet_layer_head = 0x14;
static const size_t th19_bullet_layer_next = 0x6B8;
static const size_t th19_bullet_position = 0x650;
static const size_t th19_bullet_velocity = 0x65C;
static const size_t th19_bullet_flags = 0x10;     // 0x200 holds it (it is neither moved nor drawn)
static const size_t th19_bullet_state = 0x1044;
static const size_t th19_bullet_scale_a = 0x1038; // the two factors the game copies into the
static const size_t th19_bullet_scale_b = 0x1024; // global scale before it moves the bullet
static const size_t th19_bullet_sprite = 0x64C;   // the id of the sprite that draws it

// In the sprite manager: how to find a sprite by id, and where its drawn position is
static const size_t th19_sprite_stride = 0x64C;
static const size_t th19_sprite_base = 0x754;
static const size_t th19_sprite_in_use = 0x644;
static const size_t th19_sprite_id = 0x4D8;
static const size_t th19_sprite_position = 0x614;

// How many of the game's updates it runs per rendered frame, minus one (the game never writes it)
static unsigned char* th19_frames_per_render = (unsigned char*)rebase(0x0062BDDC);
// The game's own thread/state object, whose flags say whether the picture is held
static void** th19_game_object = (void**)rebase(0x005D1A54);
static const size_t th19_game_flags = 0x140;

// The sprite a bullet's id names, or null if there is none (the array branch of the game's own
// lookup, reproduced - see the comment above)
static void* th19_sprite_for_id(unsigned int id) {
	void* manager = *th19_sprite_manager;
	if (manager == nullptr || id == 0)
		return nullptr;

	if ((id & 0xFFFF) == 0xFFFF) {
		// The ids 0xFFFF and up are kept on the manager's own lists instead (two of them, each with
		// two ends), whose nodes hold the sprite and the next node
		static const size_t heads[] = { 0x714, 0x71C, 0x734, 0x73C };
		for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); ++i) {
			for (void* node = *(void**)((char*)manager + heads[i]); node != nullptr;
				node = *(void**)((char*)node + 4)) {
				void* sprite = *(void**)node;
				if (sprite != nullptr && *(unsigned int*)((char*)sprite + th19_sprite_id) == id)
					return sprite;
			}
		}
		return nullptr;
	}

	char* sprite = (char*)manager + th19_sprite_base + (size_t)(id & 0xFFFF) * th19_sprite_stride;
	if (sprite[th19_sprite_in_use] == 0 || *(unsigned int*)(sprite + th19_sprite_id) != id)
		return nullptr;
	return sprite;
}

// Whether the picture is held: the bullet tasks stop for these, and a picture the game is not
// updating must not be moved forward either
static bool th19_bullets_held() {
	void* game = *th19_game_object;
	if (game == nullptr)
		return false;
	// The update task stops for the low bits and for 0x800, the draw task for bit 2 (see
	// FUN_004F4950 and FUN_004F4C30)
	const unsigned int flags = *(unsigned int*)((char*)game + th19_game_flags);
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0 || (flags & 0x800) != 0;
}

// The scale a bullet moves by: the one the game copies into its global scale before moving it, or
// 1 for the states that don't get that copy and for anything that doesn't look like a scale
static float th19_bullet_step_scale(const void* bullet) {
	const int state = *(const int*)((const char*)bullet + th19_bullet_state);
	if (state == 2 || state == 3 || state == 4)
		return 1.0f;

	float scale = *(const float*)((const char*)bullet + th19_bullet_scale_a) *
		*(const float*)((const char*)bullet + th19_bullet_scale_b);
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;
	return scale;
}

// The bullets that were moved for the presentation being drawn, where they were, and where the
// sprites that draw them were. Both sides together can have 2001 bullets each, so a little more
// than that is the most this list can hold; only the bullets that were saved are moved, so a list
// that somehow grew past that would only cost the projection of the bullets at its end.
static const size_t th19_bullets_saved_max = 2001 * 2;
struct Th19SavedBullet {
	float* position;      // in the bullet: where it is
	float position_value[3];
	float* sprite;        // in the sprite that draws it: where it is drawn, or null
	float sprite_value[3];
};
static Th19SavedBullet th19_saved_bullets[th19_bullets_saved_max];
static size_t th19_saved_bullets_count;

// What the last presentation projected, for the diagnostics (see th19_diag_dump)
static unsigned int th19_bullets_in_play;
static unsigned int th19_bullets_moved;
static unsigned int th19_bullets_no_sprite_id;   // drawn without a sprite object of their own
static unsigned int th19_bullets_no_sprite;      // ...or with an id no sprite was found for

// Moves every bullet in play forward by t frames (t = 0 is the state the game is in) and remembers
// what it changed, so th19_bullets_undo can put it back.
static void th19_bullets_advance(float t) {
	th19_saved_bullets_count = 0;
	th19_bullets_in_play = 0;
	th19_bullets_moved = 0;
	th19_bullets_no_sprite_id = 0;
	th19_bullets_no_sprite = 0;

	if (th19_bullets_held())
		return;

	unsigned int frames_per_render = *th19_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	for (size_t side = 0; side < 2; ++side) {
		char* manager = *(char**)(th19_side_table + th19_side_stride * side);
		if (manager == nullptr)
			continue;

		for (size_t layer = 0; layer < th19_bullet_layers; ++layer) {
			void* bullet = *(void**)(manager + th19_bullet_layer_head + layer * 4);
			// The list is the game's own and this runs where nothing can change it; the bound is
			// only so a list that got broken some other way can't run away with the frame
			for (size_t guard = 0; bullet != nullptr && guard < th19_bullets_saved_max * 4; ++guard) {
				void* next = *(void**)((char*)bullet + th19_bullet_layer_next);
				const unsigned int flags = *(const unsigned int*)((char*)bullet + th19_bullet_flags);
				// A held bullet is neither moved nor drawn, so there is nothing to project for it
				if ((flags & 0x200) == 0) {
					++th19_bullets_in_play;
					if (th19_saved_bullets_count < th19_bullets_saved_max) {
						const float step = t * (float)(frames_per_render + 1) * th19_bullet_step_scale(bullet);
						float* position = (float*)((char*)bullet + th19_bullet_position);
						const float* velocity = (const float*)((char*)bullet + th19_bullet_velocity);
						Th19SavedBullet& saved = th19_saved_bullets[th19_saved_bullets_count++];
						saved.position = position;

						// The sprite's copy of the position is what the draw walk puts on screen, and
						// the update walk that writes it isn't replayed - so it has to be moved too
						const unsigned int sprite_id =
							*(const unsigned int*)((char*)bullet + th19_bullet_sprite);
						if (sprite_id == 0)
							++th19_bullets_no_sprite_id;
						void* sprite = th19_sprite_for_id(sprite_id);
						saved.sprite = sprite != nullptr
							? (float*)((char*)sprite + th19_sprite_position)
							: nullptr;
						if (saved.sprite == nullptr && sprite_id != 0)
							++th19_bullets_no_sprite;

						for (int axis = 0; axis < 3; ++axis) {
							saved.position_value[axis] = position[axis];
							position[axis] += velocity[axis] * step;
						}
						if (saved.sprite != nullptr) {
							for (int axis = 0; axis < 3; ++axis) {
								saved.sprite_value[axis] = saved.sprite[axis];
								saved.sprite[axis] = position[axis];
							}
						}
						++th19_bullets_moved;
					}
				}
				bullet = next;
			}
		}
	}
}

// Puts the positions back, so the game's own logic sees the state it left behind and not the
// projected one.
static void th19_bullets_undo() {
	for (size_t i = 0; i < th19_saved_bullets_count; ++i) {
		const Th19SavedBullet& saved = th19_saved_bullets[i];
		for (int axis = 0; axis < 3; ++axis) {
			saved.position[axis] = saved.position_value[axis];
			if (saved.sprite != nullptr)
				saved.sprite[axis] = saved.sprite_value[axis];
		}
	}
	th19_saved_bullets_count = 0;
}

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
	fprintf(file, "bullets: %u in play, %u projected forward (both sides, last presentation)\n",
		th19_bullets_in_play, th19_bullets_moved);
	// Not an error either way: a bullet without a sprite id is drawn from its own position (the
	// draw walk copies it there itself, see th19_extra_presentation), and a bullet whose sprite
	// wasn't found is one the game's own update walk doesn't find one for either - it looks the
	// same id up the same way. Both numbers are here to tell those from a lookup that broke.
	fprintf(file, "bullets: %u had no sprite id, %u had an id whose sprite wasn't found\n",
		th19_bullets_no_sprite_id, th19_bullets_no_sprite);

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
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed.
static void th19_extra_presentation(float t) {
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

	// The queue was just reset, so the objects have to be drawn again to fill it. The bullets are
	// drawn where they will be t frames from now; the render bakes the positions into the sprite
	// queue, so they can go back right after it.
	ToolState::ReadyToDraw();
	th19_bullets_advance(t);
	th19_render_objects();
	th19_bullets_undo();
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

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along
					th19_extra_presentation((float)k / (float)(extras + 1));
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
