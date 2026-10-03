// 离屏渲染输出实现。

#include "offscreen_output.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>

#include <cstdio>

#include "../state/lua_behavior.h"
#include "scene.h"

namespace live2tee {

namespace {

// FBO 内容是预乘 alpha(见 TeeScene::Render),转回直通 alpha:
// canvas putImageData / QPainter 都按直通 alpha 解释像素。
void UnpremultiplyInPlace(QImage& img)
{
	if (img.isNull() || img.format() != QImage::Format_RGBA8888)
		return;
	for (int y = 0; y < img.height(); ++y) {
		QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
		for (int x = 0; x < img.width(); ++x) {
			const QRgb p = line[x];
			const int a = qAlpha(p);
			if (a == 0 || a == 255)
				continue;
			const int r = qMin(255, qRed(p) * 255 / a);
			const int g = qMin(255, qGreen(p) * 255 / a);
			const int b = qMin(255, qBlue(p) * 255 / a);
			line[x] = qRgba(r, g, b, a);
		}
	}
}

} // namespace

OffscreenOutput::OffscreenOutput(const AppConfig& cfg, InputQueue* queue, QObject* parent)
	: QObject(parent)
	, cfg_(cfg)
	, queue_(queue)
{
}

OffscreenOutput::~OffscreenOutput()
{
	if (context_ && fbo_) {
		context_->makeCurrent(surface_);
		delete fbo_;
		fbo_ = nullptr;
		scene_.reset(); // GL 对象必须在上下文存活时销毁
		context_->doneCurrent();
	}
	delete context_;
	delete surface_;
}

bool OffscreenOutput::Initialize()
{
	QSurfaceFormat fmt;
	fmt.setVersion(2, 1);
	fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
	fmt.setOption(QSurfaceFormat::DeprecatedFunctions, true);
	fmt.setAlphaBufferSize(8);

	context_ = new QOpenGLContext;
	context_->setFormat(fmt);
	if (!context_->create()) {
		std::fprintf(stderr, "offscreen: context create failed\n");
		return false;
	}

	surface_ = new QOffscreenSurface;
	surface_->setFormat(fmt);
	surface_->create();
	if (!surface_->isValid()) {
		std::fprintf(stderr, "offscreen: surface invalid\n");
		return false;
	}

	context_->makeCurrent(surface_);
	auto* f = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_2_1>(context_);
	if (f)
		f->initializeOpenGLFunctions();

	scene_ = std::make_unique<TeeScene>(cfg_, queue_);
	const bool ok = scene_->Init(f);
	if (!ok)
		std::fprintf(stderr, "offscreen: scene init failed\n");

	// 启动锚定:初始偏移 = 当前真实光标方位,Tee 一启动就朝向鼠标
	scene_->CalibrateMouseOffset();

	// 行为脚本:按 cfg_.behavior_scripts 加载(空列表/全部失败 = 内置默认)
	ReloadBehaviorScripts();

	fbo_ = new QOpenGLFramebufferObject(cfg_.output_size, cfg_.output_size);
	context_->doneCurrent();

	connect(&timer_, &QTimer::timeout, this, &OffscreenOutput::RenderFrame);
	RestartTimer();
	return ok;
}

void OffscreenOutput::ReloadBehaviorScripts()
{
	if (!scene_)
		return;
	QStringList paths;
	paths.reserve(cfg_.behavior_scripts.size());
	for (const QString& name : cfg_.behavior_scripts)
		paths << cfg_.ResolvedScriptsDir() + QStringLiteral("/") + name;

	std::string lua_err;
	if (auto lua_behavior = LuaBehavior::Load(paths, &lua_err)) {
		std::fprintf(stderr, "behavior: 已加载脚本 [%s]\n",
					 cfg_.behavior_scripts.join(QStringLiteral(", ")).toUtf8().constData());
		scene_->SetBehavior(std::move(lua_behavior));
	} else {
		// 空列表(用户清空)/全部缺失或失败:回退内置默认行为
		scene_->SetBehavior(nullptr);
	}
	if (!lua_err.empty())
		std::fprintf(stderr, "behavior: 部分脚本加载失败,已跳过:\n%s",
					 lua_err.c_str());
}

void OffscreenOutput::RestartTimer()
{
	const int fps = std::max(1, cfg_.output_fps);
	timer_.start(1000 / fps);
}

void OffscreenOutput::ApplyConfig(const AppConfig& cfg)
{
	if (!context_)
		return;

	const int old_size = cfg_.output_size;
	const int old_fps = cfg_.output_fps;
	cfg_ = cfg;

	context_->makeCurrent(surface_);
	scene_->ApplyConfig(cfg_);
	if (cfg_.output_size != old_size) {
		delete fbo_;
		fbo_ = new QOpenGLFramebufferObject(cfg_.output_size, cfg_.output_size);
	}
	context_->doneCurrent();

	// 脚本列表/内容可能已变化,每次应用都重建行为映射器
	ReloadBehaviorScripts();

	if (cfg_.output_fps != old_fps)
		RestartTimer();
}

void OffscreenOutput::RenderFrame()
{
	if (!context_ || !fbo_)
		return;

	context_->makeCurrent(surface_);
	fbo_->bind();
	scene_->Render(fbo_->width(), fbo_->height());
	QImage img = fbo_->toImage(); // 默认翻转为常规自上而下方向
	fbo_->release();
	context_->doneCurrent();

	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	UnpremultiplyInPlace(img);

	emit FrameReady(img);
}

} // namespace live2tee
