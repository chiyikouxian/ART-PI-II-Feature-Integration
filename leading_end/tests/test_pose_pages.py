from flask import render_template


def _login(client):
    return client.post(
        '/auth/login',
        data={'username': 'testuser', 'password': 'password123'},
    )


def test_real_time_translate_page_renders_for_authenticated_user(client, auth_user):
    _login(client)

    response = client.get('/real-time-translate')

    assert response.status_code == 200
    assert b'window.runAllTests' in response.data


def test_legacy_gesture_route_redirects_to_real_time_page(client, auth_user):
    _login(client)

    response = client.get('/gesture-recognition')

    assert response.status_code == 302
    assert response.headers['Location'].endswith('/real-time-translate')


def test_legacy_gesture_template_still_renders(app):
    with app.test_request_context():
        rendered = render_template('gesture_recognition.html')

    assert 'window.runAllTests' in rendered


def test_channel_mapping_keeps_right_hand_channels_editable(client, auth_user):
    _login(client)

    response = client.get('/channel-mapping')

    assert response.status_code == 200
    assert b'RIGHT_DEFAULT_MAP' in response.data
    assert b'RIGHT_FIXED_MAP' not in response.data
    assert b"data-dev=\"' + dev + '\"" in response.data
