from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import math
import textwrap


W, H = 2400, 1550
BG = (248, 248, 246)
GROUP_FILL = (255, 250, 214)
GROUP_OUTLINE = (214, 204, 126)
BOX_FILL = (246, 241, 255)
BOX_OUTLINE = (156, 143, 196)
TEXT = (46, 46, 54)
LINE = (88, 88, 108)
ACCENT = (54, 105, 201)
MUTED = (120, 120, 120)


def load_font(size: int, bold: bool = False):
    candidates = []
    if bold:
        candidates += [
            r"C:\Windows\Fonts\msyhbd.ttc",
            r"C:\Windows\Fonts\simhei.ttf",
        ]
    candidates += [
        r"C:\Windows\Fonts\msyh.ttc",
        r"C:\Windows\Fonts\simsun.ttc",
        r"C:\Windows\Fonts\simhei.ttf",
        r"C:\Windows\Fonts\arial.ttf",
    ]
    for path in candidates:
        if Path(path).exists():
            return ImageFont.truetype(path, size=size)
    return ImageFont.load_default()


FONT_TITLE = load_font(40, bold=True)
FONT_GROUP = load_font(28, bold=True)
FONT_BOX = load_font(24, bold=True)
FONT_BODY = load_font(20, bold=False)
FONT_SMALL = load_font(18, bold=False)


img = Image.new("RGB", (W, H), BG)
draw = ImageDraw.Draw(img)


