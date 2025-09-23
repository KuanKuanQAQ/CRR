KERNEL="../build/arch/arm64/boot/Image"
DRIVE="./bullseye.img"
MNTPATH="./mnt_path/"

sudo qemu-system-aarch64 -s \
    -machine virt -enable-kvm -cpu host \
    -smp 1 -m 16G \
    -kernel "$KERNEL" \
    -drive file="$DRIVE",if=virtio,format=raw \
    -fsdev local,path="$MNTPATH",security_model=mapped,id=dev-1 \
    -device virtio-9p,fsdev=dev-1,mount_tag=mount-1 \
    -nographic \
    -append "kaslr console=ttyAMA0 root=/dev/vda"
