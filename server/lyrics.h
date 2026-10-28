#pragma once
#include <QtCore>
#include <QtNetwork>
#include <algorithm>
#include <functional>
#include <memory>
#include "online.h"

// ============================================================================
//  Lyrics: text from Genius, timing from LRCLIB.
//
//  Genius has no timestamps at all, so a line can only be "synced" if the timing
//  comes from somewhere else. LRCLIB (a free community database of .lrc files)
//  supplies the times; each Genius line is matched to the LRC line it corresponds
//  to, so you read Genius' text (with its [Verse]/[Chorus] markers) on LRCLIB's
//  clock. If a song has no timed version, the Genius text is shown un-synced
//  instead of faking a sync.
// ============================================================================

struct LyricLine {
    QString text;
    qint64  timeMs = -1;      // -1 = not timed (section headers, blank lines, un-synced lyrics)
    bool    header = false;   // "[Chorus]" style marker
};

struct LyricsDoc {
    QVector<LyricLine> lines;
    bool    synced = false;
    bool    instrumental = false;
    bool    approx = false;           // timing is from a recording whose length differs noticeably
    QString textSource, timeSource;
    int     matchedPct = -1;          // share of Genius lines that were matched to a timed line

    bool isEmpty() const { return lines.isEmpty() && !instrumental; }

    QJsonObject toJson() const
    {
        QJsonArray arr;
        for (const LyricLine& l : lines) {
            QJsonArray a;
            a.append(double(l.timeMs));
            a.append(l.text);
            a.append(l.header);
            arr.append(a);
        }
        QJsonObject o;
        o.insert(QStringLiteral("v"), 1);
        o.insert(QStringLiteral("saved"), double(QDateTime::currentSecsSinceEpoch()));
        o.insert(QStringLiteral("synced"), synced);
        o.insert(QStringLiteral("instrumental"), instrumental);
        o.insert(QStringLiteral("approx"), approx);
        o.insert(QStringLiteral("textSource"), textSource);
        o.insert(QStringLiteral("timeSource"), timeSource);
        o.insert(QStringLiteral("matchedPct"), matchedPct);
        o.insert(QStringLiteral("lines"), arr);
        return o;
    }

    static LyricsDoc fromJson(const QJsonObject& o)
    {
        LyricsDoc d;
        d.synced = o.value(QLatin1String("synced")).toBool();
        d.instrumental = o.value(QLatin1String("instrumental")).toBool();
        d.approx = o.value(QLatin1String("approx")).toBool();
        d.textSource = jstr(o, "textSource");
        d.timeSource = jstr(o, "timeSource");
        d.matchedPct = o.value(QLatin1String("matchedPct")).toInt(-1);
        const QJsonArray arr = jarr(o, "lines");
        for (const QJsonValue& v : arr) {
            const QJsonArray a = v.toArray();
            LyricLine l;
            l.timeMs = qint64(a.at(0).toDouble(-1));
            l.text = a.at(1).toString();
            l.header = a.at(2).toBool();
            d.lines.push_back(l);
        }
        return d;
    }
};

// ---------------------------------------------------------------------------
//  .lrc parsing
// ---------------------------------------------------------------------------
struct TimedText {
    qint64  t = 0;
    QString text;
};

inline QVector<TimedText> parseLrc(const QString& lrc)
{
    static const QRegularExpression stamp(QStringLiteral(R"RX(\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])RX"));
    QVector<TimedText> out;
    const QStringList lines = lrc.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        QString line = raw;
        line.remove(QLatin1Char('\r'));
        QVector<qint64> times;
        int pos = 0;
        for (;;) {
            const QRegularExpressionMatch m = stamp.match(line, pos);
            if (!m.hasMatch() || m.capturedStart() != pos) break;
            qint64 ms = m.captured(1).toLongLong() * 60000 + m.captured(2).toLongLong() * 1000;
            const QString frac = m.captured(3);
            if (!frac.isEmpty()) ms += frac.leftJustified(3, QLatin1Char('0')).left(3).toInt();
            times.push_back(ms);
            pos = int(m.capturedEnd());
        }
        if (times.isEmpty()) continue;                 // [ar:..] style tags and junk
        const QString text = line.mid(pos).trimmed();
        for (qint64 t : times) {
            TimedText tt;
            tt.t = t;
            tt.text = text;
            out.push_back(tt);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const TimedText& a, const TimedText& b) { return a.t < b.t; });
    return out;
}

