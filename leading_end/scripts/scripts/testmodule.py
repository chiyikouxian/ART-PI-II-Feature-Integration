#此文件是blender的实时骨骼驱动脚本，负责从串口接收姿态数据并驱动3D模型的骨骼进行实时动画
import sys
import os
import json
import logging

import bpy
import numpy as np
import serial

# Robustly load uartcomm.py from the .blend directory or its scripts/ subfolder
UARTTable = None
try:
    from uartcomm import UARTTable as _UARTTable
    UARTTable = _UARTTable
except Exception:
    blend_dir = bpy.path.abspath("//")
    candidates = [
        os.path.join(blend_dir, "uartcomm.py"),
        os.path.join(blend_dir, "scripts", "uartcomm.py"),
    ]
    loaded = False
    for cand in candidates:
        if os.path.exists(cand):
            import importlib.util
            spec = importlib.util.spec_from_file_location("uartcomm", cand)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            UARTTable = mod.UARTTable
            print(f"[INFO] uartcomm loaded from: {cand}")
            loaded = True
            break
    if not loaded:
        raise FileNotFoundError(
            "未找到 uartcomm.py。请将 uartcomm.py 放到 .blend 同目录或其 scripts/ 子目录后重试。\n"+
            f"搜索路径: {candidates}"
        )

#设置串口端口和更新频率
MCU_COM_PORT = "COM5"  # 修改为COM5
# FPS = 60      # 原始值，数据率不足时会卡顿
FPS = 30        # 优化: 匹配STM32数据率(约33Hz)，减少卡顿

# 运行模式配置
# SINGLE_SENSOR_MODE = True  时只用 /joint/0 驱动 SINGLE_SENSOR_BONE_NAME
# SINGLE_SENSOR_MODE = False 时使用双传感器独立模式或层级模式
SINGLE_SENSOR_MODE = False  # 使用双传感器层级模式

# 双传感器模式配置
# DUAL_SENSOR_INDEPENDENT = True  独立模式：各自独立旋转
# DUAL_SENSOR_INDEPENDENT = False 层级模式：子骨骼使用相对旋转（推荐用于手部）
DUAL_SENSOR_INDEPENDENT = False  # 使用层级模式
BONE_JOINT_0 = "FingerA.3.R"  # joint0控制子级（手指末端）
BONE_JOINT_1 = "FingerA.2.R"  # joint1控制父级（手指中段）

# 单传感器模式的骨骼名称（仅 SINGLE_SENSOR_MODE=True 时使用）
SINGLE_SENSOR_BONE_NAME = "Hand.R"

# 校准配置
# 启动时自动校准：记录初始姿态作为零点，之后的运动都是相对于初始姿态
CALIBRATION_ENABLED = True
# 校准等待时间（秒）：启动后等待数据稳定再采样
CALIBRATION_DELAY_FRAMES = 10  # 约0.3秒（30FPS下）


# set up logging
#设置blender专用日志记录器，输出调试信息
logger = logging.getLogger("BPY")
logger.setLevel(logging.DEBUG)

if not logger.handlers:
    ch = logging.StreamHandler()
    ch.setStream(sys.stdout)
    ch.setLevel(logging.DEBUG)
    # ch.setFormatter(logging.Formatter("%(asctime)s %(levelname)s [%(name)s::%(threadName)s]: %(message)s", datefmt="%Y-%m-%d %H:%M:%S"))
    ch.setFormatter(logging.Formatter("%(asctime)s %(levelname)s [%(name)s]: %(message)s", datefmt="%Y-%m-%d %H:%M:%S"))
    logger.addHandler(ch)


# get scene armature object
#获取场景中的骨架对象和需要驱动的骨骼
armature = bpy.data.objects["骨架"]

