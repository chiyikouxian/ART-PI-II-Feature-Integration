"""
模型单元测试
"""
import pytest
from app.models.user import User
from app.models.glove_data import GloveData


class TestUserModel:
    """用户模型测试"""

    def test_password_hashing(self, app):
        """测试密码哈希"""
        with app.app_context():
            user = User(username='test', email='test@test.com')
            user.set_password('secret')
            assert user.password_hash != 'secret'
            assert user.check_password('secret')
            assert not user.check_password('wrong')

    def test_user_repr(self, app):
        """测试用户表示"""
        with app.app_context():
            user = User(username='john', email='john@test.com')
            assert repr(user) == '<User john>'


class TestGloveDataModel:
    """手套数据模型测试"""

    def test_glove_data_creation(self, app):
        """测试手套数据创建"""
        with app.app_context():
            from app import db
            data = GloveData(
                gesture_type='fist',
                sensor_data='{"flex": [1,2,3,4,5]}'
            )
            db.session.add(data)
            db.session.commit()
            assert data.id is not None
