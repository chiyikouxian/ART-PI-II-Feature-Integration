from __future__ import annotations

import argparse
import asyncio
import json
import os
import socket
import sys
import time
from typing import Any, Dict, List, Optional


def _load_wifi_modules() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    svc_dir = os.path.join(here, "wifi_service")
    if svc_dir not in sys.path:
        sys.path.insert(0, svc_dir)


_load_wifi_modules()

from button_controller import (  # type: ignore  # noqa: E402
    LinuxButton,
    PairedSampleLite,
    _auto_pick_input_device,
    _fetch_all_pairs,
    _parse_int_csv,
    _parse_key_codes,
    _parse_str_list,
)
from ipc_client import IpcClient, IpcClientConfig  # type: ignore  # noqa: E402


DEFAULT_ASSEMBLE_API_KEY = "sk-c186c954dc4f4f4fa11c72988f391c7e"


class RttUdpClient:
    def __init__(self, host: str, port: int, timeout_s: float) -> None:
        self._host = host
        self._port = int(port)
        self._timeout_s = float(timeout_s)

    async def request(self, obj: Dict[str, Any]) -> Dict[str, Any]:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, self._request_sync, obj)

    def _request_sync(self, obj: Dict[str, Any]) -> Dict[str, Any]:
        payload = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(self._timeout_s)
            sock.sendto(payload, (self._host, self._port))
            data, _addr = sock.recvfrom(2048)
        text = data.decode("utf-8", errors="replace").strip()
        out = json.loads(text)
        if not isinstance(out, dict):
            raise RuntimeError(f"bad RTT response: {text!r}")
        if out.get("ok") is False:
            raise RuntimeError(f"RTT rejected {obj!r}: {out}")
        return out


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Rock Linux orchestrator: pwr button -> RTT state machine -> "
            "WiFi ArtPi collection -> RKNN inference -> send sentence to left ArtPi"
        )
    )

    p.add_argument("--rtt_host", default=os.environ.get("RTT_HOST", "10.10.10.30"))
    p.add_argument("--rtt_port", type=int, default=int(os.environ.get("RTT_PORT", "9100")))
    p.add_argument("--rtt_timeout_s", type=float, default=float(os.environ.get("RTT_TIMEOUT_S", "5")))

    p.add_argument(
        "--socket",
        default=os.environ.get("WIFI_SERVICE_SOCKET", "/run/wifi_service.sock"),
        help="wireless service unix socket path",
    )
    p.add_argument(
        "--button_device",
        default=os.environ.get("WIFI_BUTTON_DEVICE", ""),
        help="/dev/input/eventX. Omit to auto-pick",
    )
    p.add_argument(
        "--button_key_codes",
        default=os.environ.get("WIFI_BUTTON_KEY_CODES", "116"),
        help="key codes comma-separated. Default 116 (KEY_POWER). Use 'any' only for probing",
    )
    p.add_argument("--debounce_ms", type=int, default=int(os.environ.get("WIFI_BUTTON_DEBOUNCE_MS", "200")))

    p.add_argument("--left_target", default=os.environ.get("WIFI_LEFT_TARGET", "ART-Pi2-IMU-L"))
    p.add_argument(
        "--status_target",
        default=os.environ.get("WIFI_STATUS_TARGET", ""),
        help="target for SAY status prompts. Empty means broadcast to all send-capable ArtPi targets.",
    )
    p.add_argument(
        "--send_newline",
        action="store_true",
        help="append newline when sending final sentence to left ArtPi",
    )
    p.add_argument("--say_start", default=os.environ.get("WIFI_SAY_START", "SAY:模型开始采集"))
    p.add_argument("--say_stop", default=os.environ.get("WIFI_SAY_STOP", "SAY:模型结束采集"))

    p.add_argument("--rknn", default=os.environ.get("WIFI_RKNN", os.path.join("models", "signblstm_rk3588.rknn")))
    p.add_argument(
        "--dataset_dir",
        default=os.environ.get("WIFI_DATASET_DIR", os.path.join("data", "processed", "final_blstm_dataset")),
    )
    p.add_argument("--active_hand", default=os.environ.get("WIFI_ACTIVE_HAND", "right"), choices=["left", "right", "both"])
    p.add_argument("--remove_words", default=os.environ.get("WIFI_REMOVE_WORDS", ""))
    p.add_argument(
        "--win_len_ensemble",
        default=os.environ.get("WIFI_WIN_LEN_ENSEMBLE", "auto"),
        choices=["auto", "off"],
    )
    p.add_argument("--win_len_candidates", default=os.environ.get("WIFI_WIN_LEN_CANDIDATES", "16,20"))
    p.add_argument("--win_len", type=int, default=int(os.environ.get("WIFI_WIN_LEN", "20")))
    p.add_argument("--step", type=int, default=int(os.environ.get("WIFI_STEP", "1")))
    p.add_argument("--conf_low", type=float, default=float(os.environ.get("WIFI_CONF_LOW", "0.30")))
    p.add_argument("--energy_threshold", type=float, default=float(os.environ.get("WIFI_ENERGY_TH", "0.0")))
    p.add_argument("--min_consecutive_windows", type=int, default=int(os.environ.get("WIFI_MIN_CONSEC", "2")))
    p.add_argument("--gap_merge_max", type=int, default=int(os.environ.get("WIFI_GAP_MERGE", "5")))
    p.add_argument("--endpoint", default=os.environ.get("WIFI_ENDPOINT", "auto"), choices=["auto", "off"])
    p.add_argument("--tail_drop_frames", type=int, default=int(os.environ.get("WIFI_TAIL_DROP_FRAMES", "8")))

    p.add_argument("--poll_sleep_ms", type=int, default=20)
    p.add_argument(
        "--trigger_source",
        default=os.environ.get("WIFI_TRIGGER_SOURCE", "linux_button"),
        choices=["linux_button", "rtt_udp", "auto_special", "hybrid"],
    )
    p.add_argument("--rtt_event_host", default=os.environ.get("WIFI_RTT_EVENT_HOST", "10.10.10.31"))
    p.add_argument("--rtt_event_port", type=int, default=int(os.environ.get("WIFI_RTT_EVENT_PORT", "9103")))
    p.add_argument("--hybrid_linux_button", type=int, default=int(os.environ.get("WIFI_HYBRID_LINUX_BUTTON", "0")))
    p.add_argument("--mode_switch_taps", type=int, default=int(os.environ.get("WIFI_MODE_SWITCH_TAPS", "3")))
    p.add_argument("--mode_switch_window_ms", type=int, default=int(os.environ.get("WIFI_MODE_SWITCH_WINDOW_MS", "1500")))
    p.add_argument("--manual_trigger_delay_ms", type=int, default=int(os.environ.get("WIFI_MANUAL_TRIGGER_DELAY_MS", "500")))
    p.add_argument("--right_target", default=os.environ.get("WIFI_RIGHT_TARGET", "ART-Pi2-IMU-R"))
    p.add_argument("--special_window", type=int, default=int(os.environ.get("WIFI_SPECIAL_WINDOW", "25")))
    p.add_argument("--special_min_pairs", type=int, default=int(os.environ.get("WIFI_SPECIAL_MIN_PAIRS", "20")))
    p.add_argument("--special_cooldown_ms", type=int, default=int(os.environ.get("WIFI_SPECIAL_COOLDOWN_MS", "1800")))
    p.add_argument("--special_pass_ratio", type=float, default=float(os.environ.get("WIFI_SPECIAL_PASS_RATIO", "0.85")))
    p.add_argument("--special_shrink_ratio", type=float, default=float(os.environ.get("WIFI_SPECIAL_SHRINK_RATIO", "0.12")))
    p.add_argument("--special_dorsal_var_max", type=float, default=float(os.environ.get("WIFI_SPECIAL_DORSAL_VAR_MAX", "900000")))
    p.add_argument("--special_finger_var_max", type=float, default=float(os.environ.get("WIFI_SPECIAL_FINGER_VAR_MAX", "2500000")))
    p.add_argument("--special_release_ratio", type=float, default=float(os.environ.get("WIFI_SPECIAL_RELEASE_RATIO", "0.30")))
    p.add_argument(
        "--special_release_motion_min",
        type=float,
        default=float(os.environ.get("WIFI_SPECIAL_RELEASE_MOTION_MIN", "250")),
        help="minimum mean absolute paired-IMU delta observed after STOP to re-arm auto special trigger",
    )
    p.add_argument("--special_debug", type=int, default=int(os.environ.get("WIFI_SPECIAL_DEBUG", "1")))
    p.add_argument("--special_fresh_templates", type=int, default=int(os.environ.get("WIFI_SPECIAL_FRESH_TEMPLATES", "1")))
    p.add_argument("--special_template_timeout_s", type=float, default=float(os.environ.get("WIFI_SPECIAL_TEMPLATE_TIMEOUT_S", "30")))
    p.add_argument(
        "--special_start_single_hand",
        default=os.environ.get("WIFI_SPECIAL_START_SINGLE_HAND", "off"),
        choices=["off", "left", "right", "any"],
    )
    p.add_argument(
        "--auto_protocol",
        default=os.environ.get("WIFI_AUTO_PROTOCOL", "artpi"),
        choices=["artpi", "rock"],
        help="auto trigger protocol: artpi uses DATA/WAITING_STOP from ArtPi; rock uses Rock-side special matching",
    )
    p.add_argument("--stop_drain_ms", type=int, default=400)
    p.add_argument("--stop_drain_idle_ms", type=int, default=160)
    p.add_argument("--start_ready_delay_ms", type=int, default=int(os.environ.get("WIFI_START_READY_DELAY_MS", "1000")))
    p.add_argument("--start_pair_wait_s", type=float, default=float(os.environ.get("WIFI_START_PAIR_WAIT_S", "12")))
    p.add_argument("--start_pair_min_pairs", type=int, default=int(os.environ.get("WIFI_START_PAIR_MIN_PAIRS", "3")))
    p.add_argument("--artpi_cmd_gap_ms", type=int, default=int(os.environ.get("WIFI_ARTPI_CMD_GAP_MS", "300")))
    p.add_argument(
        "--artpi_auto_rearm_ms",
        type=int,
        default=int(os.environ.get("WIFI_ARTPI_AUTO_REARM_MS", "2000")),
        help="after ArtPi auto STOP, discard residual DATA until this delay and a quiet window pass",
    )
    p.add_argument(
        "--artpi_auto_quiet_ms",
        type=int,
        default=int(os.environ.get("WIFI_ARTPI_AUTO_QUIET_MS", "0")),
        help="optional no-pair quiet window before ArtPi auto START can re-arm after STOP; 0 disables it",
    )
    p.add_argument(
        "--artpi_auto_head_drop_pairs",
        type=int,
        default=int(os.environ.get("WIFI_ARTPI_AUTO_HEAD_DROP_PAIRS", "12")),
        help="drop this many paired samples at the beginning before ArtPi-auto inference",
    )
    p.add_argument(
        "--artpi_auto_tail_drop_pairs",
        type=int,
        default=int(os.environ.get("WIFI_ARTPI_AUTO_TAIL_DROP_PAIRS", "15")),
        help="drop this many paired samples at the end before ArtPi-auto inference",
    )
    p.add_argument(
        "--assemble",
        action="store_true",
        help="enable LLM sentence assembly; also auto-enabled when --assemble_api_key is set",
    )
    p.add_argument(
        "--no_assemble",
        action="store_true",
        help="disable LLM sentence assembly even when an API key is present",
    )
    p.add_argument("--assemble_api_key", default=os.environ.get("DEEPSEEK_API_KEY", DEFAULT_ASSEMBLE_API_KEY))
    p.add_argument("--assemble_base_url", default=os.environ.get("ASSEMBLE_BASE_URL", "https://api.deepseek.com"))
    p.add_argument("--assemble_model", default=os.environ.get("ASSEMBLE_MODEL", "deepseek-chat"))
    p.add_argument("--assemble_timeout", type=float, default=float(os.environ.get("ASSEMBLE_TIMEOUT", "2")))
    return p


