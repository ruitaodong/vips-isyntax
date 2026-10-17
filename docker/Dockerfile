# libvips + the vips-isyntax plugin on Ubuntu focal (20.04, libvips 8.9), jammy (22.04, 8.12) or noble (24.04, 8.15).
# Build from the repository root:
#   docker build -f docker/Dockerfile --build-arg UBUNTU=noble -t vips-isyntax-noble .
#   docker run --rm -v "$PWD":/data vips-isyntax-noble vipsheader -a slide.isyntax
#   docker run --rm -v "$PWD":/data vips-isyntax-noble test_isyntax.sh slide.isyntax
ARG UBUNTU=noble

# ---- build stage: libisyntax + the plugin, compiled against this release's libvips ----
FROM ubuntu:${UBUNTU} AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake curl ca-certificates pkg-config libvips-dev \
 && rm -rf /var/lib/apt/lists/*
COPY build.sh vips-isyntax.c isyntax-crop.c test_isyntax.sh /src/
# build.sh installs where this libvips looks for plugins:
#   8.15: <libdir>/vips-modules-8.15/   8.12: <libdir>/vips-plugins-8.12/   8.9: <libdir>/vips-plugins-8.9/*.plg
RUN DESTDIR=/out bash /src/build.sh \
 && install -D -m 0755 /src/isyntax-crop /out/usr/local/bin/isyntax-crop \
 && install -D -m 0755 /src/test_isyntax.sh /out/usr/local/bin/test_isyntax.sh

# ---- runtime image ----
FROM ubuntu:${UBUNTU}
ENV DEBIAN_FRONTEND=noninteractive
# libvips-tools pulls in the right library package (libvips42 on focal/jammy, libvips42t64 on noble)
RUN apt-get update \
 && apt-get install -y --no-install-recommends libvips-tools python3 python3-venv ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/ /

# pyvips WITHOUT [binary]: it uses the system libvips, so it sees the plugin
# (pyvips[binary] brings its own libvips, which never loads system plugins)
RUN python3 -m venv /opt/venv \
 && /opt/venv/bin/pip install --no-cache-dir --upgrade pip \
 && /opt/venv/bin/pip install --no-cache-dir pyvips
ENV PATH=/opt/venv/bin:$PATH

# fail the build if the plugin isn't visible to the CLI or to Python; prints why if not
COPY docker/check-plugin.sh /usr/local/bin/check-plugin.sh
RUN chmod +x /usr/local/bin/check-plugin.sh && check-plugin.sh

WORKDIR /data
