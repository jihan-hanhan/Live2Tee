// 配置 GUI 实现。

#include "control_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

#if defined(_WIN32)
	#include <windows.h> // GetAsyncKeyState:检测全局左键按下沿
#endif

#include "skin_picker_dialog.h"

namespace live2tee {

ControlWindow::ControlWindow(const AppConfig& cfg, QWidget* parent)
	: QWidget(parent)
	, cfg_(cfg)
{
	setWindowTitle(tr("Live2Tee 设置"));
	setFixedSize(460, 0); // 高度自适应内容

	auto* root = new QVBoxLayout(this);

	// ---- 目录 ----
	assets_edit_ = new QLineEdit(this);
	auto* assets_browse = new QPushButton(tr("浏览..."), this);
	connect(assets_browse, &QPushButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(
			this, tr("选择 assets 目录"), assets_edit_->text());
		if (!dir.isEmpty()) {
			assets_edit_->setText(QDir::toNativeSeparators(dir));
			UpdatePageUrl();
		}
	});

	skins_edit_ = new QLineEdit(this);
	auto* skins_browse = new QPushButton(tr("浏览..."), this);
	connect(skins_browse, &QPushButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(
			this, tr("选择 skins 目录"), skins_edit_->text());
		if (!dir.isEmpty()) {
			skins_edit_->setText(QDir::toNativeSeparators(dir));
			RefreshSkinList();
		}
	});

	auto* assets_row = new QWidget(this);
	auto* assets_row_lay = new QHBoxLayout(assets_row);
	assets_row_lay->setContentsMargins(0, 0, 0, 0);
	assets_row_lay->addWidget(assets_edit_, 1);
	assets_row_lay->addWidget(assets_browse);

	auto* skins_row = new QWidget(this);
	auto* skins_row_lay = new QHBoxLayout(skins_row);
	skins_row_lay->setContentsMargins(0, 0, 0, 0);
	skins_row_lay->addWidget(skins_edit_, 1);
	skins_row_lay->addWidget(skins_browse);

	auto* dir_form = new QFormLayout;
	dir_form->addRow(tr("assets 目录:"), assets_row);
	dir_form->addRow(tr("skins 目录:"), skins_row);

	// ---- 皮肤 / 缩放 / 背景 ----
	skin_combo_ = new QComboBox(this);
	auto* skin_pick_btn = new QPushButton(tr("选择..."), this);
	skin_pick_btn->setToolTip(tr("打开皮肤选择窗口,按 Tee 渲染预览图挑选"));
	connect(skin_pick_btn, &QPushButton::clicked, this, [this] {
		// 以编辑框中的当前目录为准(用户可能刚"浏览..."了新目录,尚未点应用)
		AppConfig tmp = cfg_;
		tmp.assets_dir = assets_edit_->text().trimmed();
		tmp.skins_dir = skins_edit_->text().trimmed();
		tmp.skin = skin_combo_->currentText();
		SkinPickerDialog dlg(tmp, this);
		if (dlg.exec() == QDialog::Accepted) {
			const QString name = dlg.SelectedSkin();
			int idx = skin_combo_->findText(name);
			if (idx < 0) {
				skin_combo_->addItem(name);
				idx = skin_combo_->count() - 1;
			}
			skin_combo_->setCurrentIndex(idx); // 仅回选,保存/生效仍走"应用"
		}
	});

	auto* skin_row = new QWidget(this);
	auto* skin_row_lay = new QHBoxLayout(skin_row);
	skin_row_lay->setContentsMargins(0, 0, 0, 0);
	skin_row_lay->addWidget(skin_pick_btn);
	skin_row_lay->addWidget(skin_combo_, 1);

	scale_spin_ = new QDoubleSpinBox(this);
	scale_spin_->setRange(0.25, 4.0);
	scale_spin_->setSingleStep(0.05);
	scale_spin_->setDecimals(2);
	scale_spin_->setToolTip(tr("整体缩放 Tee/武器/表情"));

	bg_combo_ = new QComboBox(this);
	bg_combo_->addItem(tr("透明背景"));
	bg_combo_->addItem(tr("绿幕"));

	auto* misc_form = new QFormLayout;
	misc_form->addRow(tr("皮肤:"), skin_row);
	misc_form->addRow(tr("渲染缩放:"), scale_spin_);
	misc_form->addRow(tr("背景(仅预览):"), bg_combo_);

	// ---- 浏览器源输出 ----
	auto* output_group = new QGroupBox(tr("浏览器源输出(OBS)"), this);

	browser_check_ = new QCheckBox(tr("启用 WebSocket 推帧"), output_group);

	port_spin_ = new QSpinBox(output_group);
	port_spin_->setRange(1024, 65535);

	size_combo_ = new QComboBox(output_group);
	for (int s : {256, 384, 512, 768, 1024})
		size_combo_->addItem(QStringLiteral("%1 × %1").arg(s), s);

	fps_combo_ = new QComboBox(output_group);
	fps_combo_->addItem(QStringLiteral("30"), 30);
	fps_combo_->addItem(QStringLiteral("60"), 60);

	url_edit_ = new QLineEdit(output_group);
	url_edit_->setReadOnly(true);

	auto* url_copy = new QPushButton(tr("复制 URL"), output_group);
	connect(url_copy, &QPushButton::clicked, this, [this] {
		QApplication::clipboard()->setText(url_edit_->text());
	});

	auto* output_form = new QFormLayout(output_group);
	output_form->addRow(browser_check_);
	output_form->addRow(tr("端口:"), port_spin_);
	output_form->addRow(tr("分辨率:"), size_combo_);
	output_form->addRow(tr("帧率:"), fps_combo_);
	output_form->addRow(tr("浏览器源 URL:"), url_edit_);
	output_form->addRow(QString(), url_copy);

	preview_check_ = new QCheckBox(tr("显示桌面预览窗口(调试用)"), this);

	// ---- 鼠标偏移校准(长时间使用后朝向漂移时重新对齐真实光标) ----
	auto* reset_mouse_btn = new QPushButton(tr("修正鼠标位置(对齐当前光标)"), this);
	reset_mouse_btn->setToolTip(
		tr("长时间使用后若 Tee 朝向与鼠标方位不符,点击把虚拟鼠标\n"
		   "重新锚定到当前真实光标位置(Tee 朝向 = 光标相对屏幕中心的方位)"));
	connect(reset_mouse_btn, &QPushButton::clicked,
			this, &ControlWindow::ResetVMouseRequested);

	// ---- 设置朝向原点:用户移动光标到目标位置后按左键确认 ----
	origin_btn_ = new QPushButton(tr("设置朝向原点..."), this);
	origin_btn_->setToolTip(
		tr("点击后把光标移到 Tee 应当朝向参考的原点位置,再按一次鼠标左键确认。\n"
		   "确认后 Tee 朝向 = 光标相对该原点的方位(而非屏幕中心)。"));
	connect(origin_btn_, &QPushButton::clicked, this, &ControlWindow::BeginSetOrigin);

	origin_hint_ = new QLabel(this);
	origin_hint_->setStyleSheet(QStringLiteral("color:#2a7;"));
	origin_hint_->hide();

	origin_timer_ = new QTimer(this);
	origin_timer_->setInterval(20);
	connect(origin_timer_, &QTimer::timeout, this, &ControlWindow::PollOriginClick);

	root->addLayout(dir_form);
	root->addLayout(misc_form);
	root->addWidget(output_group);
	root->addWidget(preview_check_);
	root->addWidget(reset_mouse_btn);
	root->addWidget(origin_btn_);
	root->addWidget(origin_hint_);

	// 端口变化时 URL 实时跟随
	connect(port_spin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
		url_edit_->setText(PageUrlFor(v));
	});

	auto* buttons = new QHBoxLayout;
	auto* apply_btn = new QPushButton(tr("应用"), this);
	auto* quit_btn = new QPushButton(tr("退出程序"), this);
	buttons->addStretch(1);
	buttons->addWidget(apply_btn);
	buttons->addWidget(quit_btn);
	root->addLayout(buttons);

	connect(apply_btn, &QPushButton::clicked, this, &ControlWindow::OnApply);
	connect(quit_btn, &QPushButton::clicked, this, [] { qApp->quit(); });

	LoadFromConfig();
}

