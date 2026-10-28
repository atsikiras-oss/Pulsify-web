// Small UI toolkit: DOM builder, icons, toasts, context menu, modals, formatting.

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

export function h(tag, props = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else if (k === 'html') el.innerHTML = v;                 // trusted markup only (icons)
    else if (k.startsWith('on')) el.addEventListener(k.slice(2).toLowerCase(), v);
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k === 'dataset') Object.assign(el.dataset, v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat(Infinity)) {
    if (kid == null || kid === false) continue;
    el.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
  }
  return el;
}

export const clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };

// ---------------------------------------------------------------- icons (24x24, currentColor)
const P = {
  play: 'M8 5v14l11-7z',
  pause: 'M6 5h4v14H6zM14 5h4v14h-4z',
  next: 'M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z',
  prev: 'M6 6h2v12H6zM9.5 12l8.5 6V6l-8.5 6z',
  shuffle: 'M10.59 9.17L5.41 4 4 5.41l5.17 5.17 1.42-1.41zM14.5 4l2.04 2.04L4 18.59 5.41 20 17.96 7.46 20 9.5V4h-5.5zm.33 9.41l-1.41 1.41 3.13 3.13L14.5 20H20v-5.5l-2.04 2.04-3.13-3.13z',
  repeat: 'M7 7h10v3l4-4-4-4v3H5v6h2V7zm10 10H7v-3l-4 4 4 4v-3h12v-6h-2v4z',
  volume: 'M3 9v6h4l5 5V4L7 9H3zm13.5 3A4.5 4.5 0 0 0 14 7.97v8.05c1.48-.73 2.5-2.25 2.5-4.02z',
  mute: 'M16.5 12A4.5 4.5 0 0 0 14 7.97v2.21l2.45 2.45c.03-.2.05-.41.05-.63zm2.5 0c0 .94-.2 1.82-.54 2.64l1.51 1.51A8.8 8.8 0 0 0 21 12c0-4.28-2.99-7.86-7-8.77v2.06c2.89.86 5 3.54 5 6.71zM4.27 3L3 4.27 7.73 9H3v6h4l5 5v-6.73l4.25 4.25c-.67.52-1.42.93-2.25 1.18v2.06a8.99 8.99 0 0 0 3.69-1.81L19.73 21 21 19.73l-9-9L4.27 3zM12 4L9.91 6.09 12 8.18V4z',
  search: 'M15.5 14h-.79l-.28-.27A6.47 6.47 0 0 0 16 9.5 6.5 6.5 0 1 0 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z',
  home: 'M12 3l9 8h-3v9h-5v-6h-2v6H6v-9H3l9-8z',
  library: 'M4 4h2v16H4zM8 4h2v16H8zM12.2 4.6l1.9-.5 4.3 15.5-1.9.5z',
  plus: 'M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z',
  queue: 'M3 6h14v2H3zM3 11h14v2H3zM3 16h9v2H3zM18 13v8l5-4z',
  mic: 'M12 14a3 3 0 0 0 3-3V5a3 3 0 0 0-6 0v6a3 3 0 0 0 3 3zm5-3a5 5 0 0 1-10 0H5a7 7 0 0 0 6 6.92V21h2v-3.08A7 7 0 0 0 19 11h-2z',
  more: 'M6 10a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm6 0a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm6 0a2 2 0 1 0 0 4 2 2 0 0 0 0-4z',
  back: 'M15.41 7.41L14 6l-6 6 6 6 1.41-1.41L10.83 12z',
  forward: 'M8.59 16.59L10 18l6-6-6-6-1.41 1.41L13.17 12z',
  close: 'M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z',
  settings: 'M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.49.49 0 0 0-.59-.22l-2.39.96a7.03 7.03 0 0 0-1.62-.94l-.36-2.54a.48.48 0 0 0-.48-.41h-3.84a.48.48 0 0 0-.48.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96a.48.48 0 0 0-.59.22L2.74 8.87a.48.48 0 0 0 .12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.48-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32a.49.49 0 0 0-.12-.61l-2.01-1.58zM12 15.6A3.6 3.6 0 1 1 12 8.4a3.6 3.6 0 0 1 0 7.2z',
  trash: 'M6 19a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z',
  folder: 'M10 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-2z',
  music: 'M12 3v10.55A4 4 0 1 0 14 17V7h4V3h-6z',
  import: 'M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z',
  check: 'M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z',
  user: 'M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8zm0 2c-2.67 0-8 1.34-8 4v2h16v-2c0-2.66-5.33-4-8-4z',
  edit: 'M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z',
  refresh: 'M17.65 6.35A7.96 7.96 0 0 0 12 4a8 8 0 1 0 7.73 10h-2.08A6 6 0 1 1 12 6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z',
  down: 'M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6z',
  info: 'M3 4h18v16H3V4zm12 2v12h4V6h-4z',
  grip: 'M9 4a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zm6 0a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zM9 10.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zm6 0a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zM9 17a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zm6 0a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z',
};

export function icon(name, size = 20) {
  const extra = name === 'repeat1' ? '<text x="12" y="15.2" text-anchor="middle" font-size="8" font-weight="700" fill="currentColor">1</text>' : '';
  const path = P[name === 'repeat1' ? 'repeat' : name] || '';
  return h('span', {
    class: 'ico',
    html: `<svg viewBox="0 0 24 24" width="${size}" height="${size}" fill="currentColor" aria-hidden="true"><path d="${path}"/>${extra}</svg>`,
  });
}

