import time
import serial
import struct
import ctypes
from ctypes import wintypes
import sys

# ================= 代码说明 =================
# 代码说明：1
# ================= 代码说明 =================

# ================= 🔧 配置文件 (CONFIG) =================
SERIAL_PORT = 'COM7'    # # 确认这是正确的串口号，在设备管理器里看
BAUD_RATE   = 921600    # 提升至 92w 以通过 1000Hz 数据
SENSITIVITY = 1.0       # 鼠标透传倍率 (1.0 = 1:1)，觉得慢改成 2.0
# [热键配置]
# 0x24 = Home, 0x14 = CapsLock, 0x05 = 侧键(XBUTTON1)
TOGGLE_KEY  = 0x24      # 0x24 = Home 键 (控制透传的开启/关闭)
DEBUG_LOG   = False     # 关闭日志以减少 I/O 延迟，已经注释掉了，需要自己改
# ========================================================

# --- 全局变量 ---
ser = None
current_buttons = 0    # 全局变量：当前鼠标按键状态位掩码
titan_enabled = True   # 全局变量：透传功能开关状态
last_toggle_state = False # 用于按键去抖

# 加载 DLL
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# 64位参数类型补丁
if ctypes.sizeof(ctypes.c_void_p) == 8:
    user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.DefWindowProcW.restype = wintypes.LPARAM
    user32.GetRawInputData.argtypes = [wintypes.LPARAM, wintypes.UINT, ctypes.c_void_p, ctypes.POINTER(wintypes.UINT), wintypes.UINT]
    user32.GetRawInputData.restype = wintypes.UINT

# 回调类型定义
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_int64, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

# 正确的结构体定义 (自动计算大小)
class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [
        ("dwType", wintypes.DWORD),
        ("dwSize", wintypes.DWORD),
        ("hDevice", wintypes.HANDLE),
        ("wParam", wintypes.WPARAM),
    ]

class RAWMOUSE(ctypes.Structure):
    _anonymous_ = ("u",)
    class _U(ctypes.Union):
        _fields_ = [("ulButtons", wintypes.ULONG), ("struct", wintypes.ULONG)] # 简化联合体
    _fields_ = [
        ("usFlags", wintypes.USHORT),
        ("u", _U),
        ("ulRawButtons", wintypes.ULONG),
        ("lLastX", wintypes.LONG),
        ("lLastY", wintypes.LONG),
        ("ulExtraInformation", wintypes.ULONG),
    ]

class RAWINPUT(ctypes.Structure):
    _fields_ = [
        ("header", RAWINPUTHEADER),
        ("mouse", RAWMOUSE),
    ]

# 自动获取正确的大小 (64位下应为 24)
RAWINPUTHEADER_SIZE = ctypes.sizeof(RAWINPUTHEADER)

