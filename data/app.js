/* =====================================================================
 *  app.js  -  AquaController モック (フロントエンドのみ / v3)
 *  - 水温は最優先: 専用の 2秒ループで常時監視 (モード/オーバーレイに関係なく動作)
 *  - AP(初回) / STA / スタンドアローン の遷移
 *  - 共有カーブエディタ CurveEditor:
 *      ・ライト: R/G/B/W 各チャンネルを 24h で打点 → Catmull-Rom で滑らか補間
 *      ・ファン: 水温(°C) → 風量(%) を打点
 *  - ヒーター: キープ目標温度コントロール (上限 35.0°C)
 *  - 24時間の色(RGB)プレビュー帯は残置
 *  ※ バックエンドは無し。実機では各操作が AsyncWebServer API に対応する。
 * ===================================================================*/
"use strict";

const $  = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const hex2 = n => clamp(n | 0, 0, 255).toString(16).padStart(2, "0").toUpperCase();
const rgb  = (r, g, b) => `rgb(${r|0},${g|0},${b|0})`;
const cssVar = v => getComputedStyle(document.documentElement).getPropertyValue(v).trim() || "#888";
const pad2 = n => String(n | 0).padStart(2, "0");

const FAN_RPM_MAX = 2200, FAN_AIRFLOW_K = 0.013;
const SAFE_LO = 23.0, SAFE_HI = 29.0;          // 水温の安全域 (生体保護)
const DAYMIN = 1440;
const CH_COLOR = { r: "#ff6b6b", g: "#51e08a", b: "#4aa8ff", w: "#dfefff" };

/* ===================================================================
 *  データモデル
 * ===================================================================*/
/* ライト: チャンネルごとに {x: 分(0..1440), y: 光量%(0..100)} のキーフレーム */
const DEF_LIGHT = () => ({
  r: [{x:0,y:0},{x:360,y:24},{x:540,y:47},{x:1020,y:47},{x:1140,y:47},{x:1260,y:0},{x:1440,y:0}],
  g: [{x:0,y:0},{x:360,y:16},{x:540,y:55},{x:1020,y:55},{x:1140,y:12},{x:1260,y:0},{x:1440,y:0}],
  b: [{x:0,y:0},{x:360,y:4 },{x:540,y:71},{x:1020,y:71},{x:1140,y:4 },{x:1260,y:0},{x:1440,y:0}],
  w: [{x:0,y:3},{x:300,y:3},{x:360,y:8},{x:540,y:24},{x:1020,y:24},{x:1140,y:4},{x:1260,y:3},{x:1440,y:3}],
});
let lightCurve = DEF_LIGHT();
const ledPresets = {};

/* ファン: {x: 水温°C, y: 風量%} */
const DEF_FAN = () => [
  {x:24,y:0},{x:26,y:0},{x:27,y:40},{x:28.5,y:70},{x:30,y:100},{x:35,y:100},
];
let fanCurve = DEF_FAN();
const fanPresets = {};

/* ヒーター: キープ目標温度 + 許容幅 (上限 35°C) */
const heater = { target: 25.0, hyst: 0.5, min: 18.0, max: 35.0, on: false };

/* 実機ライブ層フック。aqua-live.js が API 到達時に値を流し込む。
   null のままならフロントエンド単体シミュレーション(モック)動作。 */
var AQ_LIVE = null;

const state = {
  mode: "ap-setup",
  ssid: "AquaHome-5G", ip: "192.168.1.42", rssi: -52,
  charts: {}, series: { water: true, air: true, press: true, humid: true },
  pphase: Math.random() * Math.PI * 2, tempDemo: 0,
  water: NaN,
};

/* ===================================================================
 *  Catmull-Rom サンプラ (端点はクランプ)。点列は x 昇順前提。
 * ===================================================================*/
function sampleCurve(pts, x) {
  const n = pts.length;
  if (!n) return 0;
  if (x <= pts[0].x) return pts[0].y;
  if (x >= pts[n - 1].x) return pts[n - 1].y;
  let i = 0;
  while (i < n - 1 && !(x >= pts[i].x && x <= pts[i + 1].x)) i++;
  const p1 = pts[i], p2 = pts[i + 1];
  const p0 = pts[i - 1] || p1, p3 = pts[i + 2] || p2;
  const span = (p2.x - p1.x) || 1e-6, t = (x - p1.x) / span;
  const t2 = t * t, t3 = t2 * t;
  const y = 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t
    + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2
    + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
  return y;
}

/* RGBW を 0..255 で返す (各チャンネル % → 255) */
function ledAt(min) {
  const s = ch => clamp(Math.round(sampleCurve(lightCurve[ch], min) / 100 * 255), 0, 255);
  return { r: s("r"), g: s("g"), b: s("b"), w: s("w") };
}
/* 風量カーブから duty(%) */
function dutyFromTemp(t) { return clamp(sampleCurve(fanCurve, t), 0, 100); }

/* ===================================================================
 *  共有カーブエディタ
 * ===================================================================*/
