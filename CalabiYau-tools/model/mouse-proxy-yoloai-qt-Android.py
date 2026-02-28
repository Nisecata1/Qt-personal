# 从模块中导入指定对象，供后续逻辑调用。
from __future__ import annotations

import mmap
import socket
import struct
import time
# 从模块中导入指定对象，供后续逻辑调用。
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import torch
from ultralytics import YOLO

# Let cudnn pick fast kernels for current device.
# 启用 cudnn 自动算子选择，以提升推理速度。
torch.backends.cudnn.benchmark = True

# ================= Protocol Constants =================
# 配置共享内存对象名，必须与 C++ 发布端保持完全一致。
SHARED_MAPPING_NAME = r"Local\ScrcpyAIVision"
# 配置中心 ROI 的边长上限，单位为像素。
MAX_ROI_SIZE = 256
# 计算最大像素载荷字节数，BGR24 每像素占 3 字节。
MAX_PAYLOAD_BYTES = MAX_ROI_SIZE * MAX_ROI_SIZE * 3

# 定义共享内存头的 struct 小端解析格式。
HEADER_FMT = "<IHHIIIIHHHHQII"
# 计算共享内存头的字节大小并用于一致性校验。
HEADER_SIZE = struct.calcsize(HEADER_FMT)
# 定义 seq 字段在头结构中的字节偏移。
SEQ_OFFSET = 8
# 定义共享内存头魔数，用于快速识别协议。
HEADER_MAGIC = 0x56414353  # "SCAV"
# 定义共享内存协议版本号。
HEADER_VERSION = 1
# 定义共享内存头期望长度，防止结构漂移。
HEADER_EXPECTED_BYTES = 48
# 定义像素格式常量，当前约定为 BGR24。
PIXEL_FORMAT_BGR24 = 1
# 计算共享内存总长度（头 + 最大载荷）。
MAPPING_SIZE = HEADER_EXPECTED_BYTES + MAX_PAYLOAD_BYTES

# 定义 UDP AI 增量包的小端打包格式。
PACKET_FMT = "<IHHIff"
# 计算 UDP 包固定长度并在启动时校验。
PACKET_SIZE = struct.calcsize(PACKET_FMT)
# 定义 UDP 包魔数，用于过滤无效数据。
PACKET_MAGIC = 0x31444941  # "AID1"
# 定义 UDP 包版本号。
PACKET_VERSION = 1
# 定义 flags 的 bit0，表示当前帧是否存在目标。
FLAG_HAS_TARGET = 0x0001
# 配置 UDP 回传目标主机地址。端口
UDP_HOST = "127.0.0.1"
UDP_PORT = 12345

# 根据条件判断选择执行分支。
if HEADER_SIZE != HEADER_EXPECTED_BYTES:
    # 主动抛出异常，明确标记不可继续的状态。
    raise RuntimeError(f"Header size mismatch: {HEADER_SIZE} != {HEADER_EXPECTED_BYTES}")
if PACKET_SIZE != 20:
    raise RuntimeError(f"UDP packet size mismatch: {PACKET_SIZE} != 20")

# ================= Runtime Config =================
# 配置空转时让出 CPU 的睡眠时长（秒）。
POLL_SLEEP_SEC = 0.001
# 配置性能监控总开关。
PERF_MONITOR_ENABLED = False
# 配置性能统计打印周期（秒）。
PERF_MONITOR_INTERVAL_SEC = 1.0

# 配置当前使用的锁定参数预设档位。
AIM_TUNE_PROFILE = "A"  # A / B / C
# 配置动态 kp 的距离增益系数。
AIM_KP_DIST_GAIN = 0.001
# 定义不同档位的锁定参数集合。
AIM_TUNE_PROFILES = {
   
    "A": {
        "label": "stable",
        "kalman_enabled": True,
        "base_kp": 0.015,
        "kp_max": 0.05,
        "body_offset": 0.50,
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
        "base_kp": 0.02,
        "kp_max": 0.06,
        "body_offset": 0.34,
    },
}

# 配置速度外推系数，用于抵消链路延迟。
PREDICTION_FACTOR = 4.0
# 配置 YOLO 置信度阈值。
YOLO_CONF = 0.45
# 配置参与检测的类别 ID 白名单。
YOLO_CLASSES = [0, 1]
# 配置单帧最大检测框数量。
YOLO_MAX_DET = 10
# 配置 TensorRT engine 模型文件名。
MODEL_ENGINE_NAME = "kalabiqiu v8.engine"
# 配置 PyTorch pt 模型回退文件名。
MODEL_PT_NAME = "kalabiqiu v8.pt"


