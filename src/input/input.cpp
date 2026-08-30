// 全局输入线程实现。
// Windows:WH_MOUSE_LL / WH_KEYBOARD_LL + 消息循环;钩子回调需要本线程
// 拥有消息循环,且 SetWindowsHookEx 必须在该线程内调用。
// 其他平台:暂为 no-op(编译通过、无输入),TODO: X11/Wayland/macOS 后端。

#include "input.h"

#include <cstdio>

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
#else
bool QueryMouseOffsetFromScreenCenter(int& off_x, int& off_y)
{
	(void)off_x; (void)off_y;
	return false; // TODO: X11(XQueryPointer)/macOS(CGEventGetLocation) 后端
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

#else // !LIVE2TEE_INPUT_WIN32 —— 空实现,TODO: 接入平台后端

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

#endif // LIVE2TEE_INPUT_WIN32

} // namespace live2tee
