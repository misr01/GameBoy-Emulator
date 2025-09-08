#include "memory.h"

MemoryState memory;

void initMemory() {
    memset(memory.vram, 0x00, sizeof(memory.vram));
    memset(memory.eram, 0x00, sizeof(memory.eram));
    memset(memory.wram, 0x00, sizeof(memory.wram));
    memset(memory.hram, 0x00, sizeof(memory.hram));
    memset(memory.oam, 0x00, sizeof(memory.oam));
    memset(memory.unusable, 0xFF, sizeof(memory.unusable));
    memset(memory.io, 0xFF, sizeof(memory.io));
    memset(memory.cartridge, 0xFF, sizeof(memory.cartridge));
    memory.ie_reg = 0x00;

    // 0x0000-0x3FFF: ROM Bank 0
    for (int i = 0x0000; i <= 0x3FFF; i++)
        memory.memoryMap[i] = &memory.cartridge[i];
    // 0x4000-0x7FFF: ROM Bank 1 (switchable, but just point to next for now)
    for (int i = 0x4000; i <= 0x7FFF; i++)
        memory.memoryMap[i] = &memory.cartridge[i];
    // 0x8000-0x9FFF: VRAM
    for (int i = 0x8000; i <= 0x9FFF; i++)
        memory.memoryMap[i] = &memory.vram[i - 0x8000];
    // 0xA000-0xBFFF: External RAM
    for (int i = 0xA000; i <= 0xBFFF; i++)
        memory.memoryMap[i] = &memory.eram[i - 0xA000];
    // 0xC000-0xDFFF: Work RAM
    for (int i = 0xC000; i <= 0xDFFF; i++)
        memory.memoryMap[i] = &memory.wram[i - 0xC000];
    // 0xE000-0xFDFF: Echo RAM (mirror of 0xC000-0xDDFF)
    for (int i = 0xE000; i <= 0xFDFF; i++)
        memory.memoryMap[i] = &memory.wram[i - 0xE000];
    // 0xFE00-0xFE9F: OAM
    for (int i = 0xFE00; i <= 0xFE9F; i++)
        memory.memoryMap[i] = &memory.oam[i - 0xFE00];
    // 0xFEA0-0xFEFF: Unusable
    for (int i = 0xFEA0; i <= 0xFEFF; i++)
        memory.memoryMap[i] = &memory.unusable[i - 0xFEA0];
    // 0xFF00-0xFF7F: I/O Registers
    for (int i = 0xFF00; i <= 0xFF7F; i++)
        memory.memoryMap[i] = &memory.io[i - 0xFF00];
    // 0xFF80-0xFFFE: High RAM
    for (int i = 0xFF80; i <= 0xFFFE; i++)
        memory.memoryMap[i] = &memory.hram[i - 0xFF80];
    // 0xFFFF: Interrupt Enable Register
    memory.memoryMap[0xFFFF] = &memory.ie_reg;

    *memory.memoryMap[0xFF00] = 0xCF; // set joypad register to 0xCF 
    *memory.memoryMap[0xFF01] = 0x00; // set serial transfer data register to 0
    *memory.memoryMap[0xFF02] = 0x7E; // set serial transfer control register to 0x7E
    *memory.memoryMap[0xFF04] = 0xAB; // set timer counter to 0xAB
    *memory.memoryMap[0xFF05] = 0x00; // set timer modulo to 0
    *memory.memoryMap[0xFF06] = 0x00; // set timer control to 0
    *memory.memoryMap[0xFF07] = 0xF8; // set timer control to 0xF8
    *memory.memoryMap[0xFF0F] = 0xE1; // set interrupt flag to 0xE1 (VBlank, LCDC, Timer)
    *memory.memoryMap[0xFF10] = 0x80; // set LCDC to 0x80 (LCD enabled, window enabled, sprite size 8x8)
    *memory.memoryMap[0xFF11] = 0xBF; // 
    *memory.memoryMap[0xFF12] = 0xF3; // 
    *memory.memoryMap[0xFF13] = 0xFF; //
    *memory.memoryMap[0xFF14] = 0xBF; // 
    *memory.memoryMap[0xFF16] = 0x3F; // 
    *memory.memoryMap[0xFF17] = 0x00; //
    *memory.memoryMap[0xFF18] = 0xFF; // 
    *memory.memoryMap[0xFF19] = 0xBF; //
    *memory.memoryMap[0xFF1A] = 0x7F; //
    *memory.memoryMap[0xFF1B] = 0xFF; //
    *memory.memoryMap[0xFF1C] = 0x9F; //
    *memory.memoryMap[0xFF1D] = 0xFF; //
    *memory.memoryMap[0xFF1E] = 0xBF; //
    *memory.memoryMap[0xFF20] = 0xFF; //
    *memory.memoryMap[0xFF21] = 0x00; //
    *memory.memoryMap[0xFF22] = 0x00; //
    *memory.memoryMap[0xFF23] = 0xBF; //
    *memory.memoryMap[0xFF24] = 0x77; // set LCDC status to 0x77 (LCD enabled, window enabled, sprite size 8x8)    
    *memory.memoryMap[0xFF25] = 0xF3; // 
    *memory.memoryMap[0xFF26] = 0xF1; // set LCDC to 0xF8 (LCD enabled, window enabled, sprite size 8x8)
    *memory.memoryMap[0xFF40] = 0x91; // set LCDC to 0x91 (LCD enabled, window enabled, sprite size 8x8)
    *memory.memoryMap[0xFF41] = 0x85; // STAT: set to 0x85 (mode 1, LY=0, LYC=0, OAM=1, VBlank=1)
    *memory.memoryMap[0xFF42] = 0x00; // set scroll Y to 0
    *memory.memoryMap[0xFF43] = 0x00; // set scroll X to 0
    *memory.memoryMap[0xFF44] = 0x00; // set LY to 0x00 (LY = 0)   
    *memory.memoryMap[0xFF45] = 0x00; // set LYC to 0
    *memory.memoryMap[0xFF46] = 0xFF; // set DMA to 0xFF 
    *memory.memoryMap[0xFF47] = 0xFC; // set BGP to 0xFC (background palette)
    *memory.memoryMap[0xFF48] = 0xFF; // set OBP0 to 0xFF (sprite palette 0)
    *memory.memoryMap[0xFF49] = 0xFF; // set OBP1 to 0xFF (sprite palette 1)
    *memory.memoryMap[0xFF4A] = 0x00; // set WY to 0
    *memory.memoryMap[0xFF4B] = 0x00; // set WX to 0
    *memory.memoryMap[0xFFFF] = 0x00; // set IE to 0
}

