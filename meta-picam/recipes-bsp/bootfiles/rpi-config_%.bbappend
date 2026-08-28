# ==============================================================================
# File:        rpi-config_%.bbappend
# Summary:     add include usercfg.txt to config.txt and comment out vc4-kms-v3d dtoverlay
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


# rpi-config's own do_deploy() ends with:
#     printf "${RPI_EXTRA_CONFIG}\n" >> $CONFIG
# writing to ${DEPLOYDIR}/${BOOTFILES_DIR_NAME}/config.txt — the exact file
# and path that actually ships to the FAT boot partition. This is the
# supported extension point; 
RPI_EXTRA_CONFIG = "include usercfg.txt"

# Comment out the vc4-kms-v3d dtoverlay baked into meta-raspberrypi's own
# config.txt template. RPI_EXTRA_CONFIG can only append new lines, it can't
# remove/disable one already written earlier in the file by the base
# recipe -- this needs a direct sed against the deployed file instead.
# Safe to rerun: the base recipe's do_deploy() copies its template fresh
# into DEPLOYDIR on every build (not incrementally), so this can't
# accidentally double-comment the line across rebuilds.
do_deploy:append() {
    sed -i 's/^dtoverlay=vc4-kms-v3d/#dtoverlay=vc4-kms-v3d/' ${DEPLOYDIR}/${BOOTFILES_DIR_NAME}/config.txt
}