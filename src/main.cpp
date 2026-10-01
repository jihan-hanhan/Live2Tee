// Live2Tee 主程序(Qt6):
//   OffscreenOutput —— 离屏渲染 Tee 场景,按输出帧率产出直通 alpha 的 RGBA 帧
//   BrowserServer   —— 本地 WebSocket 推帧,OBS 浏览器源加载 assets/browser/
//                      live2tee.html 消费(真 alpha 直出,无需色键,不占桌面)
//   PreviewWindow   —— 可选的桌面预览(调试用)
//   ControlWindow   —— 配置 GUI(热键 / 托盘 / 预览双右键唤起)
// 全局输入(鼠标/键盘钩子)驱动 Tee 动作,与任何窗口的焦点无关。

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSurfaceFormat>
#include <QSystemTrayIcon>
#include <QTimer>

#include <cstdio>

#include "config.h"
#include "gui/control_window.h"
#include "gui/preview_window.h"
#include "input/input.h"
#include "net/browser_server.h"
#include "render/offscreen_output.h"
#include "render/scene.h"

namespace {

// 托盘图标:运行时画一个简化的 Tee(棕圆 + 白眼),不依赖资源文件。
QPixmap MakeTrayIcon()
{
	QPixmap pm(32, 32);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(QColor(34, 32, 52), 3));
	p.setBrush(QColor(104, 74, 0));
	p.drawEllipse(3, 3, 26, 26);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	p.drawEllipse(9, 11, 6, 7);
	p.drawEllipse(19, 11, 6, 7);
	p.setBrush(QColor(34, 32, 52));
	p.drawEllipse(12, 13, 2, 3);
	p.drawEllipse(20, 13, 2, 3);
	return pm;
}

} // namespace

// 递归复制目录(首次运行时把捆绑 assets 复制到用户可写的 XDG 数据目录)
static bool CopyDirRecursive(const QString& src, const QString& dst)
{
	QDir src_dir(src);
	if (!src_dir.exists())
		return false;
	QDir().mkpath(dst);
	const QStringList entries = src_dir.entryList(
		QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString& entry : entries) {
		const QString src_path = src + QStringLiteral("/") + entry;
		const QString dst_path = dst + QStringLiteral("/") + entry;
		if (QFileInfo(src_path).isDir()) {
			if (!CopyDirRecursive(src_path, dst_path))
				return false;
		} else {
			if (!QFile::copy(src_path, dst_path))
				return false;
		}
	}
	return true;
}

int main(int argc, char* argv[])
{
#if defined(LIVE2TEE_INPUT_X11)
	// X11 输入后端需要 X 服务器(Wayland 会话下经 XWayland)。若 Qt 走 Wayland
	// 平台插件,离屏 GL 会经 EGL->Zink->lavapipe 路径初始化,Mesa 软渲染在
	// 该路径上崩溃(SIGSEGV),且 Wayland 原生协议不允许全局输入捕获。
	// 有 DISPLAY 且用户未显式指定平台时强制 xcb;用户显式设置则尊重用户。
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qgetenv("DISPLAY").isEmpty())
		qputenv("QT_QPA_PLATFORM", "xcb");