# --- 定义 WNDCLASS ---
class WNDCLASS(ctypes.Structure):
    _fields_ = [("style", wintypes.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                ("hCursor", wintypes.HICON), ("hbrBackground", wintypes.HBRUSH), ("lpszMenuName", wintypes.LPCWSTR),
                ("lpszClassName", wintypes.LPCWSTR)]

































# 发送逻辑 (send_move)：修改函数签名以接受按键状态，并打包进协议。
def send_move(dx, dy, buttons=0, wheel=0):  # wheel是滚轮
    """发送移动及按键指令"""
    if not ser: return
    
    # 应用灵敏度
    dx = int(dx * SENSITIVITY)
    dy = int(dy * SENSITIVITY)
    # 安全限幅
    dx = max(-32000, min(32000, dx))
    dy = max(-32000, min(32000, dy))
    # 滚轮限幅 (int8 范围 -127 ~ 127)
    wheel = max(-127, min(127, int(wheel)))

    try:
        # 格式: '<BBhhBbB' (9字节) -> HEAD, CMD, DX, DY, BTN, WHEEL, TAIL
        # 注意: 'b' 代表 signed char (有符号字节)，用于滚轮
        packet = struct.pack('<BBhhBbB', 0xA5, 0x01, dx, dy, buttons, wheel, 0x5A)
        ser.write(packet)
        # 调试打印 (增加按键状态显示，如果嫌太刷屏可以注释掉)
        print(f"🚀 Move: {dx}, {dy} | Btn: {buttons:08b}") 
    except:
        pass


# 回调处理 (wnd_proc)
# 解析按键标志位，维护全局状态，并在发生按键事件时立即触发发送。
# 增加对 RI_MOUSE_WHEEL (0x0400) 的解析
def wnd_proc(hwnd, msg, wparam, lparam):

    global current_buttons # 引用全局变量

    if msg == 0x00FF:  # WM_INPUT
        data_size = wintypes.UINT(0)
        # 1. 获取数据大小
        res = user32.GetRawInputData(lparam, 0x10000003, None, ctypes.byref(data_size), RAWINPUTHEADER_SIZE)
        
        if data_size.value > 0:
            raw_data = ctypes.create_string_buffer(data_size.value)
            # 2. 获取实际数据
            res = user32.GetRawInputData(lparam, 0x10000003, raw_data, ctypes.byref(data_size), RAWINPUTHEADER_SIZE)
            
            # 解析
            if res > 0:
                raw = ctypes.cast(raw_data, ctypes.POINTER(RAWINPUT)).contents
                if raw.header.dwType == 0: # RIM_TYPEMOUSE
                    dx = raw.mouse.lLastX
                    dy = raw.mouse.lLastY
                    
                    # 获取按键标志位 (ulButtons 低16位包含 usButtonFlags)
                    flags = raw.mouse.u.ulButtons & 0xFFFF

                    # 记录旧状态用于对比
                    old_buttons = current_buttons

                    # === 按键状态机映射 ===
                    # 滚轮
                    wheel_step = 0
                    if flags & 0x0400: # RI_MOUSE_WHEEL
                        # 滚轮数据在 ulButtons 的高 16 位
                        # ctypes.c_short 强制转换处理负数 (向下滚动)
                        delta = ctypes.c_short((raw.mouse.u.ulButtons >> 16) & 0xFFFF).value
                        # Windows 标准刻度是 120，归一化为 1
                        wheel_step = int(delta / 120)
                    # 左键 (Bit 0)
                    if flags & 0x0001: current_buttons |= 1   # Down
                    if flags & 0x0002: current_buttons &= ~1  # Up
                    # 右键 (Bit 1)
                    if flags & 0x0004: current_buttons |= 2
                    if flags & 0x0008: current_buttons &= ~2
                    # 中键 (Bit 2)
                    if flags & 0x0010: current_buttons |= 4
                    if flags & 0x0020: current_buttons &= ~4
                    # 侧键1 (Bit 3) - 通常是 Back
                    if flags & 0x0040: current_buttons |= 8
                    if flags & 0x0080: current_buttons &= ~8
                    # 侧键2 (Bit 4) - 通常是 Forward
                    if flags & 0x0100: current_buttons |= 16
                    if flags & 0x0200: current_buttons &= ~16
                    
                    # 只有在功能开启时才发送数据
                    if titan_enabled :
                        # 逻辑：有移动 或 有按键标志位 时发送，还有加入 wheel_step != 0 的判断
                        if dx != 0 or dy != 0 or (flags & 0x03FF) or wheel_step != 0:
                            send_move(dx, dy, current_buttons, wheel_step)
                        
                        # # [诊断] 仅在按键状态发生实际改变时打印，避免刷屏
                        # if DEBUG_LOG :
                        #     if current_buttons != old_buttons:
                        #         print(f"🖱️ 按键动作! 状态码: {current_buttons:08b} (发送包中)")

    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)




















# --- 新增：光标限制相关定义 ---
class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]

