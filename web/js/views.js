// Pages and side panels. Every view takes a root element and returns a dispose() function.
import { h, clear, icon, fmtTime, fmtTotal, plural, toast, promptBox, confirmBox, debounce } from './ui.js';
import { api, bus, fromYt, fromLocal, fromEntry, fromDeezer, coverOf, trackKey, toEntry, ytThumb } from './api.js';
import { store, findPlaylist, renamePlaylist, deletePlaylist, createPlaylist, refreshPlaylists } from './store.js';
import { player, play, setShuffle, seek, upcoming, removeUpcoming, clearUpNext } from './player.js';
import { cover, trackCover, mosaic, trackList, card, tile, shelf, skeletonShelf, pageHeader, markPlaying } from './components.js';
import { addToPlaylistModal, folderPickerModal, importModal, settingsModal } from './dialogs.js';

const go = (hash) => { location.hash = hash; };
const subs = () => { const l = []; return { on: (n, f) => l.push(bus.on(n, f)), off: () => l.forEach((f) => f()) }; };

const DISCOVER = [
  ['Trending now', 'top hits this week official audio'],
  ['Chill vibes', 'chill songs to relax'],
  ['Workout energy', 'workout motivation songs'],
  ['Throwback hits', '2000s hits'],
  ['Late night drive', 'late night drive songs'],
  ['Acoustic & unplugged', 'acoustic songs'],
];

const trackCards = (tracks) => tracks.map((t, i) => card({ title: t.title, sub: t.artist, image: t.thumb || coverOf(t), track: t, onPlay: () => play(tracks, i), onClick: () => play(tracks, i) }));

function errorNote(msg, withSettings) {
  return h('div', { class: 'note' }, h('span', { text: msg }),
    withSettings ? h('button', { class: 'btn small', text: 'Open settings', onclick: () => settingsModal('sources') }) : null);
}

