/*
 * @Date: 2024-05-22
 * @Description: LCD显示屏初始化
 */
#ifndef __BSP_LCD_H
#define __BSP_LCD_H

#include "main.h"

//LCD重要参数集
typedef struct
{
    uint16_t width;   //LCD 宽度
    uint16_t height;  //LCD 高度
    uint16_t id;      //LCD ID
    uint8_t dir;      //横屏还是竖屏控制：0，竖屏；1，横屏。
    uint16_t wramcmd; //开始写gram指令
    uint16_t setxcmd; //设置x坐标指令
    uint16_t setycmd; //设置y坐标指令
} _lcd_dev;

//LCD参数
extern _lcd_dev lcddev; //管理LCD重要参数


//LCD地址结构体
typedef struct
{
    uint16_t LCD_REG;
    uint16_t LCD_RAM;
} LCD_TypeDef;
//使用NOR/SRAM的 Bank1.sector4,地址位HADDR[27,26]=11 A0作为数据命令区分线
//注意设置时STM32内部会右移一位对其! 111 1110=0X7E
#define LCD_BASE ((uint32_t)(0x6C000000))
// FSMC 映射寄存器按 volatile 访问，防止编译器省略或合并硬件读写。
#define LCD ((volatile LCD_TypeDef *)LCD_BASE)


//扫描方式
#define L2R_U2D 0 //从左到右,从上到下
#define L2R_D2U 1 //从左到右,从下到上
#define R2L_U2D 2 //从右到左,从上到下
#define R2L_D2U 3 //从右到左,从下到上

#define U2D_L2R 4 //从上到下,从左到右
#define U2D_R2L 5 //从上到下,从右到左
#define D2U_L2R 6 //从下到上,从左到右
#define D2U_R2L 7 //从下到上,从右到左

#define DFT_SCAN_DIR D2U_L2R //默认的扫描方向

// 保留当前屏幕初始化和 LVGL 刷新使用的接口。
void LCD_Init(void);
void LCD_WR_REG(uint16_t regval);
void LCD_WR_DATA(uint16_t data);
void LCD_WriteReg(uint16_t reg, uint16_t value);
void LCD_WriteRAM_Prepare(void);
void LCD_SetCursor(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey);
void LCD_Scan_Dir(uint8_t dir);
void LCD_Display_Dir(uint8_t dir);
void LCD_Color_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t *color);

#endif
