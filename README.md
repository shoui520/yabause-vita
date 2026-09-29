# Yabause-Vita
Yet Another Broken And Useless Saturn Emulator, for PS Vita.

Based on [libretro/yabause (yabasanshiro)](https://github.com/libretro/yabause/tree/yabasanshiro)  

**Current status**: Now plays many games at full speed, but it's still an unfinished, developer oriented emulator.

## Running

Prepare `sega_101.bin` as `ux0:data/yabause-vita/bios.bin`.    

Edit `ux0:data/yabause-vita/boot-path.txt` to a `.cue` file of a Sega Saturn disc. Example:
```text
ux0:data/yabause-vita/disc/Daytona USA (Japan)/Daytona USA (Japan).cue
```

Then launch Yabause Vita. 

yabause-vita uses vitaGL, so `libshaccCg.suprx` is required to use this emulator.  

Default pad mapping:
* D-pad: D-pad
* SELECT: Create savestate
* START: START
* Square: X
* Triangle: Y
* L: Z
* Cross: A
* Circle: B
* R: C 
* Unmapped: L & R
