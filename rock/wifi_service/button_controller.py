from __future__ import annotations

import argparse
import asyncio
import os
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

from ipc_client import IpcClient, IpcClientConfig


# Linux input_event (include/uapi/linux/input.h)
_INPUT_EVENT_STRUCT = struct.Struct("llHHI")  # timeval(sec,usec), type, code, value
_EV_KEY = 0x01
_KEY_DOWN = 1
_DEFAULT_BUTTON_KEY_CODES = "28"


@dataclass
class PairedSampleLite:
    left_ts_ms: int
    right_ts_ms: int
    left_imu: List[int]
    right_imu: List[int]


def _auto_pick_input_device() -> Optional[str]:
    base = Path("/dev/input")
    if not base.exists():
        return None
    candidates = sorted(base.glob("event*"))
    if not candidates:
        return None

    best: Tuple[int, Optional[Path]] = (-1, None)
    for p in candidates:
        score = 0
        name_path = Path("/sys/class/input") / p.name / "device" / "name"
        try:
            name = name_path.read_text(encoding="utf-8", errors="ignore").lower()
        except Exception:
            name = ""
        if "gpio" in name:
            score += 3
        if "key" in name or "keys" in name:
            score += 2
        if "power" in name:
            score += 1
        if score > best[0]:
            best = (score, p)

    return str(best[1]) if best[1] is not None else str(candidates[0])


class LinuxButton:
    def __init__(
        self,
        device: str,
        *,
        key_codes: Optional[Iterable[int]],
        debounce_ms: int = 200,
    ) -> None:
        self._device = device
        self._key_codes = None if key_codes is None else set(int(x) for x in key_codes)
        self._debounce_ms = int(debounce_ms)
        self._fd: Optional[int] = None
        self._last_down_ts = 0.0

    def open(self) -> None:
        self._fd = os.open(self._device, os.O_RDONLY | os.O_NONBLOCK)

    def close(self) -> None:
        if self._fd is not None:
            try:
                os.close(self._fd)
            except Exception:
                pass
            self._fd = None

    def poll_key_event(self) -> Optional[int]:
        if self._fd is None:
            raise RuntimeError("button not opened")

        try:
            data = os.read(self._fd, _INPUT_EVENT_STRUCT.size)
        except BlockingIOError:
            return False
        except OSError:
            return False

        if len(data) != _INPUT_EVENT_STRUCT.size:
            return False

        _sec, _usec, etype, code, value = _INPUT_EVENT_STRUCT.unpack(data)
        if etype != _EV_KEY:
            return None
        if self._key_codes is not None and int(code) not in self._key_codes:
            return None
        return int(value)

    def poll_press(self) -> bool:
        value = self.poll_key_event()
        if value is None:
            return False
        if int(value) != _KEY_DOWN:
            return False

        now = time.monotonic()
        if (now - self._last_down_ts) * 1000.0 < self._debounce_ms:
            return False
        self._last_down_ts = now
        return True


def _parse_int_csv(s: str) -> List[int]:
    out: List[int] = []
    for part in str(s).replace("，", ",").split(","):
        part = part.strip()
        if not part:
            continue
        out.append(int(part))
    return out


def _parse_key_codes(s: str) -> Optional[List[int]]:
    value = str(s).strip()
    if not value or value.lower() == "any":
        return None
    return _parse_int_csv(value)


