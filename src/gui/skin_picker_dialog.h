#pragma once

// 皮肤选择子窗口(布局对齐 DDNet 的 Tee 设置页):
//   - "你的皮肤":当前皮肤的 Tee 预览 + 皮肤名(可直接输入精确名字回车);
//   - "皮肤名称前缀":可清除的前缀输入框 + kitty / santa 快捷按钮。前缀按
//     DDNet 语义生效:对任意皮肤 X,若 "<前缀>_X.png" 存在则显示/选择它,
//     否则 X 保持不变(无变体则无变化);
//   - 搜索框:按皮肤名即时过滤;
//   - 下方皮肤网格:用真实 Tee 渲染管线(离屏 OpenGL/FBO)把每个皮肤渲染成
//     完整站立 Tee 图标。
//
//   性能策略(与 DDNet 的"滚到哪加载到哪"一致):
//     - 窗口立即打开,网格只先创建轻量按钮(文件名),不渲染任何 Tee;
//     - QTimer 时间片按视口位置懒渲染:可视行优先,并向上下各预渲染若干行;
//     - 渲染结果按 文件路径+修改时间+大小 缓存在进程内,再次打开/过滤即时显示。
//   选择结果仅回传给调用方(下拉框),是否保存/生效仍由"应用"按钮决定。

#include <QDialog>
#include <QString>
#include <memory>

#include "../config.h"

class QShowEvent;

namespace live2tee {

class SkinPickerDialog : public QDialog {
	Q_OBJECT
public:
	// cfg 提供 assets 目录(取 game.png/emoticons.png)、skins 目录与
	// 当前选中皮肤(cfg.skin,用于初始高亮与预览)。
	explicit SkinPickerDialog(const AppConfig& cfg, QWidget* parent = nullptr);
	~SkinPickerDialog() override;

	// 用户选中的皮肤文件名(相对 skins 目录,含 .png,已应用前缀解析);
	// 取消时为空。
	QString SelectedSkin() const;

protected:
	void showEvent(QShowEvent* e) override; // 首次显示时滚动到当前皮肤

private:
	void RebuildGrid(); // 按搜索词重建按钮网格(图标懒渲染/缓存命中即时显示)
	void ApplyPrefix(); // 前缀改变:重解全部条目的图标键、刷新预览(不重建网格)
	void UpdatePreview(); // 按当前选中行+前缀刷新"你的皮肤"预览
	QString ResolveFile(const QString& file) const; // 前缀解析:prefix_file 存在则用之,否则原样
	QIcon EnsureIcon(const QString& path); // 渲染单个 Tee 图标并写入缓存(缓存命中直接返回)

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace live2tee
