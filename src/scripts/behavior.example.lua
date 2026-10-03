-- ============================================================================
-- Live2Tee 行为脚本示例
--
-- 用法:把本文件复制为同目录下的 behavior.lua 后按需修改。
--   assets/scripts/behavior.lua 存在时自动启用(启动日志会打印
--   "behavior: 已加载脚本");删除文件即恢复内置默认行为。
-- 修改后需重启程序生效。
--
-- 可用沙箱库:base / coroutine / table / string / math / utf8
-- (没有 os / io / debug,无法执行程序或访问文件系统;print 可用)
--
-- 事件回调(全部可选,只定义你需要的;每帧/每事件在渲染线程同步调用,
-- 禁止写死循环,定时逻辑用 on_tick + 时间戳实现):
--   on_mouse_move(dx, dy)      全局鼠标相对移动
--   on_mouse_left(pressed)     鼠标左键按下/抬起
--   on_mouse_right(pressed)    鼠标右键按下/抬起
--   on_key(key, pressed)       键盘按下/抬起,key 为 VK 码(用 KEY 表)
--   on_tick(now)               每帧一次,now 为程序启动后的秒数
--
-- 动作原语:
--   play_hammer()              挥锤(400ms)
--   play_hook()                挥钩(300ms)
--   show_emoticon([id])        头顶表情,0..15 指定编号;留空/-1/越界 = 随机
--   clear_emoticon()           提前关闭表情气泡
--
-- 朝向原语:
--   add_aim(dx, dy)            累加相对偏移(默认跟随鼠标用)
--   set_aim(x, y)              直接设置虚拟鼠标偏移
--   set_aim_dir(x, y)          用方向向量设朝向(自动归一化),如 set_aim_dir(1,-1) 右上
--   reset_aim()                回正(朝右)
--   aim_x(), aim_y()           当前偏移
--
-- 状态/输入查询:
--   action()                   0=空闲 1=挥锤中 2=挥钩中
--   action_elapsed()           当前动作已进行秒数
--   emoticon_id()              当前气泡编号,-1 = 无
--   emoticon_elapsed()         气泡已显示秒数
--   emote_active()             是否有气泡
--   now()                      当前秒数
--   key_down(KEY.F1)           某键是否按住
--   key_held_ms(KEY.SPACE)     某键已按住毫秒数(未按住=0),可做长按判定
--   mouse_left_down() / mouse_left_held_ms()
--   mouse_right_down() / mouse_right_held_ms()
--   idle_ms()                  距上次任何输入的毫秒
--   random_emoticon()          随机表情编号
--
-- KEY 常量:KEY.F1..KEY.F12、KEY.A..KEY.Z、KEY["0"]..KEY["9"]、KEY.SPACE、
--   KEY.ENTER、KEY.ESC、KEY.TAB、KEY.BACKSPACE、KEY.SHIFT/CTRL/ALT、
--   KEY.LEFT/UP/RIGHT/DOWN
--
-- 表情编号(emoticons.png 4x4 行优先):
--   0 OOOP!  1 !      2 爱心   3 汗滴   4 ...
--   5 音符   6 SORRY! 7 鬼魂   8 怒气   9 砸扁
--   10 爆炸  11 骂人  12 ZZZ   13 WTF   14 ∩∩    15 ???
-- ============================================================================

-- 下一次允许"空闲自动表情"的时刻(秒)
local next_idle_emote = 0

function on_mouse_move(dx, dy)
	add_aim(dx, dy) -- 默认:Tee 朝向跟随全局鼠标
end

function on_mouse_left(pressed)
	if pressed then
		play_hammer()
	end
end

function on_mouse_right(pressed)
	if pressed then
		play_hook()
	end
end

function on_key(key, pressed)
	if not pressed then
		return
	end

	if key == KEY.F1 then
		show_emoticon(8)                     -- F1:指定"怒气"
	elseif key == KEY.F2 then
		show_emoticon(random_emoticon())     -- F2:随机
	elseif key == KEY.F3 then
		set_aim_dir(-1, 1)                   -- F3:朝左下
	elseif key == KEY.F4 then
		reset_aim()                          -- F4:回正朝右
	elseif key == KEY.SPACE and key_held_ms(KEY.SPACE) > 800 then
		play_hook()                          -- 长按空格 >0.8s:挥钩
	else
		show_emoticon()                      -- 其他键:随机表情
	end
end

function on_tick(now)
	-- 空闲超过 10 秒后,每 8~15 秒自动冒一个随机表情
	if idle_ms() > 10000 and now >= next_idle_emote then
		show_emoticon()
		next_idle_emote = now + 8 + math.random() * 7
	end
end
