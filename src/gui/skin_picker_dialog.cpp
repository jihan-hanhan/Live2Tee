// 皮肤选择子窗口实现:布局对齐 DDNet —— "你的皮肤"(预览+名字)、
// "皮肤名称前缀"(可清除输入框 + kitty/santa 按钮)、搜索过滤,以及视口感知
// 的 Tee 图标懒渲染网格(滚到哪渲染到哪)。

#include "skin_picker_dialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLVersionFunctionsFactory>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

#include "../input/input.h"
#include "../render/scene.h"

namespace live2tee {

namespace {

constexpr int RENDER_SIZE = 192;        // 离屏 FBO 边长(超采样,保证高分屏清晰)
constexpr float TEE_ICON_SCALE = 0.5f;  // Tee 直径 128 / 画布 192
constexpr int ICON_DISPLAY = 96;        // 按钮图标显示边长
constexpr int PREVIEW_SIZE = 96;        // "你的皮肤"预览边长
constexpr int COLS = 5;                 // 网格列数
constexpr int ROW_LOOKAHEAD = 3;        // 视口外上下各预渲染的行数
constexpr int FRAME_BUDGET_MS = 16;     // 单次时间片预算,保持界面流畅

// DDNet 的内置前缀:对皮肤 X,若 "<前缀>_X" 存在则换用,否则保持 X。
const QStringList& KnownPrefixes()
{
	static const QStringList kPrefixes{QStringLiteral("kitty"), QStringLiteral("santa")};
	return kPrefixes;
}

// 与 OffscreenOutput 相同:FBO 里是预乘 alpha,读回后转成直通 alpha,
// 否则 QPixmap/按钮按直通解释会出现暗边。
void UnpremultiplyInPlace(QImage& img)
{
	if (img.isNull() || img.format() != QImage::Format_RGBA8888)
		return;
	for (int y = 0; y < img.height(); ++y) {
		QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
		for (int x = 0; x < img.width(); ++x) {
			const QRgb p = line[x];
			const int a = qAlpha(p);
			if (a == 0 || a == 255)
				continue;
			const int r = qMin(255, qRed(p) * 255 / a);
			const int g = qMin(255, qGreen(p) * 255 / a);
			const int b = qMin(255, qBlue(p) * 255 / a);
			line[x] = qRgba(r, g, b, a);
		}
	}
}

// 渲染图标的进程内缓存:文件被替换(大小/时间戳变化)后自动失效。
struct IconCache {
	QString key;
	QIcon icon;
};
QHash<QString, IconCache>& CacheStore()
{
	static QHash<QString, IconCache> s;
	return s;
}

QString MakeCacheKey(const QString& path)
{
	const QFileInfo fi(path);
	return fi.absoluteFilePath() + QStringLiteral("|%1|%2")
		.arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
}

// 逐皮肤离屏渲染器:独立 GL 上下文/表面/FBO(与主 OffscreenOutput 互不
// 干扰,均在 GUI 线程使用)。支持批量:一次 makeCurrent 内渲染多张图。
class SkinIconRenderer {
public:
	SkinIconRenderer() = default;
	~SkinIconRenderer() {
		if (context_) {
			if (in_batch_)
				context_->doneCurrent();
			context_->makeCurrent(surface_);
			delete fbo_;
			fbo_ = nullptr;
			scene_.reset(); // GL 对象必须在上下文存活时销毁
			context_->doneCurrent();
		}
		delete context_;
		delete surface_;
	}
	SkinIconRenderer(const SkinIconRenderer&) = delete;
	SkinIconRenderer& operator=(const SkinIconRenderer&) = delete;

	bool Init(const AppConfig& cfg) {
		QSurfaceFormat fmt;
		fmt.setVersion(2, 1);
		fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
		fmt.setOption(QSurfaceFormat::DeprecatedFunctions, true);
		fmt.setAlphaBufferSize(8);

		context_ = new QOpenGLContext;
		context_->setFormat(fmt);
		if (!context_->create()) {
			std::fprintf(stderr, "skin picker: GL context create failed\n");
			return false;
		}
		surface_ = new QOffscreenSurface;
		surface_->setFormat(fmt);
		surface_->create();
		if (!surface_->isValid()) {
			std::fprintf(stderr, "skin picker: offscreen surface invalid\n");
			return false;
		}

		context_->makeCurrent(surface_);
		auto* f = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_2_1>(context_);
		if (f)
			f->initializeOpenGLFunctions();

		cfg_ = cfg;
		cfg_.render_scale = TEE_ICON_SCALE; // 图标统一比例,不跟随用户缩放
		scene_ = std::make_unique<TeeScene>(cfg_, &queue_);
		if (!scene_->Init(f)) {
			context_->doneCurrent();
			return false;
		}
		fbo_ = new QOpenGLFramebufferObject(RENDER_SIZE, RENDER_SIZE);
		context_->doneCurrent();
		return true;
	}

