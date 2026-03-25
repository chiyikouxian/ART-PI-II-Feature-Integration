"""
手套数据采集模块
"""
import time
import threading
from typing import Callable, Optional


class GloveCollector:
    """手套传感器数据采集器"""

    def __init__(self, port: str = None, baudrate: int = 115200):
        """
        初始化采集器

        Args:
            port: 串口端口号（如 COM3 或 /dev/ttyUSB0）
            baudrate: 波特率
        """
        self.port = port
        self.baudrate = baudrate
        self.is_running = False
        self._thread: Optional[threading.Thread] = None
        self._callback: Optional[Callable] = None
        self._serial = None

    def connect(self) -> bool:
        """连接手套设备"""
        try:
            # TODO: 根据实际硬件实现串口连接
            # import serial
            # self._serial = serial.Serial(self.port, self.baudrate, timeout=1)
            print(f"连接到设备: {self.port}")
            return True
        except Exception as e:
            print(f"连接失败: {e}")
            return False

    def disconnect(self):
        """断开连接"""
        self.stop()
        if self._serial:
            self._serial.close()
            self._serial = None

    def start(self, callback: Callable):
        """
        开始数据采集

        Args:
            callback: 数据回调函数，接收采集到的数据
        """
        if self.is_running:
            return

        self._callback = callback
        self.is_running = True
        self._thread = threading.Thread(target=self._collect_loop, daemon=True)
        self._thread.start()

    def stop(self):
        """停止数据采集"""
        self.is_running = False
        if self._thread:
            self._thread.join(timeout=1)
            self._thread = None

    def _collect_loop(self):
        """数据采集循环"""
        while self.is_running:
            try:
                data = self._read_data()
                if data and self._callback:
                    self._callback(data)
            except Exception as e:
                print(f"采集错误: {e}")
            time.sleep(0.01)  # 100Hz 采样率

    def _read_data(self) -> Optional[dict]:
        """
        读取传感器数据

        Returns:
            传感器数据字典
        """
        # TODO: 根据实际硬件协议实现数据读取
        # 示例数据结构
        return {
            'timestamp': time.time(),
            'flex_sensors': [0] * 5,  # 5个弯曲传感器
            'imu': {
                'accel': [0, 0, 0],
                'gyro': [0, 0, 0],
                'mag': [0, 0, 0]
            }
        }
