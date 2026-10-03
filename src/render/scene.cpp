// TeeScene 实现:渲染管线从旧 TeeGLWidget(paintGL)原样移植,
// 唯一的行为变更是混合模式换成了预乘 alpha(离屏透明合成的正确公式)。

#include "scene.h"

#include <QCursor>
#include <QFileInfo>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLVersionFunctionsFactory>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gl_backend.h"

namespace live2tee {

namespace {

constexpr float TEE_SIZE = 256.0f; // Tee 直径(像素)

// 4K 协议-7 图集的精灵区域配置(8x4 个 512 tile 网格,UV 归一化,
// 与皮肤文件实际分辨率无关 —— 任何标准 7 模板皮肤都能用)。
void ConfigureSpriteRegions(teer::CTeeRenderer& renderer)
{
	const float TW = 4096.0f, TH = 2048.0f;
	using teer::SSpriteRegion;
	renderer.SetSpriteRegion(teer::TEE_SPRITE_BODY,          SSpriteRegion(0.0f / TW,          0.0f / TH, 1536.0f / TW, 1536.0f / TH, 1536, 1536));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_BODY_OUTLINE,  SSpriteRegion(1536.0f / TW,       0.0f / TH, 3072.0f / TW, 1536.0f / TH, 1536, 1536));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_FOOT,          SSpriteRegion(3072.0f / TW,     512.0f / TH, 4096.0f / TW, 1024.0f / TH, 1024, 512));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_FOOT_OUTLINE,  SSpriteRegion(3072.0f / TW,    1024.0f / TH, 4096.0f / TW, 1536.0f / TH, 1024, 512));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_EYES_NORMAL,   SSpriteRegion(1024.0f / TW,    1536.0f / TH, 1536.0f / TW, 2048.0f / TH, 512, 512));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_EYES_ANGRY,    SSpriteRegion(1536.0f / TW,    1536.0f / TH, 2048.0f / TW, 2048.0f / TH, 512, 512));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_EYES_PAIN,     SSpriteRegion(2048.0f / TW,    1536.0f / TH, 2560.0f / TW, 2048.0f / TH, 512, 512));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_EYES_HAPPY,    SSpriteRegion(2560.0f / TW,    1536.0f / TH, 3072.0f / TW, 2048.0f / TH, 512, 512));
	renderer.SetSpriteRegion(teer::TEE_SPRITE_EYES_SURPRISE, SSpriteRegion(3584.0f / TW,    1536.0f / TH, 4096.0f / TW, 2048.0f / TH, 512, 512));
}

void BuildSkinInfo(teer::STeeRenderInfo& info, std::uint32_t skin_gl_id, float tee_size)
{
	info.Reset();
	info.m_Size = tee_size;
	info.m_GotAirJump = true;

	teer::SSixupSkin& six = info.m_aSixup[0];
	six.Reset();
	six.m_aOriginalTextures[teer::SKINPART_BODY] = teer::STextureHandle(skin_gl_id);
	six.m_aOriginalTextures[teer::SKINPART_EYES] = teer::STextureHandle(skin_gl_id);
	six.m_aOriginalTextures[teer::SKINPART_FEET] = teer::STextureHandle(skin_gl_id);
	info.m_Skin6EyePair = true;
	info.m_Skin6EyeSeparationScale = 1.0f;

	six.m_aColors[teer::SKINPART_BODY] = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	six.m_aColors[teer::SKINPART_EYES] = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	six.m_aColors[teer::SKINPART_FEET] = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
}

// ---- 武器/钩爪绘制(从旧 main.cpp -> tee_window.cpp 原样移植) --------------
// game.png 是 32x16 网格,每格 64x64 像素,共 2048x1024。
constexpr int GAME_COLS = 32;
constexpr int GAME_ROWS = 16;
constexpr float GAME_TILE_U = 1.0f / GAME_COLS;
constexpr float GAME_TILE_V = 1.0f / GAME_ROWS;

