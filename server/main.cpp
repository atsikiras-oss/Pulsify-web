// ============================================================================
//  Pulsify server  -  the C++ half of Pulsify Web.
//
//  Everything that needs the operating system or has to hide from CORS lives here:
//    yt-dlp (search / resolve / install), the YouTube stream relay, the local library,
//    playlists + history, lyrics (Genius + LRCLIB), Deezer charts, Google sign-in,
//    Discord Rich Presence.  The browser UI (web/) talks to it over http://127.0.0.1:<port>.
//
//  Settings and data are shared with the old Qt desktop app (same QSettings keys,
//  same pulse-data.json), so your playlists carry over.
// ============================================================================
#include <QtCore>
#include <QtNetwork>
#include <cstdio>

#include "online.h"
#include "relay.h"
#include "playlists.h"
#include "google.h"
#include "discord.h"
#include "lyrics.h"
#include "library.h"
#include "httpserver.h"

namespace {

// ------------------------------------------------------------------ JSON helpers
QJsonObject itemJson(const OnlineItem& it)
{
    QJsonObject o = Yt::toJson(it);
    o.insert(QStringLiteral("thumb"), it.thumbUrl);
    return o;
}

QJsonArray itemsJson(const QVector<OnlineItem>& v)
{
    QJsonArray a;
    for (const OnlineItem& it : v) a.push_back(itemJson(it));
    return a;
}

QJsonObject streamJson(const StreamInfo& s)
{
    return QJsonObject{
        { QStringLiteral("container"), s.container }, { QStringLiteral("codec"), s.codec },
        { QStringLiteral("bitrate"), s.bitrateKbps }, { QStringLiteral("sampleRate"), s.sampleRate },
        { QStringLiteral("views"), double(s.views) }, { QStringLiteral("uploadDate"), s.uploadDate },
        { QStringLiteral("channel"), s.channel },     { QStringLiteral("webUrl"), s.webUrl },
    };
}

QJsonObject entryToWeb(const PlaylistEntry& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("online"), e.online);
    if (e.online) o.insert(QStringLiteral("videoId"), e.videoId);
    else          o.insert(QStringLiteral("path"), e.path);
    o.insert(QStringLiteral("title"), e.title);
    o.insert(QStringLiteral("artist"), e.artist);
    o.insert(QStringLiteral("album"), e.album);
    o.insert(QStringLiteral("dur"), double(e.durationMs));
    return o;
}

PlaylistEntry entryFromWeb(const QJsonObject& o)
{
    PlaylistEntry e;
    e.online = o.value(QLatin1String("online")).toBool();
    e.videoId = o.value(QLatin1String("videoId")).toString();
    e.path = o.value(QLatin1String("path")).toString();
    e.title = o.value(QLatin1String("title")).toString();
    e.artist = o.value(QLatin1String("artist")).toString();
    e.album = o.value(QLatin1String("album")).toString();
    e.durationMs = qint64(o.value(QLatin1String("dur")).toDouble());
    return e;
}

QVector<PlaylistEntry> entriesFromWeb(const QJsonArray& a)
{
    QVector<PlaylistEntry> v;
    for (const QJsonValue& x : a) {
        const PlaylistEntry e = entryFromWeb(x.toObject());
        // Only real songs: a YouTube id or a path. Unmatched imports are resolved before they get here.
        if (e.online ? Yt::validId(e.videoId) : !e.path.isEmpty()) v.push_back(e);
    }
    return v;
}

QString nativeUrlOpener()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("cmd");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("open");
#else
    return QStringLiteral("xdg-open");
#endif
}

// Opens the UI in its own window (Edge / Chrome "app mode": no tabs, no address bar) so it feels like
// a desktop program; falls back to the default browser.
void openInBrowser(const QString& url, bool appWindow)
{
    if (appWindow) {
        QStringList candidates;
#if defined(Q_OS_WIN)
        const QStringList roots = { qEnvironmentVariable("ProgramFiles(x86)"), qEnvironmentVariable("ProgramFiles"),
                                    qEnvironmentVariable("LOCALAPPDATA") };
        for (const QString& r : roots) {
            if (r.isEmpty()) continue;
            candidates << r + QStringLiteral("/Microsoft/Edge/Application/msedge.exe")
                       << r + QStringLiteral("/Google/Chrome/Application/chrome.exe")
                       << r + QStringLiteral("/BraveSoftware/Brave-Browser/Application/brave.exe");
        }
#elif defined(Q_OS_MACOS)
        candidates << QStringLiteral("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
                   << QStringLiteral("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge");
#else
        for (const char* n : { "google-chrome", "chromium", "chromium-browser", "microsoft-edge", "brave-browser" }) {
            const QString p = QStandardPaths::findExecutable(QString::fromLatin1(n));
            if (!p.isEmpty()) candidates << p;
        }
#endif
        for (const QString& c : std::as_const(candidates)) {
            if (QFileInfo::exists(c)
                && QProcess::startDetached(c, { QStringLiteral("--app=") + url, QStringLiteral("--window-size=1320,840") }))
                return;
        }
    }
#if defined(Q_OS_WIN)
    QProcess::startDetached(nativeUrlOpener(), { QStringLiteral("/c"), QStringLiteral("start"), QStringLiteral(""), url });
#else
    QProcess::startDetached(nativeUrlOpener(), { url });
#endif
}

} // namespace

