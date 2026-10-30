# T1 使用与内置 G-Code 更换指南

## 功能与引脚

| 功能 | GPIO / 行为 |
| --- | --- |
| STSPIN220 EN/FAULT | GPIO8，开漏输出，内部上拉；低电平关闭功率输出 |
| STSPIN220 STBY/RESET | GPIO7；启动时先低，再拉高锁存细分，运行时保持高 |
| MODE1 / MODE2 | 硬件连接 3.3V |
| X STEP / DIR | GPIO9 / GPIO10 |
| Y STEP / DIR | GPIO4 / GPIO2 |
| 无源蜂鸣器 | GPIO6；PWM 方波驱动 |
| 离线按钮 | GPIO36，低有效，外部上拉；固件关闭内部上下拉 |
| X / Y 限位 | GPIO38 / GPIO35，低有效，内部上拉 |
| 风扇 | GPIO37，高有效 |
| 原生 USB | GPIO19 = D−，GPIO20 = D+；USB CDC 提供 Grbl 通信 |

### STSPIN220 启动

`main/my_plugin.c` 在 grblHAL 配置步进 RMT 外设之前执行：

1. EN 拉低，STBY 拉低。
2. X/Y 的 STEP 和 DIR 均拉低。
3. 等待 1ms，STBY 拉高，再保持 STEP/DIR 低 1ms。
4. 将 STEP/DIR 交还运动驱动；EN/FAULT 保持开漏，STBY 保持高。

