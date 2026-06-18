from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from typing import Any, Dict, Optional


async def _request_unix(path: str, req: Dict[str, Any]) -> Dict[str, Any]:
    reader, writer = await asyncio.open_unix_connection(path)
    try:
        writer.write((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
        await writer.drain()
        line = await reader.readline()
        if not line:
            raise RuntimeError("no response")
        return json.loads(line.decode("utf-8", errors="strict"))
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def _request_tcp(host: str, port: int, req: Dict[str, Any]) -> Dict[str, Any]:
    reader, writer = await asyncio.open_connection(host, port)
    try:
        writer.write((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
        await writer.drain()
        line = await reader.readline()
        if not line:
            raise RuntimeError("no response")
        return json.loads(line.decode("utf-8", errors="strict"))
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="wifi_service IPC client")

    p.add_argument(
        "--socket",
        default=os.environ.get("WIFI_SERVICE_SOCKET", "/run/wifi_service.sock"),
        help="unix socket path (Linux). Default: /run/wifi_service.sock",
    )
    p.add_argument(
        "--host",
        default=os.environ.get("WIFI_SERVICE_HOST", "127.0.0.1"),
        help="tcp host (Windows/dev). Default: 127.0.0.1",
    )
    p.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("WIFI_SERVICE_PORT", "8765")),
        help="tcp port (Windows/dev). Default: 8765",
    )

    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping", help="health check")

    send = sub.add_parser("send", help="send UTF-8 text over WiFi")
    send.add_argument("text", help="text to send")
    send.add_argument(
        "--target",
        default=None,
        help="target id (e.g. artpi_1). If omitted, broadcast to all targets.",
    )
    send.add_argument(
        "--no-newline",
        action="store_true",
        help="do not append newline for framing",
    )

    op = sub.add_parser("op", help="send operation command (start / stop / reset-seq)")
    op.add_argument(
        "operation",
        choices=["start", "stop", "reset-seq"],
        help="operation: start, stop, or reset-seq",
    )
    op.add_argument(
        "--target",
        default="both",
        choices=["both", "left", "right", "ART-Pi2-IMU-L", "ART-Pi2-IMU-R"],
        help="target hand(s). Default: both",
    )

    sub.add_parser("clear", help="clear aligner buffer (pairs/frames)")

    return p


async def _main_async(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)

    if args.cmd == "ping":
        req: Dict[str, Any] = {"cmd": "ping"}
    elif args.cmd == "send":
        req = {
            "cmd": "send",
            "text": args.text,
            "add_newline": not args.no_newline,
        }
        if args.target:
            req["target"] = args.target
    elif args.cmd == "op":
        req = {
            "cmd": "op_cmd",
            "op": args.operation.replace("-", "_"),
            "target": args.target,
        }
    elif args.cmd == "clear":
        req = {"cmd": "clear_pairs"}
    else:
        raise AssertionError("unreachable")

    # Prefer unix socket on Linux.
    if os.name != "nt":
        resp = await _request_unix(args.socket, req)
    else:
        resp = await _request_tcp(args.host, args.port, req)

    sys.stdout.write(json.dumps(resp, ensure_ascii=False, indent=2) + "\n")
    return 0


def main() -> None:
    raise SystemExit(asyncio.run(_main_async()))


if __name__ == "__main__":
    main()
