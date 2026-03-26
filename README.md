# 基于RK3588与ART-PI II、ESP32S3的边缘端双向手语翻译系统

## 项目简介

本项目是一套完整的 **双向手语翻译系统**，涵盖嵌入式数据采集端和Web可视化后端，实现手势数据的实时采集、无线传输、3D骨骼模型可视化与AI辅助翻译。

系统由三大核心模块组成：

| 模块 | 目录 | 说明 |
|------|------|------|
| 左手数据采集端 | `art_pi2_left/` | ART-Pi2 开发板，采集左手11路IMU传感器数据，WiFi TCP上传 |
| 右手数据采集端 | `art_pi2_right/` | ART-Pi2 开发板，采集右手11路IMU传感器数据，附加语音助手/麦克风模块 |
| Web可视化后端 | `leading_end/` | Flask + Three.js + Layui，实时3D手部骨骼可视化与手语翻译 |

---

## 系统架构

```
┌──────────────────┐     WiFi TCP (JSON)     ┌──────────────────────────────┐
│  左手手套 (LEFT)  │ ──── port 8266 ────────→│                              │
│  ART-Pi2 + IMU×11│                         │   Flask Web Server           │
│  + OLED + VTX316 │                         │   (Python + Three.js)        │
└──────────────────┘                         │                              │
                                             │  ┌─ 实时翻译 (3D骨骼模型)    │
┌──────────────────┐     WiFi TCP (JSON)     │  ├─ 设备数据监控             │
│  右手手套 (RIGHT) │ ──── port 8266 ────────→│  ├─ 通道映射配置             │
│  ART-Pi2 + IMU×11│                         │  ├─ AI引擎管理               │
│  + 麦克风 + 语音  │                         │  └─ 界面管理                 │
└──────────────────┘                         └──────────────────────────────┘
```

---

## 硬件平台

### MCU规格

- **芯片**: STM32H750XBHX (STM32H7RS系列, ARM Cortex-M7)
- **主频**: 480 MHz
- **RAM**: 456 KB (AXI SRAM)
- **Flash**: 外部XSPI2 Flash (131 MB+)
- **开发板**: ART-Pi2
- **RTOS**: RT-Thread v5.1.0

### 传感器配置 (每只手)

通过双 **TCA9548A** I2C多路复用器连接 **11路IMU传感器**：

| 通道 | I2C总线 | TCA通道 | 传感器 | 数据维度 |
|------|---------|---------|--------|----------|
| CH0~CH7 | I2C1 (PB8/PB9) | TCA#1 CH0~CH7 | 8× MPU6050 | 6-DOF (ax,ay,az,gx,gy,gz) |
| CH8~CH9 | I2C2 (PE1/PE2) | TCA#2 CH0~CH1 | 2× MPU6050 | 6-DOF (ax,ay,az,gx,gy,gz) |
| CH10 | I2C2 (PE1/PE2) | TCA#2 CH2 | 1× ICM-20948 | 9-DOF (+mx,my,mz磁力计) |

### 引脚分配

```
I2C1 (PB8-SCL, PB9-SDA) → TCA9548A #1 (0x70) → 8× MPU6050
I2C2 (PE1-SCL, PE2-SDA) → TCA9548A #2 (0x70) → 2× MPU6050 + ICM-20948 + OLED
UART1 (PF13-TX, PF12-RX) ← 外部串口 / VTX316语音合成
UART4 (PD0-TX, PD1-RX)   → 控制台 (MSH Shell, 115200 baud)
ADC1 CH12 (PC2)           ← 锂电池分压 (2× 1KΩ, 比例1:2)
GPIO PO5                  → LED心跳指示灯
WiFi                      → 板载CYW43438模块
```

---

## 嵌入式端功能

### 多线程架构

```
main()
  ├─ Thread: iic_drv   [优先级20, 栈2KB]  → I2C/OLED初始化 (一次性)
  ├─ Thread: uart_oled  [优先级18, 栈2KB]  → UART接收 + OLED滚动显示
  ├─ Thread: mpu6050    [优先级16, 栈4KB]  → 10Hz 11路IMU数据采集
  ├─ Thread: bat_adc    [优先级22, 栈1KB]  → 1Hz 电池电压ADC采样
  ├─ Thread: tcp_cli    [优先级22, 栈4KB]  → WiFi TCP JSON数据上传
  └─ Main Loop: LED心跳 (500ms)
```

