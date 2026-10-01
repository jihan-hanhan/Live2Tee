// 全局输入线程实现。
// Windows:WH_MOUSE_LL / WH_KEYBOARD_LL + 消息循环;钩子回调需要本线程
// 拥有消息循环,且 SetWindowsHookEx 必须在该线程内调用。
// X11(LIVE2TEE_INPUT_X11,CMake 检测到 X11/Xi 时定义):
//   XInput2 raw 事件 + 独立 Display 连接,见本文件 X11 分支说明。
// 其他平台:no-op 空桩(编译通过、无输入),TODO: Wayland/macOS 后端。

#include "input.h"

#include <cstdio>

#if !LIVE2TEE_INPUT_WIN32 && defined(__linux__)
// 这些头必须在 namespace live2tee 之外包含:C 头文件在命名空间内包含是未定义行为,
// 会把 libc/libstdc++ 符号声明进 live2tee::std,污染 std 命名空间。
#if defined(LIVE2TEE_INPUT_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#endif
#if defined(LIVE2TEE_INPUT_EVDEV)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <signal.h>
#include <vector>
#endif
#include <poll.h>
#include <cmath>
#endif

namespace live2tee {

// ---------------------------------------------------------------------------
// 平台光标查询(启动锚定 / 手动校准用)
// ---------------------------------------------------------------------------
#if LIVE2TEE_INPUT_WIN32
bool QueryMouseOffsetFromScreenCenter(int& off_x, int& off_y)
{
	POINT pt;
	if (!GetCursorPos(&pt))
		return false;
	// 虚拟桌面(所有显示器)中心为基准,多屏下任意位置都有意义
	const int cx = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) / 2;
	const int cy = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) / 2;
	off_x = static_cast<int>(pt.x) - cx;
	off_y = static_cast<int>(pt.y) - cy;
	return true;
}

bool QueryGlobalLeftButtonDown()
{
	return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
#elif defined(__linux__)

// 优先读 evdev 设备的实时按键状态(EVIOCGKEY),evdev 未激活时
// 回退 XQueryPointer 的按钮掩码(纯 X11 全局有效;XWayland 下光标位于
// 原生 Wayland 窗口时状态不可见)。实现见文件后部 Linux 后端区。
bool QueryGlobalLeftButtonDown();

#if defined(LIVE2TEE_INPUT_X11)

// <X11/Xlib.h> 已在文件顶部 namespace 之外包含

bool QueryMouseOffsetFromScreenCenter(int& off_x, int& off_y)
{
	// 由主线程(GUI 校准按钮)偶发调用:开一个短连接查询后立即关闭,
	// 不与输入线程的 Display 连接共享(Xlib 的一个 Display 只能单线程用)。
	Display* dpy = XOpenDisplay(nullptr);
	if (!dpy)
		return false;

	const Window root = DefaultRootWindow(dpy);
	Window root_ret, child_ret;
	int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
	unsigned int mask = 0;
	const Bool ok = XQueryPointer(dpy, root, &root_ret, &child_ret,
								  &root_x, &root_y, &win_x, &win_y, &mask);
	// X 屏通常经 XRandR 跨所有显示器,尺寸即虚拟桌面大小,
	// 与 Windows 的 SM_CXVIRTUALSCREEN 中心语义对齐。
	const int w = XDisplayWidth(dpy, DefaultScreen(dpy));
	const int h = XDisplayHeight(dpy, DefaultScreen(dpy));
	XCloseDisplay(dpy);

	if (!ok)
		return false;
	off_x = root_x - w / 2;
	off_y = root_y - h / 2;
	return true;
}
#else
bool QueryMouseOffsetFromScreenCenter(int& off_x, int& off_y)
{
	(void)off_x; (void)off_y;
	return false; // Linux 无 XI2(evdev-only 构建)时取不到全局光标位置
}
#endif

#else // 非 Windows 非 Linux(macOS 等)

bool QueryMouseOffsetFromScreenCenter(int& off_x, int& off_y)
{
	(void)off_x; (void)off_y;
	return false; // TODO: macOS(CGEventGetLocation) 后端
}

bool QueryGlobalLeftButtonDown()
{
	return false; // TODO: macOS
}
#endif

// ---------------------------------------------------------------------------
// InputQueue(平台无关)
// ---------------------------------------------------------------------------
void InputQueue::Push(InputEvent ev)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_queue.push(ev);
}

bool InputQueue::Pop(InputEvent& out)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_queue.empty())
		return false;
	out = m_queue.front();
	m_queue.pop();
	return true;
}

void InputQueue::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::queue<InputEvent> empty;
	m_queue.swap(empty);
}

// ---------------------------------------------------------------------------
// InputThread
// ---------------------------------------------------------------------------
static InputThread* g_active_thread = nullptr;

#if LIVE2TEE_INPUT_WIN32

InputThread::InputThread(InputQueue& queue) : m_queue(queue) {}