class CurveEditor {
  constructor(canvas, cfg) {
    this.cv = canvas; this.ctx = canvas.getContext("2d");
    this.xMin = cfg.xMin; this.xMax = cfg.xMax; this.yMin = cfg.yMin ?? 0; this.yMax = cfg.yMax ?? 100;
    this.xTicks = cfg.xTicks; this.xTickFmt = cfg.xTickFmt; this.yTicks = cfg.yTicks ?? [0,25,50,75,100];
    this.tracks = cfg.tracks;                    // [{key,color,points}]
    this.activeKey = this.tracks[0].key;
    this.bands = cfg.bands ?? [];                // [{x0,x1,color}]
    this.markersFn = cfg.markersFn ?? (() => []);// () => [{x,color,label}]
    this.onChange = cfg.onChange ?? (() => {});
    this.els = cfg.els ?? {};                    // {x,y,del,xParse,xFormat}
    this.minPts = cfg.minPts ?? 2;
    this.pad = { l: 40, r: 14, t: 14, b: 28 };
    this.sel = null; this.drag = false;
    this._bindPointer(); this._bindInputs();
    this.resize();
  }
  active() { return this.tracks.find(t => t.key === this.activeKey); }
  setActive(key) { this.activeKey = key; this.sel = null; this._syncInputs(); this.render(); }

  /* --- 座標変換 (CSS px) --- */
  _plot() { const r = this.cv.getBoundingClientRect();
    return { w: r.width, h: r.height,
      pw: r.width - this.pad.l - this.pad.r, ph: r.height - this.pad.t - this.pad.b }; }
  _sx(x) { const g = this._plot(); return this.pad.l + (x - this.xMin) / (this.xMax - this.xMin) * g.pw; }
  _sy(y) { const g = this._plot(); return this.pad.t + (1 - (y - this.yMin) / (this.yMax - this.yMin)) * g.ph; }
  _ix(px) { const g = this._plot(); return this.xMin + (px - this.pad.l) / g.pw * (this.xMax - this.xMin); }
  _iy(py) { const g = this._plot(); return this.yMin + (1 - (py - this.pad.t) / g.ph) * (this.yMax - this.yMin); }

  resize() {
    const r = this.cv.getBoundingClientRect();
    if (r.width < 4) return;                      // 非表示タブ等はスキップ
    const dpr = window.devicePixelRatio || 1;
    this.cv.width = Math.round(r.width * dpr);
    this.cv.height = Math.round(r.height * dpr);
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.render();
  }

  _clampX(pts, i, x) {                            // 隣接点を跨がせない
    const eps = (this.xMax - this.xMin) * 0.004;
    const lo = i > 0 ? pts[i - 1].x + eps : this.xMin;
    const hi = i < pts.length - 1 ? pts[i + 1].x - eps : this.xMax;
    return clamp(x, lo, hi);
  }
  _hit(px, py) {                                  // アクティブトラックの点を探す
    const pts = this.active().points, R = 16;
    for (let i = 0; i < pts.length; i++) {
      const dx = this._sx(pts[i].x) - px, dy = this._sy(pts[i].y) - py;
      if (dx * dx + dy * dy <= R * R) return i;
    }
    return -1;
  }

  _bindPointer() {
    const down = e => {
      const r = this.cv.getBoundingClientRect();
      const px = e.clientX - r.left, py = e.clientY - r.top;
      if (px < this.pad.l - 8 || px > r.width - this.pad.r + 8) return;
      const pts = this.active().points;
      let i = this._hit(px, py);
      if (i < 0) {                                // 追加
        const nx = clamp(this._ix(px), this.xMin, this.xMax);
        const ny = clamp(this._iy(py), this.yMin, this.yMax);
        const near = pts.findIndex(p => Math.abs(this._sx(p.x) - px) < 10);
        if (near >= 0) { i = near; pts[i].y = ny; }      // 近接 x は上書き
        else { pts.push({ x: nx, y: ny }); pts.sort((a, b) => a.x - b.x); i = pts.indexOf(pts.find(p => p.x === nx)); }
      }
      this.sel = i; this.drag = true; this.cv.setPointerCapture?.(e.pointerId);
      this._syncInputs(); this.render(); this.onChange(); e.preventDefault();
    };
    const move = e => {
      if (!this.drag || this.sel == null) return;
      const r = this.cv.getBoundingClientRect();
      const pts = this.active().points;
      pts[this.sel].x = this._clampX(pts, this.sel, this._ix(e.clientX - r.left));
      pts[this.sel].y = clamp(this._iy(e.clientY - r.top), this.yMin, this.yMax);
      this._syncInputs(); this.render(); this.onChange();
    };
    const up = () => { this.drag = false; };
    this.cv.addEventListener("pointerdown", down);
    this.cv.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  }

  _bindInputs() {
    const { x, y, del } = this.els;
    if (x) x.addEventListener("input", () => {
      if (this.sel == null) return;
      const pts = this.active().points;
      const v = this.els.xParse ? this.els.xParse(x.value) : parseFloat(x.value);
      if (Number.isFinite(v)) { pts[this.sel].x = this._clampX(pts, this.sel, v); this.render(); this.onChange(); }
    });
    if (y) y.addEventListener("input", () => {
      if (this.sel == null) return;
      const v = parseFloat(y.value);
      if (Number.isFinite(v)) { this.active().points[this.sel].y = clamp(v, this.yMin, this.yMax); this.render(); this.onChange(); }
    });
    if (del) del.addEventListener("click", () => {
      const pts = this.active().points;
      if (this.sel == null || pts.length <= this.minPts) return;
      pts.splice(this.sel, 1); this.sel = null; this._syncInputs(); this.render(); this.onChange();
    });
  }
  _syncInputs() {
    const { x, y, del } = this.els, has = this.sel != null;
    const p = has ? this.active().points[this.sel] : null;
    if (x) { x.disabled = !has; x.value = has ? (this.els.xFormat ? this.els.xFormat(p.x) : p.x.toFixed(1)) : ""; }
    if (y) { y.disabled = !has; y.value = has ? Math.round(p.y) : ""; }
    if (del) del.disabled = !has || this.active().points.length <= this.minPts;
  }
  setData() { this.sel = null; this._syncInputs(); this.render(); this.onChange(); }

