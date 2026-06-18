from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import numpy as np

from preprocess_runtime import adjust_seq_length, causal_moving_average


DEFAULT_ASSEMBLE_API_KEY = "sk-c186c954dc4f4f4fa11c72988f391c7e"


@dataclass
class WindowPrediction:
    start_frame: int
    end_frame: int
    center_frame: int
    pred_id: int
    conf: float
    energy: float


def _softmax(x: np.ndarray) -> np.ndarray:
    x = x.astype(np.float64, copy=False)
    x = x - np.max(x)
    e = np.exp(x)
    s = np.sum(e)
    if s <= 0:
        return np.zeros_like(x, dtype=np.float64)
    return e / s


def _frame_motion_energy(seq: np.ndarray, active_hand: str) -> np.ndarray:
    if seq.shape[0] < 2:
        return np.zeros((0,), dtype=np.float64)
    if active_hand == "left":
        active = seq[:, :69]
    elif active_hand == "right":
        active = seq[:, 69:138]
    else:
        # both: use full
        active = seq
    diff = np.diff(active.astype(np.float64), axis=0)
    return np.linalg.norm(diff, axis=1)


def _window_motion_energy(window: np.ndarray, active_hand: str) -> float:
    if window.shape[0] < 2:
        return 0.0
    if active_hand == "left":
        active = window[:, :69]
    elif active_hand == "right":
        active = window[:, 69:138]
    else:
        active = window
    diff = np.diff(active.astype(np.float64), axis=0)
    return float(np.mean(np.linalg.norm(diff, axis=1)))


