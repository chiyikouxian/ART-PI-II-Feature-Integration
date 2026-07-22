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
│  左手手套 (LEFT)  │ ──── port 9109 ────────→│                              │
│  ART-Pi2 + IMU×11│                         │   Flask Web Server           │
│  + OLED + VTX316 │ ──── port 9101 ────────→│   (Python + Three.js)        │
│  + 9-DOF ICM-20948│                        │                              │
└──────────────────┘                         │  ┌─ 实时翻译 (3D骨骼模型)    │
                                             │  ├─ 设备数据监控             │
┌──────────────────┐     WiFi TCP (JSON)     │  ├─ 通道映射配置             │
│  右手手套 (RIGHT) │ ──── port 9109 ────────→│  ├─ AI引擎管理               │
│  ART-Pi2 + IMU×11│                         │  └─ 界面管理                 │
│  + 麦克风 + 语音  │ ──── port 9102 ────────→│                              │
│  + 9-DOF ICM-20948│                        │                              │
└──────────────────┘                         └──────────────────────────────┘
                                                  ▲
                                                  │ UDP 9108 ARTPI_PC,1,9109\n
                                            (PC broadcaster)

ROCK 边缘设备 (Plan B 自建热点, 热点接口 wlan1, 192.168.1.1/24, 已验证 SSID rockchip_4eabbe)
   - 左手 192.168.1.1:9101 / 右手 192.168.1.1:9102: 接收 [DATA]<ts>,hand,<seq>,69 raw ints> 原始CSV
   - 右手 STT: `http://192.168.1.1:8080/stt`（复用 ROCK_SERVER_IP，与 ROCK IMU 服务共用主机地址）
   - 下发 CMD:START / CMD:STOP / CMD:RESET_SEQ / MODE:MANUAL / MODE:AUTO / SAY:<text>
   - 复用同一 TCP session 传递 MODEL:* 与 WAITING_STOP:<hand>\n sideband
   - 与 PC discovery 9108/9109 完全独立
```

> 三条主干通信路径：
> 1. **PC TCP 9109**（JSON 双向，左手端 translated_text 也通过此通道上行）
> 2. **PC UDP 9108**（PC 广播 ARTPI_PC,1,9109 给两块 ART-Pi2 做地址自动发现）
> 3. **ROCK WiFi TCP**（左手 9101 / 右手 9102，原始 IMU CSV + 控制命令）
>
> **Plan B 内部网络（固定，已真机验证）：** ROCK 用自建热点作为双手内部网络——
> 热点接口 `wlan1`、热点侧固定 IP `192.168.1.1/24`，热点 SSID 为已验证的
> `rockchip_4eabbe`（固件内配置的已验证热点凭据，见各端 `main.c` /
> `net_manager.c`）。两手上电后自动连接该热点，
> 等 DHCP/WiFi Ready 后主动连接 `192.168.1.1` 的 9101/9102，右手 STT 走
> `http://192.168.1.1:8080/stt`（与 ROCK IMU 服务共用同一 IP）。ROCK 的 `wlan0` 等
> 外部接口用于连接答辩室 WiFi / 手机热点，其地址变化不影响上述三条内部链路；ROCK 服务
> 地址为固定值，不随 DHCP 默认网关动态改变（网关仅用于诊断，正常应为 `192.168.1.1`）。
> ROCK endpoint 与 PC 9108/9109 discovery 完全独立，互不影响。

PC端每秒向 `255.255.255.255:9108/UDP` 和当前热点的 `/24` 定向广播地址（例如 `192.168.137.255:9108`）同时广播：

```text
ARTPI_PC,1,9109\n
```

左右手开发板从 `recvfrom()` 的来源地址取得当前PC IPv4地址。PC同时保留两种广播，并每2秒对当前 `/24` 热点子网执行一次小型UDP单播发现扫描，用于兼容会过滤全部客户端广播、但允许客户端单播的手机热点。手机热点重连导致PC地址变化时，`server_config` 原子更新 `IP + port + generation`，PC侧TCP客户端检测到 generation 变化后自动断开并连接新地址，无需重新烧录。

---

## 硬件平台

### MCU规格

