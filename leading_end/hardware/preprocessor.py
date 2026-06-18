"""
数据预处理模块
"""
from typing import Dict, List, Any


class DataPreprocessor:
    """传感器数据预处理器"""

    def __init__(self):
        """初始化预处理器"""
        self.calibration_data = None

    def process(self, raw_data: Dict[str, Any]) -> Dict[str, Any]:
        """
        预处理原始传感器数据

        Args:
            raw_data: 原始传感器数据

        Returns:
            预处理后的数据
        """
        processed = {}

        # 处理弯曲传感器数据
        if 'flex_sensors' in raw_data:
            processed['flex_normalized'] = self._normalize_flex(raw_data['flex_sensors'])

        # 处理IMU数据
        if 'imu' in raw_data:
            processed['imu_filtered'] = self._filter_imu(raw_data['imu'])

        return processed

    def _normalize_flex(self, flex_data: List[int]) -> List[float]:
        """
        归一化弯曲传感器数据

        Args:
            flex_data: 原始弯曲传感器值

        Returns:
            归一化后的值 (0-1)
        """
        if not flex_data:
            return []

        # 假设传感器范围 0-1023
        min_val, max_val = 0, 1023
        normalized = [(v - min_val) / (max_val - min_val) for v in flex_data]
        return [max(0, min(1, v)) for v in normalized]

    def _filter_imu(self, imu_data: Dict[str, List[float]]) -> Dict[str, List[float]]:
        """
        滤波IMU数据

        Args:
            imu_data: 原始IMU数据

        Returns:
            滤波后的IMU数据
        """
        filtered = {}

        for key in ['accel', 'gyro', 'mag']:
            if key in imu_data:
                # 简单的低通滤波（实际应用中可使用卡尔曼滤波等）
                filtered[key] = self._low_pass_filter(imu_data[key])

        return filtered

    def _low_pass_filter(self, data: List[float], alpha: float = 0.8) -> List[float]:
        """
        简单低通滤波

        Args:
            data: 输入数据
            alpha: 滤波系数

        Returns:
            滤波后的数据
        """
        if not data:
            return []
        return [v * alpha for v in data]

    def calibrate(self, calibration_samples: List[Dict]) -> None:
        """
        校准传感器

        Args:
            calibration_samples: 校准样本数据
        """
        if not calibration_samples:
            return

        # 计算校准参数
        self.calibration_data = {
            'flex_offset': [0] * 5,
            'imu_offset': {'accel': [0, 0, 0], 'gyro': [0, 0, 0]}
        }
        # TODO: 实现具体校准算法
