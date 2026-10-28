// Dialogs: add to playlist, folder picker, import (link / text / CSV / Google), settings + appearance.
import { h, clear, icon, openModal, promptBox, toast, plural, debounce } from './ui.js';
import { api, bus, fromYt, toEntry, ensurePlayable } from './api.js';
import { store, refreshPlaylists, refreshLibrary, refreshGoogle, createPlaylist, addToPlaylist } from './store.js';
import { PRESETS, getTheme, setTheme, imageFileToDataUrl } from './theme.js';

// ================================================================== add to playlist
export function addToPlaylistModal(tracks) {
  if (!tracks.length) return;
  const list = h('div', { class: 'pick-list' });
  const m = openModal('Add to playlist', h('div', {},
    h('button', {
      class: 'pick-item accent', onclick: async () => {
        const name = await promptBox('New playlist', 'Playlist name', '', 'Create');
        if (name === null) return;
        m.close();
        try {
          const r = await createPlaylist(name);
          await addToPlaylist(r.id, tracks);
        } catch (e) { toast(e.message, 'error'); }
      },
    }, icon('plus', 20), h('span', { text: 'New playlist' })),
    list));
  if (!store.playlists.length) list.append(h('p', { class: 'muted', text: 'You don\u2019t have any playlists yet.' }));
  for (const p of store.playlists) {
    list.append(h('button', {
      class: 'pick-item', onclick: async () => {
        m.close();
        try { await addToPlaylist(p.id, tracks); } catch (e) { toast(e.message, 'error'); }
      },
    }, icon('music', 20), h('span', { class: 'grow', text: p.name }), h('span', { class: 'muted small', text: plural(p.entries.length, 'song') })));
  }
}

// ================================================================== folder picker
export function folderPickerModal() {
  const pathInput = h('input', { class: 'text-input', type: 'text', placeholder: 'Folder path' });
  const list = h('div', { class: 'pick-list tall' });
  const info = h('div', { class: 'muted small' });
  const roots = h('div', { class: 'row wrap gap' });
  let current = '';
  let parent = '';

  const go = async (path) => {
    try {
      const d = await api.q('/api/fs', { path });
      current = d.path; parent = d.parent || '';
      pathInput.value = current;
      clear(list);
      if (parent) list.append(h('button', { class: 'pick-item', onclick: () => go(parent) }, icon('back', 20), h('span', { text: '..' })));
      for (const n of d.dirs) list.append(h('button', { class: 'pick-item', onclick: () => go(current.replace(/\/$/, '') + '/' + n) }, icon('folder', 20), h('span', { text: n })));
      if (!d.dirs.length) list.append(h('p', { class: 'muted', text: 'No sub-folders here.' }));
      info.textContent = d.audioFiles ? `${plural(d.audioFiles, 'audio file')} in this folder (sub-folders are scanned too)` : 'Sub-folders are scanned too.';
      clear(roots);
      for (const r of d.roots) roots.append(h('button', { class: 'chip', text: r, onclick: () => go(r) }));
      if (d.home) roots.append(h('button', { class: 'chip', text: 'Home', onclick: () => go(d.home) }));
    } catch (e) { toast(e.message, 'error'); }
  };
  pathInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') go(pathInput.value); });

  const m = openModal('Add a music folder', h('div', {},
    roots, pathInput, list, info,
    h('div', { class: 'row end gap', style: { marginTop: '16px' } },
      h('button', { class: 'btn ghost', text: 'Cancel', onclick: () => m.close() }),
      h('button', {
        class: 'btn primary', text: 'Use this folder', onclick: async () => {
          try {
            store.library = await api.post('/api/library/add', { path: current });
            bus.emit('store:library');
            toast('Scanning your music\u2026');
            m.close();
          } catch (e) { toast(e.message, 'error'); }
        },
      }))), { wide: true });
  go('');
}

// ================================================================== import
function parseCsvRows(text, delim) {
  const rows = []; let row = []; let cell = ''; let q = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (q) {
      if (c === '"') { if (text[i + 1] === '"') { cell += '"'; i++; } else q = false; } else cell += c;
    } else if (c === '"') q = true;
    else if (c === delim) { row.push(cell); cell = ''; }
    else if (c === '\n') { row.push(cell); rows.push(row); row = []; cell = ''; }
    else if (c !== '\r') cell += c;
  }
  if (cell.length || row.length) { row.push(cell); rows.push(row); }
  return rows;
}

