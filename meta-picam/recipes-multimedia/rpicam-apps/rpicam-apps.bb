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

SRCREV = "18f23231bd805810d7056d20e03da709f1042940"
SRC_URI = "git://github.com/raspberrypi/rpicam-apps.git;protocol=https;branch=main"

S = "${WORKDIR}/git"

DEPENDS = "libcamera libjpeg-turbo tiff boost meson-native ninja-native libexif libpng"
PREFERRED_VERSION_libcamera = "0.4.0"

inherit meson pkgconfig

do_install:append() {
     # Install binaries
    install -d ${D}${bindir}
    for app in rpicam-hello rpicam-jpeg rpicam-raw rpicam-still rpicam-vid; do
        install -m 0755 ${B}/apps/$app ${D}${bindir}/
    done


    # Install shared library and symlink. TODO: ANY VERISON SUPPORT
    install -d ${D}${libdir}
    install -m 0644 ${B}/rpicam_app.so.1.5.3 ${D}${libdir}/
    ln -sf rpicam_app.so.1.5.3 ${D}${libdir}/rpicam_app.so

    # Extract the installed versioned .so file name
    # versioned=$(basename $(find ${B} -name 'rpicam_app.so.*' | head -n 1))
    # ln -sf ${versioned} ${D}${libdir}/rpicam_app.so

    # NO NEED TO INSTALL AS MESON TAKES CARE OF
    # # Install postproc shared object
    # install -d ${D}${libdir}/rpicam-apps-postproc
    # install -m 0644 ${B}/post_processing/core-postproc.so ${D}${libdir}/rpicam-apps-postproc/

    # # Install camera assets
    # install -d ${D}${datadir}/rpi-camera-assets
    # install -m 0644 ${S}/assets/*.json ${D}${datadir}/rpi-camera-assets/
}

FILES:${PN} += " \
    ${bindir}/rpicam-* \
    ${libdir}/rpicam_app.so.1.5.3 \
    ${libdir}/rpicam-apps-postproc/core-postproc.so \
    ${datadir}/rpi-camera-assets \
    ${datadir}/rpi-camera-assets/* \
"
FILES:${PN}-dev += "${libdir}/rpicam_app.so"

RDEPENDS:${PN} += "python3-core"
