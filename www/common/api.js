// Small client for the pnt-edge API v1: fetch helpers and a reconnecting SSE subscription.
export async function getJson(path) {
  const r = await fetch(path, { credentials: 'same-origin' });
  if (r.status === 401) { if (location.pathname !== '/login') location.href = '/login'; throw new Error('unauthorized'); }
  if (!r.ok) throw new Error(path + ': ' + r.status);
  return r.json();
}
export async function post(path, body, method = 'POST') {
  const r = await fetch(path, { method, credentials: 'same-origin', headers: { 'Content-Type': 'application/json' }, body: body == null ? undefined : JSON.stringify(body) });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || (path + ': ' + r.status));
  return j;
}
/// Calls onUpdate({status, solution}) on every server push; reconnects on failure; onState('live'|'reconnecting').
export function subscribe(onUpdate, onState) {
  let es = null, timer = null;
  const open = () => {
    es = new EventSource('/api/v1/stream', { withCredentials: true });
    es.addEventListener('update', (e) => { try { onUpdate(JSON.parse(e.data)); } catch (err) { console.error(err); } });
    es.onopen = () => onState && onState('live');
    es.onerror = () => { onState && onState('reconnecting'); es.close(); clearTimeout(timer); timer = setTimeout(open, 2000); };
  };
  open();
  return () => { clearTimeout(timer); es && es.close(); };
}
export const fmt = {
  num: (x, d = 2) => (x == null || Number.isNaN(x) ? '—' : Number(x).toFixed(d)),
  deg: (rad_or_deg) => (rad_or_deg == null ? '—' : Number(rad_or_deg).toFixed(5)),
  age: (s) => (s == null || s < 0 ? 'none' : s < 60 ? s.toFixed(1) + ' s' : (s / 60).toFixed(1) + ' min'),
  time: (s) => (s ? new Date(s * 1000).toISOString().replace('T', ' ').slice(0, 19) : '—'),
  uptime: (s) => { s = Math.floor(s || 0); const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60); return `${h}h ${m}m ${s % 60}s`; },
};
export function healthClass(h) {
  if (!h) return '';
  if (h === 'ok') return 'ok';
  if (h === 'aligning' || h === 'alternate' || h === 'starting') return 'warn';
  return 'bad';
}
export function paceClass(p) {
  return { PRIMARY: 'ok', ALTERNATE: 'warn', CONTINGENCY: 'warn', EMERGENCY: 'bad' }[p] || '';
}
