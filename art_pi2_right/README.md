# ART-Pi2 多传感器数据采集与WiFi传输系统

## 项目简介

本项目基于 **ART-Pi2 开发板**（STM32H7RS MCU, Cortex-M7）和 **RT-Thread v5.1.0 RTOS**，实现了一个多路IMU传感器数据采集、锂电池电压监测、OLED实时显示与WiFi远程传输的综合嵌入式系统。

### 核心功能

- **11路IMU传感器数据采集**：通过双TCA9548A I2C多路复用器，采集8路MPU6050（6轴）+ 2路MPU6050（6轴）+ 1路MPU9250（9轴，含磁力计）
- **WiFi TCP双向通信**：通过板载CYW43438 WiFi模块，以JSON格式将传感器数据实时发送到PC端，同时支持接收PC下发命令
- **PC地址自动发现**：监听UDP `9108`广播并自动连接PC TCP `9109`，热点分配新IP后无需重新烧录
- **锂电池电压监测**：ADC采集锂电池分压电压，8次滑动平均滤波，14级精细电池图标显示
- **OLED实时显示**：128×64 SH1106 OLED屏幕，顶部状态栏显示电池电压/电量图标，下方3行显示串口接收文本
- **串口文本接收显示**：UART1接收外部串口文本，按行解析后滚动显示到OLED

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│  main线程: LED心跳闪烁 (PO5, 500ms) + 创建以下子线程         │
├─────────────────────────────────────────────────────────────┤
│  Thread 1: iic_drv   [优先级20, 栈2KB]                      │
│    → 初始化I2C2互斥锁、TCA9548A探测、OLED初始化              │
│    → 一次性任务，完成后自动退出                               │
│                                                             │
│  Thread 2: uart_oled [优先级18, 栈2KB]                      │
│    → UART1接收文本 → OLED滚动显示 (3行)                      │
│    → 每3秒刷新电池状态栏 (局部更新)                           │
│                                                             │
│  Thread 3: mpu6050   [优先级16, 栈4KB]                      │
│    → 10Hz周期轮询11路IMU传感器                               │
│    → 提供 mpu_get_channel_data() 接口供其他模块读取          │
│                                                             │
│  Thread 4: bat_adc   [优先级22, 栈1KB]                      │
│    → ADC1 CH12 (PC2) 采集锂电池分压                          │
│    → 1Hz采样, 8次滑动平均滤波                                │
│                                                             │
│  Thread 5: pc_disc  [优先级24, 栈2KB]                       │
│    → UDP 9108监听PC地址广播，WiFi重连后重建socket             │
│                                                             │
│  Thread 6: tcp_cli   [优先级22, 栈4KB]                       │
│    → WiFi TCP 9109发送JSON数据，端点变化后自动重连            │
│    → 支持双向通信: 接收PC下发命令                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 硬件连接

```
STM32H7RS (ART-Pi2) 引脚分配:

I2C1 (PB8-SCL, PB9-SDA) → TCA9548A #1 (0x70) → CH0~CH7: 8× MPU6050 (6-DOF)
I2C2 (PE1-SCL, PE2-SDA) → TCA9548A #2 (0x70) → CH0~CH1: 2× MPU6050 (6-DOF)
                                               → CH2:     MPU9250 (9-DOF, 含AK8963磁力计)
                                               → CH3:     OLED SH1106 (128×64)

UART1 (PF13-TX, PF12-RX) ← 外部串口输入 (P1排针)
UART4 (PD0-TX, PD1-RX)   → 控制台 (MSH Shell)

ADC1 CH12 (PC2) ← 锂电池分压 (2× 1KΩ电阻, 分压比1:2)

GPIO PO5 → LED心跳指示灯

WiFi: 板载CYW43438模块 → TCP数据传输
```

### 电池采集电路

```
锂电池(+) ──── R1(1KΩ) ──┬── R2(1KΩ) ──── GND
                          │
                         PC2 (ADC1_INP12)

分压公式: V_adc = V_bat × R2/(R1+R2) = V_bat × 1/2
ADC 12位分辨率, 参考电压 3.3V
满电压 4.2V → 截止电压 3.0V, 线性映射0~100%
```

### OLED显示布局 (128×64像素)

```
┌────────────────────────────────────────────────┐
│ Y=0~7   (8px):  状态栏 [X.XXV XX%] [电池图标]   │
│ Y=8~23  (16px): 文本第1行 (8×16字体)            │
│ Y=24~39 (16px): 文本第2行 (8×16字体)            │
│ Y=40~55 (16px): 文本第3行 (8×16字体)            │
│ Y=56~63 (8px):  (未使用)                        │
└────────────────────────────────────────────────┘
状态栏每3秒局部刷新, 不影响文本区域
电池图标共14级精细显示 (0%~100%, 每级约8%区间)
```

---

## 项目目录结构

