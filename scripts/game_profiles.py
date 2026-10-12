"""Per-game settings for the games this port runs (by title id in sce_sys/param.sfo).

modules: bundled sce_module PRX files linked into the image (link_modules.py), in order.
env:     run.py defaults for that game (the user's own environment still wins).
"""
from pathlib import Path

PROFILES = {
    # MediEvil (2019, Unreal Engine 4). NpToolkit2 is the game's own PSN toolkit; the runtime
    # answers its system calls as an offline console. GoW3's temporal upscaler hooks and image
    # readback mode do not fit this renderer: off and linear (otherwise surfaces turn black).
    'CUSA11227': {
        'name': 'MediEvil',
        'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
        'env': {'GOW3_UPSCALER': 'none', 'GOW3_READBACK_LINEAR': '1', 'GOW3_VBLANK_HZ': '60'},
        # Render resolution through a UE4 config override pak (ue4_override.py), up to 4K.
        'ue4_render_scale': True,
    },
    'CUSA01623': {
        'name': 'God of War III',
        'modules': ('libc.prx', 'libSceFios2.prx'),
        'env': {},
    },
}
DEFAULT = {'name': None, 'modules': ('libc.prx', 'libSceFios2.prx'), 'env': {}}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def profile(game):
    """The profile of a game folder (DEFAULT for unknown games)."""
    found = PROFILES.get(title_id(game) or '')
    if not found:
        return DEFAULT
    # Only modules the dump actually ships are linked.
    modules = tuple(m for m in found['modules'] if (Path(game) / 'sce_module' / m).is_file())
    return {**found, 'modules': modules}
