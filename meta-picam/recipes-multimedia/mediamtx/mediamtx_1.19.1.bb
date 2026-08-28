# ==============================================================================
# File:        mediamtx_1.19.1.bb
# Summary:     add MediaMTX Go-based RTSP/RTMP/HLS/WebRTC media server to the image for Raspberry Pi with camera support
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


SUMMARY = "MediaMTX - Go-based RTSP/RTMP/HLS/WebRTC media server and proxy"
DESCRIPTION = "Ready-to-use, zero-dependency real-time media server used here as the \
RTSP endpoint that pir-mqtt pushes its motion-triggered video stream into. \
Runs locally on the RPi so pir-mqtt can rtspclientsink to 127.0.0.1:8554, \
while remote clients (the PC) pull the stream over the network."
HOMEPAGE = "https://github.com/bluenviron/mediamtx"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=77fd2623bd5398430be5ce60489c2e81"

# MediaMTX is distributed as prebuilt, statically-linked Go binaries built by
# GitHub's own release workflow (see the release notes: binaries are produced
# reproducibly by .github/workflows/release.yml and their checksums are
# published alongside every release). We fetch the arch-matching tarball
# directly rather than cross-compiling Go in-tree.
MEDIAMTX_ARCH = "unsupported-arch-${TARGET_ARCH}"
MEDIAMTX_ARCH:aarch64 = "arm64"
MEDIAMTX_ARCH:arm = "armv7"

SRC_URI = "https://github.com/bluenviron/mediamtx/releases/download/v${PV}/mediamtx_v${PV}_linux_${MEDIAMTX_ARCH}.tar.gz \
           file://mediamtx.yml \
           file://mediamtx.service \
"

# Verified against the checksums.sha256 file published with the v1.19.1 release.
# (bitbake's parser doesn't accept a flag combined with an override, e.g.
# SRC_URI[sha256sum]:aarch64, so the override lives on a plain variable and
# the flag assignment below just references it.)
MEDIAMTX_SHA256 = "unsupported-arch-${TARGET_ARCH}"
MEDIAMTX_SHA256:aarch64 = "97a277cf24153e168008c18da53fe84e8d364456e2d7b457dc0457666c32867b"
MEDIAMTX_SHA256:arm = "052654f2268ad0604f2bb277e417cf3c122d7399f814e6d1ca2dbcf180ed7fe9"

SRC_URI[sha256sum] = "${MEDIAMTX_SHA256}"

# The release tarball is flat: mediamtx, mediamtx.yml, LICENSE (no subdirectory).
# NOTE: don't be tempted to use ${UNPACKDIR} here even though it exists on
# scarthgap. On this release the do_unpack[cleandirs] default (an anonymous
# python snippet in base.bbclass) doesn't reliably have UNPACKDIR populated
# yet when it evaluates S, which throws "Directory name ${@d.getVar('S')
# contains unexpanded bitbake variable" at do_unpack time. WORKDIR is the
# correct/working choice on scarthgap; revisit only if you upgrade past it.
S = "${WORKDIR}"

inherit systemd

# Nothing to compile: this is a prebuilt release binary.
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/mediamtx ${D}${bindir}/mediamtx

    install -d ${D}${sysconfdir}
    install -m 0644 ${WORKDIR}/mediamtx.yml ${D}${sysconfdir}/mediamtx.yml

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/mediamtx.service ${D}${systemd_system_unitdir}/mediamtx.service
}

FILES:${PN} += "${systemd_system_unitdir}/mediamtx.service"
CONFFILES:${PN} += "${sysconfdir}/mediamtx.yml"

SYSTEMD_SERVICE:${PN} = "mediamtx.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# This is a prebuilt, statically-linked Go binary (verified: `file` reports
# "statically linked", no PT_DYNAMIC segment) built by upstream's own release
# workflow, not by our toolchain. Two QA checks otherwise fire on that:
#  - ldflags: looks for a GNU_HASH/dynamic section as evidence of OE's
#    hardening LDFLAGS; a static binary has no dynamic section at all.
#  - already-stripped: guards against foreign binaries with no debug info to
#    split; harmless to keep even though this particular release still ships
#    debug_info (do_package will strip it and produce a normal -dbg package).
INSANE_SKIP:${PN} += "already-stripped ldflags"