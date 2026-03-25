"""
API视图
"""
import os
from flask import Blueprint, jsonify, request, current_app
from flask_login import login_required
from werkzeug.utils import secure_filename
from app import db
from app.models.glove_data import GloveData
from app.models.menu_item import MenuItem
from app.models.ai_engine import AIEngine
from app.services.glove_service import GloveService

api_bp = Blueprint('api', __name__)


@api_bp.route('/glove/data', methods=['POST'])
def receive_glove_data():
    """接收手套传感器数据"""
    data = request.get_json()
    if not data:
        return jsonify({'error': '无效的数据'}), 400

    try:
        result = GloveService.save_sensor_data(data)
        return jsonify({'success': True, 'id': result.id})
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@api_bp.route('/glove/data', methods=['GET'])
def get_glove_data():
    """获取手套数据列表"""
    page = request.args.get('page', 1, type=int)
    per_page = request.args.get('per_page', 20, type=int)

    pagination = GloveData.query.order_by(GloveData.timestamp.desc()).paginate(
        page=page, per_page=per_page, error_out=False
    )

    return jsonify({
        'data': [{'id': d.id, 'gesture_type': d.gesture_type,
                  'timestamp': d.timestamp.isoformat()} for d in pagination.items],
        'total': pagination.total,
        'pages': pagination.pages,
        'current_page': page
    })


@api_bp.route('/glove/recognize', methods=['POST'])
def recognize_gesture():
    """手势识别接口"""
    data = request.get_json()
    if not data or 'sensor_data' not in data:
        return jsonify({'error': '缺少传感器数据'}), 400

    try:
        result = GloveService.recognize_gesture(data['sensor_data'])
        return jsonify({'success': True, 'gesture': result})
    except Exception as e:
        return jsonify({'error': str(e)}), 500


# ====== 设备监控 API ======

@api_bp.route('/device/status', methods=['GET'])
def get_device_status():
    """获取设备连接状态 (支持 ?device=right 查询指定设备)"""
    from app.services.tcp_server import get_device_status as _get_status
    device_id = request.args.get('device', None)
    status = _get_status(device_id)
    return jsonify({'code': 0, 'data': status})


@api_bp.route('/device/latest', methods=['GET'])
def get_device_latest():
    """获取最新一条设备数据 (支持 ?device=right)"""
    from app.services.tcp_server import get_latest_data
    device_id = request.args.get('device', None)
    data = get_latest_data(device_id)
    return jsonify({'code': 0, 'data': data})


@api_bp.route('/device/history', methods=['GET'])
def get_device_history():
    """获取设备历史数据 (支持 ?device=right&limit=50)"""
    from app.services.tcp_server import get_history
    limit = request.args.get('limit', 50, type=int)
    device_id = request.args.get('device', None)
    data = get_history(limit, device_id)
    return jsonify({'code': 0, 'data': data, 'count': len(data)})


@api_bp.route('/device/list', methods=['GET'])
def get_device_list():
    """获取所有在线设备列表"""
    from app.services.tcp_server import get_connected_devices
    devices = get_connected_devices()
    return jsonify({'code': 0, 'data': devices, 'count': len(devices)})


@api_bp.route('/device/send', methods=['POST'])
def send_to_device():
    """向指定设备发送数据
    POST JSON: {"device": "right", "data": "hello"}
    """
    from app.services.tcp_server import send_to_device as _send
    body = request.get_json()
    if not body or 'device' not in body or 'data' not in body:
        return jsonify({'code': 1, 'msg': '缺少 device 或 data 字段'})

    ok = _send(body['device'], body['data'])
    if ok:
        return jsonify({'code': 0, 'msg': '发送成功'})
    else:
        return jsonify({'code': 1, 'msg': '发送失败，设备可能未连接'})


# ====== 菜单管理 API ======

@api_bp.route('/menu/list', methods=['GET'])
@login_required
def get_menu_list():
    """获取所有菜单项"""
    from flask import url_for as _url_for
    top_level = request.args.get('top_level', '', type=str)
    # 已移除的页面，不再在菜单管理中显示
    _removed_endpoints = {'main.index', 'main.dashboard'}
    query = MenuItem.query.filter(MenuItem.endpoint.notin_(_removed_endpoints))\
        .order_by(MenuItem.sort_order.asc())
    if top_level == '1':
        query = query.filter(MenuItem.parent_id.is_(None))
    items = query.all()
    data = []
    for item in items:
        d = item.to_dict()
        try:
            d['url'] = _url_for(item.endpoint)
        except Exception:
            d['url'] = '#'
        data.append(d)
    return jsonify({
        'code': 0,
        'msg': '',
        'count': len(data),
        'data': data
    })


