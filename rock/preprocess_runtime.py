from typing import Optional, Tuple

import numpy as np


HAND_FEATURE_DIM = 69
PAIR_FEATURE_DIM = HAND_FEATURE_DIM * 2


def _check_frames(frames: np.ndarray, name: str = "frames") -> None:
    if frames.ndim != 2:
        raise ValueError(f"{name} must be a 2D array (T, F), got shape={frames.shape}")


def causal_moving_average(frames: np.ndarray, window_size: int = 5) -> np.ndarray:
    """Causal smoothing used online: only current and previous frames are visible."""
    _check_frames(frames)

    frame_count = frames.shape[0]
    if frame_count == 0 or window_size <= 1:
        return frames

    win = min(int(window_size), frame_count)
    cumsum = np.cumsum(frames, axis=0, dtype=np.float64)
    smooth_frames = np.empty_like(frames, dtype=np.float64)

    for row in range(frame_count):
        start = row - win + 1
        if start <= 0:
            win_sum = cumsum[row]
            real_win = row + 1
        else:
            win_sum = cumsum[row] - cumsum[start - 1]
            real_win = win
        smooth_frames[row] = win_sum / real_win

    return smooth_frames.astype(frames.dtype, copy=False)


def calc_frame_modlength(frames: np.ndarray, mode: str = "frame_diff") -> np.ndarray:
    """Motion strength for endpoint trimming."""
    _check_frames(frames)

    frame_count = frames.shape[0]
    if frame_count == 0:
        return np.zeros((0,), dtype=np.float32)

    frame_arr = frames.astype(np.float32, copy=False)
    if mode == "norm":
        return np.linalg.norm(frame_arr, axis=1).astype(np.float32, copy=False)

    if mode != "frame_diff":
        raise ValueError(f"unsupported mode={mode}, use frame_diff or norm")

    if frame_count < 2:
        return np.zeros((frame_count,), dtype=np.float32)

    diff_len = np.linalg.norm(np.diff(frame_arr, axis=0), axis=1)
    # Keep the same length as the raw sequence; this matches the training script.
    motion = np.concatenate([[diff_len[0]], diff_len, [diff_len[-1]]])[:frame_count]
    return motion.astype(np.float32, copy=False)


def _motion_threshold(motion: np.ndarray, stat_type: str) -> float:
    if stat_type == "median":
        return float(np.median(motion))
    return float(np.mean(motion))


def _scan_endpoint(
    motion: np.ndarray,
    high_threshold: float,
    low_threshold: float,
    frame_count: int,
) -> Tuple[int, int]:
    high_mask = motion >= high_threshold
    if not np.any(high_mask):
        return 0, frame_count - 1

    start_core = int(np.argmax(high_mask))
    start_idx = 0
    for pos in range(start_core, -1, -1):
        if motion[pos] < low_threshold:
            start_idx = pos + 1
            break

    end_core = int(len(high_mask) - 1 - np.argmax(high_mask[::-1]))
    end_idx = frame_count - 1
    for pos in range(end_core, frame_count):
        if motion[pos] < low_threshold:
            end_idx = pos - 1
            break

    return max(0, start_idx), min(frame_count - 1, end_idx)


def action_endpoint_detection(
    frames: np.ndarray,
    *,
    min_valid_frames: int = 40,
    modlength_calc_mode: str = "frame_diff",
    high_threshold_coef: float = 0.2,
    low_threshold_coef: float = 0.05,
    threshold_stat_type: str = "median",
) -> Tuple[np.ndarray, Tuple[int, int]]:
    """Cut the meaningful action part from a recorded sequence."""
    _check_frames(frames)

    frame_count = frames.shape[0]
    if frame_count == 0:
        return frames, (0, -1)
    if frame_count <= min_valid_frames:
        return frames, (0, frame_count - 1)

    motion = calc_frame_modlength(frames, mode=modlength_calc_mode)
    base_value = _motion_threshold(motion, threshold_stat_type)
    start_idx, end_idx = _scan_endpoint(
        motion,
        base_value * high_threshold_coef,
        base_value * low_threshold_coef,
        frame_count,
    )

    if (end_idx - start_idx + 1) < min_valid_frames:
        return frames, (0, frame_count - 1)
    return frames[start_idx : end_idx + 1], (start_idx, end_idx)


