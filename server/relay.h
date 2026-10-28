#pragma once
// ============================================================================
//  StreamRelay
//
//  YouTube's audio URLs are fussy: the server drops long open-ended downloads,
//  throttles clients it doesn't like, and the URLs expire.  When the connection
//  breaks in the middle of a song the media backend's demuxer sees a truncated
//  file ("demuxing failed" / early end of media) and playback dies or skips.
//
//  The relay is a tiny HTTP server on 127.0.0.1.  QMediaPlayer is pointed at
//  http://127.0.0.1:<port>/s/<id> and the relay fetches the real stream in small
//  Range requests (the way yt-dlp does), with the headers yt-dlp asked for.
//  If a chunk fails it is simply requested again from the byte where it stopped;
//  the player never notices.  If the URL has expired the relay asks the app for
//  a fresh one (upstreamRejected -> refreshUpstream) and carries on seamlessly.
// ============================================================================
#include <QtCore>
#include <QtNetwork>

class RelayConn;

struct RelaySession
{
    using Headers = QVector<QPair<QByteArray, QByteArray>>;

    quint64    id = 0;
    QUrl       url;
    Headers    headers;
    qint64     total = -1;                    // size of the stream in bytes
    QByteArray mime = "audio/mp4";
    quint64    urlGen = 0;                    // bumps whenever the upstream URL is replaced
    bool       rejectReported = false;

    QNetworkRequest request(qint64 from, qint64 to) const
    {
        QNetworkRequest rq(url);
        for (const auto& h : headers) {
            const QByteArray n = h.first.toLower();
            if (n == "range" || n == "host" || n == "content-length" || n == "accept-encoding" || n == "connection")
                continue;
            rq.setRawHeader(h.first, h.second);
        }
        if (!rq.hasRawHeader("User-Agent"))
            rq.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0");
        rq.setRawHeader("Accept-Encoding", "identity");
        rq.setRawHeader("Range", "bytes=" + QByteArray::number(from) + "-" + QByteArray::number(to));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(15000);
        return rq;
    }
};

class StreamRelay : public QObject
{
    Q_OBJECT
public:
    explicit StreamRelay(QNetworkAccessManager* nam, QObject* parent = nullptr);   // defined below RelayConn

    QNetworkAccessManager* nam() const { return m_nam; }
    quint64 current() const { return m_sess ? m_sess->id : 0; }

    const RelaySession* session(quint64 id) const { return (m_sess && m_sess->id == id) ? m_sess.get() : nullptr; }

    // Registers an upstream stream.  Answers asynchronously with ready() or failed().
    quint64 open(const QString& upstreamUrl, const RelaySession::Headers& headers)
    {
        close();
        const quint64 id = ++m_next;
        auto s = std::make_unique<RelaySession>();
        s->id = id;
        s->url = QUrl(upstreamUrl);
        s->headers = headers;
        m_sess = std::move(s);

        if (!m_server.isListening() && !m_server.listen(QHostAddress::LocalHost, 0)) {
            QTimer::singleShot(0, this, [this, id] { emit failed(id, QStringLiteral("relay can't listen")); });
            return id;
        }

        // YouTube puts the size and type in the URL, which saves a round trip.
        const QUrlQuery q(m_sess->url);
        const qint64 clen = q.queryItemValue(QStringLiteral("clen")).toLongLong();
        const QString mime = q.queryItemValue(QStringLiteral("mime"), QUrl::FullyDecoded);
        if (mime.startsWith(QLatin1String("audio/"))) m_sess->mime = mime.toLatin1();
        if (clen > 0) {
            m_sess->total = clen;
            QTimer::singleShot(0, this, [this, id] { announce(id); });
        } else {
            probe(id);
        }
        return id;
    }

    // The upstream URL expired: swap in a fresh one; stalled connections resume by themselves.
    void refreshUpstream(quint64 id, const QString& url, const RelaySession::Headers& headers)
    {
        if (!m_sess || m_sess->id != id) return;
        m_sess->url = QUrl(url);
        if (!headers.isEmpty()) m_sess->headers = headers;
        ++m_sess->urlGen;
        m_sess->rejectReported = false;
        emit sessionRefreshed(id);
    }

    void close()
    {
        if (m_probe) { m_probe->disconnect(this); m_probe->abort(); m_probe->deleteLater(); m_probe = nullptr; }
        if (!m_sess) return;
        const quint64 id = m_sess->id;
        m_sess.reset();
        emit sessionClosed(id);
    }

    // Called by connections; emits upstreamRejected once per URL generation.
    void reportRejected(quint64 id, quint64 gen)
    {
        if (!m_sess || m_sess->id != id || m_sess->urlGen != gen || m_sess->rejectReported) return;
        m_sess->rejectReported = true;
        emit upstreamRejected(id);
    }

signals:
    void ready(quint64 session, const QUrl& local);
    void failed(quint64 session, const QString& why);
    void upstreamRejected(quint64 session);
    void sessionRefreshed(quint64 session);
    void sessionClosed(quint64 session);

private:
    void announce(quint64 id)
    {
        if (!m_sess || m_sess->id != id) return;
        emit ready(id, QUrl(QStringLiteral("http://127.0.0.1:%1/s/%2").arg(m_server.serverPort()).arg(id)));
    }

