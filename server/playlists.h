#pragma once
#include <QtCore>

// ============================================================================
//  Playlist storage.
//
//  Playlists hold references (a file path or a YouTube video id plus the tags
//  we knew at the time), never the audio itself. Everything is kept in one small
//  JSON file in Pulsify's app-data folder. "Recently played" lives in the same file.
// ============================================================================

struct PlaylistEntry {
    bool    online = false;
    QString path;                 // local file (online == false)
    QString videoId;              // YouTube id (online == true)
    QString title, artist, album;
    qint64  durationMs = 0;

    QString key() const
    {
        return online ? QStringLiteral("yt:") + videoId : QStringLiteral("f:") + path;
    }
};

struct Playlist {
    QString id, name;
    QString source;               // e.g. "yt:PLxxxx" when imported from YouTube (lets a re-import top it up)
    qint64  created = 0;
    QVector<PlaylistEntry> entries;
};

class PlaylistStore
{
public:
    static QString filePath()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + QStringLiteral("/pulse-data.json");
    }

    void load()
    {
        m_lists.clear();
        m_recent.clear();
        QFile f(filePath());
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        for (const QJsonValue& v : root.value(QLatin1String("playlists")).toArray()) {
            const QJsonObject o = v.toObject();
            Playlist p;
            p.id = o.value(QLatin1String("id")).toString();
            p.name = o.value(QLatin1String("name")).toString();
            p.created = qint64(o.value(QLatin1String("created")).toDouble());
            p.source = o.value(QLatin1String("src")).toString();
            if (p.id.isEmpty()) continue;
            if (p.name.isEmpty()) p.name = QStringLiteral("Playlist");
            for (const QJsonValue& e : o.value(QLatin1String("entries")).toArray()) {
                const PlaylistEntry pe = entryFromJson(e.toObject());
                if (!pe.key().endsWith(QLatin1Char(':'))) p.entries.push_back(pe);
            }
            m_lists.push_back(p);
        }
        for (const QJsonValue& e : root.value(QLatin1String("recent")).toArray()) {
            const PlaylistEntry pe = entryFromJson(e.toObject());
            if (!pe.key().endsWith(QLatin1Char(':'))) m_recent.push_back(pe);
        }
    }

    void save() const
    {
        QJsonArray lists;
        for (const Playlist& p : m_lists) {
            QJsonArray es;
            for (const PlaylistEntry& e : p.entries) es.push_back(entryToJson(e));
            QJsonObject o;
            o.insert(QStringLiteral("id"), p.id);
            o.insert(QStringLiteral("name"), p.name);
            o.insert(QStringLiteral("created"), double(p.created));
            if (!p.source.isEmpty()) o.insert(QStringLiteral("src"), p.source);
            o.insert(QStringLiteral("entries"), es);
            lists.push_back(o);
        }
        QJsonArray rec;
        for (const PlaylistEntry& e : m_recent) rec.push_back(entryToJson(e));
        QJsonObject root;
        root.insert(QStringLiteral("playlists"), lists);
        root.insert(QStringLiteral("recent"), rec);

        QDir().mkpath(QFileInfo(filePath()).absolutePath());
        QSaveFile f(filePath());
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }

    // ---- playlists
    const QVector<Playlist>& playlists() const { return m_lists; }

    Playlist* find(const QString& id)
    {
        for (Playlist& p : m_lists) if (p.id == id) return &p;
        return nullptr;
    }
    const Playlist* find(const QString& id) const
    {
        for (const Playlist& p : m_lists) if (p.id == id) return &p;
        return nullptr;
    }

    QString nextDefaultName() const
    {
        QSet<QString> used;
        for (const Playlist& p : m_lists) used.insert(p.name);
        for (int n = int(m_lists.size()) + 1;; ++n) {
            const QString cand = QStringLiteral("My playlist #%1").arg(n);
            if (!used.contains(cand)) return cand;
        }
    }

    QString create(const QString& name)
    {
        Playlist p;
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.name = name.trimmed().isEmpty() ? nextDefaultName() : name.trimmed();
        p.created = QDateTime::currentMSecsSinceEpoch();
        m_lists.prepend(p);                 // newest first, like Spotify
        save();
        return p.id;
    }

    const Playlist* findBySource(const QString& source) const
    {
        if (source.isEmpty()) return nullptr;
        for (const Playlist& p : m_lists) if (p.source == source) return &p;
        return nullptr;
    }

    // Creates a playlist that already has songs (one disk write, duplicates dropped).
    QString createFrom(const QString& name, const QString& source, const QVector<PlaylistEntry>& entries)
    {
        Playlist p;
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.name = name.trimmed().isEmpty() ? nextDefaultName() : name.trimmed();
        p.source = source;
        p.created = QDateTime::currentMSecsSinceEpoch();
        QSet<QString> seen;
        for (const PlaylistEntry& e : entries) {
            if (seen.contains(e.key())) continue;
            seen.insert(e.key());
            p.entries.push_back(e);
        }
        const QString id = p.id;
        m_lists.prepend(p);
        save();
        return id;
    }

    // Appends the songs that are not in the playlist yet; returns how many were new.
    int addMany(const QString& id, const QVector<PlaylistEntry>& entries)
    {
        Playlist* p = find(id);
        if (!p) return 0;
        QSet<QString> have;
        for (const PlaylistEntry& e : p->entries) have.insert(e.key());
        int n = 0;
        for (const PlaylistEntry& e : entries) {
            if (have.contains(e.key())) continue;
            have.insert(e.key());
            p->entries.push_back(e);
            ++n;
        }
        if (n) save();
        return n;
    }

    // Moves one entry (indices into the stored entry list).
    bool moveEntry(const QString& id, int from, int to)
    {
        Playlist* p = find(id);
        if (!p || from == to || from < 0 || to < 0 || from >= p->entries.size() || to >= p->entries.size())
            return false;
        p->entries.move(from, to);
        save();
        return true;
    }

    void rename(const QString& id, const QString& name)
    {
        if (Playlist* p = find(id)) {
            if (!name.trimmed().isEmpty()) p->name = name.trimmed();
            save();
        }
    }

    void remove(const QString& id)
    {
        for (int i = 0; i < m_lists.size(); ++i)
            if (m_lists[i].id == id) { m_lists.removeAt(i); break; }
        save();
    }

    // false = the song was already in the playlist
    bool add(const QString& id, const PlaylistEntry& e)
    {
        Playlist* p = find(id);
        if (!p) return false;
        for (const PlaylistEntry& x : p->entries)
            if (x.key() == e.key()) return false;
        p->entries.push_back(e);
        save();
        return true;
    }

    void removeAt(const QString& id, int row)
    {
        Playlist* p = find(id);
        if (!p || row < 0 || row >= p->entries.size()) return;
        p->entries.removeAt(row);
        save();
    }

    // ---- recently played (newest first)
    const QVector<PlaylistEntry>& recent() const { return m_recent; }

    void pushRecent(const PlaylistEntry& e)
    {
        for (int i = 0; i < m_recent.size(); ++i)
            if (m_recent[i].key() == e.key()) { m_recent.removeAt(i); break; }
        m_recent.prepend(e);
        while (m_recent.size() > 30) m_recent.removeLast();
        save();
    }

private:
    static QJsonObject entryToJson(const PlaylistEntry& e)
    {
        QJsonObject o;
        o.insert(QStringLiteral("o"), e.online);
        if (e.online) o.insert(QStringLiteral("v"), e.videoId);
        else          o.insert(QStringLiteral("p"), e.path);
        o.insert(QStringLiteral("t"), e.title);
        o.insert(QStringLiteral("a"), e.artist);
        o.insert(QStringLiteral("b"), e.album);
        o.insert(QStringLiteral("d"), double(e.durationMs));
        return o;
    }

    static PlaylistEntry entryFromJson(const QJsonObject& o)
    {
        PlaylistEntry e;
        e.online = o.value(QLatin1String("o")).toBool();
        e.videoId = o.value(QLatin1String("v")).toString();
        e.path = o.value(QLatin1String("p")).toString();
        e.title = o.value(QLatin1String("t")).toString();
        e.artist = o.value(QLatin1String("a")).toString();
        e.album = o.value(QLatin1String("b")).toString();
        e.durationMs = qint64(o.value(QLatin1String("d")).toDouble());
        return e;
    }

    QVector<Playlist>      m_lists;
    QVector<PlaylistEntry> m_recent;
};