def _resize_by_pad_or_cut(frames: np.ndarray, target_seq_len: int) -> np.ndarray:
    if frames.shape[0] > target_seq_len:
        return frames[:target_seq_len]

    pad_len = target_seq_len - frames.shape[0]
    pad = np.repeat(frames[-1:], pad_len, axis=0)
    return np.concatenate([frames, pad], axis=0)


def _resize_by_interp(frames: np.ndarray, target_seq_len: int) -> np.ndarray:
    old_x = np.linspace(0.0, 1.0, num=frames.shape[0], dtype=np.float64)
    new_x = np.linspace(0.0, 1.0, num=target_seq_len, dtype=np.float64)
    frame_f64 = frames.astype(np.float64, copy=False)

    resized = np.empty((target_seq_len, frame_f64.shape[1]), dtype=np.float64)
    for col in range(frame_f64.shape[1]):
        resized[:, col] = np.interp(new_x, old_x, frame_f64[:, col])
    return resized.astype(frames.dtype, copy=False)


def adjust_seq_length(frames: np.ndarray, target_seq_len: int, mode: str = "interp") -> np.ndarray:
    """Resize sequence length before model inference."""
    _check_frames(frames)

    now_len = frames.shape[0]
    if now_len == 0:
        raise ValueError("frames cannot be empty")
    if now_len == target_seq_len:
        return frames

    if mode == "trunc_pad":
        return _resize_by_pad_or_cut(frames, target_seq_len)
    if mode == "interp":
        return _resize_by_interp(frames, target_seq_len)

    raise ValueError(f"unsupported mode={mode}, use interp or trunc_pad")


def preprocess_frames(
    frames: np.ndarray,
    *,
    seq_len: int = 40,
    enable_smooth: bool = True,
    smooth_window: int = 5,
    enable_endpoint_detect: bool = True,
    modlength_calc_mode: str = "frame_diff",
    high_threshold_coef: float = 0.2,
    low_threshold_coef: float = 0.05,
    threshold_stat_type: str = "median",
    min_valid_frames: int = 40,
    adjust_mode: str = "interp",
) -> np.ndarray:
    """Single-hand runtime preprocessing, kept for small offline tests too."""
    if not isinstance(frames, np.ndarray):
        frames = np.asarray(frames)
    _check_frames(frames)

    work_frames = frames.astype(np.float32, copy=False)
    if enable_smooth:
        work_frames = causal_moving_average(work_frames, window_size=smooth_window)

    if enable_endpoint_detect:
        work_frames, _ = action_endpoint_detection(
            work_frames,
            min_valid_frames=min_valid_frames,
            modlength_calc_mode=modlength_calc_mode,
            high_threshold_coef=high_threshold_coef,
            low_threshold_coef=low_threshold_coef,
            threshold_stat_type=threshold_stat_type,
        )

    return adjust_seq_length(work_frames, target_seq_len=seq_len, mode=adjust_mode)


def _interp_hand(
    hand_ts: Optional[np.ndarray],
    hand_feat: Optional[np.ndarray],
    target_ts: np.ndarray,
) -> np.ndarray:
    if hand_ts is None or hand_feat is None or len(hand_ts) == 0:
        return np.zeros((len(target_ts), HAND_FEATURE_DIM), dtype=np.float32)

    order = np.argsort(hand_ts)
    ts_sorted = hand_ts[order]
    feat_sorted = hand_feat[order]

    unique_ts, keep_idx = np.unique(ts_sorted, return_index=True)
    unique_feat = feat_sorted[keep_idx]
    if len(unique_ts) < 2:
        return np.tile(unique_feat, (len(target_ts), 1)).astype(np.float32)

    aligned_feat = np.zeros((len(target_ts), HAND_FEATURE_DIM), dtype=np.float64)
    for col in range(HAND_FEATURE_DIM):
        aligned_feat[:, col] = np.interp(
            target_ts,
            unique_ts,
            unique_feat[:, col].astype(np.float64),
        )
    return aligned_feat.astype(np.float32)


def _smooth_hand(feat: Optional[np.ndarray], smooth_window: int) -> Optional[np.ndarray]:
    if feat is not None and len(feat) > 0:
        return causal_moving_average(feat, window_size=smooth_window)
    return feat


def _zero_start_ts(ts: Optional[np.ndarray]) -> Optional[np.ndarray]:
    if ts is not None and len(ts) > 0:
        return ts - ts[0]
    return ts