  /* --- 描画 --- */
  render() {
    const g = this._plot(); if (g.w < 4) return;
    const c = this.ctx; c.clearRect(0, 0, g.w, g.h);
    const line = cssVar("--line"), sub = cssVar("--sub");

    // 安全域などの帯
    for (const b of this.bands) {
      c.fillStyle = b.color;
      const x0 = this._sx(clamp(b.x0, this.xMin, this.xMax)), x1 = this._sx(clamp(b.x1, this.xMin, this.xMax));
      c.fillRect(x0, this.pad.t, x1 - x0, g.ph);
    }
    // グリッド + 目盛
    c.strokeStyle = line; c.fillStyle = sub; c.lineWidth = 1; c.font = "11px ui-monospace, monospace";
    c.textAlign = "right"; c.textBaseline = "middle";
    for (const yv of this.yTicks) {
      const sy = this._sy(yv);
      c.globalAlpha = .5; c.beginPath(); c.moveTo(this.pad.l, sy); c.lineTo(g.w - this.pad.r, sy); c.stroke();
      c.globalAlpha = 1; c.fillText(yv, this.pad.l - 6, sy);
    }
    c.textAlign = "center"; c.textBaseline = "top";
    for (const xv of this.xTicks) {
      const sx = this._sx(xv);
      c.globalAlpha = .4; c.beginPath(); c.moveTo(sx, this.pad.t); c.lineTo(sx, g.h - this.pad.b); c.stroke();
      c.globalAlpha = 1; c.fillText(this.xTickFmt ? this.xTickFmt(xv) : xv, sx, g.h - this.pad.b + 6);
    }
    // 動的マーカ (現在時刻 / 現在水温)
    for (const m of this.markersFn()) {
      const sx = this._sx(clamp(m.x, this.xMin, this.xMax));
      c.strokeStyle = m.color; c.lineWidth = 2; c.setLineDash([4, 3]);
      c.beginPath(); c.moveTo(sx, this.pad.t); c.lineTo(sx, g.h - this.pad.b); c.stroke();
      c.setLineDash([]);
    }
    // 非アクティブトラック (薄く)
    for (const t of this.tracks) {
      if (t.key === this.activeKey) continue;
      this._stroke(t.points, t.color, 1.3, .32);
    }
    // アクティブトラック + 塗り + 点
    const a = this.active();
    this._stroke(a.points, a.color, 2.4, 1, true);
    for (let i = 0; i < a.points.length; i++) {
      const sx = this._sx(a.points[i].x), sy = this._sy(a.points[i].y), seld = i === this.sel;
      c.beginPath(); c.arc(sx, sy, seld ? 6 : 4.5, 0, 7);
      c.fillStyle = a.color; c.fill();
      c.lineWidth = 2; c.strokeStyle = seld ? cssVar("--ink") : cssVar("--bg2");
      c.stroke();
    }
  }
  _stroke(pts, color, lw, alpha, fill) {
    const c = this.ctx, g = this._plot();
    c.save(); c.globalAlpha = alpha; c.strokeStyle = color; c.lineWidth = lw;
    c.lineJoin = "round"; c.beginPath();
    const x0 = this.pad.l, x1 = g.w - this.pad.r;
    for (let sx = x0; sx <= x1; sx += 2) {
      const sy = this._sy(clamp(sampleCurve(pts, this._ix(sx)), this.yMin, this.yMax));
      sx === x0 ? c.moveTo(sx, sy) : c.lineTo(sx, sy);
    }
    c.stroke();
    if (fill) {
      c.lineTo(x1, this._sy(this.yMin)); c.lineTo(x0, this._sy(this.yMin)); c.closePath();
      c.globalAlpha = alpha * .12; c.fillStyle = color; c.fill();
    }
    c.restore();
  }
}

/* ===================================================================
 *  タブ / テーマ
 * ===================================================================*/
let ledEditor, fanEditor;
$$(".tab").forEach(b => b.addEventListener("click", () => {
  $$(".tab").forEach(x => x.classList.toggle("is-active", x === b));
  const id = b.dataset.tab;
  $$(".page").forEach(p => p.classList.toggle("is-active", p.id === id));
  if (id === "history") loadHistory();
  if (id === "light")   { ledEditor?.resize(); drawGradients(); }
  if (id === "control") { fanEditor?.resize(); syncHeaterUI(); }
}));
$("#themeToggle").addEventListener("click", () => {
  const cur = document.documentElement.getAttribute("data-theme");
  document.documentElement.setAttribute("data-theme", cur === "dark" ? "day" : "dark");
  if ($("#history").classList.contains("is-active")) loadHistory();
  ledEditor?.render();
  if (fanEditor) { fanEditor.tracks[0].color = cssVar("--accent"); fanEditor.bands[0].color = okBand(); fanEditor.render(); }
});

/* ===================================================================
 *  最優先: 水温 2秒監視ループ (常時稼働)
 * ===================================================================*/