// ============================================================================
class App : public QObject
{
public:
    explicit App(QObject* parent = nullptr)
        : QObject(parent), m_relay(&m_nam), m_google(), m_discord()
    {
        m_store.load();
        wireSignals();
        m_srv.setHandler([this](HttpConn* c, const HttpRequest& r) { route(c, r); });
        QTimer::singleShot(0, this, [] { Yt::pruneAudioCache(); });
        scanLibrary();

        // Like a desktop app: when the window is closed (no page connected for a while), quit.
        m_idle.setInterval(1000);
        connect(&m_idle, &QTimer::timeout, this, [this] {
            if (m_srv.sseClients() > 0) { m_seenClient = true; m_gone = 0; return; }
            if (m_seenClient && ++m_gone >= 10) QCoreApplication::quit();
        });
    }

    void enableAutoQuit() { m_idle.start(); }

    bool listen(quint16 port)
    {
        for (quint16 p = port; p < port + 20; ++p)
            if (m_srv.listen(p)) return true;
        return false;
    }
    quint16 port() const { return m_srv.port(); }
    void shutdown() { m_discord.shutdown(); }

private:
    using Conn = QPointer<HttpConn>;

    // yt-dlp can't read the browser's cookie database while the browser is open (or when it encrypts
    // cookies per-app). Rather than failing every request, drop the cookies, tell the page, retry once.
    bool cookieError(const QString& e) const
    {
        return !Yt::cookiesDisabled() && e.contains(QLatin1String("cookie"), Qt::CaseInsensitive)
               && !QSettings().value(QStringLiteral("yt/cookies")).toString().isEmpty();
    }
    void disableCookies()
    {
        Yt::cookiesDisabled() = true;
        m_srv.broadcast(QStringLiteral("notice"), QJsonObject{
            { QStringLiteral("msg"), QStringLiteral("Couldn\u2019t read your browser\u2019s cookies (browsers lock them while open), so Pulsify is continuing without them. You can turn cookies off in Settings \u2192 Sources.") } });
    }

