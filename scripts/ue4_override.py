"""Unreal Engine 4 render resolution override for MediEvil (up to 4K).

UE4 games read their config only from their paks. A small pak whose name ends in _P mounts with
priority over them, so a changed copy of Engine/Config/PS4/PS4Engine.ini (taken from the game's
own pak) wins. r.ScreenPercentage scales the 3D scene: 200% of the 1080p frame renders it at
3840x2160 (4K) and scales it down to the game's 1920x1080 output (supersampling: much sharper
edges and detail). The HUD stays at 1080p. The game itself keeps its 60 FPS cap.
The pak is written into the game's Paks folder at every start and removed for native.

Usage: python scripts/ue4_override.py <game dir> [--resolution native|480p|720p|1440p|1800p|4K]
"""
import argparse
import hashlib
import os
import struct
from pathlib import Path

PAK_MAGIC = 0x5A6F12E1
PAK_NAME = 'pakchunk99-ps4_P.pak'
CONFIG = 'Engine/Config/PS4/PS4Engine.ini'
# Render resolution (medievil.ini render_resolution) -> scene scale of the 1080p frame, in percent.
SCREEN_PERCENTAGE = {'native': None, '480p': '44.444', '720p': '66.667', '1440p': '133.333',
                     '1800p': '166.667', '4K': '200'}


def paks_dir(game):
    return Path(game) / 'overbite' / 'content' / 'paks'


def read_index(path):
    """{name: (offset, size, compression)} of a version 7 pak (unencrypted index)."""
    with open(path, 'rb') as f:
        f.seek(os.path.getsize(path) - 44)
        magic, version, index_offset, index_size = struct.unpack('<IIQQ', f.read(24))
        if magic != PAK_MAGIC or version != 7:
            raise ValueError(f'{path.name}: unsupported pak (magic {magic:#x}, version {version})')
        f.seek(index_offset)
        index = f.read(index_size)
    pos = 0

    def fstring():
        nonlocal pos
        n, = struct.unpack_from('<i', index, pos)
        pos += 4
        if n < 0:
            text = index[pos:pos - 2 * n].decode('utf-16le')[:-1]
            pos += -2 * n
        else:
            text = index[pos:pos + n].decode('latin1')[:-1]
            pos += n
        return text
    fstring()  # mount point
    count, = struct.unpack_from('<i', index, pos)
    pos += 4
    entries = {}
    for _ in range(count):
        name = fstring()
        offset, size, _uncompressed, compression = struct.unpack_from('<qqqi', index, pos)
        pos += 48
        if compression:
            blocks, = struct.unpack_from('<i', index, pos)
            pos += 4 + 16 * blocks
        pos += 5
        entries[name] = (offset, size, compression)
    return entries


def read_file(game, name):
    """A file of the game's own paks (uncompressed entries only)."""
    for pak in sorted(paks_dir(game).glob('pakchunk*-ps4.pak')):
        entries = read_index(pak)
        if name in entries:
            offset, size, compression = entries[name]
            if compression:
                raise ValueError(f'{name} is compressed')
            with open(pak, 'rb') as f:
                f.seek(offset + 53)  # the entry header in front of the data
                return f.read(size)
    raise FileNotFoundError(name)


def fstring(text):
    data = text.encode('latin1') + b'\0'
    return struct.pack('<i', len(data)) + data


def entry(offset, data):
    """FPakEntry, version 7, uncompressed and unencrypted (as UnrealPak writes it)."""
    return struct.pack('<qqqi', offset, len(data), len(data), 0) + hashlib.sha1(data).digest() + b'\0' + \
        struct.pack('<I', 0)


def write_pak(path, files, mount='../../../'):
    body, records = b'', []
    for name, data in files.items():
        records.append((name, len(body)))
        body += entry(0, data) + data  # the copy in front of the data has offset 0
    index = fstring(mount) + struct.pack('<i', len(files))
    for (name, offset), data in zip(records, files.values()):
        index += fstring(name) + entry(offset, data)
    footer = bytes(16) + b'\0' + struct.pack('<IIQQ', PAK_MAGIC, 7, len(body), len(index)) + \
        hashlib.sha1(index).digest()
    tmp = path.with_suffix('.tmp')
    tmp.write_bytes(body + index + footer)
    tmp.replace(path)


