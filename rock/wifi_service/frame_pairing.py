from __future__ import annotations

import logging
from collections import deque
from dataclasses import dataclass
from typing import Deque, Optional, Tuple


UINT32_MOD = 2**32
FRAME_FIELD_COUNT = 72
IMU_FIELD_COUNT = 69


@dataclass
class SensorFrame:
    timestamp_ms: int
    hand_type: str
    frame_seq: int
    imu: list[int]
    local_timestamp: float
    interpolated: bool = False


@dataclass
class PairedSample:
    frame_seq: int
    left: SensorFrame
    right: SensorFrame
    interpolated: bool


def parse_frame(text: str, local_ts: float) -> Optional[SensorFrame]:
    log = logging.getLogger("wifi_frame_parser")
    if not text.startswith("[DATA]"):
        log.warning("discard frame without [DATA] prefix: %r", text)
        return None

    body = text[len("[DATA]") :]
    parts = body.split(",")
    if len(parts) != FRAME_FIELD_COUNT:
        log.warning("discard frame with invalid field count: got=%d text=%r", len(parts), text)
        return None

    try:
        timestamp_ms = int(parts[0])
        hand_type = parts[1]
        frame_seq = int(parts[2]) % UINT32_MOD
        if hand_type not in ("left", "right"):
            log.warning("discard frame with invalid hand_type: %r", hand_type)
            return None
        imu = [int(value) for value in parts[3:]]
    except ValueError as exc:
        log.warning("discard frame with parse error: %s text=%r", exc, text)
        return None

    if len(imu) != IMU_FIELD_COUNT:
        log.warning("discard frame with invalid imu count: got=%d", len(imu))
        return None

    return SensorFrame(
        timestamp_ms=timestamp_ms,
        hand_type=hand_type,
        frame_seq=frame_seq,
        imu=imu,
        local_timestamp=local_ts,
    )


def seq_distance(a: int, b: int) -> int:
    delta = (b - a) % UINT32_MOD
    if delta < 2**31:
        return delta
    return delta - UINT32_MOD


def interpolate_frame(prev: SensorFrame, next_frame: SensorFrame, seq: int) -> SensorFrame:
    total = seq_distance(prev.frame_seq, next_frame.frame_seq)
    if total <= 0:
        raise ValueError("invalid interpolation range")

    step = seq_distance(prev.frame_seq, seq)
    ratio = step / total
    imu = [
        int(round(left + (right - left) * ratio))
        for left, right in zip(prev.imu, next_frame.imu)
    ]
    timestamp_ms = int(round(prev.timestamp_ms + (next_frame.timestamp_ms - prev.timestamp_ms) * ratio))
    return SensorFrame(
        timestamp_ms=timestamp_ms,
        hand_type=prev.hand_type,
        frame_seq=seq % UINT32_MOD,
        imu=imu,
        local_timestamp=(prev.local_timestamp + next_frame.local_timestamp) / 2.0,
        interpolated=True,
    )


class FrameBuffer:
    MAX_SIZE = 128

    def __init__(self, hand: str) -> None:
        self.hand = hand
        self._frames: dict[int, SensorFrame] = {}
        self._seq_order: Deque[int] = deque()
        self._use_ts_fallback = False
        self._last_seq: Optional[int] = None
        self._log = logging.getLogger(f"frame_buffer.{hand}")

    def add(self, frame: SensorFrame) -> None:
        if frame.frame_seq in self._frames:
            self._log.warning("duplicate frame_seq on %s: %u", self.hand, frame.frame_seq)
            self._use_ts_fallback = True
            return

        if self._last_seq is not None and seq_distance(self._last_seq, frame.frame_seq) < -10:
            self._log.warning(
                "severely out-of-order frame on %s: last=%u got=%u",
                self.hand,
                self._last_seq,
                frame.frame_seq,
            )
            self._use_ts_fallback = True

        self._frames[frame.frame_seq] = frame
        self._seq_order.append(frame.frame_seq)
        while len(self._frames) > self.MAX_SIZE:
            oldest = self.oldest_seq()
            if oldest is None:
                break
            self.pop(oldest)
        self._last_seq = frame.frame_seq

    def get(self, seq: int) -> Optional[SensorFrame]:
        return self._frames.get(seq)

    def pop(self, seq: int) -> Optional[SensorFrame]:
        frame = self._frames.pop(seq, None)
        while self._seq_order and self._seq_order[0] not in self._frames:
            self._seq_order.popleft()
        while self._seq_order and self._seq_order[-1] not in self._frames:
            self._seq_order.pop()
        return frame

    def oldest_seq(self) -> Optional[int]:
        while self._seq_order and self._seq_order[0] not in self._frames:
            self._seq_order.popleft()
        return self._seq_order[0] if self._seq_order else None

    def newest_seq(self) -> Optional[int]:
        while self._seq_order and self._seq_order[-1] not in self._frames:
            self._seq_order.pop()
        return self._seq_order[-1] if self._seq_order else None

    def clear(self) -> None:
        self._frames.clear()
        self._seq_order.clear()
        self._use_ts_fallback = False
        self._last_seq = None