// ---------------------------------------------------------------- formatting
export function fmtTime(ms) {
  if (!isFinite(ms) || ms < 0) return '0:00';
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  const hr = Math.floor(m / 60);
  const ss = String(s % 60).padStart(2, '0');
  return hr > 0 ? `${hr}:${String(m % 60).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
}

export function fmtTotal(ms) {
  const m = Math.round(ms / 60000);
  if (m < 60) return `${m} min`;
  return `${Math.floor(m / 60)} hr ${m % 60} min`;
}

export const debounce = (fn, ms) => {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
};

export const plural = (n, one, many = one + 's') => `${n} ${n === 1 ? one : many}`;

// ---------------------------------------------------------------- toasts
export function toast(msg, kind = '') {
  const root = $('#toasts');
  const el = h('div', { class: 'toast ' + kind, text: msg });
  root.append(el);
  requestAnimationFrame(() => el.classList.add('show'));
  setTimeout(() => { el.classList.remove('show'); setTimeout(() => el.remove(), 250); }, kind === 'error' ? 5200 : 2600);
}

// ---------------------------------------------------------------- context menu
let closeMenuFn = null;
export function closeMenu() { if (closeMenuFn) closeMenuFn(); }

// items: [{label, icon?, onClick, danger?, disabled?} | 'sep']
export function openMenu(x, y, items) {
  closeMenu();
  const root = $('#menu-root');
  const menu = h('div', { class: 'menu', role: 'menu' });
  for (const it of items) {
    if (it === 'sep') { menu.append(h('div', { class: 'menu-sep' })); continue; }
    menu.append(h('button', {
      class: 'menu-item' + (it.danger ? ' danger' : ''), role: 'menuitem', disabled: it.disabled,
      onclick: (e) => { e.stopPropagation(); closeMenu(); it.onClick && it.onClick(); },
    }, it.icon ? icon(it.icon, 18) : h('span', { class: 'ico-gap' }), h('span', { text: it.label })));
  }
  root.append(menu);
  const r = menu.getBoundingClientRect();
  menu.style.left = Math.max(8, Math.min(x, innerWidth - r.width - 8)) + 'px';
  menu.style.top = Math.max(8, Math.min(y, innerHeight - r.height - 8)) + 'px';
  const off = (e) => { if (!menu.contains(e.target)) closeMenu(); };
  const key = (e) => { if (e.key === 'Escape') closeMenu(); };
  setTimeout(() => { document.addEventListener('mousedown', off, true); document.addEventListener('keydown', key, true); }, 0);
  closeMenuFn = () => {
    menu.remove();
    document.removeEventListener('mousedown', off, true);
    document.removeEventListener('keydown', key, true);
    closeMenuFn = null;
  };
}

// ---------------------------------------------------------------- modal
export function openModal(title, body, { wide = false, onClose } = {}) {
  const root = $('#modal-root');
  const box = h('div', { class: 'modal' + (wide ? ' wide' : ''), role: 'dialog', 'aria-label': title });
  const back = h('div', { class: 'modal-back' }, box);
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    document.removeEventListener('keydown', key, true);
    back.classList.add('closing');
    setTimeout(() => back.remove(), 170);
    onClose && onClose();
  };
  const key = (e) => { if (e.key === 'Escape') { e.stopPropagation(); close(); } };
  box.append(
    h('div', { class: 'modal-head' }, h('h2', { text: title }),
      h('button', { class: 'icon-btn', title: 'Close', onclick: close }, icon('close', 20))),
    h('div', { class: 'modal-body' }, body),
  );
  back.addEventListener('mousedown', (e) => { if (e.target === back) close(); });
  document.addEventListener('keydown', key, true);
  root.append(back);
  return { close, el: box, body: $('.modal-body', box) };
}

export function confirmBox(title, message, okLabel = 'Delete') {
  return new Promise((resolve) => {
    let done = false;
    const finish = (v) => { if (!done) { done = true; resolve(v); m.close(); } };
    const m = openModal(title, h('div', {},
      h('p', { class: 'muted', text: message }),
      h('div', { class: 'row end gap' },
        h('button', { class: 'btn ghost', text: 'Cancel', onclick: () => finish(false) }),
        h('button', { class: 'btn danger', text: okLabel, onclick: () => finish(true) }))),
      { onClose: () => { if (!done) { done = true; resolve(false); } } });
  });
}

export function promptBox(title, label, value = '', okLabel = 'Save') {
  return new Promise((resolve) => {
    let done = false;
    const input = h('input', { class: 'text-input', type: 'text', value, maxlength: 120, placeholder: label });
    const finish = (v) => { if (!done) { done = true; resolve(v); m.close(); } };
    input.addEventListener('keydown', (e) => { if (e.key === 'Enter') finish(input.value.trim()); });
    const m = openModal(title, h('div', {},
      input,
      h('div', { class: 'row end gap', style: { marginTop: '16px' } },
        h('button', { class: 'btn ghost', text: 'Cancel', onclick: () => finish(null) }),
        h('button', { class: 'btn primary', text: okLabel, onclick: () => finish(input.value.trim()) }))),
      { onClose: () => { if (!done) { done = true; resolve(null); } } });
    setTimeout(() => { input.focus(); input.select(); }, 30);
  });
}

// yt-dlp errors are long and repeat themselves; keep one readable sentence.
export function tidyError(msg = '') {
  let s = String(msg).replace(/\s+/g, ' ').trim();
  const parts = s.split(/ERROR:\s*/i).map((x) => x.trim()).filter(Boolean);
  s = parts[parts.length - 1] || s;
  s = s.replace(/\s*See https?:\/\/\S+ for more info\.?/i, '').trim();
  if (/cookie/i.test(s)) s = 'Couldn\u2019t read the browser\u2019s cookies \u2013 close the browser or turn cookies off in Settings \u2192 Sources.';
  return s.length > 220 ? s.slice(0, 217) + '\u2026' : s;
}
