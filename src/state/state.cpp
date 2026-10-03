// Tee 状态机实现:动作原语 + 超时回收 + 朝向/动画查询。
// "输入事件 -> 原语"的映射策略不在本文件,见 src/state/behavior.*。

#include "state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace live2tee {

// 距虚拟鼠标小于这个阈值时方向退化,避免抖动
static constexpr float DIRECTION_DEADZONE = 8.0f;
// SetAimDirection 归一化后采用的偏移幅度(远离死区、远低于钳制上限)
static constexpr float AIM_DIRECTION_MAGNITUDE = 1000.0f;

// ---------------------------------------------------------------------------
// 动作原语
// ---------------------------------------------------------------------------

void TeeState::PlayHammer(float now)
{
	action = EAction::Hammer;
	action_start_time = now;
}

void TeeState::PlayHook(float now)
{
	action = EAction::Hook;
	action_start_time = now;
}

void TeeState::ShowEmoticon(int id, float now)
{
	if (id < 0 || id >= teer::NUM_EMOTICONS)
		id = RandomEmoticonId();
	emoticon_id = id;
	emoticon_start_time = now;
}

void TeeState::ClearEmoticon()
{
	emoticon_id = -1;
}

// ---------------------------------------------------------------------------
// 朝向原语
// ---------------------------------------------------------------------------

void TeeState::AddAimOffset(float dx, float dy)
{
	SetAimOffset(mouse_off_x + dx, mouse_off_y + dy);
}

void TeeState::SetAimOffset(float x, float y)
{
	mouse_off_x = std::clamp(x, -AIM_OFFSET_LIMIT, AIM_OFFSET_LIMIT);
	mouse_off_y = std::clamp(y, -AIM_OFFSET_LIMIT, AIM_OFFSET_LIMIT);
}

void TeeState::SetAimDirection(float dir_x, float dir_y)
{
	const float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
	if (len < 1e-6f)
		return;
	SetAimOffset(dir_x / len * AIM_DIRECTION_MAGNITUDE,
				 dir_y / len * AIM_DIRECTION_MAGNITUDE);
}

void TeeState::ResetMouseOffset()
{
	mouse_off_x = 0.0f;
	mouse_off_y = 0.0f;
}

// ---------------------------------------------------------------------------
// 帧推进 / 查询
// ---------------------------------------------------------------------------

void TeeState::Tick(float now)
{
	// 气泡独立到期,不受动作切换影响
	if (emoticon_id >= 0 && now - emoticon_start_time >= EMOTE_DURATION_SEC)
		emoticon_id = -1;

	if (action == EAction::Idle)
		return;

	const float elapsed = now - action_start_time;
	switch (action) {
	case EAction::Hammer:
		if (elapsed >= HAMMER_DURATION_SEC)
			action = EAction::Idle;
		break;
	case EAction::Hook:
		if (elapsed >= HOOK_DURATION_SEC)
			action = EAction::Idle;
		break;
	default: break;
	}
}

bool TeeState::EmoteActive(float now) const
{
	return emoticon_id >= 0 && now - emoticon_start_time < EMOTE_DURATION_SEC;
}

teer::vec2 TeeState::ComputeDirection() const
{
	const float len = std::sqrt(mouse_off_x * mouse_off_x + mouse_off_y * mouse_off_y);
	if (len < DIRECTION_DEADZONE)
		return teer::vec2(1.0f, 0.0f); // 默认朝右
	return teer::vec2(mouse_off_x / len, mouse_off_y / len);
}

const teer::CAnimState* TeeState::GetAnimState(float now) const
{
	// Hammer 动作:复刻 CAnimState::GetIdle() 的姿态(BASE + IDLE),
	// 再叠加 hammer_swing(只动 attach 通道)。
	// 注意:不能只用 Set(IDLE),会缺 BASE 的 body Y=-4 / foot Y=10,
	// 导致脚从 Y=10 跑到 Y=0(脚往上跑)。
	if (action == EAction::Hammer) {
		const float t = (now - action_start_time) / HAMMER_DURATION_SEC;
		static teer::CAnimState hammer_state;
		hammer_state.Set(&teer::s_aAnimations[teer::ANIM_BASE], 0.0f);
		hammer_state.Add(&teer::s_aAnimations[teer::ANIM_IDLE], 0.0f, 1.0f);
		hammer_state.Add(&teer::s_aAnimations[teer::ANIM_HAMMER_SWING], t, 1.0f);
		return &hammer_state;
	}
	// Idle / Hook / Emote 都用 idle 姿态
	return teer::CAnimState::GetIdle();
}

// 气泡 → 眼睛表情映射(用户指定的对照表,编号即 emoticons.png 4x4 行优先 0..15)
static teer::EEmote EmoticonEyeMapping(int id)
{
	switch (id) {
	case 0:  return teer::EMOTE_PAIN;     // 0: OOOP!
	case 1:  return teer::EMOTE_SURPRISE; // 1: !
	case 2:  return teer::EMOTE_HAPPY;    // 2: 爱心
	case 3:  return teer::EMOTE_BLINK;    // 3: 汗滴
	case 4:  return teer::EMOTE_BLINK;    // 4: ...
	case 5:  return teer::EMOTE_HAPPY;    // 5: 音符
	case 6:  return teer::EMOTE_PAIN;     // 6: SORRY!
	case 7:  return teer::EMOTE_SURPRISE; // 7: 鬼魂
	case 8:  return teer::EMOTE_PAIN;     // 8: 怒气标记
	case 9:  return teer::EMOTE_ANGRY;    // 9: 砸扁云
	case 10: return teer::EMOTE_ANGRY;    // 10: 恶魔爆炸
	case 11: return teer::EMOTE_ANGRY;    // 11: #!!**! 骂人云
	case 12: return teer::EMOTE_BLINK;    // 12: ZZZ
	case 13: return teer::EMOTE_SURPRISE; // 13: WTF
	case 14: return teer::EMOTE_HAPPY;    // 14: 眼睛∩∩
	case 15: return teer::EMOTE_SURPRISE; // 15: ??? 问号云
	default: return teer::EMOTE_NORMAL;
	}
}

teer::EEmote TeeState::GetEmote() const
{
	// 眼睛跟随当前气泡(气泡与 Hammer/Hook 共存时,眼睛仍由气泡驱动);
	// 锤子/钩爪本身不改眼睛
	if (emoticon_id >= 0)
		return EmoticonEyeMapping(emoticon_id);
	return teer::EMOTE_NORMAL;
}

int RandomEmoticonId()
{
	static thread_local std::mt19937 rng(static_cast<std::mt19937::result_type>(
		std::chrono::steady_clock::now().time_since_epoch().count()));
	std::uniform_int_distribution<int> dist(0, teer::NUM_EMOTICONS - 1);
	return dist(rng);
}

} // namespace live2tee