- **芯片**: STM32H7S7L8Hx (STM32H7RS系列, ARM Cortex-M7)
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
  ├─ Thread: autostart       [优先级25, 栈4KB] → 上电一次性串联 WiFi→discovery→TCP→ROCK→语音助手 (仅右手)
  ├─ Thread: iic_drv         [优先级20, 栈2KB] → I2C/OLED初始化 (一次性)
  ├─ Thread: vtx316          [优先级20, 栈2KB] → VTX316语音合成初始化 (一次性, 仅左手端)
  ├─ Thread: mpu6050         [优先级16, 栈4KB] → 10Hz 11路IMU数据采集
  ├─ Thread: bat_adc         [优先级22, 栈1KB] → 1Hz 电池电压ADC采样
  ├─ Thread: pc_disc         [优先级24, 栈2KB] → UDP 9108监听PC地址广播
  ├─ Thread: tcp_cli         [优先级22, 栈4KB] → TCP 9109 JSON数据上传与自动重连
  ├─ Thread: imu_wifi_sender [优先级21, 栈4KB] → 每90 ms 上行原始CSV到ROCK 9101/9102
  ├─ Thread: button          [优先级19, 栈1KB] → PE0 物理按键切换 AUTO/MANUAL (仅左手端)
  └─ Main Loop: LED心跳 (500ms)
```

> 注: 右手端 (`art_pi2_right`) 额外创建 `uart_oled` 线程 [优先级18, 栈2KB] 用于UART接收+OLED滚动显示，不包含 `vtx316` 线程。

### 核心功能

- **11路IMU传感器采集**: 10Hz采样率，TCA9548A通道切换，支持MPU6050(6轴)和ICM-20948(9轴)
- **WiFi TCP双向通信**: JSON格式数据实时上传至PC端TCP `9109`，支持接收PC下发命令
- **PC地址自动发现**: UDP `9108`严格解析PC广播，使用来源IPv4地址更新端点；热点换网后通过generation触发自动重连
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
# WiFi / 网络
msh> wifi join <SSID> <password>

# PC 9109 控制（正常上电流程会自动启动并使用发现的PC端点）
msh> tcp_start [ip] [port]
msh> tcp_stop
msh> tcp_client_start / tcp_client_stop   # 与 tcp_start/tcp_stop 等价
msh> tcp_left_stat / tcp_right_stat       # 本端 TCP 收发计数 + generation

# PC 端点（覆盖 discovery）；STT 端点固定为 ROCK_SERVER_IP:8080，右手无 set_stt_ip
msh> set_server_ip <ip> [port]
msh> get_server_ip
msh> get_stt_ip                   # 只读；右手恒返回 ROCK_SERVER_IP:8080

# 状态查询
msh> bat                          # 电池电压 / 电量
msh> pc_disc_stat                 # PC discovery 状态与最近一次广播
msh> auto_status                  # autostart 7 步串联结果
msh> net_stat                     # WiFi / PC discovery / TCP / ROCK 链路一览 (仅右手端)

# WiFi 凭据 / PC 端点 profile（右手端, 仅内存, 重启丢失, 不管理 ROCK/STT）
msh> profile add <name> <ssid> <password> <server_ip>
msh> profile del <name>
msh> profile use <name>
msh> profile list
msh> profile current

# 语音助手（仅右手端）
msh> va_init / va_start / va_stop / va_trigger / va_status / va_reload_stt

# 输出示例
msh> bat
Battery: 3.850V  71%
```

---

## Web可视化后端

### 技术栈

