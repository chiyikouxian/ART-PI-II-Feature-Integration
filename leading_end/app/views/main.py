"""
主页视图
"""
from flask import Blueprint, render_template, redirect, url_for
from flask_login import login_required, current_user

main_bp = Blueprint('main', __name__)


@main_bp.route('/')
@login_required
def index():
    """首页 - 直接跳转到实时翻译"""
    return redirect(url_for('main.real_time_translate'))


@main_bp.route('/gesture-recognition')
@login_required
def gesture_recognition():
    """手势识别与翻译页面（旧页面，重定向到实时翻译）"""
    return redirect(url_for('main.real_time_translate'))


@main_bp.route('/real-time-translate')
@login_required
def real_time_translate():
    """实时翻译页面 - 3D骨骼模型 + 翻译结果"""
    return render_template('real_time_translate.html')


@main_bp.route('/channel-mapping')
@login_required
def channel_mapping():
    """通道映射页面 - 传感器通道与骨骼绑定配置"""
    return render_template('channel_mapping.html')


@main_bp.route('/menu-manage')
@login_required
def menu_manage():
    """界面管理页面"""
    return render_template('menu_manage.html')


@main_bp.route('/device-monitor')
@login_required
def device_monitor():
    """设备数据监控页面"""
    return render_template('device_monitor.html')


@main_bp.route('/ai-engine')
@login_required
def ai_engine():
    """AI引擎管理页面"""
    return render_template('ai_engine.html')
