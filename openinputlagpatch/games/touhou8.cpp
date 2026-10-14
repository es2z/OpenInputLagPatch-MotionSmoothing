// Touhou 8: Imperishable Night v1.00d

#include <stdio.h>
#include <string.h>
#include "patch_util.h"
#include "d3d8_hook.h"
#include "limiter.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou8.h"
#include "config.h"

using namespace Touhou8;

CEngine* CEngine::Instance = (CEngine*)0x017CE758;
CGame* CGame::Instance = (CGame*)0x0160F508;

int __fastcall th8_window_update_hook(void* self) {
    Limiter::Tick();
    return CWindowManager__Update(self);
}

// ---------------------------------------------------------------------------
// Input handling, ported from vpatch (see the notes for the whole trail).
//
// th08 creates its keyboard device with SetCooperativeLevel(hwnd, 0x16) (0x446B81 / 0x446B98),
// which is DISCL_NOWINKEY | DISCL_FOREGROUND | DISCL_NONEXCLUSIVE: while the game window is not the
// foreground window the device is unacquired and its state stops being updated, so a key held when
// the focus was lost and released while away comes back still reading as held. The game never
// re-syncs the state when it is activated again (WM_ACTIVATEAPP at main.cpp:501 only touches the
// cursor flags, Controller::ResetKeyboard is startup-only, and the devices are only released on
// exit), so the key stays stuck until it is pressed again.
//
// vpatch installs four patches for this, unconditionally in its th08 build, and these are those
// four, at the same addresses:
//
//   1. the call to Controller::GetInput inside Supervisor::OnUpdate (0x4455F0, right after
//      g_LastFrameInput = g_CurFrameInput) is redirected into the wrapper below;
//   2. the function that ends at 0x452D52 (the one that fills the game's state copy) gets its ret
//      replaced by a store, so returning from it raises the flag;
//   3. 0x452319, at the top of the delayed-input-copy updater, spends the flag: the function
//      returns 1 straight away for that frame instead of updating the copy;
//   4. 0x452326's branch target moves from 0x452332 to 0x45234E, so the copy work is skipped in
//      the case that patch 2's flag is not the one being handled.
//
// Note what the wrapper does and does not do: it keeps 0x164D52C / 0x164D534 (the input copy and
// its previous frame, which the pause/menu edge detection reads) under its own control, with the
// Pause bit (bit 3) filtered out and a blanked frame when the flag is up, but it always returns the
// real input to the game logic. Nothing here touches the DirectInput device itself, so this is
// vpatch's own fix for its own symptom, not a general stale-device fix.
// ---------------------------------------------------------------------------
static DWORD th8_input_discard;

// Where the game calls Controller::GetInput (0x4455F0), vpatch's 60003DE0
static __int16 th8_input_hook() {
    __int16 input = get_input();
    if ((*(volatile BYTE*)0x0164D0B4 & 8) == 0) {
        __int16 copy = input;
        if (th8_input_discard != 0)
            copy = 0;
        *(volatile WORD*)0x0164D534 = *(volatile WORD*)0x0164D52C;
        *(volatile WORD*)0x0164D52C = (WORD)(copy & 0xFFF7);
    }
    return input;
}

// The ret at 0x452D52 of the function that fills the game's state copy (vpatch stores the flag
// from the value the game happens to have in edx there)
static __declspec(naked) void th8_input_flag_setter() {
    __asm { mov dword ptr [th8_input_discard], edx }
    __asm { ret }
}

// 0x452319, the top of the delayed-input-copy updater: spend the flag (vpatch's 60003E20)
static __declspec(naked) void th8_input_flag_consumer() {
    __asm { mov eax, dword ptr [th8_input_discard] }
    __asm { test eax, eax }
    __asm { jne spend }
    __asm { mov eax, dword ptr [0164D0B4h] }
    __asm { mov ecx, 0045231Eh }
    __asm { jmp ecx }
spend:
    __asm { xor eax, eax }
    __asm { mov dword ptr [th8_input_discard], eax }
    __asm { mov ecx, 0045247Bh }
    __asm { jmp ecx }
}

// Takes over the game's present call and the draw chain call that precedes it (defined below,
// after the comment that explains what a frame is made of)
static void th8_install_interpolation();

