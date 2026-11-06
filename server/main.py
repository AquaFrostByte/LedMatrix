"""LED matrix server: web UI + REST API + GIF converter + MQTT bridge + Home Assistant discovery.

The server is the source of truth. The full desired state is published retained on
<BASE_TOPIC>/set, so the ESP32 gets the right state as soon as it (re)connects.
"""
import io
import json
import logging
import math
import os
import re
import secrets
import threading
import unicodedata
from contextlib import asynccontextmanager
from pathlib import Path

import paho.mqtt.client as mqtt
from fastapi import Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from fastapi.staticfiles import StaticFiles
from PIL import Image, ImageSequence

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ledmatrix")

# ------------------------------------------------------------------ config
MQTT_HOST = os.getenv("MQTT_HOST", "localhost")  # docker-compose sets this to "mosquitto"
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")
T = os.getenv("BASE_TOPIC", "ledmatrix")
HA = os.getenv("HA_PREFIX", "homeassistant")
UI_USER = os.getenv("UI_USER", "")
UI_PASS = os.getenv("UI_PASS", "")
DATA = Path(os.getenv("DATA_DIR") or Path(__file__).resolve().parent / "data")  # Docker sets DATA_DIR=/data
GIF_DIR = DATA / "gifs"
STATE_FILE = DATA / "state.json"
GIF_DIR.mkdir(parents=True, exist_ok=True)

PANEL = 64
MAX_UPLOAD = 25 * 1024 * 1024
MAX_GIF_BYTES = int(os.getenv("MAX_GIF_BYTES", "400000"))  # per converted GIF (LittleFS is small)

OVERLAYS = ["none", "clock", "text", "life"]
TEXT_SIZES = {"Small": 1, "Normal": 2, "Big": 3, "Huge": 4}

DEFAULT_STATE = {
    "power": True,
    "brightness": 100,
    "color": [255, 255, 255],
    "gif_enabled": True,
    "gif": "",
    "overlay": "clock",
    "text": "Hello",
    "text_size": 2,
    "scroll_delay": 18,
    "clock_24h": True,
}

lock = threading.RLock()
state = dict(DEFAULT_STATE)
esp = {"online": False}


# ------------------------------------------------------------------ state
def load_state():
    if STATE_FILE.exists():
        try:
            state.update({k: v for k, v in json.loads(STATE_FILE.read_text()).items() if k in DEFAULT_STATE})
        except Exception:
            log.exception("could not read state file, using defaults")


def save_state():
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state))
    tmp.replace(STATE_FILE)


def clamp(v, lo, hi):
    return max(lo, min(hi, int(v)))


_TRANS = str.maketrans({"ä": "ae", "ö": "oe", "ü": "ue", "ß": "ss", "Ä": "Ae", "Ö": "Oe", "Ü": "Ue"})


def ascii_text(s: str) -> str:
    """The panel font is ASCII only, so transliterate umlauts etc."""
    s = unicodedata.normalize("NFKD", str(s).translate(_TRANS)).encode("ascii", "ignore").decode()
    return "".join(c for c in s if 32 <= ord(c) < 127)[:100]


def gif_names():
    return sorted(p.name for p in GIF_DIR.glob("*.gif"))


def sanitize(p: dict) -> dict:
    out = {}
    if "power" in p:
        out["power"] = bool(p["power"])
    if "brightness" in p:
        out["brightness"] = clamp(p["brightness"], 0, 255)
    if "color" in p and isinstance(p["color"], (list, tuple)) and len(p["color"]) == 3:
        out["color"] = [clamp(x, 0, 255) for x in p["color"]]
    if "gif_enabled" in p:
        out["gif_enabled"] = bool(p["gif_enabled"])
    if "gif" in p:
        if p["gif"] == "" or p["gif"] in gif_names():
            out["gif"] = p["gif"]
    if "overlay" in p and p["overlay"] in OVERLAYS:
        out["overlay"] = p["overlay"]
    if "text" in p:
        out["text"] = ascii_text(p["text"])
    if "text_size" in p:
        out["text_size"] = clamp(p["text_size"], 1, 4)
    if "scroll_delay" in p:
        out["scroll_delay"] = clamp(p["scroll_delay"], 1, 150)
    if "clock_24h" in p:
        out["clock_24h"] = bool(p["clock_24h"])
    return out