constexpr float HAMMER_VISUAL_SIZE = 96.0f;
constexpr float HAMMER_OFFSET_X = 4.0f;
constexpr float HAMMER_OFFSET_Y = -20.0f;
constexpr float HOOK_CHAIN_SEG = 12.0f;
constexpr float HOOK_HEAD_W = 14.0f;
constexpr float HOOK_HEAD_H = 10.0f;
constexpr float HOOK_MAX_LEN = 96.0f; // 64 基准尺寸下的最大钩长(实际按 scale 缩放)
constexpr float HOOK_DURATION = 0.30f;

void DrawHammer(GLBackend& backend, std::uint32_t game_tex,
				teer::vec2 tee_pos, const teer::CAnimState* anim, teer::vec2 dir, float scale)
{
	const teer::CAnimKeyframe* attach = anim->GetAttach();
	const bool facing_left = dir.x < 0.0f;

	const float wx = tee_pos.x + (attach->m_X + (facing_left ? -HAMMER_OFFSET_X : HAMMER_OFFSET_X)) * scale;
	const float wy = tee_pos.y + (attach->m_Y + HAMMER_OFFSET_Y) * scale;

	const float pi = 3.14159265358979323846f;
	// 与 DDNet 游戏原版一致的成对公式(游戏里两朝向本就互为镜像):
	//   左向:QuadsSetSubset(0,1,1,0) V 翻转 + rot(-π/2-a)
	//   右向:QuadsSetSubset(0,0,1,1) 原样 + rot(-π/2+a)
	// 注意:右向用 +π/2+a 会整体转 180°(上下颠倒);局部 FlipX 也会颠倒。
	float rot;
	bool flip_v;
	if (facing_left) {
		rot = -pi / 2.0f - attach->m_Angle * pi * 2.0f;
		flip_v = true;
	} else {
		rot = -pi / 2.0f + attach->m_Angle * pi * 2.0f;
		flip_v = false;
	}

	const float w = HAMMER_VISUAL_SIZE * 0.8f * scale;
	const float h = HAMMER_VISUAL_SIZE * 0.6f * scale;

	teer::STeeQuad q;
	q.m_Texture = teer::STextureHandle(game_tex);
	q.m_Position = teer::vec2(wx, wy);
	q.m_Width = w;
	q.m_Height = h;
	q.m_Rotation = rot;
	q.m_Color = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	q.m_U0 = 2.0f * GAME_TILE_U;
	q.m_V0 = 1.0f * GAME_TILE_V;
	q.m_U1 = (2.0f + 4.0f) * GAME_TILE_U;
	q.m_V1 = (1.0f + 3.0f) * GAME_TILE_V;
	if (flip_v) {
		// 垂直翻转 V(QuadsSetSubset(0,1,1,0)),不是水平镜像。
		const float v_tmp = q.m_V0;
		q.m_V0 = q.m_V1;
		q.m_V1 = v_tmp;
	}
	q.m_FlipX = false;
	backend.DrawQuad(q);
}