class RttButtonEventProtocol(asyncio.DatagramProtocol):
    def __init__(self, queue: "asyncio.Queue[Dict[str, Any]]") -> None:
        self.queue = queue

    def datagram_received(self, data: bytes, addr: Any) -> None:
        text = data.decode("utf-8", errors="replace").strip()
        try:
            obj = json.loads(text)
            if not isinstance(obj, dict):
                return
            cmd = str(obj.get("cmd", "")).strip().lower()
            event = str(obj.get("event", "")).strip().lower()
            if cmd in ("toggle", "button", "press") or event in ("toggle", "button", "press"):
                obj["_addr"] = str(addr)
                try:
                    self.queue.put_nowait(obj)
                except asyncio.QueueFull:
                    pass
        except Exception as exc:
            print(f"[orchestrator] bad RTT GPIO event from {addr}: {text!r} err={exc!r}")


async def _open_rtt_button_event_server(host: str, port: int) -> tuple:
    loop = asyncio.get_running_loop()
    queue: asyncio.Queue[Dict[str, Any]] = asyncio.Queue(maxsize=32)
    transport, _protocol = await loop.create_datagram_endpoint(
        lambda: RttButtonEventProtocol(queue),
        local_addr=(host, int(port)),
    )
    return transport, queue


async def _next_rtt_button_event(queue: "asyncio.Queue[Dict[str, Any]]", timeout_s: float) -> Optional[Dict[str, Any]]:
    try:
        return await asyncio.wait_for(queue.get(), timeout=max(0.001, float(timeout_s)))
    except asyncio.TimeoutError:
        return None


def _open_linux_button_from_args(args: argparse.Namespace) -> tuple[LinuxButton, str]:
    dev = str(args.button_device).strip()
    if not dev:
        dev = _auto_pick_input_device() or ""
    if not dev:
        raise RuntimeError("cannot find /dev/input/eventX; pass --button_device")

    key_codes = _parse_key_codes(args.button_key_codes)
    button = LinuxButton(dev, key_codes=key_codes, debounce_ms=args.debounce_ms)
    button.open()
    desc = f"linux button device: {dev} key_codes={key_codes if key_codes is not None else 'ANY'}"
    return button, desc


RIGHT_DORSAL_ACCEL_WINDOWS = ((-3000, 0), (-9000, -7000), (1000, 3000))
LEFT_DORSAL_ACCEL_WINDOWS = ((-858, 1890), (-8548, -7158), (998, 3496))

RIGHT_FINGER_WINDOWS = (
    (-4410, -1986, -16324, -14570, -8746, -1668),
    (-3648, -1264, -16758, -15362, -4200, 1510),
    (-1000, 5000, -15500, 11002, -20000, -13000),
    (-2502, 1456, -19200, -6958, -20000, -12000),
    (-628, 3114, -5200, 4724, -15000, -4000),
    (-2672, 1256, -18000, -4960, -19000, -12000),
    (-1036, 2368, -8100, 15570, -17894, -7946),
    (-2308, 1602, -20100, -7170, -20000, -13000),
    (-2212, 3398, -11950, 10662, -18460, -11836),
    (-1812, 2998, -19500, -9430, -18178, -10498),
)

LEFT_FINGER_WINDOWS = (
    (4000, 8000, -16500, -14000, -2000, 3000),
    (2500, 6500, -16500, -13500, -2500, 5500),
    (-7000, -3000, -16000, -10000, -16500, -11000),
    (-2000, 2000, -11000, 0, -17000, -12000),
    (-5500, 0, -14000, -8000, -16000, -6000),
    (-3500, 2000, -14000, -4000, -19000, -13000),
    (-1500, 4500, -15000, -9000, -16000, -8000),
    (-1500, 4500, -12000, -4000, -19000, -14000),
    (3500, 9000, -18000, -12000, -19000, -12000),
    (0, 6000, -14000, -6000, -19000, -13000),
)


