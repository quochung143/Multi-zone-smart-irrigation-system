"""Phần mềm cấu hình gateway qua USB: Wi-Fi, broker MQTT (LAN hoặc Internet), xem trạng thái.

Thay cho gõ lệnh trên console "gw>" (docs/SERVER.md §8.3, §14). Dùng các lệnh của firmware:
- đọc: net_json, status_json, wifi_scan (in 1 dòng "@<json>")
- ghi: wifi, broker, broker_ip, mqtt_user rồi restart (giống gõ tay)

Chạy:   python server/tools/gw_config.py
Đóng gói .exe: server/tools/build_gw_config.ps1
"""
import json
import queue
import re
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

BAUD = 115200
TIMEOUT_S = 4.0
SCAN_TIMEOUT_S = 8.0
INTERNET_URL = "wss://mqtt.quochung.xyz"
LAN_PORT = 1883
POLL_MS = 3000


def quote(s: str) -> str:
    """Tham số cho esp_console: luôn đặt trong ngoặc kép, thoát \\ và \"."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# Console gateway in lại (echo) lệnh vừa gõ: che mật khẩu trước khi hiện lên khung log
_SECRET = re.compile(r'((?:wifi|mqtt_user)\s+"(?:[^"\\]|\\.)*"\s+)".*"')


def mask(line: str) -> str:
    return _SECRET.sub(r'\1"***"', line)


class Gateway:
    """Cổng COM tới console gw>: một luồng đọc dòng, lệnh gửi tuần tự (khoá)."""

    def __init__(self, port: str, on_line):
        # Giữ DTR/RTS ở mức nghỉ để Windows không reset gateway (giống gw_bridge.py)
        self.ser = serial.serial_for_url(port, baudrate=BAUD, timeout=0.2, do_not_open=True)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.lines: "queue.Queue[str]" = queue.Queue()
        self.on_line = on_line
        self.lock = threading.Lock()
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while self.alive:
            try:
                buf += self.ser.read(256)
            except (serial.SerialException, OSError, AttributeError, TypeError):
                # AttributeError/TypeError: cổng vừa bị close() ở luồng giao diện
                if self.alive:
                    self.alive = False
                    self.on_line("!! Mất kết nối cổng COM")
                break
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip("\r")
                self.lines.put(line)
                self.on_line(line)

    def close(self):
        self.alive = False
        try:
            self.ser.close()
        except serial.SerialException:
            pass

    def _send(self, cmd: str):
        while not self.lines.empty():
            self.lines.get_nowait()
        self.ser.write((cmd + "\n").encode("utf-8"))

    def query(self, cmd: str, timeout: float = TIMEOUT_S):
        """Gửi lệnh *_json/wifi_scan, trả về JSON của dòng "@..." đầu tiên."""
        with self.lock:
            self._send(cmd)
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                try:
                    line = self.lines.get(timeout=0.1)
                except queue.Empty:
                    continue
                i = line.find("@{") if "@{" in line else line.find("@[")
                if i >= 0:
                    return json.loads(line[i + 1:])
            raise TimeoutError(f"Gateway không trả lời lệnh '{cmd}'. Firmware cũ? Hãy nạp firmware mới.")

    def setcmd(self, cmd: str):
        """Lệnh ghi cấu hình: chờ 'Da luu' hoặc báo lỗi của firmware."""
        with self.lock:
            self._send(cmd)
            end = time.monotonic() + TIMEOUT_S
            while time.monotonic() < end:
                try:
                    line = self.lines.get(timeout=0.1)
                except queue.Empty:
                    continue
                if "Da luu" in line:
                    return
                if "khong hop le" in line or "Cu phap" in line:
                    raise ValueError(f"Gateway từ chối: {cmd.split()[0]} ({line.strip()})")
            raise TimeoutError(f"Gateway không xác nhận lệnh '{cmd.split()[0]}'")

    def restart(self):
        with self.lock:
            self._send("restart")


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Cấu hình Gateway · Tưới cây đa vùng")
        self.minsize(760, 640)
        self.gw: Gateway | None = None
        self.current: dict = {}
        self.ui_q: "queue.Queue" = queue.Queue()
        self._build()
        self.refresh_ports()
        self.after(100, self._drain)
        self.after(POLL_MS, self._poll)
        self.protocol("WM_DELETE_WINDOW", self._quit)

    # ---------- giao diện ----------
    def _build(self):
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Head.TLabel", font=("Segoe UI", 11, "bold"))
        style.configure("Ok.TLabel", foreground="#047857")
        style.configure("Bad.TLabel", foreground="#be123c")
        pad = {"padx": 8, "pady": 4}

        top = ttk.Frame(self, padding=10)
        top.pack(fill="x")
        ttk.Label(top, text="Cổng COM").pack(side="left")
        self.port = ttk.Combobox(top, width=34, state="readonly")
        self.port.pack(side="left", padx=6)
        ttk.Button(top, text="Làm mới", command=self.refresh_ports).pack(side="left")
        self.btn_conn = ttk.Button(top, text="Kết nối", command=self.toggle_connect)
        self.btn_conn.pack(side="left", padx=6)
        self.conn_lbl = ttk.Label(top, text="Chưa kết nối", style="Bad.TLabel")
        self.conn_lbl.pack(side="left", padx=6)

        body = ttk.Frame(self, padding=(10, 0))
        body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=1)

        # Wi-Fi
        wf = ttk.LabelFrame(body, text="Wi-Fi gateway sẽ vào", padding=8)
        wf.grid(row=0, column=0, sticky="nsew", padx=(0, 6), pady=6)
        wf.columnconfigure(1, weight=1)
        ttk.Label(wf, text="Tên mạng").grid(row=0, column=0, sticky="w", **pad)
        self.ssid = ttk.Combobox(wf)
        self.ssid.grid(row=0, column=1, sticky="ew", **pad)
        self.btn_scan = ttk.Button(wf, text="Quét", command=self.scan, state="disabled")
        self.btn_scan.grid(row=0, column=2, **pad)
        ttk.Label(wf, text="Mật khẩu").grid(row=1, column=0, sticky="w", **pad)
        self.wpass = ttk.Entry(wf, show="•")
        self.wpass.grid(row=1, column=1, sticky="ew", **pad)
        self.show_pw = tk.BooleanVar()
        ttk.Checkbutton(wf, text="Hiện", variable=self.show_pw, command=self._toggle_pw).grid(row=1, column=2, **pad)
        self.wpass_hint = ttk.Label(wf, text="", foreground="#5b6b62")
        self.wpass_hint.grid(row=2, column=1, columnspan=2, sticky="w", padx=8)

        # Broker
        bf = ttk.LabelFrame(body, text="Broker MQTT (server)", padding=8)
        bf.grid(row=0, column=1, sticky="nsew", padx=(6, 0), pady=6)
        bf.columnconfigure(1, weight=1)
        self.mode = tk.StringVar(value="lan")
        rb = ttk.Frame(bf)
        rb.grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Radiobutton(rb, text="Cùng mạng (LAN)", value="lan", variable=self.mode, command=self._mode_changed).pack(side="left")
        ttk.Radiobutton(rb, text="Qua Internet", value="inet", variable=self.mode, command=self._mode_changed).pack(side="left", padx=10)
        self.lan = ttk.Frame(bf)
        self.lan.columnconfigure(1, weight=1)
        ttk.Label(self.lan, text="Laptop").grid(row=0, column=0, sticky="w", **pad)
        self.host = ttk.Entry(self.lan)
        self.host.grid(row=0, column=1, sticky="ew", **pad)
        ttk.Label(self.lan, text="Cổng").grid(row=1, column=0, sticky="w", **pad)
        self.lport = ttk.Entry(self.lan, width=8)
        self.lport.grid(row=1, column=1, sticky="w", **pad)
        ttk.Label(self.lan, text="IP dự phòng").grid(row=2, column=0, sticky="w", **pad)
        self.ip = ttk.Entry(self.lan)
        self.ip.grid(row=2, column=1, sticky="ew", **pad)
        ttk.Label(self.lan, text="Ví dụ: TEN-LAPTOP.local hoặc 192.168.1.20", foreground="#5b6b62").grid(
            row=3, column=1, sticky="w", padx=8)
        self.inet = ttk.Frame(bf)
        self.inet.columnconfigure(1, weight=1)
        ttk.Label(self.inet, text="URL").grid(row=0, column=0, sticky="w", **pad)
        self.url = ttk.Entry(self.inet)
        self.url.grid(row=0, column=1, sticky="ew", **pad)
        ttk.Label(self.inet, text="wss://… đi qua Cloudflare Tunnel, có TLS", foreground="#5b6b62").grid(
            row=1, column=1, sticky="w", padx=8)
        acc = ttk.Frame(bf)
        acc.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        acc.columnconfigure(1, weight=1)
        ttk.Label(acc, text="Tài khoản MQTT").grid(row=0, column=0, sticky="w", **pad)
        self.muser = ttk.Entry(acc)
        self.muser.grid(row=0, column=1, sticky="ew", **pad)
        ttk.Label(acc, text="Mật khẩu MQTT").grid(row=1, column=0, sticky="w", **pad)
        self.mpass = ttk.Entry(acc, show="•")
        self.mpass.grid(row=1, column=1, sticky="ew", **pad)
        self.mpass_hint = ttk.Label(acc, text="", foreground="#5b6b62")
        self.mpass_hint.grid(row=2, column=1, sticky="w", padx=8)
        self.host.insert(0, "")
        self.lport.insert(0, str(LAN_PORT))
        self.url.insert(0, INTERNET_URL)
        self.muser.insert(0, "gateway")
        self._mode_changed()

        act = ttk.Frame(body)
        act.grid(row=1, column=0, columnspan=2, sticky="ew", pady=4)
        self.btn_read = ttk.Button(act, text="Đọc cấu hình từ gateway", command=self.read_config, state="disabled")
        self.btn_read.pack(side="left")
        self.btn_save = ttk.Button(act, text="Lưu và khởi động lại gateway", command=self.save, state="disabled")
        self.btn_save.pack(side="right")

        # Trạng thái
        sf = ttk.LabelFrame(body, text="Trạng thái gateway (tự cập nhật 3 giây)", padding=8)
        sf.grid(row=2, column=0, columnspan=2, sticky="nsew", pady=6)
        body.rowconfigure(2, weight=1)
        sf.columnconfigure(1, weight=1)
        sf.columnconfigure(3, weight=1)
        self.st = {}
        for i, (key, label) in enumerate([("wifi", "Wi-Fi"), ("ip", "IP · kênh"), ("mqtt", "MQTT"),
                                          ("server", "Server"), ("broker", "Broker"), ("mac", "MAC")]):
            r, c = divmod(i, 2)
            ttk.Label(sf, text=label).grid(row=r, column=c * 2, sticky="w", padx=8, pady=2)
            v = ttk.Label(sf, text="–")
            v.grid(row=r, column=c * 2 + 1, sticky="w", padx=8, pady=2)
            self.st[key] = v
        self.zones = ttk.Treeview(sf, columns=("zone", "state", "soil", "rssi", "age", "pump"), show="headings", height=3)
        for col, txt, w in [("zone", "Zone", 60), ("state", "Trạng thái", 110), ("soil", "Độ ẩm đất", 90),
                            ("rssi", "RSSI", 80), ("age", "Gói cuối", 90), ("pump", "Bơm", 80)]:
            self.zones.heading(col, text=txt)
            self.zones.column(col, width=w, anchor="center")
        self.zones.grid(row=3, column=0, columnspan=4, sticky="ew", padx=8, pady=(8, 0))

        # Log
        lf = ttk.LabelFrame(self, text="Console gateway", padding=6)
        lf.pack(fill="both", expand=False, padx=10, pady=(0, 10))
        self.log = tk.Text(lf, height=8, font=("Consolas", 9), bg="#0a4225", fg="#d1fae5", insertbackground="#d1fae5")
        self.log.pack(fill="both", expand=True)

    def _toggle_pw(self):
        self.wpass.configure(show="" if self.show_pw.get() else "•")

    def _mode_changed(self):
        (self.inet if self.mode.get() == "lan" else self.lan).grid_forget()
        (self.lan if self.mode.get() == "lan" else self.inet).grid(row=1, column=0, columnspan=2, sticky="ew")

    def _set_conn_ui(self, on: bool):
        state = "normal" if on else "disabled"
        for b in (self.btn_scan, self.btn_read, self.btn_save):
            b.configure(state=state)
        self.btn_conn.configure(text="Ngắt" if on else "Kết nối")
        self.conn_lbl.configure(text="Đã kết nối" if on else "Chưa kết nối", style="Ok.TLabel" if on else "Bad.TLabel")

    # ---------- luồng nền -> giao diện ----------
    def _bg(self, fn, done=None, busy: str | None = None):
        """Chạy lệnh serial ở luồng nền, kết quả trả về luồng giao diện."""
        if busy:
            self.conn_lbl.configure(text=busy, style="TLabel")

        def run():
            try:
                res = fn()
                self.ui_q.put((done, res, None))
            except Exception as e:  # noqa: BLE001 - mọi lỗi đều báo lên giao diện
                self.ui_q.put((done, None, e))
        threading.Thread(target=run, daemon=True).start()

    def _drain(self):
        while not self.ui_q.empty():
            item = self.ui_q.get_nowait()
            if isinstance(item, str):
                self.log.insert("end", mask(item) + "\n")
                self.log.see("end")
                if int(self.log.index("end").split(".")[0]) > 600:
                    self.log.delete("1.0", "200.0")
                continue
            done, res, err = item
            if self.gw:
                self.conn_lbl.configure(text="Đã kết nối", style="Ok.TLabel")
            if err:
                messagebox.showerror("Lỗi", str(err))
            elif done:
                done(res)
        self.after(100, self._drain)

    def _poll(self):
        if self.gw and self.gw.alive and not self.gw.lock.locked():
            self._bg(self._status_or_none, self._show_status_quiet)
        self.after(POLL_MS, self._poll)

    def _status_or_none(self):
        """Tự cập nhật: gateway đang khởi động lại thì im lặng, không bật hộp thoại lỗi."""
        try:
            return self.gw.query("status_json") if self.gw else None
        except (TimeoutError, ValueError, serial.SerialException):
            return None

    # ---------- thao tác ----------
    def refresh_ports(self):
        ports = list_ports.comports()
        self.port_map = {f"{p.device} · {p.description}": p.device for p in ports}
        self.port["values"] = list(self.port_map)
        if ports and not self.port.get():
            # Ưu tiên chip USB-UART hay gặp trên board ESP32
            pick = next((k for k in self.port_map if any(s in k for s in ("CP210", "CH340", "CH910", "USB"))), None)
            self.port.set(pick or list(self.port_map)[0])

    def toggle_connect(self):
        if self.gw:
            self.gw.close()
            self.gw = None
            self._set_conn_ui(False)
            return
        if not self.port.get():
            messagebox.showwarning("Chưa chọn cổng", "Cắm gateway vào USB rồi bấm Làm mới.")
            return
        try:
            self.gw = Gateway(self.port_map[self.port.get()], lambda line: self.ui_q.put(line))
        except serial.SerialException as e:
            messagebox.showerror("Không mở được cổng", f"{e}\n\nĐóng idf.py monitor / phần mềm khác đang dùng cổng này.")
            return
        self._set_conn_ui(True)
        self.read_config()

    def read_config(self):
        self._bg(lambda: (self.gw.query("net_json"), self.gw.query("status_json")), self._show_config,
                 busy="Đang đọc cấu hình…")

    def _show_config(self, res):
        cfg, status = res
        self.current = cfg
        self._fill(self.ssid, cfg.get("ssid", ""))
        self.wpass.delete(0, "end")
        self.wpass_hint.configure(text="Để trống = giữ mật khẩu đã lưu" if cfg.get("has_pass") else "")
        host = cfg.get("host", "")
        if "://" in host:
            self.mode.set("inet")
            self._fill(self.url, host)
        else:
            self.mode.set("lan")
            self._fill(self.host, host)
            self._fill(self.lport, str(cfg.get("port", LAN_PORT)))
            self._fill(self.ip, cfg.get("ip", ""))
        self._mode_changed()
        self._fill(self.muser, cfg.get("user", "gateway"))
        self.mpass.delete(0, "end")
        self.mpass_hint.configure(text="Để trống = giữ mật khẩu đã lưu" if cfg.get("has_mpass") else "")
        self._show_status(status)

    @staticmethod
    def _fill(widget, value: str):
        if isinstance(widget, ttk.Combobox):
            widget.set(value)
        else:
            widget.delete(0, "end")
            widget.insert(0, value)

    def _show_status_quiet(self, status):
        if status:
            self._show_status(status)

    def _show_status(self, s: dict):
        def put(key, text, ok=None):
            self.st[key].configure(text=text, style="TLabel" if ok is None else ("Ok.TLabel" if ok else "Bad.TLabel"))
        put("wifi", f"{s.get('ssid') or '(chưa cấu hình)'} · {'đã nối' if s['wifi'] else 'chưa nối'}", s["wifi"])
        put("ip", f"{s.get('ip') or '–'} · kênh {s.get('channel', '–')}")
        put("mqtt", "Đã nối" if s["mqtt"] else "Chưa nối", s["mqtt"])
        put("server", "Đang chạy" if s["server"] else "Không thấy", s["server"])
        put("broker", s.get("broker") or "–")
        put("mac", s.get("mac", "–"))
        self.zones.delete(*self.zones.get_children())
        for z in s.get("zones", []):
            state = "chưa điền MAC" if not z["configured"] else ("ONLINE" if z["online"] else "offline")
            self.zones.insert("", "end", values=(
                z["zone"], state,
                f"{z['soil']:.1f} %" if "soil" in z else "–",
                f"{z['rssi']} dBm" if "rssi" in z else "–",
                f"{z['age_s']} s trước" if "age_s" in z else "–",
                "BẬT" if z.get("pump") else "tắt"))

    def scan(self):
        def done(aps):
            if isinstance(aps, dict) and "error" in aps:
                messagebox.showwarning("Không quét được", f"{aps['error']}\nGateway đang nối Wi-Fi, thử lại sau vài giây.")
                return
            best: dict[str, dict] = {}
            for ap in aps:
                if ap["ssid"] and (ap["ssid"] not in best or ap["rssi"] > best[ap["ssid"]]["rssi"]):
                    best[ap["ssid"]] = ap
            names = [a["ssid"] for a in sorted(best.values(), key=lambda a: -a["rssi"])]
            self.ssid["values"] = names
            self.ssid.focus_set()
            if not names:
                messagebox.showinfo("Quét Wi-Fi", "Không thấy mạng nào.")
            else:
                self.conn_lbl.configure(text=f"Tìm thấy {len(names)} mạng: bấm mũi tên ở ô Tên mạng để chọn",
                                        style="Ok.TLabel")
        self._bg(lambda: self.gw.query("wifi_scan", SCAN_TIMEOUT_S), done, busy="Đang quét Wi-Fi…")

    def _commands(self) -> list[str]:
        """Từ form -> danh sách lệnh console. Chỉ gửi phần thay đổi; mật khẩu trống = giữ cũ."""
        cur = self.current
        cmds = []
        ssid, wpass = self.ssid.get().strip(), self.wpass.get()
        if not ssid:
            raise ValueError("Chưa nhập tên Wi-Fi.")
        if wpass or ssid != cur.get("ssid"):
            if not wpass and not messagebox.askyesno("Mạng không mật khẩu?",
                                                     f"Chưa nhập mật khẩu cho '{ssid}'.\nĐây là mạng mở (không mật khẩu)?"):
                raise ValueError("Nhập mật khẩu Wi-Fi.")
            cmds.append(f"wifi {quote(ssid)}" + (f" {quote(wpass)}" if wpass else ""))

        if self.mode.get() == "inet":
            url = self.url.get().strip()
            if not url.startswith(("wss://", "mqtts://")):
                raise ValueError("URL broker qua Internet phải bắt đầu bằng wss:// (có mã hoá).")
            if url != cur.get("host"):
                cmds.append(f"broker {quote(url)}")
        else:
            host, port, ip = self.host.get().strip(), self.lport.get().strip() or str(LAN_PORT), self.ip.get().strip()
            if not host or "://" in host:
                raise ValueError("Nhập tên laptop (TEN-LAPTOP.local) hoặc IP.")
            if not port.isdigit():
                raise ValueError("Cổng phải là số.")
            if host != cur.get("host") or int(port) != cur.get("port"):
                cmds.append(f"broker {quote(host)} {port}")
            if ip != cur.get("ip"):
                cmds.append(f"broker_ip {quote(ip or '-')}")

        user, mpass = self.muser.get().strip(), self.mpass.get()
        if mpass or user != cur.get("user"):
            if not mpass:
                raise ValueError("Đổi tài khoản MQTT thì phải nhập cả mật khẩu.")
            cmds.append(f"mqtt_user {quote(user)} {quote(mpass)}")
        elif not cur.get("has_mpass"):
            raise ValueError("Gateway chưa có mật khẩu MQTT: hãy nhập.")
        return cmds

    def save(self):
        try:
            cmds = self._commands()
        except ValueError as e:
            messagebox.showwarning("Kiểm tra lại", str(e))
            return
        if not cmds:
            messagebox.showinfo("Không có thay đổi", "Cấu hình giống hệt trên gateway.")
            return

        def run():
            for c in cmds:
                self.gw.setcmd(c)
            self.gw.restart()
            return len(cmds)

        def done(n):
            messagebox.showinfo("Đã lưu", f"Đã lưu {n} mục, gateway đang khởi động lại.\n"
                                          "Chờ khoảng 10 giây rồi xem phần Trạng thái.")
            self.after(6000, self.read_config)
        self._bg(run, done, busy="Đang lưu…")

    def _quit(self):
        if self.gw:
            self.gw.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
