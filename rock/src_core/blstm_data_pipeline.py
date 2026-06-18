import os
# =========================================
# 本文件已实现：主动手基准对齐+双手并集对齐+无填充+特征拼接
# 1. 只做时序裁剪，无任何静息填充，所有输入均为真实采集数据
# 2. 主动手动作完整，非主动手严格跟随主动手动作区间
# 3. 支持双手并集对齐，特征拼接顺序保持一致
# 4. active_hand 仅用于预处理，不作为模型输入
# 5. 训练/推理输入完全一致，适配注意力机制
# =========================================
import numpy as np
import pandas as pd
from tqdm import tqdm
from scipy.interpolate import interp1d
from sklearn.model_selection import train_test_split
import json
import shutil

def read_csv_with_fallback(csv_path):
    encodings_to_try = ["utf-8", "utf-8-sig", "gbk", "gb2312", "cp936", "latin1"]
    na_values = ["nan", "NaN", "NAN", "null", "NULL"]
    last_error = None

    for enc in encodings_to_try:
        try:
            return pd.read_csv(csv_path, encoding=enc, na_values=na_values, keep_default_na=True)
        except UnicodeDecodeError as e:
            last_error = e
            continue

    raise last_error


def organize_final_dataset_dir(final_dataset_dir):
    subdirs = {
        "train": "train",
        "val": "val",
        "test": "test",
        "normalize": "normalize",
        "meta": "meta",
    }

    file_to_subdir = {
        "train_data.npy": subdirs["train"],
        "train_labels.npy": subdirs["train"],
        "val_data.npy": subdirs["val"],
        "val_labels.npy": subdirs["val"],
        "test_data.npy": subdirs["test"],
        "test_labels.npy": subdirs["test"],
        "normalize_mean.npy": subdirs["normalize"],
        "normalize_std.npy": subdirs["normalize"],
        "class2label.json": subdirs["meta"],
        "label2class.json": subdirs["meta"],
        "sample_process_info.json": subdirs["meta"],
    }

    for subdir in set(file_to_subdir.values()):
        os.makedirs(os.path.join(final_dataset_dir, subdir), exist_ok=True)

    for filename, subdir in file_to_subdir.items():
        src = os.path.join(final_dataset_dir, filename)
        if not os.path.exists(src):
            continue
        dst = os.path.join(final_dataset_dir, subdir, filename)

        if os.path.abspath(src) == os.path.abspath(dst):
            continue

        os.replace(src, dst)


