FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://25-wlan0.network"

do_install:append() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/25-wlan0.network ${D}${sysconfdir}/systemd/network/
}