void DrawHook(GLBackend& backend, std::uint32_t game_tex,
			  teer::vec2 tee_pos, teer::vec2 dir, float elapsed, float scale)
{
	float progress = elapsed / HOOK_DURATION;
	if (progress > 1.0f) progress = 1.0f;

	float extend_factor = (progress < 0.5f) ? (progress * 2.0f) : ((1.0f - progress) * 2.0f);
	const float chain_len = HOOK_MAX_LEN * scale * extend_factor;

	float dx = dir.x, dy = dir.y;
	const float dlen = std::sqrt(dx * dx + dy * dy);
	if (dlen < 0.001f) { dx = 1.0f; dy = 0.0f; }
	else { dx /= dlen; dy /= dlen; }
	const teer::vec2 dir_n = teer::vec2(dx, dy);
	const float chain_angle = std::atan2(dy, dx);

	const float seg_len = HOOK_CHAIN_SEG * scale;
	const float seg_w = seg_len * (16.0f / 24.0f);
	const float step = seg_len;
	const int num_seg = std::max(1, static_cast<int>(chain_len / step));

	teer::STeeQuad chain_q;
	chain_q.m_Texture = teer::STextureHandle(game_tex);
	chain_q.m_Width = seg_len;
	chain_q.m_Height = seg_w;
	chain_q.m_Rotation = chain_angle;
	chain_q.m_Color = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	chain_q.m_U0 = 2.0f * GAME_TILE_U;
	chain_q.m_V0 = 0.0f;
	chain_q.m_U1 = 3.0f * GAME_TILE_U;
	chain_q.m_V1 = 1.0f * GAME_TILE_V;
	chain_q.m_FlipX = false;

	for (int i = 0; i < num_seg; ++i) {
		const float dist = (i + 0.5f) * step;
		if (dist > chain_len)
			break;
		chain_q.m_Position = teer::vec2(
			tee_pos.x + dir_n.x * dist,
			tee_pos.y + dir_n.y * dist);
		backend.DrawQuad(chain_q);
	}

	const teer::vec2 hook_pos = teer::vec2(
		tee_pos.x + dir_n.x * chain_len,
		tee_pos.y + dir_n.y * chain_len);

	teer::STeeQuad head;
	head.m_Texture = teer::STextureHandle(game_tex);
	head.m_Position = hook_pos;
	head.m_Width = HOOK_HEAD_W * scale;
	head.m_Height = HOOK_HEAD_H * scale;
	head.m_Rotation = chain_angle;
	head.m_Color = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	head.m_U0 = 3.0f * GAME_TILE_U;
	head.m_V0 = 0.0f;
	head.m_U1 = 5.0f * GAME_TILE_U;
	head.m_V1 = 1.0f * GAME_TILE_V;
	head.m_FlipX = false;
	backend.DrawQuad(head);
}

// Tee 的手(DDnet RenderHand + CalculateHandPosition/CalculateHandAngle)。
void DrawHand(GLBackend& backend, std::uint32_t skin_tex,
			  teer::vec2 tee_pos, teer::vec2 dir, float scale)
{
	const float pi = 3.14159265358979323846f;
	float dx = dir.x, dy = dir.y;
	const float dlen = std::sqrt(dx * dx + dy * dy);
	if (dlen < 0.001f) { dx = 1.0f; dy = 0.0f; }
	else { dx /= dlen; dy /= dlen; }
	const teer::vec2 dir_n = teer::vec2(dx, dy);

	const teer::vec2 hand_pos = tee_pos + dir_n * (1.0f + 20.0f) * scale;

	const float ang = std::atan2(dy, dx);
	const float hand_angle = (dir_n.x < 0.0f) ? (ang + pi / 2.0f) : (ang - pi / 2.0f);

	const float size = 20.0f * scale;

	// 4K 皮肤图集:手 outline tile(7,0),手 base tile(6,0)
	constexpr float SKIN_TW = 4096.0f;
	constexpr float SKIN_TH = 2048.0f;

	teer::STeeQuad q;
	q.m_Texture = teer::STextureHandle(skin_tex);
	q.m_Position = hand_pos;
	q.m_Width = size;
	q.m_Height = size;
	q.m_Rotation = hand_angle;
	q.m_Color = teer::ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	q.m_FlipX = false;

	// 先 outline 再 base(DDnet RenderHand7 顺序)
	q.m_U0 = 3584.0f / SKIN_TW;
	q.m_V0 = 0.0f;
	q.m_U1 = 4096.0f / SKIN_TW;
	q.m_V1 = 512.0f / SKIN_TH;
	backend.DrawQuad(q);

	q.m_U0 = 3072.0f / SKIN_TW;
	q.m_V0 = 0.0f;
	q.m_U1 = 3584.0f / SKIN_TW;
	q.m_V1 = 512.0f / SKIN_TH;
	backend.DrawQuad(q);
}

} // namespace

