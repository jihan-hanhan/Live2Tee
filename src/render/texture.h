#pragma once

// PNG 纹理加载:用 stb_image 读 PNG,上传到 OpenGL,返回 GL 纹理 id。
// GL 纹理 id 直接作为 teer::STextureHandle::m_Id 使用。
// GL 调用经 Qt 的 QOpenGLFunctions_2_1 分发(跨平台)。

#include <cstdint>
#include <string>

class QOpenGLFunctions_2_1;

namespace live2tee {

struct Texture {
	std::uint32_t gl_id = 0;  // OpenGL 纹理 id,直接作为 STextureHandle.m_Id
	int width = 0;
	int height = 0;

	bool Valid() const { return gl_id != 0; }
};

// 加载 PNG 文件,上传到 GL,Rgba8 + 线性过滤。成功返回 true。
// 需要当前线程有激活的 GL 上下文,并传入已初始化的 2.1 函数表。
// premultiply = 上传前把 RGB 乘以 A(预乘 alpha 纹理),
// 配合 glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) 得到正确的
// 透明底离屏合成;给不透明目标渲染时可关掉。
bool LoadPng(const std::string& path, Texture& out, QOpenGLFunctions_2_1* f,
			 bool premultiply = false);

// 删除 GL 纹理(需要在有上下文的线程调用)。
void DeleteTexture(Texture& tex, QOpenGLFunctions_2_1* f);

} // namespace live2tee