def _endpoint_trim_sequence(
    seq: np.ndarray,
    *,
    active_hand: str,
    win_len: int,
    mode: str = "auto",
    smooth_window: int = 5,
    alpha: float = 0.30,
) -> np.ndarray:
    mode = str(mode).lower()
    if mode == "off":
        return seq
    if mode != "auto":
        raise ValueError("endpoint mode must be auto/off")

    t = int(seq.shape[0])
    if t < max(2, int(win_len)):
        return seq

    energy = _frame_motion_energy(seq, active_hand)
    if energy.size == 0:
        return seq

    if smooth_window and smooth_window > 1:
        energy_sm = causal_moving_average(energy.reshape(-1, 1), window_size=int(smooth_window)).reshape(-1)
    else:
        energy_sm = energy

    p10 = float(np.percentile(energy_sm, 10))
    p90 = float(np.percentile(energy_sm, 90))
    threshold = p10 + float(alpha) * max(0.0, p90 - p10)
    active_mask = energy_sm >= threshold
    if not np.any(active_mask):
        return seq

    idx = np.flatnonzero(active_mask)
    start = int(max(0, idx[0] - 1))
    end = int(min(t - 1, idx[-1] + 1))

    pad = max(0, int(win_len) // 2)
    start = max(0, start - pad)
    end = min(t - 1, end + pad)

    trimmed = seq[start : end + 1]
    if trimmed.shape[0] < int(win_len):
        return seq
    return trimmed


def _segments_to_words(segments: List[dict], label2class: Dict[int, str]) -> List[str]:
    out: List[str] = []
    for seg in segments:
        out.append(label2class.get(int(seg["word"]), str(seg["word"])))
    return out


def _score_segments_simple(segments: List[dict]) -> float:
    if not segments:
        return -1e9
    conf_sum = float(sum(seg.get("avg_conf", 0.0) for seg in segments))
    n = len(segments)
    return conf_sum - 0.05 * max(0, n - 1)


def _strip_banned_from_text(text: str, banned_words: List[str]) -> str:
    if not text:
        return ""
    out = str(text)
    for w in banned_words:
        if not w:
            continue
        out = out.replace(w, "")
    return out.strip()


PROTECTED_SUBJECT_WORDS = {
    "我",
    "你",
    "他",
    "她",
    "我们",
    "你们",
    "他们",
    "她们",
}


WORD_ME = "\u6211"
WORD_YOU = "\u4f60"
WORD_HE = "\u4ed6"
WORD_SHE = "\u5979"
WORD_WE = "\u6211\u4eec"
WORD_YOU_PLURAL = "\u4f60\u4eec"
WORD_HE_PLURAL = "\u4ed6\u4eec"
WORD_SHE_PLURAL = "\u5979\u4eec"
WORD_GOOD = "\u597d"
WORD_THANKS = "\u8c22\u8c22"

PROTECTED_SUBJECT_WORDS = {
    WORD_ME,
    WORD_YOU,
    WORD_HE,
    WORD_SHE,
    WORD_WE,
    WORD_YOU_PLURAL,
    WORD_HE_PLURAL,
    WORD_SHE_PLURAL,
}


def _segment_word(seg: dict, label2class: Dict[int, str]) -> str:
    return label2class.get(int(seg["word"]), str(seg["word"]))


def _motion_adjust_word(word: str, avg_energy: float, low_motion_threshold: float) -> str:
    # "Good" and "thanks" are visually close. Here the useful discriminator
    # is temporal: "thanks" keeps moving, while "good" settles after posing.
    if word == WORD_THANKS and low_motion_threshold > 0 and float(avg_energy) <= float(low_motion_threshold):
        return WORD_GOOD
    return word


def _segments_to_motion_adjusted_words(
    segments: List[dict],
    label2class: Dict[int, str],
    *,
    low_motion_threshold: float,
) -> List[str]:
    out: List[str] = []
    for seg in segments:
        word = _segment_word(seg, label2class)
        out.append(_motion_adjust_word(word, float(seg.get("avg_energy", 0.0)), low_motion_threshold))
    return out


def _restore_missing_subject_prefix(sentence: str, recognized_words: List[str]) -> str:
    if not sentence or not recognized_words:
        return sentence
    first = (recognized_words[0] or "").strip()
    if first and first in PROTECTED_SUBJECT_WORDS and first not in sentence:
        return first + sentence
    return sentence


def _clean_llm_sentence(text: str) -> str:
    out = (text or "").strip()
    if not out:
        return ""

    if "**" in out:
        parts = out.split("**")
        bold_parts = [x.strip() for i, x in enumerate(parts) if i % 2 == 1 and x.strip()]
        if bold_parts:
            out = bold_parts[-1]

    lines = [x.strip() for x in out.splitlines() if x.strip()]
    if lines:
        out = lines[-1]

    for sep in ("最终句子是：", "最终句子:", "输出：", "输出:", "答案：", "答案:", "是：", "是:"):
        if sep in out:
            out = out.split(sep)[-1].strip()

    out = out.strip(" 「」“”'`*")
    for sep in (
        "\u6700\u7ec8\u53e5\u5b50\u662f\uff1a",
        "\u6700\u7ec8\u53e5\u5b50:",
        "\u8f93\u51fa\uff1a",
        "\u8f93\u51fa:",
        "\u7b54\u6848\uff1a",
        "\u7b54\u6848:",
        "\u662f\uff1a",
        "\u662f:",
    ):
        if sep in out:
            out = out.split(sep)[-1].strip()

    out = out.strip(" \u300c\u300d\u201c\u201d'`*")
    return out


def assemble_sentence_llm(
    words: List[str],
    *,
    api_key: Optional[str],
    base_url: str,
    model: str,
    system_prompt: str,
    timeout: Optional[float] = None,
) -> str:
    if not words:
        return ""
    if not api_key:
        raise RuntimeError("missing assemble API key")

    url = base_url.rstrip("/") + "/chat/completions"
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": " ".join(words)},
        ],
        "stream": False,
    }
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout or 10.0) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"LLM HTTP {exc.code}: {detail}") from exc

    out = json.loads(raw)
    text = out["choices"][0]["message"]["content"]
    return _clean_llm_sentence(text)


