from ultralytics import YOLO
import torch
import os

# 获取脚本所在的目录
current_dir = os.path.dirname(os.path.abspath(__file__))
print(f"-------- 环境侦查 --------")
print(f"脚本当前位置: {current_dir}")
print(f"该文件夹下的所有文件: {os.listdir(current_dir)}")
print(f"------------------------")

# 获取当前脚本所在的绝对路径
current_dir = os.path.dirname(os.path.abspath(__file__))
# 拼接出模型的绝对路径
model_path = os.path.join(current_dir, 'kalabiqiu v8.pt')

print(f"正在尝试加载模型: {model_path}") # 打印出来让你看清楚它到底在哪找

# 1. 检查 CUDA (显卡) 是否可用
print(f"CUDA Available: {torch.cuda.is_available()}")
if torch.cuda.is_available():
    print(f"GPU Device: {torch.cuda.get_device_name(0)}")
else:
    print("WARNING: Using CPU! Check your PyTorch installation.")

# 2. 加载模型 (替换为你实际的路径)
try:
    # 加载模型
    model = YOLO(model_path)
    
    # 3. 打印模型详细信息
    print("\n[Model Info]")
    # 这一步会输出模型的层结构，如果是 C2f 模块则是 v8，C3 则是 v5
    model.info() 
    
    # 4. 简单推理测试 (随便生成一个黑图片测一下能不能跑通)
    print("\n[Inference Test]")
    image_path = os.path.join(current_dir, 'text_img.png')
    results = model(image_path, imgsz=256) # 使用卖家说的 256 尺寸
    
    print(f"Success! Detected {len(results.boxes)} objects.") 


except Exception as e:
    print(f"\nError loading model: {e}")
    print("可能是 YOLOv5 的旧格式，需要用 yolov5 的仓库加载，或者模型损坏。")