const gauss = s => (Math.random() + Math.random() + Math.random() - 1.5) * s;
function readWaterTemp() {
  if (AQ_LIVE && AQ_LIVE.water != null) return AQ_LIVE.water + state.tempDemo;
  const now = Date.now() / 1000, df = (now % 86400) / 86400;
  let w = 25.6 + 0.5 * Math.sin((df - 0.40) * 2 * Math.PI) + gauss(0.06);
  w = Math.max(22, w) + state.tempDemo;
  return w;
}
function tempTick() {
  const w = state.water = readWaterTemp();
  const rangeOut = (w < SAFE_LO || w > SAFE_HI);
  // 実機の生体保護アラート (aqua-live が AQ_LIVE に反映)。範囲逸脱より優先して明示。
  const L = (typeof AQ_LIVE !== "undefined" && AQ_LIVE) ? AQ_LIVE : null;
  const faultMsg =
    L && L.sensorFault ? "⚠ 水温センサーが応答していません。配線/センサーを確認してください。" :
    L && L.heatFault   ? "⚠ ヒーターON継続でも水温が上がりません。ヒーター故障・断線の可能性。" :
    L && L.coolFault   ? "⚠ ファンON継続でも水温が下がりません。ファン停止・故障の可能性。" : "";
  const out = rangeOut || !!faultMsg;
  $("#rWater").textContent = w.toFixed(2);
  $("#chipWater").textContent = w.toFixed(2);
  $("#cWater").classList.toggle("alert", out);
  $("#tempChip").classList.toggle("alert", out);
  $("#emerg").classList.toggle("active", out);
  $("#emergText").textContent = faultMsg
    ? faultMsg
    : rangeOut
    ? `⚠ 水温が安全域を外れています: ${w.toFixed(2)} °C (安全 ${SAFE_LO}–${SAFE_HI} °C)`
    : `水温は安全域です (${SAFE_LO.toFixed(1)}–${SAFE_HI.toFixed(1)} °C)`;
  // 制御タブが開いていれば現在水温マーカ更新
  if ($("#control").classList.contains("is-active")) { fanEditor?.render(); updateThermCur(); }
}
setInterval(tempTick, 2000);
tempTick();

$("#emergBtn").addEventListener("click", () => {
  const w = state.water;
  if (w < SAFE_LO)      alert(`緊急対応 (モック)\n水温 ${w.toFixed(2)}°C — 低温。\n・ヒーター強制ON\n・管理者へ通知送信`);
  else if (w > SAFE_HI) alert(`緊急対応 (モック)\n水温 ${w.toFixed(2)}°C — 高温。\n・ファン100% / 照明減光\n・管理者へ通知送信`);
  else                  alert("現在は安全域です。緊急対応は不要です。");
});
$(".guard").addEventListener("click", () => {
  state.tempDemo = state.tempDemo === 0 ? 6 : state.tempDemo === 6 ? -6 : 0;
  tempTick();
});

/* ===================================================================
 *  その他ライブ値 (水温以外)
 * ===================================================================*/
function liveTick() {
  const now = Date.now() / 1000, df = (now % 86400) / 86400, days = now / 86400;
  const w = state.water || 26;
  let air, press, duty, rpm, humid, humidValid;
  if (AQ_LIVE) {
    air   = AQ_LIVE.air   != null ? AQ_LIVE.air   : 26;
    press = AQ_LIVE.press != null ? AQ_LIVE.press : 1013;
    duty  = AQ_LIVE.duty  != null ? Math.round(AQ_LIVE.duty) : Math.round(dutyFromTemp(w));
    rpm   = AQ_LIVE.rpm   != null ? AQ_LIVE.rpm : 0;
    humid = AQ_LIVE.humidity; humidValid = AQ_LIVE.humidityValid;
    if (AQ_LIVE.heaterOn != null) heater.on = AQ_LIVE.heaterOn;
  } else {
    air = 26 + 2.5 * Math.sin((df - 0.25) * 2 * Math.PI) + gauss(0.25);
    press = 1013 + 6.5 * Math.sin(state.pphase + days * (2 * Math.PI / 3.2)) + gauss(0.4);
    duty = Math.round(dutyFromTemp(w));
    rpm = duty < 1 ? 0 : Math.max(350, Math.round(FAN_RPM_MAX * duty / 100 * (0.97 + Math.random() * 0.05)));
    humid = 55 + 8 * Math.sin((df - 0.55) * 2 * Math.PI) + gauss(0.6); humidValid = true;  // モック湿度
    // ヒーター: 目標キープ + ヒステリシス (latch)
    heater.on = heater.on ? (w < heater.target) : (w < heater.target - heater.hyst);
  }
  const d = new Date(); const c = ledAt(d.getHours() * 60 + d.getMinutes());

  $("#rAir").textContent = air.toFixed(2);
  $("#rPress").textContent = press.toFixed(1);
  $("#rHumid").textContent = humidValid && humid != null ? humid.toFixed(1) : "--";
  $("#rRpm").textContent = rpm; $("#rDuty").textContent = duty;
  $("#rAirflow").textContent = (rpm * FAN_AIRFLOW_K).toFixed(2);
  $("#rHeater").textContent = heater.on ? "ON" : "OFF";
  $("#rHeaterSub").textContent = `目標 ${heater.target.toFixed(1)} °C キープ`;
  $("#cHeater").classList.toggle("on", heater.on);

  const hexc = `#${hex2(c.r)}${hex2(c.g)}${hex2(c.b)}`;
  $("#ledSwatch").style.background = hexc;
  $("#ledSwatch").style.boxShadow = c.w ? `0 0 ${8 + c.w / 6}px rgba(255,255,255,${c.w / 320})` : "none";
  $("#ledText").textContent = `${hexc} / W${c.w}`;
}
setInterval(liveTick, 1500); liveTick();

