# ==============================================================================
# File:        pir-mqtt_1.0.bbappend
# Summary:     add pir-mqtt motion alert application to the image for Raspberry Pi with camera support
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


ESCRIPTION = "PIR-MQTT Motion alert with GStreamer and OpenCV"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"


# SRC_URI = "git://github.com/eduardpo/motion-stream.git;protocol=https;branch=master \
#            file://pir-mqtt.service \
#            file://config.ini \
# "


# # Use AUTOREV to always get the latest commit from the specified branch
# SRCREV = "${AUTOREV}"

# S = "${WORKDIR}/git"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " file://pir-mqtt.service \
            file://config.ini \
            file://pir_mqtt.cpp"

S = "${WORKDIR}"

DEPENDS = "mosquitto libgpiod gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly opencv"


do_compile() {
    export PKG_CONFIG_SYSROOT_DIR="${STAGING_DIR_HOST}"
    export PKG_CONFIG_PATH="${STAGING_DIR_HOST}${libdir}/pkgconfig:${STAGING_DIR_HOST}${datadir}/pkgconfig"

    ${CXX} ${S}/pir_mqtt.cpp -o pir_mqtt \
        $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 opencv4) \
        -lmosquitto -lgpiod -Wl,--hash-style=gnu
}

do_install() {
    install -Dm755 pir_mqtt ${D}${bindir}/pir_mqtt
    install -Dm644 ${WORKDIR}/pir-mqtt.service ${D}${systemd_system_unitdir}/pir-mqtt.service
    install -Dm644 ${WORKDIR}/config.ini ${D}${sysconfdir}/pir_mqtt/config.ini
}


inherit systemd pkgconfig

# SYSTEMD_SERVICE:${PN} = "pir_mqtt.service"
# SYSTEMD_AUTO_ENABLE:${PN} = "enable"


# Specify which package contains the systemd service.
# ${PN} expands to the package name, e.g., 'pir-mqtt-alert'.
SYSTEMD_PACKAGES = "${PN}"

# List the systemd service file(s) that should be enabled by default.
# This ensures that 'systemctl enable pir-mqtt-alert.service' is run during image creation.
SYSTEMD_SERVICE:${PN} = "pir-mqtt.service"

# Optional: Define RRECOMMENDS if there are other packages that are highly recommended
# but not strictly required for the application to function.
# For example, if you want to ensure the base system includes tools for debugging MQTT.
# RRECOMMENDS:${PN} += "mosquitto-clients"

# Optional: Specify dependencies for the runtime package.
# This ensures that libmosquitto and libgpiod runtime libraries are included in the image
# when this application is added.
RDEPENDS:${PN} += "mosquitto libgpiod gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly opencv"