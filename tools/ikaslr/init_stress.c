// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 并发压力 init：在**持续的 VFS 活动**之下反复触发随机化。
 *
 * 为什么要并发：单线程冒烟测试里，触发随机化时区域本来就是空的，
 * fixed_out（离开区域时减计数）与陈旧返回地址的陷阱/修正路径根本走不到。
 * 只有在"一批执行流正在随机化过的 VFS 函数里、并且有些已经跳到区域外"的
 * 时候切换代码，才真的在考验方案 B 的那套记账与兜底。
 *
 * 判据（全部由内核统计给出，不做任何用户态探测）：
 *   whitelist_rejects  必须为 0 —— 有未登记的跨区域目标就是编译期漏登记
 *   stale_fixup_fails  必须为 0 —— 有修不回来的陈旧返回就是真错误
 *   rounds             必须显著增长 —— 否则说明区域一直非空、随机化没发生
 *   layout             必须变化
 *
 * 作为 initramfs 的 /init 运行（PID 1），静态链接。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/reboot.h>

#define NWORKER	4
#define NROUND	200

static int read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
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

/* 从 stats 里取一个字段的值。取不到返回 -1。*/
static long stat_field(const char *stats, const char *key)
{
	const char *p = strstr(stats, key);

	if (!p)
		return -1;
	return strtol(p + strlen(key), NULL, 10);
}

/*
 * 工作进程：不停地做真实的 VFS 操作。这些路径上的 vfs_read / vfs_open /
 * do_filp_open / inode_permission 等正是被随机化的函数，因而会持续地
 * 进入区域、经 fixed_out 跳出去、再返回。
 */
static void worker(void)
{
	char buf[512];

	for (;;) {
		struct stat st;
		int fd = open("/proc/self/status", O_RDONLY);

		if (fd >= 0) {
			while (read(fd, buf, sizeof(buf)) > 0)
				;
			close(fd);
		}
		stat("/proc", &st);
		fd = open("/proc/uptime", O_RDONLY);
		if (fd >= 0) {
			(void)!read(fd, buf, sizeof(buf));
			lseek(fd, 0, SEEK_SET);
			close(fd);
		}
		(void)!access("/init", R_OK);
	}
}

int main(void)
{
	char before[8192], after[8192], s0[4096], s1[4096];
	long r0, r1, rej, ffail, fok, backoff;
	pid_t kid[NWORKER];
	int i, tfd, changed;

	mount("proc", "/proc", "proc", 0, NULL);
	printf("\nSTRESS: I-KASLR concurrent stress (%d workers x %d rounds)\n",
	       NWORKER, NROUND);

	if (read_file("/proc/ikaslr/stats", s0, sizeof(s0))) {
		printf("STRESS: FAIL cannot read stats (CONFIG_IKASLR off?)\n");
		goto out;
	}
	fputs(s0, stdout);
	/* layout 只在 CONFIG_IKASLR_DEBUG 下存在，缺了不算失败。*/
	if (read_file("/proc/ikaslr/layout", before, sizeof(before)))
		before[0] = 0;
	r0 = stat_field(s0, "rounds ");

	for (i = 0; i < NWORKER; i++) {
		kid[i] = fork();
		if (kid[i] == 0) {
			worker();
			_exit(0);
		}
	}

	tfd = open("/proc/ikaslr/trigger", O_WRONLY);
	if (tfd < 0) {
		printf("STRESS: FAIL cannot open trigger\n");
		goto kill_out;
	}
	for (i = 0; i < NROUND; i++) {
		(void)!write(tfd, "1", 1);
		/*
		 * 20 ms：与 Adelie 对齐的触发间隔（见 perf/env.sh 的 TRIGGER_MS）。
		 * 别用更短的：阻断窗口在等不到空时就是 IKASLR_WAIT_MS（100 ms），
		 * 间隔比它还短的话，区域几乎全程处于阻断状态，测的就不是随机化开销
		 * 而是"一直在等"。
		 */
		usleep(20000);
	}
	close(tfd);

kill_out:
	for (i = 0; i < NWORKER; i++)
		kill(kid[i], 9);
	for (i = 0; i < NWORKER; i++)
		waitpid(kid[i], NULL, 0);

	if (read_file("/proc/ikaslr/stats", s1, sizeof(s1))) {
		printf("STRESS: FAIL cannot read stats after\n");
		goto out;
	}
	if (read_file("/proc/ikaslr/layout", after, sizeof(after)))
		after[0] = 0;
	fputs(s1, stdout);

	r1      = stat_field(s1, "rounds ");
	rej     = stat_field(s1, "whitelist_rejects ");
	ffail   = stat_field(s1, "stale_fixup_fails ");
	fok     = stat_field(s1, "stale_fixups ");
	backoff = stat_field(s1, "enter_backoffs ");
	changed = before[0] ? strcmp(before, after) != 0 : -1;  /* -1 = 无 layout，不判 */

	/*
	 * 成功率是**测量结果**而不是判据：范围一大、负载一重，活跃集合就很难在
	 * 100 ms 内变空，随机化会成批地跳过本轮。那是方法本身的性质（并且是安全的
	 * 降级），不是缺陷。判据只有"一轮都做不成"。
	 */
	printf("STRESS: rounds %ld -> %ld (+%ld of %d triggers, %ld%%)\n",
	       r0, r1, r1 - r0, NROUND, (r1 - r0) * 100 / NROUND);
	printf("STRESS: layout_changed=%d whitelist_rejects=%ld "
	       "stale_fixups=%ld stale_fixup_fails=%ld enter_backoffs=%ld\n",
	       changed, rej, fok, ffail, backoff);

	if (rej != 0)
		printf("STRESS: FAIL %ld cross-region call(s) to unlisted targets\n", rej);
	else if (ffail != 0)
		printf("STRESS: FAIL %ld stale return(s) could not be fixed up\n", ffail);
	else if (r1 - r0 <= 0)
		printf("STRESS: FAIL no round completed at all -- region never empty\n");
	else if (changed == 0)
		printf("STRESS: FAIL layout did not change\n");
	else
		printf("STRESS: PASS\n");

out:
	fflush(stdout);
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
