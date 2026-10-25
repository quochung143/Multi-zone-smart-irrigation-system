// Dashboard: REST /api/* + WebSocket /ws (docs/SERVER.md §Dashboard)
const ZONES = [1, 2, 3];
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const COLORS = ZONES.map((z) => css(`--z${z}`));
const MODE_VI = { MANUAL: "Thủ công", THRESHOLD: "Ngưỡng", AI: "AI" };
const ACTION_VI = {
  water: "tưới", none: "không cần tưới", skip_manual: "chế độ thủ công", skip_offline: "nút offline",
  skip_stale: "dữ liệu cũ", skip_sensor: "lỗi cảm biến đất", skip_no_model: "chưa có model",
  skip_pump_on: "bơm đang chạy", skip_cooldown: "đang cooldown", skip_gw_offline: "gateway offline",
};
const CFG_KEYS = ["soil_low", "soil_target", "pump_max_s", "threshold_dur", "cooldown_s", "fallback_s", "pump_flow_ml_s"];

const state = { zones: {}, gateway: false, mqtt: false, model: null, hours: 6 };
const cards = {};
const minis = {};

const $ = (sel, root = document) => root.querySelector(sel);
const fmtTime = (iso) => (iso ? new Date(iso).toLocaleTimeString("vi-VN", { hour12: false }) : "–");
const fmtNum = (v, unit = "", d = 1) => (v == null ? "–" : `${Number(v).toFixed(d)}${unit}`);
const mean = (xs) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : null);

const toLogin = () => location.assign(`/login?next=${encodeURIComponent(location.pathname)}`);

async function api(method, path, body) {
  const res = await fetch(path, {
    method, headers: body ? { "Content-Type": "application/json" } : {}, body: body && JSON.stringify(body),
  });
  if (res.status === 401) { toLogin(); throw new Error("Phiên đăng nhập đã hết"); }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(Array.isArray(data.detail) ? data.detail.map((d) => d.msg || d).join("; ") : data.detail || res.statusText);
  return data;
}

// ---------- thẻ Zone ----------
function buildCards() {
  const tpl = $("#zone-tpl");
  for (const z of ZONES) {
    const mini = $("#mini-tpl").content.firstElementChild.cloneNode(true);
    mini.href = `#/zones/${z}`;
    mini.style.setProperty("--zc", COLORS[z - 1]);
    $("#minis").append(mini);
    minis[z] = mini;

    const el = tpl.content.firstElementChild.cloneNode(true);
    el.style.setProperty("--zc", COLORS[z - 1]);
    el.id = `zone-${z}`;
    $("#zones").append(el);
    cards[z] = el;
    const water = (dur) => act(z, () => api("POST", `/api/zones/${z}/water`, { dur }), `Đã gửi lệnh tưới ${dur} s`);
    $("[data-c=mode]", el).addEventListener("click", (e) => {
      const mode = e.target.closest("button")?.dataset.v;
      if (mode && mode !== state.zones[z]?.mode) saveZone(z, { mode });
    });
    el.querySelectorAll("[data-c=preset]").forEach((b) => b.addEventListener("click", () => {
      $("[data-c=dur]", el).value = b.dataset.s;
      water(Number(b.dataset.s));
    }));
    $("[data-c=water]", el).addEventListener("click", () => water(Number($("[data-c=dur]", el).value)));
    $("[data-c=stop]", el).addEventListener("click", () => act(z, () => api("POST", `/api/zones/${z}/stop`), "Đã gửi lệnh dừng"));
    $("[data-c=cfg]", el).addEventListener("submit", (e) => {
      e.preventDefault();
      const f = e.target;
      const body = { collect_mode: f.collect_mode.checked };
      for (const k of CFG_KEYS) body[k] = Number(f[k].value);
      saveZone(z, body);
    });
  }
}

function say(z, text, bad = false) {
  const m = $("[data-f=msg]", cards[z]);
  m.textContent = text;
  m.classList.toggle("bad", bad);
}

async function act(z, fn, okText) {
  try { await fn(); say(z, okText); } catch (e) { say(z, e.message, true); }
}

async function saveZone(z, body) {
  try {
    const r = await api("PUT", `/api/zones/${z}`, body);
    applyZone(r);
    say(z, r.cfg_sent ? "Đã lưu, đã gửi cfg xuống nút" : "Đã lưu (gateway offline, cfg sẽ gửi khi online)");
  } catch (e) {
    say(z, e.message, true);
    renderZone(z);
  }
}

