"""
通用工具函数
"""
import secrets
from datetime import datetime


def format_datetime(dt: datetime, fmt: str = '%Y-%m-%d %H:%M:%S') -> str:
    """格式化日期时间"""
    if dt is None:
        return ''
    return dt.strftime(fmt)


def generate_token(length: int = 32) -> str:
    """生成随机令牌"""
    return secrets.token_hex(length)


def seed_menu_items():
    """初始化默认菜单项，并自动补充新增的系统菜单（支持子菜单）"""
    from app import db
    from app.models.menu_item import MenuItem

    # 顶级菜单（parent_id=None）
    defaults = [
        {'name': '手势识别与翻译', 'endpoint': 'main.gesture_recognition', 'icon': 'layui-icon-read',
         'sort_order': 0, 'is_visible': True, 'is_system': True},
        {'name': '设备数据监控', 'endpoint': 'main.device_monitor', 'icon': 'layui-icon-chart-screen',
         'sort_order': 1, 'is_visible': True, 'is_system': True},
        {'name': 'AI引擎管理', 'endpoint': 'main.ai_engine', 'icon': 'layui-icon-engine',
         'sort_order': 2, 'is_visible': True, 'is_system': True},
        {'name': '界面管理', 'endpoint': 'main.menu_manage', 'icon': 'layui-icon-set',
         'sort_order': 3, 'is_visible': True, 'is_system': True},
    ]

    # 隐藏已移除的菜单项
    removed_endpoints = ['main.index', 'main.dashboard']
    for ep in removed_endpoints:
        old_item = MenuItem.query.filter_by(endpoint=ep).first()
        if old_item:
            old_item.is_visible = False

    for d in defaults:
        existing = MenuItem.query.filter_by(endpoint=d['endpoint']).first()
        if not existing:
            db.session.add(MenuItem(**d))

    db.session.flush()  # 确保父级 ID 已分配

    # "手势识别与翻译" 的子菜单
    parent = MenuItem.query.filter_by(endpoint='main.gesture_recognition').first()
    if parent:
        children = [
            {'name': '实时翻译', 'endpoint': 'main.real_time_translate',
             'sort_order': 0, 'is_visible': True, 'is_system': True, 'parent_id': parent.id},
            {'name': '通道映射', 'endpoint': 'main.channel_mapping',
             'sort_order': 1, 'is_visible': True, 'is_system': True, 'parent_id': parent.id},
        ]
        for c in children:
            existing = MenuItem.query.filter_by(endpoint=c['endpoint']).first()
            if not existing:
                db.session.add(MenuItem(**c))

    db.session.commit()
