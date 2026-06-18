"""
AI引擎模型
"""
from datetime import datetime
from app import db


class AIEngine(db.Model):
    """AI引擎配置表 - 存储第三方大模型服务接入信息"""
    __tablename__ = 'ai_engines'

    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(64), nullable=False)
    api_url = db.Column(db.String(256), nullable=False)
    api_key = db.Column(db.String(256), nullable=False)
    model_name = db.Column(db.String(128), nullable=False)
    system_prompt = db.Column(db.Text, nullable=True, default='', server_default='')
    is_active = db.Column(db.Boolean, nullable=False, default=True)
    created_at = db.Column(db.DateTime, default=datetime.utcnow)
    updated_at = db.Column(db.DateTime, default=datetime.utcnow, onupdate=datetime.utcnow)

    def to_dict(self, mask_key=True):
        key = ('****' + self.api_key[-4:]) if mask_key and len(self.api_key) > 4 else '****'
        return {
            'id': self.id,
            'name': self.name,
            'api_url': self.api_url,
            'api_key': key,
            'model_name': self.model_name,
            'system_prompt': self.system_prompt or '',
            'is_active': self.is_active,
            'created_at': self.created_at.strftime('%Y-%m-%d %H:%M') if self.created_at else '',
        }

    def __repr__(self):
        return f'<AIEngine {self.name} ({self.model_name})>'
