# ==============================================================================
# File:        systemd_%.bbappend
# Summary:     create wlan0 network configuration
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://25-wlan0.network"

do_install:append() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/25-wlan0.network ${D}${sysconfdir}/systemd/network/
}