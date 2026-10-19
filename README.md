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

## Building with kas

The build runs kas 5.5 in an Ubuntu 22.04 container, keeping the Kirkstone build
environment independent of the host distribution. Install Docker and kas 5.5
on the host. On Ubuntu 24.04, the recommended `pipx` installation is:

```sh
sudo apt update
sudo apt install pipx -y
pipx ensurepath
# Open a new shell (or log out and back in) so the pipx path is available.
pipx install 'kas==5.5'
kas --version
```

`python3-newt` and `--system-site-packages` are only needed for kas's optional
interactive `menu` command; they are not needed to build this image.

From the repository root, build the container image once, then invoke the build:

```sh
docker build -f kas/Dockerfile -t yocto-kas-jammy:5.5 kas
mkdir -p /mnt/data_ntfs/yocto-cache/downloads /mnt/data_ntfs/yocto-cache/ccache
export KAS_CONTAINER_IMAGE=yocto-kas-jammy:5.5
export KAS_BUILD_DIR="$PWD/build_rpi"
export DL_DIR=/downloads
kas-container \
  --runtime-args "-v /mnt/data_ntfs/yocto-cache/downloads:/downloads -v /mnt/data_ntfs/yocto-cache/ccache:/ccache" \
  build kas/kirkstone.yml
```

The image includes `kas/container-entrypoint`, which forwards the command from
`kas-container` to kas inside Docker. If an older image produces
`exec shell failed: No such file or directory`, rebuild the image with the
`docker build` command above. You do not need to rebuild it for ordinary image
builds.

The last command is the actual image build: `kas-container` starts the configured
Docker image, kas reads [`kas/kirkstone.yml`](kas/kirkstone.yml), and BitBake
builds the configured `core-image-picam` target. The kas file already contains
the machine, layers, target, and BitBake configuration. The extra Docker runtime
arguments bind-mount the host cache directories; those host-specific paths
cannot be set in a portable kas project file.

Downloads and ccache are persisted at
`/mnt/data_ntfs/yocto-cache/{downloads,ccache}`, matching the previous Docker
setup. Docker reuses the built image on later builds; run `docker build` again
only after changing `kas/Dockerfile` or when you want to refresh its base image.
Keep the kas version in the host `pipx` environment and the container aligned.
For a different target, set `KAS_TARGET=your-target` before the `kas-container`
command.

To open kas's interactive build shell instead, use:

```sh
kas-container \
  --runtime-args "-v /mnt/data_ntfs/yocto-cache/downloads:/downloads -v /mnt/data_ntfs/yocto-cache/ccache:/ccache" \
  shell kas/kirkstone.yml
```

Exit the shell with `exit` or Ctrl+D. The container entrypoint forwards kas
commands and runs them with the invoking host user's UID and GID so files
created in the mounted work and cache directories remain accessible.
