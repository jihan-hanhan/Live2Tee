// GLBackend:把 STeeQuad 用 OpenGL 2.1 兼容(立即模式)画成旋转过的四边形。
// 所有 GL 调用经 QOpenGLFunctions_2_1 分发(跨平台,由 Qt 上下文解析)。
// 投影是像素坐标系(左上原点,y 向下),由 TeeGLWidget 用 glOrtho 设置好。

#include "gl_backend.h"

#include <QOpenGLFunctions_2_1>

#include <cmath>

namespace live2tee {

void GLBackend::BeginTee()
{
	// 立即模式无批处理,留空。
}

void GLBackend::EndTee()
{
	// 同上。
}

void GLBackend::DrawQuad(const teer::STeeQuad& q)
{
	if (q.m_Texture.m_Id == 0 || !f_)
		return;

	f_->glBindTexture(GL_TEXTURE_2D, q.m_Texture.m_Id);
	f_->glColor4f(q.m_Color.r, q.m_Color.g, q.m_Color.b, q.m_Color.a);

	const float hw = q.m_Width * 0.5f;
	const float hh = q.m_Height * 0.5f;
	const float cos_a = std::cos(q.m_Rotation);
	const float sin_a = std::sin(q.m_Rotation);

	// 本地四角(中心为原点),y 向下屏幕坐标。
	//   局部 (-hw,-hh) = 四边形上左角  -> UV (u0,v0) = 图片上左
	//   局部 (+hw,-hh) = 四边形上右角  -> UV (u1,v0)
	//   局部 (+hw, hh) = 四边形下右角  -> UV (u1,v1)
	//   局部 (-hw, hh) = 四边形下左角  -> UV (u0,v1)
	// PNG 经 stb 默认加载、glTexImage2D 上传后,V=0 采样到图片顶部,与图片坐标一致。
	struct Corner {
		float lx, ly;
		float u, v;
	};
	Corner c[4];
	const float u0 = q.m_U0, u1 = q.m_U1, v0 = q.m_V0, v1 = q.m_V1;
	if (!q.m_FlipX) {
		c[0] = {-hw, -hh, u0, v0};
		c[1] = { hw, -hh, u1, v0};
		c[2] = { hw,  hh, u1, v1};
		c[3] = {-hw,  hh, u0, v1};
	} else {
		// 水平镜像:u 取反
		c[0] = {-hw, -hh, u1, v0};
		c[1] = { hw, -hh, u0, v0};
		c[2] = { hw,  hh, u0, v1};
		c[3] = {-hw,  hh, u1, v1};
	}

	f_->glBegin(GL_QUADS);
	for (const Corner& cc : c) {
		const float wx = q.m_Position.x + (cc.lx * cos_a - cc.ly * sin_a);
		const float wy = q.m_Position.y + (cc.lx * sin_a + cc.ly * cos_a);
		f_->glTexCoord2f(cc.u, cc.v);
		f_->glVertex2f(wx, wy);
	}
	f_->glEnd();
}

} // namespace live2tee
