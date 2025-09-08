#include "cpu.h"
#include "memory.h"
#include "input.h"
#include "ppu.h"

CPUState CPUreg;

void initCPU(){
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

    CPUreg.haltMode = 0; // not in halt mode
    CPUreg.CPUtimer = 0; // initialize CPU timer
    CPUreg.EIFlag = 0; // EI not just executed
    CPUreg.CBFlag = 0; // CB prefix not just executed
    CPUreg.cyclesAccumulated = 0; // no cycles accumulated yet
}

//MBC Bank handling  
void updateBanks() {
    // --- ROM bank 0 ---
    for (int i = 0x0000; i <= 0x3FFF; i++)
        memory.memoryMap[i] = &memory.cartridge[i];

    // --- Switchable ROM bank ---
    uint8_t bank = memory.mbc_rom_bank & 0x7F;
    if (bank == 0) bank = 1;
    if (bank >= memory.totalRomBanks) bank %= memory.totalRomBanks;

    uint32_t rom_offset = bank * 0x4000;
    for (int i = 0x4000; i <= 0x7FFF; i++)
        memory.memoryMap[i] = &memory.cartridge[rom_offset + (i - 0x4000)];

    // --- External RAM / RTC ---
    if (memory.mbcType == 1) { // MBC1
        uint8_t ram_bank = 0;
        if (memory.mbc1_mode) {
            if (memory.mbc_ram_bank < memory.totalRamBanks)
                ram_bank = memory.mbc_ram_bank;
        }
        uint32_t ram_offset = ram_bank * 0x2000;
        for (int i = 0xA000; i <= 0xBFFF; i++)
            memory.memoryMap[i] = (memory.mbc_ram_enable && memory.totalRamBanks > 0)
                         ? &memory.eram[ram_offset + (i - 0xA000)]
                         : &memory.unusable[0];
    }
    else if (memory.mbcType == 3) { // MBC3
        if (memory.mbc_ram_bank <= 0x03 && memory.totalRamBanks > 0) {
            uint32_t ram_offset = memory.mbc_ram_bank * 0x2000;
            for (int i = 0xA000; i <= 0xBFFF; i++)
                memory.memoryMap[i] = memory.mbc_ram_enable
                             ? &memory.eram[ram_offset + (i - 0xA000)]
                             : &memory.unusable[0];
        } else if (memory.mbc_ram_bank >= 0x08 && memory.mbc_ram_bank <= 0x0C) {
            // Map RTC registers (fake: reads/writes handled manually)
            for (int i = 0xA000; i <= 0xBFFF; i++)
                memory.memoryMap[i] = &memory.unusable[0]; // trap accesses
        } else {
            for (int i = 0xA000; i <= 0xBFFF; i++)
                memory.memoryMap[i] = &memory.unusable[0];
        }
    }
}
                                                       
void handleMBCWrite(uint16_t addr, uint8_t value) {
    if (memory.mbcType == 1) {
        if (addr <= 0x1FFF) {
            memory.mbc_ram_enable = ((value & 0x0F) == 0x0A);
            updateBanks();
        }
        else if (addr <= 0x3FFF) {
            memory.mbc_rom_bank = (memory.mbc_rom_bank & 0x60) | (value & 0x1F);
            if ((memory.mbc_rom_bank & 0x1F) == 0) memory.mbc_rom_bank |= 1;
            updateBanks();
        }
        else if (addr <= 0x5FFF) {
            uint8_t upper2 = value & 0x03;
            if (memory.mbc1_mode)
                memory.mbc_ram_bank = upper2;
            else {
                memory.mbc_rom_bank = (memory.mbc_rom_bank & 0x1F) | (upper2 << 5);
                if ((memory.mbc_rom_bank & 0x7F) == 0) memory.mbc_rom_bank |= 1;
            }
            updateBanks();
        }
        else if (addr <= 0x7FFF) {
            memory.mbc1_mode = value & 0x01;
            updateBanks();
        }
    }
    else if (memory.mbcType == 3) {
        if (addr <= 0x1FFF) {
            memory.mbc_ram_enable = ((value & 0x0F) == 0x0A);
            updateBanks();
        }
        else if (addr <= 0x3FFF) {
            memory.mbc_rom_bank = value & 0x7F;
            if (memory.mbc_rom_bank == 0) memory.mbc_rom_bank = 1;
            updateBanks();
        }
        else if (addr <= 0x5FFF) {
            memory.mbc_ram_bank = value;  // 0–3 = RAM, 08–0C = RTC
            updateBanks();
        }
        else if (addr <= 0x7FFF) {
            if (memory.mbc3_rtc_latch == 0 && value == 1) { //only triggers when going from 0 to 1
                // Latch RTC registers from system clock
                time_t t = time(NULL);
                struct tm *tm = localtime(&t);

                memory.mbc3_rtc_regs[0] = tm->tm_sec;        // seconds
                memory.mbc3_rtc_regs[1] = tm->tm_min;        // minutes
                memory.mbc3_rtc_regs[2] = tm->tm_hour;       // hours
                memory.mbc3_rtc_regs[3] = tm->tm_mday & 0xFF; // lower 8 bits of day
                memory.mbc3_rtc_regs[4] = ((tm->tm_yday & 0x01) << 0) | 0; // upper day bit, control flags = 0
                }
            memory.mbc3_rtc_latch = value;
            }
        }
    if (addr >= 0xA000 && addr <= 0xBFFF) {
        if (memory.mbc_ram_enable) {
            *memory.memoryMap[addr] = value;  // write to ERAM
        }
        return;
    }
    }

