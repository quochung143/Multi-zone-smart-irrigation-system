"""FastAPI app: khởi động serial bridge, decision scheduler và phục vụ dashboard (docs/DESIGN.md §7)."""
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

app = FastAPI(title="Multi-zone smart irrigation")

# TODO: include api router + ws; khởi động serial_bridge và decision scheduler trong lifespan

app.mount("/", StaticFiles(directory=Path(__file__).parent.parent / "static", html=True), name="static")
