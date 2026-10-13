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

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullet manager is the object the side table holds at 0x5BA568 (one record every 0x30
// bytes: the record's +0 is the manager and its +4 the player, the same shape th19 has). The
// thing that builds it is FUN_00480FA0: it loads bullet.anm into it, stores itself into the side
// record through FUN_004117A0 (which is `*record = this`), makes a pool of 2000 bullets of 0x528
// bytes at +0x60 (FUN_0047C6A0 is `this + count * 0x528`), and registers the two tasks the game
// drives it with - the update FUN_004849D0 (id 0x26) and the draw FUN_00485310 (id 0x29).
//
// The bullets in play are the ones on the six layer chains the update walk FUN_0047D600 builds
// every frame: the heads are at manager+0x14 (the tails at +0x2C), one chain per layer, threaded
// through bullet+0x60, and the layer a bullet belongs to is at +0x44 - all of that read out of
// FUN_0047D600 itself and confirmed by the draw walk (see below). That walk is the only thing
// that moves bullets, and the extra presentations only run the render walk, so a bullet is never
// stepped twice by them.
//
// A bullet keeps its position at +0x64 (three floats) and its velocity at +0x70 (three), and
// FUN_00485B60 (the per bullet update, which the walk reaches through FUN_0047D960) moves it like
// this:
//
//   state 1     position += velocity * the global scale at [0x5AEFE4]
//   state 2/3   position += velocity / 2
//
// The global the first one uses is not the per frame setting th18 had: before it moves a bullet
// whose state (+0x50) isn't 2, 3 or 4 the update copies that bullet's own two factors into it
// ([bullet+0x4D8] * [bullet+0x4E8], at the top of FUN_00485B60) and puts the old value back when
// it is done, so the scale a bullet moves by is the one it carries. The states 2, 3 and 4 never
// get that copy and divide their velocity by 2 instead, which is the half step the other games'
// states take - so the state is worked out per bullet here rather than the global being read (a
// scratch the sub-functions write, not the per frame setting).
//
// The sprites are the other half: the draw walk draws each of the six chains (FUN_0047DC90 calls
// FUN_0047DB70 once per layer) and FUN_0047DB70, for every bullet on its chain, copies
// bullet+0x64/+0x68/+0x6C into the sprite object it keeps at bullet+0x1C (at sprite+0x5BC). That
// copy happens inside the render walk the extra presentations replay, so moving the bullet's own
// position is all an extra presentation has to do, exactly like th19's bullets - there is no
// second copy to chase.

// In the side table: how far apart its records are, and the most of them to look at (th19 has two
// sides; this game has one player, but the engine's table is the same one, so the walk below takes
// every record it finds a manager in rather than assuming an index)
static char* th20_side_table = (char*)rebase(0x005BA568);
static const size_t th20_side_stride = 0x30;
static const size_t th20_side_max = 8;

// In a bullet manager: the six layer chains the update walk builds (heads, then tails) and the
// number of bullets it walked. In a bullet: where it is, where it is going, what it is doing, the
// two factors the game copies into the global scale before moving it, and the flags
static const size_t th20_bullet_layers = 6;
static const size_t th20_bullet_layer_head = 0x14;
static const size_t th20_bullet_layer_tail = 0x2C;
static const size_t th20_bullet_layer_next = 0x60;
static const size_t th20_bullet_count = 0x44;
static const size_t th20_bullet_layer = 0x44;
static const size_t th20_bullet_position = 0x64;
static const size_t th20_bullet_velocity = 0x70;
static const size_t th20_bullet_flags = 0x14;     // 0x200 holds it (it is neither moved nor drawn)
static const size_t th20_bullet_state = 0x50;
static const size_t th20_bullet_scale_a = 0x4D8;  // the two factors the game copies into the
static const size_t th20_bullet_scale_b = 0x4E8;  // global scale before it moves the bullet
static const size_t th20_bullet_pool_max = 2000;  // the pool FUN_00480FA0 makes

// How many of the game's updates it runs per rendered frame, minus one (the game never writes it)
static unsigned char* th20_frames_per_render = (unsigned char*)rebase(0x005C4F84);
// The game's own thread/state object, whose flags say whether the picture is held
static void** th20_game_thread = (void**)rebase(0x005BA828);
static const size_t th20_thread_flags = 0xE8;

// Whether a pointer looks like something that can be read, so a read that has gone stale can't
// take the game down
static bool th20_readable(const void* pointer, size_t size) {
	const DWORD address = (DWORD)pointer;
	return address > 0x10000 && address < 0x7FFF0000 && !IsBadReadPtr(pointer, size);
}

// Whether the picture is held: the bullet tasks stop for these flags (FUN_004849D0 returns early
// for the low bits, FUN_00485310 for bit 2, and FUN_0047D600 skips a bullet's update for 0x800),
// and a picture the game is not updating must not be moved forward either
static bool th20_bullets_held() {
	void* thread = *th20_game_thread;
	if (thread == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)thread + th20_thread_flags);
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0 || (flags & 0x800) != 0;
}

