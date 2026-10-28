#pragma once
#include <QtCore>
#include <QtNetwork>
#include <functional>
#include <memory>
#include <QQueue>
#ifdef Q_OS_WIN
#  include <QtCore/qt_windows.h>
#endif

// ============================================================================
//  Online (YouTube) backend.
//
//  YouTube has no public audio-stream API, so Pulsify drives the open-source
//  `yt-dlp` tool: it searches, returns clean metadata, and resolves a direct
//  audio URL which QMediaPlayer then streams. Pulsify can download yt-dlp itself.
// ============================================================================

// Tiny JSON accessors (a bare const char* key is ambiguous between overloads in Qt 6).
inline QString     jstr(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
inline QJsonObject jobj(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toObject(); }
inline QJsonArray  jarr(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toArray(); }
inline double      jnum(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toDouble(); }

// What yt-dlp told us about the audio stream itself (only filled in by a stream resolve).
struct StreamInfo {
    QString container, codec, channel, uploadDate, webUrl;   // "m4a", "mp4a.40.2", ...
    int     bitrateKbps = 0, sampleRate = 0;
    qint64  views = 0;
    QVector<QPair<QByteArray, QByteArray>> headers;          // HTTP headers the stream URL needs (for relay)
    bool    known() const { return !container.isEmpty() || !codec.isEmpty() || bitrateKbps > 0; }
};

struct OnlineItem {
    QString id, title, artist, album, thumbUrl;
    qint64  durationMs = 0;
    StreamInfo stream;
    bool    valid() const { return !id.isEmpty(); }
};

namespace Yt {

// GUI apps (especially on macOS) often don't see the shell's PATH.
inline QStringList extraBinDirs()
{
    QStringList d;
    d << QStringLiteral("/opt/homebrew/bin")
      << QStringLiteral("/usr/local/bin")
      << QDir::homePath() + QStringLiteral("/.local/bin")
      << QDir::homePath() + QStringLiteral("/.deno/bin")
      << QDir::homePath() + QStringLiteral("/.bun/bin");
#ifdef Q_OS_WIN
    d << QDir::homePath() + QStringLiteral("/AppData/Local/Microsoft/WinGet/Links")
      << QStringLiteral("C:/Program Files/nodejs");
#endif
    return d;
}

inline QString findExe(const QString& name)
{
    QString p = QStandardPaths::findExecutable(name);
    if (p.isEmpty()) p = QStandardPaths::findExecutable(name, extraBinDirs());
    return p;
}

inline QString dataDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return d;
}

inline QString cacheDir(const QString& sub)
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1Char('/') + sub;
    QDir().mkpath(d);
    return d;
}

// Where the in-app installer puts yt-dlp.
inline QString managedPath()
{
    return dataDir() + QStringLiteral("/bin/") +
#ifdef Q_OS_WIN
        QStringLiteral("yt-dlp.exe");
#else
        QStringLiteral("yt-dlp");
#endif
}

inline QString findYtDlp()
{
    QSettings s;
    const QString custom = s.value(QStringLiteral("yt/path")).toString();
    if (!custom.isEmpty() && QFileInfo::exists(custom)) return custom;
    if (QFileInfo::exists(managedPath())) return managedPath();
    const QString beside = QCoreApplication::applicationDirPath() + QLatin1Char('/')
#ifdef Q_OS_WIN
        + QStringLiteral("yt-dlp.exe");
#else
        + QStringLiteral("yt-dlp");
#endif
    if (QFileInfo::exists(beside)) return beside;
    return findExe(QStringLiteral("yt-dlp"));
}

// yt-dlp needs an external JavaScript runtime for reliable YouTube support.
// Returns {runtime name, path}; empty when none is installed.
inline QPair<QString, QString> jsRuntime()
{
    static const char* names[] = { "deno", "node", "bun", "qjs" };
    for (const char* n : names) {
        const QString p = findExe(QString::fromLatin1(n));
        if (p.isEmpty()) continue;
        const QString nm = QString::fromLatin1(n);
        return qMakePair(nm == QLatin1String("qjs") ? QStringLiteral("quickjs") : nm, p);
    }
    return qMakePair(QString(), QString());
}

// Set when yt-dlp could not read the browser's cookie database (e.g. Chrome is open and locks it).
inline bool& cookiesDisabled() { static bool v = false; return v; }