def apply_patch(patch: dict):
    with lock:
        clean = sanitize(patch)
        # picking a GIF implies "show GIFs"
        if clean.get("gif") and "gif_enabled" not in clean:
            clean["gif_enabled"] = True
        state.update(clean)
        save_state()
        publish_set()
        publish_ha_state()
    return clean


# ------------------------------------------------------------------ MQTT
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ledmatrix-server")
if MQTT_USER:
    client.username_pw_set(MQTT_USER, MQTT_PASS)
client.will_set(f"{T}/server/status", "offline", qos=1, retain=True)
client.reconnect_delay_set(1, 30)


def publish_set():
    client.publish(f"{T}/set", json.dumps(state), qos=1, retain=True)


def publish_cmd(**cmd):
    client.publish(f"{T}/cmd", json.dumps(cmd), qos=1)


def publish_ha_state():
    with lock:
        size_label = next((k for k, v in TEXT_SIZES.items() if v == state["text_size"]), "Normal")
        light = {
            "state": "ON" if state["power"] else "OFF",
            "brightness": state["brightness"],
            "color_mode": "rgb",
            "color": dict(zip("rgb", state["color"])),
        }
        misc = {
            "gif_enabled": "ON" if state["gif_enabled"] else "OFF",
            "clock_24h": "ON" if state["clock_24h"] else "OFF",
            "overlay": state["overlay"],
            "gif": state["gif"] or "-",
            "text": state["text"],
            "text_size": size_label,
            "scroll_delay": state["scroll_delay"],
        }
    client.publish(f"{T}/ha/light/state", json.dumps(light), retain=True)
    client.publish(f"{T}/ha/state", json.dumps(misc), retain=True)


def publish_discovery():
    device = {
        "identifiers": ["ledmatrix_64x64"],
        "name": "LED Matrix",
        "manufacturer": "DIY",
        "model": "ESP32 HUB75 64x64",
    }
    avail = {
        "availability": [{"topic": f"{T}/server/status"}, {"topic": f"{T}/status"}],
        "availability_mode": "all",
    }
    st_topic = f"{T}/ha/state"

    def cfg(component, key, body):
        body = {"unique_id": f"ledmatrix_{key}", "device": device, **avail, **body}
        client.publish(f"{HA}/{component}/ledmatrix/{key}/config", json.dumps(body), retain=True)

    cfg("light", "display", {
        "name": "Display", "schema": "json",
        "command_topic": f"{T}/ha/light/set", "state_topic": f"{T}/ha/light/state",
        "brightness": True, "brightness_scale": 255, "supported_color_modes": ["rgb"],
    })
    for key, name in (("gif_enabled", "GIF background"), ("clock_24h", "24 hour clock")):
        cfg("switch", key, {
            "name": name, "state_topic": st_topic, "command_topic": f"{T}/ha/{key}/set",
            "value_template": "{{ value_json.%s }}" % key, "payload_on": "ON", "payload_off": "OFF",
        })
    cfg("select", "overlay", {
        "name": "Overlay", "options": OVERLAYS, "state_topic": st_topic,
        "command_topic": f"{T}/ha/overlay/set", "value_template": "{{ value_json.overlay }}",
    })
    cfg("select", "gif", {
        "name": "GIF", "options": gif_names() or ["-"], "state_topic": st_topic,
        "command_topic": f"{T}/ha/gif/set", "value_template": "{{ value_json.gif }}",
    })
    cfg("select", "text_size", {
        "name": "Text size", "options": list(TEXT_SIZES), "state_topic": st_topic,
        "command_topic": f"{T}/ha/text_size/set", "value_template": "{{ value_json.text_size }}",
    })
    cfg("text", "text", {
        "name": "Scrolling text", "min": 0, "max": 100, "state_topic": st_topic,
        "command_topic": f"{T}/ha/text/set", "value_template": "{{ value_json.text }}",
    })
    cfg("number", "scroll_delay", {
        "name": "Scroll delay", "min": 1, "max": 150, "step": 1, "mode": "slider",
        "unit_of_measurement": "ms", "state_topic": st_topic,
        "command_topic": f"{T}/ha/scroll_delay/set", "value_template": "{{ value_json.scroll_delay }}",
    })
    cfg("button", "reboot", {
        "name": "Reboot", "command_topic": f"{T}/ha/reboot/set", "payload_press": "PRESS",
        "device_class": "restart", "entity_category": "config",
    })
    cfg("sensor", "rssi", {
        "name": "WiFi signal", "state_topic": f"{T}/telemetry", "value_template": "{{ value_json.rssi }}",
        "device_class": "signal_strength", "unit_of_measurement": "dBm", "entity_category": "diagnostic",
    })
    cfg("sensor", "free_heap", {
        "name": "Free memory", "state_topic": f"{T}/telemetry", "value_template": "{{ value_json.free_heap }}",
        "unit_of_measurement": "B", "entity_category": "diagnostic",
    })