// ---------------------------------------------------------------------------
// TeeScene
// ---------------------------------------------------------------------------
float TeeScene::TeeSize() const
{
	return TEE_SIZE * cfg_.render_scale;
}

TeeScene::TeeScene(const AppConfig& cfg, InputQueue* queue)
	: cfg_(cfg)
	, queue_(queue)
	, behavior_(std::make_unique<BuiltinBehavior>())
	, t0_(std::chrono::steady_clock::now())
{
}

TeeScene::~TeeScene() = default;

bool TeeScene::Init(QOpenGLFunctions_2_1* f)
{
	f_ = f;
	if (!f_)
		return false;

	backend_ = std::make_unique<GLBackend>(f_);
	renderer_ = std::make_unique<teer::CTeeRenderer>(backend_.get());
	ConfigureSpriteRegions(*renderer_);

	ReloadTextures();
	return true;
}

void TeeScene::CalibrateMouseOffset()
{
	has_custom_origin_ = false; // 回到屏幕中心模式
	int off_x = 0, off_y = 0;
	if (QueryMouseOffsetFromScreenCenter(off_x, off_y)) {
		state_.SetAimOffset(static_cast<float>(off_x),
							static_cast<float>(off_y));
	} else {
		state_.ResetMouseOffset();
	}
}

void TeeScene::SetMouseOrigin(const QPoint& origin)
{
	// 全程使用 Qt 逻辑像素:QCursor::pos() 与传入的 origin 同属 Qt
	// 屏幕逻辑坐标,高 DPI 缩放一致,无需手动换算物理像素。
	// 把虚拟偏移锚定为"光标 - 原点",使 Tee 朝向 = 光标相对 origin 的方位。
	const QPoint cursor = QCursor::pos();
	state_.SetAimOffset(static_cast<float>(cursor.x() - origin.x()),
						static_cast<float>(cursor.y() - origin.y()));
	// 记住原点,供周期重锚定(ReAnchorMouse)沿用同一参考系
	has_custom_origin_ = true;
	custom_origin_ = origin;
}

void TeeScene::ReAnchorMouse()
{
	const QPoint cursor = QCursor::pos();
	// XWayland 下光标位于原生 Wayland 窗口时,光标查询结果冻结在最后离开
	// X11 窗口的位置。位置不变 = 光标静止(无漂移)或查询冻结(不可信),
	// 两种情形都跳过锚定,避免把积分误差"校正"到过期位置。
	if (cursor == last_reanchor_cursor_)
		return;
	last_reanchor_cursor_ = cursor;

	if (has_custom_origin_) {
		state_.SetAimOffset(static_cast<float>(cursor.x() - custom_origin_.x()),
							static_cast<float>(cursor.y() - custom_origin_.y()));
	} else {
		int off_x = 0, off_y = 0;
		if (QueryMouseOffsetFromScreenCenter(off_x, off_y)) {
			state_.SetAimOffset(static_cast<float>(off_x),
								static_cast<float>(off_y));
		}
	}
}

void TeeScene::ReloadTextures()
{
	// 需在 makeCurrent 后调用(Init / ApplyConfig 已保证)
	if (!f_)
		return;

	DeleteTexture(tex_skin_, f_);
	DeleteTexture(tex_emo_, f_);
	DeleteTexture(tex_weapon_, f_);

	// 皮肤:cfg.skin 缺失/失效时,回退到 skins 目录第一个 png
	QString skin_path = cfg_.SkinPath();
	if (skin_path.isEmpty() || !QFileInfo::exists(skin_path)) {
		const QStringList skins = ScanSkinFiles(cfg_.ResolvedSkinsDir());
		if (!skins.isEmpty())
			skin_path = cfg_.ResolvedSkinsDir() + QStringLiteral("/") + skins.first();
	}
	if (!LoadPng(skin_path.toStdString(), tex_skin_, f_, true))
		std::fprintf(stderr, "skin load failed: %s\n", skin_path.toUtf8().constData());
	if (!LoadPng((cfg_.ResolvedAssetsDir() + QStringLiteral("/emoticons.png")).toStdString(), tex_emo_, f_, true))
		std::fprintf(stderr, "emoticons.png load failed\n");
	if (!LoadPng((cfg_.ResolvedAssetsDir() + QStringLiteral("/game.png")).toStdString(), tex_weapon_, f_, true))
		std::fprintf(stderr, "game.png load failed\n");

	BuildSkinInfo(info_, tex_skin_.gl_id, TeeSize());

	// CEmoticonRenderer 的贴图句柄不可变,直接重建
	emo_ = std::make_unique<teer::CEmoticonRenderer>(
		backend_.get(), teer::STextureHandle(tex_emo_.gl_id));
	emo_->ConfigureEmoticonGrid(4, 4);

	textures_ready_ = tex_skin_.Valid() && tex_emo_.Valid() && tex_weapon_.Valid();
}

