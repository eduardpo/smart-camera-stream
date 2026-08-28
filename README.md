#### This repo is derived from my Capstone Project:
https://github.com/cu-ecen-aeld/final-project-eduardpo/wiki/Final-Project-Overview

Added support for RTP/RTSP/WebRTC streaming, OpenCV software/hardware motion detection to complement infrared detection, core pir_mqtt app is now part of the custom layer.

- mqtt_motion_stream.py: mqtt listener managed by service firing gst-launch playback on received alert
- mqtt-motion-stream.service: should be placed inside: *~/.config/systemd/user/*
- to check for sercvice status: *systemctl --user status mqtt_motion_stream.service*

<p float="left">
  <img src="https://github.com/user-attachments/assets/034dc9e2-69d1-4a78-b591-dfdce56ca95b" width="45%" />
  <img src="https://github.com/user-attachments/assets/950b264d-c84d-4929-bf63-01af2a9834b9" width="45%" />
</p>