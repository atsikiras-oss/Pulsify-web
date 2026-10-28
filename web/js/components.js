// Reusable pieces: cover art, track rows/lists (with context menu, drag-reorder), cards and shelves.
import { h, clear, icon, fmtTime, openMenu, toast, confirmBox } from './ui.js';
import { trackKey, coverOf } from './api.js';
import { player, play, enqueue } from './player.js';
import { store, removeFromPlaylist, moveInPlaylist } from './store.js';
import { addToPlaylistModal } from './dialogs.js';

// ---------------------------------------------------------------- cover art
export function cover(src, cls = '') {
  const box = h('div', { class: 'cover ' + cls });
  const fallback = () => { clear(box); box.append(icon('music', 22)); box.classList.add('empty'); };
  if (!src) { fallback(); return box; }
  const img = h('img', { class: 'fade', src, loading: 'lazy', alt: '', referrerpolicy: 'no-referrer', draggable: 'false' });
  img.onerror = fallback;
  img.onload = () => img.classList.add('in');
  if (img.complete && img.naturalWidth) img.classList.add('in');
  box.append(img);
  return box;
}

export const trackCover = (t, cls) => cover(coverOf(t), cls);

// 2x2 mosaic for playlists
export function mosaic(tracks, cls = '') {
  const imgs = [...new Set(tracks.map(coverOf).filter(Boolean))].slice(0, 4);
  if (imgs.length < 4) return cover(imgs[0] || '', cls);
  const box = h('div', { class: 'cover mosaic ' + cls });
  for (const s of imgs) box.append(h('img', { src: s, loading: 'lazy', alt: '', referrerpolicy: 'no-referrer' }));
  return box;
}

// ---------------------------------------------------------------- "now playing" marker on rows
export function markPlaying(root = document) {
  const cur = player.current ? trackKey(player.current) : null;
  for (const row of root.querySelectorAll('.trow, .card[data-key], .tile[data-key]')) {
    const on = cur && row.dataset.key === cur;
    row.classList.toggle('current', !!on);
    row.classList.toggle('is-playing', !!on && player.state === 'playing');
  }
}

// ---------------------------------------------------------------- context menu for one track
export function trackMenu(x, y, track, opts = {}) {
  const items = [
    { label: 'Play', icon: 'play', onClick: () => (opts.onPlay ? opts.onPlay() : play([track], 0)) },
    { label: 'Play next', icon: 'queue', onClick: () => { enqueue(track, true); toast('Will play next'); } },
    { label: 'Add to queue', icon: 'plus', onClick: () => { enqueue(track); toast('Added to queue'); } },
    'sep',
    { label: 'Add to playlist\u2026', icon: 'music', onClick: () => addToPlaylistModal([track]) },
  ];
  if (opts.playlistId != null && opts.row != null)
    items.push({ label: 'Remove from this playlist', icon: 'trash', danger: true, onClick: () => removeFromPlaylist(opts.playlistId, opts.row).catch((e) => toast(e.message, 'error')) });
  openMenu(x, y, items);
}

// ---------------------------------------------------------------- a single row
export function trackRow(track, index, list, opts = {}) {
  const row = h('div', { class: 'trow', dataset: { key: trackKey(track) }, style: { '--i': Math.min(index % 150, 22) } });
  const playHere = () => play(opts.context || list, (opts.context || list).indexOf(track), { playlistId: opts.playlistId });
  const num = h('div', { class: 'tnum', onclick: playHere },
    h('span', { class: 'n', text: String(index + 1) }),
    h('span', { class: 'pl' }, icon('play', 16)),
    h('span', { class: 'eq' }, h('i'), h('i'), h('i')));
  const main = h('div', { class: 'tmain' },
    cover(coverOf(track), 'sm'),
    h('div', { class: 'ttext' },
      h('div', { class: 'ttitle', text: track.title || 'Unknown title' }),
      h('div', { class: 'tartist', text: track.artist || '' })));
  const album = h('div', { class: 'talbum', text: track.album || '' });
  const dur = h('div', { class: 'tdur', text: track.dur ? fmtTime(track.dur) : '' });
  const more = h('button', { class: 'icon-btn tmore', title: 'More', onclick: (e) => { e.stopPropagation(); const r = e.currentTarget.getBoundingClientRect(); trackMenu(r.right - 190, r.bottom + 4, track, { ...opts, onPlay: playHere }); } }, icon('more', 20));
  row.append(opts.reorder ? h('div', { class: 'grip', title: 'Drag to reorder' }, icon('grip', 16)) : null, num, main, album, dur, more);
  row.addEventListener('dblclick', playHere);
  row.addEventListener('contextmenu', (e) => { e.preventDefault(); trackMenu(e.clientX, e.clientY, track, { ...opts, onPlay: playHere }); });
  track._durEl = dur;
  return row;
}

// ---------------------------------------------------------------- a list of rows (rendered in chunks)
// opts: { playlistId, reorder, context, emptyText }
export function trackList(tracks, opts = {}) {
  const wrap = h('div', { class: 'tlist' + (opts.reorder ? ' reorder' : '') });
  wrap.append(h('div', { class: 'thead' + (opts.reorder ? ' has-grip' : '') },
    opts.reorder ? h('div') : null, h('div', { class: 'tnum', text: '#' }), h('div', { text: 'Title' }), h('div', { class: 'talbum', text: 'Album' }), h('div', { class: 'tdur', text: 'Time' }), h('div')));
  const body = h('div', { class: 'tbody' });
  wrap.append(body);
  if (!tracks.length) { wrap.append(h('p', { class: 'empty-note', text: opts.emptyText || 'Nothing here yet.' })); return wrap; }

  let shown = 0;
  const sentinel = h('div', { class: 'sentinel' });
  const renderMore = () => {
    const end = Math.min(tracks.length, shown + 150);
    for (let i = shown; i < end; i++) {
      const row = trackRow(tracks[i], i, tracks, { ...opts, row: i });
      if (opts.reorder) attachDrag(row, i, body, opts);
      body.append(row);
    }
    shown = end;
    markPlaying(body);
    if (shown >= tracks.length) { sentinel.remove(); io.disconnect(); }
  };
  const io = new IntersectionObserver((e) => { if (e.some((x) => x.isIntersecting)) renderMore(); }, { rootMargin: '600px' });
  wrap.append(sentinel);
  renderMore();
  if (shown < tracks.length) io.observe(sentinel);
  return wrap;
}