def assemble_sentence_llm_from_candidates(
    candidates: List[dict],
    *,
    api_key: str,
    base_url: str,
    model: str,
    timeout: Optional[float] = None,
    banned_words: Optional[List[str]] = None,
    system_prompt: Optional[str] = None,
) -> str:
    lines = [
        "下面是连续手语识别得到的多组候选词序列。",
        "请选择最合理的一组或综合多组候选，整理成一句自然中文。",
        "允许删除明显误识别的多余词、重复词、开头噪声词；不要编造候选词之外的新含义。",
        "例子：候选包含“很 你 好”和“你 好”时，最终输出“你好”。",
        "只输出最终一句中文，不要解释，不要 Markdown，不要引号。",
        "",
    ]
    for c in candidates:
        wl = c.get("win_len")
        words = c.get("words", [])
        lines.append(f"candidate(win_len={wl}): {' '.join(words)}")
        for seg in c.get("segments_prompt", c.get("segments", [])):
            lines.append(
                "  segment: "
                f"raw={seg.get('raw_word', seg.get('word'))} "
                f"word={seg.get('word')} "
                f"frames={seg.get('start_frame')}-{seg.get('end_frame')} "
                f"conf={float(seg.get('avg_conf', 0.0)):.3f} "
                f"motion={float(seg.get('avg_energy', 0.0)):.3f} "
                f"low_motion_th={float(c.get('low_motion_threshold', 0.0)):.3f}"
            )

    if system_prompt is None:
        system_prompt = (
            "你负责把连续手语识别出的候选词整理成一句自然中文。"
            "输入可能包含重复词、误识别词、开头或结尾噪声词。"
            "当某个候选明显更自然时，优先采用更自然的候选。"
            "例如“很 你 好”和“你 好”应输出“你好”。"
            "只输出最终一句中文，不要解释。"
        )
        system_prompt = (
            "You assemble continuous sign-language recognition candidates into one natural Chinese sentence. "
            "Candidates may contain duplicate words, noisy first/last words, or one wrong word. "
            "Prefer the shorter and more natural candidate when two candidates overlap. "
            "Example: if candidates include '\u5f88 \u4f60 \u597d' and '\u4f60 \u597d', output '\u4f60\u597d'. "
            "Important domain rule: '\u597d' and '\u8c22\u8c22' are visually similar, but '\u8c22\u8c22' keeps moving while '\u597d' becomes nearly static after the pose. "
            "If a segment raw word is '\u8c22\u8c22' but its corrected word is '\u597d' or its motion is at/below low_motion_th, treat it as '\u597d'. "
            "Output only the final Chinese sentence. Do not explain. Do not use Markdown or quotes."
        )
    if banned_words:
        bw = "、".join([w for w in banned_words if w])
        if bw:
            system_prompt = system_prompt.rstrip() + f"\nDo not include these words: {bw}."

    return assemble_sentence_llm(
        ["\n".join(lines)],
        api_key=api_key,
        base_url=base_url,
        model=model,
        system_prompt=system_prompt,
        timeout=timeout,
    )


class RknnClassifier:
    def __init__(
        self,
        *,
        rknn_path: str,
        core_mask: Optional[int] = None,
    ) -> None:
        self._rknn_path = rknn_path
        self._core_mask = core_mask
        self._rknn = None

    def open(self) -> None:
        from rknnlite.api import RKNNLite

        rknn = RKNNLite()
        ret = rknn.load_rknn(self._rknn_path)
        if ret != 0:
            raise RuntimeError(f"load_rknn failed: ret={ret}")

        if self._core_mask is not None:
            ret = rknn.init_runtime(core_mask=self._core_mask)
            if ret != 0:
                ret = rknn.init_runtime()
        else:
            ret = rknn.init_runtime()

        if ret != 0:
            raise RuntimeError(f"init_runtime failed: ret={ret}")

        self._rknn = rknn

    def close(self) -> None:
        if self._rknn is None:
            return
        try:
            self._rknn.release()
        except Exception:
            pass
        self._rknn = None

    def predict_logits(self, x_40_138_norm: np.ndarray) -> np.ndarray:
        if self._rknn is None:
            raise RuntimeError("RKNN not initialized")
        xb = np.expand_dims(x_40_138_norm.astype(np.float32, copy=False), axis=0)
        outputs = self._rknn.inference(inputs=[xb])
        logits = np.asarray(outputs[0]).reshape(-1)
        return logits


def load_label2class(dataset_dir: str) -> Dict[int, str]:
    path = os.path.join(dataset_dir, "meta", "label2class.json")
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    out: Dict[int, str] = {}
    for k, v in raw.items():
        try:
            out[int(k)] = str(v)
        except Exception:
            pass
    return out


def load_normalize(dataset_dir: str) -> Tuple[np.ndarray, np.ndarray]:
    mean = np.load(os.path.join(dataset_dir, "normalize", "normalize_mean.npy")).astype(np.float32, copy=False)
    std = np.load(os.path.join(dataset_dir, "normalize", "normalize_std.npy")).astype(np.float32, copy=False)
    std = np.maximum(std, 1e-8)
    if len(mean) != 138 or len(std) != 138:
        raise ValueError(f"mean/std length must be 138 but got {len(mean)}/{len(std)}")
    return mean, std