    // ------------------------------------------------------------------ signal wiring
    void wireSignals()
    {
        // ---- yt-dlp search stream
        connect(&m_yt, &YtDlp::searchBatch, this, [this](quint64 tok, const QVector<OnlineItem>& items) {
            m_srv.broadcast(QStringLiteral("search"), QJsonObject{ { QStringLiteral("token"), double(tok) },
                                                                    { QStringLiteral("items"), itemsJson(items) } });
        });
        connect(&m_yt, &YtDlp::searchDone, this, [this](quint64 tok, const QString& err) {
            if (!err.isEmpty() && !m_searchRetried && !m_lastSearch.isEmpty() && cookieError(err)) {
                disableCookies();
                m_searchRetried = true;
                const quint64 again = m_yt.search(m_lastSearch, 30);
                m_srv.broadcast(QStringLiteral("searchRetry"), QJsonObject{ { QStringLiteral("from"), double(tok) },
                                                                             { QStringLiteral("to"), double(again) } });
                return;
            }
            m_srv.broadcast(QStringLiteral("searchDone"), QJsonObject{ { QStringLiteral("token"), double(tok) },
                                                                        { QStringLiteral("error"), err } });
        });
        connect(&m_yt, &YtDlp::toolMessage, this, [this](const QString& m) {
            m_srv.broadcast(QStringLiteral("tool"), QJsonObject{ { QStringLiteral("msg"), m }, { QStringLiteral("done"), false } });
        });
        connect(&m_yt, &YtDlp::toolFinished, this, [this](bool ok, const QString& m) {
            m_srv.broadcast(QStringLiteral("tool"), QJsonObject{ { QStringLiteral("msg"), m }, { QStringLiteral("done"), true },
                                                                  { QStringLiteral("ok"), ok } });
        });

        // ---- discover shelves / matching ("fetch" jobs)
        connect(&m_yt, &YtDlp::fetched, this, [this](quint64 tok, const QVector<OnlineItem>& items, const QString& err) {
            const auto it = m_fetchWait.find(tok);
            if (it == m_fetchWait.end()) return;
            const FetchWait w = it.value();
            m_fetchWait.erase(it);
            if (!err.isEmpty() && !w.retried && cookieError(err)) {
                disableCookies();
                FetchWait again = w;
                again.retried = true;
                m_fetchWait.insert(m_yt.fetch(w.key, w.kind == QLatin1String("match") ? 6 : 24), again);
                return;
            }
            if (w.kind == QLatin1String("shelf") && err.isEmpty() && !items.isEmpty())
                m_shelfCache.insert(w.key, { QDateTime::currentSecsSinceEpoch(), itemsJson(items) });
            if (!w.conn) return;
            if (!err.isEmpty()) { w.conn->error(502, err); return; }
            if (w.kind == QLatin1String("match")) {
                w.conn->json(QJsonObject{ { QStringLiteral("item"), pickMatch(items, w.dur) } });
            } else {
                w.conn->json(QJsonObject{ { QStringLiteral("items"), itemsJson(items) } });
            }
        });

        // ---- play: resolve -> relay
        connect(&m_yt, &YtDlp::resolved, this,
                [this](quint64 tok, const QString& vid, const QString& url, const OnlineItem& meta, const QString& err) {
            if (tok != m_playTok) return;
            Conn c = m_playConn;
            if (c && !err.isEmpty() && !m_playRetried && cookieError(err)) {
                disableCookies();
                m_playRetried = true;
                m_playTok = m_yt.resolve(vid);
                return;
            }
            m_playConn.clear();
            if (!c) return;
            if (!err.isEmpty()) { c->error(502, err); return; }
            m_prefetched.insert(vid, { url, meta, QDateTime::currentSecsSinceEpoch() });
            openRelay(c, vid, url, meta);
        });
        connect(&m_yt, &YtDlp::prefetched, this,
                [this](const QString& vid, const QString& url, const OnlineItem& meta, const QString& err) {
            if (m_refreshSid && m_refreshVid == vid) {              // a stream URL expired mid-song: swap it in
                const quint64 sid = m_refreshSid;
                m_refreshSid = 0;
                if (err.isEmpty()) m_relay.refreshUpstream(sid, url, meta.stream.headers);
                return;
            }
            if (err.isEmpty()) m_prefetched.insert(vid, { url, meta, QDateTime::currentSecsSinceEpoch() });
        });
        connect(&m_relay, &StreamRelay::ready, this, [this](quint64 sid, const QUrl& local) {
            const auto it = m_sessWait.find(sid);
            if (it == m_sessWait.end()) return;
            const SessWait w = it.value();
            m_sessWait.erase(it);
            if (!w.conn) return;
            w.conn->json(QJsonObject{ { QStringLiteral("url"), local.toString() },
                                       { QStringLiteral("item"), itemJson(w.meta) },
                                       { QStringLiteral("stream"), streamJson(w.meta.stream) } });
        });
        connect(&m_relay, &StreamRelay::failed, this, [this](quint64 sid, const QString& why) {
            const auto it = m_sessWait.find(sid);
            if (it == m_sessWait.end()) return;
            const SessWait w = it.value();
            m_sessWait.erase(it);
            if (w.conn) w.conn->error(502, QStringLiteral("Stream error: ") + why);
        });
        connect(&m_relay, &StreamRelay::upstreamRejected, this, [this](quint64 sid) {
            const QString vid = m_sessVideo.value(sid);
            if (vid.isEmpty()) return;
            m_refreshSid = sid;
            m_refreshVid = vid;
            m_yt.cancelPrefetch();
            m_yt.prefetch(vid);
        });

        // ---- lyrics
        connect(&m_lyrics, &LyricsService::ready, this, [this](const QString& key, const LyricsDoc& d) {
            const QList<Conn> waiters = m_lyricsWait.take(key);
            for (const Conn& c : waiters) if (c) c->json(d.toJson());
        });
        connect(&m_lyrics, &LyricsService::failed, this, [this](const QString& key, const QString& msg) {
            const QList<Conn> waiters = m_lyricsWait.take(key);
            for (const Conn& c : waiters)
                if (c) c->json(QJsonObject{ { QStringLiteral("none"), true }, { QStringLiteral("message"), msg } });
        });

        // ---- discord
        connect(&m_discord, &DiscordRpc::statusChanged, this, [this](const QString& text, bool ok) {
            m_srv.broadcast(QStringLiteral("discord"), QJsonObject{ { QStringLiteral("status"), text }, { QStringLiteral("connected"), ok } });
        });

        // ---- google
        connect(&m_google, &GoogleAccount::stateChanged, this, [this] { m_srv.broadcast(QStringLiteral("google"), googleState()); });
        connect(&m_google, &GoogleAccount::openBrowser, this, [this](const QString& url) {
            Conn c = m_signInConn;
            m_signInConn.clear();
            if (c) c->json(QJsonObject{ { QStringLiteral("url"), url } });
        });
        connect(&m_google, &GoogleAccount::signInFailed, this, [this](const QString& err) {
            if (m_signInConn) { m_signInConn->error(400, err); m_signInConn.clear(); }
            m_srv.broadcast(QStringLiteral("googleError"), QJsonObject{ { QStringLiteral("error"), err } });
        });
        connect(&m_google, &GoogleAccount::playlistsReady, this, [this](const QVector<RemotePlaylist>& lists, const QString& err) {
            Conn c = m_gPlaylistsConn;
            m_gPlaylistsConn.clear();
            if (!c) return;
            if (!err.isEmpty()) { c->error(502, err); return; }
            QJsonArray a;
            for (const RemotePlaylist& p : lists)
                a.push_back(QJsonObject{ { QStringLiteral("id"), p.id }, { QStringLiteral("title"), p.title }, { QStringLiteral("count"), p.count } });
            c->json(QJsonObject{ { QStringLiteral("playlists"), a } });
        });
        connect(&m_google, &GoogleAccount::itemsProgress, this, [this](const QString& id, int got, int total) {
            m_srv.broadcast(QStringLiteral("googleProgress"), QJsonObject{ { QStringLiteral("id"), id }, { QStringLiteral("got"), got },
                                                                           { QStringLiteral("total"), total } });
        });
        connect(&m_google, &GoogleAccount::itemsReady, this,
                [this](const QString&, const QVector<OnlineItem>& items, const QString& err) {
            Conn c = m_gItemsConn;
            m_gItemsConn.clear();
            if (!c) return;
            if (!err.isEmpty()) { c->error(502, err); return; }
            c->json(QJsonObject{ { QStringLiteral("items"), itemsJson(items) } });
        });
    }

    // ------------------------------------------------------------------ small helpers
    QJsonObject googleState() const
    {
        return QJsonObject{
            { QStringLiteral("hasCredentials"), m_google.hasCredentials() },
            { QStringLiteral("clientId"), m_google.clientId() },
            { QStringLiteral("hasSecret"), !m_google.clientSecret().isEmpty() },
            { QStringLiteral("signedIn"), m_google.signedIn() },
            { QStringLiteral("signingIn"), m_google.signingIn() },
            { QStringLiteral("name"), m_google.accountName() },
            { QStringLiteral("avatar"), m_google.avatarUrl() },
        };
    }

