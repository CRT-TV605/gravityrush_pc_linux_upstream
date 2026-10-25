#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts gravityrush-runtime (Windows and Linux; run.sh calls it).

Environment variables: GOW3_GAME_DIR, GOW3_DATA_DIR, GOW3_PATCHES, GOW3_MODS_*, GOW3_USER_DIR, ...
Extra arguments go to gravityrush-runtime. `--software` (Linux) renders with Mesa's lavapipe.
The in-game "Apply and restart" runs this script again through GOW3_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent
sys.path.insert(0, str(PORT / 'scripts'))
from platform_paths import EXE, WINDOWS, data_dir, host_environment  # noqa: E402


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def running(pid):
    """Whether a process exists and has not exited (a zombie, unreaped by its parent, has)."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:  # /proc/<pid>/stat: "pid (name) state ..."
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
    except (OSError, IndexError):
        return True


def wait_for(pid, timeout=30):
    if WINDOWS:
        kernel32 = ctypes.windll.kernel32
        SYNCHRONIZE = 0x00100000
        handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
        if handle:
            kernel32.WaitForSingleObject(handle, timeout * 1000)
            kernel32.CloseHandle(handle)
        return
    deadline = time.monotonic() + timeout
    while running(pid) and time.monotonic() < deadline:
        time.sleep(0.05)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def software_rendering(env):
    """--software: Mesa's lavapipe instead of the GPU (Linux; slow, for tests without a GPU)."""
    if not env.get('VK_DRIVER_FILES'):
        for folder in ('/run/opengl-driver/share/vulkan/icd.d', '/usr/share/vulkan/icd.d'):
            found = sorted(Path(folder).glob('lvp_icd*.json')) if Path(folder).is_dir() else []
            if found:
                env['VK_DRIVER_FILES'] = str(found[0])
                break
    if not env.get('VK_DRIVER_FILES'):
        fail('Lavapipe not found; set VK_DRIVER_FILES.')
    env['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'


def optimize_fsr4(env):
    """FSR 4: faster post passes next to the downloaded ones (tools/fsr4_optimize.sh, incremental).
    Linux source trees only, when spirv-cross, glslang and Perl are there; the game works without."""
    if WINDOWS or getattr(sys, 'frozen', False) or env.get('GOW3_PREBUILT'):
        return
    folder = Path(env.get('GOW3_FSR4_DIR') or PORT / 'fsr4_shaders')
    script = PORT / 'tools' / 'fsr4_optimize.sh'
    if not folder.is_dir() or not script.is_file() or \
            not all(shutil.which(tool) for tool in ('bash', 'spirv-cross', 'glslangValidator', 'perl')):
        return
    if subprocess.run(['bash', str(script)], stdin=subprocess.DEVNULL, env=env).returncode:
        print('FSR 4: optimized post passes not built', file=sys.stderr, flush=True)


def run_script(name, *args, capture=False):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                creationflags=no_console())
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed launcher child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # Relative paths (GOW3_GAME_DIR, GOW3_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    # Frozen on Linux: the runtime gets the system's libraries, not the launcher's.
    if getattr(sys, 'frozen', False) and not WINDOWS:
        host = host_environment()
        env.clear()
        env.update(host)
    if args[:1] == ['--software']:
        args = args[1:]
        software_rendering(env)
    # Windows: the port folder. Linux: ~/.local/share/gravityrush (scripts/platform_paths.py).
    data = data_dir(PORT)
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    if not (data / 'gravityrush.ini').exists() and (data / 'gow3.ini').exists():
        (data / 'gow3.ini').rename(data / 'gravityrush.ini')  # settings of builds before 1.1
    env.setdefault('GOW3_CONFIG', str(data / 'gravityrush.ini'))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    for key in env.pop('GOW3_DERIVED', '').split(','):
        env.pop(key, None)
    derived = []
    import graphics_options
    graphics, reverted = graphics_options.prepare(env['GOW3_CONFIG'], out / 'graphics_state.json')
    if reverted:
        print('Graphics: the last restart never reached the game; previous options restored', flush=True)
    env.update(graphics_options.environment(graphics))
    # FSR 4 / 4.1.1 assets: next to run.py, else in the data directory (a read-only port folder).
    for key, name in (('GOW3_FSR4_DIR', 'fsr4_shaders'), ('GOW3_FSR411_DIR', 'fsr4_411')):
        if not env.get(key) and not (PORT / name).is_dir() and (data / name).is_dir():
            env[key] = str(data / name)
    optimize_fsr4(env)

    runtime = 'gravityrush-runtime' + EXE
    probe = Path(env['GOW3_PROBE']) if env.get('GOW3_PROBE') else find_executable(runtime)
    if not probe or not probe.is_file():
        fail(f'{runtime} not found: build it with build.sh ' + ('(MSYS2 CLANG64) ' if WINDOWS else '') +
             'or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    game = Path(env.get('GOW3_GAME_DIR', PORT.parent / 'CUSA01113'))
    if game.suffix.lower() == '.pkg' and game.is_file():
        # A PKG is unpacked once beside it (<folder>/<TITLE_ID>); later starts reuse the folder.
        # Release packages leave the unpacker out: the game folder is prepared by the player.
        try:
            import pkg_extract
        except ImportError:
            fail(f'{game.name} is a package: unpack it with your own tools and choose the game folder '
                 '(the one with eboot.bin).')
        target = Path(env.get('GOW3_PKG_TARGET') or pkg_extract.default_target(game))
        if not pkg_extract.is_extracted(game, target):
            print(f'Unpacking {game.name} into {target} (first start only)…', flush=True)
            last = [-1]

            def report(done, total, rate):
                percent = done * 100 // max(total, 1)
                if percent != last[0]:
                    last[0] = percent
                    print(f'PKG: {percent}% ({done >> 20} of {total >> 20} MB, {rate:.0f} MB/s)', flush=True)
            try:
                pkg_extract.extract(game, target, report)
            except (OSError, ValueError) as error:
                fail(f'Could not unpack {game}: {error}')
        game = target
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (choose the game folder: the one with eboot.bin, sce_module and sce_sys).')
    import game_profiles
    profile = game_profiles.profile(game)
    if profile['name']:
        print(f'Game profile: {profile["name"]} ({", ".join(profile["modules"])})', flush=True)
    for key, value in profile['env'].items():
        env.setdefault(key, value)
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('GOW3_MODS_DIR', data / 'mods'),
                           '--config', env.get('GOW3_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('GOW3_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out, '--modules', *profile['modules'])
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('GOW3_CONTENT_SKU', 'full'))

        from patches import (eboot_matches, game_app_version, game_profile, game_title_id,
                             patch_requirements, selected_patches)
        profile = game_profile(game_title_id(game))
        patched = bool(profile) and ((game_app_version(game) == profile[2] and eboot_matches(game, profile))
                                     or bool(env.get('GOW3_FORCE_PATCHES')))
        # The window shows the port's icon, else the game's (BMP: the format SDL loads without
        # extra libraries). launcher/gravityrush.bmp is ready-made, so no Pillow is needed for it.
        if (PORT / 'launcher' / 'gravityrush.bmp').is_file():
            env.setdefault('GOW3_WINDOW_ICON', str(PORT / 'launcher' / 'gravityrush.bmp'))
        try:
            from PIL import Image
            source = PORT / 'launcher' / 'gravityrush.png'
            if not source.is_file():
                source = game / 'sce_sys' / 'icon0.png'
            with Image.open(source) as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('GOW3_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # Linear image readbacks fix its corrupted textures. (The temporal AA follows
        # gravityrush.ini's upscaler; GOW3_UPSCALER=none leaves it out entirely.)
        env.setdefault('GOW3_READBACK_LINEAR', '1')
        if patched:
            # The patch notes give the direct memory and VBlank rate the selected patches need.
            names = selected_patches(profile[1], profile[2], env.get('GOW3_PATCHES', ''), env.get('GOW3_PATCHES_ONLY') == '1')
            # gravityrush.ini decides the resolution and frame rate patches (in-game menu, launcher).
            import xml.etree.ElementTree as ET
            known = {m.get('Name') for m in ET.parse(profile[1]).getroot().iter('Metadata')}
            # GOW3_CAMERA_FIX=0 leaves the gravity-shift camera fix out (A/B tests).
            names = graphics_options.patch_names(names, graphics, graphics_options.TEXTURE_FIX in known,
                                                 graphics_options.CAMERA_FIX in known and env.get('GOW3_CAMERA_FIX') != '0')
            if env.get('GOW3_CAMERA_FIX') == '0':
                names = [n for n in names if n != graphics_options.CAMERA_FIX]
            env['GOW3_PATCHES'] = ';'.join(names)
            env['GOW3_PATCHES_ONLY'] = '1'
            dmem, vblank = patch_requirements(profile[1], names, profile[2])
            for key, value in (('GOW3_DMEM_MB', dmem), ('GOW3_VBLANK_HZ', vblank)):
                if value and key not in env:
                    env[key] = str(value)
                    derived.append(key)
        run_script('patches.py', '--out', out, '--extra', env.get('GOW3_PATCHES', ''), '--game-dir', game,
                   '--patches-dir', env.get('GOW3_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('GOW3_PATCHES_CONFIG', data / 'patches.json'))
        if 'GOW3_VBLANK_HZ' not in env:
            env['GOW3_VBLANK_HZ'] = '60'
            derived.append('GOW3_VBLANK_HZ')
        env['GOW3_DERIVED'] = ','.join(derived)

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('GOW3_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('GOW3_GPU_USER_DIR', str(user_dir))
        from save_backup import backup
        try:
            # The launcher's choices: GOW3_SAVE_BACKUP=0 turns it off, GOW3_SAVE_BACKUP_KEEP how many stay.
            keep = int(env.get('GOW3_SAVE_BACKUP_KEEP', '10') or 10)
            if env.get('GOW3_SAVE_BACKUP', '1') != '0' and (made := backup(user_dir, keep=keep)):
                print(f'Saves backed up to {made}', flush=True)
        except OSError as error:
            print(f'Save backup failed, starting anyway: {error}', flush=True)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        # Windows: CreateProcess parses it; Linux: /bin/sh -c (src/probe.c, runtime_restart).
        env['GOW3_RESTART_COMMAND'] = subprocess.list2cmdline(restart) if WINDOWS else shlex.join(restart)
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('GOW3_TIMEOUT', '0'), *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