	void BeginBatch() {
		if (context_ && !in_batch_) {
			context_->makeCurrent(surface_);
			in_batch_ = true;
		}
	}

	// 渲染指定皮肤文件的站立 Tee(直通 alpha)。必须位于 Begin/EndBatch 之间。
	QImage Grab(const QString& skin_path) {
		if (!context_ || !fbo_ || !in_batch_)
			return {};
		if (skin_path != last_skin_path_) {
			if (!scene_->ReloadSkinTexture(skin_path))
				return {};
			last_skin_path_ = skin_path;
		}
		fbo_->bind();
		scene_->Render(RENDER_SIZE, RENDER_SIZE);
		QImage img = fbo_->toImage();
		fbo_->release();
		if (img.format() != QImage::Format_RGBA8888)
			img = img.convertToFormat(QImage::Format_RGBA8888);
		UnpremultiplyInPlace(img);
		return img;
	}

	void EndBatch() {
		if (context_ && in_batch_) {
			context_->doneCurrent();
			in_batch_ = false;
		}
	}

private:
	AppConfig cfg_;
	InputQueue queue_; // 永不投递事件:Tee 保持 idle 站立、默认朝右
	QOffscreenSurface* surface_ = nullptr;
	QOpenGLContext* context_ = nullptr;
	QOpenGLFramebufferObject* fbo_ = nullptr;
	std::unique_ptr<TeeScene> scene_;
	QString last_skin_path_;
	bool in_batch_ = false;
};

} // namespace

struct SkinPickerDialog::Impl {
	AppConfig cfg;
	QString skins_dir;
	QStringList all_skins;
	QString selected;      // 回传给调用方的文件名(已应用前缀解析);未选择为空
	QString selected_row;  // 当前选中对应的网格行文件名(基皮,不含前缀)
	QString prefix;        // 当前皮肤名前缀(无下划线),空串表示无前缀

	QLabel* preview_label = nullptr;
	QLineEdit* name_edit = nullptr;
	QLineEdit* search_edit = nullptr;
	QLineEdit* prefix_edit = nullptr;
	QPushButton* btn_kitty = nullptr;
	QPushButton* btn_santa = nullptr;
	QLabel* count_label = nullptr;
	QLabel* warn_label = nullptr;
	QLabel* empty_label = nullptr;
	QScrollArea* scroll = nullptr;
	QWidget* grid_host = nullptr;
	QGridLayout* grid = nullptr;
	QButtonGroup* group = nullptr;
	QToolButton* initial_btn = nullptr;

	SkinIconRenderer renderer;
	bool gl_ok = false;

	struct Entry {
		QString file;      // 网格行对应的文件名(如 coala.png / santa_coala.png)
		QString path;      // 实际渲染的绝对路径(应用前缀解析后)
		QString cache_key;
		QToolButton* btn;
	};
	std::vector<Entry> entries;

