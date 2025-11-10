KERNEL="../build/arch/arm64/boot/Image"
DRIVE="./bullseye.img"
MNTPATH="./mnt_path/"

sudo qemu-system-aarch64 -s -S \
    -machine virt -cpu host -enable-kvm \
    -smp 1 -m 16G \
    -kernel "$KERNEL" \
    -drive file="$DRIVE",if=virtio,format=raw \
    -fsdev local,path="$MNTPATH",security_model=mapped,id=dev-1 \
    -device virtio-9p,fsdev=dev-1,mount_tag=mount-1 \
    -net nic -net tap,ifname=tap0,script=no,downscript=no \
    -nographic \
    -append "nokaslr console=ttyAMA0 root=/dev/vda earlyprintk=serial ramdisk_size=2097152"

# -machine virt -cpu host -enable-kvm \
# -machine virt -cpu cortex-a76 \