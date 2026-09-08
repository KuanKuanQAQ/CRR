KERNEL="..//build/arch/x86/boot/bzImage"
DRIVE="./bullseye.img"
MNTPATH="./mnt_path/"

sudo qemu-system-x86_64 -s \
    -enable-kvm -cpu host -smp 1 \
    -m 16G \
    -kernel "$KERNEL" \
    -drive file="$DRIVE",if=virtio,format=raw \
    -fsdev local,path="$MNTPATH",security_model=mapped,id=dev-1 \
    -device virtio-9p,fsdev=dev-1,mount_tag=mount-1 \
    -nographic \
    -append "kaslr console=ttyS0 root=/dev/vda earlyprintk=serial ramdisk_size=2097152"
