"""Lớp lưu trữ (DESIGN §8). MongoStore cho chạy thật; MemoryStore (MONGO_URI=memory://) cho chạy thử và test.

Hai lớp có cùng phương thức; các module khác chỉ gọi qua đây, không chạm trực tiếp vào collection.
"""
from __future__ import annotations

import itertools
import logging
import statistics
import time
from datetime import datetime, timezone
from typing import Any, Protocol

log = logging.getLogger(__name__)


def _initial_seq() -> int:
    # Không bắt đầu từ 1: nút nhớ seq lệnh cuối và bỏ qua lệnh trùng seq
    return int(time.time()) % 50000 + 1


def _wrap(seq: int) -> int:
    return (seq - 1) % 0xFFFF + 1  # 1..65535


class Store(Protocol):
    async def init(self) -> None: ...
    async def close(self) -> None: ...
    async def load_zones(self) -> list[dict]: ...
    async def save_zone(self, cfg: dict) -> None: ...
    async def next_seq(self, zone_id: int) -> int: ...
    async def insert_reading(self, doc: dict) -> None: ...
    async def readings(self, zone_id: int | None, frm: datetime, to: datetime, bucket_s: int | None) -> list[dict]: ...
    async def avg_soil(self, zone_id: int, frm: datetime, to: datetime) -> float | None: ...
    async def last_reading_ts(self) -> datetime | None: ...
    async def insert_command(self, doc: dict) -> None: ...
    async def update_command(self, zone_id: int, seq: int, fields: dict) -> None: ...
    async def find_command(self, zone_id: int, seq: int) -> dict | None: ...
    async def commands(self, limit: int) -> list[dict]: ...
    async def insert_event(self, doc: dict) -> Any: ...
    async def update_event(self, event_id: Any, fields: dict) -> None: ...
    async def open_event(self, zone_id: int) -> dict | None: ...
    async def last_water_end(self, zone_id: int) -> datetime | None: ...
    async def events(self, zone_id: int | None, frm: datetime, to: datetime) -> list[dict]: ...
    async def events_pending_after(self, ended_before: datetime) -> list[dict]: ...
    async def insert_prediction(self, doc: dict) -> None: ...
    async def predictions(self, zone_id: int | None, frm: datetime, to: datetime) -> list[dict]: ...
    async def insert_node_status(self, doc: dict) -> None: ...
    async def insert_link_stat(self, doc: dict) -> None: ...
    async def open_session(self, kind: str, ts: datetime) -> Any: ...
    async def close_session(self, kind: str, session_id: Any, ts: datetime) -> None: ...
    async def close_dangling_sessions(self, kind: str, end_ts: datetime) -> int: ...


def make_store(uri: str, db_name: str) -> Store:
    return MemoryStore() if uri.startswith("memory://") else MongoStore(uri, db_name)


def _clean(doc: dict | None) -> dict | None:
    """Bỏ _id ObjectId để trả thẳng ra JSON."""
    if doc is None:
        return None
    d = dict(doc)
    if "_id" in d and not isinstance(d["_id"], int):
        d["_id"] = str(d["_id"])
    return d


