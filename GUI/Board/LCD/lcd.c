#include "lcd.h"
#include <stddef.h>

// 当前 OTM8009A 屏幕参数，供窗口设置及初始化使用。
_lcd_dev lcddev;

void LCD_WR_REG(uint16_t regval)
{
	LCD->LCD_REG = regval; // 写入要写的寄存器序号
}

void LCD_WR_DATA(uint16_t data)
{
	LCD->LCD_RAM = data;
}

void LCD_WriteReg(uint16_t LCD_Reg, uint16_t LCD_RegValue)
{
	LCD->LCD_REG = LCD_Reg;			 // 写入要写的寄存器序号
	LCD->LCD_RAM = LCD_RegValue; // 写入数据
}

void LCD_WriteRAM_Prepare(void)
{
	LCD->LCD_REG = lcddev.wramcmd;
}

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

// Debug 下保持原有单函数优化和 volatile 像素写入。
#if defined(CANSIGHT_LCD_FILL_O2) && CANSIGHT_LCD_FILL_O2 && defined(__GNUC__)
__attribute__((optimize("O2")))
#endif
void LCD_Color_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t *color)
{
	if(color == NULL || ex < sx || ey < sy) return;
	uint32_t count = (uint32_t)(ex - sx + 1U) * (uint32_t)(ey - sy + 1U);
	// 窗口内按现有扫描方向自动递增，一块像素数据仅设置一次窗口。
	LCD_SetCursor(sx, sy, ex, ey);
	LCD_WriteRAM_Prepare();
	volatile uint16_t *ram = &LCD->LCD_RAM;
	while(count-- != 0U) {
		*ram = *color++;
	}
}