function applyZone(snap) {
  state.zones[snap.id] = { ...state.zones[snap.id], ...snap };
  renderZone(snap.id);
}

function soilState(s) {
  const soil = s.tel?.soil;
  if (!s.online) return ["off", "Offline"];
  if (soil == null) return ["", "Chưa có dữ liệu"];
  if (s.soil_low != null && soil < s.soil_low) return ["dry", "Khô · cần tưới"];
  if (s.soil_target != null && soil >= s.soil_target) return ["wet", "Đủ nước"];
  return ["ok", "Ổn định"];
}

function renderZone(z) {
  const s = state.zones[z];
  const el = cards[z];
  if (!s || !el) return;
  const tel = s.tel || {};
  const set = (f, v) => ($(`[data-f=${f}]`, el).textContent = v);
  set("name", s.name);
  const online = $("[data-f=online]", el);
  online.textContent = s.online ? "Online" : "Offline";
  online.classList.toggle("on", !!s.online);
  el.classList.toggle("offline", !s.online);
  $(`[data-zdot="${z}"]`).className = `zdot ${s.pump ? "pump" : s.online ? "on" : "off"}`;
  set("last_seen", s.last_seen ? `thấy lúc ${fmtTime(s.last_seen)}` : "chưa thấy nút");
  const [cls, label] = soilState(s);
  const badge = $("[data-f=state]", el);
  badge.className = `badge ${cls}`;
  badge.textContent = label;

  set("soil", fmtNum(tel.soil, "", 0));
  const pct = (v) => `${Math.min(100, Math.max(0, Number(v) || 0))}%`;
  const gauge = $("[data-f=gauge]", el);
  gauge.style.setProperty("--v", tel.soil == null ? 0 : Math.min(100, Math.max(0, tel.soil)) / 100);
  gauge.style.setProperty("--low", pct(s.soil_low));
  gauge.style.setProperty("--target", pct(s.soil_target));
  gauge.classList.toggle("dry", cls === "dry");
  set("low", fmtNum(s.soil_low, " %", 0));
  set("target", fmtNum(s.soil_target, " %", 0));

  set("temp", fmtNum(tel.temp, " °C"));
  set("hum", fmtNum(tel.hum, " %"));
  set("rssi", tel.rssi == null ? "–" : `${tel.rssi} dBm`);
  set("node_mode", tel.mode || tel.node_mode || "–");
  const pump = $("[data-f=pump]", el);
  pump.textContent = s.pump ? "Đang tưới" : "Đang tắt";
  pump.classList.toggle("on", !!s.pump);

  const p = s.last_prediction;
  set("prediction", p ? `${fmtTime(p.ts)} · ${MODE_VI[s.mode] || s.mode}: ${ACTION_VI[p.action_taken] || p.action_taken}` +
    (p.p_need != null ? ` · p=${p.p_need.toFixed(2)}` : "") + (p.dur ? ` · ${p.dur} s` : "") : "Chưa có quyết định");

  for (const b of $("[data-c=mode]", el).children) b.setAttribute("aria-pressed", b.dataset.v === s.mode);
  $("[data-c=dur]", el).max = s.pump_max_s;
  const f = $("[data-c=cfg]", el);
  if (!f.contains(document.activeElement)) {
    for (const k of CFG_KEYS) f[k].value = s[k];
    f.collect_mode.checked = s.collect_mode;
  }
  const canSend = state.gateway && s.online;
  $("[data-c=water]", el).disabled = !canSend;
  el.querySelectorAll("[data-c=preset]").forEach((b) => { b.disabled = !canSend || Number(b.dataset.s) > s.pump_max_s; });
  $("[data-c=stop]", el).disabled = !state.gateway;
  renderMini(z, s, cls, label);
  renderSummary();
}

// Thẻ tóm tắt ở trang Tổng quan: chép lại những gì thẻ điều khiển đã tính
function renderMini(z, s, cls, label) {
  const el = minis[z];
  const tel = s.tel || {};
  const set = (f, v) => ($(`[data-f=${f}]`, el).textContent = v);
  set("name", s.name);
  const online = $("[data-f=online]", el);
  online.textContent = s.online ? "Online" : "Offline";
  online.classList.toggle("on", !!s.online);
  el.classList.toggle("offline", !s.online);
  set("mode", MODE_VI[s.mode] || s.mode || "–");
  const badge = $("[data-f=state]", el);
  badge.className = `badge ${cls}`;
  badge.textContent = label;
  set("soil", fmtNum(tel.soil, "", 0));
  const src = $("[data-f=gauge]", cards[z]);
  const gauge = $("[data-f=gauge]", el);
  gauge.style.cssText = src.style.cssText;
  gauge.className = src.className;
  set("air", `${fmtNum(tel.temp, " °C")} · ${fmtNum(tel.hum, " %")}`);
  const pump = $("[data-f=pump]", el);
  pump.textContent = s.pump ? "Đang tưới" : "Đang tắt";
  pump.classList.toggle("on", !!s.pump);
}

