#!/bin/sh
# Phased workload; snapshot fanin_pairs after each phase into /mnt/snap/.
S=/mnt/snap; mkdir -p $S
snap() { cat /sys/kernel/debug/fanin_pairs > $S/$1.txt; date +%s.%N > $S/$1.time; echo "phase $1 done" > /dev/kmsg; }
snap P0-boot

# ---- P1: general workload
exec 3>/mnt/P1.log; 
( dhclient -v enp0s4; ip -br a; ping -c 3 10.0.2.2; curl -sS -m 5 http://10.0.2.2:1/ ; \
  ip link add dummy0 type dummy; ip addr add 10.9.9.1/24 dev dummy0; ip link set dummy0 up; \
  ip link add br0 type bridge; ip link set br0 up; ip link add veth0 type veth peer name veth1; ip link set veth0 up; ip link set veth1 up; \
  tc qdisc add dev dummy0 root fq_codel; tc -s qdisc; nft add table inet t; nft add chain inet t c '{ type filter hook input priority 0; }'; nft list ruleset; \
  ping -c 2 127.0.0.1; ping -6 -c 2 ::1 ) >&3 2>&3
( cd /tmp && gcc -O2 -pthread -o stress /mnt/stress.c && ./stress && ./stress ) >&3 2>&3
( mkdir -p /tmp/w && cd /tmp/w && dd if=/dev/urandom of=big bs=1M count=64 && sync && \
  tar czf big.tgz big && tar xzf big.tgz -C /tmp && cp -r /usr/include inc && du -sh inc && find inc -name '*.h' | xargs grep -l define | wc -l && \
  dd if=big of=/dev/null bs=4k iflag=direct && dd if=/dev/zero of=d bs=4k count=1000 oflag=direct,dsync && \
  ln -s big lnk && ln big hard && chmod 600 big && chown nobody big && truncate -s 1M big && mkfifo f && (echo x > f &) && cat f && \
  rm -rf inc big* d lnk hard f && echo 3 > /proc/sys/vm/drop_caches ) >&3 2>&3
( mount -t tmpfs none /media && dd if=/dev/zero of=/media/z bs=1M count=16 && umount /media; \
  dd if=/dev/zero of=/tmp/img bs=1M count=64 && mkfs.ext4 -q /tmp/img && mount -o loop /tmp/img /media && echo hi > /media/a && sync && umount /media; \
  cp /etc/services /mnt/services.copy && md5sum /mnt/services.copy && rm /mnt/services.copy; \
  unshare -mnpuif --mount-proc sh -c 'hostname ns; ip link; ps; mount -t tmpfs x /tmp'; \
  mkdir -p /sys/fs/cgroup/fan && echo $$ > /sys/fs/cgroup/fan/cgroup.procs && echo $$ > /sys/fs/cgroup/cgroup.procs && rmdir /sys/fs/cgroup/fan; \
  journalctl -n 50 --no-pager | tail -2; systemctl list-units --no-pager | wc -l; ps aux | wc -l; top -bn1 | head -5; \
  su - a -c 'id; ls ~'; sleep 1; dmesg | tail -2 ) >&3 2>&3
snap P1-workload

# ---- P2: read every sysfs / procfs file once (monitoring-agent style)
( find /sys /proc/sys /proc/self /proc/1 -maxdepth 12 -type f -perm -u=r 2>/dev/null \
    | grep -vE '^/sys/kernel/debug|trace_pipe|/proc/(self|1)/(task|mem|pagemap|kcore)|/sys/firmware/efi|/sys/power/' \
    | while read f; do timeout 2 dd if="$f" of=/dev/null bs=64k count=4 2>/dev/null; done
  for f in /proc/*; do [ -f "$f" ] && [ "$f" != /proc/kmsg ] && [ "$f" != /proc/kcore ] && timeout 2 dd if="$f" of=/dev/null bs=64k count=4 2>/dev/null; done ) >/mnt/P2.log 2>&1
snap P2-sysfs

# ---- P3: cold paths
( for c in 1 2 3; do echo 0 > /sys/devices/system/cpu/cpu$c/online; echo 1 > /sys/devices/system/cpu/cpu$c/online; done
  echo mem > /sys/power/pm_test 2>/dev/null; echo freezer > /sys/power/pm_test; echo freeze > /sys/power/state
  echo 1 > /proc/sys/kernel/sysrq; echo m > /proc/sysrq-trigger; echo w > /proc/sysrq-trigger
  swapoff -a; dd if=/dev/zero of=/tmp/sw bs=1M count=64 && chmod 600 /tmp/sw && mkswap /tmp/sw && swapon /tmp/sw && swapoff /tmp/sw
  echo 1 > /proc/sys/vm/compact_memory; sync; echo 3 > /proc/sys/vm/drop_caches ) >/mnt/P3.log 2>&1
snap P3-cold
cp /proc/kallsyms /mnt/snap/kallsyms 2>/dev/null
touch /mnt/snap/ALLDONE

# ---- P4: broad syscall surface via stress-ng (installed here, so apt itself is part of P4)
( export DEBIAN_FRONTEND=noninteractive; apt-get update -q && apt-get install -y -q stress-ng ) >/mnt/P4-apt.log 2>&1
snap P4a-apt
mkdir -p /tmp/sng && cd /tmp/sng
for cls in cpu cpu-cache memory vm io filesystem pipe scheduler interrupt os network kernel security device; do
  echo "== $cls $(date +%s)" >> /mnt/P4.log
  timeout 900 stress-ng --class $cls --sequential 1 --timeout 3s --temp-path /tmp/sng --metrics-brief >> /mnt/P4.log 2>&1
  snap P4-$cls
done
cd /
snap P4-stressng
touch /mnt/snap/ALLDONE2