// ---------------------------------------------------------------------------
//  Matching helpers
// ---------------------------------------------------------------------------
inline QString lyricKey(const QString& s)              // letters + digits only, lower-case
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s)
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}

// Like lyricKey, but ignores "(backing vocals)" which timed versions often leave out.
inline QString lineKey(const QString& s)
{
    static const QRegularExpression paren(QStringLiteral(R"RX(\([^)]*\))RX"));
    QString t = s;
    t.remove(paren);
    QString k = lyricKey(t);
    if (k.isEmpty()) k = lyricKey(s);
    return k;
}

// Sorensen-Dice similarity on character bigrams (works for any script).
inline double bigramDice(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    if (a == b) return 1.0;
    if (a.size() < 2 || b.size() < 2) return 0.0;
    QHash<quint32, int> ca;
    for (int i = 0; i + 1 < a.size(); ++i)
        ++ca[(quint32(a.at(i).unicode()) << 16) | quint32(a.at(i + 1).unicode())];
    int common = 0;
    for (int i = 0; i + 1 < b.size(); ++i) {
        const quint32 bg = (quint32(b.at(i).unicode()) << 16) | quint32(b.at(i + 1).unicode());
        const auto it = ca.find(bg);
        if (it != ca.end() && it.value() > 0) { --it.value(); ++common; }
    }
    return 2.0 * common / double((a.size() - 1) + (b.size() - 1));
}

struct AlignResult {
    QVector<LyricLine> lines;
    int matchedPct = 0;
};

// Gives every line of the Genius text a time taken from the timed (LRC) lines.
// Lines that can't be matched are interpolated between their matched neighbours.
inline AlignResult alignLyrics(const QStringList& gl, const QVector<TimedText>& lrc)
{
    AlignResult res;
    const int n = int(gl.size());
    const int m = int(lrc.size());
    res.lines.resize(n);

    QVector<QString> gk(n), lk(m);
    QVector<int> real;                                   // lines that actually carry words
    for (int i = 0; i < n; ++i) {
        LyricLine& L = res.lines[i];
        L.text = gl[i];
        const QString t = gl[i].trimmed();
        L.header = t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'));
        if (!t.isEmpty() && !L.header) {
            gk[i] = lineKey(t);
            if (!gk[i].isEmpty()) real.push_back(i);
        }
    }
    for (int j = 0; j < m; ++j) lk[j] = lineKey(lrc[j].text);

    QVector<qint64> when(n, -1);
    int j = 0, matched = 0;
    const int window = 12;
    for (int i : real) {
        int best = -1;
        double bestScore = 0.0;
        const int lim = qMin(m, j + window);
        for (int k = j; k < lim; ++k) {
            if (lk[k].isEmpty()) continue;
            double s = bigramDice(gk[i], lk[k]);
            if (k + 1 < m && !lk[k + 1].isEmpty())       // one Genius line may span two timed lines
                s = qMax(s, bigramDice(gk[i], lk[k] + lk[k + 1]));
            s -= 0.004 * (k - j);                        // prefer the nearest candidate on ties
            if (s > bestScore) { bestScore = s; best = k; }
        }
        if (best >= 0 && bestScore >= 0.55) {
            when[i] = lrc[best].t;
            j = best + 1;
            ++matched;
        }
    }
    res.matchedPct = real.isEmpty() ? 0 : int(100.0 * matched / real.size());

    QVector<int> anchors;
    for (int i : real) if (when[i] >= 0) anchors.push_back(i);
    if (anchors.isEmpty()) return res;

    auto weight = [&gk](int i) { return qMax(6, int(gk[i].size())); };

    // between two anchors: share the time by line length
    for (int a = 0; a + 1 < anchors.size(); ++a) {
        const int ia = anchors[a], ib = anchors[a + 1];
        QVector<int> gap;
        for (int i : real) if (i > ia && i < ib) gap.push_back(i);
        if (gap.isEmpty()) continue;
        const qint64 ta = when[ia], tb = when[ib];
        const qint64 span = qMax<qint64>(0, tb - ta);
        qint64 total = weight(ia);
        for (int g : gap) total += weight(g);
        qint64 cum = weight(ia);
        for (int g : gap) {
            when[g] = ta + span * cum / total;
            cum += weight(g);
        }
    }
    // before the first / after the last anchor: a few seconds per line
    {
        QVector<int> lead, trail;
        for (int i : real) {
            if (i < anchors.first()) lead.push_back(i);
            else if (i > anchors.last()) trail.push_back(i);
        }
        for (int k = 0; k < lead.size(); ++k)
            when[lead[k]] = qMax<qint64>(0, when[anchors.first()] - qint64(lead.size() - k) * 2500);
        for (int k = 0; k < trail.size(); ++k)
            when[trail[k]] = when[anchors.last()] + qint64(k + 1) * 3200;
    }
    qint64 prev = 0;                                     // times never run backwards
    for (int i : real) {
        if (when[i] < prev) when[i] = prev;
        prev = when[i];
        res.lines[i].timeMs = when[i];
    }
    return res;
}