// The scale a bullet moves by: the one the game copies into its global scale before moving it, or
// the half step the states that never get that copy take
static float th20_bullet_step_scale(const void* bullet) {
	const int state = *(const int*)((const char*)bullet + th20_bullet_state);
	if (state == 2 || state == 3 || state == 4)
		return 0.5f;

	float scale = *(const float*)((const char*)bullet + th20_bullet_scale_a) *
		*(const float*)((const char*)bullet + th20_bullet_scale_b);
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;
	return scale;
}

// The side records that hold a bullet manager, for the diagnostics: how many were found and how
// many bullets they say they walked (the managers' own counts are an independent way to tell a
// walk that stopped early from an empty stage)
static char* th20_bullet_managers[th20_side_max];
static size_t th20_bullet_manager_count;
static unsigned int th20_bullets_managers_say;

// A manager, if the record holds one that looks like the ones this was written for: its count is
// within the pool it made, and the layer heads are readable
static char* th20_bullet_manager(size_t side) {
	char* manager = *(char**)(th20_side_table + th20_side_stride * side);
	if (manager == nullptr)
		return nullptr;
	if (!th20_readable(manager + th20_bullet_count, 4) ||
		!th20_readable(manager + th20_bullet_layer_head + th20_bullet_layers * sizeof(DWORD), 4))
		return nullptr;
	const unsigned int count = *(const unsigned int*)(manager + th20_bullet_count);
	if (count > th20_bullet_pool_max + 1)
		return nullptr;
	return manager;
}

// The bullets a presentation moved, where they were, so it can put them back. Both sides together
// can have 2000 bullets each, so a little more than that is the most this list can hold; only the
// bullets that were saved are moved, so a list that somehow grew past that would only cost the
// projection of the bullets at its end.
static const size_t th20_bullets_saved_max = th20_bullet_pool_max * 2;
struct Th20SavedBullet {
	float* position;      // in the bullet: where it is
	float position_value[3];
};
static Th20SavedBullet th20_saved_bullets[th20_bullets_saved_max];
static size_t th20_saved_bullets_count;

// What the last presentation projected, for the diagnostics (see th20_diag_dump)
static unsigned int th20_bullets_in_play;
static unsigned int th20_bullets_moved;

static void th20_bullet_find_managers();

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player
// ---------------------------------------------------------------------------
//
// The player is the object the side table holds at +4 (0x5BA56C, thprac's PLAYER_PTR), and it is a
// big one, the same kind of object as th19's PlayerInf: its own fields go past +0x14854, and
// thprac's th20 module reaches a per character timer at player+0x22B4+0x12580 and the per
// character cycle at player+0x684+0x12C*i, neither of which makes sense for anything else.
//
// Where it keeps its position was measured rather than read: while it was being played the whole
// object was watched for floats that move, and three x's came out with the same value and the same
// largest step of 4.5 pixels a frame (the player's speed - th19's player moves 4.5 as well):
// +0x614, +0x5E4 and +0x694. Two of them are the same point in the coordinates other parts of the
// game want: the y at +0x5E4 is 32 more than the y at +0x614 and the one at +0x694 is 32 less,
// which is the field's own offset from the top of the picture. The function at 0x4F7430 - the one
// thprac's th20 module hooks as its player state - writes the position as a vec3 at +0x614
// (0x4F74D1 adds 614h to the player and the three stores after it write the vector), which is what
// says that one is the player's own.
//
// Unlike the bullets there is no velocity to read: what the player moves by comes out of the input
// the game processed that frame, in per character code. It is measured instead, the way the games
// after th13 measure their players: the position the game's own presentation showed is kept, and so
// is how far the player moved to reach it, and an extra presentation draws the player that same
// position moved t of that step further along - extrapolation, so the picture is ahead of the
// game's own frame rather than behind it. A step larger than 48px isn't a step: the player died,
// respawned or the stage changed, and 0 is used for the frame the measurement starts on.
//
// All three copies of the position are moved - a presentation that moved only one of them would
// leave whichever one the drawing reads behind - and all three are put back right after the render
// walk, so the game's own logic never sees a projected position.

