# ==============================================================================
# File:        rpi-bootfiles.bbappend
# Summary:     install usercfg.txt into the boot partition of the SD card
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://usercfg.txt"

# Ship usercfg.txt itself into the real bootfiles deploy dir (confirmed from
# upstream rpi-bootfiles.bb: do_deploy() writes to
# ${DEPLOYDIR}/${BOOTFILES_DIR_NAME}, which IMAGE_BOOT_FILES += "bootfiles/*"
# in core-image-picam.bb then picks up). This is a plain deploy target, not
# do_install — rpi-bootfiles carries INHIBIT_DEFAULT_DEPS = "1" and produces
# no package, so anything written under do_install's ${D} never reaches the
# card regardless of path.
do_deploy:append() {
    install -Dm0644 ${WORKDIR}/usercfg.txt ${DEPLOYDIR}/${BOOTFILES_DIR_NAME}/usercfg.txt
}