void th8_install_patches() {
    {
        // Skip the original frame limiter
        BYTE patch[] = { 0xEB };
        patch_bytes(0x00441EE6, patch, sizeof(patch));
    }
    {
        // Don't sleep in the main loop
        BYTE patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        patch_bytes(0x00441ECF, patch, sizeof(patch));
    }
    {
        // Hook window update
        patch_call(0x00441B15, th8_window_update_hook);
    }
    {
        // vpatch's input handling for this game (see the note above), applied the way vpatch
        // applies it: always, not behind a config option. It is what takes this game's frame of
        // input lag out, and it is also what keeps keys from being left held when the window
        // loses focus - a vpatched game has both, so this one has to as well.
        {
            // The call to Controller::GetInput in Supervisor::OnUpdate
            patch_call(0x004455F0, th8_input_hook);
        }
        {
            // The updater's flag check: return early instead of updating the copy
            patch_jmp(0x00452319, th8_input_flag_consumer);
        }
        {
            // Its branch target moves one step on, so the copy work is skipped
            BYTE patch[] = { 0x75, 0x26 };
            patch_bytes(0x00452326, patch, sizeof(patch));
        }
        {
            // The function that fills the state copy raises the flag where it returns
            patch_jmp(0x00452D52, th8_input_flag_setter);
        }
    }
    if (Config::Interpolation != 0 || Config::InterpolationFPS != 0)
        th8_install_interpolation();
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// A frame of this game, in the order GameWindow::Render (0x441E70..0x442051) runs it:
//
//     0x441F07  viewport = { 0, 0, 640, 480 } / device->SetViewport
//     0x441F48  call 0x43CA50    Chain::RunCalcChain - the input is read at its head, by
//                                Supervisor::OnUpdate (chain priority 0)
//     0x441FB4  device->BeginScene
//     0x441FC8  AnmManager::ClearVertexBuffer        (ECX = [0x18BDC90])
//     0x441FD3  [0x17CEAA8] = 0xFF                   g_Supervisor.fogState = FOG_UNSET
//     0x441FDD  call 0x448229    Supervisor::DisableFog               (ECX = 0x17CE758)
//     0x441FEC  call 0x43CB60    Chain::RunDrawChain - everything on screen is drawn here
//     0x441FF7  AnmManager::FlushVertexBuffer
//     0x44200E  device->SetTexture(0, NULL)
//     0x442022  device->EndScene
//     0x44203D  call 0x442060    GameWindow::Present
//     0x442069  device->Present  (through the hook the patch has on the device)
//                ... and then AnmManager::TakeScreencaptures reads back buffer 0 back, which the
//                afterimage effects and the pause screen background draw from
//
// The block from 0x441FB4 to 0x442027 is what draws the frame, and 0x44203D is where it reaches the
// display - so an extra presentation is that block run again and presented again, right after the
// game's own present, spread over what is left of the frame. That is the time the game would
// otherwise spend sitting in the limiter waiting for the next frame.
//
// Two things that happen inside that block have to be kept out of the extras:
//
//   - The game counts its own frame rate off the draw pass. Supervisor::DrawFpsCounter (0x445BC0,
//     a draw chain callback) calls Supervisor::CalculateFps (0x446F53) with its "draw it" argument
//     set, and that counts the pass (0x17CF4C0 += frameskipConfig + 1, 0x446F72..0x446F7C), keeps a
//     500ms window over it and works the game's own frame rate out of it. The rate it works out is
//     drawn in the bottom right corner ("%.02ffps" formatted into 0x17CF3B8, 0x447031..0x44703B)
//     and is what the game's frame rate correction is driven by (Supervisor::lagNumerator at
//     0x17CE8E8 and the rounded rate at 0x17CE8F0, both written by the same function). Run once
//     per extra presentation it would count three passes per frame, and the game would read its own
//     frame rate as three times what it is and correct for that. The state that function works in
//     is put back around every extra pass (th8_fps_state below), which leaves the game's accounting
//     exactly where its own pass left it and makes the extras draw the same frame rate text that
//     pass drew.
//   - The game's own frame skip (g_Supervisor.cfg.frameskipConfig <= framesSinceRedraw, 0x441FB2)
//     can leave a frame with no draw pass at all: it then presents the picture it already had. The
//     extras have nothing of their own to draw in that frame, so they are left out of it (see
//     th8_drew_frame) and the game's own present simply runs at its own rate.
//
// On text, this game's draw passes are self-contained: the string list everything draws through is
// drawn and then cleared inside the same pass (the low priority ascii pass runs
// AsciiManager::OnDrawLowPrioImpl and then ResetStrings, AsciiManager.cpp 189..195), and what
// appends to it (the HUD, Gui.cpp 1227 and on, at chain priority 17 against the ascii pass' 20)
// runs earlier in that same pass. So an extra pass draws the same text the game's own pass drew
// instead of stacking a copy of it on top, which is what th12 to th20 had to deal with here.

// The device the frame is drawn with and presented through. The game reads it from here itself
// (0x441FB4, 0x442069): g_Supervisor.d3dDevice.
static IDirect3DDevice8* th8_device() {
    return *(IDirect3DDevice8**)0x017CE760;
}

// The game's own calls the extra presentations make, at the addresses the game calls them from (see
// the frame above). The draw chain takes the supervisor's critical section itself, so it can be run
// from inside the frame the same way the game runs it.
static void** th8_anm_manager = (void**)0x018BDC90; // g_AnmManager
static void* th8_supervisor = (void*)0x017CE758;    // g_Supervisor
static void* th8_chain = (void*)0x0164F548;         // g_Chain
static DWORD* th8_fog_state = (DWORD*)0x017CEAA8;   // g_Supervisor.fogState

static auto th8_clear_vertex_buffer = (void(__fastcall*)(void*))0x00462E00; // 0x441FC8
static auto th8_flush_vertex_buffer = (void(__fastcall*)(void*))0x00462E40; // 0x441FF7
static auto th8_disable_fog = (int(__fastcall*)(void*))0x00448229;          // 0x441FDD
static auto th8_run_draw_chain = (int(__fastcall*)(void*))0x0043CB60;       // 0x441FEC
static auto th8_game_present = (void(__fastcall*)(void*))0x00442060;        // 0x44203D

// What the scene is drawn with, taken off the device where the game's own draw pass starts (the
// viewport is set to the play field while the stage is drawn and put back by the HUD, and the menus
// change it too, so an extra presentation has to put back whatever the frame it repeats used)
static D3DVIEWPORT8 th8_scene_viewport = {};
static D3DMATRIX th8_scene_view = {};
static D3DMATRIX th8_scene_projection = {};
static bool th8_scene_transforms = false;

// Whether the game drew this frame (see the frame skip note above)
static bool th8_drew_frame = false;

// The AnmManager's cached copy of the texture it last bound (AnmManager+0x24BC, compared against
// the sprite's texture at 0x464CCB in AnmManager::DrawInner and only re-bound when they differ).
//
// An extra presentation draws through the same manager, and the pass it repeats ended by setting
// the device's texture to NULL (0x44200E, the game's own last step) - and, with ShowOverlay on, by
// the overlay binding its own font texture. Either way the device no longer holds what that cache
// says it holds, and since the manager only calls SetTexture when the cache says it has to, the
// first sprites the extra draws would come out with whatever was left bound: the frame rate text
// and the loading screen's text end up as plain white quads with the overlay off, and as pieces of
// the overlay's font atlas (text that looks smeared) with it on. Forgetting the cache makes the
// pass bind what it draws, exactly the way the game's own pass does when it starts a frame.
//
// (0x18BDC90 holds the manager the game itself draws through - see th8_anm_manager.)
static void th8_forget_anm_texture() {
    if (*th8_anm_manager != nullptr)
        *(void**)((char*)*th8_anm_manager + 0x24BC) = nullptr;
}

// The state Supervisor::CalculateFps (0x446F53) works in. It is called from the draw chain (see the
// comment above), so every extra presentation runs it once more, and this is what puts the game's
// accounting back the way its own pass left it:
//
//     0x17CF4C0  the count of draw passes in the current window (0x446F72, 0x446F7C, 0x446FCE)
//     0x17CF4B8  when that window started and 0x17CF4BC whether it has one (0x446F8E..0x447008)
//     0x17CF3B8  the "%.02ffps" text that count is drawn as (sprintf at 0x44703B)
//     0x17CF2B8  the replay speed text ("%2d", sprintf at 0x447137)
//     0x17CF2A8..0x17CF2B4  the state that the replay path of the counter keeps
//     (0x447149..0x4471DF; the branch it is on is dead in this build - 0x17CEA58 is only ever read)
//     0x17CE8E8  Supervisor::lagNumerator, the lag the game compensates its frame rate for
//     0x17CE8F0  Supervisor::unk198, the rounded frame rate it works out (0x447122)
//
// The whole 0x17CF2A0..0x17CF4C4 block holds only those (nothing else in the executable reads or
// writes any of it), and the two supervisor fields sit outside it, so that is what is saved and put
// back.
struct Th8FpsState {
    unsigned char block[0x224];
    float lag_numerator;
    short rounded_rate;
};

static void th8_fps_state_save(Th8FpsState* state) {
    memcpy(state->block, (void*)0x017CF2A0, sizeof(state->block));
    state->lag_numerator = *(float*)0x017CE8E8;
    state->rounded_rate = *(short*)0x017CE8F0;
}

static void th8_fps_state_restore(const Th8FpsState* state) {
    memcpy((void*)0x017CF2A0, state->block, sizeof(state->block));
    *(float*)0x017CE8E8 = state->lag_numerator;
    *(short*)0x017CE8F0 = state->rounded_rate;
}

// Where the user's tool builds its frame and where it draws it (see tool_state.h). thprac's th08
// module hooks the return of Chain::RunCalcChain (0x43CB37) with its "th08_update" hook to build
// its overlay frame and the return of Chain::RunDrawChain (0x43CC45) with its "th08_render" hook to
// draw it; both are plain `ret`s, so the defaults are right for them.
static unsigned char* th8_tool_frame_address = (unsigned char*)0x0043CB37;
static unsigned char* th8_tool_draw_address = (unsigned char*)0x0043CC45;

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the bullets
// ---------------------------------------------------------------------------
//
// The bullets the game draws live in g_BulletManager (0x00F54E90). The manager keeps six layer
// chains (layerListHeads at manager+0x6BA554, a bullet's nextInLayer at bullet+0xDC0) and its own
// draw walks them one layer after the other: BulletManager::OnDraw (0x432B50) takes each layer head
// (0x432EC3), calls BulletManager::DrawSingleBullet (0x432F20) down the chain (0x432EDE) for as
// long as there is a next one, and that draws the bullet's sprite at the bullet's own position
// (bullet+0xD44, plus the screen shake the game adds there at 0x432F9A/0x432FC6). Walking those six
// chains is therefore walking exactly what the frame's draw pass puts on screen, at the position
// the sprite is drawn at - not a pool or a guess at one.
//
// A bullet moves by its velocity added to its position once a frame: the movers are handed the two
// fields by address (`add ecx,0D50h` / `add ecx,0D44h`, 0x43149E and on) and what they run into is
// a three-float add (0x410A70: `fld [pos.x] / fadd [vel.x] / fstp [pos.x]`, and the same for y and
// z). So an extra presentation puts the bullets where the frame's own step will put them t frames
// on: position += velocity * t, drawn, and put straight back again - the game's own logic and its
// collision code still see the positions they last read. What it costs is that a bullet whose
// velocity changes within the step (an accelerating or turning one) is drawn a little off the path
// it will actually take, an error the fraction of a frame it is drawn a fraction of does not show.

// The manager's fields (see the comment above), and the pool size the manager's own chains can't
// hold more than
static const unsigned int th8_bullet_layers = 6;
static const unsigned int th8_bullet_layer_heads_offset = 0x6BA554;
static const unsigned int th8_bullet_count_offset = 0x6BA538; // the manager's own numActiveBullets
static const unsigned int th8_bullet_next_in_layer_offset = 0xDC0;
static const unsigned int th8_bullet_pos_offset = 0xD44;
static const unsigned int th8_bullet_velocity_offset = 0xD50;
static const unsigned int th8_bullets_max = 0x601; // MAX_ENEMY_BULLETS + 1
static unsigned char* th8_bullet_manager = (unsigned char*)0x00F54E90;

// What the last extra presentation moved, for the diagnostics (see th8_diag_dump)
static unsigned int th8_bullets_in_play;
static unsigned int th8_bullets_projected;
static unsigned int th8_bullets_managers_say;

// Where the bullets' positions were before they were moved, so they can be put back exactly (not
// worked out backwards - the positions have to come out bit for bit what the game left there)
static unsigned char* th8_projected_bullets[th8_bullets_max];
static float th8_projected_positions[th8_bullets_max][3];
static unsigned int th8_projected_count;

static void th8_bullets_advance(float t) {
    th8_projected_count = 0;

    for (unsigned int layer = 0; layer < th8_bullet_layers; ++layer) {
        unsigned char* bullet = *(unsigned char**)(th8_bullet_manager + th8_bullet_layer_heads_offset + layer * 4);
        while (bullet != nullptr) {
            if (th8_projected_count >= th8_bullets_max)
                break;

            float* pos = (float*)(bullet + th8_bullet_pos_offset);
            const float* velocity = (const float*)(bullet + th8_bullet_velocity_offset);

            th8_projected_bullets[th8_projected_count] = bullet;
            th8_projected_positions[th8_projected_count][0] = pos[0];
            th8_projected_positions[th8_projected_count][1] = pos[1];
            th8_projected_positions[th8_projected_count][2] = pos[2];
            ++th8_projected_count;

            pos[0] = pos[0] + velocity[0] * t;
            pos[1] = pos[1] + velocity[1] * t;
            pos[2] = pos[2] + velocity[2] * t;

            bullet = *(unsigned char**)(bullet + th8_bullet_next_in_layer_offset);
        }
    }

    th8_bullets_in_play = th8_projected_count;
    th8_bullets_projected = th8_projected_count;
    th8_bullets_managers_say = *(unsigned int*)(th8_bullet_manager + th8_bullet_count_offset);
}

static void th8_bullets_undo() {
    for (unsigned int i = 0; i < th8_projected_count; ++i) {
        float* pos = (float*)(th8_projected_bullets[i] + th8_bullet_pos_offset);
        pos[0] = th8_projected_positions[i][0];
        pos[1] = th8_projected_positions[i][1];
        pos[2] = th8_projected_positions[i][2];
    }
    th8_projected_count = 0;
}

// Runs where the game's own draw pass starts: remembers what the scene is drawn with, watches for
// the tool's frame (its overlay is built by now, from the calc chain's return, and drawn on the
// return of the chain below), and then draws exactly the way the game always did.
static void __fastcall th8_render_hook(void* chain) {
    IDirect3DDevice8* device = th8_device();
    if (device != nullptr) {
        device->GetViewport(&th8_scene_viewport);
        th8_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th8_scene_view)) &&
                               SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th8_scene_projection));
    }

    ToolState::WatchFrameReady(th8_tool_frame_address, th8_tool_draw_address);

    th8_drew_frame = true;
    th8_run_draw_chain(chain);
}

