/* =====================================================================
 *  aqua-live.js  -  実機 API 配線レイヤ (app.js の後に読み込む)
 *  - /api/state を周期ポーリング → AQ_LIVE に流し込み (app.js が表示)
 *  - /api/settings を起動時読込 → エディタへ反映、変更時に保存(POST)
 *  - /api/history をタブ表示時に取得
 *  API 到達不可なら何もしない = app.js 単体のモック動作にフォールバック。
 * ===================================================================*/
(function () {
  "use strict";
  const API = {
    state:    "/api/state",
    settings: "/api/settings",
    history:  "/api/history",
    wifi:     "/api/wifi",
    mode:     "/api/mode",
  };
  const POLL_MS = 2000;
  let liveOK = false;

  const post = (body) => fetch("/api/test", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }).catch(() => {});
  // 設定のダウンロード/アップロード共通ヘルパ
  async function downloadJson(url, filename) {
    try {
      const r = await fetch(url); const txt = await r.text();
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([txt], { type: "application/json" }));
      a.download = filename; document.body.appendChild(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 2000);
    } catch (e) { alert("ダウンロード失敗: " + e); }
  }
  function uploadJson(url, label) {
    const inp = document.createElement("input");
    inp.type = "file"; inp.accept = "application/json,.json";
    inp.onchange = async () => {
      const f = inp.files && inp.files[0]; if (!f) return;
      try {
        const obj = JSON.parse(await f.text());
        await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(obj) });
        alert(label + " を適用しました。");
      } catch (e) { alert(label + " の読み込みに失敗: " + e); }
    };
    inp.click();
  }
  function backupBar(pageId, label, url, fname, withUpload) {
    const page = document.getElementById(pageId); if (!page || page.querySelector(".backup-bar")) return;
    const bar = document.createElement("div");
    bar.className = "backup-bar";
    bar.style.cssText = "display:flex;gap:8px;justify-content:flex-end;margin:10px 0 4px";
    bar.innerHTML = `<button class="btn" data-dl>${label}をDL</button>` +
                    (withUpload ? `<button class="btn" data-ul>${label}をUL</button>` : "");
    page.appendChild(bar);
    bar.querySelector("[data-dl]").addEventListener("click", () => downloadJson(url, fname));
    const ul = bar.querySelector("[data-ul]");
    if (ul) ul.addEventListener("click", () => uploadJson(url, label));
  }
  function wireBackup() {
    backupBar("light",   "設定", "/api/settings", "settings.json", true);
    backupBar("control", "設定", "/api/settings", "settings.json", true);
    backupBar("history", "履歴(f)", "/api/history?tier=f", "history_f.json", false);
  }

  const j = async (url, opt, ms = 4000) => {
    const ac = new AbortController();
    const to = setTimeout(() => ac.abort(), ms);
    try {
      const r = await fetch(url, Object.assign({ signal: ac.signal }, opt || {}));
      if (!r.ok) throw new Error("HTTP " + r.status);
      return await r.json();
    } finally { clearTimeout(to); }
  };

  // ---- 設定 (curve) をエディタへ反映 (プリセット読込と同じ手順) ----
  function applySettings(s) {
    try {
      if (s.light) {
        ["r", "g", "b", "w"].forEach(k => {
          if (Array.isArray(s.light[k])) lightCurve[k] = s.light[k].map(p => ({ x: +p.x, y: +p.y }));
        });
        if (typeof ledEditor !== "undefined" && ledEditor) {
          ledEditor.tracks.forEach(t => t.points = lightCurve[t.key]);
          ledEditor.setData();
        }
        if (typeof drawGradients === "function") drawGradients();
      }
      if (s.fan && Array.isArray(s.fan.points)) {
        const np = s.fan.points.map(p => ({ x: +p.x, y: +p.y }));
        fanCurve.length = 0; np.forEach(p => fanCurve.push(p));
        if (typeof fanEditor !== "undefined" && fanEditor) {
          fanEditor.tracks[0].points = fanCurve; fanEditor.setData();
        }
      }
      if (s.heater) {
        if (s.heater.target != null) heater.target = +s.heater.target;
        if (s.heater.hyst != null)   heater.hyst   = +s.heater.hyst;
        if (typeof syncHeaterUI === "function") syncHeaterUI();
      }
    } catch (e) { console.warn("applySettings", e); }
  }

  // ---- 現在の設定を API スキーマへ ----
  function snapshotSettings() {
    const cp = a => a.map(p => ({ x: +p.x, y: +p.y }));
    return {
      light: { r: cp(lightCurve.r), g: cp(lightCurve.g), b: cp(lightCurve.b), w: cp(lightCurve.w) },
      fan:   { points: cp(fanCurve) },
      heater:{ target: heater.target, hyst: heater.hyst },
      safe:  { lo: (typeof SAFE_LO !== "undefined" ? SAFE_LO : 23), hi: (typeof SAFE_HI !== "undefined" ? SAFE_HI : 29) },
    };
  }

  // ---- 保存 (デバウンス POST) ----
  let pushT = null;
  function pushSettings() {
    if (!liveOK) return;
    clearTimeout(pushT);
    pushT = setTimeout(async () => {
      try {
        await j(API.settings, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(snapshotSettings()),
        });
      } catch (e) { console.warn("pushSettings", e); }
    }, 400);
  }

  // ---- ライブ状態ポーリング ----
  async function poll() {
    try {
      const s = await j(API.state);
      if (!liveOK) { liveOK = true; AQ_LIVE = AQ_LIVE || {}; }
      AQ_LIVE.water    = s.waterValid ? s.water : null;
      AQ_LIVE.air      = s.air;
      AQ_LIVE.press    = s.press;
      AQ_LIVE.humidity      = s.humidity;
      AQ_LIVE.humidityValid = !!s.humidityValid;
      AQ_LIVE.calib         = s.calib || null;
      updateCalib(s.calib);
      AQ_LIVE.duty     = s.fan ? s.fan.duty : null;
      AQ_LIVE.rpm      = s.fan ? s.fan.rpm : null;
      AQ_LIVE.heaterOn = s.heater ? s.heater.on : null;
      AQ_LIVE.led      = s.led || null;
      AQ_LIVE.sensorFault = !!s.sensorFault;   // 生体保護アラート (main.cpp が判定)
      AQ_LIVE.heatFault   = !!s.heatFault;
      AQ_LIVE.coolFault   = !!s.coolFault;
      updateChrome(s);
    } catch (e) {
      // 失敗継続でモックに退避
      if (liveOK) { liveOK = false; AQ_LIVE = null; }
    } finally {
      setTimeout(poll, POLL_MS);
    }
  }

  // ---- 履歴: タブ表示時に取得して app.js の sliceDummy へ渡す ----
  function wrapHistory() {
    if (typeof loadHistory !== "function") return;
    const orig = loadHistory;
    loadHistory = async function () {
      if (liveOK) {
        try {
          const cr = (typeof curRange === "function") ? curRange() : { tier: "f" };
          const h = await j(API.history + "?tier=" + cr.tier, null, 8000);
          AQ_LIVE = AQ_LIVE || {};
          AQ_LIVE.history = AQ_LIVE.history || {};
          AQ_LIVE.history[h.tier] = h;
        } catch (e) { console.warn("history", e); }
      }
      return orig.apply(this, arguments);
    };
  }

  // ---- エディタ/ヒーターの変更で保存をフック ----
  function wrapSavers() {
    if (typeof ledEditor !== "undefined" && ledEditor) {
      const o = ledEditor.onChange;
      ledEditor.onChange = function () { o && o.apply(this, arguments); pushSettings(); };
    }
    if (typeof fanEditor !== "undefined" && fanEditor) {
      const o = fanEditor.onChange;
      fanEditor.onChange = function () { o && o.apply(this, arguments); pushSettings(); };
    }
    // ヒーター: setTarget(関数) ラップ + hyst 入力に追従
    if (typeof setTarget === "function") {
      const o = setTarget;
      setTarget = function () { o.apply(this, arguments); pushSettings(); };
    }
    const hh = document.querySelector("#htHyst");
    if (hh) hh.addEventListener("input", pushSettings);
  }

  // ---- ネットワーク (best-effort) ----
  function wrapNetwork() {
    if (typeof setMode === "function") {
      const o = setMode;
      setMode = function (m) {
        o.apply(this, arguments);
        if (liveOK && (m === "standalone" || m === "ap")) {
          j(API.mode, { method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ mode: m }) }).catch(() => {});
        }
      };
    }
  }

  // ---- モードバー + NTP 時計 ----
  function updateChrome(s) {
    const clk = document.getElementById("ntpClock");
    if (clk) {
      const t = s.time | 0;
      if (t > 1700000000) {                       // NTP 同期済 → JST 実時刻
        const d = new Date((t + 9 * 3600) * 1000);
        const p = n => String(n).padStart(2, "0");
        clk.textContent = p(d.getUTCHours()) + ":" + p(d.getUTCMinutes()) + ":" + p(d.getUTCSeconds());
      } else {                                    // 未同期 → 起動経過
        clk.textContent = "起動 " + Math.floor(t / 3600) + "h" + String(Math.floor(t / 60) % 60).padStart(2, "0");
      }
    }
    const nm = document.getElementById("modeName"), mm = document.getElementById("modeMeta"),
          np = document.getElementById("netPill");
    if (nm) nm.textContent = (["AP", "STA", "スタンドアローン"][s.mode] || "AP") + " モード";
    if (mm) mm.textContent = "SSID: " + (s.ssid || "-") + " / IP: " + (s.ip || "-");
    if (np) { np.textContent = s.mode === 1 ? "接続中" : "AP稼働"; }
  }

  // ---- 時刻の手動設定 (システムタブの時刻カード) ----
  function wireClockSet() {
    const inp = document.getElementById("tsInput");
    const setBtn = document.getElementById("tsSet");
    const nowBtn = document.getElementById("tsNow");
    if (!inp || !setBtn || !nowBtn) return;
    const pad = n => String(n).padStart(2, "0");
    function fillNow() {
      const d = new Date();
      inp.value = d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) +
                  "T" + pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
    }
    async function postTime(epoch) {
      try {
        const r = await fetch("/api/time", { method: "POST", headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ epoch }) });
        if (!r.ok) throw new Error("HTTP " + r.status);
        alert("時刻を設定しました。");
      } catch (e) { alert("時刻設定に失敗しました: " + e); }
    }
    fillNow();
    // この端末の現在時刻 (絶対時刻) に同期。Date.now() は TZ 非依存なので最も安全。
    nowBtn.addEventListener("click", () => postTime(Math.floor(Date.now() / 1000)));
    // 手入力は JST として UTC epoch を明示計算 (ブラウザTZに依存しない。JST=UTC+9)。
    setBtn.addEventListener("click", () => {
      const v = inp.value; if (!v) return;
      const m = v.match(/(\d+)-(\d+)-(\d+)T(\d+):(\d+)(?::(\d+))?/);
      if (!m) { alert("日時の形式が不正です"); return; }
      const epoch = Math.floor(Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +(m[6] || 0)) / 1000) - 9 * 3600;
      postTime(epoch);
    });
  }

  // ---- 校正の可視化・ダウンロード ----
  function fmtOff(v) { return (v >= 0 ? "+" : "") + Number(v).toFixed(2); }
  function updateCalib(cal) {
    const set = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
    if (!cal) { ["calAir","calHumid","calPress","calDiffAir","calDiffHumid"].forEach(id => set(id, "-")); return; }
    set("calAir",   fmtOff(cal.air)   + " °C");
    set("calHumid", fmtOff(cal.humid) + " %RH");
    set("calPress", fmtOff(cal.press) + " hPa");
    if (cal.diffValid) {
      set("calDiffAir",   fmtOff(cal.diffAir)   + " °C");
      set("calDiffHumid", fmtOff(cal.diffHumid) + " %RH");
    } else { set("calDiffAir", "基準未接続"); set("calDiffHumid", "基準未接続"); }
  }
  function wireCalib() {
    const stamp = () => { const d = new Date(), p = n => String(n).padStart(2, "0");
      return d.getFullYear() + p(d.getMonth()+1) + p(d.getDate()) + "_" + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds()); };
    function download(name, mime, text) {
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([text], { type: mime })); a.download = name;
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    }
    const dj = document.getElementById("calDlJson"), dc = document.getElementById("calDlCsv");
    if (dj) dj.addEventListener("click", () => {
      const c = (AQ_LIVE && AQ_LIVE.calib) || {};
      download("calib_" + stamp() + ".json", "application/json",
        JSON.stringify({ savedAt: new Date().toISOString(), calib: c }, null, 2));
    });
    if (dc) dc.addEventListener("click", () => {
      const c = (AQ_LIVE && AQ_LIVE.calib) || {};
      const rows = [["field", "offset", "live_diff"],
        ["air", c.air, c.diffAir], ["humid", c.humid, c.diffHumid],
        ["press", c.press, ""], ["water", c.water, ""]];
      download("calib_" + stamp() + ".csv", "text/csv", rows.map(r => r.join(",")).join("\r\n"));
    });
  }

  // ---- LED 時間デバッグ (1秒=1時間, 単一トグル, グラフ時刻マーカー連動) ----
  function wireLedDebug() {
    const r = document.getElementById("ledTime"), lab = document.getElementById("ledTimeLab"),
          tog = document.getElementById("ledToggle");
    if (!r || !tog) return;
    const hhmm = m => { m = Math.round(+m); return String(m / 60 | 0).padStart(2, "0") + ":" + String(m % 60).padStart(2, "0"); };
    let timer = null, mode = "auto";
    const show = () => lab.textContent = hhmm(r.value);
    const send = () => post({ led: { min: +r.value } });

    // グラフの時刻マーカーをスライダ値に追従させる
    function setMark(active) {
      if (typeof ledEditor === "undefined" || !ledEditor) return;
      if (active) {
        const mv = +r.value;
        ledEditor.markersFn = () => [{ x: mv, color: (typeof cssVar === "function" ? cssVar("--ink") : "#fff") }];
      } else {
        ledEditor.markersFn = () => { const d = new Date(); return [{ x: d.getHours() * 60 + d.getMinutes(), color: (typeof cssVar === "function" ? cssVar("--ink") : "#fff") }]; };
      }
      ledEditor.render();
    }
    const setIcon = () => tog.innerHTML = timer ? "&#10074;&#10074;" : "&#9654;";

    r.addEventListener("input", () => { if (mode === "auto") mode = "hold"; show(); setMark(true); send(); });
    tog.addEventListener("click", () => {
      if (timer) { clearInterval(timer); timer = null; mode = "hold"; }   // 一時停止 (時刻保持)
      else {
        mode = "play";
        timer = setInterval(() => { r.value = (+r.value + 12) % 1440; show(); setMark(true); send(); }, 200); // 60min/s
      }
      setIcon();
    });
    document.getElementById("ledAuto").addEventListener("click", () => {
      if (timer) { clearInterval(timer); timer = null; }
      mode = "auto"; setIcon(); setMark(false); post({ led: { off: true } });
    });
    show(); setIcon();
    setInterval(() => { if (mode === "play" || mode === "hold") send(); }, 1000); // TTL 保持
  }

  // ---- ファン PWM デバッグ (カーソル位置の風量で実運転) ----
  function wireFanDebug() {
    const btn = document.getElementById("fanRun"), plot = document.getElementById("fanPlot");
    if (!btn || !plot) return;
    let on = false, lastDuty = 0;
    const compute = ev => {
      if (typeof fanEditor === "undefined" || !fanEditor) return null;
      const rect = plot.getBoundingClientRect();
      const px = (ev.clientX - rect.left) * (plot.width / rect.width) / (window.devicePixelRatio || 1);
      const temp = fanEditor._ix(px);
      return Math.max(0, Math.min(100, sampleCurve(fanCurve, temp)));
    };
    plot.addEventListener("pointermove", ev => {
      if (!on) return;
      const d = compute(ev); if (d == null) return;
      lastDuty = Math.round(d);
      post({ fan: { duty: lastDuty } });
    });
    btn.addEventListener("click", () => {
      on = !on;
      btn.innerHTML = on ? "&#10074;&#10074; 停止" : "&#9654; 運転";
      btn.classList.toggle("primary", on);
      if (!on) post({ fan: { off: true } });
    });
    setInterval(() => { if (on) post({ fan: { duty: lastDuty } }); }, 1000); // TTL 保持
  }

  // ---- Wi-Fi 実スキャン + 接続 (IP をリンク表示) ----
  function wireWifi() {
    const barsOf = r => r > -55 ? "▮▮▮▮" : r > -65 ? "▮▮▮▯" : r > -75 ? "▮▮▯▯" : "▮▯▯▯";
    // 実スキャン: /api/wifi/scan を数回ポーリングして一覧化
    async function realScan(ulSel, onPick) {
      const ul = document.querySelector(ulSel); if (!ul) return;
      const manualSel = ulSel === "#apScan" ? "#apSsidManual" : "#saSsidManual";
      const passSel   = ulSel === "#apScan" ? "#apPass" : "#saPass";
      ul.innerHTML = "<li>スキャン中…</li>";
      let aps = [], lastErr = "";
      for (let i = 0; i < 8; i++) {
        try {
          const d = await j(API.wifi + "/scan", null, 6000);
          if (d.aps && d.aps.length) { aps = d.aps; break; }
          if (d.scanning === false && d.aps) { aps = d.aps; break; }  // 完了(0件含む)
        } catch (e) { lastErr = String((e && e.message) || e); }
        await new Promise(r => setTimeout(r, 1200));
      }
      if (!aps.length) {
        ul.innerHTML = lastErr
          ? `<li style="color:#e66">スキャンAPIに接続できません（${lastErr}）。サーバ未応答の可能性。下の SSID 欄に直接入力して接続してください</li>`
          : "<li>APが見つかりません。下の SSID 欄に直接入力して接続してください</li>";
        return;
      }
      aps.sort((a, b) => b.rssi - a.rssi);
      ul.innerHTML = "";
      aps.forEach(ap => {
        const li = document.createElement("li");
        li.innerHTML = `<span class="lock">${ap.lock ? "🔒" : "　"}</span><span>${ap.ssid}</span>` +
                       `<span class="bars">${barsOf(ap.rssi)} ${ap.rssi}dBm</span>`;
        li.addEventListener("click", () => {
          ul.querySelectorAll("li").forEach(x => x.classList.remove("sel"));
          li.classList.add("sel");
          const mf = document.querySelector(manualSel); if (mf) mf.value = ap.ssid;  // 手入力欄へ反映
          const pf = document.querySelector(passSel); if (pf) pf.focus();            // PW にフォーカス
          onPick(ap);
        });
        ul.appendChild(li);
      });
    }
    // app.js のグローバル renderScan を「常に」実スキャンへ差し替え。
    // これにより実機ではダミー AP (DUMMY_APS) を一切表示しない。
    if (typeof renderScan !== "undefined") {
      renderScan = function (ulSel, onPick) { realScan(ulSel, onPick); };
    }
    // 起動直後に両画面のダミーを実スキャンで置換 (標準/AP どちらでも)
    realScan("#saScan", ap => { try { saPick = ap; } catch (e) {} });
    realScan("#apScan", ap => { try { apPick = ap; } catch (e) {} });

    function showStaIp(ssid, ip) {
      const ipLink = `<a href="http://${ip}" target="_blank" rel="noopener">${ip}</a>`;
      const mdns = `<a href="http://aquacontroller.local" target="_blank" rel="noopener">aquacontroller.local</a>`;
      const mm = document.getElementById("modeMeta");
      if (mm) mm.innerHTML = `STA: ${ssid} / IP: ${ipLink} / ${mdns}`;
      const wf = document.getElementById("wfIp");
      if (wf) wf.innerHTML = `${ipLink}（または ${mdns}）`;
      // AP はまもなく切断される旨を通知
      const np = document.getElementById("netPill");
      if (np) { np.className = "pill ok"; np.textContent = "STA接続・AP切断"; }
      alert("STA 接続に成功しました。\nこの機器の URL:\n  http://" + ip + "\n  http://aquacontroller.local\n\nセキュリティのため AP はまもなく切断されます。ホームネットワークに接続し直して上記 URL を開いてください。");
    }

    async function pollConnResult(ssid, mm) {
      // GET /api/wifi の conn (2=OK,3=FAIL) をポーリング
      for (let i = 0; i < 20; i++) {
        await new Promise(r => setTimeout(r, 1200));
        try {
          const w = await j(API.wifi, null, 4000);
          if (w.conn === 2 && w.ip && w.ip !== "192.168.4.1") { showStaIp(ssid, w.ip); return; }
          if (w.conn === 3) {
            if (mm) mm.textContent = "接続失敗。SSID/パスワードをご確認ください。";
            alert("接続に失敗しました。SSID とパスワードを確認して、もう一度お試しください。");
            return;   // 画面2 のまま
          }
        } catch (e) { /* AP 切断で応答なしの場合あり */ }
      }
      if (mm) mm.innerHTML = "接続確認がとれませんでした。数十秒後にホーム側で <b>http://" + "aquacontroller.local</b> をお試しください。";
    }

    async function doConnect(kind) {
      const ssid = ((document.getElementById(kind + "SsidManual") || {}).value || "").trim();
      if (!ssid) { alert("SSID を選択または入力してください"); return; }
      const pass = (document.getElementById(kind + "Pass") || {}).value || "";
      const mm = document.getElementById("modeMeta");
      if (mm) mm.textContent = "接続情報を保存し、STA 単独で再起動します…";
      try {
        await j(API.wifi, {
          method: "POST", headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ ssid, pass }),
        }, 6000);
      } catch (e) {}
      // セキュリティのため AP と STA を同時に上げない設計: 本機は再起動して STA 単独で
      // 接続する。AP は停止するので、この画面(AP経由)からは結果を確認できない。
      alert("接続情報を保存しました。\n本機は AP を停止し、再起動して「" + ssid +
            "」へ STA 単独で接続します（AP と STA の同時起動はしません）。\n\n" +
            "再起動後は家庭内 LAN 側から下記で開いてください:\n" +
            "  http://aquacontroller.local\n  （または割り当てられた IP アドレス）\n\n" +
            "接続に失敗した場合は自動で AP セットアップに戻ります。");
    }
    const sc = document.getElementById("saConnect");
    if (sc) sc.addEventListener("click", () => doConnect("sa"));
    const ac = document.getElementById("apConnect");
    if (ac) ac.addEventListener("click", () => doConnect("ap"));

    // 初回セットアップの「スタンドアローン(AP)で起動」→ wifi.ini mode=ap 保存 → ダッシュボードへ
    const gs = document.getElementById("apGoStandalone");
    if (gs) gs.addEventListener("click", () => {
      j("/api/mode", { method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode: "ap" }) }).catch(() => {});
      const ov = document.getElementById("apOverlay"); if (ov) ov.style.display = "none";
    });

    // 起動フロー: 「設定読み込み中」(#bootLoading) を出したまま /api/wifi を判定し、
    //   未設定(初回) → システムタブ(Wi-Fi設定)へ誘導 / 設定済 → モニタ表示。
    //   その後ローディングを消す (画面のちらつき・二度惑い防止)。
    (async () => {
      try {
        const w = await j(API.wifi, null, 4000);
        const ov = document.getElementById("apOverlay");
        if (ov) { ov.classList.remove("show"); ov.style.display = "none"; }  // オーバーレイは廃止
        buildWifiPanel(w);                          // Wi-Fi 詳細設定をシステムタブに構築
        if (!w.configured) {                        // 初回=未設定 → システムタブへ
          const sysTab = document.querySelector('.tab[data-tab="system"]');
          if (sysTab) sysTab.click();
        }
      } catch (e) {
        // API 到達不可 (バックエンド無しの単体プレビュー) → モニタ表示のまま
      } finally {
        hideBootLoading();   // 判定確定 → ローディング解除
      }
    })();
  }

  function hideBootLoading() {
    const b = document.getElementById("bootLoading");
    if (b) { b.classList.remove("show"); b.style.display = "none"; }
  }

  // ---- Wi-Fi 管理パネル (ダッシュボード③: AP/STA の SSID・PW・mDNS + 保存/DL/UL) ----
  function buildWifiPanel(w) {
    const scr = document.querySelector('[data-screen="wifi"]') ||
                document.getElementById("scrWifi") ||
                (document.getElementById("tabWifi") ? null : null);
    // 挿入先: Wi-Fi 画面のカード。#wfChange の親あたりに追記
    const anchor = document.getElementById("wfStandalone") || document.getElementById("wfChange");
    if (!anchor || document.getElementById("wifiMgmt")) return;
    const box = document.createElement("div");
    box.id = "wifiMgmt";
    box.style.cssText = "margin-top:14px;padding:12px;border:1px solid var(--line,#2a3550);border-radius:10px";
    box.innerHTML = `
      <h3 style="margin:0 0 8px">Wi-Fi 設定 (wifi.ini)</h3>
      <div class="field"><label>起動モード</label>
        <select id="wmMode"><option value="ap">AP スタンドアローン</option><option value="sta">STA (家庭内 Wi-Fi)</option></select></div>
      <div class="field"><label>AP SSID</label><input type="text" id="wmApSsid"></div>
      <div class="field"><label>AP パスワード <small>(8文字以上で保護 / 空=開放)</small></label><input type="text" id="wmApPw" placeholder="8文字以上 or 空"></div>
      <div class="field"><label>STA SSID</label><input type="text" id="wmStaSsid"></div>
      <div class="field"><label>STA パスワード</label><input type="password" id="wmStaPw" placeholder="変更する場合のみ入力"></div>
      <div class="field"><label>mDNS 名 <small>(例: aquacontroller → http://aquacontroller.local)</small></label><input type="text" id="wmMdns"></div>
      <div class="actions" style="display:flex;gap:8px;flex-wrap:wrap;margin-top:8px">
        <button class="btn primary" id="wmSave">保存して再起動</button>
        <button class="btn" id="wmDl">ダウンロード</button>
        <button class="btn" id="wmUl">アップロード</button>
      </div>
      <p class="hint" style="margin-top:6px">保存すると設定を wifi.ini に書き込み、約1.5秒後に再起動して反映します。</p>`;
    anchor.parentElement.appendChild(box);

    document.getElementById("wmMode").value = w.modeStr || "ap";
    document.getElementById("wmApSsid").value = w.apSsid || "AquaController";
    document.getElementById("wmStaSsid").value = w.staSsid || "";
    document.getElementById("wmMdns").value = w.mdns || "aquacontroller";

    document.getElementById("wmSave").addEventListener("click", async () => {
      const body = {
        mode: document.getElementById("wmMode").value,
        ap_ssid: document.getElementById("wmApSsid").value,
        ap_pw: document.getElementById("wmApPw").value,
        sta_ssid: document.getElementById("wmStaSsid").value,
        sta_pw: document.getElementById("wmStaPw").value,
        mdns: document.getElementById("wmMdns").value,
      };
      try {
        await j("/api/wifi/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }, 6000);
        alert("保存しました。約1.5秒後に再起動します。\n再起動後、選択したモードで起動します。");
      } catch (e) { alert("保存要求を送信しました（再起動により応答が途切れる場合があります）。"); }
    });
    document.getElementById("wmDl").addEventListener("click", () => { location.href = "/api/wifi/config"; });
    document.getElementById("wmUl").addEventListener("click", () => uploadJson("/api/wifi/config", "wifi.ini"));
  }

  // ---- 起動 ----
  async function init() {
    // 保険: 何らかの理由で判定が返らなくてもローディングで固まらないよう強制解除。
    setTimeout(hideBootLoading, 8000);
    try {
      const s = await j(API.settings);
      liveOK = true; AQ_LIVE = AQ_LIVE || {};
      applySettings(s);
    } catch (e) {
      liveOK = false;     // API 無し → モック動作のまま
    }
    wrapHistory();
    wrapSavers();
    wrapNetwork();
    wireLedDebug();
    wireFanDebug();
    wireWifi();
    wireBackup();
    wireClockSet();
    wireCalib();
    poll();
  }

  if (document.readyState === "complete" || document.readyState === "interactive") {
    setTimeout(init, 0);
  } else {
    document.addEventListener("DOMContentLoaded", init);
  }
})();
