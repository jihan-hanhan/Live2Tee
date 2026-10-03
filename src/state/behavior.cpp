// 行为映射层实现:上下文记忆 + 内置默认映射。

#include "behavior.h"

#include "state.h"

namespace live2tee {

// 键盘"任意键轮播表情"两次之间的最小间隔:限频防止气泡鬼畜
static constexpr float KEY_EMOTE_COOLDOWN_SEC = 0.30f;

static float HeldMs(float down_since, float now)
{
	return down_since >= 0.0f ? (now - down_since) * 1000.0f : 0.0f;
}

// ---------------------------------------------------------------------------
// BehaviorContext
// ---------------------------------------------------------------------------

void BehaviorContext::NoteEvent(const InputEvent& ev, float now)
{
	// 任意事件(含鼠标移动)都视为用户活动,刷新空闲计时
	last_input_time_ = now;
	has_input_ = true;

	switch (ev.kind) {
	case EInputKind::Key:
		if (ev.pressed)
			key_down_since_[ev.keycode] = now;
		else
			key_down_since_.erase(ev.keycode);
		break;
	case EInputKind::MouseLeft:
		mouse_left_down_since_ = ev.pressed ? now : -1.0f;
		break;
	case EInputKind::MouseRight:
		mouse_right_down_since_ = ev.pressed ? now : -1.0f;
		break;
	default:
		break; // MouseMove / Emoticon 不影响按住状态
	}
}

void BehaviorContext::Reset()
{
	key_down_since_.clear();
	mouse_left_down_since_ = -1.0f;
	mouse_right_down_since_ = -1.0f;
	last_input_time_ = 0.0f;
	has_input_ = false;
}

bool BehaviorContext::IsKeyDown(int vk) const
{
	return key_down_since_.find(vk) != key_down_since_.end();
}

float BehaviorContext::KeyHeldMs(int vk, float now) const
{
	const auto it = key_down_since_.find(vk);
	return it != key_down_since_.end() ? (now - it->second) * 1000.0f : 0.0f;
}

bool BehaviorContext::IsMouseLeftDown() const
{
	return mouse_left_down_since_ >= 0.0f;
}

float BehaviorContext::MouseLeftHeldMs(float now) const
{
	return HeldMs(mouse_left_down_since_, now);
}

bool BehaviorContext::IsMouseRightDown() const
{
	return mouse_right_down_since_ >= 0.0f;
}

float BehaviorContext::MouseRightHeldMs(float now) const
{
	return HeldMs(mouse_right_down_since_, now);
}

float BehaviorContext::IdleMs(float now) const
{
	return has_input_ ? (now - last_input_time_) * 1000.0f : 0.0f;
}

// ---------------------------------------------------------------------------
// BuiltinBehavior
// ---------------------------------------------------------------------------

void BuiltinBehavior::OnEvent(TeeState& state, BehaviorContext& ctx,
							  const InputEvent& ev, float now)
{
	(void)ctx;
	switch (ev.kind) {
	case EInputKind::MouseMove:
		state.AddAimOffset(static_cast<float>(ev.dx),
						   static_cast<float>(ev.dy));
		break;

	case EInputKind::MouseLeft:
		if (ev.pressed)
			state.PlayHammer(now);
		break;

	case EInputKind::MouseRight:
		if (ev.pressed)
			state.PlayHook(now);
		break;

	case EInputKind::Key:
		if (ev.pressed && now - last_key_emote_time_ >= KEY_EMOTE_COOLDOWN_SEC) {
			state.ShowEmoticon(emote_seq_ % teer::NUM_EMOTICONS, now);
			++emote_seq_;
			last_key_emote_time_ = now;
		}
		break;

	case EInputKind::Emoticon:
		// 程序化触发:指定编号,<0/越界由 ShowEmoticon 随机
		state.ShowEmoticon(ev.emoticon_id, now);
		break;
	}
}

} // namespace live2tee
