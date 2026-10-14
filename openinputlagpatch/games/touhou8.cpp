// Touhou 8: Imperishable Night v1.00d

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
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

// patch_bytes' counterpart for a site that has to be jumped to rather than called (the target is
// our own code, so the relative offset can be worked out here)
static void th8_patch_jmp(DWORD site, void* func) {
    BYTE patch[5];
    patch[0] = 0xE9;
    *(DWORD*)(patch + 1) = (DWORD)func - site - 5;
    patch_bytes(site, patch, sizeof(patch));
}

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
    if (Config::FixInputGlitching) {
        // vpatch's input handling for this game (see the note above)
        {
            // The call to Controller::GetInput in Supervisor::OnUpdate
            patch_call(0x004455F0, th8_input_hook);
        }
        {
            // The updater's flag check: return early instead of updating the copy
            th8_patch_jmp(0x00452319, th8_input_flag_consumer);
        }
        {
            // Its branch target moves one step on, so the copy work is skipped
            BYTE patch[] = { 0x75, 0x26 };
            patch_bytes(0x00452326, patch, sizeof(patch));
        }
        {
            // The function that fills the state copy raises the flag where it returns
            th8_patch_jmp(0x00452D52, th8_input_flag_setter);
        }
    }
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