    // total size unknown: ask for one byte and read it from Content-Range
    void probe(quint64 id)
    {
        QNetworkReply* r = m_nam->get(m_sess->request(0, 0));
        m_probe = r;
        connect(r, &QNetworkReply::finished, this, [this, r, id] {
            if (m_probe == r) m_probe = nullptr;
            r->deleteLater();
            if (!m_sess || m_sess->id != id) return;
            const int st = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray cr = r->rawHeader("Content-Range");              // "bytes 0-0/12345"
            const int slash = cr.lastIndexOf('/');
            const qint64 total = slash >= 0 ? cr.mid(slash + 1).toLongLong() : 0;
            if (st != 206 || total <= 0) {
                emit failed(id, QStringLiteral("server doesn't support range requests (HTTP %1)").arg(st));
                return;
            }
            m_sess->total = total;
            const QByteArray ct = r->header(QNetworkRequest::ContentTypeHeader).toByteArray();
            if (ct.startsWith("audio/")) m_sess->mime = ct;
            announce(id);
        });
    }

    QNetworkAccessManager*        m_nam;
    QTcpServer                    m_server;
    std::unique_ptr<RelaySession> m_sess;
    QNetworkReply*                m_probe = nullptr;
    quint64                       m_next = 0;
};

// ============================================================================
//  One client connection (the player may open several, e.g. when it seeks)
// ============================================================================
class RelayConn : public QObject          // no Q_OBJECT: lambdas only
{
public:
    RelayConn(StreamRelay* r, QTcpSocket* s) : QObject(r), m_r(r), m_sock(s)
    {
        s->setParent(this);
        m_retry.setSingleShot(true);
        m_wait.setSingleShot(true);
        connect(&m_retry, &QTimer::timeout, this, [this] { pump(); });
        connect(&m_wait, &QTimer::timeout, this, [this] { m_sock->abort(); });   // couldn't get a fresh URL: let the player see it
        connect(s, &QTcpSocket::readyRead, this, [this] { onRead(); });
        connect(s, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        connect(s, &QTcpSocket::disconnected, this, [this] { deleteLater(); });
        connect(r, &StreamRelay::sessionClosed, this, [this](quint64 id) { if (id == m_sid) m_sock->abort(); });
        connect(r, &StreamRelay::sessionRefreshed, this, [this](quint64 id) {
            if (id != m_sid || !m_waiting) return;
            m_waiting = false;
            m_wait.stop();
            m_retries = 0;
            pump();
        });
    }

    ~RelayConn() override { dropReply(); }

private:
    enum State { Header, Streaming, Done };
    static constexpr qint64 kChunk      = 512 * 1024;     // size of each upstream request
    static constexpr qint64 kHighWater  = 768 * 1024;     // don't fetch more while this much is unsent
    static constexpr int    kMaxRetries = 6;

    // ---------------------------------------------------------- request
    void onRead()
    {
        if (m_state != Header) { m_sock->readAll(); return; }
        m_buf += m_sock->readAll();
        const int end = m_buf.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buf.size() > 16384) m_sock->abort();
            return;
        }
        handle(m_buf.left(end));
    }

    void reply(int code, const char* text, const QByteArray& extra = QByteArray())
    {
        QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + " " + text + "\r\n";
        h += extra;
        h += "Connection: close\r\n\r\n";
        m_sock->write(h);
    }

    void handle(const QByteArray& head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QByteArray method = first.value(0);
        const QByteArray path = first.value(1);

        QByteArray range;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            if (l.size() > 6 && l.left(6).toLower() == "range:") range = l.mid(6).trimmed();
        }

        if (method != "GET" && method != "HEAD") { reply(405, "Method Not Allowed", "Content-Length: 0\r\n"); m_sock->disconnectFromHost(); return; }
        if (!path.startsWith("/s/"))             { reply(404, "Not Found", "Content-Length: 0\r\n");          m_sock->disconnectFromHost(); return; }
        m_sid = path.mid(3).split('?').value(0).toULongLong();
        const RelaySession* s = m_r->session(m_sid);
        if (!s || s->total <= 0)                 { reply(404, "Not Found", "Content-Length: 0\r\n");          m_sock->disconnectFromHost(); return; }

        const qint64 total = s->total;
        qint64 a = 0, b = total - 1;
        bool partial = false;
        if (range.toLower().startsWith("bytes=")) {
            const QByteArray spec = range.mid(6).split(',').value(0).trimmed();
            const int dash = spec.indexOf('-');
            bool okA = true, okB = true;
            if (dash < 0) { okA = false; }
            else if (dash == 0) {                                     // suffix: last N bytes
                const qint64 n = spec.mid(1).toLongLong(&okB);
                if (okB && n > 0) a = qMax<qint64>(0, total - n); else okA = false;
            } else {
                a = spec.left(dash).toLongLong(&okA);
                const QByteArray tail = spec.mid(dash + 1);
                if (!tail.isEmpty()) b = qMin<qint64>(tail.toLongLong(&okB), total - 1);
            }
            if (!okA || !okB || a < 0 || a >= total || b < a) {
                reply(416, "Range Not Satisfiable",
                      "Content-Range: bytes */" + QByteArray::number(total) + "\r\nContent-Length: 0\r\n");
                m_sock->disconnectFromHost();
                return;
            }
            partial = true;
        }

        QByteArray extra = "Content-Type: " + s->mime + "\r\nAccept-Ranges: bytes\r\n"
                         + "Content-Length: " + QByteArray::number(b - a + 1) + "\r\n";
        if (partial)
            extra += "Content-Range: bytes " + QByteArray::number(a) + "-" + QByteArray::number(b)
                   + "/" + QByteArray::number(total) + "\r\n";
        reply(partial ? 206 : 200, partial ? "Partial Content" : "OK", extra);

        if (method == "HEAD") { m_state = Done; m_sock->disconnectFromHost(); return; }
        m_cursor = a;
        m_end = b;
        m_state = Streaming;
        pump();
    }

    // ---------------------------------------------------------- upstream
    void pump()
    {
        if (m_state != Streaming || m_reply || m_waiting || m_retry.isActive()) return;
        if (m_cursor > m_end) {
            m_state = Done;
            m_sock->disconnectFromHost();
            return;
        }
        if (m_sock->bytesToWrite() > kHighWater) return;               // bytesWritten() calls us again
        const RelaySession* s = m_r->session(m_sid);
        if (!s) { m_sock->abort(); return; }

        m_chunkEnd = qMin(m_end, m_cursor + kChunk - 1);
        m_gen = s->urlGen;
        m_got = 0;
        QNetworkReply* r = m_r->nam()->get(s->request(m_cursor, m_chunkEnd));
        m_reply = r;
        connect(r, &QNetworkReply::readyRead, this, [this, r] { if (m_reply == r) onData(); });
        connect(r, &QNetworkReply::finished,  this, [this, r] { if (m_reply == r) onChunkDone(); });
    }

    int status() const { return m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0; }

    void onData()
    {
        const int st = status();
        if (st == 0) return;                                           // headers not in yet
        if (st != 206) { chunkFailed(st); return; }                    // error page / range ignored: never forward it
        QByteArray d = m_reply->readAll();
        const qint64 room = m_chunkEnd - m_cursor + 1;
        if (d.size() > room) d.truncate(int(room));
        if (d.isEmpty()) return;
        m_sock->write(d);
        m_cursor += d.size();
        m_got += d.size();
    }

    void onChunkDone()
    {
        const int st = status();
        if (st == 206) onData();
        const bool complete = (st == 206 && m_reply->error() == QNetworkReply::NoError && m_cursor > m_chunkEnd);
        if (complete) {
            dropReply();
            m_retries = 0;
            pump();
        } else {
            chunkFailed(st);
        }
    }

    void chunkFailed(int st)
    {
        const bool madeProgress = m_got > 0;
        dropReply();
        if (m_state != Streaming) return;
        if (madeProgress) m_retries = 0;

        if (st == 401 || st == 403 || st == 404 || st == 410) {        // URL expired or rejected
            const RelaySession* s = m_r->session(m_sid);
            if (!s) { m_sock->abort(); return; }
            if (s->urlGen != m_gen) { m_retry.start(0); return; }      // already replaced: just try again
            m_waiting = true;
            m_wait.start(25000);
            m_r->reportRejected(m_sid, m_gen);
            return;
        }
        if (++m_retries > kMaxRetries) { m_sock->abort(); return; }
        m_retry.start(250 * (1 << (m_retries - 1)));                   // 250, 500, 1000 ... ms
    }

    void dropReply()
    {
        if (!m_reply) return;
        QNetworkReply* r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }

    StreamRelay*   m_r;
    QTcpSocket*    m_sock;
    QByteArray     m_buf;
    State          m_state = Header;
    quint64        m_sid = 0;
    qint64         m_cursor = 0, m_end = -1, m_chunkEnd = -1, m_got = 0;
    quint64        m_gen = 0;
    int            m_retries = 0;
    bool           m_waiting = false;
    QNetworkReply* m_reply = nullptr;
    QTimer         m_retry, m_wait;
};

inline StreamRelay::StreamRelay(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), m_nam(nam)
{
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* s = m_server.nextPendingConnection()) new RelayConn(this, s);
    });
}
