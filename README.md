# MediEvil (PS4) for PC

A PC port of **MediEvil** (PlayStation 4, 2019, CUSA11227) for Windows 10 and 11, built on the
[God of War III PC port](https://github.com/FelipePrado0/gow3_pc).

The CPU side is native: the game's original x86-64 executable runs directly on the PC's processor
and a runtime written for this port replaces the PS4 system libraries, so there is no emulator.
The graphics run on the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core, which
translates the PS4 GPU commands and shaders to Vulkan.

> **No game files are included.** You need your own copy of MediEvil (CUSA11227), as an unpacked
> folder or a fake package (.pkg). This project is not affiliated with Sony Interactive
> Entertainment or Other Ocean. MediEvil is a trademark of Sony Interactive Entertainment.

## What works

- Boot, intro movies, menus and gameplay (tested on an RTX 5080: locked 60 FPS, also at 4K).
- Audio (Wwise through AudioOut, ATRAC9 and AAC).
- Controller (DualSense, Xbox and others, through SDL3) and keyboard, both remappable.
- Saving and loading; save backups before every start.
- **Install from PKG**: the launcher (or `run.py`) unpacks a fake PKG once, resumably.
- **Render resolution up to 4K**: the 3D scene is drawn at up to 3840 × 2160 and scaled to the
  game's 1080p picture (a UE4 config override pak, `scripts/ue4_override.py`).
- **Launcher** in MediEvil's colors: home with cover art and quick settings, display, performance,
  game folder / PKG install / save backups, patches, mods, controls, diagnostics and log.
- **In-game menu** (Insert, or R3+L2): image (display mode, VSync, FSR 1, sharpening),
  performance and the performance overlay.
- Online features (PSN) answer as a console that is not signed in.

### Known limits

- 60 FPS is the game's own limit; higher frame rates are not unlocked.
- The 4K mode is supersampling: the output picture stays 1920 × 1080.
- Experimental PS4 Pro mode (`GOW3_NEO=1`) stops in the texture cache.
- The God of War III cheats, multipliers and code patches do not apply and are hidden.

## Download and play

1. Download `medievil-windows.zip` from the
   [Releases page](../../releases/latest) and extract it (not inside the zip).
2. Open **`MediEvil.exe`** (Windows may warn that it is not signed: More info, Run anyway).
3. On **Game**, either **Choose .pkg…** to install from your package, or **Browse…** to the
   folder with `eboot.bin`.
4. Choose a **Render resolution** on **Home** and press **PLAY**.

`Play MediEvil.exe` starts the game with the saved settings, for shortcuts and Steam.
More in [packaging/windows/README-Windows.txt](packaging/windows/README-Windows.txt).

## Building from source

1. Install [MSYS2](https://www.msys2.org) and, in the **MSYS2 CLANG64** shell, the packages listed
   in [packaging/windows/README.md](packaging/windows/README.md). Install
   [Python 3](https://www.python.org) with `pip install pillow cryptography`.
2. Build (newer CLANG64 toolchains need the standard headers forced for some third-party code):

   ```bash
   git clone --recursive https://github.com/bc160201983/medievil_pc
   cd medievil_pc
   CXXFLAGS="-include cstdlib -include cstdio -include cwchar -include cmath -include new" bash build.sh
   ```

3. Start the launcher with `MediEvil Launcher.cmd` (or `python launcher/gow3_launcher_win.py`),
   or the game directly:

   ```powershell
   $env:GOW3_GAME_DIR = 'D:\path\CUSA11227'   # or the .pkg
   python run.py
   ```

4. Package: `bash packaging/windows/package.sh` builds `dist/medievil-windows.zip`.

## How it works

| Part | Where | What it does |
|---|---|---|
| PKG install | `scripts/pkg_extract.py` | Decrypts and unpacks fake PKGs (PFS, PFSC), as shadPS4 does |
| Game profiles | `scripts/game_profiles.py` | Modules to link and GPU defaults per title |
| Preparation | `scripts/` | Reads `eboot.bin`, links the game's `libc`, `libSceFios2` and `libSceNpToolkit2` |
| Loader | `src/probe.c` | Loads the image and starts the game's original code |
| Runtime | `src/runtime_*.c` | PS4 system functions; `runtime_extra.c` holds the ones added for MediEvil |
| GPU | `gpu/shadps4/` | shadPS4 video core (Vulkan) with the gow3 port's speed work |
| Resolution | `scripts/ue4_override.py` | Writes the UE4 override pak for the chosen render resolution |
| Launcher | `launcher/gow3_launcher_win.py` | Settings, PKG install, mods, controls and the play button |

To port another PS4 game the same way: the loader stops at the first system function the runtime
does not implement and names it; `scripts/gen_import_names.py` adds a game's imports to the name
table.

## Credits

- **FelipePrado0**: [gow3_pc](https://github.com/FelipePrado0/gow3_pc), the God of War III port
  this project is built on, and its credits: **Supermedo** and **deadinside28** (bloodborne_pc),
  **cuesta4** (shadPS4 GPU fixes). See [docs/README-gow3.md](docs/README-gow3.md).
- **shadPS4 team**: the video core, and the PKG format and fake-PKG keysets used by
  `scripts/pkg_extract.py` (`src/core/file_format/pkg.cpp`, `src/core/crypto`) and the NID name
  list (`tools/aerolib.inl`).
- Also used: LibAtrac9, FFmpeg, SDL3, Dear ImGui, sirit, magic_enum, miniz, xbyak, Vulkan Memory
  Allocator, FSR-Vulkan, MSYS2, LLVM, Pillow and cryptography.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.