// ---------------------------------------------------------------------------
//  Genius: search results + lyrics page scraping
// ---------------------------------------------------------------------------
struct GeniusHit {
    QString title, artist, url;
};

inline QVector<GeniusHit> parseGeniusSearch(const QJsonObject& root)
{
    QVector<GeniusHit> out;
    const QJsonObject resp = jobj(root, "response");
    QJsonArray hits = jarr(resp, "hits");                 // official API
    const QJsonArray sections = jarr(resp, "sections");   // genius.com/api/search/...
    for (const QJsonValue& s : sections) {
        const QJsonArray h = jarr(s.toObject(), "hits");
        for (const QJsonValue& v : h) hits.append(v);
    }
    for (const QJsonValue& v : hits) {
        const QJsonObject hit = v.toObject();
        const QString type = jstr(hit, "type");
        if (!type.isEmpty() && type != QLatin1String("song")) continue;
        const QJsonObject r = jobj(hit, "result");
        GeniusHit g;
        g.title = jstr(r, "title");
        g.artist = jstr(jobj(r, "primary_artist"), "name");
        g.url = jstr(r, "url");
        if (g.url.isEmpty() || g.title.isEmpty()) continue;
        if (g.artist.startsWith(QLatin1String("Genius"), Qt::CaseInsensitive)) continue;   // translation pages
        out.push_back(g);
    }
    return out;
}

inline int pickGenius(const QVector<GeniusHit>& hits, const QString& artist, const QString& title)
{
    const QString tk = lyricKey(title), ak = lyricKey(artist);
    int best = -1;
    double bestScore = 0.0;
    for (int i = 0; i < hits.size(); ++i) {
        const double ts = bigramDice(lyricKey(hits[i].title), tk);
        if (ts < 0.55) continue;
        double s = ts * 2.0;
        if (!ak.isEmpty()) {
            const QString hk = lyricKey(hits[i].artist);
            const double as = (!hk.isEmpty() && (hk.contains(ak) || ak.contains(hk))) ? 1.0 : bigramDice(hk, ak);
            if (as < 0.45) continue;                      // some other artist's song of the same name
            s += as;
        }
        s -= 0.02 * i;                                    // Genius' own ranking breaks ties
        if (s > bestScore) { bestScore = s; best = i; }
    }
    return best;
}

// Index just past the "</div>" that closes the div whose content starts at `from`.
// `contentEnd` receives the index of that closing tag.
inline int divEnd(const QString& s, int from, int* contentEnd)
{
    static const QRegularExpression tag(QStringLiteral(R"RX(</?div\b[^>]*>)RX"),
                                        QRegularExpression::CaseInsensitiveOption);
    int depth = 1;
    int pos = from;
    for (;;) {
        const QRegularExpressionMatch m = tag.match(s, pos);
        if (!m.hasMatch()) { *contentEnd = int(s.size()); return int(s.size()); }
        const QString t = m.captured(0);
        if (t.startsWith(QLatin1String("</"))) {
            if (--depth == 0) { *contentEnd = int(m.capturedStart()); return int(m.capturedEnd()); }
        } else if (!t.endsWith(QLatin1String("/>"))) {
            ++depth;
        }
        pos = int(m.capturedEnd());
    }
}

