// SPDX-License-Identifier: GPL-2.0
/*
 * E3-A（跳板机制的完整开销剖面）的数据采集 init。
 * 对应 设计文档/草稿/实验总清单.md §第1章 E3-A。
 *
 * 一次启动里采三块数据，全部以机器可读的行输出，由 scripts/ikaslr/e3a_collect.py
 * 汇总。**不在这里做任何统计计算**——原始值留在日志里，便于事后复核口径。
 *
 *   常驻税 A–E   ：反复读 /proc/ikaslr/bench（五档微基准），重复多次取分位
 *   随机化代价 H–M：逐次触发随机化，每次后采样三阶段耗时，得到分布而非单点
 *   跨区域频度 G ：固定墙钟时间内 enters/outs 的增量，分"空闲"与"负载"两种
 *
 * 为什么 G 要分空闲与负载两次：G 的意义是"真实负载下每秒发生多少次跨区域调用"，
 * 而采样本身（读 /proc）也会跨区域。空闲那一次就是本采集程序自己的底噪，
 * 汇总时要减掉。
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
#include <sys/wait.h>
#include <linux/reboot.h>

#define BENCH_REPEAT	7	/* 清单 §0.4：每项 >=5 次，报中位数 + 四分位 */
#define RAND_ROUNDS	120
#define G_SECONDS	5
#define NWORKER		4

static char buf[65536];

static int slurp(const char *path)
{
	int fd = open(path, O_RDONLY);
	ssize_t n, off = 0;

	if (fd < 0)
		return -1;
	while ((n = read(fd, buf + off, sizeof(buf) - 1 - off)) > 0)
		off += n;
	close(fd);
	buf[off > 0 ? off : 0] = 0;
	return off > 0 ? 0 : -1;
}

/* 从已读入 buf 的内容里取一个 "键 值" 形式的字段。取不到返回 -1。*/
static long long field(const char *key)
{
	char *p = strstr(buf, key);

	if (!p)
		return -1;
	return strtoll(p + strlen(key), NULL, 10);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 持续的 VFS 活动：G 的负载来源，也让随机化面对非空区域。*/
static void worker(void)
{
	char b[512];

	for (;;) {
		struct stat st;
		int fd = open("/proc/self/status", O_RDONLY);

		if (fd >= 0) {
			while (read(fd, b, sizeof(b)) > 0)
				;
			close(fd);
		}
		stat("/proc", &st);
		(void)!access("/init", R_OK);
	}
}

/* 固定墙钟时间内的 enters/outs 增量。*/
static void sample_g(const char *tag, int with_load)
{
	long long e0, o0, e1, o1;
	pid_t kid[NWORKER];
	double t0, t1;
	int i;

	if (slurp("/proc/ikaslr/stats"))
		return;
	e0 = field("enters ");
	o0 = field("outs ");

	if (with_load)
		for (i = 0; i < NWORKER; i++) {
			kid[i] = fork();
			if (kid[i] == 0) {
				worker();
				_exit(0);
			}
		}

	t0 = now_s();
	sleep(G_SECONDS);
	t1 = now_s();

	if (with_load) {
		for (i = 0; i < NWORKER; i++)
			kill(kid[i], 9);
		for (i = 0; i < NWORKER; i++)
			waitpid(kid[i], NULL, 0);
	}

	if (slurp("/proc/ikaslr/stats"))
		return;
	e1 = field("enters ");
	o1 = field("outs ");

	printf("E3A-G %s elapsed_s=%.3f d_enters=%lld d_outs=%lld workers=%d\n",
	       tag, t1 - t0, e1 - e0, o1 - o0, with_load ? NWORKER : 0);
	fflush(stdout);
}

int main(void)
{
	int i, tfd;

	mount("proc", "/proc", "proc", 0, NULL);
	printf("E3A-BEGIN\n");

	if (slurp("/proc/ikaslr/stats")) {
		printf("E3A-FAIL no /proc/ikaslr/stats\n");
		goto out;
	}
	printf("E3A-CFG functions=%lld rand_bytes=%lld tramp_bytes=%lld "
	       "whitelist=%lld stats_on=%d\n",
	       field("functions "), field("rand_region_bytes "),
	       field("tramp_region_bytes "), field("whitelist_entries "),
	       (int)(field("enters ") >= 0));

	/* ---- 常驻税 A–E ---- */
	for (i = 0; i < BENCH_REPEAT; i++) {
		char *line, *save;

		if (slurp("/proc/ikaslr/bench")) {
			printf("E3A-FAIL no /proc/ikaslr/bench "
			       "(CONFIG_IKASLR_DEBUG off?)\n");
			break;
		}
		for (line = strtok_r(buf, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			if (line[0] == '#' || line[0] == 0)
				continue;
			printf("E3A-BENCH rep=%d %s\n", i, line);
		}
		fflush(stdout);
	}

	/* ---- 随机化代价 H–M：逐次触发，每次后采样三阶段 ---- */
	tfd = open("/proc/ikaslr/trigger", O_WRONLY);
	if (tfd < 0) {
		printf("E3A-FAIL no trigger\n");
		goto out;
	}
	for (i = 0; i < RAND_ROUNDS; i++) {
		long long r0, r1;

		if (slurp("/proc/ikaslr/stats"))
			break;
		r0 = field("rounds ");
		(void)!write(tfd, "1", 1);
		if (slurp("/proc/ikaslr/stats"))
			break;
		r1 = field("rounds ");
		/*
		 * 只有本次触发确实换了一轮（rounds 增加）时，三阶段读数才对应
		 * 这一次随机化；否则是上一轮的残留，必须丢掉。
		 */
		printf("E3A-ROUND i=%d done=%d cp_ns=%lld wait_ns=%lld "
		       "update_ns=%lld remap_ns=%lld prep_ns=%lld residual=%lld\n",
		       i, r1 > r0, field("critical_path_ns "),
		       field("  cp_wait_ns "), field("  cp_update_ns "),
		       field("  cp_remap_ns "), field("prepare_ns "),
		       field("wait_residual "));
		fflush(stdout);
		usleep(5000);
	}
	close(tfd);

	/* ---- 跨区域调用频度 G ---- */
	sample_g("idle", 0);
	sample_g("load", 1);

	/* 收尾：把最终统计原样留在日志里，便于复核。*/
	if (!slurp("/proc/ikaslr/stats"))
		printf("E3A-STATS-BEGIN\n%sE3A-STATS-END\n", buf);
	printf("E3A-DONE\n");
out:
	fflush(stdout);
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