// One extra presentation of the frame: the render block the game runs, once more, so the state the
// game is in is drawn and presented again.
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed. The
// stage 1 code projects the objects forward by it.
static void th8_extra_presentation(float t) {
    IDirect3DDevice8* device = th8_device();
    if (device == nullptr)
        return;

    device->BeginScene();

    // Put back what the scene was drawn with (see th8_render_hook)
    if (th8_scene_transforms) {
        device->SetTransform(D3DTS_VIEW, &th8_scene_view);
        device->SetTransform(D3DTS_PROJECTION, &th8_scene_projection);
    }
    if (th8_scene_viewport.Width != 0)
        device->SetViewport(&th8_scene_viewport);

    // The block itself, in the order the game runs it (0x441FC8..0x442022)
    th8_clear_vertex_buffer(*th8_anm_manager);
    *th8_fog_state = 0xFF;
    th8_disable_fog(th8_supervisor);

    ToolState::ReadyToDraw();

    // Whatever was left bound (see th8_forget_anm_texture) is not what the manager thinks it is
    th8_forget_anm_texture();

    // Where the bullets are a fraction of a frame on from here; put back right after the pass drew
    // them (see the stage 1 comment above)
    th8_bullets_advance(t);

    // The game counts its own frame rate off this pass (see the comment above), so its state is put
    // back around it: the extras draw the same frame rate text the game's own pass drew, and the
    // game's own accounting never sees them
    Th8FpsState fps;
    th8_fps_state_save(&fps);
    th8_run_draw_chain(th8_chain);
    th8_fps_state_restore(&fps);

    th8_bullets_undo();

    th8_flush_vertex_buffer(*th8_anm_manager);
    device->SetTexture(0, nullptr);
    device->EndScene();

    // Presented with the device's own Present, without the hooks other patches and tools put in
    // front of it (see d3d8_present_bypassing_hooks, the same thing th19 and th20 do): a tool's
    // present hook measures the input latency from the input poll to the next present, which only
    // means anything when there is one present per game frame - with the extras it would report a
    // fraction of a frame and take the extra presentations into the average. The patch's own
    // overlay is drawn from that path as well, so it stays on every presentation.
    d3d8_present_bypassing_hooks();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt (the
// same file name and ring buffer shape the th10-th20 diagnostics use)
static const unsigned int th8_diag_frames_max = 300;
static const unsigned int th8_diag_presents_max = 8;
struct Th8DiagFrame {
    LARGE_INTEGER base;                                 // the game's own present of the frame
    unsigned int presents;
    LARGE_INTEGER present_time[th8_diag_presents_max];  // when each presentation happened
};
static Th8DiagFrame th8_diag_frames[th8_diag_frames_max];
static unsigned int th8_diag_count;
static bool th8_diag_key_down;
static LARGE_INTEGER th8_diag_perf_freq;

static void th8_diag_dump() {
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

    unsigned int count = th8_diag_count < th8_diag_frames_max ? th8_diag_count : th8_diag_frames_max;
    unsigned int start = th8_diag_count < th8_diag_frames_max ? 0 : th8_diag_count % th8_diag_frames_max;

    fprintf(file, "oilp frame interpolation diagnostics\n");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f, display refresh: %uHz\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "configured game frame rate: %u, BltPrepareTime %u, sleep %d\n",
            Config::GameFPS, Config::BltPrepareTime, (int)Config::Sleep);
    fprintf(file, "frames recorded: %u\n", count);
    // The game's own frame rate accounting, from its own side (see Th8FpsState): the rate it read
    // out of the last 500ms window, the text it draws for it, and the lag it compensates for. Those
    // are counted off the draw passes the extras add, so reading what the game itself makes of them
    // is what says whether putting the state back works: steady 60 and "60.00fps" is right, and
    // about three times that is the extras being counted
    fprintf(file, "game's own frame rate: %d, its text: \"%.16s\", lag: %.4f\n",
            (int)*(short*)0x017CE8F0, (char*)0x017CF3B8, *(float*)0x017CE8E8);
    fprintf(file, "frameskip config: %u, loading (Supervisor::unk178): %u\n",
            (unsigned int)*(unsigned char*)0x017CE893, (unsigned int)*(DWORD*)0x017CE8D0);
    // The bullets (see the stage 1 comment above): what the walk through the game's own layer
    // chains found, and what the manager's own counter says. A walk that stopped finding bullets
    // would say so right here. The counter reads a few higher than the chains hold, and that is the
    // update loop's own doing: it counts a bullet as active when it looks at it (0x4312CA), before
    // it notices that the bullet's despawn effect has finished, and those it turns off there
    // (0x432170 sets the bullet's state back to 0 and it is never linked into a layer) are still in
    // the count it leaves behind. The chains are the ones the frame's draw pass walks
    // (BulletManager::OnDraw, 0x432EA5), which is what the extras project.
    fprintf(file, "bullets: %u in play, %u projected forward (last presentation)\n",
            th8_bullets_in_play, th8_bullets_projected);
    fprintf(file, "bullets: the manager's own count says %u (the update's count, taken before it "
                  "turns the finished ones off)\n", th8_bullets_managers_say);

    double presents = 0.0;
    double first = 0.0;
    double last = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const Th8DiagFrame& frame = th8_diag_frames[(start + i) % th8_diag_frames_max];
        fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
        for (unsigned int p = 0; p < frame.presents && p < th8_diag_presents_max; ++p)
            fprintf(file, " [%u] %.2fms", p,
                    (double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
                        (double)th8_diag_perf_freq.QuadPart);
        fprintf(file, "\n");
        presents += frame.presents;
        if (i == 0)
            first = (double)frame.base.QuadPart;
        last = (double)frame.base.QuadPart;
    }
    if (last > first && count > 0)
        fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
                presents / (double)count, presents * (double)th8_diag_perf_freq.QuadPart / (last - first));

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
static void __fastcall th8_present_hook(void* self) {
    if (th8_diag_perf_freq.QuadPart == 0)
        QueryPerformanceFrequency(&th8_diag_perf_freq);

    LARGE_INTEGER base;
    QueryPerformanceCounter(&base);

    // The game's own presentation of this frame, exactly as it always ran it. The capture of the
    // back buffer that the afterimage effects read happens inside it, before any of the extra
    // presentations below draw over it.
    th8_game_present(self);

    // The game's own work of this frame ends here as far as the overlay is concerned: everything
    // below is the patch presenting the same frame again
    Limiter::MarkFrameWorkEnd();

    // The tool has drawn its frame by now: the other half of watching for its frame state
    ToolState::WatchFrameDrawn();

    LARGE_INTEGER present_time[th8_diag_presents_max] = {};
    unsigned int presents = 1;
    present_time[0] = base;

    // The extras have nothing to draw in a frame the game didn't (see the frame skip note above),
    // and nothing to add while the presents are being thinned out - that is there to keep a frame
    // rate above the display's from being throttled by its presents, and extras would undo it
    if (th8_drew_frame && !Limiter::PresentingEveryNthFrame() && Limiter::ShouldPresent()) {
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

                if (presents < th8_diag_presents_max)
                    QueryPerformanceCounter(&present_time[presents]);
                ++presents;

                // Where this presentation sits between the game's own one and the next one: the
                // presentations of one game frame are spread evenly over it, so the k-th of
                // extras + 1 of them draws the state k/(extras + 1) frames along
                th8_extra_presentation((float)k / (float)(extras + 1));
            }
        }
    }

    th8_drew_frame = false;

    // Record the frame for the diagnostics
    {
        Th8DiagFrame& frame = th8_diag_frames[th8_diag_count % th8_diag_frames_max];
        frame.base = base;
        frame.presents = presents;
        for (unsigned int p = 0; p < th8_diag_presents_max; ++p)
            frame.present_time[p] = p < presents ? present_time[p] : present_time[0];
        ++th8_diag_count;
    }

    // U dumps the diagnostics
    if (GetAsyncKeyState('U') & 0x8000) {
        if (!th8_diag_key_down) {
            th8_diag_key_down = true;
            th8_diag_dump();
        }
    } else {
        th8_diag_key_down = false;
    }
}

