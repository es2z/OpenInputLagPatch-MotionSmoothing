// Touhou 6: Embodiment of Scarlet Devil v1.02h

#include <stdio.h>
#include <string.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d8_hook.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou6.h"
#include "config.h"

using namespace Touhou6;

CEngine* CEngine::Instance = (CEngine*)0x006C6D18;
CWindowManager* CWindowManager::Instance = (CWindowManager*)0x006C6BD4;
CChainManager* CChainManager::Instance = (CChainManager*)0x0069D918;
CGame* CGame::Instance = (CGame*)0x0069BCA0;

// The frame interpolation below draws a frame from the calculation chain hook when it is on - see
// the stage 0 comment at the end of this file. These are the parts of it the hook itself needs.
static bool th6_interpolating;
static bool th6_drew_frame;
static void th6_begin_game_draw();
static void th6_draw_pass();

// HACK: This should actually be taking in a parameter and using that in place of CChainManager::Instance
// However, if I did that, the edi register gets corrupted by CChainManager::UpdateCalcChain
// ...and that causes a crash because "this" is saved in that register in the generated assembly for this function
// Doing it this way seems to work fine. More investigation required...
int __fastcall th6_update_calc_chain_hook() {
	auto engine = CEngine::Instance;
    // The device only exists after the game initialises D3D, so install lazily (idempotent).
    D3D8Hook::Install(engine->d3d8_device);
    Limiter::Tick();

    auto ret = CChainManager__UpdateCalcChain(CChainManager::Instance);
    if (ret != 0 && ret != -1 && engine->config.frameskip <= (int)CWindowManager::Instance->frames_skipped) {
        // The frame's own draw (see the frame interpolation comment below, which is where the
        // block lives now)
        if (th6_interpolating)
            th6_begin_game_draw();
        th6_draw_pass();
        th6_drew_frame = true;
    }

    return ret;
}

int __fastcall th6_window_update_hook() {
    // Skipping the window update while the window isn't active saves CPU, but the window
    // update is also what drives rendering, so with AlwaysBlt we have to keep calling it.
    if (!CWindowManager::Instance->activated && !Config::AlwaysBlt) {
        Sleep(16);
        return 0;
    }
    return CWindowManager__Update(CWindowManager::Instance);
}

// ---------------------------------------------------------------------------
// The frame rate the game measures for itself (vpatch's CalcFPS)
//
// Touhou 6 works its own frame rate out in DrawFpsCounter (0x423F3F): every frame it adds
// 1 + frameskipConfig to the counter at 0x6C6D08, and once half a second has gone by (taking the
// time from timeGetTime) it divides the counter by that time and clears it - that reading is what
// tells the game how much of the half second it lost, which it accumulates into 0x6C6ECC over
// 0x6C6ED0 (Supervisor::unk1b4 / unk1b8). Those two are the "processing rate" the results screen
// shows (ResultScreen.cpp:1670) and the value a replay records (ReplayManager.cpp:400); a window
// that a load or a scene change lands in reads as a slow one, and the run then carries that as lost
// frame rate for the rest of it.
//
// vpatch's CalcFPS=1 replaces that measurement: it takes over 0x423F45 (the first instruction of
// the counter, th06's own vpatch continues at 0x423FE7 with the frame rate its stub 60003DC0
// computed left in the game's fps variable). This does the same with the frame rate the limiter
// measured for itself (see Limiter::CountGameFrame): the game's counter and window are skipped, the
// game is handed that reading, and it carries on where it would have - which leaves its own
// accounting (and everything that reads it) with a run that never fell behind.
// ---------------------------------------------------------------------------
static void* th6_fps_continue(void* frame) {
    // The frame pointer this is handed is the game's own; a hook that gets the calling convention
    // wrong hands over something else entirely, which used to mean writing through it. Writing only
    // to something that can be a stack address keeps that mistake from becoming memory corruption.
    if ((size_t)frame < 0x10000)
        return (void*)0x00423FE7;
    // The counter the game adds to is left where its own code would have left it: cleared once the
    // window it belongs to is over
    *(unsigned int*)0x006C6D08 = 0;
    *(float*)((char*)frame - 0x0C) = Limiter::MeasuredFps();
    // Where vpatch continues: just before the game formats and draws the frame rate text, with the
    // per half second weighting (0x424001 on) left to run as the game wrote it
    return (void*)0x00423FE7;
}

