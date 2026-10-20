#include "delay.h"

static uint8_t  fac_us=0;							//us延时倍乘数
static uint16_t fac_ms=0;							//ms延时倍乘数,在os下,代表每个节拍的ms数

//初始化延迟函数
//当使用OS的时候,此函数会初始化OS的时钟节拍
//SYSTICK的时钟固定为AHB时钟的1/8
//SYSCLK:系统时钟频率
void delay_init(uint8_t SYSCLK)
{
#if SYSTEM_SUPPORT_OS 						//如果需要支持OS.
	uint32_t reload;
#endif
    HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);//SysTick频率为HCLK
	fac_us=SYSCLK;						//不论是否使用OS,fac_us都需要使用
#if SYSTEM_SUPPORT_OS 						//如果需要支持OS.
	reload=SYSCLK;					    //每秒钟的计数次数 单位为K
	reload*=1000000/delay_ostickspersec;	//根据delay_ostickspersec设定溢出时间
											//reload为24位寄存器,最大值:16777216,在180M下,约合0.745s左右
	fac_ms=1000/delay_ostickspersec;		//代表OS可以延时的最少单位
	SysTick->CTRL|=SysTick_CTRL_TICKINT_Msk;//开启SYSTICK中断
	SysTick->LOAD=reload; 					//每1/OS_TICKS_PER_SEC秒中断一次
	SysTick->CTRL|=SysTick_CTRL_ENABLE_Msk; //开启SYSTICK
#else
	//fac_ms=(uint16_t)fac_us*1000;				//非OS下,代表每个ms需要的systick时钟数
#endif
}

//延时nus
//nus为要延时的us数.
//注意:nus的值,不要大于798915us(最大值即2^24/fac_us@fac_us=21)
void delay_us(uint32_t nus)
{
	uint32_t ticks;
	uint32_t told,tnow,tcnt=0;
	uint32_t reload=SysTick->LOAD;				//LOAD的值
	ticks=nus*fac_us; 						//需要的节拍数
	told=SysTick->VAL;        				//刚进入时的计数器值
	while(1)
	{
		tnow=SysTick->VAL;
		if(tnow!=told)
		{
			if(tnow<told)tcnt+=told-tnow;	//这里注意一下SYSTICK是一个递减的计数器就可以了.
			else tcnt+=reload-tnow+told;
			told=tnow;
			if(tcnt>=ticks)break;			//时间超过/等于要延迟的时间,则退出.
		}
	};
}
//延时nms
//注意nms的范围
//SysTick->LOAD为24位寄存器,所以,最大延时为:
//nms<=0xffffff*8*1000/SYSCLK
//SYSCLK单位为Hz,nms单位为ms
//对168M条件下,nms<=798ms
void delay_xms(uint16_t nms)
{
	uint32_t temp;
	SysTick->LOAD=(uint32_t)nms*fac_ms;			//时间加载(SysTick->LOAD为24bit)
	SysTick->VAL =0x00;           			//清空计数器
	SysTick->CTRL|=SysTick_CTRL_ENABLE_Msk ;          //开始倒数
	do
	{
		temp=SysTick->CTRL;
	}while((temp&0x01)&&!(temp&(1<<16)));	//等待时间到达
	SysTick->CTRL&=~SysTick_CTRL_ENABLE_Msk;       //关闭计数器
	SysTick->VAL =0X00;     		  		//清空计数器
}
//延时nms
//nms:0~65535
void delay_ms(uint16_t nms)
{
	uint32_t i;
	for(i=0;i<nms;i++) delay_us(1000);
}




