class SpecialActionChecker:
    def __init__(self, args: argparse.Namespace) -> None:
        self.window = max(1, int(args.special_window))
        self.min_pairs = max(1, int(args.special_min_pairs))
        self.cooldown_s = max(0.0, int(args.special_cooldown_ms) / 1000.0)
        self.pass_ratio = min(1.0, max(0.0, float(args.special_pass_ratio)))
        self.release_ratio = min(1.0, max(0.0, float(args.special_release_ratio)))
        self.shrink_ratio = min(0.45, max(0.0, float(args.special_shrink_ratio)))
        self.dorsal_var_max = float(args.special_dorsal_var_max)
        self.finger_var_max = float(args.special_finger_var_max)
        self.release_motion_min = max(0.0, float(args.special_release_motion_min))
        self.debug = bool(int(args.special_debug))
        self._pairs: List[PairedSampleLite] = []
        self._single: Dict[str, List[List[int]]] = {"left": [], "right": []}
        self._last_trigger_ts = 0.0
        self._last_debug_ts = 0.0
        self._templates: Dict[str, Dict[str, Any]] = {}
        self._waiting_logged = False
        self._require_release = False
        self._release_motion_seen = False
        self._release_motion_peak = 0.0
        self._release_last_pair_vec: Optional[List[float]] = None
        self._release_last_single_vec: Dict[str, Optional[List[float]]] = {"left": None, "right": None}

    @property
    def enabled(self) -> bool:
        return "left" in self._templates and "right" in self._templates

    def set_templates(self, templates: Dict[str, Any]) -> bool:
        changed = False
        for hand in ("left", "right"):
            raw = templates.get(hand)
            if isinstance(raw, list):
                raw = {"finger": raw, "dorsal": None}
            if not isinstance(raw, dict):
                continue
            raw_finger = raw.get("finger")
            if not isinstance(raw_finger, list) or len(raw_finger) != 10:
                continue

            parsed = []
            ok = True
            for window in raw_finger:
                if not isinstance(window, list) or len(window) != 6:
                    ok = False
                    break
                parsed.append(tuple(int(v) for v in window))
            if not ok:
                continue

            raw_dorsal = raw.get("dorsal")
            dorsal = None
            if raw_dorsal is not None:
                if not isinstance(raw_dorsal, list) or len(raw_dorsal) != 6:
                    continue
                dorsal = tuple(int(v) for v in raw_dorsal)

            value = {
                "finger": tuple(parsed),
                "dorsal": dorsal,
            }
            if self._templates.get(hand) != value:
                self._templates[hand] = value
                changed = True
        if changed:
            self._waiting_logged = False
            dorsal_hands = [hand for hand, tpl in self._templates.items() if tpl.get("dorsal") is not None]
            print(
                f"[orchestrator] special templates loaded: "
                f"hands={sorted(self._templates.keys())} dorsal={sorted(dorsal_hands)}"
            )
        return changed

    def reset(self) -> None:
        self._pairs.clear()
        for frames in self._single.values():
            frames.clear()

    def clear_templates(self) -> None:
        self._templates.clear()
        self._waiting_logged = False
        self.reset()

    def suppress(self, now: float) -> None:
        self._last_trigger_ts = now

    def require_release(self) -> None:
        self._require_release = True
        self._release_motion_seen = False
        self._release_motion_peak = 0.0
        self._release_last_pair_vec = None
        self._release_last_single_vec = {"left": None, "right": None}
        self.reset()

    def update(self, new_samples: List[PairedSampleLite], *, now: float) -> bool:
        if not self.enabled:
            if not self._waiting_logged:
                print("[orchestrator] waiting for MODEL:left and MODEL:right templates")
                self._waiting_logged = True
            return False

        if new_samples:
            self._pairs.extend(new_samples)
            if len(self._pairs) > self.window:
                self._pairs = self._pairs[-self.window :]
            if self._require_release:
                self._observe_release_pair_motion(new_samples)

        if now - self._last_trigger_ts < self.cooldown_s:
            self._debug(now, f"cooldown active pairs={len(self._pairs)}")
            return False
        if len(self._pairs) < self.min_pairs:
            self._debug(now, f"waiting candidate pairs={len(self._pairs)}/{self.min_pairs}")
            return False

        window = self._pairs[-self.window :]
        frame_results = [self._sample_passes(sample) for sample in window]
        pass_count = sum(1 for ok in frame_results if ok)
        pass_ratio = pass_count / max(1, len(frame_results))
        dorsal_var = self._max_axis_variance(window, finger=False)
        finger_var = self._max_axis_variance(window, finger=True)

        triggerable = (
            pass_ratio >= self.pass_ratio
            and dorsal_var <= self.dorsal_var_max
            and finger_var <= self.finger_var_max
        )

        if self._require_release:
            released = self._release_motion_seen or not triggerable or pass_ratio <= self.release_ratio
            if released:
                self._require_release = False
                self.reset()
                print(
                    "[orchestrator] auto special re-armed after release "
                    f"pass_ratio={pass_ratio:.2f} triggerable={triggerable} "
                    f"dorsal_var={dorsal_var:.1f} finger_var={finger_var:.1f} "
                    f"motion_peak={self._release_motion_peak:.1f}/{self.release_motion_min:.1f}"
                )
            else:
                self._debug(
                    now,
                    "waiting special release "
                    f"pairs={len(window)} pass_ratio={pass_ratio:.2f}/{self.pass_ratio:.2f} "
                    f"dorsal_var={dorsal_var:.1f}/{self.dorsal_var_max:.1f} "
                    f"finger_var={finger_var:.1f}/{self.finger_var_max:.1f} "
                    f"motion_peak={self._release_motion_peak:.1f}/{self.release_motion_min:.1f}",
                )
            return False

        if triggerable:
            self._last_trigger_ts = now
            print(
                "[orchestrator] auto special trigger PASS "
                f"pairs={len(window)} pass_ratio={pass_ratio:.2f} "
                f"dorsal_var={dorsal_var:.1f} finger_var={finger_var:.1f}"
            )
            return True
        self._debug(
            now,
            "auto special FAIL "
            f"pairs={len(window)} pass_ratio={pass_ratio:.2f}/{self.pass_ratio:.2f} "
            f"dorsal_var={dorsal_var:.1f}/{self.dorsal_var_max:.1f} "
            f"finger_var={finger_var:.1f}/{self.finger_var_max:.1f}",
        )
        return False

    def update_single(self, hand: str, new_imus: List[List[int]], *, now: float) -> bool:
        hand = str(hand).strip().lower()
        if hand not in ("left", "right"):
            return False
        if hand not in self._templates:
            return False

        if new_imus:
            self._single[hand].extend(new_imus)
            if len(self._single[hand]) > self.window:
                self._single[hand] = self._single[hand][-self.window :]
            if self._require_release:
                self._observe_release_single_motion(hand, new_imus)

        if now - self._last_trigger_ts < self.cooldown_s:
            self._debug(now, f"single-hand cooldown active hand={hand} frames={len(self._single[hand])}")
            return False
        if len(self._single[hand]) < self.min_pairs:
            self._debug(now, f"waiting single-hand candidate hand={hand} frames={len(self._single[hand])}/{self.min_pairs}")
            return False

        window = self._single[hand][-self.window :]
        frame_results = [self._hand_passes(imu, self._templates[hand]) for imu in window]
        pass_count = sum(1 for ok in frame_results if ok)
        pass_ratio = pass_count / max(1, len(frame_results))
        dorsal_var = self._max_imu_axis_variance(window, finger=False)
        finger_var = self._max_imu_axis_variance(window, finger=True)

        triggerable = (
            pass_ratio >= self.pass_ratio
            and dorsal_var <= self.dorsal_var_max
            and finger_var <= self.finger_var_max
        )

        if self._require_release:
            released = self._release_motion_seen or not triggerable or pass_ratio <= self.release_ratio
            if released:
                self._require_release = False
                self.reset()
                print(
                    "[orchestrator] auto special re-armed after single-hand release "
                    f"hand={hand} pass_ratio={pass_ratio:.2f} triggerable={triggerable} "
                    f"dorsal_var={dorsal_var:.1f} finger_var={finger_var:.1f} "
                    f"motion_peak={self._release_motion_peak:.1f}/{self.release_motion_min:.1f}"
                )
            else:
                self._debug(
                    now,
                    "waiting single-hand special release "
                    f"hand={hand} frames={len(window)} pass_ratio={pass_ratio:.2f}/{self.pass_ratio:.2f} "
                    f"dorsal_var={dorsal_var:.1f}/{self.dorsal_var_max:.1f} "
                    f"finger_var={finger_var:.1f}/{self.finger_var_max:.1f} "
                    f"motion_peak={self._release_motion_peak:.1f}/{self.release_motion_min:.1f}",
                )
            return False

        if triggerable:
            self._last_trigger_ts = now
            print(
                "[orchestrator] auto special single-hand PASS "
                f"hand={hand} frames={len(window)} pass_ratio={pass_ratio:.2f} "
                f"dorsal_var={dorsal_var:.1f} finger_var={finger_var:.1f}"
            )
            return True

        self._debug(
            now,
            "auto special single-hand FAIL "
            f"hand={hand} frames={len(window)} pass_ratio={pass_ratio:.2f}/{self.pass_ratio:.2f} "
            f"dorsal_var={dorsal_var:.1f}/{self.dorsal_var_max:.1f} "
            f"finger_var={finger_var:.1f}/{self.finger_var_max:.1f}",
        )
        return False

    def _debug(self, now: float, message: str) -> None:
        if not self.debug:
            return
        if now - self._last_debug_ts < 1.0:
            return
        self._last_debug_ts = now
        print(f"[orchestrator] {message}")

    def _sample_passes(self, sample: PairedSampleLite) -> bool:
        return (
            self._hand_passes(sample.left_imu, self._templates["left"])
            and self._hand_passes(sample.right_imu, self._templates["right"])
        )

    def _observe_release_pair_motion(self, samples: List[PairedSampleLite]) -> None:
        for sample in samples:
            vec = self._paired_motion_vec(sample)
            self._observe_release_motion(vec, pair=True)

    def _observe_release_single_motion(self, hand: str, imus: List[List[int]]) -> None:
        for imu in imus:
            vec = [float(v) for v in imu[:63]]
            self._observe_release_motion(vec, hand=hand)

    def _observe_release_motion(
        self,
        vec: List[float],
        *,
        pair: bool = False,
        hand: Optional[str] = None,
    ) -> None:
        if not vec:
            return
        if pair:
            prev = self._release_last_pair_vec
            self._release_last_pair_vec = vec
        else:
            if hand not in self._release_last_single_vec:
                return
            prev = self._release_last_single_vec.get(hand)
            self._release_last_single_vec[hand] = vec
        if prev is None:
            return
        n = min(len(prev), len(vec))
        if n <= 0:
            return
        delta = sum(abs(vec[i] - prev[i]) for i in range(n)) / n
        self._release_motion_peak = max(self._release_motion_peak, delta)
        if delta >= self.release_motion_min:
            self._release_motion_seen = True

    def _paired_motion_vec(self, sample: PairedSampleLite) -> List[float]:
        return [float(v) for v in sample.left_imu[:63]] + [float(v) for v in sample.right_imu[:63]]

    def _hand_passes(
        self,
        imu: List[int],
        template: Dict[str, Any],
    ) -> bool:
        if len(imu) < 60:
            return False
        finger_windows = template.get("finger")
        if not isinstance(finger_windows, tuple) or len(finger_windows) != 10:
            return False
        for ch, raw_window in enumerate(finger_windows):
            base = ch * 6
            if base + 5 >= len(imu):
                return False
            for axis in range(3):
                lo = float(raw_window[axis * 2])
                hi = float(raw_window[axis * 2 + 1])
                if not self._in_shrunk_window(float(imu[base + axis]), lo, hi):
                    return False

        dorsal_window = template.get("dorsal")
        if dorsal_window is not None:
            if len(imu) < 63:
                return False
            for axis in range(3):
                lo = float(dorsal_window[axis * 2])
                hi = float(dorsal_window[axis * 2 + 1])
                if not self._in_shrunk_window(float(imu[60 + axis]), lo, hi):
                    return False
        return True

    def _in_shrunk_window(self, value: float, lo: float, hi: float) -> bool:
        if hi < lo:
            lo, hi = hi, lo
        width = hi - lo
        shrink = width * self.shrink_ratio
        return (lo + shrink) <= value <= (hi - shrink)

    def _max_axis_variance(self, samples: List[PairedSampleLite], *, finger: bool) -> float:
        if not samples:
            return 0.0
        if finger:
            indices = [ch * 6 + axis for ch in range(10) for axis in range(3)]
        else:
            indices = [60, 61, 62]
        max_var = 0.0
        for idx in indices:
            for hand_getter in (lambda s: s.left_imu, lambda s: s.right_imu):
                values = [float(hand_getter(s)[idx]) for s in samples if idx < len(hand_getter(s))]
                if len(values) < 2:
                    continue
                mean = sum(values) / len(values)
                var = sum((v - mean) ** 2 for v in values) / len(values)
                max_var = max(max_var, var)
        return max_var

    def _max_imu_axis_variance(self, imus: List[List[int]], *, finger: bool) -> float:
        if not imus:
            return 0.0
        if finger:
            indices = [ch * 6 + axis for ch in range(10) for axis in range(3)]
        else:
            indices = [60, 61, 62]
        max_var = 0.0
        for idx in indices:
            values = [float(imu[idx]) for imu in imus if idx < len(imu)]
            if len(values) < 2:
                continue
            mean = sum(values) / len(values)
            var = sum((v - mean) ** 2 for v in values) / len(values)
            max_var = max(max_var, var)
        return max_var