// ================================================================== HOME
export function homeView(root) {
  const s = subs();
  const hour = new Date().getHours();
  root.append(h('h1', { class: 'greet', text: hour < 5 ? 'Late night listening' : hour < 12 ? 'Good morning' : hour < 18 ? 'Good afternoon' : 'Good evening' }));

  // filter chips, like the desktop app: All / Your playlists / Recently played / Discover
  let chip = 0;
  const labels = ['All', 'Your playlists', 'Recently played', 'Discover'];
  const chipRow = h('div', { class: 'chips-row' }, labels.map((c, i) => h('button', { class: 'chip-btn' + (i === 0 ? ' on' : ''), text: c, onclick: () => { chip = i; applyChip(); } })));
  const tilesBox = h('div', { class: 'sec' }), plBox = h('div', { class: 'sec' }), recBox = h('div', { class: 'sec' }), discBox = h('div', { class: 'sec' });
  const note = h('div', { class: 'empty-note', hidden: true });
  root.append(chipRow, tilesBox, plBox, recBox, discBox, note);
  const groups = [[tilesBox, [0]], [plBox, [0, 1]], [recBox, [0, 2]], [discBox, [0, 3]]];

  const applyChip = () => {
    chipRow.querySelectorAll('.chip-btn').forEach((b, i) => b.classList.toggle('on', i === chip));
    for (const [el, on] of groups) {
      const show = on.includes(chip) && el.childElementCount > 0;
      if (show && el.hidden) { el.hidden = false; el.classList.remove('enter'); void el.offsetWidth; el.classList.add('enter'); }
      else if (!show) el.hidden = true;
    }
    note.hidden = !((chip === 1 && !store.playlists.length) || (chip === 2 && !store.recent.length));
    note.textContent = chip === 1 ? 'No playlists yet. Use \u201c+ Create\u201d in the sidebar.' : 'Nothing played yet. Songs you play show up here.';
  };

  const drawTop = () => {
    clear(tilesBox); clear(plBox); clear(recBox);
    const recent = store.recent.map(fromEntry);
    const tiles = [];
    for (const p of store.playlists.slice(0, 4)) {
      const tracks = p.entries.map(fromEntry);
      tiles.push(tile({ title: p.name, node: mosaic(tracks, 'tile-art'), onClick: () => go('#/playlist/' + p.id), onPlay: () => tracks.length && play(tracks, setShuffleStart(tracks), { playlistId: p.id }) }));
    }
    recent.slice(0, 8 - tiles.length).forEach((t, i) => tiles.push(tile({ title: t.title, image: coverOf(t), track: t, onPlay: () => play(recent, i) })));
    if (tiles.length) tilesBox.append(h('div', { class: 'tiles' }, tiles.map((el, i) => (el.style.setProperty('--i', i), el))));
    if (store.playlists.length) {
      plBox.append(shelf('Your playlists', store.playlists.map((p) => {
        const tracks = p.entries.map(fromEntry);
        return card({ title: p.name, sub: plural(p.entries.length, 'song'), node: mosaic(tracks), onClick: () => go('#/playlist/' + p.id), onPlay: () => tracks.length && play(tracks, setShuffleStart(tracks), { playlistId: p.id }) });
      })));
    }
    if (recent.length) recBox.append(shelf('Recently played', trackCards(recent)));
    if (!store.playlists.length && !recent.length)
      tilesBox.append(h('div', { class: 'welcome' },
        h('h2', { text: 'Welcome to Pulsify' }),
        h('p', { class: 'muted', text: 'Search for any song, add a music folder from your computer, or import a playlist. Anything you play shows up here.' }),
        h('div', { class: 'row gap wrap' },
          h('button', { class: 'btn primary', text: 'Add music folder', onclick: () => folderPickerModal() }),
          h('button', { class: 'btn', text: 'Import a playlist', onclick: () => importModal() }))));
    applyChip();
    markPlaying(root);
  };
  s.on('store:playlists', drawTop);

  // Discover shelves (yt-dlp searches, cached by the server for 8 hours)
  const slots = DISCOVER.map(([title]) => skeletonShelf(title));
  slots.forEach((el) => discBox.append(el));
  let errorShown = false;
  DISCOVER.forEach(async ([title, query], i) => {
    try {
      const r = await api.q('/api/shelf', { q: query });
      if (!root.isConnected) return;
      const tracks = r.items.map(fromYt);
      if (tracks.length) { const el = shelf(title, trackCards(tracks)); slots[i].replaceWith(el); slots[i] = el; markPlaying(el); }
      else slots[i].remove();
    } catch (e) {
      if (!root.isConnected) return;
      if (errorShown) { slots[i].remove(); return; }              // one message is enough
      errorShown = true;
      slots[i].replaceWith(h('section', { class: 'shelf' }, h('div', { class: 'shelf-head' }, h('h2', { text: 'Discover' })),
        errorNote(e.message, true)));
    }
  });

  // Deezer charts
  const charts = h('div');
  discBox.append(charts);
  (async () => {
    try {
      const [tr, ar, al] = await Promise.all([
        api.q('/api/deezer', { path: 'chart/0/tracks', limit: 50 }),
        api.q('/api/deezer', { path: 'chart/0/artists', limit: 30 }),
        api.q('/api/deezer', { path: 'chart/0/albums', limit: 30 }),
      ]);
      if (!root.isConnected) return;
      const tracks = (tr.data || []).map((o) => fromDeezer(o)).filter((t) => t.title);
      if (tracks.length) charts.append(shelf('Top songs right now', trackCards(tracks), { sub: 'Matched on YouTube when you press play' }));
      const artists = (ar.data || []).filter((a) => a.id && a.name);
      if (artists.length) charts.append(shelf('Popular artists', artists.map((a) => card({
        title: a.name, sub: 'Artist', image: a.picture_big || a.picture_medium, round: true,
        onClick: () => go(`#/artist/${a.id}?name=${encodeURIComponent(a.name)}&img=${encodeURIComponent(a.picture_big || a.picture_medium || '')}`),
      }))));
      const albums = (al.data || []).filter((a) => a.id && a.title);
      if (albums.length) charts.append(shelf('Popular albums', albums.map((a) => card({
        title: a.title, sub: (a.artist && a.artist.name) || '', image: a.cover_big || a.cover_medium,
        onClick: () => go(`#/album/${a.id}?name=${encodeURIComponent(a.title)}&artist=${encodeURIComponent((a.artist && a.artist.name) || '')}&img=${encodeURIComponent(a.cover_big || a.cover_medium || '')}`),
      }))));
      markPlaying(charts);
    } catch { /* offline: the YouTube shelves above still work */ }
  })();

  drawTop();
  s.on('player:track', () => markPlaying(root));
  s.on('player:state', () => markPlaying(root));
  return s.off;
}