依据 [STSPIN220 规格书 Rev 5](https://www.st.com/resource/en/datasheet/stspin220.pdf) 表 8，此组合锁存为 1/256；表 5 要求 MODE 建立时间至少 1µs、保持时间至少 100µs。EN 同时是芯片故障开漏输出，因此不使用推挽高电平。器件没有可读的细分寄存器，`$T1` 显示的是固件配置值。

普通 Ctrl-X 不重新拉低 STBY，以免在运动状态未知时改变电机序列；上电或完整重启重新锁存。

### 提示音

- 开机：1047 / 1319 / 1568 / 2093Hz 四个音，每个 250ms，共约 1 秒，与开机风扇检查同时进行。
- GPIO36：低电平稳定约 30ms 后，“滴”约 100ms。
- X/Y 限位：低电平稳定约 20ms 后，“滴”约 100ms；回零中的限位触发也会提示。
- 同一次持续按下不连续重复鸣叫；接近同时到达的事件可以合并为一声。
- 蜂鸣器使用 LEDC Timer3 / Channel7，激光使用独立的 Timer0 / Channel0。提示音不在步进或 GPIO 中断内生成，也不阻塞运动。

## 日常离线操作

1. 上电，听启动音、观察风扇短转，等待原有 T0 流程完成校准。
2. 将雕刻材料置于合适位置。默认文件坐标在 42mm × 42mm 工作区内。
3. 按下 GPIO36 按钮，低电平保持至少 30ms；应听到短提示音并开始执行内置文件。
4. 长按仅执行一次。任务完成后松开并重新按下，才能再次执行。
5. 未完成开机校准、报警、运行或暂停期间的按下不会排队等待执行；仍有按键提示音。

默认文件为 `main/GCode/HadesLogo.gc`，2507 字节，由构建系统原样嵌入 Flash，不需要 SD 卡、网络或电脑。引脚 36 与本板 SD SPI 时钟冲突，所以该板配置不允许同时启用 SD 卡。

离线执行使用当前工作坐标偏移；默认由 T0 建立 G54 + G92 原点。若通过 USB 修改过 G54/G92，请完整重启恢复开机校准后再运行内置任务。

当前文件使用 `M3`、`S204`、`F300`，设备 `$30=1000`，因此 S204 对应 20.4% 的指令功率；文件注释写的“80%”不会改变实际功率。此实现没有修改原文件的 S 值。

### 暂停、继续、停止

连接 USB CDC 后可发送实时命令：

| 命令 | 功能 |
| --- | --- |
| `?` | 查询实时状态 |
| `!` | 暂停 |
| `~` | 继续 |
| Ctrl-X（字节 `0x18`） | 立即复位，取消任务 |

执行离线文件时，USB 的普通 G-Code 和 `$` 命令会被丢弃，避免与内置任务混合执行或在任务结束后意外执行；上述实时命令仍然有效。任务结束后恢复普通 USB 通信。

取消或复位后须完整重启、重新完成 T0 校准，才能再次按按钮离线执行。拔掉 USB 不会取消离线任务。

## 更换内置 G-Code

### 方法 A：替换默认文件（推荐）

1. 备份 `main/GCode/HadesLogo.gc`。
2. 从雕刻软件导出标准 Grbl G-Code，覆盖此文件。建议明确包含 `G21 G90 G94 G54`，使用当前工作原点，末尾包含 `M5` 和 `M2` 或 `M30`。
3. 核对 XY 坐标位于 0～42mm 内，核对 `F` 和 `S`，导出软件的 S 最大值应与设备 `$30` 一致。不要把 GRBL 配置命令或重新定义原点的命令混入默认雕刻任务。
4. 在 WSL 项目根目录执行：

   ```bash
   . /home/hades/idf4d4/esp-idf/export.sh
   idf.py -D T1_GCODE_FILE="$PWD/main/GCode/HadesLogo.gc" build
   idf.py -p /dev/ttyACM0 flash
   ```

   串口名以实际设备为准。当前的独立 USB 转 UART 口可用于烧录。

5. 烧录后重新校准，再触发 GPIO36 验证新图案。改动源文件后，必须重新构建并烧录；只替换磁盘文件不会改变已烧录的设备。

### 方法 B：使用其他文件名

例如文件为 `main/GCode/NewLogo.gc`：

```bash
. /home/hades/idf4d4/esp-idf/export.sh
idf.py -D T1_GCODE_FILE="$PWD/main/GCode/NewLogo.gc" build
idf.py -p /dev/ttyACM0 flash
```

`T1_GCODE_FILE` 是 CMake 缓存参数，后续构建继续使用该路径；切回默认文件时重新指定方法 A 的路径。可以检查：

```bash
rg T1_GCODE_FILE build/CMakeCache.txt
```

不需要修改 C 代码或嵌入符号名，构建统一使用 `t1_job`。构建系统会跟踪所选文件内容的变化。

## USB CDC 连接与诊断

连接 GPIO19/20 的原生 USB 数据接口。在 WSL 中，将应用 CDC 设备映射进去，再选择它对应的 `/dev/ttyACM*`。应用默认描述为 Espressif CDC，VID:PID 通常为 `303a:4001`。

注意区分：

- `1a86:55d3`：独立 USB 转 UART 调试/烧录口，本配置的 Grbl 操作接口已切到原生 USB。
- `303a:1001`：ESP32-S3 USB Serial/JTAG / 下载模式。
- `303a:4001`：应用 TinyUSB CDC，提供 Grbl 操作。

复位或进入下载模式时 USB 可能断开并重新枚举，WSL 可能需要重新映射。终端可选择 115200；原生 CDC 的数据传输不受 UART 波特率限制。

空闲时可使用：

```text
$I
$$
$pins
$T1
$G
$#
?
```

`$T1` 报告配置的细分、STBY 电平、GPIO36/限位原始电平，以及启动音、短提示音、限位和 GPIO36 事件计数。原始电平 `1` 表示未触发，`0` 表示触发。计数用于核对软件事件处理，不代表已测得蜂鸣器声压或电机细分波形。

## 当前验证状态

### LightBurn USB CDC 连接参数（2026-09-16 实测）

仅接原生 USB 时，本轮设备为 `/dev/ttyACM0`，VID:PID `303a:4001`。端口编号随枚举顺序变化。

| 设置 | 值 |
| --- | --- |
| 设备类型 | GRBL |
| 连接 | Serial / USB 串口 |
| 波特率 | 115200 |
| 数据格式 | 8 数据位、无校验、1 停止位（8N1） |
| 流控 | 无；关闭 RTS/CTS、XON/XOFF |
| Enable DTR Signal | 开启或关闭均可（当前固件已取消依赖） |
| RTS | 实测开关均可，诊断使用关闭 |
| 手动终端命令结束符 | LF、CR、CRLF 均实测可用；实时 `?` 无需换行 |
| 工作尺寸 / S-value max | 42 × 42mm / 1000，与当前固件参数一致 |

旧固件曾复现 DTR=false 时无响应。当前固件已改为按 USB 就绪状态判断连接，DTR/RTS 不再用于通信门控。上电后先等待主机发送首个字节，再开始发送响应，避免串口尚未配置时的启动文本回显；连接后可先发送 `?` 或 `$I`。发送缓冲区连续 100ms 无进展时丢弃待发送内容，防止主机不读取时阻塞离线操作。LightBurn 的 Enable DTR Signal 可开启或关闭。设置说明见 [LightBurn 官方文档](https://docs.lightburnsoftware.com/2.1/Reference/DeviceSettings/BasicSettings/)。

如果 LightBurn 运行在 Windows，先在 PowerShell 执行 `usbipd list`，找到 `303a:4001` 对应 BUSID，再执行 `usbipd detach --busid <BUSID>`，之后在 LightBurn 选择 Windows 设备管理器显示的 COM 口。USB 附加到 WSL 时不能同时由 Windows 使用，见 [微软文档](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)。测试脚本已关闭串口，但不会自动解除 WSL 的 USB 映射。

旧固件复现记录：`test-results/t1/usb-cdc-lightburn.log`。取消 DTR 依赖后的记录：`test-results/t1/usb-cdc-dtr-independent.log`、`usb-cdc-dtr-independent-results.json`，测试端口为 `/dev/ttyACM1`，`/dev/ttyACM0` 为调试口。

### 功能验证记录

- IDF 4.4 构建通过，已通过调试口烧录，Flash 校验成功。
- 内嵌 G-Code 与源文件逐字节一致；当前文件的显式 XY 坐标范围均在 42mm 内。
- `python3 tests/test_t1_offline.py`：去抖、校准门控、长按/忙碌处理、文件流、EOF、复位和错误清理通过。
- `python3 tests/test_machine_startup.py`：实际源码函数的 GPIO 时序、1 秒音阶、100ms 短音及输入去抖模拟测试通过。
- 用户已确认蜂鸣器正常。2026-09-16 修复初始化末尾将 STBY 再次拉低的问题：锁存完成后必须保持高电平，否则电机功率输出被关闭。启动时序测试已复现该问题，并在修复后通过；修复固件已编译和烧录。
- 用户随后确认电机工作正常、GPIO36 触发 G-Code 正常；调试串口日志已记录回零成功与 G92 设零完成。
- 2026-09-16 原生 USB CDC 实机通信测试通过：WSL `/dev/ttyACM1`，VID:PID `303a:4001`；`$I`、`$T1`、`$G`、`$#`、`$$` 均正常返回 `ok`，连续 10 次 `?` 全部收到状态帧，关闭串口后重新打开仍可查询。当前状态 `Idle`，XY 机器位置与 G92 偏移均为 -42mm，工作坐标为零，STBY=1。
- 测试记录位于 `test-results/t1/usb-cdc.log` 与 `usb-cdc-results.json`。本次验证范围为命令查询、实时状态和串口重新打开；未测试物理拔插或通过 USB 执行雕刻。

2026-09-16 取消 DTR 依赖后的最终验证：完整重启、无 CDC 上位机时自动校准和设零通过；DTR=false 首次连接全部查询通过，四种 DTR/RTS 组合均通过，连续 10 次状态查询通过。发送停滞与首字节门控的源码回归测试 `tests/test_usb_tx_stall.py` 通过。

## 首次设备初始化

固件内置当前实机读取的 89 项 Grbl 参数。首次无标记时连续鸣叫 3 秒，写入并读回校验参数后记录完成标记，再执行正常启动流程；以后启动保留现有参数。参数清单和维护方式见 [首次初始化说明](FACTORY_INIT.md)。