async def _send_artpi_op(client: IpcClient, op: str, target: str = "both") -> Dict[str, Any]:
    # This deliberately preserves the existing ArtPi text protocol in service_main:
    # start -> CMD:START, stop -> CMD:STOP, reset_seq -> CMD:RESET_SEQ.
    return await client.request({"cmd": "op_cmd", "op": op, "target": target}, timeout=3.0)


async def _fetch_hand_imus(client: IpcClient, hand: str, *, max_items: int = 200) -> List[List[int]]:
    imus: List[List[int]] = []
    for _ in range(max_items):
        data = await client.request({"cmd": "fetch_frame", "hand": hand}, timeout=2.0)
        frame = data.get("frame")
        if frame is None:
            break
        raw_imu = frame.get("imu", [])
        if isinstance(raw_imu, list):
            imus.append([int(x) for x in raw_imu])
    return imus


async def _fetch_waiting_stop(client: IpcClient) -> Optional[Dict[str, Any]]:
    data = await client.request({"cmd": "fetch_waiting_stop"}, timeout=2.0)
    event = data.get("event")
    return event if isinstance(event, dict) else None


async def _wait_artpi_sendable(
    client: IpcClient,
    *,
    timeout_s: float = 10.0,
    poll_s: float = 0.2,
) -> bool:
    deadline = time.monotonic() + max(0.0, float(timeout_s))
    last_targets: Dict[str, Any] = {}
    while True:
        try:
            ping = await client.request({"cmd": "ping"}, timeout=3.0)
            last_targets = ping.get("targets", {})
            if last_targets and all(bool(v.get("can_send")) for v in last_targets.values()):
                return True
            if last_targets:
                not_ready = {
                    str(k): v
                    for k, v in last_targets.items()
                    if not bool(v.get("can_send"))
                }
                if not_ready:
                    last_targets = {"not_ready": not_ready}
        except Exception as exc:
            last_targets = {"error": repr(exc)}

        if time.monotonic() >= deadline:
            print(f"[orchestrator] ArtPi not sendable after wait: {last_targets!r}")
            return False
        await asyncio.sleep(max(0.05, float(poll_s)))