const setShuffleStart = (tracks) => (player.shuffle ? Math.floor(Math.random() * tracks.length) : 0);

// ================================================================== SEARCH
export function searchView(root, q) {
  const s = subs();
  q = (q || '').trim();
  if (!q) {
    root.append(h('h1', { text: 'Search' }), h('p', { class: 'muted', text: 'Type a song, artist or album in the box at the top.' }));
    return () => {};
  }
  root.append(h('h1', { class: 'ellip', text: `Results for \u201c${q}\u201d` }));
  const lower = q.toLowerCase();

  const plBox = h('div'), localBox = h('div'), ytBox = h('div');
  root.append(plBox, localBox, ytBox);

  const pls = store.playlists.filter((p) => p.name.toLowerCase().includes(lower));
  if (pls.length) plBox.append(shelf('Playlists', pls.map((p) => {
    const tracks = p.entries.map(fromEntry);
    return card({ title: p.name, sub: plural(p.entries.length, 'song'), node: mosaic(tracks), onClick: () => go('#/playlist/' + p.id) });
  })));

  const local = store.library.tracks.filter((t) => `${t.title} ${t.artist} ${t.album}`.toLowerCase().includes(lower)).slice(0, 12).map(fromLocal);
  if (local.length) localBox.append(h('h2', { class: 'sec-title', text: 'On this computer' }), trackList(local));

  // YouTube results stream in while yt-dlp works
  const results = [];
  const status = h('div', { class: 'muted small', text: 'Searching YouTube\u2026' });
  const list = h('div');
  ytBox.append(h('h2', { class: 'sec-title', text: 'On YouTube' }), status, list);
  let token = null, finished = false;
  const redraw = debounce(() => {
    clear(list);
    if (!results.length) return;
    const top = results[0];
    list.append(h('div', { class: 'top-result' },
      h('div', { class: 'tr-card' }, trackCover(top, 'lg'), h('div', { class: 'tr-title', text: top.title }), h('div', { class: 'tr-sub', text: top.artist }),
        h('button', { class: 'play-fab', title: 'Play', onclick: () => play(results, 0) }, icon('play', 28)))),
      trackList(results));
    markPlaying(list);
  }, 120);

  s.on('srv:searchRetry', (d) => { if (d.from === token) token = d.to; });
  s.on('srv:search', (d) => {
    if (d.token !== token) return;
    results.push(...d.items.map(fromYt));
    status.textContent = finished ? '' : `Searching YouTube\u2026 ${results.length} found`;
    redraw();
  });
  s.on('srv:searchDone', (d) => {
    if (d.token !== token) return;
    finished = true;
    status.textContent = d.error ? d.error : (results.length ? '' : 'No results on YouTube.');
    if (d.error && /yt-dlp/.test(d.error)) status.append(' ', h('button', { class: 'btn small', text: 'Open settings', onclick: () => settingsModal('sources') }));
  });
  api.post('/api/search', { q }).then((r) => { token = r.token; })
    .catch((e) => { status.textContent = e.message; if (/yt-dlp/.test(e.message)) status.append(' ', h('button', { class: 'btn small', text: 'Open settings', onclick: () => settingsModal('sources') })); });

  s.on('player:track', () => markPlaying(root));
  s.on('player:state', () => markPlaying(root));
  return () => { s.off(); };
}

