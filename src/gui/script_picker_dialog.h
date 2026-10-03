#pragma once

// 脚本选择子窗口(管理 assets/scripts 下的 Lua 行为脚本):
//   - 左列"已有的脚本":扫描 scripts 目录全部 *.lua,每行 名称 + add 按钮;
//   - 右列"已加载的脚本":当前启用列表(顺序 = 优先级,顶部最高),
//     每行 名称 + up / down / 移除 按钮;
//   - 右下 应用 / Cancel。
// 应用后由控制窗口立即保存 config 并热更新脚本(无需再点主窗口"应用")。

#include <QDialog>
#include <QStringList>

class QVBoxLayout;

namespace live2tee {

struct AppConfig;

class ScriptPickerDialog : public QDialog {
	Q_OBJECT
public:
	// cfg 提供 scripts 目录(ResolvedScriptsDir)与当前已加载列表
	// (cfg.behavior_scripts,用于初始化右列)。
	explicit ScriptPickerDialog(const AppConfig& cfg, QWidget* parent = nullptr);

	// 已加载列表(顺序 = 优先级,顶部最高);仅在 Accepted 时有意义。
	QStringList SelectedScripts() const;

private:
	void RebuildLists();                    // 按 available_/loaded_ 重建两列行
	void AddScript(const QString& name);    // 左 -> 右(追加到末尾)
	void RemoveScript(const QString& name); // 右 -> 左(按文件名排序插回)
	void MoveScript(int index, int dir);    // 右列内交换(dir = -1 上 / +1 下)

	QStringList available_; // 左列:可添加的脚本(排序)
	QStringList loaded_;    // 右列:已加载(顺序 = 优先级)
	QVBoxLayout* avail_lay_ = nullptr;
	QVBoxLayout* loaded_lay_ = nullptr;
};

} // namespace live2tee