static const size_t th20_player_position = 0x614;
static const size_t th20_player_copies[] = { 0x614, 0x5E4, 0x694 };
static const size_t th20_player_copies_count = 3;

struct Th20PlayerMotion {
	void* player;      // the player object this was measured from
	bool tracked;
	float position[3]; // where the game's own presentation showed it
	float delta[3];    // how far it moved to get there (the last game frame's step)
};
static Th20PlayerMotion th20_player_motion;
static float* th20_player_written[th20_player_copies_count];
static float th20_player_written_value[th20_player_copies_count][3];
static unsigned int th20_players_moved;
static float th20_player_movement; // the largest step seen since the last dump, in pixels

// The player object, or null while there is none
static void* th20_player() {
	void* player = *(void**)(th20_side_table + 4);
	if (player == nullptr || !th20_readable((char*)player + th20_player_position, 12))
		return nullptr;
	return player;
}

// Measures where the player is and how far it moved, once per game frame, where the game has
// finished its own (in the present before the extra presentations run)
static void th20_player_measure() {
	Th20PlayerMotion& motion = th20_player_motion;
	void* player = th20_player();
	if (player == nullptr) {
		motion.tracked = false;
		return;
	}

	const float* position = (const float*)((char*)player + th20_player_position);
	float move_squared = 0.0f;
	for (int axis = 0; axis < 3; ++axis) {
		// A player that wasn't this object a frame ago has no step to project
		float delta = 0.0f;
		if (motion.tracked && motion.player == player) {
			delta = position[axis] - motion.position[axis];
			if (!(fabsf(delta) <= 48.0f))
				delta = 0.0f;
		}
		motion.delta[axis] = delta;
		motion.position[axis] = position[axis];
		move_squared += delta * delta;
	}
	// How far the player actually moved in the frame, not how far it moved along one axis: a step
	// that goes diagonally is the same speed as one that goes straight
	const float move = sqrtf(move_squared);
	if (move > th20_player_movement)
		th20_player_movement = move;
	motion.player = player;
	motion.tracked = true;
}

// Moves the player t of the last frame's step ahead of where the game's presentation left it
static void th20_player_advance(float t) {
	for (size_t i = 0; i < th20_player_copies_count; ++i)
		th20_player_written[i] = nullptr;
	th20_players_moved = 0;

	const Th20PlayerMotion& motion = th20_player_motion;
	if (!motion.tracked || motion.player == nullptr || th20_bullets_held())
		return;

	for (size_t i = 0; i < th20_player_copies_count; ++i) {
		float* position = (float*)((char*)motion.player + th20_player_copies[i]);
		if (!th20_readable(position, 12))
			continue;
		for (int axis = 0; axis < 3; ++axis) {
			th20_player_written_value[i][axis] = position[axis];
			position[axis] += motion.delta[axis] * t;
		}
		th20_player_written[i] = position;
	}
	++th20_players_moved;
}