void LDVal8(uint8_t value, uint8_t *dest, uint16_t addr, bool isMemory, uint16_t src) {

    uint8_t mode = *memory.memoryMap[0xFF41] & 0x03; //PPU mode (0 = HBlank, 1 = VBlank, 2 = OAM, 3 = Transfer)

    if(addr == 0xFF40 && (value >> 7) == 0) { // If LCDC is disabled, reset LY to 0
        LCDUpdate(0);
        //fprintf(logFile, "LCDC disabled, cycle: %ld\n", realCyclesAccumulated);
        *dest = value; // Write value to LCDC
        return; // Exit early if LCDC is disabled
    }


    if((addr == 0xFF40) && ((value >> 7) == 1) && (ppu.LCDdisabled == 1)) { // If LCDC is enabled, reset LY to 
        ppu.LCDdelayflag = 4; // Set LCD delay flag to turn back on
        //fprintf(logFile, "LCDC delay flag, cycle: %ld\n", realCyclesAccumulated);
        *dest = value; // Write value to LCDC
        return; // Exit early if LCDC is enabled
    }

    if ((src >= 0xA000 && src <= 0xBFFF) && memory.mbcType == 3 && memory.mbc_ram_bank >= 0x08 && memory.mbc_ram_bank <= 0x0C){
        // Map bank to RTC register index
        uint8_t rtcIndex = memory.mbc_ram_bank - 0x08;
        value = memory.mbc3_rtc_regs[rtcIndex];  // Return RTC value 
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
    if (addr >= 0xFE00 && addr <= 0xFE9F &&(mode == 2 || mode == 3 || ppu.DMAFlag == 1)) {
        return;
    }
    if (src >= 0xFE00 && src <= 0xFE9F &&
        (mode == 2 || mode == 3 || ppu.DMAFlag == 1)) {
        value = 0xFF;
         // Block read from OAM during Mode 2, 3, or DMA
    }


    if (src == 0xFF00){
        // Special case for Joypad register
        value = readJoypad(*memory.memoryMap[0xFF00]);
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
                *memory.memoryMap[0xFF00] = readJoypad(*memory.memoryMap[0xFF00]);
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

                ppu.DMAFlag = 1;
                *dest = value; // Store DMA source address
                uint8_t dmaSource = *memory.memoryMap[0xFF46]; // Get DMA source address
                uint16_t sourceAddr = dmaSource << 8; // Convert to 16-bit address
                uint16_t destAddr = 0xFE00; // OAM starts at 0xFE00
                for (int i = 0; i < 0xA0; i++) { // Transfer 160 bytes (40 sprites * 4 bytes each)
                    *memory.memoryMap[destAddr + i] = *memory.memoryMap[sourceAddr + i];
                }
                ppu.DMACycles = 640; // DMA takes 640 T cycles to complete
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
    CPUreg.cyclesAccumulated += 4;
}

void op_0x10(){
    //printf("stop called, not implemented yet\n");
    CPUreg.PC += 2; //increment PC by 1
    CPUreg.cyclesAccumulated += 4; //increment cycles by 4
    printf("Stop instruction executed, PC incremented to 0x%04X\n", CPUreg.PC);
  } //stop IMPLEMENT LATER

void op_0x20(){ //jump with relative offset if zero flag not set
    int zeroFlag = CPUreg.af.F & 0x80; // check if zero flag is set
    if (zeroFlag == 0) {
        CPUreg.PC += 2 + ((int8_t)*memory.memoryMap[CPUreg.PC + 1]); //signed integer added to PC 12T
        CPUreg.cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2; //increment PC by 2 if zero flag is set
        CPUreg.cyclesAccumulated += 8;
    }
}

void op_0x76() { // HALT
    uint8_t IE = *memory.memoryMap[0xFFFF];
    uint8_t IF = *memory.memoryMap[0xFF0F];
    if (CPUreg.IME == 0 && (IE & IF & 0x1F)) {
        // HALT bug: interrupts disabled, but at least one is pending
        CPUreg.haltMode = 2;
        CPUreg.cyclesAccumulated += 4;
    } else {
        // Normal halt
        CPUreg.haltMode = 1;
        CPUreg.cyclesAccumulated += 4;
    }
    CPUreg.PC += 1;
    //printf("Halt mode: %d, IE: %02X, IF: %02X, IME: %d\n", haltMode, IE, IF, CPUreg.IME);
}

//Purple interrupt instructions

void op_0xF3(){ //disable interrupts 4T 1PC
    CPUreg.IME = 0; //disable interrupts
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xFB(){ //enable interrupts 4T 1PC LOOK INTO CYCLE ACCURATE IMPLEMENTATION LATER
    CPUreg.EIFlag = 1;
    CPUreg.PC += 1; //increment PC by 1
    CPUreg.cyclesAccumulated += 4; //set EI flag to 1, will be processed after next instruction
}

//Orange Jump instructions
void relJumpIf(int condition) {
    if (condition) {
        CPUreg.PC += 2 + (int8_t)*memory.memoryMap[CPUreg.PC + 1];
        CPUreg.cyclesAccumulated += 12;
    } else {
        CPUreg.PC += 2;
        CPUreg.cyclesAccumulated += 8;
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
    CPUreg.cyclesAccumulated += 4;
}

//Orange call instructions

void conditionalCall(int condition) {
    if (condition) {
        *memory.memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) >> 8;
        *memory.memoryMap[--CPUreg.SP] = (CPUreg.PC + 3) & 0xFF;
        CPUreg.PC = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1];
        CPUreg.cyclesAccumulated += 24;
    } else {
        CPUreg.PC += 3;
        CPUreg.cyclesAccumulated += 12;
    }
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
        CPUreg.PC = (*memory.memoryMap[CPUreg.SP + 1] << 8) | *memory.memoryMap[CPUreg.SP];
        CPUreg.SP += 2;
        CPUreg.cyclesAccumulated += 20;
    } else {
        CPUreg.PC += 1;
        CPUreg.cyclesAccumulated += 8;
    }
}

void unconditionalReturn() {
    CPUreg.PC = (*memory.memoryMap[CPUreg.SP + 1] << 8) | *memory.memoryMap[CPUreg.SP]; //set PC to value on stack
    CPUreg.SP += 2; //increment stack pointer by 2
    CPUreg.cyclesAccumulated += 16;
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
    *memory.memoryMap[--CPUreg.SP] = (CPUreg.PC) >> 8; //push high byte of PC onto stack
    *memory.memoryMap[--CPUreg.SP] = (CPUreg.PC) & 0xFF; //push low byte of PC onto stack
    CPUreg.PC = addr; //set PC to restart address
    CPUreg.cyclesAccumulated += 16;
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
    *low = *memory.memoryMap[CPUreg.PC + 1];
    *high = *memory.memoryMap[CPUreg.PC + 2];
    CPUreg.PC += 3;
    CPUreg.cyclesAccumulated += 12;
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
    LDVal8(*memory.memoryMap[CPUreg.PC + 1], ((uint8_t*)&CPUreg.SP), 0xFFFF, 0, CPUreg.PC + 1);

    // Then load the high byte into the upper 8 bits of SP
    LDVal8(*memory.memoryMap[CPUreg.PC + 2], ((uint8_t*)&CPUreg.SP) + 1, 0xFFFF, 0, CPUreg.PC + 2);

    CPUreg.PC += 3;
    CPUreg.cyclesAccumulated += 12;
}

//Green SP instructions

void pushReg(uint8_t high, uint8_t low) {
    *memory.memoryMap[--CPUreg.SP] = high;
    *memory.memoryMap[--CPUreg.SP] = low;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 16;
}

void op_0xC5() { pushReg(CPUreg.bc.B, CPUreg.bc.C); }
void op_0xD5() { pushReg(CPUreg.de.D, CPUreg.de.E); }
void op_0xE5() { pushReg(CPUreg.hl.H, CPUreg.hl.L); }
void op_0xF5() { pushReg(CPUreg.af.A, CPUreg.af.F & 0xF0); }

void op_0xF8() { // LD HL, SP + r8
    int8_t offset = (int8_t)*memory.memoryMap[CPUreg.PC + 1]; //convert to signed 8 bit then cast signed 16 bit so it sign extends correctly e.g. 1000 0000 become 1111 1111 1000 000 (cast to int8_t first) instead of 0000 0000 1000 0000 (cast straight to int16_t)
    int16_t signedOffset = (int16_t)offset; // sign extend the offset
    uint16_t result = CPUreg.SP + signedOffset;

    setZeroFlag(0);
    setSubtractFlag(0);
    uint16_t sum = CPUreg.SP + offset;
    setCarryFlag(((CPUreg.SP & 0xFF) + (offset & 0xFF)) > 0xFF);
    setHalfCarryFlag(((CPUreg.SP & 0xF) + (offset & 0xF)) > 0xF);

    CPUreg.hl.HL = result;
    CPUreg.PC += 2;
    CPUreg.cyclesAccumulated += 12;
}

void op_0xF9(){ //load HL into SP 8T 1PC
    LDVal16(CPUreg.hl.HL, &CPUreg.SP); //set SP to HL value
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
}


void popReg(uint8_t *high, uint8_t *low, int isAF) {
    *low = *memory.memoryMap[CPUreg.SP];
    *high = *memory.memoryMap[CPUreg.SP + 1];
    if (isAF) *low &= 0xF0;
    CPUreg.SP += 2;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 12;
}

void op_0xC1() { popReg(&CPUreg.bc.B, &CPUreg.bc.C, 0); }
void op_0xD1() { popReg(&CPUreg.de.D, &CPUreg.de.E, 0); }
void op_0xE1() { popReg(&CPUreg.hl.H, &CPUreg.hl.L, 0); }
void op_0xF1() { popReg(&CPUreg.af.A, &CPUreg.af.F, 1); }

void op_0x08() {
    uint16_t addr = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1];
    LDVal8(CPUreg.SP & 0xFF, memory.memoryMap[addr],addr, 1,0xFFFF);
    LDVal8((CPUreg.SP >> 8) & 0xFF, memory.memoryMap[addr + 1],addr, 1,0xFFFF);
    CPUreg.PC += 3;
    CPUreg.cyclesAccumulated += 20;
}

