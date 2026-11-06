# LED Matrix – server + thin ESP32 client

```
Browser / Home Assistant ──► server (FastAPI, :8000) ──MQTT──► ESP32 (HUB75 64x64)
                                  │                              ▲
                                  └── GIF files (/gifs/…) ───────┘ downloaded once, cached in LittleFS
```

The ESP32 no longer serves a website. It plays one GIF, draws the overlay (clock / scrolling
text / Game of Life) and listens for MQTT. All uploads are converted on the server to 64×64,
so the ESP only ever sees tiny files.

## 1. Server (on your always-on machine)

```bash
git clone https://github.com/YOUR_USER/YOUR_REPO.git && cd YOUR_REPO
cp .env.example .env        # set MQTT_USER / MQTT_PASS (and optionally UI_USER / UI_PASS)

# create the MQTT user once (same name/password as in .env)
docker run --rm -it -v "$PWD/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
  mosquitto_passwd -c /mosquitto/config/passwd ledmatrix

docker compose up -d --build
```

Open `http://<server-ip>:8000`. Data (GIFs, state) lives in `./data`.
The `/gifs/` path stays open even when the UI login is on, because the ESP32 downloads from it.

If Mosquitto logs a permission error for `passwd`, run
`sudo chown 1883:1883 mosquitto/passwd && sudo chmod 640 mosquitto/passwd` and restart the container.

**Prebuilt image instead of building:** the GitHub Action in `.github/workflows/docker.yml`
publishes `ghcr.io/<your-user>/ledmatrix-server` on every push to `main`. Set
`LEDMATRIX_IMAGE=ghcr.io/<your-user>/ledmatrix-server:latest` in `.env` and run
`docker compose pull && docker compose up -d`. (After the first run, make the package public in
GitHub under Packages if you want others to pull it without logging in.)

Already have Mosquitto (e.g. the HA add-on)? Delete the `mosquitto` service from the compose file and
set `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS` in `.env`.

Without Docker: `pip install -r server/requirements.txt && python server/main.py`
(configure through the same environment variables).

## 2. ESP32

1. Arduino libraries: **ESP32-HUB75-MatrixPanel-I2S-DMA**, **AnimatedGIF**, **PubSubClient**,
   **ArduinoJson (v7)**. You can drop AsyncTCP and ESPAsyncWebServer.
2. Copy `esp32/ledmatrix/config.example.h` to `config.h` (git-ignored) and fill in WiFi, `SERVER_URL`,
   `MQTT_HOST` (your server's IP) and the MQTT user/password.
3. Flash `esp32/ledmatrix/ledmatrix.ino`. The old GIFs on the ESP aren't needed any more –
   re-upload them through the new web UI (they get converted to 64×64 and pushed automatically).

## 3. Home Assistant

Needs the MQTT integration pointed at the same broker. Entities appear automatically
(MQTT discovery) under the device **LED Matrix**: light (on/off, brightness, colour), GIF background
and 24h switches, overlay / GIF / text-size selects, scrolling text, scroll delay, reboot button,
WiFi signal and free memory sensors.

## Why it was slow before (fixed)

- `GIFDraw` runs once **per scanline**: it called `getLocalTime`, redrew the clock and the
  scrolling text and ran Game of Life for every line of every frame. Now it only copies pixels;
  overlays are drawn once per tick.
- The web server, file listing (built from big `String`s), and polling all competed with the display DMA for RAM/CPU.
- Several bugs: `dma_display->color565()` called on a null pointer at global init, `ESP.restart()`
  inside a request handler, display/WiFi setup done twice, GIF loop with no time for the web server.

## MQTT topics (base `ledmatrix`)

| Topic | Direction | Content |
|---|---|---|
| `ledmatrix/set` | server → ESP (retained) | full desired state as JSON |
| `ledmatrix/cmd` | server → ESP | `{"reboot":true}`, `{"delete":"x.gif"}` |
| `ledmatrix/status` | ESP → server (retained, LWT) | `online` / `offline` |
| `ledmatrix/telemetry` | ESP → server, every 30 s | ip, rssi, heap, uptime, fw |
| `ledmatrix/ha/…` | HA ↔ server | discovery-based entity topics |

## Notes

- Text is ASCII only (the panel font); the server converts ä/ö/ü/ß to ae/oe/ue/ss.
- Scroll "delay" is ms per pixel step, so lower = faster.
- If the server is down the ESP keeps showing its last state (the clock keeps running from the RTC); it
  picks up the retained state again on reconnect.
- The firmware hasn't been compiled on real hardware by me – if the compiler complains about a library
  API version, send me the error and I'll adjust.
