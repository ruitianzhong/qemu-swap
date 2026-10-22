#!/bin/bash
# Configure (if needed), build and install QEMU from this source tree.
# This script lives in <repo>/vm_swap/ and operates on the parent source tree.
#
# Usage (from the repository root):
#   ./vm_swap/build.sh              configure if needed, build, install
#   ./vm_swap/build.sh --clean      remove build/ and build from scratch
#   ./vm_swap/build.sh --no-install build only, do not install
#   ./vm_swap/build.sh --help
#
# Environment:
#   PREFIX   install prefix          (default: /opt/qemu-exp)
#   TARGETS  configure --target-list (default: x86_64-softmmu,aarch64-softmmu)
#   JOBS     parallel make jobs      (default: nproc)
#
# Prerequisites: run ./vm_swap/install.sh once to install the apt packages.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOURCE_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$SOURCE_DIR/build"
cd "$SOURCE_DIR"

PREFIX="${PREFIX:-/opt/qemu-exp}"
TARGETS="${TARGETS:-x86_64-softmmu,aarch64-softmmu}"
JOBS="${JOBS:-$(nproc)}"
DO_INSTALL=1
DO_CLEAN=0

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Configure (if needed), build and install QEMU from this tree.
Run it from the repository root; the source root is the parent of this script.

Options:
  --clean        remove build/ and start from scratch
  --no-install   build only, do not run 'make install'
  -h, --help     show this help

Environment variables:
  PREFIX   install prefix          (default: /opt/qemu-exp)
  TARGETS  configure --target-list (default: x86_64-softmmu,aarch64-softmmu)
  JOBS     parallel make jobs      (default: nproc)

Examples:
  ./vm_swap/install.sh                 # install build dependencies
  ./vm_swap/build.sh                   # configure + build + install
  ./vm_swap/build.sh --clean           # fresh build
  ./vm_swap/build.sh --no-install      # build only
  PREFIX=/opt/qemu-exp TARGETS=x86_64-softmmu ./vm_swap/build.sh
  JOBS=16 ./vm_swap/build.sh
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            DO_CLEAN=1
            shift
            ;;
        --no-install)
            DO_INSTALL=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

for tool in gcc g++ make python3 meson ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: '$tool' not found. Install build dependencies with:" >&2
        echo "    ./vm_swap/install.sh" >&2
        exit 1
    fi
done

if [[ $DO_CLEAN -eq 1 ]]; then
    echo "==> Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [[ -f config-host.mak ]]; then
    echo "==> build/ is already configured, reusing it"
    echo "    (run '$0 --clean' to reconfigure with different PREFIX/TARGETS)"
else
    echo "==> Configuring: --prefix=$PREFIX --target-list=$TARGETS"
    "$SOURCE_DIR/configure" --prefix="$PREFIX" --target-list="$TARGETS"
fi

echo "==> Building with $JOBS jobs"
make -j"$JOBS"

if [[ $DO_INSTALL -eq 1 ]]; then
    echo "==> Installing to $PREFIX"
    if ! make install; then
        cat >&2 <<EOF

ERROR: 'make install' failed.
If $PREFIX is not writable, install with:
    sudo make -C $BUILD_DIR install
EOF
        exit 1
    fi
fi

if [[ $DO_INSTALL -eq 1 ]]; then
    cat <<EOF

==> Done. QEMU installed under: $PREFIX

Next steps:
  - QEMU used by vm_swap is $PREFIX/bin/qemu-system-x86_64
    (change QEMU_PATH at the top of vm_swap/vm_swap.py if PREFIX differs).
  - Verify the vm-swap / dirty-save option:
      $PREFIX/bin/qemu-system-x86_64 --help 2>&1 | grep dirty-save
  - Run the fork/dirty-save test:
      see vm_swap/README.md
EOF
else
    cat <<EOF

==> Build done. QEMU binaries are in: $BUILD_DIR

Next steps:
  - Install them with:
      sudo make -C $BUILD_DIR install
  - vm_swap expects $PREFIX/bin/qemu-system-x86_64
  - Run the fork/dirty-save test:
      see vm_swap/README.md
EOF
fi

