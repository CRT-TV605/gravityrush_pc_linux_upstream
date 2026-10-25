# Gravity Rush on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 with the same sources as
on Linux. Players: see `README-Windows.txt` (it ships in the package).

## Build

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`):

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

magic_enum, miniz and xbyak (not packaged by MSYS2) are submodules under `gpu/third_party/`.
Then, in the repository:

```bash
git submodule update --init --recursive
bash build.sh                      # out/gravityrush-runtime.exe, out/gravityrush-gpu-check.exe
GOW3_GAME_DIR=F:/path/to/CUSA01113 python run.py
python launcher/gravityrush_launcher.py
bash packaging/windows/package.sh  # dist/gravityrush-windows/ and .zip: the launcher, the game + DLLs
```

`run.py` is the same script on Windows and Linux (`run.sh` calls it there; it works from a plain Windows Python too).
The package freezes the launcher with PyInstaller (a private venv in `out/pyenv`, from a
python.org Python) into `Gravity Rush Launcher.exe`, which also runs `run.py` (`--run`), the preparation
scripts (`--script`) and the game without the window (`--play`): players need no Python.
`Gravity Rush.exe` (`packaging/windows/play.c`) is a small native program that runs
`Gravity Rush Launcher.exe --play` from its folder and waits for it, for players who want to start the
game directly with the saved settings.
`patches.py` applies the built-in patches only to the game build they were checked against
(CUSA01113 01.00, by eboot.bin sha256; `GOW3_FORCE_PATCHES=1` overrides); other builds run unpatched.
From a source tree it adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to `PATH` for the DLLs.
`bash tools/fetch_fsr4_assets.sh` works in the CLANG64 shell as on Linux.

DLSS (optional): `DLSS_SDK_ROOT=<checkout of github.com/NVIDIA/DLSS> bash
packaging/windows/build_dlss.sh` builds `gow3_dlss.dll` (`gpu/dlss_bridge`, the only code that
uses the NVIDIA SDK, compiled with MSVC 2022) and copies it with `nvngx_dlss.dll` next to
`out/gravityrush-runtime.exe` with NVIDIA's license; `package.sh` ships the three when present. The renderer loads the bridge at run
time (`vk_dlss.cpp`), adds the Vulkan extensions NGX asks for, and DLSS gets the same inputs as
FSR 3 (scene color, depth, render-pixel motion vectors, jitter). `GOW3_DLSS=0` turns it off.

The launcher window is `launcher/launcher_ui.py` (its texts in English, Italian, French, German
and Spanish). The icon is drawn by `packaging/windows/make_icon.py` and built into the launcher,
`Gravity Rush.exe`, `gravityrush-runtime.exe` (the game window) and `gravityrush-gpu-check.exe`.

## What is different from Linux

| Linux | Windows |
|---|---|
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base set with `arch_prctl` | GS is the TEB and cannot move. The loader rewrites the displacement to `gs:[0x1480 + 8*slot]` (the TEB's `TlsSlots[slot]`, same 9-byte instruction) and each guest thread keeps its TCB in that TLS slot. The slot is claimed first thing in `main` (Vulkan drivers take many). |
| One memfd for direct + flexible memory, `mmap(MAP_FIXED)` views | A pagefile-backed section; the PS4 range 64 GiB–1 TiB is reserved as one placeholder and views replace placeholders (`VirtualAlloc2`/`MapViewOfFile3`, as in shadPS4). Partial unmaps split views and restore page protections (GPU tracking). |
| `-no-pie`, heap below 1 TiB | `--disable-high-entropy-va`: heaps and thread stacks stay low and out of the PS4 range. |
| SIGSEGV handler, `sigsetjmp` recovery | Vectored exception handler; `gow3_setjmp`/`gow3_longjmp` (`src/compat_win.c`) restore all Win64 callee-saved registers without SEH unwinding (guest frames have no unwind data; clang's `__builtin_longjmp` restores a wrong frame pointer on Win64). |
| Guest thread stacks from `runtime_low_map` | Windows allocates them, fully committed (`pthread_attr_setstacksize`): guest code has no stack probes and would skip the guard page. The main thread stack is 16 MiB committed. |
| glibc `nanosleep`, monotonic condition variables | High-resolution waitable timers (`compat_sleep_ns`; winpthreads' sleep rounds to 15.6 ms); semaphore deadlines use `CLOCK_REALTIME` (winpthreads rejects monotonic condition variables). |
| `libgow3gpu.so` | `libgow3gpu.a`, linked into `gravityrush-runtime.exe` by CMake (`gpu/CMakeLists.txt`), with a manifest for UTF-8 paths and long paths. |
| In-game restart: `GOW3_RESTART_COMMAND` (from `run.py`) through `/bin/sh` (`posix_spawn`), else `execlp bash run.sh`. The launcher keeps the game in its own process group. | `run.py` puts its command line in `GOW3_RESTART_COMMAND`; the new launch waits for the old process (`--after PID`). The launcher keeps the game in a job object. |
| Mod overlay with symlinks | Symlinks when allowed (Developer Mode), else junctions for folders and hard links for files. |

Diagnostics added on the way: `GOW3_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer
and the presented image of the next frame to `GOW3_DUMP_DIR` (raw 32-bit pixels), which also
works for menus and movies.

Not ported: the watchdog/`--timeout` thread dumps (Linux signals), userfaultfd tracking
(`GOW3_UFFD`), MangoHud, PGO (GCC flags) and
the `tests/` programs (several use Linux-only APIs).