def infer_windows_on_sequence(
    seq_T138: np.ndarray,
    *,
    clf: RknnClassifier,
    mean: np.ndarray,
    std: np.ndarray,
    active_hand: str = "right",
    win_len: int = 20,
    step: int = 1,
    conf_low: float = 0.30,
    energy_threshold: float = 0.0,
    enable_smooth: bool = True,
    smooth_window: int = 5,
) -> List[WindowPrediction]:
    if seq_T138.ndim != 2 or seq_T138.shape[1] != 138:
        raise ValueError(f"seq must be (T,138) but got {seq_T138.shape}")

    T = int(seq_T138.shape[0])
    if T < max(2, int(win_len)):
        return []

    seq = seq_T138.astype(np.float32, copy=False)
    if enable_smooth and smooth_window > 1:
        seq = causal_moving_average(seq, window_size=int(smooth_window))

    energy = _frame_motion_energy(seq, active_hand)

    preds: List[WindowPrediction] = []
    for start in range(0, T - int(win_len) + 1, int(step)):
        window = seq[start : start + int(win_len)]
        center = start + int(win_len) // 2

        x40 = adjust_seq_length(window, target_seq_len=40, mode="interp")
        xnorm = (x40 - mean) / std
        e = _window_motion_energy(xnorm, active_hand)
        if e < float(energy_threshold):
            continue

        logits = clf.predict_logits(xnorm)
        probs = _softmax(logits)
        pred_id = int(np.argmax(probs))
        conf = float(probs[pred_id])
        if conf < float(conf_low):
            continue

        preds.append(
            WindowPrediction(
                start_frame=int(start),
                end_frame=int(start + int(win_len)),
                center_frame=int(center),
                pred_id=pred_id,
                conf=conf,
                energy=e,
            )
        )

    return preds


def cluster_windows(
    predictions: List[WindowPrediction],
    *,
    step: int = 1,
    min_consecutive_windows: int = 2,
    gap_merge_max: int = 5,
    possible_window_count: Optional[int] = None,
) -> List[dict]:
    if not predictions:
        return []

    if possible_window_count is not None and possible_window_count < min_consecutive_windows:
        effective_min = 1
    else:
        effective_min = int(min_consecutive_windows)

    predictions = sorted(predictions, key=lambda p: p.start_frame)
    clusters: List[List[WindowPrediction]] = []
    current = [predictions[0]]
    for pred in predictions[1:]:
        prev = current[-1]
        if pred.pred_id == prev.pred_id and (pred.start_frame - prev.start_frame) <= int(step) + 2:
            current.append(pred)
        else:
            clusters.append(current)
            current = [pred]
    clusters.append(current)

    word_segments: List[dict] = []
    for cluster in clusters:
        if len(cluster) >= effective_min:
            word_segments.append(
                {
                    "word": int(cluster[0].pred_id),
                    "start_frame": int(min(c.start_frame for c in cluster)),
                    "end_frame": int(max(c.end_frame for c in cluster)),
                    "avg_conf": float(np.mean([c.conf for c in cluster])),
                    "avg_energy": float(np.mean([c.energy for c in cluster])),
                }
            )

    if not word_segments:
        return []

    merged = [word_segments[0]]
    for seg in word_segments[1:]:
        last = merged[-1]
        if seg["word"] == last["word"] and (seg["start_frame"] - last["end_frame"]) <= int(gap_merge_max):
            last["end_frame"] = seg["end_frame"]
            last["avg_conf"] = max(float(last["avg_conf"]), float(seg["avg_conf"]))
        else:
            merged.append(seg)

    merged.sort(key=lambda x: x["start_frame"])
    resolved: List[dict] = []
    for seg in merged:
        if not resolved:
            resolved.append(seg)
            continue
        prev = resolved[-1]
        if seg["start_frame"] < prev["end_frame"]:
            if seg["avg_conf"] > prev["avg_conf"]:
                prev["end_frame"] = seg["start_frame"]
                if prev["end_frame"] <= prev["start_frame"]:
                    resolved.pop()
                resolved.append(seg)
            else:
                seg["start_frame"] = prev["end_frame"]
                if seg["start_frame"] < seg["end_frame"]:
                    resolved.append(seg)
        else:
            resolved.append(seg)

    return resolved


def _pick_win_lens(win_len_ensemble: str, win_len_candidates: Tuple[int, ...], win_len: int) -> List[int]:
    if win_len_ensemble != "auto":
        return [int(win_len)]

    win_lens = [int(size) for size in win_len_candidates if int(size) > 0]
    return win_lens or [int(win_len)]


