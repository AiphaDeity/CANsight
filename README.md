# CanSight

CanSight 是基于 STM32F407 和 LVGL 的经典 CAN 总线查看与日志记录项目。设备使用触摸屏显示报文，将接收记录保存到 SD 卡，并计划支持在设备上读取历史日志进行回放。

项目保留“接收”和“SD 回放”两个页面，不提供手动发送报文和追踪页面。Windows 模拟器用于开发界面，与 STM32 固件共用界面和字体源码。

## 当前进度

项目处于界面与业务数据接入阶段。接收页已完成真实 CAN 报文显示的基础实机验证，SD 回放页仍使用内置示例数据。本轮新增的真实状态栏与异常提示待实机验证。

| 模块 | 当前状态 |
| --- | --- |
| LCD 显示与 GT911 触摸 | 已接入 LVGL，完成实机显示和触摸验证 |
| CAN 接收 | 已实现 FIFO0 中断接收、软件队列和接收时间戳 |
| SD 日志 | 已实现 CSV 追加写入、批量缓冲和定期同步 |
| 接收页面 | 已接入真实 CAN 统计，每约 100 ms 刷新，支持 ID 筛选、十六进制输入和每页 8 行翻页 |
| SD 回放页面 | 已实现布局和示例数据筛选，文件列表、进度和播放控制尚未接入实际日志 |
| 中文字体 | 已接入 14 px 字体子集，两种构建共用 |
| Windows 模拟器 | 可独立运行，定时生成示例帧并交给共用统计模块，无需连接 MCU |
| 实时统计 | 按 ID、标准/扩展、数据/远程分别汇总，保存最新数据、计数和相邻两帧间隔 |
| CAN、SD 状态显示 | 已接入控制器运行状态和日志状态，支持接收异常计数显示，待实机验证 |

**开发板的接收表格、总帧数及状态栏已接入实际数据。回放文件名与进度仍为示例值。Windows 模拟器使用本机生成的示例帧和预设运行状态，不读取开发板的 CAN、SD 状态。**

## 硬件与软件配置

| 项目 | 当前配置 |
| --- | --- |
| MCU | STM32F407IGH6，Cortex-M4，主频 168 MHz |
| 内存 | 1 MB Flash、128 KB SRAM、64 KB CCMRAM |
| 显示 | 800 × 480 LCD，FSMC 接口，RGB565 |
| 触摸 | GT911，I²C 接口 |
| CAN | CAN1，500 kbit/s，Normal 模式 |
| SD 卡 | SDIO 接口，FatFs 文件系统 |
| 时间戳 | TIM2，1 MHz 计数，32 位微秒时间戳 |
| 调试串口 | USART6，115200 bit/s，8N1 |
| 图形库 | LVGL 9.6 |
| 构建工具 | CMake 3.22 及以上、Ninja、GCC |

引脚和外设参数见 [cansight.ioc](cansight.ioc) 及 `Core/Src` 下的初始化代码。更换开发板时，需要核对时钟、引脚、LCD 驱动、触摸方向和 CAN 收发器连接。

当前 CAN 控制器处于 Normal 模式。虽然应用未提供发送功能，控制器仍会对正确接收的报文发送 ACK。静默监听模式尚未作为配置选项接入。

## 工程结构

```text
Core/
  Inc/                          固件头文件与 CanFrame 定义
  Src/main.c                    初始化、CAN 接收队列、主循环与串口输出
  Src/can_logger.c              SD 日志记录
  Src/can_stats.c               CAN 报文汇总、周期和计数
GUI/
  App/ui.c                      共用界面、统计刷新与筛选逻辑
  App/ui.h                      界面初始化接口与字体声明
  App/lv_font_cansight_14.c      中文和 ASCII 字体子集
  Board/LCD/                    LCD 驱动
  Board/Touch/                  GT911 驱动
  Port/                         STM32 的 LVGL 显示与触摸适配
FATFS/                          FatFs 配置与 SD 卡适配
Drivers/                        CMSIS 与 STM32 HAL
Middlewares/Third_Party/
  lvgl/                         LVGL 源码
  lv_conf.h                     STM32 使用的 LVGL 配置
simulator/
  main.c                        Windows 窗口与鼠标输入
  CMakeLists.txt                Windows 模拟器构建配置
cmake/                          ARM 工具链与 CubeMX 构建配置
CMakeLists.txt                  STM32 固件构建入口
CMakePresets.json               Debug、Release 构建预设
STM32F407xx_FLASH.ld            固件链接脚本
cansight.ioc                    CubeMX 工程配置
```

## 构建 STM32 固件

安装 CMake、Ninja 和 Arm GNU Toolchain，并确保 `cmake`、`ninja`、`arm-none-eabi-gcc`、`arm-none-eabi-g++` 可从终端调用。也可以使用 STM32 开发工具提供的相应工具链环境。

在仓库根目录执行：

