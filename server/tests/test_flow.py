"""Luồng chính không cần broker/MongoDB: ingest -> registry/events/commands -> decision."""
import asyncio
import json

import pytest

import app.commands
from app.commands import GatewayOffline
from app.registry import Mode, ZoneConfig
from conftest import FakeML, at


def tel(soil=40.0, pump=0, flags=0):
    return json.dumps({"t": "tel", "zone": 1, "seq": 1, "soil": soil, "soil_raw": 2300, "temp": 28.0,
                       "hum": 70.0, "pump": pump, "mode": "normal", "flags": flags, "up": 10, "rssi": -60}).encode()


async def gw_online(env, now=None):
    await env.ingest.handle("irr/gw/status", b"online", now or at(0))


def test_gateway_online_resyncs_time_and_cfg(env):
    async def run():
        await gw_online(env)
        topics = [t for t, _ in env.link.sent]
        assert topics[0] == "irr/srv/time"
        assert [o["zone"] for o in env.link.cmds("cfg")] == [1, 2, 3]
        assert env.link.cmds("cfg")[0] == {"t": "cfg", "low": 35.0, "max": 30, "fb": 10, "zone": 1,
                                           "seq": env.link.cmds("cfg")[0]["seq"]}
        assert len(env.store.cols["gw_sessions"]) == 1
    asyncio.run(run())


def test_tel_stores_reading_and_marks_online(env):
    async def run():
        await env.ingest.handle("irr/z/1/tel", tel(42.5), at(0))
        st = env.registry.zones[1].state
        assert st.online and st.tel["soil"] == 42.5 and st.last_seen == at(0)
        assert env.store.cols["readings"][0]["zone_id"] == 1
        assert env.store.cols["node_status"][0]["online"] is True
    asyncio.run(run())


def test_bad_messages_are_dropped(env):
    async def run():
        for topic, payload in [("irr/z/9/tel", tel()), ("irr/z/1/tel", b"{oops"), ("irr/x", b"{}"),
                               ("irr/z/1/cmd", b"{}"), ("irr/z/1/tel", b"[1]")]:
            await env.ingest.handle(topic, payload, at(0))
        assert env.store.cols["readings"] == []
    asyncio.run(run())


def test_command_ack_and_no_ack(env, monkeypatch):
    monkeypatch.setattr(app.commands, "ACK_TIMEOUT_S", 0.05)

    async def run():
        with pytest.raises(GatewayOffline):
            await env.commands.water(1, 10, "manual")
        await gw_online(env)
        ok = await env.commands.water(1, 10, "manual", force=True)
        lost = await env.commands.water(2, 10, "threshold")
        assert env.link.cmds("cmd")[0] == {"t": "cmd", "dur": 10, "force": 1, "zone": 1, "seq": ok["seq"]}
        await env.ingest.handle("irr/z/1/ack", json.dumps({"seq": ok["seq"], "status": "ok"}).encode(), at(1))
        await asyncio.sleep(0.1)
        assert (await env.store.find_command(1, ok["seq"]))["status"] == "ok"
        assert (await env.store.find_command(2, lost["seq"]))["status"] == "no_ack"
    asyncio.run(run())


def test_gateway_offline_fails_pending(env):
    async def run():
        await gw_online(env)
        cmd = await env.commands.water(1, 10, "manual")
        await env.ingest.handle("irr/gw/status", b"offline", at(1))
        assert (await env.store.find_command(1, cmd["seq"]))["status"] == "no_ack"
        assert env.store.cols["gw_sessions"][0]["end_ts"] == at(1)
    asyncio.run(run())


def test_seq_is_unique_and_wraps(env):
    async def run():
        env.store.zones[1]["next_seq"] = 65535
        assert [await env.store.next_seq(1) for _ in range(3)] == [65535, 1, 2]
    asyncio.run(run())


def test_pump_on_off_builds_event(env):
    async def run():
        await gw_online(env)
        await env.ingest.handle("irr/z/1/tel", tel(30.0), at(0))
        cmd = await env.commands.water(1, 15, "threshold")
        await env.ingest.handle("irr/z/1/pump", json.dumps({"on": 1, "src": "cmd", "cmd_seq": cmd["seq"],
                                                            "dur": 0}).encode(), at(1))
        assert env.registry.zones[1].state.pump is True
        await env.ingest.handle("irr/z/1/pump", json.dumps({"on": 0, "src": "cmd", "cmd_seq": cmd["seq"],
                                                            "dur": 15}).encode(), at(16))
        ev = env.store.cols["irrigation_events"][0]
        assert ev["decided_by"] == "threshold" and ev["source"] == "cmd"
        assert ev["soil_before"] == 30.0 and ev["duration_s"] == 15 and ev["end_ts"] == at(16)
        assert env.registry.zones[1].state.last_water_end == at(16)
        assert env.registry.zones[1].state.pump is False
    asyncio.run(run())


