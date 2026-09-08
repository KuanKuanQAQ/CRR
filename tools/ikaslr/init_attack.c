// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 攻击载体冒烟测试的 init。
 *
 * 扮演一个已经拿到任意读写原语的攻击者（经 /proc/ikaslr/attack 后门），
 * 演示：读随机化区域的代码字节、以及任意写。这是第 4 章检测实验的基础动作——
 * 当 XOM 检测（S2.1/S2.2）就位后，同样的读取会被捕获并触发随机化。
 *
 * 目标地址从 /proc/ikaslr/layout 读到（该接口仅调试配置提供），因此这里不需要
 * 猜地址；真实攻击者要先经信息泄露获得地址，那是被检测的对象，不在本冒烟范围。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

static unsigned long first_body_addr(void)
{
	char buf[4096], *p;
	unsigned long addr = 0;
	int fd = open("/proc/ikaslr/layout", O_RDONLY);
	ssize_t n;

	printf("ATTACK: uid=%d layout fd=%d\n", getuid(), fd);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	printf("ATTACK: layout[0:40]=%.40s\n", buf);
	/* 每行: "<name>   <hexaddr>   <size>"（%px，无 0x 前缀，addr 以 ff 开头）*/
	p = strstr(buf, " ff");
	if (p)
		addr = strtoul(p + 1, NULL, 16);
	return addr;
}

int main(void)
{
	unsigned long target;
	char cmd[128], out[64];
	int fd, i;

	mount("proc", "/proc", "proc", 0, NULL);
	printf("\nATTACK: eval vector smoke test\n");

	target = first_body_addr();
	if (!target) {
		printf("ATTACK: FAIL cannot read layout\n");
		goto out;
	}
	printf("ATTACK: target body at 0x%lx\n", target);

	/* 任意读：读该函数体的前 16 字节代码。*/
	fd = open("/proc/ikaslr/attack", O_RDWR);
	if (fd < 0) {
		printf("ATTACK: FAIL cannot open backdoor (module not built?)\n");
		goto out;
	}
	snprintf(cmd, sizeof(cmd), "r %lx 16", target);
	if (write(fd, cmd, strlen(cmd)) < 0) {
		printf("ATTACK: FAIL read command\n");
		close(fd); goto out;
	}
	lseek(fd, 0, SEEK_SET);
	int got = read(fd, out, 16);
	printf("ATTACK: read %d code bytes:", got);
	for (i = 0; i < got; i++)
		printf(" %02x", (unsigned char)out[i]);
	printf("\n");
	/* 头一字节应是 endbr64 的 0xf3（x86 IBT）——证明确实读到了代码。*/
	printf("ATTACK: %s (leaked real code)\n",
	       (got >= 1 && (unsigned char)out[0] == 0xf3) ? "PASS" : "CHECK");
	close(fd);

out:
	fflush(stdout);
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
