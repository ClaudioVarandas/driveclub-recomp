# DriveClub Recomp

**DriveClub for PS4 (version 1.28), running natively on Windows.**

<img width="1920" height="1080" alt="s009 20" src="https://github.com/user-attachments/assets/4c0fe155-eae9-4c78-aa24-4a5b154e2b9f" />

The source code is in this repository and the build is on the [Releases](../../releases) page.

## How to play
1. Download `driveclub-recomp-0.1.0-win64.zip` and extract it anywhere you can write to (not `Program Files`).
2. Have **your own** game dumps ready:
   - the **version 1.28 update** folder (it contains `eboot.bin`)
   - the **base game** folder (it contains `game000.dat`)
3. Double-click `DriveClub.exe`, press **Browse** next to each folder, check the settings and press **Play**.

Nothing to install. Your own files are never modified or copied, and the folders you chose are remembered.

Logs are in the `logs` folder.

## Highlights
- The game's own x86-64 code runs directly on your CPU, with no CPU emulation. The PS4 system libraries are reimplemented and the GPU commands are translated to Vulkan, similar to Wine and DXVK
- Playable from the menus through races, photo mode and saves
- About 60 FPS at 4K in the heaviest scenes on our test PC (RTX 5070 Ti, Ryzen 7 5800X3D), higher in lighter ones, with an unlocked frame rate option in the launcher that keeps the game at its normal speed
- Runs on NVIDIA, AMD and Intel graphics through Vulkan, with Resizable BAR used automatically where the driver allows it
- Keyboard racing layout and any gamepad (Xbox, PlayStation, Switch Pro, 8BitDo)

## Controls
Any gamepad works. Keyboard, mouse and gamepad can be used at the same time.

| Action | Keyboard |
|---|---|
| Accelerate | W |
| Brake / reverse | S |
| Steer | A / D |
| Handbrake | Space |
| Shift down / up | Q / E |
| Change camera | C |
| Touchpad left / right half | R / T (R is Reset to Track) |
| Accept | Enter / Z |
| Back | Backspace |
| Pause | Esc / P |
| D-pad | Arrow keys |
| Look around | I J K L or the mouse |

F7 turns mouse look on or off. The bindings are in `data\user\input_config\default.ini` and can be changed there.

## Settings
The window that opens when you start `DriveClub.exe` has every setting. Press **Play** to save them; they are kept for later starts.

**Performance preset:** Handheld, Low, Medium, High or Ultra pick the render and output sizes for your PC. You can still change each size afterwards.

**Frame rate:** 30, 60 (deltatime), 60 with a fixed tick, or **Unlocked**. Unlocked has no frame cap and the game keeps its normal speed.

**Resolution:** output from 720p to 8K, render size from 720p to 8K, and ultrawide sizes with a field-of-view fix. Sizes above 1080p use more memory; the needed memory patch is added for you.

**Mirror and reflection updates:** **Live** (default) draws the small scene of mirrors, rain and reflections every frame. **Static (faster)** draws it once and keeps that picture, which is much faster but stops reflections and rain on the glass from updating. The Handheld preset uses it.

**Gameplay:** player name, borderless fullscreen, FPS counter, motion blur and depth of field off, no reset-to-track timer, no track barriers, and photo mode options.

**Skip the graphics card check:** the launcher checks that your card can run the game (Vulkan 1.3). If it ever refuses a card that works, turn this on to start anyway.

## Known issues
- **Some reflections are too bright**, for example rain drops on the side window and the paint on cars on the stage select screen.
- **Static (faster)** stops reflections and rain on the glass from updating.
- **The processor is the limit.** Heavy scenes (many cars, trees and rain) drop frames first on slower CPUs.
- **First play builds shaders:** short stutters in the first minutes until they are cached. This gets better on later runs.
- **Online features do not work.**
- Tested on an NVIDIA graphics card only. AMD and Intel are untested.

## Notes
- You must supply your own, legally obtained dump of the game. No game files are included.
- **Tested with:** DriveClub, title ID `CUSA00093`, version 1.28. Other versions and repacked dumps have not been tested and may not work.
- SmartScreen may warn (*More info*, then *Run anyway*). Verify with the checksum.
- Requires Windows 10/11 64-bit and a graphics card with a Vulkan 1.3 driver. About 40 GB of free space is needed for the game dumps.

## Building from source
See [BUILDING.md](BUILDING.md).

## Reporting problems
Open an [issue](../../issues) and attach the newest file from the `logs` folder, with your graphics card, driver version and what happened.

## Credits
The community patches for DriveClub are by Kravickas, TonySSS, ZipperedRain, kalaposfos, squidbus, illusion and u/Charlie_Muggins.

Also used: [SDL3](https://github.com/libsdl-org/SDL) and [FFmpeg](https://ffmpeg.org).

## License
GNU General Public License version 2 or later ([LICENSE](LICENSE)). Parts of the code derive from other GPL projects and keep their original notices in the source files. Third-party components keep their own licenses ([THIRD_PARTY_NOTICES.txt](packaging/release/THIRD_PARTY_NOTICES.txt)).

DriveClub is a trademark and copyright of its respective owners; this project is not affiliated with or endorsed by them.
