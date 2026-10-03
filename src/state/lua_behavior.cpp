// Lua 行为映射器实现:沙箱 VM、Tee 原语绑定、事件回调分发。
//
// 暴露给脚本的全局 API(详见 assets/scripts/behavior.example.lua):
//   动作: play_hammer() play_hook() show_emoticon([id]) clear_emoticon()
//   朝向: add_aim(dx,dy) set_aim(x,y) set_aim_dir(x,y) reset_aim()
//         aim_x() aim_y()
//   查询: action() action_elapsed() emoticon_id() emoticon_elapsed()
//         emote_active() now()
//   输入: key_down(vk) key_held_ms(vk) mouse_left_down() mouse_left_held_ms()
//         mouse_right_down() mouse_right_held_ms() idle_ms()
//   其他: random_emoticon()、KEY 常量表(KEY.F1 / KEY.SPACE ...)
//
// 事件回调(全部可选,脚本只定义需要的):
//   on_mouse_move(dx, dy)  on_mouse_left(pressed)  on_mouse_right(pressed)
//   on_key(vk, pressed)    on_tick(now)

#include "lua_behavior.h"

#include "state.h"

#include <QFile>

#include <array>
#include <chrono>
#include <cstdio>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace live2tee {

namespace {

// 每次回调期间发布到 Lua registry 的调用上下文(指向栈对象,不持有)
constexpr char kCtxRegistryKey = 0;
struct ScriptCtx {
	TeeState* state = nullptr;
	BehaviorContext* bctx = nullptr;
	float now = 0.0f;
};

ScriptCtx* GetCtx(lua_State* L)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX,
				const_cast<void*>(static_cast<const void*>(&kCtxRegistryKey)));
	auto* ctx = static_cast<ScriptCtx*>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return ctx;
}

// ---- 动作原语 ----

int fn_play_hammer(lua_State* L)
{
	auto* c = GetCtx(L);
	c->state->PlayHammer(c->now);
	return 0;
}

int fn_play_hook(lua_State* L)
{
	auto* c = GetCtx(L);
	c->state->PlayHook(c->now);
	return 0;
}

int fn_show_emoticon(lua_State* L)
{
	auto* c = GetCtx(L);
	const int id = static_cast<int>(luaL_optinteger(L, 1, -1)); // 缺省 = 随机
	c->state->ShowEmoticon(id, c->now);
	return 0;
}

int fn_clear_emoticon(lua_State* L)
{
	GetCtx(L)->state->ClearEmoticon();
	return 0;
}

// ---- 朝向原语 ----

int fn_add_aim(lua_State* L)
{
	auto* c = GetCtx(L);
	c->state->AddAimOffset(static_cast<float>(luaL_checknumber(L, 1)),
						   static_cast<float>(luaL_checknumber(L, 2)));
	return 0;
}

int fn_set_aim(lua_State* L)
{
	auto* c = GetCtx(L);
	c->state->SetAimOffset(static_cast<float>(luaL_checknumber(L, 1)),
						   static_cast<float>(luaL_checknumber(L, 2)));
	return 0;
}

int fn_set_aim_dir(lua_State* L)
{
	auto* c = GetCtx(L);
	c->state->SetAimDirection(static_cast<float>(luaL_checknumber(L, 1)),
							  static_cast<float>(luaL_checknumber(L, 2)));
	return 0;
}

int fn_reset_aim(lua_State* L)
{
	GetCtx(L)->state->ResetMouseOffset();
	return 0;
}

int fn_aim_x(lua_State* L)
{
	lua_pushnumber(L, GetCtx(L)->state->mouse_off_x);
	return 1;
}

int fn_aim_y(lua_State* L)
{
	lua_pushnumber(L, GetCtx(L)->state->mouse_off_y);
	return 1;
}

// ---- 状态查询 ----