void TeeScene::ApplyConfig(const AppConfig& cfg)
{
	// 必须在覆盖 cfg_ 之前比较旧配置,否则换肤不会触发纹理重载
	const bool reload = (cfg.ResolvedAssetsDir() != cfg_.ResolvedAssetsDir()) ||
						(cfg.ResolvedSkinsDir() != cfg_.ResolvedSkinsDir()) ||
						(cfg.skin != cfg_.skin);
	cfg_ = cfg;
	// render_scale 可能单独变化,皮肤信息(尺寸)需要重建
	BuildSkinInfo(info_, tex_skin_.gl_id, TeeSize());
	if (reload)
		ReloadTextures(); // 需调用方已 makeCurrent
}

bool TeeScene::ReloadSkinTexture(const QString& skin_path)
{
	// 仅替换皮肤(供皮肤选择窗口逐个生成缩略图),
	// game.png / emoticons.png 保持不变,避免每个皮肤重复解码上传。
	if (!f_)
		return false;
	Texture next;
	if (!LoadPng(skin_path.toStdString(), next, f_, true)) {
		std::fprintf(stderr, "skin load failed: %s\n", skin_path.toUtf8().constData());
		return false;
	}
	DeleteTexture(tex_skin_, f_);
	tex_skin_ = next;
	BuildSkinInfo(info_, tex_skin_.gl_id, TeeSize());
	textures_ready_ = tex_skin_.Valid() && tex_emo_.Valid() && tex_weapon_.Valid();
	return true;
}

void TeeScene::TriggerEmoticon(int id)
{
	if (!queue_)
		return; // 无输入队列的场景无法对齐内部时间基准,忽略
	InputEvent ev;
	ev.kind = EInputKind::Emoticon;
	ev.emoticon_id = id; // <0/越界由 TeeState::ShowEmoticon 随机
	queue_->Push(ev);
}

void TeeScene::SetBehavior(std::unique_ptr<IBehavior> behavior)
{
	behavior_ = behavior ? std::move(behavior)
	                     : std::make_unique<BuiltinBehavior>();
	behavior_ctx_.Reset(); // 新旧映射器不共享按住状态,避免沿用旧状态误判
}

