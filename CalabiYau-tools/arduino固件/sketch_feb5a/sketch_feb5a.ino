#include <Mouse.h>


const uint8_t HEAD = 0xA5;
const uint8_t TAIL = 0x5A;
const int PACKET_SIZE = 9;
uint8_t buffer[PACKET_SIZE];


void setup() {
  Mouse.begin();
  // 串口/引脚配置 (需与 Python 一致), 通常 RP2040-Zero 的 Serial1 是 TX=0, RX=1
  Serial1.setRX(1);
  Serial1.setTX(0);
  Serial1.begin(921600);
  // 稍微等待一下 USB 就绪
  delay(100);
}


// 优化：添加状态缓存，防止按住按键时重复发送 USB 报告
uint8_t last_btn_flags = 0; 
// 缓存上一次发送的按键状态，用于去重
uint8_t last_sent_buttons = 0;



void loop() {


  // 优化：累积处理逻辑
  // 定义累积变量 (购物车) 
  int total_dx = 0;
  int total_dy = 0;
  int total_wheel = 0;

  // 记录这一批数据中最新的按键状态(默认维持旧状态)
  // 初始值设为 last_sent_buttons，如果没有读到新数据，就保持不变
  uint8_t latest_buttons = last_sent_buttons; // 默认维持旧状态
  // 标记是否真的读到了有效数据
  bool has_data = false;


  // === 疯狂读取循环：极速清空缓冲区 ===
  // 只要缓冲区里有够一个包的数据，就一直读！
  // 限制单次最大处理包数 (例如 50)，防止 Python 发疯导致单片机卡死在 while 里出不来
  // count < 30 : 强制每处理50个包就去发送一次 USB
  // available() 返回 “当前串口对象 Serial1 的接收缓冲区里还有多少字节没被读走”
  int packet_count = 0;

  while (Serial1.available() >= PACKET_SIZE && packet_count < 50) {  // 若出现抖动/负载升高再回到 50
    
    // 校验包头 0xA5
    if (Serial1.peek() != HEAD) {
      Serial1.read(); // 错位修正：扔掉 1 个错位字节，仅丢弃 1 字节，尝试对齐下一字节
      continue;       // 继续找下一个
    }

    // 读取数据：读取完整一包 (9字节) 
    Serial1.readBytes(buffer, PACKET_SIZE);

    // 校验包尾 0x5A
    if (buffer[8] == TAIL) {
      // 解析位移
      int16_t dx = (buffer[3] << 8) | buffer[2];
      int16_t dy = (buffer[5] << 8) | buffer[4];
      // 解析滚轮 (索引 7，即 TAIL 之前的一位)
      // 使用 int8_t 强制转换，因为滚轮有正负 (上滚/下滚)
      int8_t wheel = (int8_t)buffer[7];
      // 读取包里的按键
      // Python发送的位掩码: 左(1), 右(2), 中(4), 侧1(8), 侧2(16)
      uint8_t btn_flags = buffer[6];

      // [核心逻辑] 位移累加: 位移是"相对"的，必须累加，不能丢弃！
      // 这一帧的移动加到总数里
      total_dx += dx;
      total_dy += dy;
      total_wheel += wheel;

      // --- [核心逻辑] 按键更新 ---
      // 我们只保留最新的按键状态 (直接覆盖旧的)
      // 因为如果中间按下又抬起，对于游戏来说可能太快了检测不到，
      // 或者我们希望尽快响应现在的状态。
      latest_buttons = btn_flags;
      
      has_data = true; // 标记我们确实拿到了数据
      packet_count++;
    }
  }


  // === 3. 统一发送 (结账) ===
  // 如果刚才读到了数据，现在一次性合并发送给电脑
  if (has_data) {
    
    // --- 处理按键 (仅当状态确实改变了才发送/执行 Press/Release) ---
    // 这能极大减少 USB 通信量，解决按键连发导致的卡顿
    // 对比“这一批最新的状态”和“上次发送给电脑的状态”
    if (latest_buttons != last_sent_buttons) {
      
      // 左键 (Bit 0)
      if ((latest_buttons & 0x01) != (last_sent_buttons & 0x01)) {
        if (latest_buttons & 0x01) Mouse.press(MOUSE_LEFT);
        else Mouse.release(MOUSE_LEFT);
      }
      
      // 右键 (Bit 1)
      if ((latest_buttons & 0x02) != (last_sent_buttons & 0x02)) {
        if (latest_buttons & 0x02) Mouse.press(MOUSE_RIGHT);
        else Mouse.release(MOUSE_RIGHT);
      }
      
      // 中键 (Bit 2)
      if ((latest_buttons & 0x04) != (last_sent_buttons & 0x04)) {
        if (latest_buttons & 0x04) Mouse.press(MOUSE_MIDDLE);
        else Mouse.release(MOUSE_MIDDLE);
      }
      
      // // 侧键1 (Bit 3) - 也就是 Python 发送的 8
      // if ((latest_buttons & 0x08) != (last_sent_buttons & 0x08)) {
      //   if (latest_buttons & 0x08) Mouse.press(MOUSE_BACK);
      //   else Mouse.release(MOUSE_BACK);
      // }

      // // 侧键2 (Bit 4) - 也就是 Python 发送的 16
      // if ((latest_buttons & 0x10) != (last_sent_buttons & 0x10)) {
      //   if (latest_buttons & 0x10) Mouse.press(MOUSE_FORWARD);
      //   else Mouse.release(MOUSE_FORWARD);
      // }
      
      // 更新缓存
      last_sent_buttons = latest_buttons;
    }


    // [核心修改] 分片循环发送：解决 127 截断问题
    // 只要还有剩余位移没发完，就一直发
    // 只有总位移不为0时才发送，减少 USB 负载
    // 由于上面的 Mouse.press 会自动发送报告，所以即使不调用 move，按键也会生效
    while (total_dx != 0 || total_dy != 0 || total_wheel != 0) {
      // 这里的 total_dx 是刚才那一批包里所有 dx 的总和
      // 限制单次最大移动，防止数据溢出 (虽然 int 很大，但保持稳健)
      // 这一步不是必须的，但对于 FPS 游戏防止视角瞬间 180 度翻转有帮助
      
      // 取出本次能发的最大值 (-127 到 127)
      int8_t send_dx = constrain(total_dx, -127, 127);
      int8_t send_dy = constrain(total_dy, -127, 127);
      int8_t send_wheel = constrain(total_wheel, -127, 127);

      // 发送
      Mouse.move(send_dx, send_dy, send_wheel);

      // 从总账里扣除
      total_dx -= send_dx;
      total_dy -= send_dy;
      total_wheel -= send_wheel;
      
      // 极小延迟，防止 USB 协议栈拥堵 (视情况可删，RP2040 性能通常不需要)
      // delayMicroseconds(100);
    }
  }
}





