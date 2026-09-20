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

/*
 * 采 enters/outs 的增量：**必须与吞吐测量用同一个负载、同一个时间窗**。
 * 第一版把 G 取自另一个采集程序（init_e3a 的 worker），两者每轮的系统调用组合
 * 不同，代入模型自然对不上——那不是"模型有二阶效应"，那是口径错了。
 */
static long long stat_field(const char *key)
{
	static char buf[8192];
	int fd = open("/proc/ikaslr/stats", O_RDONLY);
	ssize_t n;
	char *p;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, key);
	return p ? strtoll(p + strlen(key), NULL, 10) : -1;
}

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
	long long warm, n, e0, o0, e1, o1;
	double t0, t1;

	mount("proc", "/proc", "proc", 0, NULL);

	warm = spin(0.5);			/* 预热：让页缓存与分支预测就位 */

	e0 = stat_field("enters ");
	o0 = stat_field("outs ");
	t0 = now_s();
	n = spin(SECONDS);
	t1 = now_s();
	e1 = stat_field("enters ");
	o1 = stat_field("outs ");

	printf("TPUT loops=%lld elapsed_s=%.3f loops_per_s=%.0f "
	       "syscalls_per_s=%.0f warm=%lld\n",
	       n, t1 - t0, n / (t1 - t0), 4.0 * n / (t1 - t0), warm);
	if (e0 >= 0 && e1 >= 0)
		printf("TPUT-G d_enters=%lld d_outs=%lld per_loop_in=%.1f "
		       "per_loop_out=%.1f g_in_per_s=%.0f g_out_per_s=%.0f\n",
		       e1 - e0, o1 - o0, (double)(e1 - e0) / n,
		       (double)(o1 - o0) / n, (e1 - e0) / (t1 - t0),
		       (o1 - o0) / (t1 - t0));
	else
		printf("TPUT-G none (base kernel or CONFIG_IKASLR_STATS off)\n");

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
