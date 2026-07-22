# Project Context

## Purpose

本项目是一套 **边缘端双向手语翻译系统**，由两套 ART-Pi2 数据采集端、一套 PC Web 可视化后端和一个 ROCK 边缘推理设备组成。目标是把用户双手的 IMU 传感器数据、锂电池状态、PC 控制指令、ROCK 模型推理结果整合为一套可实时可视化的双语（手势→文本 / 文本→语音）通道，服务于手语翻译研究与原型验证。

主要用户场景：

- 研究人员戴上双手手套采集手语动作，前端用 Three.js 骨骼模型实时回放。
- ROCK 边缘设备订阅 IMU CSV，调用手语识别模型下发翻译文本与控制命令。
- PC 端 Flask 应用作为中央数据中枢与可视化平台，统一管理 9109 设备会话、9108 地址广播、9101/9102 ROCK 链路。

## Tech Stack

- **嵌入式端（左手 / 右手）**
  - ART-Pi2 开发板（STM32H7S7L8Hx，ARM Cortex-M7，480 MHz，456 KB AXI SRAM，外部 XSPI2 Flash 131 MB+）
  - RT-Thread v5.1.0
  - Keil MDK v5 + ARM Compiler V6.22
  - CMSIS / STM32H7 HAL Driver / TouchGFX
  - NimBLE-v1.0.0（BLE，默认启动路径不启用）
  - wifi-host-driver-latest / netutils-latest（CYW43438）
  - I2C：双 TCA9548A + 8×MPU6050 + 2×MPU6050 + 1×ICM-20948（9-DOF, AK09916）
  - VTX316 语音合成（仅左手）/ INMP441 麦克风（仅右手）
- **PC Web 后端**
  - Python 3 + Flask + SQLite
  - Layui v2.13.3 + Three.js
  - 自定义 TCP Server（端口 9109，JSON 双向）
  - 自定义 UDP Discovery Broadcaster（端口 9108，`ARTPI_PC,1,9109\n`）
- **AI / 数据**
  - ROCK 边缘设备（Plan B 自建热点，热点接口 wlan1 / 192.168.1.1/24，已通过真机验证的热点 SSID `rockchip_4eabbe`）：左手 CSV→`192.168.1.1:9101`、右手 CSV→`192.168.1.1:9102`
  - 右手 STT：`http://192.168.1.1:8080/stt`（复用 ROCK_SERVER_IP，与 ROCK IMU 服务同一主机）；ROCK 外部 WiFi 地址变化不影响内部链路
  - BLSTM 数据集：`rock/data/processed/final_blstm_dataset/`
- **规范**
  - OpenSpec（`openspec/`）：proposal → implementation → archive，AGENTS 流程

## Project Conventions

### Code Style

- 嵌入式 C：
  - 缩进 4 空格，禁止 tabs。
  - 线程入口函数统一 `<name>_thread_entry(void *param)`。
  - 互斥资源：`i2c2_mutex_take()` / `i2c2_mutex_release()`。
  - 全局命名：模块前缀 `<module>_<verb>`（如 `tcp_client_start`）。
- 后端 Python：PEP 8 + 类型注解；Flask blueprint 按功能拆分；服务以 dataclass 形式持有状态。
- 文档：项目自有 README 4 份（根 + 左手 + 右手 + 顶层 doc），不重复但互相引用。

### Architecture Patterns

- 三条主干通信路径完全独立，互不影响：
  1. **PC TCP 9109**：JSON 双向，最多 10 设备，HELLO + PING/PONG 会话维护。
  2. **PC UDP 9108**：PC broadcaster，limited broadcast + `/24` directed broadcast + 每 2 s `/24` peer unicast sweep。
  3. **ROCK WiFi TCP**：左手 9101 / 右手 9102，原始 72 字段 CSV，90 ms，受 `OP_STATE_RUNNING` gate。