```
art_pi2_right/
├── applications/                   # 应用层
│   ├── main.c                      # 主入口, 线程创建, LED心跳
│   ├── adc_battery.c/h             # 锂电池ADC采集 (HAL直驱, 滑动滤波)
│   ├── tcp_client.c/h              # PC TCP 9109双向通信 (JSON格式)
│   ├── pc_discovery.c/h            # UDP 9108 PC地址自动发现
│   └── server_config.c/h            # 线程安全端点与generation管理
├── IIC/                            # I2C通信模块
│   ├── iic_thread.c/h              # I2C/OLED初始化线程
│   ├── tca9548a.c/h                # TCA9548A 8通道I2C多路复用器驱动
│   ├── i2c2_mutex.c/h              # I2C2总线互斥锁 (OLED与MPU6050共享)
│   └── OLED/                       # OLED显示驱动
│       ├── OLED.c/h                # SH1106驱动 (GPIO bit-bang I2C)
│       └── OLED_Data.c/h           # 字体数据 + 14级电池图标
├── UART/                           # 串口通信模块
│   └── uart_oled_thread.c/h        # UART1接收 + OLED显示 + 电池状态栏
├── mpu6050/                        # IMU传感器模块
│   ├── mpu6050_thread.c/h          # 多路传感器采集线程 (11通道)
│   └── mpu6050.c/h                 # MPU6050/MPU9250底层I2C寄存器驱动
├── board/                          # 板级支持
│   ├── board.c/h                   # 板级初始化
│   ├── CubeMX_Config/              # STM32CubeMX生成的HAL配置
│   └── port/                       # FAL Flash分区, 文件系统
├── rt-thread/                      # RT-Thread内核源码
├── libraries/                      # STM32 HAL库, CMSIS
├── packages/                       # WiFi驱动(CYW43438), netutils
├── rtconfig.h                      # RT-Thread内核配置
├── project.uvprojx                 # Keil MDK工程文件
└── SConstruct/SConscript           # SCons构建脚本
```

---

## I2C总线共享机制

本项目中I2C2总线被OLED显示和MPU6050传感器共享，采用以下机制避免冲突：

1. **互斥锁保护**：`i2c2_mutex` 提供 `i2c2_mutex_take()` / `i2c2_mutex_release()` 接口
2. **双I2C驱动方式**：
   - MPU6050线程使用 **RT-Thread I2C框架**（软件I2C bit-bang）
   - OLED线程使用 **GPIO直接bit-bang**（独立于RT-Thread框架）
3. **TCA9548A通道切换**：每次操作OLED前需禁用所有通道再重新选择CH3，确保通道缓存与实际状态一致
4. **GPIO重初始化**：OLED操作前调用 `OLED_I2C_Init()` 重新配置PE1/PE2引脚状态

---

## 传感器数据接口

```c
/* 获取指定通道传感器数据 */
rt_err_t mpu_get_channel_data(int ch, mpu_channel_data_t *out);

/* 通道分配:
 *   ch0~ch7:  I2C1 TCA9548A CH0~CH7 → MPU6050 (6-DOF: ax,ay,az,gx,gy,gz)
 *   ch8~ch9:  I2C2 TCA9548A CH0~CH1 → MPU6050 (6-DOF)
 *   ch10:     I2C2 TCA9548A CH2     → MPU9250 (9-DOF: +mx,my,mz磁力计)
 */

/* 获取电池状态 */
rt_uint32_t battery_get_voltage(void);     /* 返回mV */
rt_uint8_t  battery_get_percentage(void);  /* 返回0~100 */
```

---

## 编译与下载

### 环境要求

- **IDE**：Keil MDK v5 (ARM Compiler V6.22)
- **开发板**：ART-Pi2 (STM32H7S7L8H6H)
- **RTOS**：RT-Thread v5.1.0

### 编译步骤

1. 使用 `pkgs --update` 命令下载在线软件包（wifi-host-driver、netutils）
2. 打开 `project.uvprojx` 工程文件
3. 编译（Build）
4. 通过 ST-Link 下载固件到开发板

### WiFi资源文件

如使用SD卡加载WiFi资源文件，需将以下文件放到SD卡根目录：

```
packages/wifi-host-driver-latest/wifi-host-driver/WiFi_Host_Driver/resources/clm/COMPONENT_43438/43438A1.clm_blob
packages/wifi-host-driver-latest/wifi-host-driver/WiFi_Host_Driver/resources/firmware/COMPONENT_43438/43438A1.bin
```

---

## 使用方法

### PC地址自动发现

PC前端每秒发送 `ARTPI_PC,1,9109\n` 到两种广播地址，并每2秒对当前 `/24` 热点子网发送一次单播发现兜底。开发板仅信任UDP来源IPv4地址，并严格校验magic、版本和端口。端点变化会递增generation，`tcp_client`在运行循环中检测变化并自动重连。

启动顺序为：`net_manager`确认WiFi就绪 → `pc_discovery_start()` → 最多等待3秒首个广播 → `tcp_client_start()`。等待超时不会停止后台发现，后续收到广播仍会更新端点并触发重连。

可使用以下MSH命令查看状态：

```bash
msh> pc_disc_stat
```

### MSH命令

```bash
# 连接WiFi
msh> wifi join <SSID> <password>

# 手动启动TCP数据传输（正常上电流程会自动启动并使用发现的PC端点）
msh> tcp_start [ip] [port]

# 停止TCP传输
msh> tcp_stop

# 查询电池电压和电量
msh> bat
Battery: 3.850V  71%
```

### TCP数据格式

传感器数据以JSON格式通过WiFi TCP `9109`发送到PC端，包含11路IMU数据和电池电压信息，发送频率10Hz。TCP连接使用HELLO与PING/PONG维护会话状态。

---

## 注意事项

1. 确保使用 `pkgs --update` 命令完成 `wifi-host-driver-latest` 软件包的下载
2. 编译优化等级可调高至 `-O3` 以提高WiFi吞吐量
3. I2C2总线上OLED和MPU6050传感器共享，通过互斥锁保护，切勿在未获取锁的情况下直接操作
4. 电池ADC采集使用STM32 HAL库直接驱动（非RT-Thread ADC框架），需确保 `stm32h7rsxx_hal_conf.h` 中已启用 `HAL_ADC_MODULE_ENABLED`
5. OLED通过TCA9548A通道3连接，每次操作前需重新选择通道
6. Windows防火墙需允许Python专用网络通信；PC与开发板必须连接同一热点且热点不能启用客户端隔离
