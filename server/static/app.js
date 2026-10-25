// Dashboard: REST /api/* + WebSocket /ws (docs/SERVER.md §Dashboard)
const ZONES = [1, 2, 3];
const COLORS = ZONES.map((z) => getComputedStyle(document.documentElement).getPropertyValue(`--z${z}`).trim());
const MODE_VI = { MANUAL: "Thủ công", THRESHOLD: "Ngưỡng", AI: "AI" };
const ACTION_VI = {
  water: "tưới", none: "không cần tưới", skip_manual: "chế độ thủ công", skip_offline: "nút offline",
  skip_stale: "dữ liệu cũ", skip_sensor: "lỗi cảm biến đất", skip_no_model: "chưa có model",
  skip_pump_on: "bơm đang chạy", skip_cooldown: "đang cooldown", skip_gw_offline: "gateway offline",
};

const state = { zones: {}, gateway: false, mqtt: false, model: null, hours: 6 };
const cards = {};

const $ = (sel, root = document) => root.querySelector(sel);
const fmtTime = (iso) => (iso ? new Date(iso).toLocaleTimeString("vi-VN", { hour12: false }) : "–");
const fmtNum = (v, unit = "", d = 1) => (v == null ? "–" : `${Number(v).toFixed(d)}${unit}`);

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
    const el = tpl.content.firstElementChild.cloneNode(true);
    el.style.setProperty("--zc", COLORS[z - 1]);
    $("#zones").append(el);
    cards[z] = el;
    $("[data-c=mode]", el).addEventListener("change", (e) => saveZone(z, { mode: e.target.value }));
    $("[data-c=water]", el).addEventListener("click", () => act(z, () =>
      api("POST", `/api/zones/${z}/water`, { dur: Number($("[data-c=dur]", el).value) }), "Đã gửi lệnh tưới"));
    $("[data-c=stop]", el).addEventListener("click", () => act(z, () => api("POST", `/api/zones/${z}/stop`), "Đã gửi lệnh dừng"));
    $("[data-c=cfg]", el).addEventListener("submit", (e) => {
      e.preventDefault();
      const f = e.target;
      const body = { collect_mode: f.collect_mode.checked };
      for (const k of ["soil_low", "soil_target", "pump_max_s", "threshold_dur", "cooldown_s", "fallback_s", "pump_flow_ml_s"]) {
        body[k] = Number(f[k].value);
      }
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

function renderZone(z) {
  const s = state.zones[z];
  const el = cards[z];
  if (!s || !el) return;
  const tel = s.tel || {};
  const set = (f, v) => ($(`[data-f=${f}]`, el).textContent = v);
  set("name", s.name);
  const dot = $("[data-f=online]", el);
  dot.textContent = s.online ? "online" : "offline";
  dot.classList.toggle("on", !!s.online);
  set("soil", fmtNum(tel.soil));
  set("temp", fmtNum(tel.temp, " °C"));
  set("hum", fmtNum(tel.hum, " %"));
  const pump = $("[data-f=pump]", el);
  pump.textContent = s.pump ? "ĐANG BẬT" : "tắt";
  pump.classList.toggle("pump-on", !!s.pump);
  set("node_mode", tel.mode || tel.node_mode || "–");
  set("rssi", tel.rssi == null ? "–" : `${tel.rssi} dBm`);
  set("last_seen", fmtTime(s.last_seen));
  const p = s.last_prediction;
  set("prediction", p ? `${fmtTime(p.ts)} ${ACTION_VI[p.action_taken] || p.action_taken}` +
    (p.p_need != null ? ` · p=${p.p_need.toFixed(2)}` : "") + (p.dur ? ` · ${p.dur} s` : "") : "–");
  $("[data-c=mode]", el).value = s.mode;
  $("[data-c=dur]", el).max = s.pump_max_s;
  const f = $("[data-c=cfg]", el);
  if (!f.contains(document.activeElement)) {
    for (const k of ["soil_low", "soil_target", "pump_max_s", "threshold_dur", "cooldown_s", "fallback_s", "pump_flow_ml_s"]) f[k].value = s[k];
    f.collect_mode.checked = s.collect_mode;
  }
  const canSend = state.gateway && s.online;
  $("[data-c=water]", el).disabled = !canSend;
  $("[data-c=stop]", el).disabled = !state.gateway;
}

function renderPills(wsOk) {
  const pill = (id, ok, text) => {
    const p = $(id);
    p.textContent = text;
    p.className = `pill ${ok ? "ok" : "bad"}`;
  };
  if (wsOk !== undefined) pill("#pill-ws", wsOk, wsOk ? "Dashboard: đã kết nối" : "Dashboard: mất kết nối");
  pill("#pill-mqtt", state.mqtt, `MQTT: ${state.mqtt ? "OK" : "mất"}`);
  pill("#pill-gw", state.gateway, `Gateway: ${state.gateway ? "online" : "offline"}`);
  pill("#pill-model", !!state.model, `Model: ${state.model || "chưa có"}`);
}

// ---------- biểu đồ ----------
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
      scales: { x: { type: "linear", ticks: timeTicks(6) }, y: { title: { display: true, text: yTitle } } },
      plugins: {
        tooltip: { callbacks: { title: (items) => new Date(items[0].parsed.x).toLocaleString("vi-VN", { hour12: false }) } },
      },
      elements: { point: { radius: 0 }, line: { borderWidth: 2, tension: 0.2 } },
    },
  });
}

