"""
API接口测试
"""
import json
import pytest


class TestGloveAPI:
    """手套数据API测试"""

    def test_receive_glove_data(self, client):
        """测试接收手套数据"""
        data = {
            'sensor_data': {'flex_sensors': [100, 200, 300, 400, 500]},
            'gesture_type': 'open_hand'
        }
        response = client.post('/api/glove/data',
                               data=json.dumps(data),
                               content_type='application/json')
        assert response.status_code == 200
        result = json.loads(response.data)
        assert result['success'] is True

    def test_receive_invalid_data(self, client):
        """测试接收无效数据"""
        response = client.post('/api/glove/data',
                               data='invalid',
                               content_type='application/json')
        assert response.status_code == 400

    def test_get_glove_data_list(self, client):
        """测试获取手套数据列表"""
        response = client.get('/api/glove/data')
        assert response.status_code == 200
        result = json.loads(response.data)
        assert 'data' in result
        assert 'total' in result
