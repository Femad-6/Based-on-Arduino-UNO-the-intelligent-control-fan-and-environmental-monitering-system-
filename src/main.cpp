#include <Arduino.h>
#include <DHT.h>
#include <IRremote.h>
#include <Servo.h>
#include <string.h>

// DHT11 设置
#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- 补充缺失的定义 ---
#define IR_RECEIVE_PIN 3 // 将红外接收脚改为 3，避免与 WiFi (11) 冲突
// 舵机信号线连接到该引脚
#define SERVO_PIN 6
// 扫角范围（0~180）。若舵机会“顶到限位”抖动，可改成 10~170 或 20~160。
#define SERVO_MIN_ANGLE 10
#define SERVO_MAX_ANGLE 170

Servo fanServo;
static bool servoAttached = false;

static void servoAttachIfNeeded()
{
  if (!servoAttached)
  {
    fanServo.attach(SERVO_PIN);
    servoAttached = true;
  }
}

static void servoDetachIfNeeded()
{
  if (servoAttached)
  {
    fanServo.detach();
    servoAttached = false;
  }
}

// 全局变量
int fanSpeed = 0;
bool manualOverride = false;
float tempThreshold = 28.0;

static void updateServoSimulateRotation(int speedPwm)
{
  static int pos = (SERVO_MIN_ANGLE + SERVO_MAX_ANGLE) / 2;
  static int dir = 1;
  static unsigned long lastStepMs = 0;

  // 周期锁存：一个“往复周期”(SERVO_MIN -> SERVO_MAX -> SERVO_MIN) 内速度保持不变。
  // 收到的新速度先进入 pending，在到达 SERVO_MIN（下一个周期开始）时再生效。
  static int cyclePwm = 0;
  static int pendingPwm = 0;

  pendingPwm = constrain(speedPwm, 0, 255);
  // 从静止启动：无需等待“下一个周期”，直接采用当前目标
  if (cyclePwm == 0)
    cyclePwm = pendingPwm;

  if (cyclePwm == 0)
  {
    // 停止时不强制“回中”，避免突然大幅转动；保持当前位置即可
    return;
  }

  const unsigned long now = millis();
  // 让“模拟旋转”更像连续转动：速度越大，扫描越快。
  // 目标：最快约 6s、最慢约 18s 扫完 SERVO_MIN_ANGLE -> SERVO_MAX_ANGLE（单向）。
  // 说明：舵机不是连续电机，这里用“往复扫角”来模拟旋转观感。
  const unsigned long sweepRange = (unsigned long)(SERVO_MAX_ANGLE - SERVO_MIN_ANGLE);
  const unsigned long targetSweepMs = (unsigned long)map(cyclePwm, 1, 255, 12000, 3000);
  const int stepDegrees = constrain(map(cyclePwm, 1, 255, 1, 2), 1, 2);
  const unsigned long stepIntervalMs = (targetSweepMs * (unsigned long)stepDegrees) / (sweepRange == 0 ? 1UL : sweepRange);
  if (now - lastStepMs < (unsigned long)stepIntervalMs)
    return;
  lastStepMs = now;

  pos += dir * stepDegrees;
  if (pos >= SERVO_MAX_ANGLE)
  {
    pos = SERVO_MAX_ANGLE;
    dir = -1;
  }
  else if (pos <= SERVO_MIN_ANGLE)
  {
    pos = SERVO_MIN_ANGLE;
    dir = 1;

    // 到达最小角：一个周期结束/下个周期开始，此时才允许更新速度
    // AUTO 模式更慢：每个周期允许的速度变化更小
    const int maxDeltaPerCycle = manualOverride ? 30 : 10;
    if (cyclePwm < pendingPwm)
      cyclePwm = min(cyclePwm + maxDeltaPerCycle, pendingPwm);
    else if (cyclePwm > pendingPwm)
      cyclePwm = max(cyclePwm - maxDeltaPerCycle, pendingPwm);
  }

  servoAttachIfNeeded();
  fanServo.write(pos);
}

// ----------------- 红外按键映射（可按需修改） -----------------
// 下面这组 command 值是常见 21 键 NEC 遥控器的“命令字节”。
// 不同遥控器可能不同：若不匹配，请先打开串口监视器查看打印出来的 IR cmd，再改这里。
#define IR_CMD_0 0x42
#define IR_CMD_1 0x16
#define IR_CMD_2 0x19
#define IR_CMD_3 0xD
#define IR_CMD_4 0xC
#define IR_CMD_5 0x18
#define IR_CMD_6 0x5E
#define IR_CMD_7 0x8
#define IR_CMD_8 0x1C
#define IR_CMD_9 0x5A
// 69
#define IR_CMD_VOL_MINUS 0x07 // 作为“减速”
#define IR_CMD_VOL_PLUS 0x09  // 作为“加速”
#define IR_CMD_100_PLUS 0x4A  // 设为 100%
#define IR_CMD_200_PLUS 0x45  // 切换 AUTO/MANUAL

