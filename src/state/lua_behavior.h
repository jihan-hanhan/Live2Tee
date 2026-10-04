#pragma once

// Lua 行为映射器:加载并运行 assets/scripts 下的一个或多个 Lua 脚本,
// 把输入事件分发给脚本回调,脚本通过注册的全局原语函数控制
// Tee(play_hammer/show_emoticon/set_aim...)。
//
// 多脚本:Load 按路径列表顺序(=优先级,从高到低)依次加载;同一回调被
// 多个脚本定义时,首个(最高优先级)定义生效,其余被忽略。脚本未定义的
// 回调槽位回退到内置默认行为(BuiltinBehavior)——脚本只覆写需要的事件
// 类型,其余输入(如鼠标移动朝向跟随)保持默认。每个脚本在
// 独立的 _ENV 沙盒表中执行(__index=_G 读原语/KEY),顶层定义互不污染,
// 脚本内 upvalue/沙盒内状态在回调间持续有效。
//
// 安全:独立 lua_State,只打开 base/string/table/math/utf8/coroutine 库,
// 不加载 os/io/debug/package,脚本无法执行外部程序或读写文件。
// 容错:空列表/全部文件缺失或失败 = "未配置"(返回 nullptr,沿用内置行为);
// 单个脚本编译/初始化失败跳过该脚本(err 记录),不影响其余脚本;
// 单个回调运行时报错只停用该回调,不影响其他与渲染。

#include <QString>
#include <QStringList>

#include <memory>
#include <string>

#include "behavior.h"

struct lua_State;

namespace live2tee {

class LuaBehavior : public IBehavior {
public:
	// 加载脚本列表(完整路径,顺序=优先级从高到低):
	//   空列表 / 全部缺失或加载失败 -> 返回 nullptr(沿用内置行为);
	//   单个脚本失败 -> 跳过,err 追加 "路径: 错误" 每行一条。
	static std::unique_ptr<LuaBehavior> Load(const QStringList& paths, std::string* err = nullptr);
	~LuaBehavior() override;

	void OnEvent(TeeState& state, BehaviorContext& ctx,
				 const InputEvent& ev, float now) override;
	void OnTick(TeeState& state, BehaviorContext& ctx, float now) override;
	bool OwnsMouseMotion() const override; // 脚本定义了 on_mouse_move = true

private:
	LuaBehavior() = default;

	// 注册全部沙箱库与 Tee 原语/KEY 常量表
	void RegisterApi();
	// 调用槽位上的回调(nargs 个参数已压栈);
	// 槽位为空则弹掉参数;运行报错则打印并永久停用该槽位。
	void Dispatch(int slot, const char* fn_name, int nargs);

	// 每次 Lua 调用前把 state/ctx/now 发布到 registry 供 C 原语取回
	void PublishContext(TeeState& state, BehaviorContext& ctx, float now);

	lua_State* L_ = nullptr;

	// 脚本未覆写的回调槽位回退到内置默认行为(见 OnEvent)
	std::unique_ptr<IBehavior> fallback_;

	// 回调槽序号
	enum { CB_MOVE = 0, CB_LBTN, CB_RBTN, CB_KEY, CB_TICK, CB_COUNT };
	// 每槽位一个 registry ref(LUA_NOREF = 无定义/已停用);
	// 槽位被最高优先级提供该回调的脚本占据
	int cb_refs_[CB_COUNT] = {};
	std::string cb_owner_[CB_COUNT]; // 提供该回调的脚本路径(报错日志用)
};

} // namespace live2tee