# 定义类 ReaderStatus，用于封装该职责模块。
class ReaderStatus:
    # 设置常量 OK 的默认值。
    OK = "ok"
    # 设置常量 MAPPING_UNAVAILABLE 的默认值。
    MAPPING_UNAVAILABLE = "mapping_unavailable"
    # 设置常量 SEQ_UNSTABLE 的默认值。
    SEQ_UNSTABLE = "seq_unstable"
    # 设置常量 FRAME_UNCHANGED 的默认值。
    FRAME_UNCHANGED = "frame_unchanged"
    # 设置常量 HEADER_INVALID 的默认值。
    HEADER_INVALID = "header_invalid"


# 声明装饰器，用于修改后续定义的行为。
@dataclass
# 定义类 VisionFrame，用于封装该职责模块。
class VisionFrame:
   
    frame_id: int
   
    width: int
   
    height: int
   
    timestamp_us: int
   
    frame_bgr: np.ndarray


# 定义类 SimpleKalman，用于封装该职责模块。
class SimpleKalman:
    # 定义函数 __init__，承载对应功能流程。
    def __init__(self) -> None:
       
        self.kf = cv2.KalmanFilter(4, 2)
       
        self.kf.measurementMatrix = np.array([[1, 0, 0, 0], [0, 1, 0, 0]], np.float32)
       
        self.kf.transitionMatrix = np.array(
            # 开始多行表达式或复合结构。
            [[1, 0, 1, 0], [0, 1, 0, 1], [0, 0, 1, 0], [0, 0, 0, 1]],
           
            np.float32,
        )
       
        self.kf.processNoiseCov = np.eye(4, dtype=np.float32) * 0.05
       
        self.kf.measurementNoiseCov = np.eye(2, dtype=np.float32) * 0.1
       
        self.first_frame = True

    # 定义函数 reset，承载对应功能流程。
    def reset(self) -> None:
       
        self.first_frame = True

    # 定义函数 update，承载对应功能流程。
    def update(self, x: float, y: float) -> tuple[float, float, float, float]:
        # 根据条件判断选择执行分支。
        if self.first_frame:
           
            self.kf.statePre = np.array([[x], [y], [0], [0]], np.float32)
           
            self.kf.statePost = np.array([[x], [y], [0], [0]], np.float32)
           
            self.first_frame = False

        # 设置常量 measurement 的默认值。
        measurement = np.array([[np.float32(x)], [np.float32(y)]])
       
        self.kf.correct(measurement)
        # 设置常量 prediction 的默认值。
        prediction = self.kf.predict()

        # 设置常量 vx 的默认值。
        vx = float(self.kf.statePost[2, 0])
        # 设置常量 vy 的默认值。
        vy = float(self.kf.statePost[3, 0])
        # 返回当前结果并结束函数执行。
        return float(prediction[0, 0]), float(prediction[1, 0]), vx, vy


