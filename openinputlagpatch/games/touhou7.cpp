// Touhou 7: Perfect Cherry Blossom v1.00b

#include <stdio.h>
#include <string.h>
#include "patch_util.h"
#include "d3d8_hook.h"
#include "limiter.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou7.h"
#include "config.h"

using namespace Touhou7;

CEngine* CEngine::Instance = (CEngine*)0x00575950;
CWindowManager* CWindowManager::Instance = (CWindowManager*)0x00575C20;
CChainManager* CChainManager::Instance = (CChainManager*)0x00626218;
CGame* CGame::Instance = (CGame*)0x00626270;
CTextureManager** CTextureManager::InstancePtr = (CTextureManager**)0x004B9E44;

// The frame interpolation below is what draws a frame from the calculation chain hook when it is
// on - see the stage 0 comment at the end of this file. These are the parts of it the hook itself
// needs.
static bool th7_interpolating;
static bool th7_drew_frame;
static void th7_begin_game_draw();
static void th7_draw_pass();

int __fastcall th7_update_calc_chain_hook() {
	auto engine = CEngine::Instance;
	Limiter::Tick();

    auto ret = CChainManager__UpdateCalcChain(CChainManager::Instance);
    if (ret != 0 && ret != -1 && engine->config.frameskip <= (char)CWindowManager::Instance->frames_skipped) {
        // The frame's own draw (see the frame interpolation comment below, which is where the
        // block lives now)
        if (th7_interpolating)
            th7_begin_game_draw();
        th7_draw_pass();
        th7_drew_frame = true;
    }

    return ret;
}

int __fastcall th7_window_update_hook() {
    // Same as th06: the window update is what drives rendering, so with AlwaysBlt we must
    // keep calling it even while the window isn't active.
    if (!CWindowManager::Instance->activated && !Config::AlwaysBlt) {
        Sleep(16);
        return 0;
    }
    return CWindowManager__Update(CWindowManager::Instance);
}

// ---------------------------------------------------------------------------
// Input handling, ported from vpatch (the same design as the one in touhou8.cpp).
//
// Perfect Cherry Blossom reads the keyboard into a slot of its own (the call to
// Controller::GetInput at 0x00437D80 and the store at 0x00437D85), and the player reads a copy of
// that input - 0x004B9E50, with 0x004B9E58 as its previous frame - which the game only advances in
// a job of its own: 0x00442CD0 tests [0x0062F648] & 4 at 0x00442CD9 and does the "previous = copy;
// copy = raw" pair at 0x00442CF2. That job runs after the player has already used the copy, so the
// player saw the previous frame's input: the extra frame of input lag vpatch's readme lists for
// this game beside the drawing one, and the one its rev4 took out.
//
// vpatch's four patches are what does that, and they are applied here the way vpatch applies them:
// unconditionally, not behind a config option (a vpatched game always behaves this way).
//
//   1. the call to Controller::GetInput at 0x00437D80 goes to the wrapper below, so the copy is
//      advanced at the moment the input is read - and left alone while a replay is playing
//      ([0x0062F648] & 8), because there the replay manager feeds it;
//   2. the top of the game's own copy job (0x00442CD9) is taken over and spends the flag below:
//      the frame the flag is up that job returns 1 straight away;
//   3. the copy pair at 0x00442CF2 is skipped (jmp 0x00442D0E), so the copy is advanced once per
//      frame, by the wrapper;
//   4. the function that ends at 0x004433A5 gets its ret replaced by a store, so returning from it
//      raises that flag.
// ---------------------------------------------------------------------------
static DWORD th7_input_discard;

// Where the game calls Controller::GetInput (0x00437D80)
static __int16 th7_input_hook() {
    __int16 input = get_input();
    if ((*(volatile BYTE*)0x0062F648 & 8) == 0) {
        *(volatile WORD*)0x004B9E58 = *(volatile WORD*)0x004B9E50;
        *(volatile WORD*)0x004B9E50 = (WORD)input;
    }
    return input;
}

// The ret at 0x004433A5 of the function that ends there
static __declspec(naked) void th7_input_flag_setter() {
    __asm { mov dword ptr [th7_input_discard], edx }
    __asm { ret }
}

// 0x00442CD9, the top of the game's own copy job
static __declspec(naked) void th7_input_flag_consumer() {
    __asm { mov eax, dword ptr [th7_input_discard] }
    __asm { test eax, eax }
    __asm { jne spend }
    __asm { mov eax, dword ptr [0062F648h] }
    __asm { mov ecx, 00442CDEh }
    __asm { jmp ecx }
spend:
    __asm { xor eax, eax }
    __asm { mov dword ptr [th7_input_discard], eax }
    __asm { inc eax }
    __asm { mov ecx, 00442E46h }
    __asm { jmp ecx }
}

// Takes over the two present calls of GameWindow::Render (defined below, after the comment that
// explains what a frame is made of)
static void th7_install_interpolation();

