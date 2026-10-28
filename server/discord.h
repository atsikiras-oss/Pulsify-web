#pragma once
// ============================================================================
//  Discord Rich Presence for Pulsify
//
//  Talks to the running Discord client over its local IPC socket (a named pipe on
//  Windows, a unix socket on Linux/macOS). No external library needed.
//
//  Shows "Listening to Pulsify" with:
//    - song title (line 1) and artist (line 2)
//    - a live progress bar that stays in sync (start/end timestamps, re-synced on seek,
//      pause/resume, and when the stream stalls and drifts)
//    - the album cover: found online (Deezer, then iTunes) by artist + title so it works
//      for local files too; YouTube tracks fall back to their video thumbnail.
//
//  You need a Discord "Application" (free): https://discord.com/developers/applications
//  Name it "Pulsify" (that name is what Discord shows after "Listening to"), then copy its
//  Application ID into Settings -> Discord (or bake it in with -DPULSE_DISCORD_CLIENT_ID=...).
// ============================================================================
#include <QtCore>
#include <QtNetwork>
#include <QLocalSocket>

#ifndef PULSE_DISCORD_CLIENT_ID
#define PULSE_DISCORD_CLIENT_ID ""
#endif

// What the player tells the RPC about the current song. Position is "where the song is right now".
struct RpcTrack {
    bool    valid = false;          // false = nothing playing -> presence is cleared
    bool    playing = false;        // true = playing, false = paused / loading
    bool    paused = false;         // paused (shows "Paused"); neither playing nor paused = loading
    QString title, artist, album;
    qint64  durationMs = 0;
    qint64  positionMs = 0;
    QString fallbackImage;          // https URL used when no cover can be found online (e.g. YouTube thumbnail)
};

class DiscordRpc : public QObject
{
    Q_OBJECT
public:
    explicit DiscordRpc(QObject* parent = nullptr) : QObject(parent)
    {
        m_sock = new QLocalSocket(this);
        connect(m_sock, &QLocalSocket::connected, this, &DiscordRpc::onConnected);
        connect(m_sock, &QLocalSocket::disconnected, this, &DiscordRpc::onDisconnected);
        connect(m_sock, &QLocalSocket::readyRead, this, &DiscordRpc::onReadyRead);
        connect(m_sock, &QLocalSocket::errorOccurred, this, &DiscordRpc::onSockError);

        m_retry.setSingleShot(true);
        connect(&m_retry, &QTimer::timeout, this, [this] { tryConnect(); });

        m_flush.setSingleShot(true);
        connect(&m_flush, &QTimer::timeout, this, [this] { flush(); });

        m_artWait.setSingleShot(true);
        m_artWait.setInterval(2000);
        connect(&m_artWait, &QTimer::timeout, this, [this] { m_artWaiting = false; rebuild(); });

        reloadSettings();
    }

    ~DiscordRpc() override { shutdown(); }

    static QString defaultClientId() { return QStringLiteral(PULSE_DISCORD_CLIENT_ID); }

    QString status() const { return m_status; }
    bool    isConnected() const { return m_ready; }
    bool    isEnabled() const { return m_enabled; }

    // Reads discord/enabled and discord/clientId from QSettings and applies them.
    void reloadSettings()
    {
        QSettings s;
        const bool en = s.value(QStringLiteral("discord/enabled"), true).toBool();
        const QString id = s.value(QStringLiteral("discord/clientId"), defaultClientId()).toString().trimmed();
        const bool idChanged = (id != m_clientId);
        m_enabled = en;
        m_clientId = id;

        if (!m_enabled) {
            disconnectNow(true);
            setStatus(QStringLiteral("Off"), false);
            return;
        }
        if (m_clientId.isEmpty()) {
            disconnectNow(false);
            setStatus(QStringLiteral("Paste your Discord Application ID to turn this on"), false);
            return;
        }
        static const QRegularExpression digits(QStringLiteral("^\\d{15,25}$"));
        if (!digits.match(m_clientId).hasMatch()) {
            disconnectNow(false);
            setStatus(QStringLiteral("That doesn't look like an Application ID (it is a long number)"), false);
            return;
        }
        if (idChanged) disconnectNow(false);
        if (m_sock->state() == QLocalSocket::UnconnectedState) {
            setStatus(QStringLiteral("Looking for Discord..."), false);
            tryConnect();
        } else {
            m_sentFp.clear();
            rebuild();
        }
    }

