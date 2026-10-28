#pragma once
#include <QtCore>
#include <QtNetwork>
#include <functional>
#include <memory>
#include "online.h"

// ============================================================================
//  Google account: sign in with Google and read your YouTube playlists.
//
//  * OAuth 2.0 for desktop apps: the system browser opens Google's consent page,
//    Google redirects to http://127.0.0.1:<port>, and Pulsify swaps the code for
//    tokens (PKCE). Pulsify never sees your password.
//  * Only the read-only scope  youtube.readonly  is requested.
//  * Google requires every app to use its own OAuth client, so you create a free
//    "Desktop app" client once in Google Cloud Console and paste its ID/secret into
//    Pulsify (see README.txt). Pulsify cannot ship one for you.
//  * The refresh token and client details are kept in Pulsify's QSettings store
//    (plain text, per user). Sign out revokes the token and deletes them.
// ============================================================================

struct RemotePlaylist {
    QString id, title;
    int     count = -1;          // -1 = unknown
};

class GoogleAccount : public QObject
{
    Q_OBJECT
public:
    explicit GoogleAccount(QObject* parent = nullptr) : QObject(parent)
    {
        m_nam = new QNetworkAccessManager(this);
        m_signInTimer.setSingleShot(true);
        connect(&m_signInTimer, &QTimer::timeout, this, [this] { failSignIn(QStringLiteral("Sign-in timed out. Try again.")); });
        load();
        if (signedIn()) {
            if (m_avatarUrl.isEmpty()) QTimer::singleShot(0, this, [this] { refreshProfile(); });
            else downloadAvatar();
        }
    }

    static QString scope() { return QStringLiteral("https://www.googleapis.com/auth/youtube.readonly"); }

    bool hasCredentials() const { return !m_clientId.isEmpty() && !m_clientSecret.isEmpty(); }
    bool signedIn() const { return !m_refresh.isEmpty(); }
    bool signingIn() const { return m_server != nullptr; }
    QString clientId() const { return m_clientId; }
    QString clientSecret() const { return m_clientSecret; }
    QString accountName() const { return m_name; }
    QString avatarUrl() const { return m_avatarUrl; }

    // Re-reads the channel (name + profile picture); used when we are signed in but have no picture yet.
    void refreshProfile() { if (signedIn()) fetchAccountName(); }

    void setCredentials(const QString& id, const QString& secret)
    {
        const QString i = id.trimmed(), s = secret.trimmed();
        if (i != m_clientId || s != m_clientSecret) {
            // a token belongs to the client that issued it
            m_refresh.clear(); m_access.clear(); m_expiry = 0; m_name.clear(); m_avatarUrl.clear();
        }
        m_clientId = i;
        m_clientSecret = s;
        save();
        emit stateChanged();
    }

