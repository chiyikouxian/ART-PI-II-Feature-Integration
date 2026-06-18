from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import time
from collections import deque
from dataclasses import dataclass
from typing import Any, Deque, Dict, List, Optional

from frame_pairing import PairAligner, PairedSample, parse_frame
from wifi_ipc import IpcConfig, IpcServer
from wifi_resource_manager import InstanceLock, default_lock_path, wait_for_file_disappear

SPECIAL_TEMPLATE_FINGER_VALUE_COUNT = 60
SPECIAL_TEMPLATE_DORSAL_VALUE_COUNT = 6
SPECIAL_TEMPLATE_VALUE_COUNTS = (
    SPECIAL_TEMPLATE_FINGER_VALUE_COUNT,
    SPECIAL_TEMPLATE_FINGER_VALUE_COUNT + SPECIAL_TEMPLATE_DORSAL_VALUE_COUNT,
)


@dataclass
class TextCodec:
    codec: str = "utf-8"
    newline: str = "\n"


@dataclass
class TargetConfig:
    id: str
    device_name: str
    listen_host: str
    listen_port: int
    accept_timeout_s: float
    reconnect_interval_s: float


@dataclass
class AppConfig:
    targets: List[TargetConfig]
    codec: TextCodec
    ipc: IpcConfig
    log_level: str = "INFO"


def _setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )


def load_config(path: str) -> AppConfig:
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)

    encoding = raw.get("encoding", {})
    ipc = raw.get("ipc", {})
    logging_cfg = raw.get("logging", {})

    targets_raw = raw.get("targets", [])
    if not isinstance(targets_raw, list) or not targets_raw:
        raise ValueError("config must contain non-empty array: targets")

    targets: List[TargetConfig] = []
    for t in targets_raw:
        if not isinstance(t, dict):
            raise ValueError("each targets[] entry must be an object")

        tid = str(t.get("id", "")).strip()
        if not tid:
            raise ValueError("each targets[] entry must have non-empty 'id'")

        targets.append(
            TargetConfig(
                id=tid,
                device_name=str(t.get("device_name", tid)).strip(),
                listen_host=str(t.get("listen_host", "0.0.0.0")).strip(),
                listen_port=int(t.get("listen_port", 0)),
                accept_timeout_s=float(t.get("accept_timeout_s", 15.0)),
                reconnect_interval_s=float(t.get("reconnect_interval_s", 3.0)),
            )
        )

    ids = [t.id for t in targets]
    if len(set(ids)) != len(ids):
        raise ValueError("targets[].id must be unique")

    return AppConfig(
        targets=targets,
        codec=TextCodec(
            codec=str(encoding.get("codec", "utf-8")),
            newline=str(encoding.get("newline", "\n")),
        ),
        ipc=IpcConfig(
            unix_socket_path=str(ipc.get("unix_socket_path", "/run/wifi_service.sock")),
            tcp_host=str(ipc.get("tcp_host", "127.0.0.1")),
            tcp_port=int(ipc.get("tcp_port", 8765)),
        ),
        log_level=str(logging_cfg.get("level", "INFO")),
    )


def parse_special_template(text: str) -> Optional[tuple[str, Dict[str, Any]]]:
    if not text.startswith("MODEL:"):
        return None

    body = text[len("MODEL:") :]
    parts = [part.strip() for part in body.split(",")]
    value_count = len(parts) - 1
    if value_count not in SPECIAL_TEMPLATE_VALUE_COUNTS:
        raise ValueError(
            f"MODEL value count must be one of {SPECIAL_TEMPLATE_VALUE_COUNTS}, got {value_count}"
        )

    hand = parts[0].lower()
    if hand not in ("left", "right"):
        raise ValueError(f"MODEL hand must be left/right, got {parts[0]!r}")

    values = [int(value) for value in parts[1:]]
    finger_values = values[:SPECIAL_TEMPLATE_FINGER_VALUE_COUNT]
    finger_windows = [finger_values[i : i + 6] for i in range(0, len(finger_values), 6)]
    if len(finger_windows) != 10:
        raise ValueError(f"MODEL must contain 10 finger channel windows, got {len(finger_windows)}")

    dorsal_window = None
    if value_count == SPECIAL_TEMPLATE_FINGER_VALUE_COUNT + SPECIAL_TEMPLATE_DORSAL_VALUE_COUNT:
        dorsal_window = values[SPECIAL_TEMPLATE_FINGER_VALUE_COUNT:]

    return hand, {
        "finger": finger_windows,
        "dorsal": dorsal_window,
    }


