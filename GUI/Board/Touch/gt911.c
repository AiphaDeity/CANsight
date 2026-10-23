#include "gt911.h"
#include "lcd_touch.h"
#include "usart.h"
#include "string.h"
#include <stdio.h>
#include "lcd.h"
#include "main.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"

uint16_t line_x,line_y;	
uint16_t CooX,CooY;

uint8_t touch_buf[4];
uint8_t touch_event = 0;

// 正常触摸事务约毫秒内完成，故障时不允许单次 HAL 等待长达数秒。
__IO uint32_t  I2CTimeout = 20U;

static void Set_GPIO_MODER(GPIO_TypeDef* GPIOx, uint32_t moder, uint16_t GPIO_Pin)
{
  assert_param(IS_GPIO_ALL_INSTANCE(GPIOx));
  assert_param(IS_GPIO_MODE(moder));

  uint32_t reg = 0;

  reg = GPIOx->MODER;
  reg &= ~((GPIO_MODER_MODE0_Msk) << (GPIO_Pin * 2U));
  reg |= ((moder) << (GPIO_Pin * 2U));
  GPIOx->MODER = reg;
}

void init_gt911(uint16_t slave_addr)
{
	  Set_GPIO_MODER(GPIOI, GPIO_MODE_OUTPUT_PP, 2);

	  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_RESET);
	  HAL_GPIO_WritePin(GPIOI, GPIO_PIN_2, GPIO_PIN_RESET);

	  HAL_Delay(5);
	  HAL_GPIO_WritePin(GPIOI, GPIO_PIN_2, (slave_addr == 0x14) ? GPIO_PIN_SET : GPIO_PIN_RESET);
	  HAL_Delay(1);
	  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_SET);
	  HAL_Delay(10);

	  Set_GPIO_MODER(GPIOI, GPIO_MODE_INPUT, 2);
}

int gt911_send_data(uint16_t cmd, uint8_t* data, int len)
{
	return HAL_I2C_Mem_Write(&hi2c1, ((GT911_I2C_SLAVE<<1)|I2C_WRITE_BIT), cmd, I2C_MEMADD_SIZE_16BIT, data, len, I2CTimeout);
}

int gt911_read_data(uint16_t cmd, uint8_t* data, int len)
{
	return HAL_I2C_Mem_Read(&hi2c1, ((GT911_I2C_SLAVE<<1)|I2C_READ_BIT), cmd, I2C_MEMADD_SIZE_16BIT, data, len, I2CTimeout);
}

const uint16_t GT911_TPX_TBL[5]={GT_TP1_REG,GT_TP2_REG,GT_TP3_REG,GT_TP4_REG,GT_TP5_REG};
uint8_t gt911_scanning_event(uint8_t mode)
{
	uint8_t buf[4];
	uint8_t i=0;
	uint8_t res=0;
	uint8_t temp;
	uint8_t tempsta;
	
	static uint8_t t=0;//控制查询间隔,从而降低CPU占用率   
	t++;
	
	if ((t % 10) == 0 && t > 0)		//空闲时,每进入10次CTP_Scan函数才检测1次,从而节省CPU使用率
	{
		gt911_read_data(GT_GSTID_REG,&mode,1);	//读取触摸点的状态 
		
		if(mode&0X80&&((mode&0XF)<6)) {
			temp=0;
			gt911_send_data(GT_GSTID_REG,&temp,1);//清标志 		
		}	
			
		if((mode&0XF)&&((mode&0XF)<6)) {
			temp=0XFF<<(mode&0XF);		//将点的个数转换为1的位数,匹配tp_dev.sta定义 
			tempsta=tp_dev.sta;			//保存当前的tp_dev.sta值
			tp_dev.sta=(~temp)|TP_PRES_DOWN|TP_CATH_PRES; 
			tp_dev.x[4]=tp_dev.x[0];	//保存触点0的数据
			tp_dev.y[4]=tp_dev.y[0];
			
			for(i=0;i<5;i++) {
				if(tp_dev.sta&(1<<i)) {		//触摸有效?
					gt911_read_data(GT911_TPX_TBL[i],buf,4);	//读取XY坐标值
					if(tp_dev.touchtype&0X01) {		//横屏
						tp_dev.y[i]=((uint16_t)buf[1]<<8)+buf[0];
						tp_dev.x[i]=800-(((uint16_t)buf[3]<<8)+buf[2]);
					} else {
						tp_dev.x[i]=((uint16_t)buf[1]<<8)+buf[0];
						tp_dev.y[i]=((uint16_t)buf[3]<<8)+buf[2];
					}  
				}			
			} 
			printf("x[%d],y[%d]\r\n",tp_dev.x[0],tp_dev.y[0]);
			res=1;
			if(tp_dev.x[0]>lcddev.width||tp_dev.y[0]>lcddev.height) { 	//非法数据(坐标超出了)
				if((mode&0XF)>1) {  //有其他点有数据,则复第二个触点的数据到第一个触点.
					tp_dev.x[0]=tp_dev.x[1];
					tp_dev.y[0]=tp_dev.y[1];
					t=0;				//触发一次,则会最少连续监测10次,从而提高命中率
				} else {   //非法数据,则忽略此次数据(还原原来的) 
					tp_dev.x[0]=tp_dev.x[4];
					tp_dev.y[0]=tp_dev.y[4];
					mode=0X80;		
					tp_dev.sta=tempsta;	//恢复tp_dev.sta
				}
			}else t=0;					//触发一次,则会最少连续监测10次,从而提高命中率
		}
	}

	if((mode&0X8F)==0X80)//无触摸点按下
	{ 
		if(tp_dev.sta & TP_PRES_DOWN)	//之前是被按下的
		{
			tp_dev.sta&=~(1<<7);	//标记按键松开
			line_x=tp_dev.x[0];
			line_y=tp_dev.y[0];
			CooX=tp_dev.x[0];
			CooY=tp_dev.y[0];
		}else						//之前就没有被按下
		{ 
			tp_dev.x[0]=0xffff;
			tp_dev.y[0]=0xffff;
			tp_dev.sta&=0XE0;	//清除点有效标记	
		}	 
	} 	
	if(t>240)t=10;//重新从10开始计数
	
	return res;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	if (GPIO_Pin == GPIO_PIN_2) {
		touch_event = 1;
	}
}
























