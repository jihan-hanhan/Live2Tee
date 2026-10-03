#pragma once

// 行为映射层:把全局输入事件翻译成 TeeState 动作原语调用。
//   输入事件流 -> IBehavior::OnEvent -> TeeState::PlayHammer/ShowEmoticon/...
// 内置 BuiltinBehavior 复刻历史默认行为(左键锤/右键钩/任意键轮播表情);
// 未来的 LuaBehavior 实现同一接口,由 TeeScene::SetBehavior() 热替换,
// 渲染管线与状态机无需任何改动。

#include <unordered_map>

#include "input/input.h"

namespace live2tee {

class TeeState;

// 行为上下文:跨事件的记忆(按键/鼠标键按住时刻、输入空闲时间),
// 供映射器实现"长按 N 毫秒""空闲表演"等判定。
// 由 TeeScene 在事件进映射器之前更新;映射器只读。
class BehaviorContext {
public:
	// 记录一个事件对按住状态/空闲计时的影响(在 OnEvent 之前调用)。
	void NoteEvent(const InputEvent& ev, float now);
	// 清空全部记忆(切换行为映射器时调用)。
	void Reset();

	bool IsKeyDown(int vk) const;
	float KeyHeldMs(int vk, float now) const; // 未按住返回 0

	bool IsMouseLeftDown() const;
	float MouseLeftHeldMs(float now) const;
	bool IsMouseRightDown() const;
	float MouseRightHeldMs(float now) const;

	// 距上一次任意输入事件的毫秒数(首事件之前为 0)。
	float IdleMs(float now) const;

private:
	std::unordered_map<int, float> key_down_since_; // VK -> 按下时刻(秒)
	float mouse_left_down_since_ = -1.0f;
	float mouse_right_down_since_ = -1.0f;
	float last_input_time_ = 0.0f;
	bool has_input_ = false;
};

// 行为映射器接口。
// 实现方约束:
//   - OnEvent/OnTick 在渲染线程同步调用,必须快速返回,禁止阻塞/长循环;
//     延迟与定时编排用 now 时间戳自行记账,在后续 OnTick 中完成。
//   - 只能通过 TeeState 的动作原语改变状态,不直接渲染、不碰线程。
class IBehavior {
public:
	virtual ~IBehavior() = default;

	// 每个输入事件一次(此时 BehaviorContext 已更新完该事件)。
	virtual void OnEvent(TeeState& state, BehaviorContext& ctx,
						 const InputEvent& ev, float now) = 0;

	// 每帧一次(无输入事件也调用):定时器/连招/空闲表演挂这里。
	virtual void OnTick(TeeState& state, BehaviorContext& ctx, float now) {}
};

// 内置默认行为(与历史版本完全一致):
//   鼠标移动      -> 朝向偏移累加
//   鼠标左键按下  -> 挥锤
//   鼠标右键按下  -> 挥钩
//   任意键按下    -> 顺序轮播头顶表情(300ms 冷却,防狂按鬼畜)
//   Emoticon 事件 -> 指定编号表情(<0/越界随机),不经冷却
class BuiltinBehavior : public IBehavior {
public:
	void OnEvent(TeeState& state, BehaviorContext& ctx,
				 const InputEvent& ev, float now) override;

private:
	int emote_seq_ = 0;                 // 键盘轮播序号
	float last_key_emote_time_ = -1.0e9f;
};

} // namespace live2tee
