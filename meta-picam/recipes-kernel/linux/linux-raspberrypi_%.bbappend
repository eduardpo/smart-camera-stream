# ==============================================================================
# File:        linux-raspberrypi_%.bbappend
# Summary:     mofify kernel config to enable imx219 camera driver
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://imx219.cfg"