#endif

	// GL 2.1 兼容上下文(立即模式) + alpha 通道(离屏 FBO 需要)
	QSurfaceFormat fmt;
	fmt.setVersion(2, 1);
	fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
	fmt.setOption(QSurfaceFormat::DeprecatedFunctions, true);
	fmt.setAlphaBufferSize(8);
	QSurfaceFormat::setDefaultFormat(fmt);

	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("Live2Tee"));

	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	// ---- 配置 ----
	const live2tee::AppConfig cfg = live2tee::AppConfig::Load();

	// assets 初始化:
	//   1. 便携模式(exe 同级 assets 有效)则直接使用;
	//   2. 否则若解析到 XDG 数据目录且缺少关键文件,从捆绑目录复制初始化;
	//   3. 确保 skins/ 和 browser/ 子目录存在。
	const QString assets_dir = cfg.ResolvedAssetsDir();
	const QString bundled = live2tee::AppConfig::BundledAssetsDir();
	if (assets_dir != bundled &&
		!QFileInfo::exists(assets_dir + QStringLiteral("/game.png")) &&
		QFileInfo::exists(bundled + QStringLiteral("/game.png"))) {
		std::fprintf(stderr, "first run: copying assets %s -> %s\n",
			bundled.toUtf8().constData(), assets_dir.toUtf8().constData());
		CopyDirRecursive(bundled, assets_dir);
	}
	QDir().mkpath(assets_dir + QStringLiteral("/skins"));
	QDir().mkpath(assets_dir + QStringLiteral("/browser"));

	// ---- 全局输入 ----
	live2tee::InputQueue queue;
	live2tee::InputThread input_thread(queue);
	input_thread.SetGuiHotkey(cfg.HotkeyVk());
	input_thread.Start();

	// ---- 离屏渲染输出 ----
	live2tee::OffscreenOutput output(cfg, &queue);
	if (!output.Initialize())
		std::fprintf(stderr, "offscreen renderer init failed\n");

	// ---- 浏览器源输出(OBS) ----
	live2tee::BrowserServer browser(cfg);
	if (cfg.browser_output)
		browser.Start(static_cast<quint16>(cfg.browser_port));
	QObject::connect(&output, &live2tee::OffscreenOutput::FrameReady,
					 &browser, &live2tee::BrowserServer::SendFrame);

	// ---- 预览窗口(可选) ----
	live2tee::PreviewWindow preview(cfg);
	QObject::connect(&output, &live2tee::OffscreenOutput::FrameReady,
					 &preview, &live2tee::PreviewWindow::SetFrame);
	if (cfg.preview_window)
		preview.show();

	// ---- 配置 GUI ----
	live2tee::ControlWindow gui(cfg);

	int running_port = cfg.browser_port;
	QObject::connect(&gui, &live2tee::ControlWindow::ConfigApplied,
					 [&](const live2tee::AppConfig& c) {
		output.ApplyConfig(c);
		preview.ApplyConfig(c);
		preview.setVisible(c.preview_window);
		if (c.browser_output) {
			if (!browser.IsRunning() || c.browser_port != running_port) {
				browser.Start(static_cast<quint16>(c.browser_port));
				running_port = c.browser_port;
			}
		} else {
			browser.Stop();
		}
		input_thread.SetGuiHotkey(c.HotkeyVk());
	});

	auto ShowGui = [&] {
		gui.show();
		gui.raise();
		gui.activateWindow();
	};
	QObject::connect(&preview, &live2tee::PreviewWindow::GuiRequested, ShowGui);

	// GUI"修正鼠标位置"按钮:把虚拟偏移重新锚定到当前真实光标方位
	QObject::connect(&gui, &live2tee::ControlWindow::ResetVMouseRequested, [&] {
		if (output.Scene())
			output.Scene()->CalibrateMouseOffset();
	});

	// GUI"设置朝向原点":用户选定屏幕点 origin,Tee 朝向 = 光标相对 origin 的方位
	QObject::connect(&gui, &live2tee::ControlWindow::SetOriginRequested,
					 [&](const QPoint& origin) {
		if (output.Scene())
			output.Scene()->SetMouseOrigin(origin);
	});

	// 全局热键轮询(键盘钩子线程置位标志,主线程消费)
	QTimer hotkey_poll;
	hotkey_poll.setInterval(100);
	QObject::connect(&hotkey_poll, &QTimer::timeout, [&] {
		if (input_thread.ConsumeGuiHotkey()) {
			if (gui.isVisible())
				gui.hide();
			else
				ShowGui();
		}
	});
	hotkey_poll.start();

	// ---- 托盘 ----
	QMenu tray_menu;
	QAction* settings_action = tray_menu.addAction(QStringLiteral("设置(&S)"));
	QAction* preview_action = tray_menu.addAction(QStringLiteral("预览窗口(&P)"));
	preview_action->setCheckable(true);
	preview_action->setChecked(cfg.preview_window);
	QAction* quit_action = tray_menu.addAction(QStringLiteral("退出(&Q)"));
	QObject::connect(settings_action, &QAction::triggered, ShowGui);
	QObject::connect(preview_action, &QAction::toggled,
					 &preview, &QWidget::setVisible);
	QObject::connect(quit_action, &QAction::triggered, [] { qApp->quit(); });

	QSystemTrayIcon tray(MakeTrayIcon());
	tray.setContextMenu(&tray_menu);
	tray.setToolTip(QStringLiteral("Live2Tee"));
	QObject::connect(&tray, &QSystemTrayIcon::activated,
					 [&](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::DoubleClick)
			ShowGui();
	});
	tray.show();

	const int ret = app.exec();

	input_thread.Stop();
	return ret;
}
