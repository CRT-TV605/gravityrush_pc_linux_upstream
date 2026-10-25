#!/usr/bin/env bash
# Builds dist/GravityRush-PC-<version>-Linux-x86_64.AppImage (and its .sha256, the release assets
# the launcher's update check looks for; the version is VERSION in launcher/gravityrush_launcher.py)
# after `bash build.sh`. Inside: the launcher frozen with PyInstaller (players need no Python; it
# also runs run.py with --run and the preparation scripts with --script, as on Windows), the
# runtime with libgow3gpu.so, the scripts, patches and the launcher's pictures and font.
#
# Libraries: SDL3 and FFmpeg ship in bin/lib, built by packaging/linux/build-libs.sh (SDL3 loads
# X11/Wayland/audio at run time, FFmpeg holds only the port's codecs), since few distributions
# have SDL3 and FFmpeg's sonames differ between them; with them the few small libraries the
# GPU library links (Zydis, fmt, xxHash). The Vulkan loader and drivers, glibc, libstdc++,
# X11/Wayland, audio servers, libdrm and the desktop libraries always come from the system
# (EXCLUDED below). The AppImage needs glibc as new as the build system's (Debian 13: 2.41).
#
# Needs python3 with venv and tkinter, patchelf, file and curl (packaging/linux/install-deps.sh,
# or the container in packaging/linux/Dockerfile). appimagetool is downloaded into out/ unless
# APPIMAGETOOL names one. GR_APPDIR_ONLY=1 stops after dist/gravityrush-linux/ (the AppDir).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
for file in out/gravityrush-runtime out/gravityrush-gpu-check out/gpu/libgow3gpu.so; do
    [[ -f $file ]] || { echo "Build first: bash build.sh ($file is missing)" >&2; exit 1; }
done
for tool in python3 patchelf ldd file; do
    command -v "$tool" >/dev/null || { echo "Need $tool (packaging/linux/install-deps.sh)" >&2; exit 1; }
done
python3 -c 'import tkinter' 2>/dev/null || { echo 'Need Python with tkinter (python3-tk)' >&2; exit 1; }
version=$(sed -n "s/^VERSION = '\([0-9.]*\)'.*/\1/p" launcher/gravityrush_launcher.py)
[[ -n $version ]] || { echo 'No VERSION in launcher/gravityrush_launcher.py' >&2; exit 1; }

# The launcher, frozen in a private venv (the system Python stays untouched).
if [[ ! -x out/pyenv-linux/bin/python ]]; then
    python3 -m venv out/pyenv-linux
fi
out/pyenv-linux/bin/python -m pip install -q --disable-pip-version-check pyinstaller cryptography pillow
# The scripts run inside the launcher executable (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil shlex struct tempfile xml.etree.ElementTree \
              xml.sax.saxutils urllib.request zlib hmac platform_paths game_profiles \
              graphics_options prepare link_libc link_modules mods patches save_backup content_profile \
              cryptography.hazmat.primitives.ciphers PIL.Image PIL.ImageTk PIL.ImageDraw PIL.ImageFont \
              PIL.ImageFilter PIL._tkinter_finder launcher_ui; do
    hidden+=(--hidden-import "$module")
done
out/pyenv-linux/bin/python -m PyInstaller --noconfirm --clean --log-level WARN \
    --name gravityrush-launcher --collect-submodules cryptography --distpath out/pyi-dist-linux \
    --workpath out/pyi-work-linux --specpath out/pyi-work-linux --paths "$PWD/scripts" --paths "$PWD/launcher" \
    "${hidden[@]}" launcher/gravityrush_launcher.py