# 定义类 PerfMonitor，用于封装该职责模块。
class PerfMonitor:
    # 定义函数 __init__，承载对应功能流程。
    def __init__(self, enabled: bool, interval_sec: float) -> None:
       
        self.enabled = enabled
       
        self.interval_sec = interval_sec
       
        self.window_start = time.perf_counter()
       
        self.reader_ok = 0
       
        self.reader_stale = 0
       
        self.reader_seq_fail = 0
       
        self.reader_invalid = 0
       
        self.reader_no_map = 0
       
        self.infer_count = 0
       
        self.infer_ms_sum = 0.0
       
        self.send_ok = 0
       
        self.send_fail = 0
       
        self.release_packets = 0

    # 定义函数 on_reader，承载对应功能流程。
    def on_reader(self, status: str) -> None:
        # 根据条件判断选择执行分支。
        if not self.enabled:
            # 返回当前结果并结束函数执行。
            return
        # 根据条件判断选择执行分支。
        if status == ReaderStatus.OK:
           
            self.reader_ok += 1
        # 在前置条件不满足时继续进行分支判断。
        elif status == ReaderStatus.FRAME_UNCHANGED:
           
            self.reader_stale += 1
        # 在前置条件不满足时继续进行分支判断。
        elif status == ReaderStatus.SEQ_UNSTABLE:
           
            self.reader_seq_fail += 1
        # 在前置条件不满足时继续进行分支判断。
        elif status == ReaderStatus.HEADER_INVALID:
           
            self.reader_invalid += 1
        # 在前置条件不满足时继续进行分支判断。
        elif status == ReaderStatus.MAPPING_UNAVAILABLE:
           
            self.reader_no_map += 1

    # 定义函数 on_infer，承载对应功能流程。
    def on_infer(self, infer_ms: float) -> None:
        # 根据条件判断选择执行分支。
        if not self.enabled:
            # 返回当前结果并结束函数执行。
            return
       
        self.infer_count += 1
       
        self.infer_ms_sum += infer_ms

    # 定义函数 on_send，承载对应功能流程。
    def on_send(self, ok: bool, release: bool = False) -> None:
        # 根据条件判断选择执行分支。
        if not self.enabled:
            # 返回当前结果并结束函数执行。
            return
        # 根据条件判断选择执行分支。
        if ok:
           
            self.send_ok += 1
        # 执行兜底分支以覆盖其余场景。
        else:
           
            self.send_fail += 1
        # 根据条件判断选择执行分支。
        if release:
           
            self.release_packets += 1

    # 定义函数 maybe_report，承载对应功能流程。
    def maybe_report(self) -> None:
        # 根据条件判断选择执行分支。
        if not self.enabled:
            # 返回当前结果并结束函数执行。
            return

        # 设置常量 now 的默认值。
        now = time.perf_counter()
        # 设置常量 elapsed 的默认值。
        elapsed = now - self.window_start
        # 根据条件判断选择执行分支。
        if elapsed < self.interval_sec:
            # 返回当前结果并结束函数执行。
            return

        # 设置常量 infer_avg 的默认值。
        infer_avg = self.infer_ms_sum / self.infer_count if self.infer_count else 0.0
        # 输出关键状态到控制台，便于运行期观测。
        print(
           
            "📊 Perf"
           
            f" | read_ok={self.reader_ok}"
           
            f" | stale={self.reader_stale}"
           
            f" | seq_fail={self.reader_seq_fail}"
           
            f" | invalid={self.reader_invalid}"
           
            f" | no_map={self.reader_no_map}"
           
            f" | infer={self.infer_count / elapsed:.1f}fps"
           
            f" | infer_avg={infer_avg:.2f}ms"
           
            f" | send_ok={self.send_ok}"
           
            f" | send_fail={self.send_fail}"
           
            f" | release={self.release_packets}"
        )

       
        self.window_start = now
       
        self.reader_ok = 0
       
        self.reader_stale = 0
       
        self.reader_seq_fail = 0
       
        self.reader_invalid = 0
       
        self.reader_no_map = 0
       
        self.infer_count = 0
       
        self.infer_ms_sum = 0.0
       
        self.send_ok = 0
       
        self.send_fail = 0
       
        self.release_packets = 0