    // Reads the JSON file Google lets you download for an OAuth client.
    static bool parseCredentialsFile(const QString& path, QString& id, QString& secret)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QJsonObject o = root.value(QLatin1String("installed")).toObject();
        if (o.isEmpty()) o = root.value(QLatin1String("web")).toObject();
        id = o.value(QLatin1String("client_id")).toString();
        secret = o.value(QLatin1String("client_secret")).toString();
        return !id.isEmpty() && !secret.isEmpty();
    }

    // ---------------------------------------------------------------- sign in / out
    void signIn()
    {
        if (!hasCredentials()) { emit signInFailed(QStringLiteral("Add your Google client ID and secret first (Google setup).")); return; }
        cancelSignIn();
        m_server = new QTcpServer(this);
        if (!m_server->listen(QHostAddress::LocalHost, 0)) {
            const QString e = m_server->errorString();
            m_server->deleteLater();
            m_server = nullptr;
            emit signInFailed(QStringLiteral("Couldn't open a local port for the sign-in redirect: ") + e);
            return;
        }
        m_port = m_server->serverPort();
        m_verifier = randomToken(64);
        m_state = randomToken(24);
        connect(m_server, &QTcpServer::newConnection, this, &GoogleAccount::onConnection);

        QUrl url(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("client_id"), m_clientId);
        q.addQueryItem(QStringLiteral("redirect_uri"), redirectUri());
        q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        q.addQueryItem(QStringLiteral("scope"), scope());
        q.addQueryItem(QStringLiteral("state"), m_state);
        q.addQueryItem(QStringLiteral("code_challenge"), challengeFor(m_verifier));
        q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
        q.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
        q.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
        url.setQuery(q);

        m_signInTimer.start(3 * 60 * 1000);
        emit stateChanged();
        emit openBrowser(url.toString(QUrl::FullyEncoded));
    }

    void cancelSignIn()
    {
        m_signInTimer.stop();
        if (m_server) {
            m_server->close();
            m_server->deleteLater();
            m_server = nullptr;
            emit stateChanged();
        }
    }

    void signOut()
    {
        cancelAll();
        if (!m_refresh.isEmpty()) {                       // best effort: tell Google to forget the grant
            QNetworkRequest rq{ QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")) };
            rq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("token"), m_refresh);
            QNetworkReply* r = m_nam->post(rq, q.toString(QUrl::FullyEncoded).toUtf8());
            connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        }
        m_refresh.clear(); m_access.clear(); m_expiry = 0; m_name.clear(); m_avatarUrl.clear();
        save();
        emit stateChanged();
    }

    // Drops results of anything still in flight.
    void cancelAll() { ++m_epoch; cancelSignIn(); }

    // ---------------------------------------------------------------- playlists
    void fetchPlaylists()
    {
        const quint64 ep = ++m_epoch;
        auto st = std::make_shared<QVector<RemotePlaylist>>();
        // The channel tells us the account name and the id of the "Liked videos" playlist.
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,contentDetails"));
        q.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
        apiGet(QStringLiteral("channels"), q, [this, ep, st](const QJsonObject& o, const QString& err) {
            if (ep != m_epoch) return;
            if (!err.isEmpty()) { emit playlistsReady({}, err); return; }
            const QJsonArray items = o.value(QLatin1String("items")).toArray();
            if (!items.isEmpty()) {
                const QJsonObject ch = items.first().toObject();
                const QString name = ch.value(QLatin1String("snippet")).toObject().value(QLatin1String("title")).toString();
                setAvatarUrl(thumbUrl(ch.value(QLatin1String("snippet")).toObject()));
                if (!name.isEmpty() && name != m_name) { m_name = name; save(); emit stateChanged(); }
                const QString likes = ch.value(QLatin1String("contentDetails")).toObject()
                                        .value(QLatin1String("relatedPlaylists")).toObject()
                                        .value(QLatin1String("likes")).toString();
                if (!likes.isEmpty()) st->push_back({ likes, QStringLiteral("Liked videos"), -1 });
            }
            pagePlaylists(st, QString(), ep);
        });
    }

    // Every video of one playlist, in playlist order, with title / channel / duration.
    void fetchPlaylistItems(const QString& playlistId)
    {
        const quint64 ep = ++m_epoch;
        auto ids = std::make_shared<QStringList>();
        pageItems(playlistId, ids, QString(), ep);
    }

signals:
    void stateChanged();
    void avatarChanged();
    void openBrowser(const QString& url);
    void signInFailed(const QString& error);
    void playlistsReady(const QVector<RemotePlaylist>& lists, const QString& error);
    void itemsProgress(const QString& playlistId, int got, int total);
    void itemsReady(const QString& playlistId, const QVector<OnlineItem>& items, const QString& error);