- **后端**: Python 3 + Flask + SQLite
- **前端**: Layui v2.13.3 + Three.js
- **通信**: TCP Socket Server（端口`9109`）+ UDP地址发现（端口`9108`）

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
    ↓ JSON over TCP:9109
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
│   │   └── services/              #   TCP 9109服务与UDP 9108地址广播
│   ├── templates/                 #   Jinja2 HTML模板
│   ├── static/                    #   静态资源 (Layui, Three.js, CSS, 3D模型)
│   ├── hardware/                  #   硬件抽象层 (数据采集, 信号预处理)
│   ├── config.py                  #   Flask配置
│   ├── run.py                     #   启动入口
│   └── requirements.txt           #   Python依赖
│
├── openspec/                      # OpenSpec规范、变更设计与验证任务
│   ├── AGENTS.md                  #   项目级 agent 工作指令（必读）
│   ├── project.md                 #   项目上下文（目的/技术栈/架构/约束）
│   ├── specs/                     #   现行已构建能力 (built & deployed truth)
│   │   ├── left-hand-bidirectional-translation/   # BLE + PC TCP 契约
│   │   ├── pc-endpoint-auto-discovery/            # UDP 9108 + generation
│   │   └── rock-wifi-csv-uplink/                  # ROCK 9101/9102 原始CSV
│   └── changes/                   #   提案 / 活动变更 / 已归档变更
├── a8c4b-main/                    # HZK16 16×16 中文字库样例（历史参考，与本项目无直接耦合）
├── Unicode_GB2312_GBK_convert_table-master/  # 字符编码转换表（GB2312/GBK↔Unicode）
└── README.md                      # 本文件
```

---

## OpenSpec 规范工作流

本项目采用 OpenSpec 管理协议与行为的当前事实（current truth）。

- `openspec/AGENTS.md` 是项目级 agent 必读入口，定义 proposal → implementation → archive 三阶段流程与严格校验规则。
- `openspec/project.md` 描述项目目的、技术栈、架构、测试策略与外部依赖（agent 开工前应先读）。
- `openspec/specs/<capability>/spec.md` 是已构建、已部署的现行契约（built & deployed truth）。本项目当前 3 个：
  - `left-hand-bidirectional-translation` — BLE 5.0 契约（设备名 `ART-Pi2-IMU-L`、Service `A74D0001-…`、Notify 90 ms、72 字段 CSV、默认启动路径不启用 BLE）
  - `pc-endpoint-auto-discovery` — PC UDP 9108 广播 + 双手 9109 自动重连
  - `rock-wifi-csv-uplink` — ROCK 9101/9102 原始CSV上行 + CMD/MODE/SAY 下行
- `openspec/changes/<change>/{proposal,design,tasks,specs}.md` 是尚在开发中的提案。当前活动变更：`add-dual-hand-operation-modes`（AUTO/MANUAL 状态机，26/31 tasks 完成）。
- `openspec/changes/archive/YYYY-MM-DD-…` 是已完成并归档的历史变更。

约束：

- 修改协议、端口、UUID、传感器映射、操作模式状态机前，先在 `openspec/specs/<对应 capability>/spec.md` 阅读现行契约，再决定是新增 `change` 还是修订现有 capability。
- 提案审批前不得动手改实现。归档后才能视为 deployed truth。
- 严格校验命令（在仓库根目录执行）：
  ```bash
  openspec validate --all --strict --no-interactive
  ```

---

## 编译与运行

### 嵌入式端

#### 环境要求

- **IDE**: Keil MDK v5 (ARM Compiler V6.22)
- **开发板**: ART-Pi2 (STM32H7S7L8Hx)
- **RTOS**: RT-Thread v5.1.0

#### 编译步骤

1. 使用 `pkgs --update` 下载在线软件包（wifi-host-driver、netutils）
2. 打开 `project.uvprojx` 工程文件
3. 编译 (Build)
4. 通过 ST-Link 下载固件到开发板

#### WiFi资源文件

如使用SD卡加载WiFi固件，需将以下文件放到SD卡根目录：

```
packages/wifi-host-driver-latest/wifi-host-driver/WiFi_Host_Driver/resources/clm/COMPONENT_43438/43438A1.clm_blob
packages/wifi-host-driver-latest/wifi-host-driver/WiFi_Host_Driver/resources/firmware/COMPONENT_43438/43438A1.bin
```

### Web后端

```bash
cd leading_end
pip install -r requirements.txt
python run.py
```

默认启动后访问 `http://localhost:5000`。设备TCP Server监听 `0.0.0.0:9109`，后台发现服务通过UDP `9108`广播当前PC地址。

Windows首次运行时应允许Python通过“专用网络”防火墙。可用以下命令确认TCP监听：

```powershell
netstat -ano | findstr :9109
```

### 热点IP变化验证

1. 启动 `leading_end/run.py`，确认TCP `9109`已监听。
2. 启动左右手开发板，使用 `pc_disc_stat` 确认收到有效广播并记录发现地址。
3. 关闭并重新开启手机热点，让电脑重新连接并获得新的IPv4地址。
4. 不重启、不重新烧录开发板，确认发现地址与endpoint generation发生变化。
5. 确认左右手重新连接TCP `9109`且JSON数据恢复；ROCK `9101/9102`链路不应受影响。

---

## 技术要点

1. **I2C总线共享**: I2C2被OLED和MPU6050共享，通过互斥锁 + TCA9548A通道切换 + GPIO重初始化保证安全
2. **Madgwick传感器融合**: Web端使用Madgwick滤波器将加速度计+陀螺仪原始数据融合为四元数，驱动骨骼旋转
3. **多设备TCP管理**: Flask TCP Server在`9109`支持最多10个设备同时连接，处理HELLO、PING/PONG并维护设备会话
4. **指尖联动**: 通道映射中第3节指骨旋转自动带动第4节，符合人体手指弯曲规律
5. **HAL直驱ADC**: 电池采集绕过RT-Thread ADC框架，直接使用STM32 HAL库，需确保已启用 `HAL_ADC_MODULE_ENABLED`
6. **动态端点发现**: UDP广播来源地址 + 严格协议校验 + mutex原子快照 + generation驱动重连

---

## 注意事项

1. 确保使用 `pkgs --update` 完成 WiFi驱动软件包下载
2. 编译优化等级可调至 `-O2`/`-O3` 提高WiFi吞吐量
3. I2C2总线操作必须先获取互斥锁
4. Web端Three.js模型文件上传限制64MB
