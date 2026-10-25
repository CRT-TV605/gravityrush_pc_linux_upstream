#!/usr/bin/env bash
# Installs what build.sh and packaging/linux/package.sh need on Debian/Ubuntu, Fedora and Arch
# (NixOS: shell.nix). Run as root or with sudo: sudo bash packaging/linux/install-deps.sh
# Debian/Ubuntu also get what packaging/linux/build-libs.sh needs for the AppImage (nasm, SDL3's
# X11/Wayland/audio headers).
# magic_enum, miniz, xbyak, imgui, sirit and FSR-Vulkan are submodules (gpu/third_party/).
# Debian 13 / Ubuntu 25.04 or newer: older releases have no SDL3 package.
set -euo pipefail
sudo=()
if (( EUID )); then sudo=(sudo); fi
if command -v apt-get >/dev/null; then
    "${sudo[@]}" apt-get update
    DEBIAN_FRONTEND=noninteractive "${sudo[@]}" apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates curl file patchelf squashfs-tools \
        python3 python3-venv python3-pip libpython3-dev python3-tk python3-pil python3-pil.imagetk python3-cryptography \
        libvulkan-dev glslang-tools libvulkan-memory-allocator-dev libsdl3-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
        libboost-dev libfmt-dev robin-map-dev libxxhash-dev libzydis-dev libx11-dev libfreetype-dev \
        spirv-cross perl \
        nasm zlib1g-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev libxfixes-dev \
        libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libpipewire-0.3-dev \
        libpulse-dev libasound2-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev libegl-dev
elif command -v dnf >/dev/null; then
    # FFmpeg: Fedora's ffmpeg-free, or RPM Fusion's ffmpeg-devel for every codec.
    "${sudo[@]}" dnf install -y \
        gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git curl file patchelf squashfs-tools \
        python3 python3-tkinter python3-pillow python3-pillow-tk python3-cryptography \
        vulkan-headers vulkan-loader-devel glslang vulkan-memory-allocator-devel SDL3-devel \
        libavcodec-free-devel libavformat-free-devel libavutil-free-devel libswscale-free-devel \
        libswresample-free-devel boost-devel fmt-devel robin-map-devel xxhash-devel zydis-devel \
        libX11-devel freetype-devel spirv-tools spirv-cross perl nasm
elif command -v pacman >/dev/null; then
    "${sudo[@]}" pacman -S --needed --noconfirm \
        base-devel cmake ninja pkgconf git curl file patchelf squashfs-tools \
        python python-pillow python-cryptography tk \
        vulkan-headers vulkan-icd-loader glslang vulkan-memory-allocator sdl3 ffmpeg \
        boost fmt robin-map xxhash zydis libx11 freetype2 spirv-cross perl nasm
else
    echo 'Unknown distribution: install the packages listed in packaging/linux/README-Linux.md.' >&2
    exit 1
fi

# The vendored shadPS4 video core needs Vulkan headers 1.4.366 or newer (Vulkan-Hpp included);
# Debian 13 and Ubuntu 25.x ship older ones. The headers alone (no loader) go to /usr/local,
# which compilers search before /usr. GR_VULKAN_HEADERS=0 skips this.
VULKAN_HEADERS=v1.4.366
installed=$(for header in /usr/local/include/vulkan/vulkan_core.h /usr/include/vulkan/vulkan_core.h; do
                [[ -f $header ]] && sed -n 's/^#define VK_HEADER_VERSION \([0-9]*\).*/\1/p' "$header" || true
            done | sort -n | tail -1)
if [[ ${GR_VULKAN_HEADERS:-1} != 0 && ${installed:-0} -lt ${VULKAN_HEADERS##*.} ]]; then
    echo "Vulkan headers ${installed:-missing} are too old: installing Vulkan-Headers $VULKAN_HEADERS into /usr/local"
    tmp=$(mktemp -d)
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$VULKAN_HEADERS" https://github.com/KhronosGroup/Vulkan-Headers "$tmp/src"
    cmake -S "$tmp/src" -B "$tmp/build" -G Ninja -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
    "${sudo[@]}" cmake --install "$tmp/build" >/dev/null
    rm -rf "$tmp"
fi
