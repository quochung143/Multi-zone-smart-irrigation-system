"""Chạy server: `cd server && python -m app`.

Không dùng `uvicorn --reload` / nhiều worker: mỗi tiến trình sẽ tự kết nối MQTT và chạy decision riêng.
Trên Windows phải dùng SelectorEventLoop vì aiomqtt (paho) cần add_reader, ProactorEventLoop không có.
"""
import asyncio
import sys

import uvicorn

from .config import Settings


def main() -> None:
    s = Settings()
    server = uvicorn.Server(uvicorn.Config("app.main:app", host=s.http_host, port=s.http_port, workers=1,
                                           log_config=None))
    loop_factory = asyncio.SelectorEventLoop if sys.platform == "win32" else None
    asyncio.run(server.serve(), loop_factory=loop_factory)


if __name__ == "__main__":
    main()