int fn_action(lua_State* L)
{
	lua_pushinteger(L, static_cast<lua_Integer>(GetCtx(L)->state->GetAction()));
	return 1; // 0=idle 1=hammer 2=hook
}

int fn_action_elapsed(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushnumber(L, c->state->ActionElapsed(c->now));
	return 1;
}

int fn_emoticon_id(lua_State* L)
{
	lua_pushinteger(L, GetCtx(L)->state->GetEmoticonId());
	return 1; // -1 = 无气泡
}

int fn_emoticon_elapsed(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushnumber(L, c->state->EmoticonElapsed(c->now));
	return 1;
}

int fn_emote_active(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushboolean(L, c->state->EmoteActive(c->now));
	return 1;
}

int fn_now(lua_State* L)
{
	lua_pushnumber(L, GetCtx(L)->now);
	return 1;
}

// ---- 输入上下文查询 ----

int fn_key_down(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushboolean(L, c->bctx->IsKeyDown(static_cast<int>(luaL_checkinteger(L, 1))));
	return 1;
}

int fn_key_held_ms(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushnumber(L, c->bctx->KeyHeldMs(
		static_cast<int>(luaL_checkinteger(L, 1)), c->now));
	return 1;
}

int fn_mouse_left_down(lua_State* L)
{
	lua_pushboolean(L, GetCtx(L)->bctx->IsMouseLeftDown());
	return 1;
}

int fn_mouse_left_held_ms(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushnumber(L, c->bctx->MouseLeftHeldMs(c->now));
	return 1;
}

int fn_mouse_right_down(lua_State* L)
{
	lua_pushboolean(L, GetCtx(L)->bctx->IsMouseRightDown());
	return 1;
}

int fn_mouse_right_held_ms(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushnumber(L, c->bctx->MouseRightHeldMs(c->now));
	return 1;
}

int fn_idle_ms(lua_State* L)
{
	auto* c = GetCtx(L);
	lua_pushnumber(L, c->bctx->IdleMs(c->now));
	return 1;
}

int fn_random_emoticon(lua_State* L)
{
	lua_pushinteger(L, RandomEmoticonId());
	return 1;
}

const luaL_Reg kPrimitiveFns[] = {
	{"play_hammer", fn_play_hammer},
	{"play_hook", fn_play_hook},
	{"show_emoticon", fn_show_emoticon},
	{"clear_emoticon", fn_clear_emoticon},
	{"add_aim", fn_add_aim},
	{"set_aim", fn_set_aim},
	{"set_aim_dir", fn_set_aim_dir},
	{"reset_aim", fn_reset_aim},
	{"aim_x", fn_aim_x},
	{"aim_y", fn_aim_y},
	{"action", fn_action},
	{"action_elapsed", fn_action_elapsed},
	{"emoticon_id", fn_emoticon_id},
	{"emoticon_elapsed", fn_emoticon_elapsed},
	{"emote_active", fn_emote_active},
	{"now", fn_now},
	{"key_down", fn_key_down},
	{"key_held_ms", fn_key_held_ms},
	{"mouse_left_down", fn_mouse_left_down},
	{"mouse_left_held_ms", fn_mouse_left_held_ms},
	{"mouse_right_down", fn_mouse_right_down},
	{"mouse_right_held_ms", fn_mouse_right_held_ms},
	{"idle_ms", fn_idle_ms},
	{"random_emoticon", fn_random_emoticon},
	{nullptr, nullptr}
};

// 向 KEY 表写入一个整数常量
void SetKeyConst(lua_State* L, const char* name, int vk)
{
	lua_pushinteger(L, vk);
	lua_setfield(L, -2, name);
}

