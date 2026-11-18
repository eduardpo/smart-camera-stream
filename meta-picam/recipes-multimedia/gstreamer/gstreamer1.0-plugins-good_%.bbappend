# ==============================================================================
# File:        gstreamer1.0-plugins-good_%.bbappend
# Summary:     Enable the v4l2 hw decoder for gstreamer-good
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-11-02
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


# Force the v4l2 plugin to be compiled in:
PACKAGECONFIG:append:pn-gstreamer1.0-plugins-good = " v4l2"