#include "input.h"
#include "memory.h"

InputState input;

uint8_t readJoypad(uint8_t select) {
    // select: current value at 0xFF00 (written by CPU)
    uint8_t result = 0xCF; // Upper bits default to 1, bits 4 & 5 control selection
    result |= (select & 0x30); // preserve select bits

    if (!(select & (1 << 4))) {
        result &= (0xF0 | (input.buttonState & 0x0F));
    }
    if (!(select & (1 << 5))) {
        result &= (0xF0 | ((input.buttonState >> 4) & 0x0F));
    }

    return result;
}

void handleButtonPress(SDL_Event *event) {
    int pressed = (event->type == SDL_KEYDOWN) ? 0 : 1;
    uint8_t prevState = input.buttonState;

    switch (event->key.keysym.sym) {
        // D-pad (P14 group)
        case SDLK_w: if (pressed) input.buttonState |= (1 << 2); else input.buttonState &= ~(1 << 2); break; // Up
        case SDLK_s: if (pressed) input.buttonState |= (1 << 3); else input.buttonState &= ~(1 << 3); break; // Down
        case SDLK_a: if (pressed) input.buttonState |= (1 << 1); else input.buttonState &= ~(1 << 1); break; // Left
        case SDLK_d: if (pressed) input.buttonState |= (1 << 0); else input.buttonState &= ~(1 << 0); break; // Right

        // Action buttons (P15 group)
        case SDLK_v: if (pressed) input.buttonState |= (1 << 4); else input.buttonState &= ~(1 << 4); break; // A
        case SDLK_c: if (pressed) input.buttonState |= (1 << 5); else input.buttonState &= ~(1 << 5); break; // B
        case SDLK_r: if (pressed) input.buttonState |= (1 << 6); else input.buttonState &= ~(1 << 6); break; // Select
        case SDLK_f: if (pressed) input.buttonState |= (1 << 7); else input.buttonState &= ~(1 << 7); break; // Start
    }

    // Update 0xFF00 using correct readJoypad behavior
    *memory.memoryMap[0xFF00] = readJoypad(*memory.memoryMap[0xFF00]);

    // If a new button was pressed (bit changed from 1 → 0), request joypad interrupt
    if ((prevState & ~input.buttonState) & 0xFF) {
        *memory.memoryMap[0xFF0F] |= 0x10; // Bit 4: Joypad interrupt
    }
}