@api_bp.route('/menu/update', methods=['POST'])
@login_required
def update_menu_item():
    """更新菜单项"""
    data = request.get_json()
    if not data or 'id' not in data:
        return jsonify({'code': 1, 'msg': '缺少菜单ID'})

    item = MenuItem.query.get(data['id'])
    if not item:
        return jsonify({'code': 1, 'msg': '菜单项不存在'})

    if 'name' in data:
        item.name = data['name']
    if 'is_visible' in data:
        if item.endpoint == 'main.menu_manage' and not data['is_visible']:
            return jsonify({'code': 1, 'msg': '界面管理不可隐藏'})
        item.is_visible = bool(data['is_visible'])
    if 'sort_order' in data:
        item.sort_order = int(data['sort_order'])
    if 'icon' in data:
        item.icon = data['icon']

    db.session.commit()
    return jsonify({'code': 0, 'msg': '更新成功'})


@api_bp.route('/menu/add', methods=['POST'])
@login_required
def add_menu_item():
    """新增菜单项"""
    data = request.get_json()
    if not data or not data.get('name') or not data.get('endpoint'):
        return jsonify({'code': 1, 'msg': '名称和路由不能为空'})

    max_sort = db.session.query(db.func.max(MenuItem.sort_order)).scalar() or 0
    item = MenuItem(
        name=data['name'],
        endpoint=data['endpoint'],
        icon=data.get('icon', ''),
        sort_order=max_sort + 1,
        is_visible=True,
        is_system=False
    )
    db.session.add(item)
    db.session.commit()
    return jsonify({'code': 0, 'msg': '添加成功', 'data': item.to_dict()})


@api_bp.route('/menu/delete', methods=['POST'])
@login_required
def delete_menu_item():
    """删除菜单项（系统内置项不可删除）"""
    data = request.get_json()
    if not data or 'id' not in data:
        return jsonify({'code': 1, 'msg': '缺少菜单ID'})

    item = MenuItem.query.get(data['id'])
    if not item:
        return jsonify({'code': 1, 'msg': '菜单项不存在'})
    if item.is_system:
        return jsonify({'code': 1, 'msg': '系统内置菜单不可删除，只能隐藏'})

    db.session.delete(item)
    db.session.commit()
    return jsonify({'code': 0, 'msg': '删除成功'})


# ====== AI引擎管理 API ======

@api_bp.route('/ai-engine/list', methods=['GET'])
@login_required
def get_ai_engine_list():
    """获取所有AI引擎配置"""
    engines = AIEngine.query.order_by(AIEngine.created_at.desc()).all()
    return jsonify({
        'code': 0,
        'msg': '',
        'count': len(engines),
        'data': [e.to_dict() for e in engines]
    })


@api_bp.route('/ai-engine/add', methods=['POST'])
@login_required
def add_ai_engine():
    """新增AI引擎"""
    data = request.get_json()
    if not data:
        return jsonify({'code': 1, 'msg': '无效的数据'})

    required = ['name', 'api_url', 'api_key', 'model_name']
    for field in required:
        if not data.get(field, '').strip():
            return jsonify({'code': 1, 'msg': f'{field} 不能为空'})

    engine = AIEngine(
        name=data['name'].strip(),
        api_url=data['api_url'].strip(),
        api_key=data['api_key'].strip(),
        model_name=data['model_name'].strip(),
        is_active=True
    )
    db.session.add(engine)
    db.session.commit()
    return jsonify({'code': 0, 'msg': '添加成功', 'data': engine.to_dict()})


@api_bp.route('/ai-engine/update', methods=['POST'])
@login_required
def update_ai_engine():
    """修改AI引擎"""
    data = request.get_json()
    if not data or 'id' not in data:
        return jsonify({'code': 1, 'msg': '缺少引擎ID'})

    engine = AIEngine.query.get(data['id'])
    if not engine:
        return jsonify({'code': 1, 'msg': '引擎不存在'})

    if 'name' in data:
        engine.name = data['name'].strip()
    if 'api_url' in data:
        engine.api_url = data['api_url'].strip()
    if 'api_key' in data and data['api_key'].strip() and not data['api_key'].startswith('****'):
        engine.api_key = data['api_key'].strip()
    if 'model_name' in data:
        engine.model_name = data['model_name'].strip()
    if 'is_active' in data:
        engine.is_active = bool(data['is_active'])

    db.session.commit()
    return jsonify({'code': 0, 'msg': '更新成功'})


@api_bp.route('/ai-engine/delete', methods=['POST'])
@login_required
def delete_ai_engine():
    """删除AI引擎"""
    data = request.get_json()
    if not data or 'id' not in data:
        return jsonify({'code': 1, 'msg': '缺少引擎ID'})

    engine = AIEngine.query.get(data['id'])
    if not engine:
        return jsonify({'code': 1, 'msg': '引擎不存在'})

    db.session.delete(engine)
    db.session.commit()
    return jsonify({'code': 0, 'msg': '删除成功'})


