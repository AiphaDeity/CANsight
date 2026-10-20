#include "lcd.h"
#include "stdlib.h"
#include "delay.h"
#include "stdio.h"
#include "FONT.H"

// LCD的画笔颜色和背景色
uint16_t POINT_COLOR = BLACK; // 画笔颜色
uint16_t BACK_COLOR = WHITE;	// 背景色

// 管理LCD重要参数
// 默认为竖屏
_lcd_dev lcddev;

// 写寄存器函数
// regval:寄存器值
void LCD_WR_REG(uint16_t regval)
{
	volatile int i = 0;
	regval = regval;			 // 使用-O2优化的时候,必须插入的延时
	LCD->LCD_REG = regval; // 写入要写的寄存器序号
}
// 写LCD数据
// data:要写入的值
void LCD_WR_DATA(uint16_t data)
{
	volatile int i = 0;
	data = data; // 使用-O2优化的时候,必须插入的延时
	LCD->LCD_RAM = data;
}

// 读LCD数据
// 返回值:读到的值
uint16_t LCD_RD_DATA(void)
{
	uint16_t ram; // 防止被优化
	ram = LCD->LCD_RAM;
	return ram;
}
// 写寄存器
// LCD_Reg:寄存器地址
// LCD_RegValue:要写入的数据
void LCD_WriteReg(uint16_t LCD_Reg, uint16_t LCD_RegValue)
{
	LCD->LCD_REG = LCD_Reg;			 // 写入要写的寄存器序号
	LCD->LCD_RAM = LCD_RegValue; // 写入数据
}
// 读寄存器
// LCD_Reg:寄存器地址
// 返回值:读到的数据
uint16_t LCD_ReadReg(uint16_t LCD_Reg)
{
	LCD_WR_REG(LCD_Reg); // 写入要读的寄存器序号
	delay_us(20);
	return LCD_RD_DATA(); // 返回读到的值
}
// 开始写GRAM
void LCD_WriteRAM_Prepare(void)
{
	LCD->LCD_REG = lcddev.wramcmd;
}
// LCD写GRAM
// RGB_Code:颜色值
void LCD_WriteRAM(uint16_t RGB_Code)
{
	LCD->LCD_RAM = RGB_Code; // 写十六位GRAM
}
// 从ILI93xx读出的数据为GBR格式，而我们写入的时候为RGB格式。
// 通过该函数转换
// c:GBR格式的颜色值
// 返回值：RGB格式的颜色值
uint16_t LCD_BGR2RGB(uint16_t c)
{
	uint16_t r, g, b, rgb;
	b = (c >> 0) & 0x1f;
	g = (c >> 5) & 0x3f;
	r = (c >> 11) & 0x1f;
	rgb = (b << 11) + (g << 5) + (r << 0);
	return (rgb);
}
// 当mdk -O1时间优化时需要设置
// 延时i
void opt_delay(uint8_t i)
{
	while (i--)
		;
}
// 读取个某点的颜色值
// x,y:坐标
// 返回值:此点的颜色
uint16_t LCD_ReadPoint(uint16_t x, uint16_t y)
{
	uint16_t r = 0;
	if (x >= lcddev.width || y >= lcddev.height)
		return 0; // 超过了范围,直接返回
	LCD_SetCursor(x, y, x, y);
	r = LCD_RD_DATA();		// dummy Read
	opt_delay(2);
	r = LCD_RD_DATA();			 // 实际坐标颜色

	return 0;
}
// LCD开启显示
void LCD_DisplayOn(void)
{
	if (lcddev.id == 0X8009)
		LCD_WR_REG(0X2900); // 开启显示
}
// LCD关闭显示
void LCD_DisplayOff(void)
{
	if (lcddev.id == 0X8009)
		LCD_WR_REG(0X2800); // 关闭显示
}
// 设置光标位置
// Xpos:横坐标
// Ypos:纵坐标
void LCD_SetCursor(uint16_t Xspos, uint16_t Yspos,uint16_t Xepos, uint16_t Yepos)
{
	if (lcddev.id == 0x8009)
	{
		/**
		在查看8009A的芯片手册之后，得出
		0x2A00: x轴起始位置的高8位
		0x2A01: x轴起始位置的低8位
		0x2A02: x轴最终位置的高8位
		0x2A03: x轴最终位置的低8位
		*/
		LCD_WR_REG(0x2A00);
		LCD_WR_DATA(Xspos >> 8);
		LCD_WR_REG(0x2A01);
		LCD_WR_DATA(Xspos & 0xff);
		LCD_WR_REG(0x2A02);
		LCD_WR_DATA(Xepos >> 8);
		LCD_WR_REG(0x2A03);
		LCD_WR_DATA(Xepos & 0xFF);

		/**
		在查看8009A的芯片手册之后，得出
		0x2B00: y轴起始位置的高8位
		0x2B01: y轴起始位置的低8位
		0x2B02: y轴最终位置的高8位
		0x2B03: y轴最终位置的低8位
		*/
		LCD_WR_REG(0x2B00);
		LCD_WR_DATA(Yspos >> 8);
		LCD_WR_REG(0x2B01);
		LCD_WR_DATA(Yspos & 0xff);
		LCD_WR_REG(0x2B02);
		LCD_WR_DATA(Yepos >> 8);
		LCD_WR_REG(0x2B03);
		LCD_WR_DATA(Yepos & 0xFF);

	}
}
// 设置LCD的自动扫描方向
// 注意:其他函数可能会受到此函数设置的影响(尤其是9341/6804这两个奇葩),
// 所以,一般设置为L2R_U2D即可,如果设置为其他扫描方式,可能导致显示不正常.
// dir:0~7,代表8个方向(具体定义见lcd.h)
// 9320/9325/9328/4531/4535/1505/b505/5408/9341/5310/5510/1963等IC已经实际测试
void LCD_Scan_Dir(uint8_t dir)
{
	uint16_t regval = 0;
	uint16_t dirreg = 0;
	uint16_t temp;

	if (lcddev.id == 0X5510 || lcddev.id == 0X8009)
	{
		switch (dir)
		{
		case L2R_U2D:
			regval |= (0 << 7) | (0 << 6) | (0 << 5);
			break;
		case L2R_D2U:
			regval |= (1 << 7) | (0 << 6) | (0 << 5);
			break;
		case R2L_U2D:
			regval |= (0 << 7) | (1 << 6) | (0 << 5);
			break;
		case R2L_D2U:
			regval |= (1 << 7) | (1 << 6) | (0 << 5);
			break;
		case U2D_L2R:
			regval |= (0 << 7) | (0 << 6) | (1 << 5);
			break;
		case U2D_R2L:
			regval |= (0 << 7) | (1 << 6) | (1 << 5);
			break;
		case D2U_L2R:
			regval |= (1 << 7) | (0 << 6) | (1 << 5);
			break;
		case D2U_R2L:
			regval |= (1 << 7) | (1 << 6) | (1 << 5);
			break;
		default:
			break;
		}
		if (lcddev.id == 0X5510 || lcddev.id == 0X8009)
			dirreg = 0X3600;

		LCD_WriteReg(dirreg, regval);

		if (regval & 0X20)
		{
			if (lcddev.width < lcddev.height) // 交换X,Y
			{
				temp = lcddev.width;
				lcddev.width = lcddev.height;
				lcddev.height = temp;
			}
		}
		else
		{
			if (lcddev.width > lcddev.height) // 交换X,Y
			{
				temp = lcddev.width;
				lcddev.width = lcddev.height;
				lcddev.height = temp;
			}
		}

		if (lcddev.id == 0X5510 || lcddev.id == 0X8009)
		{
			LCD_WR_REG(lcddev.setxcmd);
			LCD_WR_DATA(0);
			LCD_WR_REG(lcddev.setxcmd + 1);
			LCD_WR_DATA(0);
			LCD_WR_REG(lcddev.setxcmd + 2);
			LCD_WR_DATA((lcddev.width - 1) >> 8);
			LCD_WR_REG(lcddev.setxcmd + 3);
			LCD_WR_DATA((lcddev.width - 1) & 0XFF);
			LCD_WR_REG(lcddev.setycmd);
			LCD_WR_DATA(0);
			LCD_WR_REG(lcddev.setycmd + 1);
			LCD_WR_DATA(0);
			LCD_WR_REG(lcddev.setycmd + 2);
			LCD_WR_DATA((lcddev.height - 1) >> 8);
			LCD_WR_REG(lcddev.setycmd + 3);
			LCD_WR_DATA((lcddev.height - 1) & 0XFF);
		}
		else
		{
			LCD_WR_REG(lcddev.setxcmd);
			LCD_WR_DATA(0);
			LCD_WR_DATA(0);
			LCD_WR_DATA((lcddev.width - 1) >> 8);
			LCD_WR_DATA((lcddev.width - 1) & 0XFF);
			LCD_WR_REG(lcddev.setycmd);
			LCD_WR_DATA(0);
			LCD_WR_DATA(0);
			LCD_WR_DATA((lcddev.height - 1) >> 8);
			LCD_WR_DATA((lcddev.height - 1) & 0XFF);
		}
	}
}
// 画点
// x,y:坐标
// POINT_COLOR:此点的颜色
void LCD_DrawPoint(uint16_t x, uint16_t y)
{
	LCD_SetCursor(x, y, x, y);		// 设置光标位置
	LCD_WriteRAM_Prepare(); // 开始写入GRAM
	LCD->LCD_RAM = POINT_COLOR;
}
// 快速画点
// x,y:坐标
// color:颜色
void LCD_Fast_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
	if (lcddev.id == 0X9341 || lcddev.id == 0X5310)
	{
		LCD_WR_REG(lcddev.setxcmd);
		LCD_WR_DATA(x >> 8);
		LCD_WR_DATA(x & 0XFF);
		LCD_WR_REG(lcddev.setycmd);
		LCD_WR_DATA(y >> 8);
		LCD_WR_DATA(y & 0XFF);
	}
	else if (lcddev.id == 0X5510)
	{
		LCD_WR_REG(lcddev.setxcmd);
		LCD_WR_DATA(x >> 8);
		LCD_WR_REG(lcddev.setxcmd + 1);
		LCD_WR_DATA(x & 0XFF);
		LCD_WR_REG(lcddev.setycmd);
		LCD_WR_DATA(y >> 8);
		LCD_WR_REG(lcddev.setycmd + 1);
		LCD_WR_DATA(y & 0XFF);
	}
	else if (lcddev.id == 0X1963)
	{
		if (lcddev.dir == 0)
			x = lcddev.width - 1 - x;
		LCD_WR_REG(lcddev.setxcmd);
		LCD_WR_DATA(x >> 8);
		LCD_WR_DATA(x & 0XFF);
		LCD_WR_DATA(x >> 8);
		LCD_WR_DATA(x & 0XFF);
		LCD_WR_REG(lcddev.setycmd);
		LCD_WR_DATA(y >> 8);
		LCD_WR_DATA(y & 0XFF);
		LCD_WR_DATA(y >> 8);
		LCD_WR_DATA(y & 0XFF);
	}
	else if (lcddev.id == 0X6804)
	{
		if (lcddev.dir == 1)
			x = lcddev.width - 1 - x; // 横屏时处理
		LCD_WR_REG(lcddev.setxcmd);
		LCD_WR_DATA(x >> 8);
		LCD_WR_DATA(x & 0XFF);
		LCD_WR_REG(lcddev.setycmd);
		LCD_WR_DATA(y >> 8);
		LCD_WR_DATA(y & 0XFF);
	}
	else if (lcddev.id == 0x8009)
	{
		LCD_SetCursor(x, y, x, y); // 设置坐标
	}
	else
	{
		if (lcddev.dir == 1)
			x = lcddev.width - 1 - x; // 横屏其实就是调转x,y坐标
		LCD_WriteReg(lcddev.setxcmd, x);
		LCD_WriteReg(lcddev.setycmd, y);
	}

	LCD_WriteRAM_Prepare(); // 开始写入GRAM
	LCD->LCD_RAM = color;
}
// SSD1963 背光设置
// pwm:背光等级,0~100.越大越亮.
void LCD_SSD_BackLightSet(uint8_t pwm)
{
	LCD_WR_REG(0xBE);				 // 配置PWM输出
	LCD_WR_DATA(0x05);			 // 1设置PWM频率
	LCD_WR_DATA(pwm * 2.55); // 2设置PWM占空比
	LCD_WR_DATA(0x01);			 // 3设置C
	LCD_WR_DATA(0xFF);			 // 4设置D
	LCD_WR_DATA(0x00);			 // 5设置E
	LCD_WR_DATA(0x00);			 // 6设置F
}