class MongoStore:
    def __init__(self, uri: str, db_name: str) -> None:
        from pymongo import AsyncMongoClient

        self.client = AsyncMongoClient(uri, tz_aware=True, serverSelectionTimeoutMS=3000)
        self.db = self.client[db_name]

    async def init(self) -> None:
        await self.client.admin.command("ping")  # fail-fast nếu MongoDB không chạy
        names = await self.db.list_collection_names()
        if "readings" not in names:
            await self.db.create_collection(
                "readings", timeseries={"timeField": "ts", "metaField": "zone_id", "granularity": "seconds"})
        await self.db.irrigation_events.create_index([("zone_id", 1), ("start_ts", -1)])
        await self.db.commands.create_index([("zone_id", 1), ("seq", 1)], unique=True)
        await self.db.predictions.create_index([("zone_id", 1), ("ts", -1)])
        await self.db.node_status.create_index([("zone_id", 1), ("ts", -1)])
        await self.db.link_stats.create_index([("zone_id", 1), ("ts", -1)])

    async def close(self) -> None:
        await self.client.close()

    # ---- zones ----
    async def load_zones(self) -> list[dict]:
        return await self.db.zones.find().to_list(None)

    async def save_zone(self, cfg: dict) -> None:
        doc = {k: v for k, v in cfg.items() if k != "id"}
        await self.db.zones.update_one({"_id": cfg["id"]}, {"$set": doc}, upsert=True)

    async def next_seq(self, zone_id: int) -> int:
        await self.db.zones.update_one({"_id": zone_id, "next_seq": {"$exists": False}},
                                       {"$set": {"next_seq": _initial_seq()}})
        doc = await self.db.zones.find_one_and_update({"_id": zone_id}, {"$inc": {"next_seq": 1}})
        seq = doc["next_seq"]
        if seq > 0xFFFF:
            seq = 1
            await self.db.zones.update_one({"_id": zone_id}, {"$set": {"next_seq": 2}})
        return seq

    # ---- readings ----
    async def insert_reading(self, doc: dict) -> None:
        await self.db.readings.insert_one(dict(doc))

    async def readings(self, zone_id, frm, to, bucket_s):
        match = {"ts": {"$gte": frm, "$lte": to}}
        if zone_id is not None:
            match["zone_id"] = zone_id
        if not bucket_s:
            cur = self.db.readings.find(match, {"_id": 0}).sort("ts", 1)
            return await cur.to_list(None)
        pipeline = [
            {"$match": match},
            {"$group": {
                "_id": {"zone_id": "$zone_id",
                        "ts": {"$dateTrunc": {"date": "$ts", "unit": "second", "binSize": bucket_s}}},
                "soil": {"$avg": "$soil"}, "temp": {"$avg": "$temp"}, "hum": {"$avg": "$hum"},
                "pump": {"$max": "$pump"}}},
            {"$project": {"_id": 0, "zone_id": "$_id.zone_id", "ts": "$_id.ts",
                          "soil": 1, "temp": 1, "hum": 1, "pump": 1}},
            {"$sort": {"ts": 1}},
        ]
        return await (await self.db.readings.aggregate(pipeline)).to_list(None)

    async def avg_soil(self, zone_id, frm, to):
        pipeline = [{"$match": {"zone_id": zone_id, "ts": {"$gte": frm, "$lte": to}}},
                    {"$group": {"_id": None, "soil": {"$avg": "$soil"}}}]
        rows = await (await self.db.readings.aggregate(pipeline)).to_list(None)
        return rows[0]["soil"] if rows else None

    async def last_reading_ts(self):
        doc = await self.db.readings.find_one(sort=[("ts", -1)])
        return doc["ts"] if doc else None

    # ---- commands ----
    async def insert_command(self, doc: dict) -> None:
        await self.db.commands.insert_one(dict(doc))

    async def update_command(self, zone_id, seq, fields):
        await self.db.commands.update_one({"zone_id": zone_id, "seq": seq}, {"$set": fields})

    async def find_command(self, zone_id, seq):
        return _clean(await self.db.commands.find_one({"zone_id": zone_id, "seq": seq}))

    async def commands(self, limit):
        docs = await self.db.commands.find().sort("created_ts", -1).limit(limit).to_list(None)
        return [_clean(d) for d in docs]

    # ---- irrigation_events ----
    async def insert_event(self, doc: dict):
        return (await self.db.irrigation_events.insert_one(dict(doc))).inserted_id

    async def update_event(self, event_id, fields):
        await self.db.irrigation_events.update_one({"_id": event_id}, {"$set": fields})

    async def open_event(self, zone_id):
        return await self.db.irrigation_events.find_one({"zone_id": zone_id, "end_ts": None},
                                                        sort=[("start_ts", -1)])

    async def last_water_end(self, zone_id):
        doc = await self.db.irrigation_events.find_one({"zone_id": zone_id, "end_ts": {"$ne": None}},
                                                       sort=[("end_ts", -1)])
        return doc["end_ts"] if doc else None

    async def events(self, zone_id, frm, to):
        q = {"start_ts": {"$gte": frm, "$lte": to}}
        if zone_id is not None:
            q["zone_id"] = zone_id
        docs = await self.db.irrigation_events.find(q).sort("start_ts", 1).to_list(None)
        return [_clean(d) for d in docs]

    async def events_pending_after(self, ended_before):
        q = {"end_ts": {"$ne": None, "$lte": ended_before}, "soil_after_10m": {"$exists": False}}
        return await self.db.irrigation_events.find(q).to_list(None)

    # ---- khác ----
    async def insert_prediction(self, doc: dict) -> None:
        await self.db.predictions.insert_one(dict(doc))

    async def predictions(self, zone_id, frm, to):
        q = {"ts": {"$gte": frm, "$lte": to}}
        if zone_id is not None:
            q["zone_id"] = zone_id
        docs = await self.db.predictions.find(q, {"features": 0}).sort("ts", 1).to_list(None)
        return [_clean(d) for d in docs]

    async def insert_node_status(self, doc: dict) -> None:
        await self.db.node_status.insert_one(dict(doc))

    async def insert_link_stat(self, doc: dict) -> None:
        await self.db.link_stats.insert_one(dict(doc))

    async def open_session(self, kind, ts):
        return (await self.db[kind].insert_one({"start_ts": ts, "end_ts": None})).inserted_id

    async def close_session(self, kind, session_id, ts):
        await self.db[kind].update_one({"_id": session_id}, {"$set": {"end_ts": ts}})

    async def close_dangling_sessions(self, kind, end_ts):
        res = await self.db[kind].update_many({"end_ts": None}, {"$set": {"end_ts": end_ts, "end_estimated": True}})
        return res.modified_count