static void th20_player_undo() {
	for (size_t i = 0; i < th20_player_copies_count; ++i) {
		if (th20_player_written[i] == nullptr)
			continue;
		for (int axis = 0; axis < 3; ++axis)
			th20_player_written[i][axis] = th20_player_written_value[i][axis];
		th20_player_written[i] = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player's own shots
// ---------------------------------------------------------------------------
//
// The shots live in a pool the player owns: 256 entries of 0x124 bytes starting at player+0x22B4.
// The player's own constructor says so - it calls FUN_004F4B80 on player+0x22B4 (0x4F4968), and
// that function clears the block's tail at +0x12580, which is exactly the
// player+0x22B4+0x12580 thprac's th20 module keeps its per character timers in. What makes the
// pool itself is FUN_004F41D0: FUN_0040BC20(pool, 0x124, 0x100, FUN_004F4CB0), a pool of 0x100
// (256) entries of 0x124 bytes, each handed to FUN_004F4CB0 to be initialized. The pool's per
// frame pass is FUN_00504950 (called on player+0x22B4 at 0x4F5383).
//
// An entry keeps its position at +0x50 (three floats) - the same offset the objects of this kind
// are drawn from: their drawing code hands obj+0x50 to FUN_0040BDA0 (which is `return this`) and
// copies the three floats it gets back into the sprite's drawn position at sprite+0x5BC
// (0x5046C2, 0x504B86 and 0x5059ED all do that). Those copies happen in whatever walk draws them,
// which is the one the extra presentations replay, so moving the entry's own position is all an
// extra presentation has to do - there is no second copy to chase.
//
// There is no velocity to read and no list to walk from here: the pool's entries are all in one
// array, and which of them are in play is worked out by measuring, the way the player itself is.
// An entry that is not in use doesn't move, so it has no step to project and is left alone; one
// that was just handed a new shot jumps, so its step is thrown away for that frame (anything past
// 48px is a respawn rather than a step). That also means no state has to be kept about which
// entries the game considers live.

static const size_t th20_shot_pool = 0x22B4;   // in the player: the first of 256 slots of 0x124
static const size_t th20_shot_slots = 0x100;
static const size_t th20_shot_stride = 0x124;
static const size_t th20_shot_position = 0x50; // in a slot: its position, 3 floats
static const size_t th20_shot_sprite_id = 0x18; // ...and the id of the sprite that draws it

// In the sprite (animation) manager: how to find a sprite by id, and where its drawn position is.
// The game looks a sprite up in FUN_0044CD00, whose array branch is reproduced here:
// sprite = [0x5C0028] + 0x710 + (id & 0xFFFF) * 0x600 (FUN_004494D0 is `this + n * 0x600`),
// the id it is for is at +0x4C0 (FUN_00449460 compares it), the in use byte is at +0x5F8, and the
// three floats the drawing puts on screen are at +0x5BC.
static const size_t th20_sprite_stride = 0x600;
static const size_t th20_sprite_base = 0x710;
static const size_t th20_sprite_in_use = 0x5F8;
static const size_t th20_sprite_id = 0x4C0;
static const size_t th20_sprite_position = 0x5BC;

static char* th20_sprite_for_id(unsigned int id) {
	char* manager = (char*)*th20_sprite_manager;
	if (manager == nullptr || id == 0 || (id & 0xFFFF) == 0xFFFF)
		return nullptr;
	char* sprite = manager + th20_sprite_base + (size_t)(id & 0xFFFF) * th20_sprite_stride;
	if (!th20_readable(sprite + th20_sprite_position, th20_sprite_in_use - th20_sprite_position + 1) ||
		sprite[th20_sprite_in_use] == 0 || *(unsigned int*)(sprite + th20_sprite_id) != id)
		return nullptr;
	return sprite;
}

struct Th20ShotMotion {
	void* player;
	bool tracked;
	float position[3];
	float delta[3];
};
static Th20ShotMotion th20_shot_motion[th20_shot_slots];
static float* th20_shot_written[th20_shot_slots];
static float th20_shot_written_value[th20_shot_slots][3];
static float* th20_shot_sprite_written[th20_shot_slots];
static float th20_shot_sprite_written_value[th20_shot_slots][3];
static unsigned int th20_shots_in_play;
static unsigned int th20_shots_moved;
static unsigned int th20_shots_with_sprite;
static unsigned int th20_shots_without_sprite;

static float* th20_shot_position_at(char* player, size_t slot) {
	char* position = player + th20_shot_pool + th20_shot_stride * slot + th20_shot_position;
	return th20_readable(position, 12) ? (float*)position : nullptr;
}

// Measures where each shot is and how far it moved, once per game frame (see th20_player_measure)
static void th20_shots_measure() {
	th20_shots_in_play = 0;
	char* player = (char*)th20_player();
	for (size_t slot = 0; slot < th20_shot_slots; ++slot) {
		Th20ShotMotion& motion = th20_shot_motion[slot];
		const float* position = player != nullptr ? th20_shot_position_at(player, slot) : nullptr;
		if (position == nullptr) {
			motion.tracked = false;
			continue;
		}

		float move_squared = 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			float delta = 0.0f;
			if (motion.tracked && motion.player == player) {
				delta = position[axis] - motion.position[axis];
				if (!(fabsf(delta) <= 48.0f))
					delta = 0.0f;
			}
			motion.delta[axis] = delta;
			motion.position[axis] = position[axis];
			move_squared += delta * delta;
		}
		motion.player = player;
		motion.tracked = true;
		// An entry the game isn't using stayed where it was, which is what tells the two apart
		if (move_squared > 0.0f)
			++th20_shots_in_play;
	}
}

static void th20_shots_advance(float t) {
	for (size_t slot = 0; slot < th20_shot_slots; ++slot) {
		th20_shot_written[slot] = nullptr;
		th20_shot_sprite_written[slot] = nullptr;
	}
	th20_shots_moved = 0;
	th20_shots_with_sprite = 0;
	th20_shots_without_sprite = 0;

	if (th20_bullets_held())
		return;

	for (size_t slot = 0; slot < th20_shot_slots; ++slot) {
		const Th20ShotMotion& motion = th20_shot_motion[slot];
		if (!motion.tracked || motion.player == nullptr)
			continue;
		// An entry the game isn't using is one that didn't move; there is nothing to project for it
		if (motion.delta[0] == 0.0f && motion.delta[1] == 0.0f && motion.delta[2] == 0.0f)
			continue;
		float* position = th20_shot_position_at((char*)motion.player, slot);
		if (position == nullptr)
			continue;
		for (int axis = 0; axis < 3; ++axis) {
			th20_shot_written_value[slot][axis] = position[axis];
			position[axis] += motion.delta[axis] * t;
		}
		th20_shot_written[slot] = position;
		++th20_shots_moved;

		// The sprite the shot is drawn through keeps its own copy of the position, and the game
		// writes it while it updates the shot - which the extra presentations don't replay, so it
		// has to be moved here as well (see th20_sprite_for_id and the comment above)
		const size_t offset = th20_shot_pool + th20_shot_stride * slot;
		const unsigned int id = *(const unsigned int*)((char*)motion.player + offset + th20_shot_sprite_id);
		char* sprite = th20_sprite_for_id(id);
		if (sprite == nullptr) {
			++th20_shots_without_sprite;
			continue;
		}
		float* drawn = (float*)(sprite + th20_sprite_position);
		for (int axis = 0; axis < 3; ++axis) {
			th20_shot_sprite_written_value[slot][axis] = drawn[axis];
			drawn[axis] = position[axis];
		}
		th20_shot_sprite_written[slot] = drawn;
		++th20_shots_with_sprite;
	}
}

static void th20_shots_undo() {
	for (size_t slot = 0; slot < th20_shot_slots; ++slot) {
		float* position = th20_shot_written[slot];
		if (position != nullptr) {
			for (int axis = 0; axis < 3; ++axis)
				position[axis] = th20_shot_written_value[slot][axis];
			th20_shot_written[slot] = nullptr;
		}
		float* drawn = th20_shot_sprite_written[slot];
		if (drawn != nullptr) {
			for (int axis = 0; axis < 3; ++axis)
				drawn[axis] = th20_shot_sprite_written_value[slot][axis];
			th20_shot_sprite_written[slot] = nullptr;
		}
	}
}

// Moves every bullet in play forward by t frames (t = 0 is the state the game is in) and remembers
// what it changed, so th20_bullets_undo can put it back.
static void th20_bullets_advance(float t) {
	th20_saved_bullets_count = 0;
	th20_bullets_in_play = 0;
	th20_bullets_moved = 0;

	// The managers are made when a game starts and the table is rebuilt with it, so which records
	// hold one is looked up every frame rather than once (eight pointer reads)
	th20_bullet_find_managers();
	if (th20_bullets_held())
		return;

	unsigned int frames_per_render = *th20_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	for (size_t side = 0; side < th20_bullet_manager_count; ++side) {
		char* manager = th20_bullet_managers[side];
		if (manager == nullptr)
			continue;

		for (size_t layer = 0; layer < th20_bullet_layers; ++layer) {
			void* bullet = *(void**)(manager + th20_bullet_layer_head + layer * 4);
			// The list is the game's own and this runs where nothing can change it; the bound is
			// only so a list that got broken some other way can't run away with the frame
			for (size_t guard = 0; bullet != nullptr && guard < th20_bullets_saved_max * 4; ++guard) {
				if (!th20_readable(bullet, th20_bullet_velocity + 12))
					break;
				void* next = *(void**)((char*)bullet + th20_bullet_layer_next);
				const unsigned int flags = *(const unsigned int*)((char*)bullet + th20_bullet_flags);
				// A held bullet is neither moved nor drawn, so there is nothing to project for it
				if ((flags & 0x200) == 0) {
					++th20_bullets_in_play;
					if (th20_saved_bullets_count < th20_bullets_saved_max) {
						const float step = t * (float)(frames_per_render + 1) * th20_bullet_step_scale(bullet);
						float* position = (float*)((char*)bullet + th20_bullet_position);
						const float* velocity = (const float*)((char*)bullet + th20_bullet_velocity);
						Th20SavedBullet& saved = th20_saved_bullets[th20_saved_bullets_count++];
						saved.position = position;
						for (int axis = 0; axis < 3; ++axis) {
							saved.position_value[axis] = position[axis];
							position[axis] += velocity[axis] * step;
						}
						++th20_bullets_moved;
					}
				}
				bullet = next;
			}
		}
	}
}

// Puts the positions back, so the game's own logic sees the state it left behind and not the
// projected one.
static void th20_bullets_undo() {
	for (size_t i = 0; i < th20_saved_bullets_count; ++i) {
		const Th20SavedBullet& saved = th20_saved_bullets[i];
		for (int axis = 0; axis < 3; ++axis)
			saved.position[axis] = saved.position_value[axis];
	}
	th20_saved_bullets_count = 0;
}

// Which of the side table's records hold a bullet manager, and how many bullets they say they
// walked (a second opinion on the walk, for the diagnostics)
static void th20_bullet_find_managers() {
	th20_bullet_manager_count = 0;
	th20_bullets_managers_say = 0;
	for (size_t side = 0; side < th20_side_max; ++side) {
		char* manager = th20_bullet_manager(side);
		if (manager == nullptr)
			continue;
		th20_bullet_managers[th20_bullet_manager_count++] = manager;
		th20_bullets_managers_say += *(unsigned int*)(manager + th20_bullet_count);
	}
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
	// The bullets (see th20_bullets_advance). The last line counts them from the other side - out
	// of each manager's own counter rather than by walking its chains - so a walk that stopped
	// finding bullets would say so right here: the only difference the two can have is a bullet
	// the draw walk itself leaves out, which is not on screen to project for
	fprintf(file, "bullets: %u in play, %u projected forward (last presentation)\n",
		th20_bullets_in_play, th20_bullets_moved);
	fprintf(file, "bullets: the managers' own count says %u, %u manager(s) found\n",
		th20_bullets_managers_say, (unsigned int)th20_bullet_manager_count);
	// The player (see th20_player_measure): what the last presentation moved, and how far the step
	// it projects from was
	fprintf(file, "player: %u moved, up to %.2f px a frame since the last dump\n",
		th20_players_moved, th20_player_movement);
	th20_player_movement = 0.0f;
	// The player's own shots (see th20_shots_measure): 256 slots, of which the ones in play are the
	// ones that moved
	fprintf(file, "shots: %u in play, %u projected (last presentation)\n",
		th20_shots_in_play, th20_shots_moved);
	fprintf(file, "shots: %u of the projected had the sprite that draws them (%u without)\n",
		th20_shots_with_sprite, th20_shots_without_sprite);

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
	th20_bullets_advance(t);
	th20_player_advance(t);
	th20_shots_advance(t);
	th20_render_objects(*th20_scene);
	th20_shots_undo();
	th20_player_undo();
	th20_bullets_undo();
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

	// Where the player is and how far it moved - once per game frame, where the game's own work for
	// it has finished (see th20_player_measure)
	th20_player_measure();
	th20_shots_measure();

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