    // Called by the player whenever something relevant changes. Cheap to call often:
    // identical states are ignored and updates are rate-limited for Discord.
    void setTrack(const RpcTrack& t)
    {
        m_track = t;
        if (!m_enabled || m_clientId.isEmpty()) return;

        if (t.valid) {
            const QString ak = artKeyFor(t);
            if (ak != m_artKey) {
                m_artKey = ak;
                m_artUrl.clear();
                m_artWaiting = false;
                if (!ak.isEmpty()) {
                    const auto hit = m_artCache.constFind(ak);
                    if (hit != m_artCache.constEnd()) {
                        m_artUrl = hit.value();
                    } else {
                        // Hold the first update back for a moment so the cover is there from the start
                        // instead of flashing a placeholder first.
                        m_artWaiting = true;
                        m_artWait.start();
                        lookupArt(ak, t.artist, t.title, 0);
                    }
                }
            }
        } else {
            m_artKey.clear();
            m_artUrl.clear();
            m_artWaiting = false;
            m_artWait.stop();
        }
        rebuild();
    }

    // Clears the presence and closes the connection (call on app exit).
    void shutdown()
    {
        m_enabled = false;
        m_retry.stop();
        m_flush.stop();
        m_artWait.stop();
        if (m_ready && m_sock->state() == QLocalSocket::ConnectedState) {
            sendActivity(QJsonValue(QJsonValue::Null));
            m_sock->waitForBytesWritten(300);
        }
        m_ready = false;
        m_sock->abort();
    }

signals:
    void statusChanged(const QString& text, bool connected);

private:
    // ------------------------------------------------------------------ connection
    void setStatus(const QString& s, bool connected)
    {
        if (s == m_status && connected == m_statusOk) return;
        m_status = s;
        m_statusOk = connected;
        emit statusChanged(s, connected);
    }

    void disconnectNow(bool /*clearFirst*/)
    {
        m_retry.stop();
        m_flush.stop();
        if (m_ready && m_sock->state() == QLocalSocket::ConnectedState) {
            sendActivity(QJsonValue(QJsonValue::Null));
            m_sock->waitForBytesWritten(200);
        }
        m_ready = false;
        m_connected = false;
        m_sock->abort();
        m_sentFp.clear();
    }

    QStringList candidates() const
    {
        QStringList out;
#ifdef Q_OS_WIN
        for (int i = 0; i < 10; ++i) out << QStringLiteral("discord-ipc-%1").arg(i);
#else
        QStringList dirs;
        for (const char* env : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" }) {
            const QString d = qEnvironmentVariable(env);
            if (!d.isEmpty() && !dirs.contains(d)) dirs << d;
        }
        if (!dirs.contains(QStringLiteral("/tmp"))) dirs << QStringLiteral("/tmp");
        const QStringList subs = {
            QString(), QStringLiteral("snap.discord/"), QStringLiteral("snap.discord-canary/"),
            QStringLiteral("app/com.discordapp.Discord/"), QStringLiteral("app/com.discordapp.DiscordCanary/")
        };
        for (int i = 0; i < 10; ++i)
            for (const QString& d : dirs)
                for (const QString& sub : subs) {
                    const QString p = QDir(d).filePath(sub + QStringLiteral("discord-ipc-%1").arg(i));
                    if (QFileInfo::exists(p)) out << p;
                }
#endif
        return out;
    }

