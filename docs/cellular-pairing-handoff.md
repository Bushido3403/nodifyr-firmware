# nRF9151 Cellular Pairing + Car Radar — Firmware Handoff

Use this prompt when implementing the Nodifyr cellular gateway firmware
(nRF9151 + Hologram IoT SIM). The cloud side is implemented and deployed in
the Nodifyr web app. There is **no WiFi, no SoftAP, and no BLE onboarding** —
the device onboards itself over cellular and the customer approves it from the
website.

---

## Platform assumptions

| Item | Value |
|------|-------|
| MCU / modem | nRF9151 (LTE-M / NB-IoT) |
| SIM | Hologram IoT |
| Transport | Outbound HTTPS (TLS 1.2+) to `app.nodifyr.io`; the cloud does not care how IP is obtained |
| Sensor | Car radar (speed / direction per detected vehicle) |
| Time | Device must have wall-clock time before uploading (modem network time or NTP) — `ts` is Unix epoch **milliseconds** |

## Device identity and credentials

| Credential | Role | Storage |
|-----------|------|---------|
| `hardware_id` | Public label only. Recommend the modem **IMEI** (e.g. `nrf-350457790012345`) or factory serial. Never treated as a secret. | Derived at boot or factory-set |
| `pairing_secret` | Proof of possession. High-entropy code printed on the device sticker **and** burned into device storage at manufacture. 12–64 alphanumeric chars; dashes/case are ignored by the cloud (`ABCD-EFGH-JKMN-PQRS` == `abcdefghjkmnpqrs`). | Factory-provisioned, persistent storage |
| `api_key` | Upload credential (`ngw_` + 48 hex chars). Issued **exactly once** via the pairing status poll after the customer approves the device. | Persistent storage, written immediately on receipt |
| `api_url` | Telemetry endpoint returned alongside the API key. | Persistent storage |

Never log the full `api_key` or `pairing_secret`; truncate (`ngw_...abc`).

## Cloud API

Discovery (no auth): `GET https://app.nodifyr.io/api/v1/provision/pair` returns
`{ "pair_url": ..., "status_url": ..., "telemetry_url": ... }`.

### 1. Pairing announcement — `POST /api/v1/provision/pair`

Send once cellular + TLS are up and the device has no stored `api_key`:

```http
POST /api/v1/provision/pair
Content-Type: application/json

{
  "hardware_id": "nrf-350457790012345",
  "pairing_secret": "ABCD-EFGH-JKMN-PQRS"
}
```

| Status | Body | Meaning |
|--------|------|---------|
| `200` | `{"ok":true,"status":"pending"}` | Registered; wait for customer approval |
| `200` | `{"ok":true,"status":"approved"}` | Approved; call the status endpoint to collect the key |
| `400` | `{"error":...}` | Malformed body / secret too short |
| `401` | | Secret does not match this hardware_id |
| `403` | | Device revoked — stop retrying, surface error state |
| `409` | | Already paired, or secret registered to another device |
| `429` | | Rate limited — back off |

### 2. Poll for approval — `GET /api/v1/provision/pair/status`

```http
GET /api/v1/provision/pair/status?hardware_id=nrf-350457790012345&pairing_secret=ABCD-EFGH-JKMN-PQRS
```

| Response | Meaning |
|----------|---------|
| `{"status":"pending"}` | Not approved yet — keep polling |
| `{"status":"approved","api_key":"ngw_...","api_url":"https://app.nodifyr.io/api/v1/telemetry","device_id":"<uuid>"}` | **Delivered exactly once.** Write `api_key` + `api_url` to persistent storage immediately, then switch to normal operation |
| `{"status":"active"}` | Key was already delivered on an earlier poll. If the device does not hold a key, it cannot recover on its own — surface an error (support must revoke/re-pair) |
| `401` / `403` / `429` | As above |

**Poll interval:** 30–60 s while pending. If power budget matters, back off to
2–5 min after the first hour. Stop polling on `403`.

**Write-before-ack:** persist the key before doing anything else with the
response; it is never returned again.

### 3. Telemetry upload — `POST /api/v1/telemetry` (unchanged contract)

```http
POST /api/v1/telemetry
Content-Type: application/json
X-Api-Key: ngw_<48_hex_chars>

{
  "readings": [
    {
      "ts": 1721500000123,
      "rssi": 0,
      "mac": "02:00:00:00:00:01",
      "device_type": "nodifyr.radar.car.v1",
      "sequence": 101,
      "fields": {
        "speed_centi_kph": 4520,
        "direction_deg": 180,
        "length_cm": 420,
        "confidence_pct": 91
      }
    }
  ]
}
```