private:
    // ------------------------------------------------------------ storage
    void load()
    {
        QSettings s;
        s.beginGroup(QStringLiteral("google"));
        m_clientId = s.value(QStringLiteral("clientId")).toString();
        m_clientSecret = s.value(QStringLiteral("clientSecret")).toString();
        m_refresh = s.value(QStringLiteral("refreshToken")).toString();
        m_name = s.value(QStringLiteral("accountName")).toString();
        m_avatarUrl = s.value(QStringLiteral("avatarUrl")).toString();
    }

    static QString thumbUrl(const QJsonObject& snippet)
    {
        const QJsonObject t = snippet.value(QLatin1String("thumbnails")).toObject();
        for (const char* k : { "medium", "high", "default" }) {
            const QString u = t.value(QLatin1String(k)).toObject().value(QLatin1String("url")).toString();
            if (!u.isEmpty()) return u;
        }
        return QString();
    }

    void setAvatarUrl(const QString& url)
    {
        if (url.isEmpty()) return;
        if (url == m_avatarUrl) return;
        m_avatarUrl = url;
        save();
        downloadAvatar();
    }

    void downloadAvatar() {}          // the web UI loads the picture itself from avatarUrl()

    void save() const
    {
        QSettings s;
        s.beginGroup(QStringLiteral("google"));
        s.setValue(QStringLiteral("clientId"), m_clientId);
        s.setValue(QStringLiteral("clientSecret"), m_clientSecret);
        s.setValue(QStringLiteral("refreshToken"), m_refresh);
        s.setValue(QStringLiteral("accountName"), m_name);
        s.setValue(QStringLiteral("avatarUrl"), m_avatarUrl);
    }

    // ------------------------------------------------------------ OAuth
    QString redirectUri() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port); }

    static QString randomToken(int bytes)
    {
        QByteArray b(bytes, 0);
        for (int i = 0; i < bytes; ++i) b[i] = char(QRandomGenerator::system()->bounded(256));
        return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    static QString challengeFor(const QString& verifier)
    {
        const QByteArray h = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
        return QString::fromLatin1(h.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    void failSignIn(const QString& msg)
    {
        cancelSignIn();
        emit signInFailed(msg);
    }

    void onConnection()
    {
        if (!m_server) return;
        QTcpSocket* sock = m_server->nextPendingConnection();
        if (!sock) return;
        sock->setParent(this);          // must outlive the server, which is torn down as soon as the code arrives
        auto buf = std::make_shared<QByteArray>();
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock, buf] {
            buf->append(sock->readAll());
            if (!buf->contains("\r\n\r\n") && buf->size() < 16384) return;      // wait for the full header
            const QList<QByteArray> parts = buf->left(buf->indexOf("\r\n")).split(' ');
            buf->clear();
            const QString target = parts.size() >= 2 ? QString::fromLatin1(parts[1]) : QString();
            const QUrlQuery q(QUrl(QStringLiteral("http://127.0.0.1") + target));
            const bool isCallback = q.hasQueryItem(QStringLiteral("code")) || q.hasQueryItem(QStringLiteral("error"));
            if (!isCallback) { reply(sock, 404, QStringLiteral("Not found")); return; }   // e.g. /favicon.ico

            const QString err = q.queryItemValue(QStringLiteral("error"));
            const QString code = q.queryItemValue(QStringLiteral("code"));
            const bool stateOk = (q.queryItemValue(QStringLiteral("state")) == m_state);
            if (!err.isEmpty() || code.isEmpty() || !stateOk) {
                reply(sock, 400, QStringLiteral("Sign-in was cancelled or failed. You can close this tab and return to Pulsify."));
                failSignIn(err == QLatin1String("access_denied")
                               ? QStringLiteral("Access was denied in the browser.")
                               : (!stateOk ? QStringLiteral("Sign-in response didn't match this request. Try again.")
                                           : QStringLiteral("Google returned an error: ") + err));
                return;
            }
            reply(sock, 200, QStringLiteral("You're signed in. You can close this tab and return to Pulsify."));
            const QString verifier = m_verifier, redirect = redirectUri();
            cancelSignIn();
            exchangeCode(code, verifier, redirect);
        });
    }

    static void reply(QTcpSocket* s, int status, const QString& message)
    {
        const QByteArray body = QByteArrayLiteral("<!doctype html><meta charset=utf-8><title>Pulsify</title>"
                                                  "<body style=\"font-family:sans-serif;background:#121212;color:#fff;"
                                                  "display:flex;align-items:center;justify-content:center;height:100vh\">"
                                                  "<p style=\"font-size:20px\">") + message.toHtmlEscaped().toUtf8()
                                + QByteArrayLiteral("</p></body>");
        const QByteArray head = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status)
                                + QByteArrayLiteral(" X\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ")
                                + QByteArray::number(body.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        s->write(head + body);
        s->disconnectFromHost();
    }

    void postToken(const QUrlQuery& form, std::function<void(const QJsonObject&, const QString&)> cb)
    {
        QNetworkRequest rq{ QUrl(QStringLiteral("https://oauth2.googleapis.com/token")) };
        rq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        rq.setTransferTimeout(20000);
        QNetworkReply* r = m_nam->post(rq, form.toString(QUrl::FullyEncoded).toUtf8());
        connect(r, &QNetworkReply::finished, this, [r, cb] {
            r->deleteLater();
            const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (r->error() != QNetworkReply::NoError && status == 0) { cb({}, r->errorString()); return; }
            if (status >= 400 || o.contains(QLatin1String("error"))) {
                const QString d = o.value(QLatin1String("error_description")).toString();
                cb(o, !d.isEmpty() ? d : o.value(QLatin1String("error")).toString());
                return;
            }
            cb(o, QString());
        });
    }

    void exchangeCode(const QString& code, const QString& verifier, const QString& redirect)
    {
        QUrlQuery f;
        f.addQueryItem(QStringLiteral("client_id"), m_clientId);
        f.addQueryItem(QStringLiteral("client_secret"), m_clientSecret);
        f.addQueryItem(QStringLiteral("code"), code);
        f.addQueryItem(QStringLiteral("code_verifier"), verifier);
        f.addQueryItem(QStringLiteral("redirect_uri"), redirect);
        f.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
        postToken(f, [this](const QJsonObject& o, const QString& err) {
            if (!err.isEmpty()) { emit signInFailed(QStringLiteral("Google rejected the sign-in: ") + err); return; }
            const QString refresh = o.value(QLatin1String("refresh_token")).toString();
            if (refresh.isEmpty()) {
                emit signInFailed(QStringLiteral("Google didn't return a refresh token. Remove Pulsify at "
                                                 "myaccount.google.com/permissions and sign in again."));
                return;
            }
            m_refresh = refresh;
            m_access = o.value(QLatin1String("access_token")).toString();
            m_expiry = QDateTime::currentSecsSinceEpoch() + qint64(o.value(QLatin1String("expires_in")).toInt(3600));
            m_name.clear(); m_avatarUrl.clear();
            save();
            emit stateChanged();
            fetchAccountName();
        });
    }

    void fetchAccountName()
    {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
        q.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
        apiGet(QStringLiteral("channels"), q, [this](const QJsonObject& o, const QString& err) {
            if (!err.isEmpty()) return;      // not fatal: the playlists call reports real problems
            const QJsonArray items = o.value(QLatin1String("items")).toArray();
            if (items.isEmpty()) return;
            const QJsonObject snip = items.first().toObject().value(QLatin1String("snippet")).toObject();
            m_name = snip.value(QLatin1String("title")).toString();
            setAvatarUrl(thumbUrl(snip));
            save();
            emit stateChanged();
        });
    }

    // Calls cb with a valid access token (refreshing it when needed).
    void withToken(std::function<void(const QString&, const QString&)> cb)
    {
        if (!signedIn()) { cb(QString(), QStringLiteral("Not signed in to Google.")); return; }
        if (!m_access.isEmpty() && QDateTime::currentSecsSinceEpoch() < m_expiry - 60) { cb(m_access, QString()); return; }
        m_waiters.push_back(cb);
        if (m_refreshing) return;
        m_refreshing = true;
        QUrlQuery f;
        f.addQueryItem(QStringLiteral("client_id"), m_clientId);
        f.addQueryItem(QStringLiteral("client_secret"), m_clientSecret);
        f.addQueryItem(QStringLiteral("refresh_token"), m_refresh);
        f.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        postToken(f, [this](const QJsonObject& o, const QString& err) {
            m_refreshing = false;
            QString token, error = err;
            if (err.isEmpty()) {
                token = o.value(QLatin1String("access_token")).toString();
                m_access = token;
                m_expiry = QDateTime::currentSecsSinceEpoch() + qint64(o.value(QLatin1String("expires_in")).toInt(3600));
            } else if (o.value(QLatin1String("error")).toString() == QLatin1String("invalid_grant")) {
                // revoked, or expired (apps in "Testing" mode lose their grant after 7 days)
                m_refresh.clear(); m_access.clear(); m_expiry = 0; m_name.clear(); m_avatarUrl.clear();
                save();
                emit stateChanged();
                error = QStringLiteral("Your Google sign-in expired. Please sign in again.");
            }
            const auto waiters = m_waiters;
            m_waiters.clear();
            for (const auto& w : waiters) w(token, error);
        });
    }

    // ------------------------------------------------------------ YouTube Data API
    static QString apiError(const QJsonObject& o, int status, const QString& fallback)
    {
        const QJsonObject e = o.value(QLatin1String("error")).toObject();
        const QString msg = e.value(QLatin1String("message")).toString();
        const QString reason = e.value(QLatin1String("errors")).toArray().isEmpty() ? QString()
            : e.value(QLatin1String("errors")).toArray().first().toObject().value(QLatin1String("reason")).toString();
        const QString low = msg.toLower();
        if (reason == QLatin1String("quotaExceeded") || low.contains(QStringLiteral("quota")))
            return QStringLiteral("YouTube's daily API quota for your Google project is used up. Try again tomorrow.");
        if (low.contains(QStringLiteral("has not been used")) || low.contains(QStringLiteral("is disabled"))
            || reason == QLatin1String("accessNotConfigured"))
            return QStringLiteral("Turn on \"YouTube Data API v3\" for your Google Cloud project, then try again.");
        if (reason == QLatin1String("insufficientPermissions") || low.contains(QStringLiteral("insufficient")))
            return QStringLiteral("Pulsify doesn't have permission to read your YouTube data. Sign out and sign in again.");
        if (reason == QLatin1String("playlistNotFound") || status == 404)
            return QStringLiteral("That playlist wasn't found (it may be private or deleted).");
        if (!msg.isEmpty()) return msg;
        return fallback.isEmpty() ? QStringLiteral("YouTube request failed (HTTP %1)").arg(status) : fallback;
    }

    void apiGet(const QString& path, const QUrlQuery& query,
                std::function<void(const QJsonObject&, const QString&)> cb, bool retried = false)
    {
        withToken([this, path, query, cb, retried](const QString& token, const QString& terr) {
            if (!terr.isEmpty()) { cb({}, terr); return; }
            QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/") + path);
            url.setQuery(query);
            QNetworkRequest rq(url);
            rq.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            rq.setTransferTimeout(30000);
            QNetworkReply* r = m_nam->get(rq);
            connect(r, &QNetworkReply::finished, this, [this, r, path, query, cb, retried] {
                r->deleteLater();
                const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
                if (status == 401 && !retried) {            // token rejected: force a refresh and try once more
                    m_access.clear(); m_expiry = 0;
                    apiGet(path, query, cb, true);
                    return;
                }
                if (r->error() != QNetworkReply::NoError || status >= 400) {
                    cb({}, apiError(o, status, status == 0 ? r->errorString() : QString()));
                    return;
                }
                cb(o, QString());
            });
        });
    }

    void pagePlaylists(std::shared_ptr<QVector<RemotePlaylist>> out, const QString& pageToken, quint64 ep)
    {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,contentDetails"));
        q.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
        q.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
        if (!pageToken.isEmpty()) q.addQueryItem(QStringLiteral("pageToken"), pageToken);
        apiGet(QStringLiteral("playlists"), q, [this, out, ep](const QJsonObject& o, const QString& err) {
            if (ep != m_epoch) return;
            if (!err.isEmpty()) { emit playlistsReady({}, err); return; }
            for (const QJsonValue& v : o.value(QLatin1String("items")).toArray()) {
                const QJsonObject p = v.toObject();
                RemotePlaylist rp;
                rp.id = p.value(QLatin1String("id")).toString();
                rp.title = p.value(QLatin1String("snippet")).toObject().value(QLatin1String("title")).toString();
                rp.count = p.value(QLatin1String("contentDetails")).toObject().value(QLatin1String("itemCount")).toInt(-1);
                if (!rp.id.isEmpty()) out->push_back(rp);
            }
            const QString next = o.value(QLatin1String("nextPageToken")).toString();
            if (!next.isEmpty() && out->size() < 500) pagePlaylists(out, next, ep);
            else emit playlistsReady(*out, QString());
        });
    }

    // Step 1: collect the video ids in order (cheap, 50 per call).
    void pageItems(const QString& plId, std::shared_ptr<QStringList> ids, const QString& pageToken, quint64 ep)
    {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("part"), QStringLiteral("contentDetails"));
        q.addQueryItem(QStringLiteral("playlistId"), plId);
        q.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
        if (!pageToken.isEmpty()) q.addQueryItem(QStringLiteral("pageToken"), pageToken);
        apiGet(QStringLiteral("playlistItems"), q, [this, plId, ids, ep](const QJsonObject& o, const QString& err) {
            if (ep != m_epoch) return;
            if (!err.isEmpty()) { emit itemsReady(plId, {}, err); return; }
            for (const QJsonValue& v : o.value(QLatin1String("items")).toArray()) {
                const QString vid = v.toObject().value(QLatin1String("contentDetails")).toObject()
                                        .value(QLatin1String("videoId")).toString();
                if (Yt::validId(vid)) ids->push_back(vid);
            }
            const int total = o.value(QLatin1String("pageInfo")).toObject().value(QLatin1String("totalResults")).toInt();
            emit itemsProgress(plId, int(ids->size()), total);
            const QString next = o.value(QLatin1String("nextPageToken")).toString();
            if (!next.isEmpty() && ids->size() < 5000) { pageItems(plId, ids, next, ep); return; }
            auto result = std::make_shared<QHash<QString, OnlineItem>>();
            pageVideos(plId, ids, 0, result, ep);
        });
    }

    // Step 2: titles, channels and durations, 50 videos per call. Videos that videos.list doesn't
    // return are private or deleted, so they are dropped instead of becoming dead playlist rows.
    void pageVideos(const QString& plId, std::shared_ptr<QStringList> ids, int from,
                    std::shared_ptr<QHash<QString, OnlineItem>> result, quint64 ep)
    {
        if (from >= ids->size()) {
            QVector<OnlineItem> out;
            QSet<QString> seen;
            for (const QString& id : *ids) {
                if (seen.contains(id)) continue;
                seen.insert(id);
                const auto it = result->constFind(id);
                if (it != result->constEnd()) out.push_back(it.value());
            }
            emit itemsReady(plId, out, QString());
            return;
        }
        const QStringList chunk = ids->mid(from, 50);
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,contentDetails"));
        q.addQueryItem(QStringLiteral("id"), chunk.join(QLatin1Char(',')));
        q.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
        apiGet(QStringLiteral("videos"), q, [this, plId, ids, from, result, ep](const QJsonObject& o, const QString& err) {
            if (ep != m_epoch) return;
            if (!err.isEmpty()) { emit itemsReady(plId, {}, err); return; }
            for (const QJsonValue& v : o.value(QLatin1String("items")).toArray()) {
                const QJsonObject vo = v.toObject();
                const QJsonObject sn = vo.value(QLatin1String("snippet")).toObject();
                if (sn.value(QLatin1String("liveBroadcastContent")).toString() == QLatin1String("live")) continue;
                QJsonObject j;                                     // same shape yt-dlp gives Yt::fromJson
                j.insert(QStringLiteral("id"), vo.value(QLatin1String("id")).toString());
                j.insert(QStringLiteral("title"), sn.value(QLatin1String("title")).toString());
                j.insert(QStringLiteral("channel"), sn.value(QLatin1String("channelTitle")).toString());
                j.insert(QStringLiteral("duration"),
                         isoSeconds(vo.value(QLatin1String("contentDetails")).toObject()
                                        .value(QLatin1String("duration")).toString()));
                const OnlineItem it = Yt::fromJson(j);
                if (it.valid()) result->insert(it.id, it);
            }
            pageVideos(plId, ids, from + 50, result, ep);
        });
    }

    // "PT1H2M3S" -> seconds
    static double isoSeconds(const QString& iso)
    {
        static const QRegularExpression re(
            QStringLiteral(R"(^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$)"));
        const auto m = re.match(iso);
        if (!m.hasMatch()) return 0;
        return m.captured(1).toDouble() * 86400 + m.captured(2).toDouble() * 3600
             + m.captured(3).toDouble() * 60 + m.captured(4).toDouble();
    }

    QNetworkAccessManager* m_nam = nullptr;
    QTcpServer* m_server = nullptr;
    QTimer      m_signInTimer;
    quint16     m_port = 0;
    QString     m_verifier, m_state;

    QString m_clientId, m_clientSecret, m_refresh, m_access, m_name, m_avatarUrl;
    qint64  m_expiry = 0;
    bool    m_refreshing = false;
    QVector<std::function<void(const QString&, const QString&)>> m_waiters;
    quint64 m_epoch = 0;
};
