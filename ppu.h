#ifndef PPU_H
#define PPU_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>
#include <stdbool.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> //for graphics and input of the game
#include <SDL2/SDL_ttf.h>  // fonts

typedef struct {
    uint8_t colour;
    uint8_t palette;
    int bg_priority;
    int is_sprite;
    uint8_t sprite_index;
} Pixel;

typedef struct {
    Pixel data[8]; // array to store queue elements
    int count;  // number of elements in the queue
}Queue; //Use for FIFO

typedef struct {
    int BGFetchStage; //background
    int objectFetchStage; //sprite
    int windowFetchMode; //window 
} FetchStage;

typedef struct {
    uint8_t yPos; //first byte Y=16 is top of screen y < 16 has pixels chopped
    uint8_t xPos; //2nd byte X=8 is left of screen, x < 8 has pixels chopped 
    uint8_t tileNum; //third byte start from 8000
    uint8_t flags; //4th byte
} Sprite; //Structure to hold sprite attributes for each sprite

typedef struct {
    int DMAFlag;
    int DMACycles;
    int LCDdisabled;
    int LCDdelayflag;

    int scanlineTimer;
    int mode0Timer;
    int mode1Timer;
    int mode2Timer;
    int mode3Timer;

    int xPos;
    int scxCounter;
    int newScanLine;
    int windowLine;
    int windowOnLine;
    int wasEqual; //same line check LYC interrupt

    FetchStage fetchStage;
    Queue BGFifo;
    Queue SpriteFifo;
    Sprite spriteBuffer[10]; 
    int spriteCount;

} PPUState;

extern PPUState ppu;

void initPPU();
void stepPPU(SDL_Renderer *renderer);
void LCDUpdate(int enable);

#endif