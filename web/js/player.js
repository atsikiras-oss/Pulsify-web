// The player: one <audio> element, a queue (context + "up next"), shuffle/repeat,
// stream loading through the server, Media Session (media keys) and Discord presence.
import { api, bus, ensurePlayable, toEntry, trackKey, coverOf, ytThumb } from './api.js';
import { toast } from './ui.js';
import { getPrefs, savePrefs } from './theme.js';

const audio = new Audio();
audio.preload = 'auto';

const UNPLAYABLE = /isn.t available|blocked in your country|No YouTube match|private|removed|unavailable|No playable audio/i;

export const player = {
  audio,
  current: null,          // the track being played
  ctx: [],                // the list it came from
  ctxPlaylist: null,      // id of the playlist that list is, if any
  order: [],              // indices into ctx, in play order (shuffled or not)
  pos: -1,                // position inside `order`
  upNext: [],             // "play next" / "add to queue" tracks, played before the context continues
  history: [],            // [{track, pos}] for the Previous button
  shuffle: false,
  repeat: 0,              // 0 off, 1 all, 2 one
  state: 'idle',          // idle | loading | playing | paused
  stream: null,           // yt-dlp info about the current stream
  error: '',
  volume: 0.8,
  muted: false,

  position() { return audio.currentTime * 1000; },
  duration() {
    const d = audio.duration;
    if (isFinite(d) && d > 0) return d * 1000;
    return (this.current && this.current.dur) || 0;
  },
};

let loadTok = 0;
let skipStreak = 0;

const emit = (n, d) => bus.emit('player:' + n, d);
function setState(s) { if (player.state !== s) { player.state = s; emit('state', s); syncSoon(); updateSession(); } }

// ---------------------------------------------------------------- queue building
function shuffled(n, first) {
  const a = Array.from({ length: n }, (_, i) => i).filter((i) => i !== first);
  for (let i = a.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [a[i], a[j]] = [a[j], a[i]]; }
  return first >= 0 ? [first, ...a] : a;
}

function buildOrder(startIdx) {
  const n = player.ctx.length;
  if (player.shuffle) { player.order = shuffled(n, startIdx); player.pos = 0; }
  else { player.order = Array.from({ length: n }, (_, i) => i); player.pos = Math.max(0, startIdx); }
}

// Plays `tracks[index]` and keeps the rest of the list as the context. With shuffle on and no
// index given, a random song starts and everything plays once before anything repeats.
export function play(tracks, index = -1, meta = {}) {
  if (!tracks.length) return;
  player.ctx = tracks.slice();
  player.ctxPlaylist = meta.playlistId || null;
  const start = index >= 0 ? index : (player.shuffle ? Math.floor(Math.random() * tracks.length) : 0);
  buildOrder(start);
  player.history = [];
  skipStreak = 0;
  emit('queue');
  start_(player.ctx[player.order[player.pos]], { push: false });
}

export function playOne(track) { play([track], 0); }

export function enqueue(track, front = false) {
  if (front) player.upNext.unshift(track); else player.upNext.push(track);
  if (!player.current) { next(); return; }
  emit('queue');
  prefetchNext();
}

export function removeUpcoming(i) {
  if (i < player.upNext.length) player.upNext.splice(i, 1);
  else {
    const at = player.pos + 1 + (i - player.upNext.length);
    if (at < player.order.length) player.order.splice(at, 1);
  }
  emit('queue');
}

export function clearUpNext() { player.upNext = []; emit('queue'); }

// What comes next, for the Queue panel.
export function upcoming(limit = 100) {
  const out = player.upNext.map((t) => ({ track: t, queued: true }));
  for (let i = player.pos + 1; i < player.order.length && out.length < limit; i++)
    out.push({ track: player.ctx[player.order[i]], queued: false });
  return out;
}

function peekNext() {
  if (player.upNext.length) return player.upNext[0];
  if (player.pos + 1 < player.order.length) return player.ctx[player.order[player.pos + 1]];
  if (player.repeat === 1 && player.order.length && !player.shuffle) return player.ctx[player.order[0]];
  return null;
}

