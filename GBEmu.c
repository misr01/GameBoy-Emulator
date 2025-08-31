#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>
#include <stdbool.h>
#include <time.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> //for graphics and input of the game
#include <SDL2/SDL_ttf.h>  // fonts

FILE *logFile = NULL;



uint8_t* memoryMap[0x10000]; //pointer array for addressable memory for gameboy 65,536 addresses (address in C start from 0 so need size 0xFFFF + 1 = 0x10000)

/*
0x0000 - 0x3FFF	16 KB	rom Bank 0 (fixed)
0x4000 - 0x7FFF	16 KB	rom Bank 1+ (switchable)
0x8000 - 0x9FFF	8 KB	Video RAM (VRAM)
0xA000 - 0xBFFF	8 KB	External RAM (rom RAM)
0xC000 - 0xDFFF	8 KB	Work RAM (WRAM)
0xE000 - 0xFDFF	7.5 KB	Echo RAM (mirror of 0xC000-DFFF)
0xFE00 - 0xFE9F	160 bytes	Sprite attribute table (OAM)
0xFEA0 - 0xFEFF	96 bytes	Unusable memory area
0xFF00 - 0xFF7F	128 bytes	I/O registers
0xFF80 - 0xFFFE	127 bytes	High RAM (HRAM)
0xFFFF	                1 byte	Interrupt Enable register
*/

// 16 bit registers defined using union between 8 bit registers (e.g. BC shares same memory as B and C, so update to either updates BC), little endian order
typedef union {
    struct {
        uint8_t C; //low byte
        uint8_t B; //high byte
    };
    uint16_t BC;
} RegBC;

//union so struct and uint16_t share same memory
//struct assigns C to first byte in 16 bit allocated for struct, B to second byte
//uint16_t uses little endian order (highest memory = start of value), so C is the low byte (e.g. addr 100) and B is the high byte (addr 101) and since union same address as BC
// allows to access the registers as 8 bit or 16 bit values

typedef union {
    struct {
        uint8_t E;
        uint8_t D;
    };
    uint16_t DE;
} RegDE;

typedef union {
    struct {
        uint8_t L;
        uint8_t H;
    };
    uint16_t HL;
} RegHL;

typedef union {
    struct {
        uint8_t F;  // flag register
        uint8_t A;  // accumulator
    };
    uint16_t AF;
} RegAF;

// CPU registers combined
typedef struct {
    RegAF af;
    RegBC bc;
    RegDE de;
    RegHL hl;

    uint16_t SP;  // stack pointer
    uint16_t PC;  // program counter
    uint8_t IME; // Interrupt Master Enable flag
} CPUReg; //e.g. access 16 bit af then CPUregs.af.af , 8 bit a would be CPUregs.af.a etc...

CPUReg CPUreg; //global variable for CPU registers

uint8_t cartridge[0x800000] = {0}; // up to 8MB ROM
uint8_t vram[0x2000] = {0};        // 8KB Video RAM
uint8_t eram[0x2000 * 16] = {0}; // up to 16 banks      // 8KB External RAM
uint8_t wram[0x2000] = {0};        // 8KB Work RAM
uint8_t oam[0xA0] = {0};           // 160 bytes OAM
uint8_t hram[0x7F] = {0};          // 127 bytes High RAM
uint8_t io[0x80] = {0};            // 128 bytes I/O registers
uint8_t unusable[0x60];      // 96 bytes unusable area
uint8_t ie_reg = 0;                // Interrupt Enable register

long romSize = 0;

void initialiseMemoryMap() {
    memset(unusable, 0xFF, sizeof(unusable));
    memset(io,0xFF,sizeof(io));
    memset(cartridge,0xFF,sizeof(cartridge));
    // 0x0000-0x3FFF: ROM Bank 0
    for (int i = 0x0000; i <= 0x3FFF; i++)
        memoryMap[i] = &cartridge[i];
    // 0x4000-0x7FFF: ROM Bank 1 (switchable, but just point to next for now)
    for (int i = 0x4000; i <= 0x7FFF; i++)
        memoryMap[i] = &cartridge[i];
    // 0x8000-0x9FFF: VRAM
    for (int i = 0x8000; i <= 0x9FFF; i++)
        memoryMap[i] = &vram[i - 0x8000];
    // 0xA000-0xBFFF: External RAM
    for (int i = 0xA000; i <= 0xBFFF; i++)
        memoryMap[i] = &eram[i - 0xA000];
    // 0xC000-0xDFFF: Work RAM
    for (int i = 0xC000; i <= 0xDFFF; i++)
        memoryMap[i] = &wram[i - 0xC000];
    // 0xE000-0xFDFF: Echo RAM (mirror of 0xC000-0xDDFF)
    for (int i = 0xE000; i <= 0xFDFF; i++)
        memoryMap[i] = &wram[i - 0xE000];
    // 0xFE00-0xFE9F: OAM
    for (int i = 0xFE00; i <= 0xFE9F; i++)
        memoryMap[i] = &oam[i - 0xFE00];
    // 0xFEA0-0xFEFF: Unusable
    for (int i = 0xFEA0; i <= 0xFEFF; i++)
        memoryMap[i] = &unusable[i - 0xFEA0];
    // 0xFF00-0xFF7F: I/O Registers
    for (int i = 0xFF00; i <= 0xFF7F; i++)
        memoryMap[i] = &io[i - 0xFF00];
    // 0xFF80-0xFFFE: High RAM
    for (int i = 0xFF80; i <= 0xFFFE; i++)
        memoryMap[i] = &hram[i - 0xFF80];
    // 0xFFFF: Interrupt Enable Register
    memoryMap[0xFFFF] = &ie_reg;
}

void initReg() { //inialise CPU and other reg to values after boot sequence
    CPUreg.af.F = 0xB0; // set flags to default (Z=1, N=0, H=1, C=1)
    CPUreg.af.A = 0x01; // set accumulator to 1
    CPUreg.bc.B = 0x00; // set B to 0
    CPUreg.bc.C = 0x13; // set C to 19
    CPUreg.de.D = 0x00; // set D to 0
    CPUreg.de.E = 0xD8; // set E to 216
    CPUreg.hl.H = 0x01; // set H to 1
    CPUreg.hl.L = 0x4D; // set L to 77
    CPUreg.SP = 0xFFFE; // set stack pointer to end of memory
    CPUreg.PC = 0x0100; // start execution at address 0x0100
    CPUreg.IME = 0; // disable interrupts 

    *memoryMap[0xFF00] = 0xCF; // set joypad register to 0xCF 
    *memoryMap[0xFF01] = 0x00; // set serial transfer data register to 0
    *memoryMap[0xFF02] = 0x7E; // set serial transfer control register to 0x7E
    *memoryMap[0xFF04] = 0xAB; // set timer counter to 0xAB
    *memoryMap[0xFF05] = 0x00; // set timer modulo to 0
    *memoryMap[0xFF06] = 0x00; // set timer control to 0
    *memoryMap[0xFF07] = 0xF8; // set timer control to 0xF8
    *memoryMap[0xFF0F] = 0xE1; // set interrupt flag to 0xE1 (VBlank, LCDC, Timer)
    *memoryMap[0xFF10] = 0x80; // set LCDC to 0x80 (LCD enabled, window enabled, sprite size 8x8)
    *memoryMap[0xFF11] = 0xBF; // 
    *memoryMap[0xFF12] = 0xF3; // 
    *memoryMap[0xFF13] = 0xFF; //
    *memoryMap[0xFF14] = 0xBF; // 
    *memoryMap[0xFF16] = 0x3F; // 
    *memoryMap[0xFF17] = 0x00; //
    *memoryMap[0xFF18] = 0xFF; // 
    *memoryMap[0xFF19] = 0xBF; //
    *memoryMap[0xFF1A] = 0x7F; //
    *memoryMap[0xFF1B] = 0xFF; //
    *memoryMap[0xFF1C] = 0x9F; //
    *memoryMap[0xFF1D] = 0xFF; //
    *memoryMap[0xFF1E] = 0xBF; //
    *memoryMap[0xFF20] = 0xFF; //
    *memoryMap[0xFF21] = 0x00; //
    *memoryMap[0xFF22] = 0x00; //
    *memoryMap[0xFF23] = 0xBF; //
    *memoryMap[0xFF24] = 0x77; // set LCDC status to 0x77 (LCD enabled, window enabled, sprite size 8x8)    
    *memoryMap[0xFF25] = 0xF3; // 
    *memoryMap[0xFF26] = 0xF1; // set LCDC to 0xF8 (LCD enabled, window enabled, sprite size 8x8)
    *memoryMap[0xFF40] = 0x91; // set LCDC to 0x91 (LCD enabled, window enabled, sprite size 8x8)
    *memoryMap[0xFF41] = 0x85; // STAT: set to 0x85 (mode 1, LY=0, LYC=0, OAM=1, VBlank=1)
    *memoryMap[0xFF42] = 0x00; // set scroll Y to 0
    *memoryMap[0xFF43] = 0x00; // set scroll X to 0
    *memoryMap[0xFF44] = 0x00; // set LY to 0x00 (LY = 0)   
    *memoryMap[0xFF45] = 0x00; // set LYC to 0
    *memoryMap[0xFF46] = 0xFF; // set DMA to 0xFF 
    *memoryMap[0xFF47] = 0xFC; // set BGP to 0xFC (background palette)
    *memoryMap[0xFF48] = 0xFF; // set OBP0 to 0xFF (sprite palette 0)
    *memoryMap[0xFF49] = 0xFF; // set OBP1 to 0xFF (sprite palette 1)
    *memoryMap[0xFF4A] = 0x00; // set WY to 0
    *memoryMap[0xFF4B] = 0x00; // set WX to 0
    *memoryMap[0xFFFF] = 0x00; // set IE to 0
}

int debug = 0; // debug mode flag, set to 1 to enable debug output

void logStackTop() {
    if (debug) {
        printf("SP=0x%04X Top=0x%02X%02X\n", CPUreg.SP, memoryMap[CPUreg.SP + 1], memoryMap[CPUreg.SP]);
    }
}



uint8_t mbcType = 0; // MBC type, 0 = no MBC, 1 = MBC1, etc.
uint8_t totalRomBanks;
uint8_t totalRamBanks;

uint8_t mbc1_mode = 0; // 0 = ROM banking, 1 = RAM banking
uint8_t mbc_rom_bank = 1;   // shared between MBC1/MBC3
uint8_t mbc_ram_enable = 0;
uint8_t mbc_ram_bank = 0;   // for MBC1 (mode=1) or MBC3

uint8_t mbc3_rtc_regs[5];   // 08–0C
uint8_t mbc3_rtc_latch = 0;
uint8_t mbc3_rtc_selected = 0xFF;

void updateERAMMapping() {
    if (mbcType == 1) {
        // --- MBC1 ---
        uint32_t ram_offset = mbc_ram_bank * 0x2000;
        for (int i = 0xA000; i <= 0xBFFF; i++) {
            memoryMap[i] = (mbc_ram_enable && totalRamBanks > 0)
                         ? &eram[ram_offset + (i - 0xA000)]
                         : &unusable[0];
        }
    }
    else if (mbcType == 3) {
        // --- MBC3 ---
        if (mbc_ram_bank <= 0x03 && totalRamBanks > 0) {
            uint32_t ram_offset = mbc_ram_bank * 0x2000;
            for (int i = 0xA000; i <= 0xBFFF; i++) {
                memoryMap[i] = mbc_ram_enable
                             ? &eram[ram_offset + (i - 0xA000)]
                             : &unusable[0];
            }
        }
        else if (mbc_ram_bank >= 0x08 && mbc_ram_bank <= 0x0C) {
            // RTC registers → not actual RAM, must be trapped in read/write
            for (int i = 0xA000; i <= 0xBFFF; i++) {
                memoryMap[i] = &unusable[0];
            }
        }
        else {
            // No RAM selected
            for (int i = 0xA000; i <= 0xBFFF; i++) {
                memoryMap[i] = &unusable[0];
            }
        }
    }
    else {
        // No MBC (or unsupported)
        for (int i = 0xA000; i <= 0xBFFF; i++)
            memoryMap[i] = &unusable[0];
    }
}

void saveSRAM(const char *romname) {
    if (totalRamBanks == 0) return; // no SRAM
    char savename[256];
    snprintf(savename, sizeof(savename), "%s.sav", romname);
    FILE *f = fopen(savename, "wb");
    if (f) {
        fwrite(eram, 1, totalRamBanks * 0x2000, f);
        fclose(f);
    }
}

void loadSRAM(const char *romname) {
    if (totalRamBanks == 0) return;
    char savename[256];
    snprintf(savename, sizeof(savename), "%s.sav", romname);
    FILE *f = fopen(savename, "rb");
    if (f) {
        fread(eram, 1, totalRamBanks * 0x2000, f);
        fclose(f);
    }
}

void loadrom(const char *namerom) {
    FILE *romFile = fopen(namerom, "rb");
    if (!romFile) {
        fprintf(stderr, "Failed to open rom\n");
        exit(1);
    }
    initialiseMemoryMap(); // set up pointers after loading ROM
    initReg();
    fseek(romFile, 0, SEEK_END);
    romSize = ftell(romFile);
    rewind(romFile);
    if (romSize > sizeof(cartridge)) {
        fprintf(stderr, "rom larger than 8MB\n");
        fclose(romFile);
        exit(1);
    }
    fread(cartridge, 1, romSize , romFile);
    fclose(romFile);
    switch(cartridge[0x0147]) {
        case 0x00: mbcType = 0; break; // ROM only
        case 0x01:
        case 0x02:
        case 0x03: mbcType = 1; break; // MBC1
        case 0x05:
        case 0x06: mbcType = 2; break; // MBC2
        case 0x0F:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13: mbcType = 3; break; // MBC3
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E: mbcType = 5; break; // MBC5
        default: mbcType = 0; break; // fallback
}
        // --- Compute total ROM banks ---
    uint8_t romSizeByte = cartridge[0x0148];
    if (romSizeByte <= 0x06)
        totalRomBanks = 2 << romSizeByte;
    else if (romSizeByte == 0x52)
        totalRomBanks = 72;
    else if (romSizeByte == 0x53)
        totalRomBanks = 80;
    else if (romSizeByte == 0x54)
        totalRomBanks = 96;
    else
        totalRomBanks = 128; // fallback

    // --- Compute total RAM banks ---
    uint8_t ramSizeByte = cartridge[0x0149];
    switch(ramSizeByte) {
        case 0x00: totalRamBanks = 0; break;
        case 0x01: totalRamBanks = 1; break; // 2 KB
        case 0x02: totalRamBanks = 1; break; // 8 KB
        case 0x03: totalRamBanks = 4; break; // 32 KB
        case 0x04: totalRamBanks = 16; break; // 128 KB
        case 0x05: totalRamBanks = 8; break;  // 64 KB
        default:   totalRamBanks = 0; break;
    }

    updateERAMMapping();
    loadSRAM("Tetris"); // load SRAM if available
}


/* rom header big Endian
0x0100-0x0103	4	Entry Point	Initial CPU instructions executed on boot
0x0104-0x0133	48	Nintendo Logo Bitmap	Must match official logo for cartridge to boot
0x0134-0x0143	16	Title	Game title (up to 16 characters)
0x0144-0x0145	2	Manufacturer Code	Usually 4-character code (some newer carts)
0x0146	1	CGB Flag	Indicates if game supports Game Boy Color
0x0147	1	Cartridge Type (MBC Type)	Indicates MBC, RAM, battery, etc.
0x0148	1	rom Size	Number of rom banks (code size indicator)
0x0149	1	RAM Size	External RAM size in cartridge
0x014A	1	Destination Code	0 = Japanese, 1 = Non-Japanese
0x014B	1	Old Licensee Code	Deprecated licensee identifier
0x014C	1	Mask rom Version Number	Version of the cartridge
0x014D	1	Header Checksum	Checksum of the header bytes 0x0134-0x014C
0x014E-0x014F	2	Global Checksum	Checksum of entire rom
*/

void printromHeader() { // for debug
    printf("rom Header Information:\n");
    printf("Entry Point: ");
    for (int i = 0x0100; i <= 0x0103; i++) {
        printf("%02X ", *memoryMap[i]);
    }
    printf("\nNintendo Logo: ");
    for (int i = 0x0104; i <= 0x0133; i++) {
        printf("%02X ", *memoryMap[i]);
    }
    printf("\nTitle: ");
    for (int i = 0x0134; i <= 0x0143; i++) {
        if (*memoryMap[i] >= 32 && *memoryMap[i] <= 126)
            printf("%c", *memoryMap[i]);
        else
            printf(".");
    }
    printf("\nManufacturer Code: ");
    for (int i = 0x0144; i <= 0x0145; i++) {
        printf("%c", *memoryMap[i]);
    }
    printf("\nCGB Flag: 0x%02X\n", *memoryMap[0x0146]);
    printf("Cartridge Type: 0x%02X\n", *memoryMap[0x0147]);
    printf("rom Size: 0x%02X\n", *memoryMap[0x0148]);
    printf("Actual rom file size: %ld bytes\n", romSize); 
    printf("RAM Size: 0x%02X\n", *memoryMap[0x0149]);
    printf("Destination Code: 0x%02X\n", *memoryMap[0x014A]);
    printf("Old Licensee Code: 0x%02X\n", *memoryMap[0x014B]);
    printf("Mask rom Version: 0x%02X\n", *memoryMap[0x014C]);
    printf("Header Checksum: 0x%02X\n", *memoryMap[0x014D]);
    printf("Global Checksum: 0x%02X%02X\n", *memoryMap[0x014E], *memoryMap[0x014F]);
}

uint64_t cyclesAccumulated = 0; //T Cycles accumulated, one M cycle = 4 T cycles


void updateBanks() {
    // --- ROM bank 0 ---
    for (int i = 0x0000; i <= 0x3FFF; i++)
        memoryMap[i] = &cartridge[i];

    // --- Switchable ROM bank ---
    uint8_t bank = mbc_rom_bank & 0x7F;
    if (bank == 0) bank = 1;
    if (bank >= totalRomBanks) bank %= totalRomBanks;

    uint32_t rom_offset = bank * 0x4000;
    for (int i = 0x4000; i <= 0x7FFF; i++)
        memoryMap[i] = &cartridge[rom_offset + (i - 0x4000)];

    // --- External RAM / RTC ---
    if (mbcType == 1) { // MBC1
        uint8_t ram_bank = 0;
        if (mbc1_mode) {
            if (mbc_ram_bank < totalRamBanks)
                ram_bank = mbc_ram_bank;
        }
        uint32_t ram_offset = ram_bank * 0x2000;
        for (int i = 0xA000; i <= 0xBFFF; i++)
            memoryMap[i] = (mbc_ram_enable && totalRamBanks > 0)
                         ? &eram[ram_offset + (i - 0xA000)]
                         : &unusable[0];
    }
    else if (mbcType == 3) { // MBC3
        if (mbc_ram_bank <= 0x03 && totalRamBanks > 0) {
            uint32_t ram_offset = mbc_ram_bank * 0x2000;
            for (int i = 0xA000; i <= 0xBFFF; i++)
                memoryMap[i] = mbc_ram_enable
                             ? &eram[ram_offset + (i - 0xA000)]
                             : &unusable[0];                                                
        } else if (mbc_ram_bank >= 0x08 && mbc_ram_bank <= 0x0C) {
            // Map RTC registers (fake: reads/writes handled manually)
            for (int i = 0xA000; i <= 0xBFFF; i++)
                memoryMap[i] = &unusable[0]; // trap accesses
        } else {
            for (int i = 0xA000; i <= 0xBFFF; i++)
                memoryMap[i] = &unusable[0];
        }
    }
}

