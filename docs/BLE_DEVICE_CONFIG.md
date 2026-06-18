# 双手 BLE 设备配置

## 概述

本项目包含左右手两个 ART-Pi2 开发板，通过 BLE 与 ROCK 边缘设备通信。

## 设备标识

### 左手（Left Hand）
- **BLE 设备名称**: `ART-Pi2-IMU-L`
- **MAC 地址**: `C0:4E:51:05:34:33`
- **固件路径**: `art_pi2_left/`
- **设备名称定义**: [art_pi2_left/applications/ble_app.c:33](../art_pi2_left/applications/ble_app.c#L33)

### 右手（Right Hand）
- **BLE 设备名称**: `ART-Pi2-IMU-R`
- **MAC 地址**: `C0:31:51:05:34:33`
- **固件路径**: `art_pi2_right/`
- **设备名称定义**: [art_pi2_right/applications/ble_app.c:51](../art_pi2_right/applications/ble_app.c#L51)

## BLE Service 规格

### 左手 BLE Service
- **Service UUID**: `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00`
- **Characteristics**:
  - **IMU Notify** (`A74D0002-B4E7-4C5F-9D2A-F163E80ACB00`): Read + Notify, UTF-8 CSV 文本格式，`[DATA]<timestamp>,<hand_type>,<frame_seq>,ch0_ax,...,ch10_mz\n`，最大 600 字节
  - **Channel Select** (`A74D0003-B4E7-4C5F-9D2A-F163E80ACB00`): Read + Write, 1 字节通道选择
  - **Text Receive** (`A74D0004-B4E7-4C5F-9D2A-F163E80ACB00`): Write, 最大 256 字节 UTF-8 文本

### 右手 BLE Service
- **Service UUID**: `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00`（与左手相同）
- **Characteristics**:
  - **IMU Notify** (`A74D0002-B4E7-4C5F-9D2A-F163E80ACB00`): Read + Notify, UTF-8 CSV 文本格式，`[DATA]<timestamp>,<hand_type>,<frame_seq>,ch0_ax,...,ch10_mz\n`，最大 600 字节
  - **Channel Select** (`A74D0003-B4E7-4C5F-9D2A-F163E80ACB00`): Read + Write, 1 字节通道选择
  - **Text Receive** (`A74D0004-B4E7-4C5F-9D2A-F163E80ACB00`): Write, 最大 256 字节 UTF-8 文本

## 通信架构

```
┌─────────────────────────────────────────────────────────────┐
│  ROCK 边缘设备                                               │
│  ├─ BLE Central 角色                                         │
│  ├─ 连接左手: C0:4E:51:05:34:33 (ART-Pi2-IMU-L)            │
│  ├─ 连接右手: C0:31:51:05:34:33 (ART-Pi2-IMU-R)            │
│  └─ 处理手语识别并返回文本                                   │
└─────────────────────────────────────────────────────────────┘
         ▲                                    ▲
         │ BLE                                │ BLE
         │ (IMU 上行 + 文本下行)               │ (IMU 上行)
         ▼                                    ▼
┌──────────────────────┐          ┌──────────────────────┐
│  左手 ART-Pi2        │          │  右手 ART-Pi2        │
│  C0:4E:51:05:34:33   │          │  C0:31:51:05:34:33   │
│  ART-Pi2-IMU-L       │          │  ART-Pi2-IMU-R       │
│                      │          │                      │
│  ├─ 11 通道 IMU      │          │  ├─ 11 通道 IMU      │
│  ├─ VTX316 语音      │          │  ├─ 麦克风音频采集   │
│  ├─ BLE 文本接收     │          │  └─ TCP 上报         │
│  └─ TCP 上报         │          │                      │
└──────────────────────┘          └──────────────────────┘
         │                                    │
         │ TCP (WiFi)                         │ TCP (WiFi)
         └────────────────┬───────────────────┘
                          ▼
                 ┌─────────────────┐
                 │  PC 端 TCP 服务器│
                 │  leading_end/    │
                 │  端口: 8266      │
                 └─────────────────┘
```

## MAC 地址配置说明

当前 MAC 地址由 CYW43438 蓝牙芯片固件提供，**不在应用层代码中配置**。

如需修改 MAC 地址，需要：
1. 通过 CYW43438 驱动层 HCI 命令设置
2. 或通过芯片厂商工具烧录到 OTP 区域

## 验证方法

### 扫描设备
使用 BLE 扫描工具（如 nRF Connect）应能看到：
- 设备名称: `ART-Pi2-IMU-L` 或 `ART-Pi2-IMU-R`
- MAC 地址: `C0:4E:51:05:34:33` 或 `C0:31:51:05:34:33`

### 连接测试
1. 连接到左手设备 `C0:4E:51:05:34:33`
2. 发现 Service `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00`
3. 向 Characteristic `A74D0004-B4E7-4C5F-9D2A-F163E80ACB00` 写入文本 `"SAY:测试"`
4. 观察左手 VTX316 是否播报"测试"
5. 检查 PC 端 TCP 服务器是否收到 `translated_text` 字段

## 相关文件

- 左手 BLE 应用: [art_pi2_left/applications/ble_app.c](../art_pi2_left/applications/ble_app.c)
- 左手 BLE Service: [art_pi2_left/applications/imu_ble_service.c](../art_pi2_left/applications/imu_ble_service.c)
- 右手 BLE 应用: [art_pi2_right/applications/ble_app.c](../art_pi2_right/applications/ble_app.c)
- 右手 BLE Service: [art_pi2_right/applications/imu_ble_service.c](../art_pi2_right/applications/imu_ble_service.c)
- PC 端 TCP 服务器: [leading_end/app/services/tcp_server.py](../leading_end/app/services/tcp_server.py)

## 更新日志

- 2026-04-17: 初始文档，记录左右手 MAC 地址和设备名称
- 2026-04-29: 更正 UUID 助记写法（A7IM→A74D），补全右手 Service 规格，更新 IMU Notify 格式描述（145 字节二进制→CSV 文本，含 frame_seq）
