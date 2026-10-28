// Themes: the same 14 presets as the desktop app, custom colours/gradient, and a background image.
import { api } from './api.js';

export const PRESETS = [
  { id: 'pulsify',  name: 'Pulsify Dark', bg: '#121212', accent: '#1DB954', grad: [], angle: 160, tint: true,  panel: 100 },
  { id: 'amoled',   name: 'AMOLED Black', bg: '#000000', accent: '#2EE66B', grad: [], angle: 160, tint: false, panel: 100 },
  { id: 'midnight', name: 'Midnight',     bg: '#0b1020', accent: '#5B8CFF', grad: [], angle: 160, tint: true,  panel: 100 },
  { id: 'graphite', name: 'Graphite',     bg: '#1b1d21', accent: '#f2b84b', grad: [], angle: 160, tint: true,  panel: 100 },
  { id: 'aurora',   name: 'Aurora',       bg: '#0a1a1c', accent: '#3EE6B0', grad: ['#0b5d57', '#1d8a6b', '#3a2a8f'], angle: 135, tint: false, panel: 58 },
  { id: 'sunset',   name: 'Sunset',       bg: '#1c0f1a', accent: '#FF8A5B', grad: ['#ff7e5f', '#d4407c', '#5b2a86'], angle: 150, tint: false, panel: 56 },
  { id: 'ocean',    name: 'Ocean',        bg: '#08141f', accent: '#36C5F0', grad: ['#0a4d80', '#1b8fd1', '#0b1f3a'], angle: 160, tint: false, panel: 58 },
  { id: 'rose',     name: 'Rose',         bg: '#1f0d14', accent: '#FF5C8A', grad: ['#ff5c8a', '#b0245a', '#3b0d2a'], angle: 145, tint: false, panel: 56 },
  { id: 'neon',     name: 'Neon Nights',  bg: '#0e0a1f', accent: '#B86BFF', grad: ['#3a0ca3', '#7209b7', '#f72585'], angle: 135, tint: false, panel: 56 },
  { id: 'forest',   name: 'Forest',       bg: '#0c150f', accent: '#7BD88F', grad: ['#1b5e3b', '#2d8a5f', '#081c15'], angle: 165, tint: false, panel: 58 },
  { id: 'ember',    name: 'Ember',        bg: '#160d0a', accent: '#FFB020', grad: ['#8a2b0b', '#d9541e', '#1a0a05'], angle: 155, tint: false, panel: 56 },
  { id: 'daylight', name: 'Daylight',     bg: '#f4f5f7', accent: '#1DB954', grad: [], angle: 160, tint: true,  panel: 100 },
  { id: 'peach',    name: 'Peach Fizz',   bg: '#fff4ee', accent: '#FF6B4A', grad: ['#ffd2bd', '#ffb8d1', '#c9d4ff'], angle: 150, tint: false, panel: 62 },
  { id: 'mint',     name: 'Mint Cream',   bg: '#f1faf6', accent: '#12a37a', grad: ['#c6f1de', '#d8f0ff', '#f4f1ff'], angle: 140, tint: false, panel: 64 },
];

export const DEFAULT_THEME = { id: 'pulsify', custom: null, image: '', blur: 12, dim: 45, imgPanel: 62 };

// ---------------------------------------------------------------- colour maths
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
export function hexToRgb(hex) {
  let h = String(hex || '').replace('#', '').trim();
  if (h.length === 3) h = [...h].map((c) => c + c).join('');
  if (!/^[0-9a-fA-F]{6}$/.test(h)) return [18, 18, 18];
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}
export const rgbToHex = ([r, g, b]) => '#' + [r, g, b].map((v) => clamp(Math.round(v), 0, 255).toString(16).padStart(2, '0')).join('');
const lum = ([r, g, b]) => (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);

export function resolveTheme(state) {
  if (state.id === 'custom' && state.custom) return { ...state.custom, id: 'custom', name: 'Custom' };
  return PRESETS.find((p) => p.id === state.id) || PRESETS[0];
}

