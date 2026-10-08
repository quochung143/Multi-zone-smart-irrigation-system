"""Công cụ thử gateway khi chưa có server (tuần 11).

- In mọi dòng gateway gửi lên.
- Gửi {"t":"time"} mỗi 60 s để gateway coi server còn sống (gateway mới phát beacon).
- Gõ lệnh để gửi xuống nút:
    water <zone> <giây>
    stop <zone>
    cfg <zone> <low%> <max_s> <fb_s>
    raw <json>
    quit

Chạy: python server/tools/gw_test.py COM5
"""
import json
import sys
import threading
import time

import serial

HEARTBEAT_S = 60


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    ser = serial.Serial(port, 115200, timeout=1)
    lock = threading.Lock()
    stop = threading.Event()
    # seq bắt đầu ngẫu nhiên theo thời gian để không trùng last_cmd_seq còn nhớ trong nút
    seq = {z: int(time.time()) % 50000 for z in (1, 2, 3)}

    def send(obj: dict) -> None:
        line = json.dumps(obj, separators=(",", ":"))
        with lock:
            ser.write((line + "\n").encode())
        print(f">> {line}")

    def reader() -> None:
        while not stop.is_set():
            raw = ser.readline()
            if raw:
                print(f"<< {raw.decode(errors='replace').rstrip()}")

    def heartbeat() -> None:
        while not stop.is_set():
            send({"t": "time", "epoch": int(time.time())})
            stop.wait(HEARTBEAT_S)

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=heartbeat, daemon=True).start()

    def next_seq(zone: int) -> int:
        seq[zone] = (seq[zone] + 1) & 0xFFFF
        return seq[zone]

    try:
        for line in sys.stdin:
            parts = line.split()
            if not parts:
                continue
            cmd = parts[0]
            try:
                if cmd == "quit":
                    break
                if cmd == "water":
                    z = int(parts[1])
                    send({"t": "cmd", "zone": z, "seq": next_seq(z), "dur": int(parts[2])})
                elif cmd == "stop":
                    z = int(parts[1])
                    send({"t": "stop", "zone": z, "seq": next_seq(z)})
                elif cmd == "cfg":
                    z = int(parts[1])
                    send({"t": "cfg", "zone": z, "seq": next_seq(z), "low": float(parts[2]),
                          "max": int(parts[3]), "fb": int(parts[4])})
                elif cmd == "raw":
                    send(json.loads(line[len("raw"):]))
                else:
                    print("Lenh: water <z> <s> | stop <z> | cfg <z> <low> <max> <fb> | raw <json> | quit")
            except (IndexError, ValueError) as e:
                print(f"Loi cu phap: {e}")
    finally:
        stop.set()
        ser.close()


if __name__ == "__main__":
    main()