### 核心功能

- **11路IMU传感器采集**: 10Hz采样率，TCA9548A通道切换，支持MPU6050(6轴)和ICM-20948(9轴)
- **WiFi TCP双向通信**: JSON格式数据实时上传至PC端（默认 192.168.6.92:8266），支持接收PC下发命令
- **锂电池监测**: ADC采集 + 8次滑动平均滤波，14级电池图标显示，范围3.0V~4.2V
- **OLED实时显示**: SH1106 128×64屏幕，状态栏(电压/电量) + 3行文本滚动显示
- **VTX316语音合成** (仅左手端): UART1驱动，支持非阻塞/阻塞式语音播报
- **语音助手模块** (仅右手端): INMP441数字麦克风 + VAD语音活动检测 + AI云服务对接

### 左手端 vs 右手端差异

| 功能 | 左手端 (art_pi2_left) | 右手端 (art_pi2_right) |
|------|----------------------|----------------------|
| IMU数据采集 | 11路 | 11路 |
| WiFi TCP | device: "left" | device: "right" |
| 语音合成 (VTX316) | ✅ | ❌ |
| 麦克风 (INMP441) | ❌ | ✅ |
| 语音助手 / AI云服务 | ❌ | ✅ |
| OLED显示 | ✅ | ✅ |
| 电池监测 | ✅ | ✅ |

### 传感器数据接口

```c
/* 获取指定通道传感器数据 (ch0~ch10) */
rt_err_t mpu_get_channel_data(int ch, mpu_channel_data_t *out);

/* 电池状态 */
rt_uint32_t battery_get_voltage(void);     // 返回mV
rt_uint8_t  battery_get_percentage(void);  // 返回0~100

/* TCP回调 */
void tcp_set_recv_callback(tcp_recv_callback_t callback);

/* 语音合成 (仅左手端) */
void vtx316_speak(const char *text);         // 非阻塞
rt_err_t vtx316_speak_wait(const char *text); // 阻塞等待
```

### MSH命令

```bash
# 连接WiFi
msh> wifi join <SSID> <password>

# 启动/停止TCP数据传输
msh> tcp_start [ip] [port]
msh> tcp_stop

# 查询电池
msh> bat
Battery: 3.850V  71%
```

---

## Web可视化后端

### 技术栈

- **后端**: Python 3 + Flask + SQLite
- **前端**: Layui v2.13.3 + Three.js
- **通信**: TCP Socket Server (端口8266)

### 功能页面

#### 1. 实时翻译
- Three.js渲染3D手部骨骼模型（GLB/GLTF格式）
- **Madgwick滤波器**融合加速度计+陀螺仪数据，实时驱动骨骼旋转
- 左右手双设备同时可视化
- 支持上传自定义骨骼模型
- 手势翻译结果实时显示

#### 2. 设备数据监控
- 左手/右手双面板实时数据展示
- 11通道 × 6/9轴传感器数据表格
- 设备连接状态指示
- 50ms数据刷新

#### 3. 通道映射
- 将11个传感器通道映射到3D骨骼关节
- 支持双手独立配置
- 指尖关节联动（符合人体工学）

#### 4. AI引擎管理
- 通过OpenAI API范式接入第三方大模型
- 支持多模型配置（服务商、API地址、密钥、模型名称）
- 内置聊天测试界面

#### 5. 界面管理
- 自定义导航菜单（改名、排序、显隐）

### 数据流

```
手套设备 (ART-Pi2)
    ↓ JSON over TCP:8266
Flask TCP Server (多设备管理, 线程安全)
    ↓ REST API
前端 JavaScript (50ms轮询)
    ↓ 通道映射 → 四元数旋转
Three.js 3D骨骼模型
```

### API接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/api/device/status` | GET | 设备连接状态 |
| `/api/device/latest` | GET | 最新传感器数据 |
| `/api/device/list` | GET | 已连接设备列表 |
| `/api/device/send` | POST | 向设备发送命令 |
| `/api/device/history` | GET | 历史数据查询 |
| `/api/ai-engine/*` | GET/POST | AI引擎增删改查 + 对话测试 |
| `/api/menu/*` | GET/POST | 菜单管理 |
| `/api/models/*` | GET/POST | 3D模型管理 |

