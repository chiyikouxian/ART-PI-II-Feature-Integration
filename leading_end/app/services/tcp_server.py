"""
TCP 服务器 - 多设备管理与数据转发

支持多个 ART-PI2 开发板同时连接，通过 JSON 中的 "device" 字段识别设备身份。
提供 API 接口向指定设备发送数据。

数据流:
  开发板 → TCP Server → 内存存储 → REST API → 前端
  前端/API → TCP Server → 指定开发板
"""
import socket
import threading
import json
import time
from collections import deque

# 配置
TCP_HOST = '0.0.0.0'
TCP_PORT = 9109
MAX_HISTORY = 200  # 每个设备最多保留的历史记录条数
MAX_CLIENTS = 10   # 最大同时连接数

# 线程安全锁
_lock = threading.Lock()

# 按设备ID存储: { "right": {...}, "left": {...} }
_devices = {}  # device_id -> DeviceInfo

# 外部命令存储（来自 {"type":"ext_cmd","data":"..."} 帧）
MAX_EXT_CMD = 100
_ext_cmd_list = deque(maxlen=MAX_EXT_CMD)
_ext_cmd_seq = 0  # 单调递增序号，用于前端增量拉取

_server_thread = None
_running = False
_connection_seq = 0


class DeviceInfo:
    """单个设备的连接信息和数据"""
    def __init__(self, device_id, conn, addr, connection_id):
        self.device_id = device_id
        self.conn = conn
        self.addr = f'{addr[0]}:{addr[1]}'
        self.connected = True
        self.last_seen = time.time()
        self.latest_data = None
        self.history = deque(maxlen=MAX_HISTORY)
        self.connection_id = connection_id
        # Issue 5: track per-device boot_id / session_id from HELLO so the
        # frontend can surface "device rebooted" or "device reconnected" in
        # the UI. None until the first HELLO frame is parsed.
        self.boot_id = None
        self.session_id = None

    def to_status_dict(self):
        return {
            'device': self.device_id,
            'connected': self.connected,
            'addr': self.addr,
            'last_seen': self.last_seen,
            'boot_id': self.boot_id,
            'session_id': self.session_id,
            'connection_id': self.connection_id,
        }


def _decorate_sensor_frame(parsed, dev, recv_timestamp_ms=None):
    """Attach transport metadata used by frontend ordering and reconnect logic."""
    parsed.setdefault('recv_time', time.strftime('%H:%M:%S'))
    parsed.setdefault('recv_timestamp_ms', (
        int(time.monotonic() * 1000)
        if recv_timestamp_ms is None
        else int(recv_timestamp_ms)
    ))
    parsed['transport_session_id'] = dev.connection_id
    return parsed


# ====== 对外查询接口 ======

def get_latest_data(device_id=None):
    """获取最新一条数据，指定设备或全部"""
    with _lock:
        if device_id:
            dev = _devices.get(device_id)
            return dev.latest_data if dev else None
        else:
            # 兼容旧接口: 返回最近更新的那个设备的数据
            result = None
            latest_time = 0
            for dev in _devices.values():
                if dev.latest_data and dev.last_seen > latest_time:
                    result = dev.latest_data
                    latest_time = dev.last_seen
            return result


def get_history(limit=50, device_id=None):
    """获取历史记录，可按设备过滤"""
    with _lock:
        if device_id:
            dev = _devices.get(device_id)
            if not dev:
                return []
            return list(dev.history)[:limit]
        else:
            # 合并所有设备的历史，按时间排序
            all_items = []
            for dev in _devices.values():
                all_items.extend(dev.history)
            all_items.sort(key=lambda x: x.get('recv_time', ''), reverse=True)
            return all_items[:limit]


def get_device_status(device_id=None):
    """获取设备连接状态"""
    with _lock:
        if device_id:
            dev = _devices.get(device_id)
            if dev:
                return dev.to_status_dict()
            return {'device': device_id, 'connected': False, 'addr': '', 'last_seen': 0}
        else:
            # 返回所有设备状态
            return {
                'devices': [dev.to_status_dict() for dev in _devices.values()],
                'connected': any(dev.connected for dev in _devices.values()),
                # 兼容旧接口
                'addr': ', '.join(dev.addr for dev in _devices.values() if dev.connected),
                'last_seen': max((dev.last_seen for dev in _devices.values()), default=0),
            }


def get_connected_devices():
    """获取当前在线设备列表"""
    with _lock:
        return [dev.to_status_dict() for dev in _devices.values() if dev.connected]


