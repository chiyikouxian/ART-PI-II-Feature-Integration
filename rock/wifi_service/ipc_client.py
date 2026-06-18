from __future__ import annotations

import asyncio
import json
import os
from dataclasses import dataclass
from typing import Any, Dict, Optional


@dataclass
class IpcClientConfig:
    unix_socket_path: str = "/run/wifi_service.sock"
    tcp_host: str = "127.0.0.1"
    tcp_port: int = 8765


class IpcClient:
    """Persistent JSON-lines IPC client.

    Protocol: request/response are JSON objects, one per line.
    Server response format: {"ok": bool, "data": {...}} or {"ok": false, "error": "..."}
    """

    def __init__(self, cfg: IpcClientConfig) -> None:
        self._cfg = cfg
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._lock = asyncio.Lock()

    async def connect(self) -> None:
        if self._writer is not None:
            return
        if os.name != "nt":
            self._reader, self._writer = await asyncio.open_unix_connection(self._cfg.unix_socket_path)
        else:
            self._reader, self._writer = await asyncio.open_connection(self._cfg.tcp_host, self._cfg.tcp_port)

    async def close(self) -> None:
        if self._writer is None:
            return
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except Exception:
            pass
        self._reader = None
        self._writer = None

    async def request(self, req: Dict[str, Any], *, timeout: float = 5.0) -> Dict[str, Any]:
        async with self._lock:
            await self.connect()
            assert self._reader is not None and self._writer is not None

            self._writer.write((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
            await self._writer.drain()

            line = await asyncio.wait_for(self._reader.readline(), timeout=timeout)
            if not line:
                raise RuntimeError("no response")
            resp = json.loads(line.decode("utf-8", errors="strict"))
            if not isinstance(resp, dict):
                raise RuntimeError("bad response")
            if not resp.get("ok", False):
                raise RuntimeError(str(resp.get("error", "unknown error")))
            data = resp.get("data", {})
            if not isinstance(data, dict):
                raise RuntimeError("bad response data")
            return data
