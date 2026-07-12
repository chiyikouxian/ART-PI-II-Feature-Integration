"""
核心应用包初始化
"""
from flask import Flask
from flask_sqlalchemy import SQLAlchemy
from flask_migrate import Migrate
from flask_login import LoginManager
from config import Config

db = SQLAlchemy()
migrate = Migrate()
login_manager = LoginManager()
login_manager.login_view = 'auth.login'


def create_app(config_class=Config):
    """应用工厂函数"""
    app = Flask(__name__,
                template_folder='../templates',
                static_folder='../static')
    app.config.from_object(config_class)

    # 初始化扩展
    db.init_app(app)
    migrate.init_app(app, db)
    login_manager.init_app(app)

    # 注册蓝图
    from app.views.main import main_bp
    from app.views.auth import auth_bp
    from app.views.api import api_bp

    app.register_blueprint(main_bp)
    app.register_blueprint(auth_bp, url_prefix='/auth')
    app.register_blueprint(api_bp, url_prefix='/api')

    @app.context_processor
    def inject_menu_items():
        """将菜单数据注入所有模板（仅顶级项，子菜单通过 children 关系自动加载）"""
        from app.models.menu_item import MenuItem
        try:
            items = MenuItem.query.filter_by(is_visible=True, parent_id=None)\
                .order_by(MenuItem.sort_order.asc()).all()
        except Exception:
            items = []
        return dict(menu_items=items)

    return app