static __declspec(naked) void th6_fps_hook() {
    // The game's own frame pointer, pushed the way the game's own calls pass their arguments (it
    // is what the fps variable is written through, see th6_fps_continue)
    __asm { push ebp }
    __asm { call th6_fps_continue }
    __asm { add esp, 4 }
    __asm { jmp eax }
}

// Takes over the two present calls of GameWindow::Render (defined below, after the comment that
// explains what a frame is made of)
static void th6_install_interpolation();

void th6_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xE9, 0x9E, 0x00, 0x00, 0x00 };
		patch_bytes(0x004208ED, patch, sizeof(patch));
	}
	{
		// Skip original draw chain update logic
		BYTE patch[] = { 0xE9, 0xEE, 0x00 };
		patch_bytes(0x0042071A, patch, sizeof(patch));
	}
	{
		// Hook calc chain update
		// Shaves off 1 frame of input lag by calling calc chains BEFORE drawing
		patch_call(0x00420853, th6_update_calc_chain_hook);
	}
    {
        // Hook window updating
        // Avoid eating up CPU time while minimized
        patch_call(0x004204FF, th6_window_update_hook);
    }
    if (Config::FixInputGlitching) {
        // Fix input glitching
        BYTE patch[] = { 0x00, 0x00, 0x00, 0x00, 0x74 };
        patch_bytes(0x0041DC58, patch, sizeof(patch));
    }
    if (Config::AlwaysBlt) {
        // GameWindow::Render returns early when [this+8] (isAppActive) is 0, which stops
        // rendering entirely once the window loses focus and freezes the picture.
        // Turn that conditional jump into an unconditional one (same idea as vpatch's AlwaysBlt).
        BYTE patch[] = { 0xEB, 0x07 }; // jne 004206F9 -> jmp 004206F9
        patch_bytes(0x004206F0, patch, sizeof(patch));
    }
    if (Config::CalcFPS) {
        // Replace the frame rate the game measures for itself (see the comment above)
        BYTE* site = (BYTE*)0x00423F45;
        if (site[0] == 0xA1 && *(DWORD*)(site + 1) == 0x006C6D10) {
            patch_jmp(0x00423F45, th6_fps_hook);
        } else {
            printf("th06 fps measurement at 0x423F45 doesn't look like a known one, skipping!\n");
        }
    }
    if (Config::Interpolation != 0 || Config::InterpolationFPS != 0)
        th6_install_interpolation();
}

