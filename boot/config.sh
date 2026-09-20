# Shared configuration for the QEMU boot scripts.  Sourced by boot.sh and
# boot-S.sh; not meant to be run directly.
#
# Every value can be overridden from the environment, e.g.
#     ARCH=x86_64 ./boot.sh
#     SMP=1 KASLR=kaslr ./boot.sh

# Target architecture: arm64 (default, the current line of work) or x86_64.
ARCH="${ARCH:-arm64}"

# Where "make O=<dir>" put the kernel, relative to boot/.
BUILD="${BUILD:-../build}"

DRIVE="${DRIVE:-./bullseye.img}"
MNTPATH="${MNTPATH:-./mnt_path/}"
SMP="${SMP:-4}"
MEM="${MEM:-16G}"

# nokaslr keeps addresses stable across boots, which is what you want while
# debugging re-randomization; set KASLR=kaslr to exercise boot-time KASLR.
KASLR="${KASLR:-nokaslr}"

# Attach a tap0 network interface.  Needs tap0 to exist on the host:
#     sudo ip tuntap add dev tap0 mode tap user "$USER"
#     sudo ip link set tap0 up
NET="${NET:-1}"

case "$ARCH" in
arm64)
	QEMU="qemu-system-aarch64"
	KERNEL="$BUILD/arch/arm64/boot/Image"
	CONSOLE="ttyAMA0"
	# -cpu host -enable-kvm is much faster but only works on an aarch64 host;
	# cortex-a76 is the portable fallback.  Set KVM=1 on an aarch64 machine.
	if [ "${KVM:-0}" = "1" ]; then
		MACHINE=(-machine virt -cpu host -enable-kvm)
	else
		MACHINE=(-machine virt -cpu cortex-a76)
	fi
	;;
x86_64)
	QEMU="qemu-system-x86_64"
	KERNEL="$BUILD/arch/x86/boot/bzImage"
	CONSOLE="ttyS0"
	MACHINE=(-enable-kvm -cpu host)
	;;
*)
	echo "boot: unsupported ARCH '$ARCH' (expected arm64 or x86_64)" >&2
	exit 1
	;;
esac

if [ ! -f "$KERNEL" ]; then
	echo "boot: no kernel at $KERNEL -- build it first:" >&2
	echo "      make O=build ARCH=$ARCH -j\$(nproc)" >&2
	exit 1
fi

if [ ! -f "$DRIVE" ]; then
	echo "boot: no rootfs image at $DRIVE -- create it first: ./create-image.sh" >&2
	exit 1
fi

QEMU_ARGS=(
	"${MACHINE[@]}"
	-smp "$SMP" -m "$MEM"
	-kernel "$KERNEL"
	-drive file="$DRIVE",if=virtio,format=raw
	-fsdev local,path="$MNTPATH",security_model=mapped,id=dev-1
	-device virtio-9p,fsdev=dev-1,mount_tag=mount-1
	-nographic
	-append "$KASLR console=$CONSOLE root=/dev/vda earlyprintk=serial ramdisk_size=2097152"
)

if [ "$NET" = "1" ]; then
	QEMU_ARGS+=(-net nic -net tap,ifname=tap0,script=no,downscript=no)
fi
