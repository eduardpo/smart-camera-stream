#!/bin/bash

# Define the image name (e.g., core-image-picam)
IMAGE_NAME="core-image-picam"

# --- Get the deployment directory ---
# This command extracts the 'D' variable which contains the path to the image directory.
# We then manipulate the path to get to the 'deploy-core-image-picam-image-complete' directory.
DEPLOY_DIR=$(bitbake -e "${IMAGE_NAME}" | grep ^D= | sed 's/^D="//;s/"$//')

#if [ -z "${DEPLOY_DIR}" ]; then
#    echo "Error: Could not determine deployment directory for ${IMAGE_NAME}."
#    exit 1
#fi

# Adjust the DEPLOY_DIR to point to the 'deploy-core-image-picam-image-complete' directory
# This assumes a standard Yocto build structure.
# Example: /home/ed/workspace/build_rpi/tmp/work/raspberrypi4_64-poky-linux/core-image-picam/1.0-r0/image
# becomes /home/ed/workspace/build_rpi/tmp/work/raspberrypi4_64-poky-linux/core-image-picam/1.0-r0/deploy-core-image-picam-image-complete/

# First, extract the base path up to the recipe version (e.g., 1.0-r0)
BASE_PATH=$(echo "${DEPLOY_DIR}" | sed 's/\/image$//')

# Construct the full deploy-complete directory path
#DEPLOY_COMPLETE_DIR="${BASE_PATH}/deploy-${IMAGE_NAME}-image-complete"
DEPLOY_COMPLETE_DIR="/home/ed/workspace/final-project-eduardpo/build_rpi/tmp/deploy/images/raspberrypi4-64"

if [ ! -d "${DEPLOY_COMPLETE_DIR}" ]; then
    echo "Error: Deployment complete directory not found: ${DEPLOY_COMPLETE_DIR}"
    exit 1
fi

echo "Deployment complete directory: ${DEPLOY_COMPLETE_DIR}"

# --- Find the .wic.bz2 file ---
# We use find to locate the most recent .wic.bz2 file in the deploy complete directory.
# The -print0 and xargs -0 are used for handling filenames with spaces or special characters,
# though less common for image files.
WIC_BZ2_FILE=$(find "${DEPLOY_COMPLETE_DIR}" -maxdepth 1 -name "*.rootfs.wic.bz2" -printf "%T@ %p\n" | sort -nr | head -n 1 | awk '{print $2}')

if [ -z "${WIC_BZ2_FILE}" ]; then
    echo "Error: No .wic.bz2 file found in ${DEPLOY_COMPLETE_DIR}."
    exit 1
fi

echo "Found .wic.bz2 file: ${WIC_BZ2_FILE}"

# Extract just the filename for copying and decompressing in the current directory
WIC_BZ2_BASENAME=$(basename "${WIC_BZ2_FILE}")

# --- Copy and Decompress ---
echo "Copying ${WIC_BZ2_BASENAME} to current directory..."
cp "${WIC_BZ2_FILE}" .

if [ $? -ne 0 ]; then
    echo "Error: Failed to copy ${WIC_BZ2_FILE}."
    exit 1
fi

echo "Decompressing ${WIC_BZ2_BASENAME}..."
bzip2 -d "${WIC_BZ2_BASENAME}"

if [ $? -ne 0 ]; then
    echo "Error: Failed to decompress ${WIC_BZ2_BASENAME}."
    exit 1
fi

# Determine the name of the decompressed .wic file
WIC_FILE="${WIC_BZ2_BASENAME%.bz2}"

if [ ! -f "${WIC_FILE}" ]; then
    echo "Error: Decompressed .wic file not found: ${WIC_FILE}"
    exit 1
fi

echo "Decompressed to: ${WIC_FILE}"

# --- Copy to destination ---
if [ -n "$1" ]; then
    DEST_DIR="$1"
else
    DEST_DIR="/mnt/c/rpi"
fi

mkdir -p "${DEST_DIR}"

echo "Copying ${WIC_FILE} to ${DEST_DIR}..."
cp "${WIC_FILE}" "${DEST_DIR}"

if [ $? -ne 0 ]; then
    echo "Error: Failed to copy ${WIC_FILE} to ${DEST_DIR}."
    exit 1
fi

echo "Successfully copied ${WIC_FILE} to ${DEST_DIR}"
echo "Cleaning up temporary .wic file..."
rm "${WIC_FILE}"

echo "Automation complete!"
