# FH2 Camera Mods - a plugin for Extra Options

Extra cameras for the Azimuth (PC) version of Forza Horizon 2, as a **plugin for
[Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)** by RokkuDayo (GPL, see LICENSE).
It does **not** replace `winmm.dll` - it installs into Extra Options' `mods` folder.

**Current version: v1.2.0** - plugin build, tested with Extra Options 0.4.4 and the latest Azimuth.

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
Requirement: the official **Extra Options 0.3.x** (`winmm.dll`) must already be installed in your Azimuth folder
(or you use ForzaInfinity, which ships it).
1. Download the ZIP from the Releases page and extract it anywhere.
2. Double-click `install.bat`. If it doesn't find `azimuth.exe` next to itself, a folder window opens - select your
   Azimuth folder (the one that contains `azimuth.exe`). The installer copies `mods\Camera Mods` into it.
   It does not touch `winmm.dll` or your `FH2_ExtraOptions.ini`.
   - Windows may warn that the publisher could not be verified. The scripts are plain text and not digitally signed,
     so click **Run** / **More info -> Run anyway**. You can open the `.bat` files in Notepad to check them.
3. Start the game via Azimuth as usual. `FH2_ExtraOptions.log` shows `CameraMods ...: hooks installed`.

`uninstall.bat` removes the plugin. `update.bat` downloads the newest release.

**Coming from Camera Mods v1.0.x?** Those versions replaced `winmm.dll` with a modified Extra Options.
Put the official Extra Options `winmm.dll` back first, then run `install.bat` of this version.

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
| `1` | Roof cam (front center) |
| `2` | Dashcam (rear-view mirror) |
| `3` | Dashboard cam |
| `4` | Front chase cam (from the front, looking back) |
| `U` | Shake on/off (off by default) |
| `I` | Switch drone/walk controls between keyboard and controller (default: keyboard) |
| `N` | Lock car input on/off in drone/walk mode (locked by default: keys/sticks only move the camera) |
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
- After a game update the plugin may need a new build (it refuses to load on an unknown game executable).

## Updating
`update.bat` downloads the latest release from GitHub, verifies its SHA-256 and installs it.
Because this is a plugin, a new Extra Options or Azimuth version normally keeps working without a new release.

## Build from source
Needs Zig and Python. `build_plugin.bat` produces `out\CameraMods.dll`; `make_release_plugin.bat` builds the ZIP.
The older all-in-one fork (`build.bat`, modified `winmm.dll`) is still in the repository history.

## License
GNU GPL (inherited from ForzaHorizon2_ExtraOptions). The full source is in this repository.

The roof/dashcam/dashboard/front-chase cams (1-4) are mounted to the car and have no effects (no shake, no head movement) - look around with mouse or right stick, WASD/QE adjust the mount. LB cycles: passenger, driver seat, 360, roof, dashcam, dashboard, front chase, off. Press the same key again to leave.

**Controller (XInput) in drone and walk mode** (press `I` to switch to controller; keyboard movement is then ignored): left stick = move, right stick = look, RT/LT = up/down, RB = fast, RB+X = lock car input on/off (same as `N`). In the cockpit cams (passenger, driver seat, fixed cams, 360) the controller stick is not used by the mod, those cams look around with the mouse.
