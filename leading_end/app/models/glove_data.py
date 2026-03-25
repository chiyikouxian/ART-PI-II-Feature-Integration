"""
手套数据模型
"""
from datetime import datetime
from app import db


class GloveData(db.Model):
    """手套采集数据表"""
    __tablename__ = 'glove_data'

    id = db.Column(db.Integer, primary_key=True)
    user_id = db.Column(db.Integer, db.ForeignKey('users.id'), nullable=True)
    gesture_type = db.Column(db.String(64), nullable=True, comment='手势类型')
    sensor_data = db.Column(db.Text, nullable=False, comment='传感器原始数据(JSON)')
    processed_data = db.Column(db.Text, nullable=True, comment='预处理后数据(JSON)')
    timestamp = db.Column(db.DateTime, default=datetime.utcnow, index=True)

    def __repr__(self):
        return f'<GloveData {self.id} - {self.gesture_type}>'
