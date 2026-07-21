# nodifyr-firmware

Embedded firmware for Nodifyr hardware.

## Applications

| Path | Platform | Notes |
|------|----------|-------|
| [`apps/cellular-gateway/`](apps/cellular-gateway/) | nRF9151 + NCS v3.4.0 | Cellular pairing + car radar upload (active) |

The legacy ESP SoftAP tree (`esp-test-DO_NOT_USE/`) was removed upstream; do not restore it for cellular work.

Cloud contract: [`docs/cellular-pairing-handoff.md`](docs/cellular-pairing-handoff.md).

After flash — pair with the web app: [`docs/cellular-gateway-setup.md`](docs/cellular-gateway-setup.md).

Build/flash/UART for the cellular gateway: see [`apps/cellular-gateway/README.md`](apps/cellular-gateway/README.md).