def get_ext_cmd_list(limit=50, after_seq=0):
    """获取外部命令列表
    :param limit: 最多返回条数
    :param after_seq: 只返回 seq > after_seq 的条目（用于增量拉取）
    :return: list，最新的在前
    """
    with _lock:
        if after_seq > 0:
            result = [item for item in _ext_cmd_list if item.get('seq', 0) > after_seq]
        else:
            result = list(_ext_cmd_list)
        return result[:limit]


# ====== 发送数据到设备 ======

def send_to_device(device_id, data):
    """
    向指定设备发送数据
    :param device_id: 设备ID (如 "left", "right")
    :param data: 字符串数据
    :return: True 成功, False 失败
    """
    with _lock:
        dev = _devices.get(device_id)
        if not dev or not dev.connected:
            return False
        conn = dev.conn

    try:
        if isinstance(data, str):
            data = data.encode('utf-8')
        conn.sendall(data)
        print(f'[TCP] -> [{device_id}] 发送 {len(data)} 字节')
        return True
    except Exception as e:
        print(f'[TCP] -> [{device_id}] 发送失败: {e}')
        return False


def broadcast(data, exclude_device=None):
    """
    广播数据到所有在线设备（可排除发送者）
    :param data: 字符串数据
    :param exclude_device: 排除的设备ID
    """
    with _lock:
        targets = [(dev.device_id, dev.conn) for dev in _devices.values()
                    if dev.connected and dev.device_id != exclude_device]

    for device_id, conn in targets:
        try:
            if isinstance(data, str):
                conn.sendall(data.encode('utf-8'))
            else:
                conn.sendall(data)
        except Exception as e:
            print(f'[TCP] 广播到 [{device_id}] 失败: {e}')


# ====== 连接处理 ======

