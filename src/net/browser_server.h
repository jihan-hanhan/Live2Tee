#pragma once

// 浏览器源输出:本地 WebSocket 推送 RGBA 帧。
// OBS 侧用"浏览器源"加载 assets/browser/live2tee.html?port=xxxx,
// 页面把帧 putImageData 到透明 canvas —— 真 alpha 直出,无需色键。
// 帧格式:4 字节魔数 'L2T1' + uint32 LE 宽 + uint32 LE 高 + RGBA8888 数据。

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include "../config.h"

class QWebSocket;
class QWebSocketServer;

namespace live2tee {

class BrowserServer : public QObject {
	Q_OBJECT
public:
	explicit BrowserServer(const AppConfig& cfg, QObject* parent = nullptr);

	bool Start(quint16 port);
	void Stop();
	bool IsRunning() const { return server_ != nullptr; }
	quint16 Port() const; // 运行中返回实际端口,否则返回配置端口

	// 给 OBS 浏览器源用的 URL(file:// 页面 + 端口 query)
	QString PageUrl() const;

public slots:
	void SendFrame(const QImage& frame);

private:
	void OnNewConnection();
	void OnDisconnected();

	AppConfig cfg_;
	QWebSocketServer* server_ = nullptr;
	QList<QWebSocket*> clients_;
};

} // namespace live2tee
