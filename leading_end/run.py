"""
应用启动入口
"""
import os
import sys

# 确保项目根目录在 Python 路径中
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from app import create_app, db
from app.models.user import User
from app.models.glove_data import GloveData
from app.models.menu_item import MenuItem
from app.models.ai_engine import AIEngine
from config import DevelopmentConfig

# 创建应用实例（使用开发配置，确保 app.debug=True，避免 TCP 服务器重复启动）
app = create_app(DevelopmentConfig)


@app.shell_context_processor
def make_shell_context():
    """Flask shell 上下文"""
    return {
        'db': db,
        'User': User,
        'GloveData': GloveData,
        'MenuItem': MenuItem,
        'AIEngine': AIEngine
    }


@app.cli.command()
def init_db():
    """初始化数据库"""
    db.create_all()
    print('数据库初始化完成')


@app.cli.command()
def create_admin():
    """创建管理员用户"""
    username = input('请输入管理员用户名: ')
    email = input('请输入管理员邮箱: ')
    password = input('请输入管理员密码: ')

    user = User(username=username, email=email)
    user.set_password(password)
    db.session.add(user)
    db.session.commit()
    print(f'管理员 {username} 创建成功')


def init_database():
    """启动时自动初始化数据库"""
    with app.app_context():
        # 确保 instance 目录存在
        instance_path = os.path.join(os.path.dirname(__file__), 'instance')
        if not os.path.exists(instance_path):
            os.makedirs(instance_path)

        # 创建所有数据表
        db.create_all()

        # 自动迁移: 为 menu_items 表添加 parent_id 列（如果不存在）
        try:
            from sqlalchemy import inspect, text
            inspector = inspect(db.engine)
            columns = [col['name'] for col in inspector.get_columns('menu_items')]
            if 'parent_id' not in columns:
                db.session.execute(text('ALTER TABLE menu_items ADD COLUMN parent_id INTEGER REFERENCES menu_items(id)'))
                db.session.commit()
                print('已自动添加 parent_id 列')
        except Exception as e:
            print(f'迁移检查跳过: {e}')

        # 初始化默认菜单项
        from app.utils.helpers import seed_menu_items
        seed_menu_items()

        print('数据库初始化完成')


if __name__ == '__main__':
    # 自动初始化数据库
    init_database()

    # The device TCP listener must have a single owner. Werkzeug's debug
    # reloader creates a second process, which otherwise races for port 9109
    # (the device-facing TCP listener). `use_reloader=False` below also
    # guarantees a single owner process.
    from app.services.tcp_server import start_tcp_server
    start_tcp_server()

    # PC UDP discovery: broadcasts ARTPI_PC,1,9109 to 255.255.255.255:9108
    # every second so that left/right ART-Pi2 boards can discover the PC's
    # current IP without re-flashing. Must start after TCP server so the
    # TCP port number is stable at import time.
    from app.services.discovery_service import start_discovery_service
    start_discovery_service()

    # 启动应用
    print('=' * 50)
    print('手套数据采集系统启动中...')
    print('访问地址: http://127.0.0.1:5000')
    print('=' * 50)
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)
