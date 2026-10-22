"""Giả lập gateway + 3 nút để thử giao thức JSON Serial khi chưa có board.

Mô phỏng theo firmware (gateway/main/cmd.c, node/main/pump.c, comm.c):
- Kiểm tra lệnh như gateway: "# json loi", "# lenh khong hop le".
- Nút: rej_invalid (dur > pump_max_s), rej_busy, rej_cooldown (10 phút, "force" bỏ qua), lệnh trùng seq -> ACK cũ.
- tel mỗi TEL_PERIOD_S (rút ngắn so với 30 s thật), độ ẩm giảm dần, tăng khi bơm.
- stat mỗi 60 s, node online khi có gói đầu tiên.
- Zone nằm trong --offline không trả lời: lệnh tới zone đó -> ack "timeout" sau ~0.9 s.

Chạy:  python server/tools/sim_gw.py [--offline 3] [--tel 5]
       (mở gw_test.py với cổng giả; gõ lệnh như khi có board thật)
"""
import argparse
import json
import queue
import random
import runpy
import sys
import threading
import time
from pathlib import Path

import serial

PUMP_MAX_S = 30
COOLDOWN_S = 600
ACK_TIMEOUT_S = 0.9  # 3 lần × 300 ms


class Node:
    def __init__(self, zone: int) -> None:
        self.zone = zone
        self.soil = 45.0 + zone * 5
        self.seq = 0
        self.pump_max = PUMP_MAX_S
        self.pump: tuple[float, int, int, str] | None = None  # (start, dur, cmd_seq, src)
        self.last_end: float | None = None
        self.last_cmd: tuple[int, str] | None = None  # (seq, status)
        self.rx = 0


