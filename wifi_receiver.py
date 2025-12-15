import socket
import time
import threading
import msvcrt  # 用于检测按键 (Windows)
from typing import Optional, Tuple

# 配置监听参数
HOST = '0.0.0.0'
PORT = 8080
ARDUINO_IP = None # 收到数据后自动获取
ARDUINO_PORT = 8080 # ESP-01 监听端口 (透传模式下通常不需要，但UDP需要目标端口)

sock = None
last_message = ""
is_inputting = False # 标记是否正在输入指令


def parse_user_command(cmd_raw: str) -> Tuple[Optional[str], str]:
    """把用户输入解析成要发给 Arduino 的最终指令。

    Arduino 端支持：AUTO / MANUAL / SPEED:<0-255>

    支持输入：
    - 0~100：按百分比设置（自动映射到 0~255）
    - PWM:<0~255> 或 P:<0~255>：直接按 PWM 设置
    - A/AUTO：自动模式
    - M/MAN/MANUAL：手动模式
    - SPEED:<0~255>：原样发送
    - H/HELP/?：显示帮助（不发送）
    """

    cmd = cmd_raw.strip().upper()
    if not cmd:
        return None, ""

    if cmd in {"H", "HELP", "?"}:
        help_text = (
            "可用指令:\n"
            "  0~100            设置速度百分比\n"
            "  PWM:<0~255>      直接设置 PWM\n"
            "  P:<0~255>        直接设置 PWM\n"
            "  SPEED:<0~255>    原样发送 SPEED 指令\n"
            "  A 或 AUTO        自动模式\n"
            "  M/MAN/MANUAL     手动模式\n"
        )
        return None, help_text

    if cmd.isdigit():
        val = int(cmd)
        if 0 <= val <= 100:
            pwm_val = int(val * 255 / 100)
            return f"SPEED:{pwm_val}", f"[系统] 发送设置: {val}% (PWM: {pwm_val})"
        return None, "错误: 请输入 0-100 的百分比，或用 PWM:0-255"

    if cmd.startswith("PWM:") or cmd.startswith("P:"):
        try:
            pwm_val = int(cmd.split(":", 1)[1].strip())
        except ValueError:
            return None, "错误: PWM 格式应为 PWM:0-255 或 P:0-255"
        if 0 <= pwm_val <= 255:
            percent = int(round(pwm_val * 100 / 255))
            return f"SPEED:{pwm_val}", f"[系统] 发送设置: PWM {pwm_val} (~{percent}%)"
        return None, "错误: PWM 值范围 0-255"

    if cmd in {"A", "AUTO"}:
        return "AUTO", "[系统] 切换为自动模式"
    if cmd in {"M", "MAN", "MANUAL"}:
        return "MANUAL", "[系统] 切换为手动模式"

    # 兼容：允许直接输入 SPEED:xxx
    if cmd.startswith("SPEED:"):
        return cmd, f"[系统] 发送: {cmd}"

    # 允许发送原始指令（高级用法）
    return cmd, f"[系统] 发送原始指令: {cmd}"

def receive_thread():
    global ARDUINO_IP, ARDUINO_PORT, last_message, is_inputting
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            if ARDUINO_IP is None:
                ARDUINO_IP = addr[0]
                ARDUINO_PORT = addr[1] # 自动获取 Arduino (ESP-01) 的发送端口
                print(f"\n[系统] 已锁定 Arduino: {ARDUINO_IP}:{ARDUINO_PORT}")
            
            message = data.decode('utf-8', errors='ignore').strip()
            
            # 如果正在输入指令，暂停打印接收到的数据，防止干扰
            if is_inputting:
                if message:
                    last_message = message
                continue

            if message:
                last_message = message
                # 打印接收到的数据
                print(f"\r[{time.strftime('%H:%M:%S')}] {message}   ", end='', flush=True) 
                # 使用 \r 回车符覆盖当前行，保持界面整洁
        except OSError:
            break

def main():
    global sock, ARDUINO_IP, is_inputting
    print(f"=== WiFi 远程控制终端 (UDP) ===")
    print(f"正在监听端口 {PORT} ...")
    print("操作说明:")
    print("  [监控模式] 实时显示传感器数据")
    print("  [按 'r' 键] 暂停刷新，进入指令输入模式")
    print("  [快捷键] 0=0%  1=30%  2=60%  3=100%  A=AUTO  M=MANUAL")
    print("-" * 50)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((HOST, PORT))

        # 启动接收线程
        t = threading.Thread(target=receive_thread)
        t.daemon = True
        t.start()

        print("等待 Arduino 上线...")
        
        while True:
            # 检测按键，不阻塞循环
            if msvcrt.kbhit():
                key = msvcrt.getch()
                if key.lower() == b'r':
                    is_inputting = True
                    print("\n\n>>> 进入控制模式 (输入 0-100 / PWM:0-255 / A/M；输入 H 查看帮助) <<<")
                    
                    try:
                        cmd_raw = input("指令 > ").strip().upper()
                    except EOFError:
                        break

                    if not ARDUINO_IP:
                        print("错误: 尚未连接到 Arduino。")
                    elif cmd_raw:
                        final_cmd, tip = parse_user_command(cmd_raw)
                        if tip:
                            print(tip)
                        if final_cmd:
                            sock.sendto((final_cmd + "\n").encode('utf-8'), (ARDUINO_IP, ARDUINO_PORT))
                    
                    print(">>> 返回监控模式 <<<\n")
                    is_inputting = False
                    if last_message:
                        print(f"[{time.strftime('%H:%M:%S')}] {last_message}")
                else:
                    # 监控模式快捷键：无需进入输入模式
                    if is_inputting:
                        continue
                    if not ARDUINO_IP:
                        continue

                    quick_map = {
                        b'0': '0',
                        b'1': '30',
                        b'2': '50',
                        b'3': '60',
                        b'4': '80',
                        b'5': '100',
                        b'a': 'AUTO',
                        b'A': 'AUTO',
                        b'm': 'MANUAL',
                        b'M': 'MANUAL',
                    }
                    if key in quick_map:
                        final_cmd, tip = parse_user_command(quick_map[key])
                        if tip:
                            print(f"\n{tip}")
                        if final_cmd:
                            sock.sendto((final_cmd + "\n").encode('utf-8'), (ARDUINO_IP, ARDUINO_PORT))
            
            time.sleep(0.1)

    except KeyboardInterrupt:
        print("\n程序已停止")
    finally:
        if sock:
            sock.close()

if __name__ == "__main__":
    main()
