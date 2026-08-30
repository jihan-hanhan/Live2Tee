// Tee 预览窗口实现。

#include "preview_window.h"

#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace live2tee {

PreviewWindow::PreviewWindow(const AppConfig& cfg, QWidget* parent)
	: QWidget(parent)
	, cfg_(cfg)
{
	setWindowTitle(tr("Live2Tee 预览"));
	ApplyWindowMode();
	resize(cfg_.output_size, cfg_.output_size);
}

void PreviewWindow::ApplyWindowMode()
{
	// 透明模式需要 WA_TranslucentBackground,让帧的 alpha 直接透出桌面;
	// 绿幕模式保持不透明,由 paintEvent 填纯绿。
	// 改 attribute 后必须重设 flags 并重新 show 才能生效(窗口会重建)。
	setAttribute(Qt::WA_TranslucentBackground, !cfg_.green_screen);
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	if (isVisible())
		show();
}

void PreviewWindow::ApplyConfig(const AppConfig& cfg)
{
	const bool bg_changed = (cfg.green_screen != cfg_.green_screen);
	cfg_ = cfg;
	if (bg_changed)
		ApplyWindowMode();
	resize(cfg_.output_size, cfg_.output_size);
	update();
}

void PreviewWindow::SetFrame(const QImage& frame)
{
	frame_ = frame;
	update();
}

void PreviewWindow::paintEvent(QPaintEvent* e)
{
	QWidget::paintEvent(e);
	QPainter p(this);
	if (cfg_.green_screen)
		p.fillRect(rect(), QColor(0, 255, 0)); // OBS 色度键用纯绿
	if (!frame_.isNull()) {
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(rect(), frame_);
	}
}

void PreviewWindow::mousePressEvent(QMouseEvent* e)
{
	if (e->button() == Qt::LeftButton) {
		dragging_ = true;
		drag_pos_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
		e->accept();
		return;
	}
	QWidget::mousePressEvent(e);
}

void PreviewWindow::mouseMoveEvent(QMouseEvent* e)
{
	if (dragging_ && (e->buttons() & Qt::LeftButton)) {
		move(e->globalPosition().toPoint() - drag_pos_);
		e->accept();
		return;
	}
	QWidget::mouseMoveEvent(e);
}

void PreviewWindow::mouseReleaseEvent(QMouseEvent* e)
{
	if (e->button() == Qt::LeftButton) {
		dragging_ = false; // 松开必须停止,否则之后鼠标一进窗口就会被拖走
		e->accept();
		return;
	}
	QWidget::mouseReleaseEvent(e);
}

void PreviewWindow::mouseDoubleClickEvent(QMouseEvent* e)
{
	if (e->button() == Qt::RightButton) {
		emit GuiRequested(); // 双击右键唤起配置 GUI
		e->accept();
		return;
	}
	QWidget::mouseDoubleClickEvent(e);
}

void PreviewWindow::keyPressEvent(QKeyEvent* e)
{
	if (e->key() == Qt::Key_Escape) {
		hide(); // 预览只是调试窗口,ESC 隐藏(托盘可再开)
		return;
	}
	QWidget::keyPressEvent(e);
}

} // namespace live2tee