# AppDir: the port lives in usr/lib/gravityrush (the launcher's folder is the port folder).
appdir=out/stage-linux/GravityRush.AppDir
port=$appdir/usr/lib/gravityrush
rm -rf -- out/stage-linux
mkdir -p "$port/bin/gpu" "$port/bin/lib" "$port/launcher/fonts" "$port/launcher/assets"
cp -r out/pyi-dist-linux/gravityrush-launcher/. "$port/"
# PyInstaller also collects system libraries; the launcher loads the Vulkan driver (its GPU list),
# which needs the system's C++ runtime and X11, and fontconfig must match the system's
# configuration files. (Pillow's own copies, with hashed names, stay.)
for lib in libstdc++.so.6 libgcc_s.so.1 libfontconfig.so.1 libfreetype.so.6 libexpat.so.1 libz.so.1 \
           libzstd.so.1 liblzma.so.5 libbz2.so.1.0 libffi.so.8 libX11.so.6 libXau.so.6 libXdmcp.so.6 \
           libXext.so.6 libXrender.so.1; do
    rm -f "$port/_internal/$lib"
done
cp launcher/gravityrush.png launcher/gravityrush.bmp launcher/background.jpg "$port/launcher/"
cp launcher/fonts/Cabin.ttf launcher/fonts/OFL-Cabin.txt "$port/launcher/fonts/"
# Without debug information (out/ keeps the symbols for crash reports).
strip --strip-debug -o "$port/bin/gravityrush-runtime" out/gravityrush-runtime
strip --strip-debug -o "$port/bin/gravityrush-gpu-check" out/gravityrush-gpu-check
strip --strip-debug -o "$port/bin/gpu/libgow3gpu.so" out/gpu/libgow3gpu.so
cp -r scripts patches "$port/"
# No package unpacker in releases: the player prepares the game folder with their own tools.
rm -f "$port/scripts/pkg_extract.py" "$port/scripts/pkg_keys.py"
cp run.py LICENSE README.md packaging/linux/README-Linux.md "$port/"

# Bundled libraries: everything the runtime and libgow3gpu.so link that is not EXCLUDED.
# Never bundled: glibc and the C++ runtime; what the Vulkan/VA/OpenCL drivers load themselves
# (a second copy would replace theirs); the display, audio and desktop stacks that every Linux
# desktop (and SteamOS) has: X11/Wayland, PulseAudio/PipeWire/ALSA, GLib/Cairo/Pango, GnuTLS,
# OpenSSL, Kerberos.
EXCLUDED='^(linux-vdso|ld-linux[a-z0-9_-]*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libmvec|libstdc\+\+|libgcc_s|libatomic'
EXCLUDED+='|libvulkan|libGL[a-zA-Z]*|libEGL|libOpenGL|libgbm|libdrm[a-z_]*|libOpenCL|libva[a-z0-9-]*|libvdpau|libvpl|libLLVM[a-zA-Z0-9.-]*|libelf|libsensors|libxshmfence'
EXCLUDED+='|libX[a-zA-Z0-9-]*|libxcb[a-z0-9-]*|libxkbcommon[a-z0-9-]*|libwayland-[a-z]*|libdecor-[0-9]*'
EXCLUDED+='|libasound|libpulse[a-z0-9.-]*|libpipewire-[0-9.]*|libjack|libdbus-1|libudev|libsystemd|libcap|libmount|libblkid|libselinux|libpcre2-[0-9]*'
EXCLUDED+='|libglib-2\.0|libgio-2\.0|libgobject-2\.0|libgmodule-2\.0|libgthread-2\.0|libffi|libcairo[a-z-]*|libpixman-1|libpango[a-z0-9.-]*|libharfbuzz[a-z-]*|libfribidi|libthai|libdatrie|libgraphite2|libgdk_pixbuf-2\.0|librsvg-2'
EXCLUDED+='|libfontconfig|libfreetype|libpng16|libbrotli[a-z]*|libexpat|libz|libzstd|liblzma|libbz2|libuuid'
EXCLUDED+='|libgnutls|libnettle|libhogweed|libgmp|libidn2|libunistring|libtasn1|libp11-kit|libssl|libcrypto|libgssapi_krb5|libkrb5[a-z]*|libk5crypto|libcom_err|libkeyutils)\.so'
# SDL3 and FFmpeg from packaging/linux/build-libs.sh (out/libs), not the build system's.
own=$PWD/out/libs/lib
search=out/gpu:$own
if ! LD_LIBRARY_PATH=$search ldd out/gravityrush-runtime | grep -q "$own/libSDL3"; then
    if [[ -z ${GR_SYSTEM_LIBS:-} ]]; then
        echo 'The runtime is linked with the system SDL3/FFmpeg, which tie the AppImage to this distribution.' >&2
        echo 'Run packaging/linux/build-libs.sh and rebuild (it says how), or set GR_SYSTEM_LIBS=1.' >&2
        exit 1
    fi
