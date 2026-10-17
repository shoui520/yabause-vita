# Yabause-Vita
Yet Another Broken And Useless Saturn Emulator, for PS Vita.

Based on [libretro/yabause (yabasanshiro)](https://github.com/libretro/yabause/tree/yabasanshiro)  

**Current status**: Now plays many simpler games at full speed and has a GUI, but it's still an **unfinished, developer-oriented** emulator. Some 3D games are known to have graphical glitches.  
Still undergoing development. Do not mirror .vpk's.

I recommend you build the .vpk from source. Prebuilt .vpk's from [Releases](https://github.com/shoui520/yabause-vita/releases) may not be up to date.  

## Usage

Prepare a Sega Saturn BIOS image in `ux0:data/yabause-vita/bios/`  
Put games in `ux0:data/yabause-vita/disc/`  

Controls, BIOS, region, and cartridge settings can be changed in the settings menu.  

## Advice for end users

Make sure you're on max overclock using PSVshell: 500 MHz/222 MHz/166 MHz CPU/GPU/XBR.    

This emulator is unfinished and still in development, do not expect full stability. **I highly recommend you keep save states.** You can make save states by opening the SELECT menu in game.  
If your emulator crashes when using the SELECT menu, it could be caused by having too many plugins. 

PAL (Europe) region games are not recommended as they're untested, please use NTSC (Japan, USA) region games with this emulator.   

If you hear audio choppiness, it's because the game is not running at full speed. Check the PSVshell overlay for an FPS figure.


If you get a crash, open an issue with the game name + region and attach the relevant `.psp2dmp` to the issue. If applicable, also include the `.yss` save state right before the issue occurs.