def _handle_client(conn, addr):
    """处理单个开发板连接"""
    addr_str = f'{addr[0]}:{addr[1]}'
    print(f'[TCP] 新连接: {addr_str}')

    device_id = None
    buffer = ''
    hello_boot_id = None
    hello_session_id = None

    try:
        while _running:
            try:
                chunk = conn.recv(1024)
            except socket.timeout:
                continue
            if not chunk:
                break

            raw = chunk.decode('utf-8', errors='ignore')
            buffer += raw
            lines = buffer.split('\n')
            buffer = lines.pop()  # 保留不完整的最后一段

            for line in lines:
                trimmed = line.strip()
                if not trimmed:
                    continue

                # --- Heartbeat protocol (issue 3 fix) ---
                # The ART-Pi2 boards send "PING\n" every 2s and disconnect
                # if no "PONG\n" reply arrives within 6s. Reply BEFORE the
                # JSON parser runs so a non-JSON PING line doesn't get
                # logged as "JSON 解析失败". HELLO lines are also routed
                # here so the leading session announce never reaches JSON.
                if trimmed == 'PING':
                    try:
                        conn.sendall(b'PONG\n')
                    except Exception as exc:
                        print(f'[TCP] [{addr_str}] PONG send failed: {exc}')
                    continue
                if trimmed.startswith('HELLO,'):
                    # Issue 5 fix: parse HELLO so we can:
                    #   1. log a structured session-announce (boot + session id)
                    #   2. clear the *current connection's* cached latest_data
                    #      and history so a stale "device is still showing
                    #      last frame" ghost doesn't survive a reboot / re-flash.
                    # The format on the wire is:
                    #   HELLO,<side>,<boot_hex>,<session_hex>\n
                    # where side is one of {C, S, L, R}.
                    try:
                        parts = trimmed.split(',')
                        # parts = ['HELLO', side, boot, session]
                        if len(parts) == 4:
                            side_h, boot_h, sess_h = parts[1], parts[2], parts[3]
                            boot_id = int(boot_h, 16)
                            sess_id = int(sess_h, 16)
                            hello_boot_id = boot_id
                            hello_session_id = sess_id
                            print(
                                f'[TCP] [{addr_str}] session HELLO: '
                                f'side={side_h} boot=0x{boot_id:08x} '
                                f'session=0x{sess_id:08x}'
                            )
                        else:
                            boot_id = None
                            sess_id = None
                            print(
                                f'[TCP] [{addr_str}] session HELLO: '
                                f'{trimmed} (unparsed)'
                            )

                        # Flush this connection's per-device cache. We clear
                        # *before* the next JSON frame arrives so the UI never
                        # shows a stale frame from the previous session. If
                        # device_id isn't bound yet (HELLO arrived before the
                        # first JSON), there's nothing to flush and the flush
                        # is effectively a no-op for now.
                        if device_id is not None:
                            with _lock:
                                dev = _devices.get(device_id)
                                if dev is not None and dev.conn is conn:
                                    dev.latest_data = None
                                    dev.history.clear()
                                    dev.boot_id    = boot_id
                                    dev.session_id = sess_id
                                    dev.last_seen  = time.time()
                                    print(
                                        f'[TCP] [{device_id}] session cache '
                                        f'flushed (HELLO)'
                                    )
                    except (ValueError, IndexError) as exc:
                        print(
                            f'[TCP] [{addr_str}] HELLO parse error: '
                            f'{exc!r} line={trimmed!r}'
                        )
                    continue
                # --- end heartbeat ---

                try:
                    parsed = json.loads(trimmed)
                    parsed['recv_time'] = time.strftime('%H:%M:%S')
                    parsed['recv_timestamp_ms'] = int(time.monotonic() * 1000)

                    # 外部命令帧单独存储，不参与设备注册
                    if parsed.get('type') == 'ext_cmd':
                        global _ext_cmd_seq
                        with _lock:
                            _ext_cmd_seq += 1
                            parsed['seq'] = _ext_cmd_seq
                            _ext_cmd_list.appendleft(parsed)
                        print(f'[TCP] ext_cmd from {addr_str}: {parsed.get("data")}')
                        continue

                    # 从首条传感器数据中提取 device_id
                    if device_id is None:
                        device_id = parsed.get('device', f'unknown_{addr[1]}')
                        with _lock:
                            global _connection_seq
                            _connection_seq += 1
                            # 如果该设备已有旧连接，关闭旧的
                            old_dev = _devices.get(device_id)
                            if old_dev and old_dev.connected:
                                try:
                                    old_dev.conn.close()
                                except Exception:
                                    pass
                                print(f'[TCP] [{device_id}] 旧连接已替换')
                            dev = DeviceInfo(device_id, conn, addr, _connection_seq)
                            dev.boot_id = hello_boot_id
                            dev.session_id = hello_session_id
                            _devices[device_id] = dev
                        print(f'[TCP] 设备注册: [{device_id}] from {addr_str}')

                    # 存储传感器数据
                    with _lock:
                        dev = _devices.get(device_id)
                        if dev:
                            _decorate_sensor_frame(parsed, dev)
                            dev.latest_data = parsed
                            dev.history.appendleft(parsed)
                            dev.last_seen = time.time()

                    print(f'[TCP] [{device_id}] id={parsed.get("id")}')

                    # 若帧中携带 translated_text，推入 ext_cmd 队列供实时翻译页面增量拉取
                    tr_text = parsed.get('translated_text')
                    if tr_text and isinstance(tr_text, str) and tr_text.strip():
                        with _lock:
                            _ext_cmd_seq += 1
                            _ext_cmd_list.appendleft({
                                'type': 'translated_text',
                                'data': tr_text.strip(),
                                'recv_time': parsed.get('recv_time', ''),
                                'seq': _ext_cmd_seq,
                                'device': device_id,
                            })
                        print(f'[TCP] [{device_id}] translated_text → ext_cmd seq={_ext_cmd_seq}: {tr_text.strip()}')

                except json.JSONDecodeError:
                    print(f'[TCP] JSON 解析失败: {trimmed[:80]}')

    except Exception as e:
        print(f'[TCP] [{device_id or addr_str}] 连接异常: {e}')
    finally:
        conn.close()
        if device_id:
            with _lock:
                dev = _devices.get(device_id)
                if dev and dev.conn == conn:
                    dev.connected = False
            print(f'[TCP] [{device_id}] 已断开')
        else:
            print(f'[TCP] {addr_str} 已断开 (未注册)')


def _server_loop():
    """TCP 服务器主循环"""
    global _running

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.settimeout(1.0)

    try:
        server_sock.bind((TCP_HOST, TCP_PORT))
        server_sock.listen(MAX_CLIENTS)
        print(f'[TCP] 服务已启动，监听端口 {TCP_PORT}，支持 {MAX_CLIENTS} 个设备连接')

        while _running:
            try:
                conn, addr = server_sock.accept()
                conn.settimeout(5.0)
                t = threading.Thread(target=_handle_client, args=(conn, addr),
                                     daemon=True)
                t.start()
            except socket.timeout:
                continue
    except Exception as e:
        print(f'[TCP] 服务器错误: {e}')
    finally:
        server_sock.close()
        print('[TCP] 服务已停止')


def start_tcp_server():
    """启动 TCP 服务器（后台线程）"""
    global _server_thread, _running

    if _running:
        return

    _running = True
    _server_thread = threading.Thread(target=_server_loop, daemon=True,
                                       name='tcp_server')
    _server_thread.start()


def stop_tcp_server():
    """停止 TCP 服务器"""
    global _running
    _running = False
