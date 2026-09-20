/* Syscall-surface exerciser for the fan-in census: touches kernel paths that a
 * shell pipeline alone does not (epoll, eventfd, signalfd, timerfd, memfd, splice,
 * sendfile, AF_PACKET, netlink, UDP/TCP loopback, userfaultfd, io_uring setup). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <pthread.h>

static void *thr(void *a) { usleep(1000); return a; }

int main(void)
{
	char buf[65536]; int i;
	memset(buf, 'x', sizeof buf);
	for (int round = 0; round < 3; round++) {
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int ev = eventfd(0, EFD_NONBLOCK);
	sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGUSR1);
	sigprocmask(SIG_BLOCK, &ss, NULL);
	int sf = signalfd(-1, &ss, 0);
	int tf = timerfd_create(CLOCK_MONOTONIC, 0);
	struct itimerspec its = { .it_value = { 0, 1000000 } };
	timerfd_settime(tf, 0, &its, NULL);
	struct epoll_event e = { .events = EPOLLIN };
	epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
	epoll_ctl(ep, EPOLL_CTL_ADD, sf, &e);
	epoll_ctl(ep, EPOLL_CTL_ADD, tf, &e);
	uint64_t one = 1; write(ev, &one, 8);
	kill(getpid(), SIGUSR1);
	struct epoll_event out[4];
	usleep(2000);
	epoll_wait(ep, out, 4, 10);
	read(ev, &one, 8); read(tf, &one, 8);
	struct signalfd_siginfo si; read(sf, &si, sizeof si);

	int mfd = memfd_create("f", MFD_ALLOW_SEALING);
	ftruncate(mfd, 1 << 20);
	char *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	memset(m, 1, 1 << 20); msync(m, 1 << 20, MS_SYNC); madvise(m, 1 << 20, MADV_DONTNEED);
	mprotect(m, 4096, PROT_READ); munmap(m, 1 << 20);
	fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK);

	int p[2]; pipe(p);
	int fd = open("/tmp/stress.dat", O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(fd, buf, sizeof buf); fsync(fd); fdatasync(fd);
	lseek(fd, 0, SEEK_SET);
	splice(fd, NULL, p[1], NULL, 4096, 0);
	read(p[0], buf, 4096);
	int fd2 = open("/tmp/stress.cp", O_CREAT | O_RDWR | O_TRUNC, 0644);
	off_t off = 0; sendfile(fd2, fd, &off, 8192);
	copy_file_range(fd, NULL, fd2, NULL, 4096, 0);
	fallocate(fd, 0, 0, 1 << 20);
	setxattr("/tmp/stress.dat", "user.k", "v", 1, 0);
	getxattr("/tmp/stress.dat", "user.k", buf, 10);
	int in = inotify_init1(0);
	inotify_add_watch(in, "/tmp", IN_ALL_EVENTS);
	rename("/tmp/stress.cp", "/tmp/stress.cp2"); unlink("/tmp/stress.cp2");
	close(in); close(fd); close(fd2); close(p[0]); close(p[1]);

	int us = socket(AF_INET, SOCK_DGRAM, 0), ur = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(34567) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(ur, (void *)&a, sizeof a);
	sendto(us, "hi", 2, 0, (void *)&a, sizeof a); recv(ur, buf, 10, MSG_DONTWAIT);
	int ls = socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons(34568), .sin6_addr = IN6ADDR_LOOPBACK_INIT };
	int on = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	bind(ls, (void *)&a6, sizeof a6); listen(ls, 4);
	int cs = socket(AF_INET6, SOCK_STREAM, 0); connect(cs, (void *)&a6, sizeof a6);
	int as = accept(ls, NULL, NULL);
	send(cs, buf, 60000, 0); recv(as, buf, 60000, MSG_WAITALL);
	shutdown(cs, SHUT_RDWR); close(cs); close(as); close(ls); close(us); close(ur);
	int sv[2]; socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv); send(sv[0], "x", 1, 0); recv(sv[1], buf, 1, 0);
	close(sv[0]); close(sv[1]);
	int pk = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)); recv(pk, buf, 100, MSG_DONTWAIT); close(pk);
	int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	struct { struct nlmsghdr h; struct rtgenmsg g; } req = { { NLMSG_LENGTH(sizeof(struct rtgenmsg)), RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 1, 0 }, { AF_PACKET } };
	send(nl, &req, req.h.nlmsg_len, 0); recv(nl, buf, sizeof buf, 0); close(nl);

	syscall(SYS_userfaultfd, 0);
	syscall(425 /* io_uring_setup */, 4, buf);
	pthread_t t; pthread_create(&t, NULL, thr, NULL); pthread_join(t, NULL);
	struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl); prctl(PR_SET_NAME, "stress");
	for (i = 0; i < 20; i++) { pid_t c = fork(); if (!c) { execl("/bin/true", "true", NULL); _exit(1); } waitpid(c, NULL, 0); }
	close(ep); close(ev); close(sf); close(tf); close(mfd);
	}
	return 0;
}
