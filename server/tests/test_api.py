import asyncio
import json

from fastapi import FastAPI
from fastapi.testclient import TestClient

from app import api, ws
from conftest import at


def client_for(env) -> TestClient:
    app = FastAPI()
    app.include_router(api.router)
    app.include_router(ws.router)
    app.state.svc = env
    return TestClient(app)


def test_zone_update_validation_and_ai_lock(env):
    c = client_for(env)
    assert c.put("/api/zones/1", json={"soil_low": 40}).json()["soil_low"] == 40
    assert c.put("/api/zones/1", json={"soil_target": 41}).status_code == 422
    assert c.put("/api/zones/1", json={"mode": "AI"}).status_code == 409
    assert c.put("/api/zones/7", json={}).status_code == 404


def test_manual_water_rules(env):
    c = client_for(env)
    assert c.post("/api/zones/1/water", json={"dur": 31}).status_code == 422
    assert c.post("/api/zones/1/water", json={"dur": 10}).status_code == 409  # zone offline
    tel = json.dumps({"t": "tel", "soil": 40, "pump": 0, "flags": 0}).encode()
    asyncio.run(env.ingest.handle("irr/z/1/tel", tel, at(0)))
    assert c.post("/api/zones/1/water", json={"dur": 10}).status_code == 409  # gateway offline
    asyncio.run(env.ingest.handle("irr/gw/status", b"online", at(1)))
    r = c.post("/api/zones/1/water", json={"dur": 10})
    assert r.status_code == 200 and r.json()["decided_by"] == "manual"
    assert env.link.cmds("cmd")[-1]["force"] == 1


def test_readings_bucket_and_ws_snapshot(env):
    c = client_for(env)
    assert c.get("/api/readings").json()["bucket_s"] is None
    assert c.get("/api/readings", params={"from": "2026-11-01T00:00:00Z", "to": "2026-11-02T00:00:00Z"}
                 ).json()["bucket_s"] == 300
    assert c.get("/api/readings", params={"from": "2026-11-01T00:00:00Z", "to": "2026-11-08T00:00:00Z"}
                 ).json()["bucket_s"] == 1800
    with c.websocket_connect("/ws") as sock:
        msg = sock.receive_json()
        assert msg["type"] == "snapshot" and len(msg["data"]["zones"]) == 3
