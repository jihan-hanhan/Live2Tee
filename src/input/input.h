#pragma once

// 全局输入:鼠标移动/左右键 + 键盘按键。
//   Windows:Raw Input 消息窗口(message-only + RIDEV_INPUTSINK,后台也接收),
//   鼠标运动增量/按键与键盘按键全部由 WM_INPUT 旁路拷贝提供。
//   刻意不用 WH_MOUSE_LL/WH_KEYBOARD_LL 低级钩子:钩子在系统分派路径上被
//   同步等待,回调线程的任何调度延迟都会拖慢全系统输入 —— 锁定鼠标的游戏
//   (Minecraft 等,每帧 SetCursorPos 重置光标 + 高频移动)会出现明显卡顿与
//   位移积压突跳。Raw Input 只读监听,不在分派路径上,对游戏零干扰。
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
	Emoticon, // 程序化触发表情气泡:emoticon_id 指定编号,<0 = 随机
};

struct InputEvent {
	EInputKind kind;
	int dx = 0;
	int dy = 0;
	bool pressed = false;  // 鼠标键/键盘键:按下=true, 抬起=false
	int keycode = 0;      // 键盘事件:平台相关键码(Windows = VK_*)
	int emoticon_id = -1; // Emoticon 事件:0..NUM_EMOTICONS-1 指定,<0 = 随机
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
	// 热键在输入层截获,不再作为表情按键进入输入队列;
	// 触发时置位标志,主线程用 ConsumeGuiHotkey() 轮询消费。
	void SetGuiHotkey(int vk);
	bool ConsumeGuiHotkey();

private:
	void Run();

#if LIVE2TEE_INPUT_WIN32
	static LRESULT CALLBACK RawInputWndProc(HWND wnd, UINT msg, WPARAM w, LPARAM l);

	// 在当前线程创建 message-only 窗口并注册 Raw Input(鼠标+键盘),
	// 返回窗口句柄;WM_INPUT 在该窗口过程里转成输入事件入队。
	HWND CreateRawInputWindow();

	DWORD m_thread_id = 0;
	HWND m_raw_wnd = nullptr;

	// 相对鼠标增量的屏幕钳制校正状态(见 RawInputWndProc 相对设备分支):
	// 原始计数 -> 像素的自适应比例,仅正常移动帧更新
	double m_count_to_px_x = 1.0, m_count_to_px_y = 1.0;
	bool m_have_last_cursor = false; // m_last_cursor 是否已有初值
	POINT m_last_cursor{};           // 上一次事件核对时的系统光标(像素)
	double m_rem_x = 0.0, m_rem_y = 0.0; // 缩放后不足 1 个计数的小数余量
#endif

	InputQueue& m_queue;
	std::thread m_thread;
	std::atomic<bool> m_running{false};
	std::atomic<int> m_gui_hotkey_vk{0};   // GUI 唤起热键 VK 码
	std::atomic<bool> m_gui_hotkey_flag{false}; // 热键触发标志(输入线程置位)
};

} // namespace live2tee