async def _fetch_all_pairs(client: IpcClient, *, max_items: int = 50_000) -> List[PairedSampleLite]:
    items: List[PairedSampleLite] = []
    for _ in range(max_items):
        data = await client.request({"cmd": "fetch_pair"}, timeout=2.0)
        pair = data.get("pair")
        if pair is None:
            break
        left = pair["left"]
        right = pair["right"]
        items.append(
            PairedSampleLite(
                left_ts_ms=int(left["timestamp_ms"]),
                right_ts_ms=int(right["timestamp_ms"]),
                left_imu=[int(x) for x in left["imu"]],
                right_imu=[int(x) for x in right["imu"]],
            )
        )
    return items

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="进程B：按键/流程控制器（START/STOP + RKNN 推理 + 发左手）")

    p.add_argument(
        "--socket",
        default=os.environ.get("WIFI_SERVICE_SOCKET", "/run/wifi_service.sock"),
        help="unix socket path (Linux). Default: /run/wifi_service.sock",
    )

    p.add_argument(
        "--button_device",
        default=os.environ.get("WIFI_BUTTON_DEVICE", ""),
        help="/dev/input/eventX. Omit to auto-pick",
    )
    p.add_argument(
        "--button_key_codes",
        default=os.environ.get("WIFI_BUTTON_KEY_CODES", _DEFAULT_BUTTON_KEY_CODES),
        help="key codes comma-separated. Default 28 (KEY_ENTER). Use 'any' only for probing",
    )
    p.add_argument(
        "--debounce_ms",
        type=int,
        default=int(os.environ.get("WIFI_BUTTON_DEBOUNCE_MS", "200")),
        help="debounce window in ms",
    )

    p.add_argument(
        "--left_target",
        default=os.environ.get("WIFI_LEFT_TARGET", "ART-Pi2-IMU-L"),
        help="IPC send target id for left-hand ArtPi",
    )
    p.add_argument(
        "--send_newline",
        action="store_true",
        help="append newline when sending result text",
    )

    p.add_argument(
        "--rknn",
        default=os.environ.get("WIFI_RKNN", os.path.join("models", "signblstm_rk3588.rknn")),
        help="RKNN 模型路径",
    )
    p.add_argument(
        "--dataset_dir",
        default=os.environ.get(
            "WIFI_DATASET_DIR",
            os.path.join("data", "processed", "final_blstm_dataset"),
        ),
        help="包含 normalize/ meta/ 的目录",
    )
    p.add_argument(
        "--active_hand",
        default=os.environ.get("WIFI_ACTIVE_HAND", "both"),
        choices=["left", "right", "both"],
        help="对齐时的基准手",
    )

    p.add_argument(
        "--remove_words",
        default=os.environ.get("WIFI_REMOVE_WORDS", ""),
        help="剔除指定词（逗号分隔），例如：谢谢,高兴",
    )

    p.add_argument(
        "--win_len_ensemble",
        default=os.environ.get("WIFI_WIN_LEN_ENSEMBLE", "auto"),
        choices=["auto", "off"],
        help="多 win_len 候选：auto/off",
    )
    p.add_argument(
        "--win_len_candidates",
        default=os.environ.get("WIFI_WIN_LEN_CANDIDATES", "16,20"),
        help="win_len 候选列表(逗号分隔)，例如 16,20",
    )
    p.add_argument("--win_len", type=int, default=int(os.environ.get("WIFI_WIN_LEN", "20")))
    p.add_argument("--step", type=int, default=int(os.environ.get("WIFI_STEP", "1")))
    p.add_argument("--conf_low", type=float, default=float(os.environ.get("WIFI_CONF_LOW", "0.30")))
    p.add_argument("--energy_threshold", type=float, default=float(os.environ.get("WIFI_ENERGY_TH", "0.0")))
    p.add_argument(
        "--min_consecutive_windows",
        type=int,
        default=int(os.environ.get("WIFI_MIN_CONSEC", "2")),
    )
    p.add_argument(
        "--gap_merge_max",
        type=int,
        default=int(os.environ.get("WIFI_GAP_MERGE", "5")),
    )

    p.add_argument("--poll_sleep_ms", type=int, default=20)
    p.add_argument("--stop_drain_ms", type=int, default=400)

    return p


def _parse_str_list(s: str) -> List[str]:
    out: List[str] = []
    for part in str(s).replace("，", ",").split(","):
        part = (part or "").strip()
        if not part:
            continue
        out.append(part)
    # de-dup keep order
    seen = set()
    uniq: List[str] = []
    for w in out:
        if w in seen:
            continue
        seen.add(w)
        uniq.append(w)
    return uniq