InputThread::~InputThread()
{
	Stop();
}

void InputThread::Start()
{
	if (m_running)
		return;
	m_running = true;
	m_thread = std::thread([this] { Run(); });
}

void InputThread::Stop()
{
	if (!m_running)
		return;
	m_running = false;

	if (m_mouse_hook) {
		UnhookWindowsHookEx(m_mouse_hook);
		m_mouse_hook = nullptr;
	}
	if (m_keyboard_hook) {
		UnhookWindowsHookEx(m_keyboard_hook);
		m_keyboard_hook = nullptr;
	}

	if (m_thread_id) {
		// 唤醒阻塞在 GetMessage 的线程
		PostThreadMessageW(m_thread_id, WM_QUIT, 0, 0);
	}
	if (m_thread.joinable())
		m_thread.join();

	g_active_thread = nullptr;
}

void InputThread::SetGuiHotkey(int vk)
{
	m_gui_hotkey_vk.store(vk, std::memory_order_relaxed);
}

bool InputThread::ConsumeGuiHotkey()
{
	// exchange:置 false 并返回旧值,保证多线程下只消费一次
	return m_gui_hotkey_flag.exchange(false, std::memory_order_relaxed);
}

void InputThread::Run()
{
	m_thread_id = GetCurrentThreadId();
	g_active_thread = this;

	m_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, &InputThread::LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
	m_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &InputThread::LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);

	if (!m_mouse_hook || !m_keyboard_hook) {
		std::fprintf(stderr, "SetWindowsHookEx failed: %lu\n", GetLastError());
	}

	// 鼠标运动增量走 Raw Input(message-only 窗口 + RIDEV_INPUTSINK,
	// 后台也接收)。偏移控制只需要运动增量,不需要屏幕绝对坐标。
	m_raw_wnd = CreateRawInputWindow();

	MSG msg;
	while (m_running) {
		// PeekMessage 非阻塞,这样可以轮询 m_running 退出
		if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		} else {
			Sleep(5); // 空闲让 CPU
		}
	}

	if (m_raw_wnd) {
		// 注销 Raw Input 注册并销毁消息窗口
		RAWINPUTDEVICE rid = {};
		rid.usUsagePage = 0x01;
		rid.usUsage = 0x02;
		rid.dwFlags = RIDEV_REMOVE;
		rid.hwndTarget = nullptr;
		RegisterRawInputDevices(&rid, 1, sizeof(rid));
		DestroyWindow(m_raw_wnd);
		m_raw_wnd = nullptr;
	}

	if (m_mouse_hook) {
		UnhookWindowsHookEx(m_mouse_hook);
		m_mouse_hook = nullptr;
	}
	if (m_keyboard_hook) {
		UnhookWindowsHookEx(m_keyboard_hook);
		m_keyboard_hook = nullptr;
	}
}

HWND InputThread::CreateRawInputWindow()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &InputThread::RawInputWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"Live2TeeRawInput";
	RegisterClassExW(&wc);

	HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0,
							   0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	if (!wnd) {
		std::fprintf(stderr, "CreateWindowEx(raw input) failed: %lu\n", GetLastError());
		return nullptr;
	}

	RAWINPUTDEVICE rid = {};
	rid.usUsagePage = 0x01;        // generic desktop
	rid.usUsage = 0x02;            // mouse
	rid.dwFlags = RIDEV_INPUTSINK; // 程序不在前台也接收
	rid.hwndTarget = wnd;
	if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
		std::fprintf(stderr, "RegisterRawInputDevices failed: %lu\n", GetLastError());

	return wnd;
}

LRESULT CALLBACK InputThread::RawInputWndProc(HWND wnd, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_INPUT) {
		InputThread* self = g_active_thread;
		if (!self || !self->m_running)
			return 0;

		RAWINPUT raw;
		UINT size = sizeof(raw);
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT,
							&raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
			return 0;
		if (raw.header.dwType != RIM_TYPEMOUSE)
			return 0;

		const RAWMOUSE& m = raw.data.mouse;
		if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
			// 绝对设备(数位板/远程桌面等):虚拟桌面坐标差分换算成位移。
			static bool have_last = false;
			static int last_x = 0, last_y = 0;
			const int nx = MulDiv(m.lLastX, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1, 65535)
						   + GetSystemMetrics(SM_XVIRTUALSCREEN);
			const int ny = MulDiv(m.lLastY, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1, 65535)
						   + GetSystemMetrics(SM_YVIRTUALSCREEN);
			int dx = 0, dy = 0;
			if (have_last) {
				dx = nx - last_x;
				dy = ny - last_y;
			}
			last_x = nx;
			last_y = ny;
			have_last = true;
			if (dx != 0 || dy != 0) {
				InputEvent ev;
				ev.kind = EInputKind::MouseMove;
				ev.dx = dx;
				ev.dy = dy;
				self->m_queue.Push(ev);
			}
		} else {
			// 相对设备:lLastX/lLastY 就是硬件运动增量,直接入队。
			if (m.lLastX != 0 || m.lLastY != 0) {
				InputEvent ev;
				ev.kind = EInputKind::MouseMove;
				ev.dx = static_cast<int>(m.lLastX);
				ev.dy = static_cast<int>(m.lLastY);
				self->m_queue.Push(ev);
			}
		}
		return 0;
	}
	return DefWindowProcW(wnd, msg, w, l);
}

