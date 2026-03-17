# GameBoy-Emulator
Prototype Game Boy emulator

# To be implemented
- BOOT sequence
- Audio
- Sprite FIFO for more accurate PPU
- Quick note to self before I forget yet again, ppu and cpu step work per t cycle, moved the sdl display logic inside stepppu - automatically renders new frame every vblank end, make main loop happen 4.19mhz or whatever the gameboy clock frequency was
