#!/usr/bin/env python3
"""
讯飞语音识别 HTTP 代理服务器
============================

用途：为不支持 WebSocket/TLS 的嵌入式设备（如 STM32）提供 HTTP 接口代理。

工作流程：
  STM32 ---(HTTP POST /stt)---> 本脚本 ---(WebSocket+TLS)---> 讯飞 IAT v2 API

使用方法：
  1. pip install websocket-client
  2. python xfyun_proxy.py
  3. 在开发板 MSH 中运行：
       set_stt_ip <本电脑的 WLAN IP>
       va_reload_stt
     STT URL 由 voice_assistant.c 在运行时动态拼装为
       http://<stt_ip>:8080/stt
     因此无需修改固件源码或重新烧录。

依赖：
  pip install websocket-client

注意：确保电脑和开发板在同一局域网内！
"""

import json
import time
import hashlib
import hmac
import base64
import struct
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, urlencode
from datetime import datetime
from wsgiref.handlers import format_date_time
from time import mktime
import ssl
import websocket
import threading

# ========== 讯飞 API 配置 ==========
# 与 voice_assistant_config.h 中保持一致
APPID = "c050bb08"
API_KEY = "c934cd65a7a2da392cf718c1b7687b11"
API_SECRET = "ZTg4YzEwM2M0ZTIzMzIzZjE1ZmFmYWU5"

# 讯飞 IAT WebSocket 地址
IAT_HOST = "iat-api.xfyun.cn"
IAT_PATH = "/v2/iat"

# 代理服务配置
PROXY_HOST = "0.0.0.0"  # 监听所有网卡
PROXY_PORT = 8080


def create_auth_url():
    """生成讯飞 IAT v2 鉴权 URL"""
    now = datetime.now()
    date = format_date_time(mktime(now.timetuple()))

    # 拼接签名原文
    signature_origin = (
        f"host: {IAT_HOST}\n"
        f"date: {date}\n"
        f"GET {IAT_PATH} HTTP/1.1"
    )

    # HMAC-SHA256 签名
    signature_sha = hmac.new(
        API_SECRET.encode('utf-8'),
        signature_origin.encode('utf-8'),
        digestmod=hashlib.sha256
    ).digest()
    signature = base64.b64encode(signature_sha).decode('utf-8')

    # 构造 authorization
    authorization_origin = (
        f'api_key="{API_KEY}", '
        f'algorithm="hmac-sha256", '
        f'headers="host date request-line", '
        f'signature="{signature}"'
    )
    authorization = base64.b64encode(authorization_origin.encode('utf-8')).decode('utf-8')

    # 构造完整 URL
    params = {
        "authorization": authorization,
        "date": date,
        "host": IAT_HOST
    }
    url = f"wss://{IAT_HOST}{IAT_PATH}?{urlencode(params)}"
    return url


def recognize_audio(audio_data):
    """
    通过讯飞 IAT v2 WebSocket 接口识别语音

    参数：
        audio_data: bytes, 16kHz 16bit 单声道 PCM 原始音频

    返回：
        str: 识别出的文本，失败返回 None
    """
    result_text = []
    ws_finished = threading.Event()
    ws_error = [None]

    def on_message(ws, message):
        try:
            data = json.loads(message)
            code = data.get("code", -1)
            print(f"[WS] Received: code={code}, message={data.get('message', '')}")
            if code != 0:
                ws_error[0] = f"Error code: {code}, msg: {data.get('message', '')}"
                ws_finished.set()
                return

            result = data.get("data", {}).get("result", {})
            if result:
                # 拼接识别结果
                ws_list = result.get("ws", [])
                for ws_item in ws_list:
                    for cw in ws_item.get("cw", []):
                        w = cw.get("w", "")
                        print(f"[WS] Word: '{w}'")
                        result_text.append(w)

            # 检查是否是最后一帧
            status = data.get("data", {}).get("status", 0)
            print(f"[WS] Data status: {status}")
            if status == 2:
                ws_finished.set()
        except Exception as e:
            ws_error[0] = str(e)
            print(f"[WS] Exception: {e}")
            ws_finished.set()

    def on_error(ws, error):
        ws_error[0] = str(error)
        ws_finished.set()

    def on_close(ws, close_status_code, close_msg):
        ws_finished.set()

    def on_open(ws):
        """连接建立后，分帧发送音频数据"""
        def send_audio():
            try:
                frame_size = 8000  # 每帧 8000 字节 (~250ms at 16kHz/16bit)
                total = len(audio_data)
                offset = 0
                status = 0  # 0=第一帧, 1=中间帧, 2=最后一帧

                while offset < total:
                    end = min(offset + frame_size, total)
                    chunk = audio_data[offset:end]

                    if offset == 0:
                        status = 0  # 第一帧
                    elif end >= total:
                        status = 2  # 最后一帧
                    else:
                        status = 1  # 中间帧

                    audio_b64 = base64.b64encode(chunk).decode('utf-8')

                    frame = {
                        "data": {
                            "status": status,
                            "format": "audio/L16;rate=16000",
                            "encoding": "raw",
                            "audio": audio_b64
                        }
                    }

                    # 第一帧需要包含 common 和 business 参数
                    if status == 0:
                        frame["common"] = {"app_id": APPID}
                        frame["business"] = {
                            "language": "zh_cn",
                            "domain": "iat",
                            "accent": "mandarin",
                            "vad_eos": 800,   # 静音检测 800ms（STM32 已做 VAD，无需长等待）
                            "ptt": 0          # 不加标点（嵌入式场景简化）
                        }

                    ws.send(json.dumps(frame))
                    offset = end

                    # 控制发送速率（缩短间隔加快传输）
                    if status != 2:
                        time.sleep(0.01)

            except Exception as e:
                ws_error[0] = str(e)
                ws_finished.set()

        threading.Thread(target=send_audio, daemon=True).start()

    # 创建 WebSocket 连接
    url = create_auth_url()

    ws = websocket.WebSocketApp(
        url,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
        on_open=on_open
    )

    # 启动 WebSocket（在子线程中运行）
    ws_thread = threading.Thread(
        target=ws.run_forever,
        kwargs={"sslopt": {"cert_reqs": ssl.CERT_NONE}},
        daemon=True
    )
    ws_thread.start()

    # 等待识别完成（最多30秒）
    ws_finished.wait(timeout=30)
    ws.close()

    if ws_error[0]:
        print(f"[ERROR] WebSocket error: {ws_error[0]}")
        return None

    text = "".join(result_text).strip()
    return text if text else None


