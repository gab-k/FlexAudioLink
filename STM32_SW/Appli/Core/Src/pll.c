#include "pll.h"

/**
 * @brief Reconfigures PLL3 M, N, and R dividers during runtime.
 * @note  This changes the PLL3 VCO frequency. The caller should ensure the resulting
 *        any peripheral driven by PLL3 is disabled beforehand.
 * @note  PLLM3 and PLLN3 dividers are shared with the other PLL channels PLL3_P, PLL3_Q, PLL3_R, PLL3_S
 *
 * @param new_PLLM3: New desired divider M; Valid range from 1 to 63.
 * @param new_PLLN3: New desired multiplier N; Valid range from 12 to 420.
 * @param new_PLLR3: New desired divider R; Valid range from 1 to 128.
 * @retval HAL_StatusTypeDef HAL_OK on success, HAL_TIMEOUT or HAL_ERROR on failure.
 */
HAL_StatusTypeDef Reconfigure_PLL3_MNR(uint32_t new_PLLM3, uint32_t new_PLLN3, uint32_t new_PLLR3)
{
  uint32_t tickstart;
  uint32_t f_pll_src, f_DIVM3_out, f_DIVN3_out;
  uint32_t reg_val_PLLM3, reg_val_PLLN3, reg_val_PLLR3;
  uint32_t pllckselr_reg, pll3divr1_reg, pllcfgr_reg;
  PLLSource_TypeDef pll_src;

  // --- Input Validation ---
  // Check if dividers are in valid range
  if (new_PLLM3 < 1 || new_PLLM3 > 63 || new_PLLN3 < 12 || new_PLLN3 > 420 || new_PLLR3 < 1 || new_PLLR3 > 128)
  {
    return HAL_ERROR; // Invalid divider/multiplier value
  }

  // --- Get current PLL source ---
  f_pll_src = 0;
  pll_src = get_PLL_source();
  switch (pll_src)
  {
    case PLL_SRC_HSI:
      f_pll_src = HSI_VALUE;
      break;
    case PLL_SRC_CSI:
      f_pll_src = CSI_VALUE;
      break;
    case PLL_SRC_HSE:
      f_pll_src = HSE_VALUE;
      break;
    case PLL_SRC_NONE:
      return HAL_ERROR;
  }

  // --- Check constraints ---
  // f_DIVM3_out is equivalent to f_ref3_ck in reference manual
  // 1MHz =< f_DIVM3_out =< 16MHz
  // 150MHz =< f_DIVN3_out =< 400MHz when f_DIVM3_out between 1 Mhz and 2 Mhz (VCO low range)
  // 400MHz =< f_DIVN3_out =< 1600MHz when f_DIVM3_out between 2 Mhz and 16 Mhz (VCO high range)
  //
  // 1. Check 1MHz =< f_DIVM3_out =< 16MHz
  f_DIVM3_out = f_pll_src / new_PLLM3;
  if (f_DIVM3_out < 1000000UL || f_DIVM3_out > 16000000UL)
  {
    return HAL_ERROR;
  }
  // 2. Check for valid f_DIVN3_out depending on f_DIVM3_out
  f_DIVN3_out = f_DIVM3_out * new_PLLN3;
  if (f_DIVM3_out <= 2000000UL)
  {
    // VCO low range -> 150MHz =< f_DIVN3_out =< 400MHz
    if (f_DIVN3_out < 150000000UL || f_DIVN3_out > 400000000UL)
    {
      return HAL_ERROR;
    }
  } else
  {
    // VCO high range -> 400MHz =< f_DIVN3_out =< 1600MHz
    if (f_DIVN3_out < 400000000UL || f_DIVN3_out > 1600000000UL)
    {
      return HAL_ERROR;
    }
  }

  // Convert desired divider to register value (Divider - 1)
  reg_val_PLLM3 = new_PLLM3;
  reg_val_PLLN3 = new_PLLN3 - 1;
  reg_val_PLLR3 = new_PLLR3 - 1;

  // --- Disable PLL3 ---
  // Critical section to prevent interruption during clock changes
  __disable_irq();

  // Clear PLL3PEN, PLL3QEN, PLL3REN, PLL3SEN
  CLEAR_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3PEN_Msk);
  CLEAR_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3QEN_Msk);
  CLEAR_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3REN_Msk);
  CLEAR_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3SEN_Msk);

  // Clear PLL3ON
  CLEAR_BIT(RCC->CR, RCC_CR_PLL3ON);

  // --- Wait for PLL3 to be disabled (PLL3RDY = 0) ---
  tickstart = HAL_GetTick();
  while (READ_BIT(RCC->CR, RCC_CR_PLL3RDY) != 0)
  {
    if ((HAL_GetTick() - tickstart) > PLL_LOCK_TIMEOUT_MS)
    {
      __enable_irq();
      return HAL_TIMEOUT; // Timeout waiting for PLL3 off
    }
  }

  // --- Reconfigure PLL3 M, N, R ---

  // 1. Configure DIVM3/PLL3M in RCC_PLLCKSELR
  // Read PLLCKSELR to preserve other settings (PLL Source, and other PLL DIVM values)
  pllckselr_reg = RCC->PLLCKSELR;
  // Clear current DIVM3/PLL3M bits
  pllckselr_reg &= ~(RCC_PLLCKSELR_DIVM3_Msk);
  // Set new DIVM3/PLL3M value
  pllckselr_reg |= (reg_val_PLLM3 << RCC_PLLCKSELR_DIVM3_Pos);
  // Write back to PLLCKSELR register
  RCC->PLLCKSELR = pllckselr_reg;

  // 2. Configure RCC_PLLCFGR (PLL3RGE, PLL3VCOSEL)
  pllcfgr_reg = RCC->PLLCFGR;
  // Clear PLL3RGE and PLL3VCOSEL bits
  pllcfgr_reg &= ~(RCC_PLLCFGR_PLL3RGE_Msk);
  pllcfgr_reg &= ~(RCC_PLLCFGR_PLL3VCOSEL_Msk);
  // Check for ranges and set PLL3RGE and PLL3VCOSEL bits accordingly:
  // 00: f_ref3_ck/f_DIVM3_out from 1 to 2 MHz
  // 01: f_ref3_ck/f_DIVM3_out from 2 to 4 MHz
  // 10: f_ref3_ck/f_DIVM3_out from 4 to 8 MHz
  // 11: f_ref3_ck/f_DIVM3_out from 8 to 16 MHz
  if (f_DIVM3_out < 2000000UL)
  {
    pllcfgr_reg |= (0 << RCC_PLLCFGR_PLL3RGE_Pos);
    pllcfgr_reg |= (1 << RCC_PLLCFGR_PLL3VCOSEL_Pos);
  } else if (f_DIVM3_out < 4000000UL)
  {
    pllcfgr_reg |= (1 << RCC_PLLCFGR_PLL3RGE_Pos);
    pllcfgr_reg |= (0 << RCC_PLLCFGR_PLL3VCOSEL_Pos);
  } else if (f_DIVM3_out < 8000000UL)
  {
    pllcfgr_reg |= (2 << RCC_PLLCFGR_PLL3RGE_Pos);
    pllcfgr_reg |= (0 << RCC_PLLCFGR_PLL3VCOSEL_Pos);
  } else if (f_DIVM3_out < 16000000UL)
  {
    pllcfgr_reg |= (3 << RCC_PLLCFGR_PLL3RGE_Pos);
    pllcfgr_reg |= (0 << RCC_PLLCFGR_PLL3VCOSEL_Pos);
  } else
  {
    __enable_irq();
    return HAL_ERROR;
  }

  RCC->PLLCFGR = pllcfgr_reg;

  // 3. Configure PLL3N and PLL3R in RCC_PLL3DIVR1
  // Read PLL3DIVR1 to preserve P and Q values
  pll3divr1_reg = RCC->PLL3DIVR1;
  // Clear DIVN3 and DIVR3 values
  pll3divr1_reg &= ~(RCC_PLL3DIVR1_DIVN_Msk);
  pll3divr1_reg &= ~(RCC_PLL3DIVR1_DIVR_Msk);
  // Set new DIVN3 and DIVR3 values
  pll3divr1_reg |= (reg_val_PLLN3 << RCC_PLL3DIVR1_DIVN_Pos); // 0x27
  pll3divr1_reg |= (reg_val_PLLR3 << RCC_PLL3DIVR1_DIVR_Pos); // 0x1b
  // Write back to PLL3DIVR1 register
  RCC->PLL3DIVR1 = pll3divr1_reg;

  // --- Enable PLL3 ---
  SET_BIT(RCC->CR, RCC_CR_PLL3ON);

  // --- Wait for PLL3 to Lock (PLL3RDY = 1) ---
  tickstart = HAL_GetTick(); // TODO: fix timeout, seems to interfere with disabled irqs
  while (READ_BIT(RCC->CR, RCC_CR_PLL3RDY_Msk) == 0)
  {
    if ((HAL_GetTick() - tickstart) > PLL_LOCK_TIMEOUT_MS)
    {
      // Error: PLL3 failed to lock. Maybe disable it again for safety?
      CLEAR_BIT(RCC->CR, RCC_CR_PLL3ON);
      __enable_irq();
      return HAL_TIMEOUT; // Timeout waiting for PLL3 lock
    }
  }

  // Set PLL3PEN, PLL3QEN, PLL3REN, PLL3SEN
  SET_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3PEN_Msk);
  SET_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3QEN_Msk);
  SET_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3REN_Msk);
  SET_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLL3SEN_Msk);

  // End critical section
  __enable_irq();

  return HAL_OK;
}

PLLSource_TypeDef get_PLL_source(void)
{
  uint32_t pllckselr_reg;
  uint32_t pll3src_bits;

  // Read PLL clock source select register
  pllckselr_reg = RCC->PLLCKSELR;

  // Bitmask for PLLSRC inside PLLCKSELR which are bits [0:2]
  pll3src_bits = (pllckselr_reg & RCC_PLLCKSELR_PLLSRC_Msk) >> RCC_PLLCKSELR_PLLSRC_Pos;

  switch (pll3src_bits)
  {
    case 0:
      return PLL_SRC_NONE;
    case 1:
      return PLL_SRC_HSI;
    case 2:
      return PLL_SRC_HSE;
    case 3:
      return PLL_SRC_CSI;
    // Default case can't really happen because of the bitmask above.
    // The return value is not relevant because Error_Handler() does generally not return.
    default:
      Error_Handler();
      return PLL_SRC_NONE;
  }
}