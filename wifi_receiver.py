import socket
import time
import threading
import msvcrt  # 用于检测按键 (Windows)

# 配置监听参数
HOST = '0.0.0.0'
PORT = 8080
ARDUINO_IP = None # 收到数据后自动获取
ARDUINO_PORT = 8080 # ESP-01 监听端口 (透传模式下通常不需要，但UDP需要目标端口)

sock = None
last_message = ""
is_inputting = False # 标记是否正在输入指令

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
                continue

            if message:
                # 打印接收到的数据
                print(f"\r[{time.strftime('%H:%M:%S')}] {message}   ", end='') 
                # 使用 \r 回车符覆盖当前行，保持界面整洁
        except:
            break

def main():
    global sock, ARDUINO_IP, is_inputting
    print(f"=== WiFi 远程控制终端 (UDP) ===")
    print(f"正在监听端口 {PORT} ...")
    print("操作说明:")
    print("  [监控模式] 实时显示传感器数据")
    print("  [按 'r' 键] 暂停刷新，进入指令输入模式")
    print("-" * 50)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
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
                    print("\n\n>>> 进入控制模式 (输入数值 0-100 或 A/M) <<<")
                    
                    try:
                        cmd_raw = input("指令 > ").strip().upper()
                    except EOFError:
                        break

                    if not ARDUINO_IP:
                        print("错误: 尚未连接到 Arduino。")
                    elif cmd_raw:
                        # 智能指令解析
                        final_cmd = ""
                        if cmd_raw.isdigit():
                            val = int(cmd_raw)
                            if 0 <= val <= 100:
                                # 将 0-100% 映射到 0-255 PWM
                                pwm_val = int(val * 255 / 100)
                                final_cmd = f"SPEED:{pwm_val}"
                                print(f"[系统] 发送设置: {val}% (PWM: {pwm_val})")
                            else:
                                print("错误: 请输入 0-100 之间的数值")
                        elif cmd_raw in ["A", "AUTO"]:
                            final_cmd = "AUTO"
                            print("[系统] 切换为自动模式")
                        elif cmd_raw in ["M", "MAN", "MANUAL"]:
                            final_cmd = "MANUAL"
                            print("[系统] 切换为手动模式")
                        else:
                            final_cmd = cmd_raw # 允许发送原始指令

                        if final_cmd:
                            # 发送指令给 Arduino (添加换行符)
                            sock.sendto((final_cmd + "\n").encode('utf-8'), (ARDUINO_IP, ARDUINO_PORT))
                    
                    print(">>> 返回监控模式 <<<\n")
                    is_inputting = False
                    last_message = "" # 清除缓存，强制刷新下一条数据
            
            time.sleep(0.1)

    except KeyboardInterrupt:
        print("\n程序已停止")
    finally:
        if sock:
            sock.close()

if __name__ == "__main__":
    main()
