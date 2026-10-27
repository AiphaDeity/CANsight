// GT911 板级 I2C 访问：供启动和 LVGL 输入适配使用，均在主循环上下文同步执行。
#include "gt911.h"
#include "i2c.h"

// 正常触摸事务约毫秒内完成，故障时不允许单次 HAL 等待长达数秒。
__IO uint32_t  I2CTimeout = 20U;

// 内部调用：只修改 GPIOx 指定引脚的 MODER 模式位，不重新配置上拉、速度或输出类型。
// GPIO_Pin 是引脚编号（如 2），不是 HAL 的 GPIO_PIN_2 位掩码，当前仅切换输入/输出。
static void Set_GPIO_MODER(GPIO_TypeDef* GPIOx, uint32_t moder, uint16_t GPIO_Pin)
{
  assert_param(IS_GPIO_ALL_INSTANCE(GPIOx));
  assert_param(IS_GPIO_MODE(moder));

  uint32_t reg = 0;

  reg = GPIOx->MODER;
  // 每个引脚占两位，先清除原模式再写入；不会改变其他引脚的模式。
  reg &= ~((GPIO_MODER_MODE0_Msk) << (GPIO_Pin * 2U));
  reg |= ((moder) << (GPIO_Pin * 2U));
  GPIOx->MODER = reg;
}

// main 启动时调用：PC7 复位触摸控制器，PI2 在复位期间输出地址选择电平，之后改回输入。
// slave_addr 用于选择复位电平；后续 I2C 访问仍使用头文件的 GT911_I2C_SLAVE，二者须一致。
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

	  // 复位地址选择结束后释放 PI2；当前适配轮询触摸数据，不用此脚触发 LVGL 读回调。
	  Set_GPIO_MODER(GPIOI, GPIO_MODE_INPUT, 2);
}

// 主循环调用：从 data 向 GT911 的 16 位寄存器地址 cmd 写 len 字节，同步等待 HAL 返回。
// 使用 hi2c1 和固定七位设备地址的 HAL 编码，返回 HAL 状态值，调用方检查是否为 HAL_OK。
int gt911_send_data(uint16_t cmd, uint8_t* data, int len)
{
	return HAL_I2C_Mem_Write(&hi2c1, ((GT911_I2C_SLAVE<<1)|I2C_WRITE_BIT), cmd, I2C_MEMADD_SIZE_16BIT, data, len, I2CTimeout);
}

// 主循环调用：从 GT911 的 16 位寄存器地址 cmd 读取 len 字节到 data，调用方提供容量。
// 同步 I2C 事务，返回 HAL 状态值；读取成功后是否清状态由上层适配决定。
int gt911_read_data(uint16_t cmd, uint8_t* data, int len)
{
	return HAL_I2C_Mem_Read(&hi2c1, ((GT911_I2C_SLAVE<<1)|I2C_READ_BIT), cmd, I2C_MEMADD_SIZE_16BIT, data, len, I2CTimeout);
}
