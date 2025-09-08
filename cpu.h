#ifndef CPU_H
#define CPU_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <SDL2/SDL.h> //for graphics and input of the game

// 16-bit register unions
typedef union { struct { uint8_t C, B; }; uint16_t BC; } RegBC;
typedef union { struct { uint8_t E, D; }; uint16_t DE; } RegDE;
typedef union { struct { uint8_t L, H; }; uint16_t HL; } RegHL;
typedef union { struct { uint8_t F, A; }; uint16_t AF; } RegAF;

//CPU struct
typedef struct {
    RegAF af;
    RegBC bc;
    RegDE de;
    RegHL hl;
    uint16_t SP;
    uint16_t PC;
    uint8_t IME;
    int haltMode;
    int EIFlag;
    int CBFlag;
    uint64_t cyclesAccumulated;
    int CPUtimer;
} CPUState;

extern CPUState CPUreg;

void initCPU();
void stepCPU();

#endif