// Pulls the lyrics out of a genius.com song page (div[data-lyrics-container]).
// Minimal HTML entity decoder (so the server needs no QtGui).
inline QString fromCodepoint(uint cp)
{
    const char32_t c = char32_t(cp);
    return QString::fromUcs4(&c, 1);
}

inline QString decodeHtml(const QString& s)
{
    static const QHash<QString, QString> named = {
        { QStringLiteral("amp"), QStringLiteral("&") }, { QStringLiteral("lt"), QStringLiteral("<") },
        { QStringLiteral("gt"), QStringLiteral(">") }, { QStringLiteral("quot"), QStringLiteral("\"") },
        { QStringLiteral("apos"), QStringLiteral("'") }, { QStringLiteral("nbsp"), QStringLiteral(" ") },
        { QStringLiteral("rsquo"), fromCodepoint(0x2019) }, { QStringLiteral("lsquo"), fromCodepoint(0x2018) },
        { QStringLiteral("rdquo"), fromCodepoint(0x201D) }, { QStringLiteral("ldquo"), fromCodepoint(0x201C) },
        { QStringLiteral("hellip"), fromCodepoint(0x2026) }, { QStringLiteral("ndash"), fromCodepoint(0x2013) },
        { QStringLiteral("mdash"), fromCodepoint(0x2014) },
    };
    static const QRegularExpression ent(QStringLiteral(R"RX(&(#x[0-9a-fA-F]+|#\d+|[a-zA-Z]+);)RX"));
    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = ent.globalMatch(s);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += s.mid(last, int(m.capturedStart()) - last);
        const QString e = m.captured(1);
        QString rep = m.captured(0);
        if (e.startsWith(QLatin1String("#x")))   rep = fromCodepoint(e.mid(2).toUInt(nullptr, 16));
        else if (e.startsWith(QLatin1Char('#'))) rep = fromCodepoint(e.mid(1).toUInt());
        else if (named.contains(e))              rep = named.value(e);
        out += rep;
        last = int(m.capturedEnd());
    }
    out += s.mid(last);
    return out;
}