LRESULT CALLBACK InputThread::LowLevelMouseProc(int code, WPARAM w, LPARAM l)
{
	if (code != HC_ACTION)
		return CallNextHookEx(nullptr, code, w, l);

	InputThread* self = g_active_thread;
	if (!self || !self->m_running)
		return CallNextHookEx(nullptr, code, w, l);

	switch (w) {
	// 鼠标移动不在钩子里处理:WH_MOUSE_LL 只有屏幕绝对坐标 m->pt,
	// 依赖系统坐标系且在屏幕边缘/全屏锁鼠标时会丢位移;
	// 运动增量由 Raw Input 窗口(RawInputWndProc)提供。
	case WM_LBUTTONDOWN: {
		InputEvent ev;
		ev.kind = EInputKind::MouseLeft;
		ev.pressed = true;
		self->m_queue.Push(ev);
		break;
	}
	case WM_LBUTTONUP: {
		InputEvent ev;
		ev.kind = EInputKind::MouseLeft;
		ev.pressed = false;
		self->m_queue.Push(ev);
		break;
	}
	case WM_RBUTTONDOWN: {
		InputEvent ev;
		ev.kind = EInputKind::MouseRight;
		ev.pressed = true;
		self->m_queue.Push(ev);
		break;
	}
	case WM_RBUTTONUP: {
		InputEvent ev;
		ev.kind = EInputKind::MouseRight;
		ev.pressed = false;
		self->m_queue.Push(ev);
		break;
	}
	}
	return CallNextHookEx(nullptr, code, w, l);
}

LRESULT CALLBACK InputThread::LowLevelKeyboardProc(int code, WPARAM w, LPARAM l)
{
	if (code != HC_ACTION)
		return CallNextHookEx(nullptr, code, w, l);

	InputThread* self = g_active_thread;
	if (!self || !self->m_running)
		return CallNextHookEx(nullptr, code, w, l);

	const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);

	// 按键按下状态表:过滤系统自动重复(长按)。
	// 与 DDnet 输入层一致,只有"松开→按下"的沿才算新按键,
	// 否则长按一次键盘会连发几十个 KeyDown,表情狂闪。
	static bool key_down[256] = {};
	const int key_idx = static_cast<int>(k->vkCode) & 0xFF;

	switch (w) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN: {
		if (key_down[key_idx])
			break; // 自动重复,忽略
		key_down[key_idx] = true;
		// GUI 唤起热键在钩子层截获,不进表情管线
		if (self->m_gui_hotkey_vk.load(std::memory_order_relaxed) == static_cast<int>(k->vkCode)) {
			self->m_gui_hotkey_flag.store(true, std::memory_order_relaxed);
			break;
		}
		InputEvent ev;
		ev.kind = EInputKind::Key;
		ev.pressed = true;
		ev.keycode = static_cast<int>(k->vkCode);
		self->m_queue.Push(ev);
		break;
	}
	case WM_KEYUP:
	case WM_SYSKEYUP: {
		key_down[key_idx] = false;
		InputEvent ev;
		ev.kind = EInputKind::Key;
		ev.pressed = false;
		ev.keycode = static_cast<int>(k->vkCode);
		self->m_queue.Push(ev);
		break;
	}
	}
	return CallNextHookEx(nullptr, code, w, l);
}

#elif defined(__linux__)

// Linux 全局输入后端(两级,运行时自动选择):
//   1. evdev(优先,LIVE2TEE_INPUT_EVDEV):直接读 /dev/input/event* 设备节点,
//      在内核输入层全局捕获,X11 / Wayland 会话都有效;只读不 grab,不消费
//      事件,用户打字、操作其他程序完全不受影响。需要设备读权限(通常由
//      input 用户组授予)。
//   2. XInput2 raw 事件(回退,LIVE2TEE_INPUT_X11):选听 root window 的
//      XI_RawMotion/Button/Key。纯 X11 会话下全局有效且免配置;但 Wayland
//      会话的 XWayland 只在光标位于 X11 窗口内时才收到输入(合成器不路由
//      其余输入),此时 Tee 只对预览窗口内的输入有反应。
// 两级的键码统一翻译成与 Windows 一致的 VK 码,config/GUI/状态机零改动。
// 注:X11/evdev/poll/cmath 头文件在文件顶部 namespace 之外包含。