# 定义类 SharedVisionReader，用于封装该职责模块。
class SharedVisionReader:
    # 定义函数 __init__，承载对应功能流程。
    def __init__(self, mapping_name: str, mapping_size: int) -> None:
       
        self.mapping_name = mapping_name
       
        self.mapping_size = mapping_size
       
        self.mm: mmap.mmap | None = None
       
        self.last_frame_id = 0

    # 定义函数 _ensure_mapping，承载对应功能流程。
    def _ensure_mapping(self) -> bool:
        # 根据条件判断选择执行分支。
        if self.mm is not None:
            # 返回当前结果并结束函数执行。
            return True
        # 进入异常保护区，避免单点错误中断流程。
        try:
           
            self.mm = mmap.mmap(-1, self.mapping_size, tagname=self.mapping_name, access=mmap.ACCESS_READ)
            # 返回当前结果并结束函数执行。
            return True
        # 捕获异常并执行容错处理。
        except OSError:
           
            self.mm = None
            # 返回当前结果并结束函数执行。
            return False

    # 定义函数 close，承载对应功能流程。
    def close(self) -> None:
        # 根据条件判断选择执行分支。
        if self.mm is not None:
           
            self.mm.close()
           
            self.mm = None

    # 定义函数 read_latest，承载对应功能流程。
    def read_latest(self) -> tuple[str, VisionFrame | None]:
        # 根据条件判断选择执行分支。
        if not self._ensure_mapping():
            # 返回当前结果并结束函数执行。
            return ReaderStatus.MAPPING_UNAVAILABLE, None

        # 断言关键前提，帮助快速发现异常状态。
        assert self.mm is not None

        # 设置常量 seq1 的默认值。
        seq1 = struct.unpack_from("<I", self.mm, SEQ_OFFSET)[0]
        # 根据条件判断选择执行分支。
        if seq1 & 1:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.SEQ_UNSTABLE, None

        # 设置常量 header_blob 的默认值。
        header_blob = self.mm[:HEADER_SIZE]
        # 开始多行表达式或复合结构。
        (
           
            magic,
           
            version,
           
            header_bytes,
           
            _seq,
           
            frame_id,
           
            _roi_x,
           
            _roi_y,
           
            width,
           
            height,
           
            stride,
           
            pixel_format,
           
            timestamp_us,
           
            payload_bytes,
           
            _reserved,
       
        ) = struct.unpack_from(HEADER_FMT, header_blob, 0)

        # 根据条件判断选择执行分支。
        if (
           
            magic != HEADER_MAGIC
           
            or version != HEADER_VERSION
           
            or header_bytes != HEADER_EXPECTED_BYTES
           
            or pixel_format != PIXEL_FORMAT_BGR24
       
        ):
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 根据条件判断选择执行分支。
        if frame_id == self.last_frame_id:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.FRAME_UNCHANGED, None

        # 根据条件判断选择执行分支。
        if width < 2 or height < 2:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 设置常量 active_bytes_per_row 的默认值。
        active_bytes_per_row = width * 3
        # 根据条件判断选择执行分支。
        if stride < active_bytes_per_row:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 设置常量 expected_payload 的默认值。
        expected_payload = stride * height
        # 根据条件判断选择执行分支。
        if payload_bytes != expected_payload:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 根据条件判断选择执行分支。
        if payload_bytes <= 0 or payload_bytes > MAX_PAYLOAD_BYTES:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 设置常量 payload_end 的默认值。
        payload_end = HEADER_SIZE + payload_bytes
        # 根据条件判断选择执行分支。
        if payload_end > self.mapping_size:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 设置常量 payload 的默认值。
        payload = self.mm[HEADER_SIZE:payload_end]

        # 设置常量 seq2 的默认值。
        seq2 = struct.unpack_from("<I", self.mm, SEQ_OFFSET)[0]
        # 根据条件判断选择执行分支。
        if seq1 != seq2 or (seq2 & 1):
            # 返回当前结果并结束函数执行。
            return ReaderStatus.SEQ_UNSTABLE, None

        # 设置常量 raw 的默认值。
        raw = np.frombuffer(payload, dtype=np.uint8)
        # 根据条件判断选择执行分支。
        if raw.size < expected_payload:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

        # 进入异常保护区，避免单点错误中断流程。
        try:
            # 设置常量 frame_2d 的默认值。
            frame_2d = raw[:expected_payload].reshape((height, stride))
            # 设置常量 frame_bgr 的默认值。
            frame_bgr = frame_2d[:, :active_bytes_per_row].reshape((height, width, 3)).copy()
        # 捕获异常并执行容错处理。
        except ValueError:
            # 返回当前结果并结束函数执行。
            return ReaderStatus.HEADER_INVALID, None

       
        self.last_frame_id = frame_id
        # 返回当前结果并结束函数执行。
        return ReaderStatus.OK, VisionFrame(
            # 设置常量 frame_id 的默认值。
            frame_id=frame_id,
            # 设置常量 width 的默认值。
            width=width,
            # 设置常量 height 的默认值。
            height=height,
            # 设置常量 timestamp_us 的默认值。
            timestamp_us=timestamp_us,
            # 设置常量 frame_bgr 的默认值。
            frame_bgr=frame_bgr,
        )