// ---------- tổng quan (banner + KPI) ----------
let water = [];

function renderSummary() {
  const zs = ZONES.map((z) => state.zones[z]).filter(Boolean);
  const tels = zs.filter((s) => s.online && s.tel).map((s) => s.tel);
  const online = zs.filter((s) => s.online).length;
  const pumps = zs.filter((s) => s.pump).length;
  const soils = tels.map((t) => t.soil).filter((v) => v != null);
  const dry = zs.filter((s) => soilState(s)[0] === "dry").length;

  $("#k-online").textContent = zs.length ? online : "–";
  const os = $("#k-online-sub");
  os.textContent = !zs.length ? "Đang chờ dữ liệu" : online === zs.length ? "Tất cả nút đang hoạt động" : `${zs.length - online} nút mất kết nối`;
  os.classList.toggle("warn", zs.length > 0 && online < zs.length);

  $("#k-pump").textContent = zs.length ? pumps : "–";
  $("#k-pump-sub").textContent = pumps ? zs.filter((s) => s.pump).map((s) => s.name).join(", ") : "Không có vùng nào đang tưới";

  $("#k-soil").textContent = fmtNum(mean(soils), "%");
  const ss = $("#k-soil-sub");
  ss.textContent = !soils.length ? "Chưa có số đo" : dry ? `${dry} vùng dưới ngưỡng tưới` : "Các vùng đều trên ngưỡng";
  ss.classList.toggle("warn", dry > 0);
}

function renderWater() {
  const ml = water.map((w) => w.water_ml).filter((v) => v != null);
  const times = water.reduce((a, w) => a + w.waterings, 0);
  const secs = water.reduce((a, w) => a + w.water_s, 0);
  $("#k-water").textContent = ml.length ? (ml.reduce((a, b) => a + b, 0) / 1000).toFixed(2) : "–";
  $("#k-water-sub").textContent = `${times} lần tưới · ${Math.round(secs)} s bơm` + (ml.length < water.length ? " · thiếu lưu lượng" : "");
  waterChart.data.datasets[0].data = ZONES.map((z) => {
    const w = water.find((x) => x.zone_id === z);
    return w?.water_ml != null ? w.water_ml / 1000 : 0;
  });
  waterChart.update();
}

async function loadWater() {
  const from = new Date();
  from.setHours(0, 0, 0, 0);
  try {
    water = await api("GET", `/api/stats?from=${from.toISOString()}&to=${new Date().toISOString()}`);
    renderWater();
  } catch { /* giữ số cũ */ }
}

function renderPills(wsOk) {
  const pill = (id, ok, text) => {
    const p = $(id);
    $("b", p).textContent = text;
    p.className = `stat ${ok ? "ok" : "bad"}`;
  };
  if (wsOk !== undefined) pill("#pill-ws", wsOk, wsOk ? "Đã kết nối" : "Mất kết nối");
  pill("#pill-mqtt", state.mqtt, state.mqtt ? "Online" : "Mất kết nối");
  pill("#pill-gw", state.gateway, state.gateway ? "Online" : "Offline");
  pill("#pill-model", !!state.model, state.model || "Chưa có");
  $("#stop-all").disabled = !state.gateway;
}

// ---------- biểu đồ ----------
Chart.defaults.font.family = getComputedStyle(document.body).fontFamily;
Chart.defaults.font.size = 11;
Chart.defaults.color = "#4b5563";
Chart.defaults.borderColor = "rgba(0,0,0,0.05)";
Object.assign(Chart.defaults.plugins.tooltip, {
  backgroundColor: css("--deep"), padding: 10, cornerRadius: 10, boxPadding: 4, usePointStyle: true,
});

const timeTicks = (hours) => ({
  callback: (v) => new Date(v).toLocaleString("vi-VN", hours > 24
    ? { day: "2-digit", month: "2-digit", hour: "2-digit", hour12: false }
    : { hour: "2-digit", minute: "2-digit", hour12: false }),
  maxTicksLimit: 8,
});