---

## 项目目录结构

```
art_pi2_wifi_project/
├── art_pi2_left/                  # 左手数据采集端
│   ├── applications/              #   应用层 (main.c, tcp_client, adc_battery)
│   ├── IIC/                       #   I2C驱动 (TCA9548A, OLED, 互斥锁)
│   ├── UART/                      #   串口接收 + OLED显示
│   ├── mpu6050/                   #   IMU传感器驱动与采集线程
│   ├── vtx316/                    #   VTX316语音合成驱动
│   ├── board/                     #   板级支持 (HAL配置, 链接脚本, FAL分区)
│   ├── rt-thread/                 #   RT-Thread内核源码
│   ├── libraries/                 #   STM32 HAL库 + CMSIS
│   ├── packages/                  #   WiFi驱动 (CYW43438) + netutils
│   ├── rtconfig.h                 #   RT-Thread配置
│   └── project.uvprojx            #   Keil MDK工程文件
│
├── art_pi2_right/                 # 右手数据采集端
│   ├── applications/              #   应用层 (含语音助手, AI云服务, 麦克风驱动)
│   ├── IIC/                       #   I2C驱动
│   ├── UART/                      #   串口通信
│   ├── mpu6050/                   #   IMU传感器驱动
│   ├── board/                     #   板级支持
│   └── ...                        #   (结构类似左手端)
│
├── leading_end/                   # Web可视化后端
│   ├── app/                       #   Flask应用 (models, views, services)
│   ├── templates/                 #   Jinja2 HTML模板
│   ├── static/                    #   静态资源 (Layui, Three.js, CSS, 3D模型)
│   ├── hardware/                  #   硬件抽象层 (数据采集, 信号预处理)
│   ├── config.py                  #   Flask配置
│   ├── run.py                     #   启动入口
│   └── requirements.txt           #   Python依赖
│
├── a8c4b-main/                    # A8C4B模块参考工程
├── Unicode_GB2312_GBK_convert_table-master/  # 字符编码转换表
└── README.md                      # 本文件
```

---

## 编译与运行

### 嵌入式端

#### 环境要求

- **IDE**: Keil MDK v5 (ARM Compiler V6.22)
- **开发板**: ART-Pi2 (STM32H7S7L8H6H)
- **RTOS**: RT-Thread v5.1.0

#### 编译步骤

1. 使用 `pkgs --update` 下载在线软件包（wifi-host-driver、netutils）
2. 打开 `project.uvprojx` 工程文件
3. 编译 (Build)
4. 通过 ST-Link 下载固件到开发板

#### WiFi资源文件

如使用SD卡加载WiFi固件，需将以下文件放到SD卡根目录：

```
packages/wifi-host-driver-latest/.../resources/clm/COMPONENT_43438/43438A1.clm_blob
packages/wifi-host-driver-latest/.../resources/firmware/COMPONENT_43438/43438A1.bin
```

### Web后端

```bash
cd leading_end
pip install -r requirements.txt
python run.py
```

默认启动后访问 `http://localhost:5000`，TCP Server监听端口 `8266`。

---

## 技术要点

1. **I2C总线共享**: I2C2被OLED和MPU6050共享，通过互斥锁 + TCA9548A通道切换 + GPIO重初始化保证安全
2. **Madgwick传感器融合**: Web端使用Madgwick滤波器将加速度计+陀螺仪原始数据融合为四元数，驱动骨骼旋转
3. **多设备TCP管理**: Flask TCP Server支持最多10个设备同时连接，线程安全，自动重连
4. **指尖联动**: 通道映射中第3节指骨旋转自动带动第4节，符合人体手指弯曲规律
5. **HAL直驱ADC**: 电池采集绕过RT-Thread ADC框架，直接使用STM32 HAL库，需确保已启用 `HAL_ADC_MODULE_ENABLED`

---

## 注意事项

1. 确保使用 `pkgs --update` 完成 WiFi驱动软件包下载
2. 编译优化等级可调至 `-O2`/`-O3` 提高WiFi吞吐量
3. I2C2总线操作必须先获取互斥锁
4. 右手端 `main.c` 中存在对VTX316的引用但缺少对应模块，需注意编译兼容性
5. Web端Three.js模型文件上传限制64MB
