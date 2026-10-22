"""Công cụ thử gateway khi chưa có server (tuần 10–11).

- In mọi dòng gateway gửi lên (thêm giờ nhận).
- Gửi {"t":"time"} mỗi 60 s để gateway coi server còn sống (gateway mới phát beacon).
- Gõ lệnh để gửi xuống nút:
    water <zone> <giây> [force]   force = tưới thủ công, bỏ qua thời gian nghỉ (cooldown)
    stop <zone>
    cfg <zone> <low%> <max_s> <fb_s>
    raw <json>
    stats                 tổng hợp theo zone: số gói, mất gói, RSSI, online, ACK
    selftest <zone>       kiểm tra ACK ok / rej_invalid / rej_busy / rej_cooldown / force / stop
    hb on|off             bật/tắt heartbeat (tắt để thử nút chuyển FALLBACK)
    quiet on|off          ẩn/hiện dòng tel
    quit

Chạy: python server/tools/gw_test.py COM5 [--log log.jsonl]
"""
import argparse
import json
import threading
import time
from collections import Counter, defaultdict

import serial

HEARTBEAT_S = 60
ACK_WAIT_S = 3.0  # gateway: 3 lần × 300 ms, cộng độ trễ Serial
REPLAY_LIMIT = 5  # số lần một dòng lặp lại liên tiếp thì coi là driver phát lại bộ đệm