function lineChart(canvas, datasets, yTitle) {
  return new Chart(canvas, {
    type: "line",
    data: { datasets },
    options: {
      animation: false, maintainAspectRatio: false, parsing: false, normalized: true,
      interaction: { mode: "nearest", intersect: false },
      scales: {
        x: { type: "linear", ticks: timeTicks(6), border: { display: false } },
        y: { title: { display: !!yTitle, text: yTitle }, border: { display: false } },
      },
      plugins: {
        legend: {
          labels: {
            usePointStyle: true, pointStyle: "circle", boxWidth: 8, padding: 14, color: css("--deep"), font: { weight: "bold" },
            filter: (l, d) => d.datasets[l.datasetIndex].type !== "scatter",
          },
        },
        tooltip: { callbacks: { title: (items) => new Date(items[0].parsed.x).toLocaleString("vi-VN", { hour12: false }) } },
      },
      elements: { point: { radius: 0, hoverRadius: 4 }, line: { borderWidth: 2.5, tension: 0.35 } },
    },
  });
}

const tint = (hex, a) => hex + Math.round(a * 255).toString(16).padStart(2, "0");
const soilChart = lineChart($("#soil-chart"), [
  ...ZONES.map((z) => ({
    label: `Zone ${z}`, data: [], borderColor: COLORS[z - 1], backgroundColor: COLORS[z - 1],
    fill: z === 1 && { target: "origin", above: tint(COLORS[z - 1], 0.08) },
  })),
  ...ZONES.map((z) => ({
    label: `Tưới Z${z}`, type: "scatter", data: [], borderColor: COLORS[z - 1], backgroundColor: COLORS[z - 1],
    pointStyle: "triangle", pointRadius: 6,
  })),
], "%");
const airChart = lineChart($("#air-chart"), [
  { label: "Nhiệt độ (°C)", data: [], borderColor: css("--temp"), backgroundColor: css("--temp") },
  { label: "Độ ẩm KK (%)", data: [], borderColor: css("--hum"), backgroundColor: css("--hum"), borderDash: [4, 4] },
], "");
const waterChart = new Chart($("#water-chart"), {
  type: "bar",
  data: {
    labels: ZONES.map((z) => `Zone ${z}`),
    datasets: [{ label: "Lượng nước (lít)", data: ZONES.map(() => 0), backgroundColor: COLORS, borderRadius: 12, maxBarThickness: 56 }],
  },
  options: {
    maintainAspectRatio: false,
    plugins: { legend: { display: false }, tooltip: { callbacks: { label: (c) => ` ${c.parsed.y.toFixed(2)} lít` } } },
    scales: {
      x: { grid: { display: false }, border: { display: false }, ticks: { font: { weight: "bold" } } },
      y: { beginAtZero: true, border: { display: false } },
    },
  },
});

async function loadCharts() {
  const to = new Date();
  const from = new Date(to - state.hours * 3600e3);
  const q = `from=${from.toISOString()}&to=${to.toISOString()}`;
  const [{ rows }, events] = await Promise.all([api("GET", `/api/readings?${q}`), api("GET", `/api/events?${q}`)]);
  for (const z of ZONES) {
    const zr = rows.filter((r) => r.zone_id === z);
    soilChart.data.datasets[z - 1].data = zr.map((r) => ({ x: Date.parse(r.ts), y: r.soil }));
    soilChart.data.datasets[z + 2].data = events.filter((e) => e.zone_id === z)
      .map((e) => ({ x: Date.parse(e.start_ts), y: e.soil_before ?? 0, dur: e.duration_s }));
  }
  const az = Number($("#air-zone").value);
  const ar = rows.filter((r) => r.zone_id === az);
  airChart.data.datasets[0].data = ar.map((r) => ({ x: Date.parse(r.ts), y: r.temp }));
  airChart.data.datasets[1].data = ar.map((r) => ({ x: Date.parse(r.ts), y: r.hum }));
  for (const c of [soilChart, airChart]) {
    c.options.scales.x.min = from.getTime();
    c.options.scales.x.max = to.getTime();
    c.options.scales.x.ticks = timeTicks(state.hours);
    c.update();
  }
}

