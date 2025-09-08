#ifndef MEMORY_H
#define MEMORY_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    uint8_t *memoryMap[0x10000];
    uint8_t cartridge[0x800000];
    uint8_t vram[0x2000];
    uint8_t eram[0x2000 * 16];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t io[0x80];
    uint8_t unusable[0x60];
    uint8_t ie_reg;

    long romSize;
    uint8_t mbcType;
    uint8_t totalRomBanks;
    uint8_t totalRamBanks;
    uint8_t mbc_rom_bank;
    uint8_t mbc_ram_bank;
    uint8_t mbc_ram_enable;
    uint8_t mbc1_mode;
    uint8_t mbc3_rtc_regs[5];
    uint8_t mbc3_rtc_latch;
} MemoryState;

extern MemoryState memory;

void initMemory();
void loadROM(const char *path);
void updateERAMMapping();
void printromHeader();
void saveSRAM(const char *romname);
void loadSRAM(const char *romname);

#endif