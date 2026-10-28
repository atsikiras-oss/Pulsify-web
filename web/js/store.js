// Shared client-side data: playlists, history, library, account state, plus the actions that change them.
import { api, bus, toEntry, trackKey, ensurePlayable } from './api.js';
import { toast, plural } from './ui.js';

export const store = {
  playlists: [],
  recent: [],
  library: { folders: [], tracks: [], scanning: false },
  google: { hasCredentials: false, signedIn: false },
  discord: { status: '', connected: false },
  online: true,
};

function applyPlaylists(d) {
  if (!d) return;
  if (d.playlists) store.playlists = d.playlists;
  if (d.recent) store.recent = d.recent;
  bus.emit('store:playlists');
}

export async function refreshPlaylists() { applyPlaylists(await api.get('/api/playlists')); }
export async function refreshLibrary() { store.library = await api.get('/api/library'); bus.emit('store:library'); }
export async function refreshGoogle() { store.google = await api.get('/api/google'); bus.emit('store:google'); }

export const findPlaylist = (id) => store.playlists.find((p) => p.id === id);

export async function createPlaylist(name, entries = [], source = '') {
  const r = await api.post('/api/playlists', { name, entries, source });
  await refreshPlaylists();
  return r;
}

export async function renamePlaylist(id, name) { applyPlaylists(await api.post(`/api/playlists/${id}/rename`, { name })); }
export async function deletePlaylist(id) { applyPlaylists(await api.del(`/api/playlists/${id}`)); }
export async function removeFromPlaylist(id, row) { applyPlaylists(await api.post(`/api/playlists/${id}/remove`, { row })); }
export async function moveInPlaylist(id, from, to) { applyPlaylists(await api.post(`/api/playlists/${id}/move`, { from, to })); }

// Adds songs (matching charts / imported rows to YouTube first). Returns how many were new.
export async function addToPlaylist(id, tracks) {
  const pending = tracks.filter((t) => t.kind === 'pending');
  if (pending.length) toast(pending.length === 1 ? 'Finding the song on YouTube\u2026' : `Matching ${pending.length} songs on YouTube\u2026`);
  const ready = [];
  for (const t of tracks) {
    try { ready.push(await ensurePlayable(t)); } catch (e) { toast(e.message, 'error'); }
  }
  if (!ready.length) return 0;
  const r = await api.post(`/api/playlists/${id}/add`, { entries: ready.map(toEntry) });
  applyPlaylists(r);
  const pl = findPlaylist(id);
  toast(r.added ? `Added ${plural(r.added, 'song')} to \u201c${pl ? pl.name : 'playlist'}\u201d` : 'Already in that playlist');
  return r.added;
}

export const keysInPlaylist = (id) => new Set(((findPlaylist(id) || {}).entries || []).map((e) => e.online ? 'yt:' + e.videoId : 'f:' + e.path));
export { trackKey };
