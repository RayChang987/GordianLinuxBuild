# Build environment + runtime for Gordian placement (no root/apt needed on the host).
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake libblas-dev liblapack-dev \
        python3 python3-matplotlib python3-pil \
    && rm -rf /var/lib/apt/lists/*

COPY CMakeLists.txt /opt/gordian/
COPY src /opt/gordian/src
COPY scripts /opt/gordian/scripts
RUN cmake -S /opt/gordian -B /opt/gordian/build \
    && cmake --build /opt/gordian/build -j"$(nproc)" \
    && ln -s /opt/gordian/build/gordian_linux_build /usr/local/bin/gordian \
    && ln -s /opt/gordian/scripts/visualize.py /usr/local/bin/gordian-visualize

# matplotlib needs a writable config dir when run as a non-root user
ENV MPLCONFIGDIR=/tmp/matplotlib
WORKDIR /work
