# ==============================================================================
# File:        core-image-picam.bbappend
# Summary:     create and image for Raspberry Pi with camera support
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


DESCRIPTION = "Custom image for RPi4 with libcamera, GStreamer, OpenCV, Wi-Fi, SSH"
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh"

IMAGE_INSTALL += " \
    libcamera \
    v4l-utils \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    openssh \
    iw \
    wpa-supplicant \
    opencv \
    mosquitto \
    libgpiod \
    pir-mqtt \
    rpicam-apps \
    i2c-tools \
"

# SYSTEMD_AUTO_ENABLE:pn-wpa-supplicant = "enable"

# x264
IMAGE_INSTALL += "x264 libcamera-gst"

# The correct recipe name for Kirkstone software decoders):
#IMAGE_INSTALL += "gst-libav"
IMAGE_INSTALL += "gstreamer1.0-libav"

# RTSP server (Go-based, MediaMTX) - pir-mqtt pushes its motion stream into
# this locally (127.0.0.1:8554/cam); the PC pulls it over the network.
IMAGE_INSTALL += "mediamtx"

# debugging tools
IMAGE_INSTALL += " mosquitto mosquitto-clients"
IMAGE_INSTALL += " tcpdump"

IMAGE_BOOT_FILES += "bootfiles/*"
