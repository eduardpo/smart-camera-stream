# ==============================================================================
# File:        libcamera_0.4.0.bbappend
# Summary:     add libcamera framework to the image for Raspberry Pi with camera support
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


# Enable GStreamer plugin
PACKAGECONFIG:append = " gst"

PACKAGECONFIG[gst] = "-Dgstreamer=enabled,-Dgstreamer=disabled,gstreamer1.0 gstreamer1.0-plugins-base"

# Ensure the plugin is packaged
FILES:${PN}-gst = "${libdir}/gstreamer-1.0"

# GStreamer plugin runtime dependencies (minimal and correct)
RDEPENDS:${PN}-gst += " \
    gstreamer1.0-plugins-base \
"