    void tryConnect()
    {
        if (!m_enabled || m_clientId.isEmpty()) return;
        if (m_sock->state() != QLocalSocket::UnconnectedState) return;
        m_cands = candidates();
        m_candIdx = 0;
        m_connected = false;
        connectNext();
    }

    void connectNext()
    {
        if (m_candIdx >= m_cands.size()) {
            setStatus(QStringLiteral("Discord isn't running (will keep trying)"), false);
            m_retry.start(8000);
            return;
        }
        m_sock->abort();
        m_sock->connectToServer(m_cands.at(m_candIdx));
    }

    void onSockError(QLocalSocket::LocalSocketError)
    {
        if (m_connected) return;                    // after connecting, onDisconnected handles it
        ++m_candIdx;
        QTimer::singleShot(0, this, [this] { if (m_enabled && !m_connected) connectNext(); });
    }

    void onConnected()
    {
        m_connected = true;
        m_buf.clear();
        QJsonObject hs;
        hs.insert(QStringLiteral("v"), 1);
        hs.insert(QStringLiteral("client_id"), m_clientId);
        sendFrame(0, QJsonDocument(hs).toJson(QJsonDocument::Compact));
        setStatus(QStringLiteral("Connecting to Discord..."), false);
    }

    void onDisconnected()
    {
        const bool was = m_ready;
        m_ready = false;
        m_connected = false;
        m_sentFp.clear();
        m_flush.stop();
        if (m_enabled && !m_clientId.isEmpty()) {
            if (was || m_status.startsWith(QStringLiteral("Connect")))
                setStatus(QStringLiteral("Discord closed the connection (reconnecting)"), false);
            m_retry.start(m_enabledBackoff ? 30000 : 5000);
            m_enabledBackoff = false;
        }
    }

    void onReadyRead()
    {
        m_buf += m_sock->readAll();
        while (m_buf.size() >= 8) {
            const quint32 op  = qFromLittleEndian<quint32>(m_buf.constData());
            const quint32 len = qFromLittleEndian<quint32>(m_buf.constData() + 4);
            if (len > (1u << 20)) { m_sock->abort(); return; }
            if (m_buf.size() < qsizetype(8 + len)) break;
            const QByteArray payload = m_buf.mid(8, int(len));
            m_buf.remove(0, int(8 + len));
            handleFrame(op, payload);
        }
    }

    void handleFrame(quint32 op, const QByteArray& payload)
    {
        if (op == 3) { sendFrame(4, payload); return; }                  // ping -> pong
        const QJsonObject o = QJsonDocument::fromJson(payload).object();
        if (op == 2) {                                                   // Discord closed us
            const QString msg = o.value(QStringLiteral("message")).toString();
            setStatus(msg.isEmpty() ? QStringLiteral("Discord refused the connection")
                                    : QStringLiteral("Discord: ") + msg, false);
            m_enabledBackoff = true;
            m_sock->abort();
            return;
        }
        const QString evt = o.value(QStringLiteral("evt")).toString();
        if (evt == QLatin1String("READY")) {
            m_ready = true;
            const QString user = o.value(QStringLiteral("data")).toObject()
                                   .value(QStringLiteral("user")).toObject()
                                   .value(QStringLiteral("username")).toString();
            setStatus(user.isEmpty() ? QStringLiteral("Connected to Discord")
                                     : QStringLiteral("Connected as ") + user, true);
            m_sentFp.clear();
            rebuild();
            return;
        }
        if (evt == QLatin1String("ERROR")) {
            const QString msg = o.value(QStringLiteral("data")).toObject().value(QStringLiteral("message")).toString();
            setStatus(QStringLiteral("Discord error: ") + msg, m_ready);
        }
    }

