# SME208 Desk Robot

ESP32-S3 桌面机器人项目，集成语音录制/播放、ASR/LLM/TTS 语音助手、OLED 状态显示、BLE 串口控制、舵机动作、低功耗管理，以及一个 BLE 控制的跳跃避障小游戏。

## 功能概览

- OLED 显示状态页、时钟页、使用指南、Cinnamoroll 动画和小游戏画面。
- 通过按键或 BLE 命令录音、播放、上传语音，并执行 ASR -> LLM -> TTS 流程。
- 支持 BLE UART 控制，设备名为 `DeskRobot`。
- 支持 Wi-Fi 配网和系统状态查看。
- 支持水平/垂直舵机相对移动。
- 支持低功耗阶段切换。
- 支持跳跃避障小游戏：人物固定在屏幕左侧，三种大小的矩形障碍从右向左移动，支持空中二段跳。

## 硬件与环境

目标平台：

- ESP32-S3
- ESP-IDF v5.5.3
- SSD1306 128x64 OLED，I2C
- I2S 麦克风和扬声器
- 8 个按键
- LED 灯带
- 两轴舵机

OLED 默认引脚：

| 信号 | GPIO |
| --- | --- |
| SDA | GPIO3 |
| SCL | GPIO8 |

按键默认引脚：

| 按键 | GPIO |
| --- | --- |
| K1 | GPIO1 |
| K2 | GPIO2 |
| K3 | GPIO42 |
| K4 | GPIO41 |
| K5 | GPIO40 |
| K6 | GPIO39 |
| K7 | GPIO21 |
| K8 | GPIO45 |

## 配置 API Key

复制示例配置文件：

```powershell
copy main\api_config_private.example.h main\api_config_private.h
```

然后在 `main/api_config_private.h` 中填写真实密钥：

```c
#define DASHSCOPE_API_KEY "YOUR_DASHSCOPE_API_KEY"
#define DEEPSEEK_API_KEY "YOUR_DEEPSEEK_API_KEY"
```

`main/api_config_private.h` 是本地私密配置，不应提交到仓库。

如果希望 LLM 请求走本地代理，可以覆盖：

```c
#define DEEPSEEK_LLM_URL "http://YOUR_PC_LAN_IP:8080/chat/completions"
```

本地代理脚本位于 `tools/local_ai_server.py`。

## 构建

本项目需要先加载 Espressif PowerShell 环境。请在仓库根目录执行：

```powershell
& 'C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1'
idf.py build
```

也可以使用一行命令：

```powershell
& 'C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1'; idf.py build
```

如果 PowerShell 执行策略阻止 profile 脚本：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& 'C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1'; idf.py build"
```

## 烧录

构建完成后执行：

```powershell
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口，例如 `COM5`。

## BLE 串口命令

BLE 设备名：`DeskRobot`

命令大小写不敏感，每条命令以换行结束。

### 常用流程

```text
record
record
upload
play
```

含义：

- 第一次 `record` 开始录音。
- 第二次 `record` 停止录音。
- `upload` 上传录音并执行 ASR/LLM/TTS。
- `play` 播放最后一次录音或生成的语音。

### 音频

| 命令 | 说明 |
| --- | --- |
| `record` | 开始/停止录音 |
| `record start` | 开始录音 |
| `record stop` | 停止录音 |
| `play` | 开始/停止播放 |
| `play start` | 开始播放 |
| `play stop` | 停止播放 |
| `upload` | 上传已完成录音并执行语音助手流程 |
| `volume next` | 循环音量：50%、100%、150%、200% |

### 显示与状态

| 命令 | 说明 |
| --- | --- |
| `status` | 查看 Wi-Fi、电源、显示、录音、播放等状态 |
| `display dog` | 显示 Cinnamoroll 动画 |
| `display clock` | 显示时钟 |
| `display guide` | 显示使用指南 |
| `display status` | 显示系统状态页 |
| `exit` | 退出当前显示页或小游戏，回到 idle |

### 小游戏

| 命令 | 说明 |
| --- | --- |
| `game` | 启动或重开跳跃避障小游戏 |
| `jump` | 跳跃 |
| `exit` | 退出小游戏 |

玩法：

- 人物固定在屏幕最左边。
- 三种大小的矩形障碍从右向左移动。
- 发送 `jump` 可以跳起避障。
- 空中可再发送一次 `jump` 触发二段跳。
- 碰撞后显示 `OVER 分数`。
- 游戏结束后发送 `jump` 或 `game` 可重新开始。

### 舵机

| 命令 | 说明 |
| --- | --- |
| `servo center` | 两轴舵机居中 |
| `servo down` | 向下看 |
| `servo h <deg>` | 水平方向相对移动，限制在 +/-10 度 |
| `servo v <deg>` | 垂直方向相对移动，限制在 +/-10 度 |

示例：

```text
servo h 10
servo v -10
```

### 配置与电源

| 命令 | 说明 |
| --- | --- |
| `config wifi` | 进入 Wi-Fi 配网模式 |
| `config power-timeout <stage1_sec> <stage2_sec>` | 设置低功耗空闲时间 |
| `power stage1` | 手动进入低功耗 stage1 |
| `power stage2` | 手动进入低功耗 stage2 |

### 系统

| 命令 | 说明 |
| --- | --- |
| `help` | 输出 BLE 命令帮助 |
| `rst` | 重启开发板 |

## 按键操作

| 按键 | 短按 | 长按 |
| --- | --- | --- |
| K1 | 开始/停止录音 | - |
| K2 | 开始/停止播放，或取消语音助手任务 | - |
| K3 | 进入 Cinnamoroll 显示 | 垂直舵机 -10 度 |
| K4 | 进入时钟显示 | 垂直舵机 +10 度 |
| K5 | 进入低功耗 stage2 | 水平舵机 -10 度 |
| K6 | 进入 Wi-Fi 配网 | 水平舵机 +10 度 |
| K7 | 执行语音助手流程 | 进入使用指南 |
| K8 | 切换系统状态页 | 进入 Wi-Fi 配网 |

## 目录结构

```text
main/
  audio_mic.c/.h        I2S 录音
  audio_spk.c/.h        I2S 播放
  ble_serial.c/.h       BLE UART 命令
  display.c/.h          OLED/LVGL 显示与小游戏
  key.c/.h              按键输入
  led.c/.h              LED 效果
  servo.c/.h            舵机控制
  voice_assistant.c/.h  ASR/LLM/TTS 工作流
  asr_client.c/.h       ASR 客户端
  llm_client.c/.h       LLM 客户端
  tts_client.c/.h       TTS 客户端
  wifi_network.cc/.h    Wi-Fi 配网和连接
tools/
  local_ai_server.py    可选本地 LLM 代理
docs/
  ai_service_api_research.md
```

## 注意事项

- 不要提交真实的 `main/api_config_private.h`。
- OLED 初始化失败不会阻止音频、按键和 BLE 功能继续运行。
- BLE `jump` 只有在小游戏运行时有效。
- 构建前必须加载 ESP-IDF 环境，否则 `idf.py` 可能不可用。
