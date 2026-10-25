"""Cầu nối tạm Serial <-> MQTT: cho server MQTT chạy với firmware gateway Serial hiện tại, hoặc với giả lập.

Đóng vai gateway MQTT (docs/SERVER.md §MQTT) cho tới khi firmware gateway tự nói MQTT:
- LWT irr/gw/status = offline (retain), kết nối xong publish online.
- Dòng JSON từ gateway -> irr/z/{zone}/{tel|pump|ack|node|stat}; "hello" (gateway reboot) -> offline rồi online
  để server resync.
- irr/z/{zone}/cmd và irr/srv/time -> ghi nguyên JSON xuống Serial.

Chạy (từ thư mục gốc repo):
    python server/tools/gw_bridge.py COM5 --password <mk gateway>
    python server/tools/gw_bridge.py sim --password <mk gateway> [--tel 5] [--offline 3]
"""
import argparse
import json
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

GW_STATUS = "irr/gw/status"
QOS = {"tel": 0, "stat": 0, "pump": 1, "ack": 1, "node": 1}


def open_port(port: str, args):
    if port == "sim":
        sys.path.insert(0, str(Path(__file__).parent))
        from sim_gw import FakeGateway

        return FakeGateway(set(args.offline), args.tel)
    import serial

    # Giữ DTR/RTS ở mức nghỉ để Windows không reset gateway (giống gw_test.py)
    ser = serial.serial_for_url(port, baudrate=115200, timeout=1, do_not_open=True)
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COMx hoặc 'sim' (giả lập gateway + 3 nút)")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--user", default="gateway")
    ap.add_argument("--password", default="")
    ap.add_argument("--tel", type=float, default=5, help="sim: chu kỳ tel (s)")
    ap.add_argument("--offline", type=int, nargs="*", default=[], help="sim: zone không trả lời")
    ap.add_argument("--quiet", action="store_true", help="ẩn dòng tel")
    args = ap.parse_args()

    ser = open_port(args.port, args)
    lock = threading.Lock()

    def write(obj) -> None:
        line = obj if isinstance(obj, str) else json.dumps(obj, separators=(",", ":"))
        with lock:
            ser.write((line + "\n").encode())
        print(f">> {line}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="irr-gateway")
    if args.password:
        client.username_pw_set(args.user, args.password)
    client.will_set(GW_STATUS, "offline", qos=1, retain=True)

    def on_connect(c, _userdata, _flags, reason, _props):
        if reason.is_failure:
            print(f"!! MQTT tu choi ket noi: {reason}")
            return
        print(f"== MQTT da ket noi {args.host}:{args.mqtt_port}")
        c.publish(GW_STATUS, "online", qos=1, retain=True)
        c.subscribe([("irr/z/+/cmd", 1), ("irr/srv/time", 0), ("irr/srv/status", 1)])

    def on_message(_c, _userdata, msg):
        text = msg.payload.decode(errors="replace")
        if msg.topic == "irr/srv/status":
            print(f"== server {text}")
            return
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            print(f"!! JSON loi tren {msg.topic}: {text}")
            return
        write(obj)

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect_async(args.host, args.mqtt_port, keepalive=15)
    client.loop_start()

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode(errors="replace").rstrip()
            msg = None
            if text.startswith("{"):
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    pass
            t = msg.get("t") if isinstance(msg, dict) else None
            if t in QOS and isinstance(msg.get("zone"), int):
                payload = json.dumps({"online": bool(msg["online"])} if t == "node" else msg, separators=(",", ":"))
                client.publish(f"irr/z/{msg['zone']}/{t}", payload, qos=QOS[t], retain=t == "node")
            elif t == "hello":  # gateway vừa khởi động lại: báo server resync time + cfg
                client.publish(GW_STATUS, "offline", qos=1, retain=True)
                client.publish(GW_STATUS, "online", qos=1, retain=True)
            if not (args.quiet and t == "tel"):
                print(f"<< {time.strftime('%H:%M:%S')} {text}")
    except KeyboardInterrupt:
        pass
    finally:
        client.publish(GW_STATUS, "offline", qos=1, retain=True).wait_for_publish(2)
        client.loop_stop()
        ser.close()


if __name__ == "__main__":
    main()
