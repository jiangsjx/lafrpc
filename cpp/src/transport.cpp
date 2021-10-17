#include <QtCore/qdatetime.h>
#include <QtCore/qurl.h>
#include <QtCore/qloggingcategory.h>
#include "../include/transport.h"
#include "../include/rpc.h"
#include "../include/rpc_p.h"
#include "../include/peer.h"

static Q_LOGGING_CATEGORY(logger, "logger.transport")

using namespace qtng;

BEGIN_LAFRPC_NAMESPACE


Transport::~Transport() {}


void Transport::setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::DataChannel> channel)
{
    if (rpc.isNull()) {
        return;
    }
    channel->setMaxPacketSize(rpc->maxPacketSize());

    QSharedPointer<qtng::SslSocket> sslSocket = qtng::convertSocketLikeToSslSocket(request);
    if (!sslSocket.isNull()) {
        const QByteArray &certPEM = sslSocket->peerCertificate().save(qtng::Ssl::Pem);
        const QByteArray &certHash = sslSocket->peerCertificate().digest(qtng::MessageDigest::Sha256);
        if (!certPEM.isEmpty() && !certHash.isEmpty()) {
            channel->setProperty("peer_certificate", certPEM);
            channel->setProperty("peer_certificate_hash", certHash);
        }
    }
}


TcpTransport::TcpTransport(QPointer<Rpc> rpc)
    :Transport(rpc), operations(new qtng::CoroutineGroup)
{

}


TcpTransport::~TcpTransport()
{
    delete operations;
}


bool TcpTransport::parseAddress(const QString &address, QString &host, quint16 &port)
{
    if (!canHandle(address)) {
        return false;
    }
    QUrl u(address);
    if (!u.isValid() || u.port() <= 0) {
        return false;
    }
    host = u.host();
    if (host.isEmpty()) {
        return false;
    }
    port = static_cast<quint16>(u.port());
    return port > 0;
}


class TcpTransportRequestHandler: public BaseRequestHandler
{
protected:
    virtual void handle() override { userData<TcpTransport>()->handleRequest(request); }
    virtual void finish() override {}
};


QSharedPointer<qtng::SocketLike> TcpTransport::createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<Socket> s(Socket::createConnection(host, port, nullptr, dnsCache));
    if (!s.isNull()) {
        return asSocketLike(s);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> TcpTransport::createServer(const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new TcpServer<TcpTransportRequestHandler>(host, port));
    server->setUserData(this);
    return server;
}


bool TcpTransport::startServer(const QString &address)
{
    QString hostStr;
    quint16 port;
    bool valid = parseAddress(address, hostStr, port);
    if (!valid) {
        return false;
    }

    HostAddress host(hostStr);
    if (host.isNull()) {
        const QList<HostAddress> &l = RpcPrivate::getPrivateHelper(rpc.data())->dnsCache->resolve(hostStr);
        if (l.isEmpty()) {
            return false;
        }
        host = l.first();
    }

    QSharedPointer<qtng::BaseStreamServer> server = createServer(host, port);
    if (server.isNull()) {
        return false;
    }
    return server->serveForever();
}


