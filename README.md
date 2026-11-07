# LED Matrix: Server & Thin ESP32 Client Architecture

The revised architecture transitions the ESP32 into a lightweight, high-performance thin client. By offloading heavy processing (web hosting, file conversions, state management) to a central server, the ESP32 is freed up to focus exclusively on hardware-level rendering and MQTT communication. All media uploads are pre-processed and resized to 64×64 on the server, ensuring the ESP32 only handles optimized, lightweight files.

```text
Browser / Home Assistant ──► Server (FastAPI, :8000) ──MQTT──► ESP32 (HUB75 64x64)
                                  │                            ▲
                                  └── GIF files (/gifs/…) ─────┘ (Downloaded once, cached in LittleFS)

```

## 1. Hardware & Wiring configuration

The setup requires precise GPIO mapping for both the SD Card module and the HUB75 Matrix panel to ensure stable DMA output and file reading.

### SD Card Pinout

| Function | ESP32 GPIO |
| --- | --- |
| **CS** | 23 |
| **SCK** | 19 |
| **MOSI** | 18 |
| **MISO** | 5 |

### HUB75 Matrix Pinout

For a visual reference of the physical connector layout on the panel, see image_69a75c.png.

| HUB75 Pin | ESP32 GPIO | HUB75 Pin | ESP32 GPIO |
| --- | --- | --- | --- |
| **R1** | 25 | **R2** | 14 |
| **G1** | 26 | **G2** | 12 |
| **B1** | 27 | **B2** | 13 |
| **A** | 22 | **B** | 32 |
| **C** | 33 | **D** | 17 |
| **E** | 21 | **CLK** | 16 |
| **LAT** | 4 | **OE** | 15 |
| **GND** | GND |  |  |

## 2. Server Deployment (Always-On Host)

The server acts as the control plane, media converter, and file host. Data (including states and GIFs) is persistently stored in `./data`. The `/gifs/` endpoint remains openly accessible for the ESP32 to download assets, even when the UI is secured.

### Standard Docker Setup

Clone the repository and set up your environment and MQTT credentials:

```bash
git clone https://github.com/YOUR_USER/YOUR_REPO.git && cd YOUR_REPO
cp .env.example .env  # Configure MQTT_USER, MQTT_PASS, and optionally UI_USER/UI_PASS

# Initialize the MQTT user (matching the credentials in .env)
docker run --rm -it -v "$PWD/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
  mosquitto_passwd -c /mosquitto/config/passwd ledmatrix

# Build and start the services
docker compose up -d --build

```

*Note on Permissions:* If Mosquitto throws a permission error regarding `passwd`, execute `sudo chown 1883:1883 mosquitto/passwd && sudo chmod 640 mosquitto/passwd`, then restart the container.

### Using the Prebuilt Image

Alternatively, pull the prebuilt GitHub Action image published on pushes to `main`.

1. Set `LEDMATRIX_IMAGE=ghcr.io/<your-user>/ledmatrix-server:latest` in your `.env` file.
2. Run `docker compose pull && docker compose up -d`.
*(If the package is set to private in GitHub Packages, you must make it public for unauthenticated pulls.)*

### Integration with Existing MQTT Brokers (e.g., Home Assistant Add-on)

If you already run a centralized Mosquitto broker:

1. Delete the `mosquitto` service block from `docker-compose.yml`.
2. Define `MQTT_HOST`, `MQTT_USER`, and `MQTT_PASS` in your `.env` file to point to your existing broker.

### Bare Metal Deployment (No Docker)

Install Python dependencies and execute the script directly. Ensure the same environment variables are configured in your host environment.

```bash
pip install -r server/requirements.txt
python server/main.py

```

## 3. ESP32 Client Configuration

The ESP32 firmware now runs purely as an MQTT listener and display driver.

1. **Dependencies:** Install the following via the Arduino Library Manager:
* `ESP32-HUB75-MatrixPanel-I2S-DMA`
* `AnimatedGIF`
* `PubSubClient`
* `ArduinoJson` (v7)
*(Note: `AsyncTCP` and `ESPAsyncWebServer` are no longer required and can be safely removed.)*


2. **Network Configuration:** Copy `esp32/ledmatrix/config.example.h` to `config.h` (this file is git-ignored). Define your WiFi credentials, `SERVER_URL`, `MQTT_HOST`, `MQTT_USER`, and `MQTT_PASS`.
3. **Flashing & Data Sync:** Compile and flash `esp32/ledmatrix/ledmatrix.ino`. Legacy GIFs stored on the ESP32 are obsolete. Upload your media through the new web UI; the server will automatically convert them to 64×64 and push them to the matrix.

## 4. Home Assistant Integration

The system natively supports Home Assistant via MQTT Discovery. Once the MQTT integration is pointed at the broker, a new device named **LED Matrix** will automatically populate with the following entities:

* **Light Controls:** Master On/Off, Brightness, Color selection.
* **Toggles:** GIF background, 24h clock format.
* **Selectors:** Overlays, GIF selection, Text size.
* **Inputs/Sliders:** Scrolling text content, Scroll delay (speed).
* **Sensors/Buttons:** Device reboot, WiFi signal strength, Free memory.

## 5. MQTT API Reference

Base Topic: `ledmatrix/`

| Topic | Flow | Payload Format | Description |
| --- | --- | --- | --- |
| `set` | Server → ESP | JSON | Pushes the full desired state (retained). |
| `cmd` | Server → ESP | JSON | Action commands (e.g., `{"reboot":true}`, `{"delete":"x.gif"}`). |
| `status` | ESP → Server | String | Availability state: `online` or `offline` (retained, LWT). |
| `telemetry` | ESP → Server | JSON | Heartbeat every 30s (IP, RSSI, heap, uptime, firmware version). |
| `ha/…` | HA ↔ Server | JSON/String | Home Assistant MQTT discovery and entity topics. |

## 6. Architecture & Performance Optimizations

Previous versions suffered from visual artifacting and stuttering due to resource contention. The new architecture resolves these through several key optimizations:

* **Decoupled Render Loop:** Previously, `GIFDraw` executed per-scanline, recalculating local time, redrawing text, and computing Game of Life logic for every row of every frame. The render loop now strictly handles pixel copying, with overlays calculated and drawn only once per tick.
* **Resource Reclamation:** Removing the on-device web server, massive string-building operations for file directories, and HTTP polling eliminated CPU and RAM contention with the DMA display driver.
* **Stability Fixes:** Addressed severe bugs including a null pointer exception on `dma_display->color565()` during global initialization, illegal `ESP.restart()` calls inside request handlers, redundant hardware setup routines, and blocking GIF loops that starved network processes.

## 7. Implementation Notes

* **Font Limitations:** The panel font strictly supports ASCII. The server handles automatic transliteration of German umlauts (`ä/ö/ü/ß` translates to `ae/oe/ue/ss`).
* **Scroll Speed:** The scroll delay variable represents milliseconds per pixel step; a lower value results in faster text movement.
* **Offline Resilience:** If the server drops offline, the ESP32 will continue looping its current state (the local RTC maintains clock accuracy). It will automatically fetch the latest retained state upon reconnection.
* **Hardware Compilation:** The provided firmware has not been comprehensively tested on all hardware variants. If the compiler raises library API version errors, report the output for adjustments.
