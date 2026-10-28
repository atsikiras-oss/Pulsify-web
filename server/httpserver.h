#pragma once
// ============================================================================
//  A very small HTTP/1.1 server for the Pulsify web UI (127.0.0.1 only).
//
//   * one request per connection ("Connection: close"), except SSE which stays open
//   * JSON helpers, static files from Qt resources, Range-aware file streaming
//   * Server-Sent Events: HttpServer::broadcast("name", json) reaches every open page
//   * protections against other web pages talking to it: Host/Origin checks, and every
//     state-changing request must carry the custom header  X-Pulsify
// ============================================================================
#include <QtCore>
#include <QtNetwork>
#include <functional>

struct HttpRequest {
    QByteArray method;
    QString    path;                       // decoded, no query
    QStringList parts;                     // path segments, decoded
    QUrlQuery  query;
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;

    QString q(const char* k) const { return query.queryItemValue(QString::fromLatin1(k), QUrl::FullyDecoded); }
    bool    has(const char* k) const { return query.hasQueryItem(QString::fromLatin1(k)); }
    QByteArray header(const char* k) const { return headers.value(QByteArray(k).toLower()); }
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

class HttpServer;

class HttpConn : public QObject
{
public:
    HttpConn(HttpServer* srv, QTcpSocket* s);
    ~HttpConn() override;

    bool alive() const { return m_sock && m_sock->state() == QAbstractSocket::ConnectedState && !m_done; }

    void send(int code, const QByteArray& ctype, const QByteArray& body, const QByteArray& extra = QByteArray());
    void json(const QJsonValue& v, int code = 200);
    void error(int code, const QString& msg);
    void startSse();
    void sendSse(const QString& event, const QByteArray& data);
    void sendFile(const QString& path, const QByteArray& mime, const QByteArray& rangeHeader, bool head);
    void sendResource(const QString& resPath);

private:
    friend class HttpServer;
    void onRead();
    void dispatch();
    void pump();
    void finish();

    HttpServer*  m_srv;
    QTcpSocket*  m_sock;
    QByteArray   m_buf;
    HttpRequest  m_req;
    bool         m_headerDone = false;
    bool         m_dispatched = false;
    qint64       m_need = 0;
    bool         m_done = false;
    bool         m_sse = false;
    QFile*       m_file = nullptr;
    qint64       m_left = 0;
};

class HttpServer : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<void(HttpConn*, const HttpRequest&)>;

    explicit HttpServer(QObject* parent = nullptr) : QObject(parent)
    {
        connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_tcp.nextPendingConnection()) new HttpConn(this, s);
        });
        m_beat.setInterval(20000);
        connect(&m_beat, &QTimer::timeout, this, [this] {
            for (HttpConn* c : std::as_const(m_sse)) c->sendSse(QString(), QByteArray());   // comment ping
        });
        m_beat.start();
    }

    bool listen(quint16 port) { return m_tcp.listen(QHostAddress::LocalHost, port); }
    quint16 port() const { return m_tcp.serverPort(); }
    int sseClients() const { return int(m_sse.size()); }
    void setHandler(Handler h) { m_handler = std::move(h); }

    void broadcast(const QString& event, const QJsonValue& v)
    {
        QByteArray data;
        if (v.isObject())      data = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
        else if (v.isArray())  data = QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
        else                   data = QJsonDocument(QJsonArray{ v }).toJson(QJsonDocument::Compact);
        for (HttpConn* c : std::as_const(m_sse)) c->sendSse(event, data);
    }

    bool originOk(const HttpRequest& r) const
    {
        const QByteArray host = r.header("host");
        const QByteArray p = QByteArray::number(port());
        if (host != "127.0.0.1:" + p && host != "localhost:" + p) return false;       // DNS-rebinding guard
        const QByteArray origin = r.header("origin");
        if (!origin.isEmpty() && origin != "http://127.0.0.1:" + p && origin != "http://localhost:" + p) return false;
        const bool mutating = (r.method != "GET" && r.method != "HEAD");
        if (mutating && r.header("x-pulsify").isEmpty()) return false;
        return true;
    }

