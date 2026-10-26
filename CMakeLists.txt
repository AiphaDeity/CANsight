cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME cansight)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# 从 CubeMX 链接脚本派生 CCM NOLOAD 版本，避免队列零值占用 Flash。
# 原脚本仍由 CubeMX 维护；格式变化时明确报错，防止悄悄丢失内存布局约束。
set(cansight_generated_ld "${CMAKE_CURRENT_SOURCE_DIR}/STM32F407xx_FLASH.ld")
file(READ "${cansight_generated_ld}" cansight_linker_text)
if(NOT cansight_linker_text MATCHES "[.]ccmram[ \t\r\n]*:")
    message(FATAL_ERROR "Review generated CCM section before configuring the RX queue")
endif()
string(REGEX REPLACE "[.]ccmram[ \t\r\n]*:" ".ccmram (NOLOAD) :" cansight_linker_text "${cansight_linker_text}")
string(REGEX REPLACE ">CCMRAM[ \t]+AT>[ \t]*FLASH" ">CCMRAM" cansight_linker_text "${cansight_linker_text}")
set(cansight_memory_ld "${CMAKE_CURRENT_BINARY_DIR}/cansight_memory.ld")
file(WRITE "${cansight_memory_ld}" "${cansight_linker_text}")
if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "STM32F407xx_FLASH[.]ld")
    message(FATAL_ERROR "Expected generated linker script in toolchain flags")
endif()
string(REPLACE "${cansight_generated_ld}" "${cansight_memory_ld}" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(FIND "${CMAKE_EXE_LINKER_FLAGS}" "${cansight_memory_ld}" cansight_memory_ld_position)
if(cansight_memory_ld_position EQUAL -1)
    message(FATAL_ERROR "Cannot select derived CCM linker script; review toolchain path")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cansight_generated_ld}")

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# CubeMX 会重写默认 SD 驱动；从生成的源列表移除模板，由工程自有 DMA 实现接管。
get_target_property(cansight_application_sources ${CMAKE_PROJECT_NAME} SOURCES)
list(LENGTH cansight_application_sources cansight_sources_before_sd_filter)
list(FILTER cansight_application_sources EXCLUDE REGEX "/FATFS/Target/sd_diskio[.]c$")
list(LENGTH cansight_application_sources cansight_sources_after_sd_filter)
math(EXPR cansight_sd_template_count
    "${cansight_sources_before_sd_filter} - ${cansight_sources_after_sd_filter}")
if(NOT cansight_sd_template_count EQUAL 1)
    message(FATAL_ERROR "Expected one generated sd_diskio.c; review the SD DMA driver source selection")
endif()
set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY SOURCES "${cansight_application_sources}")
message(STATUS "SD disk driver: custom DMA (Core/Src/sd_diskio_dma.c)")
set(LV_BUILD_CONF_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/lv_conf.h"
    CACHE PATH "LVGL configuration file" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 使用 LVGL 9.6 自带的 CMake 构建脚本
add_subdirectory(Middlewares/Third_Party/lvgl)

# Debug 对照仅优化 LVGL 库，PRIVATE 防止选项传播到接收、存储和显示驱动。
option(CANSIGHT_LVGL_DEBUG_O2 "Use -O2 for LVGL in firmware Debug builds" ON)
if(CANSIGHT_LVGL_DEBUG_O2)
    target_compile_options(lvgl PRIVATE
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,GNU>>:-O2>"
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU>>:-O2>"
    )
endif()
message(STATUS "LVGL Debug -O2: ${CANSIGHT_LVGL_DEBUG_O2}")

# 仅在固件 Debug 中启用单函数优化，避免改变 LCD 初始化及其他驱动代码。
option(CANSIGHT_LCD_FILL_DEBUG_O2 "Use -O2 for LCD_Color_Fill in firmware Debug builds" ON)
if(CANSIGHT_LCD_FILL_DEBUG_O2)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,GNU>>:CANSIGHT_LCD_FILL_O2=1>"
    )
endif()
message(STATUS "LCD fill Debug -O2: ${CANSIGHT_LCD_FILL_DEBUG_O2}")

# 仅优化日志、统计与时间格式化源文件，不传播到 HAL 或 SD 适配层。
option(CANSIGHT_RX_LOG_DEBUG_O2 "Use -O2 for CAN logging and statistics in firmware Debug builds" ON)
if(CANSIGHT_RX_LOG_DEBUG_O2)
    set_property(SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_logger.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_stats.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_time.c"
        APPEND PROPERTY COMPILE_OPTIONS
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,GNU>>:-O2>"
    )
endif()
message(STATUS "CAN RX/log Debug -O2: ${CANSIGHT_RX_LOG_DEBUG_O2}")

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_logger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sd_diskio_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_timestamp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/log_player.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Port/lv_port_disp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Port/lv_port_indev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/LCD/lcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/Touch/gt911.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/App/ui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/App/lv_font_cansight_14.c
)

set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${cansight_memory_ld}")

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Port
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/LCD
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/Touch
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/App

)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    # Add user defined libraries
    lvgl
)