function attachDrag(row, index, body, opts) {
  row.draggable = true;
  row.addEventListener('dragstart', (e) => { e.dataTransfer.setData('text/plain', String(index)); e.dataTransfer.effectAllowed = 'move'; row.classList.add('dragging'); });
  row.addEventListener('dragend', () => { row.classList.remove('dragging'); body.querySelectorAll('.drop-before,.drop-after').forEach((r) => r.classList.remove('drop-before', 'drop-after')); });
  row.addEventListener('dragover', (e) => {
    e.preventDefault();
    const r = row.getBoundingClientRect();
    const before = e.clientY < r.top + r.height / 2;
    row.classList.toggle('drop-before', before);
    row.classList.toggle('drop-after', !before);
  });
  row.addEventListener('dragleave', () => row.classList.remove('drop-before', 'drop-after'));
  row.addEventListener('drop', async (e) => {
    e.preventDefault();
    const from = parseInt(e.dataTransfer.getData('text/plain'), 10);
    const r = row.getBoundingClientRect();
    let to = e.clientY < r.top + r.height / 2 ? index : index + 1;
    if (from < to) to -= 1;
    row.classList.remove('drop-before', 'drop-after');
    if (!isNaN(from) && from !== to) { try { await moveInPlaylist(opts.playlistId, from, to); } catch (err) { toast(err.message, 'error'); } }
  });
}

// ---------------------------------------------------------------- cards + shelves
// opts: { title, sub, image, round, track, onClick, onPlay }
export function card(o) {
  const el = h('div', { class: 'card' + (o.round ? ' round' : ''), tabindex: 0, dataset: o.track ? { key: trackKey(o.track) } : {} },
    h('div', { class: 'card-art' },
      o.node || cover(o.image, o.round ? 'round' : ''),
      o.onPlay ? h('button', { class: 'card-play', title: 'Play', onclick: (e) => { e.stopPropagation(); o.onPlay(); } }, icon('play', 24)) : null),
    h('div', { class: 'card-title', text: o.title }),
    h('div', { class: 'card-sub', text: o.sub || '' }));
  if (o.onClick) {
    el.addEventListener('click', o.onClick);
    el.addEventListener('keydown', (e) => { if (e.key === 'Enter') o.onClick(); });
  }
  if (o.track) el.addEventListener('contextmenu', (e) => { e.preventDefault(); trackMenu(e.clientX, e.clientY, o.track, { onPlay: o.onPlay }); });
  return el;
}

export function shelf(title, cards, { sub } = {}) {
  const row = h('div', { class: 'shelf-row' }, cards);
  cards.forEach((el, i) => el.style && el.style.setProperty('--i', Math.min(i, 10)));
  const left = h('button', { class: 'icon-btn nav', title: 'Scroll left', onclick: () => row.scrollBy({ left: -row.clientWidth * 0.85, behavior: 'smooth' }) }, icon('back', 22));
  const right = h('button', { class: 'icon-btn nav', title: 'Scroll right', onclick: () => row.scrollBy({ left: row.clientWidth * 0.85, behavior: 'smooth' }) }, icon('forward', 22));
  return h('section', { class: 'shelf' },
    h('div', { class: 'shelf-head' }, h('h2', { text: title }), sub ? h('span', { class: 'muted small', text: sub }) : null, h('span', { class: 'grow' }), left, right),
    row);
}

// Compact "quick access" tile (cover on the left, title, play button on hover) like the desktop app's home grid.
export function tile(o) {
  const el = h('div', { class: 'tile', tabindex: 0, dataset: o.track ? { key: trackKey(o.track) } : {} },
    o.node || cover(o.image, 'tile-art'),
    h('div', { class: 'tile-title', text: o.title }),
    o.onPlay ? h('button', { class: 'tile-play', title: 'Play', onclick: (e) => { e.stopPropagation(); o.onPlay(); } }, icon('play', 22)) : null);
  el.addEventListener('click', o.onClick || o.onPlay);
  el.addEventListener('keydown', (e) => { if (e.key === 'Enter') (o.onClick || o.onPlay)(); });
  if (o.track) el.addEventListener('contextmenu', (e) => { e.preventDefault(); trackMenu(e.clientX, e.clientY, o.track, { onPlay: o.onPlay }); });
  return el;
}

export function skeletonShelf(title) {
  const cards = Array.from({ length: 7 }, () => h('div', { class: 'card skel' }, h('div', { class: 'card-art' }, h('div', { class: 'cover' })), h('div', { class: 'card-title' }), h('div', { class: 'card-sub' })));
  return shelf(title, cards);
}

export function pageHeader({ kicker, title, sub, art, actions }) {
  return h('div', { class: 'page-head' },
    h('div', { class: 'ph-art' }, art),
    h('div', { class: 'ph-text' },
      kicker ? h('div', { class: 'kicker', text: kicker }) : null,
      h('h1', { class: 'ph-title', text: title }),
      sub ? h('div', { class: 'ph-sub', text: sub }) : null,
      actions ? h('div', { class: 'ph-actions' }, actions) : null));
}

export { confirmBox };