FPSTarget th6_replay_callback() {
    if (CEngine::Instance->state == 2 && CGame::Instance->is_replay) {
        auto input = get_input();
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// A frame of this game, the way the patches above leave it:
//
//     Limiter::Tick()                    the patch's wait for the next frame ends here, at the top
//                                        of the calculation chain hook (0x420853)
//     Chain::RunCalcChain (0x41CA10)     the input is read at its head, by Supervisor_OnUpdate
//                                        (its chain priority is 0, see ChainPriorities.hpp)
//     [the frame's draw - see below]
//     GameWindow_Present (0x420B50)      the device's Present, and then
//                                        AnmManager::TakeScreenshotIfRequested (0x421A40), which
//                                        reads the back buffer back for the snapshot key
//
// The game's own draw of a frame is the block at 0x420720..0x420807 of GameWindow::Render
// (0x4206E0): a 640x480 viewport and a Clear when the config asks for one (0x420720 tests
// g_Supervisor.cfg.opts at 0x6C6E60, bits 3 and 4, which is ShouldForceBackbufferClear), BeginScene,
// Chain::RunDrawChain (0x41CAD0), EndScene, SetTexture(0, NULL). The patch jumps over the game's own
// copy of it (the branch at 0x42071A is made unconditional and lands at 0x42080D) and runs the same
// block from the calculation chain hook instead, which is what puts the calculation ahead of the
// drawing and takes the frame of input lag out (see the comment on that hook). The limiter paces the
// frame from the top of that hook, so what is left of a frame after the game's own present is where
// the extra presentations belong - the same shape th07, th08, th19 and th20 use.
//
// An extra presentation is therefore the whole calculation chain hook with its presentation taken
// out: wait until that presentation's moment, draw the block again, present. Nothing in the
// calculation chain runs for one, so the game's logic, its timers and its random numbers are
// untouched. The present calls at 0x420A01 and 0x420A0B (the two exits of GameWindow::Render) are
// taken over to place them.
//
// Three things have to be kept out of the extras:
//
//   - The game counts its own frame rate off the draw pass. DrawFpsCounter (0x423F3F, the draw chain
//     callback Supervisor_OnDraw at CHAIN_PRIO_DRAW_SUPERVISOR = 14) adds one plus the frame skip
//     setting to g_NumFramesSinceLastTime (0x6C6D08, 0x423F71..0x423F82) every pass, and once half a
//     second has gone by works the frame rate out of it (0x423F8A..0x423FE0) - that reading goes into
//     the "%.02ffps" text in the bottom right corner (sprintf at 0x423FF9 into g_FpsCounterBuffer,
//     0x6C6C08) and, while the game is in a menu, into how much frame rate the run lost
//     (g_Supervisor.unk1b4 at 0x6C6ECC and unk1b8 at 0x6C6ED0, 0x424001..0x4240B5, both of them
//     written from every pass with CalcFPS on). Their ratio is what the results screen shows
//     (ResultScreen.cpp:1670) and what a replay records (ReplayManager.cpp:400), so run once per
//     extra presentation and the run would carry frame rate it never lost. The state that function
//     works in is put back around every extra pass (th6_fps_state below), which leaves the game's
//     accounting exactly where its own pass left it and makes the extras draw the same frame rate
//     text that pass drew.
//   - The game's own frame skip (cfg.frameskipConfig, 0x6C6E4B, against CWindowManager's curFrame at
//     +0x10) can leave a frame with no draw pass at all: it then presents the picture it already had.
//     The extras have nothing of their own to draw in that frame, so they are left out of it (see
//     th6_drew_frame) and the game's own present simply runs at its own rate.
//   - Whatever the frame's own pass was started with has to be put back before an extra draws: the
//     pass writes state the next pass reads. See th6_begin_game_draw for what that state is here.
//
// On text, this game's draw passes are self-contained the way th07's and th08's are: the string list
// is drawn and then emptied inside the same pass (AsciiManager_OnDrawMenus, at draw chain priority
// 15, runs DrawStrings and then numStrings = 0), and what appends to it (the HUD at priority 11, the
// frame rate counter at 14) runs earlier in that same pass. So an extra pass draws the same text the
// game's own pass drew instead of stacking a copy of it on top.

// The device the frame is drawn with and presented through. The game reads it from here itself
// (0x420774, 0x420B5C): g_Supervisor.d3dDevice, i.e. CEngine::Instance + 8
static IDirect3DDevice8* th6_device() {
    return *(IDirect3DDevice8**)0x006C6D20;
}

// The game's own frame, the block described above, as one function: the calculation chain hook runs it
// for the frame's own draw, and every extra presentation runs it again. The draw chain takes no lock
// of its own, so it can be run from inside the frame the same way the game runs it.
//
// The Clear's colour is g_Stage.skyFog.color: the game reads it from 0x00487B60 in this very block
// (0x42078A) and again when it clears the depth buffer (0x41B857). g_Stage is 0x00487B10 and its
// skyFog starts at +0x48 (Stage.hpp), so the colour is at +0x50. g_Supervisor.viewport is at 0x6C6DE0
// (g_Supervisor is 0x6C6D18, viewport at +0xC8), which is the CEngine::viewport named here.
static void th6_draw_pass() {
    auto engine = CEngine::Instance;

    if (((engine->config.engine_flags >> 4) & 1) | ((engine->config.engine_flags >> 3) & 1)) {
        D3DVIEWPORT8 viewport;
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = 640;
        viewport.Height = 480;
        viewport.MinZ = 0.0;
        viewport.MaxZ = 1.0;
        engine->d3d8_device->SetViewport(&viewport);
        engine->d3d8_device->Clear(0, 0, 3, *(D3DCOLOR*)0x00487B60, 1.0, 0);
        engine->d3d8_device->SetViewport(&engine->viewport);
    }
    engine->d3d8_device->BeginScene();
    CChainManager__UpdateDrawChain(CChainManager::Instance);
    engine->d3d8_device->EndScene();
    engine->d3d8_device->SetTexture(0, 0);
}

// g_Supervisor.viewport. The play field is drawn inside a viewport of its own, and it is written from
// both sides of a frame:
//
//   - GameManager_OnUpdate (calc chain priority 4) sets it to the play field out of
//     g_GameManager.gameRegionScreenPos / gameRegionSize (GameManager.cpp:116), builds the stage's
//     camera out of its size (GameManager::SetupCamera, GameManager.cpp:511: centre = size/2, aspect
//     = width/height, distance = halfHeight / tan(fov/2); it sets D3DTS_VIEW and D3DTS_PROJECTION with
//     it) and sets the device's viewport to it.
//   - The HUD, the full screen effects, the menus and the results screen set it back to the whole
//     640x480 screen while they draw (Gui.cpp:1115, ScreenEffect.cpp:31 and :133, AsciiManager.cpp:217,
//     ResultScreen.cpp:1744), and Stage_OnDrawLowPrio sets its depth range for the stage background
//     (Stage.cpp:263).
//
// So the value an extra pass finds there is the one the frame's own pass *ended* with, and the extras
// have to start from the one that pass started with, the same way th07 does. The device's own viewport
// and the view / projection matrices are put back with it (the draw callbacks write those as well:
// Stage_OnDrawLowPrio rebuilds the stage background's camera from the viewport at Stage.cpp:265, and
// the menus set their own transforms before drawing).
//
// The view and projection matrices exist twice, and both have to be put back: g_Supervisor.viewMatrix
// (0x6C6D60) and g_Supervisor.projectionMatrix (0x6C6DA0) are what the stage is *projected* with -
// Stage::RenderObjects culls every object by projecting it through them (Stage.cpp:512-546) and places
// the billboard quads (autoRotate == 2) by projecting their corners with them and drawing the result
// (Stage.cpp:625-640) - while the device holds its own copy of the two. Both are set together by
// GameManager::SetupCamera (GameManager.cpp:507) in the calculation chain, and then *changed* during
// the pass by the stage background's own camera (GameManager::SetupCameraStageBackground, called from
// Stage_OnDrawLowPrio at Stage.cpp:265, GameManager.cpp:534). So at the end of a pass they hold the
// background's camera, at the start of the next one the calculation chain has put the play field's
// back - and an extra pass that only restored the device's would cull and place the stage with a
// camera the frame's own pass never used.
struct Th6SceneState {
    D3DVIEWPORT8 game_viewport;   // g_Supervisor.viewport, 0x6C6DE0
    D3DVIEWPORT8 device_viewport; // as the device has it when the frame's own draw starts
    D3DMATRIX view;               // g_Supervisor.viewMatrix, 0x6C6D60
    D3DMATRIX projection;         // g_Supervisor.projectionMatrix, 0x6C6DA0
    D3DMATRIX device_view;
    D3DMATRIX device_projection;
    bool captured;
    bool has_transforms;
};
static Th6SceneState th6_scene;

// The depth buffer. The calculation chain clears it once a frame - GameManager_OnUpdate sets the play
// field viewport, builds the camera, sets the device's viewport and then clears the depth buffer out
// of it (GameManager.cpp:125-126, Clear(0, NULL, D3DCLEAR_ZBUFFER, g_Stage.skyFog.color, 1.0f, 0)) -
// so the frame's own draw starts with an empty one. A pass that runs after the frame's own pass does
// not: it is depth tested against the values that pass left behind, which are the depths of
// everything it drew, including what it drew last. The HUD is drawn last and its quads sit in front
// of the stage background and of circles drawn behind an enemy's sprite - so those, which the frame's
// own pass draws first and without anything in front of them, would fail the depth test in the extras
// and simply not be drawn there. That is a picture that changes between the presentations of one
// frame (the frame rate the extras present at), which shows as flashing. Clearing the depth buffer
// with the viewport the game clears it with puts the extra pass back in the depth state the frame's
// own pass started from; it is done outside BeginScene, the same way the game does it.
static void th6_clear_depth_buffer(IDirect3DDevice8* device) {
    device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, *(D3DCOLOR*)0x00487B60, 1.0f, 0);
}

// The AnmManager's caches of what it has already told the device to do (AnmManager::current*). An
// extra presentation draws through the same manager, and a cache that still says "this is already
// set" makes the pass skip the corresponding device call - but the pass it repeats ended by setting
// the device's texture to NULL (the block's last step) and, with ShowOverlay on, by the patch's
// overlay binding its own font texture and states, which a D3D8 state block does not put back. So the
// caches do not describe the device any more, and they are all forgotten here so that the pass sets
// what it draws with, the way the game's own pass does when it starts a frame.
//
// The one that matters most in this game is the sprite cache. The texture bind sits *inside* the
// "the sprite changed" branch here (AnmManager::DrawInner, AnmManager.cpp:538-554 = 0x43288E..0x4328E8:
// `cmp currentSprite, vm->sprite` and only then `cmp currentTexture, textures[sourceFileIndex]`), so a
// sprite whose pointer is the same as the one that was drawn last skips the bind as well - and since
// the game's own pass ends with SetTexture(0, NULL), that leaves those sprites drawn with no texture
// at all (a white quad, the vertex colour) until some draw uses a different sprite. That is what the
// first test showed as bullets and lasers coming out washed out/overexposed in places, and it made the
// extra presentations differ from the frame's own one. th07 and th08 only forget the texture because
// their texture check is not nested inside the sprite one; here both have to go.
//
// The values the caches are set to are the ones Supervisor_OnDraw itself resets them to at the start
// of every draw pass (0x4237A8 sprite = NULL, 0x4237BA texture = NULL, 0x4237CC vertex shader = 0xFF,
// 0x4237DE blend mode = 0xFF, and the colour op / z write disable the same way), i.e. the game's own
// "nothing is set yet" values. currentTextureFactor is deliberately left alone: this pass does not
// keep a record of what the game sets it to either.
static const unsigned int th6_anm_texture_offset = 0x210B8;
static const unsigned int th6_anm_blend_mode_offset = 0x210BC;
static const unsigned int th6_anm_color_op_offset = 0x210BD;
static const unsigned int th6_anm_vertex_shader_offset = 0x210BE;
static const unsigned int th6_anm_z_write_offset = 0x210BF;
static const unsigned int th6_anm_sprite_offset = 0x210C0;

static void th6_forget_anm_cache() {
    unsigned char* manager = *(unsigned char**)0x006D4588;
    if (manager == nullptr)
        return;

    *(void**)(manager + th6_anm_sprite_offset) = nullptr;
    *(void**)(manager + th6_anm_texture_offset) = nullptr;
    manager[th6_anm_blend_mode_offset] = 0xFF;
    manager[th6_anm_color_op_offset] = 0xFF;
    manager[th6_anm_vertex_shader_offset] = 0xFF;
    manager[th6_anm_z_write_offset] = 0xFF;
}

// The state DrawFpsCounter (0x423F3F) works in. It is called from the draw chain, so every extra
// presentation runs it once more, and this is what puts the game's accounting back the way its own
// pass left it:
//
//     0x6C6D08  g_NumFramesSinceLastTime, the count of passes in the current half second
//               (0x423F71, 0x423F78 and 0x423F82 add to it, 0x423FE0 clears it)
//     0x6C6D0C  g_LastTime, where that half second started (0x423F63 at startup, 0x423FDB when a
//               window closes)
//     0x6C6C08  g_FpsCounterBuffer, the "%.02ffps" text the count is drawn as (sprintf at 0x423FF9,
//               handed to g_AsciiManager.AddString at 0x4240E9 unless the game is in an ending)
//     0x6C6ECC  Supervisor::unk1b4, and 0x6C6ED0 unk1b8, how much frame rate the run lost; the two of
//               them are accumulated while the game is in a menu (0x424001..0x4240B5) and their ratio
//               is what the results screen shows (ResultScreen.cpp:1670) and a replay records
//               (ReplayManager.cpp:400)
//     0x6C6D10  the "the static local above has been initialised" flag, which 0x423F58 writes once on
//               the game's first call
//
// That list is every absolute address the function writes (0x423F3F to 0x4240F0), plus the text buffer
// its sprintf writes into. They are saved and put back as one block, which is what keeps the game's
// own reading of its frame rate at 60 and its lost frame rate at zero no matter how many times a frame
// is presented.
struct Th6FpsState {
    unsigned int frames_since_last_time; // 0x6C6D08
    unsigned int last_time;              // 0x6C6D0C
    unsigned int last_time_guard;        // 0x6C6D10
    char fps_text[256];                  // 0x6C6C08
    float rate_accumulator;              // g_Supervisor.unk1b4, 0x6C6ECC
    float rate_denominator;              // g_Supervisor.unk1b8, 0x6C6ED0
};

static void th6_fps_state_save(Th6FpsState* state) {
    state->frames_since_last_time = *(unsigned int*)0x006C6D08;
    state->last_time = *(unsigned int*)0x006C6D0C;
    state->last_time_guard = *(unsigned int*)0x006C6D10;
    memcpy(state->fps_text, (void*)0x006C6C08, sizeof(state->fps_text));
    state->rate_accumulator = *(float*)0x006C6ECC;
    state->rate_denominator = *(float*)0x006C6ED0;
}

static void th6_fps_state_restore(const Th6FpsState* state) {
    *(unsigned int*)0x006C6D08 = state->frames_since_last_time;
    *(unsigned int*)0x006C6D0C = state->last_time;
    *(unsigned int*)0x006C6D10 = state->last_time_guard;
    memcpy((void*)0x006C6C08, state->fps_text, sizeof(state->fps_text));
    *(float*)0x006C6ECC = state->rate_accumulator;
    *(float*)0x006C6ED0 = state->rate_denominator;
}

// Where the user's tool builds its frame and where it draws it (see tool_state.h). thprac's th06
// module hooks the return of Chain::RunCalcChain (0x41CAAC) with its "th06_update" hook to build its
// overlay frame and the return of Chain::RunDrawChain (0x41CB6D) with its "th06_render" hook to draw
// it (thprac_th06.cpp, EHOOK_DY(th06_update, 0x41caac) and EHOOK_DY(th06_render, 0x41cb6d)); both are
// plain `ret`s, so the defaults are right for them.
static unsigned char* th6_tool_frame_address = (unsigned char*)0x0041CAAC;
static unsigned char* th6_tool_draw_address = (unsigned char*)0x0041CB6D;

// Runs where the game's own draw starts: remembers what the frame is drawn with, and watches for the
// tool's frame (its overlay is built by now, from the calculation chain's return, and drawn on the
// return of the draw chain inside the block below).
static void th6_begin_game_draw() {
    IDirect3DDevice8* device = th6_device();

    th6_scene.captured = device != nullptr;
    th6_scene.has_transforms = false;
    if (device != nullptr) {
        device->GetViewport(&th6_scene.device_viewport);
        th6_scene.has_transforms =
            SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th6_scene.device_view)) &&
            SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th6_scene.device_projection));
    }
    th6_scene.game_viewport = *(D3DVIEWPORT8*)0x006C6DE0;
    th6_scene.view = *(D3DMATRIX*)0x006C6D60;
    th6_scene.projection = *(D3DMATRIX*)0x006C6DA0;

    ToolState::WatchFrameReady(th6_tool_frame_address, th6_tool_draw_address);
}