    QJsonObject settingsJson()
    {
        QSettings s;
        return QJsonObject{
            { QStringLiteral("ytdlpPath"), s.value(QStringLiteral("ytdlpPath")).toString() },
            { QStringLiteral("ytdlpFound"), m_yt.binary() },
            { QStringLiteral("cookies"), s.value(QStringLiteral("yt/cookies")).toString() },
            { QStringLiteral("jsRuntime"), Yt::jsRuntime().first },
            { QStringLiteral("geniusToken"), s.value(QStringLiteral("lyrics/geniusToken")).toString() },
            { QStringLiteral("discord"), QJsonObject{
                { QStringLiteral("enabled"), s.value(QStringLiteral("discord/enabled"), true).toBool() },
                { QStringLiteral("clientId"), s.value(QStringLiteral("discord/clientId"), DiscordRpc::defaultClientId()).toString() },
                { QStringLiteral("status"), m_discord.status() },
                { QStringLiteral("connected"), m_discord.isConnected() } } },
        };
    }

    QJsonObject playlistsJson() const
    {
        QJsonArray lists;
        for (const Playlist& p : m_store.playlists()) {
            QJsonArray es;
            for (const PlaylistEntry& e : p.entries) es.push_back(entryToWeb(e));
            lists.push_back(QJsonObject{ { QStringLiteral("id"), p.id }, { QStringLiteral("name"), p.name },
                                         { QStringLiteral("source"), p.source }, { QStringLiteral("created"), double(p.created) },
                                         { QStringLiteral("entries"), es } });
        }
        QJsonArray rec;
        for (const PlaylistEntry& e : m_store.recent()) rec.push_back(entryToWeb(e));
        return QJsonObject{ { QStringLiteral("playlists"), lists }, { QStringLiteral("recent"), rec } };
    }

    // Picks the search result that best matches a Deezer / CSV track by duration.
    static QJsonValue pickMatch(const QVector<OnlineItem>& items, qint64 wantMs)
    {
        if (items.isEmpty()) return QJsonValue(QJsonValue::Null);
        const OnlineItem* best = &items.first();
        if (wantMs > 0) {
            qint64 bestDiff = qAbs(best->durationMs - wantMs);
            for (const OnlineItem& it : items) {
                if (it.durationMs <= 0) continue;
                const qint64 d = qAbs(it.durationMs - wantMs);
                if (d < bestDiff - 1500) { best = &it; bestDiff = d; }      // prefer a clearly closer length
            }
        }
        return itemJson(*best);
    }

    void openRelay(const Conn& c, const QString& vid, const QString& url, const OnlineItem& meta)
    {
        const quint64 sid = m_relay.open(url, meta.stream.headers);
        m_sessVideo.clear();
        m_sessVideo.insert(sid, vid);
        m_sessWait.insert(sid, SessWait{ c, meta });
    }