class PairAligner:
    INTERP_MAX_GAP = 3
    TS_FALLBACK_MAX_SEQ_OFFSET = 8
    TS_FALLBACK_MAX_DELTA_S = 1.0

    def __init__(self) -> None:
        self.left = FrameBuffer("left")
        self.right = FrameBuffer("right")
        self._pairs: Deque[PairedSample] = deque(maxlen=256)
        self._log = logging.getLogger("pair_aligner")

    def add_frame(self, frame: SensorFrame) -> None:
        if frame.hand_type == "left":
            self.left.add(frame)
        else:
            self.right.add(frame)
        self._try_align()

    def _buffer_target(self, left_seq: int, right_seq: int) -> int:
        return left_seq if seq_distance(left_seq, right_seq) >= 0 else right_seq

    def _nearest_by_timestamp(
        self,
        source: FrameBuffer,
        other: FrameBuffer,
        max_delta_s: Optional[float] = None,
    ) -> Optional[PairedSample]:
        src_seq = source.oldest_seq()
        if src_seq is None:
            return None
        src_frame = source.get(src_seq)
        if src_frame is None:
            return None

        candidates = list(other._frames.values())
        if not candidates:
            return None

        match = min(candidates, key=lambda frame: abs(frame.local_timestamp - src_frame.local_timestamp))
        delta_s = abs(match.local_timestamp - src_frame.local_timestamp)
        if max_delta_s is not None and delta_s > max_delta_s:
            return None

        source.pop(src_frame.frame_seq)
        other.pop(match.frame_seq)
        left = src_frame if src_frame.hand_type == "left" else match
        right = match if src_frame.hand_type == "left" else src_frame
        return PairedSample(
            frame_seq=left.frame_seq if src_frame.hand_type == "left" else right.frame_seq,
            left=left,
            right=right,
            interpolated=left.interpolated or right.interpolated,
        )

    def _pair_by_timestamp_fallback(self, max_delta_s: float) -> bool:
        pair = self._nearest_by_timestamp(self.right, self.left, max_delta_s)
        if pair is None:
            pair = self._nearest_by_timestamp(self.left, self.right, max_delta_s)
        if pair is None:
            return False

        self._log.warning(
            "paired by timestamp fallback: left_seq=%u right_seq=%u delta_ms=%d",
            pair.left.frame_seq,
            pair.right.frame_seq,
            int(abs(pair.left.local_timestamp - pair.right.local_timestamp) * 1000),
        )
        self._pairs.append(pair)
        return True

    def _drop_stale_oldest_by_timestamp(self, max_delta_s: float) -> bool:
        left_seq = self.left.oldest_seq()
        right_seq = self.right.oldest_seq()
        if left_seq is None or right_seq is None:
            return False

        left = self.left.get(left_seq)
        right = self.right.get(right_seq)
        if left is None or right is None:
            return False

        delta_s = left.local_timestamp - right.local_timestamp
        if abs(delta_s) <= max_delta_s:
            return False

        dropped = self.left.pop(left_seq) if delta_s < 0 else self.right.pop(right_seq)
        if dropped is None:
            return False

        self._log.warning(
            "drop stale %s frame for timestamp pairing: seq=%u delta_ms=%d",
            dropped.hand_type,
            dropped.frame_seq,
            int(abs(delta_s) * 1000),
        )
        return True

    def _find_interp_neighbors(
        self,
        buffer: FrameBuffer,
        target_seq: int,
    ) -> Optional[Tuple[SensorFrame, SensorFrame, int]]:
        prev_frame: Optional[SensorFrame] = None
        next_frame: Optional[SensorFrame] = None
        prev_dist: Optional[int] = None
        next_dist: Optional[int] = None

        for seq, frame in buffer._frames.items():
            to_target = seq_distance(seq, target_seq)
            if to_target > 0 and (prev_dist is None or to_target < prev_dist):
                prev_frame = frame
                prev_dist = to_target
            elif to_target < 0:
                forward = -to_target
                if next_dist is None or forward < next_dist:
                    next_frame = frame
                    next_dist = forward

        if prev_frame is None or next_frame is None:
            return None

        total_gap = seq_distance(prev_frame.frame_seq, next_frame.frame_seq) - 1
        if total_gap < 1:
            return None
        return prev_frame, next_frame, total_gap

    def _pair_from_seq(self, target_seq: int) -> bool:
        left_frame = self.left.get(target_seq)
        right_frame = self.right.get(target_seq)

        if left_frame is not None and right_frame is not None:
            left_frame = self.left.pop(target_seq)
            right_frame = self.right.pop(target_seq)
            if left_frame is None or right_frame is None:
                return False
            self._pairs.append(
                PairedSample(
                    frame_seq=target_seq,
                    left=left_frame,
                    right=right_frame,
                    interpolated=left_frame.interpolated or right_frame.interpolated,
                )
            )
            return True

        missing = self.left if left_frame is None else self.right
        present = self.right if missing is self.left else self.left
        present_frame = present.get(target_seq)
        if present_frame is None:
            return False

        neighbors = self._find_interp_neighbors(missing, target_seq)
        if neighbors is None:
            return False

        prev_frame, next_frame, gap = neighbors
        if gap > self.INTERP_MAX_GAP:
            self._log.warning(
                "skip interpolation on %s: target=%u gap=%d exceeds max=%d",
                missing.hand,
                target_seq,
                gap,
                self.INTERP_MAX_GAP,
            )
            return False

        synthesized = interpolate_frame(prev_frame, next_frame, target_seq)
        present_frame = present.pop(target_seq)
        if present_frame is None:
            return False

        if missing is self.left:
            left_frame = synthesized
            right_frame = present_frame
        else:
            left_frame = present_frame
            right_frame = synthesized

        self._pairs.append(
            PairedSample(
                frame_seq=target_seq,
                left=left_frame,
                right=right_frame,
                interpolated=True,
            )
        )
        return True

    def _try_align(self) -> None:
        while True:
            if self.left._use_ts_fallback or self.right._use_ts_fallback:
                if self._pair_by_timestamp_fallback(self.TS_FALLBACK_MAX_DELTA_S):
                    continue
                if self._drop_stale_oldest_by_timestamp(self.TS_FALLBACK_MAX_DELTA_S):
                    continue
                pair = self._nearest_by_timestamp(self.right, self.left)
                if pair is None:
                    pair = self._nearest_by_timestamp(self.left, self.right)
                if pair is None:
                    return
                self._pairs.append(pair)
                continue

            left_seq = self.left.oldest_seq()
            right_seq = self.right.oldest_seq()
            if left_seq is None or right_seq is None:
                return

            target_seq = self._buffer_target(left_seq, right_seq)
            if not self._pair_from_seq(target_seq):
                seq_gap = abs(seq_distance(left_seq, right_seq))
                if self._pair_by_timestamp_fallback(self.TS_FALLBACK_MAX_DELTA_S):
                    continue
                if seq_gap > self.TS_FALLBACK_MAX_SEQ_OFFSET:
                    if self._drop_stale_oldest_by_timestamp(self.TS_FALLBACK_MAX_DELTA_S):
                        continue
                return

    def fetch_pair(self) -> Optional[PairedSample]:
        if not self._pairs:
            return None
        return self._pairs.popleft()

    def pending_count(self) -> int:
        return len(self._pairs)

    def clear(self) -> None:
        self.left.clear()
        self.right.clear()
        self._pairs.clear()
