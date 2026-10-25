# Build environment for the Linux build and the AppImage (CI and machines without the
# libraries). Debian 13: SDL3 packaged, glibc 2.41, so the AppImage runs on distributions
# from 2025 on (Ubuntu 25.04+, Fedora 42+, Debian 13+, Arch; SteamOS: check `ldd --version`).
#   docker build -t gravityrush-build packaging/linux
#   docker run --rm -v "$PWD:/src" -w /src gravityrush-build bash -c 'bash packaging/linux/build-libs.sh &&
#       PKG_CONFIG_PATH=$PWD/out/libs/lib/pkgconfig bash build.sh && bash packaging/linux/package.sh'
FROM debian:13
COPY install-deps.sh /tmp/install-deps.sh
RUN bash /tmp/install-deps.sh && rm -rf /var/lib/apt/lists/* /tmp/install-deps.sh && \
    git config --system --add safe.directory '*'
