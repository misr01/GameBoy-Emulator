#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> //for unsignted ints
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> //for graphics and input of the game

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
        uint8_t C;
        uint8_t B;
    };
    uint16_t BC;
} RegBC;

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
uint8_t eram[0x2000] = {0};        // 8KB External RAM
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
    *memoryMap[0xFF04] = 0x18; // set timer counter to 0x18
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
    *memoryMap[0xFF41] = 0x80 | 0x02; // STAT: Mode 2 + LY=LYC interrupt off
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

uint8_t readByte(uint16_t addr) { //address bus 16 bits, data bus 8 bits
    return *memoryMap[addr];
}
void writeByte(uint16_t addr, uint8_t value) {
    *memoryMap[addr] = value;
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


void LDVal8(uint8_t value, uint8_t *dest) { //load 8-bit value into destination register
    *dest = value;
    //cyclesAccumulated += 4; // 4 T cycles for 8-bit load
    //fprintf(logFile, "[LDVal8] Loaded 0x%02X into %p (PC=%04X) Cycles=%d\n", value, dest, CPUreg.PC, cyclesAccumulated);
}

void LDVal16(uint16_t value, uint16_t *dest) { //load 16-bit value into destination register
    *dest = value;
    //cyclesAccumulated += 8; // 8 T cycles for 16-bit load
    //fprintf(logFile, "[LDVal16] Loaded 0x%04X into %p (PC=%04X) Cycles=%d\n", value, dest, CPUreg.PC, cyclesAccumulated);
}

uint8_t RDVal8(uint8_t *dest) { //read 8-bit value from destination register
    //cyclesAccumulated += 4; // 4 T cycles for 8-bit read
    return *dest;
}

uint16_t RDVal16(uint16_t *dest) { //read 16-bit value from destination register
    //cyclesAccumulated += 8; // 8 T cycles for 16-bit read
    return *dest;
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
    } else {
        // Normal halt
        haltMode = 1;
    }
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
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
void op_0x30(){ //jump with relative offset if carry flag is not set
    int carryFlag = CPUreg.af.F & 0x10; // check if carry flag is set
    if (carryFlag == 0) {
        CPUreg.PC += 2 + ((int8_t)*memoryMap[CPUreg.PC + 1]); //signed integer added to PC 12T
        cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2; //increment PC by 2 if carry flag is set
        cyclesAccumulated += 8;
    }
}

void op_0x28(){ //jump with relative offset if zero flag is set
    int zeroFlag = CPUreg.af.F & 0x80; // check if zero flag is set
    //fprintf(logFile,"[DEBUG] JR Z: ZeroFlag=%d, PC=%04X\n", zeroFlag != 0, CPUreg.PC);
    if (zeroFlag != 0) {
        CPUreg.PC += 2 + ((int8_t)*memoryMap[CPUreg.PC + 1]); //signed integer added to PC 12T
        cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2; //increment PC by 2 if zero flag is not set
        cyclesAccumulated += 8;
    }
}

void op_0x38(){ //jump with relative offset if carry flag is set
    int carryFlag = CPUreg.af.F & 0x10; // check if carry flag is set
    if (carryFlag != 0) {
        CPUreg.PC += 2 + ((int8_t)*memoryMap[CPUreg.PC + 1]); //signed integer added to PC 12T
        cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2; //increment PC by 2 if carry flag is not set
        cyclesAccumulated += 8;
    }
}

void op_0x18(){ //jump with relative offset 12T 2PC
    CPUreg.PC += 2 + ((int8_t)*memoryMap[CPUreg.PC + 1]); //signed integer added to PC
    cyclesAccumulated += 12;
}

void op_0xE9(){ //jump to address in HL register 4T 1PC
    CPUreg.PC = CPUreg.hl.HL; //set PC to HL value
    cyclesAccumulated += 4;
}   

//Orange call instructions

void op_0xC4(){ //call subroutine if zero flag not set 24T 3PC
    int zeroFlag = CPUreg.af.F & 0x80; // check if zero flag is set
    if (zeroFlag == 0) {
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8; //push high byte of PC onto stack
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF; //push low byte of PC onto stack
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; //set PC to address in memory
        cyclesAccumulated += 24;
    } else {
        CPUreg.PC += 3; //increment PC by 3 if zero flag is set
        cyclesAccumulated += 12;
    }
    logStackTop(); 
}

void op_0xCC(){ //call subroutine if zero flag is set 24T 3PC
    int zeroFlag = CPUreg.af.F & 0x80; // check if zero flag is set
    if (zeroFlag != 0) {
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8; //push high byte of PC onto stack
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF; //push low byte of PC onto stack
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; //set PC to address in memory
        cyclesAccumulated += 24;
    } else {
        CPUreg.PC += 3; //increment PC by 3 if zero flag is not set
        cyclesAccumulated += 12;
    }
    logStackTop();
}

void op_0xD4(){ //call subroutine if carry flag not set 24T 3PC
    int carryFlag = CPUreg.af.F & 0x10; // check if carry flag is set
    if (carryFlag == 0) {
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8; //push high byte of PC onto stack
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF; //push low byte of PC onto stack
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; //set PC to address in memory
        cyclesAccumulated += 24;
    } else {
        CPUreg.PC += 3; //increment PC by 3 if carry flag is set
        cyclesAccumulated += 12;
    }
    logStackTop();
}

void op_0xDC(){ //call subroutine if carry flag is set 24T 3PC
    int carryFlag = CPUreg.af.F & 0x10; // check if carry flag is set
    if (carryFlag != 0) {
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8; //push high byte of PC onto stack
        *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF; //push low byte of PC onto stack
        CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; //set PC to address in memory
        cyclesAccumulated += 24;
    } else {
        CPUreg.PC += 3; //increment PC by 3 if carry flag is not set
        cyclesAccumulated += 12;
    }
    logStackTop();
}

void op_0xCD(){ //call subroutine 24T 3PC
    *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8; //push high byte of PC onto stack
    *memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF; //push low byte of PC onto stack
    CPUreg.PC = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1]; //set PC to address in memory
    cyclesAccumulated += 24;
    logStackTop();
}

//Orange return instructions

void op_0xC9(){ //return from subroutine 16T 1PC
    CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
    CPUreg.SP += 2; //increment stack pointer by 2
    cyclesAccumulated += 16;
    logStackTop();
}

int realCyclesAccumulated = 0; //reset cycles accumulated
void op_0xD9(){ //return from interrupt 16T 1PC
    CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
    CPUreg.SP += 2; //increment stack pointer by 2
    CPUreg.IME = 1; //enable interrupts
    cyclesAccumulated += 16;    
    logStackTop();
    //printf("RETI: Cycles accumulated: %lu\n", realCyclesAccumulated);
    //fprintf(logFile, "[INTERRUPT Finish] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) Cycles=%d\n", *memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC, realCyclesAccumulated);
    //getchar(); // wait for user input to continue
}


void op_0xC0(){ //return if zero flag not set 20T 2PC
    int zeroFlag = getZeroFlag(); // check if zero flag is set
    if (zeroFlag == 0) {
        CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
        CPUreg.SP += 2; //increment stack pointer by 2
        cyclesAccumulated += 20;
    } else {
        CPUreg.PC += 1; //increment PC by 1 if zero flag is set
        cyclesAccumulated += 8;
    }
    logStackTop();
}

void op_0xD0(){ //return if carry flag not set 20T 2PC
    int carryFlag = getCarryFlag(); 
    if (carryFlag == 0) {
        CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
        CPUreg.SP += 2; //increment stack pointer by 2
        cyclesAccumulated += 20;
    } else {
        CPUreg.PC += 1; //increment PC by 1 if carry flag is set
        cyclesAccumulated += 8;
    }
    logStackTop();
}

void op_0xD8(){ //return if carry flag is set 20T 2PC
    int carryFlag = getCarryFlag(); // check if carry flag is set
    if (carryFlag != 0) {
        CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
        CPUreg.SP += 2; //increment stack pointer by 2
        cyclesAccumulated += 20;
    } else {
        CPUreg.PC += 1; //increment PC by 1 if carry flag is not set
        cyclesAccumulated += 8;
    }
    logStackTop();
}

void op_0xC8(){ //return if zero flag is set 20T 2PC
    int zeroFlag = getZeroFlag(); // check if zero flag is set
    if (zeroFlag != 0) {
        CPUreg.PC = (*memoryMap[CPUreg.SP + 1] << 8) | *memoryMap[CPUreg.SP]; //set PC to value on stack
        CPUreg.SP += 2; //increment stack pointer by 2
        cyclesAccumulated += 20;
    } else {
        CPUreg.PC += 1; //increment PC by 1 if zero flag is not set
        cyclesAccumulated += 8;
    }
    logStackTop();
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
void op_0x01(){ //read 2 bytes from memory and set BC to that value 12T 3PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.bc.C); //read low byte from memory 
    LDVal8(*memoryMap[CPUreg.PC + 2], &CPUreg.bc.B); //read high byte from memory
    CPUreg.PC += 3; 
    cyclesAccumulated += 12;
}

void op_0x11(){ //read 2 bytes from memory and set DE to that value 12T 3PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.de.E); //read low byte from memory 
    LDVal8(*memoryMap[CPUreg.PC + 2], &CPUreg.de.D); //read high byte from memory
    CPUreg.PC += 3; 
    cyclesAccumulated += 12;
}

void op_0x21(){ //read 2 bytes from memory and set HL to that value 12T 3PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.hl.L); //read low byte from memory 
    LDVal8(*memoryMap[CPUreg.PC + 2], &CPUreg.hl.H); //read high byte from memory
    CPUreg.PC += 3; 
    cyclesAccumulated += 12;
}

void op_0x31(){ //read 2 bytes from memory and set SP to that value 12T 3PC
    uint8_t low  = *memoryMap[CPUreg.PC + 1];
    uint16_t high = *memoryMap[CPUreg.PC + 2];
    uint16_t value = (high << 8) | low; //shift high and combine with low byte
    LDVal16(value, &CPUreg.SP);
    CPUreg.PC += 3; 
    cyclesAccumulated += 12;
}

//Green SP instructions

void op_0x08(){ //write SP to address in memory 20T 3PC
    uint16_t addr = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1];
    LDVal8(CPUreg.SP & 0xFF, memoryMap[addr]);
    LDVal8((CPUreg.SP >> 8) & 0xFF, memoryMap[addr + 1]);
    CPUreg.PC += 3; 
    cyclesAccumulated += 20;
    logStackTop();
}