void th7_install_patches() {
    {
        // Skip the original frame limiter
        BYTE patch[] = { 0xE9, 0x70, 0x01, 0x00, 0x00, 0x90 };
        patch_bytes(0x00434854, patch, sizeof(patch));
    }
    {
        // Skip original draw chain update logic
        BYTE patch[] = { 0xEB };
        patch_bytes(0x00434718, patch, sizeof(patch));
    }
    {
        // Hook calc chain update
        // Shaves off 1 frame of input lag by calling calc chains BEFORE drawing
        patch_call(0x004347DF, th7_update_calc_chain_hook);
    }
    {
        // Hook window updating
        // Avoid eating up CPU time while minimized
        patch_call(0x004341F7, th7_window_update_hook);
    }
    {
        // The input handling above, the way vpatch applies it: always
        patch_call(0x00437D80, th7_input_hook);
        patch_jmp(0x00442CD9, th7_input_flag_consumer);
        {
            // jmp 00442D0E over the copy pair, and what the five bytes it skips held
            BYTE patch[] = { 0xEB, 0x1A, 0x90, 0x90, 0x90, 0x90, 0x90 };
            patch_bytes(0x00442CF2, patch, sizeof(patch));
        }
        patch_jmp(0x004433A5, th7_input_flag_setter);
    }
    if (Config::FixInputGlitching) {
        // Fix input glitching
        BYTE patch[] = { 0x00, 0x00, 0x00, 0x00, 0x74 };
        patch_bytes(0x00430F03, patch, sizeof(patch));
    }
    if (Config::AlwaysBlt) {
        // GameWindow::Render bails out when [this+8] is 0, freezing the picture once the
        // window loses focus. Turn that conditional jump into an unconditional one.
        BYTE patch[] = { 0xEB, 0x07 }; // jne 004346F9 -> jmp 004346F9
        patch_bytes(0x004346F0, patch, sizeof(patch));
    }
    if (Config::Interpolation != 0 || Config::InterpolationFPS != 0)
        th7_install_interpolation();
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// A frame of this game, the way the patches above leave it:
//
//     Limiter::Tick()                     the patch's wait for the next frame ends here, at the
//                                         top of the calculation chain hook (0x4347DF)
//     Chain::RunCalcChain (0x42FD60)      the input is read at its head, by Supervisor::OnUpdate
//                                         (its chain priority is 0)
//     [the frame's draw - see below]
//     GameWindow::Present (0x4345C0)      the device's Present, and then
//                                         AnmManager::TakeScreenshotIfRequested, which reads the
//                                         back buffer back for the snapshot key
//
// The game's own draw of a frame is the block at 0x43471A..0x434788 of GameWindow::Render
// (0x4346E0): BeginScene, AnmManager::ResetVertexBuffer, fogEnabled = 255, Supervisor::DisableFog,
// Chain::RunDrawChain (0x42FE20), AnmManager::Flush, SetTexture(0, NULL), EndScene. The patch skips
// the game's own copy of it (the branch at 0x434718 is made unconditional) and runs the same block
// from the calculation chain hook instead, which is what puts the calculation ahead of the drawing
// and takes the frame of input lag out (see the comment on that hook). The limiter paces the frame
// from the top of that hook, so what is left of a frame after the game's own present is where the
// extra presentations belong - the same shape th08, th19 and th20 use.
//
// An extra presentation is therefore the whole calculation chain hook with its presentation taken
// out: wait until that presentation's moment, draw the block again, present. Nothing in the
// calculation chain runs for one, so the game's logic, its timers and its random numbers are
// untouched. The present calls at 0x434A0E and 0x434A18 (the two exits of GameWindow::Render) are
// taken over to place them.
//
// Two things have to be kept out of the extras:
//
//   - The game counts its own frame rate off the draw pass. Supervisor::DrawFpsCounter (0x4390A5,
//     the draw chain callback Supervisor::OnDraw at CHAIN_PRIO_DRAW_SUPERVISOR = 15) adds one plus
//     the frame skip setting to g_NumFramesSinceLastTime (0x135E1F0, 0x4390C4..0x4390CE) every
//     pass, and every 500ms works the frame rate out of it (0x43912A..0x439171): the rate goes into
//     the "%.02ffps" text in the bottom right corner (0x135E0F0, 0x43918D), into
//     g_Supervisor.curFps (0x575AD8) and into the frame rate correction (framerateMultiplier at
//     0x575AD0, fpsAccumulator at 0x575AD4 - the ratio of the two is the frame rate a replay gets
//     recorded with, ReplayManager.cpp:626, and the correction also drives CheckTiming every eighth
//     update). Run once per extra presentation the game would read its own frame rate as three
//     times what it is. The state that function works in is put back around every extra pass
//     (th7_fps_state below), which leaves the game's accounting exactly where its own pass left it
//     and makes the extras draw the same frame rate text that pass drew.
//   - The game's own frame skip (frameskipConfig at 0x575A8B) can leave a frame with no draw pass
//     at all: it then presents the picture it already had. The extras have nothing of their own to
//     draw in that frame, so they are left out of it (see th7_drew_frame) and the game's own
//     present simply runs at its own rate.
//
// On text, this game's draw passes are self-contained the way th08's are: the string list is drawn
// and then cleared inside the same pass (AsciiManager::OnDrawMenus, 0x401970, runs DrawStrings and
// then numStrings = 0), and what appends to it (the HUD at CHAIN_PRIO_DRAW_GUI = 12 and the frame
// rate counter at 15) runs earlier in that same pass. So an extra pass draws the same text the
// game's own pass drew instead of stacking a copy of it on top, which is what th12 to th20 had to
// deal with here.

// The device the frame is drawn with and presented through. The game reads it from here itself
// (0x43471A, 0x4345D1): g_Supervisor.d3dDevice
static IDirect3DDevice8* th7_device() {
    return *(IDirect3DDevice8**)0x00575958;
}

// The game's own calls the extra presentations make, at the addresses the game calls them from (see
// the frame above). The draw chain takes no lock of its own here, so it can be run from inside the
// frame the same way the game runs it.
// g_Supervisor is CEngine::Instance (0x575950); its fogEnabled is what the block's second step
// sets (0x434739)
static DWORD* th7_fog_enabled = (DWORD*)0x00575C0C;

static auto th7_game_present = (void(__cdecl*)())0x004345C0; // 0x434A0E / 0x434A18

static void th7_draw_pass() {
    auto engine = CEngine::Instance;

    engine->d3d8_device->BeginScene();
    CTextureManager__Unk44F580(*CTextureManager::InstancePtr);
    *th7_fog_enabled = 255;
    CEngine__UpdateFog(engine);
    CChainManager__UpdateDrawChain(CChainManager::Instance);
    CTextureManager__Flush(*CTextureManager::InstancePtr);
    engine->d3d8_device->SetTexture(0, 0);
    engine->d3d8_device->EndScene();
}

// What the frame's own draw was started with - an extra presentation has to start from the same
// state, and in this game that state is not all on the device.
//
// The important one is g_Supervisor.viewport (0x575A18). The play field is drawn inside a viewport
// of its own: GameManager::OnUpdate sets it to the arcade region, (32, 16, 384, 448)
// (GameManager.cpp:143, at calc chain priority 2, so it is set before every draw), the HUD and the
// full screen effects set it back to (0, 0, 640, 480) (Gui.cpp:1451, ScreenEffect.cpp:167), and the
// menus set it to the arcade region again (AsciiManager.cpp:800). Two things read it:
//
//   - AnmManager::SyncRenderState calls SetViewport with it every time a sprite's camera mode
//     changes (0x44EE18..0x44EE6C, the mode at AnmManager+0x2E4D4). That is what puts the play
//     field's draws into the play field's viewport at all.
//   - Stage::SetupCameraStageBackground builds the stage's camera out of its size
//     (Stage.cpp:1126: centerX = Width/2, centerY = Height/2, aspectRatio = Width/Height,
//     eyeZ = centerY / tan(fov/2)).
//
// So a pass that starts with the full screen viewport in there draws the whole 3D scene - the stage
// background, the art around the play field, the bullets - through a camera built for 640x480 and
// into a 640x480 viewport instead of the play field's. The background jumps, what belongs inside the
// play field is no longer clipped to it, and the art at its sides is drawn over. That is what the
// extras did: the value they found there was the one the frame's own pass had *ended* with, which is
// the full screen one the HUD leaves behind - not the one that pass started with.
//
// Two more pieces of the same state: AnmManager's draw offset (AnmManager+0x18, a Float2 that
// AnmManager.cpp:821 adds to every 2D quad and ScreenEffect.cpp:274 sets for the screen shake) and
// its cached camera mode (AnmManager+0x2E4D4), which says where the viewport and the camera are set
// up again - putting it back makes the extra pass run those setups in the same places the frame's
// own pass ran them.
//
// The view and projection matrices are put back as well, off the device like the viewport (the
// device's own viewport is the full screen one the game sets at 0x434799..0x4347D4 before the
// calculation chain)).
static D3DVIEWPORT8 th7_scene_device_viewport = {};
static D3DVIEWPORT8 th7_scene_game_viewport = {};
static D3DMATRIX th7_scene_view = {};
static D3DMATRIX th7_scene_projection = {};
static bool th7_scene_captured = false;
static bool th7_scene_transforms = false;
static float th7_scene_offset[2] = {};
static unsigned char th7_scene_camera_mode = 0;

// g_Supervisor.viewport, and the AnmManager fields above
static D3DVIEWPORT8* th7_game_viewport = (D3DVIEWPORT8*)0x00575A18;
static const unsigned int th7_anm_offset = 0x18;
static const unsigned int th7_anm_camera_mode = 0x2E4D4;

// Where the user's tool builds its frame and where it draws it (see tool_state.h). thprac's th07
// module hooks the return of Chain::RunCalcChain (0x42FDF8) with its "th07_update" hook to build
// its overlay frame and the return of Chain::RunDrawChain (0x42FEB9) with its "th07_render" hook to
// draw it; both are plain `ret`s, so the defaults are right for them.
static unsigned char* th7_tool_frame_address = (unsigned char*)0x0042FDF8;
static unsigned char* th7_tool_draw_address = (unsigned char*)0x0042FEB9;

// Runs where the game's own draw starts: remembers what the frame is drawn with, and watches for
// the tool's frame (its overlay is built by now, from the calculation chain's return, and drawn on
// the return of the draw chain inside the block below).
static void th7_begin_game_draw() {
    IDirect3DDevice8* device = th7_device();
    th7_scene_captured = device != nullptr;

    if (device != nullptr) {
        device->GetViewport(&th7_scene_device_viewport);
        th7_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th7_scene_view)) &&
                               SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th7_scene_projection));
    }

    // The state that is not on the device (see above)
    th7_scene_game_viewport = *th7_game_viewport;

    void* manager = *CTextureManager::InstancePtr;
    if (manager != nullptr) {
        th7_scene_offset[0] = *(float*)((char*)manager + th7_anm_offset);
        th7_scene_offset[1] = *(float*)((char*)manager + th7_anm_offset + 4);
        th7_scene_camera_mode = *(unsigned char*)((char*)manager + th7_anm_camera_mode);
    }

    ToolState::WatchFrameReady(th7_tool_frame_address, th7_tool_draw_address);
}