function pushTel(z, tel) {
  if (state.hours !== 6 || tel.soil == null) return;  // 24 giờ / 7 ngày đã gom nhóm: chỉ tải lại định kỳ
  const x = Date.parse(tel.ts);
  const from = x - state.hours * 3600e3;
  const add = (ds, y) => { ds.data.push({ x, y }); while (ds.data.length && ds.data[0].x < from) ds.data.shift(); };
  add(soilChart.data.datasets[z - 1], tel.soil);
  if (z === Number($("#air-zone").value)) {
    add(airChart.data.datasets[0], tel.temp);
    add(airChart.data.datasets[1], tel.hum);
  }
  for (const c of [soilChart, airChart]) { c.options.scales.x.min = from; c.options.scales.x.max = x; c.update("none"); }
}

// ---------- nhật ký ----------
const STATUS_CLASS = (s) => (s === "ok" ? "st-ok" : s === "pending" ? "st-pending" : "st-bad");
const commands = new Map();

function renderCommands() {
  const rows = [...commands.values()].sort((a, b) => Date.parse(b.created_ts) - Date.parse(a.created_ts)).slice(0, 30);
  $("#cmd-log").innerHTML = rows.map((c) => `<tr><td>${fmtTime(c.created_ts)}</td><td>Zone ${c.zone_id}</td>` +
    `<td>${c.type}${c.dur ? ` ${c.dur} s` : ""}${c.force ? " (force)" : ""}</td><td class="num">${c.seq}</td>` +
    `<td>${c.decided_by}</td><td><span class="chip ${STATUS_CLASS(c.status)}">${c.status}</span></td></tr>`).join("");
  const pending = [...commands.values()].filter((c) => c.status === "pending").length;
  setCount("#nav-pending", pending, "warn");
}

function setCount(sel, n, cls = "") {
  const el = $(sel);
  el.hidden = !n;
  el.textContent = n > 99 ? "99+" : n;
  el.className = `count ${cls}`;
}

// level: "" | "warn" | "bad" — đổi màu nhãn chủ đề trong terminal
function logEvent(topic, text, level = "") {
  const li = document.createElement("li");
  const time = document.createElement("time");
  const t = document.createElement("span");
  time.textContent = `[${fmtTime(new Date().toISOString())}]`;
  t.className = "t";
  t.textContent = ` [${topic}] `;
  li.append(time, t, text);
  if (level) li.className = level;
  const log = $("#event-log");
  log.append(li);
  while (log.children.length > 100) log.firstChild.remove();
  log.scrollTop = log.scrollHeight;
  setCount("#nav-events", log.children.length);
}

// ---------- sidebar ----------
function initSidebar() {
  const shell = $("#shell");
  const menu = $("#menu");
  const setOpen = (open) => { shell.classList.toggle("open", open); menu.setAttribute("aria-expanded", open); };
  menu.addEventListener("click", () => setOpen(!shell.classList.contains("open")));
  $("#backdrop").addEventListener("click", () => setOpen(false));
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") setOpen(false); });
  $("#sidebar").addEventListener("click", (e) => { if (e.target.closest("a")) setOpen(false); });

  // Thu gọn chỉ là tiện ích cho từng trình duyệt, nhớ bằng localStorage
  try { shell.classList.toggle("collapsed", localStorage.getItem("sidebar") === "collapsed"); } catch { /* bỏ qua */ }
  $("#collapse").addEventListener("click", () => {
    const c = shell.classList.toggle("collapsed");
    $("#collapse").setAttribute("aria-label", c ? "Mở rộng thanh bên" : "Thu gọn thanh bên");
    try { localStorage.setItem("sidebar", c ? "collapsed" : ""); } catch { /* bỏ qua */ }
  });

}

