#pragma once

// 全局输入:鼠标移动/左右键 + 键盘按键。
//   Windows:WH_MOUSE_LL + WH_KEYBOARD_LL 钩子(按键)+ Raw Input 消息窗口
//   (鼠标运动增量,需要在拥有消息循环的线程里创建,与钩子同线程)。
//   其他平台:目前为空实现(编译通过,无输入),待接入 X11/Wayland/macOS 后端。
// 偏移控制:只消费鼠标的相对运动增量(dx/dy),不依赖任何平台的
// 屏幕绝对坐标 —— 屏幕边缘钳制、全屏锁鼠标、多显示器、DPI 虚拟化都不影响。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

#if defined(_WIN32)
	#include <windows.h>
	#define LIVE2TEE_INPUT_WIN32 1
#else
	#define LIVE2TEE_INPUT_WIN32 0
#endif

namespace live2tee {

// 查询光标相对屏幕(虚拟桌面)中心的偏移。
// 用于偏移控制的启动锚定与手动校准:让虚拟偏移对齐光标的真实方位,
// 消除运动增量积分长时间累积的漂移(指针加速误差等)。
// 平台拿不到全局光标位置(如 Wayland 沙盒)时返回 false。
bool QueryMouseOffsetFromScreenCenter(int& off_x, int& off_y);

// 查询全局鼠标左键当前是否按下(供 GUI 偶发轮询,如"设置朝向原点"的确认点击)。
// 返回 false 表示未按下或平台不支持。
bool QueryGlobalLeftButtonDown();

enum class EInputKind {
	MouseMove,
	MouseLeft,
	MouseRight,
	Key,
};

struct InputEvent {
	EInputKind kind;
	int dx = 0;
	int dy = 0;
	bool pressed = false;  // 鼠标键/键盘键:按下=true, 抬起=false
	int keycode = 0;      // 键盘事件:平台相关键码(Windows = VK_*)
};

// 线程安全事件队列(多生产者单消费者)。
class InputQueue {
public:
	void Push(InputEvent ev);
	bool Pop(InputEvent& out);
	void Clear();

private:
	std::mutex m_mutex;
	std::queue<InputEvent> m_queue;
};

// 全局输入线程(非 Windows 平台为 no-op)。
class InputThread {
public:
	explicit InputThread(InputQueue& queue);
	~InputThread();

	void Start();
	void Stop();

	// 唤起 GUI 的全局热键(VK 码,0 = 禁用)。
	// 热键在键盘钩子层截获,不再作为表情按键进入输入队列;
	// 触发时置位标志,主线程用 ConsumeGuiHotkey() 轮询消费。
	void SetGuiHotkey(int vk);
	bool ConsumeGuiHotkey();

private:
	void Run();

#if LIVE2TEE_INPUT_WIN32
	static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM w, LPARAM l);
	static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w, LPARAM l);
	static LRESULT CALLBACK RawInputWndProc(HWND wnd, UINT msg, WPARAM w, LPARAM l);

	// 在当前线程创建 message-only 窗口并注册 Raw Input(鼠标),
	// 返回窗口句柄;WM_INPUT 在该窗口过程里转成 MouseMove 事件入队。
	HWND CreateRawInputWindow();

	DWORD m_thread_id = 0;
	HHOOK m_mouse_hook = nullptr;
	HHOOK m_keyboard_hook = nullptr;
	HWND m_raw_wnd = nullptr;
#endif

	InputQueue& m_queue;
	std::thread m_thread;
	std::atomic<bool> m_running{false};
	std::atomic<int> m_gui_hotkey_vk{0};   // GUI 唤起热键 VK 码
	std::atomic<bool> m_gui_hotkey_flag{false}; // 热键触发标志(钩子线程置位)
};

} // namespace live2tee
