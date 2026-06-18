#此文件负责从stm32接收json格式的姿态数据，并维护一个内存表供blender脚本读取
import sys
import json
import logging
import queue
import threading
import time

import serial


class UARTTable:
    def __init__(self, port, baudrate=115200, logging_level=logging.INFO):
        #初始化串口参数、停止信号和数据存储表
        self.port = port
        self.baudrate = 115200
        self.ser = None
        self.stop_sig = threading.Event()
        self.stop_sig.clear()

        self.data_table = {}

        #初始化日志记录器，输出到控制台，格式为“时间 级别[类别]：消息”
        self.logger = logging.getLogger(self.__class__.__name__)
        self.logger.setLevel(logging_level)
        
        if not self.logger.handlers:
            ch = logging.StreamHandler()
            ch.setStream(sys.stdout)
            ch.setLevel(logging.DEBUG)
            # ch.setFormatter(logging.Formatter("%(asctime)s %(levelname)s [%(name)s::%(threadName)s]: %(message)s", datefmt="%Y-%m-%d %H:%M:%S"))
            ch.setFormatter(logging.Formatter("%(asctime)s %(levelname)s [%(name)s]: %(message)s", datefmt="%Y-%m-%d %H:%M:%S"))
            self.logger.addHandler(ch)
    
    #尝试连接串口，失败则每0.5秒重试一次，直到成功或收到停止信号
    def connect(self):
        self.logger.info("Connecting to serial port {0}...".format(self.port))
        # close any exising connection
        if self.ser:
            self.ser.close()
        while 1:
            # if main want to exit, we no longer try connect to port
            if self.stop_sig.is_set():
                return
            try:
                self.ser = serial.Serial(self.port, baudrate=self.baudrate)
            except serial.serialutil.SerialException as e:
                self.logger.error(str(e))
                self.logger.debug("Still trying...")
                time.sleep(0.5)
                continue
            
            # we only reach here if serial connection is established
            if self.ser.is_open:
                break

        self.logger.info("Connected.")
    #停止方法：设置停止标志并关闭串口连接
    def stop(self):
        self.stop_sig.set()
        if self.ser:
            self.ser.close()
        self.logger.info("Stopped.")
    #同步启动（会阻塞主线程）
    def start(self):
        self.logger.debug("Starting...")
        self.recvPacket()
    #异步启动（在后台线程运行，不阻塞UI）
    def startThreaded(self):
        self.logger.debug("Starting thread...")
        self.t = threading.Thread(target=self.recvPacket)
        self.t.start()
    #根据键名获取对应的数据
    def get(self, key):
        return self.data_table.get(key)
    #持续读取串口数据，按行解析json，提取键值对存储到内存表
    def recvPacket(self):
        while 1:
            if self.stop_sig.is_set():
                return
            
            c = b""
            buf = b""
            while c != b"\n":
                if self.stop_sig.is_set():
                    return
                buf += c
                # atomically handle serial object exceptions
                try:
                    c = self.ser.read()
                except (AttributeError, TypeError, serial.serialutil.SerialException) as e:
                    print(e)
                    self.connect()
                    continue

            data = buf.decode()
            # self.logger.debug(data)
            try:
                data = json.loads(data)
            except json.decoder.JSONDecodeError:
                self.logger.warning("packet format error.")
                continue
            key = data.get("key")
            if not key:
                self.logger.warning("packet format error.")
                continue
            
            self.data_table[key] = data.get("value")
        

# 注释掉主程序部分，避免在 Blender 中执行
# if __name__ == "__main__":
#     try:
#         uart_table = UARTTable("COM8", logging_level=logging.DEBUG)
#         
#         # uart_table.start()
#         uart_table.startThreaded()
# 
# #        while 1:
# #            print("/joint/0:",uart_table.get("/joint/0"))
# #            print("/joint/1:",uart_table.get("/joint/1"))
# #            time.sleep(0.01)
#     except KeyboardInterrupt:
#         uart_table.stop()
