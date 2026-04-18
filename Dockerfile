# syntax=docker/dockerfile:1.6
# Pebble dev toolchain image.
# Produces a single image containing:
#   - Pebble SDK 4.9.148 + pebble-tool 5.0.31 (pinned)
#   - QEMU emulator runtime (via pebble sdk install)
#   - Python 3.13 via uv
#   - Node 20
#   - Xvfb for headless emulator rendering

FROM debian:trixie-slim AS base

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PATH="/root/.local/bin:/root/.cargo/bin:${PATH}"

# System deps:
#   - libsdl1.2debian, libsdl2-2.0-0, libfdt1, libglib2.0-0t64: required by qemu-pebble
#   - xvfb, xauth, x11-utils: headless display + xdpyinfo probe
#   - patch: for sdk-apply-patches
#   - build-essential, pkg-config: required by pebble waf builds
#   - ca-certificates, curl, git: basics
#   - python3-pip, python3-venv: uv needs them for SDK install helpers
#   - fonts-dejavu: qemu SDL text
#   - nodejs, npm: for PKJS build (installed via NodeSource)
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        git \
        libsdl1.2debian \
        libsdl2-2.0-0 \
        libfdt1 \
        libglib2.0-0t64 \
        patch \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        xvfb \
        xauth \
        x11-utils \
        fonts-dejavu \
        procps \
    && curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

# uv (Python package manager) — installs pebble-tool with pinned Python.
RUN curl -LsSf https://astral.sh/uv/install.sh | sh

# ---- SDK layer ----
FROM base AS sdk

# Pinned versions. Bump here to upgrade.
ARG PEBBLE_TOOL_VERSION=5.0.31
ARG PEBBLE_SDK_VERSION=4.9.148

# Install pebble-tool into uv-managed Python 3.13.
RUN uv tool install --python 3.13 pebble-tool==${PEBBLE_TOOL_VERSION}

# Accept the SDK licence non-interactively + install SDK.
# `pebble sdk install` prompts for Y/N — pipe yes to it.
RUN yes | pebble sdk install ${PEBBLE_SDK_VERSION}

# ---- Final image ----
FROM sdk AS final

# Node deps for PKJS bundling. Copy only manifests so this layer caches
# independent of source changes.
WORKDIR /work
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund && rm -rf /root/.npm

# Python deps for scripts/ (integration tests, screenshots, icon gen).
RUN uv pip install --system --break-system-packages \
        websocket-client \
        Pillow \
        requests

# Always-on headless display.
ENV DISPLAY=:99

# Entrypoint starts Xvfb, optionally re-applies patches, execs command.
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD []
