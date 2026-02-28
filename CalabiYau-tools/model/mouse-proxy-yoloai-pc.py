
import serial
import struct
import ctypes
from ctypes import wintypes
import sys
import time

import torch
# 启用 cudnn 自动寻找最适合当前硬件的算法
torch.backends.cudnn.benchmark = True
# 引入视觉所需库
import cv2
import numpy as np
from ultralytics import YOLO
import threading


# ================= 🔧 配置文件 (CONFIG) =================
SERIAL_PORT = 'COM7'    # # 确认这是正确的串口号，在设备管理器里看
BAUD_RATE   = 921600    # 提升至 92w 以通过 1000Hz 数据
SENSITIVITY = 1.0       # 鼠标透传倍率 (1.0 = 1:1)，觉得慢改成 2.0
# [热键配置]
# 0x24 = Home, 0x14 = CapsLock, 0x05 = 侧键(XBUTTON1)
TOGGLE_KEY  = 0x24      # 0x24 = Home 键 (控制透传的开启/关闭)
DEBUG_LOG   = False     # 关闭日志以减少 I/O 延迟，已经注释掉了，需要自己改
PERF_MONITOR_ENABLED = False  # 性能监控开关：不想测试时改为 False（或直接注释此行）
PERF_MONITOR_INTERVAL_SEC = 1.0  # 终端打印间隔（秒）

CAMERA_INDEX = 0        # 手动指定相机索引（避免误选 DroidCamera）
CAMERA_BACKEND = "DSHOW"  # 可选: DSHOW / MSMF / ANY
CAMERA_FOURCC = "YUY2"  # 可选: YUY2 / MJPG
CAMERA_WIDTH = 1920
CAMERA_HEIGHT = 1080
CAMERA_TARGET_FPS = 60

# [锁定调参] A/B 快速对比：只改 AIM_TUNE_PROFILE 并重启
# A = 稳定优先（推荐）
# B = 跟手优先（更强锁定）
# C = 关闭卡尔曼的对照组（用于判断“变软”是否由滤波导致）
# body_offset是锁敌的y轴偏移量；base_kp是基础灵敏度；kp_max是动态灵敏度上限，防止突增过高
AIM_TUNE_PROFILE = "A"  # 可选: A / B / C
AIM_KP_DIST_GAIN = 0.0001  # dynamic_kp（动态灵敏度）的距离增益，固定不变便于 A/B 比较
AIM_TUNE_PROFILES = {
    "A": {
        "label": "stable",
        "kalman_enabled": True,
        "base_kp": 0.012,
        "kp_max": 0.024,
        "body_offset": 0.30,
    },
    "B": {
        "label": "follow",
        "kalman_enabled": True,
        "base_kp": 0.015,
        "kp_max": 0.030,
        "body_offset": 0.26,
    },
    "C": {
        "label": "no_kalman",
        "kalman_enabled": False,
        "base_kp": 0.017,
        "kp_max": 0.032,
        "body_offset": 0.24,
    },
}

# 去参数微调修改灵敏度、分辨率
# 去找人逻辑修改图像识别置信度
# 报错了记得看下初始化相机设置

# ========================================================




# --- 全局变量 ---
serial_conn = None
current_buttons = 0       # 全局变量：当前鼠标按键状态位掩码
titan_enabled = True      # 全局变量：透传功能开关状态
last_toggle_state = True # 用于切换按键去抖



