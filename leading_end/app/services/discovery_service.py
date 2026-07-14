"""
PC UDP Discovery Service
每1秒广播一次 ARTPI_PC,1,<tcp_port>\n 到 255.255.255.255:9108，
开发板通过 recvfrom() 来源地址自动发现 PC IP。
支持热点重连后 IP 变化时自动重建 socket，不引入双进程问题。

网络接口选择策略:
  - 不绑定到特定 IP（不依赖 netifaces 等第三方库）
  - 创建 UDP socket 后 connect() 到 1.1.1.1:1（触发路由表查找），
    然后 getsockname() 得到本机分配给该默认路由接口的 IP
  - 当且仅当该 IP 与上次记录的 IP 不同时，才重建 socket 并重新 connect
  - 绑定到 0.0.0.0 让操作系统将广播放到正确的物理接口

这在同时存在以太网/VMware/Docker 等多个网络时仍能工作，
因为 connect() 到 1.1.1.1 永远走默认路由。
"""

import socket
import threading
import time
import logging
import ipaddress
import urllib.request
import urllib.error

TCP_PORT = 9109
UDP_PORT = 9108
BROADCAST_MSG = f"ARTPI_PC,1,{TCP_PORT}\n".encode('ascii')
LIMITED_BROADCAST_ADDR = ('255.255.255.255', UDP_PORT)
CHECK_INTERVAL = 1.0  # 秒
NO_IP_RETRY_SEC = 5
IPIFY_TIMEOUT_SEC = 4  # ipify API 超时
UNICAST_SWEEP_INTERVAL = 2.0  # 广播被热点过滤时，扫描当前 /24 子网

_log = logging.getLogger('discovery')


class _DiscoveryService:
    """幂等单例，管理 UDP 广播 daemon 线程。"""

    def __init__(self):
        self._lock = threading.Lock()
        self._thread = None
        self._running = False
        self._sock = None      # 当前广播 socket
        self._sock_ip = None   # socket 绑定到的本地 IP（来自 getsockname）
        self._last_log_startup = 0.0
        self._last_log_ip_change = 0.0
        self._last_log_error = 0.0
        self._log_startup_done = False
        self._last_unicast_sweep = 0.0

    # ---- public API ----

    def start(self):
        with self._lock:
            if self._running:
                return
            self._running = True
            self._thread = threading.Thread(target=self._loop, daemon=True,
                                            name='pc_discovery')
            self._thread.start()
            self._log_startup_done = False

    def stop(self):
        with self._lock:
            if not self._running:
                return
            self._running = False
            if self._sock:
                try:
                    self._sock.close()
                except Exception:
                    pass
                self._sock = None
        t = self._thread
        if t:
            t.join(timeout=2.0)

    # ---- internal ----

    def _log_startup(self, msg, *args):
        """启动/恢复日志：每 30s 至多一次。"""
        now = time.monotonic()
        if now - self._last_log_startup >= 30.0:
            self._last_log_startup = now
            if args:
                _log.info(msg % args)
            else:
                _log.info(msg)

    def _log_ip_change(self, old_ip, new_ip):
        """IP 变化日志：每 30s 至多一次。"""
        now = time.monotonic()
        if now - self._last_log_ip_change >= 30.0:
            self._last_log_ip_change = now
            _log.info('PC broadcast IP changed: %s -> %s', old_ip, new_ip)

    def _log_error(self, msg, *args):
        """错误日志：立即输出，但同样限制频率。"""
        now = time.monotonic()
        if now - self._last_log_error >= 5.0:
            self._last_log_error = now
            _log.error(msg, *args)

    def _detect_local_ip(self):
        """
        通过 connect+getsockname 找到分配给默认路由接口的 IP。

        connect(('1.1.1.1', 1)) 让系统查路由表：
        - 若手机热点是默认路由 → 得到热点的局域网 IP
        - 若以太网/VPN 是默认路由 → 得到对应 IP
        - VMware/Docker 虚拟接口一般不是默认路由，不会被选中
        """
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(4.0)
            s.connect(('1.1.1.1', 1))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception as e:
            _log.warning('local IP detection failed: %s', e)
            return None

    def _ensure_socket_for_ip(self, local_ip):
        """
        确保有一个绑定到 0.0.0.0 的 UDP socket，
        其 send() 走 local_ip 对应的物理接口。
        若 IP 变化或 socket 为空则重建。
        """
        if self._sock is not None and self._sock_ip == local_ip:
            return True  # socket 仍适用于当前 IP

        # 关闭旧 socket
        if self._sock is not None:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None
            self._sock_ip = None

        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            # 绑定到 local_ip，确保广播走正确的物理接口
            s.bind((local_ip, 0))
            self._sock = s
            self._sock_ip = local_ip
            return True
        except Exception as e:
            self._log_error('socket rebuild failed: %s', e)
            return False

    @staticmethod
    def _broadcast_targets(local_ip):
        """Return limited broadcast plus the common /24 hotspot broadcast.

        Some Windows WiFi/hotspot paths do not forward 255.255.255.255 even
        though peer-to-peer unicast works. Phone hotspots used by this project
        allocate /24 networks, so also send to the directed x.y.z.255 address.
        """
        directed = str(ipaddress.ip_network(f'{local_ip}/24', strict=False)
                       .broadcast_address)
        return (LIMITED_BROADCAST_ADDR, (directed, UDP_PORT))

    @staticmethod
    def _unicast_targets(local_ip):
        """Yield every peer address in the current phone-hotspot /24.

        Several phone hotspots permit client-to-client unicast but filter all
        broadcast frames. A small periodic UDP sweep keeps discovery automatic
        in that environment. The packet carries no IP; firmware still trusts
        only recvfrom()'s source address.
        """
        network = ipaddress.ip_network(f'{local_ip}/24', strict=False)
        for host in network.hosts():
            host_ip = str(host)
            if host_ip != local_ip:
                yield (host_ip, UDP_PORT)

    def _loop(self):
        """Daemon 线程主循环：每秒广播一次，IP 变化时重建 socket。"""
        while self._running:
            local_ip = self._detect_local_ip()

            if local_ip is None:
                self._log_startup('[discovery] no routable IPv4, waiting %ds...',
                                  NO_IP_RETRY_SEC)
                time.sleep(NO_IP_RETRY_SEC)
                continue

            # IP 变化或 socket 未建立 → 重建
            if local_ip != self._sock_ip or self._sock is None:
                old_ip = self._sock_ip or '(none)'
                if self._ensure_socket_for_ip(local_ip):
                    self._log_ip_change(old_ip, local_ip)

            sock = self._sock
            if sock is None:
                time.sleep(1.0)
                continue

            try:
                for target in self._broadcast_targets(local_ip):
                    sock.sendto(BROADCAST_MSG, target)

                now = time.monotonic()
                if now - self._last_unicast_sweep >= UNICAST_SWEEP_INTERVAL:
                    self._last_unicast_sweep = now
                    for target in self._unicast_targets(local_ip):
                        sock.sendto(BROADCAST_MSG, target)
            except OSError as e:
                self._log_error('send error: %s, will retry with fresh socket', e)
                self._sock_ip = None  # 触发下一次重建
            except Exception as e:
                self._log_error('unexpected send error: %s', e)

            time.sleep(CHECK_INTERVAL)


# ---- module-level幂等接口 ----

_instance = _DiscoveryService()


def start_discovery_service():
    _instance.start()


def stop_discovery_service():
    _instance.stop()