def handle_ha_command(key: str, payload: str):
    on = payload.strip().upper() == "ON"
    if key == "light":
        d = json.loads(payload)
        patch = {}
        if "state" in d:
            patch["power"] = str(d["state"]).upper() == "ON"
        if "brightness" in d:
            patch["brightness"] = d["brightness"]
            patch["power"] = True
        if isinstance(d.get("color"), dict):
            patch["color"] = [d["color"].get("r", 255), d["color"].get("g", 255), d["color"].get("b", 255)]
        apply_patch(patch)
    elif key in ("gif_enabled", "clock_24h"):
        apply_patch({key: on})
    elif key == "overlay":
        apply_patch({"overlay": payload.strip()})
    elif key == "gif":
        apply_patch({"gif": "" if payload.strip() == "-" else payload.strip()})
    elif key == "text_size":
        apply_patch({"text_size": TEXT_SIZES.get(payload.strip(), 2)})
    elif key == "text":
        apply_patch({"text": payload})
    elif key == "scroll_delay":
        apply_patch({"scroll_delay": float(payload)})
    elif key == "reboot":
        publish_cmd(reboot=True)


def on_connect(c, userdata, flags, reason_code, properties):
    log.info("MQTT connected (%s)", reason_code)
    c.subscribe([(f"{T}/status", 0), (f"{T}/telemetry", 0), (f"{T}/ha/+/set", 0), (f"{HA}/status", 0)])
    c.publish(f"{T}/server/status", "online", qos=1, retain=True)
    publish_discovery()
    with lock:
        publish_set()
        publish_ha_state()


def on_message(c, userdata, msg):
    topic, payload = msg.topic, msg.payload.decode(errors="ignore")
    try:
        if topic == f"{T}/status":
            esp["online"] = payload == "online"
        elif topic == f"{T}/telemetry":
            esp.update(json.loads(payload))
        elif topic == f"{HA}/status":
            if payload == "online":  # Home Assistant restarted
                publish_discovery()
                publish_ha_state()
        elif topic.startswith(f"{T}/ha/") and topic.endswith("/set"):
            handle_ha_command(topic[len(T) + 4:-4], payload)
    except Exception:
        log.exception("error handling %s", topic)


client.on_connect = on_connect
client.on_message = on_message


# ------------------------------------------------------------------ GIF conversion
def fit_frame(img: Image.Image, mode: str) -> Image.Image:
    w, h = img.size
    small = max(w, h) <= PANEL  # pixel art: keep sharp
    if mode == "stretch":
        return img.resize((PANEL, PANEL), Image.Resampling.NEAREST if small else Image.Resampling.LANCZOS)
    scale = max(PANEL / w, PANEL / h) if mode == "cover" else min(PANEL / w, PANEL / h)
    if scale >= 1:
        scale, resample = math.floor(scale), Image.Resampling.NEAREST
    else:
        resample = Image.Resampling.LANCZOS
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    img = img.resize((nw, nh), resample)
    canvas = Image.new("RGB", (PANEL, PANEL), (0, 0, 0))
    canvas.paste(img, ((PANEL - nw) // 2, (PANEL - nh) // 2))
    return canvas


def convert_gif(raw: bytes, fit: str) -> bytes:
    """Any image/GIF -> 64x64 GIF, small enough for the ESP32's flash."""
    im = Image.open(io.BytesIO(raw))
    base = []
    for fr in ImageSequence.Iterator(im):
        dur = fr.info.get("duration", 100) or 100
        rgba = fr.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        bg.alpha_composite(rgba)
        base.append((fit_frame(bg.convert("RGB"), fit), max(int(dur), 10)))
        if len(base) >= 1000:
            break
    if not base:
        raise ValueError("no frames")

    result = b""
    for colors, step in ((128, 1), (64, 1), (48, 2), (32, 3), (32, 4)):
        frames, durs = [], []
        for i in range(0, len(base), step):
            group = base[i:i + step]
            frames.append(group[0][0].quantize(colors=colors, method=Image.Quantize.MEDIANCUT,
                                               dither=Image.Dither.NONE))
            durs.append(max(30, sum(d for _, d in group)))
        buf = io.BytesIO()
        frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:],
                       duration=durs, loop=0, disposal=1, optimize=False)
        result = buf.getvalue()
        if len(result) <= MAX_GIF_BYTES:
            return result
    raise ValueError(f"GIF is still {len(result) // 1024} KB after reducing; use a shorter one")


