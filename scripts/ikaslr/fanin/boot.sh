#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Boot the fan-in instrumented kernel (E5-5) on a Debian bullseye image.
# The image is attached with snapshot=on, so it is never modified.
#
#   FANIN_RUN   run directory: share/ (9p, mounted at /mnt in the guest),
#               tty.sock (serial console, drive with tty.py), serial.log
#   BZIMAGE     instrumented bzImage
#   IMAGE       bullseye.img
#   QEMU        qemu-system-x86_64
W=${FANIN_RUN:-$(dirname "$(readlink -f "$0")")}
mkdir -p "$W/share"
exec "${QEMU:-qemu-system-x86_64}" \
  -enable-kvm -cpu host -smp 4 -m 4G \
  -kernel "${BZIMAGE:?set BZIMAGE}" \
  -drive file="${IMAGE:?set IMAGE}",if=virtio,format=raw,snapshot=on \
  -fsdev local,path="$W/share",security_model=none,id=dev-1 \
  -device virtio-9p-pci,fsdev=dev-1,mount_tag=mount-1 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -display none -no-reboot \
  -chardev socket,id=s0,path="$W/tty.sock",server=on,wait=off,logfile="$W/serial.log" \
  -serial chardev:s0 \
  -append "console=ttyS0 root=/dev/vda nokaslr earlyprintk=serial"