function parseDur(s) {
  s = (s || '').trim();
  if (!s) return 0;
  if (s.includes(':')) { const p = s.split(':').map(Number); return p.reduce((a, v) => a * 60 + (v || 0), 0) * 1000; }
  const n = Number(s);
  if (!isFinite(n)) return 0;
  return n > 10000 ? Math.round(n) : Math.round(n * 1000);        // milliseconds or seconds
}

export function parseSongList(text) {
  text = text.replace(/^\uFEFF/, '');
  const tabs = (text.match(/\t/g) || []).length, commas = (text.match(/,/g) || []).length;
  const rows = parseCsvRows(text, tabs > commas ? '\t' : ',');
  if (rows.length >= 2) {
    const hdr = rows[0].map((x) => x.toLowerCase().trim());
    const find = (...keys) => hdr.findIndex((x) => keys.some((k) => x.includes(k)));
    const cT = find('title', 'track name'), cA = find('artist'), cB = find('album'), cD = find('duration');
    if (cT >= 0 && cA >= 0) {
      const out = [];
      for (const r of rows.slice(1)) {
        const title = (r[cT] || '').trim();
        if (!title) continue;
        const artist = (r[cA] || '').trim();
        out.push({ kind: 'pending', title, artist, album: cB >= 0 ? (r[cB] || '').trim() : '', dur: cD >= 0 ? parseDur(r[cD]) : 0, query: `${artist} ${title}`.trim() });
      }
      if (out.length) return out;
    }
  }
  const out = [];
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const m = line.match(/^(.{1,80}?)\s*[-\u2013\u2014]\s*(.+)$/);
    const artist = m ? m[1].trim() : '', title = m ? m[2].trim() : line;
    out.push({ kind: 'pending', title, artist, album: '', dur: 0, query: `${artist} ${title}`.trim() });
  }
  return out;
}

// Finds a YouTube match for each song, three at a time. Resolves with the songs that matched.
async function matchAll(tracks, onProgress, state) {
  const out = new Array(tracks.length).fill(null);
  let next = 0, done = 0;
  const worker = async () => {
    while (!state.cancelled) {
      const i = next++;
      if (i >= tracks.length) return;
      try { out[i] = await ensurePlayable(tracks[i]); } catch { out[i] = null; }
      onProgress(++done, tracks.length);
    }
  };
  await Promise.all([worker(), worker(), worker()]);
  return out.filter(Boolean);
}

function navigateToPlaylist(id) { location.hash = '#/playlist/' + id; }

