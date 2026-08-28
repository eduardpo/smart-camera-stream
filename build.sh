# ==============================================================================
# File:        build.sh
# Summary:     This script automates the setup and build process for a Yocto Project environment, 
#              specifically targeting Raspberry Pi 4 (64-bit) using the meta-picam layer. 
#              It initializes git submodules, configures local.conf with necessary settings, 
#              adds required layers, and finally builds the specified image.
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


#!/bin/bash

git submodule sync
git submodule update --init --recursive
git submodule update --remote --force

# Use pecific commit
git -C meta-openembedded checkout --force f8dddbf

PROJ_DIR="$(pwd)"

# local.conf won't exist until this step on first execution
source poky/oe-init-build-env build_rpi
echo "Current path is: $(pwd)"


check_and_add_conf_line() {
  local CONFLINE="$1"
  cat conf/local.conf | grep "${CONFLINE}" > /dev/null
  local_conf_info=$?
  if [ $local_conf_info -ne 0 ];then
    echo "Append ${CONFLINE} in the local.conf file"
    echo ${CONFLINE} >> conf/local.conf
  else
    echo "${CONFLINE} already exists in the local.conf file"
  fi
  return 0
}

#CONFLINE="MACHINE = \"qemuarm64\""
CONFLINE="MACHINE = \"raspberrypi4-64\""
#CONFLINE="MACHINE = \"raspberrypi4\""
check_and_add_conf_line "${CONFLINE}"

# Forces BitBake to handle downloads on non-POSIX filesystems smoothly
BB_STRICT_CHECKSUM = "0"
# Fixes SQLite database locking issues on NTFS mounts
BB_SIGNATURE_HANDLER = "OEBasicHash"
# Bypass strict NTFS permission checks inside Docker
check_and_add_conf_line "export CCACHE_PERMISSIONS_CHECK_BYPASS = \"1\""

# Enable ccache for your recipes
check_and_add_conf_line "INHERIT += \"ccache\""
# yocto cache for docker compose.yml
check_and_add_conf_line "DL_DIR = \"/downloads\""
check_and_add_conf_line "CCACHE_TOPDIR = \"/ccache\""
check_and_add_conf_line "export CCACHE_DIR = \"/ccache\""

# 4-CORE & 16GB RAM PERFORMANCE OPTIMIZATION
check_and_add_conf_line "BB_NUMBER_THREADS = \"4\""
check_and_add_conf_line "PARALLEL_MAKE = \"-j 4\""

check_and_add_conf_line "INIT_MANAGER = \"systemd\""
check_and_add_conf_line "DISTRO_FEATURES:append = \" systemd\""
check_and_add_conf_line "VIRTUAL-RUNTIME_init_manager = \"systemd\""
check_and_add_conf_line "LICENSE_FLAGS_ACCEPTED = \"commercial synaptics-killswitch\""
check_and_add_conf_line "EXTRA_IMAGE_FEATURES += \"ssh-server-dropbear\""
check_and_add_conf_line "INHERIT += \"rm_work\""

declare -a layer_specs=(
  "meta-openembedded/meta-oe|$PROJ_DIR/meta-openembedded/meta-oe"
  "meta-openembedded/meta-python|$PROJ_DIR/meta-openembedded/meta-python"
  "meta-openembedded/meta-multimedia|$PROJ_DIR/meta-openembedded/meta-multimedia"
  "meta-openembedded/meta-networking|$PROJ_DIR/meta-openembedded/meta-networking"
  "meta-raspberrypi|$PROJ_DIR/meta-raspberrypi"
  "meta-picam|$PROJ_DIR/meta-picam"
)

for spec in "${layer_specs[@]}"; do
  IFS='|' read -r layer_name layer_path <<< "$spec"
  if ! (bitbake-layers show-layers 2>/dev/null | grep -q "$layer_name"); then
    echo "Adding $layer_name layer"
    bitbake-layers add-layer "$layer_path"
  else
    echo "$layer_name layer already exists"
  fi
done


#TERGET_IMAGE=core-image-minimal
#TERGET_IMAGE=core-image-base
TERGET_IMAGE=core-image-picam

if [ $# -gt 0 ]; then
  TERGET_IMAGE="$1"
fi

check_and_add_conf_line "INHERIT += \"rm_work\""

echo "Current path is: $(pwd)"
echo "Building $TERGET_IMAGE..."

set -e
bitbake $TERGET_IMAGE