private:
    friend class HttpConn;
    QTcpServer m_tcp;
    Handler    m_handler;
    QSet<HttpConn*> m_sse;
    QTimer     m_beat;
};

// ---------------------------------------------------------------------------------------------
inline HttpConn::HttpConn(HttpServer* srv, QTcpSocket* s) : QObject(srv), m_srv(srv), m_sock(s)
{
    s->setParent(this);
    connect(s, &QTcpSocket::readyRead, this, [this] { onRead(); });
    connect(s, &QTcpSocket::bytesWritten, this, [this] { pump(); });
    connect(s, &QTcpSocket::disconnected, this, [this] { deleteLater(); });
}

inline HttpConn::~HttpConn()
{
    m_srv->m_sse.remove(this);
    delete m_file;
}

inline void HttpConn::onRead()
{
    if (m_sse || m_done || m_dispatched) { m_sock->readAll(); return; }
    m_buf += m_sock->readAll();
    if (!m_headerDone) {
        const int end = m_buf.indexOf("\r\n\r\n");
        if (end < 0) { if (m_buf.size() > 32768) m_sock->abort(); return; }
        const QList<QByteArray> lines = m_buf.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        if (first.size() < 2) { m_sock->abort(); return; }
        m_req.method = first[0];
        const QUrl u = QUrl::fromEncoded("http://x" + first[1]);
        m_req.path = QUrl::fromPercentEncoding(u.path(QUrl::FullyEncoded).toUtf8());
        m_req.query = QUrlQuery(u);
        for (const QString& p : m_req.path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) m_req.parts << p;
        for (int i = 1; i < lines.size(); ++i) {
            const int c = lines[i].indexOf(':');
            if (c > 0) m_req.headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
        }
        m_need = m_req.header("content-length").toLongLong();
        if (m_need < 0 || m_need > 8 * 1024 * 1024) { error(413, QStringLiteral("Too large")); return; }
        m_buf.remove(0, end + 4);
        m_headerDone = true;
    }
    if (m_buf.size() < m_need) return;
    m_req.body = m_buf.left(int(m_need));
    m_dispatched = true;
    dispatch();
}

inline void HttpConn::dispatch()
{
    if (!m_srv->originOk(m_req)) { error(403, QStringLiteral("Forbidden")); return; }
    if (!m_srv->m_handler) { error(404, QStringLiteral("Not found")); return; }
    m_srv->m_handler(this, m_req);
}

inline void HttpConn::send(int code, const QByteArray& ctype, const QByteArray& body, const QByteArray& extra)
{
    if (!m_sock || m_done) return;
    static const QHash<int, const char*> text = {
        { 200, "OK" }, { 204, "No Content" }, { 206, "Partial Content" }, { 400, "Bad Request" },
        { 403, "Forbidden" }, { 404, "Not Found" }, { 405, "Method Not Allowed" }, { 413, "Payload Too Large" },
        { 416, "Range Not Satisfiable" }, { 500, "Internal Server Error" }, { 502, "Bad Gateway" }
    };
    QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + " " + text.value(code, "OK") + "\r\n";
    h += "Content-Type: " + ctype + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    h += "X-Content-Type-Options: nosniff\r\n";
    if (!extra.contains("Cache-Control")) h += "Cache-Control: no-store\r\n";
    h += extra + "Connection: close\r\n\r\n";
    m_sock->write(h);
    if (m_req.method != "HEAD") m_sock->write(body);
    finish();
}

inline void HttpConn::json(const QJsonValue& v, int code)
{
    QByteArray b;
    if (v.isObject())     b = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    else if (v.isArray()) b = QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    else                  b = "null";
    send(code, "application/json; charset=utf-8", b);
}

inline void HttpConn::error(int code, const QString& msg)
{
    json(QJsonObject{ { QStringLiteral("error"), msg } }, code);
}

inline void HttpConn::finish()
{
    m_done = true;
    if (m_sock) m_sock->disconnectFromHost();
}