async def _main_async(argv: Optional[List[str]] = None) -> int:
    if os.name == "nt":
        raise RuntimeError("button controller must run on Linux")

    args = _build_parser().parse_args(argv)

    dev = str(args.button_device).strip()
    if not dev:
        dev = _auto_pick_input_device() or ""
    if not dev:
        raise RuntimeError("cannot find /dev/input/eventX; please pass --button_device")

    key_codes = _parse_key_codes(args.button_key_codes)
    button = LinuxButton(dev, key_codes=key_codes, debounce_ms=args.debounce_ms)
    button.open()

    cfg = IpcClientConfig(unix_socket_path=args.socket)
    client = IpcClient(cfg)

    recording = False

    try:
        print(f"[controller] button device: {dev} key_codes={key_codes if key_codes is not None else 'ANY'}")
        print("[controller] ready. 1st=START, 2nd=STOP+INFER+SEND")

        while True:
            if button.poll_press():
                if not recording:
                    await client.request({"cmd": "clear_pairs"}, timeout=2.0)
                    await client.request({"cmd": "op_cmd", "op": "reset_seq", "target": "both"}, timeout=2.0)
                    await client.request({"cmd": "op_cmd", "op": "start", "target": "both"}, timeout=2.0)
                    recording = True
                    print("[controller] START")
                else:
                    await client.request({"cmd": "op_cmd", "op": "stop", "target": "both"}, timeout=2.0)
                    print("[controller] STOP, draining...")

                    samples: List[PairedSampleLite] = []
                    deadline = time.monotonic() + (max(0, int(args.stop_drain_ms)) / 1000.0)
                    while True:
                        samples.extend(await _fetch_all_pairs(client, max_items=1000))
                        if time.monotonic() >= deadline:
                            break
                        await asyncio.sleep(max(1, int(args.poll_sleep_ms)) / 1000.0)

                    from continuous_infer_runtime_rknn import infer_sentence_from_pairs

                    remove_words = _parse_str_list(args.remove_words)
                    win_len_candidates = tuple(_parse_int_csv(args.win_len_candidates))

                    left_imu = [s.left_imu for s in samples]
                    right_imu = [s.right_imu for s in samples]
                    out = infer_sentence_from_pairs(
                        left_imu,
                        right_imu,
                        rknn_path=args.rknn,
                        dataset_dir=args.dataset_dir,
                        active_hand=args.active_hand,
                        win_len_ensemble=args.win_len_ensemble,
                        win_len_candidates=win_len_candidates,
                        win_len=int(args.win_len),
                        step=int(args.step),
                        conf_low=float(args.conf_low),
                        energy_threshold=float(args.energy_threshold),
                        min_consecutive_windows=int(args.min_consecutive_windows),
                        gap_merge_max=int(args.gap_merge_max),
                        remove_words=remove_words,
                    )
                    sentence = str(out.get("sentence", "")).strip()
                    print(f"[controller] infer sentence: {sentence!r} pairs={len(samples)}")

                    if sentence:
                        await client.request(
                            {
                                "cmd": "send",
                                "text": sentence,
                                "target": args.left_target,
                                "add_newline": bool(args.send_newline),
                            },
                            timeout=3.0,
                        )
                        print(f"[controller] sent to left: {args.left_target}")
                    else:
                        print("[controller] empty result, skip send")

                    recording = False

            if recording:
                # Keep draining so the server queue doesn't overflow.
                _ = await _fetch_all_pairs(client, max_items=200)

            await asyncio.sleep(max(1, int(args.poll_sleep_ms)) / 1000.0)
    finally:
        try:
            await client.close()
        except Exception:
            pass
        button.close()

    return 0


def main() -> None:
    raise SystemExit(asyncio.run(_main_async()))


if __name__ == "__main__":
    main()