//Blue LD instructions (8 bit register into memory address in 16 bit register) without register increment
void storeToAddr(uint16_t addr, uint8_t value) {
    LDVal8(value, memory.memoryMap[addr],addr, 1,0xFFFF);
    if(addr == 0xFF40){
        printf("A: 0x%02X, address: 0x%04X\n", CPUreg.af.A, addr);
    }
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
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
    LDVal8(*memory.memoryMap[addr], dest,0xFFFF, 0,addr);
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
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
    LDVal8(CPUreg.af.A, memory.memoryMap[CPUreg.hl.HL],CPUreg.hl.HL,1,0xFFFF);
    CPUreg.hl.HL += step;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
}

void op_0x22() { storeAtoHLAndStep(1); }
void op_0x32() { storeAtoHLAndStep(-1); }

//Blue LD instructions (8 bit value into 8/16 bit register)
void loadImmToReg(uint8_t *reg) {
    LDVal8(*memory.memoryMap[CPUreg.PC + 1], reg,CPUreg.PC + 1,0,0xFFFF);
    CPUreg.PC += 2;
    CPUreg.cyclesAccumulated += 8;
}

void op_0x06() { loadImmToReg(&CPUreg.bc.B); }
void op_0x0E() { loadImmToReg(&CPUreg.bc.C); }
void op_0x16() { loadImmToReg(&CPUreg.de.D); }
void op_0x1E() { loadImmToReg(&CPUreg.de.E); }
void op_0x26() { loadImmToReg(&CPUreg.hl.H); }
void op_0x2E() { loadImmToReg(&CPUreg.hl.L); }
void op_0x3E() { loadImmToReg(&CPUreg.af.A); }

void op_0x36(){
    LDVal8(*memory.memoryMap[CPUreg.PC + 1], memory.memoryMap[CPUreg.hl.HL],CPUreg.hl.HL,1,CPUreg.PC + 1);
    CPUreg.PC += 2;
    CPUreg.cyclesAccumulated += 12;
} 

//Blue LD instructions (HL register address increment or decrement into 8 bit register)
void loadAFromHLAndStep(int step) {
    LDVal8(*memory.memoryMap[CPUreg.hl.HL], &CPUreg.af.A,0xFFFF,1,CPUreg.hl.HL);
    CPUreg.hl.HL += step;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
}

void op_0x2A() { loadAFromHLAndStep(1); }
void op_0x3A() { loadAFromHLAndStep(-1); }

//Blue LD instructions (8 bit register into 8 bit register)

void loadRegToReg(uint8_t src, uint8_t *dest) {
    LDVal8(src, dest,0xFFFF,0,0xFFFF);
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 4;
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
    uint8_t immediate = *memory.memoryMap[CPUreg.PC + 1]; //get immediate value from memory
    LDVal8(CPUreg.af.A, memory.memoryMap[0xFF00 + immediate],0xFF00+immediate,1,0xFFFF); //load value from A into address 0xFF00 + immediate
    CPUreg.PC += 2; 
    CPUreg.cyclesAccumulated += 12;
    if ((0xFF00 + immediate == 0xFF40 )){
        printf("A: 0x%02X, address: 0x%04X\n", CPUreg.af.A, 0xFF00 + immediate);
    }
}


void op_0xF0() { // LD A, (FF00 + n)
    uint8_t immediate = *memory.memoryMap[CPUreg.PC + 1];
    uint16_t addr = 0xFF00 + immediate;

    LDVal8(*memory.memoryMap[addr], &CPUreg.af.A,0xFFFF,1,addr);

    CPUreg.PC += 2;
    CPUreg.cyclesAccumulated += 12;
}

void op_0xE2() { storeToAddr(0xFF00 + CPUreg.bc.C, CPUreg.af.A); }
void op_0xF2() { loadFromAddr(0xFF00 + CPUreg.bc.C, &CPUreg.af.A); }

//Blue LD instructions A and 16 bit immediate address

void op_0xEA(){ // load A into address a16 16T 3PC
    uint16_t address = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1];
    LDVal8(CPUreg.af.A, memory.memoryMap[address],address,1,0xFFFF);
    if(address == 0xFF40){
    printf("A: 0x%02X, address: 0x%04X\n", CPUreg.af.A, address);
    }
    CPUreg.PC += 3; 
    CPUreg.cyclesAccumulated += 16;
}

void op_0xFA(){ // load value from address a16 into A 16T 3PC
    uint16_t address = (*memory.memoryMap[CPUreg.PC + 2] << 8 | *memory.memoryMap[CPUreg.PC + 1]); //combine low and high byte to get address
    LDVal8(*memory.memoryMap[address], &CPUreg.af.A,0xFFFF,1,address);
    CPUreg.PC += 3; 
    CPUreg.cyclesAccumulated += 16;
}

//Red ADD instructions (add 16 bit register to HL) with flags
void add16ToHL(uint16_t value) {
    uint16_t result = CPUreg.hl.HL + value;
    setSubtractFlag(0);
    setCarryFlag(result < CPUreg.hl.HL);
    setHalfCarryFlag((CPUreg.hl.HL & 0xFFF) + (value & 0xFFF) > 0xFFF);
    CPUreg.hl.HL = result;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
}

void op_0x09() { add16ToHL(CPUreg.bc.BC); }
void op_0x19() { add16ToHL(CPUreg.de.DE); }
void op_0x29() { add16ToHL(CPUreg.hl.HL); }
void op_0x39() { add16ToHL(CPUreg.SP); }

void op_0xE8() {
    int8_t offset = (int8_t)*memory.memoryMap[CPUreg.PC + 1];
    int16_t signedOffset = (int16_t)offset;
    uint16_t result = CPUreg.SP + signedOffset;

    setZeroFlag(0);
    setSubtractFlag(0);
    setHalfCarryFlag(((CPUreg.SP & 0xF) + (offset & 0xF)) > 0xF);
    setCarryFlag(((CPUreg.SP & 0xFF) + (offset & 0xFF)) > 0xFF);

    CPUreg.SP = result;
    CPUreg.PC += 2;
    CPUreg.cyclesAccumulated += 16;
}

//Red Increment instructions (increment 16 bit register by 1) with no flag
void inc16(uint16_t* reg) {
    (*reg)++;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
}

void op_0x03() { inc16(&CPUreg.bc.BC); }
void op_0x13() { inc16(&CPUreg.de.DE); }
void op_0x23() { inc16(&CPUreg.hl.HL); }
void op_0x33() { inc16(&CPUreg.SP); }

//Red Decrement instructions (decrement 16 bit register by 1) with no flag
void dec16(uint16_t* reg) {
    (*reg)--;
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 8;
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
    CPUreg.cyclesAccumulated += 4;
}

void op_0x04() { inc8(&CPUreg.bc.B); }
void op_0x14() { inc8(&CPUreg.de.D); }
void op_0x24() { inc8(&CPUreg.hl.H); }
void op_0x0C() { inc8(&CPUreg.bc.C); }
void op_0x1C() { inc8(&CPUreg.de.E); }
void op_0x2C() { inc8(&CPUreg.hl.L); }
void op_0x3C() { inc8(&CPUreg.af.A); }