// One extra presentation of the frame: the block the game draws a frame with, once more, so the state
// the game is in is drawn and presented again.
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed. The
// stage 1 code will project the objects forward by it (nothing is projected yet, so a stage 0 extra
// draws exactly the state the game's own pass drew).
static void th6_extra_presentation(float t) {
    (void)t;

    IDirect3DDevice8* device = th6_device();
    if (device == nullptr)
        return;

    // Put back what the frame's own draw was started with (see th6_begin_game_draw)
    if (th6_scene.captured) {
        *(D3DVIEWPORT8*)0x006C6DE0 = th6_scene.game_viewport;
        device->SetViewport(&th6_scene.device_viewport);
        // Both copies of the camera, the game's own (what the stage is projected with) and the
        // device's
        *(D3DMATRIX*)0x006C6D60 = th6_scene.view;
        *(D3DMATRIX*)0x006C6DA0 = th6_scene.projection;
        if (th6_scene.has_transforms) {
            device->SetTransform(D3DTS_VIEW, &th6_scene.device_view);
            device->SetTransform(D3DTS_PROJECTION, &th6_scene.device_projection);
        }
    }

    // The manager's caches do not describe the device any more (see th6_forget_anm_cache)
    th6_forget_anm_cache();

    // The frame's own pass drew against an empty depth buffer; this pass has to as well (the viewport
    // above is the one the game clears it with)
    th6_clear_depth_buffer(device);

    // The tool's overlay is drawn from the return of the draw chain inside this block, so it has to
    // know its frame is ready to be drawn again
    ToolState::ReadyToDraw();

    // The game counts its own frame rate off this pass (see the comment above), so its state is put
    // back around it
    Th6FpsState fps;
    th6_fps_state_save(&fps);
    th6_draw_pass();
    th6_fps_state_restore(&fps);

    // Presented with the device's own Present, without the hooks other patches and tools put in front
    // of it (see d3d8_present_bypassing_hooks, the same thing th07, th08, th19 and th20 do): a tool's
    // present hook measures the input latency from the input poll to the next present, which only
    // means anything when there is one present per game frame - with the extras it would report a
    // fraction of a frame and take the extra presentations into the average. The patch's own overlay
    // is drawn from that path as well, so it stays on every presentation.
    d3d8_present_bypassing_hooks();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt (the same
// file name and ring buffer shape the th07 to th20 diagnostics use)
static const unsigned int th6_diag_frames_max = 300;
static const unsigned int th6_diag_presents_max = 8;
struct Th6DiagFrame {
    LARGE_INTEGER base;                                 // the game's own present of the frame
    unsigned int presents;
    LARGE_INTEGER present_time[th6_diag_presents_max];  // when each presentation happened
};
static Th6DiagFrame th6_diag_frames[th6_diag_frames_max];
static unsigned int th6_diag_count;
static bool th6_diag_key_down;
static LARGE_INTEGER th6_diag_perf_freq;

static void th6_diag_dump() {
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

    unsigned int count = th6_diag_count < th6_diag_frames_max ? th6_diag_count : th6_diag_frames_max;
    unsigned int start = th6_diag_count < th6_diag_frames_max ? 0 : th6_diag_count % th6_diag_frames_max;

    fprintf(file, "oilp frame interpolation diagnostics\n");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f, display refresh: %uHz\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "configured game frame rate: %u, BltPrepareTime %u, sleep %d\n",
            Config::GameFPS, Config::BltPrepareTime, (int)Config::Sleep);
    fprintf(file, "frames recorded: %u\n", count);
    // The game's own frame rate accounting, from its own side (see Th6FpsState): the text it draws for
    // its frame rate, the pass count it is working on and the two accumulators its "processing rate"
    // comes out of. That count is added to once per draw pass, the extras included, and the two
    // accumulators are added to once per pass while the game is in a menu, so what the game itself
    // makes of them is what says whether putting the state back works: the text should read 60, the two
    // accumulators should be equal (0.00% lost, see ResultScreen.cpp:1670) and - with CalcFPS off - the
    // count should read about 30 (half a second of passes) rather than three times that. With CalcFPS
    // on the game's own counting is replaced (see the comment above), so the count stays cleared and
    // the text carries the frame rate the limiter measured.
    {
        float correction = *(float*)0x006C6ECC;
        float denominator = *(float*)0x006C6ED0;
        float lost = denominator != 0.0f ? (1.0f - (correction / denominator - 0.5f) * 2.0f) * 100.0f : 0.0f;
        fprintf(file, "game's own frame rate: \"%.16s\", count: %u, correction: %.4f/%.4f (%+.2f%%), "
                      "framerate multiplier: %.4f (effective %.4f)\n",
                (char*)0x006C6C08, *(unsigned int*)0x006C6D08, correction, denominator, lost,
                *(float*)0x006C6EC4, *(float*)0x006C6EC0);
    }
    fprintf(file, "frameskip config: %u, cur frame: %u\n",
            (unsigned int)*(unsigned char*)0x006C6E4B,
            (unsigned int)*(unsigned char*)(0x006C6BD4 + 0x10));
    double presents = 0.0;
    double first = 0.0;
    double last = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const Th6DiagFrame& frame = th6_diag_frames[(start + i) % th6_diag_frames_max];
        fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
        for (unsigned int p = 0; p < frame.presents && p < th6_diag_presents_max; ++p)
            fprintf(file, " [%u] %.2fms", p,
                    (double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
                        (double)th6_diag_perf_freq.QuadPart);
        fprintf(file, "\n");
        presents += frame.presents;
        if (i == 0)
            first = (double)frame.base.QuadPart;
        last = (double)frame.base.QuadPart;
    }
    if (last > first && count > 0)
        fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
                presents / (double)count,
                presents * (double)th6_diag_perf_freq.QuadPart / (last - first));

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

// Presents the frame the game just drew, and then the extra presentations the config asked for. The
// game calls this from the two exits of GameWindow::Render (0x420A01 and 0x420A0B); both are plain
// calls to the static GameWindow_Present (0x420B50), which takes no arguments (0x420B50 has no
// `mov [ebp-x], ecx`, and its callers push nothing but the device they read themselves).
static void th6_present_hook() {
    if (th6_diag_perf_freq.QuadPart == 0)
        QueryPerformanceFrequency(&th6_diag_perf_freq);

    LARGE_INTEGER base;
    QueryPerformanceCounter(&base);

    // The game's own presentation of this frame, exactly as it always ran it. The back buffer read-back
    // its snapshot key does happens inside it, before any of the extras below draw over it.
    static auto th6_game_present = (void(__cdecl*)())0x00420B50;
    th6_game_present();

    // The game's own work of this frame ends here as far as the overlay is concerned: everything below
    // is the patch presenting the same frame again
    Limiter::MarkFrameWorkEnd();

    // The tool has drawn its frame by now: the other half of watching for its frame state
    ToolState::WatchFrameDrawn();

    LARGE_INTEGER present_time[th6_diag_presents_max] = {};
    unsigned int presents = 1;
    present_time[0] = base;

    // The extras have nothing to draw in a frame the game didn't (see the frame skip note above), and
    // nothing to add while the presents are being thinned out - that is there to keep a frame rate above
    // the display's from being throttled by its presents, and extras would undo it
    if (th6_drew_frame && !Limiter::PresentingEveryNthFrame() && Limiter::ShouldPresent()) {
        double rate = Limiter::PresentRate();
        if (rate > 0.0) {
            static bool rate_printed = false;
            if (!rate_printed) {
                rate_printed = true;
                printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
                       rate, rate / Limiter::game_fps);
            }
        }

        LARGE_INTEGER spacing;
        unsigned int extras = Limiter::ExtraPresentations(rate, spacing);

        // Where the game's own part of this frame started, so the extras can tell whether they still fit
        // before the next frame is due
        LARGE_INTEGER frame_start = Limiter::FrameStart();
        LARGE_INTEGER frame_wait = Limiter::FrameWait();
        LARGE_INTEGER game_work;
        game_work.QuadPart = base.QuadPart - frame_start.QuadPart;

        if (extras > 0 && spacing.QuadPart > 0) {
            // The presentations can't run past the end of the frame either: the limiter starts the next
            // frame one frame after this one started, and a frame that overruns it makes it fall behind
            // (and resync). An extra presentation costs about as much as the game's own pass did, so the
            // ones that wouldn't fit are dropped - the frame is then presented fewer times, but never
            // late.
            bool has_deadline = frame_start.QuadPart > 0 && game_work.QuadPart > 0;
            LARGE_INTEGER deadline;
            deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

            for (unsigned int k = 1; k <= extras; ++k) {
                LARGE_INTEGER target;
                target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
                if (has_deadline && target.QuadPart + game_work.QuadPart > deadline.QuadPart)
                    continue;

                Limiter::WaitUntil(target);

                if (presents < th6_diag_presents_max)
                    QueryPerformanceCounter(&present_time[presents]);
                ++presents;

                // Where this presentation sits between the game's own one and the next one: the
                // presentations of one game frame are spread evenly over it, so the k-th of extras + 1 of
                // them draws the state k/(extras + 1) frames along
                th6_extra_presentation((float)k / (float)(extras + 1));
            }
        }
    }

    th6_drew_frame = false;

    // Record the frame for the diagnostics
    {
        Th6DiagFrame& frame = th6_diag_frames[th6_diag_count % th6_diag_frames_max];
        frame.base = base;
        frame.presents = presents;
        for (unsigned int p = 0; p < th6_diag_presents_max; ++p)
            frame.present_time[p] = p < presents ? present_time[p] : present_time[0];
        ++th6_diag_count;
    }

    // U dumps the diagnostics
    if (GetAsyncKeyState('U') & 0x8000) {
        if (!th6_diag_key_down) {
            th6_diag_key_down = true;
            th6_diag_dump();
        }
    } else {
        th6_diag_key_down = false;
    }
}