// 舵机工作时容易给供电/地线带来噪声，导致红外接收头输出毛刺被误判为“按键”。
// 这里做两层软件抗干扰：
// 1) 只接受白名单按键（其他一律忽略/不打印）
// 2) 自动锁定遥控器 address/protocol（首次收到有效非 REPEAT 指令后锁定）
static bool isWhitelistedIrCommand(uint8_t cmd)
{
  switch (cmd)
  {
  case IR_CMD_0:
  case IR_CMD_1:
  case IR_CMD_2:
  case IR_CMD_3:
  case IR_CMD_4:
  case IR_CMD_5:
  case IR_CMD_6:
  case IR_CMD_7:
  case IR_CMD_8:
  case IR_CMD_9:
  case IR_CMD_VOL_MINUS:
  case IR_CMD_VOL_PLUS:
  case IR_CMD_100_PLUS:
  case IR_CMD_200_PLUS:
    return true;
  default:
    return false;
  }
}

static bool shouldAcceptIr(const IRData &data)
{
  if (data.protocol == UNKNOWN)
    return false;
  if (data.flags & IRDATA_FLAGS_WAS_OVERFLOW)
    return false;
  if (!isWhitelistedIrCommand(data.command))
    return false;

  const bool isRepeat = (data.flags & IRDATA_FLAGS_IS_REPEAT);
  // 只允许音量键用 REPEAT 做连发，其他按键的 REPEAT 一律忽略
  if (isRepeat && !(data.command == IR_CMD_VOL_PLUS || data.command == IR_CMD_VOL_MINUS))
    return false;

  // 锁定遥控器地址/协议（减少噪声误判）
  static bool locked = false;
  static uint16_t lockedAddress = 0;
  static decode_type_t lockedProtocol = UNKNOWN;

  if (!locked)
  {
    if (!isRepeat)
    {
      locked = true;
      lockedAddress = data.address;
      lockedProtocol = data.protocol;
      Serial.print("[IR] Locked to protocol=");
      Serial.print((int)lockedProtocol);
      Serial.print(" address=0x");
      Serial.println(lockedAddress, HEX);
    }
    // 未锁定前：允许白名单按键通过
    return true;
  }

  if (data.address != lockedAddress)
    return false;
  if (data.protocol != lockedProtocol)
    return false;

  return true;
}

static inline int pwmToPercent(int pwm)
{
  return constrain(map(constrain(pwm, 0, 255), 0, 255, 0, 100), 0, 100);
}

static inline int percentToPwm(int percent)
{
  return constrain(map(constrain(percent, 0, 100), 0, 100, 0, 255), 0, 255);
}

static void setFanSpeedPercent(int percent)
{
  manualOverride = true;
  fanSpeed = percentToPwm(percent);
  Serial.print("[IR] Set Speed: ");
  Serial.print(percent);
  Serial.println("%");
}

static void adjustFanSpeedPercent(int delta)
{
  int current = pwmToPercent(fanSpeed);
  setFanSpeedPercent(current + delta);
}

static int autoSpeedPercentForTemp(float tC)
{
  // 分段线性（用户指定）：
  // 28-29℃: 0-10%
  // 29-30℃: 10-30%
  // 30-31℃: 30-60%
  // 31-32℃: 60-80%
  // >32℃: 100%
  // <28℃: 0%

  if (isnan(tC))
    return 0;

  const float t0 = 28.0f;
  const float t1 = 29.0f;
  const float t2 = 30.0f;
  const float t3 = 31.0f;
  const float t4 = 32.0f;

  if (tC < t0)
    return 0;
  if (tC > t4)
    return 100;

  // 线性插值 helper
  auto lerpPercent = [](float x, float a, float b, int pa, int pb) -> int
  {
    const float denom = (b - a);
    const float f = (denom == 0.0f) ? 0.0f : (x - a) / denom;
    const float pf = (float)pa + constrain(f, 0.0f, 1.0f) * (float)(pb - pa);
    return constrain((int)(pf + 0.5f), 0, 100);
  };

  if (tC < t1)
    return lerpPercent(tC, t0, t1, 0, 10);
  if (tC < t2)
    return lerpPercent(tC, t1, t2, 10, 30);
  if (tC < t3)
    return lerpPercent(tC, t2, t3, 30, 60);
  // tC 在 [31, 32]：到 32℃ 为 80%，只有“超过 32℃”才 100%
  return lerpPercent(tC, t3, t4, 60, 80);
}

