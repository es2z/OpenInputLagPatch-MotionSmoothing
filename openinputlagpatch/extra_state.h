#pragma once

// A value an extra presentation has to put back before it draws, and put back the way the game's own
// pass left it afterwards.
//
// Several of the games set a screen shake (or an equivalent render offset) in their calculation
// chain, read it while drawing and clear it inside their own draw pass. A game that draws once per
// frame is fine with that, but an extra presentation that repeats the draw starts from the cleared
// value and draws the frame without the offset - one shaken picture and two still ones per frame,
// which reads as a fraction of the shake. The value is therefore taken where the game's own pass
// starts, written back before each extra pass, and the value the game's own pass left is put back
// afterwards.
//
// Every one of these is the same three steps, so they live here rather than being spelled out at
// each site: an ExtraFloatState is an address plus how many floats from it belong to the offset.
// Keep the state next to the game's other scene state and use extra_float_state_save in the render
// hook, then begin/end around the object walk of an extra presentation.
struct ExtraFloatState {
    float* address;
    int count;        // floats from address, 1 to 4
    float saved[4];   // what the game's own pass started with
    float before[4];  // what was there when the extra pass began
};

inline void extra_float_state_save(ExtraFloatState* state) {
    for (int i = 0; i < state->count; ++i)
        state->saved[i] = state->address[i];
}

inline void extra_float_state_begin(ExtraFloatState* state) {
    for (int i = 0; i < state->count; ++i) {
        state->before[i] = state->address[i];
        state->address[i] = state->saved[i];
    }
}

inline void extra_float_state_end(ExtraFloatState* state) {
    for (int i = 0; i < state->count; ++i)
        state->address[i] = state->before[i];
}
