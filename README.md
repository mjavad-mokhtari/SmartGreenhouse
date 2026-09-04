# SGH Controller

Local-first ESP32 controller with independently compiled irrigation and lighting services.

## Layout

- `src/core/`: RTC, NVS-backed Wi-Fi, and status LED services.
- `src/modules/irrigation/`: active-low pump/zone outputs and soil ADC service.
- `src/modules/lighting/`: active-low lighting channel service and scheduling hook.
- `src/web/`: asynchronous HTTP API and embedded responsive dashboard.

## Build targets

```sh
pio run -e irrigation
pio run -e lighting
pio run -e unified_all_in_one
```

The default target is `unified_all_in_one`. Each environment compiles only its selected module through `build_src_filter` and defines `MODULE_IRRIGATION` and/or `MODULE_LIGHTING`.

## Upload and monitor

Set `upload_port` and `monitor_port` for the connected board, or pass the port on the command line:

```sh
pio run -e unified_all_in_one -t upload --upload-port /dev/cu.usbserial-0001
pio device monitor --port /dev/cu.usbserial-0001 -b 115200
```

On first boot without saved Wi-Fi credentials, connect to `SGH-Setup` with password `irrigation`. The dashboard is available at `http://192.168.4.1/`.

## Central server

The optional central dashboard runs from `server/` and receives event batches
from one or more controllers:

```sh
cd server
npm install
API_KEY='replace-with-a-long-random-key' PORT=3000 npm start
```

Open `http://localhost:3000/`. Configure the controller's server URL as
`http://<server-ip>:3000/`. The server stores events in `server/greenhouse.db`;
remote control and retention deletion require the same `API_KEY` via the
`X-API-Key` header.