def draw_group(x1, y1, x2, y2, title):
    draw.rounded_rectangle((x1, y1, x2, y2), radius=18, fill=GROUP_FILL, outline=GROUP_OUTLINE, width=3)
    tb = draw.textbbox((0, 0), title, font=FONT_GROUP)
    tw = tb[2] - tb[0]
    th = tb[3] - tb[1]
    label_w = tw + 30
    draw.rounded_rectangle((x1 + 20, y1 - th // 2 - 6, x1 + 20 + label_w, y1 + th // 2 + 10),
                           radius=10, fill=BG, outline=GROUP_OUTLINE, width=2)
    draw.text((x1 + 35, y1 - th // 2 + 1), title, fill=TEXT, font=FONT_GROUP)


def draw_box(x, y, w, h, title, body=None):
    draw.rounded_rectangle((x, y, x + w, y + h), radius=14, fill=BOX_FILL, outline=BOX_OUTLINE, width=2)
    tb = draw.textbbox((0, 0), title, font=FONT_BOX)
    th = tb[3] - tb[1]
    draw.text((x + 18, y + 12), title, fill=TEXT, font=FONT_BOX)
    if body:
        lines = []
        for paragraph in body:
            lines.extend(textwrap.wrap(paragraph, width=19))
        yy = y + 20 + th + 10
        for line in lines:
            draw.text((x + 18, yy), line, fill=TEXT, font=FONT_BODY)
            yy += 27


def center_of(rect):
    x, y, w, h = rect
    return (x + w / 2, y + h / 2)


def edge_point(rect, side):
    x, y, w, h = rect
    if side == "left":
        return (x, y + h / 2)
    if side == "right":
        return (x + w, y + h / 2)
    if side == "top":
        return (x + w / 2, y)
    return (x + w / 2, y + h)


def draw_arrow(points, color=LINE, width=4, dashed=False, label=None, label_pos=0.5):
    if dashed:
        dash = 14
        gap = 10
        for i in range(len(points) - 1):
            x1, y1 = points[i]
            x2, y2 = points[i + 1]
            dx, dy = x2 - x1, y2 - y1
            dist = math.hypot(dx, dy)
            if dist == 0:
                continue
            ux, uy = dx / dist, dy / dist
            t = 0
            while t < dist:
                s = t
                e = min(t + dash, dist)
                draw.line((x1 + ux * s, y1 + uy * s, x1 + ux * e, y1 + uy * e), fill=color, width=width)
                t += dash + gap
    else:
        draw.line(points, fill=color, width=width)

    x1, y1 = points[-2]
    x2, y2 = points[-1]
    ang = math.atan2(y2 - y1, x2 - x1)
    ah = 16
    aw = 8
    p1 = (x2, y2)
    p2 = (x2 - ah * math.cos(ang) + aw * math.sin(ang), y2 - ah * math.sin(ang) - aw * math.cos(ang))
    p3 = (x2 - ah * math.cos(ang) - aw * math.sin(ang), y2 - ah * math.sin(ang) + aw * math.cos(ang))
    draw.polygon([p1, p2, p3], fill=color)

    if label:
        total = 0
        segs = []
        for i in range(len(points) - 1):
            d = math.hypot(points[i + 1][0] - points[i][0], points[i + 1][1] - points[i][1])
            segs.append(d)
            total += d
        target = total * label_pos
        acc = 0
        lx, ly = points[0]
        for i, d in enumerate(segs):
            if acc + d >= target:
                t = (target - acc) / d if d else 0
                lx = points[i][0] + (points[i + 1][0] - points[i][0]) * t
                ly = points[i][1] + (points[i + 1][1] - points[i][1]) * t
                break
            acc += d
        bb = draw.textbbox((0, 0), label, font=FONT_SMALL)
        pad = 6
        draw.rounded_rectangle((lx - (bb[2]-bb[0])/2 - pad, ly - 14, lx + (bb[2]-bb[0])/2 + pad, ly + 14),
                               radius=8, fill=(255, 255, 255), outline=(210, 210, 210))
        draw.text((lx - (bb[2]-bb[0])/2, ly - 11), label, fill=color, font=FONT_SMALL)


draw.text((W // 2 - 420, 30), "双手可穿戴手语采集与双向语音互译系统 当前架构结构框图", fill=TEXT, font=FONT_TITLE)
draw.text((W // 2 - 270, 84), "主链路：WiFi/TCP 到 ROCK 5B；辅助链路：.217 调试/监控/STT 代理", fill=MUTED, font=FONT_BODY)

# Groups
draw_group(70, 150, 760, 980, "左手可穿戴端 ART-Pi2")
draw_group(1640, 150, 2330, 980, "右手可穿戴端 ART-Pi2")
draw_group(895, 170, 1505, 935, "ROCK 5B 边缘端 Linux")
draw_group(620, 1120, 1780, 1480, "PC / 辅助服务（192.168.221.217）")

# Left side boxes
left_boxes = {
    "sensors": (110, 210, 250, 120),
    "mode":    (110, 390, 250, 150),
    "wifi":    (110, 610, 250, 150),
    "tts":     (430, 260, 270, 120),
    "ui":      (430, 470, 270, 120),
    "tcp":     (430, 690, 270, 150),
}

draw_box(*left_boxes["sensors"], "11路 IMU 采集", ["10× MPU6050", "1× ICM-20948"])
draw_box(*left_boxes["mode"], "operation_mode", ["AUTO / MANUAL", "AUTO_STANDBY /", "MANUAL_SLEEP / RUNNING /", "WAITING_STOP"])
draw_box(*left_boxes["wifi"], "imu_wifi_sender", ["[DATA] CSV 上行", "90ms, left, frame_seq", "接收 MODE/CMD/SAY", "发送 MODEL / WAITING_STOP"])
draw_box(*left_boxes["tts"], "VTX316 语音播报", ["接收 SAY: 文本", "vtx316_speak_wait()"])
draw_box(*left_boxes["ui"], "本地交互", ["PC6 按键切模式", "OLED / 电池状态"])
draw_box(*left_boxes["tcp"], "tcp_client JSON 辅助上报", ["发送状态 / translated_text", "默认 .217:8266"])

# Right side boxes
right_boxes = {
    "sensors": (1680, 210, 250, 120),
    "mode":    (1680, 390, 250, 150),
    "wifi":    (1680, 610, 250, 150),
    "stt":     (2000, 240, 280, 170),
    "ui":      (2000, 470, 280, 120),
    "tcp":     (2000, 690, 280, 150),
}

draw_box(*right_boxes["sensors"], "11路 IMU 采集", ["10× MPU6050", "1× ICM-20948"])
draw_box(*right_boxes["mode"], "operation_mode", ["AUTO / MANUAL", "AUTO_STANDBY /", "MANUAL_SLEEP / RUNNING /", "WAITING_STOP"])
draw_box(*right_boxes["wifi"], "imu_wifi_sender", ["[DATA] CSV 上行", "90ms, right, frame_seq", "接收 MODE/CMD", "发送 MODEL / WAITING_STOP"])
draw_box(*right_boxes["stt"], "右手语音链路", ["INMP441 采集", "VAD / audio_process", "ai_cloud_service", "HTTP -> .217:8080/stt"])
draw_box(*right_boxes["ui"], "本地显示", ["OLED 显示 STT 文本", "状态提示"])
draw_box(*right_boxes["tcp"], "tcp_client / server_config", ["默认 .217:8266 / 8080", "辅助监控与参数配置"])

# ROCK boxes
rock_boxes = {
    "rx":    (950, 250, 500, 130),
    "pair":  (950, 450, 500, 120),
    "infer": (950, 640, 500, 150),
    "ctrl":  (950, 840, 500, 70),
}

draw_box(*rock_boxes["rx"], "WiFi/TCP 接收器", ["left  -> .239:9101", "right -> .239:9102"])
draw_box(*rock_boxes["pair"], "双手配对与缓存", ["hand + frame_seq 对齐", "窗口切分 / 缓冲"])
draw_box(*rock_boxes["infer"], "识别与语义层", ["RKNN 手语识别", "avg_energy 后处理", "DeepSeek 连词成句"])
draw_box(*rock_boxes["ctrl"], "命令与回传", ["下发 MODE/CMD/SAY，接收 MODEL / WAITING_STOP"])

# PC boxes
pc_boxes = {
    "tcp": (700, 1185, 420, 150),
    "stt": (1220, 1185, 420, 150),
    "ui":  (960, 1370, 420, 80),
}
draw_box(*pc_boxes["tcp"], "辅助 TCP 服务", ["leading_end tcp_server", "0.0.0.0:8266", "状态 / translated_text / 调试"])
draw_box(*pc_boxes["stt"], "STT 代理服务", ["xfyun proxy", "0.0.0.0:8080/stt", "右手音频 -> 识别文本"])
draw_box(*pc_boxes["ui"], "前端 / 调试界面", ["Flask / 实时监控 / 命令下发"])

# Internal arrows left
draw_arrow([edge_point(left_boxes["sensors"], "bottom"), edge_point(left_boxes["mode"], "top")], label="姿态粗判")
draw_arrow([edge_point(left_boxes["mode"], "bottom"), edge_point(left_boxes["wifi"], "top")], label="RUNNING门控")
draw_arrow([edge_point(left_boxes["ui"], "left"), (360, 530), edge_point(left_boxes["mode"], "right")], label="本地切模式", label_pos=0.25)
draw_arrow([edge_point(left_boxes["wifi"], "top"), (235, 560), edge_point(left_boxes["mode"], "bottom")], color=MUTED, width=3, label="状态反馈", label_pos=0.55)

# Internal arrows right
draw_arrow([edge_point(right_boxes["sensors"], "bottom"), edge_point(right_boxes["mode"], "top")], label="姿态粗判")
draw_arrow([edge_point(right_boxes["mode"], "bottom"), edge_point(right_boxes["wifi"], "top")], label="RUNNING门控")
draw_arrow([edge_point(right_boxes["stt"], "bottom"), (2140, 450), edge_point(right_boxes["ui"], "top")], label="识别结果")

# Main WiFi/TCP arrows to ROCK
draw_arrow([edge_point(left_boxes["wifi"], "right"), (760, 685), (880, 315), edge_point(rock_boxes["rx"], "left")],
           color=ACCENT, width=5, label="[DATA] left CSV 90ms", label_pos=0.52)
draw_arrow([edge_point(right_boxes["wifi"], "left"), (1640, 685), (1520, 315), edge_point(rock_boxes["rx"], "right")],
           color=ACCENT, width=5, label="[DATA] right CSV 90ms", label_pos=0.48)

# ROCK downlink commands
draw_arrow([edge_point(rock_boxes["ctrl"], "left"), (880, 875), (760, 760), edge_point(left_boxes["wifi"], "right")],
           color=(180, 72, 72), width=4, label="MODE / CMD / SAY", label_pos=0.45)
draw_arrow([edge_point(rock_boxes["ctrl"], "right"), (1520, 875), (1640, 760), edge_point(right_boxes["wifi"], "left")],
           color=(180, 72, 72), width=4, label="MODE / CMD", label_pos=0.45)

# SAY to left TTS
draw_arrow([(1450, 875), (1530, 875), (1530, 320), (700, 320), edge_point(left_boxes["tts"], "right")],
           color=(180, 72, 72), width=3, label="SAY: 文本播报", label_pos=0.62)

# Auxiliary uplink MODEL / WAITING_STOP
draw_arrow([edge_point(left_boxes["wifi"], "bottom"), (235, 925), (1140, 925), edge_point(rock_boxes["ctrl"], "left")],
           color=(78, 132, 78), width=3, label="MODEL / WAITING_STOP:left", label_pos=0.68)
draw_arrow([edge_point(right_boxes["wifi"], "bottom"), (1805, 925), (1310, 925), edge_point(rock_boxes["ctrl"], "right")],
           color=(78, 132, 78), width=3, label="MODEL / WAITING_STOP:right", label_pos=0.35)

# ROCK internal flow
draw_arrow([edge_point(rock_boxes["rx"], "bottom"), edge_point(rock_boxes["pair"], "top")], label="配对")
draw_arrow([edge_point(rock_boxes["pair"], "bottom"), edge_point(rock_boxes["infer"], "top")], label="窗口化")
draw_arrow([edge_point(rock_boxes["infer"], "bottom"), edge_point(rock_boxes["ctrl"], "top")], label="翻译结果")

# Auxiliary dashed links to PC services
draw_arrow([edge_point(left_boxes["tcp"], "bottom"), (565, 840), (565, 1260), edge_point(pc_boxes["tcp"], "left")],
           color=MUTED, width=3, dashed=True, label="JSON / 状态", label_pos=0.78)
draw_arrow([edge_point(right_boxes["tcp"], "bottom"), (2140, 860), (2140, 1260), edge_point(pc_boxes["tcp"], "right")],
           color=MUTED, width=3, dashed=True, label="调试链路", label_pos=0.78)
draw_arrow([edge_point(right_boxes["stt"], "bottom"), (2140, 470), (2140, 1260), edge_point(pc_boxes["stt"], "right")],
           color=MUTED, width=3, dashed=True, label="PCM -> /stt", label_pos=0.74)

# legend
legend_x, legend_y = 70, 980
draw.rounded_rectangle((legend_x, legend_y, legend_x + 650, legend_y + 110), radius=12,
                       fill=(255, 255, 255), outline=(210, 210, 210), width=2)
draw.text((legend_x + 18, legend_y + 12), "图例", fill=TEXT, font=FONT_BOX)
draw.line((legend_x + 90, legend_y + 35, legend_x + 190, legend_y + 35), fill=ACCENT, width=5)
draw.polygon([(190, legend_y + 35), (174, legend_y + 27), (174, legend_y + 43)], fill=ACCENT)
draw.text((legend_x + 210, legend_y + 21), "主链路：双手 IMU WiFi/TCP <-> ROCK", fill=TEXT, font=FONT_SMALL)
draw_arrow([(legend_x + 90, legend_y + 75), (legend_x + 190, legend_y + 75)], color=MUTED, width=3, dashed=True)
draw.text((legend_x + 210, legend_y + 61), "辅助链路：.217 调试监控 / STT 代理", fill=TEXT, font=FONT_SMALL)

out = Path(__file__).resolve().parents[1] / "current_architecture_wifi_tcp.png"
img.save(out, "PNG")
print(out)