void TeeScene::PumpInput()
{
	const float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0_).count();

	// 事件 -> 行为映射器 -> 动作原语。先更新上下文(按住时刻/空闲计时),
	// 再让映射器决策;映射器可被 SetBehavior 替换为 Lua 等实现。
	InputEvent ev;
	bool mouse_moved = false;
	if (queue_) {
		while (queue_->Pop(ev)) {
			behavior_ctx_.NoteEvent(ev, now);
			behavior_->OnEvent(state_, behavior_ctx_, ev, now);
			mouse_moved |= (ev.kind == EInputKind::MouseMove);
		}
	}
	// 自定义原点模式下,"朝向 = 光标 - 原点"是绝对关系;行为映射器对
	// MouseMove 只做相对累加(add_aim),积分漂移会把原点冲掉。
	// 移动事件处理后立刻按真实光标重锚定,保证原点持续生效。
	// (ReAnchorMouse 内部有新鲜度检测,光标查询冻结时自动跳过。)
	if (mouse_moved && has_custom_origin_)
		ReAnchorMouse();
	behavior_->OnTick(state_, behavior_ctx_, now);
	state_.Tick(now);

	// libinput 指针加速使原始位移积分系统性偏离真实光标(大幅快速移动时
	// 光标走得比原始积分远),周期性用真实光标位置重锚定压制漂移。
	// ReAnchorMouse 内部有新鲜度检测,光标查询冻结时自动跳过。
	// 是否启用及间隔由配置控制(Linux 默认开启 200ms,Windows 默认关闭)。
	if (cfg_.auto_reanchor &&
		now - last_reanchor_time_ >= cfg_.reanchor_interval_ms / 1000.0f) {
		last_reanchor_time_ = now;
		ReAnchorMouse();
	}

	// 偏移钳制已收敛到 TeeState::SetAimOffset/AddAimOffset 内部。
}

void TeeScene::Render(int w, int h)
{
	const float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0_).count();

	PumpInput();

	f_->glViewport(0, 0, w, h);
	f_->glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // 透明黑;绿幕底色由预览窗口侧填
	f_->glClear(GL_COLOR_BUFFER_BIT);

	if (!textures_ready_)
		return;

	f_->glEnable(GL_TEXTURE_2D);
	f_->glEnable(GL_BLEND);
	// 预乘 alpha 混合:纹理上传时已把 RGB 乘以 A(LoadPng premultiply),
	// GL_ONE / (1-SRC_ALPHA) 才是 "over" 算子的正确公式。
	// 旧的 SRC_ALPHA 混合在透明底 FBO 里会把边缘 alpha 平方、半透明
	// 叠加处出现暗边 —— 桌面窗口看不出来,直通 alpha 输出会露馅。
	f_->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	f_->glDisable(GL_DEPTH_TEST);

	f_->glMatrixMode(GL_PROJECTION);
	f_->glLoadIdentity();
	f_->glOrtho(0.0, static_cast<GLdouble>(w), static_cast<GLdouble>(h), 0.0, -1.0, 1.0); // 像素坐标,y 向下
	f_->glMatrixMode(GL_MODELVIEW);
	f_->glLoadIdentity();

	// Tee 居中
	const float cx = w * 0.5f;
	const float cy = h * 0.5f;
	const teer::vec2 dir = state_.ComputeDirection();
	const teer::EEmote emote = state_.GetEmote();
	const teer::CAnimState* anim = state_.GetAnimState(now);

	// 锤子/钩爪都画在 Tee 之前(DDnet:武器与钩链、钩手都在身体后面)
	const float tee_size = TeeSize();
	const float tee_scale = tee_size / 64.0f;
	if (state_.action == EAction::Hammer) {
		DrawHammer(*backend_, tex_weapon_.gl_id, teer::vec2(cx, cy), anim, dir, tee_scale);
	}
	if (state_.action == EAction::Hook) {
		const float hook_elapsed = now - state_.action_start_time;
		DrawHook(*backend_, tex_weapon_.gl_id, teer::vec2(cx, cy), dir, hook_elapsed, tee_scale);
		// 出钩时显示 Tee 的手(DDnet 出钩会画手)
		DrawHand(*backend_, tex_skin_.gl_id, teer::vec2(cx, cy), dir, tee_scale);
	}

	renderer_->RenderTee(anim, &info_, emote, dir, teer::vec2(cx, cy));

	// 头顶表情气泡(独立生命周期,可与 Hammer/Hook 动作共存)
	if (state_.EmoteActive(now)) {
		const float elapsed = now - state_.emoticon_start_time;
		emo_->SetTeeSize(tee_size); // 跟随渲染缩放
		emo_->RenderEmoticon(teer::vec2(cx, cy), state_.emoticon_id, elapsed, 1.0f);
	}
}

} // namespace live2tee