// ---------------------------------------------------------------- transport
export function next(auto = false) {
  if (auto && player.repeat === 2 && player.current) { audio.currentTime = 0; audio.play().catch(() => {}); return; }
  const cur = player.current;
  if (player.upNext.length) {
    const t = player.upNext.shift();
    if (cur) player.history.push({ track: cur, pos: player.pos });
    emit('queue');
    start_(t, { push: false });
    return;
  }
  if (player.pos + 1 < player.order.length) {
    if (cur) player.history.push({ track: cur, pos: player.pos });
    player.pos += 1;
    emit('queue');
    start_(player.ctx[player.order[player.pos]], { push: false });
    return;
  }
  if (player.repeat === 1 && player.order.length) {          // wrap around (fresh shuffle if shuffling)
    if (cur) player.history.push({ track: cur, pos: player.pos });
    if (player.shuffle) player.order = shuffled(player.ctx.length, -1);
    player.pos = 0;
    emit('queue');
    start_(player.ctx[player.order[0]], { push: false });
    return;
  }
  // end of the list
  audio.pause();
  setState('paused');
  audio.currentTime = 0;
  emit('time');
}

export function prev() {
  if (audio.currentTime > 3 || !player.history.length) { audio.currentTime = 0; emit('time'); if (player.state === 'paused') audio.play().catch(() => {}); return; }
  const item = player.history.pop();
  player.pos = item.pos;
  emit('queue');
  start_(item.track, { push: false });
}

export function toggle() {
  if (!player.current) return;
  if (audio.paused) audio.play().catch((e) => toast(e.message, 'error')); else audio.pause();
}

export function seek(ms) {
  if (!player.current) return;
  audio.currentTime = Math.max(0, ms / 1000);
  emit('time');
  syncSoon();
}

export function setVolume(v, save = true) {
  player.volume = Math.min(1, Math.max(0, v));
  audio.volume = player.volume;
  if (player.volume > 0) { player.muted = false; audio.muted = false; }
  emit('volume');
  if (save) savePrefs({ volume: player.volume });
}

export function toggleMute() {
  player.muted = !player.muted;
  audio.muted = player.muted;
  emit('volume');
}

export function setShuffle(on) {
  if (on === player.shuffle) return;
  player.shuffle = on;
  if (player.ctx.length && player.current) {
    const curIdx = player.ctx.indexOf(player.current);
    if (on) { player.order = shuffled(player.ctx.length, curIdx); player.pos = 0; }
    else { player.order = Array.from({ length: player.ctx.length }, (_, i) => i); player.pos = Math.max(0, curIdx); }
  }
  savePrefs({ shuffle: on });
  emit('mode');
  emit('queue');
  prefetchNext();
}

export function cycleRepeat() {
  player.repeat = (player.repeat + 1) % 3;
  savePrefs({ repeat: player.repeat });
  emit('mode');
}

// ---------------------------------------------------------------- loading a track
async function start_(track, { push = true } = {}) {
  if (!track) return;
  const tok = ++loadTok;
  player.current = track;
  player.stream = null;
  player.error = '';
  audio.pause();
  setState('loading');
  emit('track', track);
  updateSession();

  try {
    if (track.kind === 'local') {
      audio.src = '/api/file?path=' + encodeURIComponent(track.path);
    } else {
      if (track.kind === 'pending') {
        await ensurePlayable(track);
        if (tok !== loadTok) return;
        emit('track', track);
      }
      const r = await api.post('/api/play', { videoId: track.videoId });
      if (tok !== loadTok) return;
      audio.src = r.url;
      player.stream = r.stream || null;
      const it = r.item || {};
      if (!track.dur && it.dur) track.dur = it.dur;
      if (!track.album && it.album) track.album = it.album;
      if (!track.artist || track.artist === 'YouTube') track.artist = it.artist || track.artist;
      emit('track', track);
    }
    await audio.play();
    if (tok !== loadTok) return;
    skipStreak = 0;
  } catch (e) {
    if (tok !== loadTok) return;
    if (e.status === 409 || e.name === 'AbortError') return;              // superseded by a newer request
    handleFailure(track, e.message || 'Can\u2019t play this song');
  }
}

function handleFailure(track, msg) {
  if (track.kind !== 'local' && !track._retried && !UNPLAYABLE.test(msg)) {   // expired link etc.: resolve once more
    track._retried = true;
    start_(track, { push: false });
    return;
  }
  player.error = msg;
  setState('paused');
  emit('error', msg);
  if (UNPLAYABLE.test(msg) && skipStreak < 5 && peekNext()) {
    skipStreak++;
    toast(`Skipped \u201c${track.title}\u201d \u2013 ${msg}`, 'error');
    next();
  } else {
    toast(msg, 'error');
  }
}