Rules:

- `Authorization: Bearer ngw_...` is also accepted; `X-Api-Key` preferred.
- Max **64** readings per batch. One detected vehicle = one reading.
- **Not idempotent** — any 2xx is success (`{"ok":true,"accepted":N}`); do not
  retry a batch that may have succeeded. Non-2xx → re-queue.
- `400` means the schema rejected the payload — log and drop the batch (do not
  retry unchanged).

Field contract for `nodifyr.radar.car.v1`:

| Field | Required | Range | Meaning |
|-------|----------|-------|---------|
| `ts` | yes | epoch ms | Detection time (network-synced clock) |
| `mac` | yes | `xx:xx:xx:xx:xx:xx` | Stable synthetic id for the radar unit on this gateway (locally administered MAC is fine, e.g. `02:00:00:00:00:01`); not a BLE address |
| `rssi` | yes | -128..127 | Signal-quality proxy or `0` if unused |
| `sequence` | yes | 0..65535 | Rolling detection counter (wraps) |
| `fields.speed_centi_kph` | yes | 0..65535 | Speed × 100 (45.20 km/h → `4520`) |
| `fields.direction_deg` | yes | 0..359 | Travel direction |
| `fields.length_cm` | no | 0..65535 | Estimated vehicle length |
| `fields.confidence_pct` | no | 0..100 | Classifier confidence |

## Device state machine

```text
BOOT
 ├─ api_key in storage?  ──yes──► NORMAL: radar capture → batch → upload every N sec
 └─ no
     └─ pairing_secret in storage?  ──no──► fatal config error (factory issue)
         └─ yes
             ├─ POST /provision/pair  (retry w/ backoff on network failure)
             └─ poll GET /provision/pair/status every 30–60 s
                 ├─ "approved" → persist api_key + api_url → NORMAL
                 ├─ "active" without local key → error state (support)
                 └─ 403 revoked → error state (stop polling)
```

Customer-side flow for context: the user powers the device, then enters the
sticker code under **Devices → Pair device** in the Nodifyr dashboard. That
binds the device to their organization; the key is handed out on the device's
next status poll — usually within one poll interval.

## Security requirements

- HTTPS with certificate validation for every call (pair, status, telemetry).
- Store `api_key` and `pairing_secret` in persistent storage not exposed over
  any debug interface you can avoid; never print them in full.
- The pairing endpoints are rate limited (HTTP `429`); honor backoff.
- Treat `403` (revoked) as terminal until re-provisioned by support.

## Testing checklist (against production cloud or a dev deployment)

```bash
# 1. Device announces (simulate firmware)
curl -sS -X POST https://app.nodifyr.io/api/v1/provision/pair \
  -H "Content-Type: application/json" \
  -d '{"hardware_id":"nrf-test-001","pairing_secret":"ABCD-EFGH-JKMN-PQRS"}'
# → {"ok":true,"status":"pending"}

# 2. Human: Devices → Pair device → enter ABCD-EFGH-JKMN-PQRS

# 3. Device polls
curl -sS 'https://app.nodifyr.io/api/v1/provision/pair/status?hardware_id=nrf-test-001&pairing_secret=ABCD-EFGH-JKMN-PQRS'
# → {"status":"approved","api_key":"ngw_...","api_url":...}

# 4. Second poll returns {"status":"active"} with no key (delivered once)

# 5. Upload a detection with the key
curl -sS -X POST https://app.nodifyr.io/api/v1/telemetry \
  -H "X-Api-Key: ngw_<key>" \
  -H "Content-Type: application/json" \
  -d '{"readings":[{"ts":1721500000123,"rssi":0,"mac":"02:00:00:00:00:01","device_type":"nodifyr.radar.car.v1","sequence":1,"fields":{"speed_centi_kph":4520,"direction_deg":180}}]}'
# → {"ok":true,"accepted":1}
```

## Decided questions

| Question | Answer |
|----------|--------|
| Telemetry URL / auth | Unchanged: `POST /api/v1/telemetry` + `X-Api-Key` |
| `hardware_id` for cellular SKU | IMEI-derived (`nrf-<imei>`) or factory serial; public label only |
| Sticker secret format | 12–64 alphanumeric, separators/case ignored, unique per device |
| Approval model | Device-initiated pending + customer approves on the website (no claim codes, no SoftAP) |
| Key delivery | Exactly once via status poll; device must persist immediately |
| `ts` | Unix epoch milliseconds — device must sync clock before uploading |
| Radar payload | `nodifyr.radar.car.v1`; cars only (people/animal types will be separate `device_type`s later) |
| Idempotent retries | No — 2xx means success, never re-send that batch |
