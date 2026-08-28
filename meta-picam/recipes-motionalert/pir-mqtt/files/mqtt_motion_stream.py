# ==============================================================================
# File:        mqtt_motion_stream.py
# Summary:     Listens for MQTT messages from pir_mqtt.cpp and starts/stops a GStreamer RTSP receiver accordingly.
# Author:      Eduard Polyakov <eduardpo@gmail.com>
# Date:        2026-08-27
# Version:     1.0.0
#
# Copyright (c) 2026 Eduard Polyakov. All rights reserved.
# Licensed under the MIT License.
# ==============================================================================


#!/usr/bin/env python3

import subprocess
import signal
import sys
import logging
from logging.handlers import RotatingFileHandler
import paho.mqtt.client as mqtt

# Settings
MQTT_BROKER = "localhost"   # mosquitto runs on this PC (pir_mqtt.cpp connects out to it)
MQTT_TOPIC = "pir/motion"

# MediaMTX runs on the Pi with source: rpiCamera in its path config -- it
# drives the camera itself and the RTSP feed is *always* live, independent
# of motion state. That means there's no publisher-not-ready race to guard
# against here anymore: as soon as motion_detected arrives, the stream is
# already there to pull. Set this to the Pi's real LAN IP.
RASPBERRY_PI_HOST = "192.168.1.3"
RTSP_PORT = 8554
RTSP_PATH = "cam"
RTSP_URL = f"rtsp://{RASPBERRY_PI_HOST}:{RTSP_PORT}/{RTSP_PATH}"

LOG_FILE = "/tmp/mqtt_motion_stream.log"

GST_COMMAND = [
    "gst-launch-1.0", "-v",
    "rtspsrc", f"location={RTSP_URL}", "latency=200", "protocols=tcp",
    "!", "rtph264depay",
    "!", "avdec_h264",
    "!", "videoconvert",
    "!", "queue",
    "!", "autovideosink", "sync=false"  # sync=false reduces lag for live PIR triggers
]

# --- Logging setup -----------------------------------------------------
# journalctl already timestamps stdout when this runs as a systemd --user
# service, but console-only print() gives no persistent record outside the
# journal and no level distinction (an MQTT disconnect and a routine "motion
# waiting" message looked identical before). This adds proper timestamps +
# levels to both the console (still visible in journalctl -f) and a rotating
# file, so real problems (gst-launch failing to start, MQTT dropouts) are
# easy to grep for independent of journal retention.
logger = logging.getLogger("mqtt_motion_stream")
logger.setLevel(logging.INFO)

_console_handler = logging.StreamHandler(sys.stdout)
_file_handler = RotatingFileHandler(LOG_FILE, maxBytes=1_000_000, backupCount=3)
_formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
for _h in (_console_handler, _file_handler):
    _h.setFormatter(_formatter)
    logger.addHandler(_h)
# ------------------------------------------------------------------------

gst_process = None

def start_gstreamer():
    global gst_process
    if gst_process is None:
        logger.info(f"Starting GStreamer RTSP receiver ({RTSP_URL})...")
        try:
            gst_process = subprocess.Popen(GST_COMMAND)
        except FileNotFoundError:
            logger.error("gst-launch-1.0 not found on PATH -- is GStreamer installed?")
    else:
        logger.debug("start_gstreamer() called but a receiver is already running, ignoring")

def stop_gstreamer():
    global gst_process
    if gst_process:
        logger.info("Stopping GStreamer receiver...")
        gst_process.terminate()
        try:
            gst_process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            logger.warning("gst-launch-1.0 didn't exit within 2s, killing it")
            gst_process.kill()
        gst_process = None
    else:
        logger.debug("stop_gstreamer() called but nothing is running, ignoring")

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info(f"Connected to MQTT broker at {MQTT_BROKER}")
    else:
        logger.error(f"MQTT connection failed with code {rc}")
    client.subscribe(MQTT_TOPIC)
    logger.info(f"Subscribed to topic '{MQTT_TOPIC}'")

def on_disconnect(client, userdata, rc):
    if rc != 0:
        logger.warning(f"Unexpected MQTT disconnect (code {rc}), paho will auto-reconnect")

def on_message(client, userdata, msg):
    payload = msg.payload.decode()
    logger.info(f"MQTT: {msg.topic} = {payload}")

    if payload == "motion_detected":
        start_gstreamer()
    elif payload == "motion_waiting":
        stop_gstreamer()
    else:
        logger.warning(f"Unrecognized payload on {msg.topic}: '{payload}'")

def cleanup(signum, frame):
    logger.info("Signal received, cleaning up...")
    stop_gstreamer()
    sys.exit(0)

if __name__ == "__main__":
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    logger.info(f"Connecting to MQTT broker {MQTT_BROKER}:1883...")
    client.connect(MQTT_BROKER, 1883, 60)
    logger.info("Waiting for MQTT messages...")
    client.loop_forever()