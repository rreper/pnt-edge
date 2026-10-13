import { getJson, post, subscribe, fmt, healthClass, paceClass } from '/static/api.js';
import { Track } from '/static/track.js';
const $ = (id) => document.getElementById(id);
let tab = 'overview', status = null, solution = null, history = null, lastHistory = 0, lastRates = {}, lastRateT = 0;
const track = new Track($('track'));
$('follow').onchange = () => { track.follow = $('follow').checked; };
// tabs
for (const a of document.querySelectorAll('#nav a')) a.onclick = () => { tab = a.dataset.tab; for (const b of document.querySelectorAll('#nav a')) b.classList.toggle('active', b === a);
  for (const s of document.querySelectorAll('main > section')) s.classList.toggle('hidden', s.id !== 'tab-' + tab); refresh(); };
// plots
const plotOpts = (title, series) => ({ width: 600, height: 220, title, series: [{}, ...series], axes: [{ values: (u, v) => v.map((x) => new Date(x * 1000).toISOString().slice(11, 19)) }, {}] });
let pSigma = null, pMotion = null;
function plots(h) {
  if (!h || !h.t || !h.t.length) return;
  const w = $('p-sigma').clientWidth || 600;
  const d1 = [h.t, h.sigma_h, h.hpl.map((x) => (x > 0 ? x : null))];
  const d2 = [h.t, h.speed, h.heading_deg.map((x) => (x < 0 ? x + 360 : x))];
  if (!pSigma) { pSigma = new uPlot({ ...plotOpts('', [{ label: '1σ horizontal', stroke: '#4aa3ff' }, { label: 'HPL', stroke: '#e74c3c' }]), width: w }, d1, $('p-sigma'));
    pMotion = new uPlot({ ...plotOpts('', [{ label: 'speed', stroke: '#2ecc71' }, { label: 'heading', stroke: '#f1c40f', scale: 'deg' }]), width: w, scales: { deg: { range: [0, 360] } } }, d2, $('p-motion')); }
  else { pSigma.setData(d1); pMotion.setData(d2); }
}
function refresh() {
  if (!status) return;
  $('health').textContent = status.health || '—'; $('health').className = 'badge ' + healthClass(status.health);
  const integ = status.integrity && status.integrity.pace ? status.integrity : null;
  if (integ) { $('pace').textContent = integ.pace; $('pace').className = 'badge ' + paceClass(integ.pace); } else $('pace').className = 'badge hidden';
  if (tab === 'overview') {
    $('o-health').textContent = status.health; $('o-uptime').textContent = fmt.uptime(status.uptime_sec); $('o-solutions').textContent = status.solutions;
    $('o-age').textContent = fmt.age(status.last_solution_age_sec); $('o-error').textContent = status.filter_error ? 'YES' : 'no';
    if (solution && solution.t) { $('o-lat').textContent = fmt.deg(solution.lat_deg) + '°'; $('o-lon').textContent = fmt.deg(solution.lon_deg) + '°'; $('o-alt').textContent = fmt.num(solution.alt_m, 1) + ' m';
      $('o-hdg').textContent = fmt.num(solution.heading_deg < 0 ? solution.heading_deg + 360 : solution.heading_deg, 1) + '°'; $('o-spd').textContent = fmt.num(solution.speed_mps, 2) + ' m/s';
      $('o-sig').textContent = solution.sigma_m.map((x) => x.toFixed(2)).join(' / ') + ' m'; }
    if (integ) { $('o-pace').textContent = integ.pace; $('o-hpl').textContent = fmt.num(integ.hpl_m, 1) + ' / ' + fmt.num(integ.hal_m, 0) + ' m'; $('o-excl').textContent = integ.excluded.length ? integ.excluded.join(', ') : 'none';
      $('o-alarm').textContent = integ.alarm ? 'ACTIVE' : 'no'; $('o-fd').textContent = integ.fault_detection_available ? 'available' : 'not available'; }
    getJson('/api/v1/events?n=12').then((ev) => { $('o-events').innerHTML = ev.reverse().map((e) => `<tr><td>${fmt.time(e.t)}</td><td>${e.kind}</td><td>${e.detail}</td></tr>`).join(''); }).catch(() => {});
  }
  if (tab === 'inputs' && status.input) {
    $('i-pushed').textContent = status.input.pushed; $('i-fail').textContent = status.input.decode_failures; $('i-tov').textContent = fmt.time(status.input.last_tov_s);
    const now = Date.now() / 1000, rows = [];
    for (const [ch, n] of Object.entries(status.input.per_channel || {})) { const prev = lastRates[ch]; const rate = prev && now > lastRateT ? ((n - prev) / (now - lastRateT)) : null; rows.push(`<tr><td>${ch}</td><td class="num">${n}</td><td class="num">${rate == null ? '—' : rate.toFixed(1)}</td></tr>`); }
    lastRates = { ...(status.input.per_channel || {}) }; lastRateT = now; $('i-channels').innerHTML = rows.join('');
    const devs = status.input.devices || []; $('i-devices-panel').style.display = devs.length ? '' : 'none';
    $('i-devices').innerHTML = devs.map((d) => `<tr><td>${d.name} <span class="muted">(${d.driver})</span></td><td>${d.port}</td><td><span class="badge ${d.state === 'streaming' ? 'ok' : d.state === 'open' || d.state === 'opening' ? 'warn' : 'bad'}">${d.state}</span>${d.detail ? ' <span class="muted">' + d.detail + '</span>' : ''}</td><td class="num">${fmt.num(d.rate_hz, 1)}</td><td class="num">${d.frames}</td><td class="num">${d.errors}</td><td class="num">${d.last_frame_age_s < 0 ? '—' : fmt.num(d.last_frame_age_s, 1)}</td><td class="num">${d.clock_offset_s == null ? '—' : fmt.num(d.clock_offset_s, 3)}</td></tr>`).join('');
    const g = status.gating || {}, procs = new Set(Object.keys(g).map((k) => k.replace(/_(accepted|rejected|last_chi2)$/, '')));
    $('i-gating').innerHTML = [...procs].map((p) => `<tr><td>${p}</td><td class="num">${g[p + '_accepted'] ?? '—'}</td><td class="num">${g[p + '_rejected'] ?? '—'}</td><td class="num">${fmt.num(g[p + '_last_chi2'], 2)}</td></tr>`).join('');
  }
  if (tab === 'solution') { const now = Date.now(); if (now - lastHistory > 1000) { lastHistory = now; getJson('/api/v1/history?seconds=' + $('window').value + '&points=2000').then((h) => { history = h; track.draw(h, solution); plots(h); }).catch(() => {}); } else track.draw(history, solution); }
  if (tab === 'integrity') {
    const rows = Object.entries(integ || {}).filter(([k]) => k !== 'excluded').map(([k, v]) => `<tr><td>${k}</td><td class="num">${typeof v === 'number' ? fmt.num(v, 3) : JSON.stringify(v)}</td></tr>`);
    $('g-status').innerHTML = rows.join('') || '<tr><td class="muted">no integrity plugin linked</td></tr>';
    getJson('/api/v1/events?n=200').then((ev) => { $('g-events').innerHTML = ev.reverse().filter((e) => e.kind !== 'CONTROL').map((e) => `<tr><td>${fmt.time(e.t)}</td><td>${e.kind}</td><td>${e.detail}</td></tr>`).join(''); }).catch(() => {});
  }
  if (tab === 'outputs') { $('n-sent').textContent = status.nmea_sentences; $('n-tcp').textContent = status.tcp_clients; getJson('/api/v1/config').then((c) => { const n = c.edge.nmea || {}; $('n-udp').textContent = (n.udp_targets || []).join(', ') || 'none'; $('n-rate').textContent = n.rate_hz + ' Hz'; $('n-sentences').textContent = (n.sentences || []).join(' ');
      if (document.activeElement.id !== 'f-rate') $('f-rate').value = n.rate_hz; if (document.activeElement.id !== 'f-sentences') $('f-sentences').value = (n.sentences || []).join(' '); if (document.activeElement.id !== 'f-udp') $('f-udp').value = (n.udp_targets || []).join(' '); }).catch(() => {}); }
  if (tab === 'software' && Date.now() - swLast > 3000) loadSoftware().catch(() => {});
  if (tab === 'log') getJson('/api/v1/log?n=400&level=' + $('log-level').value).then((lines) => { const el = $('log'); const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 20; el.innerHTML = lines.map((l) => `<div class="${l.level}">${fmt.time(l.t)} [${l.source}] [${l.level}] ${l.message.replace(/</g, '&lt;')}</div>`).join(''); if (atBottom) el.scrollTop = el.scrollHeight; }).catch(() => {});
}
// software updates
let swLast = 0, swRestarting = false;
const esc = (s) => String(s ?? '').replace(/</g, '&lt;');
function showSoftware(s) {
  $('s-version').textContent = s.running.version; $('s-cobra').textContent = s.running.cobra; $('s-exe').textContent = s.running.executable; $('s-www').textContent = s.running.www;
  $('s-pending').textContent = s.restart_pending ? 'yes, restart the application to run the installed version' : 'no';
  const p = s.staged; $('s-staged').classList.toggle('hidden', !p);
  if (p) { $('s-st-file').textContent = p.file; $('s-st-size').textContent = (p.size / 1e6).toFixed(1) + ' MB'; $('s-st-version').textContent = p.version || '—';
    $('s-st-sha').textContent = p.sha256.slice(0, 16) + '… ' + (p.sha256_expected ? (p.sha256_expected === p.sha256 ? '(matches the checksum file)' : '(DOES NOT MATCH the checksum file)') : '(no checksum file given)');
    $('s-st-check').innerHTML = p.applicable ? '<span class="badge ok">ready to apply</span>' : '<span class="badge bad">not applicable</span> ' + esc(p.problem);
    $('s-st-by').textContent = fmt.time(p.uploaded_t) + ' by ' + (p.uploaded_by || '').replace('user:', ''); $('s-apply').disabled = $('s-apply-only').disabled = !p.applicable; }
  const b = s.backup; $('s-backup-panel').style.display = b ? '' : 'none';
  if (b) { $('s-prev').textContent = b.version + (b.cobra ? ' (Cobra ' + b.cobra + ')' : ''); $('s-prev-t').textContent = fmt.time(b.t); $('s-prev-by').textContent = (b.by || '').replace('user:', ''); }
  $('s-history').innerHTML = (s.history || []).slice().reverse().map((h) => `<tr><td>${fmt.time(h.t)}</td><td>${esc(h.action)}</td><td>${esc((h.who || '').replace('user:', ''))}</td><td>${esc(h.from)}</td><td>${esc(h.to)}</td><td>${esc(h.result)}</td></tr>`).join('');
}
async function loadSoftware() { swLast = Date.now(); showSoftware(await getJson('/api/v1/software')); }
document.querySelector('#nav a[data-tab=software]').addEventListener('click', () => loadSoftware().catch((e) => { $('s-result').textContent = e.message; }));
$('s-upload').onclick = async () => {
  const f = $('s-file').files[0]; if (!f) { $('s-upload-result').textContent = 'choose the package file first'; return; }
  const fd = new FormData(); fd.append('package', f, f.name); if ($('s-sha').files[0]) fd.append('sha256', $('s-sha').files[0], $('s-sha').files[0].name);
  $('s-upload-result').textContent = 'uploading ' + (f.size / 1e6).toFixed(1) + ' MB…';
  try { const r = await fetch('/api/v1/software/upload', { method: 'POST', credentials: 'same-origin', body: fd }); const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.error || r.status); showSoftware(j); $('s-upload-result').textContent = j.staged && j.staged.applicable ? 'staged, ready to apply' : 'staged, but not applicable'; }
  catch (e) { $('s-upload-result').textContent = 'failed: ' + e.message; }
};
const swAction = (path, body, el, msg) => post(path, body).then((j) => { showSoftware(j); if (j.restarting) { swRestarting = true; el.textContent = msg + ' — restarting, the page reconnects and asks you to sign in again'; } else el.textContent = msg; }).catch((e) => { el.textContent = 'failed: ' + e.message; });
$('s-apply').onclick = () => { if (confirm('Install the staged package and restart the application now?')) swAction('/api/v1/software/apply', { restart: true }, $('s-result'), 'installed'); };
$('s-apply-only').onclick = () => { if (confirm('Install the staged package? The running process keeps the old version until the next restart.')) swAction('/api/v1/software/apply', { restart: false }, $('s-result'), 'installed, restart pending'); };
$('s-discard').onclick = () => swAction('/api/v1/software/discard', {}, $('s-result'), 'discarded');
$('s-rollback').onclick = () => { if (confirm('Put the previous version back and restart the application now?')) swAction('/api/v1/software/rollback', { restart: true }, $('s-rb-result'), 'rolled back'); };
let cfgLoaded = false;
async function loadConfig() { const c = await getJson('/api/v1/config'); if (document.activeElement.id !== 'cfg-edge') $('cfg-edge').value = JSON.stringify(c.edge, null, 2); $('cfg-filter').value = JSON.stringify(c.filter, null, 2); cfgLoaded = true; }
document.querySelector('#nav a[data-tab=config]').addEventListener('click', () => { if (!cfgLoaded) loadConfig().catch((e) => { $('cfg-result').textContent = e.message; }); });
subscribe((u) => { status = u.status; solution = u.solution; refresh(); }, (s) => { $('link').textContent = s === 'live' ? 'live' : 'reconnecting…'; });
getJson('/api/v1/whoami').then((w) => { $('who').textContent = w.who.replace('user:', ''); }).catch(() => {});
getJson('/api/v1/version').then((v) => { $('o-version').textContent = v.pnt_edge; }).catch(() => {});
$('log-level').onchange = refresh;
const say = (el, p) => p.then((r) => { el.textContent = 'ok: ' + (r.queued || 'saved'); }).catch((e) => { el.textContent = 'failed: ' + e.message; });
$('apply-nmea').onclick = () => say($('cmd-result'), post('/api/v1/control/nmea', { rate_hz: Number($('f-rate').value), sentences: $('f-sentences').value.split(/\s+/).filter(Boolean), udp_targets: $('f-udp').value.split(/\s+/).filter(Boolean) }));
$('apply-hdg').onclick = () => say($('cmd-result'), post('/api/v1/control/heading', { deg: Number($('f-hdg').value), sigma_deg: Number($('f-hdgs').value) }));
$('apply-la').onclick = () => say($('cmd-result'), post('/api/v1/control/leverarm', { label: $('f-la').value, x: Number($('f-lax').value), y: Number($('f-lay').value), z: Number($('f-laz').value) }));
$('reset-filter').onclick = () => { if (confirm('Reset the navigation filter?')) say($('cmd-result'), post('/api/v1/control/reset', { what: 'filter' })); };
$('reset-app').onclick = () => { if (confirm('Restart the pnt-edge application? The service manager will start it again.')) say($('cmd-result'), post('/api/v1/control/reset', { what: 'app' })); };
$('save-edge').onclick = () => { let j; try { j = JSON.parse($('cfg-edge').value); } catch (e) { $('cfg-result').textContent = 'invalid JSON: ' + e.message; return; } say($('cfg-result'), post('/api/v1/config', j, 'PUT')); };
