/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : linked_list.c
  * Description        : This file provides code for the configuration
  *                      of the LinkedList.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "linked_list.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

DMA_NodeTypeDef I2S_Node __attribute__((section("noncacheable_buffer")));
DMA_QListTypeDef I2S2_Queue;

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
extern int32_t spk_buf[];
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/**
  * @brief  DMA Linked-list I2S2_Queue configuration
  * @param  None
  * @retval None
  */
HAL_StatusTypeDef MX_I2S2_Queue_Config(void)
{
  HAL_StatusTypeDef ret = HAL_OK;
  /* DMA node configuration declaration */
  DMA_NodeConfTypeDef pNodeConfig;

  /* Set node configuration ################################################*/
  pNodeConfig.NodeType = DMA_GPDMA_LINEAR_NODE;
  pNodeConfig.Init.Request = GPDMA1_REQUEST_SPI2_TX;
  pNodeConfig.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
  pNodeConfig.Init.Direction = DMA_MEMORY_TO_PERIPH;
  pNodeConfig.Init.SrcInc = DMA_SINC_INCREMENTED;
  pNodeConfig.Init.DestInc = DMA_DINC_FIXED;
  pNodeConfig.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_HALFWORD;
  pNodeConfig.Init.DestDataWidth = DMA_DEST_DATAWIDTH_HALFWORD;
  pNodeConfig.Init.SrcBurstLength = 2;
  pNodeConfig.Init.DestBurstLength = 2;
  pNodeConfig.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0|DMA_DEST_ALLOCATED_PORT0;
  pNodeConfig.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
  pNodeConfig.Init.Mode = DMA_NORMAL;
  pNodeConfig.TriggerConfig.TriggerPolarity = DMA_TRIG_POLARITY_MASKED;
  pNodeConfig.DataHandlingConfig.DataExchange = DMA_EXCHANGE_NONE;
  pNodeConfig.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
  pNodeConfig.SrcAddress = (uint32_t) spk_buf;
  pNodeConfig.DstAddress = (uint32_t) &(SPI2->TXDR);
  pNodeConfig.DataSize = 0;

  /* Build I2S_Node Node */
  ret |= HAL_DMAEx_List_BuildNode(&pNodeConfig, &I2S_Node);

  /* Insert I2S_Node to Queue */
  ret |= HAL_DMAEx_List_InsertNode_Tail(&I2S2_Queue, &I2S_Node);

  ret |= HAL_DMAEx_List_SetCircularMode(&I2S2_Queue);

   return ret;
}