void handleMBCWrite(uint16_t addr, uint8_t value) {
    if (mbcType == 1) {
        if (addr <= 0x1FFF) {
            mbc_ram_enable = ((value & 0x0F) == 0x0A);
            updateBanks();
        }
        else if (addr <= 0x3FFF) {
            mbc_rom_bank = (mbc_rom_bank & 0x60) | (value & 0x1F);
            if ((mbc_rom_bank & 0x1F) == 0) mbc_rom_bank |= 1;
            updateBanks();
        }
        else if (addr <= 0x5FFF) {
            uint8_t upper2 = value & 0x03;
            if (mbc1_mode)
                mbc_ram_bank = upper2;
            else {
                mbc_rom_bank = (mbc_rom_bank & 0x1F) | (upper2 << 5);
                if ((mbc_rom_bank & 0x7F) == 0) mbc_rom_bank |= 1;
            }
            updateBanks();
        }
        else if (addr <= 0x7FFF) {
            mbc1_mode = value & 0x01;
            updateBanks();
        }
    }
    else if (mbcType == 3) {
        if (addr <= 0x1FFF) {
            mbc_ram_enable = ((value & 0x0F) == 0x0A);                                                          
            updateBanks();
        }
        else if (addr <= 0x3FFF) {
            mbc_rom_bank = value & 0x7F;
            if (mbc_rom_bank == 0) mbc_rom_bank = 1;
            updateBanks();
        }
        else if (addr <= 0x5FFF) {
            mbc_ram_bank = value;  // 0–3 = RAM, 08–0C = RTC
            updateBanks();
        }
        else if (addr <= 0x7FFF) {
            if (mbc3_rtc_latch == 0 && value == 1) { //only triggers when going from 0 to 1
                // Latch RTC registers from system clock
                time_t t = time(NULL);
                struct tm *tm = localtime(&t);

                mbc3_rtc_regs[0] = tm->tm_sec;        // seconds
                mbc3_rtc_regs[1] = tm->tm_min;        // minutes
                mbc3_rtc_regs[2] = tm->tm_hour;       // hours
                mbc3_rtc_regs[3] = tm->tm_mday & 0xFF; // lower 8 bits of day
                mbc3_rtc_regs[4] = ((tm->tm_yday & 0x01) << 0) | 0; // upper day bit, control flags = 0    
                }
            mbc3_rtc_latch = value;
            }
        }
    }
    

uint8_t buttonState = 0xFF; // All buttons released (bits 0–7 high)

uint8_t readJoypad(uint8_t select) {
    // select: current value at 0xFF00 (written by CPU)
    uint8_t result = 0xCF; // Upper bits default to 1, bits 4 & 5 control selection
    result |= (select & 0x30); // preserve select bits

    if (!(select & (1 << 4))) {
        result &= (0xF0 | (buttonState & 0x0F));
    }
    if (!(select & (1 << 5))) {
        result &= (0xF0 | ((buttonState >> 4) & 0x0F));
    }

    return result;
}

void handleButtonPress(SDL_Event *event) {
    int pressed = (event->type == SDL_KEYDOWN) ? 0 : 1;
    uint8_t prevState = buttonState;

    switch (event->key.keysym.sym) {
        // D-pad (P14 group)
        case SDLK_w: if (pressed) buttonState |= (1 << 2); else buttonState &= ~(1 << 2); break; // Up
        case SDLK_s: if (pressed) buttonState |= (1 << 3); else buttonState &= ~(1 << 3); break; // Down
        case SDLK_a: if (pressed) buttonState |= (1 << 1); else buttonState &= ~(1 << 1); break; // Left
        case SDLK_d: if (pressed) buttonState |= (1 << 0); else buttonState &= ~(1 << 0); break; // Right

        // Action buttons (P15 group)
        case SDLK_v: if (pressed) buttonState |= (1 << 4); else buttonState &= ~(1 << 4); break; // A
        case SDLK_c: if (pressed) buttonState |= (1 << 5); else buttonState &= ~(1 << 5); break; // B
        case SDLK_r: if (pressed) buttonState |= (1 << 6); else buttonState &= ~(1 << 6); break; // Select
        case SDLK_f: if (pressed) buttonState |= (1 << 7); else buttonState &= ~(1 << 7); break; // Start
    }

    // Update 0xFF00 using correct readJoypad behavior
    *memoryMap[0xFF00] = readJoypad(*memoryMap[0xFF00]);

    // If a new button was pressed (bit changed from 1 → 0), request joypad interrupt
    if ((prevState & ~buttonState) & 0xFF) {
        *memoryMap[0xFF0F] |= 0x10; // Bit 4: Joypad interrupt
    }
}

int DMAFlag = 0; // DMA transfer flag, 0 = no transfer, 1 = transfer in progress
int DMAcycles = 0; // DMA cycles remaining, 0 = no transfer in progress
int LCDdisabled = 0; // LCD disabled flag, 0 = enabled, 1 = disabled
int LCDdelayflag = -1; // LCD delay flag for turning back on, set to -1 so no mode2 switch on startup ppu

    int scanlineTimer = 0; // amount of cycles for scanline
    int mode0Timer = 0; // amount of cycles for Hblank
    int mode1Timer = 120; // amount of cycles for Vblank
    int mode2Timer = 0;
    int mode3Timer = 0; // amount of cycles needed to start next operation

    int xPos = 0;
    int scxCounter = 0;
    int newScanLine = 1;

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

// Initialize queue
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

typedef struct {
    int BGFetchStage; //background
    int objectFetchStage; //sprite
    int windowFetchMode; //window 
} FetchStage;

    FetchStage fetchStage = {
        .objectFetchStage = 0,
        .BGFetchStage = 0,
        .windowFetchMode = 0 //starting from 0 means initialise fetch mode for window and clear BGFifo, and fetcher stages
    };
    Queue BGFifo = {
        .count = 0
    };
    Queue SpriteFifo = {
        .count = 0
    };

int realCyclesAccumulated = 0; //reset cycles accumulated

void LDVal8(uint8_t value, uint8_t *dest, uint16_t addr, bool isMemory, uint16_t src) {

        if (addr == 0xFF40) {
        printf(">>> LCDC WRITE: %02X at PC=%04X\n", value, CPUreg.PC);
    }





    uint8_t mode = *memoryMap[0xFF41] & 0x03; //PPU mode (0 = HBlank, 1 = VBlank, 2 = OAM, 3 = Transfer)
    if(addr == 0xFF40 && (value >> 7) == 0) { // If LCDC is disabled, reset LY to 0
        *memoryMap[0xFF44] = 0; // Reset LY to 0
        *memoryMap[0xFF41] = (*memoryMap[0xFF41] & ~0x03) | 0x00; // Set mode to 0 (HBlank)
        *memoryMap[0xFF41] &= ~(1 << 2); // Coincidence flag cleared (unless LY==LYC)
        mode0Timer = 0;
        mode1Timer = 0;
        mode2Timer = 0;
        scanlineTimer = 0;
        xPos = 0;
        LCDdisabled = 1; // Set LCD disabled flag
        newScanLine = 1;
        fetchStage.objectFetchStage = 0; // Reset object fetch stage
        fetchStage.BGFetchStage = 0; // Reset background fetch stage
        fetchStage.windowFetchMode = 0; // Reset window fetch mode
        for (int i = 0; i < 8; i++) { // Clear BGFifo
            Pixel discard;
            dequeue(&BGFifo, &discard);
            dequeue(&SpriteFifo, &discard); // Clear SpriteFifo as well
    }
    //fprintf(logFile, "LCDC disabled, cycle: %ld\n", realCyclesAccumulated);
    *dest = value; // Write value to LCDC
    return; // Exit early if LCDC is disabled
}

    if(addr == 0xFF40 && (value >> 7) == 1 && LCDdisabled == 1) { // If LCDC is enabled, reset LY to 
        LCDdelayflag = 4; // Set LCD delay flag to turn back on
        //fprintf(logFile, "LCDC delay flag, cycle: %ld\n", realCyclesAccumulated);
        *dest = value; // Write value to LCDC
        return; // Exit early if LCDC is enabled
    }

    if ((src >= 0xA000 && src <= 0xBFFF) && mbcType == 3 && mbc_ram_bank >= 0x08 && mbc_ram_bank <= 0x0C){
        // Map bank to RTC register index
        uint8_t rtcIndex = mbc_ram_bank - 0x08;
        value = mbc3_rtc_regs[rtcIndex];  // Return RTC value 
    }
    
    // Block VRAM writes during Mode 3 (Pixel Transfer)
    if (addr >= 0x8000 && addr <= 0x9FFF && mode == 3) {
        return;
    }
 
    if (src <= 0x9FFF && src >= 0x8000 && mode == 3) {
        value = 0xFF;
        // Block read from VRAM during Mode 3
    }
        
    if (src == 0xFF0F) {
        value |= 0xE0;  // ensure bits 5-7 are 1
    }

    // Block OAM writes during Mode 2, 3, or DMA
    if (addr >= 0xFE00 && addr <= 0xFE9F &&(mode == 2 || mode == 3 || DMAFlag == 1)) {
        return;
    }
    if (src >= 0xFE00 && src <= 0xFE9F &&
        (mode == 2 || mode == 3 || DMAFlag == 1)) {
        value = 0xFF;
         // Block read from OAM during Mode 2, 3, or DMA
    }


    if (src == 0xFF00){
        // Special case for Joypad register
        value = readJoypad(*memoryMap[0xFF00]);
    }
        
    if (isMemory) {
        // Intercept ROM writes
        if (((addr >= 0x0000 && addr <= 0x7FFF) || (addr >= 0xA000 && addr <= 0xBFFF))) {
            handleMBCWrite(addr,value);
            // ROM or (external RAM in MBC mode, if not mapped)
            return;
        }

        // Block unusable area
        if (addr >= 0xFEA0 && addr <= 0xFEFF) return;

        // I/O special cases
        switch (addr) {
            case 0xFF0F:  // IF
                // Writes: mask out upper bits
                *dest = (value & 0x1F) | 0xE0;
                return;
            case 0xFFFF:  // IE
                // Writes: accept all bits (0–4 valid, 5–7 stored)
                *dest = value;  // no masking to upper bits
                return;
            case 0xFF00:  // Joypad
                // Lower 4 bits are read-only (button state), mask them off
                *dest = (*dest & 0xCF) | (value & 0x30);  // Keep only bits 4 and 5
                *memoryMap[0xFF00] = readJoypad(*memoryMap[0xFF00]);
                return;
            case 0xFF04:  // DIV (Divider)
                *dest = 0;  // Writing resets to 0
                return;

            case 0xFF41:  // STAT
                // Only bits 3–6 are writable
                *dest = (*dest & 0x87) | (value & 0x78);
                return;

            case 0xFF44:  // LY (Current scanline)
                *dest = 0;
                printf("0xFF44 Written to (LY)");

                return;

            case 0xFF46:  // DMA transfer request

                DMAFlag = 1;
                *dest = value; // Store DMA source address
                uint8_t dmaSource = *memoryMap[0xFF46]; // Get DMA source address
                uint16_t sourceAddr = dmaSource << 8; // Convert to 16-bit address
                uint16_t destAddr = 0xFE00; // OAM starts at 0xFE00
                for (int i = 0; i < 0xA0; i++) { // Transfer 160 bytes (40 sprites * 4 bytes each)
                    *memoryMap[destAddr + i] = *memoryMap[sourceAddr + i];
                }
                DMAcycles = 640; // DMA takes 640 T cycles to complete
                //getchar(); // Wait for user input to continue
                //fprintf(logFile, "DMA transfer completed from %02X to OAM\n", dmaSource);
                //printf("DMA transfer completed from %02X to OAM\n", dmaSource);
                //cyclesAccumulated += 640; // DMA timing delay
            
                 return;


            

            // add other I/O special cases here

            
        }

        // Normal memory write
        if (dest) *dest = value;
    } else {
        // Register write
        if (dest) *dest = value;
    }
}

void LDVal16(uint16_t value, uint16_t *dest) { //load 16-bit value into destination register
    *dest = value;
    //cyclesAccumulated += 8; // 8 T cycles for 16-bit load
    //fprintf(logFile, "[LDVal16] Loaded 0x%04X into %p (PC=%04X) Cycles=%d\n", value, dest, CPUreg.PC, cyclesAccumulated);
}

// read/write helpers for pointer-based memory map:
void setZeroFlag(int value) { //set zero flag or reset based on value given
if (value == 0) {
    CPUreg.af.F &= 0x7F; // clear zero flag
} else {
    CPUreg.af.F |= 0x80; // set zero flag
}
CPUreg.af.F &= 0xF0; // Optional: clear unused bits before writing/pushing
}

int getZeroFlag(){
    return (CPUreg.af.F & 0x80) != 0;
}

void setSubtractFlag(int value) { //set subtract flag or reset based on value given
if (value == 0) {
    CPUreg.af.F &= 0xBF; // clear subtract flag
} else {
    CPUreg.af.F |= 0x40; // set subtract flag
}
CPUreg.af.F &= 0xF0; // Optional: clear unused bits before writing/pushing
}

int getSubtractFlag() { //get subtract flag
    return (CPUreg.af.F & 0x40) != 0;
}

void setHalfCarryFlag(int value) { //set half carry flag or reset based on value given
if (value == 0) {
    CPUreg.af.F &= 0xDF; // clear half carry flag
} else {
    CPUreg.af.F |= 0x20; // set half carry flag
}
CPUreg.af.F &= 0xF0; // Optional: clear unused bits before writing/pushing
}

int getHalfCarryFlag() { //get half carry flag
    return (CPUreg.af.F & 0x20) != 0;
}

void setCarryFlag(int value) { //set carry flag or reset based on value given
if (value == 0) {
    CPUreg.af.F &= 0xEF; // clear carry flag
} else {
    CPUreg.af.F |= 0x10; // set carry flag
}
CPUreg.af.F &= 0xF0; // Optional: clear unused bits before writing/pushing
}

int getCarryFlag() {
    return (CPUreg.af.F & 0x10) != 0;
}

//Purple instructions
void op_0x00(){ //increment PC by 1 4T 1PC
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x10(){
    //printf("stop called, not implemented yet\n");
    CPUreg.PC += 2; //increment PC by 1
    cyclesAccumulated += 4; //increment cycles by 4
    printf("Stop instruction executed, PC incremented to 0x%04X\n", CPUreg.PC);
  } //stop IMPLEMENT LATER

void op_0x20(){ //jump with relative offset if zero flag not set
    int zeroFlag = CPUreg.af.F & 0x80; // check if zero flag is set
    if (zeroFlag == 0) {
        CPUreg.PC += 2 + ((int8_t)*memoryMap[CPUreg.PC + 1]); //signed integer added to PC 12T
        cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2; //increment PC by 2 if zero flag is set
        cyclesAccumulated += 8;
    }
}

int haltMode = 0; // 0 = running, 1 = halted, 2 = halt bug
void op_0x76() { // HALT
    uint8_t IE = *memoryMap[0xFFFF];
    uint8_t IF = *memoryMap[0xFF0F];
    if (CPUreg.IME == 0 && (IE & IF & 0x1F)) {
        // HALT bug: interrupts disabled, but at least one is pending
        haltMode = 2;
        cyclesAccumulated += 4;
    } else {
        // Normal halt
        haltMode = 1;
        cyclesAccumulated += 4;
    }
    CPUreg.PC += 1;
    //printf("Halt mode: %d, IE: %02X, IF: %02X, IME: %d\n", haltMode, IE, IF, CPUreg.IME);
}


//Purple interrupt instructions

void op_0xF3(){ //disable interrupts 4T 1PC
    CPUreg.IME = 0; //disable interrupts
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

int EIFlag = 0; //enable interrupt flag, set to 1 if 0xFB called after 1 instruction
void op_0xFB(){ //enable interrupts 4T 1PC LOOK INTO CYCLE ACCURATE IMPLEMENTATION LATER
    EIFlag = 1;
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4; //set EI flag to 1, will be processed after next instruction
}

//Orange Jump instructions
void relJumpIf(int condition) {
    if (condition) {
        CPUreg.PC += 2 + (int8_t)*memoryMap[CPUreg.PC + 1];
        cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2;
        cyclesAccumulated += 8;
    }
}

void op_0x30() { // JR NC, r8
    relJumpIf(!getCarryFlag());
}
void op_0x28() { // JR Z, r8
    relJumpIf(getZeroFlag());
}
void op_0x38() { // JR C, r8
    relJumpIf(getCarryFlag());
}
void op_0x18() { // JR r8 (unconditional)
    relJumpIf(1);
}
void op_0xE9() { // JP (HL)
    CPUreg.PC = CPUreg.hl.HL;
    cyclesAccumulated += 4;
}


//Orange call instructions

void conditionalCall(int condition) {
    if (condition) {
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8;
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF;
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1];
        cyclesAccumulated += 24;
    } else {
        CPUreg.PC += 3;
        cyclesAccumulated += 12;
    }
    logStackTop();
}

void op_0xC4() { // CALL NZ, a16
    conditionalCall(!getZeroFlag());
}
void op_0xCC() { // CALL Z, a16
    conditionalCall(getZeroFlag());
}
void op_0xD4() { // CALL NC, a16
    conditionalCall(!getCarryFlag());
}
void op_0xDC() { // CALL C, a16
    conditionalCall(getCarryFlag());
}
void op_0xCD() { // CALL a16 (unconditional)
    conditionalCall(1);
}


//Orange return instructions

void conditionalReturn(int condition) {
    if (condition) {
        CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP];
        CPUreg.SP += 2;
        cyclesAccumulated += 20;
    } else {
        CPUreg.PC += 1;
        cyclesAccumulated += 8;
    }
    logStackTop();
}

void unconditionalReturn() {
    CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
    CPUreg.SP += 2; //increment stack pointer by 2
    cyclesAccumulated += 16;
    logStackTop();
    //printf("Unconditional return to %04X\n", CPUreg.PC);
}

void op_0xC0() { // RET NZ
    conditionalReturn(!getZeroFlag());
}
void op_0xC8() { // RET Z
    conditionalReturn(getZeroFlag());
}
void op_0xD0() { // RET NC
    conditionalReturn(!getCarryFlag());
}
void op_0xD8() { // RET C
    conditionalReturn(getCarryFlag());
}
void op_0xC9(){ //return from subroutine 16T 1PC
    unconditionalReturn();
}

void op_0xD9(){ //return from interrupt 16T 1PC
    unconditionalReturn();
    CPUreg.IME = 1;
    //printf("RETI: Cycles accumulated: %lu\n", realCyclesAccumulated);
    //fprintf(logFile, "[INTERRUPT Finish] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) Cycles=%d\n", *memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC, realCyclesAccumulated);
}

//Orange restart instructions
void restartTo(uint8_t addr) {
    CPUreg.PC += 1;
    *memoryMap[--CPUreg.SP] = (CPUreg.PC) >> 8; //push high byte of PC onto stack
    *memoryMap[--CPUreg.SP] = (CPUreg.PC) & 0xFF; //push low byte of PC onto stack
    CPUreg.PC = addr; //set PC to restart address
    cyclesAccumulated += 16;
}

void op_0xC7(){ //restart 0x00 16T 3PC
    restartTo(0x00);
}
void op_0xCF(){ //restart 0x08 16T 3PC
    restartTo(0x08);
}
void op_0xD7(){ //restart 0x10 16T 3PC
    restartTo(0x10);
}
void op_0xDF(){ //restart 0x18 16T 3PC
    restartTo(0x18);
}
void op_0xE7(){ //restart 0x20 16T 3PC
    restartTo(0x20);
}
void op_0xEF(){ //restart 0x28 16T 3PC
    restartTo(0x28);
}
void op_0xF7(){ //restart 0x30 16T 3PC
    restartTo(0x30);
}
void op_0xFF(){ //restart 0x38 16T 3PC
    restartTo(0x38);
}

//Green LD instructions (16bit int into 16 bit register)
void loadImm16ToReg(uint8_t *high, uint8_t *low) {
    *low = *memoryMap[CPUreg.PC + 1];
    *high = *memoryMap[CPUreg.PC + 2];
    CPUreg.PC += 3;
    cyclesAccumulated += 12;
}