// The AnmManager's cached copy of the texture it last bound (AnmManager+0x2E4CC, compared against
// the sprite's texture at 0x44F3D0 in AnmManager::DrawInner and only re-bound when they differ).
//
// An extra presentation draws through the same manager, and the pass it repeats ended by setting
// the device's texture to NULL (the block's last step) - and, with ShowOverlay on, by the patch's
// overlay binding its own font texture, which a D3D8 state block does not put back. Either way the
// device no longer holds what that cache says it holds, and since the manager only calls SetTexture
// when the cache says it has to, the first sprites an extra draws would come out with whatever was
// left bound. Forgetting the cache makes the pass bind what it draws, exactly the way the game's
// own pass does when it starts a frame.
//
// (0x4B9E44 holds the manager the game itself draws through - see CTextureManager::InstancePtr.)
static void th7_forget_anm_texture() {
    void* manager = *CTextureManager::InstancePtr;
    if (manager != nullptr)
        *(void**)((char*)manager + 0x2E4CC) = nullptr;
}

// The state Supervisor::DrawFpsCounter (0x4390A5) works in. It is called from the draw chain (see
// the comment above), so every extra presentation runs it once more, and this is what puts the
// game's accounting back the way its own pass left it:
//
//     0x135E1F0  the count of passes since the last window started (0x4390C4..0x4390CE)
//     0x135E2A0  when the window started, and 0x135E2A4 whether it has one
//                (the two only exist for the timeGetTime path, 0x4390E0..0x439157; the half
//                second timing the game uses here goes through the performance counter at
//                0x135E298 - 0x4392AF..0x439321 - off g_Supervisor.perfFrequency at 0x575BBC)
//     0x135DFEC  how many 500ms windows have been counted (0x439326..0x43934B)
//     0x135E0F0  the "%.02ffps" text the count is drawn as (sprintf at 0x43918D)
//     0x135DFF0  the replay speed text ("%2d", sprintf at 0x43928E)
//     0x575AD0 / 0x575AD4  Supervisor::framerateMultiplier and fpsAccumulator, the frame rate
//                correction the count drives (0x4391BB..0x439251); their ratio is the frame rate a
//                replay is recorded with (ReplayManager.cpp:626, ResultScreen.cpp:1176)
//     0x575AD8  Supervisor::curFps, the rounded frame rate (0x439274)
//
// They are saved and put back as one block, which is what keeps the game's own reading of its frame
// rate at 60 no matter how many times a frame is presented.
struct Th7FpsState {
    unsigned int frames_since_last_time; // 0x135E1F0
    unsigned int last_time;              // 0x135E2A0
    unsigned int last_time_valid;        // 0x135E2A4
    LARGE_INTEGER perf_counter;          // 0x135E298
    unsigned int update_counter;         // 0x135DFEC
    char fps_text[256];                  // 0x135E0F0
    char replay_text[256];               // 0x135DFF0
    float framerate_multiplier;          // 0x575AD0
    float fps_accumulator;               // 0x575AD4
    short cur_fps;                       // 0x575AD8
};

