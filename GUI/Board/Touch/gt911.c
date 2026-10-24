#include "gt911.h"
#include "i2c.h"

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
