#pragma once

// Tee 预览窗口(可选,调试用):显示离屏渲染输出的帧。
//   - 无边框 + 置顶 + Tool;整窗可拖动
//   - 双击右键 -> 请求唤起配置 GUI;ESC 隐藏
//   - 透明模式 = 半透明窗口 + 帧直通 alpha;绿幕模式 = 不透明绿底
// 窗口不承担渲染职责,只显示 OffscreenOutput 推来的帧。

#include <QImage>
#include <QWidget>

#include "../config.h"

class QMouseEvent;
class QKeyEvent;

namespace live2tee {

class PreviewWindow : public QWidget {
	Q_OBJECT
public:
	explicit PreviewWindow(const AppConfig& cfg, QWidget* parent = nullptr);

	void ApplyConfig(const AppConfig& cfg); // 背景模式 / 尺寸
	void SetFrame(const QImage& frame);

signals:
	void GuiRequested();

protected:
	void paintEvent(QPaintEvent* e) override;
	void mousePressEvent(QMouseEvent* e) override;
	void mouseMoveEvent(QMouseEvent* e) override;
	void mouseReleaseEvent(QMouseEvent* e) override;
	void mouseDoubleClickEvent(QMouseEvent* e) override;
	void keyPressEvent(QKeyEvent* e) override;

private:
	void ApplyWindowMode();

	AppConfig cfg_;
	QImage frame_;
	QPoint drag_pos_;
	bool dragging_ = false;
};

} // namespace live2tee