void TcpTransport::handleRequest(QSharedPointer<qtng::SocketLike> request)
{
    if (rpc.isNull()) {
        qCDebug(logger) << "rpc is gone.";
        return;
    }
    request->setOption(qtng::Socket::LowDelayOption, true);
    const QByteArray header = request->recvall(2);
    if (header == QByteArray("\x4e\x67")) {
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        setupChannel(request, channel);
        QString address = getAddressTemplate();
        const HostAddress &peerAddress = request->peerAddress();
        if (peerAddress.protocol() == HostAddress::IPv6Protocol) {
            address = address.arg(QStringLiteral("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        QSharedPointer<Peer> peer = rpc->preparePeer(channel, QString(), address);
    } else if (header == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if (request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return;
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
    } else {
    }
}


QString TcpTransport::getAddressTemplate()
{
    return QStringLiteral("tcp://%1:%2");
}


QSharedPointer<qtng::DataChannel> TcpTransport::connect(const QString &address, float timeout)
{
    if (timeout == 0.0f) {
        timeout = 5.0f;
    }
    QString host;
    quint16 port;
    bool valid = parseAddress(address, host, port);
    if (!valid) {
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<SocketLike> request = createConnection(host, port, RpcPrivate::getPrivateHelper(rpc.data())->dnsCache);
    if (request.isNull()) {
        return QSharedPointer<qtng::DataChannel>();
    }
    request->setOption(qtng::Socket::LowDelayOption, true);
    qint64 sentBytes = request->sendall("\x4e\x67");
    if (sentBytes != 2) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::PositivePole));
    setupChannel(request, channel);
    return channel;
}


QSharedPointer<qtng::SocketLike> TcpTransport::makeRawSocket(const QString &address, QByteArray &connectionId)
{
    QString host;
    quint16 port;
    bool valid = parseAddress(address, host, port);
    if (!valid) {
        return QSharedPointer<qtng::SocketLike>();
    }
    QSharedPointer<SocketLike> request = createConnection(host, port, RpcPrivate::getPrivateHelper(rpc.data())->dnsCache);
    if (request.isNull()) {
        return request;
    }
    connectionId = randomBytes(16);
    QByteArray packet = connectionId;
    packet.prepend("\x33\x74");
    qint64 sentBytes = request->sendall(packet);
    if (sentBytes != packet.size()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    if (request->recvall(2) != "\xf3\x97") {
        return QSharedPointer<qtng::SocketLike>();
    } else {
        qCDebug(logger) << "raw socket handshake finished.";
    }
    return request;
}


QSharedPointer<qtng::SocketLike> TcpTransport::takeRawSocket(const QByteArray &connectionId)
{
    const RawSocket &rawConnection = rawConnections[connectionId];
    return rawConnection.connection;
}


bool TcpTransport::canHandle(const QString &address)
{
    return address.startsWith("tcp://");
}


QSharedPointer<qtng::SocketLike> SslTransport::createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<SslSocket> ssl(SslSocket::createConnection(host, port, config, nullptr, dnsCache));
    if (ssl) {
        return asSocketLike(ssl);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> SslTransport::createServer(const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new SslServer<TcpTransportRequestHandler>(host, port, config));
    server->setUserData(this);
    return server;
}


bool SslTransport::canHandle(const QString &address)
{
    return address.startsWith("ssl://");
}


QString SslTransport::getAddressTemplate()
{
    return QStringLiteral("ssl://%1:%2");
}


class KcpSocketWithFilter: public qtng::KcpSocket
{
public:
    KcpSocketWithFilter(qtng::HostAddress::NetworkLayerProtocol protocol, QPointer<Rpc> rpc);
    virtual bool filter(char *data, qint32 *len, HostAddress *addr, quint16 *port) override;
    QPointer<Rpc> rpc;
};


KcpSocketWithFilter::KcpSocketWithFilter(qtng::HostAddress::NetworkLayerProtocol protocol, QPointer<Rpc> rpc)
    : qtng::KcpSocket(protocol)
    , rpc(rpc) {}


bool KcpSocketWithFilter::filter(char *data, qint32 *len, HostAddress *addr, quint16 *port)
{
    if (rpc.isNull() || rpc->kcpFilter().isNull()) {
        return false;
    }
    return rpc->kcpFilter()->filter(this, data, len, addr, port);
}


class KcpServerWithFilter: public KcpServer<TcpTransportRequestHandler>
{
public:
    KcpServerWithFilter(const HostAddress &serverAddress, quint16 serverPort)
        : KcpServer<TcpTransportRequestHandler>(serverAddress, serverPort) {}
protected:
    virtual QSharedPointer<SocketLike> serverCreate() override;
};


QSharedPointer<SocketLike> KcpServerWithFilter::serverCreate()
{
    QPointer<Rpc> rpc = static_cast<KcpTransport*>(userData())->rpc;
    return asSocketLike(createServer<KcpSocketWithFilter>(serverAddress(), serverPort(), 0,
            [rpc] (HostAddress::NetworkLayerProtocol family) { return new KcpSocketWithFilter(family, rpc); }));
}


QSharedPointer<qtng::SocketLike> KcpTransport::createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<KcpSocket> kcp(qtng::createConnection<KcpSocket>(host, port, nullptr, dnsCache, HostAddress::AnyIPProtocol,
                [this] (HostAddress::NetworkLayerProtocol protocol) { return new KcpSocketWithFilter(protocol, rpc); }));
    if (kcp) {
        return asSocketLike(kcp);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> KcpTransport::createServer(const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new KcpServerWithFilter(host, port));
    server->setUserData(this);
    return server;
}


bool KcpTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp://");
}


QString KcpTransport::getAddressTemplate()
{
    return QStringLiteral("kcp://%1:%2");
}


typedef WithSsl<KcpServerWithFilter> SslKcpServerWithFilter;


QSharedPointer<qtng::SocketLike> KcpSslTransport::createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<KcpSocket> kcp(qtng::createConnection<KcpSocket>(host, port, nullptr, dnsCache, HostAddress::AnyIPProtocol,
                                               [this] (HostAddress::NetworkLayerProtocol protocol) { return new KcpSocketWithFilter(protocol, rpc); }));
    if (kcp) {
        QSharedPointer<SslSocket> ssl(new qtng::SslSocket(asSocketLike(kcp), config));
        if (!ssl->handshake(false)) {
            return QSharedPointer<SocketLike>();
        }
        return qtng::asSocketLike(ssl);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> KcpSslTransport::createServer(const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new SslKcpServerWithFilter(host, port, config));
    server->setUserData(this);
    return server;
}


bool KcpSslTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp+ssl://") || address.startsWith("ssl+kcp://");
}


QString KcpSslTransport::getAddressTemplate()
{
    return QStringLiteral("kcp+ssl://%1:%2");
}


struct LafrpcHttpData
{
    HttpTransport *transport;
    QString rpcPath;
};


class LafrpcHttpRequestHandler: public qtng::SimpleHttpRequestHandler
{
public:
    LafrpcHttpRequestHandler()
        :SimpleHttpRequestHandler(), closeRequest(true) {}
protected:
    virtual bool setup() override;
    virtual void doPOST() override;
    virtual void finish() override;
    virtual QByteArray tryToHandleMagicCode(bool *done) override;
private:
    HttpTransport *transport;
    QPointer<Rpc> rpc;
    QString rpcPath;
    bool closeRequest;
};


bool LafrpcHttpRequestHandler::setup()
{
    LafrpcHttpData *data = userData<LafrpcHttpData>();
    transport = data->transport;
    rpc = data->transport->rpc;
    rpcPath = data->rpcPath;
    rootDir = data->transport->rootDir;
    if (rpc.isNull() || rpcPath.isEmpty()) {
        return false;
    }
    return true;
}


void LafrpcHttpRequestHandler::doPOST()
{
    if (path != rpcPath) {
        return qtng::SimpleHttpRequestHandler::doPOST();
    }
    const QByteArray &connectionHeader = header(ConnectionHeader);
    if (connectionHeader.toLower() != "upgrade") {
        sendError(qtng::NotFound);
        return;
    }
    const QByteArray &upgradeHeader = header(UpgradeHeader);
    if (upgradeHeader.toLower() != "lafrpc") {
        sendError(qtng::NotFound);
        return;
    }
    if (rpc.isNull()) {
        sendError(qtng::ServiceUnavailable);
        return;
    }
    closeConnection = Yes;
    sendResponse(qtng::SwitchProtocol);
    if (!endHeader()) {
        return;
    }

    request->setOption(qtng::Socket::LowDelayOption, true);

    const QByteArray &rpcHeader = request->recvall(2);
    if (rpcHeader == QByteArray("\x4e\x67")) {
        if (rpc.isNull()) {
            qCDebug(logger) << "rpc is gone.";
            return;
        }
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        transport->setupChannel(request, channel);
        QString address;
        if (server->isSecure()) {
            address = QStringLiteral("https://%1:%2");
        } else {
            address = QStringLiteral("http://%1:%2");
        }
        const HostAddress &peerAddress = request->peerAddress();
        if (peerAddress.protocol() == HostAddress::IPv6Protocol) {
            address = address.arg(QStringLiteral("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        QSharedPointer<Peer> peer = rpc->preparePeer(channel, QString(), address);
        if (!peer.isNull()) {
            closeRequest = false;
        }
    } else if (rpcHeader == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if (request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return;
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        transport->rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
        closeRequest = false;
    } else {
    }
}


void LafrpcHttpRequestHandler::finish()
{
    if (closeRequest) {
        request->close();
    }
}


QByteArray LafrpcHttpRequestHandler::tryToHandleMagicCode(bool *done)
{
    *done = false;
    const QByteArray &rpcHeader = request->recvall(2);
    if (rpcHeader == QByteArray("\x4e\x67")) {
        closeConnection = Yes;
        if(rpc.isNull()) {
            qCDebug(logger) << "rpc is gone.";
            *done = true;
            return QByteArray();
        }
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        transport->setupChannel(request, channel);
        QString address;
        if (server->isSecure()) {
            address = QStringLiteral("https://%1:%2");
        } else {
            address = QStringLiteral("http://%1:%2");
        }
        const HostAddress &peerAddress = request->peerAddress();
        if (peerAddress.protocol() == HostAddress::IPv6Protocol) {
            address = address.arg(QStringLiteral("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        QSharedPointer<Peer> peer = rpc->preparePeer(channel, QString(), address);
        if (!peer.isNull()) {
            closeRequest = false;
        }
        *done = true;
        return QByteArray();
    } else if (rpcHeader == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if(request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return QByteArray();
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        transport->rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
        closeRequest = false;
        *done = true;
        return QByteArray();
    } else {
        *done = false;
        return rpcHeader;
    }
}


bool HttpTransport::startServer(const QString &address)
{
    QUrl u(address);
    if (!u.isValid()) {
        return false;
    }
    quint16 port;
    QSharedPointer<qtng::BaseStreamServer> server;
    HostAddress host = HostAddress(u.host());
    QString rpcPath = u.path();
    if (rpcPath.isEmpty()) {
        rpcPath = "/";
    }
    if (host.isNull()) {
        qCWarning(logger) << "require ip address to start http server.";
        return false;
    }

    if (u.scheme() == "https") {
        port = static_cast<quint16>(u.port(443));
        server.reset(new qtng::SslServer<LafrpcHttpRequestHandler>(host, port, config));
    } else {
        port = static_cast<quint16>(u.port(80));
        server.reset(new qtng::TcpServer<LafrpcHttpRequestHandler>(host, port));
    }
    LafrpcHttpData data;
    data.transport = this;
    data.rpcPath = rpcPath;
    server->setUserData(&data);
    return server->serveForever();
}


QSharedPointer<qtng::SocketLike> httpConnect(qtng::HttpSession &session, const QString &address)
{
    qtng::HttpRequest request("POST", address);
    request.setStreamResponse(true);
    request.addHeader("Connection", "Upgrade");
    request.addHeader("Upgrade", "lafrpc");
    qtng::HttpResponse response = session.send(request);
    if (!response.isOk()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    if (response.statusCode() != qtng::SwitchProtocol) {
        qCDebug(logger) << "server is a plain http server, while does not support lafrpc.";
        return QSharedPointer<qtng::SocketLike>();
    }
    QByteArray leftBytes;
    QSharedPointer<qtng::SocketLike> stream = response.takeStream(&leftBytes);
    if (Q_UNLIKELY(stream.isNull())) {
        qCWarning(logger) << "got invalid stream";
        return QSharedPointer<qtng::SocketLike>();
    }
    if (Q_UNLIKELY(!leftBytes.isEmpty())) {
        qCWarning(logger) << "the server should not send body.";
        return QSharedPointer<qtng::SocketLike>();
    }
    stream->setOption(qtng::Socket::LowDelayOption, true);
    return stream;
}


QSharedPointer<qtng::DataChannel> HttpTransport::connect(const QString &address, float )
{
    QSharedPointer<qtng::SocketLike> stream = httpConnect(session, address);
    if (stream.isNull()) {
        return QSharedPointer<qtng::DataChannel>();
    }
    qint64 sentBytes = stream->sendall("\x4e\x67");
    if(sentBytes != 2) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(stream, qtng::PositivePole));
    setupChannel(stream, channel);
    return channel;
}


QSharedPointer<qtng::SocketLike> HttpTransport::makeRawSocket(const QString &address, QByteArray &connectionId)
{
    QSharedPointer<qtng::SocketLike> stream = httpConnect(session, address);
    if (stream.isNull()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    connectionId = randomBytes(16);
    QByteArray packet = connectionId;
    packet.prepend("\x33\x74");
    qint64 sentBytes = stream->sendall(packet);
    if(sentBytes != packet.size()) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::SocketLike>();
    }
    if (stream->recvall(2) != "\xf3\x97") {
        return QSharedPointer<qtng::SocketLike>();
    } else {
        qCDebug(logger) << "raw socket handshake finished.";
    }
    return stream;
}


QSharedPointer<qtng::SocketLike> HttpTransport::takeRawSocket(const QByteArray &connectionId)
{
    const RawSocket &rawConnection = rawConnections[connectionId];
    return rawConnection.connection;
}


bool HttpTransport::canHandle(const QString &address)
{
    if (address.startsWith("https://", Qt::CaseInsensitive) || address.startsWith("http://", Qt::CaseInsensitive)) {
        return true;
    } else {
        return false;
    }
}


END_LAFRPC_NAMESPACE
