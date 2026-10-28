import { tidyError } from './ui.js';

// Talks to the C++ server: JSON calls, the server-sent event stream, and the track model.

async function call(method, path, body) {
  const opt = { method, headers: { 'X-Pulsify': '1' } };
  if (body !== undefined) { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body); }
  let res;
  try { res = await fetch(path, opt); }
  catch { throw new Error('Can\u2019t reach the Pulsify server. Is it still running?'); }
  let data = null;
  try { data = await res.json(); } catch { /* empty body */ }
  if (!res.ok) {
    const err = new Error(tidyError((data && data.error) || `Request failed (${res.status})`));
    err.status = res.status;
    throw err;
  }
  return data;
}

export const api = {
  get: (p) => call('GET', p),
  post: (p, b = {}) => call('POST', p, b),
  del: (p) => call('DELETE', p),
  q: (p, params) => call('GET', p + '?' + new URLSearchParams(params).toString()),
};

// ---------------------------------------------------------------- tiny event bus
const listeners = new Map();
export const bus = {
  on(name, fn) { (listeners.get(name) || listeners.set(name, new Set()).get(name)).add(fn); return () => listeners.get(name).delete(fn); },
  emit(name, data) { (listeners.get(name) || []).forEach((fn) => { try { fn(data); } catch (e) { console.error(e); } }); },
};

// ---------------------------------------------------------------- server events (EventSource reconnects by itself)
export function connectEvents() {
  const es = new EventSource('/api/events');
  const names = ['hello', 'search', 'searchDone', 'tool', 'library', 'discord', 'google', 'googleError', 'googleProgress', 'notice', 'searchRetry'];
  for (const n of names) {
    es.addEventListener(n, (e) => { try { bus.emit('srv:' + n, JSON.parse(e.data)); } catch { /* ignore */ } });
  }
  es.onopen = () => bus.emit('srv:online', true);
  es.onerror = () => bus.emit('srv:online', false);
  return es;
}

// ---------------------------------------------------------------- track model
//  kind: 'local'   -> file on this computer (path)
//        'yt'      -> YouTube video (videoId)
//        'pending' -> known song (e.g. from the charts) without a YouTube match yet (query)
export const ytThumb = (id) => `https://i.ytimg.com/vi/${id}/mqdefault.jpg`;

export const fromYt = (it) => ({ kind: 'yt', videoId: it.id, title: it.title, artist: it.artist, album: it.album || '', dur: it.dur || 0, thumb: it.thumb || ytThumb(it.id) });
export const fromLocal = (t) => ({ kind: 'local', path: t.path, title: t.title, artist: t.artist, album: t.album || '', dur: t.dur || 0, hasCover: !!t.cover });
export const fromEntry = (e) => e.online
  ? { kind: 'yt', videoId: e.videoId, title: e.title, artist: e.artist, album: e.album || '', dur: e.dur || 0, thumb: ytThumb(e.videoId) }
  : { kind: 'local', path: e.path, title: e.title, artist: e.artist, album: e.album || '', dur: e.dur || 0, hasCover: true };

export function fromDeezer(o, albumFallback = '', coverFallback = '') {
  const title = (o.title_short || o.title || '').trim();
  const artist = ((o.artist && o.artist.name) || '').trim();
  const al = o.album || {};
  return {
    kind: 'pending', title, artist,
    album: al.title || albumFallback,
    thumb: al.cover_big || al.cover_medium || coverFallback,
    dur: Math.round((o.duration || 0) * 1000),
    query: `${artist} ${title}`.trim(),
  };
}

export function toEntry(t) {
  return t.kind === 'local'
    ? { online: false, path: t.path, title: t.title, artist: t.artist, album: t.album || '', dur: t.dur || 0 }
    : { online: true, videoId: t.videoId, title: t.title, artist: t.artist, album: t.album || '', dur: t.dur || 0 };
}

export const trackKey = (t) => t.kind === 'local' ? 'f:' + t.path : t.kind === 'yt' ? 'yt:' + t.videoId : 'q:' + t.query;
export const coverOf = (t) => t.kind === 'local' ? (t.hasCover ? '/api/cover?path=' + encodeURIComponent(t.path) : '') : (t.thumb || '');

// Turns a pending track into a playable YouTube track (cached on the object).
export async function ensurePlayable(t) {
  if (t.kind !== 'pending') return t;
  if (t._matching) return t._matching;
  t._matching = api.q('/api/match', { q: t.query, dur: t.dur || 0 }).then((r) => {
    if (!r.item) throw new Error('No YouTube match for \u201c' + t.query + '\u201d');
    t.kind = 'yt';
    t.videoId = r.item.id;
    if (!t.thumb) t.thumb = r.item.thumb;
    if (!t.dur) t.dur = r.item.dur;
    return t;
  }).finally(() => { t._matching = null; });
  return t._matching;
}