#if defined(LIVE2TEE_INPUT_EVDEV)
namespace {

struct EvdevDevice {
	int fd = -1;
	bool abs_pointer = false; // 触摸板等绝对坐标设备:ABS_X/Y 需差分
	int last_x = 0, last_y = 0;
	bool have_last = false;
};

bool TestBit(const unsigned long* bits, int bit)
{
	return (bits[bit / (8 * sizeof(unsigned long))]
			>> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

// evdev 打开/分类与帮手发送逻辑由 libc-only 的 evdev_helper.c 提供
// (主程序与独立帮手二进制共用,帮手可被 pkexec 以 root 执行)。
extern "C" {
int l2t_evdev_open_all(int fds[], int abs_flags[], int max_devs);
}

// 扫描并打开 /dev/input/event* 中的键盘与指针设备(只读、不 grab)。
// 返回打开的设备数;权限不足(EACCES)的设备跳过。
int OpenEvdevDevices(EvdevDevice* devs, int max_devs)
{
	int fds[64];
	int abs_flags[64];
	const int n = l2t_evdev_open_all(fds, abs_flags,
									 max_devs < 64 ? max_devs : 64);
	for (int i = 0; i < n; ++i) {
		devs[i].fd = fds[i];
		devs[i].abs_pointer = abs_flags[i] != 0;
	}
	return n;
}

// evdev 键码 -> Windows VK 码(F1-F12 -> 0x70..0x7B,与 config 热键值对齐)。
// 映射不到的键返回 0x88 占位:仍能触发表情气泡,但不会误匹配任何热键。
int EvdevKeyToVk(int code)
{
	if (code >= KEY_1 && code <= KEY_9) return '1' + (code - KEY_1);
	if (code == KEY_0) return '0';
	if (code >= KEY_Q && code <= KEY_P) return 'Q' + (code - KEY_Q);
	if (code >= KEY_A && code <= KEY_L) return 'A' + (code - KEY_A);
	if (code >= KEY_Z && code <= KEY_M) return 'Z' + (code - KEY_Z);
	if (code >= KEY_F1 && code <= KEY_F12) return 0x70 + (code - KEY_F1);
	switch (code) {
	case KEY_SPACE:     return 0x20;
	case KEY_ENTER:
	case KEY_KPENTER:   return 0x0D;
	case KEY_ESC:       return 0x1B;
	case KEY_BACKSPACE: return 0x08;
	case KEY_TAB:       return 0x09;
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT: return 0x10;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL: return 0x11;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:  return 0x12;
	case KEY_LEFT:      return 0x25;
	case KEY_UP:        return 0x26;
	case KEY_RIGHT:     return 0x27;
	case KEY_DOWN:      return 0x28;
	}
	return 0x88;
}

// ---- pkexec 特权帮手 -------------------------------------------------------
// 主进程无 evdev 读权限时,fork+execlp pkexec 以 root 执行同目录的
// live2tee-evdev-helper(libc-only 微型程序),帮手打开设备后经 unix socket
// 用 SCM_RIGHTS 把 fd 传回本进程。帮手立即退出,主进程全程非 root。
// AppImage 的 FUSE 挂载 root 无权访问,故帮手总是被复制到 /tmp 后再执行。

// 复制帮手二进制到临时目录(目录已由调用方建好,0700)。
bool CopyHelperBinary(const char* src, const char* dst)
{
	const int in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return false;
	const int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
	if (out < 0) {
		close(in);
		return false;
	}
	char buf[16384];
	bool ok = true;
	for (;;) {
		const ssize_t r = read(in, buf, sizeof(buf));
		if (r < 0) { ok = false; break; }
		if (r == 0) break;
		ssize_t off = 0;
		while (off < r) {
			const ssize_t w = write(out, buf + off, static_cast<size_t>(r - off));
			if (w <= 0) { ok = false; break; }
			off += w;
		}
		if (!ok) break;
	}
	close(in);
	close(out);
	return ok;
}

// 主进程侧:弹 pkexec 授权,收回帮手传来的已打开 fd。返回设备数(0=失败/取消)。
int OpenEvdevDevicesViaHelper(EvdevDevice* devs, int max_devs,
								  std::atomic<bool>& running)
{
	// 帮手程序与主程序同目录(开发构建:build/;AppImage:usr/bin/)
	char exe_path[PATH_MAX];
	const ssize_t plen = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
	if (plen <= 0)
		return 0;
	exe_path[plen] = '\0';
	char* slash = std::strrchr(exe_path, '/');
	if (!slash)
		return 0;
	char helper_src[PATH_MAX];
	std::snprintf(helper_src, sizeof(helper_src), "%.*s/live2tee-evdev-helper",
				  static_cast<int>(slash - exe_path), exe_path);

	// socket 放在 0700 的临时目录里:只有本用户和 root(帮手)能连
	char dir[] = "/tmp/live2tee-XXXXXX";
	if (!mkdtemp(dir))
		return 0;
	char sock_path[108];
	std::snprintf(sock_path, sizeof(sock_path), "%s/sock", dir);

	const int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		rmdir(dir);
		return 0;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
		listen(listen_fd, 1) != 0) {
		close(listen_fd);
		unlink(sock_path);
		rmdir(dir);
		return 0;
	}

	// 把帮手复制到 /tmp 临时目录(AppImage 的 FUSE 挂载 root 无权访问,
	// 必须复制到真实文件系统才能被 pkexec 以 root 执行)
	char helper_bin[PATH_MAX];
	std::snprintf(helper_bin, sizeof(helper_bin), "%s/helper", dir);
	if (!CopyHelperBinary(helper_src, helper_bin)) {
		std::fprintf(stderr,
			"InputThread: 无法复制 live2tee-evdev-helper 到 /tmp,跳过特权帮手\n");
		close(listen_fd);
		unlink(sock_path);
		rmdir(dir);
		return 0;
	}

	std::fprintf(stderr,
		"InputThread: 无 evdev 权限,请求管理员授权以打开输入设备...\n");
	const pid_t pid = fork();
	if (pid == 0) {
		execlp("pkexec", "pkexec", helper_bin, sock_path,
			   static_cast<char*>(nullptr));
		_exit(127); // pkexec 不存在等
	}

	int ndev = 0;
	if (pid > 0) {
		// 等"帮手连接"或"pkexec 退出(授权被拒)"或"程序退出",先到为准。
		// 用户输密码可能耗时较长,轮询等待。
		int conn = -1;
		int status = 0;
		while (running.load(std::memory_order_relaxed)) {
			struct pollfd pfd{listen_fd, POLLIN, 0};
			if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
				conn = accept(listen_fd, nullptr, nullptr);
				if (conn >= 0)
					fcntl(conn, F_SETFD, FD_CLOEXEC);
				break;
			}
			if (waitpid(pid, &status, WNOHANG) == pid)
				break; // pkexec 已退出(授权被拒/失败),不会有连接了
		}
		if (conn >= 0) {
			uint8_t buf[1 + 64];
			iovec iov{};
			iov.iov_base = buf;
			iov.iov_len = sizeof(buf);
			char cbuf[CMSG_SPACE(sizeof(int) * 64)];
			std::memset(cbuf, 0, sizeof(cbuf));
			msghdr msg{};
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = cbuf;
			msg.msg_controllen = sizeof(cbuf);
			const ssize_t r = recvmsg(conn, &msg, MSG_CMSG_CLOEXEC);
			if (r >= 1 && buf[0] > 0 && !(msg.msg_flags & MSG_CTRUNC)) {
				cmsghdr* c = CMSG_FIRSTHDR(&msg);
				if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
					const int nrecv = static_cast<int>(
						(c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
					const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
					const int want = buf[0] < nrecv ? buf[0] : nrecv;
					for (int i = 0; i < want && ndev < max_devs; ++i) {
						devs[ndev].fd = fds[i];
						devs[ndev].abs_pointer = (buf[1 + i] & 1) != 0;
						++ndev;
					}
					for (int i = want; i < nrecv; ++i)
						close(fds[i]); // 超出上限的多余 fd
				}
			}
			close(conn);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			if (ndev > 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
				// 帮手异常退出:不信任收到的 fd
				for (int i = 0; i < ndev; ++i)
					close(devs[i].fd);
				ndev = 0;
			}
		} else {
			// 程序退出但 pkexec 可能还在等密码:尝试终止(可能因 euid 是
			// root 而 EPERM,此时授权框会留着,用户取消/输入后帮手因连不上
			// socket 自行退出,无害)
			if (waitpid(pid, &status, WNOHANG) == 0) {
				kill(pid, SIGTERM);
				while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
					;
			}
		}
	}
	close(listen_fd);
	unlink(sock_path);
	unlink(helper_bin);
	rmdir(dir);
	return ndev;
}

void EvdevLoop(EvdevDevice* devs, int ndev, InputQueue& queue,
			   std::atomic<int>& hotkey_vk, std::atomic<bool>& hotkey_flag,
			   std::atomic<bool>& running)
{
	std::fprintf(stderr, "InputThread: evdev backend active (%d device(s))\n", ndev);
	struct pollfd pfds[64];
	double acc_x = 0.0, acc_y = 0.0;

	while (running.load(std::memory_order_relaxed)) {
		for (int i = 0; i < ndev; ++i) {
			pfds[i].fd = devs[i].fd;
			pfds[i].events = POLLIN;
			pfds[i].revents = 0;
		}
		if (poll(pfds, static_cast<nfds_t>(ndev), 150) <= 0)
			continue; // 超时:回 while 检查 running

		for (int i = 0; i < ndev; ++i) {
			EvdevDevice& dev = devs[i];
			if (!(pfds[i].revents & POLLIN))
				continue;
			struct input_event evs[64];
			for (;;) {
				const ssize_t got = read(dev.fd, evs, sizeof(evs));
				if (got < static_cast<ssize_t>(sizeof(input_event)))
					break; // EAGAIN(非阻塞)或设备断开
				const int cnt = static_cast<int>(got / sizeof(input_event));
				for (int k = 0; k < cnt; ++k) {
					const struct input_event& ev = evs[k];
					if (ev.type == EV_REL) {
						if (ev.code == REL_X)
							acc_x += ev.value;
						else if (ev.code == REL_Y)
							acc_y += ev.value;
					} else if (ev.type == EV_ABS && dev.abs_pointer) {
						if (ev.code == ABS_X || ev.code == ABS_Y) {
							int& last = (ev.code == ABS_X) ? dev.last_x : dev.last_y;
							// 触摸板 ABS 单位与鼠标 REL 计数不同源,直接差分,
							// 灵敏度可能与鼠标不同(直播场景通常用鼠标)。
							if (dev.have_last) {
								if (ev.code == ABS_X)
									acc_x += ev.value - last;
								else
									acc_y += ev.value - last;
							}
							last = ev.value;
						}
					} else if (ev.type == EV_KEY) {
						if (ev.value == 2)
							continue; // 内核自动重复,忽略(只放行按下/抬起沿)
						if (dev.abs_pointer && ev.code == BTN_TOUCH && ev.value == 0) {
							dev.have_last = false; // 抬指后重新锚定,防跳变
							continue;
						}
						if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) {
							InputEvent bev;
							bev.kind = (ev.code == BTN_LEFT) ? EInputKind::MouseLeft
															 : EInputKind::MouseRight;
							bev.pressed = (ev.value == 1);
							queue.Push(bev);
							continue;
						}
						const int vk = EvdevKeyToVk(ev.code);
						if (ev.value == 1) {
							if (hotkey_vk.load(std::memory_order_relaxed) == vk) {
								// GUI 唤起热键在输入层截获,不进表情管线
								hotkey_flag.store(true, std::memory_order_relaxed);
								continue;
							}
						}
						InputEvent kev;
						kev.kind = EInputKind::Key;
						kev.pressed = (ev.value == 1);
						kev.keycode = vk;
						queue.Push(kev);
					}
				}
			}
		}

		// 本轮 poll 汇总各设备的位移,作为一次 MouseMove 入队
		const int dx = static_cast<int>(std::lround(acc_x));
		const int dy = static_cast<int>(std::lround(acc_y));
		if (dx != 0 || dy != 0) {
			acc_x -= dx;
			acc_y -= dy;
			InputEvent mev;
			mev.kind = EInputKind::MouseMove;
			mev.dx = dx;
			mev.dy = dy;
			queue.Push(mev);
		}
	}
}

// 已打开的 evdev fd 发布给 GUI 线程做偶发按键状态查询(EVIOCGKEY)。
std::mutex g_evdev_fds_mutex;
std::vector<int> g_evdev_fds;

void PublishEvdevFds(const EvdevDevice* devs, int ndev)
{
	std::lock_guard<std::mutex> lock(g_evdev_fds_mutex);
	g_evdev_fds.clear();
	for (int i = 0; i < ndev; ++i)
		g_evdev_fds.push_back(devs[i].fd);
}

void UnpublishEvdevFds()
{
	std::lock_guard<std::mutex> lock(g_evdev_fds_mutex);
	g_evdev_fds.clear();
}

} // namespace