void op_0xC1(){ //pop value from stack into BC 12T 3PC
    CPUreg.bc.C = *memoryMap[CPUreg.SP]; //read low byte from stack
    CPUreg.bc.B = *memoryMap[CPUreg.SP + 1]; //read high byte from stack
    CPUreg.SP += 2; //increment stack pointer by 2
    CPUreg.PC += 1;     
    cyclesAccumulated += 12;
    logStackTop(); //log stack top after pop
}

void op_0xD1(){ //pop value from stack into DE 12T 3PC
    CPUreg.de.E = *memoryMap[CPUreg.SP]; //read low byte from stack
    CPUreg.de.D = *memoryMap[CPUreg.SP + 1]; //read high byte from stack
    CPUreg.SP += 2; //increment stack pointer by 2
    CPUreg.PC += 1; 
    cyclesAccumulated += 12;
    logStackTop(); //log stack top after pop
}

void op_0xE1(){ //pop value from stack into HL 12T 3PC
    CPUreg.hl.L = *memoryMap[CPUreg.SP]; //read low byte from stack
    CPUreg.hl.H = *memoryMap[CPUreg.SP + 1]; //read high byte from stack
    CPUreg.SP += 2; //increment stack pointer by 2
    CPUreg.PC += 1; 
    cyclesAccumulated += 12;
    logStackTop(); //log stack top after pop
}

void op_0xF1(){ //pop value from stack into AF 12T 3PC lower 4 bits of F are not used so 0
    CPUreg.af.F = *memoryMap[CPUreg.SP] & 0xF0; //read low byte from stack and keep only upper 4 bits
    CPUreg.af.A = *memoryMap[CPUreg.SP + 1]; //read high byte from stack
    CPUreg.SP += 2; //increment stack pointer by 2
    CPUreg.PC += 1; 
    cyclesAccumulated += 12;
    logStackTop(); //log stack top after pop
}

void op_0xC5(){ //push BC onto stack 16T 3PC
    *memoryMap[--CPUreg.SP] = CPUreg.bc.B; //decrement stack pointer then write high byte to stack
    *memoryMap[--CPUreg.SP] = CPUreg.bc.C; //decrement stack pointer then write low byte to stack
    CPUreg.PC += 1; 
    cyclesAccumulated += 16;
    logStackTop(); //log stack top after push
}

void op_0xD5(){ //push DE onto stack 16T 3PC
    *memoryMap[--CPUreg.SP] = CPUreg.de.D; //decrement stack pointer then write high byte to stack
    *memoryMap[--CPUreg.SP] = CPUreg.de.E; //decrement stack pointer then write low byte to stack
    CPUreg.PC += 1; 
    cyclesAccumulated += 16;
    logStackTop(); //log stack top after push
}

void op_0xE5(){ //push HL onto stack 16T 3PC
    *memoryMap[--CPUreg.SP] = CPUreg.hl.H; //decrement stack pointer then write high byte to stack
    *memoryMap[--CPUreg.SP] = CPUreg.hl.L; //decrement stack pointer then write low byte to stack
    CPUreg.PC += 1; 
    cyclesAccumulated += 16;
    logStackTop(); //log stack top after push
}

void op_0xF5(){ //push AF onto stack 16T 3PC lower 4 bits of F are not used so 0
    *memoryMap[--CPUreg.SP] = CPUreg.af.A; //decrement stack pointer then write high byte to stack
    *memoryMap[--CPUreg.SP] = CPUreg.af.F & 0xF0; //decrement stack pointer then write low byte to stack (upper 4 bits only)
    CPUreg.PC += 1; 
    cyclesAccumulated += 16;
    logStackTop(); //log stack top after push
}

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

