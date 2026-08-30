#pragma once

// Tee 状态机:虚拟鼠标位置 + 当前动作。
// 不含 Walk —— 直播场景不需要移动动画。
// 表情气泡是独立生命周期,可与 Hammer/Hook 共存。

#include <tee_anim.h>
#include <tee_emoticon.h>
#include <tee_math.h>

#include "input/input.h"

namespace live2tee {

enum class EAction {
	Idle = 0,
	Hammer,
	Hook,
};

struct TeeState {
	// 虚拟鼠标相对 Tee 中心的偏移量(偏移控制)。
	// 全局鼠标的运动增量直接累加到这里,不依赖任何平台的
	// 屏幕绝对坐标 —— 朝向只取决于偏移方向。
	float mouse_off_x = 0.0f;
	float mouse_off_y = 0.0f;

	EAction action = EAction::Idle;
	float action_start_time = 0.0f;  // 动作开始时刻(秒)

	// 头顶表情气泡(独立生命周期,不占动作位,可与 Hammer/Hook 共存)。
	int emoticon_id = -1;
	float emoticon_start_time = 0.0f;

	// 应用一个输入事件(更新鼠标偏移 + 设置动作)。
	void Apply(const InputEvent& ev, float now);

	// 推进时间,清理超时动作 (Hammer 400ms / Hook 600ms / Emote 2000ms)。
	void Tick(float now);

	// 计算朝向:鼠标偏移方向的单位向量(偏移控制,与 Tee 位置无关)。
	// 偏移过近时退化为 (1, 0) 朝右,避免方向抖动。
	teer::vec2 ComputeDirection() const;

	// 鼠标偏移清零:朝向回到默认(右),用于长时间使用后的手动校准。
	void ResetMouseOffset();

	// 根据当前动作返回 CAnimState 指针 (idle / hammer_swing)。
	const teer::CAnimState* GetAnimState(float now) const;

	// 头顶气泡是否在显示中。
	bool EmoteActive(float now) const;

	// 返回眼睛表情:气泡激活时跟随气泡映射,否则普通眼。
	teer::EEmote GetEmote() const;
};

} // namespace live2tee