// ---------------------------------------------------------------- audio element events
audio.addEventListener('playing', () => { if (player.current) player.current._retried = false; setState('playing'); noteRecent(); prefetchNext(); });
audio.addEventListener('pause', () => { if (player.current && !audio.ended && player.state !== 'loading') setState('paused'); });
audio.addEventListener('waiting', () => { if (player.state === 'playing') emit('buffering', true); });
audio.addEventListener('canplay', () => emit('buffering', false));
audio.addEventListener('timeupdate', () => emit('time'));
audio.addEventListener('seeked', () => syncSoon());
audio.addEventListener('ended', () => next(true));
audio.addEventListener('durationchange', () => {
  const t = player.current;
  if (t && isFinite(audio.duration) && audio.duration > 0 && (!t.dur || Math.abs(t.dur - audio.duration * 1000) > 1500) && t.kind === 'local') t.dur = Math.round(audio.duration * 1000);
  emit('time');
});
audio.addEventListener('error', () => {
  const t = player.current;
  if (!t || !audio.src) return;
  if (t.kind !== 'local' && !t._retried) { t._retried = true; start_(t, { push: false }); return; }
  handleFailure(t, t.kind === 'local' ? 'Can\u2019t play this file' : 'The stream stopped working');
});

// ---------------------------------------------------------------- recents, prefetch
let recentFor = null;
function noteRecent() {
  const t = player.current;
  if (!t || recentFor === t || t.kind === 'pending') return;
  recentFor = t;
  api.post('/api/recent', { entry: toEntry(t) }).then(() => bus.emit('recent:changed')).catch(() => {});
}

async function prefetchNext() {
  const n = peekNext();
  if (!n || n.kind === 'local') return;
  try {
    await ensurePlayable(n);
    if (n === peekNext()) await api.post('/api/prefetch', { videoId: n.videoId });
  } catch { /* the song will be resolved when it is played */ }
}

// ---------------------------------------------------------------- media keys / lock screen
function updateSession() {
  if (!('mediaSession' in navigator)) return;
  const t = player.current;
  if (!t) { navigator.mediaSession.metadata = null; return; }
  const cover = coverOf(t);
  const src = cover ? new URL(cover, location.href).href : '';
  navigator.mediaSession.metadata = new MediaMetadata({
    title: t.title || '', artist: t.artist || '', album: t.album || '',
    artwork: src ? [{ src, sizes: '320x320' }] : [],
  });
  navigator.mediaSession.playbackState = player.state === 'playing' ? 'playing' : player.state === 'paused' ? 'paused' : 'none';
}

if ('mediaSession' in navigator) {
  const ms = navigator.mediaSession;
  const set = (a, f) => { try { ms.setActionHandler(a, f); } catch { /* unsupported action */ } };
  set('play', () => toggle());
  set('pause', () => toggle());
  set('previoustrack', () => prev());
  set('nexttrack', () => next());
  set('seekto', (d) => seek((d.seekTime || 0) * 1000));
}

// ---------------------------------------------------------------- Discord Rich Presence
let syncTimer = null;
function syncSoon() { clearTimeout(syncTimer); syncTimer = setTimeout(sendPresence, 250); }

function sendPresence() {
  const t = player.current;
  let body = { valid: false };
  if (t && player.state !== 'idle') {
    const fb = t.kind === 'local' ? '' : (t.videoId ? ytThumb(t.videoId) : (/^https:/.test(t.thumb || '') ? t.thumb : ''));
    body = {
      valid: true, playing: player.state === 'playing', paused: player.state === 'paused',
      title: t.title, artist: t.artist, album: t.album || '',
      durationMs: Math.round(player.duration()), positionMs: Math.round(player.position()),
      fallbackImage: fb,
    };
  }
  api.post('/api/discord/presence', body).catch(() => {});
}
setInterval(sendPresence, 5000);

// ---------------------------------------------------------------- boot
export function initPlayer() {
  const p = getPrefs();
  player.shuffle = !!p.shuffle;
  player.repeat = p.repeat | 0;
  setVolume(typeof p.volume === 'number' ? p.volume : 0.8, false);
  window.addEventListener('beforeunload', () => {
    try { navigator.sendBeacon && fetch('/api/discord/presence', { method: 'POST', keepalive: true, headers: { 'X-Pulsify': '1', 'Content-Type': 'application/json' }, body: '{"valid":false}' }); } catch { /* ignore */ }
  });
}

export { trackKey };