# ================= 卡尔曼滤波器类 =================
class SimpleKalman:
    def __init__(self):
        # 4个状态变量: [x, y, vx, vy] (坐标 + 速度)
        # 2个测量变量: [mx, my] (YOLO 看到的坐标)
        self.kf = cv2.KalmanFilter(4, 2)
        self.kf.measurementMatrix = np.array([[1,0,0,0], [0,1,0,0]], np.float32)
        self.kf.transitionMatrix = np.array([[1,0,1,0], [0,1,0,1], [0,0,1,0], [0,0,0,1]], np.float32)
        
        # --- 🔧 调参核心区域 (这里决定手感) ---
        # Q (Process Noise): 预测噪声。值越小，系统越相信预测（惯性大，更平滑，但变向反应慢）。
        # 值越大，系统越相信现实（反应快，但会抖）。
        # 建议：0.01 (极稳) ~ 0.1 (敏捷)
        self.kf.processNoiseCov = np.eye(4, dtype=np.float32) * 0.05

        # R (Measurement Noise): 测量噪声。值越大，表示我觉得 YOLO 越不准（会强力平滑抖动）。
        # 建议：0.1 ~ 10.0
        self.kf.measurementNoiseCov = np.eye(2, dtype=np.float32) * 0.1
        
        self.first_frame = True

    def update(self, x, y):
        # 第一次数据直接初始化，防止准星从 0,0 飞过来
        if self.first_frame:
            self.kf.statePre = np.array([[x], [y], [0], [0]], np.float32)
            self.kf.statePost = np.array([[x], [y], [0], [0]], np.float32)
            self.first_frame = False
        
        # 1. 测量 (Measurement)
        measurement = np.array([[np.float32(x)], [np.float32(y)]])
        
        # 2. 修正 (Correct)
        self.kf.correct(measurement)
        
        # 3. 预测 (Predict)
        prediction = self.kf.predict()
        
        # 获取速度 (Velocity)
        # statePost 的后两个值就是 vx, vy (像素/帧)
        vx = float(self.kf.statePost[2, 0])
        vy = float(self.kf.statePost[3, 0])

        # 返回预测的坐标 (px, py) + 速度
        return float(prediction[0, 0]), float(prediction[1, 0]), vx, vy
# ========================================================


# 定义共享状态类 (AI 的坐标往这里写)
class AimState:
    def __init__(self):
        self.dx = 0                  # AI 计算出的 X 轴修正量
        self.dy = 0                  # AI 计算出的 Y 轴修正量
        self.lock = threading.Lock() # 线程锁，防止多线程同时修改状态

    # 视觉线程用这个函数“写入”
    def update(self, dx, dy):
        with self.lock:
            self.dx = dx
            self.dy = dy

    # 主线程用这个函数“读取并清空”
    def consume(self):
        """取出当前修正量并重置 (避免重复叠加)"""
        with self.lock:
            x, y = self.dx, self.dy
            self.dx = 0 # 读完/取出后清零，防止一帧画面导致鼠标持续移动（震荡）
            self.dy = 0
            return x, y

# 实例化全局对象
aim_state = AimState()


# --- 性能监控模块（可选） ---
class PerfMonitor:
    def __init__(self, enabled=False, interval_sec=1.0):
        # enabled=False 时整个模块不输出统计
        self.enabled = enabled
        # 统计窗口长度（秒）
        self.interval_sec = interval_sec
        self.lock = threading.Lock()
        # 窗口起始时间
        self.window_start = time.perf_counter()
        # 采集帧计数
        self.capture_frames = 0
        # 推理次数与推理耗时累计
        self.infer_count = 0
        self.infer_ms_sum = 0.0
        # 帧龄（从采到开始推理的延迟）累计
        self.frame_age_count = 0
        self.frame_age_ms_sum = 0.0
        # 串口发送包计数
        self.send_packets = 0

    def on_capture(self):
        if not self.enabled:
            return
        with self.lock:
            self.capture_frames += 1

    def on_infer(self, infer_ms, frame_age_ms=None):
        if not self.enabled:
            return
        with self.lock:
            self.infer_count += 1
            self.infer_ms_sum += infer_ms
            if frame_age_ms is not None:
                self.frame_age_count += 1
                self.frame_age_ms_sum += max(frame_age_ms, 0.0)

    def on_send(self):
        if not self.enabled:
            return
        with self.lock:
            self.send_packets += 1

    def maybe_report(self):
        if not self.enabled:
            return

        now = time.perf_counter()
        report_line = None
        with self.lock:
            elapsed = now - self.window_start
            if elapsed < self.interval_sec:
                return

            capture_fps = self.capture_frames / elapsed
            infer_fps = self.infer_count / elapsed
            send_hz = self.send_packets / elapsed
            infer_avg_ms = (self.infer_ms_sum / self.infer_count) if self.infer_count else 0.0
            frame_age_avg_ms = (self.frame_age_ms_sum / self.frame_age_count) if self.frame_age_count else 0.0

            report_line = (
                f"📊 Perf | cap={capture_fps:.1f}fps | infer={infer_fps:.1f}fps "
                f"| infer_avg={infer_avg_ms:.2f}ms | frame_age={frame_age_avg_ms:.2f}ms "
                f"| send={send_hz:.1f}hz"
            )

            self.window_start = now
            self.capture_frames = 0
            self.infer_count = 0
            self.infer_ms_sum = 0.0
            self.frame_age_count = 0
            self.frame_age_ms_sum = 0.0
            self.send_packets = 0

        if report_line:
            print(report_line)