void loadROM(const char *namerom) { //Make sure to init Mem before calling
    FILE *romFile = fopen(namerom, "rb");
    if (!romFile) {
        fprintf(stderr, "Failed to open rom\n");
        exit(1);
    }
    fseek(romFile, 0, SEEK_END);
    memory.romSize = ftell(romFile);
    rewind(romFile);
    if (memory.romSize > sizeof(memory.cartridge)) {
        fprintf(stderr, "rom larger than 8MB\n");
        fclose(romFile);
        exit(1);
    }
    fread(memory.cartridge, 1, memory.romSize , romFile);
    fclose(romFile);
    switch(memory.cartridge[0x0147]) {
        case 0x00: memory.mbcType = 0; break; // ROM only
        case 0x01:
        case 0x02:
        case 0x03: memory.mbcType = 1; break; // MBC1
        case 0x05:
        case 0x06: memory.mbcType = 2; break; // MBC2
        case 0x0F:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13: memory.mbcType = 3; break; // MBC3
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E: memory.mbcType = 5; break; // MBC5
        default: memory.mbcType = 0; break; // fallback
}
        // --- Compute total ROM banks ---
    uint8_t romSizeByte = memory.cartridge[0x0148];
    if (romSizeByte <= 0x06)
        memory.totalRomBanks = 2 << romSizeByte;
    else if (romSizeByte == 0x52)
        memory.totalRomBanks = 72;
    else if (romSizeByte == 0x53)
        memory.totalRomBanks = 80;
    else if (romSizeByte == 0x54)
        memory.totalRomBanks = 96;
    else
        memory.totalRomBanks = 128; // fallback

    // --- Compute total RAM banks ---
    uint8_t ramSizeByte = memory.cartridge[0x0149];
    switch(ramSizeByte) {
        case 0x00: memory.totalRamBanks = 0; break;
        case 0x01: memory.totalRamBanks = 1; break; // 2 KB
        case 0x02: memory.totalRamBanks = 1; break; // 8 KB
        case 0x03: memory.totalRamBanks = 4; break; // 32 KB
        case 0x04: memory.totalRamBanks = 16; break; // 128 KB
        case 0x05: memory.totalRamBanks = 8; break;  // 64 KB
        default:   memory.totalRamBanks = 0; break;
    }

 //update ERAM and loadSRAM after calling
}