class ZoneStats:
    def __init__(self) -> None:
        self.tel = 0
        self.last_tel: dict | None = None
        self.rssi: list[int] = []
        self.online: bool | None = None
        self.gw_stat: dict | None = None
        self.acks: Counter = Counter()
        self.pump_events = 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM5")
    ap.add_argument("--log", help="ghi mọi dòng JSON nhận được ra file .jsonl")
    args = ap.parse_args()

    # Giữ DTR/RTS ở mức nghỉ trước khi mở để Windows không reset gateway (EN/IO0 nối qua DTR/RTS)
    ser = serial.serial_for_url(args.port, baudrate=115200, timeout=1, do_not_open=True)
    ser.dtr = False
    ser.rts = False
    ser.open()
    # Bỏ dữ liệu cũ chip USB-UART còn giữ từ lúc chưa ai mở cổng (driver CP210x có thể phát lại nhiều lần)
    time.sleep(0.2)
    ser.reset_input_buffer()
    log = open(args.log, "a", encoding="utf-8") if args.log else None
    lock = threading.Lock()
    stop = threading.Event()
    hb_enabled = threading.Event()
    hb_enabled.set()
    quiet = threading.Event()
    # seq bắt đầu ngẫu nhiên theo thời gian để không trùng last_cmd_seq còn nhớ trong nút
    seq = {z: int(time.time()) % 50000 for z in (1, 2, 3)}
    zones: dict[int, ZoneStats] = defaultdict(ZoneStats)
    acks: dict[tuple[int, int], str] = {}  # (zone, seq) -> status
    pumps: list[dict] = []
    cond = threading.Condition()

    def send(obj: dict) -> None:
        line = json.dumps(obj, separators=(",", ":"))
        with lock:
            ser.write((line + "\n").encode())
        print(f">> {line}")

    def handle(msg: dict) -> None:
        t, z = msg.get("t"), msg.get("zone")
        zs = zones[z] if z else None
        with cond:
            if t == "tel":
                zs.tel += 1
                zs.last_tel = msg
                zs.rssi = (zs.rssi + [msg.get("rssi", 0)])[-100:]
            elif t == "ack":
                zs.acks[msg["status"]] += 1
                acks[(z, msg["seq"])] = msg["status"]
            elif t == "pump":
                zs.pump_events += 1
                pumps.append(msg)
            elif t == "node":
                zs.online = msg["online"]
            elif t == "stat":
                zs.gw_stat = msg
            cond.notify_all()

    def reader() -> None:
        last_text, repeats = None, 0
        while not stop.is_set():
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode(errors="replace").rstrip()
            # Gateway không bao giờ in 2 dòng giống hệt liên tiếp nhiều lần: đó là driver phát lại bộ đệm cũ
            repeats = repeats + 1 if text == last_text else 0
            last_text = text
            if repeats >= REPLAY_LIMIT:
                if repeats == REPLAY_LIMIT:
                    print("!! Cung mot dong lap lai lien tuc (driver USB phat lai du lieu cu): xoa bo dem, bo qua")
                ser.reset_input_buffer()
                continue
            msg = None
            if text.startswith("{"):
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    pass
            if msg is not None:
                handle(msg)
                if log:
                    log.write(json.dumps({"ts": time.time(), **msg}, separators=(",", ":")) + "\n")
                    log.flush()
                if quiet.is_set() and msg.get("t") == "tel":
                    continue
            print(f"<< {time.strftime('%H:%M:%S')} {text}")

    def heartbeat() -> None:
        while not stop.is_set():
            if hb_enabled.is_set():
                send({"t": "time", "epoch": int(time.time())})
            stop.wait(HEARTBEAT_S)

    def next_seq(zone: int) -> int:
        seq[zone] = (seq[zone] + 1) & 0xFFFF
        return seq[zone]

    def command(obj: dict, wait: bool = False) -> str | None:
        z = obj["zone"]
        obj["seq"] = next_seq(z)
        send(obj)
        if not wait:
            return None
        key = (z, obj["seq"])
        with cond:
            cond.wait_for(lambda: key in acks, timeout=ACK_WAIT_S)
            return acks.get(key, "khong_co_ack")

    def wait_pump(zone: int, on: int, start: int, timeout: float) -> dict | None:
        with cond:
            ok = cond.wait_for(lambda: any(p["zone"] == zone and p["on"] == on for p in pumps[start:]),
                               timeout=timeout)
            return next(p for p in pumps[start:] if p["zone"] == zone and p["on"] == on) if ok else None

    def selftest(z: int) -> None:
        results = []

        def check(name: str, got, expect) -> bool:
            ok = got == expect
            results.append((name, ok, got))
            print(f"   [{'PASS' if ok else 'FAIL'}] {name}: {got} (mong doi {expect})")
            return ok

        print(f"== selftest Zone {z} (bom se chay 2 lan)")
        check("cfg 35%/30s/10s", command({"t": "cfg", "zone": z, "low": 35.0, "max": 30, "fb": 10}, True), "ok")
        check("water 999 s", command({"t": "cmd", "zone": z, "dur": 999}, True), "rej_invalid")
        mark = len(pumps)
        first = command({"t": "cmd", "zone": z, "dur": 3}, True)
        if first == "rej_cooldown":
            print("   Nut dang cooldown: go 'cooldown_clear' tren console nut roi chay lai.")
            return
        check("water 3 s", first, "ok")
        check("water khi bom dang chay", command({"t": "cmd", "zone": z, "dur": 3}, True), "rej_busy")
        off = wait_pump(z, 0, mark, 8)
        check("pump OFF sau ~3 s", off and off["dur"], 3)
        check("water trong cooldown", command({"t": "cmd", "zone": z, "dur": 3}, True), "rej_cooldown")
        mark = len(pumps)
        check("water force trong cooldown", command({"t": "cmd", "zone": z, "dur": 5, "force": 1}, True), "ok")
        on = wait_pump(z, 1, mark, 2)
        check("pump ON src=force", on and on["src"], "force")
        check("stop khi bom dang chay", command({"t": "stop", "zone": z}, True), "ok")
        off = wait_pump(z, 0, mark, 2)
        check("pump OFF sau stop", off is not None, True)
        passed = sum(ok for _, ok, _ in results)
        print(f"== Ket qua: {passed}/{len(results)} PASS. Nut con cooldown 10 phut ('cooldown_clear' de xoa).")

    def show_stats() -> None:
        with cond:
            for z in sorted(k for k in zones if k):
                s = zones[z]
                rssi = f"{sum(s.rssi) / len(s.rssi):.0f} dBm" if s.rssi else "-"
                print(f"Zone {z}: online={s.online} tel={s.tel} rssi_tb={rssi} pump_ev={s.pump_events} "
                      f"ack={dict(s.acks)}")
                if s.gw_stat:
                    g = s.gw_stat
                    total = g["rx"] + g["lost"]
                    loss = 100.0 * g["lost"] / total if total else 0.0
                    print(f"        gateway: rx={g['rx']} lost={g['lost']} ({loss:.2f}%) dup={g['dup']} "
                          f"bad={g['bad']}")
                if s.last_tel:
                    t = s.last_tel
                    print(f"        cuoi: soil={t['soil']}% raw={t['soil_raw']} temp={t['temp']} hum={t['hum']} "
                          f"pump={t['pump']} mode={t['mode']} flags={t['flags']} up={t['up']}s")
            if not any(zones):
                print("Chua nhan duoc goi nao tu nut.")

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=heartbeat, daemon=True).start()

    usage = ("Lenh: water <z> <s> [force] | stop <z> | cfg <z> <low> <max> <fb> | raw <json> | stats | "
             "selftest <z> | hb on|off | quiet on|off | quit")
    print(usage)
    try:
        while True:
            try:
                line = input()
            except EOFError:
                break
            parts = line.split()
            if not parts:
                continue
            cmd = parts[0]
            try:
                if cmd == "quit":
                    break
                if cmd == "water":
                    obj = {"t": "cmd", "zone": int(parts[1]), "dur": int(parts[2])}
                    if len(parts) > 3 and parts[3] == "force":
                        obj["force"] = 1
                    command(obj)
                elif cmd == "stop":
                    command({"t": "stop", "zone": int(parts[1])})
                elif cmd == "cfg":
                    command({"t": "cfg", "zone": int(parts[1]), "low": float(parts[2]),
                             "max": int(parts[3]), "fb": int(parts[4])})
                elif cmd == "raw":
                    send(json.loads(line[len("raw"):]))
                elif cmd == "stats":
                    show_stats()
                elif cmd == "selftest":
                    selftest(int(parts[1]))
                elif cmd == "hb":
                    (hb_enabled.set if parts[1] == "on" else hb_enabled.clear)()
                    print(f"Heartbeat {'BAT' if hb_enabled.is_set() else 'TAT'} "
                          "(tat > 3 phut: gateway ngung beacon; > 5 phut nua: nut vao FALLBACK)")
                elif cmd == "quiet":
                    (quiet.set if parts[1] == "on" else quiet.clear)()
                else:
                    print(usage)
            except (IndexError, ValueError, KeyError) as e:
                print(f"Loi cu phap: {e}")
    finally:
        stop.set()
        ser.close()
        if log:
            log.close()


if __name__ == "__main__":
    main()