perf_monitor = PerfMonitor(
    enabled=PERF_MONITOR_ENABLED,
    interval_sec=PERF_MONITOR_INTERVAL_SEC,
)

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










# 新增一个全局变量，存放最新的一帧画面
latest_frame = None
# 该帧的采集时间戳（仅性能监控开启时更新）
latest_frame_ts = 0.0
frame_lock = threading.Lock()


# 1. 专门的采集线程：只管读，不管算
def camera_reader_worker(cap):
    global latest_frame, latest_frame_ts
    while cap.isOpened():
        ret, frame = cap.read()
        if not ret:
            # 相机偶发丢帧时避免空转占满 CPU
            time.sleep(0.005)
            continue
        with frame_lock:
            latest_frame = frame
            # 关闭性能监控时，不做计时调用，彻底跳过额外开销
            if PERF_MONITOR_ENABLED:
                latest_frame_ts = time.perf_counter()
        if PERF_MONITOR_ENABLED:
            perf_monitor.on_capture()
        # cap.read() 本身就会等待硬件信号（阻塞式），所以不需要额外的 sleep


def configure_camera_capture(cap):
    # 先设置分辨率，再设置帧率
    # 采集卡记得把这几行格式设置加上，否则延迟会很高
    f0, f1, f2, f3 = (CAMERA_FOURCC + "    ")[:4]
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(f0, f1, f2, f3))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAMERA_WIDTH)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAMERA_HEIGHT)
    cap.set(cv2.CAP_PROP_FPS, CAMERA_TARGET_FPS)
    # 设置缓冲区为1，防止积压旧帧 (降低物理延迟)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)


def get_camera_backend_value(name):
    backend_map = {
        "DSHOW": getattr(cv2, "CAP_DSHOW", None),
        "MSMF": getattr(cv2, "CAP_MSMF", None),
        "ANY": getattr(cv2, "CAP_ANY", None),
    }
    return backend_map.get(str(name).upper())

def get_aim_tune_profile(profile_key):
    key = str(profile_key).upper()
    profile = AIM_TUNE_PROFILES.get(key)
    if profile is None:
        key = "A"
        profile = AIM_TUNE_PROFILES[key]
        print(f"⚠️ AIM_TUNE_PROFILE={profile_key} 无效，回退到 {key}")
    return key, profile
























