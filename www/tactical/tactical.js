import { getJson, post, subscribe, fmt, healthClass, paceClass } from '/static/api.js';
import { Track } from '/static/track.js';
const $ = (id) => document.getElementById(id);
const track = new Track($('track'));
let history = null, lastHistory = 0;
function render({ status, solution }) {
  if (status && status.health) { $('health').textContent = status.health; $('health').className = 'badge ' + healthClass(status.health); }
  const integ = status && status.integrity && status.integrity.pace ? status.integrity : null;
  if (integ) { $('pace').textContent = integ.pace; $('pace').className = 'badge ' + paceClass(integ.pace); $('hal').textContent = fmt.num(integ.hal_m, 0) + ' m';
    $('excluded').textContent = integ.excluded && integ.excluded.length ? integ.excluded.join(', ') : 'none'; $('alarm').textContent = integ.alarm ? 'ACTIVE' : 'no'; }
  else { $('pace').className = 'badge hidden'; }
  if (status) { $('age').textContent = fmt.age(status.last_solution_age_sec); const q = status.last_solution ? (integ ? ({PRIMARY:1,ALTERNATE:6,CONTINGENCY:6,EMERGENCY:0}[integ.pace]) : 1) : 0; $('fix').textContent = ['invalid','GPS','DGPS','','RTK','RTK float','estimated'][q] || q; }
  if (solution && solution.t) {
    $('lat').textContent = fmt.deg(solution.lat_deg) + '°'; $('lon').textContent = fmt.deg(solution.lon_deg) + '°'; $('alt').textContent = fmt.num(solution.alt_m, 1) + ' m HAE';
    $('heading').innerHTML = fmt.num(solution.heading_deg < 0 ? solution.heading_deg + 360 : solution.heading_deg, 1) + '<span class="unit">° true</span>';
    $('speed').innerHTML = fmt.num(solution.speed_mps * 1.943844, 1) + '<span class="unit">kn</span> <span class="unit">' + fmt.num(solution.speed_mps, 1) + ' m/s</span>';
    $('sigh').textContent = fmt.num(Math.hypot(solution.sigma_m[0], solution.sigma_m[1]), 2) + ' m';
    $('hpl').innerHTML = (solution.hpl_m > 0 ? fmt.num(solution.hpl_m, 1) : '—') + '<span class="unit">m HPL</span>';
    const sl = status && status.last_solution ? status.last_solution.vel_ned_mps : null;
    $('vel').textContent = sl ? sl.map((v) => v.toFixed(2)).join(' / ') + ' m/s' : '—';
  }
  if (status && status.input && status.input.per_channel) {
    const el = $('sources'); el.innerHTML = '';
    for (const [ch, n] of Object.entries(status.input.per_channel)) { const dt = document.createElement('dt'); dt.textContent = ch.split('/').slice(-2).join('/'); const dd = document.createElement('dd'); dd.textContent = n; el.append(dt, dd); }
  }
  const now = Date.now();
  if (now - lastHistory > 1000) { lastHistory = now; getJson('/api/v1/history?seconds=1200&points=1500').then((h) => { history = h; track.draw(history, solution); }).catch(() => {}); }
  else track.draw(history, solution);
}
subscribe(render, (s) => { $('link').textContent = s === 'live' ? 'live' : 'reconnecting…'; });
setInterval(() => { $('clock').textContent = new Date().toISOString().replace('T', ' ').slice(0, 19) + ' UTC'; }, 1000);
$('reset').onclick = async () => {
  if (!confirm('Reset the navigation filter? The solution will be unavailable while it re-aligns.')) return;
  $('reset').disabled = true;
  try { await post('/api/v1/control/reset', { what: 'filter' }); $('lastcmd').textContent = 'reset filter at ' + new Date().toISOString().slice(11, 19) + ' UTC'; }
  catch (e) { $('lastcmd').textContent = 'failed: ' + e.message; }
  setTimeout(() => { $('reset').disabled = false; }, 5000);
};