void op_0x34() {
    uint8_t value = *memory.memoryMap[CPUreg.hl.HL];
    setHalfCarryFlag((value & 0x0F) == 0x0F);
    value++;
    *memory.memoryMap[CPUreg.hl.HL] = value;
    setZeroFlag(value == 0);
    setSubtractFlag(0);
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 12;
}

void dec8(uint8_t* reg) {
    setHalfCarryFlag((*reg & 0x0F) == 0x00);
    (*reg)--;
    setZeroFlag(*reg == 0);
    setSubtractFlag(1);
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 4;
}

void op_0x05() { dec8(&CPUreg.bc.B); }
void op_0x15() { dec8(&CPUreg.de.D); }
void op_0x25() { dec8(&CPUreg.hl.H); }
void op_0x0D() { dec8(&CPUreg.bc.C); }
void op_0x1D() { dec8(&CPUreg.de.E); }
void op_0x2D() { dec8(&CPUreg.hl.L); }
void op_0x3D() { dec8(&CPUreg.af.A); }

void op_0x35() {
    uint8_t value = *memory.memoryMap[CPUreg.hl.HL];
    setHalfCarryFlag((value & 0x0F) == 0x00);
    value--;
    *memory.memoryMap[CPUreg.hl.HL] = value;
    setZeroFlag(value == 0);
    setSubtractFlag(1);
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += 12;
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

void op_0x80() { addToA(CPUreg.bc.B); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x81() { addToA(CPUreg.bc.C); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x82() { addToA(CPUreg.de.D); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x83() { addToA(CPUreg.de.E); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x84() { addToA(CPUreg.hl.H); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x85() { addToA(CPUreg.hl.L); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x86() { addToA(*memory.memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 8; }
void op_0x87() { addToA(CPUreg.af.A); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }

void op_0x88() { adcToA(CPUreg.bc.B); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x89() { adcToA(CPUreg.bc.C); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x8A() { adcToA(CPUreg.de.D); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x8B() { adcToA(CPUreg.de.E); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x8C() { adcToA(CPUreg.hl.H); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x8D() { adcToA(CPUreg.hl.L); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x8E() { adcToA(*memory.memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 8; }
void op_0x8F() { adcToA(CPUreg.af.A); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }

void op_0xC6() { addToA(*memory.memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; CPUreg.cyclesAccumulated += 8; }
void op_0xCE() { adcToA(*memory.memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; CPUreg.cyclesAccumulated += 8; }

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

void op_0x90() { subFromA(CPUreg.bc.B); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x91() { subFromA(CPUreg.bc.C); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x92() { subFromA(CPUreg.de.D); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x93() { subFromA(CPUreg.de.E); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x94() { subFromA(CPUreg.hl.H); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x95() { subFromA(CPUreg.hl.L); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x96() { subFromA(*memory.memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 8; }
void op_0x97() { subFromA(CPUreg.af.A); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x98() { sbcFromA(CPUreg.bc.B); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x99() { sbcFromA(CPUreg.bc.C); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x9A() { sbcFromA(CPUreg.de.D); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x9B() { sbcFromA(CPUreg.de.E); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x9C() { sbcFromA(CPUreg.hl.H); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x9D() { sbcFromA(CPUreg.hl.L); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0x9E() { sbcFromA(*memory.memoryMap[CPUreg.hl.HL]); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 8; }
void op_0x9F() { sbcFromA(CPUreg.af.A); CPUreg.PC += 1; CPUreg.cyclesAccumulated += 4; }
void op_0xD6() { subFromA(*memory.memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; CPUreg.cyclesAccumulated += 8; }
void op_0xDE() { sbcFromA(*memory.memoryMap[CPUreg.PC + 1]); CPUreg.PC += 2; CPUreg.cyclesAccumulated += 8; }

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
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA1(){ //AND C with A 4T 1PC
    andWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA2(){ //AND D with A 4T 1PC
    andWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA3(){ //AND E with A 4T 1PC
    andWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA4(){ //AND H with A 4T 1PC
    andWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA5(){ //AND L with A 4T 1PC
    andWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA6(){ //AND value at address HL with A 8T 1PC
    andWithA(*memory.memoryMap[CPUreg.hl.HL]); // perform AND operation with value at address HL
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 8;
}

void op_0xA7(){ //AND A with A 4T 1PC
    andWithA(CPUreg.af.A); // perform AND operation with itself
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xE6(){ //AND immediate value with A 8T 2PC
    uint8_t value = *memory.memoryMap[CPUreg.PC + 1]; // get immediate value
    andWithA(value); // perform AND operation with immediate value
    CPUreg.PC += 2; 
    CPUreg.cyclesAccumulated += 8;
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
    CPUreg.cyclesAccumulated += 4;
}

void op_0xA9(){ //XOR C with A 4T 1PC
    xorWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xAA(){ //XOR D with A 4T 1PC
    xorWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xAB(){ //XOR E with A 4T 1PC
    xorWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xAC(){ //XOR H with A 4T 1PC
    xorWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xAD(){ //XOR L with A 4T 1PC
    xorWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xAE(){ //XOR value at address HL with A 8T 1PC
    xorWithA(*memory.memoryMap[CPUreg.hl.HL]); // perform XOR operation with value at address HL
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 8;
}

void op_0xAF(){ //XOR A with A 4T 1PC
    xorWithA(CPUreg.af.A); // perform XOR operation with itself
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xEE(){ //XOR immediate value with A 8T 2PC
    uint8_t value = *memory.memoryMap[CPUreg.PC + 1]; // get immediate value
    xorWithA(value); // perform XOR operation with immediate value
    CPUreg.PC += 2; 
    CPUreg.cyclesAccumulated += 8;
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
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB1(){ //OR C with A 4T 1PC
    orWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB2(){ //OR D with A 4T 1PC
    orWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB3(){ //OR E with A 4T 1PC
    orWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB4(){ //OR H with A 4T 1PC
    orWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB5(){ //OR L with A 4T 1PC
    orWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB6(){ //OR value at address HL with A 8T 1PC
    orWithA(*memory.memoryMap[CPUreg.hl.HL]); // perform OR operation with value at address HL
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 8;
}

void op_0xB7(){ //OR A with A 4T 1PC
    orWithA(CPUreg.af.A); // perform OR operation with itself
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xF6(){ //OR immediate value with A 8T 2PC
    uint8_t value = *memory.memoryMap[CPUreg.PC + 1]; // get immediate value
    orWithA(value); // perform OR operation with immediate value
    CPUreg.PC += 2; 
    CPUreg.cyclesAccumulated += 8;
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
    CPUreg.cyclesAccumulated += 4;
}

void op_0xB9(){ //compare C with A 4T 1PC
    compareWithA(CPUreg.bc.C);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xBA(){ //compare D with A 4T 1PC
    compareWithA(CPUreg.de.D);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xBB(){ //compare E with A 4T 1PC
    compareWithA(CPUreg.de.E);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xBC(){ //compare H with A 4T 1PC
    compareWithA(CPUreg.hl.H);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xBD(){ //compare L with A 4T 1PC
    compareWithA(CPUreg.hl.L);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xBE(){ //compare value at address HL with A 8T 1PC
    compareWithA(*memory.memoryMap[CPUreg.hl.HL]); // perform comparison with value at address HL
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 8;
}

void op_0xBF(){ //compare A with A 4T 1PC
    compareWithA(CPUreg.af.A); // perform comparison with itself
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
}

void op_0xFE(){ //compare immediate value with A 8T 2PC
    uint8_t value = *memory.memoryMap[CPUreg.PC + 1]; // get immediate value
    compareWithA(value); // perform comparison with immediate value
    CPUreg.PC += 2; 
    CPUreg.cyclesAccumulated += 8;
}

//Purple instruction DAA, SCF, CCF, CPL

void op_0x37(){ //SCF 4T 1PC
    setCarryFlag(1); // set carry flag
    setHalfCarryFlag(0); // clear half carry flag
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    CPUreg.cyclesAccumulated += 4;
}

void op_0x3F(){ //CCF 4T 1PC
    setCarryFlag(!getCarryFlag()); // flip carry flag
    setHalfCarryFlag(0); // clear half carry flag
    setSubtractFlag(0); // clear subtract flag
    CPUreg.PC += 1; //increment PC by 1
    CPUreg.cyclesAccumulated += 4;
}

void op_0x2F(){ //CPL 4T 1PC
    CPUreg.af.A = ~CPUreg.af.A; // complement A (flip all bits 1s complement)
    setHalfCarryFlag(1); // set half carry flag
    setSubtractFlag(1); // set subtract flag
    CPUreg.PC += 1; //increment PC by 1
    CPUreg.cyclesAccumulated += 4;
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
    CPUreg.cyclesAccumulated += 4;
}

//Orange jump instructions (jump to address) with flags

void op_0xC2(){ //jump to 16 bit address if zero flag is 0 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getZeroFlag() == 0) { // if zero flag is not set
        CPUreg.PC = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1]; // jump to immediate address
        CPUreg.cyclesAccumulated += 16;
    } else { // if zero flag is set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if zero flag is set
        CPUreg.cyclesAccumulated += 12;
    }
}

void op_0xD2(){ //jump to 16 bit address if carry flag is 0 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getCarryFlag() == 0) { // if carry flag is not set
        CPUreg.PC = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1]; // jump to immediate address
        CPUreg.cyclesAccumulated += 16;
    } else { // if carry flag is set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if carry flag is set
        CPUreg.cyclesAccumulated += 12;
    }
}

void op_0xCA(){ //jump to 16 bit address if zero flag is 1 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getZeroFlag() == 1) { // if zero flag is set
        CPUreg.PC = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1]; // jump to immediate address
        CPUreg.cyclesAccumulated += 16;
    } else { // if zero flag is not set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if zero flag is not set
        CPUreg.cyclesAccumulated += 12;
    }
}

void op_0xDA(){ //jump to 16 bit address if carry flag is 1 otherwise do nothing 16/12T 3PC/Changes PC to immediate address
    if (getCarryFlag() == 1) { // if carry flag is set
        CPUreg.PC = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1]; // jump to immediate address
        CPUreg.cyclesAccumulated += 16;
    } else { // if carry flag is not set, skip jump
        CPUreg.PC += 3; // increment PC by 3 if carry flag is not set
        CPUreg.cyclesAccumulated += 12;
    }
}

void op_0xC3(){ //jump to 16 bit address unconditionally 16T Changes PC to immediate address
    CPUreg.PC = (*memory.memoryMap[CPUreg.PC + 2] << 8) | *memory.memoryMap[CPUreg.PC + 1]; // jump to immediate address
    CPUreg.cyclesAccumulated += 16;
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
    CPUreg.cyclesAccumulated += 4;
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
    CPUreg.cyclesAccumulated += 4;
}

void op_0x0F(){ //Rotate right 1 bit store LSB before rotate in carr flag and old LSB is new MSB
    setZeroFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);
    uint8_t LSB = CPUreg.af.A & 0x01;
    setCarryFlag(LSB);
    CPUreg.af.A = (LSB << 7) | (CPUreg.af.A >> 1);
    CPUreg.PC += 1; 
    CPUreg.cyclesAccumulated += 4;
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
    CPUreg.cyclesAccumulated += 4;
}

//Start of CB instructions
void rotateLeft(uint8_t *dest){ 

}

//Rotate left through carry, store MSB in carry flag and rotate LSB to MSB

void rlc(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | msb;
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB00() { rlc(&CPUreg.bc.B, false); }
void op_0xCB01() { rlc(&CPUreg.bc.C, false); }
void op_0xCB02() { rlc(&CPUreg.de.D, false); }
void op_0xCB03() { rlc(&CPUreg.de.E, false); }
void op_0xCB04() { rlc(&CPUreg.hl.H, false); }
void op_0xCB05() { rlc(&CPUreg.hl.L, false); }
void op_0xCB06() { rlc(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB07() { rlc(&CPUreg.af.A, false); }


 //Rotate right through carry, store LSB in carry flag and rotate MSB to LSB

void rrc(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value = (value >> 1) | (lsb << 7);
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB08() { rrc(&CPUreg.bc.B, false); }
void op_0xCB09() { rrc(&CPUreg.bc.C, false); }
void op_0xCB0A() { rrc(&CPUreg.de.D, false); }
void op_0xCB0B() { rrc(&CPUreg.de.E, false); }
void op_0xCB0C() { rrc(&CPUreg.hl.H, false); }
void op_0xCB0D() { rrc(&CPUreg.hl.L, false); }
void op_0xCB0E() { rrc(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB0F() { rrc(&CPUreg.af.A, false); }

// RL 

void rl(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t msb = (value >> 7) & 0x01;
    value = (value << 1) | getCarryFlag();
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB10() { rl(&CPUreg.bc.B, false); }
void op_0xCB11() { rl(&CPUreg.bc.C, false); }
void op_0xCB12() { rl(&CPUreg.de.D, false); }
void op_0xCB13() { rl(&CPUreg.de.E, false); }
void op_0xCB14() { rl(&CPUreg.hl.H, false); }
void op_0xCB15() { rl(&CPUreg.hl.L, false); }
void op_0xCB16() { rl(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB17() { rl(&CPUreg.af.A, false); }
// RR

void rr(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value = (value >> 1) | (getCarryFlag() << 7);
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB18() { rr(&CPUreg.bc.B, false); }
void op_0xCB19() { rr(&CPUreg.bc.C, false); }
void op_0xCB1A() { rr(&CPUreg.de.D, false); }
void op_0xCB1B() { rr(&CPUreg.de.E, false); }
void op_0xCB1C() { rr(&CPUreg.hl.H, false); }
void op_0xCB1D() { rr(&CPUreg.hl.L, false); }
void op_0xCB1E() { rr(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB1F() { rr(&CPUreg.af.A, false); }

//SLA 

void sla(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t msb = (value >> 7) & 0x01;
    value <<= 1;
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(msb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB20() { sla(&CPUreg.bc.B, false); }
void op_0xCB21() { sla(&CPUreg.bc.C, false); }
void op_0xCB22() { sla(&CPUreg.de.D, false); }
void op_0xCB23() { sla(&CPUreg.de.E, false); }
void op_0xCB24() { sla(&CPUreg.hl.H, false); }
void op_0xCB25() { sla(&CPUreg.hl.L, false); }
void op_0xCB26() { sla(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB27() { sla(&CPUreg.af.A, false); }


//SRA

void sra(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value = (value >> 1) | (value & 0x80);
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB28() { sra(&CPUreg.bc.B, false); }
void op_0xCB29() { sra(&CPUreg.bc.C, false); }
void op_0xCB2A() { sra(&CPUreg.de.D, false); }
void op_0xCB2B() { sra(&CPUreg.de.E, false); }
void op_0xCB2C() { sra(&CPUreg.hl.H, false); }
void op_0xCB2D() { sra(&CPUreg.hl.L, false); }
void op_0xCB2E() { sra(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB2F() { sra(&CPUreg.af.A, false); }

//SWAP

void swap(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4);
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(0);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB30() { swap(&CPUreg.bc.B, false); }
void op_0xCB31() { swap(&CPUreg.bc.C, false); }
void op_0xCB32() { swap(&CPUreg.de.D, false); }
void op_0xCB33() { swap(&CPUreg.de.E, false); }
void op_0xCB34() { swap(&CPUreg.hl.H, false); }
void op_0xCB35() { swap(&CPUreg.hl.L, false); }
void op_0xCB36() { swap(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB37() { swap(&CPUreg.af.A, false); }

//SRL

void srl(uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    uint8_t lsb = value & 0x01;
    value >>= 1;
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }

    setZeroFlag(value == 0);
    setCarryFlag(lsb);
    setHalfCarryFlag(0);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

void op_0xCB38() { srl(&CPUreg.bc.B, false); }
void op_0xCB39() { srl(&CPUreg.bc.C, false); }
void op_0xCB3A() { srl(&CPUreg.de.D, false); }
void op_0xCB3B() { srl(&CPUreg.de.E, false); }
void op_0xCB3C() { srl(&CPUreg.hl.H, false); }
void op_0xCB3D() { srl(&CPUreg.hl.L, false); }
void op_0xCB3E() { srl(memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB3F() { srl(&CPUreg.af.A, false); }

//BIT

void bitTest(uint8_t bit, uint8_t value, bool isMemory) {
    setZeroFlag(((value >> bit) & 0x01) == 0);
    setHalfCarryFlag(1);
    setSubtractFlag(0);

    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 8 : 4;
}

// BIT 0
void op_0xCB40() { bitTest(0, CPUreg.bc.B, false); }
void op_0xCB41() { bitTest(0, CPUreg.bc.C, false); }
void op_0xCB42() { bitTest(0, CPUreg.de.D, false); }
void op_0xCB43() { bitTest(0, CPUreg.de.E, false); }
void op_0xCB44() { bitTest(0, CPUreg.hl.H, false); }
void op_0xCB45() { bitTest(0, CPUreg.hl.L, false); }
void op_0xCB46() { bitTest(0, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB47() { bitTest(0, CPUreg.af.A, false); }

// BIT 1
void op_0xCB48() { bitTest(1, CPUreg.bc.B, false); }
void op_0xCB49() { bitTest(1, CPUreg.bc.C, false); }
void op_0xCB4A() { bitTest(1, CPUreg.de.D, false); }
void op_0xCB4B() { bitTest(1, CPUreg.de.E, false); }
void op_0xCB4C() { bitTest(1, CPUreg.hl.H, false); }
void op_0xCB4D() { bitTest(1, CPUreg.hl.L, false); }
void op_0xCB4E() { bitTest(1, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB4F() { bitTest(1, CPUreg.af.A, false); }

// BIT 2
void op_0xCB50() { bitTest(2, CPUreg.bc.B, false); }
void op_0xCB51() { bitTest(2, CPUreg.bc.C, false); }
void op_0xCB52() { bitTest(2, CPUreg.de.D, false); }
void op_0xCB53() { bitTest(2, CPUreg.de.E, false); }
void op_0xCB54() { bitTest(2, CPUreg.hl.H, false); }
void op_0xCB55() { bitTest(2, CPUreg.hl.L, false); }
void op_0xCB56() { bitTest(2, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB57() { bitTest(2, CPUreg.af.A, false); }

// BIT 3
void op_0xCB58() { bitTest(3, CPUreg.bc.B, false); }
void op_0xCB59() { bitTest(3, CPUreg.bc.C, false); }
void op_0xCB5A() { bitTest(3, CPUreg.de.D, false); }
void op_0xCB5B() { bitTest(3, CPUreg.de.E, false); }
void op_0xCB5C() { bitTest(3, CPUreg.hl.H, false); }
void op_0xCB5D() { bitTest(3, CPUreg.hl.L, false); }
void op_0xCB5E() { bitTest(3, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB5F() { bitTest(3, CPUreg.af.A, false); }

// BIT 4
void op_0xCB60() { bitTest(4, CPUreg.bc.B, false); }
void op_0xCB61() { bitTest(4, CPUreg.bc.C, false); }
void op_0xCB62() { bitTest(4, CPUreg.de.D, false); }
void op_0xCB63() { bitTest(4, CPUreg.de.E, false); }
void op_0xCB64() { bitTest(4, CPUreg.hl.H, false); }
void op_0xCB65() { bitTest(4, CPUreg.hl.L, false); }
void op_0xCB66() { bitTest(4, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB67() { bitTest(4, CPUreg.af.A, false); }

// BIT 5
void op_0xCB68() { bitTest(5, CPUreg.bc.B, false); }
void op_0xCB69() { bitTest(5, CPUreg.bc.C, false); }
void op_0xCB6A() { bitTest(5, CPUreg.de.D, false); }
void op_0xCB6B() { bitTest(5, CPUreg.de.E, false); }
void op_0xCB6C() { bitTest(5, CPUreg.hl.H, false); }
void op_0xCB6D() { bitTest(5, CPUreg.hl.L, false); }
void op_0xCB6E() { bitTest(5, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB6F() { bitTest(5, CPUreg.af.A, false); }

// BIT 6
void op_0xCB70() { bitTest(6, CPUreg.bc.B, false); }
void op_0xCB71() { bitTest(6, CPUreg.bc.C, false); }
void op_0xCB72() { bitTest(6, CPUreg.de.D, false); }
void op_0xCB73() { bitTest(6, CPUreg.de.E, false); }
void op_0xCB74() { bitTest(6, CPUreg.hl.H, false); }
void op_0xCB75() { bitTest(6, CPUreg.hl.L, false); }
void op_0xCB76() { bitTest(6, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB77() { bitTest(6, CPUreg.af.A, false); }

// BIT 7
void op_0xCB78() { bitTest(7, CPUreg.bc.B, false); }
void op_0xCB79() { bitTest(7, CPUreg.bc.C, false); }
void op_0xCB7A() { bitTest(7, CPUreg.de.D, false); }
void op_0xCB7B() { bitTest(7, CPUreg.de.E, false); }
void op_0xCB7C() { bitTest(7, CPUreg.hl.H, false); }
void op_0xCB7D() { bitTest(7, CPUreg.hl.L, false); }
void op_0xCB7E() { bitTest(7, *memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB7F() { bitTest(7, CPUreg.af.A, false); }



//RES

void resBit(uint8_t bit, uint8_t* reg, bool isMemory) {
    uint8_t value = *reg; // get current value of the register or memory location
    value &= ~(1 << bit); // clear the specified bit
    if (isMemory) {

        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

// RES 0
void op_0xCB80() { resBit(0, &CPUreg.bc.B, false); }
void op_0xCB81() { resBit(0, &CPUreg.bc.C, false); }
void op_0xCB82() { resBit(0, &CPUreg.de.D, false); }
void op_0xCB83() { resBit(0, &CPUreg.de.E, false); }
void op_0xCB84() { resBit(0, &CPUreg.hl.H, false); }
void op_0xCB85() { resBit(0, &CPUreg.hl.L, false); }
void op_0xCB86() { resBit(0, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB87() { resBit(0, &CPUreg.af.A, false); }

// RES 1
void op_0xCB88() { resBit(1, &CPUreg.bc.B, false); }
void op_0xCB89() { resBit(1, &CPUreg.bc.C, false); }
void op_0xCB8A() { resBit(1, &CPUreg.de.D, false); }
void op_0xCB8B() { resBit(1, &CPUreg.de.E, false); }
void op_0xCB8C() { resBit(1, &CPUreg.hl.H, false); }
void op_0xCB8D() { resBit(1, &CPUreg.hl.L, false); }
void op_0xCB8E() { resBit(1, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB8F() { resBit(1, &CPUreg.af.A, false); }

// RES 2
void op_0xCB90() { resBit(2, &CPUreg.bc.B, false); }
void op_0xCB91() { resBit(2, &CPUreg.bc.C, false); }
void op_0xCB92() { resBit(2, &CPUreg.de.D, false); }
void op_0xCB93() { resBit(2, &CPUreg.de.E, false); }
void op_0xCB94() { resBit(2, &CPUreg.hl.H, false); }
void op_0xCB95() { resBit(2, &CPUreg.hl.L, false); }
void op_0xCB96() { resBit(2, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB97() { resBit(2, &CPUreg.af.A, false); }

// RES 3
void op_0xCB98() { resBit(3, &CPUreg.bc.B, false); }
void op_0xCB99() { resBit(3, &CPUreg.bc.C, false); }
void op_0xCB9A() { resBit(3, &CPUreg.de.D, false); }
void op_0xCB9B() { resBit(3, &CPUreg.de.E, false); }
void op_0xCB9C() { resBit(3, &CPUreg.hl.H, false); }
void op_0xCB9D() { resBit(3, &CPUreg.hl.L, false); }
void op_0xCB9E() { resBit(3, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCB9F() { resBit(3, &CPUreg.af.A, false); }

// RES 4
void op_0xCBA0() { resBit(4, &CPUreg.bc.B, false); }
void op_0xCBA1() { resBit(4, &CPUreg.bc.C, false); }
void op_0xCBA2() { resBit(4, &CPUreg.de.D, false); }
void op_0xCBA3() { resBit(4, &CPUreg.de.E, false); }
void op_0xCBA4() { resBit(4, &CPUreg.hl.H, false); }
void op_0xCBA5() { resBit(4, &CPUreg.hl.L, false); }
void op_0xCBA6() { resBit(4, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBA7() { resBit(4, &CPUreg.af.A, false); }

// RES 5
void op_0xCBA8() { resBit(5, &CPUreg.bc.B, false); }
void op_0xCBA9() { resBit(5, &CPUreg.bc.C, false); }
void op_0xCBAA() { resBit(5, &CPUreg.de.D, false); }
void op_0xCBAB() { resBit(5, &CPUreg.de.E, false); }
void op_0xCBAC() { resBit(5, &CPUreg.hl.H, false); }
void op_0xCBAD() { resBit(5, &CPUreg.hl.L, false); }
void op_0xCBAE() { resBit(5, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBAF() { resBit(5, &CPUreg.af.A, false); }

// RES 6
void op_0xCBB0() { resBit(6, &CPUreg.bc.B, false); }
void op_0xCBB1() { resBit(6, &CPUreg.bc.C, false); }
void op_0xCBB2() { resBit(6, &CPUreg.de.D, false); }
void op_0xCBB3() { resBit(6, &CPUreg.de.E, false); }
void op_0xCBB4() { resBit(6, &CPUreg.hl.H, false); }
void op_0xCBB5() { resBit(6, &CPUreg.hl.L, false); }
void op_0xCBB6() { resBit(6, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBB7() { resBit(6, &CPUreg.af.A, false); }

// RES 7
void op_0xCBB8() { resBit(7, &CPUreg.bc.B, false); }
void op_0xCBB9() { resBit(7, &CPUreg.bc.C, false); }
void op_0xCBBA() { resBit(7, &CPUreg.de.D, false); }
void op_0xCBBB() { resBit(7, &CPUreg.de.E, false); }
void op_0xCBBC() { resBit(7, &CPUreg.hl.H, false); }
void op_0xCBBD() { resBit(7, &CPUreg.hl.L, false); }
void op_0xCBBE() { 
    resBit(7, memory.memoryMap[CPUreg.hl.HL], true);
}
void op_0xCBBF() { resBit(7, &CPUreg.af.A, false); }

//SET

void setBit(uint8_t bit, uint8_t* reg, bool isMemory) {
    uint8_t value = *reg;
    value |= (1 << bit);
    if (isMemory) {
        LDVal8(value, memory.memoryMap[CPUreg.hl.HL], CPUreg.hl.HL, 1, 0xFFFF); // if memory, store value in memory
    }
    else{
        *reg = value;
    }
    CPUreg.PC += 1;
    CPUreg.cyclesAccumulated += isMemory ? 12 : 4;
}

// SET 0
void op_0xCBC0() { setBit(0, &CPUreg.bc.B, false); }
void op_0xCBC1() { setBit(0, &CPUreg.bc.C, false); }
void op_0xCBC2() { setBit(0, &CPUreg.de.D, false); }
void op_0xCBC3() { setBit(0, &CPUreg.de.E, false); }
void op_0xCBC4() { setBit(0, &CPUreg.hl.H, false); }
void op_0xCBC5() { setBit(0, &CPUreg.hl.L, false); }
void op_0xCBC6() { setBit(0, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBC7() { setBit(0, &CPUreg.af.A, false); }

// SET 1
void op_0xCBC8() { setBit(1, &CPUreg.bc.B, false); }
void op_0xCBC9() { setBit(1, &CPUreg.bc.C, false); }
void op_0xCBCA() { setBit(1, &CPUreg.de.D, false); }
void op_0xCBCB() { setBit(1, &CPUreg.de.E, false); }
void op_0xCBCC() { setBit(1, &CPUreg.hl.H, false); }
void op_0xCBCD() { setBit(1, &CPUreg.hl.L, false); }
void op_0xCBCE() { setBit(1, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBCF() { setBit(1, &CPUreg.af.A, false); }

// SET 2
void op_0xCBD0() { setBit(2, &CPUreg.bc.B, false); }
void op_0xCBD1() { setBit(2, &CPUreg.bc.C, false); }
void op_0xCBD2() { setBit(2, &CPUreg.de.D, false); }
void op_0xCBD3() { setBit(2, &CPUreg.de.E, false); }
void op_0xCBD4() { setBit(2, &CPUreg.hl.H, false); }
void op_0xCBD5() { setBit(2, &CPUreg.hl.L, false); }
void op_0xCBD6() { setBit(2, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBD7() { setBit(2, &CPUreg.af.A, false); }

// SET 3
void op_0xCBD8() { setBit(3, &CPUreg.bc.B, false); }
void op_0xCBD9() { setBit(3, &CPUreg.bc.C, false); }
void op_0xCBDA() { setBit(3, &CPUreg.de.D, false); }
void op_0xCBDB() { setBit(3, &CPUreg.de.E, false); }
void op_0xCBDC() { setBit(3, &CPUreg.hl.H, false); }
void op_0xCBDD() { setBit(3, &CPUreg.hl.L, false); }
void op_0xCBDE() { setBit(3, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBDF() { setBit(3, &CPUreg.af.A, false); }

// SET 4
void op_0xCBE0() { setBit(4, &CPUreg.bc.B, false); }
void op_0xCBE1() { setBit(4, &CPUreg.bc.C, false); }
void op_0xCBE2() { setBit(4, &CPUreg.de.D, false); }
void op_0xCBE3() { setBit(4, &CPUreg.de.E, false); }
void op_0xCBE4() { setBit(4, &CPUreg.hl.H, false); }
void op_0xCBE5() { setBit(4, &CPUreg.hl.L, false); }
void op_0xCBE6() { setBit(4, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBE7() { setBit(4, &CPUreg.af.A, false); }

// SET 5
void op_0xCBE8() { setBit(5, &CPUreg.bc.B, false); }
void op_0xCBE9() { setBit(5, &CPUreg.bc.C, false); }
void op_0xCBEA() { setBit(5, &CPUreg.de.D, false); }
void op_0xCBEB() { setBit(5, &CPUreg.de.E, false); }
void op_0xCBEC() { setBit(5, &CPUreg.hl.H, false); }
void op_0xCBED() { setBit(5, &CPUreg.hl.L, false); }
void op_0xCBEE() { setBit(5, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBEF() { setBit(5, &CPUreg.af.A, false); }

// SET 6
void op_0xCBF0() { setBit(6, &CPUreg.bc.B, false); }
void op_0xCBF1() { setBit(6, &CPUreg.bc.C, false); }
void op_0xCBF2() { setBit(6, &CPUreg.de.D, false); }
void op_0xCBF3() { setBit(6, &CPUreg.de.E, false); }
void op_0xCBF4() { setBit(6, &CPUreg.hl.H, false); }
void op_0xCBF5() { setBit(6, &CPUreg.hl.L, false); }
void op_0xCBF6() { setBit(6, memory.memoryMap[CPUreg.hl.HL], true); }
void op_0xCBF7() { setBit(6, &CPUreg.af.A, false); }

// SET 7
void op_0xCBF8() { setBit(7, &CPUreg.bc.B, false); }
void op_0xCBF9() { setBit(7, &CPUreg.bc.C, false); }
void op_0xCBFA() { setBit(7, &CPUreg.de.D, false); }
void op_0xCBFB() { setBit(7, &CPUreg.de.E, false); }
void op_0xCBFC() { setBit(7, &CPUreg.hl.H, false); }
void op_0xCBFD() { setBit(7, &CPUreg.hl.L, false); }
void op_0xCBFE() { 
    setBit(7, memory.memoryMap[CPUreg.hl.HL], true);

 }
void op_0xCBFF() { setBit(7, &CPUreg.af.A, false); }

void op_0xCB(){
    CPUreg.CBFlag = 1;
    CPUreg.PC += 1; //increment PC by 1
    CPUreg.cyclesAccumulated += 4; //CB instruction takes 4 cycles at start
    //getchar();
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
            //exit(1); // Exit if an unknown opcode is encountered
            //CPUreg.PC += 1; // increment PC to skip unknown opcode
            break;
    }
}

void handleInterrupts() {
    uint8_t IE = *memory.memoryMap[0xFFFF]; // Interrupt Enable
    uint8_t IF = *memory.memoryMap[0xFF0F]; // Interrupt Flag

    if (CPUreg.haltMode == 1 && (IE & IF & 0x1F) != 0) {
        CPUreg.haltMode = 0; // CPU will resume 
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
           // printf("IF BEFORE: 0x%02X\n", *memory.memoryMap[0xFF0F]);

            *memory.memoryMap[0xFF0F] &= ~interrupts[i].mask; // Clear IF


            //fired = *memory.memoryMap[0xFFFF] & *memory.memoryMap[0xFF0F];
            //if (!fired) return;

            // Push PC to stack (high byte first)
            *memory.memoryMap[--CPUreg.SP] = (CPUreg.PC >> 8) & 0xFF;
            *memory.memoryMap[--CPUreg.SP] = CPUreg.PC & 0xFF;

            CPUreg.PC = interrupts[i].vector; // Jump to interrupt vector
            CPUreg.cyclesAccumulated += 20; // Interrupt takes 20 cycles
            /*
            if (i==4){
                printf("Interrupt %d fired, PC set to %04X\n", i, CPUreg.PC);
            }
                */
            //printf("INT fired: type=%d, pushing PC=%04X to SP=%04X\n", i, CPUreg.PC, CPUreg.SP);
            //printf("[INTERRUPT] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) \n",*memory.memoryMap[0xFFFF], *memory.memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC);
            //printf("IF AFTER:  0x%02X\n", *memoryMap[0xFF0F]);
            //fprintf(logFile, "[INTERRUPT] IE=0x%02X IF=0x%02X IME=%d (PC=%04X) Cycles=%d\n", *memoryMap[0xFFFF], *memoryMap[0xFF0F], CPUreg.IME, CPUreg.PC, realCyclesAccumulated);

            return; // Only handle one interrupt at a time
        }
    }
}

void stepCPU(){
    if(CPUreg.CBFlag != 1){ //make sure interrupt not called before CB instruction finishes execution
        handleInterrupts();
        }

    // Fetch opcode
    uint8_t opcode = *memory.memoryMap[CPUreg.PC];

    // Execute opcode
    if (CPUreg.CPUtimer > 0) {
        CPUreg.CPUtimer--;
    }
    if (CPUreg.CPUtimer == 0){
                //fprintf(logFile, "PC: %04X, Opcode: %02X, Cycles: %d, SP: %04X, A: %02X, B: %02X, C: %02X, D: %02X, E: %02X, F: %02X, H: %02X, L: %02X, LY: %02X, LYC: %02X, FF80: %02X, FF85: %02X, FF00: %02X, FFFF: %02X, FF0F: %02X, FF40: %02X, FF41: %02X, FF44: %02X, FF45: %02X\n", CPUreg.PC, opcode, realCyclesAccumulated, CPUreg.SP, CPUreg.af.A, CPUreg.bc.B, CPUreg.bc.C, CPUreg.de.D, CPUreg.de.E, CPUreg.af.F, CPUreg.hl.H, CPUreg.hl.L, *(ppu.ly), *(ppu.lyc), *memoryMap[0xFF80], *memoryMap[0xFF85], *memoryMap[0xFF00], *memoryMap[0xFFFF], *memoryMap[0xFF0F], *memoryMap[0xFF40], *memoryMap[0xFF41], *memoryMap[0xFF44], *memoryMap[0xFF45]);
                //fprintf(logFile, "Mode 1 Timer: %d, Mode 0 Timer: %d, Mode 2 Timer: %d, Mode 3 Timer: %d, xPos: %d, scanlineTimer: %d\n", mode1Timer, mode0Timer, mode2Timer, mode3Timer, xPos, scanlineTimer);
                //halt mode 1 handled in interrupt handler
                //fprintf(logFile, "Haltmode %d, CPUtimer: %d\n", haltMode, CPUtimer);

            if(CPUreg.haltMode != 1){ //execute CPU only if haltMode is not 1 (CPU not halted)

            if (CPUreg.haltMode == 2) {
                // HALT bug: execute same instruction twice
                //pcBacktrace[backtraceIndex] = CPUreg.PC - 1;
            // backtraceIndex = (backtraceIndex + 1) % 8;
                if (CPUreg.EIFlag == 1) CPUreg.EIFlag = -1; //after EI flag set to -1 so interrupt enabled after 1 instruction delay
                else if (CPUreg.EIFlag == -1 && CPUreg.CBFlag == 0) { 
                    CPUreg.IME = 1;
                    CPUreg.EIFlag = 0;
                }

                CPUreg.PC--; //set PC back by one so byte is read twice
                executeOpcode(opcode);

                CPUreg.CPUtimer = CPUreg.cyclesAccumulated;
                CPUreg.haltMode = 0;
                CPUreg.cyclesAccumulated = 0;
            } else {
                // --- Normal CPU execution ---
                    if (CPUreg.CBFlag == 0) {
                        if (CPUreg.EIFlag == 1) CPUreg.EIFlag = -1;
                        else if (CPUreg.EIFlag == -1 && CPUreg.CBFlag == 0) {
                            CPUreg.IME = 1;
                            CPUreg.EIFlag = 0;
                        }
                        //printf("Executing opcode %02X at PC=%04X\n", opcode, CPUreg.PC);
                        executeOpcode(opcode);
                        CPUreg.CPUtimer = CPUreg.cyclesAccumulated;
                    } else {
                        if (CPUreg.EIFlag == -1){
                            CPUreg.IME = 1;
                            CPUreg.EIFlag = 0;
                        } 
                        executeOpcodeCB(opcode);
                        //printf("Executing opcode %02X at PC=%04X\n", opcode, CPUreg.PC);
                        CPUreg.CPUtimer = CPUreg.cyclesAccumulated;
                        CPUreg.CBFlag = 0;
                    }

                    CPUreg.cyclesAccumulated = 0;

                }
            }
        }     

}