# 定义类 AiUdpSender，用于封装该职责模块。
class AiUdpSender:
    # 定义函数 __init__，承载对应功能流程。
    def __init__(self, host: str, port: int) -> None:
       
        self.addr = (host, port)
       
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
       
        self.last_error_log_ts = 0.0

    # 定义函数 close，承载对应功能流程。
    def close(self) -> None:
       
        self.sock.close()

    # 定义函数 send，承载对应功能流程。
    def send(self, frame_id: int, has_target: bool, ai_dx: float, ai_dy: float) -> bool:
        # 设置常量 flags 的默认值。
        flags = FLAG_HAS_TARGET if has_target else 0
        # 设置常量 packet 的默认值。
        packet = struct.pack(
           
            PACKET_FMT,
           
            PACKET_MAGIC,
           
            PACKET_VERSION,
           
            flags,
           
            frame_id & 0xFFFFFFFF,
           
            float(ai_dx),
           
            float(ai_dy),
        )
        # 进入异常保护区，避免单点错误中断流程。
        try:
           
            self.sock.sendto(packet, self.addr)
            # 返回当前结果并结束函数执行。
            return True
        # 捕获异常并执行容错处理。
        except OSError as exc:
            # 设置常量 now 的默认值。
            now = time.perf_counter()
            # 根据条件判断选择执行分支。
            if now - self.last_error_log_ts >= 1.0:
                # 输出关键状态到控制台，便于运行期观测。
                print(f"⚠️ UDP send failed: {exc}")
               
                self.last_error_log_ts = now
            # 返回当前结果并结束函数执行。
            return False


# 定义类 AimController，用于封装该职责模块。
class AimController:
    # 定义函数 __init__，承载对应功能流程。
    def __init__(self, profile: dict[str, float | bool | str]) -> None:
       
        self.base_kp = float(profile["base_kp"])
       
        self.kp_max = float(profile["kp_max"])
       
        self.body_offset = float(profile["body_offset"])
        # 设置常量 kalman_enabled 的默认值。
        kalman_enabled = bool(profile["kalman_enabled"])
       
        self.kalman = SimpleKalman() if kalman_enabled else None
       
        self.sx = 0.0
       
        self.sy = 0.0

    # 定义函数 _reset，承载对应功能流程。
    def _reset(self) -> None:
       
        self.sx = 0.0
       
        self.sy = 0.0
        # 根据条件判断选择执行分支。
        if self.kalman is not None:
           
            self.kalman.reset()

    # 定义函数 compute，承载对应功能流程。
    def compute(self, result, width: int, height: int) -> tuple[bool, float, float]:
        # 设置常量 boxes 的默认值。
        boxes = result.boxes
        # 根据条件判断选择执行分支。
        if boxes is None or len(boxes) == 0:
           
            self._reset()
            # 返回当前结果并结束函数执行。
            return False, 0.0, 0.0

       
        heads: list[tuple[float, np.ndarray]] = []
       
        bodies: list[tuple[float, np.ndarray]] = []

        # 遍历集合元素并逐个处理。
        for box in boxes:
            # 设置常量 cls_id 的默认值。
            cls_id = int(box.cls[0])
            # 设置常量 conf 的默认值。
            conf = float(box.conf[0]) if box.conf is not None else 0.0
            # 设置常量 xyxy 的默认值。
            xyxy = box.xyxy[0].cpu().numpy()
            # 根据条件判断选择执行分支。
            if cls_id == 1:
               
                heads.append((conf, xyxy))
            # 在前置条件不满足时继续进行分支判断。
            elif cls_id == 0:
               
                bodies.append((conf, xyxy))

        # 根据条件判断选择执行分支。
        if heads:
           
            _, target_box = max(heads, key=lambda x: x[0])
           
            x1, y1, x2, y2 = target_box
            # 设置常量 target_cx 的默认值。
            target_cx = (x1 + x2) * 0.5
            # 设置常量 body_height 的默认值。
            body_height = y2 - y1
            # 设置常量 target_cy 的默认值。
            target_cy = ((y1 + y2) * 0.5) + (body_height * 0.5)
        # 在前置条件不满足时继续进行分支判断。
        elif bodies:
           
            _, target_box = max(bodies, key=lambda x: x[0])
           
            x1, y1, x2, y2 = target_box
            # 设置常量 target_cx 的默认值。
            target_cx = (x1 + x2) * 0.5
            # 设置常量 body_height 的默认值。
            body_height = y2 - y1
            # 设置常量 target_cy 的默认值。
            target_cy = ((y1 + y2) * 0.5) - (body_height * self.body_offset)
        # 执行兜底分支以覆盖其余场景。
        else:
           
            self._reset()
            # 返回当前结果并结束函数执行。
            return False, 0.0, 0.0

        # 根据条件判断选择执行分支。
        if self.kalman is not None:
           
            smooth_cx, smooth_cy, vx, vy = self.kalman.update(float(target_cx), float(target_cy))
        # 执行兜底分支以覆盖其余场景。
        else:
           
            smooth_cx, smooth_cy = float(target_cx), float(target_cy)
           
            vx, vy = 0.0, 0.0

        # 设置常量 predicted_x 的默认值。
        predicted_x = smooth_cx + (vx * PREDICTION_FACTOR)
        # 设置常量 predicted_y 的默认值。
        predicted_y = smooth_cy + (vy * PREDICTION_FACTOR)

        # 设置常量 diff_x 的默认值。
        diff_x = predicted_x - (width * 0.5)
        # 设置常量 diff_y 的默认值。
        diff_y = predicted_y - (height * 0.5)

        # 设置常量 dist 的默认值。
        dist = (diff_x * diff_x + diff_y * diff_y) ** 0.5
        # 设置常量 dynamic_kp 的默认值。
        dynamic_kp = self.base_kp + (dist * AIM_KP_DIST_GAIN)
        # 根据条件判断选择执行分支。
        if dynamic_kp > self.kp_max:
            # 设置常量 dynamic_kp 的默认值。
            dynamic_kp = self.kp_max

       
        # 直接算出浮点位移，避免整数截断损失微小控制量。
        move_x = diff_x * dynamic_kp
        # 直接算出浮点位移，避免整数截断损失微小控制量。
        move_y = diff_y * dynamic_kp

        # 返回当前结果并结束函数执行。
        return True, float(move_x), float(move_y)