static void th7_fps_state_save(Th7FpsState* state) {
    state->frames_since_last_time = *(unsigned int*)0x0135E1F0;
    state->last_time = *(unsigned int*)0x0135E2A0;
    state->last_time_valid = *(unsigned int*)0x0135E2A4;
    state->perf_counter = *(LARGE_INTEGER*)0x0135E298;
    state->update_counter = *(unsigned int*)0x0135DFEC;
    memcpy(state->fps_text, (void*)0x0135E0F0, sizeof(state->fps_text));
    memcpy(state->replay_text, (void*)0x0135DFF0, sizeof(state->replay_text));
    state->framerate_multiplier = *(float*)0x00575AD0;
    state->fps_accumulator = *(float*)0x00575AD4;
    state->cur_fps = *(short*)0x00575AD8;
}

static void th7_fps_state_restore(const Th7FpsState* state) {
    *(unsigned int*)0x0135E1F0 = state->frames_since_last_time;
    *(unsigned int*)0x0135E2A0 = state->last_time;
    *(unsigned int*)0x0135E2A4 = state->last_time_valid;
    *(LARGE_INTEGER*)0x0135E298 = state->perf_counter;
    *(unsigned int*)0x0135DFEC = state->update_counter;
    memcpy((void*)0x0135E0F0, state->fps_text, sizeof(state->fps_text));
    memcpy((void*)0x0135DFF0, state->replay_text, sizeof(state->replay_text));
    *(float*)0x00575AD0 = state->framerate_multiplier;
    *(float*)0x00575AD4 = state->fps_accumulator;
    *(short*)0x00575AD8 = state->cur_fps;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the bullets
// ---------------------------------------------------------------------------
//
// The bullets the game draws live in g_BulletManager (0x0062F958). The manager's own draw
// (BulletManager::OnDraw, 0x426C40) walks six layer chains - the heads at manager+0x37A144
// (BulletManager::bulletsPtrs), a bullet's next at bullet+0xC04 - and draws each one it finds by
// calling Bullet::Draw (0x426B00, from 0x426F3E) for as long as there is a next one (0x426F46).
// The chains are rebuilt from the bullet array every frame by BulletManager::OnUpdate (0x425A50):
// it walks the 1024 slots, skips the ones whose state (bullet+0xBFC, 16 bit) is BULLET_INACTIVE,
// counts the rest into bulletCount (manager+0x37A128, 0x425B3B) and pushes each one it got to the
// end of the switch onto the chain its collision type names (bullet+0xB8A, 0x426357..0x426387).
// Walking those six chains is therefore walking exactly what the frame's draw pass puts on screen,
// at the position it puts it at - not a pool or a guess at one.
//
// A bullet's own position is what the draw reads: Bullet::Draw copies pos (bullet+0xB8C, three
// floats) plus the play field's corner into the sprite VM it is about to draw
// (0x426B6C..0x426B96). So a bullet is moved the way th08's shots are - its own position is moved
// before the pass and put back after, and no hook is needed.
//
// How far it moves in a frame is the velocity (bullet+0xB98, three floats) that the update adds to
// its position once a frame, scaled by what the bullet is doing at the time:
//
//     state 1  BULLET_NORMAL           pos += velocity              (0x425CB0..0x425CF7)
//     state 2  BULLET_SPAWNING_FAST    pos += velocity / 2.0f       (0x425F05.., fdiv 0x498A70 = 2)
//     state 3  BULLET_SPAWNING_NORMAL  pos += velocity / 2.5f       (0x426010.., fdiv 0x498D50 = 2.5)
//     state 4  BULLET_SPAWNING_SLOW    pos += velocity / 3.0f       (0x42611A, fdiv 0x498A8C = 3)
//     state 5  BULLET_DESPAWN          pos += velocity / 2.0f       (0x426202, fdiv 0x498A70 = 2)
//
// Reading the velocity, the way th08's bullets do, is the right thing here as well: what is in the
// field is the value the update just added to the position, so adding a fraction of it puts the
// bullet exactly a fraction of a frame's step on, and a bullet that is not moving is left where it
// is. Nothing else moves a bullet's position from one frame to the next - the four steps back it
// takes when it is spawned are part of the spawn, and the game's own pass draws it there too.
//
// One thing does stop all of that: while the game has time stopped, the update returns at the top
// and moves nothing at all, nor does it rebuild the chains (0x425A74..0x425A84 reads
// g_GameManager.isTimeStopped at 0x62627C and returns CONTINUE). There is no step to project then,
// and the bullets have to be left exactly where the game froze them.

static const unsigned int th7_bullet_layers = 6;
static const unsigned int th7_bullet_layer_heads_offset = 0x37A144;  // BulletManager::bulletsPtrs
static const unsigned int th7_bullet_count_offset = 0x37A128;        // BulletManager::bulletCount
static const unsigned int th7_bullet_pos_offset = 0xB8C;
static const unsigned int th7_bullet_velocity_offset = 0xB98;
static const unsigned int th7_bullet_state_offset = 0xBFC;           // 16 bit
static const unsigned int th7_bullet_next_offset = 0xC04;
static const unsigned int th7_bullets_max = 1024;                    // MAX_BULLETS
static unsigned char* th7_bullet_manager = (unsigned char*)0x0062F958; // g_BulletManager

// g_GameManager.isTimeStopped (0x62627C, GameManager+0x0C)
static unsigned char* th7_time_stopped = (unsigned char*)0x0062627C;

// The fraction of a frame's step the update moves a bullet by in each of the states above
static float th7_bullet_step_scale(unsigned int state) {
    switch (state) {
        case 2: // BULLET_SPAWNING_FAST
        case 5: // BULLET_DESPAWN
            return 0.5f;
        case 3: // BULLET_SPAWNING_NORMAL
            return 0.4f; // 1 / 2.5
        case 4: // BULLET_SPAWNING_SLOW
            return 1.0f / 3.0f;
        default: // BULLET_NORMAL
            return 1.0f;
    }
}

// What the last extra presentation found and moved, for the diagnostics
static unsigned int th7_bullets_in_play;
static unsigned int th7_bullets_projected;
static unsigned int th7_bullets_managers_say;

// Where the bullets' positions were before they were moved, so they can be put back exactly (not
// worked out backwards - the positions have to come out bit for bit what the game left there)
static unsigned char* th7_projected_bullets[th7_bullets_max];
static float th7_projected_positions[th7_bullets_max][3];
static unsigned int th7_projected_count;

static void th7_bullets_advance(float t) {
    th7_projected_count = 0;

    // Nothing moves while the game has time stopped (see the comment above), so neither do the
    // extras: the bullets stay exactly where the game froze them
    if (*th7_time_stopped)
        return;

    for (unsigned int layer = 0; layer < th7_bullet_layers; ++layer) {
        unsigned char* bullet = *(unsigned char**)(th7_bullet_manager + th7_bullet_layer_heads_offset + layer * 4);
        while (bullet != nullptr) {
            if (th7_projected_count >= th7_bullets_max)
                break;

            const unsigned int state = *(const unsigned short*)(bullet + th7_bullet_state_offset);
            const float scale = th7_bullet_step_scale(state);
            float* pos = (float*)(bullet + th7_bullet_pos_offset);
            const float* velocity = (const float*)(bullet + th7_bullet_velocity_offset);

            th7_projected_bullets[th7_projected_count] = bullet;
            th7_projected_positions[th7_projected_count][0] = pos[0];
            th7_projected_positions[th7_projected_count][1] = pos[1];
            th7_projected_positions[th7_projected_count][2] = pos[2];
            ++th7_projected_count;

            pos[0] = pos[0] + velocity[0] * scale * t;
            pos[1] = pos[1] + velocity[1] * scale * t;
            pos[2] = pos[2] + velocity[2] * scale * t;

            bullet = *(unsigned char**)(bullet + th7_bullet_next_offset);
        }
    }

    th7_bullets_in_play = th7_projected_count;
    th7_bullets_projected = th7_projected_count;
    th7_bullets_managers_say = *(unsigned int*)(th7_bullet_manager + th7_bullet_count_offset);
}

static void th7_bullets_undo() {
    for (unsigned int i = 0; i < th7_projected_count; ++i) {
        float* pos = (float*)(th7_projected_bullets[i] + th7_bullet_pos_offset);
        pos[0] = th7_projected_positions[i][0];
        pos[1] = th7_projected_positions[i][1];
        pos[2] = th7_projected_positions[i][2];
    }
    th7_projected_count = 0;
}

// One extra presentation of the frame: the block the game draws a frame with, once more, so the
// state the game is in is drawn and presented again.
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed. The
// stage 1 code below projects the objects forward by it.
static void th7_extra_presentation(float t) {
    IDirect3DDevice8* device = th7_device();
    if (device == nullptr)
        return;

    // Put back what the frame's own draw was started with (see th7_begin_game_draw) - the viewport
    // the game's own drawing reads as well as the one the device has, and the manager's own state
    if (th7_scene_captured) {
        *th7_game_viewport = th7_scene_game_viewport;
        device->SetViewport(&th7_scene_device_viewport);
        if (th7_scene_transforms) {
            device->SetTransform(D3DTS_VIEW, &th7_scene_view);
            device->SetTransform(D3DTS_PROJECTION, &th7_scene_projection);
        }

        void* manager = *CTextureManager::InstancePtr;
        if (manager != nullptr) {
            *(float*)((char*)manager + th7_anm_offset) = th7_scene_offset[0];
            *(float*)((char*)manager + th7_anm_offset + 4) = th7_scene_offset[1];
            *(unsigned char*)((char*)manager + th7_anm_camera_mode) = th7_scene_camera_mode;
        }
    }

    // Whatever was left bound is not what the manager thinks it is (see th7_forget_anm_texture)
    th7_forget_anm_texture();

    // The tool's overlay is drawn from the return of the draw chain inside this block, so it has to
    // know its frame is ready to be drawn again
    ToolState::ReadyToDraw();

    // Where the bullets are a fraction of a frame on from here; put back right after the pass drew
    // them (see the stage 1 comment above)
    th7_bullets_advance(t);

    // The game counts its own frame rate off this pass (see the comment above), so its state is put
    // back around it: the extras draw the same frame rate text the game's own pass drew, and the
    // game's own accounting never sees them
    Th7FpsState fps;
    th7_fps_state_save(&fps);
    th7_draw_pass();
    th7_fps_state_restore(&fps);

    th7_bullets_undo();

    // Presented with the device's own Present, without the hooks other patches and tools put in
    // front of it (see d3d8_present_bypassing_hooks, the same thing th08, th19 and th20 do): a
    // tool's present hook measures the input latency from the input poll to the next present, which
    // only means anything when there is one present per game frame - with the extras it would
    // report a fraction of a frame and take the extra presentations into the average. The patch's
    // own overlay is drawn from that path as well, so it stays on every presentation.
    d3d8_present_bypassing_hooks();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt (the
// same file name and ring buffer shape the th08 to th20 diagnostics use)
static const unsigned int th7_diag_frames_max = 300;
static const unsigned int th7_diag_presents_max = 8;
struct Th7DiagFrame {
    LARGE_INTEGER base;                                 // the game's own present of the frame
    unsigned int presents;
    LARGE_INTEGER present_time[th7_diag_presents_max];  // when each presentation happened
};
static Th7DiagFrame th7_diag_frames[th7_diag_frames_max];
static unsigned int th7_diag_count;
static bool th7_diag_key_down;
static LARGE_INTEGER th7_diag_perf_freq;

static void th7_diag_dump() {
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

    unsigned int count = th7_diag_count < th7_diag_frames_max ? th7_diag_count : th7_diag_frames_max;
    unsigned int start = th7_diag_count < th7_diag_frames_max ? 0 : th7_diag_count % th7_diag_frames_max;

    fprintf(file, "oilp frame interpolation diagnostics\n");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f, display refresh: %uHz\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "configured game frame rate: %u, BltPrepareTime %u, sleep %d\n",
            Config::GameFPS, Config::BltPrepareTime, (int)Config::Sleep);
    fprintf(file, "frames recorded: %u\n", count);
    // The game's own frame rate accounting, from its own side (see Th7FpsState): the rate it last
    // worked out, the text it draws for it, the frame rate correction it drives and the count it is
    // working on. That count is added to once per draw pass, the extras included, so reading what
    // the game itself makes of them is what says whether putting the state back works: steady 60
    // and "60.00fps" is right, and about three times that is the extras being counted
    fprintf(file, "game's own frame rate: %d, its text: \"%.16s\", correction: %.4f/%.4f, count: %u\n",
            (int)*(short*)0x00575AD8, (char*)0x0135E0F0, *(float*)0x00575AD0, *(float*)0x00575AD4,
            *(unsigned int*)0x0135E1F0);
    fprintf(file, "frameskip config: %u\n", (unsigned int)*(unsigned char*)0x00575A8B);
    // The bullets (see the stage 1 comment above): what the walk through the game's own layer
    // chains found, and what the manager's own counter says. The counter reads a few higher than
    // the chains hold, and that is the update loop's own doing: it counts a bullet as active
    // (0x425B3B) before it gets to the end of the switch that links it into a layer, and the ones
    // it turns off on the way there - the bullets whose despawn effect has finished or which left
    // the play field, which it puts back to BULLET_INACTIVE (0x417B20) - stay in the count it
    // leaves behind. The chains are the ones the frame's draw pass walks (BulletManager::OnDraw,
    // 0x426C40), which is what the extras project.
    fprintf(file, "bullets: %u in play, %u projected forward (last presentation)\n",
            th7_bullets_in_play, th7_bullets_projected);
    fprintf(file, "bullets: the manager's own count says %u (the update's count, taken before it "
                  "turns the bullets it removes off)\n", th7_bullets_managers_say);

    double presents = 0.0;
    double first = 0.0;
    double last = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const Th7DiagFrame& frame = th7_diag_frames[(start + i) % th7_diag_frames_max];
        fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
        for (unsigned int p = 0; p < frame.presents && p < th7_diag_presents_max; ++p)
            fprintf(file, " [%u] %.2fms", p,
                    (double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
                        (double)th7_diag_perf_freq.QuadPart);
        fprintf(file, "\n");
        presents += frame.presents;
        if (i == 0)
            first = (double)frame.base.QuadPart;
        last = (double)frame.base.QuadPart;
    }
    if (last > first && count > 0)
        fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
                presents / (double)count, presents * (double)th7_diag_perf_freq.QuadPart / (last - first));

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
// The game calls this from the two exits of GameWindow::Render (0x434A0E and 0x434A18); both are
// plain calls to the static GameWindow::Present (0x4345C0), which takes no arguments.
static void th7_present_hook() {
    if (th7_diag_perf_freq.QuadPart == 0)
        QueryPerformanceFrequency(&th7_diag_perf_freq);

    LARGE_INTEGER base;
    QueryPerformanceCounter(&base);

    // The game's own presentation of this frame, exactly as it always ran it. The back buffer
    // read-back its snapshot key does happens inside it, before any of the extras below draw over
    // it.
    th7_game_present();

    // The game's own work of this frame ends here as far as the overlay is concerned: everything
    // below is the patch presenting the same frame again
    Limiter::MarkFrameWorkEnd();

    // The tool has drawn its frame by now: the other half of watching for its frame state
    ToolState::WatchFrameDrawn();

    LARGE_INTEGER present_time[th7_diag_presents_max] = {};
    unsigned int presents = 1;
    present_time[0] = base;

    // The extras have nothing to draw in a frame the game didn't (see the frame skip note above),
    // and nothing to add while the presents are being thinned out - that is there to keep a frame
    // rate above the display's from being throttled by its presents, and extras would undo it
    if (th7_drew_frame && !Limiter::PresentingEveryNthFrame() && Limiter::ShouldPresent()) {
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

        // Where the game's own part of this frame started, so the extras can tell whether they
        // still fit before the next frame is due
        LARGE_INTEGER frame_start = Limiter::FrameStart();
        LARGE_INTEGER frame_wait = Limiter::FrameWait();
        LARGE_INTEGER game_work;
        game_work.QuadPart = base.QuadPart - frame_start.QuadPart;

        if (extras > 0 && spacing.QuadPart > 0) {
            // The presentations can't run past the end of the frame either: the limiter starts the
            // next frame one frame after this one started, and a frame that overruns it makes it
            // fall behind (and resync). An extra presentation costs about as much as the game's own
            // pass did, so the ones that wouldn't fit are dropped - the frame is then presented
            // fewer times, but never late.
            bool has_deadline = frame_start.QuadPart > 0 && game_work.QuadPart > 0;
            LARGE_INTEGER deadline;
            deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

            for (unsigned int k = 1; k <= extras; ++k) {
                LARGE_INTEGER target;
                target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
                if (has_deadline && target.QuadPart + game_work.QuadPart > deadline.QuadPart)
                    continue;

                Limiter::WaitUntil(target);

                if (presents < th7_diag_presents_max)
                    QueryPerformanceCounter(&present_time[presents]);
                ++presents;

                // Where this presentation sits between the game's own one and the next one: the
                // presentations of one game frame are spread evenly over it, so the k-th of
                // extras + 1 of them draws the state k/(extras + 1) frames along
                th7_extra_presentation((float)k / (float)(extras + 1));
            }
        }
    }

    th7_drew_frame = false;

    // Record the frame for the diagnostics
    {
        Th7DiagFrame& frame = th7_diag_frames[th7_diag_count % th7_diag_frames_max];
        frame.base = base;
        frame.presents = presents;
        for (unsigned int p = 0; p < th7_diag_presents_max; ++p)
            frame.present_time[p] = p < presents ? present_time[p] : present_time[0];
        ++th7_diag_count;
    }

    // U dumps the diagnostics
    if (GetAsyncKeyState('U') & 0x8000) {
        if (!th7_diag_key_down) {
            th7_diag_key_down = true;
            th7_diag_dump();
        }
    } else {
        th7_diag_key_down = false;
    }
}

// Takes over the two present calls the frame ends with
static void th7_install_interpolation() {
    {
        // call 0x4345C0, GameWindow::Present (0x434A0E) - the exit the catch-up loop takes, which
        // then goes round to draw again (taken when the game's own frame skip setting is on)
        BYTE* site = (BYTE*)0x00434A0E;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x004345C0 - (0x00434A0E + 5)) {
            patch_call(0x00434A0E, th7_present_hook);
        } else {
            printf("th07 present call at 0x434A0E doesn't look like a known one, skipping!\n");
        }
    }
    {
        // call 0x4345C0, GameWindow::Present (0x434A18) - the exit a normal frame takes, which
        // ends the frame there (curFrame = 0, g_FrameCount++)
        BYTE* site = (BYTE*)0x00434A18;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x004345C0 - (0x00434A18 + 5)) {
            patch_call(0x00434A18, th7_present_hook);
        } else {
            printf("th07 present call at 0x434A18 doesn't look like a known one, skipping!\n");
        }
    }
    {
        // The rate the frames are actually presented at, next to the render time line in the bottom
        // left corner - the same readout th08 and th10 to th20 show while the interpolation is on.
        // The presentation counting itself lives in the D3D8 hook, which sees every present. (The
        // overlay the two lines go on is only there when ShowOverlay is on, so there is nothing
        // else to check here.)
        overlay_show_present_rate(true);
    }

    // From here on the calculation chain hook draws the frame (see the stage 0 comment above)
    th7_interpolating = true;

    printf("Frame interpolation: th07 extra presentations installed (setting %d)\n", Config::Interpolation);
}

FPSTarget th7_replay_callback() {
    if (CEngine::Instance->state == 2 && ((CGame::Instance->flags >> 3) & 1) != 0) {
        auto input = get_input();
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}
