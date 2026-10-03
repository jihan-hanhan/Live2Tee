#pragma once

// 离屏渲染输出:QOffscreenSurface + FBO,按输出帧率渲染 Tee 场景并读回。
// 帧为直通 alpha 的 RGBA8888(FBO 里是预乘 alpha,读回时已转回直通),
// 经 FrameReady 信号分发给浏览器源输出 / 预览窗口。

#include <QImage>
#include <QObject>
#include <QTimer>
#include <memory>

#include "../config.h"
#include "../input/input.h"

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;

namespace live2tee {

class TeeScene;

class OffscreenOutput : public QObject {
	Q_OBJECT
public:
	OffscreenOutput(const AppConfig& cfg, InputQueue* queue, QObject* parent = nullptr);
	~OffscreenOutput() override; // .cpp 定义(unique_ptr 成员的前置类型)

	// 创建离屏上下文 + 场景。失败时仅打印日志,不影响程序其他部分。
	bool Initialize();

	void ApplyConfig(const AppConfig& cfg);

	TeeScene* Scene() const { return scene_.get(); }

signals:
	void FrameReady(const QImage& frame); // 直通 alpha 的 RGBA8888

private slots:
	void RenderFrame();

private:
	void RestartTimer();
	// 按 cfg_.behavior_scripts 重建行为映射器(空列表/全部失败 = 内置默认);
	// 启动 Initialize 与每次 ApplyConfig 均调用
	void ReloadBehaviorScripts();

	AppConfig cfg_;
	InputQueue* queue_;
	QOffscreenSurface* surface_ = nullptr;
	QOpenGLContext* context_ = nullptr;
	QOpenGLFramebufferObject* fbo_ = nullptr;
	std::unique_ptr<TeeScene> scene_;
	QTimer timer_;
};

} // namespace live2tee
