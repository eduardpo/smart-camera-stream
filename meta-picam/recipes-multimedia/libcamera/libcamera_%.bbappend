# ==============================================================================
# File:        libcamera_%.bbappend
# Summary:     add libcamera framework to the image for Raspberry Pi with camera support
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


PACKAGECONFIG:append = " gst"

# GStreamer plugin runtime dependencies (minimal and correct)
RDEPENDS:${PN}-gst += " \
    gstreamer1.0-plugins-base \
"