```powershell
cmake --preset Debug
cmake --build --preset Debug --parallel 4
```

主要输出：

- `build/Debug/cansight.elf`：烧录和调试文件。
- `build/Debug/cansight.map`：链接映射，用于查看代码和数据占用。

使用 ST-Link 配合 STM32CubeProgrammer 或开发环境，将 ELF 文件烧录到开发板。

Release 构建：

```powershell
cmake --preset Release
cmake --build --preset Release --parallel 4
```

Release 文件位于 `build/Release/`。当前 Debug 使用 `-O0 -g3`，Release 使用 `-Os -g0`。

## 构建 Windows 模拟器

模拟器使用 Windows 本机编译器，不能使用 `arm-none-eabi` 工具链。下面以安装在 `C:/msys64` 的 MSYS2 UCRT64 GCC 为例，要求 CMake 和 Ninja 已加入 `PATH`。

在仓库根目录的 PowerShell 中执行首次配置：

```powershell
cmake -S simulator -B simulator/build -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
```

编译并启动：

```powershell
cmake --build simulator/build --target cansight_sim --parallel 4
.\simulator\build\cansight_sim.exe
```

如果 MSYS2 安装在其他位置，修改编译器路径。若运行时提示缺少 MSYS2 运行库，将对应的 `ucrt64/bin` 目录加入当前终端的 `PATH`。

修改界面后，关闭模拟器窗口，再执行编译和启动命令。更换编译器时应使用新的构建目录，避免沿用旧工具链缓存。

模拟器和开发板直接编译同一份 `GUI/App/ui.c` 与字体文件，因此界面改动会作用于两端。模拟器使用 Windows 显示驱动和系统内存分配；开发板使用 LCD、触摸适配及 `lv_conf.h` 中的内存配置。模拟器不包含 CAN 接收和 SD 日志硬件逻辑。

## 界面操作

- 点击“接收”或“SD 回放”切换页面。
- 点击 ID 输入框，使用十六进制键盘输入筛选值。
- 输入 `237` 或 `0x237` 均表示十六进制 ID `0x237`，按数值精确匹配。
- 输入框留空或点击“清除”时取消 ID 筛选。
- 同一 ID 的标准帧和扩展帧均可匹配，表格使用“类型”列区分。
- 筛选只改变当前页面的显示结果；两个页面分别保存筛选值。
- 接收页每页最多显示 8 条汇总记录，通过 `<`、`>` 翻页，按首次收到该报文组合的顺序排列。
- 类型列中的 `RTR` 表示远程帧；远程帧的数据列为空。只收到一帧时周期为 `--`，之后显示最近两帧的时间间隔，单位为毫秒，保留三位小数。

回放页的文件选择、上一帧、下一帧、播放和停止功能仍待实现。

## CAN 接收与日志记录

当前固件的数据流：

```text
CAN1 FIFO0 中断
    → 读取报文并记录 TIM2 时间戳
    → 放入软件接收队列
    → 主循环取出报文
    → CanLogger_AddFrame() 缓冲 / 写入 CSV
    → CanStats_AddFrame() 更新汇总

LVGL 100 ms 定时器 → 读取统计结果 → 刷新接收表格与总帧数

主循环另行调用 lv_timer_handler() 和 CanLogger_Process()
```

CAN 中断不调用 LVGL 或文件系统。界面由 `ui_init()` 创建，主循环持续调用 `lv_timer_handler()` 处理刷新和输入。

统计模块使用固定的 128 条汇总记录，以 ID、IDE、RTR 共同作为键。同一数值 ID 的标准/扩展帧、数据/远程帧分别统计。表满后，已有记录继续更新，新组合不入表并累计未入表帧数，界面显示 `ID table full` 提示。统计表容量不会限制日志写入，也不会因筛选而改变。

顶部总帧数为主循环交给统计模块的报文数量，包含未入表帧；它等于各条目计数之和加未入表帧数，不包含硬件 FIFO 或软件接收队列中丢失的报文。计数器为 32 位无符号数。周期使用无符号时间戳相减，可跨一次 TIM2 回绕，但同一组合相邻两帧的间隔必须小于约 71.6 分钟。

主循环每轮最多处理 32 帧，再返回 LVGL 处理。逐帧串口打印默认关闭；需要时可将 `CAN_RX_DEBUG_PRINT` 定义为 `1`。

### 运行状态与异常提示

状态栏和异常提示约每 100 ms 更新一次，不以是否收到新报文作为刷新条件。

