// 浏览器源输出实现:QWebSocketServer 推 RGBA 二进制帧。

#include "browser_server.h"

#include <QHostAddress>
#include <QImage>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>

#include <cstdio>
#include <cstring>

namespace live2tee {

BrowserServer::BrowserServer(const AppConfig& cfg, QObject* parent)
	: QObject(parent)
	, cfg_(cfg)
{
}

bool BrowserServer::Start(quint16 port)
{
	Stop();
	server_ = new QWebSocketServer(QStringLiteral("Live2Tee"),
								   QWebSocketServer::NonSecureMode, this);
	if (!server_->listen(QHostAddress::LocalHost, port)) {
		std::fprintf(stderr, "browser output: listen %u failed\n", static_cast<unsigned>(port));
		server_->deleteLater();
		server_ = nullptr;
		return false;
	}
	connect(server_, &QWebSocketServer::newConnection,
			this, &BrowserServer::OnNewConnection);
	std::printf("browser output: %s\n", PageUrl().toUtf8().constData());
	return true;
}

void BrowserServer::Stop()
{
	if (!server_)
		return;
	server_->close();
	// socket 是 server 的子对象,随 server 一起销毁
	clients_.clear();
	server_->deleteLater();
	server_ = nullptr;
}

quint16 BrowserServer::Port() const
{
	return server_ ? server_->serverPort() : static_cast<quint16>(cfg_.browser_port);
}

QString BrowserServer::PageUrl() const
{
	QUrl url = QUrl::fromLocalFile(
		cfg_.ResolvedAssetsDir() + QStringLiteral("/browser/live2tee.html"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("port"), QString::number(Port()));
	url.setQuery(query);
	return url.toString();
}

void BrowserServer::OnNewConnection()
{
	while (server_->hasPendingConnections()) {
		QWebSocket* sock = server_->nextPendingConnection();
		if (!sock)
			break;
		clients_.append(sock);
		connect(sock, &QWebSocket::disconnected,
				this, &BrowserServer::OnDisconnected);
	}
}

void BrowserServer::OnDisconnected()
{
	if (auto* sock = qobject_cast<QWebSocket*>(sender())) {
		clients_.removeAll(sock);
		sock->deleteLater();
	}
}

void BrowserServer::SendFrame(const QImage& frame)
{
	if (!server_ || clients_.isEmpty() || frame.isNull())
		return;

	QImage img = frame;
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	// RGBA8888 每像素 4 字节,行Stride天然 4 字节对齐,无 padding
	if (img.bytesPerLine() != img.width() * 4)
		img = img.convertToFormat(QImage::Format_RGBA8888); // 不应发生;兜底

	const int w = img.width();
	const int h = img.height();
	QByteArray payload;
	payload.resize(12 + w * h * 4);
	payload[0] = 'L';
	payload[1] = '2';
	payload[2] = 'T';
	payload[3] = '1';
	const quint32 wh[2] = {static_cast<quint32>(w), static_cast<quint32>(h)};
	std::memcpy(payload.data() + 4, wh, sizeof(wh)); // x86/ARM 均为小端
	std::memcpy(payload.data() + 12, img.constBits(),
				static_cast<size_t>(w) * h * 4);

	for (QWebSocket* client : clients_) {
		if (client)
			client->sendBinaryMessage(payload);
	}
}

} // namespace live2tee