void op_0x01() { // LD BC, imm16
    loadImm16ToReg(&CPUreg.bc.B, &CPUreg.bc.C);
}
void op_0x11() { // LD DE, imm16
    loadImm16ToReg(&CPUreg.de.D, &CPUreg.de.E);
}
void op_0x21() { // LD HL, imm16
    loadImm16ToReg(&CPUreg.hl.H, &CPUreg.hl.L);
}

void op_0x31() { // LD SP, imm16
    // First load the low byte into the lower 8 bits of SP
    LDVal8(*memoryMap[CPUreg.PC + 1], ((uint8_t*)&CPUreg.SP), 0xFFFF, 0, CPUreg.PC + 1);

    // Then load the high byte into the upper 8 bits of SP
    LDVal8(*memoryMap[CPUreg.PC + 2], ((uint8_t*)&CPUreg.SP) + 1, 0xFFFF, 0, CPUreg.PC + 2);

    CPUreg.PC += 3;
    cyclesAccumulated += 12;
}

//Green SP instructions

void pushReg(uint8_t high, uint8_t low) {
    *memoryMap[--CPUreg.SP] = high;
    *memoryMap[--CPUreg.SP] = low;
    CPUreg.PC += 1;
    cyclesAccumulated += 16;
    logStackTop();
}

void op_0xC5() { pushReg(CPUreg.bc.B, CPUreg.bc.C); }
void op_0xD5() { pushReg(CPUreg.de.D, CPUreg.de.E); }
void op_0xE5() { pushReg(CPUreg.hl.H, CPUreg.hl.L); }
void op_0xF5() { pushReg(CPUreg.af.A, CPUreg.af.F & 0xF0); }

void op_0xF8() { // LD HL, SP + r8
    int8_t offset = (int8_t)*memoryMap[CPUreg.PC + 1]; //convert to signed 8 bit then cast signed 16 bit so it sign extends correctly e.g. 1000 0000 become 1111 1111 1000 000 (cast to int8_t first) instead of 0000 0000 1000 0000 (cast straight to int16_t)
    int16_t signedOffset = (int16_t)offset; // sign extend the offset
    uint16_t result = CPUreg.SP + signedOffset;

    setZeroFlag(0);
    setSubtractFlag(0);
    uint16_t sum = CPUreg.SP + offset;
    setCarryFlag(((CPUreg.SP & 0xFF) + (offset & 0xFF)) > 0xFF);
    setHalfCarryFlag(((CPUreg.SP & 0xF) + (offset & 0xF)) > 0xF);

    CPUreg.hl.HL = result;
    CPUreg.PC += 2;
    cyclesAccumulated += 12;
    logStackTop(); //log stack top after operation
}

void op_0xF9(){ //load HL into SP 8T 1PC
    LDVal16(CPUreg.hl.HL, &CPUreg.SP); //set SP to HL value
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
    logStackTop(); //log stack top after operation
}


void popReg(uint8_t *high, uint8_t *low, int isAF) {
    *low = *memoryMap[CPUreg.SP];
    *high = *memoryMap[CPUreg.SP + 1];
    if (isAF) *low &= 0xF0;
    CPUreg.SP += 2;
    CPUreg.PC += 1;
    cyclesAccumulated += 12;
    logStackTop();
}

void op_0xC1() { popReg(&CPUreg.bc.B, &CPUreg.bc.C, 0); }
void op_0xD1() { popReg(&CPUreg.de.D, &CPUreg.de.E, 0); }
void op_0xE1() { popReg(&CPUreg.hl.H, &CPUreg.hl.L, 0); }
void op_0xF1() { popReg(&CPUreg.af.A, &CPUreg.af.F, 1); }

void op_0x08() {
    uint16_t addr = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1];
    LDVal8(CPUreg.SP & 0xFF, memoryMap[addr],addr, 1,0xFFFF);
    LDVal8((CPUreg.SP >> 8) & 0xFF, memoryMap[addr + 1],addr, 1,0xFFFF);
    CPUreg.PC += 3;
    cyclesAccumulated += 20;
    logStackTop();
}

