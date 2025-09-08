#include "ppu.h"
#include "memory.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>
#include <stdbool.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> //for graphics and input of the game

PPUState ppu;
int display[160][144]; //display array, 160x144 pixels, each pixel is 5x5 window pixel

// Initialise queue
void initQueue(Queue* q) {
    q->count = 0;
}

// Check if empty
int isEmpty(Queue* q) {
    return q->count == 0;
}

// Check if full
int isFull(Queue* q) {
    return q->count == 8;
}

// Enqueue value
int enqueue(Queue* q, Pixel pixel) {
    if (isFull(q)) return 1;
    q->data[q->count++] = pixel;
    //printf("Enqueued pixel, new count=%d\n", q->count);
    return 0;
}

// Dequeue and shift
int dequeue(Queue* q, Pixel* pixel) {
    if (isEmpty(q)) return 0;

    *pixel = q->data[0];

    // Shift elements
    for (int i = 1; i < q->count; i++) {
        q->data[i - 1] = q->data[i];
    }

    q->count--;
    return 1;
}
void initPPU() {
    ppu.DMAFlag = 0;
    ppu.DMACycles = 0;
    ppu.LCDdisabled = 0;
    ppu.LCDdelayflag = -1;

    ppu.scanlineTimer = 0;
    ppu.mode0Timer = 0;
    ppu.mode1Timer = 120;
    ppu.mode2Timer = 0;
    ppu.mode3Timer = 0;

    ppu.xPos = 0;
    ppu.scxCounter = 0;
    ppu.newScanLine = 1;
    ppu.windowLine = 0;
    ppu.windowOnLine = 0;
    ppu.wasEqual = 0;


    ppu.fetchStage.BGFetchStage = 0;
    ppu.fetchStage.objectFetchStage = 0;
    ppu.fetchStage.windowFetchMode = 0;

    ppu.BGFifo.count = 0;
    ppu.SpriteFifo.count = 0;

    memset(ppu.spriteBuffer, 0, sizeof(ppu.spriteBuffer));
    ppu.spriteCount = 0;
    memset(display, 0, sizeof(display));

}

void LCDUpdate(int enable) {
    if (enable) {
        ppu.LCDdelayflag = 4;
    }                                                       
    else{
        *memory.memoryMap[0xFF44] = 0; // Reset LY to 0
        *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x00; // Set mode to 0 (HBlank)
        *memory.memoryMap[0xFF41] &= ~(1 << 2); // Coincidence flag cleared (unless LY==LYC)
        ppu.mode0Timer = 0;
        ppu.mode1Timer = 0;
        ppu.mode2Timer = 0;
        ppu.scanlineTimer = 0;
        ppu.xPos = 0;
        ppu.LCDdisabled = 1; // Set LCD disabled flag
        ppu.newScanLine = 1;
        ppu.fetchStage.objectFetchStage = 0; // Reset object fetch stage
        ppu.fetchStage.BGFetchStage = 0; // Reset background fetch stage
        ppu.fetchStage.windowFetchMode = 0; // Reset window fetch mode
        for (int i = 0; i < 8; i++) { // Clear BGFifo
            Pixel discard;
            dequeue(&ppu.BGFifo, &discard);
            dequeue(&ppu.SpriteFifo, &discard); // Clear SpriteFifo as well

        }
    }
}

//Mode 2

int spriteSearchOAM(uint8_t ly, Sprite* buffer) { //pass in sprite array of size 10
    int count = 0; //track number of sprites in buffer currently
    Sprite spr;

    for (int i = 0; i < 40; i++) { //read all 40 sprites X attributes
        for (int j = 0; j < 4; j++) { 
            uint8_t address = memory.oam[i * 4 + j]; //get value at specific OAM address
            switch (j) {
                case 0: spr.yPos = address; break;
                case 1: spr.xPos = address; break;
                case 2: spr.tileNum = address; break;
                case 3: spr.flags = address; break;
            }
        }
        uint8_t lcdc = *memory.memoryMap[0xFF40]; //get sprites height from 2nd bit of lcdc
        int spriteHeight;
        if ((lcdc & 0x04) == 0){
            spriteHeight = 8;
        }
        else{
            spriteHeight = 16;
        }

        if (spr.xPos > 0 && (ly + 16) >= spr.yPos && (ly + 16) < (spr.yPos + spriteHeight)) { //check whether sprite is on the line
            if (count < 10) { //make sure only 10 sprites per line max
                buffer[count++] = spr;
            } else {
                break;
            }
        }
    }
    
        for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (buffer[j].xPos > buffer[j + 1].xPos) {
                Sprite tmp = buffer[j];
                buffer[j] = buffer[j + 1];
                buffer[j + 1] = tmp;
            }
        }
    }
    

    return count; //returns how many sprites in the sprite buffer
}