def parse_waiting_stop(text: str) -> Optional[str]:
    value = text.strip()
    if not value.startswith("WAITING_STOP:"):
        return None
    hand = value[len("WAITING_STOP:") :].strip().lower()
    if hand not in ("left", "right"):
        raise ValueError(f"WAITING_STOP hand must be left/right, got {hand!r}")
    return hand


class WifiPeer:
    def __init__(
        self,
        target_name: str,
        listen_host: str,
        listen_port: int,
        connect_timeout_s: float,
        codec: TextCodec,
    ) -> None:
        self._target_name = target_name
        self._listen_host = listen_host
        self._listen_port = int(listen_port)
        self._connect_timeout_s = float(connect_timeout_s)
        self._codec = codec
        self._server: Optional[asyncio.AbstractServer] = None
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._connected_evt = asyncio.Event()
        self._disconnected_evt = asyncio.Event()
        self._disconnected_evt.set()
        self._notify_cb = None
        self._read_task: Optional[asyncio.Task] = None
        self._lock = asyncio.Lock()

    def set_notify_callback(self, cb) -> None:
        self._notify_cb = cb

    @property
    def is_connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    @property
    def can_send(self) -> bool:
        return self.is_connected

    async def start(self) -> None:
        if self._server is not None:
            return
        self._server = await asyncio.start_server(
            self._client_connected,
            host=self._listen_host,
            port=self._listen_port,
        )

    async def _client_connected(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        peer = writer.get_extra_info("peername")
        log = logging.getLogger("wifi_service")
        async with self._lock:
            if self._writer is not None:
                try:
                    self._writer.close()
                    await self._writer.wait_closed()
                except Exception:
                    pass
            self._reader = reader
            self._writer = writer
            self._connected_evt.set()
            self._disconnected_evt.clear()
            log.info("[%s] wifi client connected: %s", self._target_name, peer)
            self._read_task = asyncio.create_task(self._read_loop(reader, writer, peer))

    async def _read_loop(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        peer: Any,
    ) -> None:
        log = logging.getLogger("wifi_service")
        try:
            while not reader.at_eof():
                line = await reader.readline()
                if not line:
                    break
                text = line.decode(self._codec.codec, errors="replace").strip()
                if not text:
                    continue
                log.info("[%s] RX text: %s", self._target_name, text)
                if self._notify_cb:
                    try:
                        await self._notify_cb(text)
                    except Exception:
                        log.exception("notify callback failed (%s)", self._target_name)
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("[%s] wifi read loop failed", self._target_name)
        finally:
            async with self._lock:
                if self._writer is writer:
                    self._reader = None
                    self._writer = None
                    self._connected_evt.clear()
                    self._disconnected_evt.set()
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            log.info("[%s] wifi client disconnected: %s", self._target_name, peer)

    async def wait_connected(self) -> None:
        await self._connected_evt.wait()

    async def wait_disconnected(self) -> None:
        await self._disconnected_evt.wait()

    async def ensure_connected(self) -> None:
        if self.is_connected:
            return
        await asyncio.wait_for(self._connected_evt.wait(), timeout=self._connect_timeout_s)

    async def send(self, data: bytes) -> None:
        await self.ensure_connected()
        writer = self._writer
        if writer is None:
            raise RuntimeError("wifi client not connected")
        try:
            writer.write(data)
            await writer.drain()
        except Exception:
            async with self._lock:
                if self._writer is writer:
                    self._reader = None
                    self._writer = None
                    self._connected_evt.clear()
                    self._disconnected_evt.set()
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            raise

    async def send_text(self, text: str, add_newline: bool = True) -> None:
        payload = text
        if add_newline and self._codec.newline:
            payload += self._codec.newline
        await self.send(payload.encode(self._codec.codec, errors="strict"))

    async def close(self) -> None:
        if self._read_task is not None:
            self._read_task.cancel()
            try:
                await self._read_task
            except Exception:
                pass
            self._read_task = None
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
            self._writer = None
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None


async def _peer_loop(peer: WifiPeer, reconnect_interval_s: float, label: str) -> None:
    log = logging.getLogger("wifi_service")
    while True:
        try:
            await peer.wait_connected()
            await peer.wait_disconnected()
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.warning("wifi loop error (%s): %s", label, exc)
        await asyncio.sleep(reconnect_interval_s)


async def _announce_when_all_connected(
    peers: Dict[str, WifiPeer],
    *,
    connected_text: str = "wifi已连接",
    disconnected_text: str = "wifi已断连",
    stable_s: float = 0.3,
    cooldown_s: float = 30.0,
) -> None:
    log = logging.getLogger("wifi_service")
    left_targets = [tid for tid in sorted(peers.keys()) if "IMU-L" in tid]
    target = left_targets[0] if left_targets else (sorted(peers.keys())[0] if peers else "")
    if not target:
        log.warning("skip wifi status announcement: no wifi target configured")
        return

    stable_since: Optional[float] = None
    announced_for_current_connection = False
    last_announce_ts = 0.0
    was_target_sendable = False

    while True:
        now = time.monotonic()
        target_sendable = target in peers and peers[target].can_send

        if target_sendable:
            if stable_since is None:
                stable_since = now
            stable_long_enough = (now - stable_since) >= max(0.0, float(stable_s))
            cooldown_ok = (now - last_announce_ts) >= max(0.0, float(cooldown_s))
            if stable_long_enough and not announced_for_current_connection and cooldown_ok:
                try:
                    await peers[target].send_text(connected_text, add_newline=True)
                    last_announce_ts = now
                    announced_for_current_connection = True
                    log.info("sent wifi connected announcement to %s: %s", target, connected_text)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    log.warning("wifi connected announcement failed (%s): %s", target, exc)
            was_all_connected = True
        else:
            if was_all_connected and target_sendable:
                try:
                    await peers[target].send_text(disconnected_text, add_newline=True)
                    log.info("sent wifi disconnected announcement to %s: %s", target, disconnected_text)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    log.warning("wifi disconnected announcement failed (%s): %s", target, exc)
            stable_since = None
            announced_for_current_connection = False
            was_all_connected = all_connected

        await asyncio.sleep(0.5)


async def _announce_rock_ready_and_disconnect(
    peers: Dict[str, WifiPeer],
    *,
    ready_text: str = "SAY:rock已就绪",
    disconnected_text: str = "SAY:wifi已断开",
    stable_s: float = 0.3,
    cooldown_s: float = 30.0,
) -> None:
    log = logging.getLogger("wifi_service")
    left_targets = [tid for tid in sorted(peers.keys()) if "IMU-L" in tid]
    target = left_targets[0] if left_targets else (sorted(peers.keys())[0] if peers else "")
    if not target:
        log.warning("skip rock ready announcement: no wifi target configured")
        return

    stable_since: Optional[float] = None
    ready_announced_for_current_connection = False
    last_announce_ts = 0.0
    was_target_sendable = False

    while True:
        now = time.monotonic()
        target_sendable = target in peers and peers[target].can_send

        if target_sendable:
            if stable_since is None:
                stable_since = now
            stable_long_enough = (now - stable_since) >= max(0.0, float(stable_s))
            cooldown_ok = (now - last_announce_ts) >= max(0.0, float(cooldown_s))
            if stable_long_enough and not ready_announced_for_current_connection and cooldown_ok:
                try:
                    await peers[target].send_text(ready_text, add_newline=True)
                    last_announce_ts = now
                    ready_announced_for_current_connection = True
                    log.info("sent rock ready announcement to %s: %s", target, ready_text)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    log.warning("rock ready announcement failed (%s): %s", target, exc)
            was_target_sendable = True
        else:
            if was_target_sendable and target in peers and peers[target].is_connected:
                try:
                    await peers[target].send_text(disconnected_text, add_newline=True)
                    log.info("sent wifi disconnected announcement to %s: %s", target, disconnected_text)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    log.warning("wifi disconnected announcement failed (%s): %s", target, exc)
            stable_since = None
            ready_announced_for_current_connection = False
            was_target_sendable = False

        await asyncio.sleep(0.5)


async def _run(cfg: AppConfig) -> None:
    log = logging.getLogger("wifi_service")
    peers: Dict[str, WifiPeer] = {}
    aligner = PairAligner()
    special_templates: Dict[str, Dict[str, Any]] = {}
    waiting_stop_events: Deque[Dict[str, Any]] = deque(maxlen=20)

    for t in cfg.targets:
        peer = WifiPeer(
            target_name=t.device_name,
            listen_host=t.listen_host,
            listen_port=t.listen_port,
            connect_timeout_s=t.accept_timeout_s,
            codec=cfg.codec,
        )

        async def _on_msg(text: str, tid: str = t.id) -> None:
            if text.startswith("MODEL:"):
                try:
                    hand, template = parse_special_template(text)
                    if hand is None:
                        return
                    special_templates[hand] = template
                    dorsal_ready = template.get("dorsal") is not None
                    log.info(
                        "special template updated: hand=%s source=%s channels=%d dorsal=%s",
                        hand,
                        tid,
                        len(template.get("finger", [])),
                        dorsal_ready,
                    )
                except Exception as exc:
                    log.warning("discard invalid special template from %s: %s text=%r", tid, exc, text)
                return

            if text.startswith("WAITING_STOP:"):
                try:
                    hand = parse_waiting_stop(text)
                    if hand is None:
                        return
                    event = {
                        "hand": hand,
                        "source": tid,
                        "local_timestamp": time.monotonic(),
                        "text": text,
                    }
                    waiting_stop_events.append(event)
                    log.info("waiting stop received: hand=%s source=%s", hand, tid)
                except Exception as exc:
                    log.warning("discard invalid waiting stop from %s: %s text=%r", tid, exc, text)
                return

            frame = parse_frame(text, time.monotonic())
            if frame is None:
                return
            expected = "left" if "IMU-L" in tid else "right" if "IMU-R" in tid else None
            if expected and frame.hand_type != expected:
                log.warning(
                    "hand_type mismatch: target=%s expected=%s got=%s",
                    tid,
                    expected,
                    frame.hand_type,
                )
            aligner.add_frame(frame)
            await asyncio.sleep(0)

        peer.set_notify_callback(_on_msg)
        peers[t.id] = peer
        await peer.start()
        log.info("[%s] listening on %s:%s", t.id, t.listen_host, t.listen_port)

    async def handle_ipc(req: Dict[str, Any]) -> Dict[str, Any]:
        cmd = str(req.get("cmd", "")).lower()

        if cmd in ("ping", "health"):
            return {
                "status": "ok",
                "targets": {
                    tid: {
                        "wifi_connected": peers[tid].is_connected,
                        "can_send": peers[tid].can_send,
                    }
                    for tid in sorted(peers.keys())
                },
            }

        if cmd == "send":
            text = req.get("text", "")
            if not isinstance(text, str):
                raise ValueError("'text' must be a string")

            add_newline = bool(req.get("add_newline", True))
            target: Optional[str] = None
            if "target" in req and req.get("target") is not None:
                target = str(req.get("target", "")).strip()

            if target:
                if target not in peers:
                    raise ValueError(f"unknown target id: {target}")
                await peers[target].send_text(text, add_newline=add_newline)
                return {"sent": len(text), "targets": [target]}

            sendable = [tid for tid in sorted(peers.keys()) if peers[tid].can_send]
            if not sendable:
                raise ValueError("no send-capable targets configured")

            await asyncio.gather(*[peers[tid].send_text(text, add_newline=add_newline) for tid in sendable])
            return {"sent": len(text), "targets": sendable}

        if cmd == "op_cmd":
            op = str(req.get("op", "")).strip().lower()
            target_raw = str(req.get("target", "both")).strip()
            target_key = target_raw.lower()

            op_map = {
                "start": "CMD:START",
                "stop": "CMD:STOP",
                "reset_seq": "CMD:RESET_SEQ",
                "reset-seq": "CMD:RESET_SEQ",
            }
            if op not in op_map:
                raise ValueError(f"unknown op: {op}. Allowed: start, stop, reset_seq")

            command_text = op_map[op]

            if target_key in ("left", "right"):
                expected = "IMU-L" if target_key == "left" else "IMU-R"
                resolved = [tid for tid in sorted(peers.keys()) if expected in tid]
                if not resolved:
                    raise ValueError(f"no target found for: {target_raw}")
            elif target_key == "both":
                resolved = sorted(peers.keys())
            elif target_raw in peers:
                resolved = [target_raw]
            else:
                raise ValueError(f"unknown target: {target_raw}")

            targets = [t for t in resolved if t in peers and peers[t].can_send]
            if not targets:
                raise ValueError("no send-capable targets resolved")

            await asyncio.gather(*[peers[tid].send_text(command_text, add_newline=True) for tid in targets])
            return {
                "status": "ok",
                "op": op,
                "command": command_text,
                "targets": targets,
            }

        if cmd == "fetch_pair":
            sample = aligner.fetch_pair()
            if sample is None:
                return {"status": "ok", "pair": None}
            return {
                "status": "ok",
                "pair": {
                    "frame_seq": sample.frame_seq,
                    "interpolated": sample.interpolated,
                    "left": {
                        "timestamp_ms": sample.left.timestamp_ms,
                        "frame_seq": sample.left.frame_seq,
                        "imu": sample.left.imu,
                    },
                    "right": {
                        "timestamp_ms": sample.right.timestamp_ms,
                        "frame_seq": sample.right.frame_seq,
                        "imu": sample.right.imu,
                    },
                },
            }

        if cmd in ("fetch_frame", "fetch_hand_frame"):
            hand = str(req.get("hand", "")).strip().lower()
            if hand not in ("left", "right"):
                raise ValueError("'hand' must be left or right")
            buffer = aligner.left if hand == "left" else aligner.right
            seq = buffer.oldest_seq()
            if seq is None:
                return {"status": "ok", "frame": None}
            frame = buffer.pop(seq)
            if frame is None:
                return {"status": "ok", "frame": None}
            return {
                "status": "ok",
                "frame": {
                    "timestamp_ms": frame.timestamp_ms,
                    "hand": frame.hand_type,
                    "frame_seq": frame.frame_seq,
                    "imu": frame.imu,
                },
            }

        if cmd == "get_special_templates":
            return {
                "status": "ok",
                "templates": special_templates,
                "ready": ("left" in special_templates and "right" in special_templates),
            }

        if cmd in ("fetch_waiting_stop", "fetch_stop_event"):
            if not waiting_stop_events:
                return {"status": "ok", "event": None}
            return {"status": "ok", "event": waiting_stop_events.popleft()}

        if cmd in ("clear_waiting_stop", "clear_stop_events"):
            waiting_stop_events.clear()
            return {"status": "ok", "cleared": True}

        if cmd in ("clear_special_templates", "reset_special_templates"):
            special_templates.clear()
            return {"status": "ok", "cleared": True}

        if cmd in ("clear", "clear_pairs", "reset_buffer"):
            aligner.clear()
            return {"status": "ok", "cleared": True}

        raise ValueError(f"unknown cmd: {cmd}")

    ipc_server = IpcServer(cfg.ipc, handle_ipc)
    await ipc_server.start()

    tasks = [
        asyncio.create_task(_peer_loop(peers[t.id], t.reconnect_interval_s, t.id))
        for t in cfg.targets
    ]
    tasks.append(asyncio.create_task(_announce_rock_ready_and_disconnect(peers)))

    try:
        await asyncio.gather(*tasks)
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for peer in peers.values():
            try:
                await peer.close()
            except Exception:
                pass


async def main_async() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    cfg_path = os.environ.get("WIFI_SERVICE_CONFIG", os.path.join(here, "service_config.json"))
    cfg = load_config(cfg_path)

    _setup_logging(cfg.log_level)
    log = logging.getLogger("wifi_service")
    log.info("config: %s", cfg_path)
    log.info("targets: %s", [t.id for t in cfg.targets])

    lock_path = default_lock_path()
    wait_for_file_disappear(lock_path, timeout_s=0.3)
    lock = InstanceLock(lock_path)
    lock.acquire()

    stop_evt = asyncio.Event()

    def _stop(*_args: Any) -> None:
        stop_evt.set()

    loop = asyncio.get_running_loop()
    for sig in (getattr(signal, "SIGTERM", None), getattr(signal, "SIGINT", None)):
        if sig is None:
            continue
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            pass

    task = asyncio.create_task(_run(cfg))
    await stop_evt.wait()

    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        lock.release()


def main() -> None:
    asyncio.run(main_async())


if __name__ == "__main__":
    main()