class MemoryStore:
    """Cùng giao diện với MongoStore, dữ liệu mất khi tắt server."""

    def __init__(self) -> None:
        self.zones: dict[int, dict] = {}
        self.cols: dict[str, list[dict]] = {k: [] for k in (
            "readings", "commands", "irrigation_events", "predictions", "node_status", "link_stats",
            "gw_sessions", "srv_sessions")}
        self._ids = itertools.count(1)

    async def init(self) -> None:
        log.warning("MONGO_URI=memory://: dữ liệu chỉ nằm trong RAM")

    async def close(self) -> None:
        pass

    @staticmethod
    def _in(doc: dict, key: str, frm: datetime, to: datetime, zone_id: int | None) -> bool:
        return frm <= doc[key] <= to and (zone_id is None or doc["zone_id"] == zone_id)

    async def load_zones(self):
        return [dict(d) for d in self.zones.values()]

    async def save_zone(self, cfg):
        doc = self.zones.setdefault(cfg["id"], {"_id": cfg["id"]})
        doc.update({k: v for k, v in cfg.items() if k != "id"})

    async def next_seq(self, zone_id):
        doc = self.zones.setdefault(zone_id, {"_id": zone_id})
        seq = _wrap(doc.get("next_seq", _initial_seq()))
        doc["next_seq"] = seq + 1
        return seq

    async def insert_reading(self, doc):
        self.cols["readings"].append(dict(doc))

    async def readings(self, zone_id, frm, to, bucket_s):
        rows = sorted((d for d in self.cols["readings"] if self._in(d, "ts", frm, to, zone_id)),
                      key=lambda d: d["ts"])
        if not bucket_s:
            return [dict(d) for d in rows]
        groups: dict[tuple, list[dict]] = {}
        for d in rows:
            epoch = int(d["ts"].timestamp()) // bucket_s * bucket_s
            groups.setdefault((d["zone_id"], epoch), []).append(d)
        out = []
        for (z, epoch), ds in groups.items():
            avg = lambda k: statistics.fmean(x[k] for x in ds if x.get(k) is not None) if any(  # noqa: E731
                x.get(k) is not None for x in ds) else None
            out.append({"zone_id": z, "ts": datetime.fromtimestamp(epoch, timezone.utc), "soil": avg("soil"),
                        "temp": avg("temp"), "hum": avg("hum"), "pump": max(x["pump"] for x in ds)})
        return sorted(out, key=lambda d: d["ts"])

    async def avg_soil(self, zone_id, frm, to):
        vals = [d["soil"] for d in self.cols["readings"] if self._in(d, "ts", frm, to, zone_id)]
        return statistics.fmean(vals) if vals else None

    async def last_reading_ts(self):
        return max((d["ts"] for d in self.cols["readings"]), default=None)

    async def insert_command(self, doc):
        self.cols["commands"].append(dict(doc))

    async def update_command(self, zone_id, seq, fields):
        for d in self.cols["commands"]:
            if d["zone_id"] == zone_id and d["seq"] == seq:
                d.update(fields)

    async def find_command(self, zone_id, seq):
        return next((dict(d) for d in reversed(self.cols["commands"])
                     if d["zone_id"] == zone_id and d["seq"] == seq), None)

    async def commands(self, limit):
        return [dict(d) for d in sorted(self.cols["commands"], key=lambda d: d["created_ts"], reverse=True)[:limit]]

    async def insert_event(self, doc):
        d = {**doc, "_id": next(self._ids)}
        self.cols["irrigation_events"].append(d)
        return d["_id"]

    async def update_event(self, event_id, fields):
        for d in self.cols["irrigation_events"]:
            if d["_id"] == event_id:
                d.update(fields)

    async def open_event(self, zone_id):
        rows = [d for d in self.cols["irrigation_events"] if d["zone_id"] == zone_id and d.get("end_ts") is None]
        return dict(max(rows, key=lambda d: d["start_ts"])) if rows else None

    async def last_water_end(self, zone_id):
        return max((d["end_ts"] for d in self.cols["irrigation_events"]
                    if d["zone_id"] == zone_id and d.get("end_ts")), default=None)

    async def events(self, zone_id, frm, to):
        return sorted((dict(d) for d in self.cols["irrigation_events"]
                       if self._in(d, "start_ts", frm, to, zone_id)), key=lambda d: d["start_ts"])

    async def events_pending_after(self, ended_before):
        return [dict(d) for d in self.cols["irrigation_events"]
                if d.get("end_ts") and d["end_ts"] <= ended_before and "soil_after_10m" not in d]

    async def insert_prediction(self, doc):
        self.cols["predictions"].append(dict(doc))

    async def predictions(self, zone_id, frm, to):
        return [{k: v for k, v in d.items() if k != "features"} for d in self.cols["predictions"]
                if self._in(d, "ts", frm, to, zone_id)]

    async def insert_node_status(self, doc):
        self.cols["node_status"].append(dict(doc))

    async def insert_link_stat(self, doc):
        self.cols["link_stats"].append(dict(doc))

    async def open_session(self, kind, ts):
        d = {"_id": next(self._ids), "start_ts": ts, "end_ts": None}
        self.cols[kind].append(d)
        return d["_id"]

    async def close_session(self, kind, session_id, ts):
        for d in self.cols[kind]:
            if d["_id"] == session_id:
                d["end_ts"] = ts

    async def close_dangling_sessions(self, kind, end_ts):
        n = 0
        for d in self.cols[kind]:
            if d["end_ts"] is None:
                d.update(end_ts=end_ts, end_estimated=True)
                n += 1
        return n
