# ==============================================================================
# File:        gstreamer1.0-plugins-ugly_%.bbappend
# Summary:     Enable the x264 software encoder plugin for gstreamer-ugly
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


# Force the x264 plugin to be compiled in:
PACKAGECONFIG:append = " x264"