# 定义函数 get_aim_tune_profile，承载对应功能流程。
def get_aim_tune_profile(profile_key: str) -> tuple[str, dict[str, float | bool | str]]:
    # 设置常量 key 的默认值。
    key = str(profile_key).upper()
    # 设置常量 profile 的默认值。
    profile = AIM_TUNE_PROFILES.get(key)
    # 根据条件判断选择执行分支。
    if profile is None:
        # 设置常量 key 的默认值。
        key = "A"
        # 设置常量 profile 的默认值。
        profile = AIM_TUNE_PROFILES[key]
        # 输出关键状态到控制台，便于运行期观测。
        print(f"⚠️ AIM_TUNE_PROFILE={profile_key} invalid, fallback to {key}")
    # 返回当前结果并结束函数执行。
    return key, profile


# 定义函数 load_model，承载对应功能流程。
def load_model(model_dir: Path) -> YOLO:
    # 设置常量 engine_path 的默认值。
    engine_path = model_dir / MODEL_ENGINE_NAME
    # 设置常量 pt_path 的默认值。
    pt_path = model_dir / MODEL_PT_NAME

    # 根据条件判断选择执行分支。
    if engine_path.exists():
        # 进入异常保护区，避免单点错误中断流程。
        try:
            # 输出关键状态到控制台，便于运行期观测。
            print(f"🧠 Loading engine model: {engine_path.name}")
            # 返回当前结果并结束函数执行。
            return YOLO(str(engine_path), task="detect")
        # 捕获异常并执行容错处理。
        except Exception as exc:
            # 输出关键状态到控制台，便于运行期观测。
            print(f"⚠️ Engine load failed, fallback to pt. reason: {exc}")

    # 根据条件判断选择执行分支。
    if not pt_path.exists():
        # 主动抛出异常，明确标记不可继续的状态。
        raise FileNotFoundError(f"Missing model file: {pt_path}")

    # 输出关键状态到控制台，便于运行期观测。
    print(f"🧠 Loading pt model: {pt_path.name}")
    # 返回当前结果并结束函数执行。
    return YOLO(str(pt_path))