class FakeGateway:
    def __init__(self, offline: set[int], tel_period: float) -> None:
        self.out: queue.Queue[bytes] = queue.Queue()
        self.buf = b""
        self.offline = offline
        self.tel_period = tel_period
        self.nodes = {z: Node(z) for z in (1, 2, 3)}
        self.online: set[int] = set()
        self.start = time.time()
        threading.Thread(target=self._tick, daemon=True).start()
        self._emit({"t": "hello", "fw": "gw-sim", "mac": "00:00:00:00:00:00"})

    # ---------- phía gateway -> server ----------
    def _emit(self, obj) -> None:
        line = obj if isinstance(obj, str) else json.dumps(obj, separators=(",", ":"))
        self.out.put((line + "\n").encode())

    def _uplink(self, n: Node, obj: dict) -> None:
        n.seq += 1
        n.rx += 1
        if n.zone not in self.online:
            self.online.add(n.zone)
            self._emit({"t": "node", "zone": n.zone, "online": True})
        self._emit(obj)

    def _tick(self) -> None:
        next_tel = {z: time.time() + 1 + z for z in self.nodes}
        next_stat = time.time() + 60
        while True:
            time.sleep(0.1)
            now = time.time()
            for z, n in self.nodes.items():
                if z in self.offline:
                    continue
                if n.pump:
                    start, dur, cseq, src = n.pump
                    n.soil = min(95.0, n.soil + 0.15)
                    if now - start >= dur:
                        n.pump, n.last_end = None, now
                        self._uplink(n, {"t": "pump", "zone": z, "on": 0, "src": src, "cmd_seq": cseq,
                                         "dur": round(now - start)})
                else:
                    n.soil = max(5.0, n.soil - 0.002)
                if now >= next_tel[z]:
                    next_tel[z] = now + self.tel_period
                    self._uplink(n, {
                        "t": "tel", "zone": z, "seq": n.seq + 1, "soil": round(n.soil, 1),
                        "soil_raw": int(3000 - n.soil * 17), "temp": round(28 + random.uniform(-0.3, 0.3), 2),
                        "hum": round(70 + random.uniform(-1, 1), 2), "pump": int(n.pump is not None),
                        "mode": "normal",
                        "flags": 4 if n.last_end and now - n.last_end < COOLDOWN_S else 0,
                        "up": int(now - self.start), "rssi": random.randint(-65, -50)})
            if now >= next_stat:
                next_stat = now + 60
                for z, n in self.nodes.items():
                    if n.rx:
                        self._emit({"t": "stat", "zone": z, "rx": n.rx, "lost": 0, "dup": 0, "bad": 0})

    # ---------- phía server -> gateway ----------
    def write(self, data: bytes) -> int:
        self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            self._handle_line(line.decode(errors="replace").strip())
        return len(data)

    def _handle_line(self, line: str) -> None:
        if not line.startswith("{"):
            return
        try:
            m = json.loads(line)
        except json.JSONDecodeError:
            return self._emit("# json loi")
        t = m.get("t")
        if t in ("time", "ping"):
            return
        zone, seq = m.get("zone"), m.get("seq")
        valid = isinstance(zone, int) and 1 <= zone <= 3 and isinstance(seq, int) and 0 <= seq <= 0xFFFF
        if t == "cmd":
            valid = valid and isinstance(m.get("dur"), int) and 0 < m["dur"] <= 0xFFFF
        elif t == "cfg":
            valid = (valid and isinstance(m.get("low"), (int, float)) and 0 <= m["low"] <= 100
                     and isinstance(m.get("max"), int) and m["max"] > 0
                     and isinstance(m.get("fb"), int) and m["fb"] > 0)
        elif t != "stop":
            valid = False
        if not valid:
            return self._emit("# lenh khong hop le")
        threading.Thread(target=self._deliver, args=(m,), daemon=True).start()

    def _deliver(self, m: dict) -> None:
        zone, seq = m["zone"], m["seq"]
        if zone in self.offline:
            time.sleep(ACK_TIMEOUT_S)
            return self._emit({"t": "ack", "zone": zone, "seq": seq, "status": "timeout"})
        time.sleep(0.02)
        n = self.nodes[zone]
        if n.last_cmd and n.last_cmd[0] == seq:  # lệnh trùng: chỉ gửi lại ACK cũ
            return self._uplink(n, {"t": "ack", "zone": zone, "seq": seq, "status": n.last_cmd[1]})
        now = time.time()
        pump_on = None
        if m["t"] == "cfg":
            ok = m["max"] <= PUMP_MAX_S and m["fb"] <= m["max"] and m["low"] <= 100
            status = "ok" if ok else "rej_invalid"
            if ok:
                n.pump_max = m["max"]
        elif m["t"] == "stop":
            status = "ok"
            if n.pump:
                n.pump = (n.pump[0], 0, n.pump[2], n.pump[3])  # _tick sẽ tắt và phát pump on=0
        else:
            dur = m["dur"]
            src = "force" if m.get("force") else "cmd"
            if dur > n.pump_max:
                status = "rej_invalid"
            elif n.pump:
                status = "rej_busy"
            elif src == "cmd" and n.last_end and now - n.last_end < COOLDOWN_S:
                status = "rej_cooldown"
            else:
                status = "ok"
                n.pump = (now, dur, seq, src)
                pump_on = {"t": "pump", "zone": zone, "on": 1, "src": src, "cmd_seq": seq, "dur": 0}
        n.last_cmd = (seq, status)
        self._uplink(n, {"t": "ack", "zone": zone, "seq": seq, "status": status})
        if pump_on:
            self._uplink(n, pump_on)

    def readline(self) -> bytes:
        try:
            return self.out.get(timeout=1)
        except queue.Empty:
            return b""

    dtr = rts = False

    def open(self) -> None:
        pass

    def reset_input_buffer(self) -> None:
        pass

    def close(self) -> None:
        pass


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--offline", type=int, nargs="*", default=[], help="zone không trả lời (thử timeout)")
    ap.add_argument("--tel", type=float, default=5, help="chu kỳ tel giả lập (s), thật là 30 s")
    args, rest = ap.parse_known_args()

    gw = FakeGateway(set(args.offline), args.tel)
    serial.serial_for_url = lambda *a, **k: gw
    sys.argv = ["gw_test.py", "sim://", *rest]
    runpy.run_path(str(Path(__file__).with_name("gw_test.py")), run_name="__main__")


if __name__ == "__main__":
    main()
