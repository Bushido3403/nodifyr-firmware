# Nodifyr cellular gateway (nRF9151)

Freestanding NCS **v3.4.0** application for the **nRF9151 DK**
(`nrf9151dk/nrf9151/ns`, PCA10171) and **Thingy:91 X**
(`thingy91x/nrf9151/ns`, PCA20065).

Attaches LTE via Hologram (`APN=hologram`, IPv4), pairs through the Nodifyr provision API, persists `api_key` / `api_url` in Settings/NVS, and uploads synthetic `nodifyr.radar.car.v1` readings over HTTPS with TLS validation (ISRG Root X1).

Contract: [`docs/cellular-pairing-handoff.md`](../../docs/cellular-pairing-handoff.md).

**After flash — pair with the web app:** [`docs/cellular-gateway-setup.md`](../../docs/cellular-gateway-setup.md).

Validated against sibling **nodifyr-app** (`device-types.ts`, pair/status routes, telemetry schema):
`nodifyr.radar.car.v1` allowlisted; `nodifyr.radar.gap.v1` **not** registered (gap Kconfig stays off);
`X-Api-Key`; batch max 64; `direction_deg` 0..359; `api_url` is a full `https://…/api/v1/telemetry` URL.

## Status

| Area | Status |
|------|--------|
| App scaffold + sources (tasks 2–6) | Implemented in-repo |
| Local NCS install / build / flash / UART | **Deferred** — requires a complete NCS v3.4.0 install on the laptop |
| Hardware acceptance (SIM, DK, cloud pair) | **Requires hardware** |

`CONFIG_NODIFYR_GAP_TELEMETRY` defaults to **n**. Do not enable until nodifyr-app registers `nodifyr.radar.gap.v1`.

## Layout

```
apps/cellular-gateway/
  CMakeLists.txt
  prj.conf
  Kconfig
  sysbuild.conf
  boards/nrf9151dk_nrf9151_ns.conf
  boards/thingy91x_nrf9151_ns.conf   # MINIMAL TF-M (factory partitions)
  cert/isrgrootx1.pem
  src/
    main.c          # BOOT → PAIRING | NORMAL | ERROR | FATAL
    lte.c           # modem init, CA provision, LTE attach
    identity.c      # Settings/NVS + IMEI hardware_id
    https_client.c  # TLS socket + http_client_req
    pairing.c       # POST /pair + GET /status
    device_config.c # GET /api/v1/config (upload mode + duty cycle)
    telemetry.c     # ring buffer + detailed/summary + batch upload
    radar_stub.c    # synthetic car.v1 producer
    json_util.c
```

## Runtime config (dashboard → device)

After pairing, each telemetry cycle also fetches `GET /api/v1/config`:

| Key | Effect |
|-----|--------|
| `upload_mode` | `detailed` = every detection; `summary` = one condensed reading per upload interval |
| `upload_interval_sec` | How often to upload **and** check for config updates |

Each upload also sends `device_ts` (wall-clock epoch ms) and, on Thingy:91 X,
`battery_pct` (0–100 from nPM1300 voltage). Empty `readings` is allowed for a
battery heartbeat.

Edit under **Devices → Configure**. Cost estimates use **$0.03/MB**.

## Dev pairing secret

Lab default is hardcoded in [`prj.conf`](prj.conf):

```text
CONFIG_NODIFYR_PAIRING_SECRET="NODI-FYR1-DK91-BRNG"
```

Use that code in **Devices → Pair device**. Override at build time if needed:

```bash
west build ... -- -DCONFIG_NODIFYR_PAIRING_SECRET=\"YOUR-CODE-HERE\"
```

Leave empty for release; production devices must be factory-provisioned into Settings.

## Laptop: build (NCS v3.4.0)

Resolve your install path (do this on the laptop with a **complete** SDK):

```bash
nrfutil sdk-manager list --all-fields
```

Set variables (example path — replace with yours):

```bash
export NCS_VERSION=v3.4.0
export NCS_DIR="$HOME/ncs/v3.4.0"   # from sdk-manager list
export APP_DIR="$(pwd)/apps/cellular-gateway"
export BUILD_DIR="$APP_DIR/build"
```

Build freestanding with sysbuild (required for out-of-tree `/ns` apps):