# 定义函数 main_loop，承载对应功能流程。
def main_loop() -> None:
   
    profile_key, profile = get_aim_tune_profile(AIM_TUNE_PROFILE)
    # 输出关键状态到控制台，便于运行期观测。
    print(
       
        f"🎯 Tune {profile_key}({profile['label']})"
       
        f" | kalman={profile['kalman_enabled']}"
       
        f" | base_kp={profile['base_kp']}"
       
        f" | kp_max={profile['kp_max']}"
       
        f" | body_offset={profile['body_offset']}"
    )
    # 根据条件判断选择执行分支。
    if PERF_MONITOR_ENABLED:
        # 输出关键状态到控制台，便于运行期观测。
        print(f"📊 Perf monitor enabled, interval={PERF_MONITOR_INTERVAL_SEC:.1f}s")

    # 设置常量 model_dir 的默认值。
    model_dir = Path(__file__).resolve().parent
    # 设置常量 model 的默认值。
    model = load_model(model_dir)

    # 设置常量 reader 的默认值。
    reader = SharedVisionReader(SHARED_MAPPING_NAME, MAPPING_SIZE)
    # 设置常量 sender 的默认值。
    sender = AiUdpSender(UDP_HOST, UDP_PORT)
    # 设置常量 aim 的默认值。
    aim = AimController(profile)
    # 设置常量 perf 的默认值。
    perf = PerfMonitor(PERF_MONITOR_ENABLED, PERF_MONITOR_INTERVAL_SEC)

    # 输出关键状态到控制台，便于运行期观测。
    print(f"🛰️ Shared memory: {SHARED_MAPPING_NAME}")
    # 输出关键状态到控制台，便于运行期观测。
    print(f"📡 UDP target: {UDP_HOST}:{UDP_PORT}")
    # 输出关键状态到控制台，便于运行期观测。
    print("🚀 AI loop started. Press Ctrl+C to stop.")

    # 设置常量 last_has_target 的默认值。
    last_has_target = False

    # 进入异常保护区，避免单点错误中断流程。
    try:
        # 持续循环处理，直到条件不再满足。
        while True:
           
            status, frame = reader.read_latest()
           
            perf.on_reader(status)

            # 根据条件判断选择执行分支。
            if status != ReaderStatus.OK or frame is None:
                # Prevent single-core 100% burn while waiting/new-frame racing.
               
                time.sleep(POLL_SLEEP_SEC)
               
                perf.maybe_report()
               
                continue

            # 设置常量 infer_start 的默认值。
            infer_start = time.perf_counter()
            # 设置常量 results 的默认值。
            results = model(
               
                frame.frame_bgr,
                # 设置常量 imgsz 的默认值。
                imgsz=MAX_ROI_SIZE,
                # 设置常量 conf 的默认值。
                conf=YOLO_CONF,
                # 设置常量 classes 的默认值。
                classes=YOLO_CLASSES,
                # 设置常量 max_det 的默认值。
                max_det=YOLO_MAX_DET,
                # 设置常量 verbose 的默认值。
                verbose=False,
            )
            # 设置常量 infer_ms 的默认值。
            infer_ms = (time.perf_counter() - infer_start) * 1000.0
           
            perf.on_infer(infer_ms)

           
            has_target, ai_dx, ai_dy = aim.compute(results[0], frame.width, frame.height)

            # 根据条件判断选择执行分支。
            if has_target:
                # 设置常量 sent 的默认值。
                sent = sender.send(frame.frame_id, True, ai_dx, ai_dy)
               
                perf.on_send(sent, release=False)
            # 执行兜底分支以覆盖其余场景。
            else:
                # Send one release signal only on target-loss edge.
                # 根据条件判断选择执行分支。
                if last_has_target:
                    # 设置常量 sent 的默认值。
                    sent = sender.send(frame.frame_id, False, 0.0, 0.0)
                   
                    perf.on_send(sent, release=True)

            # 设置常量 last_has_target 的默认值。
            last_has_target = has_target
           
            perf.maybe_report()
    # 执行收尾逻辑，确保资源被正确释放。
    finally:
       
        reader.close()
       
        sender.close()


# 定义函数 main，承载对应功能流程。
def main() -> int:
    # 进入异常保护区，避免单点错误中断流程。
    try:
       
        main_loop()
        # 返回当前结果并结束函数执行。
        return 0
    # 捕获异常并执行容错处理。
    except KeyboardInterrupt:
        # 输出关键状态到控制台，便于运行期观测。
        print("\n[AI] Interrupted by user.")
        # 返回当前结果并结束函数执行。
        return 130
    # 捕获异常并执行容错处理。
    except Exception as exc:
        # 输出关键状态到控制台，便于运行期观测。
        print(f"❌ Fatal error: {exc}")
        # 返回当前结果并结束函数执行。
        return 1


# 根据条件判断选择执行分支。
if __name__ == "__main__":
    # 主动抛出异常，明确标记不可继续的状态。
    raise SystemExit(main())
