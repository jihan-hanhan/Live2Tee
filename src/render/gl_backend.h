#pragma once

// ITeeRenderBackend 的 OpenGL 2.1 兼容(立即模式)实现。
// 不直接链 OpenGL,所有 GL 调用经 Qt 的 QOpenGLFunctions_2_1 分发,
// 由 QOpenGLWidget 的上下文解析函数指针 —— 跨平台,无需 glad/GLEW。
// 把 STeeQuad 翻译成 glBegin/glEnd 四边形;性能对几十个四边形/帧足够。
// 投影矩阵由 TeeGLWidget 用 glOrtho 设置成像素坐标 (左上原点,y 向下)。

#include <tee_backend.h>

class QOpenGLFunctions_2_1;

namespace live2tee {

class GLBackend : public teer::ITeeRenderBackend {
public:
	// 必须传入已 initialize 的 2.1 函数表(来自当前 GL 上下文)。
	explicit GLBackend(QOpenGLFunctions_2_1* funcs) : f_(funcs) {}

	void BeginTee() override;
	void EndTee() override;
	void DrawQuad(const teer::STeeQuad& quad) override;

private:
	QOpenGLFunctions_2_1* f_;
};

} // namespace live2tee
