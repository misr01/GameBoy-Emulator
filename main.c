#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>
#include <stdbool.h>
#include <time.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> //for graphics and input of the game

#include "cpu.h"
#include "memory.h"
#include "input.h"
#include "ppu.h"

FILE *logFile = NULL; //for debugging

int main(){
    int div_counter = 0;
    int tima_counter = 0;

    SDL_Init(SDL_INIT_VIDEO);
    if (TTF_Init() < 0) {
    printf("Failed to initialize SDL_ttf: %s\n", TTF_GetError());
    return 1;
    }

    SDL_Window *window = SDL_CreateWindow("GB-EMU", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 720, 0); //window width and height 160x144
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED); //default driver gpu accelerated if possible

    initMemory();
    loadROM("Tetris.gb"); //initialises  memory in this function as well
    updateERAMMapping();
    loadSRAM("Tetris");
    initCPU();
    initPPU();
    printromHeader();
    printf("Press Enter to start...\n");
    getchar();

    logFile = fopen("emu_log.txt", "w"); //debug file
    if (!logFile) {
        perror("Failed to open log file");
        exit(1);
    }

    int open = 1;
    SDL_Event event;
    int overflowFlag = -1;
    uint8_t serialByte;
    int serialCounter = 0;
    int serialInProgress = 0;
    int isPaused = 0;
    int stepMode = 0;
    uint16_t divInternal = 0;
    uint16_t prev_div = 0;
    int realCyclesAccumulated = 0;

    while(open) {

            if (!isPaused || stepMode) {
                //fprintf(logFile, "PC: %04X, Opcode: %02X, Cycles: %d, SP: %04X, A: %02X, B: %02X, C: %02X, D: %02X, E: %02X, F: %02X, H: %02X, L: %02X, FF80: %02X, FF85: %02X, FF00: %02X, FFFF: %02X, FF0F: %02X, FF40: %02X, FF41: %02X, FF44: %02X, FF45: %02X\n", CPUreg.PC, *memory.memoryMap[CPUreg.PC], realCyclesAccumulated, CPUreg.SP, CPUreg.af.A, CPUreg.bc.B, CPUreg.bc.C, CPUreg.de.D, CPUreg.de.E, CPUreg.af.F, CPUreg.hl.H, CPUreg.hl.L, *memory.memoryMap[0xFF80], *memory.memoryMap[0xFF85], *memory.memoryMap[0xFF00], *memory.memoryMap[0xFFFF], *memory.memoryMap[0xFF0F], *memory.memoryMap[0xFF40], *memory.memoryMap[0xFF41], *memory.memoryMap[0xFF44], *memory.memoryMap[0xFF45]);
                stepCPU();
                stepPPU(renderer);     
                //fprintf(logFile, "xPos: %02X, LY: %02X, scx: %02X,BGFetchStage: %d, windowFetchMode %d, wy %02X, wx %02X, LCDC: %02X, BGFifoCount: %d \n  ", ppu.xPos, *memory.memoryMap[0xFF44], *memory.memoryMap[0xFF43], ppu.fetchStage.BGFetchStage, ppu.fetchStage.windowFetchMode, *memory.memoryMap[0xFF4A], *memory.memoryMap[0xFF4B], *memory.memoryMap[0xFF40], ppu.BGFifo.count);

                //Serial communication

                if (!serialInProgress && (*memory.memoryMap[0xFF02] & 0x80)) {
                    // Start transfer
                    serialInProgress = 1;
                    serialCounter = 1024;
                    serialByte = *memory.memoryMap[0xFF01];
                }

                if (serialInProgress) {
                    serialCounter--;
                    //*memoryMap[0xFF02] |= 0x80; // Set SC bit to indicate transfer in progress
                    if (serialCounter == 0) {
                        // Transfer complete
                        *memory.memoryMap[0xFF01] = 0xFF;   // echo back
                        *memory.memoryMap[0xFF02] &= ~0x80;       // clear SC
                        //*memoryMap[0xFF0F] |= 0x08;        // request serial interrupt
                        //don't request interrupt since no link cable support yet in this emulator
                        printf("%c", serialByte);          // optional
                        fflush(stdout);
                        serialInProgress = 0;
                    }
                }

                // Timer and DIV handling
                divInternal++; // increment by 1 CPU cycle
                *memory.memoryMap[0xFF04] = divInternal >> 8; // upper 8 bits visible as DIV

                uint8_t tac = *memory.memoryMap[0xFF07];
                if (tac & 0x04) { // Timer enabled
                    // Which DIV bit triggers TIMA, not actually done by cycles elapsed but monitoring DIV bits
                    const uint8_t timerBit[4] = {9, 3, 5, 7};
                    uint16_t mask = 1 << timerBit[tac & 0x03];

                    // Check for rising edge of the relevant DIV bit
                    if ((prev_div & mask) != 0 && (divInternal & mask) == 0) { //check if relevant bit changed from 1 to 0 (falling edge)
                        if (++(*memory.memoryMap[0xFF05]) == 0) { // TIMA overflow
                            overflowFlag = 4; // 4 CPU cycles delay for TIMA reload
                        }
                    }
                }
                prev_div = divInternal;

                // Handle TIMA reload and interrupt
                if (overflowFlag > 0) {
                    overflowFlag--; 
                    *memory.memoryMap[0xFF05] = 0; // keep TIMA at 0 for 4 cycle period
                    if (overflowFlag == 0) {
                        *memory.memoryMap[0xFF05] = *memory.memoryMap[0xFF06]; // Reload TIMA from TMA
                        *memory.memoryMap[0xFF0F] |= 0x04;              // Request timer interrupt
                    // printf("Timer interrrupt requested\n");
                    }
                }

                if (stepMode) stepMode = 0;
                
            } //end of pause
            realCyclesAccumulated++;
    if(realCyclesAccumulated % (16) == 0){
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                saveSRAM("Tetris");
                printf("Exiting emulator...\n");
                getchar();
                open = 0; //close emulator loop and exit
                break;
            }

            if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_SPACE: // Toggle pause
                        isPaused = !isPaused;
                        break;
                    case SDLK_n: // Step through instruction
                        if (isPaused) stepMode = 1;
                        break;
                }
            }

            handleButtonPress(&event);
            }
    }
        } //end of while open

            

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}