export function importModal(tab = 'link') {
  const body = h('div', { class: 'import' });
  const tabs = h('div', { class: 'tabs' });
  const pane = h('div', { class: 'pane' });
  body.append(tabs, pane);
  let unsub = [];
  const m = openModal('Import a playlist', body, { wide: true, onClose: () => unsub.forEach((f) => f()) });
  const names = { link: 'YouTube link', text: 'Paste or CSV', google: 'Google account' };

  const show = (t) => {
    unsub.forEach((f) => f()); unsub = [];
    clear(tabs);
    for (const k of Object.keys(names)) tabs.append(h('button', { class: 'tab' + (k === t ? ' active' : ''), text: names[k], onclick: () => show(k) }));
    clear(pane);
    if (t === 'link') linkPane(pane);
    else if (t === 'text') textPane(pane);
    else googlePane(pane, unsub, () => show('google'));
  };

  const linkPane = (root) => {
    const input = h('input', { class: 'text-input', type: 'text', placeholder: 'https://www.youtube.com/playlist?list=\u2026  (YouTube or YouTube Music)' });
    const status = h('div', { class: 'muted small' });
    const btn = h('button', { class: 'btn primary', text: 'Import' });
    btn.onclick = async () => {
      btn.disabled = true; status.textContent = 'Reading the playlist\u2026 this can take a minute for long ones.';
      try {
        const r = await api.post('/api/import/yt', { link: input.value.trim() });
        const res = await createPlaylist(r.name || 'Imported playlist', r.items.map((it) => toEntry(fromYt(it))), r.source);
        toast(res.existing ? `Updated: ${plural(res.added, 'new song')} added` : `Imported ${plural(r.items.length, 'song')}`);
        m.close(); navigateToPlaylist(res.id);
      } catch (e) { status.textContent = e.message; btn.disabled = false; }
    };
    root.append(
      h('p', { class: 'muted', text: 'Paste the link of a public or unlisted playlist. Importing it again later only adds the new songs.' }),
      input, h('div', { class: 'row end gap', style: { marginTop: '12px' } }, btn), status);
    setTimeout(() => input.focus(), 30);
  };

  const textPane = (root) => {
    const area = h('textarea', { class: 'text-input area', rows: 8, placeholder: 'One song per line:\nArtist - Title\nAnother Artist - Another Title\n\u2026or choose a CSV (Spotify exports from Exportify etc.)' });
    const file = h('input', { type: 'file', accept: '.csv,.tsv,.txt,text/*', class: 'hidden' });
    const nameIn = h('input', { class: 'text-input', type: 'text', placeholder: 'Playlist name', value: 'Imported songs' });
    const bar = h('div', { class: 'progress' }, h('div', { class: 'fill' }));
    const status = h('div', { class: 'muted small' });
    const go = h('button', { class: 'btn primary', text: 'Match & import' });
    const stop = h('button', { class: 'btn ghost hidden', text: 'Cancel' });
    const state = { cancelled: false };
    file.onchange = async () => {
      const f = file.files[0]; if (!f) return;
      area.value = await f.text();
      nameIn.value = f.name.replace(/\.[^.]+$/, '');
    };
    stop.onclick = () => { state.cancelled = true; status.textContent = 'Cancelling\u2026'; };
    go.onclick = async () => {
      const songs = parseSongList(area.value);
      if (!songs.length) { status.textContent = 'No songs found. Paste one per line (Artist - Title) or choose a CSV.'; return; }
      go.disabled = true; stop.classList.remove('hidden'); bar.classList.add('on'); state.cancelled = false;
      status.textContent = `Finding ${plural(songs.length, 'song')} on YouTube\u2026`;
      const ok = await matchAll(songs, (d, n) => { bar.firstChild.style.width = (100 * d / n) + '%'; status.textContent = `Matched ${d} of ${n}\u2026`; }, state);
      if (!ok.length) { status.textContent = state.cancelled ? 'Cancelled.' : 'None of the songs could be matched.'; go.disabled = false; stop.classList.add('hidden'); return; }
      try {
        const res = await createPlaylist(nameIn.value.trim() || 'Imported songs', ok.map(toEntry));
        toast(`Imported ${ok.length} of ${songs.length} songs`);
        m.close(); navigateToPlaylist(res.id);
      } catch (e) { status.textContent = e.message; go.disabled = false; }
    };
    root.append(
      h('div', { class: 'row gap' }, h('button', { class: 'btn', onclick: () => file.click() }, icon('import', 18), ' Choose CSV / text file'), file),
      area, nameIn, bar,
      h('div', { class: 'row end gap', style: { marginTop: '12px' } }, stop, go), status);
  };

  show(tab);
}

