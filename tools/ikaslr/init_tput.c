// SPDX-License-Identifier: GPL-2.0
/*
 * 直接的吞吐 A/B：在固定墙钟时间内数完成了多少次系统调用循环。
 *
 * 用途：交叉验证 E3-A 的开销模型。模型说
 *     总开销 ≈ G × 常驻税 + M × K
 * 若 G（跨区域调用频度）与常驻税都测准了，那么同一负载在 Base 与 +R 两档下的
 * **吞吐比**应当与模型预测相符。这是把微观数字接回端到端的最短路径，不需要
 * 装任何 benchmark。
 *
 * 刻意选一个**纯系统调用**的循环（open/read/close + stat + access，全在 procfs
 * 上、不碰真实存储），因为它把跳板税放大到最明显——真实负载只会比它更轻。
 * 因此本程序给的是**上界**，不是典型值。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <linux/reboot.h>

#define SECONDS	5

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 一轮 = 4 次系统调用。返回完成的轮数。*/
static long long spin(double secs)
{
	char b[256];
	double t0 = now_s();
	long long n = 0;

	for (;;) {
		struct stat st;
		int fd = open("/proc/uptime", O_RDONLY);

		if (fd >= 0) {
			(void)!read(fd, b, sizeof(b));
			close(fd);
		}
		stat("/proc", &st);
		n++;
		/* 每 4096 轮查一次时间，免得 clock_gettime 本身成为负载 */
		if ((n & 0xfff) == 0 && now_s() - t0 >= secs)
			break;
	}
	return n;
}

int main(void)
{
	long long warm, n;
	double t0, t1;

	mount("proc", "/proc", "proc", 0, NULL);

	warm = spin(0.5);			/* 预热：让页缓存与分支预测就位 */
	t0 = now_s();
	n = spin(SECONDS);
	t1 = now_s();

	printf("TPUT loops=%lld elapsed_s=%.3f loops_per_s=%.0f "
	       "syscalls_per_s=%.0f warm=%lld\n",
	       n, t1 - t0, n / (t1 - t0), 4.0 * n / (t1 - t0), warm);

	/* 同一次启动里把 ikaslr 统计也带出来，便于把吞吐与 G 对齐。*/
	{
		int fd = open("/proc/ikaslr/stats", O_RDONLY);
		char buf[8192];
		ssize_t r;

		if (fd >= 0) {
			r = read(fd, buf, sizeof(buf) - 1);
			if (r > 0) {
				buf[r] = 0;
				printf("TPUT-STATS-BEGIN\n%sTPUT-STATS-END\n", buf);
			}
			close(fd);
		} else {
			printf("TPUT-STATS none (base kernel)\n");
		}
	}
	printf("TPUT-DONE\n");
	fflush(stdout);
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
