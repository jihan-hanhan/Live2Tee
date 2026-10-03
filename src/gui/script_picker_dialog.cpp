#include "script_picker_dialog.h"

#include "../config.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace live2tee {

namespace {

// 清空布局中全部行(同步 delete,不用 deleteLater — 避免刷新时旧行残留)
void ClearLayout(QLayout* lay)
{
	while (QLayoutItem* item = lay->takeAt(0)) {
		if (QWidget* w = item->widget()) {
			lay->removeWidget(w);
			delete w;
		}
		delete item;
	}
}

} // namespace

ScriptPickerDialog::ScriptPickerDialog(const AppConfig& cfg, QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("选择脚本"));
	resize(680, 460);

	// 左列 = 扫描到的全部脚本 - 已加载;右列保持配置原顺序
	available_ = ScanLuaScripts(cfg.ResolvedScriptsDir());
	for (const QString& name : cfg.behavior_scripts)
		available_.removeOne(name);
	loaded_ = cfg.behavior_scripts;

	auto* root = new QVBoxLayout(this);

	// ---- 两列 ----
	auto* columns = new QHBoxLayout;

	auto* avail_group = new QGroupBox(tr("已有的脚本:"), this);
	auto* avail_outer = new QVBoxLayout(avail_group);
	auto* avail_scroll = new QScrollArea(avail_group);
	avail_scroll->setWidgetResizable(true);
	auto* avail_inner = new QWidget(avail_scroll);
	avail_lay_ = new QVBoxLayout(avail_inner);
	avail_lay_->setContentsMargins(4, 4, 4, 4);
	avail_scroll->setWidget(avail_inner);
	avail_outer->addWidget(avail_scroll);

	auto* loaded_group = new QGroupBox(tr("已加载的脚本(顶部优先级最高):"), this);
	auto* loaded_outer = new QVBoxLayout(loaded_group);
	auto* loaded_scroll = new QScrollArea(loaded_group);
	loaded_scroll->setWidgetResizable(true);
	auto* loaded_inner = new QWidget(loaded_scroll);
	loaded_lay_ = new QVBoxLayout(loaded_inner);
	loaded_lay_->setContentsMargins(4, 4, 4, 4);
	loaded_scroll->setWidget(loaded_inner);
	loaded_outer->addWidget(loaded_scroll);

	columns->addWidget(avail_group, 1);
	columns->addWidget(loaded_group, 1);
	root->addLayout(columns, 1);

	// ---- 底部按钮 ----
	auto* buttons = new QHBoxLayout;
	auto* apply_btn = new QPushButton(tr("应用"), this);
	auto* cancel_btn = new QPushButton(tr("Cancel"), this);
	buttons->addStretch(1);
	buttons->addWidget(apply_btn);
	buttons->addWidget(cancel_btn);
	root->addLayout(buttons);

	connect(apply_btn, &QPushButton::clicked, this, &QDialog::accept);
	connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);

	RebuildLists();
}

QStringList ScriptPickerDialog::SelectedScripts() const
{
	return loaded_;
}

void ScriptPickerDialog::RebuildLists()
{
	ClearLayout(avail_lay_);
	ClearLayout(loaded_lay_);

	// 左列:名称 + add
	for (const QString& name : available_) {
		auto* row = new QWidget;
		auto* lay = new QHBoxLayout(row);
		lay->setContentsMargins(2, 2, 2, 2);
		auto* label = new QLabel(name, row);
		auto* add_btn = new QPushButton(tr("add"), row);
		connect(add_btn, &QPushButton::clicked, this, [this, name] { AddScript(name); });
		lay->addWidget(label, 1);
		lay->addWidget(add_btn);
		avail_lay_->addWidget(row);
	}
	avail_lay_->addStretch(1);

	// 右列:名称 + up/down/移除(首行禁 up,末行禁 down)
	for (int i = 0; i < loaded_.size(); ++i) {
		const QString name = loaded_.at(i);
		auto* row = new QWidget;
		auto* lay = new QHBoxLayout(row);
		lay->setContentsMargins(2, 2, 2, 2);
		auto* label = new QLabel(name, row);
		auto* up_btn = new QPushButton(tr("up"), row);
		auto* down_btn = new QPushButton(tr("down"), row);
		auto* remove_btn = new QPushButton(tr("移除"), row);
		up_btn->setEnabled(i > 0);
		down_btn->setEnabled(i < loaded_.size() - 1);
		connect(up_btn, &QPushButton::clicked, this, [this, i] { MoveScript(i, -1); });
		connect(down_btn, &QPushButton::clicked, this, [this, i] { MoveScript(i, 1); });
		connect(remove_btn, &QPushButton::clicked, this, [this, name] { RemoveScript(name); });
		lay->addWidget(label, 1);
		lay->addWidget(up_btn);
		lay->addWidget(down_btn);
		lay->addWidget(remove_btn);
		loaded_lay_->addWidget(row);
	}
	loaded_lay_->addStretch(1);
}

void ScriptPickerDialog::AddScript(const QString& name)
{
	if (!available_.removeOne(name))
		return;
	loaded_ << name; // 新加入的放末尾(优先级最低)
	RebuildLists();
}

void ScriptPickerDialog::RemoveScript(const QString& name)
{
	if (!loaded_.removeOne(name))
		return;
	// 按文件名排序插回左列
	const auto it = std::lower_bound(available_.begin(), available_.end(), name);
	available_.insert(it, name);
	RebuildLists();
}

void ScriptPickerDialog::MoveScript(int index, int dir)
{
	const int target = index + dir;
	if (index < 0 || index >= loaded_.size() || target < 0 || target >= loaded_.size())
		return;
	loaded_.swapItemsAt(index, target);
	RebuildLists();
}

} // namespace live2tee