// 设置LCD显示方向
// dir:0,竖屏；1,横屏
void LCD_Display_Dir(uint8_t dir)
{
	if (dir == 0) // 竖屏
	{
		lcddev.dir = 0x00;
		lcddev.width = 480;
		lcddev.height = 800;
		lcddev.setxcmd = 0X2A00;
		lcddev.setycmd = 0X2B00;
		lcddev.wramcmd = 0X2C00;
	}
	else // 横屏
	{
		lcddev.dir = 0x01;
		lcddev.width = 800;			 // LCD 宽度
		lcddev.height = 480;		 // LCD 高度
		lcddev.setxcmd = 0X2A00; // 设置x坐标指令2A
		lcddev.setycmd = 0X2B00; // 设置y坐标指令2B
		lcddev.wramcmd = 0X2C00; // 开始写gram指令
	}
	LCD_Scan_Dir(DFT_SCAN_DIR); // 默认扫描方向
}
// 设置窗口,并自动设置画点坐标到窗口左上角(sx,sy).
// sx,sy:窗口起始坐标(左上角)
// width,height:窗口宽度和高度,必须大于0!!
// 窗体大小:width*height.
void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height)
{
	uint16_t twidth, theight;
	twidth = sx + width - 1;
	theight = sy + height - 1;

	LCD_WR_REG(lcddev.setxcmd);
	LCD_WR_DATA(sx >> 8);
	LCD_WR_REG(lcddev.setxcmd + 1);
	LCD_WR_DATA(sx & 0XFF);
	LCD_WR_REG(lcddev.setxcmd + 2);
	LCD_WR_DATA(twidth >> 8);
	LCD_WR_REG(lcddev.setxcmd + 3);
	LCD_WR_DATA(twidth & 0XFF);
	LCD_WR_REG(lcddev.setycmd);
	LCD_WR_DATA(sy >> 8);
	LCD_WR_REG(lcddev.setycmd + 1);
	LCD_WR_DATA(sy & 0XFF);
	LCD_WR_REG(lcddev.setycmd + 2);
	LCD_WR_DATA(theight >> 8);
	LCD_WR_REG(lcddev.setycmd + 3);
	LCD_WR_DATA(theight & 0XFF);
}