def test_lost_pump_off_closed_by_tel(env):
    async def run():
        await env.ingest.handle("irr/z/1/pump", b'{"on":1,"src":"fallback","cmd_seq":0,"dur":0}', at(0))
        await env.ingest.handle("irr/z/1/tel", tel(pump=0), at(60))
        ev = env.store.cols["irrigation_events"][0]
        assert ev["decided_by"] == "node" and ev["source"] == "fallback"
        assert ev["end_estimated"] and ev["end_ts"] == at(30)  # chặn ở pump_max_s
    asyncio.run(run())


def test_pump_off_without_on_is_reconstructed(env):
    async def run():
        await env.ingest.handle("irr/z/2/pump", b'{"on":0,"src":"force","cmd_seq":5,"dur":8}', at(100))
        ev = env.store.cols["irrigation_events"][0]
        assert ev["start_ts"] == at(92) and ev["start_estimated"] and ev["decided_by"] == "manual"
    asyncio.run(run())


def test_soil_after_10m(env):
    async def run():
        await env.ingest.handle("irr/z/1/pump", b'{"on":1,"src":"cmd","cmd_seq":0,"dur":0}', at(0))
        await env.ingest.handle("irr/z/1/pump", b'{"on":0,"src":"cmd","cmd_seq":0,"dur":10}', at(10))
        for minute, soil in [(7, 99), (9, 50), (11, 52), (13, 99)]:
            await env.ingest.handle("irr/z/1/tel", tel(soil), at(10 + minute * 60))
        assert await env.events.fill_soil_after(at(10 + 11 * 60)) == 0  # chưa đủ 12 phút
        assert await env.events.fill_soil_after(at(10 + 13 * 60)) == 1
        assert env.store.cols["irrigation_events"][0]["soil_after_10m"] == 51.0
    asyncio.run(run())


def test_threshold_decisions(env):
    async def run():
        z = env.registry.zones[1]
        await env.registry.update_config(1, {"collect_mode": False, "threshold_dur": 12})
        assert (await env.decision.decide_zone(1, at(0)))["action_taken"] == "skip_offline"
        await gw_online(env)
        await env.ingest.handle("irr/z/1/tel", tel(50.0), at(0))
        assert (await env.decision.decide_zone(1, at(10)))["action_taken"] == "none"
        assert (await env.decision.decide_zone(1, at(500)))["action_taken"] == "skip_stale"
        await env.ingest.handle("irr/z/1/tel", tel(30.0, flags=2), at(500))
        assert (await env.decision.decide_zone(1, at(510)))["action_taken"] == "skip_sensor"
        await env.ingest.handle("irr/z/1/tel", tel(30.0), at(520))
        doc = await env.decision.decide_zone(1, at(530))
        assert doc["action_taken"] == "water" and doc["dur"] == 12
        assert env.link.cmds("cmd")[-1]["dur"] == 12 and "force" not in env.link.cmds("cmd")[-1]
        z.state.last_water_end = at(520)
        assert (await env.decision.decide_zone(1, at(540)))["action_taken"] == "skip_cooldown"
        await env.registry.update_config(1, {"mode": Mode.MANUAL})
        assert (await env.decision.decide_zone(1, at(540)))["action_taken"] == "skip_manual"
        assert len(env.store.cols["predictions"]) == 7
    asyncio.run(run())


def test_collect_mode_random_duration(env):
    async def run():
        await gw_online(env)
        await env.ingest.handle("irr/z/1/tel", tel(20.0), at(0))
        doc = await env.decision.decide_zone(1, at(1))
        assert 5 <= doc["dur"] <= 25
    asyncio.run(run())


def test_ai_decisions(env):
    async def run():
        await gw_online(env)
        await env.registry.update_config(1, {"mode": Mode.AI})
        await env.ingest.handle("irr/z/1/tel", tel(45.0), at(0))
        assert (await env.decision.decide_zone(1, at(1)))["action_taken"] == "skip_no_model"
        env.decision.ml = FakeML(p=0.8, dur=99.0)
        doc = await env.decision.decide_zone(1, at(2))
        assert doc["action_taken"] == "water" and doc["dur"] == 30 and doc["p_need"] == 0.8  # clip pump_max_s
        env.decision.ml = FakeML(p=0.2, dur=10.0)
        assert (await env.decision.decide_zone(1, at(3)))["action_taken"] == "none"
    asyncio.run(run())


@pytest.mark.parametrize("changes", [{"soil_low": 5}, {"soil_low": 58}, {"pump_max_s": 31},
                                     {"threshold_dur": 31}, {"fallback_s": 40}])
def test_zone_config_validation(changes):
    with pytest.raises(ValueError):
        ZoneConfig(id=1, name="Z1", **changes)


def test_retained_node_online_is_ignored_and_gw_offline_marks_nodes_offline(env):
    async def run():
        await env.ingest.handle("irr/z/3/node", b'{"online":true}', at(0), retain=True)
        assert env.registry.zones[3].state.online is False
        await gw_online(env)
        await env.ingest.handle("irr/z/1/tel", tel(), at(1))
        await env.ingest.handle("irr/gw/status", b"offline", at(2))
        assert env.registry.zones[1].state.online is False
    asyncio.run(run())
