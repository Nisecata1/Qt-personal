import serial
import time
import struct

# ================= 配置 =================
# 这里的 COM 口是你 USB转TTL 插入电脑后显示的端口号
# 请去“设备管理器 -> 端口”查看，例如 COM3, COM5 等
SERIAL_PORT = 'COM6'  # <--- 请根据实际情况修改 !!!
BAUD_RATE = 115200

def main():
    try:
        # 打开串口
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"✅ 串口 {SERIAL_PORT} 已打开，准备发送指令...")
        time.sleep(2) # 等待 Arduino 重启
        
        print("🔄 测试开始：鼠标将画一个正方形")
        
        # 定义移动动作序列 (dx, dy)
        # 注意：这里发的是相对位移
        actions = [
            (50, 0),   # 向右
            (0, 50),   # 向下
            (-50, 0),  # 向左
            (0, -50)   # 向上
        ]
        
        for i in range(4): # 循环 4 次
            for dx, dy in actions:
                # --- 核心：打包协议 ---
                # 格式: <BBhhBB (Little-Endian)
                # B=HEAD(0xA5), B=CMD(1), h=dx, h=dy, B=BTN(0), B=TAIL(0x5A)
                packet = struct.pack('<BBhhBB', 0xA5, 0x01, dx, dy, 0, 0x5A)
                
                # 发送
                ser.write(packet)
                print(f"📤 发送: dx={dx}, dy={dy} | Hex: {packet.hex().upper()}")
                
                # 稍微延时，让人眼能看清移动
                time.sleep(0.5)
                
        print("✅ 测试结束")
        ser.close()

    except serial.SerialException as e:
        print(f"❌ 串口错误: {e}")
        print("提示：请检查 USB转TTL 是否插入，以及 COM 号是否填对。")

if __name__ == "__main__":
    main()
