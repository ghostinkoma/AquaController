/* ダミー履歴をブラウザ内で生成（gen_dummy.py 相当・外部ファイル不要）。
   形状: window.AQUA_DUMMY = { f|m|h: {tier,step,base,water[],air[],press[],rpm[],airflow[],fanOn[]} } */
(function () {
  // --- 決定論的 PRNG (mulberry32) ---
  let _s = 42 >>> 0;
  function rnd() { _s |= 0; _s = (_s + 0x6D2B79F5) | 0; let t = Math.imul(_s ^ (_s >>> 15), 1 | _s); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }
  function gauss(mu, sd) { let u = 0, v = 0; while (u === 0) u = rnd(); while (v === 0) v = rnd(); return mu + sd * Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
  function uni(a, b) { return a + (b - a) * rnd(); }

  const TAU = Math.PI * 2, NOW = Math.floor(Date.now() / 1000);
  const FAN_RPM_MAX = 2200, FAN_AIRFLOW_K = 0.013;
  const CURVE = [[26.0, 0], [27.0, 40], [28.5, 70], [30.0, 100]];
  function dutyFromTemp(t) {
    if (t <= CURVE[0][0]) return CURVE[0][1];
    if (t >= CURVE[CURVE.length - 1][0]) return CURVE[CURVE.length - 1][1];
    for (let i = 0; i < CURVE.length - 1; i++) {
      const [t0, d0] = CURVE[i], [t1, d1] = CURVE[i + 1];
      if (t0 <= t && t <= t1) { const f = (t - t0) / (t1 - t0); return d0 + (d1 - d0) * f; }
    }
    return 0;
  }
  function gen(step, n) {
    const base = NOW - (n - 1) * step;
    const water = [], air = [], press = [], rpm = [], airflow = [], fanOn = [];
    const pPhase = uni(0, TAU);
    for (let i = 0; i < n; i++) {
      const t = base + i * step;
      const dayFrac = (((t % 86400) + 86400) % 86400) / 86400.0;
      const diurnal = Math.sin((dayFrac - 0.25) * TAU);
      const a = 26.0 + 2.5 * diurnal + gauss(0, 0.25);
      let w = 25.6 + 0.5 * Math.sin((dayFrac - 0.40) * TAU) + gauss(0, 0.08);
      w = Math.max(22.0, w);
      const days = t / 86400.0;
      const pr = 1013.0 + 6.5 * Math.sin(pPhase + days * (TAU / 3.2)) + gauss(0, 0.4);
      const duty = dutyFromTemp(w);
      let r;
      if (duty < 1) r = 0; else { r = Math.round(FAN_RPM_MAX * duty / 100.0 * uni(0.97, 1.02)); r = Math.max(350, r); }
      let on = duty >= 1 ? step : 0; on = Math.min(on, 255);
      water.push(+w.toFixed(2)); air.push(+a.toFixed(2)); press.push(+pr.toFixed(1));
      rpm.push(r); airflow.push(+(r * FAN_AIRFLOW_K).toFixed(2)); fanOn.push(on);
    }
    return { step, base, water, air, press, rpm, airflow, fanOn };
  }
  window.AQUA_DUMMY = {
    f: Object.assign({ tier: "f" }, gen(12, 7200)),
    m: Object.assign({ tier: "m" }, gen(60, 10080)),
    h: Object.assign({ tier: "h" }, gen(720, 14400)),
  };
})();