// ================================================================== LIBRARY (local files)
export function libraryView(root) {
  const s = subs();
  const head = h('div');
  const body = h('div');
  const filter = h('input', { class: 'text-input', type: 'search', placeholder: 'Filter your library' });
  root.append(head, body);
  let q = '';
  let killDur = false;

  const durQueue = new Audio();
  durQueue.preload = 'metadata';
  const fillDurations = async (tracks) => {
    for (const t of tracks) {
      if (killDur || !root.isConnected) return;
      if (t.dur) continue;
      await new Promise((resolve) => {
        const done = () => { durQueue.onloadedmetadata = durQueue.onerror = null; clearTimeout(tm); resolve(); };
        const tm = setTimeout(done, 3000);
        durQueue.onloadedmetadata = () => { if (isFinite(durQueue.duration)) { t.dur = Math.round(durQueue.duration * 1000); if (t._durEl) t._durEl.textContent = fmtTime(t.dur); } done(); };
        durQueue.onerror = done;
        durQueue.src = '/api/file?path=' + encodeURIComponent(t.path);
      });
    }
  };

  const draw = () => {
    clear(head); clear(body);
    const lib = store.library;
    head.append(
      h('div', { class: 'row gap center wrap' }, h('h1', { text: 'Your library' }), h('span', { class: 'grow' }),
        h('button', { class: 'btn', onclick: () => importModal() }, icon('import', 18), ' Import playlist'),
        h('button', { class: 'btn primary', onclick: () => folderPickerModal() }, icon('plus', 18), ' Add folder')),
      lib.folders.length ? h('div', { class: 'chips' }, lib.folders.map((f) => h('span', { class: 'chip static' }, icon('folder', 14), ' ' + f,
        h('button', { class: 'chip-x', title: 'Remove folder', onclick: async () => { store.library = await api.post('/api/library/remove', { path: f }); bus.emit('store:library'); } }, icon('close', 14))))) : null,
      lib.scanning ? h('div', { class: 'muted small', text: 'Scanning your music\u2026' }) : null);
    if (!lib.folders.length) {
      body.append(h('div', { class: 'welcome' }, h('h2', { text: 'Play music from your computer' }),
        h('p', { class: 'muted', text: 'Add a folder and Pulsify will find every mp3, flac, m4a, ogg, opus and wav inside it, including sub-folders.' }),
        h('button', { class: 'btn primary', text: 'Add music folder', onclick: () => folderPickerModal() })));
      return;
    }
    const tracks = lib.tracks.map(fromLocal);
    const shown = q ? tracks.filter((t) => `${t.title} ${t.artist} ${t.album}`.toLowerCase().includes(q)) : tracks;
    body.append(
      h('div', { class: 'row gap center', style: { margin: '12px 0' } }, filter,
        h('span', { class: 'muted small', text: plural(shown.length, 'song') }),
        h('span', { class: 'grow' }),
        h('button', { class: 'btn primary', onclick: () => shown.length && play(shown, setShuffleStart(shown)) }, icon('play', 18), ' Play'),
        h('button', { class: 'btn', onclick: () => { if (shown.length) { setShuffle(true); play(shown); } } }, icon('shuffle', 18), ' Shuffle')),
      trackList(shown, { emptyText: lib.scanning ? 'Scanning\u2026' : 'No songs match.' }));
    fillDurations(shown.slice(0, 200));
  };
  filter.oninput = debounce(() => { q = filter.value.trim().toLowerCase(); const hadFocus = document.activeElement === filter; draw(); if (hadFocus) { filter.focus(); } }, 200);
  draw();
  s.on('store:library', () => { const v = filter.value; draw(); filter.value = v; });
  s.on('player:track', () => markPlaying(root));
  s.on('player:state', () => markPlaying(root));
  return () => { killDur = true; durQueue.src = ''; s.off(); };
}

