#!/bin/bash

git submodule sync
git submodule update --init --recursive
git submodule update --remote --force

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

# cat conf/local.conf | grep "${CONFLINE}" > /dev/null
# local_conf_info=$?
# if [ $local_conf_info -ne 0 ];then
# 	echo "Append ${CONFLINE} in the local.conf file"
# 	echo ${CONFLINE} >> conf/local.conf
# else
# 	echo "${CONFLINE} already exists in the local.conf file"
# fi


CONFLINE="DL_DIR = \"$(pwd)/../downloads/\""
check_and_add_conf_line "${CONFLINE}"

# cat conf/local.conf | grep "${CONFLINE}" > /dev/null
# local_conf_info=$?
# if [ $local_conf_info -ne 0 ];then
# 	echo "Append ${CONFLINE} in the local.conf file"
# 	echo ${CONFLINE} >> conf/local.conf
# else
# 	echo "${CONFLINE} already exists in the local.conf file"
# fi


CONFLINE="INIT_MANAGER = \"systemd\""
check_and_add_conf_line "${CONFLINE}"

# cat conf/local.conf | grep "${CONFLINE}" > /dev/null
# local_conf_info=$?
# if [ $local_conf_info -ne 0 ];then
# 	echo "Append ${CONFLINE} in the local.conf file"
# 	echo ${CONFLINE} >> conf/local.conf
# else
# 	echo "${CONFLINE} already exists in the local.conf file"
# fi

CONFLINE="DISTRO_FEATURES:append = \" systemd\""
check_and_add_conf_line "${CONFLINE}"

# cat conf/local.conf | grep "${CONFLINE}" > /dev/null
# local_conf_info=$?
# if [ $local_conf_info -ne 0 ];then
# 	echo "Append ${CONFLINE} in the local.conf file"
# 	echo ${CONFLINE} >> conf/local.conf
# else
# 	echo "${CONFLINE} already exists in the local.conf file"
# fi

CONFLINE="VIRTUAL-RUNTIME_init_manager = \"systemd\""
check_and_add_conf_line "${CONFLINE}"

# cat conf/local.conf | grep "${CONFLINE}" > /dev/null
# local_conf_info=$?
# if [ $local_conf_info -ne 0 ];then
# 	echo "Append ${CONFLINE} in the local.conf file"
# 	echo ${CONFLINE} >> conf/local.conf
# else
# 	echo "${CONFLINE} already exists in the local.conf file"
# fi

CONFLINE="LICENSE_FLAGS_ACCEPTED = \"commercial\""
check_and_add_conf_line "${CONFLINE}"

# cat conf/local.conf | grep "${CONFLINE}" > /dev/null
# local_conf_info=$?
# if [ $local_conf_info -ne 0 ];then
# 	echo "Append ${CONFLINE} in the local.conf file"
# 	echo ${CONFLINE} >> conf/local.conf
# else
# 	echo "${CONFLINE} already exists in the local.conf file"
# fi

# bitbake-layers show-layers | grep "meta-aesd" > /dev/null
# layer_info=$?

# if [ $layer_info -ne 0 ];then
# 	echo "Adding meta-aesd layer"
# 	bitbake-layers add-layer ../meta-aesd
# else
# 	echo "meta-aesd layer already exists"
# fi

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
#TERGET_IMAGE=core-image-aesd

if [ $# -gt 0 ]; then
  TERGET_IMAGE="$1"
fi

# add EXTRA_IMAGE_FEATURES here only for NOT CUSTOM images
if [ "$TERGET_IMAGE" != "core-image-picam" ]; then
  CONFLINE="EXTRA_IMAGE_FEATURES += \"ssh-server-dropbear\""
  cat conf/local.conf | grep "${CONFLINE}" > /dev/null
  local_conf_info=$?

  if [ $local_conf_info -ne 0 ];then
    echo "Append ${CONFLINE} in the local.conf file"
    echo ${CONFLINE} >> conf/local.conf
    
  else
    echo "${CONFLINE} already exists in the local.conf file"
  fi
fi

check_and_add_conf_line "INHERIT += \"rm_work\""

echo "Current path is: $(pwd)"
echo "Building $TERGET_IMAGE..."

set -e
bitbake $TERGET_IMAGE
