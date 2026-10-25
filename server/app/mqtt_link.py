"""Kết nối MQTT tới Mosquitto (thay serial_bridge). Topic: docs/SERVER.md §MQTT.

- LWT irr/srv/status = offline (retain); kết nối xong publish online.
- irr/srv/time mỗi 60 s: đồng bộ giờ + heartbeat cho gateway.
- Mất broker thì thử lại sau 5 s.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from collections.abc import Awaitable, Callable

import aiomqtt

from .config import MQTT_RETRY_S, TIME_PERIOD_S, Settings

log = logging.getLogger(__name__)

UP_KINDS = ("tel", "pump", "ack", "node", "stat")
GW_STATUS = "irr/gw/status"
SRV_STATUS = "irr/srv/status"
SRV_TIME = "irr/srv/time"


def cmd_topic(zone_id: int) -> str:
    return f"irr/z/{zone_id}/cmd"


class LinkDown(RuntimeError):
    pass


class MqttLink:
    def __init__(self, settings: Settings,
                 on_message: Callable[[str, bytes, bool], Awaitable[None]],
                 on_disconnect: Callable[[], Awaitable[None]]) -> None:
        self.s = settings
        self.on_message = on_message
        self.on_disconnect = on_disconnect
        self.client: aiomqtt.Client | None = None

    @property
    def connected(self) -> bool:
        return self.client is not None

    async def publish(self, topic: str, obj: dict, qos: int = 1, retain: bool = False) -> None:
        if self.client is None:
            raise LinkDown("chưa kết nối broker")
        payload = json.dumps(obj, separators=(",", ":"))
        try:
            await self.client.publish(topic, payload, qos=qos, retain=retain)
        except aiomqtt.MqttError as e:
            raise LinkDown(str(e)) from e
        log.info("MQTT >> %s %s", topic, payload)

    async def publish_time(self) -> None:
        await self.publish(SRV_TIME, {"t": "time", "epoch": int(time.time())}, qos=0)

    async def run(self) -> None:
        will = aiomqtt.Will(SRV_STATUS, "offline", qos=1, retain=True)
        while True:
            try:
                async with aiomqtt.Client(self.s.mqtt_host, self.s.mqtt_port, identifier="irr-server",
                                          username=self.s.mqtt_user or None, password=self.s.mqtt_pass or None,
                                          will=will, keepalive=15) as client:
                    await self._session(client)
            except aiomqtt.MqttError as e:
                log.warning("MQTT mất kết nối (%s), thử lại sau %d s", e, MQTT_RETRY_S)
            finally:
                if self.client is not None:
                    self.client = None
                    await self.on_disconnect()
            await asyncio.sleep(MQTT_RETRY_S)

    async def _session(self, client: aiomqtt.Client) -> None:
        self.client = client
        log.info("MQTT đã kết nối %s:%d", self.s.mqtt_host, self.s.mqtt_port)
        await client.publish(SRV_STATUS, "online", qos=1, retain=True)
        await self.publish_time()
        for kind in UP_KINDS:
            await client.subscribe(f"irr/z/+/{kind}", qos=1)
        await client.subscribe(GW_STATUS, qos=1)
        ticker = asyncio.create_task(self._time_loop())
        try:
            async for msg in client.messages:
                try:
                    await self.on_message(str(msg.topic), bytes(msg.payload), bool(msg.retain))
                except Exception:
                    log.exception("Lỗi xử lý %s", msg.topic)
        except asyncio.CancelledError:
            # Tắt server có chủ đích: báo offline ngay, không đợi LWT
            try:
                await client.publish(SRV_STATUS, "offline", qos=1, retain=True)
            except aiomqtt.MqttError:
                pass
            raise
        finally:
            ticker.cancel()

    async def _time_loop(self) -> None:
        while True:
            await asyncio.sleep(TIME_PERIOD_S)
            try:
                await self.publish_time()
            except LinkDown:
                return