/* ===================================================================
 *  ライト: CurveEditor + グラデーション帯
 * ===================================================================*/
const minToHHMM = m => `${pad2((m / 60) | 0)}:${pad2(m % 60)}`;
const hhmmToMin = s => { const [h, m] = String(s).split(":").map(Number); return clamp((h || 0) * 60 + (m || 0), 0, DAYMIN); };

function buildLedEditor() {
  ledEditor = new CurveEditor($("#ledPlot"), {
    xMin: 0, xMax: DAYMIN, yMin: 0, yMax: 100,
    xTicks: [0,180,360,540,720,900,1080,1260,1440],
    xTickFmt: v => `${(v/60)|0}`,
    tracks: ["r","g","b","w"].map(k => ({ key: k, color: CH_COLOR[k], points: lightCurve[k] })),
    markersFn: () => { const d = new Date(); return [{ x: d.getHours()*60 + d.getMinutes(), color: cssVar("--ink") }]; },
    onChange: drawGradients,
    els: { x: $("#ledX"), y: $("#ledY"), del: $("#ledDel"),
           xParse: hhmmToMin, xFormat: minToHHMM },
  });
}
$$("#chanBtns .chanbtn").forEach(b => b.addEventListener("click", () => {
  $$("#chanBtns .chanbtn").forEach(x => x.classList.toggle("is-active", x === b));
  ledEditor.setActive(b.dataset.ch);
}));
$("#ledReset").addEventListener("click", () => {
  lightCurve = DEF_LIGHT();
  ledEditor.tracks.forEach(t => t.points = lightCurve[t.key]);
  ledEditor.setData(); drawGradients();
});

function drawGradients() {
  const cR = $("#gradRGB"), cW = $("#gradW"); if (!cR) return;
  const wpx = cR.width, gR = cR.getContext("2d"), gW = cW.getContext("2d");
  for (let x = 0; x < wpx; x++) {
    const min = x / wpx * DAYMIN, c = ledAt(min);
    gR.fillStyle = rgb(c.r, c.g, c.b); gR.fillRect(x, 0, 1, cR.height);
    gW.fillStyle = rgb(c.w, c.w, c.w); gW.fillRect(x, 0, 1, cW.height);
  }
  const d = new Date(), nx = (d.getHours() * 60 + d.getMinutes()) / DAYMIN * wpx;
  [[gR, cR], [gW, cW]].forEach(([g, c]) => {
    g.strokeStyle = "rgba(255,255,255,.9)"; g.lineWidth = 2;
    g.beginPath(); g.moveTo(nx, 0); g.lineTo(nx, c.height); g.stroke();
  });
}
setInterval(() => { if ($("#light").classList.contains("is-active")) drawGradients(); }, 30000);

/* プリセット (マイコン本体 /api/presets = LittleFS に保存。type=led/fan 別。
   全端末・全ブラウザ・再起動で共有 = どの端末からも同じプリセットを参照できる。
   API 到達不可 (バックエンド無しの単体プレビュー) 時はメモリにフォールバック)。 */
function refreshLedPresetList() {
  $("#ledPresetList").innerHTML = Object.keys(ledPresets).map(n => `<option>${n}</option>`).join("");
}
function refreshFanPresetList() {
  $("#fanPresetList").innerHTML = Object.keys(fanPresets).map(n => `<option>${n}</option>`).join("");
}
function refreshPresetList() { refreshLedPresetList(); }  // 後方互換 (旧呼び出し用)

