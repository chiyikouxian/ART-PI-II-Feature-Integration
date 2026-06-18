"""
菜单项模型
"""
from datetime import datetime
from app import db


class MenuItem(db.Model):
    """侧边栏菜单项配置表"""
    __tablename__ = 'menu_items'

    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(64), nullable=False)
    endpoint = db.Column(db.String(128), nullable=False)
    icon = db.Column(db.String(64), nullable=True, default='')
    sort_order = db.Column(db.Integer, nullable=False, default=0)
    is_visible = db.Column(db.Boolean, nullable=False, default=True)
    is_system = db.Column(db.Boolean, nullable=False, default=False)
    parent_id = db.Column(db.Integer, db.ForeignKey('menu_items.id'), nullable=True, default=None)
    created_at = db.Column(db.DateTime, default=datetime.utcnow)
    updated_at = db.Column(db.DateTime, default=datetime.utcnow, onupdate=datetime.utcnow)

    children = db.relationship('MenuItem', backref=db.backref('parent', remote_side=[id]),
                               order_by='MenuItem.sort_order')

    def to_dict(self):
        return {
            'id': self.id,
            'name': self.name,
            'endpoint': self.endpoint,
            'icon': self.icon,
            'sort_order': self.sort_order,
            'is_visible': self.is_visible,
            'is_system': self.is_system,
            'parent_id': self.parent_id,
        }

    def __repr__(self):
        return f'<MenuItem {self.name} -> {self.endpoint}>'