//Mode 3
void pixelPushBG(Queue* fifo, uint8_t xPos, FetchStage* stage, int fetchWindow) { //retrieve pixels tiles and push current tile row to FIFO Queue 
    //printf("pixelPushBG called, queue count=%d\n", fifo->count);
    if (isEmpty(fifo)) { //Queue must be empty before allowing more pixels to be pushed
        //printf("pixelPushBG: queue is empty, filling...\n");
        uint8_t lcdc = *memory.memoryMap[0xFF40]; // LCD Control
        uint8_t ly   = *memory.memoryMap[0xFF44]; // Current scanline
        uint8_t wx   = *memory.memoryMap[0xFF4B]; // Window X
        uint8_t wy   = *memory.memoryMap[0xFF4A]; // Window Y
        uint8_t bgp  = *memory.memoryMap[0xFF47]; // BG Palette

        uint16_t tileMapBase;
        uint16_t mapX, mapY;
        int useUnsignedTiles = (lcdc & 0x10) != 0; //Whether tiles are in signed/unsigned address area 

        if (fetchWindow) { //if fetching window tile
            // Window tile map base depends on LCDC bit 6
            tileMapBase = (lcdc & 0x40) ? 0x9C00 : 0x9800;

            // Window coordinates relative to window start, NO scrolling applied
            mapX = xPos - (wx - 7);
            mapY = ppu.windowLine; //ly - wy;
          //  printf("Window line: %d, LY: %d\n", windowLine, ly);
        } else { //if fetching background tile
            // Background tile map base depends on LCDC bit 3
            tileMapBase = (lcdc & 0x08) ? 0x9C00 : 0x9800;

            // Background coordinates - NO x scrolling applied here, scroll handled by xPos logic later
            uint8_t scy = *memory.memoryMap[0xFF42];
            mapY = (ly + scy) & 0xFF; //y scroll added with wrap 0-255

            uint8_t scx = *memory.memoryMap[0xFF43];
            mapX = (xPos + scx) & 0xFF; //coarse scroll
        }

        // Make sure mapX and mapY are valid (0-255)
        mapX &= 0xFF;
        mapY &= 0xFF;

        uint16_t tileRow = mapY / 8;
        uint16_t tileCol = mapX / 8;
        uint16_t tileIndexAddr = tileMapBase + tileRow * 32 + tileCol;
        int8_t tileNum = *memory.memoryMap[tileIndexAddr];

        uint16_t tileAddr;
        if (useUnsignedTiles) {
            tileAddr = 0x8000 + ((uint8_t)tileNum * 16);
        } else {
            tileAddr = 0x9000 + (tileNum * 16);
        }

        uint8_t line = mapY % 8;
        uint8_t byte1 = *memory.memoryMap[tileAddr + line * 2];
        uint8_t byte2 = *memory.memoryMap[tileAddr + line * 2 + 1];

        // Push 8 pixels (MSB to LSB)
        for (int i = 7; i >= 0; i--) {
            uint8_t lo = (byte1 >> i) & 1;
            uint8_t hi = (byte2 >> i) & 1;
            uint8_t colourId = (hi << 1) | lo;
            uint8_t colour = (bgp >> (colourId * 2)) & 0x03;

            Pixel p = {
                .colour = colour,
                .palette = 0,
                .bg_priority = 0,
                .is_sprite = 0,
                .sprite_index = 0
            };
            enqueue(fifo, p);
        }
        stage->BGFetchStage = 0; // Reset fetch stage after pushing
       //printf("TileNum: %d, Addr: 0x%04X, line=%d, byte1=0x%02X, byte2=0x%02X, TileIndexAddr: 0x%04X, TileRow=%d, TileCol=%d, tileMapBase=%04X, LCDC: 0x%02X, xPos: %d, scx: %d, scy: %d,fetchWindow: %d, wx: %d, windowline: %d, PC: %04X, wy: %d, windowFetchMode: %d, IE: 0x%02X, IF: 0x%02X, IME: %d\n", tileNum, tileAddr, *memory.memoryMap[0xFF44], byte1, byte2, tileIndexAddr, tileRow, tileCol, tileMapBase, lcdc, xPos, *memory.memoryMap[0xFF43], *memory.memoryMap[0xFF42],fetchWindow, *memory.memoryMap[0xFF4B], ppu.windowLine, CPUreg.PC, *memory.memoryMap[0xFF4A], ppu.fetchStage.windowFetchMode, *memory.memoryMap[0xFFFF], *memory.memoryMap[0xFF0F], CPUreg.IME);
    }
    
}