//Blue LD instructions (8 bit register into memory address in 16 bit register) without register increment
void op_0x70(){ //load value from register B to address at HL 8T 1PC
    LDVal8(CPUreg.bc.B, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x71(){ //load value from register C to address at HL 8T 1PC
    LDVal8(CPUreg.bc.C, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
} 

void op_0x72(){ //load value from register D to address at HL 8T 1PC
    LDVal8(CPUreg.de.D, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x73(){ //load value from register E to address at HL 8T 1PC
    LDVal8(CPUreg.de.E, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x74(){ //load value from register H to address at HL 8T 1PC
    LDVal8(CPUreg.hl.H, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x75(){ //load value from register L to address at HL 8T 1PC
    LDVal8(CPUreg.hl.L, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x77(){ //load value from register A to address at HL 8T 1PC
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x02(){ //write register A to address BC 8T 1PC
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.bc.BC]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x12(){ //write register A to address DE 8T 1PC
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.de.DE]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Blue LD instructions (memory address in 16 bit register into 8 bit register) without register increment
void op_0x46(){ //load value from address HL into register B 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.bc.B); //load value from address HL into register B
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x56(){ //load value from address HL into register D 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.de.D); //load value from address HL into register D
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x66(){ //load value from address HL into register H 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.hl.H); //load value from address HL into register H
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x4E(){ //load value from address HL into register C 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.bc.C); //load value from address HL into register C
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x5E(){ //load value from address HL into register E 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.de.E); //load value from address HL into register E
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x6E(){ //load value from address HL into register L 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.hl.L); //load value from address HL into register L
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x7E(){ //load value from address HL into register A 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.af.A); //load value from address HL into register A
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x0A(){ //load value from address BC into register A 8T 1PC
    LDVal8(*memoryMap[CPUreg.bc.BC], &CPUreg.af.A); //load value from address BC into register A
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x1A(){ //load value from address DE into register A 8T 1PC
    LDVal8(*memoryMap[CPUreg.de.DE], &CPUreg.af.A); //load value from address DE into register A
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Blue LD instructions (8 bit register into HL register with increment or decrement)

void op_0x22(){ //write register A to address HL and increment HL 8T 1PC
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.hl.HL]);
    CPUreg.hl.HL += 1; //increment HL by 1
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x32(){ //write register A to address HL and decrement HL 8T 1PC
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.hl.HL]);
    CPUreg.hl.HL -= 1; //decrement HL by 1
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Blue LD instructions (8 bit value into 8/16 bit register)
void op_0x06(){ //load immediate value into register B 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.bc.B); //load immediate value into register B
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x16(){ //load immediate value into register D 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.de.D); //load immediate value into register D
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x26(){ //load immediate value into register H 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.hl.H); //load immediate value into register H
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x0E(){ //load immediate value into register C 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.bc.C); //load immediate value into register C
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x1E(){ //load immediate value into register E 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.de.E); //load immediate value into register E
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x2E(){ //load immediate value into register L 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.hl.L); //load immediate value into register L
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x3E(){ //load immediate value into register A 8T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], &CPUreg.af.A); //load immediate value into register A
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0x36(){ //load immediate value into address HL 12T 2PC
    LDVal8(*memoryMap[CPUreg.PC + 1], memoryMap[CPUreg.hl.HL]); //load immediate value into address HL
    CPUreg.PC += 2; 
    cyclesAccumulated += 12;
}

//Blue LD instructions (HL register address increment or decrement into 8 bit register)
void op_0x2A(){ //load value from address HL into register A and increment HL 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.af.A); //load value from address HL into register A
    CPUreg.hl.HL += 1; //increment HL by 1
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x3A(){ //load value from address HL into register A and decrement HL 8T 1PC
    LDVal8(*memoryMap[CPUreg.hl.HL], &CPUreg.af.A); //load value from address HL into register A
    CPUreg.hl.HL -= 1; //decrement HL by 1
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Blue LD instructions (8 bit register into 8 bit register)

void op_0x40(){ //load value from register B to register B 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x50(){ //load value from register B to register D 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x60(){ //load value from register B to register H 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x41(){ //load value from register C to register B 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x51(){ //load value from register C to register D 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x61(){ //load value from register C to register H 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x42(){ //load value from register D to register B 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x52(){ //load value from register D to register D 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x62(){ //load value from register D to register H 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x43(){ //load value from register E to register B 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x53(){ //load value from register E to register D 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x63(){ //load value from register E to register H 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x44(){ //load value from register H to register B 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x54(){ //load value from register H to register D 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x64(){ //load value from register H to register H 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x45(){ //load value from register L to register B 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x55(){ //load value from register L to register D 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x65(){ //load value from register L to register H 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x47(){ //load value from register A to register B 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x57(){ //load value from register A to register D 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x67(){ //load value from register A to register H 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x48(){ //load value from register B to register C 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x58(){ //load value from register B to register E 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x68(){ //load value from register B to register L 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x78(){ //load value from register B to register A 4T 1PC
    LDVal8(CPUreg.bc.B, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x49(){ //load value from register C to register C 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x59(){ //load value from register C to register E 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x69(){ //load value from register C to register L 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x79(){ //load value from register C to register A 4T 1PC
    LDVal8(CPUreg.bc.C, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x4A(){ //load value from register D to register C 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x5A(){ //load value from register D to register E 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x6A(){ //load value from register D to register L 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x7A(){ //load value from register D to register A 4T 1PC
    LDVal8(CPUreg.de.D, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x4B(){ //load value from register E to register C 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x5B(){ //load value from register E to register E 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x6B(){ //load value from register E to register L 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x7B(){ //load value from register E to register A 4T 1PC
    LDVal8(CPUreg.de.E, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x4C(){ //load value from register H to register C 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x5C(){ //load value from register H to register E 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x6C(){ //load value from register H to register L 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x7C(){ //load value from register H to register A 4T 1PC
    LDVal8(CPUreg.hl.H, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x4D(){ //load value from register L to register C 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x5D(){ //load value from register L to register E 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x6D(){ //load value from register L to register L 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}   

void op_0x7D(){ //load value from register L to register A 4T 1PC
    LDVal8(CPUreg.hl.L, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x4F(){ //load value from register A to register C 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x5F(){ //load value from register A to register E 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}   

void op_0x6F(){ //load value from register A to register L 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x7F(){ //load value from register A to register A 4T 1PC
    LDVal8(CPUreg.af.A, &CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

//Blue load A C and a8 A - 0xFF00-0xFFFF instructions

void op_0xE0(){ //load A into address 0xFF00 + immediate 12T 2PC
    uint8_t immediate = *memoryMap[CPUreg.PC + 1]; //get immediate value from memory
    LDVal8(CPUreg.af.A, memoryMap[0xFF00 + immediate]); //load value from A into address 0xFF00 + immediate
    CPUreg.PC += 2; 
    cyclesAccumulated += 12;
}

uint8_t buttonState = 0xFF; // All buttons released (bits 0–7 high)

void updateJoypadRegister() {
    uint8_t select = *memoryMap[0xFF00] & 0xF0;
    uint8_t result = 0xCF | select;
    uint8_t lower = 0x0F;

    if (!(select & (1 << 4))) {
        lower &= (buttonState & 0x0F); // D-pad
    }

    if (!(select & (1 << 5))) {
        lower &= ((buttonState >> 4) & 0x0F); // Action buttons
    }

    result = (result & 0xF0) | lower;
    *memoryMap[0xFF00] = result;
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

    // --- Update FF00 to reflect current button states ---
    uint8_t select = *memoryMap[0xFF00] & 0xF0; // Get selection bits (P14/P15)
    uint8_t result = 0xCF | select;

    uint8_t lower = 0x0F;

    if (!(select & (1 << 4))) { // P14 low → D-pad selected
        lower &= (buttonState & 0x0F);
    }

    if (!(select & (1 << 5))) { // P15 low → Action buttons selected
        lower &= ((buttonState >> 4) & 0x0F);
    }

    result = (result & 0xF0) | lower;
    *memoryMap[0xFF00] = result;

    // Trigger joypad interrupt if any new button press occurred
    if ((prevState & ~buttonState) & 0xFF) {
        *memoryMap[0xFF0F] |= 0x10; // Bit 4 = Joypad interrupt
    }
}

uint8_t readJoypad(uint8_t select) {
    // select: current value at 0xFF00 (written by CPU)
    uint8_t result = 0xCF; // Upper bits default to 1, bits 4 & 5 control selection
    result |= (select & 0x30); // preserve select bits

    if (!(select & (1 << 4))) {
        // P14 low: D-pad selected
        result &= (0xF0 | (buttonState & 0x0F));
    } else if (!(select & (1 << 5))) {
        // P15 low: Action buttons selected
        result &= (0xF0 | ((buttonState >> 4) & 0x0F));
    }

    return result;
}

void op_0xF0() { // LD A, (FF00 + n)
    uint8_t immediate = *memoryMap[CPUreg.PC + 1];
    uint16_t addr = 0xFF00 + immediate;

    uint8_t value;
    if (addr == 0xFF00) {
       // value = readJoypad(*memoryMap[0xFF00]);
        //printf("Read Joypad: %02X, FF00: %02X\n", value, *memoryMap[0xFF00]); // Debugging output
        value = *memoryMap[addr];
    } else {
        value = *memoryMap[addr];
    }

    LDVal8(value, &CPUreg.af.A);

    CPUreg.PC += 2;
    cyclesAccumulated += 12;
}

void op_0xE2(){ //load value from register A into address C for 0xFF00-0xFFFF  8T 1PC
    LDVal8(CPUreg.af.A, memoryMap[CPUreg.bc.C + 0xFF00]); 
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0xF2(){ //load value from address 0xFF00-0xFFFF into register A 8T 1PC
    LDVal8(*memoryMap[CPUreg.bc.C + 0xFF00], &CPUreg.af.A); 
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Blue LD instructions A and 16 bit immediate address

void op_0xEA(){ // load A into address a16 16T 3PC
    uint16_t address = (*memoryMap[CPUreg.PC + 2] << 8) | *memoryMap[CPUreg.PC + 1];
    LDVal8(CPUreg.af.A, memoryMap[address]);
    CPUreg.PC += 3; 
    cyclesAccumulated += 16;
}

void op_0xFA(){ // load value from address a16 into A 16T 3PC
    uint16_t address = (*memoryMap[CPUreg.PC + 2] << 8 | *memoryMap[CPUreg.PC + 1]); //combine low and high byte to get address
    LDVal8(*memoryMap[address], &CPUreg.af.A);
    CPUreg.PC += 3; 
    cyclesAccumulated += 16;
}

//Red ADD instructions (add 16 bit register to HL) with flags
void op_0x09(){ //add BC to HL 8T 1PC
    uint16_t result = CPUreg.hl.HL + CPUreg.bc.BC;
    setSubtractFlag(0); // clear subtract flag
    setCarryFlag(result < CPUreg.hl.HL); // set carry flag if result is less than HL
    setHalfCarryFlag((CPUreg.hl.HL & 0xFFF) + (CPUreg.bc.BC & 0xFFF) > 0xFFF); // set half carry flag if low nibble overflows
    CPUreg.hl.HL = result; // update HL with result
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x19(){ //add DE to HL 8T 1PC
    uint16_t result = CPUreg.hl.HL + CPUreg.de.DE;
    setSubtractFlag(0); // clear subtract flag
    setCarryFlag(result < CPUreg.hl.HL); // set carry flag if result is less than HL
    setHalfCarryFlag((CPUreg.hl.HL & 0xFFF) + (CPUreg.de.DE & 0xFFF) > 0xFFF); // set half carry flag if low nibble overflows
    CPUreg.hl.HL = result; // update HL with result
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x29(){ //add HL to HL 8T 1PC
    uint16_t result = CPUreg.hl.HL + CPUreg.hl.HL;
    setSubtractFlag(0); // clear subtract flag
    setCarryFlag(result < CPUreg.hl.HL); // set carry flag if result is less than HL
    setHalfCarryFlag((CPUreg.hl.HL & 0xFFF) + (CPUreg.hl.HL & 0xFFF) > 0xFFF); // set half carry flag if low nibble overflows
    CPUreg.hl.HL = result; // update HL with result
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x39(){ //add SP to HL 8T 1PC
    uint16_t result = CPUreg.hl.HL + CPUreg.SP;
    setSubtractFlag(0); // clear subtract flag
    setCarryFlag(result < CPUreg.hl.HL); // set carry flag if result is less than HL
    setHalfCarryFlag((CPUreg.hl.HL & 0xFFF) + (CPUreg.SP & 0xFFF) > 0xFFF); // set half carry flag if low nibble overflows
    CPUreg.hl.HL = result; // update HL with result
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0xE8() {
    int8_t offset = (int8_t)*memoryMap[CPUreg.PC + 1]; // signed immediate
    int16_t signedOffset = (int16_t)offset; // sign extend
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

void op_0x03(){ //increment BC 8T 1PC
    CPUreg.bc.BC += 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x13(){ //increment DE 8T 1PC
    CPUreg.de.DE += 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x23(){ //increment HL 8T 1PC
    CPUreg.hl.HL += 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x33(){ //increment SP 8T 1PC
    CPUreg.SP += 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Red Decrement instructions (decrement 16 bit register by 1) with no flag
void op_0x0B(){ //decrement BC 8T 1PC
    CPUreg.bc.BC -= 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x1B(){ //decrement DE 8T 1PC
    CPUreg.de.DE -= 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x2B(){ //decrement HL 8T 1PC
    CPUreg.hl.HL -= 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x3B(){ //decrement SP 8T 1PC
    CPUreg.SP -= 1;
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

//Yellow Increment instructions (increment 8 bit register by 1) with flags

void op_0x04(){ //increment B 4T 1PC
    setHalfCarryFlag((CPUreg.bc.B & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.bc.B += 1;
    if (CPUreg.bc.B == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x14(){ //increment D 4T 1PC
    setHalfCarryFlag((CPUreg.de.D & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.de.D += 1;
    if (CPUreg.de.D == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x24(){ //increment H 4T 1PC
    setHalfCarryFlag((CPUreg.hl.H & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.hl.H += 1;
    if (CPUreg.hl.H == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x34(){ //increment address at HL 12T 1PC
    uint8_t value = *memoryMap[CPUreg.hl.HL]; // get value from address HL
    setHalfCarryFlag((value & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    value += 1; // increment value
    *memoryMap[CPUreg.hl.HL] = value; // store incremented value back to address HL
    if (value == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 12;
}

void op_0x0C(){ //increment C 4T 1PC
    setHalfCarryFlag((CPUreg.bc.C & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.bc.C += 1;
    if (CPUreg.bc.C == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x1C(){ //increment E 4T 1PC
    setHalfCarryFlag((CPUreg.de.E & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.de.E += 1;
    if (CPUreg.de.E == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x2C(){ //increment L 4T 1PC
    setHalfCarryFlag((CPUreg.hl.L & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.hl.L += 1;
    if (CPUreg.hl.L == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x3C(){ //increment A 4T 1PC
    setHalfCarryFlag((CPUreg.af.A & 0x0F) == 0x0F); // set half carry flag if low nibble is 1111 before increment
    CPUreg.af.A += 1;
    if (CPUreg.af.A == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

//Yellow Decrement instructions (decrement 8 bit register by 1) with flags
void op_0x05(){ //decrement B 4T 1PC
    setHalfCarryFlag((CPUreg.bc.B & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.bc.B -= 1;
    if (CPUreg.bc.B == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x15(){ //decrement D 4T 1PC
    setHalfCarryFlag((CPUreg.de.D & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.de.D -= 1;
    if (CPUreg.de.D == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x25(){ //decrement H 4T 1PC
    setHalfCarryFlag((CPUreg.hl.H & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.hl.H -= 1;
    if (CPUreg.hl.H == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x35(){ //decrement address at HL 12T 1PC
    setHalfCarryFlag((*memoryMap[CPUreg.hl.HL] & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    uint8_t value = *memoryMap[CPUreg.hl.HL]; // get value from address HL
    value -= 1; // decrement value
    *memoryMap[CPUreg.hl.HL] = value; // store decremented value back to address HL
    if (value == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 12;
}

void op_0x0D(){ //decrement C 4T 1PC
    setHalfCarryFlag((CPUreg.bc.C & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.bc.C -= 1;
    if (CPUreg.bc.C == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x1D(){ //decrement E 4T 1PC
    setHalfCarryFlag((CPUreg.de.E & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.de.E -= 1;
    if (CPUreg.de.E == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x2D(){ //decrement L 4T 1PC
    setHalfCarryFlag((CPUreg.hl.L & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.hl.L -= 1;
    if (CPUreg.hl.L == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
}

void op_0x3D(){ //decrement A 4T 1PC
    setHalfCarryFlag((CPUreg.af.A & 0x0F) == 0x00); // set half carry flag if low nibble is 0000 before decrement
    CPUreg.af.A -= 1;
    if (CPUreg.af.A == 0) {
        setZeroFlag(1); // set zero flag
    } else {
        setZeroFlag(0); // clear zero flag
    }
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    cyclesAccumulated += 4;
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

void op_0x80(){ //add B to A 4T 1PC
    addToA(CPUreg.bc.B);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x81(){ //add C to A 4T 1PC
    addToA(CPUreg.bc.C);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x82(){ //add D to A 4T 1PC
    addToA(CPUreg.de.D);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x83(){ //add E to A 4T 1PC
    addToA(CPUreg.de.E);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x84(){ //add H to A 4T 1PC
    addToA(CPUreg.hl.H);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x85(){ //add L to A 4T 1PC
    addToA(CPUreg.hl.L);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x86(){ //add value at address HL to A 8T 1PC
    addToA(*memoryMap[CPUreg.hl.HL]); //add value at address HL to A
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x87(){ //add A to A 4T 1PC
    addToA(CPUreg.af.A);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x88(){ // add Carry Flag + B to A 4T 1PC
    adcToA(CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x89(){ // add Carry Flag + C to A 4T 1PC
    adcToA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x8A(){ // add Carry Flag + D to A 4T 1PC
    adcToA(CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x8B(){ // add Carry Flag + E to A 4T 1PC
    adcToA(CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x8C(){ // add Carry Flag + H to A 4T 1PC
    adcToA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x8D(){ // add Carry Flag + L to A 4T 1PC
    adcToA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x8E() { // add Carry Flag + value at address HL to A 8T 1PC
    adcToA(*memoryMap[CPUreg.hl.HL]); // add value at address HL to A
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x8F(){ // add Carry Flag + A to A 4T 1PC
    adcToA(CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xC6(){ // add immediate value to A 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1]; // get immediate value from memory
    addToA(value); // add immediate value to A
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0xCE(){ // add immediate value + Carry Flag to A 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1];
    adcToA(value);
    CPUreg.PC += 2;
    cyclesAccumulated += 8;
}

//Yellow Subtract instructions (subtract 8 bit register from 8 bit register) with flags
void subFromA(uint8_t value) {
    uint16_t result = CPUreg.af.A - value;

    setHalfCarryFlag((CPUreg.af.A & 0x0F) < (value & 0x0F)); // half-borrow occurred
    setCarryFlag(CPUreg.af.A < value); // full borrow occurred
    CPUreg.af.A = result & 0xFF;

    setZeroFlag((CPUreg.af.A & 0xFF) == 0); // result is zero
    setSubtractFlag(1); // subtract flag set
}

void sbcFromA(uint8_t value) {
    uint8_t a = CPUreg.af.A;
    uint8_t carry = getCarryFlag();  // usually 0 or 1

    uint16_t result = a - value - carry;

    setZeroFlag((result & 0xFF) == 0);                        // Z flag: zero result
    setSubtractFlag(1);                                       // N flag: subtraction
    setHalfCarryFlag((a & 0x0F) < ((value & 0x0F) + carry)); // H flag: half borrow
    setCarryFlag(a < (value + carry));                        // C flag: full borrow

    CPUreg.af.A = result & 0xFF;
}

void op_0x90(){ //subtract B from A 4T 1PC
    subFromA(CPUreg.bc.B);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x91(){ //subtract C from A 4T 1PC
    subFromA(CPUreg.bc.C);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x92(){ //subtract D from A 4T 1PC
    subFromA(CPUreg.de.D);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x93(){ //subtract E from A 4T 1PC
    subFromA(CPUreg.de.E);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x94(){ //subtract H from A 4T 1PC
    subFromA(CPUreg.hl.H);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x95(){ //subtract L from A 4T 1PC
    subFromA(CPUreg.hl.L);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x96(){ //subtract value at address HL from A 8T 1PC
    subFromA(*memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1; 
    cyclesAccumulated += 8;
}

void op_0x97(){ //subtract A from A 4T 1PC
    subFromA(CPUreg.af.A);
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0x98(){ // subtract Carry Flag + B from A 4T 1PC
    sbcFromA(CPUreg.bc.B);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x99(){ // subtract Carry Flag + C from A 4T 1PC
    int carry = getCarryFlag();
    sbcFromA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x9A(){ // subtract Carry Flag + D from A 4T 1PC
    sbcFromA(CPUreg.de.D);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x9B(){ // subtract Carry Flag + E from A 4T 1PC
    sbcFromA(CPUreg.de.E);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x9C(){ // subtract Carry Flag + H from A 4T 1PC
    sbcFromA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x9D(){ // subtract Carry Flag + L from A 4T 1PC
    sbcFromA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0x9E() {  // subtract Carry Flag + value at address HL from A 8T 1PC
    sbcFromA(*memoryMap[CPUreg.hl.HL]);
    CPUreg.PC += 1;
    cyclesAccumulated += 8;
}

void op_0x9F(){ // subtract Carry Flag + A from A 4T 1PC
    sbcFromA(CPUreg.af.A);
    CPUreg.PC += 1; 
    cyclesAccumulated += 4;
}

void op_0xD6(){ // SUB A, immediate 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1];
    subFromA(value);
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

void op_0xDE(){ // SBC A, immediate (subtract immediate + Carry Flag from A) 8T 2PC
    uint8_t value = *memoryMap[CPUreg.PC + 1];
    sbcFromA(value);
    CPUreg.PC += 2; 
    cyclesAccumulated += 8;
}

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

void op_0xCB00() {  // RLC B
    uint8_t value = CPUreg.bc.B;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB01() {  // RLC C
    uint8_t value = CPUreg.bc.C;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB02() {  // RLC D
    uint8_t value = CPUreg.de.D;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.de.D = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB03() {  // RLC E
    uint8_t value = CPUreg.de.E;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.de.E = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB04() {  // RLC H
    uint8_t value = CPUreg.hl.H;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB05() {  // RLC L
    uint8_t value = CPUreg.hl.L;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB06() {  // RLC (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    *valuePtr = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}   

void op_0xCB07() {  // RLC A
    uint8_t value = CPUreg.af.A;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    CPUreg.af.A = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

 //Rotate right through carry, store LSB in carry flag and rotate MSB to LSB

void op_0xCB08() {  // RRC B
    uint8_t value = CPUreg.bc.B;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB09() {  // RRC C
    uint8_t value = CPUreg.bc.C;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB0A() {  // RRC D
    uint8_t value = CPUreg.de.D;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.de.D = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB0B() {  // RRC E
    uint8_t value = CPUreg.de.E;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.de.E = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB0C() {  // RRC H
    uint8_t value = CPUreg.hl.H;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB0D() {  // RRC L
    uint8_t value = CPUreg.hl.L;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB0E() {  // RRC (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    *valuePtr = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB0F() {  // RRC A
    uint8_t value = CPUreg.af.A;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (lsb << 7); // rotate right
    CPUreg.af.A = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

// RL 

void op_0xCB10() {  // RL B
    uint8_t value = CPUreg.bc.B;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB11() {  // RL C
    uint8_t value = CPUreg.bc.C;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB12() {  // RL D
    uint8_t value = CPUreg.de.D;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.de.D = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB13() {  // RL E
    uint8_t value = CPUreg.de.E;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.de.E = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB14() {  // RL H
    uint8_t value = CPUreg.hl.H;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB15() {  // RL L
    uint8_t value = CPUreg.hl.L;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB16() {  // RL (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    *valuePtr = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB17() {  // RL A
    uint8_t value = CPUreg.af.A;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1) | getCarryFlag(); // rotate left and add carry flag to LSB
    CPUreg.af.A = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

// RR

void op_0xCB18() {  // RR B
    uint8_t value = CPUreg.bc.B;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB19() {  // RR C
    uint8_t value = CPUreg.bc.C;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB1A() {  // RR D
    uint8_t value = CPUreg.de.D;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.de.D = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB1B() {  // RR E
    uint8_t value = CPUreg.de.E;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.de.E = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB1C() {  // RR H
    uint8_t value = CPUreg.hl.H;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB1D() {  // RR L
    uint8_t value = CPUreg.hl.L;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB1E() {  // RR (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    *valuePtr = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB1F() {  // RR A
    uint8_t value = CPUreg.af.A;
    uint8_t lsb = value & 0x01; // get LSB
    value = (value >> 1) | (getCarryFlag() << 7); // rotate right and add carry flag to MSB
    CPUreg.af.A = value;

    setZeroFlag(value == 0);     
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

//SLA 

void op_0xCB20() {  // SLA B
    uint8_t value = CPUreg.bc.B;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB21() {  // SLA C
    uint8_t value = CPUreg.bc.C;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB22() {  // SLA D
    uint8_t value = CPUreg.de.D;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.de.D = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB23() {  // SLA E
    uint8_t value = CPUreg.de.E;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.de.E = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB24() {  // SLA H
    uint8_t value = CPUreg.hl.H;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB25() {  // SLA L
    uint8_t value = CPUreg.hl.L;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB26() {  // SLA (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    *valuePtr = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB27() {  // SLA A
    uint8_t value = CPUreg.af.A;
    uint8_t msb = (value >> 7) & 0x01; // get MSB
    value = (value << 1); // shift left
    CPUreg.af.A = value;

    setZeroFlag(value == 0);     
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4; //first PC and 4 cycle done by inital CB instruction
}

//SRA

void op_0xCB28() {  // SRA B
    uint8_t value = CPUreg.bc.B;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB29() {  // SRA C
    uint8_t value = CPUreg.bc.C;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB2A() {  // SRA D
    uint8_t value = CPUreg.de.D;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.de.D = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB2B() {  // SRA E
    uint8_t value = CPUreg.de.E;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.de.E = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB2C() {  // SRA H
    uint8_t value = CPUreg.hl.H;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB2D() {  // SRA L
    uint8_t value = CPUreg.hl.L;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB2E() {  // SRA (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    *valuePtr = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12;
}

void op_0xCB2F() {  // SRA A
    uint8_t value = CPUreg.af.A;
    uint8_t lsb = value & 0x01;          //bit 0 before shift
    uint8_t msb = value & 0x80;          //preserve original bit 7

    value = (value >> 1) | msb;          //shift right, keep MSB
    CPUreg.af.A = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);                   // use LSB for Carry
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

//SWAP

void op_0xCB30() {  // SWAP B
    uint8_t value = CPUreg.bc.B;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB31() {  // SWAP C
    uint8_t value = CPUreg.bc.C;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB32() {  // SWAP D
    uint8_t value = CPUreg.de.D;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.de.D = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB33() {  // SWAP E
    uint8_t value = CPUreg.de.E;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.de.E = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB34() {  // SWAP H
    uint8_t value = CPUreg.hl.H;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB35() {  // SWAP L
    uint8_t value = CPUreg.hl.L;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB36() {  // SWAP (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    *valuePtr = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12;
}

void op_0xCB37() {  // SWAP A
    uint8_t value = CPUreg.af.A;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4); // swap nibbles
    CPUreg.af.A = value;

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

//SRL

void op_0xCB38() {  // SRL B
    uint8_t value = CPUreg.bc.B;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.bc.B = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB39() {  // SRL C
    uint8_t value = CPUreg.bc.C;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.bc.C = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB3A() {  // SRL D
    uint8_t value = CPUreg.de.D;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.de.D = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB3B() {  // SRL E
    uint8_t value = CPUreg.de.E;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.de.E = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB3C() {  // SRL H
    uint8_t value = CPUreg.hl.H;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.hl.H = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB3D() {  // SRL L
    uint8_t value = CPUreg.hl.L;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.hl.L = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB3E() {  // SRL (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    *valuePtr = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12;
}

void op_0xCB3F() {  // SRL A
    uint8_t value = CPUreg.af.A;
    uint8_t lsb = value & 0x01; // get LSB
    value >>= 1; // shift right
    CPUreg.af.A = value;

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

//BIT

void op_0xCB40() {  // BIT 0, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB41() {  // BIT 0, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB42() {  // BIT 0, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB43() {  // BIT 0, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB44() {  // BIT 0, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB45() {  // BIT 0, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB46() {  // BIT 0, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB47() {  // BIT 0, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB48() {  // BIT 1, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB49() {  // BIT 1, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB4A() {  // BIT 1, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB4B() {  // BIT 1, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB4C() {  // BIT 1, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB4D() {  // BIT 1, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB4E() {  // BIT 1, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB4F() {  // BIT 1, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x02) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB50() {  // BIT 2, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB51() {  // BIT 2, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB52() {  // BIT 2, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB53() {  // BIT 2, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB54() {  // BIT 2, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB55() {  // BIT 2, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB56() {  // BIT 2, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB57() {  // BIT 2, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x04) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB58() {  // BIT 3, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB59() {  // BIT 3, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}


void op_0xCB5A() {  // BIT 3, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB5B() {  // BIT 3, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB5C() {  // BIT 3, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB5D() {  // BIT 3, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB5E() {  // BIT 3, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB5F() {  // BIT 3, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x08) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB60() {  // BIT 4, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB61() {  // BIT 4, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB62() {  // BIT 4, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB63() {  // BIT 4, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB64() {  // BIT 4, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB65() {  // BIT 4, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB66() {  // BIT 4, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB67() {  // BIT 4, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x10) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB68() {  // BIT 5, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB69() {  // BIT 5, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB6A() {  // BIT 5, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB6B() {  // BIT 5, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB6C() {  // BIT 5, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB6D() {  // BIT 5, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB6E() {  // BIT 5, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB6F() {  // BIT 5, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x20) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB70() {  // BIT 6, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB71() {  // BIT 6, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB72() {  // BIT 6, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB73() {  // BIT 6, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB74() {  // BIT 6, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB75() {  // BIT 6, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB76() {  // BIT 6, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB77() {  // BIT 6, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x40) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB78() {  // BIT 7, B
    uint8_t value = CPUreg.bc.B;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB79() {  // BIT 7, C
    uint8_t value = CPUreg.bc.C;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB7A() {  // BIT 7, D
    uint8_t value = CPUreg.de.D;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB7B() {  // BIT 7, E
    uint8_t value = CPUreg.de.E;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB7C() {  // BIT 7, H
    uint8_t value = CPUreg.hl.H;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB7D() {  // BIT 7, L
    uint8_t value = CPUreg.hl.L;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB7E() {  // BIT 7, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    uint8_t value = *valuePtr;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 8; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB7F() {  // BIT 7, A
    uint8_t value = CPUreg.af.A;
    setZeroFlag((value & 0x80) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

//RES

void op_0xCB80() {  // RES 0, B
    CPUreg.bc.B &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB81() {  // RES 0, C
    CPUreg.bc.C &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB82() {  // RES 0, D
    CPUreg.de.D &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB83() {  // RES 0, E
    CPUreg.de.E &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB84() {  // RES 0, H
    CPUreg.hl.H &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB85() {  // RES 0, L
    CPUreg.hl.L &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB86() {  // RES 0, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB87() {  // RES 0, A
    CPUreg.af.A &= ~0x01; // clear bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB88() {  // RES 1, B
    CPUreg.bc.B &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB89() {  // RES 1, C
    CPUreg.bc.C &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB8A() {  // RES 1, D
    CPUreg.de.D &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB8B() {  // RES 1, E
    CPUreg.de.E &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB8C() {  // RES 1, H
    CPUreg.hl.H &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB8D() {  // RES 1, L
    CPUreg.hl.L &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB8E() {  // RES 1, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB8F() {  // RES 1, A
    CPUreg.af.A &= ~0x02; // clear bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB90() {  // RES 2, B
    CPUreg.bc.B &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB91() {  // RES 2, C
    CPUreg.bc.C &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB92() {  // RES 2, D
    CPUreg.de.D &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB93() {  // RES 2, E
    CPUreg.de.E &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB94() {  // RES 2, H
    CPUreg.hl.H &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB95() {  // RES 2, L
    CPUreg.hl.L &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB96() {  // RES 2, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB97() {  // RES 2, A
    CPUreg.af.A &= ~0x04; // clear bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB98() {  // RES 3, B
    CPUreg.bc.B &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB99() {  // RES 3, C
    CPUreg.bc.C &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB9A() {  // RES 3, D
    CPUreg.de.D &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}
void op_0xCB9B() {  // RES 3, E
    CPUreg.de.E &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB9C() {  // RES 3, H
    CPUreg.hl.H &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB9D() {  // RES 3, L
    CPUreg.hl.L &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCB9E() {  // RES 3, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCB9F() {  // RES 3, A
    CPUreg.af.A &= ~0x08; // clear bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA0() {  // RES 4, B
    CPUreg.bc.B &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA1() {  // RES 4, C
    CPUreg.bc.C &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA2() {  // RES 4, D
    CPUreg.de.D &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA3() {  // RES 4, E
    CPUreg.de.E &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA4() {  // RES 4, H
    CPUreg.hl.H &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA5() {  // RES 4, L
    CPUreg.hl.L &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA6() {  // RES 4, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBA7() {  // RES 4, A
    CPUreg.af.A &= ~0x10; // clear bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA8() {  // RES 5, B
    CPUreg.bc.B &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBA9() {  // RES 5, C
    CPUreg.bc.C &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBAA() {  // RES 5, D
    CPUreg.de.D &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBAB() {  // RES 5, E
    CPUreg.de.E &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBAC() {  // RES 5, H
    CPUreg.hl.H &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBAD() {  // RES 5, L
    CPUreg.hl.L &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBAE() {  // RES 5, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBAF() {  // RES 5, A
    CPUreg.af.A &= ~0x20; // clear bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB0() {  // RES 6, B
    CPUreg.bc.B &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB1() {  // RES 6, C
    CPUreg.bc.C &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB2() {  // RES 6, D
    CPUreg.de.D &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB3() {  // RES 6, E
    CPUreg.de.E &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB4() {  // RES 6, H
    CPUreg.hl.H &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB5() {  // RES 6, L
    CPUreg.hl.L &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB6() {  // RES 6, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBB7() {  // RES 6, A
    CPUreg.af.A &= ~0x40; // clear bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB8() {  // RES 7, B
    CPUreg.bc.B &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBB9() {  // RES 7, C
    CPUreg.bc.C &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBBA() {  // RES 7, D
    CPUreg.de.D &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBBB() {  // RES 7, E
    CPUreg.de.E &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBBC() {  // RES 7, H
    CPUreg.hl.H &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBBD() {  // RES 7, L
    CPUreg.hl.L &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBBE() {  // RES 7, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBBF() {  // RES 7, A
    CPUreg.af.A &= ~0x80; // clear bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

//SET

void op_0xCBC0() {  // SET 0, B
    CPUreg.bc.B |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC1() {  // SET 0, C
    CPUreg.bc.C |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC2() {  // SET 0, D
    CPUreg.de.D |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC3() {  // SET 0, E
    CPUreg.de.E |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC4() {  // SET 0, H
    CPUreg.hl.H |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC5() {  // SET 0, L
    CPUreg.hl.L |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC6() {  // SET 0, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBC7() {  // SET 0, A
    CPUreg.af.A |= 0x01; // set bit 0
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC8() {  // SET 1, B
    CPUreg.bc.B |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBC9() {  // SET 1, C
    CPUreg.bc.C |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBCA() {  // SET 1, D
    CPUreg.de.D |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBCB() {  // SET 1, E
    CPUreg.de.E |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBCC() {  // SET 1, H
    CPUreg.hl.H |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBCD() {  // SET 1, L
    CPUreg.hl.L |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBCE() {  // SET 1, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBCF() {  // SET 1, A
    CPUreg.af.A |= 0x02; // set bit 1
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD0() {  // SET 2, B
    CPUreg.bc.B |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD1() {  // SET 2, C
    CPUreg.bc.C |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD2() {  // SET 2, D
    CPUreg.de.D |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD3() {  // SET 2, E
    CPUreg.de.E |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD4() {  // SET 2, H
    CPUreg.hl.H |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD5() {  // SET 2, L
    CPUreg.hl.L |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD6() {  // SET 2, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBD7() {  // SET 2, A
    CPUreg.af.A |= 0x04; // set bit 2
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD8() {  // SET 3, B
    CPUreg.bc.B |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBD9() {  // SET 3, C
    CPUreg.bc.C |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBDA() {  // SET 3, D
    CPUreg.de.D |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBDB() {  // SET 3, E
    CPUreg.de.E |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBDC() {  // SET 3, H
    CPUreg.hl.H |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBDD() {  // SET 3, L
    CPUreg.hl.L |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBDE() {  // SET 3, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBDF() {  // SET 3, A
    CPUreg.af.A |= 0x08; // set bit 3
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE0() {  // SET 4, B
    CPUreg.bc.B |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE1() {  // SET 4, C
    CPUreg.bc.C |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE2() {  // SET 4, D
    CPUreg.de.D |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE3() {  // SET 4, E
    CPUreg.de.E |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE4() {  // SET 4, H
    CPUreg.hl.H |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE5() {  // SET 4, L
    CPUreg.hl.L |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE6() {  // SET 4, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBE7() {  // SET 4, A
    CPUreg.af.A |= 0x10; // set bit 4
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE8() {  // SET 5, B
    CPUreg.bc.B |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBE9() {  // SET 5, C
    CPUreg.bc.C |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBEA() {  // SET 5, D
    CPUreg.de.D |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBEB() {  // SET 5, E
    CPUreg.de.E |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBEC() {  // SET 5, H
    CPUreg.hl.H |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBED() {  // SET 5, L
    CPUreg.hl.L |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBEE() {  // SET 5, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBEF() {  // SET 5, A
    CPUreg.af.A |= 0x20; // set bit 5
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF0() {  // SET 6, B
    CPUreg.bc.B |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF1() {  // SET 6, C
    CPUreg.bc.C |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}   

void op_0xCBF2() {  // SET 6, D
    CPUreg.de.D |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF3() {  // SET 6, E
    CPUreg.de.E |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF4() {  // SET 6, H
    CPUreg.hl.H |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF5() {  // SET 6, L
    CPUreg.hl.L |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF6() {  // SET 6, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBF7() {  // SET 6, A
    CPUreg.af.A |= 0x40; // set bit 6
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF8() {  // SET 7, B
    CPUreg.bc.B |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBF9() {  // SET 7, C
    CPUreg.bc.C |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBFA() {  // SET 7, D
    CPUreg.de.D |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBFB() {  // SET 7, E
    CPUreg.de.E |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBFC() {  // SET 7, H
    CPUreg.hl.H |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBFD() {  // SET 7, L
    CPUreg.hl.L |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

void op_0xCBFE() {  // SET 7, (HL)
    uint8_t *valuePtr = memoryMap[CPUreg.hl.HL];
    *valuePtr |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 12; //first PC and 4 cycle done by inital CB instruction
}

void op_0xCBFF() {  // SET 7, A
    CPUreg.af.A |= 0x80; // set bit 7
    CPUreg.PC += 1;
    cyclesAccumulated += 4;
}

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
    return count; //returns how many sprites in the sprite buffer
}

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
} fetchStage;

void pixelPushBG(Queue* fifo, uint8_t xPos, fetchStage* stage, int fetchWindow) { //retrieve pixels tiles and push current tile row to FIFO Queue 
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
            mapY = ly - wy;
        } else { //if fetching background tile
            // Background tile map base depends on LCDC bit 3
            tileMapBase = (lcdc & 0x08) ? 0x9C00 : 0x9800;

            // Background coordinates - NO x scrolling applied here, scroll handled by xPos logic later
            uint8_t scy = *memoryMap[0xFF42];
            mapY = (ly + scy) & 0xFF; //y scroll added with wrap 0-255

            uint8_t scx = *memoryMap[0xFF43];
            mapX = (xPos + scx) & 0xFF;
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
       // printf("TileNum: %d, Addr: 0x%04X, line=%d, byte1=0x%02X, byte2=0x%02X\n", tileNum, tileAddr, line, byte1, byte2);
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

int scxCounter = 0;
int newScanLine = 1;

void handleInterrupts() {
    if (CPUreg.IME == 0) return; // Interrupts disabled

    uint8_t IE = *memoryMap[0xFFFF]; // Interrupt Enable
    uint8_t IF = *memoryMap[0xFF0F]; // Interrupt Flag
    uint8_t fired = IE & IF;

    if (!fired) return; // No interrupt requested

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
            //printf("[INTERRUPT] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) Cycles=%d\n",*memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC, realCyclesAccumulated);
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


int div_counter = 0;
int tima_counter = 0;

int main(){
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *window = SDL_CreateWindow("GB-EMU", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 720, 0); //window width and height 160x144
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED); //default driver gpu accelerated if possible
    memset(display, 0, sizeof(display));
    SDL_Window *tileWindow = SDL_CreateWindow(
    "Tile Viewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
    128, 192, 0); // 16 tiles * 8 = 128, 24 tiles * 8 = 192

    logFile = fopen("emu_log.txt", "w");
    if (!logFile) {
        perror("Failed to open log file");
        exit(1);
    }

SDL_Renderer *tileRenderer = SDL_CreateRenderer(tileWindow, -1, SDL_RENDERER_ACCELERATED);

    loadrom("Tetris.gb"); //initialises  memory and CPU registers in this function as well
    printromHeader();
    printf("Press Enter to exit...\n");
    getchar();

    PPU ppu = { //initialise ppu
    .mode = memoryMap[0xFF41], // Mode bits are in STAT
    .ly = memoryMap[0xFF44],   // LY
    .lyc = memoryMap[0xFF45],  // LYC
    .dotCounter = 0,
    .frameReady = 0
};

    fetchStage fetchStage = {
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
    
    int scanlineTimer = 0; // amount of cycles for scanline
    int mode0Timer = 0; // amount of cycles for Hblank
    int mode1Timer = 0; // amount of cycles for Vblank
    int mode2Timer = 0;
    int mode3Timer = 0; // amount of cycles needed to start next operation

    int xPos = 0;
    Sprite spriteBuffer[10]; 

    cyclesAccumulated = 0;
    int CPUtimer = 0; //timer for CPU cycles

    int open = 1;
    SDL_Event event;

    int DMAFlag = 0; // DMA transfer flag
    
    //Uint32 lastTick = SDL_GetTicks(); //timing
    int FF85Flag = 0; // Flag for FF85 register


    while (open) {
       /* Uint32 now = SDL_GetTicks();
        //if (now - lastTick < 1000) { //timing
          SDL_Delay(1); // Sleep a bit to avoid busy-waiting
            continue;
        }
        lastTick = now;
        */

        while (SDL_PollEvent(&event)) { //Check if user quit or not
            if (event.type == SDL_QUIT) open = 0; //SDL_QUIT is close window button
            handleButtonPress(&event); //handle button press events
            //*memoryMap[0xFF00] = readJoypad(*memoryMap[0xFF00]); //read joypad state and write to memory
        }
        updateJoypadRegister();

    if (CPUreg.IME && (*memoryMap[0xFFFF] & *memoryMap[0xFF0F])) { // Check if interrupts are enabled and any interrupt is requested
        handleInterrupts();
    }

    //DMA

    
        if ((*memoryMap[0xFF46] != 0xFF) && (DMAFlag == 0)) { // DMA transfer requested
            DMAFlag = 1;
            uint8_t dmaSource = *memoryMap[0xFF46]; // Get DMA source address
            uint16_t sourceAddr = dmaSource << 8; // Convert to 16-bit address
            uint16_t destAddr = 0xFE00; // OAM starts at 0xFE00
            for (int i = 0; i < 0xA0; i++) { // Transfer 160 bytes (40 sprites * 4 bytes each)
                *memoryMap[destAddr + i] = *memoryMap[sourceAddr + i];
            }
            *memoryMap[0xFF46] = 0xFF; // Reset DMA register
            DMAFlag = 0;
            //getchar(); // Wait for user input to continue
            //fprintf(logFile, "DMA transfer completed from %02X to OAM\n", dmaSource);
            //printf("DMA transfer completed from %02X to OAM\n", dmaSource);
            //cyclesAccumulated += 640; // DMA timing delay
        }


            

        uint16_t oldSP = CPUreg.SP;

            // --- HALT and HALT bug handling ---
        uint8_t opcode = *memoryMap[CPUreg.PC];
        
        
        uint8_t prevFF00 = *memoryMap[0xFF00]; // Save previous FF00 value 

        /*
        for (uint16_t addr = 0x0000; addr <= 0x3FFF; addr++) {
            if (*memoryMap[addr] == 0x0A) {
                //printf("Found 0x0A at address: 0x%04X\n", addr);
                break;
            }
            else{
                memset(eram,0xFF,sizeof(eram));
            }
        }
            */

            
        if (CPUtimer == 0){
            if (opcode == 0xC3 || opcode == 0xCD || opcode == 0xC9) {  // JP, CALL, RET
    //fprintf(logFile,"Jump/Call/Return at PC=%04X, opcode=%02X, target depends on next byte(s)\n", CPUreg.PC, opcode);
}           

            fprintf(logFile, "%04X %02X\n", CPUreg.PC, opcode);
            //printf("PC: %04X, Opcode: %02X\n", CPUreg.PC, opcode);
            //fprintf(logFile, "PC: %04X, Opcode: %02X, Cycles: %d, SP: %04X, A: %02X, B: %02X, C: %02X, D: %02X, E: %02X, F: %02X, H: %02X, L: %02X, LY: %02X, LYC: %02X, FF80: %02X, FF85: %02X, FF00: %02X\n", CPUreg.PC, opcode, realCyclesAccumulated, CPUreg.SP, CPUreg.af.A, CPUreg.bc.B, CPUreg.bc.C, CPUreg.de.D, CPUreg.de.E, CPUreg.af.F, CPUreg.hl.H, CPUreg.hl.L, *(ppu.ly), *(ppu.lyc), *memoryMap[0xFF80], *memoryMap[0xFF85], *memoryMap[0xFF00]);
        }
        if (haltMode == 1) {
            uint8_t IE = *memoryMap[0xFFFF];
            uint8_t IF = *memoryMap[0xFF0F];

            // Wake up if any interrupt is pending
            if ((IE & IF & 0x1F) != 0) {
                haltMode = 0;
            } else {
                // Don't fetch or execute instructions — but still let system tick
                // No continue here, just skip execution block below
            }
        } else if (haltMode == 2) {
            // HALT bug: execute same instruction twice
            pcBacktrace[backtraceIndex] = CPUreg.PC - 1;
            backtraceIndex = (backtraceIndex + 1) % 8;
            executeOpcode(opcode);
            CPUtimer = cyclesAccumulated;
            haltMode = 0;
            cyclesAccumulated = 0;
        } else {
            // --- Normal CPU execution ---
            pcBacktrace[backtraceIndex] = CPUreg.PC - 1;
            backtraceIndex = (backtraceIndex + 1) % 8;
            if (CPUtimer > 0) {
                CPUtimer--;
            } else {
                if (CBFlag == 0) {
                    //printf("Executing opcode %02X at PC=%04X\n", opcode, CPUreg.PC);
                    executeOpcode(opcode);
                    CPUtimer = cyclesAccumulated;
                    if (EIFlag == 1) EIFlag = -1;
                    else if (EIFlag == -1 || EIFlag == -2) {
                        CPUreg.IME = 1;
                        EIFlag = 0;
                    }
                } else {
                    executeOpcodeCB(opcode);
                    //printf("Executing opcode %02X at PC=%04X\n", opcode, CPUreg.PC);
                    CPUtimer = cyclesAccumulated;
                    CBFlag = 0;
                    if (EIFlag == -1) EIFlag = -2;
                }
                cyclesAccumulated = 0;
            }
        }
        *memoryMap[0xFF00] = (*memoryMap[0xFF00] & 0xF0) | (prevFF00 & 0x0F); //preserve lower bits

        if (CPUreg.SP != oldSP && debug == 1) {
            printf("[SP CHANGE] from %04X → %04X at PC=%04X\n", oldSP, CPUreg.SP, CPUreg.PC);
            oldSP = CPUreg.SP;
        }

    //CPU
    /*
    uint8_t opcode = *memoryMap[CPUreg.PC]; //fetch opcode from memory at PC address 
    
    //PC incremented in opcodes functions
    //printf("Before Cycles accumulated: %d\n", realCyclesAccumulated);
    //printf("F flags: Z=%d N=%d H=%d C=%d\n", getZeroFlag(), getSubtractFlag(), getHalfCarryFlag(), getCarryFlag());
    //printf("Before PC: %04X, OP: %02X, B: %02X, F: %02X\n", CPUreg.PC, opcode, CPUreg.bc.B, CPUreg.af.F);

    if (CPUtimer > 0) { //if CPU timer is greater than 0, then we are still in the middle of executing an opcode
        CPUtimer--;
    }
    else {
        if (CBFlag == 0) {
            executeOpcode(opcode); //execute the op code once last opcode is done (cycle count for previous opcode is done)
            CPUtimer = cyclesAccumulated; //potentuially just change cyclesAccumulated to CPUtimer later in code
        
            if (EIFlag == 1){ // EIflag to cause 1 instruction delay after EI opcode
                EIFlag = -1; // reset EIFlag to -1 after executing EI opcode
            }
            else if (EIFlag == -1) { // after 1 instruction delay, set IME to 1 and reset EIFlag
                CPUreg.IME = 1;
                EIFlag = 0; // reset after executing EI opcode
            }
            else if (EIFlag == -2) { // after 1 instruction delay, set IME to 1 and reset EIFlag
                CPUreg.IME = 1;
                EIFlag = 0; // reset after executing EI opcode
            }
        }
        else { //CBFlag is set, execute CB opcode
            executeOpcodeCB(opcode);
            CPUtimer = cyclesAccumulated; //set CPU timer to cycles accumulated
            CBFlag = 0; //reset CBFlag after executing CB opcode
            if (EIFlag == -1) { // after 1 instruction delay, set IME to 1 and reset EIFlag
                EIFlag = -2; // reset after executing EI opcode
                CBFlag = 0;
            }
        }
        cyclesAccumulated = 0;

    }
    */

    //printf(" After PC: %04X, OP: %02X, B: %02X, F: %02X\n", CPUreg.PC, opcode, CPUreg.bc.B, CPUreg.af.F);
    //printf("F flags: Z=%d N=%d H=%d C=%d\n",getZeroFlag(), getSubtractFlag(), getHalfCarryFlag(), getCarryFlag());
    //printf("After Cycles accumulated: %d\n OPCode: %d \n PC: %d\n", realCyclesAccumulated, opcode, CPUreg.PC);
    //printf("input: %02X\n ppu mode: %02X\n BGFetchStage: %02X\n", *memoryMap[0xFF00], *ppu.mode, fetchStage.BGFetchStage); //print input state
    //getchar(); //wait for user input to continue, remove later

    //PPU 

    switch(*ppu.mode & 0x03){ //get which mode the PPU is currently in
        case(0):
            mode0Timer = 456 - scanlineTimer; //HBlank lasts 456 cycles, subtract the cycles already used in this scanline
            //printf("Mode 0: HBlank, Timer: %d\n", mode0Timer);
            if (mode0Timer == 0) { //if timer is 0, then
                if (*ppu.ly == 144) { //Start VBlank 
                    drawDisplay(renderer); //draw display to window 
                    drawTileViewer(tileRenderer);
                    SDL_RenderPresent(renderer); //update window with everything drawn
                    *memoryMap[0xFF0F] |= 0x01; // Set VBlank flag in IF register
                    //printf("VBlank request at cycle %d\n", realCyclesAccumulated);
                    if (debug == 1) {
                        printf("VBlank started at cycle %d\n", realCyclesAccumulated);
                    }
                    // Enter VBlank mode
                    *ppu.mode = (*ppu.mode & 0xFC) | 0x01; // Mode 1
                    if (*memoryMap[0xFF41] & 0x10) { // Bit 4: VBlank STAT interrupt enable
                        *memoryMap[0xFF0F] |= 0x02;  // STAT interrupt request flag
                    }

                    mode1Timer = 456; // VBlank lasts 10 lines of 456 cycles each
                    scanlineTimer = 0; //reset scanline timer
                }
                else{   
                    *ppu.mode = (*ppu.mode & 0xFC) | (2 & 0x03); //set mode to OAM scan
                    if (*memoryMap[0xFF41] & 0x20) { // Bit 5: OAM STAT interrupt enable
                        *memoryMap[0xFF0F] |= 0x02;  // STAT interrupt request flag
                    }
                    scanlineTimer = 0; //reset scanline timer
                }
            }
            break;

        case(1):
            if (*memoryMap[0xFF44] == *memoryMap[0xFF45]) { // Check LYC register for coincidence
                        *memoryMap[0xFF41] |= 0x04; // Set coincidence flag (bit 2)
                        if (*memoryMap[0xFF41] & 0x40) { // If coincidence interrupt is enabled
                            *memoryMap[0xFF0F] |= 0x02; // Request STAT interrupt (bit 1 of IF)
                        }
                    } else {
                        *memoryMap[0xFF41] &= ~0x04; // Clear coincidence flag
                    }
            if(*ppu.ly != 153){
                if(mode1Timer == 0){
                    (*ppu.ly)++; // increment LY 
                    //printf("Scanline %d\n", *ppu.ly);
                    if (*ppu.ly == *ppu.lyc) { // Check LYC register for coincidence
                        *memoryMap[0xFF41] |= 0x04; // Set coincidence flag (bit 2)

                    if (*memoryMap[0xFF41] & 0x40) { // If coincidence interrupt is enabled
                        *memoryMap[0xFF0F] |= 0x02; // Request STAT interrupt (bit 1 of IF)
                    }
                    } 
                    else {
                        *memoryMap[0xFF41] &= ~0x04; // Clear coincidence flag
                    }

                    mode1Timer = 456; // VBlank lasts 10 lines of 456 cycles each
                    scanlineTimer = 0; //reset scanline timer
                }
                
                else { //if LY is 153, then we are in VBlank mode
                    mode1Timer-- ; //decrement mode1Timer until it reaches 0
                }
            }
            else{ //LY is 153, last VBLANK line
                if(mode1Timer == 0){
                    *ppu.ly = 0; // reset LY to 0
                    scanlineTimer = 0; //reset scanline timer
                    if (*ppu.ly == *ppu.lyc) { // Check LYC register for coincidence
                        *memoryMap[0xFF41] |= 0x04; // Set coincidence flag (bit 2)

                    if (*memoryMap[0xFF41] & 0x40) { // If coincidence interrupt is enabled
                        *memoryMap[0xFF0F] |= 0x02; // Request STAT interrupt (bit 1 of IF)
                    }
                    } 
                    else {
                        *memoryMap[0xFF41] &= ~0x04; // Clear coincidence flag
                    }
                    *ppu.mode = (*ppu.mode & 0xFC) | (2 & 0x03); // set mode to OAM scan
                    mode1Timer = 456; // reset timer for next VBlank
                }

                else {
                    mode1Timer--; //decrement mode1Timer until it reaches 0
                }
            }
            break;

        //OAM scan, wait 80 cycles then load the spriteBuffer and change to mode 3

        case(2): 
            if(mode2Timer != 80){
                mode2Timer++; //wait 80 cycles before switching modes
            }
            else if(mode2Timer == 80){
                spriteSearchOAM(*memoryMap[0xFF44], spriteBuffer); //store sprites in sprite buffer
                mode2Timer = 0; //reset timer for next mode 2 check
                *memoryMap[0xFF41] = (*memoryMap[0xFF41] & 0xFC) | (3 & 0x03); //set to mode 3
            }
            break;

        //Mode 3 fetching and pushing queues, change to HBlank after scanline done, Vblank when all scanlines done

        case(3): //Currently the BG / Window fetcher  no sprite
            uint8_t lcdc = *memoryMap[0xFF40];
            uint8_t wx = *memoryMap[0xFF4B];
            int windowEnabled = (lcdc & 0x20) != 0;
            int windowStartX = wx - 7;
            uint8_t wy = *memoryMap[0xFF4A];

            if (windowEnabled && xPos >= windowStartX && fetchStage.windowFetchMode == 0 && *ppu.ly == wy) { //if window is enabled for first time, clear queue and fetch windows
                for (int i = 0; i < 8; i++) {
                    Pixel temp;
                    dequeue(&BGFifo, &temp);
                }
                fetchStage.BGFetchStage = 0;
                mode3Timer = 2;
                fetchStage.windowFetchMode = 1; 
            } 
            else if(windowEnabled && fetchStage.windowFetchMode == 1 && mode3Timer == 0){ //fetching window tiles 
                if(fetchStage.BGFetchStage == 0){
                    fetchStage.BGFetchStage = 1;
                    mode3Timer = 2;
                }
                else if(fetchStage.BGFetchStage == 1){
                    fetchStage.BGFetchStage = 2;
                    mode3Timer = 2;
                }
                else if(fetchStage.BGFetchStage == 2){
                    fetchStage.BGFetchStage = 3;
                    mode3Timer = 2;
                }
                else if (fetchStage.BGFetchStage == 3){ //window xPos - (wx-7) handled in function for window mode
                    pixelPushBG(&BGFifo, xPos, &fetchStage, 1); //sets BGfetchStage to 0 if success otherwise stays on 3
                    mode3Timer = 2;
                }
            }
            else if (windowEnabled == 0 && fetchStage.windowFetchMode == 1){ // window turned off and back to background mode?
                for (int i = 0; i < 8; i++) {
                    Pixel temp;
                    dequeue(&BGFifo, &temp);
                }
                fetchStage.BGFetchStage = 0;
                fetchStage.windowFetchMode = 0;
                mode3Timer = 2;
            }
            else if (windowEnabled == 0 && fetchStage.windowFetchMode == 0 && fetchStage.BGFetchStage == 0 && mode3Timer == 0){
                fetchStage.BGFetchStage = 1; //start fetching background tiles
                mode3Timer = 2;
            }     
            else if (windowEnabled == 0 && fetchStage.windowFetchMode == 0 && fetchStage.BGFetchStage == 1 && mode3Timer == 0 ){
                fetchStage.BGFetchStage = 2;
                mode3Timer = 2;
            }
            else if (windowEnabled == 0 && fetchStage.windowFetchMode == 0 && fetchStage.BGFetchStage == 2 && mode3Timer == 0 ){
                fetchStage.BGFetchStage = 3;
                mode3Timer = 2;
            }
            else if (windowEnabled == 0 && fetchStage.windowFetchMode == 0 && fetchStage.BGFetchStage == 3 && mode3Timer == 0 ){
                //switches to mode 0 in bgpush function if needed
                pixelPushBG(&BGFifo, xPos , &fetchStage, 0); //pixel push for background (0) mode not window mode, scx handled by discarding later on
                //getchar(); //wait for user input to continue, remove later
                mode3Timer = 2;
            }

            //FIFO pixel pushing to screen mode 3
            //scxCounter = 0, newScanLine = 1, set outside main loop

            uint8_t scx = *memoryMap[0xFF43];
            if(xPos == 0 && newScanLine){ //set counter for pixels to discard because of scroll on this scanline
                scxCounter = scx % 8;
                newScanLine = 0;
            }

            if (BGFifo.count > 0) {
                if(scxCounter > 0){  //don't do anything with pixel, discard x bits for scroll
                    Pixel discard;
                    dequeue(&BGFifo, &discard);
                    scxCounter--;
                }
                else{
                    Pixel pixelToPush;
                    dequeue(&BGFifo, &pixelToPush);
                    // do pixel mixing etc here
                    // Send to display buffer...
                    int y = *ppu.ly;
                    if (y < 144) {
                        display[xPos][y] = pixelToPush.colour;
                        //printf("Draw: (%d, %d) = %d\n", xPos, y, pixelToPush.colour);
                    }

                    xPos++; // increment xPos here
                }
                
            } //need spritemixing etc but this would be the pixel added to display buffer for that scanline/xPos
            if (xPos >= 160) {
                *ppu.mode = (*ppu.mode & 0xFC) | 0x00; // Mode 0 (HBlank)
                if (*memoryMap[0xFF41] & 0x08) { // Bit 3: HBlank STAT interrupt enable
                    *memoryMap[0xFF0F] |= 0x02;  // Set STAT interrupt request flag (bit 1)
                }
                xPos = 0; //reset xPos for next line
                fetchStage.BGFetchStage = 0;
                //update LY or VBLANK, VBLANK is hanndledd in HBlank
                (*ppu.ly)++;
                //printf("Scanline %d completed, moving to next line\n", *ppu.ly);

                if (*ppu.ly == *ppu.lyc) { // Check LYC register for coincidence
                    *memoryMap[0xFF41] |= 0x04; // Set coincidence flag (bit 2)
                    if (*memoryMap[0xFF41] & 0x40) { // If coincidence interrupt is enabled
                        *memoryMap[0xFF0F] |= 0x02; // Request STAT interrupt (bit 1 of IF)
                    }
                } else {
                    *memoryMap[0xFF41] &= ~0x04; // Clear coincidence flag
                }

                newScanLine = 1;  //reset newScanLine for next loop
            }
            break;
        }

    //SDL_Delay(1000/63); //fps
    mode3Timer--; //decrement mode3Timer for next loop, if 0 then fetch next pixel
    realCyclesAccumulated += 1; //increment real cycles accumulated for each loop
    scanlineTimer += 1; //increment scanline timer for each loop
    if (mode3Timer <= 0) {
        mode3Timer = 0; //reset mode3Timer for next loop
    }

    //Serial communication

    if (*memoryMap[0xFF02] & 0x80) { // Transfer requested
        // Print the byte being "sent" over serial
        printf("%c", *memoryMap[0xFF01]);
        fflush(stdout); // Ensure immediate output

        // Simulate transfer: echo back the same byte
        *memoryMap[0xFF01] = *memoryMap[0xFF01];
        *memoryMap[0xFF02] &= ~0x80; // Clear transfer bit
        *memoryMap[0xFF0F] |= 0x08;  // Request serial interrupt
    }

    // Timer handling

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
                *memoryMap[0xFF05] = *memoryMap[0xFF06]; // Load TMA
                *memoryMap[0xFF0F] |= 0x04; // Request timer interrupt
            }
        }
    }
    //printQueueState("BGFifo", &BGFifo);
    //printQueueState("SpriteFifo", &SpriteFifo);
    //printf("Mode 0: Scanlne, Timer: %d\n", scanlineTimer);
    //printf("ly: %d, xPos: %d\n", *ppu.ly, xPos);
    //printf("PC: %04X, OP: %02X\n", CPUreg.PC, *memoryMap[CPUreg.PC]);

    
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
                                  
}



































































































/*
 void draw_scanline() {
    uint8_t lcdc = *memoryMap[0xFF40]; //Control flags
    uint8_t scy = *memoryMap[0xFF42]; //scroll X
    uint8_t scx = *memoryMap[0xFF43]; //scroll Y
    uint8_t ly  = *memoryMap[0xFF44]; //Which scanline currently on (0-153)
    uint8_t bgp = *memoryMap[0xFF47]; //Background palette, controls which colour is shown for each colour value assigned to pixel (0-3)

    // Tile map 32x32 grid containing tile numbers to draw (0–255 or -128 to 127), 2 tile maps, stored in tileMapBase
   //  tile numbers give actual tile at that position, they are 8x8 using 2 bits per pixel (4 colour) and stored as 16 bytes, using 2 bytes per row
   //  , tile data stored in tileDataBase using either signed or unsigned numbers
   //  x9800–0x9BFF → Tile Map 0
     // 0x9C00–0x9FFF → Tile Map 1
    // 0x8000–0x8FFF → 256 tiles (unsigned)
    // 0x8800–0x97FF → 256 tiles (signed, -128 to 127) 

    uint16_t tileMapBase = (lcdc & 0x08) ? 0x9C00 : 0x9800; //bit 3, which tile map to choose 0 or 1, (0 for tile map 0 and 1 for tile map 1 )
    uint16_t useUnsignedTiles = ((lcdc & 0x10) != 0); //bit 4, whether to use signed or unsigned tiles (0–255 or -128 to 127, 1 is unsigned, 0 is signed) signed and unsigned have different tiles even for same number, don't overlap in memory e.g sig 10 != unsig 10 

     for (int x = 0; x < 160; x++) {
        uint8_t scrollX = scx; //position on 256 x 256 tile map
        uint8_t scrollY = scy;
        uint16_t mapX = (x + scrollX) & 0xFF; //x position on map for current scanline
        uint16_t mapY = (ly + scrollY) & 0xFF; //scanline + yscroll (Y position on map)

        uint16_t tileRow = mapY / 8;
        uint16_t tileCol = mapX / 8;
        uint16_t tileIndexAddr = tileMapBase + tileRow * 32 + tileCol; //get tile number from tilemap
        int8_t tileNum = *memoryMap[tileIndexAddr]; //number of tile to use for address

        uint16_t tileAddr; //address for tiles depending on signed vs unsigned
        if (useUnsignedTiles != 0) {
            tileAddr = 0x8000 + ((uint8_t)tileNum * 16);
        } else {
            tileAddr = 0x9000 + (tileNum * 16);
        }

        uint8_t line = mapY % 8; //which row of the tile, currently at, 0-7
        uint8_t byte1 = *memoryMap[tileAddr + line * 2]; //byte 1 of tile
        uint8_t byte2 = *memoryMap[tileAddr + line * 2 + 1]; //byte 2 of tile

        // Column inside tile (0–7), extract pixel bits from MSB to LSB
        uint8_t bitIndex = 7 - (mapX % 8);
        uint8_t lo = (byte1 >> bitIndex) & 0x01;
        uint8_t hi = (byte2 >> bitIndex) & 0x01;
        uint8_t colourId = (hi << 1) | lo;

        // Apply palette (each 2 bits in BGP maps colorId to grayscale color 0-3)
        uint8_t colour = (bgp >> (colourId * 2)) & 0x03;

        // Write pixel to framebuffer
        displayBuffer[ly][x] = colour;
    }
} */