// ---- Google tab (shared by the import dialog and Settings)
function googlePane(root, unsub, rerender) {
  const g = store.google;
  const status = h('div', { class: 'muted small' });
  if (!g.hasCredentials) {
    const id = h('input', { class: 'text-input', type: 'text', placeholder: 'Client ID', value: g.clientId || '' });
    const secret = h('input', { class: 'text-input', type: 'password', placeholder: 'Client secret' });
    const file = h('input', { type: 'file', accept: '.json,application/json', class: 'hidden' });
    file.onchange = async () => {
      try {
        const j = JSON.parse(await file.files[0].text());
        const o = j.installed || j.web || {};
        if (!o.client_id || !o.client_secret) throw new Error('That JSON has no client_id / client_secret.');
        id.value = o.client_id; secret.value = o.client_secret;
      } catch (e) { status.textContent = e.message; }
    };
    root.append(
      h('p', { class: 'muted', text: 'Google requires every app to use its own OAuth client. Create a free \u201cDesktop app\u201d client in Google Cloud Console (turn on \u201cYouTube Data API v3\u201d), then paste its ID and secret here \u2013 or load the downloaded client_secret.json. Pulsify only asks for read-only access to your YouTube playlists.' }),
      id, secret,
      h('div', { class: 'row gap', style: { marginTop: '12px' } },
        h('button', { class: 'btn', onclick: () => file.click() }, icon('import', 18), ' Load client_secret.json'), file,
        h('span', { class: 'grow' }),
        h('button', {
          class: 'btn primary', text: 'Save', onclick: async () => {
            try { store.google = await api.post('/api/google/credentials', { clientId: id.value, clientSecret: secret.value }); rerender(); } catch (e) { status.textContent = e.message; }
          },
        })),
      status);
    return;
  }
  if (!g.signedIn) {
    const btn = h('button', { class: 'btn primary', text: g.signingIn ? 'Waiting for the browser\u2026' : 'Sign in with Google' });
    btn.onclick = async () => {
      btn.disabled = true; status.textContent = 'Opening Google in a new tab\u2026';
      try { const r = await api.post('/api/google/signin', {}); window.open(r.url, '_blank', 'noopener'); status.textContent = 'Finish signing in in the new tab, then come back here.'; }
      catch (e) { status.textContent = e.message; btn.disabled = false; }
    };
    unsub.push(bus.on('store:google', rerender));
    unsub.push(bus.on('srv:googleError', (d) => { status.textContent = d.error; btn.disabled = false; }));
    root.append(
      h('p', { class: 'muted', text: 'Sign in to import your own YouTube playlists (including Liked videos).' }),
      h('div', { class: 'row gap' }, btn,
        h('button', { class: 'btn ghost', text: 'Change client ID / secret', onclick: async () => { store.google = await api.post('/api/google/credentials', { clientId: '', clientSecret: '' }); rerender(); } })),
      status);
    return;
  }
  const list = h('div', { class: 'pick-list tall' }, h('p', { class: 'muted', text: 'Loading your playlists\u2026' }));
  const who = h('div', { class: 'row gap center' },
    g.avatar ? h('img', { class: 'avatar', src: g.avatar, referrerpolicy: 'no-referrer', alt: '' }) : icon('user', 28),
    h('strong', { text: g.name || 'Signed in' }), h('span', { class: 'grow' }),
    h('button', { class: 'btn ghost', text: 'Sign out', onclick: async () => { store.google = await api.post('/api/google/signout', {}); bus.emit('store:google'); rerender(); } }));
  root.append(who, list, status);
  unsub.push(bus.on('srv:googleProgress', (d) => { status.textContent = `Reading playlist\u2026 ${d.got}${d.total ? ' of ' + d.total : ''}`; }));
  api.get('/api/google/playlists').then((r) => {
    clear(list);
    if (!r.playlists.length) list.append(h('p', { class: 'muted', text: 'No playlists found.' }));
    for (const p of r.playlists) {
      const btn = h('button', { class: 'btn small', text: 'Import' });
      btn.onclick = async () => {
        btn.disabled = true; status.textContent = `Reading \u201c${p.title}\u201d\u2026`;
        try {
          const it = await api.q('/api/google/items', { id: p.id });
          const res = await createPlaylist(p.title, it.items.map((x) => toEntry(fromYt(x))), 'yt:' + p.id);
          toast(res.existing ? `Updated: ${plural(res.added, 'new song')} added` : `Imported ${plural(it.items.length, 'song')}`);
          navigateToPlaylist(res.id);
          document.querySelector('#modal-root .modal-back') && document.querySelector('#modal-root .modal-back').remove();
        } catch (e) { status.textContent = e.message; btn.disabled = false; }
      };
      list.append(h('div', { class: 'pick-item static' }, icon('music', 20), h('span', { class: 'grow', text: p.title }),
        h('span', { class: 'muted small', text: p.count >= 0 ? plural(p.count, 'song') : '' }), btn));
    }
  }).catch((e) => { clear(list); list.append(h('p', { class: 'muted', text: e.message })); });
}

// ================================================================== settings
const BROWSERS = ['', 'chrome', 'firefox', 'edge', 'brave', 'opera', 'vivaldi', 'safari', 'chromium'];