async def _send_sentence_with_retry(
    client: IpcClient,
    *,
    text: str,
    target: Optional[str],
    add_newline: bool,
    attempts: int = 3,
) -> Dict[str, Any]:
    last_exc: Optional[BaseException] = None
    for attempt in range(1, max(1, int(attempts)) + 1):
        try:
            ping = await client.request({"cmd": "ping"}, timeout=3.0)
            target_name = str(target or "").strip()
            if target_name:
                target_state = ping.get("targets", {}).get(target_name, {})
                print(f"[orchestrator] send precheck attempt={attempt} target={target_name} state={target_state}")
                req = {
                    "cmd": "send",
                    "text": text,
                    "target": target_name,
                    "add_newline": bool(add_newline),
                }
            else:
                targets_state = ping.get("targets", {})
                sendable = [k for k, v in targets_state.items() if bool(v.get("can_send"))]
                print(f"[orchestrator] send precheck attempt={attempt} target=ALL sendable={sendable}")
                req = {
                    "cmd": "send",
                    "text": text,
                    "add_newline": bool(add_newline),
                }
            resp = await client.request(req, timeout=3.0)
            print(f"[orchestrator] send response attempt={attempt}: {resp}")
            return resp
        except Exception as exc:
            last_exc = exc
            print(f"[orchestrator] send failed attempt={attempt}: {exc!r}")
            await asyncio.sleep(0.5)
    raise RuntimeError(f"send sentence failed after {attempts} attempts: {last_exc!r}")


async def _send_status_sentence(client: IpcClient, args: argparse.Namespace, text: str) -> None:
    if not str(text).strip():
        return
    try:
        target = str(getattr(args, "status_target", "") or "").strip() or None
        resp = await _send_sentence_with_retry(
            client,
            text=str(text),
            target=target,
            add_newline=True,
            attempts=2,
        )
        print(f"[orchestrator] sent status text={text!r} response={resp!r}")
    except Exception as exc:
        print(f"[orchestrator] status send failed text={text!r}: {exc!r}")


async def _send_hybrid_mode_prompt(client: IpcClient, args: argparse.Namespace, mode: str) -> None:
    if mode == "manual":
        mode_cmd = "MODE:MANUAL"
        left_text = "SAY:按键模式"
        right_text = "按键"
    else:
        mode_cmd = "MODE:AUTO"
        left_text = "SAY:自动化模式"
        right_text = "自动化"

    try:
        mode_resp = await _send_sentence_with_retry(
            client,
            text=mode_cmd,
            target=None,
            add_newline=True,
            attempts=2,
        )
        print(f"[orchestrator] sent mode command to ArtPi targets: text={mode_cmd!r} response={mode_resp!r}")
    except Exception as exc:
        print(f"[orchestrator] mode command failed text={mode_cmd!r}: {exc!r}")

    try:
        left_resp = await _send_sentence_with_retry(
            client,
            text=left_text,
            target=args.left_target,
            add_newline=True,
            attempts=2,
        )
        print(f"[orchestrator] sent mode prompt to left: text={left_text!r} response={left_resp!r}")
    except Exception as exc:
        print(f"[orchestrator] left mode prompt failed text={left_text!r}: {exc!r}")

    try:
        right_resp = await _send_sentence_with_retry(
            client,
            text=right_text,
            target=args.right_target,
            add_newline=True,
            attempts=2,
        )
        print(f"[orchestrator] sent mode prompt to right: text={right_text!r} response={right_resp!r}")
    except Exception as exc:
        print(f"[orchestrator] right mode prompt failed text={right_text!r}: {exc!r}")


async def _refresh_special_templates(client: IpcClient, checker: SpecialActionChecker) -> None:
    try:
        data = await client.request({"cmd": "get_special_templates"}, timeout=2.0)
    except Exception as exc:
        print(f"[orchestrator] get special templates failed: {exc!r}")
        return
    templates = data.get("templates", {})
    if isinstance(templates, dict):
        checker.set_templates(templates)


async def _clear_special_templates(client: IpcClient, checker: SpecialActionChecker) -> None:
    checker.clear_templates()
    try:
        resp = await client.request({"cmd": "clear_special_templates"}, timeout=2.0)
        print(f"[orchestrator] cleared cached special templates: {resp}")
    except Exception as exc:
        print(f"[orchestrator] clear special templates failed: {exc!r}")


async def _prepare_artpi_auto_templates(
    wifi_client: IpcClient,
    args: argparse.Namespace,
    checker: SpecialActionChecker,
) -> bool:
    if bool(int(args.special_fresh_templates)):
        await _clear_special_templates(wifi_client, checker)
    ready = await _wait_special_templates(
        wifi_client,
        checker,
        timeout_s=float(args.special_template_timeout_s),
    )
    if not ready:
        print("[orchestrator] auto special disabled: fresh templates not ready")
        return False
    await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
    await wifi_client.request({"cmd": "clear_waiting_stop"}, timeout=2.0)
    checker.reset()
    print("[orchestrator] cleared stale pairs after fresh special templates")
    return True


async def _begin_artpi_auto_template_wait(
    wifi_client: IpcClient,
    checker: SpecialActionChecker,
) -> None:
    await _clear_special_templates(wifi_client, checker)
    await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
    await wifi_client.request({"cmd": "clear_waiting_stop"}, timeout=2.0)
    print("[orchestrator] waiting for fresh ArtPi templates in auto mode")


async def _poll_artpi_auto_templates(
    wifi_client: IpcClient,
    checker: SpecialActionChecker,
) -> bool:
    await _refresh_special_templates(wifi_client, checker)
    if not checker.enabled:
        return False
    await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
    await wifi_client.request({"cmd": "clear_waiting_stop"}, timeout=2.0)
    checker.reset()
    print("[orchestrator] ArtPi auto templates ready; cleared stale pairs")
    return True


async def _wait_special_templates(
    client: IpcClient,
    checker: SpecialActionChecker,
    *,
    poll_s: float = 0.2,
    timeout_s: Optional[float] = None,
) -> bool:
    print("[orchestrator] waiting for special templates from ArtPi...")
    deadline = None if timeout_s is None else time.monotonic() + max(0.0, float(timeout_s))
    while not checker.enabled:
        await _refresh_special_templates(client, checker)
        if checker.enabled:
            return True
        if deadline is not None and time.monotonic() >= deadline:
            print("[orchestrator] special template wait timed out")
            return False
        await asyncio.sleep(max(0.05, float(poll_s)))
    return True


async def _wait_for_fresh_pairs(
    client: IpcClient,
    *,
    timeout_s: float,
    min_pairs: int,
    poll_s: float,
) -> List[PairedSampleLite]:
    deadline = time.monotonic() + max(0.0, float(timeout_s))
    pairs: List[PairedSampleLite] = []
    min_pairs = max(1, int(min_pairs))
    last_log = 0.0
    print(f"[orchestrator] waiting for paired left/right samples min={min_pairs} timeout={timeout_s:.1f}s")
    while time.monotonic() < deadline:
        new_pairs = await _fetch_all_pairs(client, max_items=200)
        if new_pairs:
            pairs.extend(new_pairs)
            if len(pairs) >= min_pairs:
                print(f"[orchestrator] paired stream ready pairs={len(pairs)}")
                return pairs
        now = time.monotonic()
        if now - last_log >= 1.0:
            last_log = now
            print(f"[orchestrator] waiting paired stream pairs={len(pairs)}/{min_pairs}")
        await asyncio.sleep(max(0.02, float(poll_s)))
    print(f"[orchestrator] paired stream wait timed out pairs={len(pairs)}/{min_pairs}")
    return pairs