inline QString extractGeniusLyrics(const QString& page, const QString& title)
{
    static const QRegularExpression openRe(
        QStringLiteral(R"RX(<div[^>]*data-lyrics-container="true"[^>]*>)RX"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression excludeRe(
        QStringLiteral(R"RX(<div[^>]*data-exclude-from-selection="true"[^>]*>)RX"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression brRe(QStringLiteral(R"RX(<br\s*/?>)RX"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tagRe(QStringLiteral(R"RX(<[^>]*>)RX"));
    const QChar nl(0xE000);                               // survives the HTML decoder, unlike "\n"

    QString joined;
    int pos = 0;
    for (;;) {
        const QRegularExpressionMatch m = openRe.match(page, pos);
        if (!m.hasMatch()) break;
        const int start = int(m.capturedEnd());
        int contentEnd = 0;
        const int after = divEnd(page, start, &contentEnd);
        QString chunk = page.mid(start, contentEnd - start);

        for (;;) {                                        // drop "Contributors / Translations" header blocks
            const QRegularExpressionMatch ex = excludeRe.match(chunk);
            if (!ex.hasMatch()) break;
            int ce = 0;
            const int e = divEnd(chunk, int(ex.capturedEnd()), &ce);
            chunk.remove(int(ex.capturedStart()), e - int(ex.capturedStart()));
        }
        chunk.replace(brRe, QString(nl));
        chunk.remove(tagRe);
        QString text = decodeHtml(chunk);
        text.replace(nl, QLatin1Char('\n'));
        text.replace(QChar(0x00A0), QLatin1Char(' '));
        joined += text + QStringLiteral("\n\n");
        pos = after;
    }
    if (joined.isEmpty()) return QString();

    const QString tk = lyricKey(title);
    QStringList out;
    const QStringList lines = joined.split(QLatin1Char('\n'));
    static const QRegularExpression embed(QStringLiteral(R"RX(^\d*Embed$)RX"));
    for (const QString& raw : lines) {
        const QString l = raw.trimmed();
        if (embed.match(l).hasMatch() || l == QLatin1String("You might also like")) continue;
        if (l.isEmpty() && (out.isEmpty() || out.last().isEmpty())) continue;   // collapse blank runs
        if (out.isEmpty() && !tk.isEmpty() && l.endsWith(QLatin1String("Lyrics"), Qt::CaseInsensitive)
            && lyricKey(l).contains(tk))
            continue;                                      // "<Song> Lyrics" banner
        out << l;
    }
    while (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
    return out.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
//  Query cleanup
// ---------------------------------------------------------------------------

// Bring Yt:: helpers into scope for lyrics use
using Yt::cleanTitle;

// cleanChannel strips "- Topic" / "VEVO" from a channel name (same logic as Yt::cleanArtist)
inline QString cleanChannel(const QString& c)
{
    return Yt::cleanArtist(c);
}

inline QString lyricsTitle(QString t)
{
    t = cleanTitle(t);
    static const QRegularExpression feat(
        QStringLiteral(R"RX(\s*[\(\[]\s*(?:feat|ft|featuring)\b[^\)\]]*[\)\]])RX"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tail(
        QStringLiteral(R"RX(\s+[-\x{2013}]\s+(?:\d{4}\s+)?(?:remaster(?:ed)?|radio edit|single version|album version|mono|stereo|deluxe|bonus track).*$)RX"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(feat);
    t.remove(tail);
    return t.simplified();
}

inline QString lyricsArtist(QString a)
{
    a = a.trimmed();
    if (a.compare(QLatin1String("Unknown artist"), Qt::CaseInsensitive) == 0) return QString();
    static const QRegularExpression sep(QStringLiteral(R"RX(\s*(?:,|&|\bfeat\b\.?|\bft\b\.?)\s*)RX"),
                                        QRegularExpression::CaseInsensitiveOption);
    const QStringList parts = a.split(sep, Qt::SkipEmptyParts);
    return cleanChannel(parts.isEmpty() ? a : parts.first());
}

inline QVector<LyricLine> linesFromText(const QStringList& in)
{
    QVector<LyricLine> out;
    for (const QString& raw : in) {
        const QString t = raw.trimmed();
        if (t.isEmpty() && (out.isEmpty() || out.last().text.isEmpty())) continue;
        LyricLine l;
        l.text = t;
        l.header = t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'));
        out.push_back(l);
    }
    while (!out.isEmpty() && out.last().text.isEmpty()) out.removeLast();
    return out;
}

// ============================================================================
//  LyricsService: fetches both sources in parallel, merges them, caches the result
// ============================================================================
class LyricsService : public QObject
{
    Q_OBJECT
public:
    explicit LyricsService(QObject* parent = nullptr) : QObject(parent) {}

    static QString cachePath(const QString& key)
    {
        return Yt::cacheDir(QStringLiteral("lyrics")) + QLatin1Char('/')
               + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex())
               + QStringLiteral(".json");
    }

    static void clearCache()
    {
        QDir d(Yt::cacheDir(QStringLiteral("lyrics")));
        const QFileInfoList files = d.entryInfoList(QDir::Files);
        for (const QFileInfo& fi : files) QFile::remove(fi.absoluteFilePath());
    }

    void fetch(const QString& key, const QString& artist, const QString& title,
               const QString& album, qint64 durMs, bool refresh = false)
    {
        if (!refresh) {
            LyricsDoc cached;
            if (loadCache(key, cached)) {
                QTimer::singleShot(0, this, [this, key, cached] { emit ready(key, cached); });
                return;
            }
        }
        if (m_inflight.contains(key)) return;
        m_inflight.insert(key);
        auto job = std::make_shared<Job>();
        job->key = key;
        job->artist = lyricsArtist(artist);
        job->title = lyricsTitle(title);
        job->album = album;
        job->durMs = durMs;
        startGenius(job);
        startLrclib(job, false);
    }

signals:
    void ready(const QString& key, const LyricsDoc& doc);
    void failed(const QString& key, const QString& message);

private:
    struct Job {
        QString key, artist, title, album;
        qint64  durMs = 0;
        bool    gDone = false, lDone = false;
        QStringList gLines;
        QString lSynced, lPlain;
        bool    lInstr = false, lApprox = false;
    };

    static QString enc(const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }

    static QNetworkRequest makeReq(const QUrl& url, bool browser)
    {
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      browser ? QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36")
                              : QStringLiteral("Pulse/1.2 (desktop music player)"));
        req.setRawHeader("Accept-Language", "en-US,en;q=0.9");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(15000);
        return req;
    }

    // ---- Genius (text) -------------------------------------------------------
    void startGenius(const std::shared_ptr<Job>& job)
    {
        const QString q = (job->artist + QLatin1Char(' ') + job->title).trimmed();
        const QString token = QSettings().value(QStringLiteral("lyrics/geniusToken")).toString().trimmed();
        QUrl url;
        if (!token.isEmpty()) url = QUrl(QStringLiteral("https://api.genius.com/search?q=") + enc(q));
        else                  url = QUrl(QStringLiteral("https://genius.com/api/search/song?per_page=8&q=") + enc(q));
        QNetworkRequest req = makeReq(url, true);
        if (!token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        QNetworkReply* rep = m_nam.get(req);
        connect(rep, &QNetworkReply::finished, this, [this, rep, job] {
            rep->deleteLater();
            const QJsonObject root = QJsonDocument::fromJson(rep->readAll()).object();
            const QVector<GeniusHit> hits = parseGeniusSearch(root);
            const int pick = pickGenius(hits, job->artist, job->title);
            if (pick < 0) { job->gDone = true; maybeFinish(job); return; }
            fetchGeniusPage(job, hits[pick].url);
        });
    }

    void fetchGeniusPage(const std::shared_ptr<Job>& job, const QString& url)
    {
        QNetworkRequest req = makeReq(QUrl(url), true);
        req.setRawHeader("Accept", "text/html,application/xhtml+xml");
        QNetworkReply* rep = m_nam.get(req);
        connect(rep, &QNetworkReply::finished, this, [this, rep, job] {
            rep->deleteLater();
            const QString html = QString::fromUtf8(rep->readAll());
            const QString text = extractGeniusLyrics(html, job->title);
            if (!text.isEmpty()) job->gLines = text.split(QLatin1Char('\n'));
            job->gDone = true;
            maybeFinish(job);
        });
    }

    // ---- LRCLIB (timing) -----------------------------------------------------
    void startLrclib(const std::shared_ptr<Job>& job, bool broad)
    {
        QString url = QStringLiteral("https://lrclib.net/api/search?");
        if (!broad && !job->artist.isEmpty())
            url += QStringLiteral("track_name=") + enc(job->title) + QStringLiteral("&artist_name=") + enc(job->artist);
        else
            url += QStringLiteral("q=") + enc((job->artist + QLatin1Char(' ') + job->title).trimmed());
        QNetworkReply* rep = m_nam.get(makeReq(QUrl(url), false));
        connect(rep, &QNetworkReply::finished, this, [this, rep, job, broad] {
            rep->deleteLater();
            const QJsonArray arr = QJsonDocument::fromJson(rep->readAll()).array();
            if (arr.isEmpty() && !broad && !job->artist.isEmpty()) { startLrclib(job, true); return; }
            pickLrclib(job, arr);
            if (!broad && job->lSynced.isEmpty() && job->lPlain.isEmpty() && !job->lInstr) { startLrclib(job, true); return; }
            job->lDone = true;
            maybeFinish(job);
        });
    }

    static void pickLrclib(const std::shared_ptr<Job>& job, const QJsonArray& arr)
    {
        const QString tk = lyricKey(job->title), ak = lyricKey(job->artist);
        int best = -1;
        double bestScore = -1e9;
        qint64 bestDiff = -1;
        for (int i = 0; i < arr.size(); ++i) {
            const QJsonObject o = arr.at(i).toObject();
            const double ts = bigramDice(lyricKey(jstr(o, "trackName")), tk);
            if (ts < 0.55) continue;
            double s = ts * 100.0;
            if (!ak.isEmpty()) {
                const QString hk = lyricKey(jstr(o, "artistName"));
                const double as = (!hk.isEmpty() && (hk.contains(ak) || ak.contains(hk))) ? 1.0 : bigramDice(hk, ak);
                if (as < 0.45) continue;
                s += as * 50.0;
            }
            const bool hasSync = !jstr(o, "syncedLyrics").trimmed().isEmpty();
            const bool hasPlain = !jstr(o, "plainLyrics").trimmed().isEmpty();
            const bool instr = o.value(QLatin1String("instrumental")).toBool();
            if (hasSync) s += 60.0;
            else if (!hasPlain && !instr) continue;
            qint64 diff = -1;
            const double dur = jnum(o, "duration");
            if (job->durMs > 0 && dur > 0) {
                diff = qAbs(qint64(dur * 1000.0) - job->durMs);
                s += diff <= 2500 ? 60.0 : diff <= 6000 ? 40.0 : diff <= 15000 ? 15.0 : -20.0;
            }
            if (s > bestScore) { bestScore = s; best = i; bestDiff = diff; }
        }
        if (best < 0) return;
        const QJsonObject o = arr.at(best).toObject();
        job->lSynced = jstr(o, "syncedLyrics");
        job->lPlain = jstr(o, "plainLyrics");
        job->lInstr = o.value(QLatin1String("instrumental")).toBool();
        job->lApprox = bestDiff > 8000;
    }

    // ---- merge ---------------------------------------------------------------
    void maybeFinish(const std::shared_ptr<Job>& job)
    {
        if (!job->gDone || !job->lDone) return;
        m_inflight.remove(job->key);
        const LyricsDoc doc = combine(*job);
        if (doc.isEmpty()) {
            emit failed(job->key, QStringLiteral("No lyrics found for this song."));
            return;
        }
        saveCache(job->key, doc);
        emit ready(job->key, doc);
    }

    static LyricsDoc combine(const Job& j)
    {
        LyricsDoc d;
        d.approx = j.lApprox;
        const QVector<TimedText> timed = parseLrc(j.lSynced);
        bool haveTimed = false;
        for (const TimedText& t : timed)
            if (!t.text.isEmpty()) { haveTimed = true; break; }

        if (!haveTimed && j.lInstr && j.gLines.isEmpty() && j.lPlain.trimmed().isEmpty()) {
            d.instrumental = true;
            d.timeSource = QStringLiteral("LRCLIB");
            return d;
        }
        if (haveTimed && !j.gLines.isEmpty()) {
            const AlignResult a = alignLyrics(j.gLines, timed);
            if (a.matchedPct >= 40) {                       // same song and wording: Genius text on LRCLIB's clock
                d.lines = a.lines;
                d.synced = true;
                d.textSource = QStringLiteral("Genius");
                d.timeSource = QStringLiteral("LRCLIB");
                d.matchedPct = a.matchedPct;
                return d;
            }
        }
        if (haveTimed) {                                    // Genius missing or too different: use the timed text itself
            for (const TimedText& t : timed) {
                LyricLine l;
                l.text = t.text.isEmpty() ? QString(QChar(0x266A)) : t.text;
                l.timeMs = t.t;
                d.lines.push_back(l);
            }
            d.synced = true;
            d.textSource = d.timeSource = QStringLiteral("LRCLIB");
            return d;
        }
        if (!j.gLines.isEmpty()) {                          // text only
            d.lines = linesFromText(j.gLines);
            d.textSource = QStringLiteral("Genius");
            return d;
        }
        if (!j.lPlain.trimmed().isEmpty()) {
            d.lines = linesFromText(j.lPlain.split(QLatin1Char('\n')));
            d.textSource = QStringLiteral("LRCLIB");
        }
        return d;
    }

    // ---- cache ---------------------------------------------------------------
    static bool loadCache(const QString& key, LyricsDoc& out)
    {
        QFile f(cachePath(key));
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.value(QLatin1String("v")).toInt() != 1) return false;
        const qint64 age = QDateTime::currentSecsSinceEpoch() - qint64(jnum(o, "saved"));
        const bool synced = o.value(QLatin1String("synced")).toBool();
        if (age > (synced ? 90LL * 86400 : 3LL * 86400)) return false;   // un-synced results get retried sooner
        out = LyricsDoc::fromJson(o);
        return !out.isEmpty();
    }

    static void saveCache(const QString& key, const LyricsDoc& d)
    {
        QSaveFile f(cachePath(key));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(d.toJson()).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }

    QNetworkAccessManager m_nam;
    QSet<QString>         m_inflight;
};