fi
libs=$(LD_LIBRARY_PATH=$search ldd out/gravityrush-runtime out/gpu/libgow3gpu.so |
       awk '$2 == "=>" && $3 ~ /^\// {print $3}' | sort -u)
for lib in $libs; do
    name=$(basename "$lib")
    [[ $name == libgow3gpu.so ]] && continue
    if [[ $name =~ $EXCLUDED ]]; then continue; fi
    cp -L "$lib" "$port/bin/lib/$name"
    strip --strip-unneeded "$port/bin/lib/$name" 2>/dev/null || true
    patchelf --set-rpath '$ORIGIN' "$port/bin/lib/$name"
done
# bin/gpu (as build.sh links it) and bin/lib; nothing needs LD_LIBRARY_PATH.
patchelf --set-rpath '$ORIGIN/gpu:$ORIGIN/lib' "$port/bin/gravityrush-runtime"
patchelf --set-rpath '$ORIGIN/../lib' "$port/bin/gpu/libgow3gpu.so"
missing=$(ldd "$port/bin/gravityrush-runtime" "$port/bin/gpu/libgow3gpu.so" | grep 'not found' || true)
[[ -z $missing ]] || { echo "Unresolved libraries in the package:" >&2; echo "$missing" >&2; exit 1; }

# The licenses of what ships: SDL3 and FFmpeg as build-libs.sh built them (with their exact
# sources, as the LGPL asks), the build system's packages for the other bundled libraries
# (their copyright files), and the Python parts frozen into the launcher.
licenses=$port/THIRD-PARTY-LICENSES
mkdir -p "$licenses"
{
    echo "Third-party components shipped with the Gravity Rush PC port (Linux)."
    echo "Each component's license is in the folder of its name."
    echo
    if [[ -f $port/bin/lib/libSDL3.so.0 && -f out/libs-src/SDL/LICENSE.txt ]]; then
        mkdir -p "$licenses/SDL3"
        cp out/libs-src/SDL/LICENSE.txt "$licenses/SDL3/"
        echo "bin/lib/libSDL3.so.0: SDL $(sed -n 's/^Version: //p' "$own/pkgconfig/sdl3.pc") (zlib license),"
        echo "  source: https://github.com/libsdl-org/SDL tag release-$(sed -n 's/^Version: //p' "$own/pkgconfig/sdl3.pc")"
    fi
    if [[ -f $port/bin/lib/libavcodec.so.61 && -f out/libs-src/FFmpeg/COPYING.LGPLv2.1 ]]; then
        mkdir -p "$licenses/FFmpeg"
        cp out/libs-src/FFmpeg/COPYING.LGPLv2.1 out/libs-src/FFmpeg/LICENSE.md "$licenses/FFmpeg/"
        echo "bin/lib/libav*, libsw*: FFmpeg $(cat out/libs-src/FFmpeg/RELEASE) (LGPL 2.1 or later; no GPL parts),"
        echo "  source: https://github.com/FFmpeg/FFmpeg tag n$(cat out/libs-src/FFmpeg/RELEASE), unmodified,"
        echo "  built by packaging/linux/build-libs.sh of this port with:"
        sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"/    \1/p' out/libs-src/FFmpeg/config.h | sed 's#--prefix=[^ ]* --libdir=[^ ]* ##'
    fi
    if command -v dpkg >/dev/null; then
        for lib in "$port"/bin/lib/*.so*; do
            [[ -f $own/$(basename "$lib") ]] && continue  # built above, not a package's
            dpkg -S "$(basename "$lib")" 2>/dev/null | head -1 | cut -d: -f1 || true
        done | sort -u | while read -r pkg; do
            [[ -n $pkg ]] || continue
            echo "Debian package $pkg $(dpkg-query -W -f='${Version}' "$pkg") (sources: https://sources.debian.org/src/$(dpkg-query -W -f='${source:Package}' "$pkg")/)"
            if [[ -f /usr/share/doc/$pkg/copyright ]]; then
                mkdir -p "$licenses/$pkg"
                cp "/usr/share/doc/$pkg/copyright" "$licenses/$pkg/"
            fi
        done
    else
        ls "$port/bin/lib"
    fi
    echo
    echo "The launcher is frozen with PyInstaller (GPL with a bootloader exception) and contains Python"
    echo "(PSF License), Tcl/Tk (BSD-style), Pillow (MIT-CMU) and cryptography (Apache-2.0 / BSD)."
    echo "The launcher font Cabin is under the SIL Open Font License (launcher/fonts/OFL-Cabin.txt)."
    echo "The port itself is GPL-2.0-or-later (LICENSE); its source: https://github.com/LucaPazienza/gravityrush_pc"
} > "$licenses/README.txt"

# AppImage metadata: the entry point, the desktop entry and the icon.
install -m 755 packaging/linux/AppRun "$appdir/AppRun"
cp packaging/linux/gravityrush.desktop "$appdir/"
cp launcher/gravityrush.png "$appdir/gravityrush.png"
ln -sf gravityrush.png "$appdir/.DirIcon"
mkdir -p "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/256x256/apps"
cp packaging/linux/gravityrush.desktop "$appdir/usr/share/applications/"
cp launcher/gravityrush.png "$appdir/usr/share/icons/hicolor/256x256/apps/"
find "$appdir" -name __pycache__ -prune -exec rm -r {} +

# No game files and nothing a player made (saves, shader caches built from the game's shaders,
# settings, logs) may ship: stop when one is found.
forbidden=$(find "$appdir" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname user -o -iname cache -o -iname '*.log' -o -iname gravityrush.ini -o -iname settings.json \
    -o -iname mods.json -o -iname patches.json -o -iname fsr4_shaders \
    -o -iname '*.spv' -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
mkdir -p dist
rm -rf dist/gravityrush-linux
cp -a "$appdir" dist/gravityrush-linux
if [[ -n ${GR_APPDIR_ONLY:-} ]]; then
    echo "AppDir: $PWD/dist/gravityrush-linux (run ./dist/gravityrush-linux/AppRun)"
    exit 0
fi

tool=${APPIMAGETOOL:-}
if [[ -z $tool ]]; then
    tool=out/appimagetool-x86_64.AppImage
    if [[ ! -x $tool ]]; then
        curl -fsSL --retry 3 -o "$tool.part" \
            https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
        chmod +x "$tool.part" && mv "$tool.part" "$tool"
    fi
fi
# appimagetool is an AppImage itself. Containers have no FUSE (APPIMAGE_EXTRACT_AND_RUN), and
# emulators (Rosetta, qemu-user) do not start AppImages at all: there its squashfs, which
# starts right after the ELF runtime's section headers, is unpacked and run instead.
run_tool=("$tool")
if ! APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --version >/dev/null 2>&1; then
    command -v unsquashfs >/dev/null || { echo 'Need unsquashfs (squashfs-tools) to run appimagetool here' >&2; exit 1; }
    offset=$(python3 -c 'import struct, sys
h = open(sys.argv[1], "rb").read(64)
shoff, = struct.unpack_from("<Q", h, 40); size, count = struct.unpack_from("<HH", h, 58)
print(shoff + size * count)' "$tool")
    rm -rf out/appimagetool
    unsquashfs -q -o "$offset" -d out/appimagetool "$tool" >/dev/null
    run_tool=(out/appimagetool/AppRun)
fi
image="GravityRush-PC-$version-Linux-x86_64.AppImage"
rm -f "dist/$image" "dist/$image.sha256"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "${run_tool[@]}" --no-appstream "$appdir" "dist/$image"
# The update check compares against this (and against GitHub's own digest of the asset).
(cd dist && sha256sum "$image" > "$image.sha256")
du -sh "$appdir" "dist/$image"