async def _infer_result(args: argparse.Namespace, samples: List[PairedSampleLite]) -> Dict[str, Any]:
    from continuous_infer_runtime_rknn import infer_sentence_from_pairs

    remove_words = _parse_str_list(args.remove_words)
    win_len_candidates = tuple(_parse_int_csv(args.win_len_candidates))

    left_imu = [s.left_imu for s in samples]
    right_imu = [s.right_imu for s in samples]

    start = time.monotonic()
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
        endpoint=str(args.endpoint),
        remove_words=remove_words,
        enable_llm_assemble=(False if args.no_assemble else (bool(args.assemble) or bool(args.assemble_api_key))),
        assemble_api_key=str(args.assemble_api_key or ""),
        assemble_base_url=str(args.assemble_base_url),
        assemble_model=str(args.assemble_model),
        assemble_timeout=float(args.assemble_timeout) if args.assemble_timeout is not None else None,
        tail_drop_frames=int(args.tail_drop_frames),
    )
    out["infer_elapsed_ms"] = int((time.monotonic() - start) * 1000)
    return out


def _min_infer_pairs(args: argparse.Namespace) -> int:
    candidates = _parse_int_csv(args.win_len_candidates)
    if not candidates:
        candidates = [int(args.win_len)]
    valid = [int(v) for v in candidates if int(v) > 0]
    if not valid:
        valid = [max(2, int(args.win_len))]
    return max(2, min(valid))


def _trim_artpi_auto_samples(
    args: argparse.Namespace,
    samples: List[PairedSampleLite],
) -> List[PairedSampleLite]:
    total = len(samples)
    if total <= 0:
        return samples

    min_keep = _min_infer_pairs(args)
    head = max(0, int(args.artpi_auto_head_drop_pairs))
    tail = max(0, int(args.artpi_auto_tail_drop_pairs))
    requested_head = head
    requested_tail = tail

    if total - head - tail < min_keep:
        overflow = min(head + tail, min_keep - (total - head - tail))
        tail_reduce = min(tail, overflow)
        tail -= tail_reduce
        overflow -= tail_reduce
        if overflow > 0:
            head = max(0, head - overflow)

    end = total - tail if tail > 0 else total
    trimmed = samples[head:end]
    print(
        "[orchestrator] ArtPi auto trim for infer: "
        f"raw_pairs={total} used_pairs={len(trimmed)} "
        f"head_drop={head}/{requested_head} tail_drop={tail}/{requested_tail} "
        f"min_keep={min_keep}"
    )
    return trimmed


