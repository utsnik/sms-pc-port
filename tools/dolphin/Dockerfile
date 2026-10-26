FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ninja-build \
    libasound2-dev libavcodec-dev libavformat-dev libavutil-dev libbluetooth-dev \
    libbz2-dev libcurl4-openssl-dev libenet-dev libevdev-dev libhidapi-dev \
    liblz4-dev libmbedtls-dev libminiupnpc-dev libpulse-dev libsfml-dev \
    libswscale-dev libudev-dev libusb-1.0-0-dev libx11-dev libxi-dev libxrandr-dev \
    libxxhash-dev libzstd-dev libspng-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
