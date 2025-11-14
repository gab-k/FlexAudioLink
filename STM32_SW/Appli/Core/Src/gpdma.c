/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    gpdma.c
 * @brief   This file provides code for the configuration
 *          of the GPDMA instances.
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
#include "gpdma.h"

/* USER CODE BEGIN 0 */
extern I2S_HandleTypeDef hi2s2;
/* USER CODE END 0 */

DMA_HandleTypeDef handle_GPDMA1_Channel1;
DMA_HandleTypeDef handle_GPDMA1_Channel0;

/* GPDMA1 init function */
void MX_GPDMA1_Init(void)
{

  /* USER CODE BEGIN GPDMA1_Init 0 */

  /* USER CODE END GPDMA1_Init 0 */

  /* Peripheral clock enable */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* GPDMA1 interrupt Init */
    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);

  /* USER CODE BEGIN GPDMA1_Init 1 */

  /* USER CODE END GPDMA1_Init 1 */
  handle_GPDMA1_Channel1.Instance = GPDMA1_Channel1;
  handle_GPDMA1_Channel1.InitLinkedList.Priority = DMA_LOW_PRIORITY_LOW_WEIGHT;
  handle_GPDMA1_Channel1.InitLinkedList.LinkStepMode = DMA_LSM_FULL_EXECUTION;
  handle_GPDMA1_Channel1.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
  handle_GPDMA1_Channel1.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  handle_GPDMA1_Channel1.InitLinkedList.LinkedListMode = DMA_LINKEDLIST_CIRCULAR;
  if (HAL_DMAEx_List_Init(&handle_GPDMA1_Channel1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel1, DMA_CHANNEL_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  handle_GPDMA1_Channel0.Instance = GPDMA1_Channel0;
  handle_GPDMA1_Channel0.InitLinkedList.Priority = DMA_LOW_PRIORITY_LOW_WEIGHT;
  handle_GPDMA1_Channel0.InitLinkedList.LinkStepMode = DMA_LSM_FULL_EXECUTION;
  handle_GPDMA1_Channel0.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
  handle_GPDMA1_Channel0.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  handle_GPDMA1_Channel0.InitLinkedList.LinkedListMode = DMA_LINKEDLIST_CIRCULAR;
  if (HAL_DMAEx_List_Init(&handle_GPDMA1_Channel0) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel0, DMA_CHANNEL_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN GPDMA1_Init 2 */
  /* USER CODE END GPDMA1_Init 2 */

}

/* USER CODE BEGIN 1 */
void Init_I2S2_RX_DMA_Queue(DMA_HandleTypeDef *GPDMA_channel_handle, DMA_QListTypeDef *queue)
{
  if (MX_I2S2_RX_Queue_Config() != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_ConvertQToDynamic(queue) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_Init(GPDMA_channel_handle) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_LinkQ(GPDMA_channel_handle, queue) != HAL_OK)
  {
    Error_Handler();
  }
  
  // Link DMA handle to the I2S handle
  __HAL_LINKDMA(&hi2s2, hdmarx, *GPDMA_channel_handle);
  
  // RX Callbacks
  if (HAL_DMA_RegisterCallback(GPDMA_channel_handle, HAL_DMA_XFER_HALFCPLT_CB_ID, I2S_DMA_RX_HalfCpltCallback) != HAL_OK)
  {
      Error_Handler();
  }
  if (HAL_DMA_RegisterCallback(GPDMA_channel_handle, HAL_DMA_XFER_CPLT_CB_ID, I2S_DMA_RX_CpltCallback) != HAL_OK)
  {
      Error_Handler();
  }
  if (HAL_DMA_RegisterCallback(GPDMA_channel_handle, HAL_DMA_XFER_ERROR_CB_ID, I2S_DMA_RX_ErrorCallback) != HAL_OK)
  {
      Error_Handler();
  }
}

void Init_I2S2_TX_DMA_Queue(DMA_HandleTypeDef *GPDMA_channel_handle, DMA_QListTypeDef *queue)
{
  if (MX_I2S2_TX_Queue_Config() != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_ConvertQToDynamic(queue) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_Init(GPDMA_channel_handle) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_LinkQ(GPDMA_channel_handle, queue) != HAL_OK)
  {
    Error_Handler();
  }

  // Link TX DMA handle to the I2S handle
  __HAL_LINKDMA(&hi2s2, hdmatx, *GPDMA_channel_handle);

    // TX Callbacks
  if (HAL_DMA_RegisterCallback(GPDMA_channel_handle, HAL_DMA_XFER_HALFCPLT_CB_ID, I2S_DMA_TX_HalfCpltCallback) != HAL_OK)
  {
      Error_Handler();
  }
  if (HAL_DMA_RegisterCallback(GPDMA_channel_handle, HAL_DMA_XFER_CPLT_CB_ID, I2S_DMA_TX_CpltCallback) != HAL_OK)
  {
      Error_Handler();
  }
  if (HAL_DMA_RegisterCallback(GPDMA_channel_handle, HAL_DMA_XFER_ERROR_CB_ID, I2S_DMA_TX_ErrorCallback) != HAL_OK)
  {
      Error_Handler();
  }
}

/* USER CODE END 1 */