	QTimer lazy_timer;
	unsigned generation = 0; // 重建网格的代际令牌(旧时间片不会写入新网格)
};

SkinPickerDialog::SkinPickerDialog(const AppConfig& cfg, QWidget* parent)
	: QDialog(parent)
	, impl_(std::make_unique<Impl>())
{
	auto& I = *impl_;
	I.cfg = cfg;
	I.skins_dir = cfg.ResolvedSkinsDir();
	I.all_skins = ScanSkinFiles(I.skins_dir);

	setWindowTitle(tr("选择皮肤"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(720, 600);

	auto* root = new QVBoxLayout(this);

	// ---- 第一行:"你的皮肤"(预览 + 名字) ... "皮肤名称前缀" ----
	auto* top_row = new QHBoxLayout;
	root->addLayout(top_row);

	top_row->addWidget(new QLabel(tr("你的皮肤:"), this));

	I.preview_label = new QLabel(this);
	I.preview_label->setFixedSize(PREVIEW_SIZE, PREVIEW_SIZE);
	I.preview_label->setFrameShape(QFrame::StyledPanel);
	I.preview_label->setAlignment(Qt::AlignCenter);
	top_row->addWidget(I.preview_label);

	I.name_edit = new QLineEdit(this);
	I.name_edit->setPlaceholderText(tr("输入皮肤名后回车直接使用"));
	I.name_edit->setClearButtonEnabled(false);
	I.name_edit->setMinimumWidth(200);
	top_row->addWidget(I.name_edit, 1);

	auto* prefix_box = new QGroupBox(tr("皮肤名称前缀"), this);
	auto* prefix_lay = new QVBoxLayout(prefix_box);
	prefix_lay->setContentsMargins(8, 6, 8, 8);
	I.prefix_edit = new QLineEdit(prefix_box);
	I.prefix_edit->setPlaceholderText(tr("无前缀"));
	I.prefix_edit->setClearButtonEnabled(true);
	prefix_lay->addWidget(I.prefix_edit);
	auto* prefix_btns = new QHBoxLayout;
	I.btn_kitty = new QPushButton(QStringLiteral("kitty"), prefix_box);
	I.btn_santa = new QPushButton(QStringLiteral("santa"), prefix_box);
	I.btn_kitty->setCheckable(true);
	I.btn_santa->setCheckable(true);
	prefix_btns->addWidget(I.btn_kitty);
	prefix_btns->addWidget(I.btn_santa);
	prefix_lay->addLayout(prefix_btns);
	top_row->addWidget(prefix_box);

	// ---- 第二行:搜索过滤 + 计数 ----
	auto* search_row = new QHBoxLayout;
	search_row->addWidget(new QLabel(tr("搜索:"), this));
	I.search_edit = new QLineEdit(this);
	I.search_edit->setPlaceholderText(tr("输入皮肤名过滤..."));
	I.search_edit->setClearButtonEnabled(true);
	search_row->addWidget(I.search_edit, 1);
	I.count_label = new QLabel(this);
	I.count_label->setToolTip(QDir::toNativeSeparators(I.skins_dir));
	search_row->addWidget(I.count_label);
	root->addLayout(search_row);

	I.warn_label = new QLabel(this);
	I.warn_label->setStyleSheet(QStringLiteral("color:#a00;"));
	I.warn_label->setWordWrap(true);
	I.warn_label->hide();
	root->addWidget(I.warn_label);

	I.empty_label = new QLabel(this);
	I.empty_label->setAlignment(Qt::AlignCenter);
	I.empty_label->hide();
	root->addWidget(I.empty_label);

	I.scroll = new QScrollArea(this);
	I.scroll->setWidgetResizable(true);
	root->addWidget(I.scroll, 1);

	I.grid_host = new QWidget;
	I.grid = new QGridLayout(I.grid_host);
	I.grid->setSpacing(6);
	I.group = new QButtonGroup(I.grid_host); // 默认互斥
	I.scroll->setWidget(I.grid_host);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	root->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// 渲染器只需在 assets 就位时初始化一次,之后换肤只换皮肤纹理
	const bool assets_ok =
		QFileInfo::exists(cfg.ResolvedAssetsDir() + QStringLiteral("/game.png")) &&
		QFileInfo::exists(cfg.ResolvedAssetsDir() + QStringLiteral("/emoticons.png"));
	I.gl_ok = assets_ok && I.renderer.Init(cfg);
	if (!I.gl_ok) {
		I.warn_label->setText(
			tr("Tee 预览不可用(assets 缺少 game.png/emoticons.png,或 OpenGL 初始化失败),\n"
			   "以下仅按文件名列出,仍可选择。"));
		I.warn_label->show();
	}

	// 懒渲染时间片:滚动停止/网格重建后合并触发一次,随后按预算续期
	I.lazy_timer.setSingleShot(true);
	connect(&I.lazy_timer, &QTimer::timeout, this, [this] {
		auto& II = *impl_;
		if (!II.gl_ok)
			return;
		const unsigned gen = II.generation;

		// 视口在网格坐标系中的可见矩形,向上下各扩预渲染行
		const int row_h = 138;
		QRect vis = II.grid_host->visibleRegion().boundingRect();
		if (vis.isEmpty()) {
			// 窗口尚未完成首次布局:先准备最上面两行
			vis = QRect(0, 0, II.grid_host->width() > 0 ? II.grid_host->width() : 600,
						ROW_LOOKAHEAD * row_h);
		}
		const QRect expanded = vis.adjusted(0, -ROW_LOOKAHEAD * row_h,
											0, ROW_LOOKAHEAD * row_h);

		struct Job {
			Impl::Entry* entry;
			int score;
		};
		std::vector<Job> jobs;
		for (Impl::Entry& e : II.entries) {
			if (CacheStore().contains(e.cache_key))
				continue; // 已有缓存(按钮在重建时已挂图标)
			const QRect g = e.btn->geometry();
			if (g.isNull() || !g.intersects(expanded))
				continue;
			const bool visible = g.intersects(vis);
			const int dist = std::abs(g.center().y() - vis.center().y());
			jobs.push_back({&e, (visible ? 0 : 100000) + dist});
		}
		if (jobs.empty())
			return;
		std::sort(jobs.begin(), jobs.end(),
				  [](const Job& a, const Job& b) { return a.score < b.score; });

		II.renderer.BeginBatch();
		const auto t_start = std::chrono::steady_clock::now();
		bool did_any = false;
		bool more = false;
		for (const Job& job : jobs) {
			if (gen != II.generation)
				break; // 网格在本时间片期间被重建(保险)
			if (did_any) {
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t_start).count();
				if (elapsed >= FRAME_BUDGET_MS) {
					more = true;
					break;
				}
			}
			const QImage img = II.renderer.Grab(job.entry->path);
			if (!img.isNull()) {
				const QIcon icon(QPixmap::fromImage(img));
				CacheStore().insert(job.entry->cache_key, {job.entry->cache_key, icon});
				job.entry->btn->setIcon(icon);
				did_any = true;
			}
		}
		II.renderer.EndBatch();

		// 仍有待渲染项:尽快续期;否则停止,等下一次滚动/搜索/前缀切换再唤醒
		if (gen == II.generation && more)
			II.lazy_timer.start(0);
	});

	connect(I.scroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
			[this] { impl_->lazy_timer.start(30); });
	connect(I.search_edit, &QLineEdit::textChanged, this, [this](const QString&) {
		RebuildGrid();
	});

	// 前缀:输入框文本即前缀(可任意输入,变体不存在时自然不生效);
	// 清除按钮清空 → 无前缀。
	connect(I.prefix_edit, &QLineEdit::textChanged, this, [this](const QString&) {
		ApplyPrefix();
	});
	// kitty / santa 快捷按钮:再次点击已激活的按钮相当于取消前缀。
	for (int i = 0; i < KnownPrefixes().size(); ++i) {
		const QString p = KnownPrefixes()[i];
		QPushButton* btn = (i == 0) ? I.btn_kitty : I.btn_santa;
		connect(btn, &QPushButton::clicked, this, [this, p] {
			auto& II = *impl_;
			{
				const QSignalBlocker blocker(II.prefix_edit);
				if (II.prefix == p)
					II.prefix_edit->clear();
				else
					II.prefix_edit->setText(p);
			}
			ApplyPrefix();
		});
	}

	// "你的皮肤"名字框:输入完整皮肤名后【回车】→ 精确选中并关闭;
	// 失焦仅把文本还原为当前选中名,绝不触发 accept —— 否则点击其他控件
	// (如前缀按钮)造成的失焦会先于按钮点击关窗。
	const auto find_exact = [this](const QString& typed) {
		auto& II = *impl_;
		for (const QString& f : II.all_skins) {
			if (QFileInfo(f).completeBaseName().compare(typed, Qt::CaseInsensitive) == 0)
				return f;
		}
		return QString();
	};
	connect(I.name_edit, &QLineEdit::returnPressed, this, [this, find_exact] {
		auto& II = *impl_;
		const QString match = find_exact(II.name_edit->text().trimmed());
		if (match.isEmpty())
			return; // 无精确匹配:不关闭,等待用户继续输入
		II.selected_row = match;
		II.selected = ResolveFile(match);
		{
			const QSignalBlocker blocker(II.name_edit);
			II.name_edit->setText(QFileInfo(match).completeBaseName());
		}
		UpdatePreview();
		accept();
	});
	connect(I.name_edit, &QLineEdit::editingFinished, this, [this, find_exact] {
		auto& II = *impl_;
		const QString typed = II.name_edit->text().trimmed();
		const QString match = find_exact(typed);
		const QString cur = II.selected_row.isEmpty() ? II.cfg.skin : II.selected_row;
		// 文本已等于当前皮肤名(大小写归一)则无需改动;否则还原。
		if (!match.isEmpty() &&
			QFileInfo(cur).completeBaseName().compare(
				QFileInfo(match).completeBaseName(), Qt::CaseInsensitive) == 0)
			return;
		const QSignalBlocker blocker(II.name_edit);
		II.name_edit->setText(QFileInfo(cur).completeBaseName());
	});

	// 初始状态:从当前皮肤文件名中识别已知前缀
	// (如 santa_coala.png → 前缀 santa + 选中行 coala.png)
	I.selected_row = I.cfg.skin;
	if (!I.all_skins.contains(I.selected_row, Qt::CaseSensitive))
		I.selected_row.clear(); // 当前皮肤不在列表中(如文件缺失):无选中行
	for (const QString& p : KnownPrefixes()) {
		const QString head = p + QLatin1Char('_');
		if (I.cfg.skin.startsWith(head, Qt::CaseInsensitive)) {
			const QString base = I.cfg.skin.mid(head.size());
			if (I.all_skins.contains(base, Qt::CaseSensitive)) {
				I.prefix = p;
				I.selected_row = base;
			}
			break;
		}
	}
	{
		const QSignalBlocker blocker(I.prefix_edit);
		I.prefix_edit->setText(I.prefix);
	}
	{
		const QString cur = I.selected_row.isEmpty() ? I.cfg.skin : I.selected_row;
		const QSignalBlocker blocker(I.name_edit);
		I.name_edit->setText(QFileInfo(cur).completeBaseName());
	}

	RebuildGrid();
	ApplyPrefix(); // 点亮前缀按钮、刷新"你的皮肤"预览、安排首屏懒渲染
}

SkinPickerDialog::~SkinPickerDialog() = default;

QString SkinPickerDialog::ResolveFile(const QString& file) const
{
	const auto& I = *impl_;
	if (!I.prefix.isEmpty()) {
		const QString candidate = I.prefix + QLatin1Char('_') + file;
		if (QFileInfo::exists(I.skins_dir + QLatin1Char('/') + candidate))
			return candidate; // 有此前缀的变体:换用
	}
	return file; // 无变体(或无前缀):保持原样
}

QIcon SkinPickerDialog::EnsureIcon(const QString& path)
{
	auto& I = *impl_;
	if (!I.gl_ok)
		return {};
	const QString key = MakeCacheKey(path);
	const auto it = CacheStore().constFind(key);
	if (it != CacheStore().cend())
		return it->icon;
	I.renderer.BeginBatch();
	const QImage img = I.renderer.Grab(path);
	I.renderer.EndBatch();
	if (img.isNull())
		return {};
	QIcon icon(QPixmap::fromImage(img));
	CacheStore().insert(key, {key, icon});
	return icon;
}

void SkinPickerDialog::UpdatePreview()
{
	auto& I = *impl_;
	const QString file = I.selected_row.isEmpty() ? I.cfg.skin : ResolveFile(I.selected_row);
	const QString path = I.skins_dir + QLatin1Char('/') + file;

	QIcon icon;
	if (!file.isEmpty() && QFileInfo::exists(path))
		icon = EnsureIcon(path);
	if (icon.isNull()) {
		I.preview_label->clear();
		I.preview_label->setText(tr("无预览"));
		return;
	}
	const QPixmap pm = icon.pixmap(RENDER_SIZE, RENDER_SIZE);
	I.preview_label->setPixmap(
		pm.scaled(PREVIEW_SIZE, PREVIEW_SIZE, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void SkinPickerDialog::ApplyPrefix()
{
	auto& I = *impl_;
	I.prefix = I.prefix_edit->text().trimmed();

	I.btn_kitty->setChecked(I.prefix == QStringLiteral("kitty"));
	I.btn_santa->setChecked(I.prefix == QStringLiteral("santa"));

	if (!I.selected_row.isEmpty())
		I.selected = ResolveFile(I.selected_row);
	// 先刷当前皮肤预览:会把选中行的新图标写入缓存,下面重挂网格时可直接命中,
	// 避免选中格在懒渲染时间片启动前短暂空白。
	UpdatePreview();

	// 不重建网格:仅按新前缀重解每个条目的渲染目标,
	// 缓存命中立即换图,未命中则清空图标交给懒渲染。
	for (Impl::Entry& e : I.entries) {
		const QString resolved = ResolveFile(e.file);
		const QString new_path = I.skins_dir + QLatin1Char('/') + resolved;
		if (new_path == e.path)
			continue;
		e.path = new_path;
		e.cache_key = MakeCacheKey(new_path);
		const auto it = CacheStore().constFind(e.cache_key);
		e.btn->setIcon(it != CacheStore().cend() ? it->icon : QIcon());
	}

	I.lazy_timer.start(0);
}

void SkinPickerDialog::RebuildGrid()
{
	auto& I = *impl_;
	++I.generation; // 使任何尚未执行/在途的旧加载逻辑失效

	// 清空旧按钮(网格里只有按钮)。此处重建由搜索框/构造触发,不会来自按钮
	// 自身的信号,故直接同步 delete:deleteLater 的 DeferredDelete 事件可能因
	// Windows UIA SendMessage 回调的事件循环层级而迟迟不投递,导致旧按钮残留。
	while (QLayoutItem* item = I.grid->takeAt(0)) {
		if (QWidget* w = item->widget())
			delete w; // 同步地隐藏、移出 QButtonGroup 并释放
		delete item;
	}
	I.entries.clear();
	I.initial_btn = nullptr;

	const QString needle = I.search_edit->text().trimmed().toLower();
	const QFontMetrics fm(font());

	int row = 0;
	for (const QString& name : I.all_skins) {
		if (!needle.isEmpty() &&
			!QFileInfo(name).completeBaseName().toLower().contains(needle))
			continue;

		auto* btn = new QToolButton(I.grid_host);
		btn->setCheckable(true);
		btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
		btn->setIconSize(QSize(ICON_DISPLAY, ICON_DISPLAY));
		btn->setFixedSize(112, 132);
		btn->setText(fm.elidedText(QFileInfo(name).completeBaseName(),
								   Qt::ElideMiddle, 104));
		btn->setToolTip(name);

		// 网格行始终是基皮名;图标按当前前缀解析(有变体显示变体)。
		const QString resolved = ResolveFile(name);
		const QString path = I.skins_dir + QLatin1Char('/') + resolved;
		const QString key = MakeCacheKey(path);
		const auto cit = CacheStore().constFind(key);
		if (cit != CacheStore().cend())
			btn->setIcon(cit->icon); // 缓存命中:图标即时显示
		I.grid->addWidget(btn, row / COLS, row % COLS, Qt::AlignCenter);
		I.group->addButton(btn);
		I.entries.push_back({name, path, key, btn});

		if (name == I.selected_row) {
			btn->setChecked(true);
			I.initial_btn = btn;
		}
		connect(btn, &QToolButton::clicked, this, [this, name] {
			auto& II = *impl_;
			II.selected_row = name;
			II.selected = ResolveFile(name); // 回传前缀解析后的实际文件名
			{
				const QSignalBlocker blocker(II.name_edit);
				II.name_edit->setText(QFileInfo(name).completeBaseName());
			}
			UpdatePreview();
			accept();
		});
		++row;
	}

	const int matched = row;
	I.count_label->setText(
		tr("共 %1 个皮肤%2")
			.arg(I.all_skins.size())
			.arg(needle.isEmpty() ? QString() : tr("(匹配 %1)").arg(matched)));
	I.empty_label->setVisible(matched == 0);
	if (matched == 0)
		I.empty_label->setText(tr("没有匹配“%1”的皮肤。")
			.arg(I.search_edit->text().trimmed()));

	// 合并触发一次懒渲染(30ms 内的多次重建只产生一次)
	I.lazy_timer.start(30);
}

QString SkinPickerDialog::SelectedSkin() const
{
	return impl_->selected;
}

void SkinPickerDialog::showEvent(QShowEvent* e)
{
	QDialog::showEvent(e);
	// 打开时把当前选中皮肤滚动到可视区域,并立即为其附近安排渲染
	if (impl_->initial_btn) {
		impl_->scroll->ensureWidgetVisible(impl_->initial_btn, 0, 120);
		impl_->lazy_timer.start(0);
	}
}

} // namespace live2tee