// 构造 KEY 常量表(VK 码,与各平台输入后端翻译结果一致)
void RegisterKeyTable(lua_State* L)
{
	lua_newtable(L);
	SetKeyConst(L, "SPACE", 0x20);
	SetKeyConst(L, "ENTER", 0x0D);
	SetKeyConst(L, "ESC", 0x1B);
	SetKeyConst(L, "TAB", 0x09);
	SetKeyConst(L, "BACKSPACE", 0x08);
	SetKeyConst(L, "SHIFT", 0x10);
	SetKeyConst(L, "CTRL", 0x11);
	SetKeyConst(L, "ALT", 0x12);
	SetKeyConst(L, "LEFT", 0x25);
	SetKeyConst(L, "UP", 0x26);
	SetKeyConst(L, "RIGHT", 0x27);
	SetKeyConst(L, "DOWN", 0x28);
	for (int i = 0; i < 10; ++i) {
		char name[2];
		name[0] = static_cast<char>('0' + i);
		name[1] = '\0';
		SetKeyConst(L, name, '0' + i);
	}
	for (char c = 'A'; c <= 'Z'; ++c) {
		char name[2] = {c, '\0'};
		SetKeyConst(L, name, c);
	}
	for (int i = 1; i <= 12; ++i) {
		char name[4];
		std::snprintf(name, sizeof(name), "F%d", i);
		SetKeyConst(L, name, 0x70 + (i - 1));
	}
	lua_setglobal(L, "KEY");
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期 / 加载
// ---------------------------------------------------------------------------

std::unique_ptr<LuaBehavior> LuaBehavior::Load(const QStringList& paths, std::string* err)
{
	if (paths.isEmpty())
		return nullptr; // 空列表 = 用户主动清空,沿用内置默认行为

	auto beh = std::unique_ptr<LuaBehavior>(new LuaBehavior());
	beh->L_ = luaL_newstate();
	if (!beh->L_) {
		if (err)
			*err = "luaL_newstate 失败(内存不足)";
		return nullptr;
	}
	beh->RegisterApi();
	beh->fallback_ = std::make_unique<BuiltinBehavior>();
	for (int& ref : beh->cb_refs_)
		ref = LUA_NOREF;

	// 回调名表(与槽位序号一致)
	static const std::array<const char*, CB_COUNT> kCbNames = {
		"on_mouse_move", "on_mouse_left", "on_mouse_right", "on_key", "on_tick"
	};

	int loaded = 0;
	// 按优先级从高到低逐个加载;首个提供某回调的脚本占据该槽位
	for (const QString& path : paths) {
		QFile file(path);
		if (!file.exists())
			continue; // 文件缺失 = 未配置,静默跳过(与单脚本时代语义一致)
		if (!file.open(QIODevice::ReadOnly)) {
			if (err)
				*err += path.toUtf8().toStdString() + ": 无法打开,已跳过\n";
			continue;
		}
		const QByteArray bytes = file.readAll();
		const std::string name = path.toUtf8().toStdString();

		lua_State* L = beh->L_;
		// chunk 名带 @ 前缀,Lua 报错时显示文件路径
		const QByteArray chunk_name = QByteArray("@") + path.toUtf8();
		if (luaL_loadbuffer(L, bytes.constData(),
							static_cast<size_t>(bytes.size()),
							chunk_name.constData()) != LUA_OK) {
			if (err) {
				const char* msg = lua_tostring(L, -1);
				*err += name + ": " + (msg ? msg : "编译失败") + "\n";
			}
			lua_pop(L, 1); // 错误消息
			continue;
		}
		// 栈:[chunk]
		// 每脚本独立 _ENV 沙盒表:metatable.__index=_G 让脚本读原语/KEY,
		// 顶层 function 定义全部落进该表,脚本之间互不污染全局环境
		lua_newtable(L);                // [chunk, env]
		lua_newtable(L);                // [chunk, env, mt]
		lua_pushglobaltable(L);         // [chunk, env, mt, _G]
		lua_setfield(L, -2, "__index"); // [chunk, env, mt]
		lua_setmetatable(L, -2);        // [chunk, env]
		lua_pushvalue(L, -1);           // [chunk, env, env]
		lua_setupvalue(L, -3, 1);       // chunk._ENV = env -> [chunk, env]
		lua_pushvalue(L, -2);           // [chunk, env, chunk]
		if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			// 栈:[chunk, env, errmsg]
			if (err) {
				const char* msg = lua_tostring(L, -1);
				*err += name + ": " + (msg ? msg : "初始化失败") + "\n";
			}
			lua_pop(L, 3); // errmsg, env, chunk
			continue;
		}
		// 栈:[chunk, env];从 env 提取回调到 registry。
		// 函数持有 _ENV 上值,env 表随函数存活,脚本状态在回调间持续有效
		for (int i = 0; i < CB_COUNT; ++i) {
			lua_getfield(L, -1, kCbNames[i]); // [chunk, env, fn]
			if (lua_isfunction(L, -1) && beh->cb_refs_[i] == LUA_NOREF) {
				beh->cb_refs_[i] = luaL_ref(L, LUA_REGISTRYINDEX); // 弹出并持有
				beh->cb_owner_[i] = name;
			} else {
				lua_pop(L, 1); // 未定义/低优先级重复定义:忽略
			}
		}
		lua_pop(L, 2); // env, chunk
		++loaded;
	}

	if (loaded == 0)
		return nullptr; // 全部缺失/失败:沿用内置(析构关闭 state)
	return beh;
}

