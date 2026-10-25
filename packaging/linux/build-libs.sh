#!/usr/bin/env bash
# Builds the SDL3 and FFmpeg the AppImage ships, into out/libs (packaging/linux/package.sh):
#   bash packaging/linux/build-libs.sh
#   rm -rf out/gpu && PKG_CONFIG_PATH=$PWD/out/libs/lib/pkgconfig bash build.sh
#   bash packaging/linux/package.sh
# Distribution builds link X11, PipeWire, VA-API, VDPAU, OpenCL, Intel VPL, librsvg and dozens
# of codecs directly, so an AppImage with them depends on what each system happens to have.
# These builds do not: SDL3 opens X11/Wayland/PipeWire/PulseAudio/ALSA/libdecor/udev at run
# time (whatever the system has), and FFmpeg holds only what the port decodes (H.264/HEVC
# movies in MP4, AAC/MP3 audio, PNG), needing nothing but glibc and zlib.
# A plain `bash build.sh` (development) keeps using the system's SDL3 and FFmpeg.
# Needs the SDL3 development dependencies and nasm (packaging/linux/install-deps.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
SDL_TAG=${SDL_TAG:-release-3.4.18}
FFMPEG_TAG=${FFMPEG_TAG:-n7.1.5}
prefix=$PWD/out/libs
src=out/libs-src
mkdir -p "$src"
jobs=$(nproc)

fetch() { # fetch <url> <tag> <dir>
    if [[ ! -d $3/.git ]] || [[ $(git -C "$3" describe --tags 2>/dev/null) != "$2" ]]; then
        rm -rf "$3"
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$3"
    fi
}

if [[ ! -f $prefix/lib/pkgconfig/sdl3.pc ]] || ! grep -q "Version: ${SDL_TAG#release-}" "$prefix/lib/pkgconfig/sdl3.pc"; then
    fetch https://github.com/libsdl-org/SDL "$SDL_TAG" "$src/SDL"
    # SDL_DEPS_SHARED (the default): every system library is opened at run time.
    cmake -S "$src/SDL" -B "$src/SDL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_DEPS_SHARED=ON >/dev/null
    cmake --build "$src/SDL/build" -j "$jobs" >/dev/null
    cmake --install "$src/SDL/build" >/dev/null
    echo "SDL3 ${SDL_TAG#release-} in $prefix"
fi

if [[ ! -f $prefix/lib/pkgconfig/libavcodec.pc || ! -f $prefix/.ffmpeg-$FFMPEG_TAG ]]; then
    fetch https://github.com/FFmpeg/FFmpeg "$FFMPEG_TAG" "$src/FFmpeg"
    (
        cd "$src/FFmpeg"
        # --disable-autodetect: no VA-API, VDPAU, Vulkan, X11, SDL, network or external codecs.
        ./configure --prefix="$prefix" --libdir="$prefix/lib" --enable-shared --disable-static \
            --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything \
            --disable-avdevice --disable-avfilter --enable-zlib --enable-pic \
            --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
            --enable-decoder=h264,hevc,aac,aac_latm,aac_fixed,mp3,mp3float,png,pcm_s16le,pcm_s16be,pcm_f32le \
            --enable-demuxer=mov,h264,hevc,aac,mp3,wav,mpegts,mpegps,m4v \
            --enable-parser=h264,hevc,aac,aac_latm,mpegaudio,png \
            --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata \
            --enable-protocol=file,pipe >/dev/null
        make -j "$jobs" >/dev/null
        make install >/dev/null
    )
    touch "$prefix/.ffmpeg-$FFMPEG_TAG"
    echo "FFmpeg ${FFMPEG_TAG#n} in $prefix"
fi
echo "Next: rm -rf out/gpu && PKG_CONFIG_PATH=$prefix/lib/pkgconfig bash build.sh && bash packaging/linux/package.sh"