# 重点：视觉线程的工作函数
def vision_worker():

    profile_key, profile = get_aim_tune_profile(AIM_TUNE_PROFILE)
    # [参数微调]
    # 检测区域大小
    ROI_SIZE = 256
    # base_kp: 基础灵敏度。越大越跟手，越小越稳
    base_kp = float(profile["base_kp"])
    kp_max = float(profile["kp_max"])
    body_offset = float(profile["body_offset"])
    kalman_enabled = bool(profile["kalman_enabled"])

    # 压枪参数
    # 如果感觉压不住，把这个数字改大 (比如 3 或 4)
    # 如果压太低了，改小 (比如 0.1)
    RECOIL_STRENGTH = 0.4  
    # 延迟预判系数
    # 你的延迟大概 80ms (约 5 帧)，所以预判系数设为 4.0 ~ 6.0
    # 如果准星还是追在屁股后面 -> 调大 PREDICTION_FACTOR (试 6.0, 8.0)。
    PREDICTION_FACTOR = 4.0

    # 初始化卡尔曼滤波器（可由预设开关）
    kalman = SimpleKalman() if kalman_enabled else None

    # 累积器变量
    # 这两个变量就像“存钱罐”，把微小的浮点数存起来
    sx = 0.0
    sy = 0.0


    print("👁️ 视觉后台线程启动...")
    print(
        f"🎯 锁定预设 {profile_key}({profile['label']}) | "
        f"kalman={kalman_enabled} | base_kp={base_kp:.3f} | "
        f"kp_max={kp_max:.3f} | body_offset={body_offset:.2f}"
    )
    if PERF_MONITOR_ENABLED:
        print(f"📊 性能监控开启：每 {PERF_MONITOR_INTERVAL_SEC:.1f}s 打印一次（不测时可将 PERF_MONITOR_ENABLED 设为 False）")
    # 模型加载，确保模型文件在同一目录
    try:
        model = YOLO('kalabiqiu v8.engine', task='detect') 
    except:
        print("⚠️ 未找到 engine，回退到 pt 模式")
        model = YOLO('kalabiqiu v8.pt')


    # 初始化相机：按全局配置手动指定，避免自动选到错误设备
    backend_name = str(CAMERA_BACKEND).upper()
    backend_value = get_camera_backend_value(backend_name)
    if backend_value is None:
        print(f"❌ 相机后端配置无效: CAMERA_BACKEND={CAMERA_BACKEND}")
        print("   可选值: DSHOW / MSMF / ANY")
        return
    cap = cv2.VideoCapture(CAMERA_INDEX, backend_value)
    if not cap.isOpened():
        print(f"❌ 相机打开失败: backend={backend_name}, index={CAMERA_INDEX}")
        print("   请确认你手动指定的是采集卡设备，不是 DroidCamera。")
        return

    configure_camera_capture(cap)
    ret, _ = cap.read()
    if not ret:
        cap.release()
        print(f"❌ 相机读取失败: backend={backend_name}, index={CAMERA_INDEX}")
        return

    print(f"✅ 相机连接成功: index={CAMERA_INDEX}, backend={backend_name}")


    # ================= 格式与帧率诊断 =================
    # 获取当前实际工作的 FourCC 代码
    actual_fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
    # 将整数解码为 4 个字符 (例如 'MJPG' 或 'YUY2')
    codec_msg = "".join([chr((actual_fourcc >> 8 * i) & 0xFF) for i in range(4)]).upper().strip()
    actual_fps = cap.get(cv2.CAP_PROP_FPS)
    
    print(f"\n{'='*10} 📷 最终链路状态 {'='*10}")
    print(f"格式: {codec_msg} | 目标FPS: {CAMERA_TARGET_FPS} | 驱动报告: {actual_fps}")
    
    if codec_msg in ['YUY2', 'YUYV'] and actual_fps > 50:
        print("✅ YUY2 无损格式 + 60FPS 已激活！")
        print("   说明: 你的 USB 3.0 接口带宽充足，无需压缩，延迟最低。")
    elif codec_msg == 'MJPG':
        print("✅ [标准状态] MJPG 压缩格式 + 60FPS 已激活。")
    else:
        print(f"⚠️ [提示] 当前状态: {codec_msg} @ {actual_fps}FPS (只要流畅就没问题)")
    print(f"{'='*35}\n")
    # ========================================================


    # 启动采集线程（不断轮询往全局变量传最新的帧）
    t_reader = threading.Thread(target=camera_reader_worker, args=(cap,), daemon=True)
    t_reader.start()

    # 2. 预先计算裁剪坐标 (静态计算，移到循环外)
    # 裁剪中心区域 (ROI - Region of Interest)
    # 这是提升 FPS 的最关键逻辑。你的准星永远在屏幕中心 (960, 540)
    # 我们只关心准星周围 ROI_SIZE (如 256x256) 的区域
    center_x, center_y = 1920 // 2, 1080 // 2
    roi_x1 = center_x - (ROI_SIZE // 2)
    roi_y1 = center_y - (ROI_SIZE // 2)
    roi_x2 = roi_x1 + ROI_SIZE  # 提前算好右下角
    roi_y2 = roi_y1 + ROI_SIZE

    while True:  # 如果主开关没开，视觉就休息一下，省CPU
        if not titan_enabled:
            time.sleep(0.1)
            # 暂停时清空累积器，防止攒了一堆过期数据突然爆发
            sx = 0.0
            sy = 0.0
            continue

        # === 从全局变量拿最新的一帧 ===
        # 优化: 只拷贝 ROI
        roi_frame = None
        # 仅在监控开启时追踪这帧的采集时间
        if PERF_MONITOR_ENABLED:
            capture_ts = 0.0
        with frame_lock:
            if latest_frame is not None:
                # 关键修改：直接在全局大图上切片，然后只 .copy() 这一小块，降低耗时
                # 且能迅速释放锁，让采集线程继续工作
                # 这一步把采集卡的 1920x1080 (200万像素) 的一帧变成了 256x256 (6.5万像素) 给到 roi_frame。
                # 在 while 循环内，对“当下的 frame”进行切片。
                roi_frame = latest_frame[roi_y1:roi_y2, roi_x1:roi_x2].copy()
                if PERF_MONITOR_ENABLED:
                    capture_ts = latest_frame_ts

        # 如果还没采到图，或者切片失败
        if roi_frame is None:
            time.sleep(0.001)
            continue

        # 推理与筛选 (Inference & Filtering)
        # 关闭监控时，不做 infer_start 计时
        if PERF_MONITOR_ENABLED:
            infer_start = time.perf_counter()
        # roi_frame 是 224x224，直接喂给模型，拿框
        # conf=0.4是置信度, 锁头就指定classes=[1]，锁身体就是0
        # verbose=False 防止控制台刷屏
        results = model(roi_frame, imgsz=ROI_SIZE, verbose=False, conf=0.45, classes=[0, 1], max_det=10)

        if PERF_MONITOR_ENABLED:
            infer_ms = (time.perf_counter() - infer_start) * 1000.0
            frame_age_ms = ((infer_start - capture_ts) * 1000.0) if capture_ts > 0.0 else None
            perf_monitor.on_infer(infer_ms=infer_ms, frame_age_ms=frame_age_ms)
            perf_monitor.maybe_report()

        # =========== 智能找目标逻辑 =============
        target_box = None
        target_type = 'none' # 用于调试，显示当前锁的是啥

        # 1. 先把检测到的框分类
        heads = []
        bodies = []
        
        if results[0].boxes is not None:
            for box in results[0].boxes:
                cls_id = int(box.cls[0])
                # 假设 1=头, 0=身体 (如果不准，请交换这两个数字)
                if cls_id == 1:
                    heads.append(box.xyxy[0].cpu().numpy())
                elif cls_id == 0:
                    bodies.append(box.xyxy[0].cpu().numpy())

        # 2. 优先级决策
        # 策略：有头锁头，没头锁身体的脖子位置
        if len(heads) > 0:
            # === 情况 A: 发现头框 ===
            # 找离准星最近的一个头 (防止锁到旁边的人)
            # 这里简化处理，直接取置信度最高的头(通常是列表第一个)
            target_box = heads[0] 
            target_type = 'head'
            
            x1, y1, x2, y2 = target_box
            target_cx = (x1 + x2) / 2
            body_height = y2 - y1
            target_cy = (y1 + y2) / 2 + (body_height * 0.5)   # 给个偏移
            
        elif len(bodies) > 0:
            # === 情况 B: 只有身体 (头被遮挡或没识别到) ===
            target_box = bodies[0]
            target_type = 'body'
            
            x1, y1, x2, y2 = target_box
            target_cx = (x1 + x2) / 2
            
            # 身体修正 (Body Offset)
            # 我们不锁身体中心 (肚脐眼)，我们要锁"脖子"
            # 算法：中心 Y 减去 身体高度的 35% ~ 40%
            body_height = y2 - y1
            target_cy = ((y1 + y2) / 2) - (body_height * body_offset) 

        else:
            # 啥都没看到
            target_box = None


        # PID 控制与坐标转换 (Control Logic)
          
        # 计算移动量：将“看到的位置”转化为“鼠标移动的距离”。
        if target_box is not None:
            
            # === 卡尔曼滤波介入 ===
            # 我们把"抖动"的坐标喂给卡尔曼，吐出来"平滑"的坐标 和 速度(vx, vy)
            if kalman_enabled:
                smooth_cx, smooth_cy, vx, vy = kalman.update(target_cx, target_cy)
            else:
                smooth_cx, smooth_cy = target_cx, target_cy
                vx, vy = 0.0, 0.0
            # =====================
            
            # ================= 延迟预判 (Prediction) =================
            # 原理：目标实际位置 = 当前看到的平滑位置 + (速度 * 延迟帧数)
            # 这能让你直接锁在敌人“未来”的位置，解决追尾问题
            predicted_x = smooth_cx + (vx * PREDICTION_FACTOR)
            predicted_y = smooth_cy + (vy * PREDICTION_FACTOR)
            
            # 使用“预判坐标”来计算 diff
            # ROI 的中心就是 (ROI_SIZE / 2)
            # 计算差距：目标在哪里 - 中心在哪里。
            diff_x = predicted_x - (ROI_SIZE // 2)
            diff_y = predicted_y - (ROI_SIZE // 2) + 0  # 这里不需要额外的固定抬枪了，因为上面已经算过 Body Offset

            # ================= 动态灵敏度算法 =================
            # 1. 计算准星离目标的“直线距离” (欧几里得距离)
            dist = (diff_x**2 + diff_y**2)**0.5

            # 定义两个档位的灵敏度
            
            # dynamic_kp: 动态灵敏度。当距离越远，这个值介入越多，帮你快速拉枪。
            # 距离每增加 1 像素，KP 增加 0.0001 (下面自己定义)
            dynamic_kp = base_kp + (dist * AIM_KP_DIST_GAIN)

            # [安全锁] 给 KP 设个上限，防止甩太猛飞出去了
            if dynamic_kp > kp_max:
                dynamic_kp = kp_max

            # 3. 计算这一帧的浮点移动量(比如 0.23)
            fx = diff_x * dynamic_kp
            fy = diff_y * dynamic_kp

            # ================= 自动压枪提前计算 =================
            # 必须在 sx/sy 累积之前把压枪的值加进去，这样 0.5 才能被攒起来
            
            import __main__ 
            if (__main__.current_buttons & 1): # 检测左键
                # 直接加在浮点数 fy 上，让累积器去处理小数
                fy += RECOIL_STRENGTH
                
            # ================= 累积器控制逻辑 =================
            # 存进“存钱罐”: 这里的 sx, sy 是你在 while 外面定义的那个累积器变量
            sx += fx
            sy += fy

            # 看看能不能提出整数 (int(sx) 会把1.2 变成 1，把 0.9 变成 0)
            move_x = int(sx)
            move_y = int(sy)

            # 把提出来的整数从存钱罐里扣掉 (1.2 - 1 = 0.2，剩下的小数留给下一帧)
            sx -= move_x
            sy -= move_y
            # ===============================


            # 写入共享内存
            aim_state.update(move_x, move_y)

        else:
            # 丢失目标时，慢慢衰减累积器（可选），这里直接清零比较安全
            sx = 0.0
            sy = 0.0
            aim_state.update(0, 0)





















# 发送逻辑 (send_move)：修改函数签名以接受按键状态，并打包进协议。在下面的wnd_proc函数里会用到
def send_move(dx, dy, buttons=0, wheel=0):  # wheel是滚轮
    """发送移动及按键指令"""
    if not serial_conn: return
    
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
        # 注意: 'b' 代表 signed char (有符号字节，用于滚轮)
        packet = struct.pack('<BBhhBbB', 0xA5, 0x01, dx, dy, buttons, wheel, 0x5A)
        serial_conn.write(packet)
        # 关闭监控时，完全跳过发送计数
        if PERF_MONITOR_ENABLED:
            perf_monitor.on_send()
        # 调试打印 (增加按键状态显示)
        if DEBUG_LOG:
            print(f"🚀 Move: {dx}, {dy} | Btn: {buttons:08b}") 
    except:
        pass


# 回调处理 (wnd_proc)
# 解析按键标志位，维护全局状态，并在发生按键事件时立即触发发送。
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
                        # 1. 从邮箱里取 AI 计算出的位移
                        ai_dx, ai_dy = aim_state.consume()
                        
                        # 2. 这里的策略是：不管开不开镜，只要 titan_enabled 开了就介入
                        # 如果你想仅在右键时介入，可以加： if (current_buttons & 2): ...
                        
                        # 3. 最终移动量 = 手的移动(dx) + AI的位移(ai_dx)
                        final_dx = dx + ai_dx
                        final_dy = dy + ai_dy

                        # 逻辑：有移动(物理或AI) 或有按键标志位 时发送
                        if final_dx != 0 or final_dy != 0 or (flags & 0x03FF) or wheel_step != 0:
                            send_move(final_dx, final_dy, current_buttons, wheel_step)

    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

















# --- 鼠标控制开关，光标限制范围相关定义 ---
class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]

def toggle_host_cursor(lock):
    """
    Lock=True  -> 将光标死锁在屏幕某位置(0,0)，防止误触宿主机桌面
    Lock=False -> 释放光标，恢复正常操作
    """
    if lock:
        # 限制在 (1000,1000) 到 (1001,1001) 的 1 像素区域
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
    global serial_conn, titan_enabled, last_toggle_state

    print("💎 启动 Titan 最终修复版...")
    
    # 1. 连接硬件
    try:
        serial_conn = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.01)
        print(f"✅ 串口 {SERIAL_PORT} 连接成功")
    except Exception as e:
        print(f"❌ 串口错误: {e}")
        return

    # 2. 硬件自检
    print("🧪 发送自检跳动...")
    send_move(0, 50)
    
    # 启动视觉分身
    t = threading.Thread(target=vision_worker, daemon=True)
    t.start()
    print("🚀 视觉引导已加载，随时准备介入...")


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
        
    print("\n✅ 系统就绪。")
    print(f"⌨️ [Home] 键切换控制状态 (当前: {'开启' if titan_enabled else '关闭'})")
    print("🖱️ 现在移动台式机鼠标，笔记本应该会同步移动（且无视屏幕边界）。")
    print("   (按 Ctrl+C 退出)")
    
    toggle_host_cursor(titan_enabled)

    # 4. 消息循环
    msg = wintypes.MSG()

    # 使用 PeekMessage 配合 while 循环，实现非阻塞的热键监听
    # 将 GetMessage 替换为 PeekMessage 模式，以免阻塞导致无法检测热键
    try:
        # 每秒循环约 1000 次（由 time.sleep(0.001) 限制），大部分时间都在“空转”检测有没有事件
        while True:
            # 处理 Windows 消息
            while user32.PeekMessageW(ctypes.byref(msg), 0, 0, 0, 1) != 0:  # PM_REMOVE = 1
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            

            # === 2. [新增/主动] 每一帧主动检查 AI (你的手没动，但 AI 要动) ===
            if titan_enabled:
                # 主动去邮箱看看有没有囤积的 AI 数据
                # 这里涉及线程锁，只有启用时才进来
                ai_dx, ai_dy = aim_state.consume()
                
                # 如果有 AI 指令 (且刚才没被 wnd_proc 顺手带走)
                # 只有真正需要移动时才调用串口，节省 USB 带宽
                if ai_dx != 0 or ai_dy != 0:
                    send_move(ai_dx, ai_dy, current_buttons, 0)


            # # 必须在每一帧都强制重新锁定主机鼠标
            # if titan_enabled:
            #      toggle_host_cursor(True)

            # --- 热键检测逻辑 ---
            # 检测 TOGGLE_KEY (Home 键)
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
        print("\n⚠️ 正在释放光标...")
        # 脚本退出/崩溃时，强制释放光标，否则你只能重启电脑
        toggle_host_cursor(False)
        # 退出时关闭串口
        if serial_conn: serial_conn.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        # 退出时关闭串口
        if serial_conn: serial_conn.close()