| 显示 | 判定规则 |
| --- | --- |
| CAN1 未启动 | 控制器未处于监听状态，且未检测到启动失败或当前控制器错误 |
| CAN1 运行中 | HAL 状态为监听，且当前无错误警告、错误被动或 Bus-off 标志 |
| CAN1 错误 | 初始化或启动失败、HAL 错误状态，或当前存在上述控制器错误标志 |
| SD 未挂载 | 尚未成功挂载文件系统，包括启动时无卡或挂载失败 |
| SD 就绪 | 文件系统已挂载，但活动日志尚未就绪；当前主要为初始化中的过渡状态 |
| SD 记录中 | 活动日志已成功打开并完成初始同步，可以接受记录 |
| SD 错误 | 日志打开、格式化、写入或同步失败，日志功能已停止 |

“CAN1 运行中”表示控制器处于接收工作状态，不表示总线上一定有其他设备。停止报文发送不会改变该状态。错误警告或错误被动状态解除后可重新显示运行中；当前未实现 Bus-off 自动恢复及启动失败后的自动重试。

异常栏各字段定义如下：

- `Queue lost`：软件接收队列已确认丢弃的帧数。
- `FIFO overruns`：硬件 FIFO0 溢出事件数，无法据此推算精确丢帧数。
- `HAL`：本次启动以来累计发生的 HAL CAN 错误位，以十六进制显示。
- `SD`：最近一次日志相关的 FatFs 错误码，初始化成功后为 0。

上述异常记录持续保留至设备复位，历史错误与当前 CAN 状态分别显示。SD 状态来自文件系统操作结果，不提供独立的物理插卡检测；拔卡后的状态更新取决于后续文件操作何时返回错误。本阶段不支持热插拔后的自动重新挂载。

日志保存在 SD 卡根目录的 `CANLOG.CSV`。文件不存在时创建，存在时从末尾继续追加；当前没有按启动次数或大小自动归档。日志使用 4096 字节文本缓冲区，并在主循环中约每秒写出剩余内容、调用 `f_sync()`。

CSV 格式示例：

```csv
timestamp_us,format,type,id,dlc,data_hex
123456,STD,DATA,0x237,8,12 27 65 74 83 92 87 33
123956,EXT,REMOTE,0x123,8,
```

| 字段 | 含义 |
| --- | --- |
| `timestamp_us` | 接收时的 TIM2 计数值，单位为微秒，不是日期时间 |
| `format` | `STD` 为标准帧，`EXT` 为扩展帧 |
| `type` | `DATA` 为数据帧，`REMOTE` 为远程帧 |
| `id` | 十六进制 CAN ID |
| `dlc` | DLC，当前接收路径限制为 0～8 |
| `data_hex` | 以空格分隔的十六进制数据字节；远程帧为空 |

当前时间戳约每 71.6 分钟回绕一次，设备复位后也会重新计时。由于日志跨启动追加，文件中的时间戳不保证持续递增。历史回放接入前，需要补充日志归档和时间戳处理。

## 字体与内存

界面使用 `lv_font_cansight_14`，由 `NotoSansSC-Regular.ttf` 转换为 LVGL C 字体，字号 14 px、4 bpp，未启用压缩。字体仅包含 ASCII 和当前界面所需的中文、符号，不包含完整中文字库。

新增中文文案时，需要将新增字符加入字体转换范围并重新生成 `GUI/App/lv_font_cansight_14.c`。转换参数记录在该文件头部。字体源码已加入根目录和模拟器的两份 `CMakeLists.txt`；替换文件且保留名称时无需再次添加。

STM32 当前配置了 32 KB LVGL 内存池，另有 800 × 10 像素的 RGB565 显示缓冲区，占 16000 字节。链接报告中的 SRAM 占用包含预留的 LVGL 内存池，但不能反映池内运行时剩余空间。增加控件和业务缓冲区时，应同时关注链接占用、运行时分配和栈空间。

## 后续开发顺序

1. 在开发板上验证本轮状态显示和异常提示。
2. 实现日志归档，区分正在写入的文件和可回放的历史文件。
3. 接入历史文件列表和 CSV 流式读取。
4. 实现逐帧查看、ID 筛选及时间戳回绕处理。
5. 接入播放、暂停、停止和进度显示；回放仅用于显示，不向 CAN 总线发送报文。
6. 进行边接收边回放、高负载接收、SD 异常、长时间运行及内存占用验证。

## 开发说明

- CubeMX 生成文件中的自定义代码应放在 `USER CODE` 区域，重新生成后检查差异。
- `Core/Src/can.c` 的 `HAL_CAN_Init()` 失败分支已改为返回，由主循环显示错误。该分支位于生成区域，CubeMX 重新生成后须核对，避免恢复为阻塞式 `Error_Handler()`。
- 新增共用界面源码时，分别更新固件和模拟器的 CMake 配置。
- 逐帧串口调试输出使用阻塞发送，默认关闭；开启后会影响高报文速率下的处理能力。
- `build/` 等构建目录与 `.vscode/` 本地配置不纳入版本管理。若本地关闭 cSpell，提交前应自行检查命名和拼写。
- STM32 HAL、CMSIS、FatFs、LVGL 和字体等第三方内容遵循各自附带的许可说明。
