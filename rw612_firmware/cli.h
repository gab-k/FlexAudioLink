/**
 * @file cli.h
 * @brief Header file for the Command Line Interface (CLI) module.
 * @details This file provides definitions, external variables, and function prototypes
 *          for the CLI module, which handles command reception, parsing, and execution
 *          via a dedicated USB CDC interface.
 *
 * @note The CLI operates on the CDC interface.
 * @note Relies on TinyUSB's wanted_char feature for line-based input handling.
 */
#ifndef INC_CLI_H_
#define INC_CLI_H_

#include <stdint.h>
#include <stdbool.h>
#include "tusb.h" // For TinyUSB functions
#include <stdio.h> // For printf, vsnprintf

//=====================================================================================================================
// Defines
//=====================================================================================================================
/** @brief Maximum length of a command line string receivable via CLI. */
#define CLI_MAX_CMD_LEN 64

//=====================================================================================================================
// Public Function Prototypes
//=====================================================================================================================

/**
 * @brief Initializes the Command Line Interface (CLI) module.
 * @details Configures the CLI's USB CDC interface to trigger the tud_cdc_rx_wanted_cb() callback upon receiving
 *          specific line-ending characters (\n and \r).
 * @param None
 * @return None
 * @note This function must be called after the TinyUSB stack is initialized
 *       (`tusb_init()`) and the CDC interfaces are configured.
 */
void cli_init(void);

/**
 * @brief Main task loop for the CLI.
 * @details Waits for commands from the USB callback and executes them.
 * @param pvParameters FreeRTOS task parameters (unused).
 */
void cli_task(void *pvParameters);

#endif /* INC_CLI_H_ */