inline QStringList baseArgs()
{
    QSettings s;
    QStringList a;
    a << QStringLiteral("--ignore-config") << QStringLiteral("--no-warnings")
      << QStringLiteral("--socket-timeout") << QStringLiteral("20")
      << QStringLiteral("--retries") << QStringLiteral("3");
    const QPair<QString, QString> js = jsRuntime();
    if (!js.first.isEmpty())
        a << QStringLiteral("--js-runtimes") << js.first + QLatin1Char(':') + js.second;
    const QString browser = s.value(QStringLiteral("yt/cookies")).toString();
    if (!browser.isEmpty() && !cookiesDisabled())
        a << QStringLiteral("--cookies-from-browser") << browser;
    return a;
}

// Cookies-from-browser and JS runtime flags (see Settings); empty when neither is configured.
inline QStringList extraArgs()
{
    QSettings s;
    QStringList a;
    const QPair<QString, QString> js = jsRuntime();
    if (!js.first.isEmpty())
        a << QStringLiteral("--js-runtimes") << js.first + QLatin1Char(':') + js.second;
    const QString browser = s.value(QStringLiteral("yt/cookies")).toString();
    if (!browser.isEmpty() && !cookiesDisabled())
        a << QStringLiteral("--cookies-from-browser") << browser;
    return a;
}

inline void pruneAudioCache()
{
    QDir d(cacheDir(QStringLiteral("audio")));
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-3);
    const QFileInfoList files = d.entryInfoList(QDir::Files);
    for (const QFileInfo& fi : files)
        if (fi.lastModified() < cutoff) QFile::remove(fi.absoluteFilePath());
}

inline void clearAudioCache()
{
    QDir d(cacheDir(QStringLiteral("audio")));
    const QFileInfoList files = d.entryInfoList(QDir::Files);
    for (const QFileInfo& fi : files) QFile::remove(fi.absoluteFilePath());
}

inline bool validId(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    return re.match(s).hasMatch();
}

inline QString thumbUrl(const QString& id, const char* quality)
{
    return QStringLiteral("https://i.ytimg.com/vi/%1/%2.jpg").arg(id, QLatin1String(quality));
}