// ================================================================== PLAYLIST
export function playlistView(root, id) {
  const s = subs();
  const draw = () => {
    clear(root);
    const p = findPlaylist(id);
    if (!p) { root.append(h('h1', { text: 'Playlist not found' }), h('button', { class: 'btn', text: 'Home', onclick: () => go('#/home') })); return; }
    const tracks = p.entries.map(fromEntry);
    const total = tracks.reduce((a, t) => a + (t.dur || 0), 0);
    const actions = [
      h('button', { class: 'play-fab', title: 'Play', onclick: () => tracks.length && play(tracks, setShuffleStart(tracks), { playlistId: id }) }, icon('play', 28)),
      h('button', { class: 'icon-btn big' + (player.shuffle ? ' on' : ''), title: 'Shuffle', onclick: () => { setShuffle(!player.shuffle); draw(); } }, icon('shuffle', 24)),
      p.source && p.source.startsWith('yt:') ? h('button', { class: 'btn', title: 'Add songs that were added on YouTube since the import', onclick: async (e) => {
        const b = e.currentTarget; b.disabled = true;
        try {
          const r = await api.post('/api/import/yt', { link: 'https://www.youtube.com/playlist?list=' + p.source.slice(3) });
          const res = await createPlaylist(r.name, r.items.map((it) => toEntry(fromYt(it))), p.source);
          toast(res.added ? `Added ${plural(res.added, 'new song')}` : 'Already up to date');
        } catch (err) { toast(err.message, 'error'); b.disabled = false; }
      } }, icon('refresh', 18), ' Update from YouTube') : null,
      h('button', { class: 'btn', onclick: async () => { const n = await promptBox('Rename playlist', 'Name', p.name); if (n) renamePlaylist(id, n).catch((e) => toast(e.message, 'error')); } }, icon('edit', 18), ' Rename'),
      h('button', { class: 'btn danger-ghost', onclick: async () => { if (await confirmBox('Delete playlist', `Delete \u201c${p.name}\u201d? The songs themselves aren\u2019t touched.`)) { await deletePlaylist(id); go('#/home'); } } }, icon('trash', 18), ' Delete'),
    ];
    root.append(
      pageHeader({ kicker: 'Playlist', title: p.name, sub: `${plural(tracks.length, 'song')}${total ? ' \u00b7 ' + fmtTotal(total) : ''}`, art: mosaic(tracks, 'xl'), actions }),
      trackList(tracks, { playlistId: id, reorder: true, emptyText: 'This playlist is empty. Use \u201cAdd to playlist\u201d on any song.' }));
    markPlaying(root);
  };
  draw();
  s.on('store:playlists', draw);
  s.on('player:track', () => markPlaying(root));
  s.on('player:state', () => markPlaying(root));
  return s.off;
}

// ================================================================== ARTIST / ALBUM (Deezer)
export function collectionView(root, kind, id, query) {
  const s = subs();
  const name = query.get('name') || (kind === 'artist' ? 'Artist' : 'Album');
  const img = query.get('img') || '';
  root.append(h('div', { class: 'muted', text: 'Loading\u2026' }));
  const path = kind === 'artist' ? `artist/${id}/top` : `album/${id}/tracks`;
  api.q('/api/deezer', { path, limit: kind === 'artist' ? 50 : 100 }).then((r) => {
    if (!root.isConnected) return;
    clear(root);
    const tracks = (r.data || []).map((o) => fromDeezer(o, kind === 'album' ? name : '', kind === 'album' ? img : '')).filter((t) => t.title);
    const total = tracks.reduce((a, t) => a + (t.dur || 0), 0);
    root.append(
      pageHeader({
        kicker: kind === 'artist' ? 'Artist' : 'Album', title: name,
        sub: kind === 'album' ? `${query.get('artist') || ''} \u00b7 ${plural(tracks.length, 'song')} \u00b7 ${fmtTotal(total)}` : `Top ${plural(tracks.length, 'song')}`,
        art: cover(img, kind === 'artist' ? 'xl round' : 'xl'),
        actions: [
          h('button', { class: 'play-fab', onclick: () => tracks.length && play(tracks, setShuffleStart(tracks)) }, icon('play', 28)),
          h('button', { class: 'btn', onclick: () => addToPlaylistModal(tracks) }, icon('plus', 18), ' Add all to playlist'),
        ],
      }),
      trackList(tracks, { emptyText: 'Nothing found.' }));
    markPlaying(root);
  }).catch((e) => { clear(root); root.append(errorNote(e.message)); });
  s.on('player:track', () => markPlaying(root));
  s.on('player:state', () => markPlaying(root));
  return s.off;
}

// ================================================================== LYRICS
const lyricsCache = new Map();