def toggle_host_cursor(lock):
    """
    战术锁定：
    Lock=True  -> 将光标死锁在屏幕左上角 (0,0)，防止误触宿主机桌面
    Lock=False -> 释放光标，恢复正常操作
    """
    if lock:
        # 限制在 (0,0) 到 (1,1) 的 1 像素区域
        # 随便，别超了就行
        rect = RECT(1000,1000,1001,1001)
        user32.ClipCursor(ctypes.byref(rect))
        # 可选：如果你希望光标彻底消失，可以取消下面这行的注释
        # while user32.ShowCursor(False) >= 0: pass
    else:
        # 释放限制
        user32.ClipCursor(None)
        # while user32.ShowCursor(True) < 0: pass



























def main():
    global ser, titan_enabled, last_toggle_state

    print("💎 启动 Titan 最终修复版...")
    
    # 1. 连接硬件
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.01)
        print(f"✅ 串口 {SERIAL_PORT} 连接成功")
    except Exception as e:
        print(f"❌ 串口错误: {e}")
        return

    # 2. 硬件自检
    print("⚡ 发送自检跳动...")
    send_move(0, 50)
    
    # 3. 注册 Raw Input
    # 保持引用防止被垃圾回收
    proc = WNDPROC(wnd_proc)
    
    wndclass = WNDCLASS()
    wndclass.lpfnWndProc = proc
    wndclass.lpszClassName = "TitanFinalFix"
    wndclass.hInstance = kernel32.GetModuleHandleW(None)
    
    user32.RegisterClassW(ctypes.byref(wndclass))
    
    # 创建消息接收窗口
    hwnd = user32.CreateWindowExW(0, wndclass.lpszClassName, "Hidden", 0, 0, 0, 0, 0, 0, 0, 0, 0)
    
    # 注册设备
    class RID(ctypes.Structure):
        _fields_ = [("usUsagePage", wintypes.USHORT), ("usUsage", wintypes.USHORT), ("dwFlags", wintypes.DWORD), ("hwndTarget", wintypes.HWND)]
    
    # RIDEV_INPUTSINK (0x100) = 后台接收
    rid = RID(0x01, 0x02, 0x00000100, hwnd)
    
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
        print(f"❌ 注册失败, 错误码: {kernel32.GetLastError()}")
        return
        
    print("\n✅ 系统就绪！")
    print(f"⌨️  按 [Home] 键切换控制状态 (当前: {'开启' if titan_enabled else '关闭'})")
    print("🖱️ 现在移动台式机鼠标，笔记本应该会同步移动 (且无视屏幕边界)。")
    print("   (按 Ctrl+C 退出)")
    
    toggle_host_cursor(titan_enabled)

    # 4. 消息循环
    msg = wintypes.MSG()

    # 使用 PeekMessage 配合 while 循环，实现非阻塞的热键监听
    # 将 GetMessage 替换为 PeekMessage 模式，以免阻塞导致无法检测热键
    try:
        # 每秒循环约 1000 次（受 time.sleep(0.001) 限制），大部分时间都在“空转”检测有没有事发生
        while True:
            # 处理 Windows 消息
            while user32.PeekMessageW(ctypes.byref(msg), 0, 0, 0, 1) != 0:  # PM_REMOVE = 1
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            
            # # 必须在每一帧都强制重新锁定主机鼠标
            # if titan_enabled:
            #      toggle_host_cursor(True)

            # --- 热键检测逻辑 ---
            # 检测 TOGGLE_KEY (Home键)
            # GetAsyncKeyState 返回值的最高位表示当前是否按下
            key_down = (user32.GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0
            
            if key_down and not last_toggle_state:
                titan_enabled = not titan_enabled
                # 切换光标锁定状态
                toggle_host_cursor(titan_enabled)
                status = "[🟢 战斗模式 - 宿主机锁定]" if titan_enabled else "[🔴 桌面模式 - 宿主机释放]"
                print(f"🔄 状态切换: {status}")

            last_toggle_state = key_down
            time.sleep(0.001) # 避免 CPU 占用 100%
    finally:
        # [关键] 脚本退出/崩溃时，强制释放光标，否则你只能重启电脑
        print("\n⚠️ 正在释放光标...")
        toggle_host_cursor(False)
        if ser: ser.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        if ser: ser.close()