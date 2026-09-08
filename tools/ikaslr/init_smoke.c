// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 冒烟测试的 init：挂载 /proc，读取统计与布局，触发若干次随机化，
 * 验证布局确实变化，然后关机。
 *
 * 作为 initramfs 的 /init 运行（PID 1），静态链接，无任何外部依赖。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

static void dump(const char *path)
{
	char buf[4096];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		printf("SMOKE: cannot open %s\n", path);
		return;
	}
	printf("---- %s ----\n", path);
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = 0;
		fputs(buf, stdout);
	}
	close(fd);
	fflush(stdout);
}

/* 把 layout 的内容读进 buf，用于前后对比。*/
static int snapshot(char *buf, size_t cap)
{
	int fd = open("/proc/ikaslr/layout", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return 0;
}

static int trigger(void)
{
	int fd = open("/proc/ikaslr/trigger", O_WRONLY);

	if (fd < 0)
		return -1;
	if (write(fd, "1", 1) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int main(void)
{
	char before[8192], after[8192];
	int changed = 0, i;

	mount("proc", "/proc", "proc", 0, NULL);
	printf("\nSMOKE: I-KASLR userspace smoke test\n");

	dump("/proc/ikaslr/stats");
	dump("/proc/ikaslr/layout");

	if (snapshot(before, sizeof(before))) {
		printf("SMOKE: FAIL cannot read layout\n");
		goto out;
	}

	/* 触发若干次随机化，每次之间让补充变体的工作队列有机会跑完。*/
	for (i = 0; i < 8; i++) {
		if (trigger()) {
			printf("SMOKE: FAIL cannot write trigger\n");
			goto out;
		}
		usleep(20000);
	}

	if (snapshot(after, sizeof(after))) {
		printf("SMOKE: FAIL cannot read layout after\n");
		goto out;
	}
	changed = strcmp(before, after) != 0;

	printf("---- after 8 triggers ----\n%s", after);
	dump("/proc/ikaslr/stats");
	dump("/proc/ikaslr/bench");
	printf("SMOKE: layout changed = %d\n", changed);
	printf("SMOKE: %s\n", changed ? "PASS" : "FAIL (layout did not change)");

out:
	fflush(stdout);
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