//SDL display buffer

void drawDisplay(SDL_Renderer *renderer){
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); //black pixels
    SDL_RenderClear(renderer); //turn screen to black

    for (int y = 0; y < 144; y++) {
        for (int x = 0; x < 160; x++) {
            int c = display[x][y];
            switch (c) {
                case 0: SDL_SetRenderDrawColor(renderer, 224, 248, 208, 255); break; // lightest
                case 1: SDL_SetRenderDrawColor(renderer, 136, 192, 112, 255); break; // light
                case 2: SDL_SetRenderDrawColor(renderer, 52, 104, 86, 255); break;   // dark
                case 3: SDL_SetRenderDrawColor(renderer, 8, 24, 32, 255); break;     // darkest
                default: continue; // skip if not a valid colour
            }
            SDL_Rect rect = { x * 5, y * 5, 5, 5 };
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}

//Interrupt
void checkLYC() {

    uint8_t ly  = *memory.memoryMap[0xFF44];
    uint8_t lyc = *memory.memoryMap[0xFF45];
    uint8_t* stat = memory.memoryMap[0xFF41];
    uint8_t* if_reg = memory.memoryMap[0xFF0F];

    uint8_t equal = (ly == lyc);

    if (equal) {
        *stat |= (1 << 2); // Set coincidence flag

        if (!ppu.wasEqual && (*stat & (1 << 6))) {
            *if_reg |= 0x02; // Request STAT interrupt (bit 1)
            //printf("LYC STAT interrupt requested at line %d, LYC: %d, STAT: 0x%02X, if_reg: 0x%02X\n", ly, lyc, *stat, *if_reg);
        }
    } else {
        *stat &= ~(1 << 2); // Clear coincidence flag
    }

    ppu.wasEqual = equal;
}

//Main PPU loop
void stepPPU(SDL_Renderer *renderer){
    if (ppu.LCDdelayflag == 0 && ppu.LCDdisabled == 1) { 
        *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x02; // Set mode to 2 (OAM)
        ppu.LCDdisabled = 0; // Reset LCD disabled flag
        //fprintf(logFile, "LCDC enabled, cycle: %ld\n", realCyclesAccumulated);
    }
    switch(*memory.memoryMap[0xFF41] & 0x03){ //get which mode the PPU is currently in
        case(0): //HBlank
            ppu.mode0Timer = 456 - ppu.scanlineTimer; //HBlank lasts 456 cycles, subtract the cycles already used in this scanline
            //printf("Mode 0: HBlank, Timer: %d\n", mode0Timer);
            if (ppu.mode0Timer == 0) { //if timer is 0, then
                if (*memory.memoryMap[0xFF44] == 143) { //Start VBlank 
                    (*memory.memoryMap[0xFF44])++; //ly++
                    checkLYC();
                    drawDisplay(renderer); //draw display to window 
                    SDL_RenderPresent(renderer); //update window with everything drawn

                    *memory.memoryMap[0xFF0F] |= 0x01; // Set VBlank flag in IF register

                    // Enter VBlank mode
                    *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x01; //set to mode1
                    if (*memory.memoryMap[0xFF41] & 0x10) { // Bit 4: VBlank STAT interrupt enable
                        *memory.memoryMap[0xFF0F] |= 0x02;  // STAT interrupt request flag
                        //printf("Vblank STAT interrupt requested at line %d\n", *memory.memoryMap[0xFF44]);
                    }

                    ppu.mode1Timer = 456; // VBlank lasts 10 lines of 456 cycles each
                    ppu.scanlineTimer = 0; //reset scanline timer
                }
                else{   
                    (*memory.memoryMap[0xFF44])++; //ly++
                    checkLYC(); //increment LY and check LYC register for coincidence with LY
                    *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x02; // Set to Mode 2 (OAM scan)
                    if (*memory.memoryMap[0xFF41] & 0x20) { // Bit 5: OAM STAT interrupt enable
                        *memory.memoryMap[0xFF0F] |= 0x02;  // STAT interrupt request flag
                        //printf("OAM STAT interrupt requested at line %d\n", *memory.memoryMap[0xFF44]);
                    }
                    ppu.scanlineTimer = 0; //reset scanline timer
                }
            ppu.windowOnLine = 0; //reset internal window line counter flag for that scanline, so doesn't increment multiple time per line
            }
            break;

        case(1): // VBlank
            if(*memory.memoryMap[0xFF44] != 153 && *memory.memoryMap[0xFF44] != 0){ //ly != 153 and != 0
                if(ppu.mode1Timer == 0){
                    (*memory.memoryMap[0xFF44])++; // increment LY 
                    checkLYC();

                    ppu.mode1Timer = 456; // VBlank lasts 10 lines of 456 cycles each
                    ppu.scanlineTimer = 0; //reset scanline timer
                }
                
                else { //if LY is 153, then we are in VBlank mode
                    ppu.mode1Timer-- ; //decrement mode1Timer until it reaches 0
                }
            }
            else{ //LY is 153, last VBLANK line
                if(ppu.mode1Timer == 448){ 
                    *memory.memoryMap[0xFF44]= 0; //line 153 quirk, after 8 cycles ly is set to 0, then continue rest of cycles to end of vblank
                    checkLYC();
                    ppu.mode1Timer--; //decrement mode1Timer until it reaches 0
                }
                else if(ppu.mode1Timer == 0){ 
                    ppu.scanlineTimer = 0; //reset scanline timer
                    *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x02; // Set to Mode 2 (OAM scan)
                    ppu.mode1Timer = 456; // reset timer for next VBlank
                    ppu.windowLine = 0; // reset internal window line counter
                }

                else {
                    ppu.mode1Timer--; //decrement mode1Timer until it reaches 0
                }
            }
            break;

        //OAM scan, wait 80 cycles then load the spriteBuffer and change to mode 3

        case(2):  // OAM Scan
            if(ppu.mode2Timer != 80){
                ppu.mode2Timer++; //wait 80 cycles before switching modes
            }
            else if(ppu.mode2Timer == 80){
                ppu.spriteCount = spriteSearchOAM(*memory.memoryMap[0xFF44], ppu.spriteBuffer); //store sprites in sprite buffer and no. of sprites
                ppu.mode2Timer = 0; //reset timer for next mode 2 check
                //*memoryMap[0xFF41] = (*memoryMap[0xFF41] & 0xFC) | (3 & 0x03); //set to mode 3
                *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x03;
            }
            break;

        //Mode 3 fetching and pushing queues, change to HBlank after scanline done, Vblank when all scanlines done

        case(3):  // BG/Window fetcher (with sprite mix)
            uint8_t lcdc = *memory.memoryMap[0xFF40];
            uint8_t wx   = *memory.memoryMap[0xFF4B];
            uint8_t wy   = *memory.memoryMap[0xFF4A];
            uint8_t scx  = *memory.memoryMap[0xFF43];

            int windowEnabled = (lcdc & 0x20) != 0;
            int windowStartX  = (int)wx - 7;

            // Window is *actually* visible at this pixel?
            int windowVisibleNow =
                windowEnabled &&
                (*memory.memoryMap[0xFF44] >= wy) &&          // window starts at WY and continues downward
                (ppu.xPos >= windowStartX);     // and only after WX-7 horizontally

            // One-time switch from BG -> Window when it first becomes visible this scanline
            if (!ppu.fetchStage.windowFetchMode && windowVisibleNow) {
                // flush any queued BG pixels so the window starts cleanly
                for (int i = 0; i < 8; i++) {
                    Pixel tmp;
                    dequeue(&ppu.BGFifo, &tmp);
                }
                ppu.fetchStage.BGFetchStage    = 0;
                ppu.fetchStage.windowFetchMode = 1;
                ppu.mode3Timer = 2;
            }

            // Advance the correct pipeline when ready
            if (ppu.fetchStage.windowFetchMode) {
                // If window got disabled mid-line, drop back to BG cleanly
                if (!windowEnabled) {
                    for (int i = 0; i < 8; i++) {
                        Pixel tmp;
                        dequeue(&ppu.BGFifo, &tmp);
                    }
                    ppu.fetchStage.BGFetchStage    = 0;
                    ppu.fetchStage.windowFetchMode = 0;
                    ppu.mode3Timer = 2;
                } else if (ppu.mode3Timer == 0) {
                    // Window pipeline stages
                    if      (ppu.fetchStage.BGFetchStage == 0) { ppu.fetchStage.BGFetchStage = 1; ppu.mode3Timer = 2; }
                    else if (ppu.fetchStage.BGFetchStage == 1) { ppu.fetchStage.BGFetchStage = 2; ppu.mode3Timer = 2; }
                    else if (ppu.fetchStage.BGFetchStage == 2) { ppu.fetchStage.BGFetchStage = 3; ppu.mode3Timer = 2; }
                    else /* BGFetchStage == 3 */ {
                        // xPos - (wx-7) handled inside pixelPush for window mode
                        pixelPushBG(&ppu.BGFifo, ppu.xPos, &ppu.fetchStage, /*windowMode=*/1);
                        ppu.mode3Timer = 2;
                    }
                }
            } else {
                // Background pipeline (runs even if windowEnabled=1 but not yet visible)
                if (ppu.mode3Timer == 0) {
                    if      (ppu.fetchStage.BGFetchStage == 0) { ppu.fetchStage.BGFetchStage = 1; ppu.mode3Timer = 2; }
                    else if (ppu.fetchStage.BGFetchStage == 1) { ppu.fetchStage.BGFetchStage = 2; ppu.mode3Timer = 2; }
                    else if (ppu.fetchStage.BGFetchStage == 2) { ppu.fetchStage.BGFetchStage = 3; ppu.mode3Timer = 2; }
                    else /* BGFetchStage == 3 */ {
                        // BG push (SCX handled by discarding below)
                        pixelPushBG(&ppu.BGFifo, ppu.xPos, &ppu.fetchStage, /*windowMode=*/0);
                        ppu.mode3Timer = 2;
                    }
                }
            }

            // Set discard count once per new scanline
            //Need to add 8 pixel discard
            if (ppu.xPos == 0 && ppu.newScanLine) {
                ppu.scxCounter = scx & 7; //lower 3 bits of scx for fine scroll, and with 0b0111
                ppu.newScanLine = 0;
            }

            // FIFO -> screen (sprite mixing preserved)
            if (ppu.BGFifo.count > 0) {
                if (ppu.scxCounter > 0) {
                    Pixel discard;
                    dequeue(&ppu.BGFifo, &discard);
                    ppu.scxCounter--;
                } else {
                    Pixel pixelToPush;
                    dequeue(&ppu.BGFifo, &pixelToPush);

                    int y = *memory.memoryMap[0xFF44]; //ly

                    uint8_t finalColour = 0;  
                    if (lcdc & 0x01) { //if BG/Window enable bit is 0 then send pixel of colour 0
                        finalColour = pixelToPush.colour;
                    }

                    // --- sprite mixing (unchanged logic) ---
                    if (lcdc & 0x02) { //if sprite bit is enabled
                        for (int i = 0; i < ppu.spriteCount; i++) {
                            Sprite *spr = &ppu.spriteBuffer[i];
                            if (spr->xPos == 0 || spr->xPos >= 168) continue;

                            int spriteX = spr->xPos - 8;
                            int spriteY = spr->yPos - 16;
                            int spriteHeight = (*memory.memoryMap[0xFF40] & 0x04) ? 16 : 8;

                            if (ppu.xPos >= spriteX && ppu.xPos < spriteX + 8 &&
                                y   >= spriteY && y   < spriteY + spriteHeight) {

                                int tileLine = y - spriteY;
                                if (spr->flags & 0x40) tileLine = spriteHeight - 1 - tileLine; // Y flip

                                uint16_t tileNum = spr->tileNum;
                                if (spriteHeight == 16) tileNum &= 0xFE;

                                uint16_t tileAddr = 0x8000 + tileNum * 16 + tileLine * 2;
                                uint8_t byte1 = *memory.memoryMap[tileAddr];
                                uint8_t byte2 = *memory.memoryMap[tileAddr + 1];

                                int bit = (spr->flags & 0x20) ? (ppu.xPos - spriteX) : (7 - (ppu.xPos - spriteX)); // X flip
                                int colorId = ((byte2 >> bit) & 1) << 1 | ((byte1 >> bit) & 1);
                                if (colorId != 0) {
                                    uint8_t palette = (spr->flags & 0x10) ? *memory.memoryMap[0xFF49] : *memory.memoryMap[0xFF48];
                                    uint8_t spriteColour = (palette >> (colorId * 2)) & 0x03;
                                    if (pixelToPush.colour == 0 || !(spr->flags & 0x80)) {
                                        finalColour = spriteColour;
                                    }
                                    break;
                                }
                            }
                        }
                    }

                    if (y < 144) display[ppu.xPos][y] = finalColour;
                    ppu.xPos++;
                }
            }

            // End of visible scanline -> HBlank
            if (ppu.xPos >= 160) {
                *memory.memoryMap[0xFF41] = (*memory.memoryMap[0xFF41] & ~0x03) | 0x00; // Mode 0 (HBlank)
                if (*memory.memoryMap[0xFF41] & 0x08) *memory.memoryMap[0xFF0F] |= 0x02; // STAT HBlank
                //printf("Hblank STAT interrupt requested at line %d\n", *memory.memoryMap[0xFF44]);
                ppu.xPos = 0;
                while (ppu.BGFifo.count > 0) { //clear FIFO for next scanline
                    Pixel tmp;
                    dequeue(&ppu.BGFifo, &tmp);
                }
                ppu.fetchStage.BGFetchStage = 0;
                ppu.newScanLine = 1;
                ppu.fetchStage.windowFetchMode = 0; //reset window fetch mode for next scanline
                // LY advance handled in HBlank
                if (windowVisibleNow && ppu.windowOnLine == 0) { //make sure windowLine only increments once per scanline if window is on it
                    ppu.windowLine++;
                    ppu.windowOnLine = 1;
                }
            } 

            
            break;
        }
                

    //SDL_Delay(1000/63); //fps
    if (ppu.LCDdisabled == 0) { //if LCD is enabled, then continue with PPU
        ppu.mode3Timer--; //decrement mode3Timer for next loop, if 0 then fetch next pixel
        ppu.scanlineTimer += 1; //increment scanline timer for each loop
        if (ppu.mode3Timer <= 0) {
            ppu.mode3Timer = 0; //reset mode3Timer for next loop
        }
    }
    ppu.LCDdelayflag--;
    checkLYC(); // Check LYC register for coincidence with LY regardless of LCD state

    if (ppu.DMACycles == 0 && ppu.DMAFlag == 1) { // If DMA transfer is completed
        ppu.DMAFlag = 0; // Reset DMA flag
    }
    else if (ppu.DMACycles > 0) { // If DMA transfer is in progress
        ppu.DMACycles--;
    }


}