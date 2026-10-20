#include "lcd_touch.h"
#include "lcd.h"
#include <stdio.h>

tp_dev_t tp_dev;

//触摸屏初始化
//返回值:0,没有进行校准
//       1,进行过校准
uint8_t TP_Init(void)
{
  init_gt911(GT911_I2C_SLAVE);

  tp_dev.touchtype |= 0X80;				 //电容屏
  tp_dev.touchtype |= lcddev.dir & 0X01; //横屏还是竖屏
	
  return 0;
}