# get the bones we need
bone_root = armature.pose.bones.get("root")
bone_upper_arm_R = armature.pose.bones.get("armUp.R")
bone_lower_arm_R = armature.pose.bones.get("armDown.R")
bone_single = armature.pose.bones.get(SINGLE_SENSOR_BONE_NAME)

# 双传感器独立模式的骨骼
bone_joint_0 = armature.pose.bones.get(BONE_JOINT_0)
bone_joint_1 = armature.pose.bones.get(BONE_JOINT_1)
#创建串口通信实例
uart_table = UARTTable(MCU_COM_PORT, logging_level=logging.DEBUG)
# 暴露给控制台/驱动命名空间，便于调试访问
try:
    bpy.app.driver_namespace['uart_table'] = uart_table
    print('[INFO] uart_table exposed to bpy.app.driver_namespace')
except Exception:
    pass

"""
@param bone: the bone object, obtained from armature.pose.bone["<bone_name>"]
@param rotation: rotation in quaternion (w, x, y, z)
"""
#将四元数[w,x,y,z]直接赋值给骨骼的旋转四元数
def setBoneRotation(bone, rotation):
    w, x, y, z = rotation
    bone.rotation_mode = 'QUATERNION'
    bone.rotation_quaternion[0] = w
    bone.rotation_quaternion[1] = x
    bone.rotation_quaternion[2] = y
    bone.rotation_quaternion[3] = z

"""
a piece of math code copied from StackOverflow
https://stackoverflow.com/questions/39000758/how-to-multiply-two-quaternions-by-python-or-numpy

@param q1: in form (w, x, y, z)
@param q0: @see q1
"""
#实现四元数乘法运算，用于计算相对旋转
def multiplyQuaternion(q1, q0):
    w0, x0, y0, z0 = q0
    w1, x1, y1, z1 = q1
    return np.array([-x1 * x0 - y1 * y0 - z1 * z0 + w1 * w0,
                     x1 * w0 + y1 * z0 - z1 * y0 + w1 * x0,
                     -x1 * z0 + y1 * w0 + z1 * x0 + w1 * y0,
                     x1 * y0 - y1 * x0 + z1 * w0 + w1 * z0], dtype=np.float64)

def quaternionInverse(q):
    """计算四元数的逆（共轭），用于校准"""
    return q * np.array([1, -1, -1, -1])
                     
