# Cellular gateway setup (after flash)

How to bring up a flashed Nodifyr nRF9151 cellular gateway with the web app at
[app.nodifyr.io](https://app.nodifyr.io).

This covers **customer / lab bring-up**. For the HTTP contract and firmware
state machine, see [`cellular-pairing-handoff.md`](cellular-pairing-handoff.md).
For build and flash commands, see [`apps/cellular-gateway/README.md`](../apps/cellular-gateway/README.md).

## What you need

| Item | Notes |
|------|-------|
| Flashed nRF9151 DK (or product board) | Board target `nrf9151dk/nrf9151/ns` |
| Hologram IoT SIM | APN `hologram`, IPv4 — seated in the modem |
| Antenna | LTE antenna connected |
| Pairing code | Printed on the device sticker, **or** the same value you baked into the image for bring-up |
| Nodifyr account | Org admin access at [app.nodifyr.io](https://app.nodifyr.io) |

There is **no Wi‑Fi SoftAP and no BLE onboarding**. The gateway talks only
outbound HTTPS to `app.nodifyr.io` over cellular.

### Bring-up pairing secret (lab only)

Production units must have `pairing_secret` factory-written into Settings/NVS.

**Current lab default** (hardcoded in
[`apps/cellular-gateway/prj.conf`](../apps/cellular-gateway/prj.conf)):

```text
NODI-FYR1-DK91-BRNG
```

Dashes and capitalization are ignored by the cloud. Type that code under
**Devices → Pair device**. Override at build time with
`-DCONFIG_NODIFYR_PAIRING_SECRET=\"…\"` if you need a different value; leave
empty for release images that expect factory NVS provisioning.

## End-to-end flow

```text
1. Power on with SIM + antenna
2. Device attaches LTE → POST /api/v1/provision/pair  (status: pending)
3. You open Devices → Pair device → enter sticker code
4. Device GET /api/v1/provision/pair/status → receives api_key once
5. Device uploads nodifyr.radar.car.v1 readings → Devices page updates
```

**Order matters:** the gateway must announce itself (step 2) **before** you
approve in the web app. Approving a code the cloud has never seen returns
“No device found for this pairing code.”

## Step 1 — Power on and wait for cellular

1. Insert the Hologram SIM and connect the LTE antenna.
2. Power the board (USB on the DK is fine).
3. Optional: watch UART (`115200 8N1`, assert **DTR**). Expected progression:

| Log theme | Meaning |
|-----------|---------|
| Modem / LTE attach | APN hologram, IPv4 |
| IMEI / IP | Network up (`hardware_id` is `nrf-<imei>`) |
| `POST /pair` | Device registered as **pending** |
| `Pairing pending; next poll in … s` | Waiting for you in the dashboard (~45 s polls) |

If you see `FATAL: no pairing_secret`, the image was not seeded and flash has
no secret — rebuild with `CONFIG_NODIFYR_PAIRING_SECRET` or factory-provision
NVS.

If LTE never attaches, check SIM activation in Hologram, APN, antenna, and
modem firmware — not the web app.

## Step 2 — Pair in the web app

1. Sign in at [https://app.nodifyr.io](https://app.nodifyr.io).
2. Open **Devices**.
3. Click **Pair device**.
4. Enter the **pairing code** from the sticker (or your bring-up
   `CONFIG_NODIFYR_PAIRING_SECRET`). Optional: set a display name
   (e.g. `Main St radar`).
5. Submit **Pair device**.

On success the UI confirms the device is in your organization and that upload
credentials are delivered on the gateway’s next cellular check-in (typically
within one poll interval, ~30–60 seconds).

You should then see the gateway under **Paired** with its
`nrf-<imei>` hardware id.

## Step 3 — Confirm the device finished pairing

On the next status poll the firmware:

1. Receives `api_key` + `api_url` **once**
2. Writes them to Settings/NVS immediately
3. Leaves PAIRING and enters NORMAL (radar stub + telemetry)

UART cues:

- Truncated key log (`api_key: ngw_...xyz`) — never the full key
- `Pairing complete — entering NORMAL`
- Periodic stub car detections and HTTP telemetry posts

Reboot after a successful pair should skip pairing (`Boot: api_key present`)
and go straight to NORMAL.

## Step 3b — Configure upload mode and interval

Under **Devices → Configure**:

| Setting | Meaning |
|---------|---------|
| Upload mode | **Detailed** = every vehicle event; **Summary** = one condensed reading per interval |
| Upload interval | How often the gateway uploads data **and** checks for config updates |

The UI shows an estimated monthly cellular cost at **$0.03/MB**. Changes apply
on the device’s next upload cycle (or immediately after reboot).

## Step 4 — Confirm data in the web app

On **Devices**:

| Section | What to look for |
|---------|------------------|
| **Paired** | Device listed; **Last seen** updates after uploads; status moves toward online |
| **Sensors** | Sensor nodes appear once `nodifyr.radar.car.v1` readings land |

The lab image emits **synthetic** car detections every
`CONFIG_NODIFYR_RADAR_STUB_INTERVAL_S` seconds (default 15). Real radar
hardware is not required for this check.

Also useful: **Events** for recent activity once readings exist.

Refresh the Devices page if the UI was open during the first upload.

## Day-2 behavior

- **Power cycle** — stays paired; credentials survive in NVS.
- **Re-pair / lost key** — the API key is delivered only once. If flash is
  erased after approval, the status endpoint returns `active` without a key
  and the device cannot self-recover; support must revoke and re-provision.
- **Revoked device** — cloud returns `403`; firmware stops polling and enters
  ERROR.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---------|--------------|------------|
| Dashboard: “No device found for this pairing code” | Device has not POSTed `/pair` yet, or wrong code | Power on first; wait for LTE + pair announce; confirm sticker matches flash secret |
| Dashboard: “Device already paired” | Already bound to an org | Use Devices list; revoke/re-pair only via support flow |
| UART stuck on `Pairing pending` | Approved with a different secret, or approve never happened | Confirm the exact code used in **Pair device**; wait one poll (~45 s) after approve |
| `FATAL: no pairing_secret` | Empty NVS and empty Kconfig seed | Flash with `-DCONFIG_NODIFYR_PAIRING_SECRET=…` |
| LTE never connects | SIM / APN / RF | Hologram portal, antenna, `hologram` APN |
| Paired but no Sensors | Upload failing (TLS, time, auth) | Check UART for HTTP errors; wall clock must sync before meaningful `ts` |
| Telemetry `401` / `403` | Bad or revoked key | ERROR state on device; re-provision via support |

### Quick cloud sanity check (optional)

With the same secret the device uses (lab only):

```bash
# After the device has announced once:
curl -sS -X POST https://app.nodifyr.io/api/v1/provision/pair \
  -H "Content-Type: application/json" \
  -d '{"hardware_id":"nrf-<imei>","pairing_secret":"ABCD-EFGH-JKMN-PQRS"}'
# → {"ok":true,"status":"pending"}  (or approved if you already paired in UI)
```

Prefer using the real device + dashboard rather than forging approvals in
production.

## Security reminders

- Never paste full `api_key` or `pairing_secret` into tickets or logs.
- Treat sticker codes like device passwords.
- Always use production HTTPS with certificate validation (firmware already
  provisions ISRG Root X1 for Let’s Encrypt).

## Related docs

- [`cellular-pairing-handoff.md`](cellular-pairing-handoff.md) — API contract
- [`apps/cellular-gateway/README.md`](../apps/cellular-gateway/README.md) — build, flash, UART
- [`apps/cellular-gateway/scripts/README.md`](../apps/cellular-gateway/scripts/README.md) — UART monitor helper