export async function settingsModal(startTab = 'sources') {
  let s;
  try { s = await api.get('/api/settings'); } catch (e) { toast(e.message, 'error'); return; }
  const tabs = h('div', { class: 'tabs' });
  const pane = h('div', { class: 'pane scroll' });
  let unsub = [];
  const m = openModal('Settings', h('div', { class: 'settings' }, tabs, pane), { wide: true, onClose: () => unsub.forEach((f) => f()) });
  const names = { sources: 'Sources', integrations: 'Integrations', appearance: 'Appearance' };
  const save = async (patch) => { try { s = await api.post('/api/settings', patch); } catch (e) { toast(e.message, 'error'); } return s; };
  const saveText = debounce(save, 500);

  const section = (title, ...kids) => h('section', { class: 'sec' }, h('h3', { text: title }), ...kids);
  const field = (label, input, hint) => h('label', { class: 'field' }, h('span', { class: 'lbl', text: label }), input, hint ? h('span', { class: 'muted small', text: hint }) : null);

  const sources = () => {
    const stat = h('div', { class: 'status-line', text: s.ytdlpFound ? `yt-dlp found: ${s.ytdlpFound}` : 'yt-dlp isn\u2019t installed yet \u2013 Pulsify needs it to search and play YouTube.' });
    unsub.push(bus.on('srv:tool', async (d) => {
      stat.textContent = d.msg;
      if (d.done) { s = await api.get('/api/settings'); if (d.ok) stat.textContent = `${d.msg} (${s.ytdlpFound})`; }
    }));
    const pathIn = h('input', { class: 'text-input', type: 'text', value: s.ytdlpPath || '', placeholder: 'Leave empty to use the automatic copy' });
    pathIn.oninput = () => saveText({ ytdlpPath: pathIn.value });
    const sel = h('select', { class: 'text-input' }, BROWSERS.map((b) => h('option', { value: b, text: b || 'Don\u2019t use cookies', selected: b === (s.cookies || '') })));
    sel.onchange = () => save({ cookies: sel.value });
    return [
      section('yt-dlp', stat,
        h('div', { class: 'row gap wrap', style: { margin: '10px 0' } },
          h('button', { class: 'btn primary', text: s.ytdlpFound ? 'Reinstall' : 'Download yt-dlp', onclick: () => api.post('/api/ytdlp/install') }),
          h('button', { class: 'btn', text: 'Update', onclick: () => api.post('/api/ytdlp/update') })),
        field('Custom yt-dlp path', pathIn),
        field('Use cookies from', sel, 'Helps when YouTube asks you to sign in or confirm you\u2019re not a bot.'),
        h('p', { class: 'muted small', text: s.jsRuntime ? `JavaScript runtime for yt-dlp: ${s.jsRuntime}` : 'Tip: install Deno or Node.js so yt-dlp can solve YouTube\u2019s challenges reliably.' })),
      section('Cache',
        h('button', { class: 'btn', text: 'Clear audio, lyrics and home caches', onclick: async () => { await api.post('/api/cache/clear'); toast('Caches cleared'); } })),
      section('Music folders',
        h('p', { class: 'muted small', text: store.library.folders.length ? store.library.folders.join('\n') : 'No folders yet.', style: { whiteSpace: 'pre-line' } }),
        h('button', { class: 'btn', text: 'Add folder\u2026', onclick: () => folderPickerModal() })),
    ];
  };

  const integrations = () => {
    const d = s.discord || {};
    const dStat = h('div', { class: 'status-line', text: d.status || '' });
    unsub.push(bus.on('srv:discord', (x) => { dStat.textContent = x.status; }));
    const en = h('input', { type: 'checkbox', checked: d.enabled !== false });
    en.onchange = () => save({ discord: { enabled: en.checked } });
    const cid = h('input', { class: 'text-input', type: 'text', value: d.clientId || '', placeholder: 'Application ID (a long number)' });
    cid.oninput = () => saveText({ discord: { clientId: cid.value } });
    const gen = h('input', { class: 'text-input', type: 'password', value: s.geniusToken || '', placeholder: 'Optional: Genius API token' });
    gen.oninput = () => saveText({ geniusToken: gen.value });
    const gBox = h('div');
    const renderG = () => { clear(gBox); googlePane(gBox, unsub, renderG); };
    renderG();
    return [
      section('Discord Rich Presence', h('label', { class: 'check' }, en, ' Show what I\u2019m listening to'),
        field('Application ID', cid, 'Create a free application named \u201cPulsify\u201d at discord.com/developers/applications and paste its Application ID.'), dStat),
      section('Lyrics', field('Genius token', gen, 'Optional. Lyrics work without it (Genius + LRCLIB).')),
      section('Google account', gBox),
    ];
  };

  const appearance = () => {
    const t = getTheme();
    const grid = h('div', { class: 'swatches' });
    const draw = () => {
      clear(grid);
      for (const p of PRESETS) {
        const grad = (p.grad && p.grad.length >= 2) ? `linear-gradient(${p.angle + 90}deg, ${p.grad.join(',')})` : p.bg;
        grid.append(h('button', {
          class: 'swatch' + (getTheme().id === p.id ? ' on' : ''), title: p.name,
          onclick: () => { setTheme({ id: p.id }); draw(); custom(); },
        }, h('span', { class: 'sw-bg', style: { background: grad } }, h('span', { class: 'sw-dot', style: { background: p.accent } })), h('span', { text: p.name })));
      }
    };
    const customBox = h('div');
    const custom = () => {
      clear(customBox);
      const cur = { bg: '#121212', accent: '#1DB954', grad: [], angle: 160, tint: true, panel: 100, ...(getTheme().custom || {}) };
      const base = PRESETS.find((p) => p.id === getTheme().id) || cur;
      const c = getTheme().id === 'custom' ? cur : { ...base };
      const commit = () => { setTheme({ id: 'custom', custom: c }); draw(); };
      const color = (label, key) => field(label, h('input', { type: 'color', value: c[key], oninput: (e) => { c[key] = e.target.value; commit(); } }));
      const useGrad = h('input', { type: 'checkbox', checked: (c.grad || []).length >= 2 });
      const g0 = h('input', { type: 'color', value: (c.grad || [])[0] || c.accent });
      const g1 = h('input', { type: 'color', value: (c.grad || [])[1] || c.bg });
      const g2 = h('input', { type: 'color', value: (c.grad || [])[2] || c.bg });
      const gradUpd = () => { c.grad = useGrad.checked ? [g0.value, g1.value, g2.value] : []; commit(); };
      [useGrad, g0, g1, g2].forEach((el) => el.addEventListener('input', gradUpd));
      customBox.append(
        h('div', { class: 'row gap wrap' }, color('Panel colour', 'bg'), color('Accent', 'accent')),
        h('label', { class: 'check' }, useGrad, ' Gradient backdrop'), h('div', { class: 'row gap' }, g0, g1, g2),
        field('Gradient angle', h('input', { type: 'range', min: 0, max: 360, value: c.angle, oninput: (e) => { c.angle = +e.target.value; commit(); } })),
        field('Panel opacity', h('input', { type: 'range', min: 15, max: 100, value: c.panel, oninput: (e) => { c.panel = +e.target.value; commit(); } })),
        h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: c.tint, onchange: (e) => { c.tint = e.target.checked; commit(); } }), ' Tint pages with the album cover'));
    };
    draw(); custom();

    const img = h('input', { type: 'file', accept: 'image/*', class: 'hidden' });
    img.onchange = async () => {
      try { setTheme({ image: await imageFileToDataUrl(img.files[0]) }); imgControls(); } catch (e) { toast(e.message, 'error'); }
    };
    const imgBox = h('div');
    const imgControls = () => {
      clear(imgBox);
      const cur = getTheme();
      if (!cur.image) return;
      imgBox.append(
        field('Blur', h('input', { type: 'range', min: 0, max: 40, value: cur.blur, oninput: (e) => setTheme({ blur: +e.target.value }) })),
        field('Dim', h('input', { type: 'range', min: 0, max: 90, value: cur.dim, oninput: (e) => setTheme({ dim: +e.target.value }) })),
        field('Panel opacity over the image', h('input', { type: 'range', min: 15, max: 100, value: cur.imgPanel, oninput: (e) => setTheme({ imgPanel: +e.target.value }) })),
        h('button', { class: 'btn ghost', text: 'Remove background image', onclick: () => { setTheme({ image: '' }); imgControls(); } }));
    };
    imgControls();
    return [
      section('Theme', grid),
      section('Customise', customBox),
      section('Background image', h('button', { class: 'btn', onclick: () => img.click() }, icon('import', 18), ' Choose image\u2026'), img, imgBox),
    ];
  };

  const show = (t) => {
    unsub.forEach((f) => f()); unsub = [];
    clear(tabs);
    for (const k of Object.keys(names)) tabs.append(h('button', { class: 'tab' + (k === t ? ' active' : ''), text: names[k], onclick: () => show(k) }));
    clear(pane);
    pane.append(...({ sources, integrations, appearance }[t])());
  };
  show(startTab);
}