// 通过已打开的 evdev 设备实时读取全局左键状态。
// 线程安全:已打开 fd 列表被 mutex 保护。
bool QueryGlobalLeftButtonDown()
{
	std::lock_guard<std::mutex> lock(g_evdev_fds_mutex);
	for (int fd : g_evdev_fds) {
		if (fd < 0)
			continue;
		unsigned long bits[(KEY_CNT + 63) / 64] = {};
		if (ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) != -1) {
			if (TestBit(bits, BTN_LEFT))
				return true;
		}
	}
	return false;
}

#endif // LIVE2TEE_INPUT_EVDEV

#if defined(LIVE2TEE_INPUT_X11)
namespace {

// 把 X11 keysym 翻译成与 Windows 后端一致的 VK 码:
// config 的热键值(F1-F12 -> VK 0x70..0x7B)因此跨平台统一,无需改 config/GUI。
// 表情管线对"任意键"触发,不关心具体键值;映射不到的键返回 0。
int KeysymToVk(KeySym ks)
{
	if (ks >= XK_a && ks <= XK_z)
		return static_cast<int>(ks) - (XK_a - 0x41); // VK_A..VK_Z
	if (ks >= XK_A && ks <= XK_Z)
		return static_cast<int>(ks);
	if (ks >= XK_0 && ks <= XK_9)
		return static_cast<int>(ks); // VK_0..VK_9 = ASCII
	if (ks >= XK_F1 && ks <= XK_F24)
		return 0x70 + static_cast<int>(ks - XK_F1); // VK_F1..VK_F24
	switch (ks) {
	case XK_space:     return 0x20;
	case XK_Return:
	case XK_KP_Enter:  return 0x0D;
	case XK_Escape:    return 0x1B;
	case XK_BackSpace: return 0x08;
	case XK_Tab:       return 0x09;
	case XK_Shift_L:
	case XK_Shift_R:   return 0x10;
	case XK_Control_L:
	case XK_Control_R: return 0x11;
	case XK_Alt_L:
	case XK_Alt_R:     return 0x12;
	case XK_Left:      return 0x25;
	case XK_Up:        return 0x26;
	case XK_Right:     return 0x27;
	case XK_Down:      return 0x28;
	}
	return 0;
}

// 取 XI_RawMotion 中指定 valuator 的原始值(which=0 水平增量,1 垂直增量)。
bool RawValuator(const XIRawEvent* e, int which, double& out)
{
	const double* v = e->raw_values;
	for (int i = 0; i < e->valuators.mask_len * 8; ++i) {
		if (!XIMaskIsSet(e->valuators.mask, i))
			continue;
		if (i == which) {
			out = *v;
			return true;
		}
		++v;
	}
	return false;
}

} // namespace
#endif // LIVE2TEE_INPUT_X11