LuaBehavior::~LuaBehavior()
{
	if (L_) {
		for (int i = 0; i < CB_COUNT; ++i) {
			if (cb_refs_[i] != LUA_NOREF)
				luaL_unref(L_, LUA_REGISTRYINDEX, cb_refs_[i]);
		}
		lua_close(L_);
	}
}

void LuaBehavior::RegisterApi()
{
	lua_State* L = L_;

	// 沙箱:只开无副作用的标准库;os/io/debug/package 不加载,
	// 脚本无法执行进程、读写文件、加载原生模块或调试绕过。
	static const luaL_Reg kSafeLibs[] = {
		{LUA_GNAME, luaopen_base},
		{LUA_COLIBNAME, luaopen_coroutine},
		{LUA_TABLIBNAME, luaopen_table},
		{LUA_STRLIBNAME, luaopen_string},
		{LUA_MATHLIBNAME, luaopen_math},
		{LUA_UTF8LIBNAME, luaopen_utf8},
		{nullptr, nullptr}
	};
	for (const luaL_Reg& lib : kSafeLibs) {
		if (!lib.name)
			break; // luaL_requiref 不检测哨兵,遇 {nullptr,nullptr} 必须停
		luaL_requiref(L, lib.name, lib.func, 1);
		lua_pop(L, 1);
	}
	// base 中唯一能碰文件系统的两个函数移除(print 保留,输出到 stdout 便于调试)
	lua_pushnil(L);
	lua_setglobal(L, "dofile");
	lua_pushnil(L);
	lua_setglobal(L, "loadfile");

	// 将 Tee 原语注册到全局表(luaL_setfuncs 要求栈顶为目标表)
	lua_pushglobaltable(L);
	luaL_setfuncs(L, kPrimitiveFns, 0);
	lua_pop(L, 1);

	RegisterKeyTable(L);

	// 播种 math.random(脚本做随机表情/抖动时用)
	const auto seed = static_cast<lua_Integer>(
		std::chrono::steady_clock::now().time_since_epoch().count());
	lua_getglobal(L, "math");
	lua_getfield(L, -1, "randomseed");
	lua_pushinteger(L, seed);
	lua_call(L, 1, 0);
	lua_pop(L, 1);
}

void LuaBehavior::PublishContext(TeeState& state, BehaviorContext& ctx, float now)
{
	thread_local ScriptCtx script_ctx;
	script_ctx.state = &state;
	script_ctx.bctx = &ctx;
	script_ctx.now = now;
	lua_pushlightuserdata(L_, &script_ctx);
	lua_rawsetp(L_, LUA_REGISTRYINDEX,
				const_cast<void*>(static_cast<const void*>(&kCtxRegistryKey)));
}

