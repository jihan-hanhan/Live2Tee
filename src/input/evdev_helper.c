// evdev 特权帮手(仅 Linux):只链接 libc 的微型程序。
//
// 背景:AppImage 经 FUSE 挂载在 /tmp/.mount_*,FUSE 默认连 root 都无权访问,
// pkexec 无法以 root 重执行挂载点内的主程序;且主程序动态链接 Qt,单独复制
// 到 /tmp 也无法加载。因此把"打开 evdev 设备 + 经 unix socket 传 fd"的逻辑
// 做成独立的 libc-only 二进制:运行时复制到 /tmp 后 pkexec 之。
//
// 本文件同时被主程序(不定义 L2T_HELPER_MAIN,仅用 l2t_evdev_open_all)与
// 帮手程序(定义 L2T_HELPER_MAIN,含 main)编译。

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

static int test_bit(const unsigned long* bits, int bit)
{
	return (bits[bit / (8 * sizeof(unsigned long))]
			>> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

// 扫描并打开 /dev/input/event* 中的键盘与指针设备(只读、不 grab)。
// fds/abs_flags 为平行数组,返回打开的设备数;权限不足(EACCES)的设备跳过。
// abs_flags[i] = 1 表示该设备是纯绝对坐标指针(触摸板等,ABS_X/Y 需差分)。
int l2t_evdev_open_all(int fds[], int abs_flags[], int max_devs)
{
	DIR* dir = opendir("/dev/input");
	if (!dir)
		return 0;
	int n = 0;
	while (n < max_devs) {
		struct dirent* de = readdir(dir);
		if (!de)
			break;
		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		char path[64];
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		unsigned long evbits[(EV_CNT + 63) / 64] = {};
		unsigned long keybits[(KEY_CNT + 63) / 64] = {};
		unsigned long relbits[(REL_CNT + 63) / 64] = {};
		unsigned long absbits[(ABS_CNT + 63) / 64] = {};
		ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
		int has_rel = 0, is_keyboard = 0, is_pointer = 0, is_abs = 0;
		if (test_bit(evbits, EV_KEY)) {
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
			is_keyboard = test_bit(keybits, KEY_A) && test_bit(keybits, KEY_Z);
			if (test_bit(keybits, BTN_LEFT) || test_bit(keybits, BTN_RIGHT))
				is_pointer = 1;
		}
		if (test_bit(evbits, EV_REL)) {
			ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
			has_rel = test_bit(relbits, REL_X) && test_bit(relbits, REL_Y);
			if (has_rel)
				is_pointer = 1;
		}
		if (test_bit(evbits, EV_ABS)) {
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
			if (test_bit(absbits, ABS_X) && test_bit(absbits, ABS_Y)) {
				is_pointer = 1;
				is_abs = 1;
			}
		}
		if (!is_keyboard && !is_pointer) {
			close(fd);
			continue;
		}
		fds[n] = fd;
		abs_flags[n] = (is_abs && !has_rel) ? 1 : 0; // 同时有 REL 的按相对处理
		++n;
	}
	closedir(dir);
	return n;
}

// 帮手侧(以 root 运行):打开设备,把 fd 与 abs 标志经 SCM_RIGHTS 发给主进程。
// 协议:buf[0] = 设备数,buf[1+i] = abs 标志,辅助数据携带全部 fd。
// 返回进程退出码。
int l2t_evdev_send_fds(const char* sock_path)
{
	const int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sock < 0)
		return 1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		close(sock);
		return 1;
	}

	int fds[64];
	int abs_flags[64];
	const int ndev = l2t_evdev_open_all(fds, abs_flags, 64);
	uint8_t buf[1 + 64];
	buf[0] = (uint8_t)ndev;
	for (int i = 0; i < ndev; ++i)
		buf[1 + i] = (uint8_t)abs_flags[i];

	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = buf;
	iov.iov_len = (size_t)(1 + ndev);
	char cbuf[CMSG_SPACE(sizeof(int) * 64)];
	memset(cbuf, 0, sizeof(cbuf));
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (ndev > 0) {
		msg.msg_control = cbuf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * ndev);
		struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int) * ndev);
		memcpy(CMSG_DATA(c), fds, sizeof(int) * ndev);
	}
	const int ok = sendmsg(sock, &msg, 0) == (ssize_t)iov.iov_len;
	close(sock);
	for (int i = 0; i < ndev; ++i)
		close(fds[i]); // 关闭帮手侧副本;主进程持有收到的 fd
	return ok ? 0 : 1;
}

#ifdef L2T_HELPER_MAIN
// 帮手程序入口:live2tee-evdev-helper <unix socket 路径>
int main(int argc, char* argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <unix socket path>\n", argv[0]);
		return 2;
	}
	return l2t_evdev_send_fds(argv[1]);
}
#endif
