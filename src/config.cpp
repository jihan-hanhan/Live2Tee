// AppConfig 实现:JSON 读写 + 默认路径解析 + 热键解析。

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace live2tee {

QStringList ScanSkinFiles(const QString& skins_dir)
{
	QDir dir(skins_dir);
	if (!dir.exists())
		return {};
	return dir.entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
}

QString AppConfig::ConfigPath() const
{
	return QCoreApplication::applicationDirPath() + QStringLiteral("/config.json");
}

QString AppConfig::ResolvedAssetsDir() const
{
	if (!assets_dir.isEmpty())
		return QDir(assets_dir).absolutePath();
	return QCoreApplication::applicationDirPath() + QStringLiteral("/assets");
}

QString AppConfig::ResolvedSkinsDir() const
{
	if (!skins_dir.isEmpty())
		return QDir(skins_dir).absolutePath();
	return ResolvedAssetsDir() + QStringLiteral("/skins");
}

QString AppConfig::SkinPath() const
{
	if (skin.isEmpty())
		return QString();
	return ResolvedSkinsDir() + QStringLiteral("/") + skin;
}

int AppConfig::HotkeyVk() const
{
	// 仅支持 F1-F12(VK 0x70-0x7B);跨平台注意:热键检测在 Windows
	// 键盘钩子里,其他平台无实现,该值不产生副作用。
	const QString name = gui_hotkey.trimmed().toUpper();
	if (name.size() == 2 && name.startsWith(QLatin1Char('F'))) {
		bool ok = false;
		const int n = name.mid(1).toInt(&ok);
		if (ok && n >= 1 && n <= 12)
			return 0x70 + (n - 1);
	}
	return 0;
}

AppConfig AppConfig::Load()
{
	AppConfig cfg;
	QFile file(cfg.ConfigPath());
	if (!file.open(QIODevice::ReadOnly))
		return cfg;

	const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
	cfg.assets_dir = obj.value(QStringLiteral("assets_dir")).toString();
	cfg.skins_dir = obj.value(QStringLiteral("skins_dir")).toString();
	cfg.skin = obj.value(QStringLiteral("skin")).toString();
	cfg.output_size = obj.value(QStringLiteral("output_size")).toInt(cfg.output_size);
	cfg.output_fps = obj.value(QStringLiteral("output_fps")).toInt(cfg.output_fps);
	cfg.browser_output = obj.value(QStringLiteral("browser_output")).toBool(cfg.browser_output);
	cfg.browser_port = obj.value(QStringLiteral("browser_port")).toInt(cfg.browser_port);
	cfg.preview_window = obj.value(QStringLiteral("preview_window")).toBool(cfg.preview_window);
	cfg.green_screen = obj.value(QStringLiteral("green_screen")).toBool(false);
	cfg.render_scale = static_cast<float>(
		obj.value(QStringLiteral("render_scale")).toDouble(cfg.render_scale));
	cfg.gui_hotkey = obj.value(QStringLiteral("gui_hotkey")).toString(cfg.gui_hotkey);

	cfg.output_size = std::clamp(cfg.output_size, 128, 2048);
	cfg.output_fps = std::clamp(cfg.output_fps, 1, 120);
	cfg.browser_port = std::clamp(cfg.browser_port, 1024, 65535);
	cfg.render_scale = std::clamp(cfg.render_scale, 0.2f, 5.0f);
	return cfg;
}

bool AppConfig::Save() const
{
	QJsonObject obj;
	obj.insert(QStringLiteral("assets_dir"), assets_dir);
	obj.insert(QStringLiteral("skins_dir"), skins_dir);
	obj.insert(QStringLiteral("skin"), skin);
	obj.insert(QStringLiteral("output_size"), output_size);
	obj.insert(QStringLiteral("output_fps"), output_fps);
	obj.insert(QStringLiteral("browser_output"), browser_output);
	obj.insert(QStringLiteral("browser_port"), browser_port);
	obj.insert(QStringLiteral("preview_window"), preview_window);
	obj.insert(QStringLiteral("green_screen"), green_screen);
	obj.insert(QStringLiteral("render_scale"), static_cast<double>(render_scale));
	obj.insert(QStringLiteral("gui_hotkey"), gui_hotkey);

	QFile file(ConfigPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(QJsonDocument(obj).toJson());
	return true;
}

} // namespace live2tee