    bool inLibrary(const QString& path) const
    {
        const QString canon = QFileInfo(path).canonicalFilePath();
        if (canon.isEmpty() || !Local::isAudio(canon)) return false;
        for (const QString& f : m_folders) {
            const QString root = QFileInfo(f).canonicalFilePath();
            if (!root.isEmpty() && (canon == root || canon.startsWith(root + QLatin1Char('/')))) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ library
    void scanLibrary()
    {
        m_folders = QSettings().value(QStringLiteral("libraryFolders")).toStringList();
        m_tracks.clear();
        m_scanning = true;
        auto files = std::make_shared<QStringList>();
        for (const QString& f : std::as_const(m_folders)) *files += Local::scan(f);
        files->removeDuplicates();
        const quint64 gen = ++m_scanGen;
        m_srv.broadcast(QStringLiteral("library"), libraryJson());
        QTimer::singleShot(0, this, [this, files, gen] { scanStep(files, 0, gen); });
    }

    void scanStep(const std::shared_ptr<QStringList>& files, int from, quint64 gen)
    {
        if (gen != m_scanGen) return;                               // a newer scan took over
        const int to = qMin(int(files->size()), from + 40);
        for (int i = from; i < to; ++i) m_tracks.push_back(Local::trackFor(files->at(i)));
        if (to < files->size()) {
            QTimer::singleShot(0, this, [this, files, to, gen] { scanStep(files, to, gen); });
            return;
        }
        m_scanning = false;
        m_srv.broadcast(QStringLiteral("library"), libraryJson());
    }

    QJsonObject libraryJson() const
    {
        QJsonArray t;
        for (const LocalTrack& x : m_tracks) t.push_back(x.toJson());
        return QJsonObject{ { QStringLiteral("folders"), QJsonArray::fromStringList(m_folders) },
                            { QStringLiteral("tracks"), t }, { QStringLiteral("scanning"), m_scanning } };
    }

    // ------------------------------------------------------------------ Deezer proxy
    void deezer(HttpConn* c, const HttpRequest& r)
    {
        const QString path = r.q("path");
        static const QRegularExpression ok(QStringLiteral(R"(^(chart/0/(tracks|artists|albums)|artist/\d+/top|album/\d+/tracks|artist/\d+)$)"));
        if (!ok.match(path).hasMatch()) { c->error(400, QStringLiteral("Unsupported path")); return; }
        const QString limit = r.q("limit").isEmpty() ? QStringLiteral("50") : QString::number(qBound(1, r.q("limit").toInt(), 100));
        const QString url = QStringLiteral("https://api.deezer.com/%1?limit=%2").arg(path, limit);
        const auto hit = m_deezerCache.constFind(url);
        if (hit != m_deezerCache.constEnd() && QDateTime::currentSecsSinceEpoch() - hit->first < 600) {
            c->send(200, "application/json; charset=utf-8", hit->second);
            return;
        }
        QNetworkRequest rq{ QUrl(url) };
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Pulse/1.2"));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(15000);
        QNetworkReply* rep = m_nam.get(rq);
        Conn conn(c);
        connect(rep, &QNetworkReply::finished, this, [this, rep, conn, url] {
            rep->deleteLater();
            if (!conn) return;
            if (rep->error() != QNetworkReply::NoError) { conn->error(502, rep->errorString()); return; }
            const QByteArray body = rep->readAll();
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            if (o.contains(QLatin1String("error"))) {
                conn->error(502, jstr(jobj(o, "error"), "message"));
                return;
            }
            m_deezerCache.insert(url, qMakePair(QDateTime::currentSecsSinceEpoch(), body));
            conn->send(200, "application/json; charset=utf-8", body);
        });
    }

    // ------------------------------------------------------------------ routing
    void route(HttpConn* c, const HttpRequest& r)
    {
        const QString& p = r.path;
        const bool get = (r.method == "GET" || r.method == "HEAD");

        if (!p.startsWith(QLatin1String("/api/"))) { serveStatic(c, r); return; }
        if (p == QLatin1String("/api/events") && get) { c->startSse(); sendHello(c); return; }

        // ---------------- prefs / settings / state
        if (p == QLatin1String("/api/prefs")) {
            QSettings s;
            if (get) { c->json(QJsonDocument::fromJson(s.value(QStringLiteral("web/prefs")).toByteArray()).object()); return; }
            s.setValue(QStringLiteral("web/prefs"), QJsonDocument(r.json()).toJson(QJsonDocument::Compact));
            c->json(QJsonObject{ { QStringLiteral("ok"), true } });
            return;
        }
        if (p == QLatin1String("/api/settings")) {
            if (!get) {
                const QJsonObject o = r.json();
                QSettings s;
                if (o.contains(QLatin1String("ytdlpPath"))) s.setValue(QStringLiteral("ytdlpPath"), jstr(o, "ytdlpPath").trimmed());
                if (o.contains(QLatin1String("cookies")))   s.setValue(QStringLiteral("yt/cookies"), jstr(o, "cookies").trimmed());
                if (o.contains(QLatin1String("geniusToken"))) s.setValue(QStringLiteral("lyrics/geniusToken"), jstr(o, "geniusToken").trimmed());
                if (o.contains(QLatin1String("discord"))) {
                    const QJsonObject d = jobj(o, "discord");
                    if (d.contains(QLatin1String("enabled"))) s.setValue(QStringLiteral("discord/enabled"), d.value(QLatin1String("enabled")).toBool());
                    if (d.contains(QLatin1String("clientId"))) s.setValue(QStringLiteral("discord/clientId"), jstr(d, "clientId").trimmed());
                    s.sync();
                    m_discord.reloadSettings();
                }
            }
            c->json(settingsJson());
            return;
        }
        if (p == QLatin1String("/api/cache/clear") && r.method == "POST") {
            Yt::clearAudioCache();
            LyricsService::clearCache();
            m_shelfCache.clear();
            m_deezerCache.clear();
            c->json(QJsonObject{ { QStringLiteral("ok"), true } });
            return;
        }

        // ---------------- yt-dlp
        if (p == QLatin1String("/api/ytdlp/install") && r.method == "POST") { m_yt.install(); c->json(QJsonObject{ { QStringLiteral("ok"), true } }); return; }
        if (p == QLatin1String("/api/ytdlp/update") && r.method == "POST")  { m_yt.selfUpdate(); c->json(QJsonObject{ { QStringLiteral("ok"), true } }); return; }

        // ---------------- search / shelves / matching
        if (p == QLatin1String("/api/search") && r.method == "POST") {
            const QString q = jstr(r.json(), "q").trimmed();
            if (q.isEmpty()) { c->error(400, QStringLiteral("Empty query")); return; }
            if (!m_yt.available()) { c->error(502, QStringLiteral("yt-dlp isn\u2019t installed \u2013 open Settings to get it")); return; }
            m_lastSearch = q;
            m_searchRetried = false;
            c->json(QJsonObject{ { QStringLiteral("token"), double(m_yt.search(q, 30)) } });
            return;
        }
        if (p == QLatin1String("/api/shelf") && get) {
            const QString q = r.q("q");
            if (q.isEmpty()) { c->error(400, QStringLiteral("Missing q")); return; }
            const auto hit = m_shelfCache.constFind(q);
            if (hit != m_shelfCache.constEnd() && QDateTime::currentSecsSinceEpoch() - hit->first < 8 * 3600) {
                c->json(QJsonObject{ { QStringLiteral("items"), hit->second } });
                return;
            }
            if (!m_yt.available()) { c->error(502, QStringLiteral("yt-dlp isn\u2019t installed \u2013 open Settings to get it")); return; }
            m_fetchWait.insert(m_yt.fetch(q, 24), FetchWait{ Conn(c), QStringLiteral("shelf"), q, 0 });
            return;
        }
        if (p == QLatin1String("/api/match") && get) {
            const QString q = r.q("q");
            if (q.isEmpty()) { c->error(400, QStringLiteral("Missing q")); return; }
            if (!m_yt.available()) { c->error(502, QStringLiteral("yt-dlp isn\u2019t installed \u2013 open Settings to get it")); return; }
            m_fetchWait.insert(m_yt.fetch(q, 6), FetchWait{ Conn(c), QStringLiteral("match"), q, r.q("dur").toLongLong() });
            return;
        }

        // ---------------- playback
        if (p == QLatin1String("/api/play") && r.method == "POST") {
            const QString vid = jstr(r.json(), "videoId");
            if (!Yt::validId(vid)) { c->error(400, QStringLiteral("Invalid video id")); return; }
            if (!m_yt.available()) { c->error(502, QStringLiteral("yt-dlp isn\u2019t installed \u2013 open Settings to get it")); return; }
            if (m_playConn) m_playConn->error(409, QStringLiteral("superseded"));
            m_playConn.clear();
            const auto pf = m_prefetched.constFind(vid);
            if (pf != m_prefetched.constEnd() && QDateTime::currentSecsSinceEpoch() - pf->ts < 1200) {
                m_yt.cancelResolve();
                openRelay(Conn(c), vid, pf->url, pf->meta);
                return;
            }
            m_playConn = Conn(c);
            m_playRetried = false;
            m_playTok = m_yt.resolve(vid);
            return;
        }
        if (p == QLatin1String("/api/prefetch") && r.method == "POST") {
            const QString vid = jstr(r.json(), "videoId");
            const auto pf = m_prefetched.constFind(vid);
            if (Yt::validId(vid) && m_yt.available() && (pf == m_prefetched.constEnd() || QDateTime::currentSecsSinceEpoch() - pf->ts >= 1200))
                m_yt.prefetch(vid);
            c->json(QJsonObject{ { QStringLiteral("ok"), true } });
            return;
        }
        if (p == QLatin1String("/api/stop") && r.method == "POST") {
            m_relay.close();
            c->json(QJsonObject{ { QStringLiteral("ok"), true } });
            return;
        }

        // ---------------- playlists + history
        if (p == QLatin1String("/api/playlists") && get) { c->json(playlistsJson()); return; }
        if (p == QLatin1String("/api/playlists") && r.method == "POST") {
            const QJsonObject o = r.json();
            const QString name = jstr(o, "name");
            const QString source = jstr(o, "source");
            const QVector<PlaylistEntry> es = entriesFromWeb(jarr(o, "entries"));
            if (!source.isEmpty() && m_store.findBySource(source)) {      // re-import tops the playlist up
                const QString id = m_store.findBySource(source)->id;
                const int added = m_store.addMany(id, es);
                c->json(QJsonObject{ { QStringLiteral("id"), id }, { QStringLiteral("added"), added }, { QStringLiteral("existing"), true } });
                return;
            }
            const QString id = (es.isEmpty() && source.isEmpty()) ? m_store.create(name) : m_store.createFrom(name, source, es);
            c->json(QJsonObject{ { QStringLiteral("id"), id }, { QStringLiteral("added"), int(es.size()) } });
            return;
        }
        if (p == QLatin1String("/api/recent") && r.method == "POST") {
            const PlaylistEntry e = entryFromWeb(jobj(r.json(), "entry"));
            if (e.online ? Yt::validId(e.videoId) : !e.path.isEmpty()) m_store.pushRecent(e);
            c->json(QJsonObject{ { QStringLiteral("ok"), true } });
            return;
        }
        if (r.parts.size() >= 3 && r.parts[1] == QLatin1String("playlists")) {
            const QString id = r.parts[2];
            const QString act = r.parts.value(3);
            const QJsonObject o = r.json();
            if (!m_store.find(id)) { c->error(404, QStringLiteral("No such playlist")); return; }
            if (r.method == "DELETE" && act.isEmpty())        { m_store.remove(id); c->json(playlistsJson()); return; }
            if (act == QLatin1String("rename"))               { m_store.rename(id, jstr(o, "name")); c->json(playlistsJson()); return; }
            if (act == QLatin1String("remove"))               { m_store.removeAt(id, o.value(QLatin1String("row")).toInt(-1)); c->json(playlistsJson()); return; }
            if (act == QLatin1String("move"))                 { m_store.moveEntry(id, o.value(QLatin1String("from")).toInt(-1), o.value(QLatin1String("to")).toInt(-1)); c->json(playlistsJson()); return; }
            if (act == QLatin1String("add")) {
                const int n = m_store.addMany(id, entriesFromWeb(jarr(o, "entries")));
                QJsonObject out = playlistsJson();
                out.insert(QStringLiteral("added"), n);
                c->json(out);
                return;
            }
        }

        // ---------------- local library
        if (p == QLatin1String("/api/library") && get) { c->json(libraryJson()); return; }
        if (p == QLatin1String("/api/library/add") && r.method == "POST") {
            const QString dir = QDir::fromNativeSeparators(jstr(r.json(), "path").trimmed());
            if (!QFileInfo(dir).isDir()) { c->error(400, QStringLiteral("That folder doesn\u2019t exist")); return; }
            QStringList f = QSettings().value(QStringLiteral("libraryFolders")).toStringList();
            if (!f.contains(dir)) f << dir;
            QSettings().setValue(QStringLiteral("libraryFolders"), f);
            scanLibrary();
            c->json(libraryJson());
            return;
        }
        if (p == QLatin1String("/api/library/remove") && r.method == "POST") {
            QStringList f = QSettings().value(QStringLiteral("libraryFolders")).toStringList();
            f.removeAll(jstr(r.json(), "path"));
            QSettings().setValue(QStringLiteral("libraryFolders"), f);
            scanLibrary();
            c->json(libraryJson());
            return;
        }
        if (p == QLatin1String("/api/fs") && get) { c->json(listDir(r.q("path"))); return; }
        if (p == QLatin1String("/api/file") && get) {
            const QString path = r.q("path");
            if (!inLibrary(path)) { c->error(403, QStringLiteral("Not in your library")); return; }
            c->sendFile(path, Local::mimeFor(path), r.header("range"), r.method == "HEAD");
            return;
        }
        if (p == QLatin1String("/api/cover") && get) {
            const QString path = r.q("path");
            if (!inLibrary(path)) { c->error(403, QStringLiteral("Not in your library")); return; }
            const Id3Cover cv = coverFor(path);
            if (cv.data.isEmpty()) { c->error(404, QStringLiteral("No cover")); return; }
            c->send(200, cv.mime, cv.data, "Cache-Control: private, max-age=86400\r\n");
            return;
        }

        // ---------------- lyrics
        if (p == QLatin1String("/api/lyrics") && get) {
            const QString key = r.q("key");
            if (key.isEmpty()) { c->error(400, QStringLiteral("Missing key")); return; }
            m_lyricsWait[key].append(Conn(c));
            m_lyrics.fetch(key, r.q("artist"), r.q("title"), r.q("album"), r.q("dur").toLongLong(), r.q("refresh") == QLatin1String("1"));
            return;
        }

        // ---------------- Discord
        if (p == QLatin1String("/api/discord/presence") && r.method == "POST") {
            const QJsonObject o = r.json();
            RpcTrack t;
            t.valid = o.value(QLatin1String("valid")).toBool();
            t.playing = o.value(QLatin1String("playing")).toBool();
            t.paused = o.value(QLatin1String("paused")).toBool();
            t.title = jstr(o, "title");
            t.artist = jstr(o, "artist");
            t.album = jstr(o, "album");
            t.durationMs = qint64(jnum(o, "durationMs"));
            t.positionMs = qint64(jnum(o, "positionMs"));
            t.fallbackImage = jstr(o, "fallbackImage");
            m_discord.setTrack(t);
            c->json(QJsonObject{ { QStringLiteral("ok"), true } });
            return;
        }

        // ---------------- Google
        if (p == QLatin1String("/api/google") && get) { c->json(googleState()); return; }
        if (p == QLatin1String("/api/google/credentials") && r.method == "POST") {
            const QJsonObject o = r.json();
            m_google.setCredentials(jstr(o, "clientId"), jstr(o, "clientSecret"));
            c->json(googleState());
            return;
        }
        if (p == QLatin1String("/api/google/signin") && r.method == "POST") {
            if (m_signInConn) m_signInConn->error(409, QStringLiteral("superseded"));
            m_signInConn = Conn(c);
            m_google.signIn();                        // answers through openBrowser / signInFailed
            return;
        }
        if (p == QLatin1String("/api/google/cancel") && r.method == "POST") { m_google.cancelSignIn(); c->json(googleState()); return; }
        if (p == QLatin1String("/api/google/signout") && r.method == "POST") { m_google.signOut(); c->json(googleState()); return; }
        if (p == QLatin1String("/api/google/playlists") && get) {
            if (m_gPlaylistsConn) m_gPlaylistsConn->error(409, QStringLiteral("superseded"));
            m_gPlaylistsConn = Conn(c);
            m_google.fetchPlaylists();
            return;
        }
        if (p == QLatin1String("/api/google/items") && get) {
            if (m_gItemsConn) m_gItemsConn->error(409, QStringLiteral("superseded"));
            m_gItemsConn = Conn(c);
            m_google.fetchPlaylistItems(r.q("id"));
            return;
        }

        // ---------------- link import (YouTube / YouTube Music playlist URL)
        if (p == QLatin1String("/api/import/yt") && r.method == "POST") {
            const QString pid = YtDlp::playlistIdFromLink(jstr(r.json(), "link"));
            if (pid.isEmpty()) { c->error(400, QStringLiteral("That doesn\u2019t look like a YouTube playlist link.")); return; }
            if (!m_yt.available()) { c->error(502, QStringLiteral("yt-dlp isn\u2019t installed \u2013 open Settings to get it")); return; }
            auto* job = new YtDlp(this);
            const quint64 tok = job->importPlaylist(pid);
            Conn conn(c);
            connect(job, &YtDlp::playlistImported, this,
                    [job, tok, conn, pid](quint64 t, const QString& title, const QVector<OnlineItem>& items, const QString& err) {
                if (t != tok) return;
                job->deleteLater();
                if (!conn) return;
                if (!err.isEmpty()) { conn->error(502, err); return; }
                conn->json(QJsonObject{ { QStringLiteral("name"), title }, { QStringLiteral("source"), QStringLiteral("yt:") + pid },
                                        { QStringLiteral("items"), itemsJson(items) } });
            });
            return;
        }

        // ---------------- Deezer charts
        if (p == QLatin1String("/api/deezer") && get) { deezer(c, r); return; }

        c->error(404, QStringLiteral("Unknown endpoint"));
    }

    // ------------------------------------------------------------------ misc
    struct Id3Cover { QByteArray data, mime; };

    static Id3Cover coverFor(const QString& path)
    {
        const Local::Id3 m = Local::readTags(path);
        return { m.cover, m.coverMime.isEmpty() ? QByteArray("image/jpeg") : m.coverMime };
    }

    QJsonObject listDir(const QString& in) const
    {
        QJsonArray roots;
        for (const QFileInfo& d : QDir::drives()) roots.push_back(QDir::fromNativeSeparators(d.absoluteFilePath()));
        QString path = QDir::fromNativeSeparators(in.trimmed());
        if (path.isEmpty()) {
            const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
            path = QFileInfo(music).isDir() ? music : QDir::homePath();
        }
        QDir d(path);
        QJsonArray dirs;
        if (d.exists()) {
            for (const QString& n : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::IgnoreCase | QDir::Name))
                if (!n.startsWith(QLatin1Char('.'))) dirs.push_back(n);
        }
        int audio = 0;
        if (d.exists()) {
            for (const QString& n : d.entryList(QDir::Files | QDir::Readable))
                if (Local::isAudio(n)) ++audio;
        }
        const bool isRoot = QDir(path).isRoot();
        QDir up(path);
        const QString parentPath = (!isRoot && up.cdUp()) ? up.absolutePath() : QString();
        return QJsonObject{
            { QStringLiteral("path"), QDir::fromNativeSeparators(d.absolutePath()) }, { QStringLiteral("parent"), parentPath },
            { QStringLiteral("dirs"), dirs }, { QStringLiteral("audioFiles"), audio }, { QStringLiteral("roots"), roots },
            { QStringLiteral("home"), QDir::homePath() },
        };
    }

    void sendHello(HttpConn* c)
    {
        c->sendSse(QStringLiteral("hello"), QJsonDocument(QJsonObject{ { QStringLiteral("ok"), true } }).toJson(QJsonDocument::Compact));
        c->sendSse(QStringLiteral("google"), QJsonDocument(googleState()).toJson(QJsonDocument::Compact));
        c->sendSse(QStringLiteral("discord"), QJsonDocument(QJsonObject{ { QStringLiteral("status"), m_discord.status() },
                                                                          { QStringLiteral("connected"), m_discord.isConnected() } })
                                                   .toJson(QJsonDocument::Compact));
    }

    void serveStatic(HttpConn* c, const HttpRequest& r)
    {
        if (r.method != "GET" && r.method != "HEAD") { c->error(405, QStringLiteral("Method not allowed")); return; }
        QString path = r.path;
        if (path == QLatin1String("/") || path.isEmpty()) path = QStringLiteral("/index.html");
        if (path.contains(QLatin1String(".."))) { c->error(403, QStringLiteral("Forbidden")); return; }
        const QString res = QStringLiteral(":/web") + path;
        if (!QFile::exists(res)) { c->sendResource(QStringLiteral(":/web/index.html")); return; }     // SPA fallback
        c->sendResource(res);
    }

    // ------------------------------------------------------------------ state
    struct FetchWait { Conn conn; QString kind, key; qint64 dur = 0; bool retried = false; };
    struct SessWait  { Conn conn; OnlineItem meta; };
    struct Prefetched { QString url; OnlineItem meta; qint64 ts = 0; };

    QNetworkAccessManager m_nam;
    HttpServer     m_srv;
    YtDlp          m_yt;
    StreamRelay    m_relay;
    PlaylistStore  m_store;
    GoogleAccount  m_google;
    DiscordRpc     m_discord;
    LyricsService  m_lyrics;

    QHash<quint64, FetchWait>  m_fetchWait;
    QHash<QString, QPair<qint64, QJsonArray>>  m_shelfCache;
    QHash<QString, QPair<qint64, QByteArray>>  m_deezerCache;
    QHash<QString, Prefetched> m_prefetched;
    QHash<quint64, SessWait>   m_sessWait;
    QHash<quint64, QString>    m_sessVideo;
    QHash<QString, QList<Conn>> m_lyricsWait;

    Conn    m_playConn, m_signInConn, m_gPlaylistsConn, m_gItemsConn;
    quint64 m_playTok = 0;
    bool    m_playRetried = false;
    QString m_lastSearch;
    bool    m_searchRetried = false;
    quint64 m_refreshSid = 0;
    QString m_refreshVid;

    QTimer             m_idle;
    bool               m_seenClient = false;
    int                m_gone = 0;

    QStringList        m_folders;
    QVector<LocalTrack> m_tracks;
    bool               m_scanning = false;
    quint64            m_scanGen = 0;
};

// ============================================================================
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Pulse"));            // same names as the Qt desktop app
    app.setOrganizationName(QStringLiteral("PulsePlayer"));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Pulsify Web server"));
    cli.addHelpOption();
    QCommandLineOption portOpt({ QStringLiteral("p"), QStringLiteral("port") }, QStringLiteral("Port to listen on (default 8765)."), QStringLiteral("port"), QStringLiteral("8765"));
    QCommandLineOption noBrowser(QStringLiteral("no-browser"), QStringLiteral("Don't open the browser."));
    QCommandLineOption keepOpt(QStringLiteral("keep-running"), QStringLiteral("Don't quit when the window is closed."));
    QCommandLineOption tabOpt(QStringLiteral("browser-tab"), QStringLiteral("Open in a normal browser tab instead of an app window."));
    cli.addOption(portOpt);
    cli.addOption(noBrowser);
    cli.addOption(keepOpt);
    cli.addOption(tabOpt);
    cli.process(app);

    App server;
    if (!server.listen(quint16(cli.value(portOpt).toUInt()))) {
        fprintf(stderr, "Pulsify: couldn't open a local port.\n");
        return 1;
    }
    const QString url = QStringLiteral("http://127.0.0.1:%1/").arg(server.port());
    printf("Pulsify is running at %s  (close the window to quit)\n", qPrintable(url));
    fflush(stdout);
    if (!cli.isSet(noBrowser)) openInBrowser(url, !cli.isSet(tabOpt));
    if (!cli.isSet(keepOpt) && !cli.isSet(noBrowser)) server.enableAutoQuit();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&server] { server.shutdown(); });
    return app.exec();
}