QString ControlWindow::PageUrlFor(int port) const
{
	QUrl url = QUrl::fromLocalFile(
		cfg_.ResolvedAssetsDir() + QStringLiteral("/browser/live2tee.html"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("port"), QString::number(port));
	url.setQuery(query);
	return url.toString();
}

void ControlWindow::UpdatePageUrl()
{
	url_edit_->setText(PageUrlFor(port_spin_->value()));
}

void ControlWindow::LoadFromConfig()
{
	// 空 = 默认路径,填上解析后的实际路径让用户看得见
	assets_edit_->setText(cfg_.ResolvedAssetsDir());
	skins_edit_->setText(cfg_.ResolvedSkinsDir());
	RefreshSkinList();
	const int idx = skin_combo_->findText(cfg_.skin);
	skin_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
	scale_spin_->setValue(cfg_.render_scale);
	bg_combo_->setCurrentIndex(cfg_.green_screen ? 1 : 0);

	browser_check_->setChecked(cfg_.browser_output);
	port_spin_->setValue(cfg_.browser_port);
	const int size_idx = size_combo_->findData(cfg_.output_size);
	size_combo_->setCurrentIndex(size_idx >= 0 ? size_idx : size_combo_->findData(512));
	const int fps_idx = fps_combo_->findData(cfg_.output_fps);
	fps_combo_->setCurrentIndex(fps_idx >= 0 ? fps_idx : fps_combo_->findData(60));
	UpdatePageUrl();
	preview_check_->setChecked(cfg_.preview_window);
}

void ControlWindow::RefreshSkinList()
{
	skin_combo_->blockSignals(true);
	skin_combo_->clear();
	const QStringList skins = ScanSkinFiles(skins_edit_->text());
	skin_combo_->addItems(skins);
	skin_combo_->blockSignals(false);
}

void ControlWindow::OnApply()
{
	AppConfig new_cfg;
	new_cfg.assets_dir = assets_edit_->text().trimmed();
	new_cfg.skins_dir = skins_edit_->text().trimmed();
	// 与默认值相同就存空串,保持"空 = 默认"的语义
	if (QDir(new_cfg.assets_dir).absolutePath() ==
		QCoreApplication::applicationDirPath() + QStringLiteral("/assets"))
		new_cfg.assets_dir.clear();
	new_cfg.skin = skin_combo_->currentText();
	new_cfg.render_scale = static_cast<float>(scale_spin_->value());
	new_cfg.green_screen = bg_combo_->currentIndex() == 1;
	new_cfg.browser_output = browser_check_->isChecked();
	new_cfg.browser_port = port_spin_->value();
	{
		const QVariant size_data = size_combo_->currentData();
		new_cfg.output_size = size_data.isValid() ? size_data.toInt() : 512;
		const QVariant fps_data = fps_combo_->currentData();
		new_cfg.output_fps = fps_data.isValid() ? fps_data.toInt() : 60;
	}
	new_cfg.preview_window = preview_check_->isChecked();
	new_cfg.gui_hotkey = cfg_.gui_hotkey; // 热键暂不在 GUI 暴露,保持原值

	cfg_ = new_cfg;
	if (!new_cfg.Save())
		std::fprintf(stderr, "config save failed\n");
	UpdatePageUrl();

	emit ConfigApplied(new_cfg);
}

void ControlWindow::BeginSetOrigin()
{
	origin_awaiting_ = true;
	origin_prev_down_ = false; // 上一轮先记为未按下,等待下一次按下沿
	origin_btn_->setText(tr("等待点击确认...(关闭窗口可取消)"));
	origin_btn_->setEnabled(false);
	origin_hint_->setText(tr("请将光标移到目标原点位置,然后按下鼠标左键确认。"));
	origin_hint_->show();
	origin_timer_->start();
}

void ControlWindow::PollOriginClick()
{
#if defined(_WIN32)
	// GetAsyncKeyState 的最高位表示"当前是否按下";检测 0→1 沿,
	// 这样不会把用户按"设置朝向原点"按钮那次点击误当成确认。
	const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	if (down && !origin_prev_down_) {
		const QPoint origin = QCursor::pos(); // Qt 逻辑像素,与 SetMouseOrigin 一致
		origin_prev_down_ = down;
		CancelSetOrigin();
		emit SetOriginRequested(origin);
		return;
	}
	origin_prev_down_ = down;
#else
	// 非 Windows 暂无全局输入后端:直接把当前光标位置作为原点,
	// 避免按钮无响应(实际朝向功能本身在这些平台也不可用)。
	(void)0;
	const QPoint origin = QCursor::pos();
	CancelSetOrigin();
	emit SetOriginRequested(origin);
#endif
}

void ControlWindow::CancelSetOrigin()
{
	origin_timer_->stop();
	origin_awaiting_ = false;
	origin_prev_down_ = false;
	if (origin_btn_) {
		origin_btn_->setText(tr("设置朝向原点..."));
		origin_btn_->setEnabled(true);
	}
	if (origin_hint_)
		origin_hint_->hide();
}

void ControlWindow::closeEvent(QCloseEvent* e)
{
	// 关闭 = 隐藏,之后还能通过热键/托盘/预览双右键唤起
	CancelSetOrigin();
	hide();
	e->ignore();
}

} // namespace live2tee