- PC endpoint 由 `server_config` mutex 原子发布 `IP + port + generation`；endpoint 变化 → generation++ → TCP 重连，无需重刷固件。
- BLE 默认启动路径不启用；WiFi/TCP 是 live runtime path。
- `operation_mode` 是本地状态机的唯一 owner，`imu_wifi_sender` 是 operation gate 与 ROCK 协议消费方。
- ROCK 端既订阅 CSV，又下发 `CMD:*` / `MODE:*` / `SAY:`，并通过同一 TCP session 承载 `MODEL:*` 与 `WAITING_STOP:<hand>\n` sideband。

### Testing Strategy

- 文档级：`openspec validate --all --strict --no-interactive` 必须 0 failed。
- 嵌入式端：
  - PC discovery 验证：TCP 9109 监听、UDP 9108 接收、generation 0→1、HELLO/JSON/PING/PONG、热点 off/on 自动恢复。
  - 手动模式全流程：boot → manual sleep → `CMD:START` → 90 ms stream → `CMD:STOP` → sequence reset（活动 `add-dual-hand-operation-modes/tasks.md:14`）。
  - 自动模式全流程：200 ms × 5 连击 + 6/10 手指通道 + 手背 + 低运动 → `OP_STATE_RUNNING`（活动 tasks:26,31）。
- 后端：Python `py_compile`、`netstat -ano | findstr :9109` 验证监听。
- 第三方随包（CMSIS / lwIP / NimBLE / Three.js）只走上游自带单元测试，不在本仓库重复构建。

### Git Workflow

- 单主干 `main`，所有 OpenSpec 改动通过 `openspec/changes/<change>/` 提案 → 评审 → 实施 → 归档。
- 提交粒度：每个 task 一个 commit；commit message 引用对应 capability 或 change 路径。
- 归档变更：`openspec validate add-<change> --strict --no-interactive` 通过后，用 `openspec archive` 移到 `openspec/changes/archive/YYYY-MM-DD-<change>/`。

## Domain Context

- 手语翻译领域：左右手各 11 路 IMU → 3D 骨骼模型 → 帧级对齐 → ROCK 模型推理 → 文本回写。
- 实时性要求：端到端 ≤ 150 ms；采样端 10 Hz（PC JSON）/ 11.1 Hz（ROCK CSV）/ 50 ms（前端轮询）。
- 硬件约束：电池 3.0~4.2 V，1:2 分压到 ADC；I2C2 总线被 OLED + MPU 共享，需 mutex。
- 部署场景：手机热点 + PC + 两块 ART-Pi2 + ROCK；热点 DHCP 可能换地址。

## Important Constraints

- **引脚 / 总线分配在 OpenSpec 中不承诺**，必须从代码或 `board/CubeMX_Config/` 获取。
- **PC discovery 完全实现且需多端验证**：PC DHCP 真实换地址、左手独立日志、ROCK 隔离（archive `add-pc-endpoint-auto-discovery/validation.md` 未完全闭环）。
- **ROCK 二阶段滑窗 reviewer 未实现**（活动 `add-dual-hand-operation-modes/tasks.md:28`）。
- **生产阈值 / 持久化校准未完成**（活动 design.md:181-185）。
- **BLE/WiFi 共存未做**（ROCK BLE `[FRAG]` 重组任务在 archive 但仍未勾选）。
- **OpenSpec 引用的协议字段都是强制 SHALL**；现行 `rock-wifi-csv-uplink` 依赖未归档的 `dual-hand-operation-modes`，存在 spec/能力不同步。

## External Dependencies

- ART-Pi2 板级 BSP（STM32H7RSxx HAL + RT-Thread）
- CYW43438 WiFi 资源（CLM blob + 固件 bin，SD 卡加载）
- Three.js 上游（`leading_end/three.js-dev/`）
- Layui v2.13.3
- OpenAI 范式第三方大模型（通过 Web 后端 AI 引擎管理）
- ROCK 边缘设备（IP/端口见上）
- BLSTM 数据集（`rock/data/processed/final_blstm_dataset/`）
