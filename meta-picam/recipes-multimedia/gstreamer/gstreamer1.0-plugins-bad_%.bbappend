# ==============================================================================
# File:        gstreamer1.0-plugins-bad_%.bbappend
# Summary:     Enable the libcamerasrc plugin from gstreamer-bad
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


# Enable the libcamerasrc plugin from gstreamer-bad
PACKAGECONFIG:append = " libcamera"

DEPENDS += "libcamera"
