# Gravity Rush on Linux

The port runs natively on 64-bit (x86-64) Linux: the game's original `eboot.bin` runs on the
CPU, the runtime in `src/` stands in for the PS4 system libraries, the shadPS4 video core
(`gpu/`) draws with Vulkan, and SDL3 handles the window, input and audio. No game files are
included: you need your own copy, as an unpacked game folder (`eboot.bin`, `sce_module`,
`sce_sys`) prepared with your own tools. Tested build: CUSA01113, version 01.00.

The Linux port (the Linux parts of the runtime, `run.py` and the launcher, the packaging and
this document) was written with an AI coding assistant (Claude Code), tested by a person on
one device, and has not been reviewed by the original author of the port.

## Requirements

- x86-64 CPU (the game's code runs natively; no ARM).
- A Vulkan 1.3 GPU and driver (see [Vulkan drivers](#vulkan-drivers)).
- For the AppImage: glibc 2.41 or newer (Debian 13, Ubuntu 25.04, Fedora 42, Arch or newer;
  on SteamOS run `ldd --version` in Konsole to check) and the system's Vulkan loader
  (`libvulkan.so.1`: `libvulkan1`, `vulkan-loader`, `vulkan-icd-loader`).

## Playing (AppImage)

```bash
chmod +x GravityRush-PC-*-Linux-x86_64.AppImage
./GravityRush-PC-*-Linux-x86_64.AppImage
```

The launcher opens: choose the game folder on the Game page (**Browse…**) and press **Play**.
On the Game page, **Create desktop shortcuts** adds "Gravity Rush" (plays at once with the saved settings) and
"Gravity Rush Launcher" to the applications menu and the desktop. The same file also takes:

| Command | What it does |
|---|---|
| `./GravityRush….AppImage --play` | The game with the saved settings, without the launcher window |
| `GOW3_GAME_DIR=/path/CUSA01113 ./GravityRush….AppImage --run` | `run.py` directly (all `GOW3_*` variables work) |

Without FUSE (some containers, minimal systems): `--appimage-extract` and run `squashfs-root/AppRun`.

## Where files go

The AppImage is read-only, so everything the port writes goes to:

| What | Where |
|---|---|
| Saves, GPU caches, screenshots, `last_run.log` | `~/.local/share/gravityrush/user/` |
| `gravityrush.ini` (video settings, in-game menu), `mods/`, `patches/`, `mods.json`, `patches.json` | `~/.local/share/gravityrush/` |
| Generated game image (`out/`) | `~/.local/share/gravityrush/out/` |
| FSR 4 assets (optional) | `~/.local/share/gravityrush/fsr4_shaders/`, `fsr4_411/` |
| Launcher settings (`settings.json`) | `~/.config/gravityrush-launcher/` |

`$XDG_DATA_HOME` and `$XDG_CONFIG_HOME` move them; `GOW3_DATA_DIR` sets the data folder
directly. A source tree that already has `user/` or `gravityrush.ini` next to `run.py` (saves of
the old `run.sh`) keeps using its own folder. The launcher's "Saves folder" can point elsewhere
(`GOW3_USER_DIR`).

## Building

```bash
git clone --recursive https://github.com/LucaPazienza/gravityrush_pc
cd gravityrush_pc
sudo bash packaging/linux/install-deps.sh   # Debian/Ubuntu, Fedora, Arch (NixOS: shell.nix)
bash build.sh                               # out/gravityrush-runtime, out/gpu/libgow3gpu.so
```

`build.sh` uses GCC (or `CC`), CMake and Ninja and builds with LTO (`GOW3_LTO=OFF` turns it off);
`bash build.sh --test` also builds and runs the C tests. On NixOS it re-enters `shell.nix` by
itself. `packaging/linux/Dockerfile` is the same environment as a container (CI uses it):

```bash
docker build -t gravityrush-build packaging/linux
docker run --rm -v "$PWD:/src" -w /src gravityrush-build bash -c \
  'bash packaging/linux/build-libs.sh && PKG_CONFIG_PATH=/src/out/libs/lib/pkgconfig bash build.sh && bash packaging/linux/package.sh'
```

### Packages

`install-deps.sh` installs these (the GPU library also uses magic_enum, miniz, xbyak, imgui,
sirit and FSR-Vulkan from the submodules in `gpu/third_party/`):

| | Debian 13 / Ubuntu 25.04+ | Fedora 42+ | Arch |
|---|---|---|---|
| Toolchain | `build-essential cmake ninja-build pkg-config git patchelf file curl` | `gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git patchelf file curl` | `base-devel cmake ninja pkgconf git patchelf file curl` |
| Vulkan | `libvulkan-dev glslang-tools libvulkan-memory-allocator-dev` | `vulkan-headers vulkan-loader-devel glslang vulkan-memory-allocator-devel` | `vulkan-headers vulkan-icd-loader glslang vulkan-memory-allocator` |
| SDL3, FFmpeg | `libsdl3-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev` | `SDL3-devel libavcodec-free-devel libavformat-free-devel libavutil-free-devel libswscale-free-devel libswresample-free-devel` | `sdl3 ffmpeg` |
| Others | `libboost-dev libfmt-dev robin-map-dev libxxhash-dev libzydis-dev libx11-dev libfreetype-dev` | `boost-devel fmt-devel robin-map-devel xxhash-devel zydis-devel libX11-devel freetype-devel` | `boost fmt robin-map xxhash zydis libx11 freetype2` |
| Python (launcher) | `python3 python3-venv python3-tk python3-pil python3-pil.imagetk python3-cryptography` | `python3 python3-tkinter python3-pillow python3-pillow-tk python3-cryptography` | `python python-pillow python-cryptography tk` |
| FSR 4 optimizer (optional) | `spirv-cross perl` | `spirv-cross perl` | `spirv-cross perl` |

The video core needs **recent Vulkan headers** (with Vulkan-Hpp): it builds with 1.4.366, while
1.4.309 (Debian 13) and 1.4.341 (Ubuntu 26.04) are too old. When the system's are older,
`install-deps.sh` installs Khronos' Vulkan-Headers v1.4.366 into `/usr/local` (headers only;
`GR_VULKAN_HEADERS=0` skips it; after installing them, `rm -rf out/gpu` once). SDL3 is packaged from Debian 13
/ Ubuntu 25.04 / Fedora 42 on; older releases need SDL3 built from source.
Python packages: `pip install -r requirements.txt` (Pillow; cryptography only for `.pkg` unpacking).

## Running from the source tree

```bash
GOW3_GAME_DIR=/path/to/CUSA01113 bash run.sh     # builds when needed, then run.py
python3 launcher/gravityrush_launcher.py          # the launcher
```

`run.sh` is a thin wrapper: it re-enters `shell.nix` on NixOS, runs `build.sh` (skipped with
`GOW3_PREBUILT=1` or `GOW3_PROBE=<runtime>`) and then `run.py`, which is the same script on
Windows and Linux. `bash run.sh --software` renders with Mesa's lavapipe (slow; for tests
without a GPU). Without a game folder `run.py` stops with `No eboot.bin in …`.

The in-game "Apply and restart" (a new render resolution is a patch applied at start) runs
`GOW3_RESTART_COMMAND`, which `run.py` sets to itself with `--after PID`: the runtime starts it
through `/bin/sh` with only stdin/stdout/stderr inherited (the GPU device closes with the old
process) and exits; the new `run.py` waits for the old one to end. Started from a terminal, the
shell prompt returns when the first launch ends while the restarted game keeps running.
The launcher starts `run.py` in its own process group, which "Stop the game" ends (restarted
launches stay in it).

## Packaging (AppImage)

```bash
bash packaging/linux/build-libs.sh                                    # out/libs: SDL3 and FFmpeg
rm -rf out/gpu && PKG_CONFIG_PATH=$PWD/out/libs/lib/pkgconfig bash build.sh
bash packaging/linux/package.sh   # dist/GravityRush-PC-<version>-Linux-x86_64.AppImage (+ .sha256)
```

`build-libs.sh` builds SDL3 (3.4.18) and a minimal FFmpeg (7.1.5: H.264/HEVC, AAC, MP3, PNG, the
MP4 demuxer, swscale, swresample) for the package: distribution builds link X11, PipeWire,
VA-API, VDPAU, OpenCL, Intel VPL and many codecs directly, which would tie the AppImage to
the system it was built on (tested: Debian 13's fail on Arch). This SDL3 opens X11, Wayland,
PipeWire, PulseAudio, ALSA, libdecor and udev at run time. `package.sh` refuses a runtime
linked with the system's SDL3 (`GR_SYSTEM_LIBS=1` overrides). `rm -rf out/gpu` is needed because
CMake keeps the libraries it found the first time.

It freezes the launcher with PyInstaller (a private venv in `out/pyenv-linux`), assembles an
AppDir with the port in `usr/lib/gravityrush/` and runs appimagetool (downloaded into `out/`
unless `APPIMAGETOOL` names one; `GR_APPDIR_ONLY=1` stops at `dist/gravityrush-linux/`).
`AppRun` starts the frozen launcher, which also runs `run.py` (`--run`) and the preparation
scripts (`--script`) as on Windows, so players need no Python.

Libraries: that SDL3 and FFmpeg, plus Zydis, fmt and xxHash (which the GPU library links), ship in
`bin/lib` (rpath `$ORIGIN`). **The Vulkan loader and the GPU drivers always come from the
system**, as do glibc, libstdc++, zlib, X11/Wayland, the audio servers (PulseAudio/PipeWire/ALSA,
which SDL3 loads at run time), libdrm, freetype and the desktop libraries (the full list is
`EXCLUDED` in `package.sh`). Nothing sets `LD_LIBRARY_PATH`; the launcher removes the one PyInstaller
sets before it starts the game, file managers or file choosers. The licenses of the bundled
libraries are in `THIRD-PARTY-LICENSES/`. The package refuses to build when it would hold game
files, saves, caches or FSR 4 assets, and like the Windows package it leaves out the `.pkg`
unpacker.

The launcher's update check (Advanced > Updates) looks for the Linux AppImage of a newer release
and opens its page: an AppImage is replaced by downloading the new file.

## Gamepads

SDL3 reads DualSense, DualShock 4 and Xbox controllers. Without access to their `hidraw` devices
they still work through evdev, but SDL's HIDAPI drivers (DualSense/DS4 **gyro**, which the game's
motion controls use, touchpad, light bar, rumble) need it. Steam installs suitable udev rules; if
you do not have Steam:

```bash
sudo cp packaging/linux/70-gravityrush-gamepads.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger   # then reconnect the controller
```

Xbox controllers over Bluetooth may need the `xpadneo` driver on some kernels. Inside Steam
(as a non-Steam game), Steam Input presents pads as a virtual Xbox controller, without the
PS4-style motion data: with an external DualSense/DS4, turn Steam Input off for the game to keep
the gyro. A handheld's built-in controls (Steam Deck, ROG Ally on SteamOS) only reach the game through Steam Input (as that
Xbox-style pad; its gyro can be mapped there, but not as PS4 motion).

