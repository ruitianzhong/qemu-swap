#!/bin/bash
# Install the packages needed to build QEMU and run the vm_swap harness.
#
# Usage (from the repository root):
#   ./vm_swap/install.sh
#
# Or from this directory:
#   ./install.sh

set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "ERROR: this script targets Debian/Ubuntu (apt-get not found)." >&2
    exit 1
fi

APT=(apt-get)
if [[ $EUID -ne 0 ]]; then
    APT=(sudo apt-get)
fi

echo "==> Updating package indexes"
"${APT[@]}" update

echo "==> Installing build and runtime dependencies"
"${APT[@]}" install --no-install-recommends -y \
    bash \
    bc \
    bison \
    bzip2 \
    ca-certificates \
    ccache \
    findutils \
    flex \
    g++ \
    gcc \
    git \
    iproute2 \
    libc6-dev \
    libfdt-dev \
    libffi-dev \
    libglib2.0-dev \
    libpixman-1-dev \
    locales \
    make \
    meson \
    ninja-build \
    openssh-client \
    pkgconf \
    python3 \
    python3-venv \
    sed \
    tar

# Optional, only needed for building the QEMU documentation:
# "${APT[@]}" install -y python3-sphinx python3-sphinx-rtd-theme
