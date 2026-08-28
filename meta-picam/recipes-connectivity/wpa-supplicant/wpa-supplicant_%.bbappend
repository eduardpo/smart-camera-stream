# ==============================================================================
# File:        wpa-supplicant_%.bbappend
# Summary:     install wpa_supplicant.conf into /etc/wpa_supplicant and enable systemd service for wlan0
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://wpa_supplicant.conf"

# Enable systemd template service for wlan0 automatically
SYSTEMD_SERVICE:${PN}:append = " wpa_supplicant@wlan0.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${sysconfdir}/wpa_supplicant
    install -m 0600 ${WORKDIR}/wpa_supplicant.conf ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf
}