#ifndef __GT911_H
#define __GT911_H

#include "main.h"

#define GT911_I2C_SLAVE		0x14

#define I2C_WRITE_BIT	(0)
#define I2C_READ_BIT	(1)

//I2C读写命令
#define GT_CMD_WR 		0X28     	//写命令
#define GT_CMD_RD 		0X29		//读命令
  
//GT911 部分寄存器定义
#define GT_CTRL_REG 	0X8040   	//控制寄存器
#define GT_CFGS_REG 	0X8047   	//配置起始地址寄存器
#define GT_CHECK_REG 	0X80FF   	//校验和寄存器
#define GT_PID_REG 		0X8140   	//产品ID寄存器

#define GT_GSTID_REG 	0X814E   	//当前检测到的触摸情况
#define GT_TP1_REG 		0X8150  	//第一个触摸点数据地址ַ
#define GT_TP2_REG 		0X8158		//ַ第二个触摸点数据地址
#define GT_TP3_REG 		0X8160		//第三个触摸点数据地址ַ
#define GT_TP4_REG 		0X8168		//第四个触摸点数据地址ַ
#define GT_TP5_REG 		0X8170		//第五个触摸点数据地址ַ

#define I2C_Open_FLAG_TIMEOUT         ((uint32_t)0x1000)
#define I2C_Open_LONG_TIMEOUT         ((uint32_t)0xffff)

void init_gt911(uint16_t slave_addr);
int gt911_send_data(uint16_t cmd, uint8_t* data, int len);
int gt911_read_data(uint16_t cmd, uint8_t* data, int len);

#endif













