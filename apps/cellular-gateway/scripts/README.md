# Scripts

Obtain `nordicsemi_uart_monitor.py` from the Nordic MCP resource
`resource://nordicsemi/nordicsemi_uart_monitor.py` (or copy from the nRF Connect
tools docs), then:

```bash
uv run nordicsemi_uart_monitor.py read --port /dev/tty.usbmodemXXXX --baud 115200 --duration 120
```
