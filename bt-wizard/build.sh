#!/bin/bash
# btwizard — build against the PDK with the era-appropriate Linaro toolchain.
# Modern GCC links against glibc symbols webOS 3.0.5 does not have.
set -e
export PATH=/home/jonwise/linaro-toolchain/bin:$PATH
PDK=/opt/PalmPDK
arm-linux-gnueabi-gcc -O2 -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp \
  -fsigned-char -D__webos__ -DLINUX -D_GNU_SOURCE=1 -D_REENTRANT \
  -I$PDK/include -I$PDK/include/SDL \
  -L$PDK/device/lib -Wl,-rpath-link,$PDK/device/lib \
  -o btwizard btwizard.c -lSDL -lpdl
echo "built: $(ls -la btwizard | awk '{print $5}') bytes"