export function lyricsView(root, { title = true } = {}) {
  const s = subs();
  const stage = h('div', { class: 'lyrics-stage' });
  root.append(stage);
  let doc = null, lines = [], curIdx = -2, scrollerHold = 0, loadedFor = null;

  const load = async (refresh = false) => {
    const t = player.current;
    clear(stage);
    doc = null; lines = []; curIdx = -2;
    if (!t) { stage.append(h('p', { class: 'muted center-text', text: 'Play a song to see its lyrics.' })); return; }
    const key = trackKey(t);
    loadedFor = key;
    if (title) stage.append(h('div', { class: 'lyrics-title' }, h('h1', { text: t.title }), h('div', { class: 'muted', text: t.artist })));
    const body = h('div', { class: 'lyrics-body' }, h('p', { class: 'muted', text: 'Looking for lyrics\u2026' }));
    stage.append(body);
    try {
      let d = !refresh && lyricsCache.get(key);
      if (!d) {
        d = await api.q('/api/lyrics', { key, artist: t.artist || '', title: t.title || '', album: t.album || '', dur: Math.round(player.duration() || t.dur || 0), refresh: refresh ? '1' : '0' });
        if (!d.none) lyricsCache.set(key, d);
      }
      if (loadedFor !== key || !root.isConnected) return;
      doc = d;
      clear(body);
      if (d.none) { body.append(h('p', { class: 'muted', text: d.message || 'No lyrics found for this song.' })); }
      else if (d.instrumental) { body.append(h('p', { class: 'muted big', text: '\u266a Instrumental \u266a' })); }
      else {
        d.lines.forEach(([t0, text, header], i) => {
          const el = h('div', { class: 'lyric' + (header ? ' header' : '') + (!text ? ' gap' : '') + (d.synced && t0 >= 0 ? ' timed' : ''), dataset: { i }, text: text || '\u00a0' });
          if (d.synced && t0 >= 0) el.onclick = () => seek(t0);
          lines.push({ t: t0, el });
          body.append(el);
        });
        const src = [d.textSource && `Lyrics: ${d.textSource}`, d.synced && d.timeSource && `Timing: ${d.timeSource}`, d.approx && 'timing may be slightly off for this recording'].filter(Boolean).join(' \u00b7 ');
        body.append(h('div', { class: 'lyrics-foot muted small' }, h('span', { text: src }), h('button', { class: 'btn small ghost', onclick: () => { lyricsCache.delete(key); load(true); } }, icon('refresh', 14), ' Refresh')));
        tick();
      }
    } catch (e) { if (loadedFor === key) { clear(body); body.append(h('p', { class: 'muted', text: e.message })); } }
  };

  const tick = () => {
    if (!doc || !doc.synced || !lines.length) return;
    const pos = player.position() + 250;                     // light the line slightly early
    let idx = -1;
    const timed = lines.map((l, i) => [l.t, i]).filter(([t]) => t >= 0);
    for (let a = 0, b = timed.length - 1; a <= b;) { const m = (a + b) >> 1; if (timed[m][0] <= pos) { idx = timed[m][1]; a = m + 1; } else b = m - 1; }
    if (idx === curIdx) return;
    curIdx = idx;
    lines.forEach((l, i) => { l.el.classList.toggle('cur', i === idx); l.el.classList.toggle('past', idx >= 0 && i < idx && l.t >= 0); });
    if (idx >= 0 && Date.now() > scrollerHold) {
      const el = lines[idx].el;
      const sc = root.closest('[data-scroll]') || root.closest('#view') || root;
      const delta = el.getBoundingClientRect().top - sc.getBoundingClientRect().top;
      sc.scrollTo({ top: sc.scrollTop + delta - sc.clientHeight / 2 + el.clientHeight / 2, behavior: 'smooth' });
    }
  };

  const sc = root.closest('[data-scroll]') || root.closest('#view') || root;
  const hold = () => { scrollerHold = Date.now() + 3500; };
  sc.addEventListener('wheel', hold, { passive: true });
  sc.addEventListener('touchmove', hold, { passive: true });

  load();
  s.on('player:track', load);
  s.on('player:time', tick);
  return () => { s.off(); sc.removeEventListener('wheel', hold); sc.removeEventListener('touchmove', hold); };
}