// 初始化LCD
void LCD_Init(void)
{
	HAL_Delay(50); // delay 50 ms
	//LCD_WriteReg(0x0000, 0x0001);
	HAL_Delay(100);																 // delay 50 ms

	lcddev.id = 0x8009;

	// 重新配置写时序控制寄存器的时序
	FSMC_Bank1E->BWTR[6] &= ~(0XF << 0); // 地址建立时间(ADDSET)清零
	FSMC_Bank1E->BWTR[6] &= ~(0XF << 8); // 数据保存时间清零
	FSMC_Bank1E->BWTR[6] |= 3 << 0;			 // 地址建立时间(ADDSET)为3个HCLK =18ns
	FSMC_Bank1E->BWTR[6] |= 2 << 8;			 // 数据保存时间(DATAST)为6ns*3个HCLK=18ns

	/// HSD43+OTM8009A  20221116  ok
	LCD_WR_REG(0xff00); //
	LCD_WR_DATA(0x80);
	LCD_WR_REG(0xff01); // enable EXTC
	LCD_WR_DATA(0x09);
	LCD_WR_REG(0xff02); //
	LCD_WR_DATA(0x01);

	LCD_WR_REG(0xff80); // enable Orise mode
	LCD_WR_DATA(0x80);
	LCD_WR_REG(0xff81); //
	LCD_WR_DATA(0x09);

	LCD_WR_REG(0xff03); // enable SPI+I2C cmd2 read
	LCD_WR_DATA(0x01);

	// gamma DC
	LCD_WR_REG(0xc0b4);
	LCD_WR_DATA(0x50); // Panel Driving Mode 0x50  column inversion add 20221116
	LCD_WR_REG(0xC489);
	LCD_WR_DATA(0x08); // reg off	  add
	LCD_WR_REG(0xC0a3);
	LCD_WR_DATA(0x00); // pre-charge //V02   add

	LCD_WR_REG(0xC582); // REG-pump23
	LCD_WR_DATA(0xA3);
	LCD_WR_REG(0xC590); // Pump setting (3x=D6)-->(2x=96)//v02 01/11
	LCD_WR_DATA(0xd6);
	LCD_WR_REG(0xC591); // Pump setting(VGH/VGL)
	LCD_WR_DATA(0x87);
	LCD_WR_REG(0xD800); // GVDD=4.5V
	LCD_WR_DATA(0x73);
	LCD_WR_REG(0xD801); // NGVDD=4.5V
	LCD_WR_DATA(0x71);
	// VCOMDC
	LCD_WR_REG(0xd900); // VCOMDC=
	LCD_WR_DATA(0x58);	// 0x50  20221116

	LCD_WR_REG(0xE1);
	LCD_WR_DATA(0x09);
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x14);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x07);
	LCD_WR_DATA(0x24);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x04);
	LCD_WR_DATA(0x05);
	LCD_WR_DATA(0x06);
	LCD_WR_DATA(0x14);
	LCD_WR_DATA(0x37);
	LCD_WR_DATA(0x34);
	LCD_WR_DATA(0x05);

	LCD_WR_REG(0xE2);
	LCD_WR_DATA(0x09);
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x14);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x07);
	LCD_WR_DATA(0x24);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x04);
	LCD_WR_DATA(0x05);
	LCD_WR_DATA(0x06);
	LCD_WR_DATA(0x14);
	LCD_WR_DATA(0x37);
	LCD_WR_DATA(0x34);
	LCD_WR_DATA(0x05);

	LCD_WR_REG(0xC181); // Frame rate 65Hz//V02
	LCD_WR_DATA(0x66);

	//// RGB I/F setting VSYNC for OTM8018 0x0e

	LCD_WR_REG(0xC1a1); // external Vsync(08)  /Vsync,Hsync(0c) /Vsync,Hsync,DE(0e) //V02(0e)  / all  included clk(0f)
	LCD_WR_DATA(0x08);
	LCD_WR_REG(0xC0a3); // pre-charge //V02
	LCD_WR_DATA(0x1b);
	LCD_WR_REG(0xC481); // source bias //V02
	LCD_WR_DATA(0x83);
	LCD_WR_REG(0xC592); // Pump45
	LCD_WR_DATA(0x01);	//(01)
	LCD_WR_REG(0xC5B1); // DC voltage setting ;[0]GVDD output, default: 0xa8
	LCD_WR_DATA(0xA9);

	//--------------------------------------------------------------------------------
	//		initial setting 2 < tcon_goa_wave >
	//--------------------------------------------------------------------------------
	// CE8x : vst1, vst2, vst3, vst4
	LCD_WR_REG(0xCE80); // ce81[7:0] : vst1_shift[7:0]
	LCD_WR_DATA(0x85);
	LCD_WR_REG(0xCE81); // ce82[7:0] : 0000,	vst1_width[3:0]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCE82); // ce83[7:0] : vst1_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCE83); // ce84[7:0] : vst2_shift[7:0]
	LCD_WR_DATA(0x84);
	LCD_WR_REG(0xCE84); // ce85[7:0] : 0000,	vst2_width[3:0]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCE85); // ce86[7:0] : vst2_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCE86); // ce87[7:0] : vst3_shift[7:0]
	LCD_WR_DATA(0x83);
	LCD_WR_REG(0xCE87); // ce88[7:0] : 0000,	vst3_width[3:0]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCE88); // ce89[7:0] : vst3_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCE89); // ce8a[7:0] : vst4_shift[7:0]
	LCD_WR_DATA(0x82);
	LCD_WR_REG(0xCE8a); // ce8b[7:0] : 0000,	vst4_width[3:0]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCE8b); // ce8c[7:0] : vst4_tchop[7:0]
	LCD_WR_DATA(0x00);

	// CEAx : clka1, clka2
	LCD_WR_REG(0xCEa0); // cea1[7:0] : clka1_width[3:0], clka1_shift[11:8]
	LCD_WR_DATA(0x38);
	LCD_WR_REG(0xCEa1); // cea2[7:0] : clka1_shift[7:0]
	LCD_WR_DATA(0x02);
	LCD_WR_REG(0xCEa2); // cea3[7:0] : clka1_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEa3); // cea4[7:0] : clka1_switch[7:0]
	LCD_WR_DATA(0x21);
	LCD_WR_REG(0xCEa4); // cea5[7:0] : clka1_extend[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEa5); // cea6[7:0] : clka1_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEa6); // cea7[7:0] : clka1_tglue[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEa7); // cea8[7:0] : clka2_width[3:0], clka2_shift[11:8]
	LCD_WR_DATA(0x38);
	LCD_WR_REG(0xCEa8); // cea9[7:0] : clka2_shift[7:0]
	LCD_WR_DATA(0x01);
	LCD_WR_REG(0xCEa9); // ceaa[7:0] : clka2_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEaa); // ceab[7:0] : clka2_switch[7:0]
	LCD_WR_DATA(0x22);
	LCD_WR_REG(0xCEab); // ceac[7:0] : clka2_extend
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEac); // cead[7:0] : clka2_tchop
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEad); // ceae[7:0] : clka2_tglue
	LCD_WR_DATA(0x00);

	// CEBx : clka3, clka4
	LCD_WR_REG(0xCEb0); // ceb1[7:0] : clka3_width[3:0], clka3_shift[11:8]
	LCD_WR_DATA(0x38);
	LCD_WR_REG(0xCEb1); // ceb2[7:0] : clka3_shift[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEb2); // ceb3[7:0] : clka3_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEb3); // ceb4[7:0] : clka3_switch[7:0]
	LCD_WR_DATA(0x23);
	LCD_WR_REG(0xCEb4); // ceb5[7:0] : clka3_extend[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEb5); // ceb6[7:0] : clka3_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEb6); // ceb7[7:0] : clka3_tglue[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEb7); // ceb8[7:0] : clka4_width[3:0], clka2_shift[11:8]
	LCD_WR_DATA(0x30);
	LCD_WR_REG(0xCEb8); // ceb9[7:0] : clka4_shift[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEb9); // ceba[7:0] : clka4_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEba); // cebb[7:0] : clka4_switch[7:0]
	LCD_WR_DATA(0x24);
	LCD_WR_REG(0xCEbb); // cebc[7:0] : clka4_extend
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEbc); // cebd[7:0] : clka4_tchop
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEbd); // cebe[7:0] : clka4_tglue
	LCD_WR_DATA(0x00);

	// CECx : clkb1, clkb2
	LCD_WR_REG(0xCEc0); // cec1[7:0] : clkb1_width[3:0], clkb1_shift[11:8]
	LCD_WR_DATA(0x30);
	LCD_WR_REG(0xCEc1); // cec2[7:0] : clkb1_shift[7:0]
	LCD_WR_DATA(0x01);
	LCD_WR_REG(0xCEc2); // cec3[7:0] : clkb1_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEc3); // cec4[7:0] : clkb1_switch[7:0]
	LCD_WR_DATA(0x25);
	LCD_WR_REG(0xCEc4); // cec5[7:0] : clkb1_extend[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEc5); // cec6[7:0] : clkb1_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEc6); // cec7[7:0] : clkb1_tglue[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEc7); // cec8[7:0] : clkb2_width[3:0], clkb2_shift[11:8]
	LCD_WR_DATA(0x30);
	LCD_WR_REG(0xCEc8); // cec9[7:0] : clkb2_shift[7:0]
	LCD_WR_DATA(0x02);
	LCD_WR_REG(0xCEc9); // ceca[7:0] : clkb2_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEca); // cecb[7:0] : clkb2_switch[7:0]
	LCD_WR_DATA(0x26);
	LCD_WR_REG(0xCEcb); // cecc[7:0] : clkb2_extend
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEcc); // cecd[7:0] : clkb2_tchop
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEcd); // cece[7:0] : clkb2_tglue
	LCD_WR_DATA(0x00);

	// CEDx : clkb3, clkb4
	LCD_WR_REG(0xCEd0); // ced1[7:0] : clkb3_width[3:0], clkb3_shift[11:8]
	LCD_WR_DATA(0x30);
	LCD_WR_REG(0xCEd1); // ced2[7:0] : clkb3_shift[7:0]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEd2); // ced3[7:0] : clkb3_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEd3); // ced4[7:0] : clkb3_switch[7:0]
	LCD_WR_DATA(0x27);
	LCD_WR_REG(0xCEd4); // ced5[7:0] : clkb3_extend[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEd5); // ced6[7:0] : clkb3_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEd6); // ced7[7:0] : clkb3_tglue[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEd7); // ced8[7:0] : clkb4_width[3:0], clkb4_shift[11:8]
	LCD_WR_DATA(0x30);
	LCD_WR_REG(0xCEd8); // ced9[7:0] : clkb4_shift[7:0]
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCEd9); // ceda[7:0] : clkb4_sw_tg, odd_high, flat_head, flat_tail, switch[11:8]
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCEda); // cedb[7:0] : clkb4_switch[7:0]
	LCD_WR_DATA(0x28);
	LCD_WR_REG(0xCEdb); // cedc[7:0] : clkb4_extend
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEdc); // cedd[7:0] : clkb4_tchop
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCEdd); // cede[7:0] : clkb4_tglue
	LCD_WR_DATA(0x00);

	// CFCx :
	LCD_WR_REG(0xCFc0); // cfc1[7:0] : eclk_normal_width[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc1); // cfc2[7:0] : eclk_partial_width[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc2); // cfc3[7:0] : all_normal_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc3); // cfc4[7:0] : all_partial_tchop[7:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc4); // cfc5[7:0] : eclk1_follow[3:0], eclk2_follow[3:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc5); // cfc6[7:0] : eclk3_follow[3:0], eclk4_follow[3:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc6); // cfc7[7:0] : 00, vstmask, vendmask, 00, dir1, dir2 (0=VGL, 1=VGH)
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc7); // cfc8[7:0] : reg_goa_gnd_opt, reg_goa_dpgm_tail_set, reg_goa_f_gating_en, reg_goa_f_odd_gating, toggle_mod1, 2, 3, 4
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc8); // cfc9[7:0] : duty_block[3:0], DGPM[3:0]
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCFc9); // cfca[7:0] : reg_goa_gnd_period[7:0]
	LCD_WR_DATA(0x00);

	// CFDx :
	LCD_WR_REG(0xCFd0); // cfd1[7:0] : 0000000, reg_goa_frame_odd_high
	LCD_WR_DATA(0x00);	// Parameter 1

	//--------------------------------------------------------------------------------
	//		initial setting 3 < Panel setting >
	//--------------------------------------------------------------------------------
	// cbcx
	LCD_WR_REG(0xCBc0); // cbc1[7:0] : enmode H-byte of sig1  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBc1); // cbc2[7:0] : enmode H-byte of sig2  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBc2); // cbc3[7:0] : enmode H-byte of sig3  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBc3); // cbc4[7:0] : enmode H-byte of sig4  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBc4); // cbc5[7:0] : enmode H-byte of sig5  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBc5); // cbc6[7:0] : enmode H-byte of sig6  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBc6); // cbc7[7:0] : enmode H-byte of sig7  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBc7); // cbc8[7:0] : enmode H-byte of sig8  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBc8); // cbc9[7:0] : enmode H-byte of sig9  (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBc9); // cbca[7:0] : enmode H-byte of sig10 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBca); // cbcb[7:0] : enmode H-byte of sig11 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBcb); // cbcc[7:0] : enmode H-byte of sig12 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBcc); // cbcd[7:0] : enmode H-byte of sig13 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBcd); // cbce[7:0] : enmode H-byte of sig14 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBce); // cbcf[7:0] : enmode H-byte of sig15 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);

	// cbdx
	LCD_WR_REG(0xCBd0); // cbd1[7:0] : enmode H-byte of sig16 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd1); // cbd2[7:0] : enmode H-byte of sig17 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd2); // cbd3[7:0] : enmode H-byte of sig18 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd3); // cbd4[7:0] : enmode H-byte of sig19 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd4); // cbd5[7:0] : enmode H-byte of sig20 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd5); // cbd6[7:0] : enmode H-byte of sig21 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd6); // cbd7[7:0] : enmode H-byte of sig22 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd7); // cbd8[7:0] : enmode H-byte of sig23 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd8); // cbd9[7:0] : enmode H-byte of sig24 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBd9); // cbda[7:0] : enmode H-byte of sig25 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBda); // cbdb[7:0] : enmode H-byte of sig26 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBdb); // cbdc[7:0] : enmode H-byte of sig27 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBdc); // cbdd[7:0] : enmode H-byte of sig28 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBdd); // cbde[7:0] : enmode H-byte of sig29 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCBde); // cbdf[7:0] : enmode H-byte of sig30 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x04);

	// cbex
	LCD_WR_REG(0xCBe0); // cbe1[7:0] : enmode H-byte of sig31 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe1); // cbe2[7:0] : enmode H-byte of sig32 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe2); // cbe3[7:0] : enmode H-byte of sig33 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe3); // cbe4[7:0] : enmode H-byte of sig34 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe4); // cbe5[7:0] : enmode H-byte of sig35 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe5); // cbe6[7:0] : enmode H-byte of sig36 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe6); // cbe7[7:0] : enmode H-byte of sig37 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe7); // cbe8[7:0] : enmode H-byte of sig38 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe8); // cbe9[7:0] : enmode H-byte of sig39 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCBe9); // cbea[7:0] : enmode H-byte of sig40 (pwrof_0, pwrof_1, norm, pwron_4 )
	LCD_WR_DATA(0x00);

	// cc8x
	LCD_WR_REG(0xCC80); // cc81[7:0] : reg setting for signal01 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC81); // cc82[7:0] : reg setting for signal02 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC82); // cc83[7:0] : reg setting for signal03 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC83); // cc84[7:0] : reg setting for signal04 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC84); // cc85[7:0] : reg setting for signal05 selection with u2d mode
	LCD_WR_DATA(0x0C);
	LCD_WR_REG(0xCC85); // cc86[7:0] : reg setting for signal06 selection with u2d mode
	LCD_WR_DATA(0x0A);
	LCD_WR_REG(0xCC86); // cc87[7:0] : reg setting for signal07 selection with u2d mode
	LCD_WR_DATA(0x10);
	LCD_WR_REG(0xCC87); // cc88[7:0] : reg setting for signal08 selection with u2d mode
	LCD_WR_DATA(0x0E);
	LCD_WR_REG(0xCC88); // cc89[7:0] : reg setting for signal09 selection with u2d mode
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCC89); // cc8a[7:0] : reg setting for signal10 selection with u2d mode
	LCD_WR_DATA(0x04);

	// cc9x
	LCD_WR_REG(0xCC90); // cc91[7:0] : reg setting for signal11 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC91); // cc92[7:0] : reg setting for signal12 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC92); // cc93[7:0] : reg setting for signal13 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC93); // cc94[7:0] : reg setting for signal14 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC94); // cc95[7:0] : reg setting for signal15 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC95); // cc96[7:0] : reg setting for signal16 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC96); // cc97[7:0] : reg setting for signal17 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC97); // cc98[7:0] : reg setting for signal18 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC98); // cc99[7:0] : reg setting for signal19 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC99); // cc9a[7:0] : reg setting for signal20 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC9a); // cc9b[7:0] : reg setting for signal21 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC9b); // cc9c[7:0] : reg setting for signal22 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC9c); // cc9d[7:0] : reg setting for signal23 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC9d); // cc9e[7:0] : reg setting for signal24 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCC9e); // cc9f[7:0] : reg setting for signal25 selection with u2d mode
	LCD_WR_DATA(0x0B);

	// ccax
	LCD_WR_REG(0xCCa0); // cca1[7:0] : reg setting for signal26 selection with u2d mode
	LCD_WR_DATA(0x09);
	LCD_WR_REG(0xCCa1); // cca2[7:0] : reg setting for signal27 selection with u2d mode
	LCD_WR_DATA(0x0F);
	LCD_WR_REG(0xCCa2); // cca3[7:0] : reg setting for signal28 selection with u2d mode
	LCD_WR_DATA(0x0D);
	LCD_WR_REG(0xCCa3); // cca4[7:0] : reg setting for signal29 selection with u2d mode
	LCD_WR_DATA(0x01);
	LCD_WR_REG(0xCCa4); // cca5[7:0] : reg setting for signal20 selection with u2d mode
	LCD_WR_DATA(0x02);
	LCD_WR_REG(0xCCa5); // cca6[7:0] : reg setting for signal31 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCa6); // cca7[7:0] : reg setting for signal32 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCa7); // cca8[7:0] : reg setting for signal33 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCa8); // cca9[7:0] : reg setting for signal34 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCa9); // ccaa[7:0] : reg setting for signal35 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCaa); // ccab[7:0] : reg setting for signal36 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCab); // ccac[7:0] : reg setting for signal37 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCac); // ccad[7:0] : reg setting for signal38 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCad); // ccae[7:0] : reg setting for signal39 selection with u2d mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCae); // ccaf[7:0] : reg setting for signal40 selection with u2d mode
	LCD_WR_DATA(0x00);

	// ccbx
	LCD_WR_REG(0xCCb0); // ccb1[7:0] : reg setting for signal01 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCb1); // ccb2[7:0] : reg setting for signal02 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCb2); // ccb3[7:0] : reg setting for signal03 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCb3); // ccb4[7:0] : reg setting for signal04 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCb4); // ccb5[7:0] : reg setting for signal05 selection with d2u mode
	LCD_WR_DATA(0x0D);
	LCD_WR_REG(0xCCb5); // ccb6[7:0] : reg setting for signal06 selection with d2u mode
	LCD_WR_DATA(0x0F);
	LCD_WR_REG(0xCCb6); // ccb7[7:0] : reg setting for signal07 selection with d2u mode
	LCD_WR_DATA(0x09);
	LCD_WR_REG(0xCCb7); // ccb8[7:0] : reg setting for signal08 selection with d2u mode
	LCD_WR_DATA(0x0B);
	LCD_WR_REG(0xCCb8); // ccb9[7:0] : reg setting for signal09 selection with d2u mode
	LCD_WR_DATA(0x02);
	LCD_WR_REG(0xCCb9); // ccba[7:0] : reg setting for signal10 selection with d2u mode
	LCD_WR_DATA(0x01);

	// cccx
	LCD_WR_REG(0xCCc0); // ccc1[7:0] : reg setting for signal11 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc1); // ccc2[7:0] : reg setting for signal12 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc2); // ccc3[7:0] : reg setting for signal13 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc3); // ccc4[7:0] : reg setting for signal14 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc4); // ccc5[7:0] : reg setting for signal15 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc5); // ccc6[7:0] : reg setting for signal16 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc6); // ccc7[7:0] : reg setting for signal17 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc7); // ccc8[7:0] : reg setting for signal18 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc8); // ccc9[7:0] : reg setting for signal19 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCc9); // ccca[7:0] : reg setting for signal20 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCca); // cccb[7:0] : reg setting for signal21 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCcb); // cccc[7:0] : reg setting for signal22 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCcc); // cccd[7:0] : reg setting for signal23 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCcd); // ccce[7:0] : reg setting for signal24 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCce); // cccf[7:0] : reg setting for signal25 selection with d2u mode
	LCD_WR_DATA(0x0E);

	// ccdx
	LCD_WR_REG(0xCCd0); // ccd1[7:0] : reg setting for signal26 selection with d2u mode
	LCD_WR_DATA(0x10);
	LCD_WR_REG(0xCCd1); // ccd2[7:0] : reg setting for signal27 selection with d2u mode
	LCD_WR_DATA(0x0A);
	LCD_WR_REG(0xCCd2); // ccd3[7:0] : reg setting for signal28 selection with d2u mode
	LCD_WR_DATA(0x0C);
	LCD_WR_REG(0xCCd3); // ccd4[7:0] : reg setting for signal29 selection with d2u mode
	LCD_WR_DATA(0x04);
	LCD_WR_REG(0xCCd4); // ccd5[7:0] : reg setting for signal30 selection with d2u mode
	LCD_WR_DATA(0x03);
	LCD_WR_REG(0xCCd5); // ccd6[7:0] : reg setting for signal31 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCd6); // ccd7[7:0] : reg setting for signal32 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCd7); // ccd8[7:0] : reg setting for signal33 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCd8); // ccd9[7:0] : reg setting for signal34 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCd9); // ccda[7:0] : reg setting for signal35 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCda); // ccdb[7:0] : reg setting for signal36 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCdb); // ccdc[7:0] : reg setting for signal37 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCdc); // ccdd[7:0] : reg setting for signal38 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCdd); // ccde[7:0] : reg setting for signal39 selection with d2u mode
	LCD_WR_DATA(0x00);
	LCD_WR_REG(0xCCde); // ccdf[7:0] : reg setting for signal40 selection with d2u mode
	LCD_WR_DATA(0x00);

	LCD_WR_REG(0x3A00);
	LCD_WR_DATA(0x55);

	LCD_WR_REG(0x1100);

	HAL_Delay(120);
	LCD_WR_REG(0x2900);

	HAL_Delay(50);

	LCD_Display_Dir(1); // 横屏
	HAL_Delay(1000);
}
// 清屏函数
// color:要清屏的填充色
void LCD_Clear(uint16_t color)
{
	uint32_t index = 0;
	uint32_t totalpoint = lcddev.width;
	totalpoint *= lcddev.height; // 得到总点数
	LCD_SetCursor(0, 0,lcddev.width,lcddev.height);				 // 设置光标位置
	LCD_WriteRAM_Prepare();			 // 开始写入GRAM
	for (index = 0; index < totalpoint; index++)
	{
		LCD->LCD_RAM = color;
	}
}
// 在指定区域内填充单个颜色
//(sx,sy),(ex,ey):填充矩形对角坐标,区域大小为:(ex-sx+1)*(ey-sy+1)
// color:要填充的颜色
void LCD_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
	uint16_t i, j;
	uint16_t xlen = 0;
	xlen = ex - sx + 1;
	for (i = sy; i <= ey; i++)
	{
		LCD_SetCursor(sx, i,ex,ey);		// 设置光标位置
		LCD_WriteRAM_Prepare(); // 开始写入GRAM
		for (j = 0; j < xlen; j++)
			LCD->LCD_RAM = color; // 显示颜色
	}
}
// 在指定区域内填充指定颜色块
//(sx,sy),(ex,ey):填充矩形对角坐标,区域大小为:(ex-sx+1)*(ey-sy+1)
// color:要填充的颜色
void LCD_Color_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t *color)
{
	uint16_t height, width;
	uint16_t i, j;
	width = ex - sx + 1;	// 得到填充的宽度
	height = ey - sy + 1; // 高度
	for (i = 0; i < height; i++)
	{
		LCD_SetCursor(sx, sy + i,ex, ey); // 设置光标位置
		LCD_WriteRAM_Prepare();		 // 开始写入GRAM
		for (j = 0; j < width; j++)
			LCD->LCD_RAM = color[i * width + j]; // 写入数据
	}
}
// 画线
// x1,y1:起点坐标
// x2,y2:终点坐标
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
	uint16_t t;
	int xerr = 0, yerr = 0, delta_x, delta_y, distance;
	int incx, incy, uRow, uCol;
	delta_x = x2 - x1; // 计算坐标增量
	delta_y = y2 - y1;
	uRow = x1;
	uCol = y1;
	if (delta_x > 0)
		incx = 1; // 设置单步方向
	else if (delta_x == 0)
		incx = 0; // 垂直线
	else
	{
		incx = -1;
		delta_x = -delta_x;
	}
	if (delta_y > 0)
		incy = 1;
	else if (delta_y == 0)
		incy = 0; // 水平线
	else
	{
		incy = -1;
		delta_y = -delta_y;
	}
	if (delta_x > delta_y)
		distance = delta_x; // 选取基本增量坐标轴
	else
		distance = delta_y;
	for (t = 0; t <= distance + 1; t++) // 画线输出
	{
		LCD_DrawPoint(uRow, uCol); // 画点
		xerr += delta_x;
		yerr += delta_y;
		if (xerr > distance)
		{
			xerr -= distance;
			uRow += incx;
		}
		if (yerr > distance)
		{
			yerr -= distance;
			uCol += incy;
		}
	}
}
// 画矩形
//(x1,y1),(x2,y2):矩形的对角坐标
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
	LCD_DrawLine(x1, y1, x2, y1);
	LCD_DrawLine(x1, y1, x1, y2);
	LCD_DrawLine(x1, y2, x2, y2);
	LCD_DrawLine(x2, y1, x2, y2);
}
// 在指定位置画一个指定大小的圆
//(x,y):中心点
// r    :半径
void LCD_Draw_Circle(uint16_t x0, uint16_t y0, uint8_t r)
{
	int a, b;
	int di;
	a = 0;
	b = r;
	di = 3 - (r << 1); // 判断下个点位置的标志
	while (a <= b)
	{
		LCD_DrawPoint(x0 + a, y0 - b); // 5
		LCD_DrawPoint(x0 + b, y0 - a); // 0
		LCD_DrawPoint(x0 + b, y0 + a); // 4
		LCD_DrawPoint(x0 + a, y0 + b); // 6
		LCD_DrawPoint(x0 - a, y0 + b); // 1
		LCD_DrawPoint(x0 - b, y0 + a);
		LCD_DrawPoint(x0 - a, y0 - b); // 2
		LCD_DrawPoint(x0 - b, y0 - a); // 7
		a++;
		// 使用Bresenham算法画圆
		if (di < 0)
			di += 4 * a + 6;
		else
		{
			di += 10 + 4 * (a - b);
			b--;
		}
	}
}
// 在指定位置显示一个字符
// x,y:起始坐标
// num:要显示的字符:" "--->"~"
// size:字体大小 12/16/24
// mode:叠加方式(1)还是非叠加方式(0)
void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint8_t size, uint8_t mode)
{
	uint8_t temp, t1, t;
	uint16_t y0 = y;
	uint8_t csize = (size / 8 + ((size % 8) ? 1 : 0)) * (size / 2); // 得到字体一个字符对应点阵集所占的字节数
	num = num - ' ';																								// 得到偏移后的值（ASCII字库是从空格开始取模，所以-' '就是对应字符的字库）
	for (t = 0; t < csize; t++)
	{
		if (size == 12)
			temp = asc2_1206[num][t]; // 调用1206字体
		else if (size == 16)
			temp = asc2_1608[num][t]; // 调用1608字体
		else if (size == 24)
			temp = asc2_2412[num][t]; // 调用2412字体
		else
			return; // 没有的字库
		for (t1 = 0; t1 < 8; t1++)
		{
			if (temp & 0x80)
				LCD_Fast_DrawPoint(x, y, POINT_COLOR);
			else if (mode == 0)
				LCD_Fast_DrawPoint(x, y, BACK_COLOR);
			temp <<= 1;
			y++;
			if (y >= lcddev.height)
				return; // 超区域了
			if ((y - y0) == size)
			{
				y = y0;
				x++;
				if (x >= lcddev.width)
					return; // 超区域了
				break;
			}
		}
	}
}
// m^n函数
// 返回值:m^n次方.
uint32_t LCD_Pow(uint8_t m, uint8_t n)
{
	uint32_t result = 1;
	while (n--)
		result *= m;
	return result;
}
// 显示数字,高位为0,则不显示
// x,y :起点坐标
// len :数字的位数
// size:字体大小
// color:颜色
// num:数值(0~4294967295);
void LCD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size)
{
	uint8_t t, temp;
	uint8_t enshow = 0;
	for (t = 0; t < len; t++)
	{
		temp = (num / LCD_Pow(10, len - t - 1)) % 10;
		if (enshow == 0 && t < (len - 1))
		{
			if (temp == 0)
			{
				LCD_ShowChar(x + (size / 2) * t, y, ' ', size, 0);
				continue;
			}
			else
				enshow = 1;
		}
		LCD_ShowChar(x + (size / 2) * t, y, temp + '0', size, 0);
	}
}
// 显示数字,高位为0,还是显示
// x,y:起点坐标
// num:数值(0~999999999);
// len:长度(即要显示的位数)
// size:字体大小
// mode:
//[7]:0,不填充;1,填充0.
//[6:1]:保留
//[0]:0,非叠加显示;1,叠加显示.
void LCD_ShowxNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint8_t mode)
{
	uint8_t t, temp;
	uint8_t enshow = 0;
	for (t = 0; t < len; t++)
	{
		temp = (num / LCD_Pow(10, len - t - 1)) % 10;
		if (enshow == 0 && t < (len - 1))
		{
			if (temp == 0)
			{
				if (mode & 0X80)
					LCD_ShowChar(x + (size / 2) * t, y, '0', size, mode & 0X01);
				else
					LCD_ShowChar(x + (size / 2) * t, y, ' ', size, mode & 0X01);
				continue;
			}
			else
				enshow = 1;
		}
		LCD_ShowChar(x + (size / 2) * t, y, temp + '0', size, mode & 0X01);
	}
}
// 显示字符串
// x,y:起点坐标
// width,height:区域大小
// size:字体大小
//*p:字符串起始地址
void LCD_ShowString(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, uint8_t *p)
{
	uint8_t x0 = x;
	width += x;
	height += y;
	while ((*p <= '~') && (*p >= ' ')) // 判断是不是非法字符!
	{
		if (x >= width)
		{
			x = x0;
			y += size;
		}
		if (y >= height)
			break; // 退出
		LCD_ShowChar(x, y, *p, size, 0);
		x += size / 2;
		p++;
	}
}
void LCD_DrawPoint_pic(uint16_t x, uint16_t y, uint16_t color)
{
	LCD_SetCursor(x, y,x,y);		// 设置光标位置
	LCD_WriteRAM_Prepare(); // 开始写入GRAM
	LCD->LCD_RAM = color;
}

/****************************************************************************
* 名    称：void LCD_DrawPicture(uint16_t StartX,uint16_t StartY,uint16_t EndX,uint16_t EndY,uint16_t *pic)
* 功    能：在指定座标范围显示一副图片
* 入口参数： StartX     行起始座标
*           StartY     列起始座标
*           EndX       行结束座标
*           EndY       列结束座标
						pic        图片头指针
* 出口参数：无
* 说    明：图片取模格式为水平扫描，16位颜色模式
* 调用方法：LCD_DrawPicture(0,0,100,100,(uint16_t*)demo);
****************************************************************************/
void LCD_DrawPicture(uint16_t StartX, uint16_t StartY, uint16_t Xend, uint16_t Yend, uint8_t *pic)
{
	static uint16_t i = 0, j = 0;
	uint16_t *bitmap = (uint16_t *)pic;
	for (j = 0; j < Yend - StartY; j++)
	{
		for (i = 0; i < Xend - StartX; i++)
			LCD_DrawPoint_pic(StartX + i, StartY + j, *bitmap++);
	}
}