// Takes over the two present calls the frame ends with
static void th6_install_interpolation() {
    {
        // call 0x420B50, GameWindow_Present (0x420A01) - the exit the fullscreen path takes when the
        // game's own frame skip setting says enough of this frame has been drawn, which then goes round
        // to draw again
        BYTE* site = (BYTE*)0x00420A01;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00420B50 - (0x00420A01 + 5)) {
            patch_call(0x00420A01, th6_present_hook);
        } else {
            printf("th06 present call at 0x420A01 doesn't look like a known one, skipping!\n");
        }
    }
    {
        // call 0x420B50, GameWindow_Present (0x420A0B) - the exit a normal frame takes, which ends the
        // frame there (effectiveFramerateMultiplier, curFrame = 0, g_TickCountToEffectiveFramerate++)
        BYTE* site = (BYTE*)0x00420A0B;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00420B50 - (0x00420A0B + 5)) {
            patch_call(0x00420A0B, th6_present_hook);
        } else {
            printf("th06 present call at 0x420A0B doesn't look like a known one, skipping!\n");
        }
    }
    {
        // The rate the frames are actually presented at, next to the render time line in the bottom left
        // corner - the same readout th07, th08 and th10 to th20 show while the interpolation is on. The
        // presentation counting itself lives in the D3D8 hook, which sees every present. (The overlay the
        // two lines go on is only there when ShowOverlay is on, so there is nothing else to check here.)
        overlay_show_present_rate(true);
    }

    // From here on the calculation chain hook draws the frame (see the stage 0 comment above)
    th6_interpolating = true;

    printf("Frame interpolation: th06 extra presentations installed (setting %d)\n", Config::Interpolation);
}