def _pick_base_ts(
    left_ts: Optional[np.ndarray],
    right_ts: Optional[np.ndarray],
    active_hand: str,
) -> Optional[np.ndarray]:
    if active_hand == "left":
        return left_ts if left_ts is not None else right_ts
    if active_hand == "right":
        return right_ts if right_ts is not None else left_ts
    # both 模式训练时按右手时间轴对齐，这里也保持一致。
    return right_ts if right_ts is not None else left_ts


def _endpoint_range_for_pair(
    left_frames: np.ndarray,
    right_frames: np.ndarray,
    active_hand: str,
    *,
    min_valid_frames: int,
    modlength_calc_mode: str,
    high_threshold_coef: float,
    low_threshold_coef: float,
    threshold_stat_type: str,
) -> Tuple[int, int]:
    detect_kwargs = {
        "min_valid_frames": min_valid_frames,
        "modlength_calc_mode": modlength_calc_mode,
        "high_threshold_coef": high_threshold_coef,
        "low_threshold_coef": low_threshold_coef,
        "threshold_stat_type": threshold_stat_type,
    }

    if active_hand == "left":
        _, cut = action_endpoint_detection(left_frames, **detect_kwargs)
        return cut
    if active_hand == "right":
        _, cut = action_endpoint_detection(right_frames, **detect_kwargs)
        return cut

    _, left_cut = action_endpoint_detection(left_frames, **detect_kwargs)
    _, right_cut = action_endpoint_detection(right_frames, **detect_kwargs)
    return min(left_cut[0], right_cut[0]), max(left_cut[1], right_cut[1])


def align_and_preprocess(
    left_ts: Optional[np.ndarray],
    left_feat: Optional[np.ndarray],
    right_ts: Optional[np.ndarray],
    right_feat: Optional[np.ndarray],
    active_hand: str,
    *,
    mean: np.ndarray,
    std: np.ndarray,
    seq_len: int = 40,
    sampling_period_ms: float = 90.0,
    enable_smooth: bool = True,
    smooth_window: int = 5,
    enable_endpoint_detect: bool = True,
    modlength_calc_mode: str = "frame_diff",
    high_threshold_coef: float = 0.2,
    low_threshold_coef: float = 0.05,
    threshold_stat_type: str = "median",
    min_valid_frames: int = 40,
    adjust_mode: str = "interp",
) -> np.ndarray:
    """Align left/right IMU streams and return normalized model input."""
    if len(mean) != PAIR_FEATURE_DIM or len(std) != PAIR_FEATURE_DIM:
        raise ValueError(f"mean/std length must be {PAIR_FEATURE_DIM}, got {len(mean)} / {len(std)}")

    if enable_smooth:
        left_feat = _smooth_hand(left_feat, smooth_window)
        right_feat = _smooth_hand(right_feat, smooth_window)

    left_ts = _zero_start_ts(left_ts)
    right_ts = _zero_start_ts(right_ts)

    base_ts = _pick_base_ts(left_ts, right_ts, active_hand)
    if base_ts is None or len(base_ts) == 0:
        raise ValueError("no valid timestamp data, cannot build aligned timeline")

    dt = sampling_period_ms
    timeline = np.arange(base_ts[0], base_ts[-1] + dt, dt)
    left_aligned = _interp_hand(left_ts, left_feat, timeline)
    right_aligned = _interp_hand(right_ts, right_feat, timeline)

    if enable_endpoint_detect:
        start_idx, end_idx = _endpoint_range_for_pair(
            left_aligned,
            right_aligned,
            active_hand,
            min_valid_frames=min_valid_frames,
            modlength_calc_mode=modlength_calc_mode,
            high_threshold_coef=high_threshold_coef,
            low_threshold_coef=low_threshold_coef,
            threshold_stat_type=threshold_stat_type,
        )
    else:
        start_idx, end_idx = 0, len(timeline) - 1

    left_part = left_aligned[start_idx : end_idx + 1]
    right_part = right_aligned[start_idx : end_idx + 1]

    left_final = adjust_seq_length(left_part, seq_len, mode=adjust_mode)
    right_final = adjust_seq_length(right_part, seq_len, mode=adjust_mode)
    pair_frames = np.concatenate([left_final, right_final], axis=1).astype(np.float32)

    safe_std = np.maximum(std.astype(np.float32), 1e-8)
    return (pair_frames - mean.astype(np.float32)) / safe_std