inline void HttpConn::startSse()
{
    m_sse = true;
    m_srv->m_sse.insert(this);
    m_sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                  "X-Accel-Buffering: no\r\nConnection: keep-alive\r\n\r\nretry: 2000\n\n");
}

inline void HttpConn::sendSse(const QString& event, const QByteArray& data)
{
    if (!alive()) return;
    if (event.isEmpty()) { m_sock->write(": ping\n\n"); return; }
    m_sock->write("event: " + event.toUtf8() + "\ndata: " + data + "\n\n");
}

inline void HttpConn::sendResource(const QString& resPath)
{
    QFile f(resPath);
    if (!f.open(QIODevice::ReadOnly)) { error(404, QStringLiteral("Not found")); return; }
    static const QHash<QString, QByteArray> mimes = {
        { QStringLiteral("html"), "text/html; charset=utf-8" }, { QStringLiteral("css"), "text/css; charset=utf-8" },
        { QStringLiteral("js"), "text/javascript; charset=utf-8" }, { QStringLiteral("json"), "application/json" },
        { QStringLiteral("svg"), "image/svg+xml" }, { QStringLiteral("png"), "image/png" },
        { QStringLiteral("ico"), "image/x-icon" }, { QStringLiteral("webmanifest"), "application/manifest+json" },
    };
    const QByteArray mime = mimes.value(QFileInfo(resPath).suffix().toLower(), "application/octet-stream");
    send(200, mime, f.readAll());
}

// Streams a local file; honours "Range: bytes=a-b" so <audio> can seek.
inline void HttpConn::sendFile(const QString& path, const QByteArray& mime, const QByteArray& range, bool head)
{
    auto* f = new QFile(path);
    if (!f->open(QIODevice::ReadOnly)) { delete f; error(404, QStringLiteral("File not found")); return; }
    const qint64 total = f->size();
    qint64 a = 0, b = total - 1;
    bool partial = false;
    if (range.toLower().startsWith("bytes=") && total > 0) {
        const QByteArray spec = range.mid(6).split(',').value(0).trimmed();
        const int dash = spec.indexOf('-');
        bool okA = dash >= 0, okB = true;
        if (okA && dash == 0) {
            const qint64 n = spec.mid(1).toLongLong(&okB);
            if (okB && n > 0) a = qMax<qint64>(0, total - n); else okA = false;
        } else if (okA) {
            a = spec.left(dash).toLongLong(&okA);
            const QByteArray tail = spec.mid(dash + 1);
            if (!tail.isEmpty()) b = qMin<qint64>(tail.toLongLong(&okB), total - 1);
        }
        if (!okA || !okB || a < 0 || a >= total || b < a) {
            delete f;
            send(416, "text/plain", QByteArray(), "Content-Range: bytes */" + QByteArray::number(total) + "\r\n");
            return;
        }
        partial = true;
    }
    QByteArray h = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    h += "Content-Type: " + mime + "\r\nAccept-Ranges: bytes\r\nContent-Length: " + QByteArray::number(b - a + 1) + "\r\n";
    if (partial)
        h += "Content-Range: bytes " + QByteArray::number(a) + "-" + QByteArray::number(b) + "/" + QByteArray::number(total) + "\r\n";
    h += "Cache-Control: private, max-age=3600\r\nConnection: close\r\n\r\n";
    m_sock->write(h);
    if (head || total == 0) { delete f; finish(); return; }
    f->seek(a);
    m_file = f;
    m_left = b - a + 1;
    pump();
}

inline void HttpConn::pump()
{
    if (!m_file || !m_sock) return;
    while (m_left > 0 && m_sock->bytesToWrite() < 512 * 1024) {
        const QByteArray chunk = m_file->read(qMin<qint64>(256 * 1024, m_left));
        if (chunk.isEmpty()) break;
        m_sock->write(chunk);
        m_left -= chunk.size();
    }
    if (m_left <= 0 || m_file->atEnd()) {
        delete m_file;
        m_file = nullptr;
        finish();
    }
}
