#!/bin/bash 

set -euo pipefail

mkdir build && cd build

../configure --prefix=/opt/qemu-exp --target-list=x86_64-softmmu,aarch64-softmmu  
# --enable-docs 
make -j$(nproc)