#继承blender的Operator类，实现实时更新功能
class ModalTimerOperator(bpy.types.Operator):
    # we need these two fields for Blender
    bl_idname = "wm.modal_timer_operator"
    bl_label = "Modal Timer Operator"

    _timer = None
    # 校准相关状态
    _calibrated = False
    _calibration_frame_count = 0
    _calib_q0_inv = None  # 传感器0的校准偏移（逆四元数）
    _calib_q1_inv = None  # 传感器1的校准偏移（逆四元数）

    #每帧执行的核心逻辑：读取串口数据，计算相对旋转，应用到骨骼
    def modal(self, context, event):
        if event.type == "ESC":
            logger.info("BlenderTimer received ESC.")
            return self.cancel(context)

        if event.type == "TIMER":
            # this will eval true every Timer delay seconds
            if SINGLE_SENSOR_MODE:
                q0 = uart_table.get("/joint/0")
                if not q0 or bone_single is None:
                    return {"PASS_THROUGH"}
                q0 = np.array(q0)

                # 校准处理
                if CALIBRATION_ENABLED:
                    if not self._calibrated:
                        self._calibration_frame_count += 1
                        if self._calibration_frame_count >= CALIBRATION_DELAY_FRAMES:
                            self._calib_q0_inv = quaternionInverse(q0)
                            self._calibrated = True
                            logger.info("Calibration complete! Initial pose recorded.")
                        return {"PASS_THROUGH"}
                    # 应用校准：q_calibrated = q_initial_inv * q_current
                    q0 = multiplyQuaternion(self._calib_q0_inv, q0)

                setBoneRotation(bone_single, q0)
            else:
                q0 = uart_table.get("/joint/0")
                q1 = uart_table.get("/joint/1")
                if not q0 or not q1:
                    return {"PASS_THROUGH"}
                # convert data to numpy arrays
                q0 = np.array(q0)
                q1 = np.array(q1)

                # 校准处理
                if CALIBRATION_ENABLED:
                    if not self._calibrated:
                        self._calibration_frame_count += 1
                        if self._calibration_frame_count >= CALIBRATION_DELAY_FRAMES:
                            self._calib_q0_inv = quaternionInverse(q0)
                            self._calib_q1_inv = quaternionInverse(q1)
                            self._calibrated = True
                            logger.info("Calibration complete! Initial pose recorded for both sensors.")
                        return {"PASS_THROUGH"}
                    # 应用校准
                    q0 = multiplyQuaternion(self._calib_q0_inv, q0)
                    q1 = multiplyQuaternion(self._calib_q1_inv, q1)

                if DUAL_SENSOR_INDEPENDENT:
                    # 独立模式：两个传感器各自独立控制一个骨骼
                    if bone_joint_0 is not None:
                        setBoneRotation(bone_joint_0, q0)
                    if bone_joint_1 is not None:
                        setBoneRotation(bone_joint_1, q1)
                else:
                    # 层级模式：计算相对旋转
                    # 注意：joint0=FingerA.3.R（子级），joint1=FingerA.2.R（父级）
                    # 因此 q1 是父级，q0 是子级

                    # get inverse transformation of q1 (parent bone: FingerA.2.R)
                    q1_inv = q1 * np.array([1, -1, -1, -1])

                    # rotate child (q0) relative to parent (q1)
                    q0_rel = multiplyQuaternion(q1_inv, q0)

                    # apply transformations
                    if bone_joint_1 is not None:
                        # joint1 是父级，使用绝对旋转
                        setBoneRotation(bone_joint_1, q1)
                    if bone_joint_0 is not None:
                        # joint0 是子级，使用相对于父级的旋转
                        setBoneRotation(bone_joint_0, q0_rel)

            # if refresh rate is too low, uncomment this line to force Blender to render viewport
#            bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)

        return {"PASS_THROUGH"}
    #启动定时器，设置更新频率
    def execute(self, context):
        # update rate is 0.01 second
        self._timer = context.window_manager.event_timer_add(1./FPS, window=context.window)
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}
     #清理资源：停止串口、重置骨骼、移除定时器
    def cancel(self, context):
        uart_table.stop()

        # reset joint position (if bones exist)
        if bone_upper_arm_R is not None:
            setBoneRotation(bone_upper_arm_R, [1, 0, 0, 0])
        if bone_lower_arm_R is not None:
            setBoneRotation(bone_lower_arm_R, [1, 0, 0, 0])
        if bone_single is not None:
            setBoneRotation(bone_single, [1, 0, 0, 0])
        if bone_joint_0 is not None:
            setBoneRotation(bone_joint_0, [1, 0, 0, 0])
        if bone_joint_1 is not None:
            setBoneRotation(bone_joint_1, [1, 0, 0, 0])
        context.window_manager.event_timer_remove(self._timer)
        logger.info("BlenderTimer Stopped.")
        try:
            if 'uart_table' in bpy.app.driver_namespace:
                del bpy.app.driver_namespace['uart_table']
        except Exception:
            pass
        return {"CANCELLED"}

#启动所有服务：注册操作符、启动串口线程、启动定时器
if __name__ == "__main__":
    try:
        logger.info("Starting services.")
        bpy.utils.register_class(ModalTimerOperator)
        
        # uart_table.start()
        uart_table.startThreaded()
        
        # start Blender timer
        bpy.ops.wm.modal_timer_operator()
        
        logger.info("All started.")
    except KeyboardInterrupt:
        uart_table.stop()
        logger.info("Received KeyboardInterrupt, stopped.")
