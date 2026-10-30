# ForzaHorizon2 ExtraOptions - Camera Mods

Fork/add-on of [ForzaHorizon2_ExtraOptions](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) by RokkuDayo
(GPL-licensed, see LICENSE). All credit for the base mod goes to the original author.
This fork adds extra cameras for the Azimuth (PC) version of Forza Horizon 2.

**Current version: v1.0.4** - based on Extra Options 0.3.9, tested with Azimuth 1.0.3.

**Publisher / Author of the camera mods:** Deuquan ([GitHub](https://github.com/Deuquan/FH2-CameraMods))

> Unofficial. Not affiliated with the original author. Use at your own risk, offline / single player only.

## Features
- **Drone / free cam** - fly around anywhere (from the driver, hood or bumper view)
- **Walk mode** - walk around on foot, optionally follow the car
- **Passenger cam** - fixed head position in the passenger seat (switch side with `C`)
- **Driver seat cam** - extra "enhanced" driver camera
- **360 cam** - fixed camera above and behind the car at 0.5x zoom, with speed/terrain shake
- **Enhanced chase cam** - shake/bobbing effect on the normal chase cameras
- **Cinema mode** (passenger), look limits, auto-recenter, per-camera saved seat positions
- Controller support (LB cycles the modded cameras)

## Install
1. Download the ZIP from the Releases page and extract it anywhere.
2. Double-click `install.bat`. If it doesn't find `azimuth.exe` next to itself, a folder window opens - select your
   Azimuth folder (the one that contains `azimuth.exe`). The installer backs up your current `winmm.dll` and installs
   the mod. Your existing `FH2_ExtraOptions.ini` is kept if the mod accepts it; otherwise it is saved as
   `FH2_ExtraOptions.ini.old` and the default INI is installed.
   - Windows may warn that the publisher could not be verified. The scripts are plain text and not digitally signed,
     so click **Run** / **More info -> Run anyway**. You can open the `.bat` files in Notepad to check them.
3. Start the game via Azimuth as usual.

`uninstall.bat` restores your original `winmm.dll`. `update.bat` downloads the newest release (see below).

Requirement: `FH2_ExtraOptions.ini` must contain `[Autodrive]` / `Enabled=1` (the default INI does).

## Controls (keyboard)
| Key | Action |
|---|---|
| `O` | Drone on/off (start from driver, hood or bumper view) |
| `W A S D` | Move, mouse = look |
| `E` / `R` | Up (walk mode: raise ground height - use it on slopes) |
| `Q` / `F` | Down |
| `Shift` / `Ctrl` | Fast / very slow |
| Mouse wheel, `PgUp`/`PgDn` | Zoom |
| `P` | Walk mode |
| `V` | Walk-follow the car |
| `G` | Keep the car in view |
| `M` | Passenger cam |
| `C` | Switch passenger side |
| `L` | Cinema mode (passenger) |
| `H` | Driver seat cam |
| `X` | 360 cam |
| `U` | Shake on/off (off by default) |
| `Y` | Reset view to forward |
| `J` | Enhanced chase cam on/off (default on) |
| `K` | Enhanced ego effect on/off (default off) |
| `F5` / `F6` | Walk bobbing less / more |
| `F7` / `F8` | Ego effect level down / up |
| `B` | Cycle format bars (aspect ratio) |

## Controller
- `LB` (from the driver view) cycles: Passenger -> Driver seat -> 360 -> off. In the chase views `LB` toggles the enhanced chase cam.
- `L3` resets the view forward.
- `RB` still cycles the game's own cameras.

## Notes / limitations
- Walk mode has no terrain query: close to the car's track the ground height is estimated, further away
  it stays where it is and you adjust it with `E`/`Q`.
- Seat settings are kept while the game runs, not across restarts.
- After a game or Azimuth update the mod may stop working until it is rebuilt.

## Updating
`update.bat` downloads the latest release from GitHub, verifies its SHA-256 and installs it. It warns you if
`azimuth.exe` changed since the last install, because the mod may then need a new build.
A new ExtraOptions version does not install itself into this fork - wait for the matching release here.

## Build from source
See `BUILD.md`. You need Zig and Python; `build.bat` produces `out\winmm.dll`. `make_release.bat` builds the ZIP.

## License
GNU GPL (inherited from ForzaHorizon2_ExtraOptions). The full source is in this repository.
