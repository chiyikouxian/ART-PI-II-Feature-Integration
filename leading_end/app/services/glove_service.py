"""
手套数据服务层
"""
import json
from app import db
from app.models.glove_data import GloveData


class GloveService:
    """手套数据处理服务"""

    @staticmethod
    def _get_preprocessor():
        """延迟导入预处理器"""
        from hardware.preprocessor import DataPreprocessor
        return DataPreprocessor()

    @staticmethod
    def save_sensor_data(data: dict) -> GloveData:
        """保存传感器数据"""
        sensor_data = json.dumps(data.get('sensor_data', {}))

        # 预处理数据
        preprocessor = GloveService._get_preprocessor()
        processed = preprocessor.process(data.get('sensor_data', {}))

        glove_data = GloveData(
            user_id=data.get('user_id'),
            gesture_type=data.get('gesture_type'),
            sensor_data=sensor_data,
            processed_data=json.dumps(processed)
        )

        db.session.add(glove_data)
        db.session.commit()

        return glove_data

    @staticmethod
    def recognize_gesture(sensor_data: dict) -> str:
        """识别手势"""
        preprocessor = GloveService._get_preprocessor()
        processed = preprocessor.process(sensor_data)

        # TODO: 接入手势识别模型
        # 目前返回占位结果
        return "unknown"

    @staticmethod
    def get_data_by_id(data_id: int) -> GloveData:
        """根据ID获取数据"""
        return GloveData.query.get(data_id)