    void sendFrame(quint32 op, const QByteArray& payload)
    {
        if (m_sock->state() != QLocalSocket::ConnectedState) return;
        QByteArray f(8, 0);
        qToLittleEndian(op, f.data());
        qToLittleEndian(quint32(payload.size()), f.data() + 4);
        f += payload;
        m_sock->write(f);
    }

    void sendActivity(const QJsonValue& activity)
    {
        QJsonObject args;
        args.insert(QStringLiteral("pid"), qint64(QCoreApplication::applicationPid()));
        if (!activity.isNull()) args.insert(QStringLiteral("activity"), activity);
        QJsonObject cmd;
        cmd.insert(QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY"));
        cmd.insert(QStringLiteral("args"), args);
        cmd.insert(QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        sendFrame(1, QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    }

    // ------------------------------------------------------------------ building the activity
    static QString clampText(QString s)
    {
        s = s.simplified();
        if (s.size() > 128) s = s.left(125) + QStringLiteral("...");
        while (s.size() < 2) s += QChar(0x200B);       // Discord rejects 1-character fields
        return s;
    }

    void rebuild()
    {
        if (!m_enabled || m_clientId.isEmpty()) return;
        if (m_artWaiting && m_track.valid) return;     // wait for the cover (max 2 s)

        QString fp;
        qint64 start = 0, end = 0;
        QJsonValue activity = QJsonValue(QJsonValue::Null);

        if (m_track.valid) {
            const RpcTrack& t = m_track;
            QJsonObject a;
            a.insert(QStringLiteral("type"), 2);                                    // "Listening to"
            a.insert(QStringLiteral("details"), clampText(t.title.isEmpty() ? QStringLiteral("Unknown title") : t.title));
            QString artist = t.artist.isEmpty() ? QStringLiteral("Unknown artist") : t.artist;
            if (t.paused) artist += QStringLiteral(" \u2022 Paused");
            a.insert(QStringLiteral("state"), clampText(artist));

            if (t.playing) {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                start = (nowMs - qMax<qint64>(0, t.positionMs)) / 1000;
                QJsonObject ts;
                ts.insert(QStringLiteral("start"), start);
                if (t.durationMs > 0) {
                    end = (nowMs - qMax<qint64>(0, t.positionMs) + t.durationMs) / 1000;
                    ts.insert(QStringLiteral("end"), end);
                }
                a.insert(QStringLiteral("timestamps"), ts);
            }

            QJsonObject assets;
            QString img = m_artUrl.isEmpty() ? t.fallbackImage : m_artUrl;
            if (img.isEmpty() || img.size() > 256 || !img.startsWith(QStringLiteral("https://")))
                img = QStringLiteral("pulsify");        // optional asset key (upload an image called "pulsify" in the Dev Portal)
            assets.insert(QStringLiteral("large_image"), img);
            const QString lt = !t.album.isEmpty() ? t.album : t.title;
            assets.insert(QStringLiteral("large_text"), clampText(lt));
            a.insert(QStringLiteral("assets"), assets);
            activity = a;

            fp = QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
                     .arg(t.title, t.artist, t.album, img)
                     .arg(t.playing ? 1 : 0).arg(t.paused ? 1 : 0).arg(t.durationMs / 1000);
        } else {
            fp = QStringLiteral("clear");
        }

        // Same song, same state? Only resend if the timeline drifted by 2 s or more.
        const auto sameAs = [&](const QString& otherFp, qint64 os, qint64 oe) {
            return fp == otherFp && qAbs(start - os) < 2 && qAbs(end - oe) < 2;
        };
        if (m_hasWant ? sameAs(m_wantFp, m_wantStart, m_wantEnd)
                      : (m_ready && sameAs(m_sentFp, m_sentStart, m_sentEnd)))
            return;

        m_want = activity;
        m_wantFp = fp; m_wantStart = start; m_wantEnd = end;
        m_hasWant = true;
        scheduleFlush();
    }

    void scheduleFlush()
    {
        if (!m_ready) return;                           // sent as soon as Discord says READY
        if (m_flush.isActive()) return;
        // Discord allows ~5 updates per 20 s; stay under that and always send the latest state.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        while (!m_sendTimes.isEmpty() && now - m_sendTimes.first() > 20000) m_sendTimes.removeFirst();
        qint64 wait = 0;
        if (m_sendTimes.size() >= 5) wait = m_sendTimes.first() + 20000 - now + 100;
        else if (!m_sendTimes.isEmpty()) wait = qMax<qint64>(0, m_sendTimes.last() + 400 - now);
        m_flush.start(int(qMax<qint64>(wait, 0)));
    }

    void flush()
    {
        if (!m_ready || !m_hasWant) return;
        sendActivity(m_want);
        m_sendTimes.append(QDateTime::currentMSecsSinceEpoch());
        m_sentFp = m_wantFp; m_sentStart = m_wantStart; m_sentEnd = m_wantEnd;
        m_hasWant = false;
    }

    // ------------------------------------------------------------------ cover lookup
    static QString norm(QString s)
    {
        static const QRegularExpression brackets(QStringLiteral("\\([^)]*\\)|\\[[^\\]]*\\]|\\{[^}]*\\}"));
        static const QRegularExpression feat(QStringLiteral("\\b(feat|ft|featuring)\\b.*$"));
        s = s.toLower();
        s.remove(brackets);
        s.remove(feat);
        QString o;
        o.reserve(s.size());
        for (const QChar c : s) o += c.isLetterOrNumber() ? c : QLatin1Char(' ');
        return o.simplified();
    }

    static QString cleanArtist(QString a)
    {
        static const QRegularExpression suffix(QStringLiteral("\\s*(-\\s*topic|vevo|official)\\s*$"),
                                               QRegularExpression::CaseInsensitiveOption);
        a = a.trimmed();
        a.remove(suffix);
        return a.trimmed();
    }

    // "Artist - Song (Official Video)" -> "Song"
    static QString cleanTitle(QString title, const QString& artist)
    {
        static const QRegularExpression brackets(QStringLiteral("\\([^)]*\\)|\\[[^\\]]*\\]"));
        title.remove(brackets);
        title = title.trimmed();
        const int dash = title.indexOf(QStringLiteral(" - "));
        if (dash > 0) {
            const QString head = norm(title.left(dash));
            const QString na = norm(artist);
            if (!na.isEmpty() && (head.contains(na) || na.contains(head)))
                title = title.mid(dash + 3).trimmed();
        }
        return title;
    }

    QString artKeyFor(const RpcTrack& t) const
    {
        const QString a = norm(cleanArtist(t.artist));
        const QString ti = norm(cleanTitle(t.title, t.artist));
        if (a.isEmpty() || ti.isEmpty()) return QString();   // without an artist a search would just guess
        return a + QLatin1Char('|') + ti;
    }

    static bool plausible(const QString& resTitle, const QString& resArtist,
                          const QString& qArtist, const QString& qTitle)
    {
        const QString rt = norm(resTitle), ra = norm(resArtist);
        const QString qt = norm(cleanTitle(qTitle, qArtist)), qa = norm(cleanArtist(qArtist));
        if (rt.size() < 2 || ra.isEmpty() || qt.isEmpty()) return false;
        const bool titleOk = qt.contains(rt) || rt.contains(qt);
        const QString hay = qa + QLatin1Char(' ') + norm(qTitle);
        const bool artistOk = (!qa.isEmpty() && (qa.contains(ra) || ra.contains(qa))) || hay.contains(ra);
        return titleOk && artistOk;
    }

    // stage 0: Deezer (strict), 1: Deezer (loose), 2: iTunes
    void lookupArt(const QString& key, const QString& artist, const QString& title, int stage)
    {
        const QString qa = cleanArtist(artist);
        const QString qt = cleanTitle(title, artist);
        QUrl url;
        if (stage == 0 || stage == 1) {
            url = QUrl(QStringLiteral("https://api.deezer.com/search"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("q"), stage == 0
                ? QStringLiteral("artist:\"%1\" track:\"%2\"").arg(qa, qt)
                : QStringLiteral("%1 %2").arg(qa, qt));
            q.addQueryItem(QStringLiteral("limit"), QStringLiteral("6"));
            url.setQuery(q);
        } else if (stage == 2) {
            url = QUrl(QStringLiteral("https://itunes.apple.com/search"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("term"), QStringLiteral("%1 %2").arg(qa, qt));
            q.addQueryItem(QStringLiteral("entity"), QStringLiteral("song"));
            q.addQueryItem(QStringLiteral("limit"), QStringLiteral("6"));
            url.setQuery(q);
        } else {
            finishArt(key, QString());
            return;
        }

        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(6000);
        QNetworkReply* rep = m_nam.get(req);
        connect(rep, &QNetworkReply::finished, this, [this, rep, key, artist, title, stage] {
            rep->deleteLater();
            QString found;
            if (rep->error() == QNetworkReply::NoError) {
                const QJsonObject root = QJsonDocument::fromJson(rep->readAll()).object();
                if (stage < 2) {
                    for (const QJsonValue& v : root.value(QStringLiteral("data")).toArray()) {
                        const QJsonObject o = v.toObject();
                        if (!plausible(o.value(QStringLiteral("title")).toString(),
                                       o.value(QStringLiteral("artist")).toObject().value(QStringLiteral("name")).toString(),
                                       artist, title)) continue;
                        const QJsonObject alb = o.value(QStringLiteral("album")).toObject();
                        QString c = alb.value(QStringLiteral("cover_xl")).toString();
                        if (c.isEmpty()) c = alb.value(QStringLiteral("cover_big")).toString();
                        if (!c.isEmpty() && c.size() <= 256) { found = c; break; }
                    }
                } else {
                    for (const QJsonValue& v : root.value(QStringLiteral("results")).toArray()) {
                        const QJsonObject o = v.toObject();
                        if (!plausible(o.value(QStringLiteral("trackName")).toString(),
                                       o.value(QStringLiteral("artistName")).toString(),
                                       artist, title)) continue;
                        QString c = o.value(QStringLiteral("artworkUrl100")).toString();
                        c.replace(QStringLiteral("100x100bb"), QStringLiteral("600x600bb"));
                        if (!c.isEmpty() && c.size() <= 256) { found = c; break; }
                    }
                }
            }
            if (found.isEmpty() && stage < 2) { lookupArt(key, artist, title, stage + 1); return; }
            finishArt(key, found);
        });
    }

    void finishArt(const QString& key, const QString& url)
    {
        m_artCache.insert(key, url);                    // empty = looked up, nothing found (don't ask again)
        if (key != m_artKey) return;                    // song changed while we were searching
        m_artUrl = url;
        m_artWait.stop();
        m_artWaiting = false;
        rebuild();
    }

    // ------------------------------------------------------------------ state
    QLocalSocket* m_sock = nullptr;
    QTimer        m_retry, m_flush, m_artWait;
    QNetworkAccessManager m_nam;
    QByteArray    m_buf;
    QStringList   m_cands;
    int           m_candIdx = 0;
    bool          m_connected = false, m_ready = false, m_enabled = false, m_enabledBackoff = false;
    QString       m_clientId, m_status, m_artKey, m_artUrl;
    bool          m_statusOk = false, m_artWaiting = false;
    RpcTrack      m_track;
    QHash<QString, QString> m_artCache;

    QJsonValue    m_want = QJsonValue(QJsonValue::Null);
    QString       m_wantFp, m_sentFp;
    qint64        m_wantStart = 0, m_wantEnd = 0, m_sentStart = 0, m_sentEnd = 0;
    bool          m_hasWant = false;
    QList<qint64> m_sendTimes;
};
