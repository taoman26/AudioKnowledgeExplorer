# Build environment of the AppImage: Ubuntu 22.04 (glibc 2.35), so the result runs on 22.04 and newer.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git file xz-utils patchelf desktop-file-utils \
        build-essential cmake ninja-build pkg-config \
        qt6-base-dev qt6-base-private-dev qt6-multimedia-dev libqt6sql6-sqlite libgl1-mesa-dev libxkbcommon-dev libxcb1-dev libx11-xcb-dev \
        libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
        libmecab-dev mecab-jumandic-utf8 \
        extra-cmake-modules libfcitx5utils-dev \
        gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-libav \
        gstreamer1.0-pulseaudio gstreamer1.0-alsa libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*
