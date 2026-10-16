// Touhou 6: Embodiment of Scarlet Devil v1.02h

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d8_hook.h"
#include "touhou6.h"
#include "config.h"

using namespace Touhou6;

CEngine* CEngine::Instance = (CEngine*)0x006C6D18;
CWindowManager* CWindowManager::Instance = (CWindowManager*)0x006C6BD4;
CChainManager* CChainManager::Instance = (CChainManager*)0x0069D918;
CGame* CGame::Instance = (CGame*)0x0069BCA0;

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
        if (((engine->config.engine_flags >> 4) & 1) | ((engine->config.engine_flags >> 3) & 1)) {
            D3DVIEWPORT8 viewport;
            viewport.X = 0;
            viewport.Y = 0;
            viewport.Width = 640;
            viewport.Height = 480;
            viewport.MinZ = 0.0;
            viewport.MaxZ = 1.0;
            engine->d3d8_device->SetViewport(&viewport);
            engine->d3d8_device->Clear(0, 0, 3, *(D3DCOLOR*)0x004B7B60, 1.0, 0);
            engine->d3d8_device->SetViewport(&engine->viewport);
        }
        engine->d3d8_device->BeginScene();
        CChainManager__UpdateDrawChain(CChainManager::Instance);
        engine->d3d8_device->EndScene();
        engine->d3d8_device->SetTexture(0, 0);
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