// "Song (Official Video) [HD]" -> "Song"
inline QString cleanTitle(QString t)
{
    static const QRegularExpression bracketed(
        QStringLiteral(R"(\s*[\(\[\{]\s*(?:official|lyrics?|lyric|audio|video|music\s*video|visuali[sz]er|hd|hq|4k|explicit|remaster|full\s*(?:song|album)|clip)[^\)\]\}]*[\)\]\}])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression trailing(
        QStringLiteral(R"(\s*[|\x{FF5C}]\s*(?:official|lyrics?|audio|video)[^|]*$)"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(bracketed);
    t.remove(trailing);
    return t.simplified();
}

// "ArtistVEVO" / "Artist - Topic" / "Artist Official" -> "Artist"
inline QString cleanArtist(QString a)
{
    static const QRegularExpression topic(QStringLiteral(R"(\s*-\s*Topic$)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression vevo(QStringLiteral(R"(\s*VEVO$)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression off(QStringLiteral(R"(\s+Official(?:\s+Channel)?$)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression camel(QStringLiteral(R"((?<=[a-z])(?=[A-Z]))"));
    const bool wasVevo = a.contains(vevo);
    a.remove(topic);
    a.remove(vevo);
    a.remove(off);
    if (wasVevo && !a.contains(QLatin1Char(' '))) a.replace(camel, QStringLiteral(" "));   // "EdSheeran" -> "Ed Sheeran"
    return a.simplified();
}

// Is "left" in "Left - Right" plausibly an artist name (vs. part of a long descriptive title)?
inline bool looksLikeArtist(const QString& left, const QString& channel)
{
    const QString l = left.trimmed().toLower();
    const QString c = cleanArtist(channel).toLower();
    if (!c.isEmpty() && (l == c || l.contains(c) || c.contains(l))) return true;     // matches the uploader
    if (l.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() > 5) return false;
    static const QRegularExpression notArtist(
        QStringLiteral(R"(\b(radio|mix|playlist|live|stream|beats|lo-?fi|hours?|compilation|episode|podcast|tutorial|trailer|full album)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return !notArtist.match(l).hasMatch();
}

inline QString joinArtists(const QJsonObject& o)
{
    const QString single = o.value(QLatin1String("artist")).toString().trimmed();
    if (!single.isEmpty()) return single;
    QStringList l;
    for (const QJsonValue& v : o.value(QLatin1String("artists")).toArray())
        if (!v.toString().trimmed().isEmpty()) l << v.toString().trimmed();
    return l.join(QStringLiteral(", "));
}

// Works for both flat search entries and full single-video JSON.
inline OnlineItem fromJson(const QJsonObject& o)
{
    OnlineItem it;
    it.id = o.value(QLatin1String("id")).toString();
    if (!validId(it.id)) return {};

    const QString rawTitle = o.value(QLatin1String("title")).toString();
    QString channel = o.value(QLatin1String("channel")).toString();
    if (channel.isEmpty()) channel = o.value(QLatin1String("uploader")).toString();
    const bool topic = channel.contains(QStringLiteral("- Topic"), Qt::CaseInsensitive);

    const QString ytTrack  = o.value(QLatin1String("track")).toString().simplified();
    const QString ytArtist = joinArtists(o);
    const QString ytAlbum  = o.value(QLatin1String("album")).toString().simplified();

    QString title  = ytTrack.isEmpty() ? cleanTitle(rawTitle) : ytTrack;
    QString artist = ytArtist;
    if (artist.isEmpty()) {
        artist = cleanArtist(channel);
        // "Artist - Song" is the usual upload convention on non-Topic channels.
        if (ytTrack.isEmpty() && !topic) {
            static const QRegularExpression sep(QStringLiteral(R"(^(.{1,60}?)\s+[-\x{2013}\x{2014}]\s+(.+)$)"));
            const auto m = sep.match(title);
            if (m.hasMatch() && looksLikeArtist(m.captured(1), channel)) {
                artist = m.captured(1).trimmed();
                title  = m.captured(2).trimmed();
            }
        }
    }
    if (title.isEmpty())  title = rawTitle.isEmpty() ? QStringLiteral("Unknown title") : rawTitle;
    if (artist.isEmpty()) artist = QStringLiteral("YouTube");

    it.title  = title;
    it.artist = artist;
    it.album  = !ytAlbum.isEmpty() ? ytAlbum
              : (!channel.isEmpty() && cleanArtist(channel).compare(artist, Qt::CaseInsensitive) != 0)
                    ? cleanArtist(channel) : QString();
    it.durationMs = qint64(o.value(QLatin1String("duration")).toDouble() * 1000.0);
    it.thumbUrl   = thumbUrl(it.id, "mqdefault");
    return it;
}

inline QJsonObject toJson(const OnlineItem& it)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), it.id);
    o.insert(QStringLiteral("title"), it.title);
    o.insert(QStringLiteral("artist"), it.artist);
    o.insert(QStringLiteral("album"), it.album);
    o.insert(QStringLiteral("dur"), double(it.durationMs));
    return o;
}

inline OnlineItem fromStored(const QJsonObject& o)
{
    OnlineItem it;
    it.id = o.value(QLatin1String("id")).toString();
    if (!validId(it.id)) return {};
    it.title  = o.value(QLatin1String("title")).toString();
    it.artist = o.value(QLatin1String("artist")).toString();
    it.album  = o.value(QLatin1String("album")).toString();
    it.durationMs = qint64(o.value(QLatin1String("dur")).toDouble());
    it.thumbUrl = thumbUrl(it.id, "mqdefault");
    return it;
}

// Last meaningful line of yt-dlp's stderr, made human-friendly.
inline QString friendlyError(const QString& raw)
{
    if (raw.trimmed().isEmpty()) return QStringLiteral("Something went wrong while talking to yt-dlp");
    const QString r = raw.toLower();
    auto has = [&r](const char* s) { return r.contains(QLatin1String(s)); };
    if (has("sign in to confirm") || has("not a bot"))
        return QStringLiteral("YouTube wants a sign-in check \u2013 pick a browser under \u201cUse cookies from\u201d in Settings");
    if (has("requested format is not available"))
        return QStringLiteral("No playable audio format \u2013 update yt-dlp and install a JavaScript runtime (Settings)");
    if (has("private video") || has("video unavailable") || has("this video is not available")
        || has("has been removed") || has("this video is no longer available"))
        return QStringLiteral("This video isn\u2019t available");
    if (has("403"))
        return QStringLiteral("YouTube refused the stream (403) \u2013 try updating yt-dlp in Settings");
    if (has("was not found") || has("couldn't start"))
        return QStringLiteral("yt-dlp isn\u2019t installed \u2013 open Settings to get it");
    // Legacy messages
    if (raw.contains(QStringLiteral("Sign in to confirm"), Qt::CaseInsensitive))
        return QStringLiteral("YouTube wants a sign-in / bot check. Updating yt-dlp usually fixes this.");
    if (raw.contains(QStringLiteral("Video unavailable"), Qt::CaseInsensitive))
        return QStringLiteral("This video is unavailable.");
    if (raw.contains(QStringLiteral("not available in your country"), Qt::CaseInsensitive))
        return QStringLiteral("This video is blocked in your country.");
    QString line = raw.trimmed();
    line.remove(QRegularExpression(QStringLiteral(R"(^ERROR:\s*)")));
    if (line.size() > 180) line = line.left(177) + QStringLiteral("...");
    return line;
}

} // namespace Yt

// ============================================================================
//  YtDlp: async search / stream resolving / install / update
// ============================================================================
class YtDlp : public QObject
{
    Q_OBJECT
public:
    explicit YtDlp(QObject* parent = nullptr) : QObject(parent)
    {
        m_nam = new QNetworkAccessManager(this);
    }

    static QString exeName()
    {
#ifdef Q_OS_WIN
        return QStringLiteral("yt-dlp.exe");
#else
        return QStringLiteral("yt-dlp");
#endif
    }

    static QString installPath()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + QStringLiteral("/bin/") + exeName();
    }

    // Resolution order: user-chosen path, Pulsify's own copy, PATH, next to the app,
    // then common install dirs that GUI apps often don't have on PATH.
    QString binary() const
    {
        const QString custom = QSettings().value(QStringLiteral("ytdlpPath")).toString();
        if (!custom.isEmpty() && QFileInfo(custom).isExecutable()) return custom;
        if (QFileInfo(installPath()).isExecutable()) return installPath();
        QString p = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
        if (!p.isEmpty()) return p;
        const QString beside = QCoreApplication::applicationDirPath() + QLatin1Char('/') + exeName();
        if (QFileInfo(beside).isExecutable()) return beside;
        const QStringList dirs = { QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin"),
                                   QDir::homePath() + QStringLiteral("/.local/bin") };
        return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"), dirs);
    }
    bool available() const { return !binary().isEmpty(); }

    void setCustomBinary(const QString& path)
    {
        QSettings().setValue(QStringLiteral("ytdlpPath"), path);
    }

    // ---- search: streams batches of results as yt-dlp prints them
    quint64 search(const QString& query, int count = 30)
    {
        cancelSearch();
        const quint64 tok = ++m_tok;
        m_searchTok = tok;

        QProcess* p = spawn({ QStringLiteral("--flat-playlist"), QStringLiteral("--dump-json"),
                              QStringLiteral("--no-warnings"), QStringLiteral("--ignore-config"),
                              QStringLiteral("--socket-timeout"), QStringLiteral("15"),
                              QStringLiteral("ytsearch%1:%2").arg(count).arg(query) });
        m_search = p;
        auto out = std::make_shared<QByteArray>();
        auto err = std::make_shared<QByteArray>();

        auto drain = [this, p, out, tok](bool flush) {
            out->append(p->readAllStandardOutput());
            QVector<OnlineItem> batch;
            int nl;
            while ((nl = int(out->indexOf('\n'))) >= 0 || (flush && !out->trimmed().isEmpty())) {
                const QByteArray line = nl >= 0 ? out->left(nl) : *out;
                out->remove(0, nl >= 0 ? nl + 1 : out->size());
                const QJsonObject o = QJsonDocument::fromJson(line).object();
                if (o.value(QLatin1String("live_status")).toString() == QLatin1String("is_live")) continue;
                const OnlineItem it = Yt::fromJson(o);
                if (it.valid()) batch.push_back(it);
            }
            if (!batch.isEmpty() && m_searchTok == tok) emit searchBatch(tok, batch);
        };

        connect(p, &QProcess::readyReadStandardOutput, this, [drain] { drain(false); });
        connect(p, &QProcess::readyReadStandardError, this, [p, err] { err->append(p->readAllStandardError()); });
        connect(p, &QProcess::errorOccurred, this, [this, p, tok](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            if (m_search == p) m_search = nullptr;
            p->deleteLater();
            if (m_searchTok == tok) emit searchDone(tok, QStringLiteral("Couldn't start yt-dlp"));
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, p, err, tok, drain](int code, QProcess::ExitStatus st) {
            drain(true);
            err->append(p->readAllStandardError());
            if (m_search == p) m_search = nullptr;
            p->deleteLater();
            if (m_searchTok != tok) return;     // superseded / cancelled
            const bool ok = (st == QProcess::NormalExit && code == 0);
            emit searchDone(tok, ok ? QString() : Yt::friendlyError(QString::fromUtf8(*err)));
        });
        return tok;
    }

    void cancelSearch()
    {
        m_searchTok = 0;
        if (m_search) { m_search->kill(); m_search = nullptr; }
    }

    // ---- independent, non-streaming searches used by the Home shelves.
    //      Queued; at most two yt-dlp processes run at once. Results that are not songs
    //      (live streams, hour-long mixes) are dropped.
    quint64 fetch(const QString& query, int count = 24)
    {
        const quint64 tok = ++m_tok;
        m_fetchQueue.enqueue({ tok, query, count });
        pumpFetch();
        return tok;
    }

    void cancelFetches()
    {
        m_fetchQueue.clear();
        ++m_fetchEpoch;                               // anything still running is ignored
        for (QProcess* p : m_fetchProcs) p->kill();
    }

    // ---- resolve a direct audio-stream URL (+ richer metadata) for one video
    quint64 resolve(const QString& videoId)
    {
        cancelResolve();
        return startResolve(videoId, &m_resolve, &m_resolveTok, false);
    }

    void cancelResolve()
    {
        m_resolveTok = 0;
        if (m_resolve) { m_resolve->kill(); m_resolve = nullptr; }
    }

    // Same thing for the song that will play next, so the gap between songs is short.
    // Result arrives through prefetched(); it never touches the current resolve.
    void prefetch(const QString& videoId)
    {
        if (!Yt::validId(videoId) || videoId == m_prefetchVideo) return;
        cancelPrefetch();
        m_prefetchVideo = videoId;
        startResolve(videoId, &m_prefetch, &m_prefetchTok, true);
    }

    void cancelPrefetch()
    {
        m_prefetchTok = 0;
        m_prefetchVideo.clear();
        if (m_prefetch) { m_prefetch->kill(); m_prefetch = nullptr; }
    }

    // ---- import a whole YouTube / YouTube Music playlist from a link (no sign-in needed;
    //      works for public and unlisted playlists)
    static QString playlistIdFromLink(const QString& text)
    {
        const QString t = text.trimmed();
        static const QRegularExpression rawId(QStringLiteral("^[A-Za-z0-9_-]{13,80}$"));
        if (rawId.match(t).hasMatch()) return t;
        QUrl u(t.contains(QStringLiteral("://")) ? t : QStringLiteral("https://") + t);
        const QString host = u.host().toLower();
        if (!(host == QLatin1String("youtube.com") || host.endsWith(QLatin1String(".youtube.com"))
              || host == QLatin1String("youtu.be")))
            return QString();
        const QString list = QUrlQuery(u).queryItemValue(QStringLiteral("list"));
        return rawId.match(list).hasMatch() ? list : QString();
    }

    quint64 importPlaylist(const QString& playlistId)
    {
        cancelImport();
        const quint64 tok = ++m_tok;
        m_importTok = tok;
        QProcess* p = spawn({ QStringLiteral("--flat-playlist"), QStringLiteral("--dump-json"),
                              QStringLiteral("--no-warnings"), QStringLiteral("--ignore-config"),
                              QStringLiteral("--yes-playlist"),
                              QStringLiteral("--playlist-end"), QStringLiteral("1000"),
                              QStringLiteral("--socket-timeout"), QStringLiteral("20"),
                              QStringLiteral("--"),
                              QStringLiteral("https://www.youtube.com/playlist?list=") + playlistId });
        m_import = p;
        auto out = std::make_shared<QByteArray>();
        auto err = std::make_shared<QByteArray>();
        connect(p, &QProcess::readyReadStandardOutput, this, [p, out] { out->append(p->readAllStandardOutput()); });
        connect(p, &QProcess::readyReadStandardError, this, [p, err] { err->append(p->readAllStandardError()); });
        QTimer::singleShot(180000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
        connect(p, &QProcess::errorOccurred, this, [this, p, tok](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            if (m_import == p) m_import = nullptr;
            p->deleteLater();
            if (m_importTok == tok) emit playlistImported(tok, QString(), {}, QStringLiteral("Couldn't start yt-dlp"));
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, p, out, err, tok](int code, QProcess::ExitStatus st) {
            out->append(p->readAllStandardOutput());
            err->append(p->readAllStandardError());
            if (m_import == p) m_import = nullptr;
            p->deleteLater();
            if (m_importTok != tok) return;
            QVector<OnlineItem> items;
            QSet<QString> seen;
            QString title;
            for (const QByteArray& line : out->split('\n')) {
                if (line.trimmed().isEmpty()) continue;
                const QJsonObject o = QJsonDocument::fromJson(line).object();
                if (title.isEmpty()) title = o.value(QLatin1String("playlist_title")).toString().trimmed();
                const QString t = o.value(QLatin1String("title")).toString();
                if (t == QLatin1String("[Private video]") || t == QLatin1String("[Deleted video]")) continue;
                if (o.value(QLatin1String("live_status")).toString() == QLatin1String("is_live")) continue;
                const OnlineItem it = Yt::fromJson(o);
                if (!it.valid() || seen.contains(it.id)) continue;
                seen.insert(it.id);
                items.push_back(it);
            }
            if (items.isEmpty()) {
                const bool ok = (st == QProcess::NormalExit && code == 0);
                emit playlistImported(tok, title, {},
                    ok ? QStringLiteral("That playlist is empty, or every video in it is private or deleted.")
                       : Yt::friendlyError(QString::fromUtf8(*err)));
                return;
            }
            // yt-dlp may exit non-zero when a few entries failed; what we got is still useful.
            emit playlistImported(tok, title, items, QString());
        });
        return tok;
    }

    void cancelImport()
    {
        m_importTok = 0;
        if (m_import) { m_import->kill(); m_import = nullptr; }
    }

    // ---- download Pulsify's own copy of yt-dlp from the official GitHub release
    void install()
    {
        QString asset;
#if defined(Q_OS_WIN)
        asset = QStringLiteral("yt-dlp.exe");
#elif defined(Q_OS_MACOS)
        asset = QStringLiteral("yt-dlp_macos");
#else
        asset = QSysInfo::currentCpuArchitecture().startsWith(QLatin1String("arm"))
                    ? QStringLiteral("yt-dlp_linux_aarch64") : QStringLiteral("yt-dlp_linux");
#endif
        emit toolMessage(QStringLiteral("Downloading yt-dlp..."));
        QNetworkRequest rq(QUrl(QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/latest/download/") + asset));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* r = m_nam->get(rq);
        connect(r, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
            if (total > 0)
                emit toolMessage(QStringLiteral("Downloading yt-dlp... %1%").arg(int(got * 100 / total)));
        });
        connect(r, &QNetworkReply::finished, this, [this, r] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) {
                emit toolFinished(false, QStringLiteral("Download failed: ") + r->errorString());
                return;
            }
            const QByteArray data = r->readAll();
            if (data.size() < 1000000) {          // real binaries are >10 MB
                emit toolFinished(false, QStringLiteral("Download looks incomplete."));
                return;
            }
            QDir().mkpath(QFileInfo(installPath()).absolutePath());
            QSaveFile f(installPath());
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
                emit toolFinished(false, QStringLiteral("Couldn't write ") + installPath());
                return;
            }
            QFile::setPermissions(installPath(), QFile::permissions(installPath())
                                  | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther
                                  | QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            emit toolFinished(true, QStringLiteral("yt-dlp installed."));
        });
    }

    // YouTube changes often; `yt-dlp -U` fixes most sudden failures.
    void selfUpdate()
    {
        if (!available()) { emit toolFinished(false, QStringLiteral("yt-dlp not found")); return; }
        emit toolMessage(QStringLiteral("Updating yt-dlp..."));
        QProcess* p = spawn({ QStringLiteral("-U") });
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) { p->deleteLater(); emit toolFinished(false, QStringLiteral("Couldn't start yt-dlp")); }
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, p](int code, QProcess::ExitStatus) {
            const QString all = QString::fromUtf8(p->readAllStandardOutput() + p->readAllStandardError()).trimmed();
            p->deleteLater();
            if (code == 0) emit toolFinished(true, QStringLiteral("yt-dlp is up to date."));
            else emit toolFinished(false, QStringLiteral("Update failed (installed via a package manager? update it there). ")
                                          + all.section(QLatin1Char('\n'), -1).left(120));
        });
    }

signals:
    void fetched(quint64 token, const QVector<OnlineItem>& items, const QString& error);
    void searchBatch(quint64 token, const QVector<OnlineItem>& items);
    void searchDone(quint64 token, const QString& error);
    void resolved(quint64 token, const QString& videoId, const QString& streamUrl,
                  const OnlineItem& meta, const QString& error);
    void prefetched(const QString& videoId, const QString& streamUrl,
                    const OnlineItem& meta, const QString& error);
    void playlistImported(quint64 token, const QString& title, const QVector<OnlineItem>& items,
                          const QString& error);
    void toolMessage(const QString& msg);
    void toolFinished(bool ok, const QString& msg);

private:
    struct FetchJob { quint64 tok; QString query; int count; };

    // Shared by resolve() and prefetch(): `slot` / `tokSlot` point at the member that tracks this job.
    quint64 startResolve(const QString& videoId, QProcess** slot, quint64* tokSlot, bool isPrefetch)
    {
        const quint64 tok = ++m_tok;
        *tokSlot = tok;
        auto report = [this, tok, tokSlot, isPrefetch, videoId](const QString& url, const OnlineItem& meta,
                                                               const QString& err) {
            if (*tokSlot != tok) return;                       // superseded / cancelled
            if (isPrefetch) { m_prefetchVideo.clear(); emit prefetched(videoId, url, meta, err); }
            else            emit resolved(tok, videoId, url, meta, err);
        };
        if (!Yt::validId(videoId)) {
            QTimer::singleShot(0, this, [report] { report({}, {}, QStringLiteral("Invalid video id")); });
            return tok;
        }

        // Plain https m4a (AAC) first: every Qt multimedia backend can demux it, and range
        // requests against it allow the StreamRelay to work reliably.
        QProcess* p = spawn({ QStringLiteral("-f"),
                              QStringLiteral("bestaudio[ext=m4a][protocol=https]/bestaudio[protocol=https]/"
                                             "bestaudio[ext=m4a]/bestaudio/best"),
                              QStringLiteral("--no-playlist"), QStringLiteral("--dump-json"),
                              QStringLiteral("--no-warnings"), QStringLiteral("--ignore-config"),
                              QStringLiteral("--socket-timeout"), QStringLiteral("15"),
                              QStringLiteral("--"),
                              QStringLiteral("https://www.youtube.com/watch?v=") + videoId });
        *slot = p;
        auto out = std::make_shared<QByteArray>();
        auto err = std::make_shared<QByteArray>();
        connect(p, &QProcess::readyReadStandardOutput, this, [p, out] { out->append(p->readAllStandardOutput()); });
        connect(p, &QProcess::readyReadStandardError, this, [p, err] { err->append(p->readAllStandardError()); });
        QTimer::singleShot(60000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });

        connect(p, &QProcess::errorOccurred, this, [slot, p, report](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            if (*slot == p) *slot = nullptr;
            p->deleteLater();
            report({}, {}, QStringLiteral("Couldn't start yt-dlp"));
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [slot, p, out, err, report](int code, QProcess::ExitStatus st) {
            out->append(p->readAllStandardOutput());
            err->append(p->readAllStandardError());
            if (*slot == p) *slot = nullptr;
            p->deleteLater();

            if (st != QProcess::NormalExit || code != 0) {
                report({}, {}, Yt::friendlyError(QString::fromUtf8(*err)));
                return;
            }
            QJsonObject o;
            for (const QByteArray& l : out->split('\n')) {
                const QJsonObject c = QJsonDocument::fromJson(l).object();
                if (!c.isEmpty()) o = c;
            }
            QString url = o.value(QLatin1String("url")).toString();
            if (url.isEmpty()) {
                const QJsonArray rf = o.value(QLatin1String("requested_formats")).toArray();
                for (const QJsonValue& v : rf)
                    if (v.toObject().value(QLatin1String("vcodec")).toString() == QLatin1String("none"))
                        url = v.toObject().value(QLatin1String("url")).toString();
            }
            if (url.isEmpty()) {
                report({}, {}, QStringLiteral("No playable audio stream found"));
                return;
            }
            OnlineItem meta = Yt::fromJson(o);
            StreamInfo& si = meta.stream;
            si.container   = o.value(QLatin1String("ext")).toString();
            si.codec       = o.value(QLatin1String("acodec")).toString();
            si.bitrateKbps = int(o.value(QLatin1String("abr")).toDouble() + 0.5);
            si.sampleRate  = o.value(QLatin1String("asr")).toInt();
            si.views       = qint64(o.value(QLatin1String("view_count")).toDouble());
            si.uploadDate  = o.value(QLatin1String("upload_date")).toString();
            si.channel     = o.value(QLatin1String("channel")).toString();
            if (si.channel.isEmpty()) si.channel = o.value(QLatin1String("uploader")).toString();
            si.webUrl      = o.value(QLatin1String("webpage_url")).toString();
            const QJsonObject hh = o.value(QLatin1String("http_headers")).toObject();
            for (auto it = hh.begin(); it != hh.end(); ++it)
                si.headers.push_back({ it.key().toUtf8(), it.value().toString().toUtf8() });
            report(url, meta, {});
        });
        return tok;
    }

    void pumpFetch()
    {
        while (m_fetchActive < 2 && !m_fetchQueue.isEmpty()) startFetch(m_fetchQueue.dequeue());
    }

    void startFetch(const FetchJob& job)
    {
        ++m_fetchActive;
        const int epoch = m_fetchEpoch;
        QProcess* p = spawn({ QStringLiteral("--flat-playlist"), QStringLiteral("--dump-json"),
                              QStringLiteral("--no-warnings"), QStringLiteral("--ignore-config"),
                              QStringLiteral("--socket-timeout"), QStringLiteral("15"),
                              QStringLiteral("ytsearch%1:%2").arg(job.count).arg(job.query) });
        m_fetchProcs.insert(p);
        auto out = std::make_shared<QByteArray>();
        auto err = std::make_shared<QByteArray>();
        auto finished = std::make_shared<bool>(false);

        auto done = [this, p, job, epoch, out, err, finished](bool ok, const QString& startErr) {
            if (*finished) return;
            *finished = true;
            --m_fetchActive;
            m_fetchProcs.remove(p);
            p->deleteLater();
            if (epoch == m_fetchEpoch) {
                if (!startErr.isEmpty()) {
                    emit fetched(job.tok, {}, startErr);
                } else if (!ok) {
                    emit fetched(job.tok, {}, Yt::friendlyError(QString::fromUtf8(*err)));
                } else {
                    QVector<OnlineItem> items;
                    QSet<QString> seen;
                    for (const QByteArray& line : out->split('\n')) {
                        if (line.trimmed().isEmpty()) continue;
                        const QJsonObject o = QJsonDocument::fromJson(line).object();
                        if (o.value(QLatin1String("live_status")).toString() == QLatin1String("is_live")) continue;
                        const OnlineItem it = Yt::fromJson(o);
                        if (!it.valid() || seen.contains(it.id)) continue;
                        if (it.durationMs > 0 && (it.durationMs < 45000 || it.durationMs > 600000)) continue;
                        seen.insert(it.id);
                        items.push_back(it);
                    }
                    emit fetched(job.tok, items, QString());
                }
            }
            QTimer::singleShot(0, this, [this] { pumpFetch(); });
        };

        connect(p, &QProcess::readyReadStandardOutput, this, [p, out] { out->append(p->readAllStandardOutput()); });
        connect(p, &QProcess::readyReadStandardError, this, [p, err] { err->append(p->readAllStandardError()); });
        connect(p, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) done(false, QStringLiteral("Couldn't start yt-dlp"));
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [p, out, err, done](int code, QProcess::ExitStatus st) {
            out->append(p->readAllStandardOutput());
            err->append(p->readAllStandardError());
            done(st == QProcess::NormalExit && code == 0, QString());
        });
        // A failed start can be reported before the connections above exist.
        if (p->state() == QProcess::NotRunning && p->error() == QProcess::FailedToStart)
            QTimer::singleShot(0, this, [done] { done(false, QStringLiteral("Couldn't start yt-dlp")); });
        QTimer::singleShot(45000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }

    QProcess* spawn(const QStringList& args)
    {
        auto* p = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
        // Augment PATH so GUI apps find yt-dlp and JS runtimes in common install locations
        QString path = env.value(QStringLiteral("PATH"));
        for (const QString& d : Yt::extraBinDirs()) path += QString(QDir::listSeparator()) + d;
        env.insert(QStringLiteral("PATH"), path);
#ifdef Q_OS_MACOS
        env.insert(QStringLiteral("PATH"), env.value(QStringLiteral("PATH"))
                   + QStringLiteral(":/opt/homebrew/bin:/usr/local/bin"));
#endif
        p->setProcessEnvironment(env);
#ifdef Q_OS_WIN
        p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;       // no console flash from a GUI app
        });
#endif
        p->start(binary(), args.contains(QStringLiteral("-U")) ? args : Yt::extraArgs() + args);
        return p;
    }

    QNetworkAccessManager* m_nam = nullptr;
    QProcess* m_search = nullptr;
    QProcess* m_resolve = nullptr;
    QProcess* m_prefetch = nullptr;
    QProcess* m_import = nullptr;
    QString   m_prefetchVideo;
    quint64 m_tok = 0, m_searchTok = 0, m_resolveTok = 0, m_prefetchTok = 0, m_importTok = 0;
    QQueue<FetchJob> m_fetchQueue;
    QSet<QProcess*>  m_fetchProcs;
    int m_fetchActive = 0;
    int m_fetchEpoch = 0;
};

