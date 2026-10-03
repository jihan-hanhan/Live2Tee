// AppConfig 实现:JSON 读写 + 默认路径解析 + 热键解析。

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>

#include <algorithm>

namespace live2tee {

QStringList ScanSkinFiles(const QString& skins_dir)
{
	QDir dir(skins_dir);
	if (!dir.exists())
		return {};
	return dir.entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
}

QStringList ScanLuaScripts(const QString& scripts_dir)
{
	QDir dir(scripts_dir);
	if (!dir.exists())
		return {};
	return dir.entryList({QStringLiteral("*.lua")}, QDir::Files, QDir::Name);
}

QString AppConfig::ConfigPath() const
{
#if defined(__linux__)
	// 便携模式:exe 同级 config.json 已存在则沿用(向后兼容,避免旧配置丢失)
	const QString portable = QCoreApplication::applicationDirPath() + QStringLiteral("/config.json");
	if (QFileInfo::exists(portable))
		return portable;
	// XDG: ~/.config/Live2Tee/config.json
	const QString xdg = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	if (!xdg.isEmpty())
		return xdg + QStringLiteral("/config.json");
#endif
	// Windows / 便携模式回退
	return QCoreApplication::applicationDirPath() + QStringLiteral("/config.json");
}

QString AppConfig::BundledAssetsDir()
{
	// 便携模式:exe 同级 assets/
	const QString portable = QCoreApplication::applicationDirPath() + QStringLiteral("/assets");
	if (QFileInfo::exists(portable + QStringLiteral("/game.png")))
		return portable;
	// 安装模式:<prefix>/share/live2tee/assets/
	const QString installed = QDir(QCoreApplication::applicationDirPath() +
		QStringLiteral("/../share/live2tee/assets")).absolutePath();
	if (QFileInfo::exists(installed + QStringLiteral("/game.png")))
		return installed;
	return portable; // 回退(可能不存在,由调用方处理)
}

QString AppConfig::ResolvedAssetsDir() const
{
	if (!assets_dir.isEmpty())
		return QDir(assets_dir).absolutePath();
#if defined(__linux__)
	// 便携模式:exe 同级 assets 存在则直接用(开发/解压即用)
	const QString portable = QCoreApplication::applicationDirPath() + QStringLiteral("/assets");
	if (QFileInfo::exists(portable + QStringLiteral("/game.png")))
		return portable;
	// XDG 数据目录:~/.local/share/Live2Tee/assets/(首次运行由 main 复制初始化)
	const QString xdg = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (!xdg.isEmpty())
		return xdg + QStringLiteral("/assets");
#endif
	return QCoreApplication::applicationDirPath() + QStringLiteral("/assets");
}

QString AppConfig::ResolvedSkinsDir() const
{
	if (!skins_dir.isEmpty())
		return QDir(skins_dir).absolutePath();
	return ResolvedAssetsDir() + QStringLiteral("/skins");
}

QString AppConfig::ResolvedScriptsDir() const
{
	return ResolvedAssetsDir() + QStringLiteral("/scripts");
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
	// 行为脚本列表:字段缺失时默认 behavior.lua(向后兼容旧配置);
	// 显式存空数组 = 用户主动清空,回退内置默认行为
	if (obj.contains(QStringLiteral("behavior_scripts"))) {
		const QJsonArray arr = obj.value(QStringLiteral("behavior_scripts")).toArray();
		for (const QJsonValue& v : arr) {
			const QString name = v.toString().trimmed();
			if (!name.isEmpty())
				cfg.behavior_scripts << name;
		}
	} else {
		cfg.behavior_scripts = {QStringLiteral("behavior.lua")};
	}
	cfg.output_size = obj.value(QStringLiteral("output_size")).toInt(cfg.output_size);
	cfg.output_fps = obj.value(QStringLiteral("output_fps")).toInt(cfg.output_fps);
	cfg.browser_output = obj.value(QStringLiteral("browser_output")).toBool(cfg.browser_output);
	cfg.browser_port = obj.value(QStringLiteral("browser_port")).toInt(cfg.browser_port);
	cfg.preview_window = obj.value(QStringLiteral("preview_window")).toBool(cfg.preview_window);
	cfg.green_screen = obj.value(QStringLiteral("green_screen")).toBool(false);
	cfg.render_scale = static_cast<float>(
		obj.value(QStringLiteral("render_scale")).toDouble(cfg.render_scale));
	cfg.gui_hotkey = obj.value(QStringLiteral("gui_hotkey")).toString(cfg.gui_hotkey);
	cfg.auto_reanchor = obj.value(QStringLiteral("auto_reanchor")).toBool(cfg.auto_reanchor);
	cfg.reanchor_interval_ms = obj.value(QStringLiteral("reanchor_interval_ms")).toInt(cfg.reanchor_interval_ms);

	cfg.output_size = std::clamp(cfg.output_size, 128, 2048);
	cfg.output_fps = std::clamp(cfg.output_fps, 1, 120);
	cfg.browser_port = std::clamp(cfg.browser_port, 1024, 65535);
	cfg.render_scale = std::clamp(cfg.render_scale, 0.2f, 5.0f);
	cfg.reanchor_interval_ms = std::clamp(cfg.reanchor_interval_ms, 50, 2000);
	return cfg;
}

bool AppConfig::Save() const
{
	QJsonObject obj;
	obj.insert(QStringLiteral("assets_dir"), assets_dir);
	obj.insert(QStringLiteral("skins_dir"), skins_dir);
	obj.insert(QStringLiteral("skin"), skin);
	obj.insert(QStringLiteral("behavior_scripts"), QJsonArray::fromStringList(behavior_scripts));
	obj.insert(QStringLiteral("output_size"), output_size);
	obj.insert(QStringLiteral("output_fps"), output_fps);
	obj.insert(QStringLiteral("browser_output"), browser_output);
	obj.insert(QStringLiteral("browser_port"), browser_port);
	obj.insert(QStringLiteral("preview_window"), preview_window);
	obj.insert(QStringLiteral("green_screen"), green_screen);
	obj.insert(QStringLiteral("render_scale"), static_cast<double>(render_scale));
	obj.insert(QStringLiteral("gui_hotkey"), gui_hotkey);
	obj.insert(QStringLiteral("auto_reanchor"), auto_reanchor);
	obj.insert(QStringLiteral("reanchor_interval_ms"), reanchor_interval_ms);

	const QString path = ConfigPath();
	// XDG 目录可能不存在,先创建
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(QJsonDocument(obj).toJson());
	return true;
}

} // namespace live2tee