// ================================================================== RIGHT PANEL
export function panelView(el, tab) {
  clear(el);
  const t = player.current;
  const tabs = h('div', { class: 'tabs small' },
    h('button', { class: 'tab' + (tab === 'now' ? ' active' : ''), text: 'Now playing', onclick: () => bus.emit('panel:tab', 'now') }),
    h('button', { class: 'tab' + (tab === 'queue' ? ' active' : ''), text: 'Queue', onclick: () => bus.emit('panel:tab', 'queue') }),
    h('span', { class: 'grow' }),
    h('button', { class: 'icon-btn', title: 'Hide panel', onclick: () => bus.emit('panel:toggle') }, icon('close', 18)));
  el.append(tabs);
  const body = h('div', { class: 'panel-body' });
  el.append(body);

  if (tab === 'queue') {
    if (t) body.append(h('h3', { class: 'panel-h', text: 'Now playing' }), miniRow(t, true));
    const up = upcoming();
    body.append(h('div', { class: 'row center' }, h('h3', { class: 'panel-h grow', text: 'Next up' }),
      player.upNext.length ? h('button', { class: 'btn small ghost', text: 'Clear queued', onclick: clearUpNext }) : null));
    if (!up.length) body.append(h('p', { class: 'muted small', text: 'Nothing is queued. Use \u201cPlay next\u201d or \u201cAdd to queue\u201d on any song.' }));
    up.slice(0, 80).forEach((u, i) => body.append(miniRow(u.track, false, () => removeUpcoming(i), u.queued)));
    return;
  }
  if (!t) { body.append(h('p', { class: 'muted', text: 'Nothing is playing.' })); return; }
  const st = player.stream;
  body.append(
    h('div', { class: 'np-art' }, trackCover(t, 'xl')),
    h('div', { class: 'np-title', text: t.title }),
    h('div', { class: 'np-artist', text: t.artist || '' }),
    t.album ? h('div', { class: 'muted small', text: t.album }) : null,
    h('div', { class: 'row gap wrap', style: { margin: '14px 0' } },
      h('button', { class: 'btn', onclick: () => go('#/lyrics') }, icon('mic', 18), ' Lyrics'),
      h('button', { class: 'btn', onclick: () => addToPlaylistModal([t]) }, icon('plus', 18), ' Add to playlist')));
  if (st) {
    const rows = [
      st.container && ['Format', `${st.container}${st.codec ? ' \u00b7 ' + st.codec : ''}`],
      st.bitrate && ['Bitrate', `${st.bitrate} kbps`],
      st.sampleRate && ['Sample rate', `${st.sampleRate} Hz`],
      st.channel && ['Channel', st.channel],
      st.views && ['Views', st.views.toLocaleString()],
      st.uploadDate && ['Uploaded', st.uploadDate.replace(/^(\d{4})(\d{2})(\d{2})$/, '$1-$2-$3')],
    ].filter(Boolean);
    body.append(h('div', { class: 'info' }, rows.map(([k, v]) => h('div', { class: 'info-row' }, h('span', { class: 'muted', text: k }), h('span', { text: v })))),
      st.webUrl ? h('a', { class: 'link small', href: st.webUrl, target: '_blank', rel: 'noopener noreferrer', text: 'Open on YouTube' }) : null);
  } else if (t.kind === 'local') {
    body.append(h('div', { class: 'info' }, h('div', { class: 'info-row' }, h('span', { class: 'muted', text: 'File' }), h('span', { class: 'ellip', text: t.path }))));
  }
}

function miniRow(t, current, onRemove, queued) {
  return h('div', { class: 'mini' + (current ? ' current' : '') },
    trackCover(t, 'sm'),
    h('div', { class: 'ttext grow' }, h('div', { class: 'ttitle', text: t.title }), h('div', { class: 'tartist', text: (queued ? 'Queued \u00b7 ' : '') + (t.artist || '') })),
    onRemove ? h('button', { class: 'icon-btn', title: 'Remove', onclick: onRemove }, icon('close', 16)) : null);
}
