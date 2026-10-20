cmake_minimum_required(VERSION 3.22)

project(cansight_sim LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 使用 STM32 项目中已有的 LVGL 9.6 源码
set(LVGL_DIR "${CMAKE_CURRENT_LIST_DIR}/../Middlewares/Third_Party/lvgl")

# 首次运行先用 LVGL 内部默认配置
set(LV_CONF_SKIP ON CACHE BOOL "Use LVGL internal defaults" FORCE)

# 电脑模拟暂时不编译示例、Demo 和矢量图形扩展
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL
    "Do not build LVGL examples in the simulator" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL
    "Do not build LVGL demos in the simulator" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL
    "Do not build optional vector graphics support" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL
    "Do not build optional vector graphics support" FORCE)

add_subdirectory("${LVGL_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")

# 使用 LVGL 自带的 Windows 窗口和 Windows 操作系统支持
target_compile_definitions(lvgl PUBLIC
    LV_USE_WINDOWS=1
    LV_USE_OS=5
)

# 创建可执行文件
add_executable(cansight_sim main.c) 

# 链接库
target_link_libraries(cansight_sim PRIVATE
    lvgl
    user32
    gdi32
)