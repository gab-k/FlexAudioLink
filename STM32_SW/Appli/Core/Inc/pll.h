// pll.h
#ifndef INC_PLL_H_
#define INC_PLL_H_

#include "main.h"

#define PLL_LOCK_TIMEOUT_MS 100

typedef enum
{
  PLL_SRC_HSI = 0,  // 00: HSI selected as PLL clock (hsi_ck) (default after reset)
  PLL_SRC_CSI = 1,  // 01: CSI selected as PLL clock (csi_ck)
  PLL_SRC_HSE = 2,  // 10: HSE selected as PLL clock (hse_ck)
  PLL_SRC_NONE = 3, // 11: no clock send to DIVMx divider and PLLs
} PLLSource_TypeDef;

HAL_StatusTypeDef Reconfigure_PLL3_MNR(uint32_t new_PLLM3, uint32_t new_PLLN3, uint32_t new_PLLR3);
PLLSource_TypeDef get_PLL_source(void);

#endif