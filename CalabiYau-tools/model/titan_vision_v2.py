import os
import cv2
import time
import torch
import serial
import struct
import math
import numpy as np
from ultralytics import YOLO



# ================= 配置区域 =================
# 1. 通信配置
SERIAL_PORT = 'COM7'  # <--- 确保这里是你刚刚测试成功的端口，不对就去设备管理器里看
BAUD_RATE = 115200

# 2. 视觉配置
MODEL_NAME = 'kalabiqiu v8.pt'  # 请确保文件在同一目录下
ROI_SIZE = 256         # 裁剪区域大小
CONF_THRESHOLD = 0.5   # 置信度阈值
IOU_THRESHOLD = 0.5    # NMS 阈值

# 3. 瞄准控制参数 (PID 基础)
AIM_KP = 0.6           # 比例系数：越小越平滑，越大越灵敏 (建议 0.3 ~ 0.8)
MIN_MOVE_DIST = 2      # 死区：距离小于 2 像素时不移动（防抖）
HEAD_OFFSET_Y = -5     # 抬枪补偿：如果总是打脖子，可以设为 -5 往上微调
# ===========================================

def get_center_distance(box, center_x, center_y):
    """计算目标框中心到画面中心的距离"""
    x1, y1, x2, y2 = box
    box_cx = (x1 + x2) / 2
    box_cy = (y1 + y2) / 2
    return math.sqrt((box_cx - center_x)**2 + (box_cy - center_y)**2)

def main():
    # --- 初始化串口 ---
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.01)
        print(f"✅ 神经连接建立: {SERIAL_PORT}")
    except Exception as e:
        print(f"❌ 串口打开失败: {e}")
        return

    # --- 初始化模型 ---
    current_dir = os.path.dirname(os.path.abspath(__file__))
    model_path = os.path.join(current_dir, MODEL_NAME)
    try:
        # 加载模型
        model = YOLO(model_path)
        print("✅ 视觉皮层加载完成")
    except Exception as e:
        print(f"❌ 模型加载失败: {e}")
        return

    # --- 初始化相机 (强制 MJPG) ---
    cap = cv2.VideoCapture(1, cv2.CAP_DSHOW) # 注意：这里如果还是黑屏，改回 0 或 1
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1920)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)
    
    # 预热一帧
    if cap.isOpened():
        ret, _ = cap.read()
        if not ret:
            print("❌ 无法读取画面，请检查采集卡连接")
            return
        print("✅ 视觉信号已接入")
    else:
        print("❌ 无法打开采集卡")
        return

    # 屏幕中心与 ROI 坐标计算
    screen_w, screen_h = 1920, 1080
    center_x, center_y = screen_w // 2, screen_h // 2
    
    # ROI 边界
    roi_x1 = center_x - (ROI_SIZE // 2)
    roi_y1 = center_y - (ROI_SIZE // 2)
    roi_x2 = roi_x1 + ROI_SIZE
    roi_y2 = roi_y1 + ROI_SIZE
    
    print(f"🎯 警戒区域: {ROI_SIZE}x{ROI_SIZE} (中心锁定)")

    while True:
        t_start = time.time()
        ret, frame = cap.read()
        if not ret: break

        # 1. ROI 裁剪 (只处理中心区域)
        roi_frame = frame[roi_y1:roi_y2, roi_x1:roi_x2]

        # 2. 推理
        results = model(roi_frame, imgsz=256, conf=CONF_THRESHOLD, verbose=False)
        
        # 3. 目标筛选策略
        target_box = None
        min_dist = 9999
        
        # 绘制 ROI 框方便调试
        cv2.rectangle(frame, (roi_x1, roi_y1), (roi_x2, roi_y2), (0, 255, 0), 1)

        for result in results:
            boxes = result.boxes
            for box in boxes:
                # [Fix] 修复之前的解包 Bug，改用索引访问更安全
                xyxy = box.xyxy[0].cpu().numpy() # [x1, y1, x2, y2]
                x1, y1, x2, y2 = xyxy
                cls_id = int(box.cls[0])
                
                # 在 ROI 上的绝对坐标
                abs_x1, abs_y1 = int(x1) + roi_x1, int(y1) + roi_y1
                abs_x2, abs_y2 = int(x2) + roi_x1, int(y2) + roi_y1
                
                # 绘制目标
                color = (0, 0, 255) if cls_id == 1 else (255, 0, 0) # 1=头(红), 0=身(蓝)
                cv2.rectangle(frame, (abs_x1, abs_y1), (abs_x2, abs_y2), color, 2)
                
                # --- 策略核心：只锁头(1) ---
                # 如果你想同时也锁身体，可以把这个 if 去掉，或者加个优先逻辑
                if cls_id == 1: 
                    # 计算到 ROI 中心 (ROI_SIZE/2, ROI_SIZE/2) 的距离
                    # ROI 中心在 roi_frame 坐标系下就是 (160, 160)
                    roi_center = ROI_SIZE / 2
                    dist = get_center_distance(xyxy, roi_center, roi_center)
                    
                    if dist < min_dist:
                        min_dist = dist
                        target_box = xyxy # 暂存最佳目标

        # 4. 决策控制 (Decision Making)
        if target_box is not None:
            # 计算目标中心 (相对于 ROI 左上角)
            tx1, ty1, tx2, ty2 = target_box
            target_cx = (tx1 + tx2) / 2
            target_cy = (ty1 + ty2) / 2
            
            # ROI 中心 (即屏幕绝对中心)
            roi_center = ROI_SIZE / 2
            
            # 计算原始误差 (Pixel Error)
            diff_x = target_cx - roi_center
            diff_y = target_cy - roi_center + HEAD_OFFSET_Y # 加上抬枪补偿
            
            # 应用死区 (防止微小抖动)
            if abs(diff_x) < MIN_MOVE_DIST: diff_x = 0
            if abs(diff_y) < MIN_MOVE_DIST: diff_y = 0
            
            # 应用比例控制 (P-Controller)
            move_x = int(diff_x * AIM_KP)
            move_y = int(diff_y * AIM_KP)
            
            # 发送指令 (仅当需要移动时)
            if move_x != 0 or move_y != 0:
                # 限制单次最大移动量 (防止甩飞)
                move_x = max(-20, min(20, move_x))
                move_y = max(-20, min(20, move_y))
                
                # 打包: head(0xA5) cmd(0x01) dx dy btn tail(0x5A)
                packet = struct.pack('<BBhhBB', 0xA5, 0x01, move_x, move_y, 0, 0x5A)
                ser.write(packet)
                
                # 视觉反馈：画一条线指向目标
                cv2.line(frame, (center_x, center_y), 
                         (int(target_cx + roi_x1), int(target_cy + roi_y1)), (0, 255, 255), 2)

        # 5. 显示 FPS
        fps = 1 / (time.time() - t_start)
        cv2.putText(frame, f"FPS: {fps:.1f}", (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
        
        cv2.imshow('Project Titan - Link Start', frame)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    ser.close()
    cap.release()
    cv2.destroyAllWindows()

if __name__ == '__main__':
    main()