// ---------- router: #/, #/zones, #/zones/2, #/charts, #/commands, #/events ----------
function route() {
  const [name = "", zone] = location.hash.replace(/^#\/?/, "").split("/");
  const page = document.querySelector(`.page[data-page="${name || "overview"}"]`) || $(".page[data-page=overview]");
  const id = page.dataset.page;
  document.querySelectorAll(".page").forEach((p) => { p.hidden = p !== page; });
  document.querySelectorAll(".nav-item").forEach((a) => {
    const on = a.dataset.route === id;
    a.classList.toggle("active", on);
    if (on) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current");
  });
  document.querySelectorAll(".nav-sub a").forEach((a) => a.classList.toggle("active", id === "zones" && a.dataset.zone === zone));
  $("#page-title").textContent = page.dataset.title;
  document.title = `${page.dataset.title} · Tưới cây đa vùng`;

  // Chart.js không đo được kích thước khi canvas đang ẩn: vẽ lại sau khi trang hiện ra
  if (id === "charts") requestAnimationFrame(() => [soilChart, waterChart, airChart].forEach((c) => c.resize()));
  const card = id === "zones" && cards[Number(zone)];
  if (card) {
    requestAnimationFrame(() => card.scrollIntoView({ behavior: "smooth", block: "start" }));
    card.classList.remove("flash");
    void card.offsetWidth;  // chạy lại animation khi bấm cùng zone
    card.classList.add("flash");
  } else {
    window.scrollTo(0, 0);
  }
}

// ---------- WebSocket ----------
function onMessage({ type, zone, data }) {
  switch (type) {
    case "snapshot":
      state.gateway = data.gateway.online; state.mqtt = data.mqtt; state.model = data.model;
      for (const s of data.zones) applyZone(s);
      break;
    case "tel":
      state.zones[zone].tel = data; state.zones[zone].last_seen = data.ts; state.zones[zone].online = true;
      state.zones[zone].pump = !!data.pump;
      renderZone(zone); pushTel(zone, data);
      break;
    case "pump":
      state.zones[zone].pump = !!data.on;
      renderZone(zone);
      logEvent(data.on ? "PUMP_ON" : "PUMP_OFF",
        `Zone ${zone}: bơm ${data.on ? "bật" : `tắt sau ${data.event.duration_s ?? "?"} s`} (${data.event.decided_by})`,
        data.on ? "" : "warn");
      if (!data.on) { loadCharts(); loadWater(); }
      break;
    case "node":
      state.zones[zone].online = data.online; renderZone(zone);
      logEvent("NODE", `Zone ${zone}: nút ${data.online ? "online" : "offline"}`, data.online ? "" : "bad");
      break;
    case "gateway":
      state.gateway = data.online; ZONES.forEach(renderZone); renderPills();
      logEvent("GATEWAY", `Gateway ${data.online ? "online" : "offline"}`, data.online ? "" : "bad");
      break;
    case "prediction":
      state.zones[zone].last_prediction = data; renderZone(zone);
      break;
    case "zone":
      applyZone(data);
      break;
    case "command":
      commands.set(`${data.zone_id}/${data.seq}`, data); renderCommands();
      logEvent("MQTT_OUT", `Zone ${data.zone_id}: ${data.type}${data.dur ? ` ${data.dur} s` : ""} seq=${data.seq} (${data.decided_by})`);
      break;
    case "ack": {
      const c = commands.get(`${zone}/${data.seq}`);
      if (c) { Object.assign(c, data); renderCommands(); }
      break;
    }
  }
  renderPills();
}

function connect() {
  const ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
  ws.onopen = () => { renderPills(true); loadCharts(); loadWater(); logEvent("SYSTEM", "Đã kết nối WebSocket tới server"); };
  ws.onmessage = (e) => onMessage(JSON.parse(e.data));
  ws.onclose = (e) => {
    if (e.code === 4401) return toLogin();
    renderPills(false);
    logEvent("SYSTEM", "Mất kết nối WebSocket, thử lại sau 3 s", "bad");
    setTimeout(connect, 3000);
  };
}

// ---------- khởi động ----------
buildCards();
initSidebar();
addEventListener("hashchange", route);
route();
// Nút đăng xuất chỉ hiện khi server bật đăng nhập (DASH_PASS)
api("GET", "/api/health").then((h) => { $("#logout").hidden = !h.auth; }).catch(() => {});
$("#range").addEventListener("click", (e) => {
  const h = Number(e.target.dataset.h);
  if (!h) return;
  state.hours = h;
  for (const b of $("#range").children) b.classList.toggle("on", b === e.target);
  loadCharts();
});
$("#air-zone").addEventListener("change", loadCharts);
$("#clear-log").addEventListener("click", () => { $("#event-log").innerHTML = ""; setCount("#nav-events", 0); });
$("#stop-all").addEventListener("click", async () => {
  const res = await Promise.allSettled(ZONES.map((z) => api("POST", `/api/zones/${z}/stop`)));
  const failed = res.filter((r) => r.status === "rejected").length;
  logEvent("STOP_ALL", failed ? `Gửi lệnh dừng lỗi ở ${failed} vùng` : "Đã gửi lệnh dừng cho cả 3 vùng", failed ? "bad" : "warn");
});
api("GET", "/api/commands?limit=30").then((rows) => { rows.forEach((c) => commands.set(`${c.zone_id}/${c.seq}`, c)); renderCommands(); });
setInterval(() => { if (state.hours !== 6) loadCharts(); }, 300e3);
setInterval(loadWater, 300e3);
connect();
