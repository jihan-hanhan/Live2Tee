#pragma once

// 配置 GUI(独立窗口):
//   - assets / skins 目录(可自定义,带浏览按钮)
//   - 皮肤列表(自动扫描 skins 目录,下拉切换)
//   - 浏览器源输出:开关 / 端口 / 分辨率 / 帧率 / URL 复制
//   - 预览窗口开关、渲染缩放、背景(仅预览)
// 应用 = 保存 config.json + 热更新各组件;关闭 = 仅隐藏,可再次唤起。

#include <QWidget>

#include "../config.h"

class QLineEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;

namespace live2tee {

class ControlWindow : public QWidget {
	Q_OBJECT
public:
	explicit ControlWindow(const AppConfig& cfg, QWidget* parent = nullptr);

signals:
	// 用户点了"应用":携带新配置
	void ConfigApplied(const live2tee::AppConfig& cfg);

	// 用户点了"修正鼠标位置":重置虚拟鼠标偏移(朝向校准)
	void ResetVMouseRequested();

protected:
	void closeEvent(QCloseEvent* e) override; // 隐藏而非退出

private:
	void LoadFromConfig();
	void RefreshSkinList();           // 按当前 skins_dir 重新扫描
	void UpdatePageUrl();             // 按端口/资源目录刷新浏览器源 URL
	void OnApply();
	QString PageUrlFor(int port) const;

	AppConfig cfg_;
	QLineEdit* assets_edit_;
	QLineEdit* skins_edit_;
	QComboBox* skin_combo_;
	QDoubleSpinBox* scale_spin_;   // 渲染整体缩放
	QComboBox* bg_combo_;          // 0 = 透明, 1 = 绿幕(仅预览窗口)
	QCheckBox* browser_check_;     // 浏览器源输出开关
	QSpinBox* port_spin_;          // WebSocket 端口
	QComboBox* size_combo_;        // 输出分辨率
	QComboBox* fps_combo_;         // 输出帧率
	QLineEdit* url_edit_;          // OBS 浏览器源 URL(只读)
	QCheckBox* preview_check_;     // 预览窗口开关
};

} // namespace live2tee
