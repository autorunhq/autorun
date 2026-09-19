<p align="center">
  <img src="documentation/logo-image.png" alt="Autorun logo" width="220">
</p>

<h1 align="center">Autorun</h1>

<p align="center">
  Play Windows PC games on your Nintendo Switch.<br>
  <sub>Previously called Wine-NX.</sub>
</p>

Autorun is a homebrew app that runs Windows games and programs on the Switch.
It brings [Wine](https://www.winehq.org) - the layer that lets Windows software
run outside Windows - to the Switch's own operating system, and translates the
games' PC (x86) code for the Switch's ARM processor with
[Box64](https://github.com/ptitSeb/box64). You bring the games: copy your own PC
copies to the SD card and start them from Autorun's library.

> **Autorun is experimental.** Some games run well, many start and then stop on
> something that isn't done yet. Every run writes a log that says where.

![The Autorun launcher](documentation/launcher.jpg)

## What runs

Tested on a real Switch:

| Game | How it runs |
|---|---|
| Halo: Combat Evolved | Plays, at about 30 fps. Needs the [32-bit forwarder](#games-that-need-the-32-bit-forwarder). |
| WarCraft III | In game at 24-34 fps, menus at about 50 fps; the intro movie plays. Run its setup once first. |
| Need for Speed Underground 2 | Races with sound and the controller, about 20-45 fps. Needs the 32-bit forwarder. |
| Left 4 Dead 2 | Reaches the main menu. |
| OpenTTD | Up to 60 fps, with sound. |
| Quake III Arena (Quake3e) | About 37 fps at 720p. |

In progress: **The Sims 2 Legacy Collection** doesn't work yet - it starts and
then closes while loading its title screen. The **Ultimate Collection** hasn't
been tested yet.

Programs such as 7-Zip and Notepad work too. Anything not listed may or may not
run - try it, and if it stops, the log tells what it was missing.

## What you need

- A Nintendo Switch with custom firmware (Atmosphère) that can run homebrew.
- A microSD card with room for Autorun and your games.
- Your games, as installed PC folders. Autorun runs **32-bit** Windows games,
  which covers most PC games up to the early 2010s; games that exist only as
  64-bit don't run yet.

## Installing

1. Download the Autorun package (a `.zip`).
2. Unzip it to the **root** of your SD card. Everything goes into
   `switch/wine`.
3. Start **Autorun** from the Homebrew Menu.

Games need a lot of memory. Open the Homebrew Menu from a game (hold **R**
while starting it) rather than from the Album, or put Autorun on the HOME menu
with **Settings -> System -> Make an Autorun forwarder** and start it from there.

## Adding games

1. Copy the game's folder into `switch/wine/drive_c` on the SD card. That
   folder is the game's `C:` drive.
2. In Autorun, press **+** -> **Add game** and choose the game's `.exe`.
3. Press **A** to play.

**WarCraft III** needs a one-time setup, which Autorun includes: run
`C:\WarCraft III Setup\war3-setup.exe` once before playing. Its folder has a
`README.txt` with the details.

### Games that need the 32-bit forwarder

A few older games only work when loaded at fixed low memory addresses - Need for
Speed Underground 2 and Halo are two. Autorun notices when a game needs this and
offers to set it up: **Settings -> System -> Make a 32-bit forwarder** adds an
"Autorun 32-bit" icon to the HOME menu. Start those games from that icon.

## Using the launcher

| Button | What it does |
|---|---|
| **A** | Play the selected game |
| **Y** | The game's options |
| **L / R** | Switch between Home (recently played) and the Library |
| **−** | Settings (on Home), filter and sort (in the Library) |
| **+** | Add a game, or exit Autorun |

The touchscreen works everywhere too.

**A game's options (Y)** - mark it as a favorite or hide it, change its title,
give it command-line arguments, download its artwork, pick how its graphics are
drawn (Direct3D 9 through Wine or DXVK), and give it its own controls.

**Settings (−)** - show hidden games, the controls every game uses by default,
a [SteamGridDB](https://www.steamgriddb.com) API key for artwork, returning to
Autorun when a game ends, making forwarders, and the credits.

## Playing

Out of the box the controller works as a mouse and keyboard:

| Control | Sends |
|---|---|
| Right stick, or a finger on the screen | Moves the mouse pointer |
| **A** / **B** | Left / right mouse button |
| D-pad, left stick | Arrow keys |
| **+** | Esc |
| **X**, **Y** | Space, F |
| **L**, **R** | Tab, Shift |

Every button can be changed: **Settings -> Game defaults -> Controls** for all
games, or a game's options (**Y**) -> **Controls** for that game alone. Each
stick, the d-pad and the touchscreen can move the mouse or send the arrow keys
or W A S D. Games with controller support see an Xbox 360 controller.

**Hold + and − together for a second** to close a game.

## If something goes wrong

Every run leaves two logs in `switch/wine` on the SD card:

- `wine-nx-runtime.log` - the last run, whatever it was.
- `game-NAME.log` - the last run of that game, kept per game.

When reporting a problem, include the game's log and say what you saw. Turning
on **Verbose traces** in the game's options gives more detail, at some speed
cost.

## Limits

- Speed: demanding 3D games are limited by translating the game's code on the
  fly, more than by the graphics chip.
- Memory: a game gets at most about 2 GB.
- One game at a time, one controller.
- A game may need Windows files the card doesn't have yet; the log names them.

## For developers

How Autorun works, how to build it, its tests and its file layout are in
[documentation/technical.md](documentation/technical.md).

## Credits

Autorun is built from these projects; each keeps its own copyright and license.

| Project | License | Used for |
|---|---|---|
| [Wine](https://www.winehq.org) | LGPL-2.1-or-later | The Windows API, loader and graphics layers; this repository is a Wine fork |
| [Box64](https://github.com/ptitSeb/box64) (ptitSeb and contributors) | MIT | Running 32-bit x86 code |
| [DXVK](https://github.com/doitsujin/dxvk) | zlib/libpng | Direct3D 9 over Vulkan |
| [Mesa](https://mesa3d.org) and [mesa-switch](https://github.com/danfromtico/mesa-switch) | MIT (mostly) | OpenGL and Vulkan on the Switch's GPU |
| [libnx](https://github.com/switchbrew/libnx) and [devkitPro](https://devkitpro.org) | ISC, per component | The Switch system library and toolchain |
| [SDL2, SDL2_ttf](https://www.libsdl.org), [FreeType](https://freetype.org), [HarfBuzz](https://harfbuzz.github.io), [libpng](http://www.libpng.org), [zlib](https://zlib.net) | zlib, FTL, MIT, libpng | The launcher |
| [dolphin-nx](https://github.com/NaGaa95/dolphin-nx) (NaGaa95) | GPL-2.0-or-later | The look of the launcher, which is Autorun's own code |

The full list, with authors and the projects used as reference, is in
[documentation/technical.md](documentation/technical.md#credits).