## Vulkan drivers

What to expect (not tested by this project without hardware, see Known issues):

- **AMD, Mesa RADV**: the main target of shadPS4's video core. Use a recent Mesa (24.x or newer).
- **NVIDIA proprietary driver**: supported by the video core (it has NVIDIA-specific
  workarounds); use a current driver (550 or newer). Nouveau/NVK is not expected to work yet.
- **Intel, Mesa ANV** (Arc, Xe, recent integrated GPUs): expected to start; performance and
  rendering are less tested in shadPS4.
- Lavapipe (`run.sh --software`): for tests only.

`GOW3_GPU_ID` (the launcher's GPU choice) picks a card in Vulkan's order; `GOW3_VK_VALIDATION=1`
turns on the validation layers when installed (`vulkan-validationlayers`).

## FSR 4 and DLSS

FSR 3.1 and TAA are built in. FSR 4 needs its assets: `bash tools/fetch_fsr4_assets.sh` puts
them in `fsr4_shaders/` of a source tree; with the AppImage put them in
`~/.local/share/gravityrush/fsr4_shaders/` (`run.py` points `GOW3_FSR4_DIR` there when the port
has none). In a source tree `run.py` also builds the faster post passes (`tools/fsr4_optimize.sh`)
when `spirv-cross`, `glslangValidator` and Perl are installed; without them, or when that fails,
the original passes are used. When the assets are missing or the GPU cannot run them, the game
prints `Upscaler: FSR 4 unavailable: …` and goes on without it. FSR 4.1.1 assets
(`tools/fsr4cap`) work the same way in `fsr4_411/`. DLSS is Windows-only in this port.

## Known issues and what is not verified

- The port has been developed and tested on Windows. On Linux it compiles and packages
  (Debian 13 container; the CI job runs the same steps), the AppImage's runtime and launcher
  start on a clean Arch system and pass a Vulkan smoke test on lavapipe, and the game has been
  reported working on one ASUS ROG Ally (Z1 Extreme, AMD/RADV) with SteamOS. Testing is at a very
  early stage: other devices, the Steam Deck, NVIDIA and Intel GPUs are not tested yet.
- `bash build.sh --test`: `pad-test` stops at stale touchpad expectations, and the Python
  wrappers' `runtime-test` (mutex results) and `sema-test` (an import now resolved) too: these
  C tests predate changes made on Windows (where they are not built) and need updating.
  `file-mods`, `content`, `mp3`, `videodec` and `services` pass.
- The AppImage needs glibc 2.41+; older distributions should build from source.
- SteamOS (ROG Ally, Steam Deck): add the AppImage to Steam as a non-Steam game (Desktop Mode)
  to play from Game Mode.
- Wayland: SDL3 uses Wayland when available, else X11/XWayland. `SDL_VIDEODRIVER=x11` forces X11.
- The launcher keeps the normal window frame on Linux (window managers give frameless windows
  no keyboard focus). The refresh rate list comes from `xrandr` (X11/XWayland); without it only
  "Desktop" is offered.
- File choosers: `zenity` or `kdialog` when installed, else Tk's own dialog.