def safe_name(raw: str) -> str:
    stem = Path(raw or "gif").stem.lower().translate(_TRANS)
    stem = re.sub(r"[^a-z0-9_-]+", "_", stem).strip("_")[:24] or "gif"
    return stem + ".gif"


# ------------------------------------------------------------------ web app
security = HTTPBasic(auto_error=False)


def auth(creds: HTTPBasicCredentials = Depends(security)):
    if not UI_USER:
        return
    ok = creds and secrets.compare_digest(creds.username, UI_USER) and secrets.compare_digest(creds.password, UI_PASS)
    if not ok:
        raise HTTPException(401, "Unauthorized", headers={"WWW-Authenticate": "Basic"})


@asynccontextmanager
async def lifespan(app):
    load_state()
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    yield
    client.publish(f"{T}/server/status", "offline", qos=1, retain=True).wait_for_publish(2)
    client.loop_stop()


app = FastAPI(title="LED Matrix", lifespan=lifespan)
STATIC = Path(__file__).parent / "static"


@app.get("/", dependencies=[Depends(auth)])
def index():
    return FileResponse(STATIC / "index.html")


@app.get("/api/state", dependencies=[Depends(auth)])
def get_state():
    with lock:
        return {
            "state": state,
            "gifs": gif_names(),
            "overlays": OVERLAYS,
            "esp": {**esp, "mqtt": client.is_connected()},
        }


@app.post("/api/state", dependencies=[Depends(auth)])
def post_state(patch: dict):
    clean = apply_patch(patch)
    return {"applied": clean, "state": state}


@app.post("/api/gifs", dependencies=[Depends(auth)])
async def upload_gif(file: UploadFile = File(...), fit: str = Form("contain"), play: bool = Form(False)):
    raw = await file.read()
    if len(raw) > MAX_UPLOAD:
        raise HTTPException(413, "File too large")
    if fit not in ("contain", "cover", "stretch"):
        fit = "contain"
    try:
        data = await run_in_threadpool(convert_gif, raw, fit)
    except Exception as e:
        raise HTTPException(400, f"Could not convert: {e}")
    name = safe_name(file.filename)
    existed = (GIF_DIR / name).exists()
    (GIF_DIR / name).write_bytes(data)
    if existed:
        publish_cmd(delete=name)  # ESP drops its stale copy and re-downloads if it's playing
    publish_discovery()
    if play:
        apply_patch({"gif": name, "gif_enabled": True})
    return {"name": name, "bytes": len(data)}


@app.delete("/api/gifs/{name}", dependencies=[Depends(auth)])
def delete_gif(name: str):
    path = GIF_DIR / safe_name(name)
    if path.name != name or not path.exists():
        raise HTTPException(404, "Not found")
    path.unlink()
    with lock:
        if state["gif"] == name:
            apply_patch({"gif": ""})
    publish_cmd(delete=name)
    publish_discovery()
    return {"deleted": name}


@app.post("/api/reboot", dependencies=[Depends(auth)])
def reboot():
    publish_cmd(reboot=True)
    return {"ok": True}


# The ESP32 downloads from here, so it stays open even when UI auth is on.
app.mount("/gifs", StaticFiles(directory=GIF_DIR), name="gifs")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=int(os.getenv("PORT", "8000")))
