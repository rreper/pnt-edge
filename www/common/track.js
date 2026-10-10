// Track plot on a canvas: the solution path in local NED metres (north up), the latest point, its 1-sigma
// ellipse and, when known, the protection-level circle. Resolution-independent; redraw(history, solution).
export class Track {
  constructor(canvas) { this.c = canvas; this.ctx = canvas.getContext('2d'); this.follow = true; this.span = 200; }
  draw(h, sol) {
    const c = this.c, ctx = this.ctx, dpr = window.devicePixelRatio || 1;
    const W = c.clientWidth, H = c.clientHeight;
    if (c.width !== W * dpr || c.height !== H * dpr) { c.width = W * dpr; c.height = H * dpr; }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    const n = h && h.n ? h.n : [], e = h && h.e ? h.e : [];
    if (!n.length) { ctx.fillStyle = '#8b98a5'; ctx.fillText('no solution yet', 12, 20); return; }
    // span: follow the latest point, scale to the recent track
    let cx = e[e.length - 1], cy = n[n.length - 1], span = 50;
    const k0 = Math.max(0, n.length - 300);
    for (let i = k0; i < n.length; i++) span = Math.max(span, Math.abs(e[i] - cx) * 2.2, Math.abs(n[i] - cy) * 2.2);
    if (!this.follow) { cx = 0; cy = 0; for (let i = 0; i < n.length; i++) span = Math.max(span, Math.abs(e[i]) * 2.2, Math.abs(n[i]) * 2.2); }
    this.span = span;
    const s = Math.min(W, H) / span;
    const X = (east) => W / 2 + (east - cx) * s, Y = (north) => H / 2 - (north - cy) * s;
    // grid
    ctx.strokeStyle = 'rgba(139,152,165,.25)'; ctx.lineWidth = 1; ctx.font = '11px system-ui'; ctx.fillStyle = '#8b98a5';
    const step = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000].find((v) => v * s > 60) || 10000;
    const gx0 = Math.floor((cx - span) / step) * step, gy0 = Math.floor((cy - span) / step) * step;
    for (let g = gx0; g <= cx + span; g += step) { ctx.beginPath(); ctx.moveTo(X(g), 0); ctx.lineTo(X(g), H); ctx.stroke(); ctx.fillText(g + ' m', X(g) + 2, H - 4); }
    for (let g = gy0; g <= cy + span; g += step) { ctx.beginPath(); ctx.moveTo(0, Y(g)); ctx.lineTo(W, Y(g)); ctx.stroke(); ctx.fillText(g + ' m', 2, Y(g) - 2); }
    // path coloured by PACE
    const pace = h.pace || [];
    for (let i = 1; i < n.length; i++) {
      ctx.strokeStyle = { ALTERNATE: '#f1c40f', CONTINGENCY: '#e67e22', EMERGENCY: '#e74c3c' }[pace[i]] || '#4aa3ff';
      ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(X(e[i - 1]), Y(n[i - 1])); ctx.lineTo(X(e[i]), Y(n[i])); ctx.stroke();
    }
    // latest point, sigma ellipse, HPL circle, heading
    const lx = X(e[e.length - 1]), ly = Y(n[n.length - 1]);
    if (sol && sol.sigma_m) {
      ctx.strokeStyle = 'rgba(74,163,255,.8)'; ctx.lineWidth = 1.5; ctx.beginPath();
      ctx.ellipse(lx, ly, Math.max(2, sol.sigma_m[1] * s), Math.max(2, sol.sigma_m[0] * s), 0, 0, 2 * Math.PI); ctx.stroke();
    }
    if (sol && sol.hpl_m > 0) {
      ctx.strokeStyle = 'rgba(231,76,60,.7)'; ctx.setLineDash([4, 4]); ctx.beginPath(); ctx.arc(lx, ly, sol.hpl_m * s, 0, 2 * Math.PI); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = 'rgba(231,76,60,.9)'; ctx.fillText('HPL ' + sol.hpl_m.toFixed(1) + ' m', lx + sol.hpl_m * s + 4, ly);
    }
    if (sol && sol.heading_deg != null) {
      const a = sol.heading_deg * Math.PI / 180, L = 18;
      ctx.strokeStyle = '#e6edf3'; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(lx, ly); ctx.lineTo(lx + Math.sin(a) * L, ly - Math.cos(a) * L); ctx.stroke();
    }
    ctx.fillStyle = '#e6edf3'; ctx.beginPath(); ctx.arc(lx, ly, 4, 0, 2 * Math.PI); ctx.fill();
  }
}