InputThread::InputThread(InputQueue& queue) : m_queue(queue) {}

InputThread::~InputThread()
{
	Stop();
}

void InputThread::Start()
{
	if (m_running)
		return;
	m_running = true;
	m_thread = std::thread([this] { Run(); });
}

void InputThread::Stop()
{
	if (!m_running)
		return;
	m_running = false;
	// Run() 阻塞在 poll(150ms 超时),到期自行检查 m_running 退出,
	// 不需要跨线程操作 Display 连接。
	if (m_thread.joinable())
		m_thread.join();
}

void InputThread::SetGuiHotkey(int vk)
{
	m_gui_hotkey_vk.store(vk, std::memory_order_relaxed);
}

bool InputThread::ConsumeGuiHotkey()
{
	return m_gui_hotkey_flag.exchange(false, std::memory_order_relaxed);
}

void InputThread::Run()
{
#if defined(LIVE2TEE_INPUT_EVDEV)
	{
		EvdevDevice devs[64];
		int ndev = OpenEvdevDevices(devs, 64);
		if (ndev == 0)
			ndev = OpenEvdevDevicesViaHelper(devs, 64, m_running); // pkexec 授权
		if (ndev > 0) {
			PublishEvdevFds(devs, ndev); // 供 QueryGlobalLeftButtonDown 用
			EvdevLoop(devs, ndev, m_queue, m_gui_hotkey_vk,
					  m_gui_hotkey_flag, m_running);
			UnpublishEvdevFds();
			for (int i = 0; i < ndev; ++i)
				close(devs[i].fd);
			return;
		}
		std::fprintf(stderr,
			"InputThread: 无法获取 evdev 设备读权限(授权被取消?).\n"
			"  免弹窗备选:sudo usermod -aG input $USER 后重新登录,\n"
			"  或为输入设备配置 udev uaccess 规则。回退到 XI2 后端。\n");
	}
#endif

#if defined(LIVE2TEE_INPUT_X11)
	Display* dpy = XOpenDisplay(nullptr);
	if (!dpy) {
		std::fprintf(stderr,
			"InputThread(X11): cannot open X display, global input disabled\n");
		return;
	}

	int xi_opcode = 0, xi_event_base = 0, xi_error_base = 0;
	if (!XQueryExtension(dpy, "XInputExtension",
						 &xi_opcode, &xi_event_base, &xi_error_base)) {
		std::fprintf(stderr, "InputThread(X11): XInput extension missing\n");
		XCloseDisplay(dpy);
		return;
	}
	int major = 2, minor = 0;
	if (XIQueryVersion(dpy, &major, &minor) != Success) {
		std::fprintf(stderr, "InputThread(X11): XInput2 unsupported\n");
		XCloseDisplay(dpy);
		return;
	}

	// 让服务器不要为长按合成 Release/Press 对:一次长按 = 一次 Press +
	// 一次 Release,与 Windows 低级钩子边沿语义一致;再配合按下状态表,
	// 彻底过滤自动重复(否则长按会连发几十个 KeyDown,表情狂闪)。
	Bool auto_repeat_supported = False;
	XkbSetDetectableAutoRepeat(dpy, True, &auto_repeat_supported);

	const Window root = DefaultRootWindow(dpy);
	unsigned char mask[(XI_LASTEVENT + 7) / 8] = {};
	XISetMask(mask, XI_RawMotion);
	XISetMask(mask, XI_RawButtonPress);
	XISetMask(mask, XI_RawButtonRelease);
	XISetMask(mask, XI_RawKeyPress);
	XISetMask(mask, XI_RawKeyRelease);
	XIEventMask xi_mask;
	xi_mask.deviceid = XIAllDevices; // raw 事件来自物理(slave)设备,需全选
	xi_mask.mask_len = sizeof(mask);
	xi_mask.mask = mask;
	XISelectEvents(dpy, root, &xi_mask, 1);
	XSync(dpy, False);

	// 按下状态表(keycode 索引):只放行"松开->按下"的沿
	bool key_down[256] = {};
	// 小数增量余量(触摸板等设备 raw 值为浮点):累积后四舍五入取整
	double acc_x = 0.0, acc_y = 0.0;

	while (m_running.load(std::memory_order_relaxed)) {
		struct pollfd pfd;
		pfd.fd = ConnectionNumber(dpy);
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 150) <= 0)
			continue; // 超时/被信号中断:回 while 检查 m_running

		while (XPending(dpy) > 0) {
			XEvent ev;
			XNextEvent(dpy, &ev);

			if (ev.type == MappingNotify) {
				XRefreshKeyboardMapping(&ev.xmapping);
				continue;
			}
			if (ev.xcookie.type != GenericEvent ||
				ev.xcookie.extension != xi_opcode ||
				!XGetEventData(dpy, &ev.xcookie)) {
				continue;
			}

			const XIRawEvent* r =
				static_cast<const XIRawEvent*>(ev.xcookie.data);

			if (ev.xcookie.evtype == XI_RawMotion) {
				double v = 0.0;
				if (RawValuator(r, 0, v))
					acc_x += v;
				if (RawValuator(r, 1, v))
					acc_y += v;
				const int dx = static_cast<int>(std::lround(acc_x));
				const int dy = static_cast<int>(std::lround(acc_y));
				if (dx != 0 || dy != 0) {
					acc_x -= dx;
					acc_y -= dy;
					InputEvent mev;
					mev.kind = EInputKind::MouseMove;
					mev.dx = dx;
					mev.dy = dy;
					m_queue.Push(mev);
				}
			} else if (ev.xcookie.evtype == XI_RawButtonPress ||
					   ev.xcookie.evtype == XI_RawButtonRelease) {
				InputEvent bev;
				if (r->detail == 1)
					bev.kind = EInputKind::MouseLeft;
				else if (r->detail == 3)
					bev.kind = EInputKind::MouseRight;
				else {
					XFreeEventData(dpy, &ev.xcookie);
					continue; // 中键/侧键等忽略
				}
				bev.pressed = (ev.xcookie.evtype == XI_RawButtonPress);
				m_queue.Push(bev);
			} else if (ev.xcookie.evtype == XI_RawKeyPress ||
					   ev.xcookie.evtype == XI_RawKeyRelease) {
				const bool pressed = (ev.xcookie.evtype == XI_RawKeyPress);
				const int kc = r->detail & 0xFF;
				const int vk = KeysymToVk(
					XkbKeycodeToKeysym(dpy, r->detail, 0, 0));
				if (vk == 0) {
					key_down[kc] = pressed; // 维持边沿表,但不产生事件
					XFreeEventData(dpy, &ev.xcookie);
					continue;
				}
				if (pressed) {
					if (key_down[kc]) {
						XFreeEventData(dpy, &ev.xcookie);
						continue; // 自动重复
					}
					key_down[kc] = true;
					if (m_gui_hotkey_vk.load(std::memory_order_relaxed) == vk) {
						// GUI 唤起热键在输入层截获,不进表情管线
						m_gui_hotkey_flag.store(true, std::memory_order_relaxed);
					} else {
						InputEvent kev;
						kev.kind = EInputKind::Key;
						kev.pressed = true;
						kev.keycode = vk;
						m_queue.Push(kev);
					}
				} else {
					if (!key_down[kc]) {
						XFreeEventData(dpy, &ev.xcookie);
						continue; // 无对应按下(焦点变化/合成事件)
					}
					key_down[kc] = false;
					InputEvent kev;
					kev.kind = EInputKind::Key;
					kev.pressed = false;
					kev.keycode = vk;
					m_queue.Push(kev);
				}
			}
			XFreeEventData(dpy, &ev.xcookie);
		}
	}

	XCloseDisplay(dpy);
#else
	std::fprintf(stderr, "InputThread: no global input backend available\n");
#endif // LIVE2TEE_INPUT_X11
}

#else // 其他平台(如 macOS):空实现,编译通过、无输入

InputThread::InputThread(InputQueue& queue) : m_queue(queue) {}

InputThread::~InputThread() = default;

void InputThread::Start()
{
	std::fprintf(stderr, "InputThread: this platform has no global input backend yet\n");
}

void InputThread::Stop() {}

void InputThread::Run() {}

void InputThread::SetGuiHotkey(int vk) {}

bool InputThread::ConsumeGuiHotkey()
{
	return false;
}

#endif // 平台后端分支

} // namespace live2tee
