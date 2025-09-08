#ifndef INPUT_H
#define INPUT_H
#include <stdint.h>
#include <SDL2/SDL.h>

typedef struct {
    uint8_t buttonState;  // bits for joypad
} InputState;

extern InputState input;

void handleButtonPress(SDL_Event *event);
uint8_t readJoypad(uint8_t select);

#endif