#pragma once
// ============================================================================
//  Local library: finds audio files and reads their tags without any media library.
//  ID3v2 (mp3) and FLAC are parsed here (title / artist / album / cover / length);
//  every other format falls back to the file name, and the web UI fills in the
//  duration from the browser's own decoder.
// ============================================================================
#include <QtCore>

struct LocalTrack {
    QString path, title, artist, album;
    qint64  durationMs = 0;
    bool    hasCover = false;

    QJsonObject toJson() const
    {
        return QJsonObject{
            { QStringLiteral("path"), path },        { QStringLiteral("title"), title },
            { QStringLiteral("artist"), artist },    { QStringLiteral("album"), album },
            { QStringLiteral("dur"), double(durationMs) }, { QStringLiteral("cover"), hasCover },
        };
    }
};

namespace Local {

inline const QStringList& audioExts()
{
    static const QStringList e = { QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"),
                                   QStringLiteral("oga"), QStringLiteral("opus"), QStringLiteral("m4a"),
                                   QStringLiteral("aac"), QStringLiteral("wav"), QStringLiteral("webm"),
                                   QStringLiteral("mp4") };
    return e;
}

inline bool isAudio(const QString& path) { return audioExts().contains(QFileInfo(path).suffix().toLower()); }

inline QByteArray mimeFor(const QString& path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    if (s == QLatin1String("mp3"))  return "audio/mpeg";
    if (s == QLatin1String("flac")) return "audio/flac";
    if (s == QLatin1String("ogg") || s == QLatin1String("oga")) return "audio/ogg";
    if (s == QLatin1String("opus")) return "audio/ogg; codecs=opus";
    if (s == QLatin1String("m4a") || s == QLatin1String("mp4")) return "audio/mp4";
    if (s == QLatin1String("aac"))  return "audio/aac";
    if (s == QLatin1String("wav"))  return "audio/wav";
    if (s == QLatin1String("webm")) return "audio/webm";
    return "application/octet-stream";
}

inline QStringList scan(const QString& dir, int limit = 20000)
{
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext() && out.size() < limit) {
        const QString p = it.next();
        if (isAudio(p)) out << QDir::fromNativeSeparators(p);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// ---------------------------------------------------------------- ID3v2
inline quint32 syncsafe(const uchar* p) { return (quint32(p[0]) << 21) | (quint32(p[1]) << 14) | (quint32(p[2]) << 7) | p[3]; }
inline quint32 be32(const uchar* p) { return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3]; }
inline quint32 le32(const uchar* p) { return (quint32(p[3]) << 24) | (quint32(p[2]) << 16) | (quint32(p[1]) << 8) | p[0]; }

inline QString decodeText(const QByteArray& b, int enc)
{
    if (b.isEmpty()) return QString();
    QString s;
    if (enc == 0)      s = QString::fromLatin1(b);
    else if (enc == 3) s = QString::fromUtf8(b);
    else {
        const uchar* p = reinterpret_cast<const uchar*>(b.constData());
        bool le = (enc == 1);
        int off = 0;
        if (b.size() >= 2 && p[0] == 0xFF && p[1] == 0xFE) { le = true;  off = 2; }
        else if (b.size() >= 2 && p[0] == 0xFE && p[1] == 0xFF) { le = false; off = 2; }
        QString r;
        for (int i = off; i + 1 < b.size(); i += 2)
            r += QChar(le ? ushort(p[i] | (p[i + 1] << 8)) : ushort((p[i] << 8) | p[i + 1]));
        s = r;
    }
    const int nul = int(s.indexOf(QChar(0)));
    if (nul >= 0) s.truncate(nul);
    return s.trimmed();
}

struct Id3 { QString title, artist, album; qint64 lenMs = 0; QByteArray cover, coverMime; };

inline Id3 readId3(QFile& f)
{
    Id3 r;
    f.seek(0);
    const QByteArray head = f.read(10);
    if (head.size() < 10 || !head.startsWith("ID3")) return r;
    const uchar* h = reinterpret_cast<const uchar*>(head.constData());
    const int ver = h[3];
    const quint32 size = syncsafe(h + 6);
    if (size == 0 || size > 32u * 1024 * 1024) return r;
    const QByteArray tag = f.read(size);
    const uchar* d = reinterpret_cast<const uchar*>(tag.constData());
    int pos = 0;
    const int n = int(tag.size());
    if (h[5] & 0x40) {                                   // extended header
        if (ver == 4 && n >= 4) pos = int(syncsafe(d));
        else if (n >= 4)        pos = int(be32(d)) + 4;
    }
    const int idLen = (ver == 2) ? 3 : 4;
    const int hdrLen = (ver == 2) ? 6 : 10;
    while (pos + hdrLen <= n) {
        if (d[pos] == 0) break;                          // padding
        const QByteArray id = tag.mid(pos, idLen);
        quint32 fs;
        if (ver == 2)      fs = (quint32(d[pos + 3]) << 16) | (quint32(d[pos + 4]) << 8) | d[pos + 5];
        else if (ver == 4) fs = syncsafe(d + pos + 4);
        else               fs = be32(d + pos + 4);
        const int body = pos + hdrLen;
        if (fs == 0 || body + int(fs) > n) break;
        const QByteArray data = tag.mid(body, int(fs));
        pos = body + int(fs);

        auto text = [&data]() { return decodeText(data.mid(1), uchar(data.at(0))); };
        if (id == "TIT2" || id == "TT2")      r.title = text();
        else if (id == "TPE1" || id == "TP1") r.artist = text();
        else if ((id == "TPE2" || id == "TP2") && r.artist.isEmpty()) r.artist = text();
        else if (id == "TALB" || id == "TAL") r.album = text();
        else if (id == "TLEN" || id == "TLE") r.lenMs = text().toLongLong();
        else if ((id == "APIC" || id == "PIC") && r.cover.isEmpty() && data.size() > 8) {
            const int enc = uchar(data.at(0));
            int p = 1;
            if (id == "PIC") {                           // 3-letter image format
                const QByteArray fmt = data.mid(1, 3).toUpper();
                r.coverMime = (fmt == "PNG") ? "image/png" : "image/jpeg";
                p = 4;
            } else {
                const int e = data.indexOf('\0', 1);
                if (e < 0) continue;
                r.coverMime = data.mid(1, e - 1);
                if (r.coverMime == "image/jpg" || r.coverMime.isEmpty()) r.coverMime = "image/jpeg";
                p = e + 1;
            }
            ++p;                                         // picture type
            if (enc == 1 || enc == 2) {                  // UTF-16 description ends with 00 00
                while (p + 1 < data.size() && !(data.at(p) == 0 && data.at(p + 1) == 0)) p += 2;
                p += 2;
            } else {
                const int e = data.indexOf('\0', p);
                p = e < 0 ? int(data.size()) : e + 1;
            }
            if (p < data.size()) r.cover = data.mid(p);
        }
    }
    return r;
}

// ---------------------------------------------------------------- FLAC
inline Id3 readFlac(QFile& f)
{
    Id3 r;
    f.seek(0);
    if (f.read(4) != "fLaC") return r;
    for (int guard = 0; guard < 64; ++guard) {
        const QByteArray bh = f.read(4);
        if (bh.size() < 4) break;
        const uchar* h = reinterpret_cast<const uchar*>(bh.constData());
        const bool last = h[0] & 0x80;
        const int type = h[0] & 0x7F;
        const quint32 len = (quint32(h[1]) << 16) | (quint32(h[2]) << 8) | h[3];
        if (len > 32u * 1024 * 1024) break;
        const QByteArray blk = f.read(len);
        const uchar* b = reinterpret_cast<const uchar*>(blk.constData());
        if (type == 0 && blk.size() >= 18) {
            const quint32 rate = (quint32(b[10]) << 12) | (quint32(b[11]) << 4) | (b[12] >> 4);
            const quint64 samples = (quint64(b[13] & 0x0F) << 32) | be32(b + 14);
            if (rate > 0) r.lenMs = qint64(samples * 1000 / rate);
        } else if (type == 4 && blk.size() >= 8) {
            quint32 p = 4 + le32(b);
            if (p + 4 <= quint32(blk.size())) {
                const quint32 count = le32(b + p);
                p += 4;
                for (quint32 i = 0; i < count && p + 4 <= quint32(blk.size()); ++i) {
                    const quint32 l = le32(b + p);
                    p += 4;
                    if (p + l > quint32(blk.size())) break;
                    const QString kv = QString::fromUtf8(blk.mid(int(p), int(l)));
                    p += l;
                    const int eq = int(kv.indexOf(QLatin1Char('=')));
                    if (eq <= 0) continue;
                    const QString k = kv.left(eq).toUpper(), v = kv.mid(eq + 1).trimmed();
                    if (k == QLatin1String("TITLE")) r.title = v;
                    else if (k == QLatin1String("ARTIST") && r.artist.isEmpty()) r.artist = v;
                    else if (k == QLatin1String("ALBUMARTIST") && r.artist.isEmpty()) r.artist = v;
                    else if (k == QLatin1String("ALBUM")) r.album = v;
                }
            }
        } else if (type == 6 && blk.size() > 32 && r.cover.isEmpty()) {
            quint32 p = 4;
            const quint32 ml = be32(b + p); p += 4;
            if (p + ml + 4 <= quint32(blk.size())) {
                r.coverMime = blk.mid(int(p), int(ml)); p += ml;
                const quint32 dl = be32(b + p); p += 4 + dl + 16;
                if (p + 4 <= quint32(blk.size())) {
                    const quint32 pl = be32(b + p); p += 4;
                    if (p + pl <= quint32(blk.size())) r.cover = blk.mid(int(p), int(pl));
                }
            }
        }
        if (last) break;
    }
    return r;
}

inline Id3 readTags(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("mp3")) return readId3(f);
    if (ext == QLatin1String("flac")) return readFlac(f);
    return {};
}

inline LocalTrack trackFor(const QString& path)
{
    LocalTrack t;
    t.path = path;
    const Id3 m = readTags(path);
    t.title = m.title; t.artist = m.artist; t.album = m.album;
    t.durationMs = m.lenMs;
    t.hasCover = !m.cover.isEmpty();
    if (t.title.isEmpty()) {
        const QString base = QFileInfo(path).completeBaseName();
        static const QRegularExpression sep(QStringLiteral(R"(^(.{1,60}?)\s+[-\x{2013}]\s+(.+)$)"));
        const auto mm = sep.match(base);
        if (mm.hasMatch() && t.artist.isEmpty()) { t.artist = mm.captured(1).trimmed(); t.title = mm.captured(2).trimmed(); }
        else t.title = base;
    }
    if (t.artist.isEmpty()) t.artist = QStringLiteral("Unknown artist");
    return t;
}

} // namespace Local
