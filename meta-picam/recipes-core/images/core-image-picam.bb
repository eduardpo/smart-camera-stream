SUMMARY = "Minimal headless camera stream image for Raspberry Pi 4"
DESCRIPTION = "Production-ready headless Wrynose image containing camera and network stack."
LICENSE = "MIT"

# 1. Base Strategy: Safely pull in the core-image-minimal base
require recipes-core/images/core-image-minimal.bb

# 2. Package Installation: Strict modern override syntax
IMAGE_INSTALL:append = " \
    libcamera \
    libcamera-gst \
    v4l-utils \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvertscale \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    x264 \
    openssh \
    iw \
    kernel-module-brcmfmac-cyw \
    kernel-module-brcmfmac \
    kernel-module-bcm2835-isp \
    kernel-module-bcm2835-unicam-legacy \
    kernel-module-i2c-mux-pinctrl \
    linux-firmware-rpidistro-bcm43455 \
    wpa-supplicant \
    opencv \
    mosquitto \
    mosquitto-clients \
    libgpiod \
    pir-mqtt \
    rpicam-apps \
    i2c-tools \
    mediamtx \
    tcpdump \
"

# 3. Defensive Isolation: Hard block graphics at the image level
IMAGE_FEATURES:remove = "x11-base x11 wayland splashvulkan"

# 4. Modern Dependency Pruning
BAD_RECOMMENDATIONS += " \
    udev-extraconf \
    gstreamer1.0-plugins-base-apps \
"