def set_values(ini, values):
    """Sets keys in [SystemSettings], replacing existing lines."""
    text = ini.decode('utf-8-sig').replace('\r\n', '\n')
    head, sep, rest = text.partition('[SystemSettings]\n')
    if not sep:
        head, sep, rest = text + '\n', '[SystemSettings]\n', ''
    lines = [l for l in rest.split('\n') if l.split('=', 1)[0].strip() not in values]
    section_end = next((i for i, l in enumerate(lines) if l.startswith('[')), len(lines))
    lines[section_end:section_end] = [f'{k}={v}' for k, v in values.items()]
    return (head + sep + '\n'.join(lines)).replace('\n', '\r\n').encode('utf-8')


# Graphics quality (medievil.ini ue4_quality): console keeps the PS4 settings; medium and low trade
# effects for speed on integrated and older graphics cards.
QUALITY = {
    'console': {},
    # Diagnostics (not in the launcher): console quality with one effect off.
    'diag_ao': {'r.AmbientOcclusionLevels': '0'},
    'diag_ssr': {'r.SSR.Quality': '0'},
    'diag_shadow': {'r.ShadowQuality': '2', 'r.Shadow.MaxCSMResolution': '1536'},
    'diag_mb': {'r.MotionBlurQuality': '0'},
    'diag_fxaa': {'r.PostProcessAAQuality': '2'},  # FXAA instead of temporal AA
    'diag_localres': {'r.Shadow.MaxResolution': '512'},  # point/spot light shadow maps
    'diag_nocube': {'r.AllowPointLightCubemapShadows': '0'},
    'diag_fixedres': {'r.Shadow.MinResolution': '1024', 'r.Shadow.MaxResolution': '1024'},
    # Fewer shadow draws (the GPU thread tops out near 2000 draws a frame at 60 FPS).
    'diag_draws': {'r.Shadow.MinResolution': '512', 'r.Shadow.MaxResolution': '512',
                   'r.Shadow.RadiusThreshold': '0.1', 'r.Shadow.CacheWholeSceneShadows': '1',
                   'r.Shadow.CSM.MaxCascades': '2', 'r.Shadow.DistanceScale': '0.8'},
    'diag_nopoint': {'r.Shadow.MinResolution': '512', 'r.Shadow.MaxResolution': '512',
                     'r.Shadow.CacheWholeSceneShadows': '1', 'r.AllowPointLightCubemapShadows': '0'},
    'diag_fixed512': {'r.Shadow.MinResolution': '512', 'r.Shadow.MaxResolution': '512',
                      'r.Shadow.RadiusThreshold': '0.05', 'r.Shadow.CacheWholeSceneShadows': '1'},
    'diag_lod': {'r.ViewDistanceScale': '0.7', 'foliage.DensityScale': '0.6', 'grass.DensityScale': '0.6',
                 'r.Shadow.CSM.MaxCascades': '2'},
    'diag_post': {'r.VolumetricFog': '0', 'r.DepthOfFieldQuality': '0', 'r.LensFlareQuality': '0',
                  'r.SceneColorFringeQuality': '0', 'r.BloomQuality': '2', 'r.RefractionQuality': '0'},
    # Above the PS4: GPU cost, little memory (the game keeps its PS4 memory budget, so texture
    # streaming pools stay as shipped).
    'high': {
        'r.ShadowQuality': '4', 'r.Shadow.MaxCSMResolution': '2048', 'r.Shadow.MaxResolution': '2048',
        'r.Shadow.DistanceScale': '1.5', 'r.AmbientOcclusionLevels': '3', 'r.SSR.Quality': '3',
        'r.MaxAnisotropy': '16', 'r.ViewDistanceScale': '1.5', 'r.StaticMeshLODDistanceScale': '0.75',
        'foliage.LODDistanceScale': '1.5',
    },
    'ultra': {
        'r.ShadowQuality': '5', 'r.Shadow.MaxCSMResolution': '4096', 'r.Shadow.MaxResolution': '2048',
        'r.Shadow.DistanceScale': '2', 'r.Shadow.CSM.MaxCascades': '4', 'r.AmbientOcclusionLevels': '3',
        'r.SSR.Quality': '4', 'r.MaxAnisotropy': '16', 'r.ViewDistanceScale': '2',
        'r.StaticMeshLODDistanceScale': '0.5', 'foliage.LODDistanceScale': '2',
        'r.DetailMode': '2', 'r.MaterialQualityLevel': '1',
    },
    'medium': {
        'r.ShadowQuality': '2', 'r.Shadow.MaxCSMResolution': '1536', 'r.Shadow.MaxResolution': '1024',
        'r.AmbientOcclusionLevels': '1', 'r.SSR.Quality': '1', 'r.MotionBlurQuality': '2',
        'r.DepthOfFieldQuality': '1', 'r.ViewDistanceScale': '0.85',
    },
    'low': {
        'r.ShadowQuality': '1', 'r.Shadow.MaxCSMResolution': '1024', 'r.Shadow.MaxResolution': '512',
        'r.Shadow.CSM.MaxCascades': '2', 'r.VolumetricFog': '0', 'r.AmbientOcclusionLevels': '0',
        'r.SSR.Quality': '0', 'r.MotionBlurQuality': '0', 'r.DepthOfFieldQuality': '0',
        'r.LensFlareQuality': '0', 'r.SceneColorFringeQuality': '0', 'r.BloomQuality': '2',
        'r.RefractionQuality': '0', 'r.ViewDistanceScale': '0.7', 'foliage.DensityScale': '0.6',
        'grass.DensityScale': '0.6',
    },
}


