import argparse
import logging

import uvicorn

from .api.app import create_app


def main() -> None:
    parser = argparse.ArgumentParser(description="Blinky Fleet Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address")
    parser.add_argument("--port", type=int, default=8420, help="HTTP port")
    parser.add_argument(
        "--log-level", default="info", choices=["debug", "info", "warning", "error"]
    )
    parser.add_argument("--no-ble", action="store_true", help="Disable BLE discovery")
    parser.add_argument(
        "--wifi-device", action="append", metavar="HOST:PORT",
        help="Add a WiFi device (e.g., 192.168.86.238:3333). Repeatable.",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    # Parse WiFi device specs
    wifi_hosts = []
    for spec in args.wifi_device or []:
        parts = spec.rsplit(":", 1)
        host = parts[0]
        port = int(parts[1]) if len(parts) > 1 else 3333
        wifi_hosts.append({"host": host, "port": port})

    app = create_app(enable_ble=not args.no_ble, wifi_hosts=wifi_hosts)
    uvicorn.run(app, host=args.host, port=args.port, log_level=args.log_level)


if __name__ == "__main__":
    main()