async def _main_async(argv: Optional[List[str]] = None) -> int:
    if os.name == "nt":
        raise RuntimeError("rtt_wifi_orchestrator must run on Rock Linux")

    args = _build_parser().parse_args(argv)

    button: Optional[LinuxButton] = None
    rtt_event_transport = None
    rtt_event_queue = None

    if args.trigger_source == "linux_button":
        button, trigger_desc = _open_linux_button_from_args(args)
    elif args.trigger_source in ("rtt_udp", "hybrid"):
        rtt_event_transport, rtt_event_queue = await _open_rtt_button_event_server(
            args.rtt_event_host,
            int(args.rtt_event_port),
        )
        trigger_desc = f"RTT GPIO UDP event: {args.rtt_event_host}:{args.rtt_event_port}"
        if args.trigger_source == "hybrid" and bool(int(args.hybrid_linux_button)):
            button, button_desc = _open_linux_button_from_args(args)
            trigger_desc = f"{trigger_desc}; {button_desc}"
    else:
        trigger_desc = "auto special action from WiFi IMU stream"

    wifi_client = IpcClient(IpcClientConfig(unix_socket_path=args.socket))
    rtt = RttUdpClient(args.rtt_host, args.rtt_port, args.rtt_timeout_s)
    recording = False
    samples: List[PairedSampleLite] = []
    auto_special_enabled = args.trigger_source in ("auto_special", "hybrid")
    artpi_auto_enabled = auto_special_enabled and str(args.auto_protocol).lower() == "artpi"
    rock_special_enabled = auto_special_enabled and str(args.auto_protocol).lower() == "rock"
    special_checker = SpecialActionChecker(args) if auto_special_enabled else None
    artpi_auto_need_quiet = False
    artpi_auto_rearm_after = 0.0
    artpi_auto_last_residual_pair_ts = 0.0
    artpi_auto_last_rearm_log_ts = 0.0
    hybrid_mode = "auto" if artpi_auto_enabled else "manual"
    artpi_auto_templates_ready = False
    artpi_auto_template_wait_started = False
    artpi_auto_template_last_log_ts = 0.0
    mode_switch_events: List[float] = []
    pending_manual_trigger_ts = 0.0

    try:
        print(f"[orchestrator] trigger source: {args.trigger_source}; {trigger_desc}")
        print(f"[orchestrator] RTT: {args.rtt_host}:{args.rtt_port}")
        if auto_special_enabled:
            print(f"[orchestrator] auto protocol: {args.auto_protocol}")
        if args.trigger_source == "hybrid":
            print(
                "[orchestrator] hybrid runtime mode: "
                f"{hybrid_mode}; mode switch={int(args.mode_switch_taps)} events/"
                f"{int(args.mode_switch_window_ms)}ms"
            )
        print("[orchestrator] ready. 1st trigger=START, 2nd trigger=STOP+INFER+SEND")

        await wifi_client.request({"cmd": "ping"}, timeout=3.0)
        if artpi_auto_enabled:
            print("[orchestrator] RTT control skipped for ArtPi auto protocol")
        else:
            rtt_status = await rtt.request({"cmd": "status"})
            print(f"[orchestrator] RTT status: {rtt_status}")
        if special_checker is not None and not artpi_auto_enabled:
            if bool(int(args.special_fresh_templates)):
                await _clear_special_templates(wifi_client, special_checker)
            ready = await _wait_special_templates(
                wifi_client,
                special_checker,
                timeout_s=float(args.special_template_timeout_s),
            )
            if not ready:
                print("[orchestrator] auto special disabled: fresh templates not ready")
                auto_special_enabled = False
                rock_special_enabled = False
            else:
                await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                special_checker.reset()
                print("[orchestrator] cleared stale pairs after fresh special templates")
        elif special_checker is not None and artpi_auto_enabled and hybrid_mode == "auto":
            await _begin_artpi_auto_template_wait(wifi_client, special_checker)
            artpi_auto_template_wait_started = True
            artpi_auto_templates_ready = False

        while True:
            triggered = False
            trigger_reason = ""
            waiting_stop_event: Optional[Dict[str, Any]] = None
            if args.trigger_source == "linux_button":
                triggered = bool(button and button.poll_press())
                if triggered:
                    trigger_reason = "linux_button"
            elif args.trigger_source in ("rtt_udp", "hybrid"):
                event = await _next_rtt_button_event(
                    rtt_event_queue,
                    max(0.001, int(args.poll_sleep_ms) / 1000.0),
                )
                if event is not None:
                    print(f"[orchestrator] RTT GPIO event: {event}")
                    if args.trigger_source == "hybrid":
                        if recording:
                            if hybrid_mode == "manual":
                                triggered = True
                                trigger_reason = "rtt_udp"
                                print("[orchestrator] hybrid RTT button used as manual STOP while recording")
                            else:
                                print("[orchestrator] hybrid RTT button ignored while auto recording")
                        else:
                            now = time.monotonic()
                            window_s = max(0.1, int(args.mode_switch_window_ms) / 1000.0)
                            mode_switch_events = [ts for ts in mode_switch_events if now - ts <= window_s]
                            mode_switch_events.append(now)
                            print(
                                f"[orchestrator] hybrid RTT button event "
                                f"switch_count={len(mode_switch_events)}/{int(args.mode_switch_taps)} "
                                f"mode={hybrid_mode}"
                            )
                            if len(mode_switch_events) >= max(1, int(args.mode_switch_taps)):
                                hybrid_mode = "manual" if hybrid_mode == "auto" else "auto"
                                mode_switch_events.clear()
                                pending_manual_trigger_ts = 0.0
                                print(f"[orchestrator] hybrid mode switched to {hybrid_mode}")
                                await _send_hybrid_mode_prompt(wifi_client, args, hybrid_mode)
                                if hybrid_mode == "manual":
                                    artpi_auto_templates_ready = False
                                    artpi_auto_template_wait_started = False
                                    artpi_auto_need_quiet = False
                                    artpi_auto_rearm_after = 0.0
                                    artpi_auto_last_residual_pair_ts = 0.0
                                    if special_checker is not None:
                                        special_checker.clear_templates()
                                    await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                                    await wifi_client.request({"cmd": "clear_waiting_stop"}, timeout=2.0)
                                    print("[orchestrator] manual mode active: auto templates cleared")
                                elif special_checker is not None:
                                    artpi_auto_need_quiet = False
                                    artpi_auto_rearm_after = 0.0
                                    artpi_auto_last_residual_pair_ts = 0.0
                                    await _begin_artpi_auto_template_wait(wifi_client, special_checker)
                                    artpi_auto_template_wait_started = True
                                    artpi_auto_templates_ready = False
                            else:
                                if hybrid_mode == "manual":
                                    delay_s = max(0.1, int(args.manual_trigger_delay_ms) / 1000.0)
                                    pending_manual_trigger_ts = now + delay_s
                                else:
                                    pending_manual_trigger_ts = 0.0
                    else:
                        triggered = True
                        trigger_reason = "rtt_udp"

            if args.trigger_source == "hybrid" and button is not None:
                key_value = button.poll_key_event()
                if key_value in (0, 1):
                    if recording:
                        if hybrid_mode == "manual":
                            triggered = True
                            trigger_reason = "linux_button"
                            print("[orchestrator] hybrid linux button used as manual STOP while recording")
                        else:
                            print("[orchestrator] hybrid linux button ignored while auto recording")
                    else:
                        now = time.monotonic()
                        window_s = max(0.1, int(args.mode_switch_window_ms) / 1000.0)
                        mode_switch_events = [ts for ts in mode_switch_events if now - ts <= window_s]
                        mode_switch_events.append(now)
                        print(
                            f"[orchestrator] hybrid button event value={key_value} "
                            f"switch_count={len(mode_switch_events)}/{int(args.mode_switch_taps)} "
                            f"mode={hybrid_mode}"
                        )
                        if len(mode_switch_events) >= max(1, int(args.mode_switch_taps)):
                            hybrid_mode = "manual" if hybrid_mode == "auto" else "auto"
                            mode_switch_events.clear()
                            pending_manual_trigger_ts = 0.0
                            print(f"[orchestrator] hybrid mode switched to {hybrid_mode}")
                            await _send_hybrid_mode_prompt(wifi_client, args, hybrid_mode)
                            if hybrid_mode == "manual":
                                artpi_auto_templates_ready = False
                                artpi_auto_template_wait_started = False
                                artpi_auto_need_quiet = False
                                artpi_auto_rearm_after = 0.0
                                artpi_auto_last_residual_pair_ts = 0.0
                                if special_checker is not None:
                                    special_checker.clear_templates()
                                await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                                await wifi_client.request({"cmd": "clear_waiting_stop"}, timeout=2.0)
                                print("[orchestrator] manual mode active: auto templates cleared")
                            elif special_checker is not None:
                                artpi_auto_need_quiet = False
                                artpi_auto_rearm_after = 0.0
                                artpi_auto_last_residual_pair_ts = 0.0
                                await _begin_artpi_auto_template_wait(wifi_client, special_checker)
                                artpi_auto_template_wait_started = True
                                artpi_auto_templates_ready = False
                        else:
                            if hybrid_mode == "manual":
                                delay_s = max(0.1, int(args.manual_trigger_delay_ms) / 1000.0)
                                pending_manual_trigger_ts = now + delay_s
                            else:
                                pending_manual_trigger_ts = 0.0

            if (
                args.trigger_source == "hybrid"
                and pending_manual_trigger_ts > 0.0
                and time.monotonic() >= pending_manual_trigger_ts
            ):
                if hybrid_mode == "manual":
                    triggered = True
                    trigger_reason = "linux_button"
                    print("[orchestrator] hybrid manual button trigger accepted")
                else:
                    print("[orchestrator] hybrid button trigger ignored in auto mode")
                pending_manual_trigger_ts = 0.0

            current_artpi_auto_enabled = artpi_auto_enabled and (
                args.trigger_source != "hybrid" or hybrid_mode == "auto"
            )
            if current_artpi_auto_enabled and not artpi_auto_templates_ready:
                if special_checker is not None:
                    if not artpi_auto_template_wait_started:
                        await _begin_artpi_auto_template_wait(wifi_client, special_checker)
                        artpi_auto_template_wait_started = True
                    artpi_auto_templates_ready = await _poll_artpi_auto_templates(wifi_client, special_checker)
                    if not artpi_auto_templates_ready:
                        now = time.monotonic()
                        if now - artpi_auto_template_last_log_ts >= 2.0:
                            artpi_auto_template_last_log_ts = now
                            print("[orchestrator] auto mode waiting for fresh ArtPi templates")
                current_artpi_auto_enabled = current_artpi_auto_enabled and artpi_auto_templates_ready
            current_manual_trigger = trigger_reason in ("linux_button", "rtt_udp")

            if current_artpi_auto_enabled:
                waiting_stop_event = await _fetch_waiting_stop(wifi_client)
                if waiting_stop_event is not None:
                    if recording:
                        print(f"[orchestrator] WAITING_STOP received: {waiting_stop_event}")
                        triggered = True
                        trigger_reason = "artpi_auto"
                    else:
                        print(f"[orchestrator] WAITING_STOP ignored while idle: {waiting_stop_event}")

            new_samples: List[PairedSampleLite] = []
            if recording or current_artpi_auto_enabled or rock_special_enabled:
                new_samples = await _fetch_all_pairs(wifi_client, max_items=200)
                if recording and new_samples:
                    samples.extend(new_samples)
                    print(f"[orchestrator] captured batch={len(new_samples)} total={len(samples)}")
                elif current_artpi_auto_enabled and new_samples:
                    now = time.monotonic()
                    if artpi_auto_need_quiet:
                        artpi_auto_last_residual_pair_ts = now
                        if now - artpi_auto_last_rearm_log_ts >= 1.0:
                            artpi_auto_last_rearm_log_ts = now
                            print(
                                f"[orchestrator] discarding residual ArtPi auto pairs={len(new_samples)} "
                                "while waiting quiet re-arm"
                            )
                    else:
                        print(f"[orchestrator] ArtPi auto START candidate pairs={len(new_samples)}")
                        triggered = True
                        trigger_reason = "artpi_auto"
                elif rock_special_enabled and new_samples:
                    print(f"[orchestrator] auto special candidate batch={len(new_samples)}")

            if current_artpi_auto_enabled and artpi_auto_need_quiet and not recording:
                now = time.monotonic()
                quiet_s = max(0, int(args.artpi_auto_quiet_ms)) / 1000.0
                delay_ready = now >= artpi_auto_rearm_after
                quiet_ready = (
                    quiet_s <= 0.0
                    or
                    artpi_auto_last_residual_pair_ts <= 0.0
                    or now - artpi_auto_last_residual_pair_ts >= quiet_s
                )
                if delay_ready and quiet_ready:
                    artpi_auto_need_quiet = False
                    artpi_auto_last_residual_pair_ts = 0.0
                    print("[orchestrator] ArtPi auto START re-armed after quiet window")
                elif now - artpi_auto_last_rearm_log_ts >= 1.0:
                    artpi_auto_last_rearm_log_ts = now
                    print(
                        "[orchestrator] waiting ArtPi auto quiet re-arm "
                        f"delay_ready={delay_ready} quiet_ready={quiet_ready}"
                    )

            if rock_special_enabled and special_checker is not None:
                if special_checker.update(new_samples, now=time.monotonic()):
                    triggered = True
                if not triggered and not recording and not new_samples:
                    single_mode = str(args.special_start_single_hand).strip().lower()
                    if single_mode != "off":
                        hands = ("left", "right") if single_mode == "any" else (single_mode,)
                        for hand in hands:
                            hand_imus = await _fetch_hand_imus(wifi_client, hand, max_items=200)
                            if hand_imus:
                                print(
                                    f"[orchestrator] auto special single-hand candidate "
                                    f"hand={hand} batch={len(hand_imus)}"
                                )
                            if special_checker.update_single(hand, hand_imus, now=time.monotonic()):
                                triggered = True
                                break

            if triggered:
                artpi_flow_trigger = trigger_reason == "artpi_auto"
                if not recording:
                    print(f"[orchestrator] START requested source={trigger_reason or 'unknown'}")
                    if not await _wait_artpi_sendable(wifi_client):
                        print("[orchestrator] START skipped: ArtPi TCP command channel not ready")
                        continue
                    samples = []
                    await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                    if not artpi_flow_trigger:
                        await _send_artpi_op(wifi_client, "reset_seq", "both")
                        await asyncio.sleep(max(0, int(args.artpi_cmd_gap_ms)) / 1000.0)
                    if artpi_flow_trigger:
                        print("[orchestrator] RTT start skipped for ArtPi auto protocol")
                    else:
                        rtt_resp = await rtt.request({"cmd": "start"})
                        print(f"[orchestrator] RTT start: {rtt_resp}")
                    await asyncio.sleep(max(0, int(args.artpi_cmd_gap_ms)) / 1000.0)
                    await _send_status_sentence(wifi_client, args, args.say_start)
                    await asyncio.sleep(max(0, int(args.artpi_cmd_gap_ms)) / 1000.0)
                    await _send_artpi_op(wifi_client, "start", "both")
                    print("[orchestrator] ArtPi start sent")
                    if artpi_flow_trigger:
                        samples = []
                        recording = True
                        print("[orchestrator] recording enabled from ArtPi START acknowledgement path")
                    else:
                        ready_delay_s = max(0, int(args.start_ready_delay_ms)) / 1000.0
                        if ready_delay_s > 0:
                            print(f"[orchestrator] start ready delay {ready_delay_s:.3f}s, dropping pre-action frames")
                            await asyncio.sleep(ready_delay_s)
                        await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                        warmup_pairs = await _wait_for_fresh_pairs(
                            wifi_client,
                            timeout_s=float(args.start_pair_wait_s),
                            min_pairs=int(args.start_pair_min_pairs),
                            poll_s=max(1, int(args.poll_sleep_ms)) / 1000.0,
                        )
                        await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                        samples = []
                        recording = True
                        print(f"[orchestrator] recording enabled warmup_pairs={len(warmup_pairs)}")
                    if special_checker is not None:
                        special_checker.reset()
                        special_checker.suppress(time.monotonic())
                else:
                    if waiting_stop_event is not None:
                        print("[orchestrator] STOP requested by ArtPi WAITING_STOP")
                        await _send_sentence_with_retry(
                            wifi_client,
                            text=args.say_stop,
                            target=args.left_target,
                            add_newline=True,
                        )
                        print(
                            f"[orchestrator] sent stop status to left target: "
                            f"{args.left_target} text={args.say_stop!r}"
                        )
                        if artpi_flow_trigger:
                            print("[orchestrator] RTT stop skipped for ArtPi auto protocol")
                        else:
                            rtt_resp = await rtt.request({"cmd": "stop"})
                            print(f"[orchestrator] RTT stop: {rtt_resp}")
                    else:
                        print(f"[orchestrator] STOP requested source={trigger_reason or 'unknown'}")
                        if artpi_flow_trigger:
                            print("[orchestrator] RTT stop skipped for ArtPi auto protocol")
                        else:
                            rtt_resp = await rtt.request({"cmd": "stop"})
                            print(f"[orchestrator] RTT stop: {rtt_resp}")
                        await _send_artpi_op(wifi_client, "stop", "both")
                        print("[orchestrator] ArtPi stop sent, draining...")
                        await _send_status_sentence(wifi_client, args, args.say_stop)

                        deadline = time.monotonic() + (max(0, int(args.stop_drain_ms)) / 1000.0)
                        idle_s = max(0.02, int(args.stop_drain_idle_ms) / 1000.0)
                        last_new_sample = time.monotonic()
                        while True:
                            drained = await _fetch_all_pairs(wifi_client, max_items=1000)
                            if drained:
                                samples.extend(drained)
                                last_new_sample = time.monotonic()
                                print(f"[orchestrator] drained batch={len(drained)} total={len(samples)}")

                            now = time.monotonic()
                            if now >= deadline:
                                break
                            if now - last_new_sample >= idle_s:
                                break
                            await asyncio.sleep(max(1, int(args.poll_sleep_ms)) / 1000.0)

                    infer_samples = (
                        _trim_artpi_auto_samples(args, samples)
                        if artpi_flow_trigger
                        else samples
                    )
                    infer_out = await _infer_result(args, infer_samples)
                    sentence = str(infer_out.get("sentence", "")).strip()
                    print(
                        f"[orchestrator] infer sentence: {sentence!r} "
                        f"pairs={len(infer_samples)} raw_pairs={len(samples)}"
                    )
                    print(f"[orchestrator] infer elapsed_ms: {infer_out.get('infer_elapsed_ms')!r}")
                    print(f"[orchestrator] llm assemble: {infer_out.get('llm_assemble', {})!r}")
                    print(f"[orchestrator] recognized words: {infer_out.get('recognized_words', [])!r}")
                    print(f"[orchestrator] segments: {infer_out.get('segments', [])!r}")
                    print(f"[orchestrator] candidates: {infer_out.get('candidates', [])!r}")

                    if sentence:
                        response_text = sentence if sentence.startswith("SAY:") else f"SAY:{sentence}"
                        if artpi_flow_trigger:
                            result_resp = await _send_status_sentence(wifi_client, args, response_text)
                            print(
                                f"[orchestrator] sent SAY result to ArtPi targets: "
                                f"text={response_text!r} response={result_resp!r}"
                            )
                            await asyncio.sleep(max(0, int(args.artpi_cmd_gap_ms)) / 1000.0)
                            stop_resp = await _send_artpi_op(wifi_client, "stop", "both")
                            print(f"[orchestrator] ArtPi stop sent after result: {stop_resp}")
                        else:
                            await _send_sentence_with_retry(
                                wifi_client,
                                text=response_text,
                                target=args.left_target,
                                add_newline=True,
                            )
                            print(
                                f"[orchestrator] sent SAY sentence to left target: "
                                f"{args.left_target} text={response_text!r}"
                            )
                    else:
                        if artpi_flow_trigger:
                            stop_resp = await _send_artpi_op(wifi_client, "stop", "both")
                            print(f"[orchestrator] ArtPi stop sent after infer: {stop_resp}")
                        print("[orchestrator] empty sentence, skip send")

                    recording = False
                    if artpi_flow_trigger:
                        await wifi_client.request({"cmd": "clear_waiting_stop"}, timeout=2.0)
                        await wifi_client.request({"cmd": "clear_pairs"}, timeout=2.0)
                        artpi_auto_need_quiet = True
                        artpi_auto_rearm_after = time.monotonic() + (
                            max(0, int(args.artpi_auto_rearm_ms)) / 1000.0
                        )
                        artpi_auto_last_residual_pair_ts = 0.0
                        artpi_auto_last_rearm_log_ts = 0.0
                        print("[orchestrator] cleared residual ArtPi auto events and pairs after STOP")
                    if rock_special_enabled and special_checker is not None:
                        special_checker.reset()
                        special_checker.require_release()
                        special_checker.suppress(time.monotonic())

            await asyncio.sleep(max(1, int(args.poll_sleep_ms)) / 1000.0)
    finally:
        try:
            await wifi_client.close()
        except Exception:
            pass
        if button is not None:
            button.close()
        if rtt_event_transport is not None:
            rtt_event_transport.close()

    return 0


def main() -> None:
    raise SystemExit(asyncio.run(_main_async()))


if __name__ == "__main__":
    main()
