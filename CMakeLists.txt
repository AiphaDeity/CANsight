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

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)
set(LV_BUILD_CONF_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/lv_conf.h"
    CACHE PATH "LVGL configuration file" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 使用 LVGL 9.6 自带的 CMake 构建脚本
add_subdirectory(Middlewares/Third_Party/lvgl)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_logger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Port/lv_port_disp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Port/lv_port_indev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/LCD/lcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/LCD/delay.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/Touch/gt911.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Board/Touch/lcd_touch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/App/ui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/App/lv_font_cansight_14.c
)

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
