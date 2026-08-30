// Tee 状态机实现:鼠标偏移(偏移控制) + 动作优先级 + 超时回收。

#include "state.h"

#include <cmath>

namespace live2tee {

// 动作持续时间(秒)
static constexpr float HAMMER_DURATION = 0.40f;
static constexpr float HOOK_DURATION = 0.30f;
static constexpr float EMOTE_DURATION = 2.00f;
// 两次表情之间的最小间隔:快速连按/狂按键盘时限频,防止气泡狂闪
static constexpr float EMOTE_COOLDOWN = 0.30f;

// 距虚拟鼠标小于这个阈值时方向退化,避免抖动
static constexpr float DIRECTION_DEADZONE = 8.0f;

void TeeState::Apply(const InputEvent& ev, float now)
{
	switch (ev.kind) {
	case EInputKind::MouseMove:
		mouse_off_x += static_cast<float>(ev.dx);
		mouse_off_y += static_cast<float>(ev.dy);
		break;

	case EInputKind::MouseLeft:
		if (ev.pressed) {
			action = EAction::Hammer;
			action_start_time = now;
		}
		break;

	case EInputKind::MouseRight:
		if (ev.pressed) {
			action = EAction::Hook;
			action_start_time = now;
		}
		break;

	case EInputKind::Key:
		if (ev.pressed) {
			// 任意键触发头顶表情气泡。气泡生命周期独立于动作位,
			// 与 Hammer/Hook 共存;冷却期内的新按键忽略,限频防止鬼畜
			static float s_last_emote_time = -1e9f;
			if (now - s_last_emote_time >= EMOTE_COOLDOWN) {
				static unsigned int s_emote_counter = 0;
				emoticon_id = static_cast<int>((++s_emote_counter) % teer::NUM_EMOTICONS);
				emoticon_start_time = now;
				s_last_emote_time = now;
			}
		}
		break;
	}
}

void TeeState::Tick(float now)
{
	// 气泡独立到期,不受动作切换影响
	if (emoticon_id >= 0 && now - emoticon_start_time >= EMOTE_DURATION)
		emoticon_id = -1;

	if (action == EAction::Idle)
		return;

	const float elapsed = now - action_start_time;
	switch (action) {
	case EAction::Hammer:
		if (elapsed >= HAMMER_DURATION)
			action = EAction::Idle;
		break;
	case EAction::Hook:
		if (elapsed >= HOOK_DURATION)
			action = EAction::Idle;
		break;
	default: break;
	}
}

bool TeeState::EmoteActive(float now) const
{
	return emoticon_id >= 0 && now - emoticon_start_time < EMOTE_DURATION;
}

teer::vec2 TeeState::ComputeDirection() const
{
	const float len = std::sqrt(mouse_off_x * mouse_off_x + mouse_off_y * mouse_off_y);
	if (len < DIRECTION_DEADZONE)
		return teer::vec2(1.0f, 0.0f); // 默认朝右
	return teer::vec2(mouse_off_x / len, mouse_off_y / len);
}

void TeeState::ResetMouseOffset()
{
	mouse_off_x = 0.0f;
	mouse_off_y = 0.0f;
}

const teer::CAnimState* TeeState::GetAnimState(float now) const
{
	// Hammer 动作:复刻 CAnimState::GetIdle() 的姿态(BASE + IDLE),
	// 再叠加 hammer_swing(只动 attach 通道)。
	// 注意:不能只用 Set(IDLE),会缺 BASE 的 body Y=-4 / foot Y=10,
	// 导致脚从 Y=10 跑到 Y=0(脚往上跑)。
	if (action == EAction::Hammer) {
		const float t = (now - action_start_time) / HAMMER_DURATION;
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

} // namespace live2tee