void setup()
{
  Serial.begin(9600);
  pinMode(13, OUTPUT); // 内置 LED 用于指示状态

  // 初始化 DHT
  dht.begin();
  // 初始化 红外
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  // 初始化 舵机
  servoAttachIfNeeded();
  fanServo.write((SERVO_MIN_ANGLE + SERVO_MAX_ANGLE) / 2);
}

unsigned long lastSendTime = 0;

void loop()
{
  // 心跳指示：每次循环闪烁一下 LED，证明程序没卡死
  // 由于循环现在很快，我们需要降频闪烁，否则看不清
  static unsigned long lastBlinkTime = 0;
  if (millis() - lastBlinkTime > 500)
  {
    lastBlinkTime = millis();
    digitalWrite(13, !digitalRead(13));
  }

  // --- 1. 处理红外遥控信号 (非阻塞) ---
  if (IrReceiver.decode())
  {
    auto &data = IrReceiver.decodedIRData;
    const uint8_t command = data.command;
    const bool isRepeat = (data.flags & IRDATA_FLAGS_IS_REPEAT);

    if (shouldAcceptIr(data))
    {
      Serial.print("[IR] Protocol: ");
      Serial.print(data.protocol);
      Serial.print(" | cmd: 0x");
      Serial.print(command, HEX);
      if (isRepeat)
        Serial.print(" (REPEAT)");
      Serial.println();

      // 200+：切换模式
      if (command == IR_CMD_200_PLUS)
      {
        manualOverride = !manualOverride;
        Serial.print("[IR] Mode -> ");
        Serial.println(manualOverride ? "MANUAL" : "AUTO");
      }

      // 数字键：直接设置百分比（1-9 -> 10%-90%，0 -> 0%）
      if (!isRepeat)
      {
        if (command == IR_CMD_0)
          setFanSpeedPercent(0);
        else if (command == IR_CMD_1)
          setFanSpeedPercent(10);
        else if (command == IR_CMD_2)
          setFanSpeedPercent(20);
        else if (command == IR_CMD_3)
          setFanSpeedPercent(30);
        else if (command == IR_CMD_4)
          setFanSpeedPercent(40);
        else if (command == IR_CMD_5)
          setFanSpeedPercent(50);
        else if (command == IR_CMD_6)
          setFanSpeedPercent(60);
        else if (command == IR_CMD_7)
          setFanSpeedPercent(70);
        else if (command == IR_CMD_8)
          setFanSpeedPercent(80);
        else if (command == IR_CMD_9)
          setFanSpeedPercent(90);
        else if (command == IR_CMD_100_PLUS)
          setFanSpeedPercent(100);
      }

      // 音量键：加/减速（允许按住连发）
      if (command == IR_CMD_VOL_PLUS)
        adjustFanSpeedPercent(+10);
      else if (command == IR_CMD_VOL_MINUS)
        adjustFanSpeedPercent(-10);
    }
    IrReceiver.resume();
  }

  // --- 2.5 舵机模拟“旋转”（不依赖2秒定时器，保证动作平滑） ---
  updateServoSimulateRotation(fanSpeed);

  // --- 3. 定时任务：读取传感器、逻辑控制、发送数据 (每2秒一次) ---
  if (millis() - lastSendTime > 2000)
  {
    lastSendTime = millis();

    // 3.1 读取温湿度
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (isnan(h) || isnan(t))
    {
      Serial.println("DHT read failed");
      // 保持上次的值或设为0
    }

    // 3.2 逻辑控制 (自动模式)
    if (!manualOverride && !isnan(t))
    {
      const int autoPercent = autoSpeedPercentForTemp(t);
      fanSpeed = percentToPwm(autoPercent);
    }

    // 3.3 输出状态到串口（已移除 WiFi 模块）
    String statusMsg = "Temp: " + String(t, 1) + "C | Hum: " + String(h, 1) + "% | Set: " + String(tempThreshold) + "C | Mode: " + (manualOverride ? "MAN" : "AUTO") + " | Speed: " + String(map(fanSpeed, 0, 255, 0, 100)) + "%";

    Serial.println(statusMsg);
  }
}