void updateERAMMapping() {
    if (memory.mbcType == 1) {
        // --- MBC1 ---
        uint32_t ram_offset = memory.mbc_ram_bank * 0x2000;
        for (int i = 0xA000; i <= 0xBFFF; i++) {
            memory.memoryMap[i] = (memory.mbc_ram_enable && memory.totalRamBanks > 0)
                         ? &memory.eram[ram_offset + (i - 0xA000)]
                         : &memory.unusable[0];
        }
    }
    else if (memory.mbcType == 3) {
        // --- MBC3 ---
        if (memory.mbc_ram_bank <= 0x03 && memory.totalRamBanks > 0) {
            uint32_t ram_offset = memory.mbc_ram_bank * 0x2000;
            for (int i = 0xA000; i <= 0xBFFF; i++) {
                memory.memoryMap[i] = memory.mbc_ram_enable
                             ? &memory.eram[ram_offset + (i - 0xA000)]
                             : &memory.unusable[0];
            }
        }
        else if (memory.mbc_ram_bank >= 0x08 && memory.mbc_ram_bank <= 0x0C) {
            // RTC registers not actual RAM, must be trapped in read/write
            for (int i = 0xA000; i <= 0xBFFF; i++) {
                memory.memoryMap[i] = &memory.unusable[0];
            }
        }
        else {
            // No RAM selected
            for (int i = 0xA000; i <= 0xBFFF; i++) {
                memory.memoryMap[i] = &memory.unusable[0];
            }
        }
    }
    else {
        // No MBC (or unsupported)
        for (int i = 0xA000; i <= 0xBFFF; i++)
            memory.memoryMap[i] = &memory.unusable[0];
    }
}

void printromHeader() { // for debug
    printf("rom Header Information:\n");
    printf("Entry Point: ");
    for (int i = 0x0100; i <= 0x0103; i++) {
        printf("%02X ", *memory.memoryMap[i]);
    }
    printf("\nNintendo Logo: ");
    for (int i = 0x0104; i <= 0x0133; i++) {
        printf("%02X ", *memory.memoryMap[i]);
    }
    printf("\nTitle: ");
    for (int i = 0x0134; i <= 0x0143; i++) {
        if (*memory.memoryMap[i] >= 32 && *memory.memoryMap[i] <= 126)
            printf("%c", *memory.memoryMap[i]);
        else
            printf(".");
    }
    printf("\nManufacturer Code: ");
    for (int i = 0x0144; i <= 0x0145; i++) {
        printf("%c", *memory.memoryMap[i]);
    }
    printf("\nCGB Flag: 0x%02X\n", *memory.memoryMap[0x0146]);
    printf("Cartridge Type: 0x%02X\n", *memory.memoryMap[0x0147]);
    printf("rom Size: 0x%02X\n", *memory.memoryMap[0x0148]);
    printf("Actual rom file size: %ld bytes\n", memory.romSize); 
    printf("RAM Size: 0x%02X\n", *memory.memoryMap[0x0149]);
    printf("Destination Code: 0x%02X\n", *memory.memoryMap[0x014A]);
    printf("Old License Code: 0x%02X\n", *memory.memoryMap[0x014B]);
    printf("Mask rom Version: 0x%02X\n", *memory.memoryMap[0x014C]);
    printf("Header Checksum: 0x%02X\n", *memory.memoryMap[0x014D]);
    printf("Global Checksum: 0x%02X%02X\n", *memory.memoryMap[0x014E], *memory.memoryMap[0x014F]);
}

void saveSRAM(const char *romname) {
    if (memory.totalRamBanks == 0) return; // no SRAM
    char savename[256];
    snprintf(savename, sizeof(savename), "%s.sav", romname);
    FILE *f = fopen(savename, "wb");
    if (f) {
        fwrite(memory.eram, 1, memory.totalRamBanks * 0x2000, f);
        fclose(f);
    }
}

void loadSRAM(const char *romname) {
    if (memory.totalRamBanks == 0) return;
    char savename[256];
    snprintf(savename, sizeof(savename), "%s.sav", romname);
    FILE *f = fopen(savename, "rb");
    if (f) {
        fread(memory.eram, 1, memory.totalRamBanks * 0x2000, f);
        fclose(f);
    }
}