# Point and spot light shadows at one fixed size per preset, with small lights unshadowed. UE4
# sizes them by their size on screen, so walking re-creates them constantly; those re-created
# maps flickered on the port (shadows blinking while moving, MediEvil's outdoor levels).
# The second number is r.Shadow.RadiusThreshold: lights smaller than that on screen cast no shadow.
# Every shadowed point light redraws its surroundings six times, and the port's GPU thread keeps
# 60 FPS up to about 2000 draws a frame; busy outdoor scenes reached 4000 (30 FPS) before.
LOCAL_SHADOWS = {'ultra': (1024, 0.06), 'high': (1024, 0.08), 'console': (512, 0.1),
                 'medium': (512, 0.12), 'low': (256, 0.15)}
# Sun/moon shadow cascades and distance per preset (each cascade redraws the scene once).
SUN_SHADOWS = {'ultra': ('3', '1.5'), 'high': ('3', '1.2'), 'console': ('2', '0.8'),
               'medium': ('2', '0.75'), 'low': ('2', '0.6')}


def apply(game, resolution='native', quality='console'):
    """Writes or removes the override pak. Returns a short description of the active state."""
    pak = paks_dir(game) / PAK_NAME
    values = dict(QUALITY.get(quality, {}))
    if quality in LOCAL_SHADOWS:
        size, threshold = LOCAL_SHADOWS[quality]
        values.update({'r.Shadow.MinResolution': str(size), 'r.Shadow.MaxResolution': str(size),
                       'r.Shadow.RadiusThreshold': str(threshold), 'r.Shadow.CacheWholeSceneShadows': '1'})
        cascades, distance = SUN_SHADOWS[quality]
        values.update({'r.Shadow.CSM.MaxCascades': cascades, 'r.Shadow.DistanceScale': distance})
    percentage = SCREEN_PERCENTAGE.get(resolution)
    if percentage:
        values['r.ScreenPercentage'] = percentage
    if not values:
        pak.unlink(missing_ok=True)
        return 'native 1080p, console quality'
    write_pak(pak, {CONFIG: set_values(read_file(game, CONFIG), values)})
    return (f'{resolution if percentage else "native 1080p"}'
            f'{f" (r.ScreenPercentage={percentage})" if percentage else ""}, {quality} quality')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--resolution', choices=list(SCREEN_PERCENTAGE), default='native')
    parser.add_argument('--quality', choices=list(QUALITY), default='console')
    args = parser.parse_args()
    print('Render resolution:', apply(args.game, args.resolution, args.quality))
