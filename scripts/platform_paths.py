#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Where the port keeps what it writes, on Windows and on Linux (run.py and the launcher).

Windows: the port folder itself (settings, saves, mods and caches next to the executables).
Linux: $XDG_DATA_HOME/gravityrush (~/.local/share/gravityrush), since the port folder may be
read-only (the AppImage). A source tree that already holds saves or settings from before
(run.sh wrote them next to itself) keeps using them. GOW3_DATA_DIR overrides both.
"""
import os
from pathlib import Path
import sys

WINDOWS = os.name == 'nt'
# Executables: gravityrush-runtime.exe on Windows, gravityrush-runtime elsewhere.
EXE = '.exe' if WINDOWS else ''
# What an older copy of the port wrote into its own folder.
_LEGACY = ('user', 'gravityrush.ini', 'gow3.ini')


def _xdg(variable, fallback):
    value = os.environ.get(variable, '')
    # The XDG specification: relative paths are invalid and ignored.
    return Path(value) if value and os.path.isabs(value) else Path.home() / fallback


def data_dir(port):
    """The writable directory for out/, user/ (saves, GPU caches), gravityrush.ini, mods, patches."""
    if os.environ.get('GOW3_DATA_DIR'):
        return Path(os.environ['GOW3_DATA_DIR'])
    port = Path(port)
    if WINDOWS:
        return port
    if not os.environ.get('APPIMAGE') and os.access(port, os.W_OK) and \
            any((port / name).exists() for name in _LEGACY):
        return port
    return _xdg('XDG_DATA_HOME', '.local/share') / 'gravityrush'


def config_dir():
    """The launcher's own settings (settings.json): %APPDATA% or $XDG_CONFIG_HOME."""
    if WINDOWS:
        return Path(os.environ.get('APPDATA') or os.environ.get('USERPROFILE') or '.') / 'gravityrush-launcher'
    return _xdg('XDG_CONFIG_HOME', '.config') / 'gravityrush-launcher'


def host_environment(env=None):
    """The environment for programs that are not part of the frozen launcher (the game, file
    managers): PyInstaller on Linux points LD_LIBRARY_PATH at its own libraries (Tcl/Tk, X11,
    zlib, ...), which must not replace the system's in them."""
    env = dict(os.environ if env is None else env)
    if getattr(sys, 'frozen', False) and not WINDOWS:
        if 'LD_LIBRARY_PATH_ORIG' in env:
            env['LD_LIBRARY_PATH'] = env.pop('LD_LIBRARY_PATH_ORIG')
        else:
            bundle = getattr(sys, '_MEIPASS', '')
            kept = [p for p in env.get('LD_LIBRARY_PATH', '').split(os.pathsep) if p and p != bundle]
            env['LD_LIBRARY_PATH'] = os.pathsep.join(kept)
        if not env['LD_LIBRARY_PATH']:
            del env['LD_LIBRARY_PATH']
    return env