void LuaBehavior::Dispatch(int slot, const char* fn_name, int nargs)
{
	if (cb_refs_[slot] == LUA_NOREF) {
		if (nargs > 0)
			lua_pop(L_, nargs); // 调用方压好的参数直接丢弃
		return;
	}
	lua_rawgeti(L_, LUA_REGISTRYINDEX, cb_refs_[slot]);
	lua_insert(L_, -(nargs + 1)); // 栈:[参数...] -> [函数, 参数...]
	if (lua_pcall(L_, nargs, 0, 0) != LUA_OK) {
		const char* msg = lua_tostring(L_, -1);
		// 停用出错回调:鼠标移动/定时器高频触发,不能每帧刷错误日志
		std::fprintf(stderr, "%s: %s() 运行错误,该回调已停用: %s\n",
					 cb_owner_[slot].c_str(), fn_name, msg ? msg : "?");
		luaL_unref(L_, LUA_REGISTRYINDEX, cb_refs_[slot]);
		cb_refs_[slot] = LUA_NOREF;
		lua_pop(L_, 1);
	}
}

void LuaBehavior::OnEvent(TeeState& state, BehaviorContext& ctx,
						  const InputEvent& ev, float now)
{
	PublishContext(state, ctx, now);
	switch (ev.kind) {
	case EInputKind::MouseMove:
		lua_pushnumber(L_, static_cast<lua_Number>(ev.dx));
		lua_pushnumber(L_, static_cast<lua_Number>(ev.dy));
		Dispatch(CB_MOVE, "on_mouse_move", 2);
		break;
	case EInputKind::MouseLeft:
		lua_pushboolean(L_, ev.pressed);
		Dispatch(CB_LBTN, "on_mouse_left", 1);
		break;
	case EInputKind::MouseRight:
		lua_pushboolean(L_, ev.pressed);
		Dispatch(CB_RBTN, "on_mouse_right", 1);
		break;
	case EInputKind::Key:
		lua_pushinteger(L_, ev.keycode);
		lua_pushboolean(L_, ev.pressed);
		Dispatch(CB_KEY, "on_key", 2);
		break;
	case EInputKind::Emoticon:
		// 程序化触发(TriggerEmoticon API/网络指令)始终生效,
		// 不依赖脚本是否定义回调
		state.ShowEmoticon(ev.emoticon_id, now);
		break;
	}
	// 脚本缺失的回调槽位回退到内置默认行为:脚本只覆写它定义的部分,
	// 未定义的事件类型仍按 BuiltinBehavior 处理(如只定义 on_key 时
	// 鼠标移动/左右键仍保持默认的朝向跟随/锤/钩)。
	if (fallback_) {
		switch (ev.kind) {
		case EInputKind::MouseMove:
			if (cb_refs_[CB_MOVE] == LUA_NOREF) fallback_->OnEvent(state, ctx, ev, now);
			break;
		case EInputKind::MouseLeft:
			if (cb_refs_[CB_LBTN] == LUA_NOREF) fallback_->OnEvent(state, ctx, ev, now);
			break;
		case EInputKind::MouseRight:
			if (cb_refs_[CB_RBTN] == LUA_NOREF) fallback_->OnEvent(state, ctx, ev, now);
			break;
		case EInputKind::Key:
			if (cb_refs_[CB_KEY] == LUA_NOREF) fallback_->OnEvent(state, ctx, ev, now);
			break;
		case EInputKind::Emoticon:
			break; // 已在上面处理
		}
	}
}

void LuaBehavior::OnTick(TeeState& state, BehaviorContext& ctx, float now)
{
	PublishContext(state, ctx, now);
	lua_pushnumber(L_, now);
	Dispatch(CB_TICK, "on_tick", 1);
}

} // namespace live2tee
