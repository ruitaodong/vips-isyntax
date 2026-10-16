# Slide colour conversion: libvips 8.15 (Ubuntu 24.04 package), pyvips with bundled libvips 8.18,
# LittleCMS tools (tificc -c0 = exact device link) and libtiff tools for checks.
#   docker build -t slide-vips .
#   docker run --rm -v "$PWD":/data -w /data slide-vips convert_upsample.sh in.svs out.tif 0.2475
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libvips-tools \
      liblcms2-utils \
      libtiff-tools \
      python3 python3-venv \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# pyvips in a venv (Ubuntu 24.04 blocks system-wide pip); [binary] brings its own libvips 8.18,
# independent of the system libvips used by the vips command line.
RUN python3 -m venv /opt/venv \
 && /opt/venv/bin/pip install --no-cache-dir "pyvips[binary]" numpy
ENV PATH=/opt/venv/bin:$PATH

# Scripts and profiles (put them next to this Dockerfile before building)
COPY convert_upsample.sh convert_batch.sh convert_slide.py /opt/slide/
COPY profiles/ /opt/slide/profiles/
RUN chmod +x /opt/slide/*.sh /opt/slide/*.py \
 && ln -s /opt/slide/convert_upsample.sh /usr/local/bin/convert_upsample.sh \
 && ln -s /opt/slide/convert_batch.sh /usr/local/bin/convert_batch.sh \
 && ln -s /opt/slide/convert_slide.py /usr/local/bin/convert_slide.py

# No LD_LIBRARY_PATH: everything comes from the distribution or the pyvips wheel.
ENV SLIDE_PROFILES=/opt/slide/profiles
WORKDIR /data