```bash
mkdir -p "$BUILD_DIR"
nrfutil sdk-manager toolchain launch --ncs-version="$NCS_VERSION" \
  --chdir "$NCS_DIR" -- \
  west build -p -b nrf9151dk/nrf9151/ns --sysbuild \
  -d "$BUILD_DIR" "$APP_DIR" \
  > "$BUILD_DIR/build.log" 2>&1
tail -n 80 "$BUILD_DIR/build.log"
```

Optional bring-up secret:

```bash
nrfutil sdk-manager toolchain launch --ncs-version="$NCS_VERSION" \
  --chdir "$NCS_DIR" -- \
  west build -p -b nrf9151dk/nrf9151/ns --sysbuild \
  -d "$BUILD_DIR" "$APP_DIR" \
  -- -DCONFIG_NODIFYR_PAIRING_SECRET=\"ABCD-EFGH-JKMN-PQRS\" \
  > "$BUILD_DIR/build.log" 2>&1
```

Artifact (sysbuild): `$BUILD_DIR/cellular-gateway/zephyr/zephyr.hex` (confirm with `ls`).

### Thingy:91 X build

Factory layout uses MCUboot + b0 and a tiny TF-M region — the Thingy board
fragment sets `CONFIG_TFM_PROFILE_TYPE_MINIMAL=y` (do not reuse the DK SMALL
profile).

```bash
export BUILD_DIR="$APP_DIR/build_thingy91x"
nrfutil sdk-manager toolchain launch --ncs-version="$NCS_VERSION" \
  --chdir "$NCS_DIR" -- \
  west build -p -b thingy91x/nrf9151/ns --sysbuild \
  -d "$BUILD_DIR" "$APP_DIR"
```

DFU zip: `$BUILD_DIR/dfu_application.zip`.

## Laptop: flash

### nRF9151 DK (J-Link)

```bash
nrfutil device list
nrfutil sdk-manager toolchain launch --ncs-version="$NCS_VERSION" \
  --chdir "$NCS_DIR" -- \
  west flash -d "$BUILD_DIR"
```

If multiple DKs are attached, add `--dev-id <SEGGER_SN>`.

### Thingy:91 X (USB MCUboot — no on-board debugger)

1. Hold **SW3** while power-cycling (or plugging USB) to enter serial recovery.
2. Confirm the device shows as `THINGY91X_…` / `mcuBoot` in `nrfutil device list`.
3. Program the application DFU zip:

```bash
nrfutil device program \
  --firmware "$APP_DIR/build_thingy91x/dfu_application.zip" \
  --serial-number THINGY91X_B5D670E85FA
```

Do **not** `west flash` a DK `merged.hex` to the Thingy’s USB serial port.

## Laptop: UART verify

nRF9151 DK application logs are typically on the first VCOM (`uart0`), 115200 8N1. Nordic DK UART needs **DTR**.

```bash
nrfutil device list
# Note the serial port for the DK (e.g. /dev/tty.usbmodemXXXX)

nrfutil device reset --serial-number <SEGGER_SN>

# Prefer the Nordic UART monitor script (assert DTR):
uv run nordicsemi_uart_monitor.py read --port /dev/tty.usbmodemXXXX \
  --baud 115200 --duration 120
```

### Acceptance log checklist

- Cold boot without `api_key` → `POST /pair` → `status=pending`
- Dashboard sticker approve → next status poll persists `ngw_...` (truncated in logs)
- Reboot skips pairing → stub cars upload → visible in Nodifyr
- Second status poll would be `active` (no re-issue) when already keyed
- Logs never print full `pairing_secret` or `api_key`

## Still needs hardware / toolchain testing

- Full NCS v3.4.0 install completeness on this machine
- Modem firmware version on the DK
- Hologram SIM attach (APN `hologram`, IPv4-only PDN)
- TLS handshake to `app.nodifyr.io` with provisioned ISRG Root X1 (Let’s Encrypt YR1 chain)
- End-to-end pair + telemetry against production cloud
- Settings/NVS survival across reboot
- UART VCOM selection if logs do not appear on the first port

## Out of scope

ESP SoftAP tree (`esp-test-DO_NOT_USE/`), real radar UART/SPI driver, people/animal types, enabling gap telemetry before the app registers the type.