const soilChart = lineChart($("#soil-chart"), [
  ...ZONES.map((z) => ({ label: `Zone ${z}`, data: [], borderColor: COLORS[z - 1], backgroundColor: COLORS[z - 1] })),
  ...ZONES.map((z) => ({
    label: `Tưới Z${z}`, type: "scatter", data: [], borderColor: COLORS[z - 1], backgroundColor: COLORS[z - 1],
    pointStyle: "triangle", pointRadius: 7,
  })),
], "%");
const airChart = lineChart($("#air-chart"), [
  { label: "Nhiệt độ (°C)", data: [], borderColor: "#dc2626", backgroundColor: "#dc2626" },
  { label: "Độ ẩm KK (%)", data: [], borderColor: "#0891b2", backgroundColor: "#0891b2", borderDash: [5, 4] },
], "");

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
  $("#cmd-log").innerHTML = rows.map((c) => `<tr><td>${fmtTime(c.created_ts)}</td><td>${c.zone_id}</td>` +
    `<td>${c.type}${c.dur ? ` ${c.dur} s` : ""}${c.force ? " (force)" : ""}</td><td>${c.seq}</td>` +
    `<td>${c.decided_by}</td><td class="${STATUS_CLASS(c.status)}">${c.status}</td></tr>`).join("");
}

function logEvent(text) {
  const li = document.createElement("li");
  li.textContent = `${fmtTime(new Date().toISOString())} ${text}`;
  $("#event-log").prepend(li);
  while ($("#event-log").children.length > 50) $("#event-log").lastChild.remove();
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
      logEvent(`Zone ${zone}: bơm ${data.on ? "BẬT" : `TẮT sau ${data.event.duration_s ?? "?"} s`} (${data.event.decided_by})`);
      if (!data.on) loadCharts();
      break;
    case "node":
      state.zones[zone].online = data.online; renderZone(zone);
      logEvent(`Zone ${zone}: nút ${data.online ? "online" : "OFFLINE"}`);
      break;
    case "gateway":
      state.gateway = data.online; ZONES.forEach(renderZone); renderPills();
      logEvent(`Gateway ${data.online ? "online" : "OFFLINE"}`);
      break;
    case "prediction":
      state.zones[zone].last_prediction = data; renderZone(zone);
      break;
    case "zone":
      applyZone(data);
      break;
    case "command":
      commands.set(`${data.zone_id}/${data.seq}`, data); renderCommands();
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
  ws.onopen = () => { renderPills(true); loadCharts(); };
  ws.onmessage = (e) => onMessage(JSON.parse(e.data));
  ws.onclose = (e) => {
    if (e.code === 4401) return toLogin();
    renderPills(false);
    setTimeout(connect, 3000);
  };
}

// ---------- khởi động ----------
buildCards();
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
api("GET", "/api/commands?limit=30").then((rows) => { rows.forEach((c) => commands.set(`${c.zone_id}/${c.seq}`, c)); renderCommands(); });
setInterval(() => { if (state.hours !== 6) loadCharts(); }, 300e3);
connect();
