// App shell: boot, router, sidebar, top bar, player bar, right panel and keyboard shortcuts.
import { $, h, clear, icon, fmtTime, plural, toast, debounce, promptBox } from './ui.js';
import { api, bus, connectEvents, fromEntry, coverOf, trackKey } from './api.js';
import { loadPrefs, setPageTint, tintFromImage, getTheme, resolveTheme } from './theme.js';
import { store, refreshPlaylists, refreshLibrary, refreshGoogle, createPlaylist } from './store.js';
import { player, initPlayer, toggle, next, prev, seek, setVolume, toggleMute, setShuffle, cycleRepeat } from './player.js';
import { homeView, searchView, libraryView, playlistView, collectionView, lyricsView, panelView } from './views.js';
import { settingsModal, importModal, folderPickerModal, addToPlaylistModal } from './dialogs.js';
import { markPlaying, mosaic, trackCover } from './components.js';

let dispose = null;
let panelOpen = false;
let panelTab = 'now';

// ================================================================== router
function parseHash() {
  const raw = location.hash.replace(/^#/, '') || '/home';
  const [path, qs] = raw.split('?');
  return { parts: path.split('/').filter(Boolean).map(decodeURIComponent), query: new URLSearchParams(qs || ''), raw };
}

function route() {
  const { parts, query } = parseHash();
  const view = $('#view');
  if (dispose) { try { dispose(); } catch (e) { console.error(e); } dispose = null; }
  clear(view);
  view.scrollTo(0, 0);
  document.body.dataset.page = parts[0] || 'home';
  const page = h('div', { class: 'page' });
  view.append(page);
  const name = parts[0] || 'home';

  const searchBox = $('#search');
  if (name !== 'search' && searchBox && document.activeElement !== searchBox) searchBox.value = '';

  switch (name) {
    case 'search': {
      const q = decodeURIComponent(parts.slice(1).join('/'));
      if (searchBox && document.activeElement !== searchBox) searchBox.value = q;
      dispose = searchView(page, q); break;
    }
    case 'library': dispose = libraryView(page); break;
    case 'playlist': dispose = playlistView(page, parts[1]); break;
    case 'artist': case 'album': dispose = collectionView(page, name, parts[1], query); break;
    case 'lyrics': history.replaceState(null, '', '#/home'); dispose = homeView(page); openSheet('lyrics'); break;
    default: dispose = homeView(page);
  }
  drawSidebarActive();
  updateTint();
}

// ================================================================== sidebar
function newPlaylist() {
  return promptBox('New playlist', 'Playlist name', '', 'Create').then(async (name) => {
    if (name === null) return;
    try { const r = await createPlaylist(name); location.hash = '#/playlist/' + r.id; } catch (e) { toast(e.message, 'error'); }
  });
}

function drawSidebar() {
  const nav = $('#nav');
  clear(nav);
  const item = (page, ico, label, href, click) => h(click ? 'button' : 'a', { class: 'nav-item', href, dataset: { page }, onclick: click }, icon(ico, 24), h('span', { text: label }));
  nav.append(item('home', 'home', 'Home', '#/home'), item('library', 'library', 'Your Library', '#/library'),
    item('add', 'folder', 'Add folder', null, () => folderPickerModal()));

  const pl = $('#playlists');
  clear(pl);
  store.playlists.forEach((p, i) => {
    const tracks = p.entries.slice(0, 4).map(fromEntry);
    pl.append(h('a', { class: 'pl-item', href: '#/playlist/' + p.id, dataset: { id: p.id }, style: { '--i': Math.min(i, 12) } },
      mosaic(tracks, 'sm'),
      h('div', { class: 'ttext' }, h('div', { class: 'ttitle', text: p.name }), h('div', { class: 'tartist', text: 'Playlist \u00b7 ' + plural(p.entries.length, 'song') })),
      h('span', { class: 'eq mini-eq' }, h('i'), h('i'), h('i'))));
  });
  $('#pl-empty').hidden = store.playlists.length > 0;
  drawSidebarActive();
  markSidebarPlaying();
}

function markSidebarPlaying() {
  const cur = player.current;
  const playing = player.state === 'playing';
  const owner = cur ? store.playlists.find((p) => p.entries.some((e) => (e.online ? 'yt:' + e.videoId : 'f:' + e.path) === trackKey(cur))) : null;
  document.querySelectorAll('.pl-item').forEach((a) => {
    const on = !!owner && a.dataset.id === owner.id && player.ctxPlaylist === owner.id;
    a.classList.toggle('now', on);
    a.classList.toggle('now-playing', on && playing);
  });
}

function drawSidebarActive() {
  const { parts } = parseHash();
  const name = parts[0] || 'home';
  document.querySelectorAll('.nav-item').forEach((a) => a.classList.toggle('active', a.dataset.page === name));
  document.querySelectorAll('.pl-item').forEach((a) => a.classList.toggle('active', name === 'playlist' && a.dataset.id === parts[1]));
}

// ================================================================== top bar
function put(sel, name, size = 20) { const el = $(sel); clear(el); el.append(icon(name, size)); }

function setupTopbar() {
  put('#btn-back', 'back', 22); put('#btn-fwd', 'forward', 22); put('#btn-settings', 'settings', 20);
  const search = $('#search');
  const go = debounce(() => {
    const q = search.value.trim();
    const target = '#/search/' + encodeURIComponent(q);
    if (q) { if (!location.hash.startsWith('#/search')) location.hash = target; else { history.replaceState(null, '', target); route(); } }
    else if (location.hash.startsWith('#/search')) location.hash = '#/home';
  }, 380);
  search.addEventListener('input', go);
  search.addEventListener('keydown', (e) => { if (e.key === 'Enter') { const q = search.value.trim(); if (q) location.hash = '#/search/' + encodeURIComponent(q); } if (e.key === 'Escape') search.blur(); });
  $('#btn-back').onclick = () => history.back();
  $('#btn-fwd').onclick = () => history.forward();
  $('#btn-settings').onclick = () => settingsModal();
  $('#btn-new-pl').onclick = newPlaylist;
  $('#ec-create').onclick = newPlaylist;
  $('#btn-import').onclick = () => importModal();
  $('#ec-import').onclick = () => importModal('link');
  $('#btn-panel').onclick = () => togglePanel();
  drawAvatar();
}

function drawAvatar() {
  const b = $('#btn-account');
  clear(b);
  const g = store.google;
  if (g.signedIn && g.avatar) b.append(h('img', { src: g.avatar, referrerpolicy: 'no-referrer', alt: '' }));
  else if (g.signedIn && g.name) b.append(h('span', { class: 'initial', text: g.name[0].toUpperCase() }));
  else b.append(icon('user', 20));
  b.title = g.signedIn ? g.name || 'Google account' : 'Google account';
  b.onclick = () => (g.hasCredentials && g.signedIn ? settingsModal('integrations') : importModal('google'));
}

// ================================================================== right panel
function togglePanel(force) {
  panelOpen = force === undefined ? !panelOpen : force;
  $('#app').classList.toggle('panel-open', panelOpen);
  $('#btn-panel').classList.toggle('on', panelOpen);
  $('#pb-queue').classList.toggle('on', panelOpen && panelTab === 'queue');
  if (panelOpen) drawPanel();
}
function drawPanel() { if (panelOpen) panelView($('#rightpanel .rp-inner'), panelTab); $('#pb-queue').classList.toggle('on', panelOpen && panelTab === 'queue'); }

// ================================================================== player bar
const bar = {};

function setupPlayerBar() {
  const q = (s) => $(s);
  Object.assign(bar, {
    art: q('#pb-art'), title: q('#pb-title'), artist: q('#pb-artist'),
    play: q('#pb-play'), prev: q('#pb-prev'), next: q('#pb-next'), shuf: q('#pb-shuffle'), rep: q('#pb-repeat'),
    seek: q('#pb-seek'), cur: q('#pb-cur'), dur: q('#pb-dur'), vol: q('#pb-vol'), mute: q('#pb-mute'),
    lyr: q('#pb-lyrics'), queue: q('#pb-queue'),
  });
  put('#pb-prev', 'prev', 24); put('#pb-next', 'next', 24); put('#pb-add', 'plus', 22); put('#pb-lyrics', 'mic', 20);
  put('#btn-panel', 'info', 20); put('#pb-queue', 'queue', 20); put('#sheet-close', 'down', 24);
  bar.play.onclick = toggle;
  bar.prev.onclick = prev;
  bar.next.onclick = () => next(false);
  bar.shuf.onclick = () => setShuffle(!player.shuffle);
  bar.rep.onclick = cycleRepeat;
  bar.mute.onclick = toggleMute;
  bar.lyr.onclick = () => { if (sheetOpen && sheetMode === 'lyrics') closeSheet(); else openSheet('lyrics'); };
  $('#pb-add').onclick = () => player.current && addToPlaylistModal([player.current]);
  $('#pb-art').onclick = () => { if (sheetOpen) closeSheet(); else openSheet('cover'); };
  $('#sheet-close').onclick = closeSheet;
  $('#sheet-tab-cover').onclick = () => setSheetMode('cover');
  $('#sheet-tab-lyrics').onclick = () => setSheetMode('lyrics');
  bar.queue.onclick = () => { if (panelOpen && panelTab === 'queue') togglePanel(false); else { panelTab = 'queue'; togglePanel(true); drawPanel(); } };

  // seek slider: drag with the pointer, preview while dragging
  let dragging = false;
  bar.seek.addEventListener('input', () => { dragging = true; bar.cur.textContent = fmtTime(bar.seek.value / 1000 * player.duration()); paintRange(bar.seek); });
  bar.seek.addEventListener('change', () => { dragging = false; seek(bar.seek.value / 1000 * player.duration()); });
  bar.vol.addEventListener('input', () => { setVolume(bar.vol.value / 100); paintRange(bar.vol); });
  bar.dragging = () => dragging;

  const loop = () => {
    if (!bar.dragging()) {
      const d = player.duration(), p = player.position();
      bar.seek.value = d > 0 ? Math.min(1000, (p / d) * 1000) : 0;
      bar.cur.textContent = fmtTime(p);
      bar.dur.textContent = d > 0 ? fmtTime(d) : '0:00';
      paintRange(bar.seek);
    }
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
  drawBarState(); drawBarTrack(); drawBarVolume();
}

function paintRange(el) {
  const pct = ((el.value - el.min) / (el.max - el.min)) * 100;
  el.style.setProperty('--pct', pct + '%');
}

function drawBarTrack() {
  const t = player.current;
  clear(bar.art);
  if (t) bar.art.append(trackCover(t, 'md'));
  bar.title.textContent = t ? t.title : 'Nothing playing';
  bar.artist.textContent = t ? (player.error || t.artist || '') : 'Pick something to play';
  bar.title.onclick = () => { if (t) openSheet('cover'); };
  $('#pb-add').style.visibility = t ? 'visible' : 'hidden';
  drawSheet();
  document.title = t ? `${t.title} \u00b7 ${t.artist || 'Pulsify'}` : 'Pulsify';
  drawPanel();
  updateTint();
}

function drawBarState() {
  const s = player.state;
  clear(bar.play);
  bar.play.append(s === 'playing' ? icon('pause', 26) : icon('play', 26));
  bar.play.classList.toggle('loading', s === 'loading');
  $('#playerbar').classList.toggle('is-playing', s === 'playing');
  $('#pb-art').classList.toggle('spin-glow', s === 'playing');
  bar.play.title = s === 'playing' ? 'Pause' : 'Play';
  bar.shuf.classList.toggle('on', player.shuffle);
  bar.rep.classList.toggle('on', player.repeat > 0);
  clear(bar.rep); bar.rep.append(icon(player.repeat === 2 ? 'repeat1' : 'repeat', 20));
  bar.rep.title = ['Repeat off', 'Repeat all', 'Repeat one'][player.repeat];
  clear(bar.shuf); bar.shuf.append(icon('shuffle', 20));
  markPlaying();
  markSidebarPlaying();
}

function drawBarVolume() {
  clear(bar.mute);
  bar.mute.append(icon(player.muted || player.volume === 0 ? 'mute' : 'volume', 20));
  bar.vol.value = player.muted ? 0 : Math.round(player.volume * 100);
  paintRange(bar.vol);
}

async function updateTint() {
  const t = player.current;
  if (!resolveTheme(getTheme()).tint || !t || !location.hash.match(/^#\/(home|library|playlist|lyrics)?/)) { setPageTint(null); return; }
  const hex = await tintFromImage(coverOf(t));
  setPageTint(hex);
}

// ================================================================== now-playing sheet
let sheetOpen = false, sheetMode = 'cover', lyricsDispose = null, sheetBgKey = null;

function openSheet(mode = 'cover') {
  if (!player.current) { toast('Play something first'); return; }
  sheetOpen = true;
  const sheet = $('#sheet');
  sheet.classList.add('open');
  sheet.setAttribute('aria-hidden', 'false');
  setSheetMode(mode);
  drawSheet();
}

function closeSheet() {
  sheetOpen = false;
  const sheet = $('#sheet');
  sheet.classList.remove('open');
  sheet.setAttribute('aria-hidden', 'true');
  $('#pb-lyrics').classList.remove('on');
  setTimeout(() => { if (!sheetOpen) unmountLyrics(); }, 520);
}

function setSheetMode(mode) {
  sheetMode = mode;
  $('#sheet').classList.toggle('lyrics-mode', mode === 'lyrics');
  $('#sheet-tab-cover').classList.toggle('active', mode === 'cover');
  $('#sheet-tab-lyrics').classList.toggle('active', mode === 'lyrics');
  $('#pb-lyrics').classList.toggle('on', sheetOpen && mode === 'lyrics');
  if (mode === 'lyrics') mountLyrics(); else unmountLyrics();
}

function mountLyrics() {
  if (lyricsDispose) return;
  const right = $('#sheet .sheet-right');
  clear(right);
  lyricsDispose = lyricsView(right, { title: false });
}

function unmountLyrics() {
  if (lyricsDispose) { lyricsDispose(); lyricsDispose = null; }
  clear($('#sheet .sheet-right'));
}

function drawSheet() {
  const t = player.current;
  if (!t) return;
  const art = $('#sheet .sheet-art');
  clear(art);
  art.append(trackCover(t, 'xl'));
  $('#sheet .sheet-title').textContent = t.title || '';
  $('#sheet .sheet-artist').textContent = t.artist || '';
  const key = coverOf(t) || '-';
  if (key !== sheetBgKey) {                                    // cross-fade the blurred cover backdrop
    sheetBgKey = key;
    const bgs = $('#sheet .sheet-bgs');
    const layer = h('div', { class: 'sheet-bg', style: { backgroundImage: key !== '-' ? `url("${key}")` : 'none' } });
    bgs.append(layer);
    requestAnimationFrame(() => requestAnimationFrame(() => layer.classList.add('in')));
    setTimeout(() => { while (bgs.children.length > 2) bgs.firstChild.remove(); }, 1000);
  }
}

// ================================================================== keyboard
function setupKeys() {
  document.addEventListener('keydown', (e) => {
    const tag = (e.target.tagName || '').toLowerCase();
    const typing = tag === 'input' || tag === 'textarea' || tag === 'select' || e.target.isContentEditable;
    if (e.key === '/' && !typing) { e.preventDefault(); $('#search').focus(); return; }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'i') { e.preventDefault(); e.shiftKey ? togglePanel() : importModal(); return; }
    if (e.key === 'Escape' && sheetOpen && !document.querySelector('#modal-root .modal, #menu-root .menu') && !typing) { closeSheet(); return; }
    if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (document.querySelector('#modal-root .modal')) return;
    if (e.code === 'Space') { e.preventDefault(); toggle(); }
    else if (e.key === 'ArrowRight') { e.shiftKey ? next(false) : seek(player.position() + 5000); }
    else if (e.key === 'ArrowLeft') { e.shiftKey ? prev() : seek(player.position() - 5000); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); setVolume(player.volume + 0.05); }
    else if (e.key === 'ArrowDown') { e.preventDefault(); setVolume(player.volume - 0.05); }
    else if (e.key.toLowerCase() === 'm') toggleMute();
    else if (e.key.toLowerCase() === 's') setShuffle(!player.shuffle);
    else if (e.key.toLowerCase() === 'l') { if (sheetOpen && sheetMode === 'lyrics') closeSheet(); else openSheet('lyrics'); }
  });
}

// ================================================================== boot
async function boot() {
  await loadPrefs();
  initPlayer();
  setupTopbar();
  setupPlayerBar();
  setupKeys();

  bus.on('player:track', () => { drawBarTrack(); markSidebarPlaying(); });
  bus.on('player:state', () => { drawBarState(); });
  bus.on('player:mode', () => {
    drawBarState();
    for (const b of [bar.shuf, bar.rep]) { b.classList.remove('bump'); void b.offsetWidth; b.classList.add('bump'); }
  });
  bus.on('player:volume', drawBarVolume);
  bus.on('player:queue', () => { if (panelOpen && panelTab === 'queue') drawPanel(); });
  bus.on('player:error', () => { bar.artist.textContent = player.error; });
  bus.on('panel:tab', (t) => { panelTab = t; drawPanel(); });
  bus.on('panel:toggle', () => togglePanel(false));
  bus.on('store:playlists', drawSidebar);
  bus.on('store:google', drawAvatar);
  bus.on('recent:changed', () => refreshPlaylists().catch(() => {}));

  bus.on('srv:google', (g) => { store.google = g; bus.emit('store:google'); });
  bus.on('srv:library', (l) => { store.library = l; bus.emit('store:library'); });
  bus.on('srv:discord', (d) => { store.discord = d; });
  bus.on('srv:googleError', (d) => toast(d.error, 'error'));
  bus.on('srv:notice', (d) => toast(d.msg, 'error'));
  bus.on('store:playlists', markSidebarPlaying);
  document.addEventListener('contextmenu', (e) => { const t = e.target; if (!(t.closest && t.closest('input, textarea, [contenteditable]'))) e.preventDefault(); });
  bus.on('srv:online', (ok) => {
    store.online = ok;
    $('#offline').classList.toggle('show', !ok);
    if (ok) { refreshPlaylists().catch(() => {}); refreshLibrary().catch(() => {}); }
  });

  connectEvents();
  try { await Promise.all([refreshPlaylists(), refreshLibrary(), refreshGoogle()]); }
  catch (e) { toast(e.message, 'error'); }
  drawSidebar();
  drawAvatar();
  window.addEventListener('hashchange', route);
  route();
}

boot();
