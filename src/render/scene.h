#pragma once

// TeeScene:上下文无关的 Tee 渲染场景。
// 从旧 TeeGLWidget 抽取:状态机 + 纹理 + tee_render 管线,
// 可在任意 GL 2.1 兼容上下文里渲染(离屏 FBO / 预览 / 未来其他输出)。
// 渲染管线是预乘 alpha(见 Render),保证透明底离屏帧的 alpha 数学正确。

#include <chrono>
#include <memory>

#include <QPoint>

#include "../config.h"
#include "../input/input.h"
#include "../state/state.h"
#include "texture.h"
#include <tee_emoticon.h>
#include <tee_render_info.h>
#include <tee_renderer.h>

class QOpenGLFunctions_2_1;

namespace live2tee {

class GLBackend;

class TeeScene {
public:
	TeeScene(const AppConfig& cfg, InputQueue* queue);
	~TeeScene();

	// 需在当前线程有激活 GL 上下文时调用。
	bool Init(QOpenGLFunctions_2_1* f);

	// 配置变更(皮肤/目录/缩放)。纹理重载需要 makeCurrent,由调用方保证。
	void ApplyConfig(const AppConfig& cfg);

	// 预览/缩略图场景:仅替换皮肤纹理,不重新加载 game.png/emoticons.png。
	// 需在 makeCurrent 后调用;成功返回 true。
	bool ReloadSkinTexture(const QString& skin_path);

	// 渲染一帧到当前绑定的帧缓冲(像素坐标,左上原点,y 向下)。
	// 始终清为透明黑;预览窗口的绿幕底色由窗口侧自己填。
	void Render(int w, int h);

	TeeState& State() { return state_; }

	// 鼠标偏移清零(朝向校准,供 GUI"修正鼠标位置"按钮调用)。
	void ResetMouseOffset() { state_.ResetMouseOffset(); }

	// 校准:把鼠标偏移对齐到当前真实光标的方位
	// (Tee 朝向 = 光标相对屏幕中心的方位),消除积分累积漂移。
	// 平台查询失败时退化为清零(默认朝右)。
	void CalibrateMouseOffset();

	// 把朝向原点设为指定屏幕点 origin(Tee 朝向 = 光标相对 origin 的方位)。
	// 坐标用 Qt 逻辑像素(QCursor::pos),与高 DPI 缩放一致,跨 Windows/X11/macOS。
	void SetMouseOrigin(const QPoint& origin);

	// 周期性重锚定(供 Linux 帧循环调用):用真实光标位置校正积分偏移。
	// libinput 指针加速使原始位移积分系统性偏离真实光标(大幅移动尤甚),
	// 200ms 一次的重锚定把漂移压制在不可察觉的范围内。
	// 光标查询冻结(XWayland 下光标位于原生 Wayland 窗口)时自动跳过。
	void ReAnchorMouse();

private:
	float TeeSize() const; // 基础尺寸 × 用户缩放(render_scale)
	void ReloadTextures(); // 需已 makeCurrent
	void PumpInput();      // 排空全局输入队列 -> state_

	AppConfig cfg_;
	InputQueue* queue_;
	TeeState state_;

	QOpenGLFunctions_2_1* f_ = nullptr;
	std::unique_ptr<GLBackend> backend_;
	std::unique_ptr<teer::CTeeRenderer> renderer_;
	teer::STeeRenderInfo info_;
	std::unique_ptr<teer::CEmoticonRenderer> emo_;
	Texture tex_skin_, tex_emo_, tex_weapon_;

	std::chrono::steady_clock::time_point t0_;
	bool textures_ready_ = false;

	// 朝向原点模式:SetMouseOrigin 后生效,CalibrateMouseOffset 复位为屏幕中心
	bool has_custom_origin_ = false;
	QPoint custom_origin_;
	// 重锚定状态:上次查询的光标位置(新鲜度检测)与上次锚定时刻
	QPoint last_reanchor_cursor_;
	float last_reanchor_time_ = -1.0f;
};

} // namespace live2tee
