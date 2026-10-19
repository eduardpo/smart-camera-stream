FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV HOME=/tmp

RUN apt-get update \
    && apt-get install -y --allow-downgrades tar=1.34+dfsg-1build3 \
    && apt-mark hold tar \
    && apt-get install -y \
        gawk wget git diffstat unzip texinfo gcc build-essential \
        chrpath socat cpio python3 python3-pip python3-pexpect \
        xz-utils debianutils iputils-ping python3-git python3-jinja2 \
        libegl1-mesa libsdl1.2-dev xterm file locales \
        zstd lz4 rsync bc bison flex libssl-dev \
        ca-certificates sudo vim nano less curl ccache tmux util-linux \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8 \
    && python3 -m pip install --no-cache-dir "kas==5.5"

COPY container-entrypoint /usr/local/bin/container-entrypoint
RUN chmod 0755 /usr/local/bin/container-entrypoint

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/container-entrypoint"]