// ---------------------------------------------------------------- apply
export function applyTheme(state) {
  const t = resolveTheme(state);
  const bg = hexToRgb(t.bg), accent = hexToRgb(t.accent);
  const light = lum(bg) > 0.55;
  const fg = light ? [22, 22, 22] : [255, 255, 255];
  const hasImage = !!state.image;
  const grad = (t.grad || []).filter(Boolean);
  const hasBackdrop = hasImage || grad.length >= 2;
  const panel = clamp((hasImage ? state.imgPanel : t.panel) / 100, 0.15, 1);
  const onAccent = lum(accent) > 0.5 ? '#0a0a0a' : '#ffffff';
  const grey = (l) => rgbToHex(mix(bg, fg, l));
  const canvas = light ? rgbToHex(mix(bg, [0, 0, 0], 0.07)) : '#000000';

  const root = document.documentElement;
  const set = (k, v) => root.style.setProperty(k, v);
  set('--bg', rgbToHex(bg));
  set('--panel', `rgba(${bg.join(',')},${hasBackdrop ? panel : 1})`);
  set('--panel-solid', rgbToHex(bg));
  set('--raise', `rgba(${fg.join(',')},0.07)`);
  set('--raise-2', `rgba(${fg.join(',')},0.12)`);
  set('--line', `rgba(${fg.join(',')},0.10)`);
  set('--fg', rgbToHex(fg));
  set('--sub', grey(0.64));
  set('--faint', grey(0.4));
  set('--accent', rgbToHex(accent));
  set('--accent-hover', rgbToHex(mix(accent, [255, 255, 255], 0.12)));
  set('--accent-soft', `rgba(${accent.join(',')},0.18)`);
  set('--on-accent', onAccent);
  set('--canvas', canvas);
  set('--shadow', light ? 'rgba(0,0,0,.18)' : 'rgba(0,0,0,.55)');
  root.dataset.scheme = light ? 'light' : 'dark';
  root.dataset.tint = t.tint ? '1' : '0';
  root.classList.toggle('has-backdrop', hasBackdrop);

  const bgEl = document.getElementById('bg');
  if (!bgEl) return;
  if (hasImage) {
    bgEl.style.backgroundImage = `url("${state.image}")`;
    bgEl.style.backgroundSize = 'cover';
    bgEl.style.backgroundPosition = 'center';
    bgEl.style.filter = `blur(${clamp(state.blur, 0, 40)}px)`;
    bgEl.style.transform = 'scale(1.08)';
    bgEl.style.setProperty('--dim', String(clamp(state.dim, 0, 90) / 100));
    bgEl.classList.add('dimmed');
    bgEl.classList.remove('grad');
  } else if (grad.length >= 2) {
    bgEl.style.backgroundImage = `linear-gradient(${(t.angle ?? 160) + 90}deg, ${grad.join(', ')})`;
    bgEl.style.backgroundSize = '180% 180%';
    bgEl.classList.add('grad');
    bgEl.style.filter = '';
    bgEl.style.transform = '';
    bgEl.classList.remove('dimmed');
  } else {
    bgEl.classList.remove('grad');
    bgEl.style.backgroundImage = 'none';
    bgEl.style.filter = '';
    bgEl.style.transform = '';
    bgEl.classList.remove('dimmed');
  }
}

// Page tint from cover art (only readable for same-origin covers; others keep the accent-based tint).
export async function tintFromImage(url) {
  if (!url || !url.startsWith('/')) return null;
  try {
    const img = new Image();
    img.src = url;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = c.height = 24;
    const g = c.getContext('2d', { willReadFrequently: true });
    g.drawImage(img, 0, 0, 24, 24);
    const d = g.getImageData(0, 0, 24, 24).data;
    let r = 0, gg = 0, b = 0, n = 0;
    for (let i = 0; i < d.length; i += 4) { r += d[i]; gg += d[i + 1]; b += d[i + 2]; n++; }
    const avg = [r / n, gg / n, b / n];
    const mx = Math.max(...avg), mn = Math.min(...avg);
    const v = clamp(mx / 255, 0.32, 0.62);
    const k = mx > 0 ? v * 255 / mx : 1;
    const sat = mx > 0 ? (mx - mn) / mx : 0;
    const boosted = avg.map((c2) => mx - (mx - c2) * (1 + Math.min(0.25, 0.25 * (1 - sat))));
    return rgbToHex(boosted.map((c2) => c2 * k));
  } catch { return null; }
}

export function setPageTint(hex) {
  const root = document.documentElement;
  if (hex) root.style.setProperty('--tint', hex);
  else root.style.removeProperty('--tint');
}

// ---------------------------------------------------------------- state + persistence
let state = { ...DEFAULT_THEME };
export const getTheme = () => state;

export function setTheme(next, { save = true } = {}) {
  state = { ...state, ...next };
  applyTheme(state);
  if (save) persistSoon();
}

let timer = null;
function persistSoon() {
  clearTimeout(timer);
  timer = setTimeout(() => { savePrefs({ theme: state }); }, 400);
}

// ---- prefs blob shared by theme / player (stored by the server, mirrored in localStorage for instant paint)
let prefs = {};
export const getPrefs = () => prefs;

export async function loadPrefs() {
  try { prefs = JSON.parse(localStorage.getItem('pulsify.prefs') || '{}'); } catch { prefs = {}; }
  try {
    const remote = await api.get('/api/prefs');
    if (remote && typeof remote === 'object' && Object.keys(remote).length) prefs = remote;
  } catch { /* server offline: keep the local copy */ }
  state = { ...DEFAULT_THEME, ...(prefs.theme || {}) };
  applyTheme(state);
  return prefs;
}

let saveT = null;
export function savePrefs(patch) {
  prefs = { ...prefs, ...patch };
  try { localStorage.setItem('pulsify.prefs', JSON.stringify(prefs)); } catch { /* quota (big background image) */ }
  clearTimeout(saveT);
  saveT = setTimeout(() => { api.post('/api/prefs', prefs).catch(() => {}); }, 300);
}

// Shrinks a picked image so it fits comfortably in the prefs store.
export function imageFileToDataUrl(file, maxSide = 1920, quality = 0.82) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      const k = Math.min(1, maxSide / Math.max(img.width, img.height));
      const c = document.createElement('canvas');
      c.width = Math.round(img.width * k);
      c.height = Math.round(img.height * k);
      c.getContext('2d').drawImage(img, 0, 0, c.width, c.height);
      URL.revokeObjectURL(url);
      resolve(c.toDataURL('image/jpeg', quality));
    };
    img.onerror = () => { URL.revokeObjectURL(url); reject(new Error('That file isn\u2019t an image.')); };
    img.src = url;
  });
}