// Takes over the draw chain call (to watch what the scene is drawn with and where the tool's frame
// is) and the present call that follows it
static void th8_install_interpolation() {
    {
        // call 0x43CB60, Chain::RunDrawChain (0x441FEC); the draw itself runs exactly the way it
        // always did
        BYTE* site = (BYTE*)0x00441FEC;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x0043CB60 - (0x00441FEC + 5)) {
            patch_call(0x00441FEC, th8_render_hook);
        } else {
            printf("th08 draw chain call at 0x441FEC doesn't look like a known one, skipping!\n");
        }
    }
    {
        // call 0x442060, GameWindow::Present (0x44203D)
        BYTE* site = (BYTE*)0x0044203D;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00442060 - (0x0044203D + 5)) {
            patch_call(0x0044203D, th8_present_hook);
        } else {
            printf("th08 present call at 0x44203D doesn't look like a known one, skipping!\n");
        }
    }
    {
        // The rate the frames are actually presented at, next to the render time line in the bottom
        // left corner - the same readout th10 to th20 show while the interpolation is on. The
        // presentation counting itself lives in the D3D8 hook, which sees every present. (The
        // overlay the two lines go on is only there when ShowOverlay is on, so there is nothing
        // else to check here.)
        overlay_show_present_rate(true);
    }
    printf("Frame interpolation: th08 extra presentations installed (setting %d)\n", Config::Interpolation);
}

FPSTarget th8_replay_callback() {
    if (CEngine::Instance->state == 2 && ((CGame::Instance->flags >> 3) & 1) != 0) {
        auto input = get_input();
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}
