// PNG 纹理加载实现:stb_image 解码 -> OpenGL 上传。
// GL 调用经 QOpenGLFunctions_2_1 分发(跨平台,由 Qt 上下文解析)。

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

#include "texture.h"

#include <QOpenGLFunctions_2_1>

#include <cstdio>

namespace live2tee {

bool LoadPng(const std::string& path, Texture& out, QOpenGLFunctions_2_1* f,
			 bool premultiply)
{
	int w = 0, h = 0, n = 0;
	unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4); // 强制 RGBA
	if (!data) {
		std::fprintf(stderr, "LoadPng: failed %s : %s\n", path.c_str(), stbi_failure_reason());
		return false;
	}
	if (!f) {
		std::fprintf(stderr, "LoadPng: no GL functions\n");
		stbi_image_free(data);
		return false;
	}

	if (premultiply) {
		// RGB 乘以 A(预乘)。放大 256 倍做查表,避免逐像素浮点乘法。
		unsigned char table[256 * 256];
		for (int c = 0; c < 256; ++c)
			for (int a = 0; a < 256; ++a)
				table[c * 256 + a] = static_cast<unsigned char>((c * a + 127) / 255);
		const long long total = static_cast<long long>(w) * h;
		for (long long i = 0; i < total; ++i) {
			unsigned char* px = data + i * 4;
			const int a = px[3];
			if (a == 255)
				continue;
			px[0] = table[px[0] * 256 + a];
			px[1] = table[px[1] * 256 + a];
			px[2] = table[px[2] * 256 + a];
		}
	}

	GLuint id = 0;
	f->glGenTextures(1, &id);
	f->glBindTexture(GL_TEXTURE_2D, id);
	f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	f->glBindTexture(GL_TEXTURE_2D, 0);

	stbi_image_free(data);

	out.gl_id = id;
	out.width = w;
	out.height = h;
	return true;
}

void DeleteTexture(Texture& tex, QOpenGLFunctions_2_1* f)
{
	if (tex.gl_id != 0 && f) {
		f->glDeleteTextures(1, &tex.gl_id);
		tex.gl_id = 0;
	}
}

} // namespace live2tee