// デバイス → グローバル ledPresets / fanPresets を置換 (デバイスを唯一の真実に)
async function loadPresetsFromDevice() {
  try {
    const r = await fetch("/api/presets");
    if (!r.ok) throw new Error("HTTP " + r.status);
    const p = await r.json();
    const led = (p && p.led) || {}, fan = (p && p.fan) || {};
    Object.keys(ledPresets).forEach(k => delete ledPresets[k]);
    Object.keys(led).forEach(k => { ledPresets[k] = led[k]; });
    Object.keys(fanPresets).forEach(k => delete fanPresets[k]);
    Object.keys(fan).forEach(k => { fanPresets[k] = fan[k]; });
  } catch (e) { /* オフライン: メモリ保持のまま */ }
  refreshLedPresetList(); refreshFanPresetList();
}
// 共通: デバイスへ保存/削除 (POST)。失敗してもメモリ側は反映済み。
async function presetPost(body) {
  try {
    const r = await fetch("/api/presets", { method: "POST",
      headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    if (!r.ok) throw new Error("HTTP " + r.status);
    await loadPresetsFromDevice();                       // デバイスと再同期
  } catch (e) { /* オフライン時はメモリのみ */ }
}

/* --- LED プリセット --- */
$("#ledPresetSave").addEventListener("click", () => {
  const name = $("#ledPresetName").value.trim(); if (!name) return;
  const data = JSON.parse(JSON.stringify(lightCurve));  // {r,g,b,w}
  ledPresets[name] = data; refreshLedPresetList(); $("#ledPresetName").value = "";
  presetPost({ type: "led", name, data });
});
$("#ledPresetLoad").addEventListener("click", () => {
  const name = $("#ledPresetList").value; if (!ledPresets[name]) return;
  lightCurve = JSON.parse(JSON.stringify(ledPresets[name]));
  ledEditor.tracks.forEach(t => t.points = lightCurve[t.key]);
  ledEditor.setData(); drawGradients();
});
$("#ledPresetDelete").addEventListener("click", () => {
  const name = $("#ledPresetList").value; if (!name) return;
  delete ledPresets[name]; refreshLedPresetList();
  presetPost({ type: "led", delete: name });
});

/* --- ファン プリセット --- */
$("#fanPresetSave").addEventListener("click", () => {
  const name = $("#fanPresetName").value.trim(); if (!name) return;
  const data = { points: fanCurve.map(p => ({ x: +p.x, y: +p.y })) };
  fanPresets[name] = data; refreshFanPresetList(); $("#fanPresetName").value = "";
  presetPost({ type: "fan", name, data });
});
$("#fanPresetLoad").addEventListener("click", () => {
  const p = fanPresets[$("#fanPresetList").value];
  if (!p || !Array.isArray(p.points)) return;
  fanCurve.length = 0; p.points.forEach(pt => fanCurve.push({ x: +pt.x, y: +pt.y }));
  fanEditor.tracks[0].points = fanCurve; fanEditor.setData();
});
$("#fanPresetDelete").addEventListener("click", () => {
  const name = $("#fanPresetList").value; if (!name) return;
  delete fanPresets[name]; refreshFanPresetList();
  presetPost({ type: "fan", delete: name });
});

loadPresetsFromDevice();                                 // 起動時にデバイスから読み込む

/* ===================================================================
 *  ファン: CurveEditor (水温 → 風量%)
 * ===================================================================*/
function buildFanEditor() {
  fanEditor = new CurveEditor($("#fanPlot"), {
    xMin: 22, xMax: 35, yMin: 0, yMax: 100,
    xTicks: [22,24,26,28,30,32,34],
    xTickFmt: v => `${v}`,
    tracks: [{ key: "fan", color: cssVar("--accent"), points: fanCurve }],
    bands: [{ x0: SAFE_LO, x1: SAFE_HI, color: okBand() }],
    markersFn: () => Number.isFinite(state.water) ? [{ x: state.water, color: cssVar("--accent2") }] : [],
    onChange: () => {},
    els: { x: $("#fanX"), y: $("#fanY"), del: $("#fanDel"),
           xParse: v => parseFloat(v), xFormat: v => v.toFixed(1) },
  });
}
function okBand() {
  // --ok を 12% 載せた近似色を作る (canvas 用)。簡易にテーマ別固定。
  return document.documentElement.getAttribute("data-theme") === "day"
    ? "rgba(10,154,122,.12)" : "rgba(55,224,176,.12)";
}
$("#fanReset").addEventListener("click", () => {
  fanCurve = DEF_FAN();
  fanEditor.tracks[0].points = fanCurve;
  fanEditor.setData();
});

/* ===================================================================
 *  ヒーター: 目標温度コントロール (上限 35°C)
 * ===================================================================*/
function thermPct(t) { return clamp((t - heater.min) / (heater.max - heater.min), 0, 1) * 100; }
function syncHeaterUI() {
  $("#htTarget").value = heater.target.toFixed(1);
  $("#htHyst").value = heater.hyst.toFixed(1);
  $("#thermTgt").style.left = thermPct(heater.target) + "%";
  $("#thermTgtLab").textContent = `目標 ${heater.target.toFixed(1)}`;
  $("#htNote").textContent =
    `水温が ${(heater.target - heater.hyst).toFixed(1)} °C を下回るとヒーターON、${heater.target.toFixed(1)} °C で OFF。`;
  updateThermCur();
}
function updateThermCur() {
  if (!Number.isFinite(state.water)) return;
  $("#thermCur").style.left = thermPct(state.water) + "%";
  $("#thermCurLab").textContent = `現在 ${state.water.toFixed(1)}`;
}
function setTarget(v) { heater.target = clamp(Math.round(v * 10) / 10, heater.min, heater.max); syncHeaterUI(); }

$("#htTarget").addEventListener("input", e => setTarget(parseFloat(e.target.value) || heater.target));
$("#htHyst").addEventListener("input", e => { heater.hyst = clamp(parseFloat(e.target.value) || 0.5, 0.1, 3); syncHeaterUI(); });
$$(".heater-card .step").forEach(b => b.addEventListener("click", () => setTarget(heater.target + parseFloat(b.dataset.d))));

/* サーモ目標ハンドルのドラッグ */
(function bindThermDrag() {
  const track = $("#thermTrack"), handle = $("#thermTgt");
  let dragging = false;
  const toTemp = clientX => {
    const r = track.getBoundingClientRect();
    return heater.min + clamp((clientX - r.left) / r.width, 0, 1) * (heater.max - heater.min);
  };
  handle.addEventListener("pointerdown", e => { dragging = true; handle.setPointerCapture?.(e.pointerId); e.preventDefault(); });
  track.addEventListener("pointerdown", e => { if (e.target === handle || handle.contains(e.target)) return; setTarget(toTemp(e.clientX)); });
  window.addEventListener("pointermove", e => { if (dragging) setTarget(toTemp(e.clientX)); });
  window.addEventListener("pointerup", () => { dragging = false; });
})();

/* ===================================================================
 *  履歴チャート + 系列選択 (ダミー時系列)
 * ===================================================================*/
function curRange() { const b = $(".rb.is-active"); return { tier: b.dataset.tier, span: +b.dataset.span }; }
$$(".rb").forEach(b => b.addEventListener("click", () => {
  $$(".rb").forEach(x => x.classList.toggle("is-active", x === b)); loadHistory();
}));
$$("#seriesChips .chip").forEach(ch => ch.addEventListener("click", () => {
  const key = ch.dataset.series; state.series[key] = !state.series[key];
  ch.classList.toggle("is-active", state.series[key]);
  applySeriesVisibility();
}));
function applySeriesVisibility() {
  const c = state.charts.temp; if (!c) return;
  const map = { water: 0, air: 1, press: 2, humid: 3 };
  for (const k in map) c.setDatasetVisibility(map[k], state.series[k]);
  c.update();
}
function sliceDummy(tier, span) {
  const src = ((AQ_LIVE && AQ_LIVE.history) || window.AQUA_DUMMY || {})[tier]; if (!src) return null;
  const n = Math.min(Math.floor(span / src.step), src.water.length), start = src.water.length - n;
  const sl = a => a.slice(start);
  return { tier, step: src.step, base: src.base + start * src.step,
    water: sl(src.water), air: sl(src.air), press: sl(src.press),
    humid: src.humid ? sl(src.humid) : sl(src.air).map(a => 60 - (a - 26) * 3),  // 実機は humid[], モックは合成
    rpm: sl(src.rpm), airflow: sl(src.airflow), fanOn: sl(src.fanOn) };
}
function fmtLabel(epochSec, span) {
  const d = new Date(epochSec * 1000), p = x => String(x).padStart(2, "0");
  if (span <= 86400)   return `${p(d.getHours())}:${p(d.getMinutes())}`;
  if (span <= 604800)  return `${p(d.getMonth() + 1)}/${p(d.getDate())} ${p(d.getHours())}:00`;
  if (span <= 2592000) return `${p(d.getMonth() + 1)}/${p(d.getDate())}`;
  return `${d.getFullYear()}/${p(d.getMonth() + 1)}/${p(d.getDate())}`;
}
const xScale = () => ({ ticks: { maxRotation: 0, autoSkip: true, maxTicksLimit: 8, color: cssVar("--sub") },
                       grid: { color: cssVar("--line") } });
function loadHistory() {
  if (typeof Chart === "undefined") { setTimeout(loadHistory, 200); return; }
  const { tier, span } = curRange(); const h = sliceDummy(tier, span); if (!h) return;
  const labels = h.water.map((_, i) => fmtLabel(h.base + i * h.step, span));
  $("#srcBadge").textContent = `ダミー履歴 (約${Math.round(h.water.length * h.step / 86400)}日)`;
  state.charts.temp?.destroy();
  state.charts.temp = new Chart($("#tempChart"), {
    type: "line",
    data: { labels, datasets: [
      { label: "水温°C", data: h.water, borderColor: "#3ec6ff", backgroundColor: "rgba(62,198,255,.12)",
        pointRadius: 0, borderWidth: 2, tension: .25, fill: true, yAxisID: "y", hidden: !state.series.water },
      { label: "気温°C", data: h.air, borderColor: "#7CFFB2", pointRadius: 0, borderWidth: 1.5, tension: .25, yAxisID: "y", hidden: !state.series.air },
      { label: "気圧hPa", data: h.press, borderColor: "#c89bff", pointRadius: 0, borderWidth: 1, tension: .25, yAxisID: "y1", hidden: !state.series.press },
      { label: "湿度%RH", data: h.humid, borderColor: "#5fd0e0", pointRadius: 0, borderWidth: 1.5, tension: .25, yAxisID: "y2", hidden: !state.series.humid },
    ]},
    options: { animation: false, responsive: true, maintainAspectRatio: false,
      interaction: { mode: "index", intersect: false },
      plugins: { legend: { labels: { color: cssVar("--ink"), boxWidth: 12 } } },
      scales: { x: xScale(),
        y:  { position: "left",  ticks: { color: cssVar("--sub") }, grid: { color: cssVar("--line") }, title: { display: true, text: "°C", color: cssVar("--sub") } },
        y1: { position: "right", ticks: { color: cssVar("--sub") }, grid: { display: false }, title: { display: true, text: "hPa", color: cssVar("--sub") } },
        y2: { position: "right", min: 0, max: 100, display: false } } }
  });
  state.charts.fan?.destroy();
  state.charts.fan = new Chart($("#fanChart"), {
    type: "line",
    data: { labels, datasets: [
      { label: "回転 rpm", data: h.rpm, borderColor: "#ffd166", pointRadius: 0, borderWidth: 1.5, tension: .2, yAxisID: "y" },
      { label: "風量 m³/h", data: h.airflow, borderColor: "#37e0b0", pointRadius: 0, borderWidth: 1.5, tension: .2, yAxisID: "y1" },
    ]},
    options: { animation: false, responsive: true, maintainAspectRatio: false,
      interaction: { mode: "index", intersect: false },
      plugins: { legend: { labels: { color: cssVar("--ink"), boxWidth: 12 } } },
      scales: { x: xScale(),
        y:  { position: "left",  ticks: { color: cssVar("--sub") }, grid: { color: cssVar("--line") } },
        y1: { position: "right", ticks: { color: cssVar("--sub") }, grid: { display: false } } } }
  });
}

/* ===================================================================
 *  ネットワーク: AP / STA / standalone
 * ===================================================================*/
const DUMMY_APS = [
  { ssid: "AquaHome-5G", rssi: -52, lock: true },
  { ssid: "AquaHome-2.4G", rssi: -58, lock: true },
  { ssid: "Buffalo-A-3F20", rssi: -67, lock: true },
  { ssid: "aterm-1a2b3c", rssi: -71, lock: true },
  { ssid: "FreeSpot_Lobby", rssi: -78, lock: false },
];
const bars = rssi => rssi > -55 ? "▮▮▮▮" : rssi > -65 ? "▮▮▮▯" : rssi > -75 ? "▮▮▯▯" : "▮▯▯▯";
function renderScan(ulSel, onPick) {
  const ul = $(ulSel); ul.innerHTML = "";
  DUMMY_APS.forEach(ap => {
    const li = document.createElement("li");
    li.innerHTML = `<span class="lock">${ap.lock ? "🔒" : "　"}</span><span>${ap.ssid}</span><span class="bars">${bars(ap.rssi)} ${ap.rssi}dBm</span>`;
    li.addEventListener("click", () => { $$("li", ul).forEach(x => x.classList.remove("sel")); li.classList.add("sel"); onPick(ap); });
    ul.appendChild(li);
  });
}
let apPick = DUMMY_APS[0], saPick = DUMMY_APS[0];

function setMode(mode) {
  state.mode = mode;
  const bar = $("#modebar"); bar.classList.remove("standalone", "ap");
  const setV = (sel, v) => { const e = $(sel); if (e) e.textContent = v; };
  if (mode === "sta") {
    $("#modeName").textContent = "STA モード";
    $("#modeMeta").textContent = `SSID: ${state.ssid} / IP: ${state.ip}`;
    $("#netPill").className = "pill ok"; $("#netPill").textContent = "接続中 (AP オフ)";
    setV("#wfState", "STA 接続中"); setV("#wfSsid", state.ssid);
    setV("#wfIp", state.ip); setV("#wfRssi", `${state.rssi} dBm`);
  } else if (mode === "standalone") {
    bar.classList.add("standalone");
    $("#modeName").textContent = "スタンドアローンモード";
    $("#modeMeta").textContent = "AP: AquaController / 192.168.4.1 (DHCP なし)";
    $("#netPill").className = "pill sim"; $("#netPill").textContent = "単体動作";
    setV("#wfState", "スタンドアローン (AP)");
  } else {
    bar.classList.add("ap");
    $("#modeName").textContent = "AP セットアップ";
    $("#modeMeta").textContent = "192.168.4.1 (ルータ未接続)";
    $("#netPill").className = "pill bad"; $("#netPill").textContent = "未接続";
    setV("#wfState", "AP セットアップ (未接続)");
  }
}

function openOverlay(toWifi) {
  $("#apOverlay").classList.add("show");
  $("#apHome").style.display = toWifi ? "none" : "block";
  $("#apWifi").style.display = toWifi ? "block" : "none";
  if (toWifi) renderScan("#apScan", ap => apPick = ap);
}
function closeOverlay() { $("#apOverlay").classList.remove("show"); }

// オーバーレイは廃止方向 (システムタブに集約) だが、要素が残る場合に備え null 安全に配線。
$("#apGoWifi")?.addEventListener("click", () => openOverlay(true));
$("#apBack")?.addEventListener("click", () => openOverlay(false));
$("#apGoStandalone")?.addEventListener("click", () => { closeOverlay(); setMode("standalone"); });
$("#apConnect")?.addEventListener("click", () => {
  state.ssid = apPick.ssid; state.rssi = apPick.rssi;
  state.ip = "192.168.1." + (20 + Math.floor(Math.random() * 200));
  closeOverlay(); setMode("sta");
});
$("#wfChange")?.addEventListener("click", () => openOverlay(true));
$("#wfStandalone")?.addEventListener("click", () => setMode("standalone"));
$("#saRescan")?.addEventListener("click", () => renderScan("#saScan", ap => saPick = ap));
$("#saConnect")?.addEventListener("click", () => {
  state.ssid = saPick.ssid; state.rssi = saPick.rssi;
  state.ip = "192.168.1." + (20 + Math.floor(Math.random() * 200));
  setMode("sta"); $$(".tab")[0].click();
});

window.addEventListener("resize", () => {
  if ($("#light").classList.contains("is-active")) ledEditor?.resize();
  if ($("#control").classList.contains("is-active")) fanEditor?.resize();
});

/* ===================================================================
 *  起動: モードバー表示の既定値のみ設定。
 *  セットアップ・オーバーレイの表示要否 (wifi.ini の configured 有無) は
 *  aqua-live.js 側で実機の /api/wifi を確認してから決める (wireWifi 参照)。
 *  ここで無条件に表示すると、設定済みの実機でも毎回一瞬セットアップ画面が
 *  出てしまうため、単体プレビュー(バックエンド無し)のときのみ
 *  aqua-live.js がフォールバックとして表示する。
 * ===================================================================*/
(function init() {
  buildLedEditor();
  buildFanEditor();
  drawGradients();
  syncHeaterUI();
  renderScan("#saScan", ap => saPick = ap);
  setMode("ap-setup");
})();
