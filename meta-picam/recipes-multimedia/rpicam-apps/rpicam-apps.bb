# ==============================================================================
# File:        rpicam-apps.bb
# Summary:     add Raspberry Pi camera applications (formerly libcamera-apps) to the image for Raspberry Pi with camera support
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


SUMMARY = "Raspberry Pi camera applications using libcamera"
DESCRIPTION = "Official Raspberry Pi camera apps (formerly libcamera-apps), built on top of libcamera."
HOMEPAGE = "https://github.com/raspberrypi/rpicam-apps"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://license.txt;md5=a0013d1b383d72ba4bdc5b750e7d1d77"

PV = "1.12.0"
SRCREV = "f5174e5099fd1e1646622d45f86e6ac62c912776"
SRC_URI = "git://github.com/raspberrypi/rpicam-apps.git;protocol=https;branch=main;tag=v${PV}"

DEPENDS = "libcamera libjpeg-turbo tiff boost meson-native ninja-native libexif libpng"
PREFERRED_VERSION_libcamera = "0.6.0"

inherit meson pkgconfig

RDEPENDS:${PN} += "python3-core"

do_install:append() {
    sed -i \
        -e 's|${RECIPE_SYSROOT}${libdir}/libboost_program_options\.so\.[^ ]*|-lboost_program_options|' \
        ${D}${libdir}/pkgconfig/rpicam_app.pc
}

FILES:${PN} += " \
    ${libdir}/rpicam-apps-postproc \
    ${libdir}/rpicam-apps-preview \
    ${datadir}/rpi-camera-assets \
"