def _filter_segments(segments: List[dict], label2class: Dict[int, str], banned_words: set) -> List[dict]:
    if not banned_words:
        return segments
    return [
        seg for seg in segments
        if label2class.get(int(seg["word"]), str(seg["word"])) not in banned_words
    ]


def _segment_prompt_rows(
    segments: List[dict],
    label2class: Dict[int, str],
    low_motion_threshold: float,
) -> List[dict]:
    rows: List[dict] = []
    for seg in segments:
        raw_word = _segment_word(seg, label2class)
        rows.append(
            {
                "raw_word": raw_word,
                "word": _motion_adjust_word(
                    raw_word,
                    float(seg.get("avg_energy", 0.0)),
                    low_motion_threshold,
                ),
                "avg_conf": float(seg.get("avg_conf", 0.0)),
                "avg_energy": float(seg.get("avg_energy", 0.0)),
                "start_frame": int(seg.get("start_frame", 0)),
                "end_frame": int(seg.get("end_frame", 0)),
            }
        )
    return rows


def _candidate_from_window(
    seq: np.ndarray,
    *,
    clf: RknnClassifier,
    mean: np.ndarray,
    std: np.ndarray,
    label2class: Dict[int, str],
    banned_words: set,
    active_hand: str,
    win_len: int,
    step: int,
    conf_low: float,
    energy_threshold: float,
    min_consecutive_windows: int,
    gap_merge_max: int,
) -> dict:
    preds = infer_windows_on_sequence(
        seq,
        clf=clf,
        mean=mean,
        std=std,
        active_hand=active_hand,
        win_len=win_len,
        step=step,
        conf_low=conf_low,
        energy_threshold=energy_threshold,
    )
    energies = [float(pred.energy) for pred in preds if np.isfinite(float(pred.energy))]
    low_motion_threshold = float(np.percentile(energies, 35)) if energies else 0.0
    possible_count = ((seq.shape[0] - int(win_len)) // int(step) + 1) if seq.shape[0] >= int(win_len) else 0
    segments = cluster_windows(
        preds,
        step=step,
        min_consecutive_windows=min_consecutive_windows,
        gap_merge_max=gap_merge_max,
        possible_window_count=possible_count,
    )
    kept_segments = _filter_segments(segments, label2class, banned_words)
    words = _segments_to_motion_adjusted_words(
        kept_segments,
        label2class,
        low_motion_threshold=low_motion_threshold,
    )
    return {
        "win_len": int(win_len),
        "segments": segments,
        "segments_filtered": kept_segments,
        "segments_prompt": _segment_prompt_rows(kept_segments, label2class, low_motion_threshold),
        "words": words,
        "score": _score_segments_simple(kept_segments),
        "low_motion_threshold": low_motion_threshold,
    }


def _candidate_response_rows(candidates: List[dict], label2class: Dict[int, str]) -> List[dict]:
    out: List[dict] = []
    for cand in candidates:
        low_motion = float(cand.get("low_motion_threshold", 0.0))
        out.append(
            {
                "win_len": cand["win_len"],
                "words": cand["words"],
                "score": cand["score"],
                "segments": _segment_prompt_rows(
                    cand.get("segments_filtered", []),
                    label2class,
                    low_motion,
                ),
                "low_motion_threshold": cand.get("low_motion_threshold", 0.0),
            }
        )
    return out


def infer_sentence_from_pairs(
    left_imu_list: List[List[int]],
    right_imu_list: List[List[int]],
    *,
    rknn_path: str,
    dataset_dir: str,
    active_hand: str = "right",
    win_len_ensemble: str = "auto",
    win_len_candidates: Tuple[int, ...] = (16, 20),
    win_len: int = 20,
    step: int = 1,
    conf_low: float = 0.30,
    energy_threshold: float = 0.0,
    min_consecutive_windows: int = 2,
    gap_merge_max: int = 5,
    endpoint: str = "auto",
    remove_words: Optional[List[str]] = None,
    tail_drop_frames: int = 0,
    enable_llm_assemble: Optional[bool] = None,
    assemble_api_key: str = DEFAULT_ASSEMBLE_API_KEY,
    assemble_base_url: str = "https://api.deepseek.com",
    assemble_model: str = "deepseek-chat",
    assemble_timeout: Optional[float] = None,
    assemble_system_prompt: str = "",
) -> dict:
    if len(left_imu_list) != len(right_imu_list):
        raise ValueError("left/right length mismatch")
    if not left_imu_list:
        return {"sentence": "", "recognized_words": [], "segments": [], "candidates": []}

    seq = np.asarray(
        [list(l) + list(r) for l, r in zip(left_imu_list, right_imu_list)],
        dtype=np.float32,
    )
    tail_drop = max(0, int(tail_drop_frames))
    if tail_drop > 0 and seq.shape[0] > tail_drop:
        # STOP 后偶尔会混进几帧收手动作，这里先按实测参数去掉。
        seq = seq[:-tail_drop]

    mean, std = load_normalize(dataset_dir)
    label2class = load_label2class(dataset_dir)

    if remove_words is None:
        remove_words = []
    banned_set = set(remove_words)
    win_lens = _pick_win_lens(win_len_ensemble, win_len_candidates, win_len)

    seq = _endpoint_trim_sequence(seq, active_hand=active_hand, win_len=max(win_lens), mode=endpoint)

    clf = RknnClassifier(rknn_path=rknn_path)
    clf.open()
    try:
        candidate_results: List[dict] = []
        for wl in win_lens:
            candidate_results.append(
                _candidate_from_window(
                    seq,
                    clf=clf,
                    mean=mean,
                    std=std,
                    label2class=label2class,
                    banned_words=banned_set,
                    active_hand=active_hand,
                    win_len=wl,
                    step=step,
                    conf_low=conf_low,
                    energy_threshold=energy_threshold,
                    min_consecutive_windows=min_consecutive_windows,
                    gap_merge_max=gap_merge_max,
                )
            )

        best = max(candidate_results, key=lambda x: x["score"]) if candidate_results else None
        segments_best = best["segments_filtered"] if best else []
        recognized_words = (
            _segments_to_motion_adjusted_words(
                segments_best,
                label2class,
                low_motion_threshold=float(best.get("low_motion_threshold", 0.0)),
            )
            if best
            else []
        )

        sentence = "".join(recognized_words)
        if enable_llm_assemble is None:
            enable_llm_assemble = bool(assemble_api_key)

        if enable_llm_assemble and recognized_words:
            system_prompt = assemble_system_prompt or (
                "你负责把连续手语识别出的候选词整理成一句自然中文。"
                "输入可能包含重复词、误识别词、开头或结尾噪声词。"
                "当某个候选明显更自然时，优先采用更自然的候选。"
                "例如“很 你 好”和“你 好”应输出“你好”。"
                "只输出最终一句中文，不要解释。"
            )
            if not assemble_system_prompt:
                system_prompt = (
                    "You assemble continuous sign-language recognition candidates into one natural Chinese sentence. "
                    "Candidates may contain duplicate words, noisy first/last words, or one wrong word. "
                    "Prefer the shorter and more natural candidate when two candidates overlap. "
                    "Example: if candidates include '\u5f88 \u4f60 \u597d' and '\u4f60 \u597d', output '\u4f60\u597d'. "
                    "Important domain rule: '\u597d' and '\u8c22\u8c22' are visually similar, but '\u8c22\u8c22' keeps moving while '\u597d' becomes nearly static after the pose. "
                    "If a segment raw word is '\u8c22\u8c22' but its corrected word is '\u597d' or its motion is at/below low_motion_th, treat it as '\u597d'. "
                    "Output only the final Chinese sentence. Do not explain. Do not use Markdown or quotes."
                )
            try:
                if win_len_ensemble == "auto" and len(candidate_results) > 1:
                    sentence = assemble_sentence_llm_from_candidates(
                        candidate_results,
                        api_key=assemble_api_key,
                        base_url=assemble_base_url,
                        model=assemble_model,
                        timeout=assemble_timeout,
                        banned_words=remove_words,
                        system_prompt=system_prompt,
                    )
                else:
                    sentence = assemble_sentence_llm(
                        recognized_words,
                        api_key=assemble_api_key,
                        base_url=assemble_base_url,
                        model=assemble_model,
                        system_prompt=system_prompt,
                        timeout=assemble_timeout,
                    )
            except Exception as exc:
                sentence = "".join(recognized_words)

        if remove_words:
            sentence = _strip_banned_from_text(sentence, remove_words)
        sentence = _restore_missing_subject_prefix(sentence, recognized_words)

        return {
            "sentence": sentence,
            "recognized_words": recognized_words,
            "segments": best.get("segments_prompt", []) if best else [],
            "candidates": _candidate_response_rows(candidate_results, label2class),
        }
    finally:
        clf.close()