def write_final_dataset_readme(final_dataset_dir):
    readme_path = os.path.join(final_dataset_dir, "readme.txt")

    lines = [
        "final_blstm_dataset 输出说明（自动生成）",
        "",
        "该目录是 BLSTM 训练/验证/测试的最终输入数据。",
        "数据统一格式：float32 数组，形状 (样本数, SEQ_LEN, 特征维度)。",
        "其中 SEQ_LEN 来自 src/blstm_data_pipeline.py 里的 SEQ_LEN 配置。",
        "",
        "目录结构（按用途分类）：",
        "- train/：训练集",
        "  - train_data.npy：训练数据",
        "  - train_labels.npy：训练标签（数字）",
        "- val/：验证集",
        "  - val_data.npy：验证数据",
        "  - val_labels.npy：验证标签（数字）",
        "- test/：测试集",
        "  - test_data.npy：测试数据",
        "  - test_labels.npy：测试标签（数字）",
        "- normalize/：归一化参数（z-score）",
        "  - normalize_mean.npy：训练集均值（形状=(特征维度,)）",
        "  - normalize_std.npy：训练集标准差（形状=(特征维度,)）",
        "- meta/：标签映射与处理过程信息",
        "  - class2label.json：类别字符串 -> 数字标签",
        "  - label2class.json：数字标签 -> 类别字符串",
        "  - sample_process_info.json：common 数据每个 sample_id 的处理信息（原始帧数、端点范围等）",
        "",
        "训练时如何读取：",
        "- 训练读取：train/train_data.npy + train/train_labels.npy",
        "- 调参/早停读取：val/val_data.npy + val/val_labels.npy",
        "- 最终评估读取：test/test_data.npy + test/test_labels.npy",
        "",
        "提示：",
        "- 训练/推理时如果你自己写的代码原来从根目录读取 train_data.npy，需要把路径改成 train/train_data.npy。",
    ]

    with open(readme_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

# ================================== 核心参数配置区（仅需修改这里） ==================================
# -------------------------- 路径配置（完全匹配你的项目结构） --------------------------
# 原始csv数据文件路径，所有数据都在这一个文件里
RAW_CSV_PATH = "data/raw/data_raw.csv"
# 外部测试集：已禁用，留空即可
EXTERNAL_TEST_CSV_PATH = ""
# 最终输出根目录
OUTPUT_ROOT_DIR = "data/processed"
# -------------------------- 核心列名配置（必须和你的csv里的列名完全一致） --------------------------
# 样本ID列：区分每一次录制，同一个sample_id=一次完整的手语录制=一个独立样本
SAMPLE_ID_COL = "sample_id"
# 标签列：手语类别名称/数字，比如"向下""向上"
LABEL_COL = "label"
# 特征列：填写你的传感器数据列名，比如["acc_x","acc_y","acc_z","gyro_x","gyro_y","gyro_z"]
# 留空则自动使用「除了SAMPLE_ID_COL、LABEL_COL、timestamp之外的所有列」作为特征
FEATURE_COLS = [
    "f_0_s1_acc_x", "f_0_s1_acc_y", "f_0_s1_acc_z", "f_0_s1_gyro_x", "f_0_s1_gyro_y", "f_0_s1_gyro_z",
    "f_0_s2_acc_x", "f_0_s2_acc_y", "f_0_s2_acc_z", "f_0_s2_gyro_x", "f_0_s2_gyro_y", "f_0_s2_gyro_z",
    "f_1_s1_acc_x", "f_1_s1_acc_y", "f_1_s1_acc_z", "f_1_s1_gyro_x", "f_1_s1_gyro_y", "f_1_s1_gyro_z",
    "f_1_s2_acc_x", "f_1_s2_acc_y", "f_1_s2_acc_z", "f_1_s2_gyro_x", "f_1_s2_gyro_y", "f_1_s2_gyro_z",
    "f_2_s1_acc_x", "f_2_s1_acc_y", "f_2_s1_acc_z", "f_2_s1_gyro_x", "f_2_s1_gyro_y", "f_2_s1_gyro_z",
    "f_2_s2_acc_x", "f_2_s2_acc_y", "f_2_s2_acc_z", "f_2_s2_gyro_x", "f_2_s2_gyro_y", "f_2_s2_gyro_z",
    "f_3_s1_acc_x", "f_3_s1_acc_y", "f_3_s1_acc_z", "f_3_s1_gyro_x", "f_3_s1_gyro_y", "f_3_s1_gyro_z",
    "f_3_s2_acc_x", "f_3_s2_acc_y", "f_3_s2_acc_z", "f_3_s2_gyro_x", "f_3_s2_gyro_y", "f_3_s2_gyro_z",
    "f_4_s1_acc_x", "f_4_s1_acc_y", "f_4_s1_acc_z", "f_4_s1_gyro_x", "f_4_s1_gyro_y", "f_4_s1_gyro_z",
    "f_4_s2_acc_x", "f_4_s2_acc_y", "f_4_s2_acc_z", "f_4_s2_gyro_x", "f_4_s2_gyro_y", "f_4_s2_gyro_z",
    "dorsal_acc_x", "dorsal_acc_y", "dorsal_acc_z",
    "dorsal_gyro_x", "dorsal_gyro_y", "dorsal_gyro_z",
    "dorsal_mag_x", "dorsal_mag_y", "dorsal_mag_z"
]
# -------------------------- 第一步：因果滑动平均滤波参数 --------------------------
ENABLE_SMOOTH = True
SMOOTH_WINDOW = 5  # 窗口大小，建议3~7，过大可能过度平滑导致动作细节丢失，过小则平滑效果不明显
# -------------------------- 第二步：动作端点检测参数 --------------------------
ENABLE_ENDPOINT_DETECT = False
MODLENGTH_CALC_MODE = "frame_diff"
HIGH_THRESHOLD_COEF = 0.2
LOW_THRESHOLD_COEF = 0.05
THRESHOLD_STAT_TYPE = "median"
MIN_VALID_FRAMES = 40
# -------------------------- 第三步：序列长度统一参数 --------------------------
SEQ_LEN = 40  # BLSTM要求的固定序列长度
ADJUST_MODE = "interp"  # interp=线性插值（推荐） | trunc_pad=截断+填充
# -------------------------- 第四步：归一化&数据集划分参数 --------------------------
ENABLE_NORMALIZE = True
# 验证集比例（从训练集中划分）
VAL_SPLIT_RATIO = 0.1
# 测试集比例（从总数据中划分）
TEST_SPLIT_RATIO = 0.1
RANDOM_SEED = 42
# ==================================================================================================

# -------------------------- 第一步：因果滑动平均滤波核心函数 --------------------------
def causal_moving_average(data, window_size=5):
    frame_num = data.shape[0]
    if window_size <= 1 or frame_num == 0:
        return data
    if window_size > frame_num:
        window_size = frame_num

    cumsum = np.cumsum(data, axis=0)
    smoothed = np.empty_like(data, dtype=np.float64)

    for t in range(frame_num):
        start = t - window_size + 1
        if start <= 0:
            window_sum = cumsum[t]
            current_window = t + 1
        else:
            window_sum = cumsum[t] - cumsum[start - 1]
            current_window = window_size
        smoothed[t] = window_sum / current_window

    return smoothed.astype(data.dtype, copy=False)
# -------------------------- 第二步：动作端点检测核心函数 --------------------------
def calc_frame_modlength(gt_data, mode="frame_diff"):
    frame_num = gt_data.shape[0]
    flatten_data = gt_data.reshape(frame_num, -1)
    
    if mode == "frame_diff":
        if frame_num < 2:
            return np.zeros(frame_num)
        frame_diff = np.diff(flatten_data, axis=0)
        diff_modlength = np.linalg.norm(frame_diff, axis=1)
        frame_modlength = np.concatenate([[diff_modlength[0]], diff_modlength, [diff_modlength[-1]]])[:frame_num]
    else:
        frame_modlength = np.linalg.norm(flatten_data, axis=1)
    
    return frame_modlength

def action_endpoint_detection(gt_data):
    frame_num = gt_data.shape[0]
    if frame_num <= MIN_VALID_FRAMES:
        return gt_data, (0, frame_num - 1)
    
    modlength_array = calc_frame_modlength(gt_data, mode=MODLENGTH_CALC_MODE)
    
    if THRESHOLD_STAT_TYPE == "median":
        stat_value = np.median(modlength_array)
    else:
        stat_value = np.mean(modlength_array)
    high_thresh = stat_value * HIGH_THRESHOLD_COEF
    low_thresh = stat_value * LOW_THRESHOLD_COEF
    
    high_mask = modlength_array >= high_thresh
    if not np.any(high_mask):
        return gt_data, (0, frame_num - 1)
    
    start_core = np.argmax(high_mask)
    start_idx = 0
    for i in range(start_core, -1, -1):
        if modlength_array[i] < low_thresh:
            start_idx = i + 1
            break
    
    end_core = len(high_mask) - 1 - np.argmax(high_mask[::-1])
    end_idx = frame_num - 1
    for i in range(end_core, frame_num):
        if modlength_array[i] < low_thresh:
            end_idx = i - 1
            break
    
    start_idx = max(0, start_idx)
    end_idx = min(frame_num - 1, end_idx)
    if (end_idx - start_idx + 1) < MIN_VALID_FRAMES:
        return gt_data, (0, frame_num - 1)
    
    valid_action_segment = gt_data[start_idx:end_idx+1]
    return valid_action_segment, (start_idx, end_idx)

# -------------------------- 第三步：序列长度统一核心函数 --------------------------
def adjust_seq_length(gt_data, target_seq_len, mode="interp"):
    current_len = gt_data.shape[0]
    if current_len == target_seq_len:
        return gt_data
    
    original_shape = gt_data.shape
    flatten_data = gt_data.reshape(current_len, -1)
    feature_dim = flatten_data.shape[1]
    
    if mode == "interp":
        x_original = np.linspace(0, 1, current_len)
        x_target = np.linspace(0, 1, target_seq_len)
        interpolator = interp1d(x_original, flatten_data, kind="linear", axis=0)
        adjusted_data = interpolator(x_target)
    
    elif mode == "trunc_pad":
        if current_len > target_seq_len:
            adjusted_data = flatten_data[:target_seq_len]
        else:
            pad_len = target_seq_len - current_len
            last_frame = np.expand_dims(flatten_data[-1], axis=0)
            pad_frames = np.repeat(last_frame, pad_len, axis=0)
            adjusted_data = np.concatenate([flatten_data, pad_frames], axis=0)
    
    else:
        raise ValueError(f"不支持的长度调整方式：{mode}，仅支持 interp / trunc_pad")
    
    adjusted_data = adjusted_data.reshape(target_seq_len, *original_shape[1:])
    return adjusted_data

# -------------------------- 第四步：归一化&数据集划分核心函数 --------------------------
def normalize_data(train_data, val_data, test_data):
    flatten_train = train_data.reshape(-1, train_data.shape[-1])
    mean = np.mean(flatten_train, axis=0)
    std = np.std(flatten_train, axis=0)
    std[std == 0] = 1e-8
    
    train_normalized = (train_data - mean) / std
    val_normalized = (val_data - mean) / std
    test_normalized = (test_data - mean) / std
    
    return train_normalized, val_normalized, test_normalized, mean, std

def split_train_val_test(all_data, all_labels, class_names):
    # 第一步：先划分 train_val 和 test
    train_val_data, test_data, train_val_labels, test_labels = train_test_split(
        all_data, all_labels,
        test_size=TEST_SPLIT_RATIO,
        random_state=RANDOM_SEED,
        stratify=all_labels
    )
    # 第二步：从 train_val 中划分 train 和 val
    train_data, val_data, train_labels, val_labels = train_test_split(
        train_val_data, train_val_labels,
        test_size=VAL_SPLIT_RATIO,
        random_state=RANDOM_SEED,
        stratify=train_val_labels
    )
    return train_data, val_data, test_data, train_labels, val_labels, test_labels, class_names

def process_grouped_samples(sample_groups, class2label):
    """
    按 sample_id 分组处理，最终输出：
      - 数据： (N, SEQ_LEN, 138)  左手69维 + 右手69维
    遵循：
      - 主动手基准对齐：统一时间轴由主动手决定，非主动手插值跟随
      - 双手样本（both）暂以右手为基准
      - 非主动手严格跟随，不填充不改动
      - 仅在单手缺失时缺失手填0（模拟无数据）
    """
    all_data = []
    all_labels = []
    sample_info = []

    # 已知采样周期 90ms，用作统一时间轴步长
    DEFAULT_SAMPLING_PERIOD_MS = 90.0

    for sample_id, sample_df in tqdm(sample_groups, desc="处理样本"):
        try:
            # ---- 标签 ----
            original_label = str(sample_df[LABEL_COL].iloc[0])
            if original_label not in class2label:
                print(f"⚠️ 跳过未知类别样本 sample_id={sample_id}，label={original_label}")
                continue
            label = class2label[original_label]

            # ---- 主动手 ----
            if 'active_hand' in sample_df.columns:
                active_hand = str(sample_df['active_hand'].iloc[0]).lower()
            else:
                active_hand = "right"

            # ---- 按 hand_type 拆分左右手 ----
            left_df = sample_df[sample_df['hand_type'] == 'left'].copy()
            right_df = sample_df[sample_df['hand_type'] == 'right'].copy()

            has_left = len(left_df) > 0
            has_right = len(right_df) > 0

            if not has_left and not has_right:
                print(f"⚠️ 样本 {sample_id} 无任何手数据，跳过")
                continue

            # ---- 滤波（可选）----
            if ENABLE_SMOOTH:
                for col in FEATURE_COLS:
                    if has_left:
                        left_df[col] = causal_moving_average(left_df[col].values, window_size=SMOOTH_WINDOW)
                    if has_right:
                        right_df[col] = causal_moving_average(right_df[col].values, window_size=SMOOTH_WINDOW)

            # ---- 提取时间戳 + 特征（每只手 69 维）----
            def get_hand_data(hand_df):
                if hand_df is None or len(hand_df) == 0:
                    return None, None
                hand_df = hand_df.sort_values('timestamp/ms')
                timestamps = hand_df['timestamp/ms'].values.astype(np.float64)
                features = hand_df[FEATURE_COLS].values.astype(np.float32)
                return timestamps, features

            left_ts, left_feat = get_hand_data(left_df)
            right_ts, right_feat = get_hand_data(right_df)

            # ===== 关键修复：时间戳归零（相对时间） =====
            if left_ts is not None:
                left_ts = left_ts - left_ts[0]
            if right_ts is not None:
                right_ts = right_ts - right_ts[0]
            # =========================================

            # ---- 插值函数定义 ----
            def interp_hand_to_timeline(ts, feat, target_ts):
                """线性插值；若原数据缺失则返回全0数组"""
                if ts is None or feat is None:
                    return np.zeros((len(target_ts), len(FEATURE_COLS)), dtype=np.float32)
                # 保证时间戳递增，去重
                sort_idx = np.argsort(ts)
                ts = ts[sort_idx]
                feat = feat[sort_idx]
                unique_ts, idx = np.unique(ts, return_index=True)
                unique_feat = feat[idx]
                if len(unique_ts) < 2:
                    return np.tile(unique_feat, (len(target_ts), 1)).astype(np.float32)
                f = interp1d(unique_ts, unique_feat, axis=0, kind='linear',
                             bounds_error=False, fill_value='extrapolate')
                return f(target_ts).astype(np.float32)

            # ---- 主动手基准时间轴生成 ----
            # 选择基准手：优先使用 active_hand 指定的手，若缺失则用另一只手
            if active_hand == 'left':
                base_ts = left_ts
                other_ts = right_ts
            elif active_hand == 'right':
                base_ts = right_ts
                other_ts = left_ts
            else:  # both: 暂以右手为基准（可后续优化为交集）
                base_ts = right_ts if right_ts is not None else left_ts
                other_ts = left_ts if right_ts is not None else right_ts

            # 如果基准手缺失，尝试用另一只手
            if base_ts is None:
                base_ts = other_ts
                other_ts = None

            if base_ts is None:
                print(f"⚠️ 样本 {sample_id} 无有效时间戳，跳过")
                continue

            dt = DEFAULT_SAMPLING_PERIOD_MS
            t_min = base_ts[0]
            t_max = base_ts[-1]
            unified_ts = np.arange(t_min, t_max + dt, dt)

            # ---- 双手数据对齐到基准时间轴 ----
            # 注意：不管另一只手的时间戳范围如何，都插值到基准轴上（外插产生边界值）
            if active_hand == 'left' or (active_hand == 'both' and base_ts is left_ts):
                left_aligned = interp_hand_to_timeline(left_ts, left_feat, unified_ts)
                right_aligned = interp_hand_to_timeline(right_ts, right_feat, unified_ts)
            else:
                right_aligned = interp_hand_to_timeline(right_ts, right_feat, unified_ts)
                left_aligned = interp_hand_to_timeline(left_ts, left_feat, unified_ts)

            # ===== 临时诊断：仅第一个样本打印 =====
            if len(sample_info) == 0:
                print(f"  样本 {sample_id}: 左手原始帧数={len(left_ts) if left_ts is not None else 0}, 右手原始帧数={len(right_ts) if right_ts is not None else 0}")
                print(f"  主动手={'left' if (active_hand=='left' or (active_hand=='both' and base_ts is left_ts)) else 'right'}, 统一时间轴长度={len(unified_ts)}, 范围=[{t_min:.0f}, {t_max:.0f}] ms")
                print(f"  左手对齐后形状={left_aligned.shape}, 右手对齐后形状={right_aligned.shape}")
            # =========================================

            # ---- 动作端点检测 ----
            # ---- 动作端点检测 ----
            main_data = None  # 【关键修复】先初始化变量，确保所有分支都有定义
            if ENABLE_ENDPOINT_DETECT:
                if active_hand == 'left':
                    main_data = left_aligned
                elif active_hand == 'right':
                    main_data = right_aligned
                else:  # both
                    # 分别检测左右手动作段，取并集（端点检测仍在统一时间轴上）
                    _, (l_start, l_end) = action_endpoint_detection(left_aligned)
                    _, (r_start, r_end) = action_endpoint_detection(right_aligned)
                    start_idx = min(l_start, r_start)
                    end_idx = max(l_end, r_end)
                    main_data = None  # 已直接获得区间
                
                # 【关键修复】把端点检测逻辑移到这里，只在开启时执行
                if active_hand != 'both' and main_data is not None:
                    _, (start_idx, end_idx) = action_endpoint_detection(main_data)
            else:
                start_idx, end_idx = 0, len(unified_ts) - 1

            # ---- 裁剪：非主动手严格跟随 ----
            left_seg = left_aligned[start_idx:end_idx+1]    # (T_cut, 69)
            right_seg = right_aligned[start_idx:end_idx+1]  # (T_cut, 69)

            # ---- 统一长度 (e.g., 40) ----
            left_final = adjust_seq_length(left_seg, SEQ_LEN, mode=ADJUST_MODE)    # (SEQ_LEN, 69)
            right_final = adjust_seq_length(right_seg, SEQ_LEN, mode=ADJUST_MODE)  # (SEQ_LEN, 69)

            # ---- 拼接：左手 + 右手 = 138 维 ----
            final_seq = np.concatenate([left_final, right_final], axis=-1)  # (SEQ_LEN, 138)

            all_data.append(final_seq)
            all_labels.append(label)
            sample_info.append({
                "sample_id": sample_id,
                "original_label": original_label,
                "numeric_label": label,
                "original_frames": len(sample_df),
                "valid_frames": final_seq.shape[0],
                "action_start_idx": start_idx,
                "action_end_idx": end_idx,
                "hand_order": "L->R",
                "alignment_method": "active_hand_or_both_union",
                "active_hand": active_hand,
                "data_quality": {}
            })

        except Exception as e:
            print(f"❌ 处理样本 sample_id={sample_id} 失败：{str(e)}")
            continue

    return np.array(all_data), np.array(all_labels), sample_info


# -------------------------- 主流程函数（适配csv数据集） --------------------------
def run_full_pipeline():
    # 1. 初始化目录
    os.makedirs(OUTPUT_ROOT_DIR, exist_ok=True)
    final_dataset_dir = os.path.join(OUTPUT_ROOT_DIR, "final_blstm_dataset")
    os.makedirs(final_dataset_dir, exist_ok=True)
    
    # 2. 读取csv原始数据
    print("="*70)
    print(f"📂 读取原始csv数据：{RAW_CSV_PATH}")
    df = read_csv_with_fallback(RAW_CSV_PATH)
    print(f"✅ 原始数据总行数：{len(df)}，列名：{list(df.columns)}")
    
    # 3. 自动识别特征列
    global FEATURE_COLS
    if len(FEATURE_COLS) == 0:
        exclude_cols = [SAMPLE_ID_COL, LABEL_COL, "timestamp", "time"]
        FEATURE_COLS = [col for col in df.columns if col not in exclude_cols]
    print(f"✅ 识别到特征列：{FEATURE_COLS}，共 {len(FEATURE_COLS)} 维特征")
    
    # 4. 按sample_id拆分独立样本（核心：一次录制=一个sample_id=一个独立样本）
    print("="*70)
    print("🔍 按sample_id拆分独立样本，严格遵循「一次录制=一个样本」")
    sample_groups = df.groupby(SAMPLE_ID_COL)
    print(f"✅ 识别到总样本数（独立录制次数）：{len(sample_groups)}")
    
    # 5. 自动识别类别并分配数字标签
    unique_labels = sorted(df[LABEL_COL].unique())
    class2label = {str(label): idx for idx, label in enumerate(unique_labels)}
    label2class = {idx: str(label) for label, idx in class2label.items()}
    print(f"✅ 识别到手语类别：{unique_labels}，共 {len(unique_labels)} 类")
    print(f"✅ 类别-标签映射：{class2label}")
    
    # 6. 全流程处理：端点检测 → 长度统一
    print("="*70)
    print(f"🧹 第一步：因果滑动平均滤波 {'开启' if ENABLE_SMOOTH else '跳过'}，窗口={SMOOTH_WINDOW}")
    print(f"🔍 第二步：动作端点检测 {'开启' if ENABLE_ENDPOINT_DETECT else '跳过'}")
    print(f"📐 第三步：序列长度统一，目标SEQ_LEN={SEQ_LEN}，调整方式={ADJUST_MODE}")
    print("="*70)

    # 6.1 处理 RAW_CSV_PATH 样本
    all_data, all_labels, sample_info = process_grouped_samples(sample_groups, class2label)
    print(f"\n✅ 2-3步处理完成！总有效样本数：{all_data.shape[0]}")
    print(f"✅ 数据格式：{all_data.shape}（样本数, SEQ_LEN, 特征维度），完全符合BLSTM输入要求")
    
    # 7. 第四步：数据归一化 + 划分训练/验证/测试集
    print("="*70)
    print(f"📊 第四步：数据归一化 {'开启' if ENABLE_NORMALIZE else '跳过'} + 划分训练/验证/测试集")
    print("="*70)

    # 划分数据集
    train_data, val_data, test_data, train_labels, val_labels, test_labels, class_names = split_train_val_test(
        all_data, all_labels, unique_labels
    )

    # 归一化
    if ENABLE_NORMALIZE:
        train_data, val_data, test_data, mean, std = normalize_data(train_data, val_data, test_data)
        np.save(os.path.join(final_dataset_dir, "normalize_mean.npy"), mean)
        np.save(os.path.join(final_dataset_dir, "normalize_std.npy"), std)
        print(f"✅ 归一化完成，已保存均值和标准差参数")

    # 保存最终数据集
    np.save(os.path.join(final_dataset_dir, "train_data.npy"), train_data)
    np.save(os.path.join(final_dataset_dir, "train_labels.npy"), train_labels)
    np.save(os.path.join(final_dataset_dir, "val_data.npy"), val_data)
    np.save(os.path.join(final_dataset_dir, "val_labels.npy"), val_labels)
    np.save(os.path.join(final_dataset_dir, "test_data.npy"), test_data)
    np.save(os.path.join(final_dataset_dir, "test_labels.npy"), test_labels)
    
    with open(os.path.join(final_dataset_dir, "class2label.json"), "w", encoding="utf-8") as f:
        json.dump(class2label, f, ensure_ascii=False, indent=2)
    with open(os.path.join(final_dataset_dir, "label2class.json"), "w", encoding="utf-8") as f:
        json.dump(label2class, f, ensure_ascii=False, indent=2)
    with open(os.path.join(final_dataset_dir, "sample_process_info.json"), "w", encoding="utf-8") as f:
        json.dump(sample_info, f, ensure_ascii=False, indent=2)
    
    # 打印最终结果
    print(f"\n🎉 全流程执行完成！")
    print(f"✅ 训练集：{train_data.shape[0]} 个样本，验证集：{val_data.shape[0]} 个样本，测试集：{test_data.shape[0]} 个样本")
    print(f"✅ 最终数据格式：")
    print(f"   train_data.shape = {train_data.shape}（样本数, SEQ_LEN, 特征维度）")
    print(f"   train_labels.shape = {train_labels.shape}")
    print(f"   val_data.shape = {val_data.shape}（样本数, SEQ_LEN, 特征维度）")
    print(f"   val_labels.shape = {val_labels.shape}")
    print(f"   test_data.shape = {test_data.shape}（样本数, SEQ_LEN, 特征维度）")
    print(f"   test_labels.shape = {test_labels.shape}")
    organize_final_dataset_dir(final_dataset_dir)
    write_final_dataset_readme(final_dataset_dir)
    print(f"✅ 所有文件已保存并分类整理至：{final_dataset_dir}")
    print(f"✅ 训练集路径：{os.path.join(final_dataset_dir, 'train')}")
    print(f"✅ 验证集路径：{os.path.join(final_dataset_dir, 'val')}")
    print(f"✅ 测试集路径：{os.path.join(final_dataset_dir, 'test')}")
    print(f"✅ 说明文件：{os.path.join(final_dataset_dir, 'readme.txt')}")

if __name__ == "__main__":
    # 安装依赖命令（首次运行先执行）：
    # pip install numpy pandas tqdm scipy scikit-learn
    run_full_pipeline()