class ProxyHandler(BaseHTTPRequestHandler):
    """HTTP 代理请求处理"""

    def log_message(self, format, *args):
        """自定义日志格式"""
        print(f"[{time.strftime('%H:%M:%S')}] {args[0]}")

    def do_POST(self):
        if self.path == "/stt":
            self.handle_stt()
        elif self.path == "/tts":
            self.handle_tts()
        else:
            self.send_error(404, "Not Found")

    def handle_stt(self):
        """处理语音识别请求"""
        content_length = int(self.headers.get('Content-Length', 0))
        if content_length == 0:
            self.send_json(400, {"error": "No data"})
            return

        # 循环读取确保收全所有数据（STM32 分块发送时可能多次到达）
        body = b''
        remaining = content_length
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 65536))
            if not chunk:
                break
            body += chunk
            remaining -= len(chunk)

        content_type = self.headers.get('Content-Type', '')

        print(f"[STT] Received {content_length} bytes, Content-Type: {content_type}")

        # 解析请求
        audio_data = None

        if 'application/json' in content_type:
            try:
                req = json.loads(body)
                # 支持讯飞格式的 JSON 请求
                audio_b64 = None
                if 'data' in req and 'audio' in req['data']:
                    audio_b64 = req['data']['audio']
                elif 'audio' in req:
                    audio_b64 = req['audio']
                elif 'speech' in req:
                    audio_b64 = req['speech']

                if audio_b64:
                    audio_data = base64.b64decode(audio_b64)
                    print(f"[STT] Decoded audio from JSON: {len(audio_data)} bytes")
                else:
                    print("[STT] No audio field found in JSON")
                    self.send_json(400, {"error": "No audio field in JSON"})
                    return
            except json.JSONDecodeError:
                print("[STT] Invalid JSON, treating as raw PCM")
                audio_data = body
        else:
            # 原始 PCM 数据
            audio_data = body
            print(f"[STT] Raw PCM data: {len(audio_data)} bytes")

        if audio_data is None or len(audio_data) < 100:
            self.send_json(400, {"error": "Audio data too short"})
            return

        # 调用讯飞 API
        print(f"[STT] Sending to iFlytek IAT... ({len(audio_data)} bytes)")
        text = recognize_audio(audio_data)

        if text:
            print(f"[STT] Result: {text}")
            self.send_json(200, {"result": [text]})
        else:
            print("[STT] No result")
            self.send_json(200, {"result": [""]})

    def handle_tts(self):
        """处理语音合成请求 (简化版 - 返回提示)"""
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)

        print(f"[TTS] Received request: {body[:200]}")
        # TTS 功能可后续扩展
        self.send_json(501, {"error": "TTS not implemented yet"})

    def send_json(self, status_code, data):
        """发送 JSON 响应"""
        response = json.dumps(data, ensure_ascii=False)
        self.send_response(status_code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(response.encode('utf-8'))))
        self.end_headers()
        self.wfile.write(response.encode('utf-8'))


def main():
    import socket

    # 获取本机 IP
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
    except Exception:
        local_ip = "127.0.0.1"
    finally:
        s.close()

    print("=" * 50)
    print("  讯飞语音识别 HTTP 代理服务器")
    print("=" * 50)
    print(f"  APPID:     {APPID}")
    print(f"  API_KEY:   {API_KEY[:8]}...")
    print(f"  监听地址:  http://{local_ip}:{PROXY_PORT}")
    print()
    print("  在开发板 MSH 中运行以下命令，把 STT 目标指向本机:")
    print(f"    set_stt_ip {local_ip}")
    print(f"    va_reload_stt")
    print("  完成后，固件将以 http://{0}:{1}/stt 作为 STT URL。".format(local_ip, PROXY_PORT))
    print("=" * 50)
    print()
    print("等待 STM32 请求...")
    print()

    server = HTTPServer((PROXY_HOST, PROXY_PORT), ProxyHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
        server.server_close()


if __name__ == "__main__":
    main()
