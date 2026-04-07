import argparse
import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path
from urllib.parse import urlparse

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
        "--no-serial", action="store_true", help="Disable serial discovery (wireless-only mode)"
    )
    parser.add_argument(
        "--wifi-device",
        action="append",
        metavar="HOST:PORT",
        help="Add a WiFi device (e.g., 192.168.86.238:3333). Repeatable.",
    )
    args = parser.parse_args()

    log_level = getattr(logging, args.log_level.upper())
    log_fmt = logging.Formatter(
        "%(asctime)s %(levelname)-5s %(name)s: %(message)s", datefmt="%H:%M:%S"
    )

    # Console handler (for tmux / interactive use)
    console = logging.StreamHandler()
    console.setFormatter(log_fmt)

    # File handler with rotation (persists across restarts)
    # 5 MB per file x 3 backups = 20 MB max. Append mode preserves logs on restart.
    log_dir = Path("/var/log/blinky-server")
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
    except PermissionError:
        log_dir = Path("/tmp")
    file_handler = RotatingFileHandler(
        log_dir / "blinky-server.log", maxBytes=5_000_000, backupCount=3
    )
    file_handler.setFormatter(log_fmt)

    logging.basicConfig(level=log_level, handlers=[console, file_handler])

    # Parse WiFi device specs (supports IPv6 bracket notation via urlparse)
    wifi_hosts = []
    for spec in args.wifi_device or []:
        # Wrap in a scheme so urlparse handles [::1]:3333 and 1.2.3.4:3333
        parsed = urlparse(f"tcp://{spec}")
        if not parsed.hostname:
            parser.error(f"Invalid --wifi-device spec: {spec}")
        host = parsed.hostname
        try:
            port = parsed.port or 3333
        except ValueError:
            parser.error(f"Invalid port in --wifi-device spec: {spec}")
        wifi_hosts.append({"host": host, "port": port})

    app = create_app(
        enable_ble=not args.no_ble,
        enable_serial=not args.no_serial,
        wifi_hosts=wifi_hosts,
    )
    uvicorn.run(app, host=args.host, port=args.port, log_level=args.log_level)


if __name__ == "__main__":
    main()