//Blue LD instructions (8 bit register into memory address in 16 bit register) without register increment
void storeToAddr(uint16_t addr, uint8_t value) {
    LDVal8(value, memoryMap[addr],addr, 1,0xFFFF);
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x70() { storeToAddr(CPUreg.hl.HL, CPUreg.bc.B); }
void op_0x71() { storeToAddr(CPUreg.hl.HL, CPUreg.bc.C); }
void op_0x72() { storeToAddr(CPUreg.hl.HL, CPUreg.de.D); }
void op_0x73() { storeToAddr(CPUreg.hl.HL, CPUreg.de.E); }
void op_0x74() { storeToAddr(CPUreg.hl.HL, CPUreg.hl.H); }
void op_0x75() { storeToAddr(CPUreg.hl.HL, CPUreg.hl.L); }
void op_0x77() { storeToAddr(CPUreg.hl.HL, CPUreg.af.A); }

void op_0x02() { storeToAddr(CPUreg.bc.BC, CPUreg.af.A); }
void op_0x12() { storeToAddr(CPUreg.de.DE, CPUreg.af.A); }


//Blue LD instructions (memory address in 16 bit register into 8 bit register) without register increment
void loadFromAddr(uint16_t addr, uint8_t *dest) {
    LDVal8(*memoryMap[addr], dest,0xFFFF, 0,addr);
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x46() { loadFromAddr(CPUreg.hl.HL, &CPUreg.bc.B); }
void op_0x4E() { loadFromAddr(CPUreg.hl.HL, &CPUreg.bc.C); }
void op_0x56() { loadFromAddr(CPUreg.hl.HL, &CPUreg.de.D); }
void op_0x5E() { loadFromAddr(CPUreg.hl.HL, &CPUreg.de.E); }
void op_0x66() { loadFromAddr(CPUreg.hl.HL, &CPUreg.hl.H); }
void op_0x6E() { loadFromAddr(CPUreg.hl.HL, &CPUreg.hl.L); }
void op_0x7E() { loadFromAddr(CPUreg.hl.HL, &CPUreg.af.A); }

void op_0x0A() { loadFromAddr(CPUreg.bc.BC, &CPUreg.af.A); }
void op_0x1A() { loadFromAddr(CPUreg.de.DE, &CPUreg.af.A); }

//Blue LD instructions (8 bit register into HL register with increment or decrement)

void storeAtoHLAndStep(int step) {
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.hl.HL],CPUreg.hl.HL,1,0xFFFF);
    CPUreg.hl.HL += step;
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x22() { storeAtoHLAndStep(1); }
void op_0x32() { storeAtoHLAndStep(-1); }

//Blue LD instructions (8 bit value into 8/16 bit register)
void loadImmToReg(uint8_t *reg) {
    LDVal8(*memoryMap[CPUreg.PC + 1], reg,CPUreg.PC + 1,0,0xFFFF);
    CPUreg.PC += 2;
    cyclesAccumulated += 8;
}

void op_0x06() { loadImmToReg(&CPUreg.bc.B); }
void op_0x0E() { loadImmToReg(&CPUreg.bc.C); }
void op_0x16() { loadImmToReg(&CPUreg.de.D); }
void op_0x1E() { loadImmToReg(&CPUreg.de.E); }
void op_0x26() { loadImmToReg(&CPUreg.hl.H); }
void op_0x2E() { loadImmToReg(&CPUreg.hl.L); }
void op_0x3E() { loadImmToReg(&CPUreg.af.A); }

void op_0x36(){
    LDVal8(*memoryMap[CPUreg.PC + 1], memoryMap[CPUreg.hl.HL],CPUreg.hl.HL,1,CPUreg.PC + 1);
    CPUreg.PC += 2;
    cyclesAccumulated += 12;
} 

//Blue LD instructions (HL register address increment or decrement into 8 bit register)
void loadAFromHLAndStep(int step) {
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.af.A,0xFFFF,1,CPUreg.hl.HL);
    CPUreg.hl.HL += step;
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x2A() { loadAFromHLAndStep(1); }
void op_0x3A() { loadAFromHLAndStep(-1); }

//Blue LD instructions (8 bit register into 8 bit register)

void loadRegToReg(uint8_t src, uint8_t *dest) {
    LDVal8(src, dest,0xFFFF,0,0xFFFF);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x40() { loadRegToReg(CPUreg.bc.B, &CPUreg.bc.B); }
void op_0x41() { loadRegToReg(CPUreg.bc.C, &CPUreg.bc.B); }
void op_0x42() { loadRegToReg(CPUreg.de.D, &CPUreg.bc.B); }
void op_0x43() { loadRegToReg(CPUreg.de.E, &CPUreg.bc.B); }
void op_0x44() { loadRegToReg(CPUreg.hl.H, &CPUreg.bc.B); }
void op_0x45() { loadRegToReg(CPUreg.hl.L, &CPUreg.bc.B); }
void op_0x47() { loadRegToReg(CPUreg.af.A, &CPUreg.bc.B); }

void op_0x48() { loadRegToReg(CPUreg.bc.B, &CPUreg.bc.C); }
void op_0x49() { loadRegToReg(CPUreg.bc.C, &CPUreg.bc.C); }
void op_0x4A() { loadRegToReg(CPUreg.de.D, &CPUreg.bc.C); }
void op_0x4B() { loadRegToReg(CPUreg.de.E, &CPUreg.bc.C); }
void op_0x4C() { loadRegToReg(CPUreg.hl.H, &CPUreg.bc.C); }
void op_0x4D() { loadRegToReg(CPUreg.hl.L, &CPUreg.bc.C); }
void op_0x4F() { loadRegToReg(CPUreg.af.A, &CPUreg.bc.C); }

void op_0x50() { loadRegToReg(CPUreg.bc.B, &CPUreg.de.D); }
void op_0x51() { loadRegToReg(CPUreg.bc.C, &CPUreg.de.D); }
void op_0x52() { loadRegToReg(CPUreg.de.D, &CPUreg.de.D); }
void op_0x53() { loadRegToReg(CPUreg.de.E, &CPUreg.de.D); }
void op_0x54() { loadRegToReg(CPUreg.hl.H, &CPUreg.de.D); }
void op_0x55() { loadRegToReg(CPUreg.hl.L, &CPUreg.de.D); }
void op_0x57() { loadRegToReg(CPUreg.af.A, &CPUreg.de.D); }

void op_0x58() { loadRegToReg(CPUreg.bc.B, &CPUreg.de.E); }
void op_0x59() { loadRegToReg(CPUreg.bc.C, &CPUreg.de.E); }
void op_0x5A() { loadRegToReg(CPUreg.de.D, &CPUreg.de.E); }
void op_0x5B() { loadRegToReg(CPUreg.de.E, &CPUreg.de.E); }
void op_0x5C() { loadRegToReg(CPUreg.hl.H, &CPUreg.de.E); }
void op_0x5D() { loadRegToReg(CPUreg.hl.L, &CPUreg.de.E); }
void op_0x5F() { loadRegToReg(CPUreg.af.A, &CPUreg.de.E); }

void op_0x60() { loadRegToReg(CPUreg.bc.B, &CPUreg.hl.H); }
void op_0x61() { loadRegToReg(CPUreg.bc.C, &CPUreg.hl.H); }
void op_0x62() { loadRegToReg(CPUreg.de.D, &CPUreg.hl.H); }
void op_0x63() { loadRegToReg(CPUreg.de.E, &CPUreg.hl.H); }
void op_0x64() { loadRegToReg(CPUreg.hl.H, &CPUreg.hl.H); }
void op_0x65() { loadRegToReg(CPUreg.hl.L, &CPUreg.hl.H); }
void op_0x67() { loadRegToReg(CPUreg.af.A, &CPUreg.hl.H); }

void op_0x68() { loadRegToReg(CPUreg.bc.B, &CPUreg.hl.L); }
void op_0x69() { loadRegToReg(CPUreg.bc.C, &CPUreg.hl.L); }
void op_0x6A() { loadRegToReg(CPUreg.de.D, &CPUreg.hl.L); }
void op_0x6B() { loadRegToReg(CPUreg.de.E, &CPUreg.hl.L); }
void op_0x6C() { loadRegToReg(CPUreg.hl.H, &CPUreg.hl.L); }
void op_0x6D() { loadRegToReg(CPUreg.hl.L, &CPUreg.hl.L); }
void op_0x6F() { loadRegToReg(CPUreg.af.A, &CPUreg.hl.L); }

void op_0x78() { loadRegToReg(CPUreg.bc.B, &CPUreg.af.A); }
void op_0x79() { loadRegToReg(CPUreg.bc.C, &CPUreg.af.A); }
void op_0x7A() { loadRegToReg(CPUreg.de.D, &CPUreg.af.A); }
void op_0x7B() { loadRegToReg(CPUreg.de.E, &CPUreg.af.A); }
void op_0x7C() { loadRegToReg(CPUreg.hl.H, &CPUreg.af.A); }
void op_0x7D() { loadRegToReg(CPUreg.hl.L, &CPUreg.af.A); }
void op_0x7F() { loadRegToReg(CPUreg.af.A, &CPUreg.af.A); }

//Blue load A C and a8 A - 0xFF00-0xFFFF instructions

void op_0xE0(){ //load A into address 0xFF00 + immediate 12T 2PC
    uint8_t immediate = *memoryMap[CPUreg.PC + 1]; //get immediate value from memory
    LDVal8(CPUreg.af.A, memoryMap[0xFF00 + immediate],0xFF00+immediate,1,0xFFFF); //load value from A into address 0xFF00 + immediate
    CPUreg.PC += 2; 
    cyclesAccumulated += 12;
}


void op_0xF0() { // LD A, (FF00 + n)
    uint8_t immediate = *memoryMap[CPUreg.PC + 1];
    uint16_t addr = 0xFF00 + immediate;

    LDVal8(*memoryMap[addr], &CPUreg.af.A,0xFFFF,1,addr);

    CPUreg.PC += 2;
    cyclesAccumulated += 12;
}

void op_0xE2() { storeToAddr(0xFF00 + CPUreg.bc.C, CPUreg.af.A); }
void op_0xF2() { loadFromAddr(0xFF00 + CPUreg.bc.C, &CPUreg.af.A); }

//Blue LD instructions A and 16 bit immediate address

void op_0xEA(){ // load A into address a16 16T 3PC
    uint16_t address = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1];
    LDVal8(CPUreg.af.A, memoryMap[address],address,1,0xFFFF);
    CPUreg.PC += 3; 
    cyclesAccumulated += 16;
}

void op_0xFA(){ // load value from address a16 into A 16T 3PC
    uint16_t address = (*memoryMap[CPUreg.PC + 2] << 8 | *memoryMap[CPUreg.PC + 1]); //combine low and high byte to get address
    LDVal8(*memoryMap[address], &CPUreg.af.A,0xFFFF,1,address);
    CPUreg.PC += 3; 
    cyclesAccumulated += 16;
}

//Red ADD instructions (add 16 bit register to HL) with flags
void add16ToHL(uint16_t value) {
    uint16_t result = CPUreg.hl.HL + value;
    setSubtractFlag(0);
    setCarryFlag(result < CPUreg.hl.HL);
    setHalfCarryFlag((CPUreg.hl.HL & 0xFFF) + (value & 0xFFF) > 0xFFF);
    CPUreg.hl.HL = result;
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x09() { add16ToHL(CPUreg.bc.BC); }
void op_0x19() { add16ToHL(CPUreg.de.DE); }
void op_0x29() { add16ToHL(CPUreg.hl.HL); }
void op_0x39() { add16ToHL(CPUreg.SP); }

void op_0xE8() {
    int8_t offset = (int8_t)*memoryMap[CPUreg.PC + 1];
    int16_t signedOffset = (int16_t)offset;
    uint16_t result = CPUreg.SP + signedOffset;

    setZeroFlag(0);
    setSubtractFlag(0);
    setHalfCarryFlag(((CPUreg.SP & 0xF) + (offset & 0xF)) > 0xF);
    setCarryFlag(((CPUreg.SP & 0xFF) + (offset & 0xFF)) > 0xFF);

    CPUreg.SP = result;
    CPUreg.PC += 2;
    cyclesAccumulated += 16;
}

//Red Increment instructions (increment 16 bit register by 1) with no flag
void inc16(uint16_t* reg) {
    (*reg)++;
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x03() { inc16(&CPUreg.bc.BC); }
void op_0x13() { inc16(&CPUreg.de.DE); }
void op_0x23() { inc16(&CPUreg.hl.HL); }
void op_0x33() { inc16(&CPUreg.SP); }

//Red Decrement instructions (decrement 16 bit register by 1) with no flag
void dec16(uint16_t* reg) {
    (*reg)--;
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x0B() { dec16(&CPUreg.bc.BC); }
void op_0x1B() { dec16(&CPUreg.de.DE); }
void op_0x2B() { dec16(&CPUreg.hl.HL); }
void op_0x3B() { dec16(&CPUreg.SP); }

//Yellow Increment instructions (increment 8 bit register by 1) with flags

void inc8(uint8_t* reg) {
    setHalfCarryFlag((*reg & 0x0F) == 0x0F);
    (*reg)++;
    setZeroFlag(*reg == 0);
    setSubtractFlag(0);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x04() { inc8(&CPUreg.bc.B); }
void op_0x14() { inc8(&CPUreg.de.D); }
void op_0x24() { inc8(&CPUreg.hl.H); }
void op_0x0C() { inc8(&CPUreg.bc.C); }
void op_0x1C() { inc8(&CPUreg.de.E); }
void op_0x2C() { inc8(&CPUreg.hl.L); }
void op_0x3C() { inc8(&CPUreg.af.A); }

void op_0x34() {
    uint8_t value = *memoryMap[CPUreg.hl.HL];
    setHalfCarryFlag((value & 0x0F) == 0x0F);
    value++;
    *memoryMap[CPUreg.hl.HL] = value;
    setZeroFlag(value == 0);
    setSubtractFlag(0);
    CPUreg.PC += 1;
    cyclesAccumulated += 12;
}

void dec8(uint8_t* reg) {
    setHalfCarryFlag((*reg & 0x0F) == 0x00);
    (*reg)--;
    setZeroFlag(*reg == 0);
    setSubtractFlag(1);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x05() { dec8(&CPUreg.bc.B); }
void op_0x15() { dec8(&CPUreg.de.D); }
void op_0x25() { dec8(&CPUreg.hl.H); }
void op_0x0D() { dec8(&CPUreg.bc.C); }
void op_0x1D() { dec8(&CPUreg.de.E); }
void op_0x2D() { dec8(&CPUreg.hl.L); }
void op_0x3D() { dec8(&CPUreg.af.A); }

void op_0x35() {
    uint8_t value = *memoryMap[CPUreg.hl.HL];
    setHalfCarryFlag((value & 0x0F) == 0x00);
    value--;
    *memoryMap[CPUreg.hl.HL] = value;
    setZeroFlag(value == 0);
    setSubtractFlag(1);
    CPUreg.PC += 1;
    cyclesAccumulated += 12;
}

//Yellow Add instructions (add 8 bit register to 8 bit register) with flags

void addToA(uint8_t value) { //function to add a value to register A with flags
    uint16_t result = CPUreg.af.A + value;
    setHalfCarryFlag(((CPUreg.af.A & 0x0F) + (value & 0x0F)) > 0x0F); // set half carry flag if low nibble addition results in carry
    setCarryFlag(result > 0xFF); // set carry flag if result is greater than 255
    CPUreg.af.A = result & 0xFF; // store result (lower 8 bits) in register A
    setZeroFlag(CPUreg.af.A == 0); // set zero flag if result is zero
    setSubtractFlag(0); // clear subtract flag
}

void adcToA(uint8_t value) {
    uint8_t a = CPUreg.af.A;
    uint8_t carry = getCarryFlag();
    uint16_t result = a + value + carry;

    setZeroFlag((result & 0xFF) == 0);
    setSubtractFlag(0);
    setHalfCarryFlag(((a & 0xF) + (value & 0xF) + carry) > 0xF);
    setCarryFlag(result > 0xFF);

    CPUreg.af.A = result & 0xFF;
}

void op_0x80() { addToA(CPUreg.bc.B); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x81() { addToA(CPUreg.bc.C); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x82() { addToA(CPUreg.de.D); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x83() { addToA(CPUreg.de.E); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x84() { addToA(CPUreg.hl.H); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x85() { addToA(CPUreg.hl.L); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x86() { addToA(*memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; cyclesAccumulated += 8; }
void op_0x87() { addToA(CPUreg.af.A); CPUreg.PC += 1; cyclesAccumulated += 4; }

void op_0x88() { adcToA(CPUreg.bc.B); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x89() { adcToA(CPUreg.bc.C); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x8A() { adcToA(CPUreg.de.D); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x8B() { adcToA(CPUreg.de.E); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x8C() { adcToA(CPUreg.hl.H); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x8D() { adcToA(CPUreg.hl.L); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x8E() { adcToA(*memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; cyclesAccumulated += 8; }
void op_0x8F() { adcToA(CPUreg.af.A); CPUreg.PC += 1; cyclesAccumulated += 4; }

void op_0xC6() { addToA(*memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; cyclesAccumulated += 8; }
void op_0xCE() { adcToA(*memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; cyclesAccumulated += 8; }

//Yellow Subtract instructions (subtract 8 bit register from 8 bit register) with flags
void subFromA(uint8_t value) {
    uint16_t result = CPUreg.af.A - value;

    setHalfCarryFlag((CPUreg.af.A & 0x0F) < (value & 0x0F));
    setCarryFlag(CPUreg.af.A < value);
    CPUreg.af.A = result & 0xFF;

    setZeroFlag((CPUreg.af.A & 0xFF) == 0);
    setSubtractFlag(1);
}

void sbcFromA(uint8_t value) {
    uint8_t a = CPUreg.af.A;
    uint8_t carry = getCarryFlag();

    uint16_t result = a - value - carry;

    setZeroFlag((result & 0xFF) == 0);
    setSubtractFlag(1);
    setHalfCarryFlag((a & 0x0F) < ((value & 0x0F) + carry));
    setCarryFlag(a < (value + carry));

    CPUreg.af.A = result & 0xFF;
}

void op_0x90() { subFromA(CPUreg.bc.B); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x91() { subFromA(CPUreg.bc.C); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x92() { subFromA(CPUreg.de.D); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x93() { subFromA(CPUreg.de.E); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x94() { subFromA(CPUreg.hl.H); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x95() { subFromA(CPUreg.hl.L); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x96() { subFromA(*memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; cyclesAccumulated += 8; }
void op_0x97() { subFromA(CPUreg.af.A); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x98() { sbcFromA(CPUreg.bc.B); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x99() { sbcFromA(CPUreg.bc.C); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x9A() { sbcFromA(CPUreg.de.D); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x9B() { sbcFromA(CPUreg.de.E); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x9C() { sbcFromA(CPUreg.hl.H); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x9D() { sbcFromA(CPUreg.hl.L); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0x9E() { sbcFromA(*memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; cyclesAccumulated += 8; }
void op_0x9F() { sbcFromA(CPUreg.af.A); CPUreg.PC += 1; cyclesAccumulated += 4; }
void op_0xD6() { subFromA(*memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; cyclesAccumulated += 8; }
void op_0xDE() { sbcFromA(*memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; cyclesAccumulated += 8; }

//Yellow AND instructions (bitwise AND operation with 8 bit register) with flags
void andWithA(uint8_t value) { // bitwise AND operation with register A
    CPUreg.af.A &= value; // perform bitwise AND operation
    setZeroFlag(CPUreg.af.A == 0); // set zero flag if result is zero
    setSubtractFlag(0); // clear subtract flag
    setHalfCarryFlag(1); // set half carry flag
    setCarryFlag(0); // clear carry flag
    //fprintf(logFile,"[DEBUG] After AND: A=%02X, Z=%d\n", CPUreg.af.A, (CPUreg.af.F & 0x80) != 0);
}

void op_0xA0(){ //AND B with A 4T 1PC
    andWithA(CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA1(){ //AND C with A 4T 1PC
    andWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA2(){ //AND D with A 4T 1PC
    andWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA3(){ //AND E with A 4T 1PC
    andWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA4(){ //AND H with A 4T 1PC
    andWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA5(){ //AND L with A 4T 1PC
    andWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA6(){ //AND value at address HL with A 8T 1PC
    andWithA(*memoryMap[CPUreg.hl.HL]); // perform AND operation with value at address HL
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0xA7(){ //AND A with A 4T 1PC
    andWithA(CPUreg.af.A); // perform AND operation with itself
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xE6(){ //AND immediate value with A 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1]; // get immediate value
    andWithA(value); // perform AND operation with immediate value
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}


//Yellow XOR instructions (bitwise XOR operation with 8 bit register) with flags
void xorWithA(uint8_t value) { // bitwise XOR operation with register A
    CPUreg.af.A ^= value; // perform bitwise XOR operation
    setZeroFlag(CPUreg.af.A == 0); // set zero flag if result is zero
    setSubtractFlag(0); // clear subtract flag
    setHalfCarryFlag(0); // clear half carry flag
    setCarryFlag(0); // clear carry flag
}

void op_0xA8(){ //XOR B with A 4T 1PC
    xorWithA(CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xA9(){ //XOR C with A 4T 1PC
    xorWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xAA(){ //XOR D with A 4T 1PC
    xorWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xAB(){ //XOR E with A 4T 1PC
    xorWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xAC(){ //XOR H with A 4T 1PC
    xorWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xAD(){ //XOR L with A 4T 1PC
    xorWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xAE(){ //XOR value at address HL with A 8T 1PC
    xorWithA(*memoryMap[CPUreg.hl.HL]); // perform XOR operation with value at address HL
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0xAF(){ //XOR A with A 4T 1PC
    xorWithA(CPUreg.af.A); // perform XOR operation with itself
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xEE(){ //XOR immediate value with A 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1]; // get immediate value
    xorWithA(value); // perform XOR operation with immediate value
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

//Yellow OR instructions (bitwise OR operation with 8 bit register) with flags
void orWithA(uint8_t value) { // bitwise OR operation with register A
    CPUreg.af.A |= value; // perform bitwise OR operation
    setZeroFlag(CPUreg.af.A == 0); // set zero flag if result is zero
    setSubtractFlag(0); // clear subtract flag
    setHalfCarryFlag(0); // clear half carry flag
    setCarryFlag(0); // clear carry flag
}

void op_0xB0(){ //OR B with A 4T 1PC
    orWithA(CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB1(){ //OR C with A 4T 1PC
    orWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB2(){ //OR D with A 4T 1PC
    orWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB3(){ //OR E with A 4T 1PC
    orWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB4(){ //OR H with A 4T 1PC
    orWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB5(){ //OR L with A 4T 1PC
    orWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB6(){ //OR value at address HL with A 8T 1PC
    orWithA(*memoryMap[CPUreg.hl.HL]); // perform OR operation with value at address HL
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0xB7(){ //OR A with A 4T 1PC
    orWithA(CPUreg.af.A); // perform OR operation with itself
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xF6(){ //OR immediate value with A 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1]; // get immediate value
    orWithA(value); // perform OR operation with immediate value
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

//Yellow Compare instructions (compare 8 bit register with A) with flags
void compareWithA(uint8_t value) { // compare value with register A
    uint16_t result = CPUreg.af.A - value; // perform subtraction
    setHalfCarryFlag((CPUreg.af.A & 0x0F) < (value & 0x0F)); // half-borrow occurred 
    setCarryFlag(CPUreg.af.A < value); // full borrow occurred
    setZeroFlag(CPUreg.af.A == value); // zero flag if equal
    setSubtractFlag(1); // subtract flag set
}

void op_0xB8(){ //compare B with A 4T 1PC
    compareWithA(CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xB9(){ //compare C with A 4T 1PC
    compareWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xBA(){ //compare D with A 4T 1PC
    compareWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xBB(){ //compare E with A 4T 1PC
    compareWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xBC(){ //compare H with A 4T 1PC
    compareWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xBD(){ //compare L with A 4T 1PC
    compareWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xBE(){ //compare value at address HL with A 8T 1PC
    compareWithA(*memoryMap[CPUreg.hl.HL]); // perform comparison with value at address HL
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0xBF(){ //compare A with A 4T 1PC
    compareWithA(CPUreg.af.A); // perform comparison with itself
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xFE(){ //compare immediate value with A 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1]; // get immediate value
    compareWithA(value); // perform comparison with immediate value
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

//Purple instruction DAA, SCF, CCF, CPL

void op_0x37(){ //SCF 4T 1PC
    setCarryFlag(1); // set carry flag
    setHalfCarryFlag(0); // clear half carry flag
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x3F(){ //CCF 4T 1PC
    setCarryFlag(!getCarryFlag()); // flip carry flag
    setHalfCarryFlag(0); // clear half carry flag
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x2F(){ //CPL 4T 1PC
    CPUreg.af.A = ~CPUreg.af.A; // complement A (flip all bits 1s complement)
    setHalfCarryFlag(1); // set half carry flag
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x27() { // DAA 4T 1PC, turn A into Binary Coded Decimal (BCD) e.g. 
    uint8_t a = CPUreg.af.A; //value to store back in register A
    int adjust = 0; //adjusted value to add to a

    if (!getSubtractFlag()) { // after addition
        if (getHalfCarryFlag() || (a & 0x0F) > 9)
            adjust |= 0x06;
        if (getCarryFlag() || a > 0x99) {
            adjust |= 0x60;
            setCarryFlag(1); //carry flag if BCD will be greater than 99 e.g. 0x99 + 0x05 = 0x04 with carry flag set to 1 which is 104 in decimal
        }
        else {
        setCarryFlag(0); // clear carry flag if no carry
        }
        a += adjust;
    } 
    else { // after subtraction
        if (getHalfCarryFlag())
            adjust |= 0x06;
        if (getCarryFlag())
            adjust |= 0x60;
        a -= adjust; //carry flag not changed for subtraction, since underflow carry flag is set by subtraction instruction itself anyway
    } // e.g. 0x01 - 0x02 , subtract flag set to 1 and carry flag set to 1 during subtraction, result is 0x99, interpreted as -1 since underflow from 0x00 to 0x99, can be known since carry and subtract flags are 1

    CPUreg.af.A = a;
    setZeroFlag(CPUreg.af.A == 0);
    setHalfCarryFlag(0); // always cleared
    // carry flag set above if needed, unchanged otherwise for subtraction
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

//Orange jump instructions (jump to address) with flags

void op_0xC2(){ //jump to 16 bit address if zero flag is 0 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getZeroFlag() == 0) { // if zero flag is not set
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; // jump to immediate address
        cyclesAccumulated += 16;
    } else { // if zero flag is set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if zero flag is set
        cyclesAccumulated += 12;
    }
}

void op_0xD2(){ //jump to 16 bit address if carry flag is 0 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getCarryFlag() == 0) { // if carry flag is not set
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; // jump to immediate address
        cyclesAccumulated += 16;
    } else { // if carry flag is set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if carry flag is set
        cyclesAccumulated += 12;
    }
}

void op_0xCA(){ //jump to 16 bit address if zero flag is 1 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getZeroFlag() == 1) { // if zero flag is set
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; // jump to immediate address
        cyclesAccumulated += 16;
    } else { // if zero flag is not set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if zero flag is not set
        cyclesAccumulated += 12;
    }
}

void op_0xDA(){ //jump to 16 bit address if carry flag is 1 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getCarryFlag() == 1) { // if carry flag is set
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; // jump to immediate address
        cyclesAccumulated += 16;
    } else { // if carry flag is not set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if carry flag is not set
        cyclesAccumulated += 12;
    }
}

void op_0xC3(){ //jump to 16 bit address unconditionally 16T Changes PC to immediate address
    CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; // jump to immediate address
    cyclesAccumulated += 16;
    //printf("Jumping to address %04X\n", CPUreg.PC);
}

//Blue rotate non CB

void op_0x07(){ //Rotate left 1 bit store MSB before rotate in carry flag
    setZeroFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);
    uint8_t MSB = (CPUreg.af.A >> 7) & 0x01;
    setCarryFlag(MSB);
    CPUreg.af.A = (CPUreg.af.A << 1) | MSB; //Rotate (shift and add old MSB to LSB)
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x17(){ //Rotate left setting old MSB to carry flag and carry flag to new LSB
    setZeroFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);
    uint8_t oldCarry = getCarryFlag(); //returns 0 or 1
    uint8_t MSB = (CPUreg.af.A >> 7) & 0x01;
    setCarryFlag(MSB);
    CPUreg.af.A = (CPUreg.af.A << 1) | oldCarry; //Rotate (shift and add old carry to LSB)
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x0F(){ //Rotate right 1 bit store LSB before rotate in carr flag and old LSB is new MSB
    setZeroFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);
    uint8_t LSB = CPUreg.af.A & 0x01;
    setCarryFlag(LSB);
    CPUreg.af.A = (LSB << 7) | (CPUreg.af.A >> 1);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x1F(){ //Rotate right 1 bit store old LSB in carry flag and old carry flag is new MSB
    setZeroFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);
    uint8_t oldCarry = getCarryFlag();
    uint8_t LSB = CPUreg.af.A & 0x01;
    setCarryFlag(LSB);
    CPUreg.af.A = (oldCarry << 7) | (CPUreg.af.A >> 1);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

//Start of CB instructions
/**/

void rotateLeft(uint8_t *dest){ 

}

//Rotate left through carry, store MSB in carry flag and rotate LSB to MSB

void rlc(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB00() { rlc(&CPUreg.bc.B, false); }
void op_0xCB01() { rlc(&CPUreg.bc.C, false); }
void op_0xCB02() { rlc(&CPUreg.de.D, false); }
void op_0xCB03() { rlc(&CPUreg.de.E, false); }
void op_0xCB04() { rlc(&CPUreg.hl.H, false); }
void op_0xCB05() { rlc(&CPUreg.hl.L, false); }
void op_0xCB06() { rlc(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB07() { rlc(&CPUreg.af.A, false); }


 //Rotate right through carry, store LSB in carry flag and rotate MSB to LSB

void rrc(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value = (value >> 1) | (lsb << 7);
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB08() { rrc(&CPUreg.bc.B, false); }
void op_0xCB09() { rrc(&CPUreg.bc.C, false); }
void op_0xCB0A() { rrc(&CPUreg.de.D, false); }
void op_0xCB0B() { rrc(&CPUreg.de.E, false); }
void op_0xCB0C() { rrc(&CPUreg.hl.H, false); }
void op_0xCB0D() { rrc(&CPUreg.hl.L, false); }
void op_0xCB0E() { rrc(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB0F() { rrc(&CPUreg.af.A, false); }

// RL 

void rl(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | getCarryFlag();
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB10() { rl(&CPUreg.bc.B, false); }
void op_0xCB11() { rl(&CPUreg.bc.C, false); }
void op_0xCB12() { rl(&CPUreg.de.D, false); }
void op_0xCB13() { rl(&CPUreg.de.E, false); }
void op_0xCB14() { rl(&CPUreg.hl.H, false); }
void op_0xCB15() { rl(&CPUreg.hl.L, false); }
void op_0xCB16() { rl(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB17() { rl(&CPUreg.af.A, false); }
// RR

void rr(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value = (value >> 1) | (getCarryFlag() << 7);
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB18() { rr(&CPUreg.bc.B, false); }
void op_0xCB19() { rr(&CPUreg.bc.C, false); }
void op_0xCB1A() { rr(&CPUreg.de.D, false); }
void op_0xCB1B() { rr(&CPUreg.de.E, false); }
void op_0xCB1C() { rr(&CPUreg.hl.H, false); }
void op_0xCB1D() { rr(&CPUreg.hl.L, false); }
void op_0xCB1E() { rr(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB1F() { rr(&CPUreg.af.A, false); }

//SLA 

void sla(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t msb = (value >> 7) & 0x01;
    value <<= 1;
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB20() { sla(&CPUreg.bc.B, false); }
void op_0xCB21() { sla(&CPUreg.bc.C, false); }
void op_0xCB22() { sla(&CPUreg.de.D, false); }
void op_0xCB23() { sla(&CPUreg.de.E, false); }
void op_0xCB24() { sla(&CPUreg.hl.H, false); }
void op_0xCB25() { sla(&CPUreg.hl.L, false); }
void op_0xCB26() { sla(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB27() { sla(&CPUreg.af.A, false); }


//SRA

void sra(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value = (value >> 1) | (value & 0x80);
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB28() { sra(&CPUreg.bc.B, false); }
void op_0xCB29() { sra(&CPUreg.bc.C, false); }
void op_0xCB2A() { sra(&CPUreg.de.D, false); }
void op_0xCB2B() { sra(&CPUreg.de.E, false); }
void op_0xCB2C() { sra(&CPUreg.hl.H, false); }
void op_0xCB2D() { sra(&CPUreg.hl.L, false); }
void op_0xCB2E() { sra(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB2F() { sra(&CPUreg.af.A, false); }

//SWAP

void swap(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4);
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB30() { swap(&CPUreg.bc.B, false); }
void op_0xCB31() { swap(&CPUreg.bc.C, false); }
void op_0xCB32() { swap(&CPUreg.de.D, false); }
void op_0xCB33() { swap(&CPUreg.de.E, false); }
void op_0xCB34() { swap(&CPUreg.hl.H, false); }
void op_0xCB35() { swap(&CPUreg.hl.L, false); }
void op_0xCB36() { swap(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB37() { swap(&CPUreg.af.A, false); }

//SRL

void srl(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value >>= 1;
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB38() { srl(&CPUreg.bc.B, false); }
void op_0xCB39() { srl(&CPUreg.bc.C, false); }
void op_0xCB3A() { srl(&CPUreg.de.D, false); }
void op_0xCB3B() { srl(&CPUreg.de.E, false); }
void op_0xCB3C() { srl(&CPUreg.hl.H, false); }
void op_0xCB3D() { srl(&CPUreg.hl.L, false); }
void op_0xCB3E() { srl(memoryMap[CPUreg.hl.HL], true); }
void op_0xCB3F() { srl(&CPUreg.af.A, false); }

//BIT

void bitTest(uint8_t bit, uint8_t value, bool isMemory) {
    setZeroFlag(((value >> bit) & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 8 : 4;
}

// BIT 0
void op_0xCB40() { bitTest(0, CPUreg.bc.B, false); }
void op_0xCB41() { bitTest(0, CPUreg.bc.C, false); }
void op_0xCB42() { bitTest(0, CPUreg.de.D, false); }
void op_0xCB43() { bitTest(0, CPUreg.de.E, false); }
void op_0xCB44() { bitTest(0, CPUreg.hl.H, false); }
void op_0xCB45() { bitTest(0, CPUreg.hl.L, false); }
void op_0xCB46() { bitTest(0, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB47() { bitTest(0, CPUreg.af.A, false); }

// BIT 1
void op_0xCB48() { bitTest(1, CPUreg.bc.B, false); }
void op_0xCB49() { bitTest(1, CPUreg.bc.C, false); }
void op_0xCB4A() { bitTest(1, CPUreg.de.D, false); }
void op_0xCB4B() { bitTest(1, CPUreg.de.E, false); }
void op_0xCB4C() { bitTest(1, CPUreg.hl.H, false); }
void op_0xCB4D() { bitTest(1, CPUreg.hl.L, false); }
void op_0xCB4E() { bitTest(1, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB4F() { bitTest(1, CPUreg.af.A, false); }

// BIT 2
void op_0xCB50() { bitTest(2, CPUreg.bc.B, false); }
void op_0xCB51() { bitTest(2, CPUreg.bc.C, false); }
void op_0xCB52() { bitTest(2, CPUreg.de.D, false); }
void op_0xCB53() { bitTest(2, CPUreg.de.E, false); }
void op_0xCB54() { bitTest(2, CPUreg.hl.H, false); }
void op_0xCB55() { bitTest(2, CPUreg.hl.L, false); }
void op_0xCB56() { bitTest(2, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB57() { bitTest(2, CPUreg.af.A, false); }

// BIT 3
void op_0xCB58() { bitTest(3, CPUreg.bc.B, false); }
void op_0xCB59() { bitTest(3, CPUreg.bc.C, false); }
void op_0xCB5A() { bitTest(3, CPUreg.de.D, false); }
void op_0xCB5B() { bitTest(3, CPUreg.de.E, false); }
void op_0xCB5C() { bitTest(3, CPUreg.hl.H, false); }
void op_0xCB5D() { bitTest(3, CPUreg.hl.L, false); }
void op_0xCB5E() { bitTest(3, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB5F() { bitTest(3, CPUreg.af.A, false); }

// BIT 4
void op_0xCB60() { bitTest(4, CPUreg.bc.B, false); }
void op_0xCB61() { bitTest(4, CPUreg.bc.C, false); }
void op_0xCB62() { bitTest(4, CPUreg.de.D, false); }
void op_0xCB63() { bitTest(4, CPUreg.de.E, false); }
void op_0xCB64() { bitTest(4, CPUreg.hl.H, false); }
void op_0xCB65() { bitTest(4, CPUreg.hl.L, false); }
void op_0xCB66() { bitTest(4, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB67() { bitTest(4, CPUreg.af.A, false); }

// BIT 5
void op_0xCB68() { bitTest(5, CPUreg.bc.B, false); }
void op_0xCB69() { bitTest(5, CPUreg.bc.C, false); }
void op_0xCB6A() { bitTest(5, CPUreg.de.D, false); }
void op_0xCB6B() { bitTest(5, CPUreg.de.E, false); }
void op_0xCB6C() { bitTest(5, CPUreg.hl.H, false); }
void op_0xCB6D() { bitTest(5, CPUreg.hl.L, false); }
void op_0xCB6E() { bitTest(5, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB6F() { bitTest(5, CPUreg.af.A, false); }

// BIT 6
void op_0xCB70() { bitTest(6, CPUreg.bc.B, false); }
void op_0xCB71() { bitTest(6, CPUreg.bc.C, false); }
void op_0xCB72() { bitTest(6, CPUreg.de.D, false); }
void op_0xCB73() { bitTest(6, CPUreg.de.E, false); }
void op_0xCB74() { bitTest(6, CPUreg.hl.H, false); }
void op_0xCB75() { bitTest(6, CPUreg.hl.L, false); }
void op_0xCB76() { bitTest(6, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB77() { bitTest(6, CPUreg.af.A, false); }

// BIT 7
void op_0xCB78() { bitTest(7, CPUreg.bc.B, false); }
void op_0xCB79() { bitTest(7, CPUreg.bc.C, false); }
void op_0xCB7A() { bitTest(7, CPUreg.de.D, false); }
void op_0xCB7B() { bitTest(7, CPUreg.de.E, false); }
void op_0xCB7C() { bitTest(7, CPUreg.hl.H, false); }
void op_0xCB7D() { bitTest(7, CPUreg.hl.L, false); }
void op_0xCB7E() { bitTest(7, *memoryMap[CPUreg.hl.HL], true); }
void op_0xCB7F() { bitTest(7, CPUreg.af.A, false); }



//RES

void resBit(uint8_t bit, uint8_t* reg, bool isMemory) {
    uint8_t value = *reg; // get current value of the register or memory location
    value &= ~(1 << bit); // clear the specified bit
    if (isMemory) {

        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }
    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

// RES 0
void op_0xCB80() { resBit(0, &CPUreg.bc.B, false); }
void op_0xCB81() { resBit(0, &CPUreg.bc.C, false); }
void op_0xCB82() { resBit(0, &CPUreg.de.D, false); }
void op_0xCB83() { resBit(0, &CPUreg.de.E, false); }
void op_0xCB84() { resBit(0, &CPUreg.hl.H, false); }
void op_0xCB85() { resBit(0, &CPUreg.hl.L, false); }
void op_0xCB86() { resBit(0, memoryMap[CPUreg.hl.HL], true); }
void op_0xCB87() { resBit(0, &CPUreg.af.A, false); }

// RES 1
void op_0xCB88() { resBit(1, &CPUreg.bc.B, false); }
void op_0xCB89() { resBit(1, &CPUreg.bc.C, false); }
void op_0xCB8A() { resBit(1, &CPUreg.de.D, false); }
void op_0xCB8B() { resBit(1, &CPUreg.de.E, false); }
void op_0xCB8C() { resBit(1, &CPUreg.hl.H, false); }
void op_0xCB8D() { resBit(1, &CPUreg.hl.L, false); }
void op_0xCB8E() { resBit(1, memoryMap[CPUreg.hl.HL], true); }
void op_0xCB8F() { resBit(1, &CPUreg.af.A, false); }

// RES 2
void op_0xCB90() { resBit(2, &CPUreg.bc.B, false); }
void op_0xCB91() { resBit(2, &CPUreg.bc.C, false); }
void op_0xCB92() { resBit(2, &CPUreg.de.D, false); }
void op_0xCB93() { resBit(2, &CPUreg.de.E, false); }
void op_0xCB94() { resBit(2, &CPUreg.hl.H, false); }
void op_0xCB95() { resBit(2, &CPUreg.hl.L, false); }
void op_0xCB96() { resBit(2, memoryMap[CPUreg.hl.HL], true); }
void op_0xCB97() { resBit(2, &CPUreg.af.A, false); }

// RES 3
void op_0xCB98() { resBit(3, &CPUreg.bc.B, false); }
void op_0xCB99() { resBit(3, &CPUreg.bc.C, false); }
void op_0xCB9A() { resBit(3, &CPUreg.de.D, false); }
void op_0xCB9B() { resBit(3, &CPUreg.de.E, false); }
void op_0xCB9C() { resBit(3, &CPUreg.hl.H, false); }
void op_0xCB9D() { resBit(3, &CPUreg.hl.L, false); }
void op_0xCB9E() { resBit(3, memoryMap[CPUreg.hl.HL], true); }
void op_0xCB9F() { resBit(3, &CPUreg.af.A, false); }

// RES 4
void op_0xCBA0() { resBit(4, &CPUreg.bc.B, false); }
void op_0xCBA1() { resBit(4, &CPUreg.bc.C, false); }
void op_0xCBA2() { resBit(4, &CPUreg.de.D, false); }
void op_0xCBA3() { resBit(4, &CPUreg.de.E, false); }
void op_0xCBA4() { resBit(4, &CPUreg.hl.H, false); }
void op_0xCBA5() { resBit(4, &CPUreg.hl.L, false); }
void op_0xCBA6() { resBit(4, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBA7() { resBit(4, &CPUreg.af.A, false); }

// RES 5
void op_0xCBA8() { resBit(5, &CPUreg.bc.B, false); }
void op_0xCBA9() { resBit(5, &CPUreg.bc.C, false); }
void op_0xCBAA() { resBit(5, &CPUreg.de.D, false); }
void op_0xCBAB() { resBit(5, &CPUreg.de.E, false); }
void op_0xCBAC() { resBit(5, &CPUreg.hl.H, false); }
void op_0xCBAD() { resBit(5, &CPUreg.hl.L, false); }
void op_0xCBAE() { resBit(5, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBAF() { resBit(5, &CPUreg.af.A, false); }

// RES 6
void op_0xCBB0() { resBit(6, &CPUreg.bc.B, false); }
void op_0xCBB1() { resBit(6, &CPUreg.bc.C, false); }
void op_0xCBB2() { resBit(6, &CPUreg.de.D, false); }
void op_0xCBB3() { resBit(6, &CPUreg.de.E, false); }
void op_0xCBB4() { resBit(6, &CPUreg.hl.H, false); }
void op_0xCBB5() { resBit(6, &CPUreg.hl.L, false); }
void op_0xCBB6() { resBit(6, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBB7() { resBit(6, &CPUreg.af.A, false); }

// RES 7
void op_0xCBB8() { resBit(7, &CPUreg.bc.B, false); }
void op_0xCBB9() { resBit(7, &CPUreg.bc.C, false); }
void op_0xCBBA() { resBit(7, &CPUreg.de.D, false); }
void op_0xCBBB() { resBit(7, &CPUreg.de.E, false); }
void op_0xCBBC() { resBit(7, &CPUreg.hl.H, false); }
void op_0xCBBD() { resBit(7, &CPUreg.hl.L, false); }
void op_0xCBBE() { 
    resBit(7, memoryMap[CPUreg.hl.HL], true);
    /*
    if(CPUreg.hl.HL == 0xFF40 ) { //disable LCD
    *memoryMap[0xFF44] = 0; // Reset LY to 0
        *memoryMap[0xFF41] = (*memoryMap[0xFF41] & ~0x03) | 0x00; // Set mode to 0 (HBlank)
        *memoryMap[0xFF41] &= ~(1 << 2); // Coincidence flag cleared (unless LY==LYC)
        mode0Timer = 0;
        mode1Timer = 0;
        mode2Timer = 0;
        scanlineTimer = 0;
        xPos = 0;
        LCDdisabled = 1; // Set LCD disabled flag
        newScanLine = 1;
        fetchStage.objectFetchStage = 0; // Reset object fetch stage
        fetchStage.BGFetchStage = 0; // Reset background fetch stage
        fetchStage.windowFetchMode = 0; // Reset window fetch mode
        for (int i = 0; i < 8; i++) { // Clear BGFifo
            Pixel discard;
            dequeue(&BGFifo, &discard);
            dequeue(&SpriteFifo, &discard); // Clear SpriteFifo as well
    }
    fprintf(logFile, "LCDC disabled CBBE, cycle: %ld\n", realCyclesAccumulated);
 }
    */
}
void op_0xCBBF() { resBit(7, &CPUreg.af.A, false); }

//SET

void setBit(uint8_t bit, uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    value |= (1 << bit);
    if (isMemory) {
        LDVal8(value, memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }
    CPUreg.PC += 1;
    cyclesAccumulated += isMemory ? 12 : 4;
}

// SET 0
void op_0xCBC0() { setBit(0, &CPUreg.bc.B, false); }
void op_0xCBC1() { setBit(0, &CPUreg.bc.C, false); }
void op_0xCBC2() { setBit(0, &CPUreg.de.D, false); }
void op_0xCBC3() { setBit(0, &CPUreg.de.E, false); }
void op_0xCBC4() { setBit(0, &CPUreg.hl.H, false); }
void op_0xCBC5() { setBit(0, &CPUreg.hl.L, false); }
void op_0xCBC6() { setBit(0, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBC7() { setBit(0, &CPUreg.af.A, false); }

// SET 1
void op_0xCBC8() { setBit(1, &CPUreg.bc.B, false); }
void op_0xCBC9() { setBit(1, &CPUreg.bc.C, false); }
void op_0xCBCA() { setBit(1, &CPUreg.de.D, false); }
void op_0xCBCB() { setBit(1, &CPUreg.de.E, false); }
void op_0xCBCC() { setBit(1, &CPUreg.hl.H, false); }
void op_0xCBCD() { setBit(1, &CPUreg.hl.L, false); }
void op_0xCBCE() { setBit(1, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBCF() { setBit(1, &CPUreg.af.A, false); }

// SET 2
void op_0xCBD0() { setBit(2, &CPUreg.bc.B, false); }
void op_0xCBD1() { setBit(2, &CPUreg.bc.C, false); }
void op_0xCBD2() { setBit(2, &CPUreg.de.D, false); }
void op_0xCBD3() { setBit(2, &CPUreg.de.E, false); }
void op_0xCBD4() { setBit(2, &CPUreg.hl.H, false); }
void op_0xCBD5() { setBit(2, &CPUreg.hl.L, false); }
void op_0xCBD6() { setBit(2, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBD7() { setBit(2, &CPUreg.af.A, false); }

// SET 3
void op_0xCBD8() { setBit(3, &CPUreg.bc.B, false); }
void op_0xCBD9() { setBit(3, &CPUreg.bc.C, false); }
void op_0xCBDA() { setBit(3, &CPUreg.de.D, false); }
void op_0xCBDB() { setBit(3, &CPUreg.de.E, false); }
void op_0xCBDC() { setBit(3, &CPUreg.hl.H, false); }
void op_0xCBDD() { setBit(3, &CPUreg.hl.L, false); }
void op_0xCBDE() { setBit(3, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBDF() { setBit(3, &CPUreg.af.A, false); }

// SET 4
void op_0xCBE0() { setBit(4, &CPUreg.bc.B, false); }
void op_0xCBE1() { setBit(4, &CPUreg.bc.C, false); }
void op_0xCBE2() { setBit(4, &CPUreg.de.D, false); }
void op_0xCBE3() { setBit(4, &CPUreg.de.E, false); }
void op_0xCBE4() { setBit(4, &CPUreg.hl.H, false); }
void op_0xCBE5() { setBit(4, &CPUreg.hl.L, false); }
void op_0xCBE6() { setBit(4, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBE7() { setBit(4, &CPUreg.af.A, false); }

// SET 5
void op_0xCBE8() { setBit(5, &CPUreg.bc.B, false); }
void op_0xCBE9() { setBit(5, &CPUreg.bc.C, false); }
void op_0xCBEA() { setBit(5, &CPUreg.de.D, false); }
void op_0xCBEB() { setBit(5, &CPUreg.de.E, false); }
void op_0xCBEC() { setBit(5, &CPUreg.hl.H, false); }
void op_0xCBED() { setBit(5, &CPUreg.hl.L, false); }
void op_0xCBEE() { setBit(5, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBEF() { setBit(5, &CPUreg.af.A, false); }

// SET 6
void op_0xCBF0() { setBit(6, &CPUreg.bc.B, false); }
void op_0xCBF1() { setBit(6, &CPUreg.bc.C, false); }
void op_0xCBF2() { setBit(6, &CPUreg.de.D, false); }
void op_0xCBF3() { setBit(6, &CPUreg.de.E, false); }
void op_0xCBF4() { setBit(6, &CPUreg.hl.H, false); }
void op_0xCBF5() { setBit(6, &CPUreg.hl.L, false); }
void op_0xCBF6() { setBit(6, memoryMap[CPUreg.hl.HL], true); }
void op_0xCBF7() { setBit(6, &CPUreg.af.A, false); }

// SET 7
void op_0xCBF8() { setBit(7, &CPUreg.bc.B, false); }
void op_0xCBF9() { setBit(7, &CPUreg.bc.C, false); }
void op_0xCBFA() { setBit(7, &CPUreg.de.D, false); }
void op_0xCBFB() { setBit(7, &CPUreg.de.E, false); }
void op_0xCBFC() { setBit(7, &CPUreg.hl.H, false); }
void op_0xCBFD() { setBit(7, &CPUreg.hl.L, false); }
void op_0xCBFE() { 
    setBit(7, memoryMap[CPUreg.hl.HL], true);
    /*
    if (CPUreg.hl.HL == 0xFF40 && LCDdisabled == 1) {
        LCDdelayflag = 4; // Set LCD delay flag to turn back on
        fprintf(logFile, "LCDC delay flag from CBFE, cycle: %ld\n", realCyclesAccumulated);
    }
        */
 }
void op_0xCBFF() { setBit(7, &CPUreg.af.A, false); }

int CBFlag = 0; //CB flag to indicate if CB instruction is being executed
void op_0xCB(){
    CBFlag = 1;
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4; //CB instruction takes 4 cycles at start
    //getchar();
}

uint16_t pcBacktrace[8];
int backtraceIndex = 0;

void dumpCPUState() {
    printf("---- CPU STATE ----\n");
    printf("AF = %04X\n", CPUreg.af.AF);
    printf("BC = %04X\n", CPUreg.bc.BC);
    printf("DE = %04X\n", CPUreg.de.DE);
    printf("HL = %04X\n", CPUreg.hl.HL);
    printf("SP = %04X\n", CPUreg.SP);
    printf("PC = %04X\n", CPUreg.PC);
    printf("IME = %d\n", CPUreg.IME);

    // Dump top of stack (avoid invalid memory access)
    printf("Stack top: ");
    for (int i = 0; i < 4; i++) {
        printf("%02X ", *memoryMap[(CPUreg.SP + i) & 0xFFFF]);
    }

    // Dump backtrace
    printf("\nBacktrace (most recent last):\n");
    for (int i = 0; i < 8; i++) {
        int idx = (backtraceIndex + i) % 8;
        printf("  PC = 0x%04X\n", pcBacktrace[idx]);
    }

    printf("\n--------------------\n");
}

void executeOpcode(uint8_t opcode) {
    switch (opcode) {
        case 0x00: op_0x00(); break;
        case 0x01: op_0x01(); break;
        case 0x02: op_0x02(); break;
        case 0x03: op_0x03(); break;
        case 0x04: op_0x04(); break;
        case 0x05: op_0x05(); break;
        case 0x06: op_0x06(); break;
        case 0x07: op_0x07(); break;
        case 0x08: op_0x08(); break;
        case 0x09: op_0x09(); break;
        case 0x0A: op_0x0A(); break;
        case 0x0B: op_0x0B(); break;
        case 0x0C: op_0x0C(); break;
        case 0x0D: op_0x0D(); break;
        case 0x0E: op_0x0E(); break;
        case 0x0F: op_0x0F(); break;
        case 0x10: op_0x10(); break;
        case 0x11: op_0x11(); break;
        case 0x12: op_0x12(); break;
        case 0x13: op_0x13(); break;
        case 0x14: op_0x14(); break;
        case 0x15: op_0x15(); break;
        case 0x16: op_0x16(); break;
        case 0x17: op_0x17(); break;
        case 0x18: op_0x18(); break;
        case 0x19: op_0x19(); break;
        case 0x1A: op_0x1A(); break;
        case 0x1B: op_0x1B(); break;
        case 0x1C: op_0x1C(); break;
        case 0x1D: op_0x1D(); break;
        case 0x1E: op_0x1E(); break;
        case 0x1F: op_0x1F(); break;
        case 0x20: op_0x20(); break;
        case 0x21: op_0x21(); break;
        case 0x22: op_0x22(); break;
        case 0x23: op_0x23(); break;
        case 0x24: op_0x24(); break;
        case 0x25: op_0x25(); break;
        case 0x26: op_0x26(); break;
        case 0x27: op_0x27(); break;
        case 0x28: op_0x28(); break;
        case 0x29: op_0x29(); break;
        case 0x2A: op_0x2A(); break;
        case 0x2B: op_0x2B(); break;
        case 0x2C: op_0x2C(); break;
        case 0x2D: op_0x2D(); break;
        case 0x2E: op_0x2E(); break;
        case 0x2F: op_0x2F(); break;
        case 0x30: op_0x30(); break;
        case 0x31: op_0x31(); break;
        case 0x32: op_0x32(); break;
        case 0x33: op_0x33(); break;
        case 0x34: op_0x34(); break;
        case 0x35: op_0x35(); break;
        case 0x36: op_0x36(); break;
        case 0x37: op_0x37(); break;
        case 0x38: op_0x38(); break;
        case 0x39: op_0x39(); break;
        case 0x3A: op_0x3A(); break;
        case 0x3B: op_0x3B(); break;
        case 0x3C: op_0x3C(); break;
        case 0x3D: op_0x3D(); break;
        case 0x3E: op_0x3E(); break;
        case 0x3F: op_0x3F(); break;
        case 0x40: op_0x40(); break;
        case 0x41: op_0x41(); break;
        case 0x42: op_0x42(); break;
        case 0x43: op_0x43(); break;
        case 0x44: op_0x44(); break;
        case 0x45: op_0x45(); break;
        case 0x46: op_0x46(); break;
        case 0x47: op_0x47(); break;
        case 0x48: op_0x48(); break;
        case 0x49: op_0x49(); break;
        case 0x4A: op_0x4A(); break;
        case 0x4B: op_0x4B(); break;
        case 0x4C: op_0x4C(); break;
        case 0x4D: op_0x4D(); break;
        case 0x4E: op_0x4E(); break;
        case 0x4F: op_0x4F(); break;
        case 0x50: op_0x50(); break;
        case 0x51: op_0x51(); break;
        case 0x52: op_0x52(); break;
        case 0x53: op_0x53(); break;
        case 0x54: op_0x54(); break;
        case 0x55: op_0x55(); break;
        case 0x56: op_0x56(); break;
        case 0x57: op_0x57(); break;
        case 0x58: op_0x58(); break;
        case 0x59: op_0x59(); break;
        case 0x5A: op_0x5A(); break;
        case 0x5B: op_0x5B(); break;
        case 0x5C: op_0x5C(); break;
        case 0x5D: op_0x5D(); break;
        case 0x5E: op_0x5E(); break;
        case 0x5F: op_0x5F(); break;
        case 0x60: op_0x60(); break;
        case 0x61: op_0x61(); break;
        case 0x62: op_0x62(); break;
        case 0x63: op_0x63(); break;
        case 0x64: op_0x64(); break;
        case 0x65: op_0x65(); break;
        case 0x66: op_0x66(); break;
        case 0x67: op_0x67(); break;
        case 0x68: op_0x68(); break;
        case 0x69: op_0x69(); break;
        case 0x6A: op_0x6A(); break;
        case 0x6B: op_0x6B(); break;
        case 0x6C: op_0x6C(); break;
        case 0x6D: op_0x6D(); break;
        case 0x6E: op_0x6E(); break;
        case 0x6F: op_0x6F(); break;
        case 0x70: op_0x70(); break;
        case 0x71: op_0x71(); break;
        case 0x72: op_0x72(); break;
        case 0x73: op_0x73(); break;
        case 0x74: op_0x74(); break;
        case 0x75: op_0x75(); break;
        case 0x76: op_0x76(); break;
        case 0x77: op_0x77(); break;
        case 0x78: op_0x78(); break;
        case 0x79: op_0x79(); break;
        case 0x7A: op_0x7A(); break;
        case 0x7B: op_0x7B(); break;
        case 0x7C: op_0x7C(); break;
        case 0x7D: op_0x7D(); break;
        case 0x7E: op_0x7E(); break;
        case 0x7F: op_0x7F(); break;
        case 0x80: op_0x80(); break;
        case 0x81: op_0x81(); break;
        case 0x82: op_0x82(); break;
        case 0x83: op_0x83(); break;
        case 0x84: op_0x84(); break;
        case 0x85: op_0x85(); break;
        case 0x86: op_0x86(); break;
        case 0x87: op_0x87(); break;
        case 0x88: op_0x88(); break;
        case 0x89: op_0x89(); break;
        case 0x8A: op_0x8A(); break;
        case 0x8B: op_0x8B(); break;
        case 0x8C: op_0x8C(); break;
        case 0x8D: op_0x8D(); break;
        case 0x8E: op_0x8E(); break;
        case 0x8F: op_0x8F(); break;
        case 0x90: op_0x90(); break;
        case 0x91: op_0x91(); break;
        case 0x92: op_0x92(); break;
        case 0x93: op_0x93(); break;
        case 0x94: op_0x94(); break;
        case 0x95: op_0x95(); break;
        case 0x96: op_0x96(); break;
        case 0x97: op_0x97(); break;
        case 0x98: op_0x98(); break;
        case 0x99: op_0x99(); break;
        case 0x9A: op_0x9A(); break;
        case 0x9B: op_0x9B(); break;
        case 0x9C: op_0x9C(); break;
        case 0x9D: op_0x9D(); break;
        case 0x9E: op_0x9E(); break;
        case 0x9F: op_0x9F(); break;
        case 0xA0: op_0xA0(); break;
        case 0xA1: op_0xA1(); break;
        case 0xA2: op_0xA2(); break;
        case 0xA3: op_0xA3(); break;
        case 0xA4: op_0xA4(); break;
        case 0xA5: op_0xA5(); break;
        case 0xA6: op_0xA6(); break;
        case 0xA7: op_0xA7(); break;
        case 0xA8: op_0xA8(); break;
        case 0xA9: op_0xA9(); break;
        case 0xAA: op_0xAA(); break;
        case 0xAB: op_0xAB(); break;
        case 0xAC: op_0xAC(); break;
        case 0xAD: op_0xAD(); break;
        case 0xAE: op_0xAE(); break;
        case 0xAF: op_0xAF(); break;
        case 0xB0: op_0xB0(); break;
        case 0xB1: op_0xB1(); break;
        case 0xB2: op_0xB2(); break;
        case 0xB3: op_0xB3(); break;
        case 0xB4: op_0xB4(); break;
        case 0xB5: op_0xB5(); break;
        case 0xB6: op_0xB6(); break;
        case 0xB7: op_0xB7(); break;
        case 0xB8: op_0xB8(); break;
        case 0xB9: op_0xB9(); break;
        case 0xBA: op_0xBA(); break;
        case 0xBB: op_0xBB(); break;
        case 0xBC: op_0xBC(); break;
        case 0xBD: op_0xBD(); break;
        case 0xBE: op_0xBE(); break;
        case 0xBF: op_0xBF(); break;
        case 0xC0: op_0xC0(); break;
        case 0xC1: op_0xC1(); break;
        case 0xC2: op_0xC2(); break;
        case 0xC3: op_0xC3(); break;
        case 0xC4: op_0xC4(); break;
        case 0xC5: op_0xC5(); break;
        case 0xC6: op_0xC6(); break;
        case 0xC7: op_0xC7(); break;
        case 0xC8: op_0xC8(); break;
        case 0xC9: op_0xC9(); break;
        case 0xCA: op_0xCA(); break;
        case 0xCB: op_0xCB(); break; //for CB instructions
        case 0xCC: op_0xCC(); break;
        case 0xCD: op_0xCD(); break;
        case 0xCE: op_0xCE(); break;
        case 0xCF: op_0xCF(); break;
        case 0xD0: op_0xD0(); break;
        case 0xD1: op_0xD1(); break;
        case 0xD2: op_0xD2(); break;
        case 0xD3: /* implement if needed */ break;
        case 0xD4: op_0xD4(); break;
        case 0xD5: op_0xD5(); break;
        case 0xD6: op_0xD6(); break;
        case 0xD7: op_0xD7(); break;
        case 0xD8: op_0xD8(); break;
        case 0xD9: op_0xD9(); break;
        case 0xDA: op_0xDA(); break;
        case 0xDB: /* implement if needed */ break;
        case 0xDC: op_0xDC(); break;
        case 0xDD: /* implement if needed */ break;
        case 0xDE: op_0xDE(); break;
        case 0xDF: op_0xDF(); break;
        case 0xE0: op_0xE0(); break;
        case 0xE1: op_0xE1(); break;
        case 0xE2: op_0xE2(); break;
        case 0xE3: /* implement if needed */ break;
        case 0xE5: op_0xE5(); break;
        case 0xE6: op_0xE6(); break;
        case 0xE7: op_0xE7(); break;
        case 0xE8: op_0xE8(); break;
        case 0xE9: op_0xE9(); break;
        case 0xEA: op_0xEA(); break;
        case 0xEB: /* implement if needed */ break;
        case 0xED: /* implement if needed */ break;
        case 0xEE: op_0xEE(); break;
        case 0xEF: op_0xEF(); break;
        case 0xF0: op_0xF0(); break;
        case 0xF1: op_0xF1(); break;
        case 0xF2: op_0xF2(); break;
        case 0xF3: op_0xF3(); break;
        case 0xF5: op_0xF5(); break;
        case 0xF6: op_0xF6(); break;
        case 0xF7: op_0xF7(); break;
        case 0xF8: op_0xF8(); break;
        case 0xF9: op_0xF9(); break;
        case 0xFA: op_0xFA(); break;
        case 0xFB: op_0xFB(); break;
        case 0xFE: op_0xFE(); break;
        case 0xFF: op_0xFF(); break;
        default: 
            printf("Invalid or unimplemented opcode: 0x%02X at PC=0x%04X\n", opcode, CPUreg.PC - 1);
            dumpCPUState();
            getchar(); // Exit if an unknown opcode is encountered
            CPUreg.PC += 1; // increment PC to skip unknown opcode
            break;
    }
}

void executeOpcodeCB(uint8_t opcode) {
    switch (opcode) {
        case 0x00: op_0xCB00(); break;
        case 0x01: op_0xCB01(); break;
        case 0x02: op_0xCB02(); break;
        case 0x03: op_0xCB03(); break;
        case 0x04: op_0xCB04(); break;
        case 0x05: op_0xCB05(); break;
        case 0x06: op_0xCB06(); break;
        case 0x07: op_0xCB07(); break;
        case 0x08: op_0xCB08(); break;
        case 0x09: op_0xCB09(); break;
        case 0x0A: op_0xCB0A(); break;
        case 0x0B: op_0xCB0B(); break;
        case 0x0C: op_0xCB0C(); break;
        case 0x0D: op_0xCB0D(); break;
        case 0x0E: op_0xCB0E(); break;
        case 0x0F: op_0xCB0F(); break;
        case 0x10: op_0xCB10(); break;
        case 0x11: op_0xCB11(); break;
        case 0x12: op_0xCB12(); break;
        case 0x13: op_0xCB13(); break;
        case 0x14: op_0xCB14(); break;
        case 0x15: op_0xCB15(); break;
        case 0x16: op_0xCB16(); break;
        case 0x17: op_0xCB17(); break;
        case 0x18: op_0xCB18(); break;
        case 0x19: op_0xCB19(); break;
        case 0x1A: op_0xCB1A(); break;
        case 0x1B: op_0xCB1B(); break;
        case 0x1C: op_0xCB1C(); break;
        case 0x1D: op_0xCB1D(); break;
        case 0x1E: op_0xCB1E(); break;
        case 0x1F: op_0xCB1F(); break;
        case 0x20: op_0xCB20(); break;
        case 0x21: op_0xCB21(); break;
        case 0x22: op_0xCB22(); break;
        case 0x23: op_0xCB23(); break;
        case 0x24: op_0xCB24(); break;
        case 0x25: op_0xCB25(); break;
        case 0x26: op_0xCB26(); break;
        case 0x27: op_0xCB27(); break;
        case 0x28: op_0xCB28(); break;
        case 0x29: op_0xCB29(); break;
        case 0x2A: op_0xCB2A(); break;
        case 0x2B: op_0xCB2B(); break;
        case 0x2C: op_0xCB2C(); break;
        case 0x2D: op_0xCB2D(); break;
        case 0x2E: op_0xCB2E(); break;
        case 0x2F: op_0xCB2F(); break;
        case 0x30: op_0xCB30(); break;
        case 0x31: op_0xCB31(); break;
        case 0x32: op_0xCB32(); break;
        case 0x33: op_0xCB33(); break;
        case 0x34: op_0xCB34(); break;
        case 0x35: op_0xCB35(); break;
        case 0x36: op_0xCB36(); break;
        case 0x37: op_0xCB37(); break;
        case 0x38: op_0xCB38(); break;
        case 0x39: op_0xCB39(); break;
        case 0x3A: op_0xCB3A(); break;
        case 0x3B: op_0xCB3B(); break;
        case 0x3C: op_0xCB3C(); break;
        case 0x3D: op_0xCB3D(); break;
        case 0x3E: op_0xCB3E(); break;
        case 0x3F: op_0xCB3F(); break;
        case 0x40: op_0xCB40(); break;
        case 0x41: op_0xCB41(); break;
        case 0x42: op_0xCB42(); break;
        case 0x43: op_0xCB43(); break;
        case 0x44: op_0xCB44(); break;
        case 0x45: op_0xCB45(); break;
        case 0x46: op_0xCB46(); break;
        case 0x47: op_0xCB47(); break;
        case 0x48: op_0xCB48(); break;
        case 0x49: op_0xCB49(); break;
        case 0x4A: op_0xCB4A(); break;
        case 0x4B: op_0xCB4B(); break;
        case 0x4C: op_0xCB4C(); break;
        case 0x4D: op_0xCB4D(); break;
        case 0x4E: op_0xCB4E(); break;
        case 0x4F: op_0xCB4F(); break;
        case 0x50: op_0xCB50(); break;
        case 0x51: op_0xCB51(); break;
        case 0x52: op_0xCB52(); break;
        case 0x53: op_0xCB53(); break;
        case 0x54: op_0xCB54(); break;
        case 0x55: op_0xCB55(); break;
        case 0x56: op_0xCB56(); break;
        case 0x57: op_0xCB57(); break;
        case 0x58: op_0xCB58(); break;
        case 0x59: op_0xCB59(); break;
        case 0x5A: op_0xCB5A(); break;
        case 0x5B: op_0xCB5B(); break;
        case 0x5C: op_0xCB5C(); break;
        case 0x5D: op_0xCB5D(); break;
        case 0x5E: op_0xCB5E(); break;
        case 0x5F: op_0xCB5F(); break;
        case 0x60: op_0xCB60(); break;
        case 0x61: op_0xCB61(); break;
        case 0x62: op_0xCB62(); break;
        case 0x63: op_0xCB63(); break;
        case 0x64: op_0xCB64(); break;
        case 0x65: op_0xCB65(); break;
        case 0x66: op_0xCB66(); break;
        case 0x67: op_0xCB67(); break;
        case 0x68: op_0xCB68(); break;
        case 0x69: op_0xCB69(); break;
        case 0x6A: op_0xCB6A(); break;
        case 0x6B: op_0xCB6B(); break;
        case 0x6C: op_0xCB6C(); break;
        case 0x6D: op_0xCB6D(); break;
        case 0x6E: op_0xCB6E(); break;
        case 0x6F: op_0xCB6F(); break;
        case 0x70: op_0xCB70(); break;
        case 0x71: op_0xCB71(); break;
        case 0x72: op_0xCB72(); break;
        case 0x73: op_0xCB73(); break;
        case 0x74: op_0xCB74(); break;
        case 0x75: op_0xCB75(); break;
        case 0x76: op_0xCB76(); break;
        case 0x77: op_0xCB77(); break;
        case 0x78: op_0xCB78(); break;
        case 0x79: op_0xCB79(); break;
        case 0x7A: op_0xCB7A(); break;
        case 0x7B: op_0xCB7B(); break;
        case 0x7C: op_0xCB7C(); break;
        case 0x7D: op_0xCB7D(); break;
        case 0x7E: op_0xCB7E(); break;
        case 0x7F: op_0xCB7F(); break;
        case 0x80: op_0xCB80(); break;
        case 0x81: op_0xCB81(); break;
        case 0x82: op_0xCB82(); break;
        case 0x83: op_0xCB83(); break;
        case 0x84: op_0xCB84(); break;
        case 0x85: op_0xCB85(); break;
        case 0x86: op_0xCB86(); break;
        case 0x87: op_0xCB87(); break;
        case 0x88: op_0xCB88(); break;
        case 0x89: op_0xCB89(); break;
        case 0x8A: op_0xCB8A(); break;
        case 0x8B: op_0xCB8B(); break;
        case 0x8C: op_0xCB8C(); break;
        case 0x8D: op_0xCB8D(); break;
        case 0x8E: op_0xCB8E(); break;
        case 0x8F: op_0xCB8F(); break;
        case 0x90: op_0xCB90(); break;
        case 0x91: op_0xCB91(); break;
        case 0x92: op_0xCB92(); break;
        case 0x93: op_0xCB93(); break;
        case 0x94: op_0xCB94(); break;
        case 0x95: op_0xCB95(); break;
        case 0x96: op_0xCB96(); break;
        case 0x97: op_0xCB97(); break;
        case 0x98: op_0xCB98(); break;
        case 0x99: op_0xCB99(); break;
        case 0x9A: op_0xCB9A(); break;
        case 0x9B: op_0xCB9B(); break;
        case 0x9C: op_0xCB9C(); break;
        case 0x9D: op_0xCB9D(); break;
        case 0x9E: op_0xCB9E(); break;
        case 0x9F: op_0xCB9F(); break;
        case 0xA0: op_0xCBA0(); break;
        case 0xA1: op_0xCBA1(); break;
        case 0xA2: op_0xCBA2(); break;
        case 0xA3: op_0xCBA3(); break;
        case 0xA4: op_0xCBA4(); break;
        case 0xA5: op_0xCBA5(); break;
        case 0xA6: op_0xCBA6(); break;
        case 0xA7: op_0xCBA7(); break;
        case 0xA8: op_0xCBA8(); break;
        case 0xA9: op_0xCBA9(); break;
        case 0xAA: op_0xCBAA(); break;
        case 0xAB: op_0xCBAB(); break;
        case 0xAC: op_0xCBAC(); break;
        case 0xAD: op_0xCBAD(); break;
        case 0xAE: op_0xCBAE(); break;
        case 0xAF: op_0xCBAF(); break;
        case 0xB0: op_0xCBB0(); break;
        case 0xB1: op_0xCBB1(); break;
        case 0xB2: op_0xCBB2(); break;
        case 0xB3: op_0xCBB3(); break;
        case 0xB4: op_0xCBB4(); break;
        case 0xB5: op_0xCBB5(); break;
        case 0xB6: op_0xCBB6(); break;
        case 0xB7: op_0xCBB7(); break;
        case 0xB8: op_0xCBB8(); break;
        case 0xB9: op_0xCBB9(); break;
        case 0xBA: op_0xCBBA(); break;
        case 0xBB: op_0xCBBB(); break;
        case 0xBC: op_0xCBBC(); break;
        case 0xBD: op_0xCBBD(); break;
        case 0xBE: op_0xCBBE(); break;
        case 0xBF: op_0xCBBF(); break;
        case 0xC0: op_0xCBC0(); break;
        case 0xC1: op_0xCBC1(); break;
        case 0xC2: op_0xCBC2(); break;
        case 0xC3: op_0xCBC3(); break;
        case 0xC4: op_0xCBC4(); break;
        case 0xC5: op_0xCBC5(); break;
        case 0xC6: op_0xCBC6(); break;
        case 0xC7: op_0xCBC7(); break;
        case 0xC8: op_0xCBC8(); break;
        case 0xC9: op_0xCBC9(); break;
        case 0xCA: op_0xCBCA(); break;
        case 0xCB: op_0xCBCB(); break;
        case 0xCC: op_0xCBCC(); break;
        case 0xCD: op_0xCBCD(); break;
        case 0xCE: op_0xCBCE(); break;
        case 0xCF: op_0xCBCF(); break;
        case 0xD0: op_0xCBD0(); break;
        case 0xD1: op_0xCBD1(); break;
        case 0xD2: op_0xCBD2(); break;
        case 0xD3: op_0xCBD3(); break;
        case 0xD4: op_0xCBD4(); break;
        case 0xD5: op_0xCBD5(); break;
        case 0xD6: op_0xCBD6(); break;
        case 0xD7: op_0xCBD7(); break;
        case 0xD8: op_0xCBD8(); break;
        case 0xD9: op_0xCBD9(); break;
        case 0xDA: op_0xCBDA(); break;
        case 0xDB: op_0xCBDB(); break;
        case 0xDC: op_0xCBDC(); break;
        case 0xDD: op_0xCBDD(); break;
        case 0xDE: op_0xCBDE(); break;
        case 0xDF: op_0xCBDF(); break;
        case 0xE0: op_0xCBE0(); break;
        case 0xE1: op_0xCBE1(); break;
        case 0xE2: op_0xCBE2(); break;
        case 0xE3: op_0xCBE3(); break;
        case 0xE4: op_0xCBE4(); break;
        case 0xE5: op_0xCBE5(); break;
        case 0xE6: op_0xCBE6(); break;
        case 0xE7: op_0xCBE7(); break;
        case 0xE8: op_0xCBE8(); break;
        case 0xE9: op_0xCBE9(); break;
        case 0xEA: op_0xCBEA(); break;
        case 0xEB: op_0xCBEB(); break;
        case 0xEC: op_0xCBEC(); break;
        case 0xED: op_0xCBED(); break;
        case 0xEE: op_0xCBEE(); break;
        case 0xEF: op_0xCBEF(); break;
        case 0xF0: op_0xCBF0(); break;
        case 0xF1: op_0xCBF1(); break;
        case 0xF2: op_0xCBF2(); break;
        case 0xF3: op_0xCBF3(); break;
        case 0xF4: op_0xCBF4(); break;
        case 0xF5: op_0xCBF5(); break;
        case 0xF6: op_0xCBF6(); break;
        case 0xF7: op_0xCBF7(); break;
        case 0xF8: op_0xCBF8(); break;
        case 0xF9: op_0xCBF9(); break;
        case 0xFA: op_0xCBFA(); break;
        case 0xFB: op_0xCBFB(); break;
        case 0xFC: op_0xCBFC(); break;
        case 0xFD: op_0xCBFD(); break;
        case 0xFE: op_0xCBFE(); break;
        case 0xFF: op_0xCBFF(); break;
        default: 
            printf("Invalid or unimplemented CB opcode: 0x%02X at PC=0x%04X\n", opcode, CPUreg.PC - 1);
            dumpCPUState();
            //exit(1); // Exit if an unknown opcode is encountered
            //CPUreg.PC += 1; // increment PC to skip unknown opcode
            break;
    }
}

//PPU
/*Screen Resolution: 160×144 visible pixels

VRAM: 8 KB (0x8000–0x9FFF)

OAM (Object Attribute Memory): 0xFE00–0xFE9F

Tiles: 8×8 pixels, 2 bits per pixel → each tile = 16 bytes

PPU Modes:

0: HBlank (idle between scanlines)
1: VBlank (idle between frames)
2: OAM Scan
3: Pixel Transfer (drawing)*/

typedef struct {
    uint8_t* mode;        // Usually from STAT register (0xFF41)
    uint8_t* ly;          // LY register (0xFF44)
    uint8_t* lyc;         // LYC register (0xFF45)
    uint16_t dotCounter;  // Internal, not in memory
    uint8_t frameReady;   // Internal, not in memory
} PPU;


//Object FIFO pixel Queue
//Background/Window FIFO pixel Queue
//Tile Fetcher steps BG/Window, 1 step, 2 step , 3 step push 8 pixel if Bg pixel queue empty otherwise keep try step 3
//Tile Fetcher steps Objects

//OAM Queue 80 clocks, first 10 sprites



typedef struct {
    uint8_t yPos; //first byte Y=16 is top of screen y < 16 has pixels chopped
    uint8_t xPos; //2nd byte X=8 is left of screen, x < 8 has pixels chopped 
    uint8_t tileNum; //third byte start from 8000
    uint8_t flags; //4th byte
} Sprite; //Structure to hold sprite attributes for each sprite


int spriteSearchOAM(uint8_t ly, Sprite* buffer) { //pass in sprite array of size 10
    int count = 0; //track number of sprites in buffer currently
    Sprite spr;

    for (int i = 0; i < 40; i++) { //read all 40 sprites X attributes
        for (int j = 0; j < 4; j++) { 
            uint8_t address = oam[i * 4 + j]; //get value at specific OAM address
            switch (j) {
                case 0: spr.yPos = address; break;
                case 1: spr.xPos = address; break;
                case 2: spr.tileNum = address; break;
                case 3: spr.flags = address; break;
            }
        }
        uint8_t lcdc = *memoryMap[0xFF40]; //get sprites height from 2nd bit of lcdc
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

int windowLine = 0;

void pixelPushBG(Queue* fifo, uint8_t xPos, FetchStage* stage, int fetchWindow) { //retrieve pixels tiles and push current tile row to FIFO Queue 
    //printf("pixelPushBG called, queue count=%d\n", fifo->count);
    if (isEmpty(fifo)) { //Queue must be empty before allowing more pixels to be pushed
        //printf("pixelPushBG: queue is empty, filling...\n");
        uint8_t lcdc = *memoryMap[0xFF40]; // LCD Control
        uint8_t ly   = *memoryMap[0xFF44]; // Current scanline
        uint8_t wx   = *memoryMap[0xFF4B]; // Window X
        uint8_t wy   = *memoryMap[0xFF4A]; // Window Y
        uint8_t bgp  = *memoryMap[0xFF47]; // BG Palette

        uint16_t tileMapBase;
        uint16_t mapX, mapY;
        int useUnsignedTiles = (lcdc & 0x10) != 0; //Whether tiles are in signed/unsigned address area 

        if (fetchWindow) { //if fetching window tile
            // Window tile map base depends on LCDC bit 6
            tileMapBase = (lcdc & 0x40) ? 0x9C00 : 0x9800;

            // Window coordinates relative to window start, NO scrolling applied
            mapX = xPos - (wx - 7);
            mapY = windowLine; //ly - wy;
          //  printf("Window line: %d, LY: %d\n", windowLine, ly);
        } else { //if fetching background tile
            // Background tile map base depends on LCDC bit 3
            tileMapBase = (lcdc & 0x08) ? 0x9C00 : 0x9800;

            // Background coordinates - NO x scrolling applied here, scroll handled by xPos logic later
            uint8_t scy = *memoryMap[0xFF42];
            mapY = (ly + scy) & 0xFF; //y scroll added with wrap 0-255

            uint8_t scx = *memoryMap[0xFF43];
            mapX = (xPos + scx) & 0xFF; //coarse scroll
        }

        // Make sure mapX and mapY are valid (0-255)
        mapX &= 0xFF;
        mapY &= 0xFF;

        uint16_t tileRow = mapY / 8;
        uint16_t tileCol = mapX / 8;
        uint16_t tileIndexAddr = tileMapBase + tileRow * 32 + tileCol;
        int8_t tileNum = *memoryMap[tileIndexAddr];
                        
        uint16_t tileAddr;
        if (useUnsignedTiles) {
            tileAddr = 0x8000 + ((uint8_t)tileNum * 16);
        } else {
            tileAddr = 0x9000 + (tileNum * 16);
        }

        uint8_t line = mapY % 8;
        uint8_t byte1 = *memoryMap[tileAddr + line * 2];
        uint8_t byte2 = *memoryMap[tileAddr + line * 2 + 1];

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
        stage->BGFetchStage = 0;
      // printf("TileNum: %d, Addr: 0x%04X, line=%d, byte1=0x%02X, byte2=0x%02X, TileIndexAddr: 0x%04X, TileRow=%d, TileCol=%d, tileMapBase=%04X, LCDC: 0x%02X, xPos: %d, scx: %d, scy: %d, fetchWindow: %d, wx: %d, windowLine: %d, PC: %04X, wy: %d, windowFetchMode: %d, IE: 0x%02X, IF: 0x%02X, IME: %d\n", tileNum, tileAddr, line, byte1, byte2, tileIndexAddr, tileRow, tileCol, tileMapBase, lcdc, xPos, *memoryMap[0xFF43], *memoryMap[0xFF42], fetchWindow, *memoryMap[0xFF4B], windowLine, CPUreg.PC, *memoryMap[0xFF4A], fetchStage.windowFetchMode, *memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME);
    }
    
}

void printQueueState(const char* name, Queue* q) {
    printf("FIFO %s: count=%d [", name, q->count);
    for (int i = 0; i < q->count; i++) {
        printf("%d", q->data[i].colour);
        if (i < q->count - 1) printf(", ");
    }
    printf("]\n");
}



//Check every x if x = sprite x - 8, if so reset Tile fetcher for Background to step 1 and start tile fetcher for object queue, pause drain of bg queue when done resume drain and bg queue unless another sprite needed
//Fetch first 8 pixels, discard them now x=8
//Check scroll and for second lot of 8 bg/window pixels make the scroll x amount of pixel get discarded after draining, then continue as normal, don't increment x while discarding ?
//then check every x for sprite/ then background then window
//if window then reset tile fetcher for background and start fetch the window
//do until end of scanline?

int display[160][144]; //display array, 160x144 pixels, each pixel is 5x5 window pixel

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
                default: continue; // skip if not a valid color
            }
            SDL_Rect rect = { x * 5, y * 5, 5, 5 };
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}


int CPUtimer = 0; //timer for CPU cycles

void handleInterrupts() {
    uint8_t IE = *memoryMap[0xFFFF]; // Interrupt Enable
    uint8_t IF = *memoryMap[0xFF0F]; // Interrupt Flag

    if (haltMode == 1 && (IE & IF & 0x1F) != 0) {
        haltMode = 0; // CPU will resume 
    }

    if (CPUreg.IME == 0) return; // Interrupts disabled

    uint8_t fired = IE & IF;

    if (!fired) return; // No interrupt requested

    //printf("Interrupt Called: IE=0x%02X IF=0x%02X IME=%d (PC=%04X)\n", IE, IF, CPUreg.IME, CPUreg.PC);

    // Interrupt priorities: VBlank > LCD STAT > Timer > Serial > Joypad
    struct { uint8_t mask; uint16_t vector; } interrupts[] = {
        {0x01, 0x40}, // VBlank
        {0x02, 0x48}, // LCD STAT
        {0x04, 0x50}, // Timer
        {0x08, 0x58}, // Serial
        {0x10, 0x60}, // Joypad
    };

    for (int i = 0; i < 5; i++) {
        if (fired & interrupts[i].mask) {
            CPUreg.IME = 0; // Disable further interrupts
           // printf("IF BEFORE: 0x%02X\n", *memoryMap[0xFF0F]);

            *memoryMap[0xFF0F] &= ~interrupts[i].mask; // Clear IF


            //fired = *memoryMap[0xFFFF] & *memoryMap[0xFF0F];
            //if (!fired) return;

            // Push PC to stack (high byte first)
            *memoryMap[--CPUreg.SP] = (CPUreg.PC >> 8) & 0xFF;
            *memoryMap[--CPUreg.SP] = CPUreg.PC & 0xFF;

            CPUreg.PC = interrupts[i].vector; // Jump to interrupt vector
            cyclesAccumulated += 20; // Interrupt takes 20 cycles
            /*
            if (i==4){
                printf("Interrupt %d fired, PC set to %04X\n", i, CPUreg.PC);
            }
                */
            //printf("INT fired: type=%d, pushing PC=%04X to SP=%04X\n", i, CPUreg.PC, CPUreg.SP);
            //printf("[INTERRUPT] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) \n",*memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC);
            //printf("IF AFTER:  0x%02X\n", *memoryMap[0xFF0F]);
            //fprintf(logFile, "[INTERRUPT] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) Cycles=%d\n", *memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC, realCyclesAccumulated);

            return; // Only handle one interrupt at a time
        }
    }
}

void drawTileViewer(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    uint8_t lcdc = *memoryMap[0xFF40];
    uint8_t bgp  = *memoryMap[0xFF47];

    int useUnsignedTiles = (lcdc & 0x10) != 0;
    
    for (int tileIndex = 0; tileIndex < 384; tileIndex++) {
        int screenX = (tileIndex % 16) * 8;
        int screenY = (tileIndex / 16) * 8;

        int8_t signedIndex = tileIndex - 128; // from -128 to 255
        uint16_t tileAddr = useUnsignedTiles
            ? 0x8000 + tileIndex * 16
            : 0x9000 + signedIndex * 16;

        for (int row = 0; row < 8; row++) {
            uint8_t byte1 = *memoryMap[tileAddr + row * 2];
            uint8_t byte2 = *memoryMap[tileAddr + row * 2 + 1];

            for (int col = 0; col < 8; col++) {
                uint8_t lo = (byte1 >> (7 - col)) & 1;
                uint8_t hi = (byte2 >> (7 - col)) & 1;
                uint8_t colorId = (hi << 1) | lo;
                uint8_t color = (bgp >> (colorId * 2)) & 0x03;

                switch (color) {
                    case 0: SDL_SetRenderDrawColor(renderer, 224, 248, 208, 255); break;
                    case 1: SDL_SetRenderDrawColor(renderer, 136, 192, 112, 255); break;
                    case 2: SDL_SetRenderDrawColor(renderer, 52, 104, 86, 255); break;
                    case 3: SDL_SetRenderDrawColor(renderer, 8, 24, 32, 255); break;
                }

                SDL_RenderDrawPoint(renderer, screenX + col, screenY + row);
            }
        }
    }

    SDL_RenderPresent(renderer);
}

    int isPaused = 0;
    int stepMode = 0;

void renderText(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y) {
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surface = TTF_RenderText_Solid(font, text, white);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    SDL_Rect dest = {x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dest);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void drawDebugWindow(SDL_Renderer *renderer, TTF_Font *font) {
    char line[64];
    int y = 10;
    uint8_t opcode = *memoryMap[CPUreg.PC];

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    sprintf(line, "AF = %04X", CPUreg.af.AF); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "BC = %04X", CPUreg.bc.BC); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "DE = %04X", CPUreg.de.DE); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "HL = %04X", CPUreg.hl.HL); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "SP = %04X", CPUreg.SP);   renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "PC = %04X", CPUreg.PC);   renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "IME = %d", CPUreg.IME);   renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "opCode = %02X", opcode);   renderText(renderer, font, line, 10, y); y += 18;

    renderText(renderer, font, "Memory:", 10, y); y += 18;
    sprintf(line, "FF00 = %02X", *memoryMap[0xFF00]); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "FF41 = %02X", *memoryMap[0xFF41]); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "FF85 = %02X", *memoryMap[0xFF85]); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "FF0F = %02X", *memoryMap[0xFF0F]); renderText(renderer, font, line, 10, y); y += 18;
    sprintf(line, "FFFF = %02X", *memoryMap[0xFFFF]); renderText(renderer, font, line, 10, y); y += 18;

    sprintf(line, "Pause = %d", isPaused); renderText(renderer, font, line, 10, y); y += 18;

    renderText(renderer, font, "Sprites:", 10, y); y += 18;

    for (int i = 0; i < 5; i++) {
        uint8_t yPos = *memoryMap[0xFE00 + i * 4 + 0];
        uint8_t xPos = *memoryMap[0xFE00 + i * 4 + 1];
        uint8_t tile = *memoryMap[0xFE00 + i * 4 + 2];
        uint8_t attr = *memoryMap[0xFE00 + i * 4 + 3];
        sprintf(line, "%02X %02X T:%02X A:%02X", xPos, yPos, tile, attr);
        renderText(renderer, font, line, 10, y);
        y += 18;
    }

    SDL_RenderPresent(renderer);
}

void checkLYC(uint8_t* memoryMap[0x10000]) {
    static uint8_t wasEqual = 0; //prevent repeated interrupt on same line

    uint8_t ly  = *memoryMap[0xFF44];
    uint8_t lyc = *memoryMap[0xFF45];
    uint8_t* stat = memoryMap[0xFF41];
    uint8_t* if_reg = memoryMap[0xFF0F];

    uint8_t equal = (ly == lyc);

    if (equal) {
        *stat |= (1 << 2); // Set coincidence flag

        if (!wasEqual && (*stat & (1 << 6))) {
            *if_reg |= 0x02; // Request STAT interrupt (bit 1)
            printf("LYC STAT interrupt requested at line %d, LYC: %d, STAT: 0x%02X, if_reg: 0x%02X\n", ly, lyc, *stat, *if_reg);
        }
    } else {
        *stat &= ~(1 << 2); // Clear coincidence flag
    }

    wasEqual = equal;
}

int div_counter = 0;
int tima_counter = 0;

int windowOnLine = 0; //flag so internal window line counter doesn't increment more than once per line


int main(){
    SDL_Init(SDL_INIT_VIDEO);
    if (TTF_Init() < 0) {
    printf("Failed to initialize SDL_ttf: %s\n", TTF_GetError());
    return 1;
}

    logFile = fopen("emu_log.txt", "w");
    if (!logFile) {
        perror("Failed to open log file");
        exit(1);
    }

    SDL_Window *window = SDL_CreateWindow("GB-EMU", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 720, 0); //window width and height 160x144
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED); //default driver gpu accelerated if possible
    memset(display, 0, sizeof(display));
    
    /*
    SDL_Window *tileWindow = SDL_CreateWindow(
    "Tile Viewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
    128, 192, 0); // 16 tiles * 8 = 128, 24 tiles * 8 = 192

    SDL_Renderer *tileRenderer = SDL_CreateRenderer(tileWindow, -1, SDL_RENDERER_ACCELERATED);
    */

    /*

SDL_Window *debugWindow = SDL_CreateWindow(
    "Debug Panel",
    SDL_WINDOWPOS_CENTERED + 400, SDL_WINDOWPOS_CENTERED,
    300, 500,  // Width, Height
    SDL_WINDOW_SHOWN
);

SDL_Renderer *debugRenderer = SDL_CreateRenderer(debugWindow, -1, SDL_RENDERER_ACCELERATED);

// Load font
TTF_Font *debugFont = TTF_OpenFont("arial.ttf", 14);
if (!debugFont) {
    printf("Failed to load font: %s\n", TTF_GetError());
    return 1;
}

SDL_Renderer *tileRenderer = SDL_CreateRenderer(tileWindow, -1, SDL_RENDERER_ACCELERATED);
*/

    loadrom("Tetris.gb"); //initialises  memory and CPU registers in this function as well
    printromHeader();
    printf("Press Enter to start...\n");
    getchar();

    PPU ppu = { //initialise ppu
    .mode = memoryMap[0xFF41], // Mode bits are in STAT
    .ly = memoryMap[0xFF44],   // LY
    .lyc = memoryMap[0xFF45],  // LYC
};


    

    Sprite spriteBuffer[10]; 

    cyclesAccumulated = 0;
    uint16_t haltPC; //for halt bug

    int open = 1;
    SDL_Event event;
    int overflowFlag = -1;
    uint8_t serialByte;
    int serialCounter = 0;
    int serialInProgress = 0; // Flag to indicate if serial transfer is in progress
    int spriteCount = 0;

    
    //Uint32 lastTick = SDL_GetTicks(); //timing



    while (open) {
       /* Uint32 now = SDL_GetTicks();
        //if (now - lastTick < 1000) { //timing
          SDL_Delay(1); // Sleep a bit to avoid busy-waiting
            continue;
        }
        lastTick = now;
        */

        
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
        


    if (!isPaused || stepMode) { //main loop in here to allow pause/step


        if(CBFlag != 1){ //make sure interrupt not called before CB instruction finishes execution
            handleInterrupts();
        }
        

        //uint16_t oldSP = CPUreg.SP;

            // --- HALT and HALT bug handling ---
        
        uint8_t opcode = *memoryMap[CPUreg.PC];


        //uint8_t prevFFCC = *memoryMap[0xFFCC]; // Save previous value of FF E1

        if (CPUtimer > 0) {
            CPUtimer--;
        }
        if (CPUtimer == 0){
            //fprintf(logFile, "PC: %04X, Opcode: %02X, Cycles: %d, SP: %04X, A: %02X, B: %02X, C: %02X, D: %02X, E: %02X, F: %02X, H: %02X, L: %02X, LY: %02X, LYC: %02X, FF80: %02X, FF85: %02X, FF00: %02X, FFFF: %02X, FF0F: %02X, FF40: %02X, FF41: %02X, FF44: %02X, FF45: %02X\n", CPUreg.PC, opcode, realCyclesAccumulated, CPUreg.SP, CPUreg.af.A, CPUreg.bc.B, CPUreg.bc.C, CPUreg.de.D, CPUreg.de.E, CPUreg.af.F, CPUreg.hl.H, CPUreg.hl.L, *(ppu.ly), *(ppu.lyc), *memoryMap[0xFF80], *memoryMap[0xFF85], *memoryMap[0xFF00], *memoryMap[0xFFFF], *memoryMap[0xFF0F], *memoryMap[0xFF40], *memoryMap[0xFF41], *memoryMap[0xFF44], *memoryMap[0xFF45]);
            //fprintf(logFile, "Mode 1 Timer: %d, Mode 0 Timer: %d, Mode 2 Timer: %d, Mode 3 Timer: %d, xPos: %d, scanlineTimer: %d\n", mode1Timer, mode0Timer, mode2Timer, mode3Timer, xPos, scanlineTimer);
            //halt mode 1 handled in interrupt handler
            //fprintf(logFile, "Haltmode %d, CPUtimer: %d\n", haltMode, CPUtimer);

        if(haltMode != 1){ //execute CPU only if haltMode is not 1 (CPU not halted)

        if (haltMode == 2) {
            // HALT bug: execute same instruction twice
            //pcBacktrace[backtraceIndex] = CPUreg.PC - 1;
           // backtraceIndex = (backtraceIndex + 1) % 8;
            if (EIFlag == 1) EIFlag = -1; //after EI flag set to -1 so interrupt enabled after 1 instruction delay
            else if (EIFlag == -1 && CBFlag == 0) { 
                CPUreg.IME = 1;
                EIFlag = 0;
            }

            CPUreg.PC--; //set PC back by one so byte is read twice
            executeOpcode(opcode);

            CPUtimer = cyclesAccumulated;
            haltMode = 0;
            cyclesAccumulated = 0;
        } else {
            // --- Normal CPU execution ---
           // pcBacktrace[backtraceIndex] = CPUreg.PC - 1;
            //backtraceIndex = (backtraceIndex + 1) % 8;
            haltPC = CPUreg.PC; // Store PC for halt bug
                if (CBFlag == 0) {
                    if (EIFlag == 1) EIFlag = -1;
                    else if (EIFlag == -1 && CBFlag == 0) {
                        CPUreg.IME = 1;
                        EIFlag = 0;
                    }
                    //printf("Executing opcode %02X at PC=%04X\n", opcode, CPUreg.PC);
                    executeOpcode(opcode);
                    CPUtimer = cyclesAccumulated;
                } else {
                    if (EIFlag == -1){
                        CPUreg.IME = 1;
                        EIFlag = 0;
                    } 
                    executeOpcodeCB(opcode);
                    //printf("Executing opcode %02X at PC=%04X\n", opcode, CPUreg.PC);
                    CPUtimer = cyclesAccumulated;
                    CBFlag = 0;
                }

                cyclesAccumulated = 0;
                
            }
        }
    }

   

    /*

        if (CPUreg.SP != oldSP && debug == 1) {
            printf("[SP CHANGE] from %04X → %04X at PC=%04X\n", oldSP, CPUreg.SP, CPUreg.PC);
            oldSP = CPUreg.SP;
        }

    uint8_t afterFFCC = *memoryMap[0xFFCC];
    if (afterFFCC != prevFFCC) {
        if (1) {
            printf("[FF CC CHANGE] from %02X → %02X at PC=%04X, OPCode: %02X\n", prevFFCC, afterFFCC, CPUreg.PC, opcode);
        }
    }
        */

    //PPU 
    if (LCDdelayflag == 0 && LCDdisabled == 1) { 
        *memoryMap[0xFF41] = (*memoryMap[0xFF41] & ~0x03) | 0x02; // Set mode to 2 (OAM)
        LCDdisabled = 0; // Reset LCD disabled flag
        //fprintf(logFile, "LCDC enabled, cycle: %ld\n", realCyclesAccumulated);
    }
        
    switch(*ppu.mode & 0x03){ //get which mode the PPU is currently in
        case(0): //HBlank
            mode0Timer = 456 - scanlineTimer; //HBlank lasts 456 cycles, subtract the cycles already used in this scanline
            //printf("Mode 0: HBlank, Timer: %d\n", mode0Timer);
            if (mode0Timer == 0) { //if timer is 0, then
                if (*ppu.ly == 143) { //Start VBlank 
                    (*ppu.ly)++;
                    checkLYC(memoryMap);
                    drawDisplay(renderer); //draw display to window 
                    //drawTileViewer(tileRenderer);
                    SDL_RenderPresent(renderer); //update window with everything drawn
                   // drawDebugWindow(debugRenderer, debugFont); //draw debug window

                    *memoryMap[0xFF0F] |= 0x01; // Set VBlank flag in IF register

                    //printf("VBlank request at cycle %d\n", realCyclesAccumulated);
                    if (debug == 1) {
                        //printf("VBlank started at cycle %d\n", realCyclesAccumulated);
                    }

                    // Enter VBlank mode
                    *ppu.mode = (*ppu.mode & ~0x03) | 0x01;
                    //*ppu.mode = (*ppu.mode & 0xFC) | 0x01; // Mode 1
                    if (*memoryMap[0xFF41] & 0x10) { // Bit 4: VBlank STAT interrupt enable
                        *memoryMap[0xFF0F] |= 0x02;  // STAT interrupt request flag
                    }

                    mode1Timer = 456; // VBlank lasts 10 lines of 456 cycles each
                    scanlineTimer = 0; //reset scanline timer
                }
                else{   
                    (*ppu.ly)++;
                    checkLYC(memoryMap); //increment LY and check LYC register for coincidence with LY
                    //*ppu.mode = (*ppu.mode & 0xFC) | (2 & 0x03); //set mode to OAM scan
                    *ppu.mode = (*ppu.mode & ~0x03) | 0x02; // Set to Mode 2 (OAM scan)
                    if (*memoryMap[0xFF41] & 0x20) { // Bit 5: OAM STAT interrupt enable
                        *memoryMap[0xFF0F] |= 0x02;  // STAT interrupt request flag
                    }
                    scanlineTimer = 0; //reset scanline timer
                }
            windowOnLine = 0; //reset internal window line counter flag for that scanline, so doesn't increment multiple time per line
            }
            break;

        case(1): // VBlank
            if(*ppu.ly != 153 && *ppu.ly != 0){
                if(mode1Timer == 0){
                    (*ppu.ly)++; // increment LY 
                    //printf("Scanline %d\n", *ppu.ly);
                    checkLYC(memoryMap);

                    mode1Timer = 456; // VBlank lasts 10 lines of 456 cycles each
                    scanlineTimer = 0; //reset scanline timer
                }
                
                else { //if LY is 153, then we are in VBlank mode
                    mode1Timer-- ; //decrement mode1Timer until it reaches 0
                }
            }
            else{ //LY is 153, last VBLANK line
                if(mode1Timer == 448){ 
                    *ppu.ly = 0; //line 153 quirk, after 8 cycles ly is set to 0, then continue rest of cycles to end of vblank
                    checkLYC(memoryMap);
                    mode1Timer--; //decrement mode1Timer until it reaches 0
                }
                else if(mode1Timer == 0){ 
                    scanlineTimer = 0; //reset scanline timer
                    //*ppu.mode = (*ppu.mode & 0xFC) | (2 & 0x03); // set mode to OAM scan
                    *ppu.mode = (*ppu.mode & ~0x03) | 0x02; // Set to Mode 2 (OAM scan)
                    mode1Timer = 456; // reset timer for next VBlank
                    windowLine = 0; // reset internal window line counter
                }

                else {
                    mode1Timer--; //decrement mode1Timer until it reaches 0
                }
            }
            break;

        //OAM scan, wait 80 cycles then load the spriteBuffer and change to mode 3

        case(2):  // OAM Scan
            if(mode2Timer != 80){
                mode2Timer++; //wait 80 cycles before switching modes
            }
            else if(mode2Timer == 80){
                spriteCount = spriteSearchOAM(*memoryMap[0xFF44], spriteBuffer); //store sprites in sprite buffer and no. of sprites
                mode2Timer = 0; //reset timer for next mode 2 check
                //*memoryMap[0xFF41] = (*memoryMap[0xFF41] & 0xFC) | (3 & 0x03); //set to mode 3
                *memoryMap[0xFF41] = (*memoryMap[0xFF41] & ~0x03) | 0x03;
            }
            break;

        //Mode 3 fetching and pushing queues, change to HBlank after scanline done, Vblank when all scanlines done

        case(3):  // BG/Window fetcher (with sprite mix)
            uint8_t lcdc = *memoryMap[0xFF40];
            uint8_t wx   = *memoryMap[0xFF4B];
            uint8_t wy   = *memoryMap[0xFF4A];
            uint8_t scx  = *memoryMap[0xFF43];

            int windowEnabled = (lcdc & 0x20) != 0;
            int windowStartX  = (int)wx - 7;

            // Window is *actually* visible at this pixel?
            int windowVisibleNow =
                windowEnabled &&
                (*ppu.ly >= wy) &&          // window starts at WY and continues downward
                (xPos >= windowStartX);     // and only after WX-7 horizontally

            // One-time switch from BG -> Window when it first becomes visible this scanline
            if (!fetchStage.windowFetchMode && windowVisibleNow) {
                // flush any queued BG pixels so the window starts cleanly
                for (int i = 0; i < 8; i++) {
                    Pixel tmp;
                    dequeue(&BGFifo, &tmp);
                }
                fetchStage.BGFetchStage    = 0;
                fetchStage.windowFetchMode = 1;
                mode3Timer = 2;
            }

            // Advance the correct pipeline when ready
            if (fetchStage.windowFetchMode) {
                // If window got disabled mid-line, drop back to BG cleanly
                if (!windowEnabled) {
                    for (int i = 0; i < 8; i++) {
                        Pixel tmp;
                        dequeue(&BGFifo, &tmp);
                    }
                    fetchStage.BGFetchStage    = 0;
                    fetchStage.windowFetchMode = 0;
                    mode3Timer = 2;
                } else if (mode3Timer == 0) {
                    // Window pipeline stages
                    if      (fetchStage.BGFetchStage == 0) { fetchStage.BGFetchStage = 1; mode3Timer = 2; }
                    else if (fetchStage.BGFetchStage == 1) { fetchStage.BGFetchStage = 2; mode3Timer = 2; }
                    else if (fetchStage.BGFetchStage == 2) { fetchStage.BGFetchStage = 3; mode3Timer = 2; }
                    else /* BGFetchStage == 3 */ {
                        // xPos - (wx-7) handled inside pixelPush for window mode
                        pixelPushBG(&BGFifo, xPos, &fetchStage, /*windowMode=*/1);
                        mode3Timer = 2;
                    }
                }
            } else {
                // Background pipeline (runs even if windowEnabled=1 but not yet visible)
                if (mode3Timer == 0) {
                    if      (fetchStage.BGFetchStage == 0) { fetchStage.BGFetchStage = 1; mode3Timer = 2; }
                    else if (fetchStage.BGFetchStage == 1) { fetchStage.BGFetchStage = 2; mode3Timer = 2; }
                    else if (fetchStage.BGFetchStage == 2) { fetchStage.BGFetchStage = 3; mode3Timer = 2; }
                    else /* BGFetchStage == 3 */ {
                        // BG push (SCX handled by discarding below)
                        pixelPushBG(&BGFifo, xPos, &fetchStage, /*windowMode=*/0);
                        mode3Timer = 2;
                    }
                }
            }

            // Set discard count once per new scanline
            //Need to add 8 pixel discard
            if (xPos == 0 && newScanLine) {
                scxCounter = scx & 7; //lower 3 bits of scx for fine scroll, and with 0b0111
                newScanLine = 0;
            }

            // FIFO -> screen (sprite mixing preserved)
            if (BGFifo.count > 0) {
                if (scxCounter > 0) {
                    Pixel discard;
                    dequeue(&BGFifo, &discard);
                    scxCounter--;
                } else {
                    Pixel pixelToPush;
                    dequeue(&BGFifo, &pixelToPush);

                    int y = *ppu.ly;

                    uint8_t finalColour = 0;  
                    if (lcdc & 0x01) { //if BG/Window enable bit is 0 then send pixel of colour 0
                        finalColour = pixelToPush.colour;
                    }

                    // --- sprite mixing (unchanged logic) ---
                    if (lcdc & 0x02) { //if sprite bit is enabled
                        for (int i = 0; i < spriteCount; i++) {
                            Sprite *spr = &spriteBuffer[i];
                            if (spr->xPos == 0 || spr->xPos >= 168) continue;

                            int spriteX = spr->xPos - 8;
                            int spriteY = spr->yPos - 16;
                            int spriteHeight = (*memoryMap[0xFF40] & 0x04) ? 16 : 8;

                            if (xPos >= spriteX && xPos < spriteX + 8 &&
                                y   >= spriteY && y   < spriteY + spriteHeight) {

                                int tileLine = y - spriteY;
                                if (spr->flags & 0x40) tileLine = spriteHeight - 1 - tileLine; // Y flip

                                uint16_t tileNum = spr->tileNum;
                                if (spriteHeight == 16) tileNum &= 0xFE;

                                uint16_t tileAddr = 0x8000 + tileNum * 16 + tileLine * 2;
                                uint8_t byte1 = *memoryMap[tileAddr];
                                uint8_t byte2 = *memoryMap[tileAddr + 1];

                                int bit = (spr->flags & 0x20) ? (xPos - spriteX) : (7 - (xPos - spriteX)); // X flip
                                int colorId = ((byte2 >> bit) & 1) << 1 | ((byte1 >> bit) & 1);
                                if (colorId != 0) {
                                    uint8_t palette = (spr->flags & 0x10) ? *memoryMap[0xFF49] : *memoryMap[0xFF48];
                                    uint8_t spriteColour = (palette >> (colorId * 2)) & 0x03;
                                    if (pixelToPush.colour == 0 || !(spr->flags & 0x80)) {
                                        finalColour = spriteColour;
                                    }
                                    break;
                                }
                            }
                        }
                    }

                    if (y < 144) display[xPos][y] = finalColour;
                    xPos++;
                }
            }

            // End of visible scanline -> HBlank
            if (xPos >= 160) {
                *ppu.mode = (*ppu.mode & ~0x03) | 0x00; // Mode 0 (HBlank)
                if (*memoryMap[0xFF41] & 0x08) *memoryMap[0xFF0F] |= 0x02; // STAT HBlank
                xPos = 0;
                while (BGFifo.count > 0) { //clear FIFO for next scanline
                    Pixel tmp; 
                    dequeue(&BGFifo, &tmp);
                }
                fetchStage.BGFetchStage = 0;
                newScanLine = 1;
                fetchStage.windowFetchMode = 0; //reset window fetch mode for next scanline
                // LY advance handled in HBlank
                if (windowVisibleNow && windowOnLine == 0) { //make sure windowLine only increments once per scanline if window is on it
                    windowLine++;
                    windowOnLine = 1;
                }
            } 

            
            break;
        }
                

    //SDL_Delay(1000/63); //fps
    if (LCDdisabled == 0) { //if LCD is enabled, then continue with PPU
        mode3Timer--; //decrement mode3Timer for next loop, if 0 then fetch next pixel
        scanlineTimer += 1; //increment scanline timer for each loop
        if (mode3Timer <= 0) {
            mode3Timer = 0; //reset mode3Timer for next loop
        }
    }
    realCyclesAccumulated += 1; //increment real cycles accumulated for each loop
    LCDdelayflag--;
    checkLYC(memoryMap); // Check LYC register for coincidence with LY regardless of LCD state
//fprintf(logFile, "xPos: %02X, LY: %02X, scx: %02X,BGFetchStage: %d, windowFetchMode %d, wy %02X, wx %02X, LCDC: %02X, BGFifoCount: %d \n  ", xPos, *memoryMap[0xFF44], *memoryMap[0xFF43], fetchStage.BGFetchStage, fetchStage.windowFetchMode, *memoryMap[0xFF4A], *memoryMap[0xFF4B], *memoryMap[0xFF40], BGFifo.count);

    //Serial communication

if (!serialInProgress && (*memoryMap[0xFF02] & 0x80)) {
    // Start transfer
    serialInProgress = 1;
    serialCounter = 1024;
    serialByte = *memoryMap[0xFF01];
}

if (serialInProgress) {
    serialCounter--;
    //*memoryMap[0xFF02] |= 0x80; // Set SC bit to indicate transfer in progress
    if (serialCounter == 0) {
        // Transfer complete
        *memoryMap[0xFF01] = 0xFF;   // echo back
        *memoryMap[0xFF02] &= ~0x80;       // clear SC
        //*memoryMap[0xFF0F] |= 0x08;        // request serial interrupt
        //don't request interrupt since no link cable support yet in this emulator
        printf("%c", serialByte);          // optional
        fflush(stdout);
        serialInProgress = 0;
    }
}

//fprintf(logFile,"FF02: %02X, FF01: %02X, Serial In Progress: %d\n", *memoryMap[0xFF02], *memoryMap[0xFF01], serialInProgress);

    /*
    if (*memoryMap[0xFF02] & 0x80) { // Transfer requested
        // Print the byte being "sent" over serial
        printf("%c", *memoryMap[0xFF01]);
        fflush(stdout); // Ensure immediate output

        // Simulate transfer: echo back the same byte
        *memoryMap[0xFF01] = *memoryMap[0xFF01];
        *memoryMap[0xFF02] &= ~0x80; // Clear transfer bit
        *memoryMap[0xFF0F] |= 0x08;  // Request serial interrupt
    }
        */

    // Timer handling

    /*
    div_counter++;
    if (div_counter >= 256) { // DIV increments every 256 cycles
        (*memoryMap[0xFF04])++;
        div_counter = 0;
    }

    uint8_t tac = *memoryMap[0xFF07];
    if (tac & 0x04) { // Timer enabled
        int freq;
        switch (tac & 0x03) {
            case 0: freq = 1024; break; // 4096 Hz
            case 1: freq = 16;   break; // 262144 Hz
            case 2: freq = 64;   break; // 65536 Hz
            case 3: freq = 256;  break; // 16384 Hz
        }
        tima_counter++;
        if (tima_counter >= freq) {
            tima_counter = 0;
            if (++(*memoryMap[0xFF05]) == 0) { // TIMA overflow
                overflowFlag = 4;
            }
        }
    }

    if (overflowFlag == 0) {
        *memoryMap[0xFF05] = *memoryMap[0xFF06]; // Load TMA
        *memoryMap[0xFF0F] |= 0x04; // Request timer interrupt
        overflowFlag = -1; //reset flag to cause 1 cycle delay on overflow
    }
    else if (overflowFlag > 0) {
        overflowFlag--;
    }
        */

    // 16-bit internal DIV counter, old timer based counter inaccurate due to DIV counter value not starting at 0 when changing frequency
    //so changed to rising edge detection of DIV bits like actual hardware
    
    static uint16_t divInternal = 0;
    static uint16_t prev_div = 0;

    divInternal++; // increment by 1 CPU cycle
    *memoryMap[0xFF04] = divInternal >> 8; // upper 8 bits visible as DIV

    uint8_t tac = *memoryMap[0xFF07];
    if (tac & 0x04) { // Timer enabled
        // Which DIV bit triggers TIMA, not actually done by cycles elapsed but monitoring DIV bits
        const uint8_t timerBit[4] = {9, 3, 5, 7};
        uint16_t mask = 1 << timerBit[tac & 0x03];

        // Check for rising edge of the relevant DIV bit
        if ((prev_div & mask) != 0 && (divInternal & mask) == 0) { //check if relevant bit changed from 1 to 0 (falling edge)
            if (++(*memoryMap[0xFF05]) == 0) { // TIMA overflow
                overflowFlag = 4; // 4 CPU cycles delay for TIMA reload
            }
        }
    }
    prev_div = divInternal;

    // Handle TIMA reload and interrupt
    if (overflowFlag > 0) {
        overflowFlag--; 
        *memoryMap[0xFF05] = 0; // keep TIMA at 0 for 4 cycle period
        if (overflowFlag == 0) {
            *memoryMap[0xFF05] = *memoryMap[0xFF06]; // Reload TIMA from TMA
            *memoryMap[0xFF0F] |= 0x04;              // Request timer interrupt
           // printf("Timer interrrupt requested\n");
        }
    }

    if (DMAcycles == 0 && DMAFlag == 1) { // If DMA transfer is completed
        DMAFlag = 0; // Reset DMA flag
    }
    else if (DMAcycles > 0) { // If DMA transfer is in progress
        DMAcycles--;
    }
    if (stepMode) stepMode = 0; // Reset after single step, end of pause loop

    //fprintf(logFile,"[PPU DEBUG] realCycles=%d | LY=%d | STAT=0x%02X\n",realCyclesAccumulated, *memoryMap[0xFF44], *memoryMap[0xFF41]);
    
}
    }
    

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
                                  
}
