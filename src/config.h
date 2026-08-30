#pragma once

// 程序配置:与可执行文件同级的 config.json。
// 空目录字段 = 使用默认值(exe 同级 assets、assets/skins)。

#include <QString>
#include <QStringList>

namespace live2tee {

// 扫描 skins 目录下的 *.png(按文件名排序)。
QStringList ScanSkinFiles(const QString& skins_dir);

struct AppConfig {
	// ---- 持久化字段 ----
	QString assets_dir;        // 空 = exe 同级 /assets
	QString skins_dir;         // 空 = <assets_dir>/skins
	QString skin;              // 皮肤文件名(相对 skins_dir),空 = 自动挑第一个
	int output_size = 512;     // 输出边长(离屏帧/预览窗口,正方形)
	int output_fps = 60;       // 输出帧率
	bool browser_output = true;  // 浏览器源(本地 WebSocket)输出开关
	int browser_port = 8787;     // WebSocket 端口(仅监听 127.0.0.1)
	bool preview_window = false; // 桌面预览窗口开关(调试用,默认关)
	bool green_screen = false;   // 仅作用于预览窗口背景;浏览器输出始终透明 alpha
	float render_scale = 1.0f;   // 渲染整体缩放(Tee/武器/表情)
	QString gui_hotkey = "F9";   // 唤起配置 GUI 的全局热键(F1-F12,空 = 禁用)

	// ---- 路径解析 ----
	QString ConfigPath() const; // config.json 的绝对路径
	QString ResolvedAssetsDir() const;
	QString ResolvedSkinsDir() const;
	QString SkinPath() const;   // 当前皮肤完整路径

	// 解析 gui_hotkey 为 Windows VK 码(F1-F12),无效/禁用返回 0。
	int HotkeyVk() const;

	// 读/写 config.json(不存在或字段缺失用默认值)。
	static AppConfig Load();
	bool Save() const;
};

} // namespace live2tee
