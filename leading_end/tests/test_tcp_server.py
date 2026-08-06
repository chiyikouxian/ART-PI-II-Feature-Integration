from app.services.tcp_server import DeviceInfo, _decorate_sensor_frame


def test_device_status_exposes_transport_connection_id():
    device = DeviceInfo('left', object(), ('127.0.0.1', 12345), 7)

    status = device.to_status_dict()

    assert device.connection_id == 7
    assert status['connection_id'] == 7
    assert status['device'] == 'left'


def test_sensor_frame_exposes_transport_session_and_receive_timestamp():
    device = DeviceInfo('right', object(), ('127.0.0.1', 12345), 11)

    frame = _decorate_sensor_frame({'id': 3, 'device': 'right'}, device, 123456)

    assert frame['transport_session_id'] == 11
    assert frame['recv_timestamp_ms'] == 123456
    assert frame['recv_time']
