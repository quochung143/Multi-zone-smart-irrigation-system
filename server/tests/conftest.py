import asyncio
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.commands import CommandManager  # noqa: E402
from app.db import MemoryStore  # noqa: E402
from app.decision import DecisionEngine  # noqa: E402
from app.events import EventTracker  # noqa: E402
from app.ingest import Ingest  # noqa: E402
from app.mqtt_link import LinkDown  # noqa: E402
from app.registry import ZoneRegistry  # noqa: E402
from app.ws import WsHub  # noqa: E402

T0 = datetime(2026, 11, 2, 8, 0, tzinfo=timezone.utc)


def at(seconds: float) -> datetime:
    return T0 + timedelta(seconds=seconds)


class FakeLink:
    def __init__(self) -> None:
        self.sent: list[tuple[str, dict]] = []
        self.connected = True

    async def publish(self, topic, obj, qos=1, retain=False):
        if not self.connected:
            raise LinkDown("test")
        self.sent.append((topic, obj))

    async def publish_time(self):
        await self.publish("irr/srv/time", {"t": "time"})

    def cmds(self, t=None):
        return [o for topic, o in self.sent if topic.endswith("/cmd") and (t is None or o["t"] == t)]


class FakeML:
    def __init__(self, p=None, dur=None):
        self.p, self.dur = p, dur

    available = property(lambda self: self.p is not None)
    version = property(lambda self: "test" if self.p is not None else None)

    async def features(self, store, zone_id, now):
        return {"soil": 40.0, "temp": 28.0, "hum": 70.0}

    def predict_need(self, x):
        return self.p

    def predict_dur(self, x, soil_target, zone_id):
        return self.dur


@pytest.fixture
def env():
    store = MemoryStore()
    ws = WsHub()
    registry = ZoneRegistry(store)
    link = FakeLink()
    ml = FakeML()
    commands = CommandManager(store, registry, link, ws)
    events = EventTracker(store, registry, ws)
    ingest = Ingest(store, registry, commands, events, link, ws)
    decision = DecisionEngine(store, registry, commands, ml, ws)
    asyncio.run(registry.load())
    e = SimpleNamespace(store=store, ws=ws, registry=registry, link=link, ml=ml, commands=commands,
                        events=events, ingest=ingest, decision=decision)
    e.snapshot = lambda: {"zones": registry.snapshot(), "gateway": {"online": registry.gw_online},
                          "mqtt": link.connected, "model": ml.version}
    return e
