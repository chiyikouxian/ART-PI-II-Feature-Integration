from __future__ import annotations

import asyncio
import json
import logging
import os
from dataclasses import dataclass
from typing import Awaitable, Callable, Dict, Optional


_LOG = logging.getLogger(__name__)

RequestHandler = Callable[[Dict], Awaitable[Dict]]


@dataclass
class IpcConfig:
    unix_socket_path: str = "/run/wifi_service.sock"
    tcp_host: str = "127.0.0.1"
    tcp_port: int = 8765


class IpcServer:
    """JSON-lines control channel for the WiFi service."""

    def __init__(self, cfg: IpcConfig, handler: RequestHandler) -> None:
        self._cfg = cfg
        self._handler = handler
        self._server: Optional[asyncio.AbstractServer] = None

    async def start(self) -> None:
        if os.name == "nt":
            await self._start_tcp()
        else:
            await self._start_unix()

    async def close(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

    async def _start_unix(self) -> None:
        sock_path = self._cfg.unix_socket_path
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass
        except Exception:
            _LOG.exception("failed to unlink stale socket: %s", sock_path)

        _LOG.info("IPC listening (unix): %s", sock_path)
        self._server = await asyncio.start_unix_server(self._client_connected, path=sock_path)

        try:
            os.chmod(sock_path, 0o600)
        except Exception:
            pass

    async def _start_tcp(self) -> None:
        _LOG.info("IPC listening (tcp): %s:%s", self._cfg.tcp_host, self._cfg.tcp_port)
        self._server = await asyncio.start_server(
            self._client_connected,
            host=self._cfg.tcp_host,
            port=self._cfg.tcp_port,
        )

    def _bad_request(self, err: Exception) -> bytes:
        resp = {"ok": False, "error": f"bad request: {err}"}
        return (json.dumps(resp) + "\n").encode("utf-8")

    async def _handle_line(self, line: bytes) -> bytes:
        try:
            req = json.loads(line.decode("utf-8", errors="strict"))
            if not isinstance(req, dict):
                raise ValueError("request must be a JSON object")
        except Exception as exc:
            return self._bad_request(exc)

        try:
            out = await self._handler(req)
            resp = {"ok": True, "data": out}
        except Exception as exc:
            _LOG.exception("handler error")
            resp = {"ok": False, "error": str(exc)}

        return (json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8")

    async def _client_connected(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        peer = writer.get_extra_info("peername")
        _LOG.info("IPC client connected: %s", peer)

        try:
            while not reader.at_eof():
                line = await reader.readline()
                if not line:
                    break

                line = line.strip()
                if not line:
                    continue

                writer.write(await self._handle_line(line))
                await writer.drain()
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            _LOG.info("IPC client disconnected: %s", peer)
