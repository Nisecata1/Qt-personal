# CalabiYau-tools 归档说明（Windows + NVIDIA）

## 1. 项目简介
本项目用于构建一套 Windows 环境下的鼠标输入透传与视觉辅助链路：通过摄像头采集画面、YOLO 检测目标、卡尔曼与控制逻辑计算位移，再与 Raw Input 鼠标输入合成后通过串口发送到 RP2040 固件执行。

## 2. 快速开始（一键部署）

### 2.1 PowerShell 方式
在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\deploy.ps1
```

### 2.2 BAT 方式
在仓库根目录执行：

```bat
.\scripts\deploy.bat
```

### 2.3 预期输出与首启注意事项
- 脚本会自动创建 `.venv`、安装依赖、检查关键文件，然后启动 `model/mouse-proxy-yoloai-pc.py`。
- 首次安装依赖耗时较长是正常现象。
- 启动后若串口或相机不可用，主程序会在控制台给出错误信息并退出。

## 3. 环境要求
- 操作系统：Windows 10/11 x64
- Python：3.10 或 3.11（需可在命令行使用 `python`）
- GPU：NVIDIA（推荐，默认按 CUDA wheel 安装 Torch）
- 硬件：
  - 串口设备（RP2040 固件端）
  - 摄像头/采集卡（建议可稳定输出 1080p/60）
- 模型文件：`model/kalabiqiu v8.engine` 或 `model/kalabiqiu v8.pt`

## 4. 依赖总表（面向 `model/mouse-proxy-yoloai-pc.py`）

### 4.1 Python 标准库/内置模块
- `struct`
- `ctypes`
- `sys`
- `time`
- `threading`
- `__main__`

说明：代码里 `import serial` 的 `serial` 来自第三方包 `pyserial`，不属于标准库。

### 4.2 第三方依赖
- `torch`
- `opencv-python`（导入名 `cv2`）
- `numpy`
- `ultralytics`
- `pyserial`（导入名 `serial`）

### 4.3 系统与硬件依赖
- Windows API：`user32.dll`、`kernel32.dll`（Raw Input 与窗口消息）
- 串口链路：PC <-> RP2040
- 固件：Arduino RP2040 侧 HID Mouse 逻辑
- 摄像头/采集卡输入
- 模型文件：`.engine`（优先）或 `.pt`（回退）

### 4.4 Torch GPU 安装说明（默认）
部署脚本默认使用以下 CUDA wheel index 安装 Torch：

```text
https://download.pytorch.org/whl/cu121
```

## 5. 架构说明

### 5.1 线程模型
- 采集线程：`camera_reader_worker`，持续抓取最新帧并写入共享缓存。
- 视觉线程：`vision_worker`，读取 ROI 后执行 YOLO 推理与控制计算，将 AI 位移写入 `aim_state`。
- 主线程：`main`，处理 Windows 消息循环与 Raw Input，合并手动输入和 AI 位移后串口发送。

### 5.2 数据流
`摄像头帧 -> ROI -> YOLO/卡尔曼 -> aim_state -> Raw Input 合成 -> 串口协议 -> RP2040 HID`

### 5.3 串口协议 `<BBhhBbB>`
- `HEAD` (`B`)：包头，固定 `0xA5`
- `CMD` (`B`)：命令字，当前 `0x01`
- `DX` (`h`)：X 轴相对位移（int16）
- `DY` (`h`)：Y 轴相对位移（int16）
- `BTN` (`B`)：按键位掩码
- `WHEEL` (`b`)：滚轮增量（int8）
- `TAIL` (`B`)：包尾，固定 `0x5A`

## 6. 功能模块说明（`model/mouse-proxy-yoloai-pc.py`）
- 配置区：串口、波特率、灵敏度、热键、性能监控开关等全局参数。
- `SimpleKalman`：目标点平滑与速度估计，用于降低抖动并做简单预测。
- `AimState`：视觉线程与主线程间共享位移的线程安全“邮箱”。
- `PerfMonitor`：采集/推理/发送频率与耗时统计（可关闭）。
- `camera_reader_worker`：专职采集最新帧，减少视觉线程阻塞。
- `vision_worker`：ROI 裁剪、目标选择、卡尔曼预测、动态灵敏度、压枪与位移产出。
- `send_move`：按协议打包并写串口。
- `wnd_proc`：Raw Input 回调，解析物理鼠标事件并合成 AI 位移。
- `toggle_host_cursor`：限制/释放宿主机光标区域。
- `main`：初始化串口、启动视觉线程、注册 Raw Input、消息循环与热键切换。

## 7. 文件用途说明（核心 + 历史标注）

### 7.1 根目录
- `README.md`：项目归档主文档（本文件）。
- `requirements.txt`：Python 依赖契约。
- `scripts/deploy.ps1`：PowerShell 一键部署入口。
- `scripts/deploy.bat`：BAT 一键部署入口（调用 `deploy.ps1`）。
- `temp.py`：临时片段/草稿，非主流程。
- `项目阶段总结.txt`：历史阶段总结文档（归档参考）。
- `新建 文本文档.txt`：历史临时记录，非运行必需。

### 7.2 `model/`
- `model/mouse-proxy-yoloai-pc.py`：当前主入口（推荐运行）。
- `model/mouse_proxy.py`：纯透传/旧主线版本（历史可参考）。
- `model/mouse_proxy_ai-test copy.py`：AI 测试副本（历史备份）。
- `model/titan_vision_v2.py`：早期视觉链路脚本（历史参考）。
- `model/0_verify_camera.py`：相机链路与 FPS 验证脚本。
- `model/0_verify_control.py`：串口协议与控制链路验证脚本。
- `model/0_verify_model.py`：模型加载与推理环境验证脚本。
- `model/0_verify_pic_mark.py`：离线图片推理与标注验证脚本。
- `model/kalabiqiu v8.engine`：TensorRT 引擎模型（优先）。
- `model/kalabiqiu v8.pt`：PyTorch 模型（回退）。
- `model/kalabiqiu v8.onnx`：中间导出格式模型（转换用途）。
- `model/kalabiqiu v8-192.engine`：历史/变体引擎模型（按需）。

### 7.3 `arduino固件/`
- `arduino固件/sketch_feb5a/sketch_feb5a.ino`：当前固件源码（推荐）。
- `arduino固件/固件代码.txt`：固件文本备份（当前版本）。
- `arduino固件/固件代码-旧版.txt`：旧固件文本备份（历史）。
- `arduino固件/sketch_feb5a/build/...`：编译产物（如 `.uf2/.elf/.bin/.map`）。

### 7.4 `runs/`
- `runs/detect/predict*`：推理结果样例输出目录（历史结果，可清理）。

## 8. 固件部署（手动）
当前一键脚本仅覆盖 Python 侧。RP2040 固件请手动刷写，保持与 Python 侧协议一致（波特率 `921600`）。

### 8.1 方式 A：Arduino IDE
1. 打开 `arduino固件/sketch_feb5a/sketch_feb5a.ino`
2. 选择正确开发板（RP2040 系列）和端口
3. 编译并上传
4. 确认固件中 `Serial1.begin(921600)` 与 Python 端 `BAUD_RATE` 一致

### 8.2 方式 B：UF2 拖拽
1. 让 RP2040 进入 BOOT 模式
2. 拖拽 `arduino固件/sketch_feb5a/build/rp2040.rp2040.rpipico/sketch_feb5a.ino.uf2` 到设备盘符
3. 重启后连接串口并与 Python 端波特率配置核对

## 9. 常见问题排查
- Python 未安装：
  - 现象：`python` 命令不可用或跳转微软商店。
  - 处理：安装官方 Python 3.10/3.11，并勾选“Add python to PATH”。
- 串口占用/端口错误：
  - 现象：主程序报串口打开失败。
  - 处理：检查 `SERIAL_PORT`、关闭占用串口的软件、核对设备管理器端口号。
- 模型文件缺失：
  - 现象：启动时报找不到模型文件。
  - 处理：确保 `model/kalabiqiu v8.engine` 或 `model/kalabiqiu v8.pt` 存在。
- 摄像头打不开：
  - 现象：`cv2.VideoCapture` 读帧失败。
  - 处理：检查设备索引、驱动、采集卡链路，先运行 `model/0_verify_camera.py` 排查。
- CUDA 不可用：
  - 现象：推理速度明显下降或使用 CPU。
  - 处理：更新 NVIDIA 驱动，确认 Torch CUDA 版本与环境匹配，检查 `torch.cuda.is_available()`。

## 10. 本次归档新增接口
- 依赖接口：`requirements.txt`
- 部署接口：`scripts/deploy.ps1`、`scripts/deploy.bat`
- 文档接口：`README.md`

运行时业务逻辑与现有 Python/固件源码保持不变。