@api_bp.route('/ai-engine/test', methods=['POST'])
@login_required
def test_ai_engine():
    """测试AI引擎连接"""
    import urllib.request
    import json as json_mod

    data = request.get_json()
    if not data or 'id' not in data:
        return jsonify({'code': 1, 'msg': '缺少引擎ID'})

    engine = AIEngine.query.get(data['id'])
    if not engine:
        return jsonify({'code': 1, 'msg': '引擎不存在'})

    try:
        url = engine.api_url.rstrip('/') + '/chat/completions'
        payload = json_mod.dumps({
            'model': engine.model_name,
            'messages': [{'role': 'user', 'content': 'hi'}],
            'max_tokens': 5
        }).encode('utf-8')

        req = urllib.request.Request(url, data=payload, headers={
            'Content-Type': 'application/json',
            'Authorization': f'Bearer {engine.api_key}'
        })
        with urllib.request.urlopen(req, timeout=10) as resp:
            resp.read()

        return jsonify({'code': 0, 'msg': '连接成功'})
    except Exception as e:
        return jsonify({'code': 1, 'msg': f'连接失败: {str(e)}'})


@api_bp.route('/ai-engine/chat', methods=['POST'])
@login_required
def chat_ai_engine():
    """与AI引擎进行对话"""
    import urllib.request
    import json as json_mod

    data = request.get_json()
    if not data or 'id' not in data or 'messages' not in data:
        return jsonify({'code': 1, 'msg': '缺少必要参数'})

    engine = AIEngine.query.get(data['id'])
    if not engine:
        return jsonify({'code': 1, 'msg': '引擎不存在'})

    try:
        url = engine.api_url.rstrip('/') + '/chat/completions'
        payload = json_mod.dumps({
            'model': engine.model_name,
            'messages': data['messages'],
            'max_tokens': 2048
        }).encode('utf-8')

        req = urllib.request.Request(url, data=payload, headers={
            'Content-Type': 'application/json',
            'Authorization': f'Bearer {engine.api_key}'
        })
        with urllib.request.urlopen(req, timeout=60) as resp:
            result = json_mod.loads(resp.read().decode('utf-8'))

        content = result.get('choices', [{}])[0].get('message', {}).get('content', '')
        return jsonify({'code': 0, 'msg': content})
    except Exception as e:
        return jsonify({'code': 1, 'msg': f'请求失败: {str(e)}'})


# ====== 骨骼模型管理 API ======

ALLOWED_MODEL_EXTENSIONS = {'.glb', '.gltf'}


def _get_models_dir():
    return os.path.join(current_app.static_folder, 'models')


@api_bp.route('/models/list', methods=['GET'])
@login_required
def list_models():
    """获取所有可用的骨骼模型"""
    models_dir = _get_models_dir()
    if not os.path.isdir(models_dir):
        return jsonify({'code': 0, 'data': []})

    models = []
    for fname in sorted(os.listdir(models_dir)):
        ext = os.path.splitext(fname)[1].lower()
        if ext in ALLOWED_MODEL_EXTENSIONS:
            fpath = os.path.join(models_dir, fname)
            models.append({
                'name': fname,
                'size': os.path.getsize(fpath),
                'is_default': fname == 'hand_set.glb',
            })
    return jsonify({'code': 0, 'data': models})


@api_bp.route('/models/upload', methods=['POST'])
@login_required
def upload_model():
    """上传新的骨骼模型文件"""
    if 'file' not in request.files:
        return jsonify({'code': 1, 'msg': '未选择文件'})

    file = request.files['file']
    if file.filename == '':
        return jsonify({'code': 1, 'msg': '未选择文件'})

    ext = os.path.splitext(file.filename)[1].lower()
    if ext not in ALLOWED_MODEL_EXTENSIONS:
        return jsonify({'code': 1, 'msg': '仅支持 .glb / .gltf 格式'})

    filename = secure_filename(file.filename)
    if not filename:
        return jsonify({'code': 1, 'msg': '文件名无效'})

    # 确保扩展名保留
    if not os.path.splitext(filename)[1]:
        filename = filename + ext

    models_dir = _get_models_dir()
    os.makedirs(models_dir, exist_ok=True)

    dest = os.path.join(models_dir, filename)
    if os.path.exists(dest):
        return jsonify({'code': 1, 'msg': f'模型 {filename} 已存在，请先删除或更换文件名'})

    file.save(dest)
    return jsonify({'code': 0, 'msg': '上传成功', 'data': {
        'name': filename,
        'size': os.path.getsize(dest),
        'is_default': False,
    }})


@api_bp.route('/models/delete', methods=['POST'])
@login_required
def delete_model():
    """删除骨骼模型文件"""
    data = request.get_json()
    if not data or 'name' not in data:
        return jsonify({'code': 1, 'msg': '缺少文件名'})

    name = data['name']
    if name == 'hand_set.glb':
        return jsonify({'code': 1, 'msg': '默认模型不可删除'})

    fpath = os.path.join(_get_models_dir(), secure_filename(name))
    if not os.path.isfile(fpath):
        return jsonify({'code': 1, 'msg': '文件不存在'})

    os.remove(fpath)
    return jsonify({'code': 0, 'msg': '删除成功'})
