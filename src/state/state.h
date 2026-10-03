#pragma once

// Tee 状态机:朝向偏移 + 当前动作 + 头顶表情。
// 本类只持有状态并提供"动作原语"(Play*/Show*/朝向设置)与状态查询,
// 不包含任何"输入事件 -> 动作"的映射策略 —— 那部分在 behavior 层
// (BuiltinBehavior / 未来的 LuaBehavior),经 TeeScene::SetBehavior 替换。
//
// 不含 Walk —— 直播场景不需要移动动画。
// 表情气泡是独立生命周期,可与 Hammer/Hook 共存。

#include <tee_anim.h>
#include <tee_emoticon.h>
#include <tee_math.h>

namespace live2tee {

enum class EAction {
	Idle = 0,
	Hammer,
	Hook,
};

struct TeeState {
	// ---- 时长常量(秒),Tick 自动回收;行为层/Lua 可读取做时序编排 ----
	static constexpr float HAMMER_DURATION_SEC = 0.40f;
	static constexpr float HOOK_DURATION_SEC   = 0.30f;
	static constexpr float EMOTE_DURATION_SEC  = 2.00f;

	// 朝向偏移钳制:偏移只取方向,绝对值无意义,防止长时间累加溢出
	static constexpr float AIM_OFFSET_LIMIT = 100000.0f;

	// ---- 状态字段(渲染层直接读;外部修改请走下方原语)----

	// 虚拟鼠标相对 Tee 中心的偏移量。全局鼠标运动增量累加到这里,
	// 不依赖任何平台的屏幕绝对坐标 —— 朝向只取决于偏移方向。
	float mouse_off_x = 0.0f;
	float mouse_off_y = 0.0f;

	EAction action = EAction::Idle;
	float action_start_time = 0.0f;  // 动作开始时刻(秒)

	// 头顶表情气泡(独立生命周期,不占动作位,可与 Hammer/Hook 共存)。
	int emoticon_id = -1;
	float emoticon_start_time = 0.0f;

	// ---- 帧推进:清理超时动作/气泡,每帧渲染前调用 ----
	void Tick(float now);

	// ---- 动作原语(行为映射层的唯一修改入口;now 为场景内部秒)----

	// 播放挥锤/挥钩(从 now 重新计时,重复调用等于重新起手)。
	void PlayHammer(float now);
	void PlayHook(float now);

	// 立即播放指定头顶表情气泡(编号 0..NUM_EMOTICONS-1,对应
	// emoticons.png 4x4 行优先);id < 0 或越界时随机。重置 2s 生命周期,
	// 不经过任何冷却(冷却是映射策略,由行为层自行实现)。
	void ShowEmoticon(int id, float now);
	// 提前关闭气泡。
	void ClearEmoticon();

	// ---- 朝向原语 ----

	// 累加相对增量(全局鼠标 dx/dy)。
	void AddAimOffset(float dx, float dy);
	// 直接设置虚拟鼠标偏移(内部钳制 ±AIM_OFFSET_LIMIT)。
	void SetAimOffset(float x, float y);
	// 用方向向量设置朝向:自动归一化并放到远离死区的幅度;(0,0) 忽略。
	void SetAimDirection(float dir_x, float dir_y);
	// 偏移清零:朝向回到默认(右),用于长时间使用后的手动校准。
	void ResetMouseOffset();

	// ---- 状态查询 ----

	EAction GetAction() const { return action; }
	float ActionElapsed(float now) const { return now - action_start_time; }

	int GetEmoticonId() const { return emoticon_id; }
	float EmoticonElapsed(float now) const { return now - emoticon_start_time; }
	bool EmoteActive(float now) const;

	// 朝向单位向量;偏移过近时退化为 (1,0) 朝右,避免方向抖动。
	teer::vec2 ComputeDirection() const;

	// 根据当前动作返回 CAnimState 指针 (idle / hammer_swing)。
	const teer::CAnimState* GetAnimState(float now) const;

	// 返回眼睛表情:气泡激活时跟随气泡映射,否则普通眼。
	teer::EEmote GetEmote() const;
};

// 随机返回一个有效表情编号 0..NUM_EMOTICONS-1。
// 需要随机表情时由调用方生成后传入(ShowEmoticon / Emoticon 事件)。
int RandomEmoticonId();

} // namespace live2tee
