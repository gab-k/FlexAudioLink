/**
 * @file cli.c
 * @brief Implementation file for the Command Line Interface (CLI) module.
 * @details This file contains the function definitions for handling CLI interactions
 *          via a dedicated USB CDC interface. It includes
 *          initialization, output formatting, command parsing and dispatch, and
 *          implementations of various command handlers. TinyUSB CDC callbacks
 *          for connection state and line reception are also included here to
 *          process incoming commands.
 *
 * @note The CLI relies on TinyUSB's wanted_char feature (tud_cdc_rx_wanted_cb)
 *       for event-driven command line reception.
 * @note Command execution is dispatched from the tud_cdc_rx_wanted_cb, which is executed in a TinyUSB context
 *       (tud_task() which is called periodically inside main loop).
 * @note Requires external definition and initialization of hxspi1 and its corresponding DMA handle.
 */
#include "cli.h"
#include "wpl.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "mode.h"
#include <string.h>   // For strtok, strcmp, strcasecmp, strncpy
#include <stdio.h>    // For vsnprintf
#include <stdarg.h>   // For va_list


//=====================================================================================================================
// Static Variables
//=====================================================================================================================
/**
 * @brief Buffer to store the complete command line received by tud_cdc_rx_wanted_cb.
 */
static char cli_cmd_buf[CLI_MAX_CMD_LEN];

/**
 * @brief Flag indicating if the CLI interface is currently connected (DTR set by host).
 * @details Used for edge-detection to send a welcome message only once upon connection.
 */
static volatile bool cli_connected = false;

/**
 * @brief The welcome message displayed when a terminal connects.
 */
#define CLI_WELCOME_MESSAGE "Wireless Headset CLI\r\nType 'help' for commands\r\n"

/**
 * @brief The message displayed when the host sends commands without a newline character.
 */
#define CLI_NO_NEWLINE_MESSAGE "Command doesn't end with a newline (\\n) character.\r\n"

/**
 *  @brief Flag to control echoing of received characters back to the host terminal. Disabled by default.
 */
static volatile bool cli_echo_enabled = false;

/**
 * @brief Queue to pass commands from USB callback (ISR/Task context) to CLI task.
 */
static QueueHandle_t cli_queue = NULL;

//=====================================================================================================================
// External Variables
//=====================================================================================================================

//=====================================================================================================================
// Static Function Prototypes""
//=====================================================================================================================
static void cli_printf(const char *format, ...);
static void cli_parse_and_execute(char *command_line);
static bool cli_cmd_help(char *args);
static bool cli_cmd_reset(char *args);
static bool cli_cmd_echo(char *args);
static bool cli_cmd_scan(char *args);
static bool cli_cmd_mode(char *args);

//=====================================================================================================================
// Public Function Definitions
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
void cli_init(void)
{
  // Create resources
  cli_queue = xQueueCreate(3, CLI_MAX_CMD_LEN);

  // Set '\n' as wanted character for the CLI interface.
  // This configures TinyUSB to call tud_cdc_rx_wanted_cb when the line feed ('\n') delimiter is received.
  tud_cdc_set_wanted_char('\n');

  // Initialize the command buffer to be empty/null-terminated
  cli_cmd_buf[0] = '\0';
  // Initialize connection state based on the DTR line state at boot.
  cli_connected = tud_cdc_connected();
}

//=====================================================================================================================
// TinyUSB CDC Callbacks (Implemented here for CLI interface handling)
//=====================================================================================================================
/**
 * @brief TinyUSB CDC receive wanted character callback implementation.
 * @details This callback is invoked by the TinyUSB stack when the registered
 *          'wanted' character (newline '\n' or carriage return '\r' for CLI)
 *          is received on the CDC config interface. It reads the complete line,
 *          and triggers the command parser.
 * @param[in] itf The interface index.
 * @param[in] wanted_char The specific character ('\n' or '\r') that triggered the callback.
 * @note This function is executed in a TinyUSB context (tud_task() which is called periodically inside main loop).
 */
void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted_char)
{
  (void)wanted_char; // Suppress unused parameter warning, the specific char isn't needed.

  // Read the entire available line (up to the delimiter) from TinyUSB's internal buffer into cli_cmd_buf.
  uint32_t available_bytes = tud_cdc_n_available(itf);
  // Limit read length to prevent buffer overflow, reserving space for null terminator.
  uint32_t read_len = (available_bytes < CLI_MAX_CMD_LEN - 1) ? available_bytes : CLI_MAX_CMD_LEN - 1;

  uint32_t count = 0;
  if (read_len > 0)
  {
    // tud_cdc_n_read consumes the data from TinyUSB's internal FIFO buffer.
    count = tud_cdc_n_read(itf, cli_cmd_buf, read_len);
  }

  // Null-terminate the received command line string in cli_cmd_buf.
  cli_cmd_buf[count] = '\0';

  // Parse and execute the received command line.
  if (cli_queue != NULL)
  {
    xQueueSend(cli_queue, cli_cmd_buf, 0);
  }
}

/**
 * @brief TinyUSB CDC receive callback implementation.
 * @details This callback is invoked by the TinyUSB stack whenever new data arrives on the CDC interface.
 *          Since the command line interface is configured to only process input upon receiving a
 *          line feed (see tud_cdc_rx_wanted_cb), this callback handles all other data.
 *          Its sole purpose here is to discard any partial or unwanted data by flushing the
 *          receive buffer, ensuring that only complete lines are processed.
 * @param[in] itf The interface index on which data was received.
 * @note This function is executed in a TinyUSB context (tud_task() which is called periodically inside the main loop).
 */
void tud_cdc_rx_cb(uint8_t itf){
  // Just flush anything without linefeed ('/n')
  tud_cdc_n_read_flush(itf);
  // Send an internal command to the CLI task to print the no newline warning message.
  if (cli_queue != NULL)
  {
    // Use a temporary buffer to safely copy the string literal into the fixed-size queue item.
    char temp_msg[CLI_MAX_CMD_LEN];
    strncpy(temp_msg, CLI_NO_NEWLINE_MESSAGE, sizeof(temp_msg));
    temp_msg[sizeof(temp_msg) - 1] = '\0'; // Ensure null termination
    xQueueSend(cli_queue, temp_msg, 0);
  }
}

/**
 * @brief TinyUSB CDC line state change callback implementation.
 * @details This callback is invoked by the TinyUSB stack when the host changes
 *          the DTR (Data Terminal Ready) or RTS (Request To Send) line states
 *          for a CDC interface. It is used here to detect connection/disconnection
 *          of the CLI interface and print the initial welcome message.
 * @param[in] itf The interface index, should be '0' because its the only CDC interface defined.
 * @param[in] dtr The new state of the DTR line (true if set, false if cleared).
 * @param[in] rts The new state of the RTS line (true if set, false if cleared).
 * @note This function is executed in a TinyUSB context (tud_task() which is called periodically inside main loop).
 */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
  (void)rts; // Suppress unused parameter warning, RTS state isn't needed.
  (void)itf; // Suppress unused parameter warning, interface index isn't needed.

  if (dtr && !cli_connected)
  {
    // Terminal just connected.
    cli_connected = true;
    // Send an internal command to the CLI task to print the welcome message.
    // This is done to avoid calling a blocking printf from the USB task context.
    if (cli_queue != NULL)
    {
      // Use a temporary buffer to safely copy the string literal into the fixed-size queue item.
      char temp_msg[CLI_MAX_CMD_LEN];
      strncpy(temp_msg, CLI_WELCOME_MESSAGE, sizeof(temp_msg));
      temp_msg[sizeof(temp_msg) - 1] = '\0'; // Ensure null termination
      xQueueSend(cli_queue, temp_msg, 0);
    }
  }
  else if (!dtr && cli_connected)
  {
    // Terminal just disconnected.
    cli_connected = false;
    cli_cmd_buf[0] = '\0'; // Clear command buffer
  }
}

/**
 * @brief Main task loop for the CLI.
 * @param pvParameters Unused.
 */
void cli_task(void *pvParameters)
{
  // Initialize CLI resources (Queue) and TinyUSB settings
  cli_init();

  char cmd_buf[CLI_MAX_CMD_LEN];

  while (1)
  {
    // Wait indefinitely for a command to arrive in the queue
    if (xQueueReceive(cli_queue, cmd_buf, portMAX_DELAY) == pdTRUE)
    {
      // Check if this is the welcome message
      if (strcmp(cmd_buf, CLI_WELCOME_MESSAGE) == 0)
      {
        // Host connected, print welcome message directly.
        cli_printf("%s", cmd_buf);
      }
      // Check if this is the now newline message
      else if (strcmp(cmd_buf, CLI_NO_NEWLINE_MESSAGE) == 0)
      {
        // Print the no newline warning message.
        cli_printf("%s", cmd_buf);
      }
      // Else this is a user command.
      else
      {
        // Echo the received command if enabled
        if (cli_echo_enabled)
        {
          cli_printf("%s", cmd_buf);
        }
        // Execute the user command in this task's context
        cli_parse_and_execute(cmd_buf);
      }
    }
  }
}

//=====================================================================================================================
// Static Function Definitions (Command Handlers)
//=====================================================================================================================
/**
 * @brief Helper to write data to CDC, handling full buffer by waiting.
 */
static void cli_cdc_write_bytes(const char *data, uint32_t len)
{
  uint32_t bytes_to_write = len;
  uint32_t written_total = 0;

  while (bytes_to_write > 0)
  {
    uint32_t available_write = tud_cdc_write_available();
    if (available_write == 0)
    {
      // Buffer full, wait a bit for host to consume data
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    uint32_t write_this_iter = (bytes_to_write < available_write) ? bytes_to_write : available_write;
    uint32_t written_now = tud_cdc_write(data + written_total, write_this_iter);

    if (written_now > 0) tud_cdc_write_flush();
    written_total += written_now;
    bytes_to_write -= written_now;
  }
}

/**
 * @brief Helper function to send formatted output to the CLI (CDC interface 1).
 * @param format The format string (like printf).
 * @param ... Variable arguments for the format string.
 * @note Prints only if the CLI interface is connected. Handles writing in chunks if CDC buffer is full.
 */
static void cli_printf(const char *format, ...)
{
  char print_buf[128];
  va_list args;
  va_start(args, format);
  // Use vsnprintf to format into buffer, ensuring no overflow. -1 leaves space for null terminator.
  int len = vsnprintf(print_buf, sizeof(print_buf) - 1, format, args);
  va_end(args);

  if (len < 0)
  {
    // vsnprintf failed (e.g., encoding error), no valid string to print.
    return;
  }

  // Clamp length to buffer size if required (vsnprintf returns needed size)
  if (len >= (int)sizeof(print_buf)) len = sizeof(print_buf) - 1;
  print_buf[len] = '\0'; // Ensure null termination

  // Write the buffer content to the CDC interface 1 (Config Interface).
  // Use helper to handle flow control (wait if buffer full)
  cli_cdc_write_bytes(print_buf, (uint32_t)len);
}

/**
 * @brief Parses a complete command line string and dispatches execution to command handlers.
 * @details This function tokenizes the input string to identify the command verb
 *          and arguments, then calls the appropriate static command handler function.
 * @param command_line The null-terminated string containing the complete command and arguments.
 * @note This function is intended to be called by the tud_cdc_rx_wanted_cb
 *       when a complete command line (terminated by '\n' or '\r') is received
 *       and buffered in cli_cmd_buf.
 */
static void cli_parse_and_execute(char *command_line)
{
  // command_line buffer is already null-terminated by tud_cdc_rx_wanted_cb before calling this function.
  // Use strtok to find the command verb. Delimiters include space, tab, carriage return, newline.
  // strtok modifies the command_line buffer.
  char *cmd_verb = strtok(command_line, " \t\r\n");

  // Check if a command verb was found.
  if (cmd_verb == NULL || *cmd_verb == '\0')
  {
    // If no command verb (empty line), return.
    return;
  }

  // --- Get Arguments using subsequent strtok calls ---
  // The first call to strtok with NULL continues from the previous position.
  // We use different delimiters here - we usually want the *rest* of the line
  // including internal spaces, potentially trimming only leading/trailing whitespace.
  // However, for simple space-separated args, subsequent strtok calls work.
  // Let's get the *entire rest of the line* as the arguments string first.
  char *args = strtok(NULL, "\r\n"); // Use only line endings as delimiters here initially

  // Trim leading whitespace from the retrieved arguments string if necessary
  if (args != NULL)
  {
    while (*args == ' ' || *args == '\t')
    {
      args++;
    }
    // If trimming resulted in an empty string, treat as no args
    if (*args == '\0')
    {
      args = NULL;
    }
  }
  // 'args' now points to the start of the first argument (after the verb),
  // or is NULL if no arguments followed. Note: This 'args' might contain
  // multiple space-separated arguments. Individual handlers like set_range
  // will parse this 'args' string further using sscanf.

  bool success = false; // Flag to indicate command execution success/failure
  bool handled = false; // Flag to indicate if the command verb was recognized

  // --- Command Dispatch Table ---
  // Compare the command verb and call the corresponding handler.
  if (strcasecmp(cmd_verb, "help") == 0)
  {
    handled = true;
    success = cli_cmd_help(args);
  } else if (strcasecmp(cmd_verb, "reset") == 0)
  {
    handled = true;
    success = cli_cmd_reset(args);
  } else if (strcasecmp(cmd_verb, "echo") == 0)
  {
    handled = true;
    success = cli_cmd_echo(args);
  } else if (strcasecmp(cmd_verb, "scan") == 0)
  {
    handled = true;
    success = cli_cmd_scan(args);
  } else if (strcasecmp(cmd_verb, "mode") == 0)
  {
    handled = true;
    success = cli_cmd_mode(args);
  }

  // --- Handle Command Execution Results ---
  if (handled)
  {
    // Print a general success/failure message based on the handler's return value.
    if (!success)
    {
      cli_printf("Command Execution Failure!\r\n");
    }
  } else
  {
    // The command verb did not match any known commands.
    cli_printf("Unknown command: '%s' type 'help' for list of commands.\r\n", cmd_verb);
  }
}

/**
 * @brief Command handler for displaying the list of available commands.
 * @param args Pointer to the argument string. Expected to be NULL or empty.
 * @return true on success, false on failure (e.g., unexpected arguments).
 */
static bool cli_cmd_help(char *args)
{
  // Check for unexpected arguments after the 'help' command verb.
  if (args != NULL && *args != '\0')
  {
    cli_printf("Usage: help\r\n");
    return false; // Indicate failure due to invalid arguments
  }

  // Print the structured help message listing available commands and their basic usage.
  cli_printf("\r\nAvailable commands:\r\n");
  cli_printf("  help                  - Show command list\r\n");
  cli_printf("  reset                 - Software reset microcontroller\r\n");
  cli_printf("  echo [on|off]         - Enable/disable/toggle console echo\r\n");
  cli_printf("  scan                  - Scan for Wi-Fi networks\r\n");
  cli_printf("  mode [usb|udp-dongle|udp-headset|ble]    - Set application mode\r\n");
  // Add help text for any other implemented commands here following the same format

  return true; // Help command execution is considered successful if called correctly
}

/**
 * @brief Command handler to reset the controller.
 * @param args Pointer to the argument string. Expected to be NULL or empty.
 * @return Does not matter for obvious reasons.
 */
static bool cli_cmd_reset(char *args)
{
  // Check for unexpected arguments after the 'reset' command verb.
  if (args != NULL && *args != '\0')
  {
    cli_printf("Usage: reset\r\n");
    return false; // Indicate failure due to invalid arguments
  }

  uint32_t delay = 100;

  cli_printf("Device resets in %u ms...\r\n", delay);

  vTaskDelay(pdMS_TO_TICKS(delay));
  
  NVIC_SystemReset();
  
  // Return to supress compiler warning, though this line will never be reached.
  return true;
}

/**
 * @brief Command handler to enable, disable, or toggle console echo.
 * @param args Pointer to the argument string. Expects "on", "off", or empty/NULL to toggle.
 * @return true on success, false on failure (e.g., invalid arguments).
 */
static bool cli_cmd_echo(char *args)
{
  bool state_changed = false;
  bool new_state = cli_echo_enabled; // Assume no change initially

  if (args == NULL || *args == '\0')
  {
    // No arguments: Toggle the current state
    new_state = !cli_echo_enabled;
    state_changed = true;
  } else
  {
    // Arguments provided: Check for "on" or "off"
    if (strcasecmp(args, "on") == 0)
    {
      if (!cli_echo_enabled)
      {
        new_state = true;
        state_changed = true;
      }
      // else: Already on, no state change needed, but command is valid.
    } else if (strcasecmp(args, "off") == 0)
    {
      if (cli_echo_enabled)
      {
        new_state = false;
        state_changed = true;
      }
      // else: Already off, no state change needed, but command is valid.
    } else
    {
      // Invalid argument
      cli_printf("Usage: echo [on|off]\r\n");
      cli_printf("(No argument toggles current state)\r\n");
      return false; // Indicate command failure due to invalid argument
    }
  }

  // Update the state if it changed
  if (state_changed)
  {
    cli_echo_enabled = new_state;
  }

  // Report the current/new state
  cli_printf("Console echo is now %s.\r\n", cli_echo_enabled ? "ON" : "OFF");

  return true; // Indicate command processed successfully
}

/**
 * @brief Pretty-prints a JSON string to the CLI output with indentation and newlines.
 *
 * @details This helper function parses a raw JSON string and formats it for readability
 *          before sending it to the USB CDC interface. It performs the following:
 *          - Increases indentation level on opening braces '{' and brackets '['.
 *          - Decreases indentation level on closing braces '}' and brackets ']'.
 *          - Adds newlines after commas and structural characters.
 *          - Preserves all characters (including whitespace) inside string quotes.
 *          - Strips existing whitespace outside of strings to ensure consistent formatting.
 *          - Buffers output locally to reduce overhead on the USB stack.
 *
 * @param[in] json The null-terminated JSON string to format and print.
 */
static void cli_print_formatted_json(const char *json)
{
    if (!json) return;

    int indent_level = 0;
    const int indent_size = 2;
    bool in_string = false;

    // Local buffer to accumulate characters before sending to the CDC interface.
    // This reduces the number of write calls.
    char buffer[128];
    int buf_idx = 0;

    // Start with a newline for clean output
    cli_cdc_write_bytes("\r\n", 2);

    for (const char *p = json; *p; p++)
    {
        // Check if buffer is nearly full. We leave a margin of 8 bytes
        // to safely handle cases where a single iteration might add multiple
        // characters (e.g., ": " or "\r\n"). Note that indentation loops
        // have their own internal buffer checks.
        if (buf_idx >= (sizeof(buffer) - 8)) {
            cli_cdc_write_bytes(buffer, buf_idx);
            buf_idx = 0;
        }

        // Detect start or end of a string.
        // We must track this because structural characters like '{', '}', ','
        // inside a string must be printed literally, not formatted.
        // We check for escaped quotes ('\"') to ensure they don't close the string.
        if (*p == '"' && (p == json || *(p - 1) != '\\'))
        {
            in_string = !in_string;
        }

        // If we are currently inside a JSON string value, print the character
        // exactly as is, preserving whitespace and formatting characters.
        if (in_string)
        {
            buffer[buf_idx++] = *p;
            continue;
        }

        // Process structural characters for formatting
        switch (*p)
        {
            case '{':
            case '[':
                // Opening brace/bracket:
                // 1. Print the character.
                // 2. Increase indentation level.
                // 3. Start a new line.
                // 4. Print indentation spaces.
                buffer[buf_idx++] = *p;
                indent_level++;
                buffer[buf_idx++] = '\r';
                buffer[buf_idx++] = '\n';
                
                // Output indentation spaces, flushing buffer if needed during the loop
                for (int i = 0; i < indent_level * indent_size; i++) {
                    if (buf_idx >= sizeof(buffer)) {
                        cli_cdc_write_bytes(buffer, buf_idx);
                        buf_idx = 0;
                    }
                    buffer[buf_idx++] = ' ';
                }
                break;

            case '}':
            case ']':
                // Closing brace/bracket:
                // 1. Decrease indentation level.
                // 2. Start a new line.
                // 3. Print indentation spaces (for the closing brace itself).
                // 4. Print the closing character.
                indent_level--;
                buffer[buf_idx++] = '\r';
                buffer[buf_idx++] = '\n';
                
                // Output indentation spaces
                for (int i = 0; i < indent_level * indent_size; i++) {
                    if (buf_idx >= sizeof(buffer)) {
                        cli_cdc_write_bytes(buffer, buf_idx);
                        buf_idx = 0;
                    }
                    buffer[buf_idx++] = ' ';
                }
                
                // Ensure space for the closing char
                if (buf_idx >= sizeof(buffer)) {
                    cli_cdc_write_bytes(buffer, buf_idx);
                    buf_idx = 0;
                }
                buffer[buf_idx++] = *p;
                break;

            case ',':
                // Comma separator:
                // 1. Print the comma.
                // 2. Start a new line.
                // 3. Print indentation spaces for the next item.
                buffer[buf_idx++] = ',';
                buffer[buf_idx++] = '\r';
                buffer[buf_idx++] = '\n';
                
                // Output indentation spaces
                for (int i = 0; i < indent_level * indent_size; i++) {
                    if (buf_idx >= sizeof(buffer)) {
                        cli_cdc_write_bytes(buffer, buf_idx);
                        buf_idx = 0;
                    }
                    buffer[buf_idx++] = ' ';
                }
                break;

            case ':':
                // Key-value separator:
                // Print colon followed by a space for readability.
                buffer[buf_idx++] = ':';
                buffer[buf_idx++] = ' ';
                break;

            // Skip whitespace characters that are outside of strings.
            // This effectively "minifies" the input before re-formatting it,
            // ensuring consistent output regardless of input formatting.
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                break;

            default:
                // Regular characters (numbers, booleans, null, etc.)
                buffer[buf_idx++] = *p;
                break;
        }
    }
    
    // Flush any remaining data in the buffer to the CDC interface
    if (buf_idx > 0) {
        cli_cdc_write_bytes(buffer, buf_idx);
    }
    cli_cdc_write_bytes("\r\n", 2);
}

/**
 * @brief Command handler for Wi-Fi scan.
 * @param args Pointer to the argument string. Expected to be NULL or empty.
 * @return true on success, false on failure.
 */
static bool cli_cmd_scan(char *args)
{
  if (args != NULL && *args != '\0')
  {
    cli_printf("Usage: scan\r\n");
    return false;
  }

  cli_printf("Scanning for Wi-Fi networks...\r\n");
  char *scanData = WPL_Scan();
  if (scanData != NULL)
  {
    cli_print_formatted_json(scanData);
    vPortFree(scanData);
  }
  else
  {
    cli_printf("Scan failed or no networks found.\r\n");
  }

  return true;
}


/**
 * @brief Command handler to change the operating mode.
 * @param args Pointer to the argument string. Expects "usb", "udp", or "ble".
 * @return true on success, false on failure (e.g., invalid arguments).
 */
static bool cli_cmd_mode(char *args)
{
  app_mode_t current_mode = get_app_mode();
  app_mode_t target_mode = current_mode;

  // Check arguments
  if (args == NULL || *args == '\0')
  {
    cli_printf("Current Mode: %s\r\n", get_app_mode_name(current_mode));
    return true;
  }
  else {
    if (strcasecmp(args, "usb") == 0) {
      target_mode = MODE_USB_AUDIO;
    }
    else if (strcasecmp(args, "udp-dongle") == 0) {
      target_mode = MODE_UDP_DONGLE_AUDIO;
    }
    else if (strcasecmp(args, "udp-headset") == 0) {
      target_mode = MODE_UDP_HEADSET_AUDIO;
    }
    else if (strcasecmp(args, "ble") == 0) {
      target_mode = MODE_BLE_AUDIO;
    }
    else {
      cli_printf("Usage: mode [usb|udp-dongle|udp-headset|ble]\r\n");
      return false; // Indicate command failure due to invalid argument
    }
  }

  // Warn user that reconnecting might be needed if usb descriptor profile needs to be updated
  usb_desc_profile_t current_profile = get_usb_profile_for_mode(current_mode);
  usb_desc_profile_t target_profile = get_usb_profile_for_mode(target_mode);
  if (current_profile != target_profile) {
    cli_printf("Warning: Re-enumerating to %s mode may require re-connecting the CDC/VCP interface.\r\n", get_app_mode_name(target_mode));
  }

  // Check if mode change is needed
  if (target_mode != current_mode) {
    bool mode_change_success = false;
    cli_printf("Changing mode from %s to %s...\r\n", get_app_mode_name(current_mode), get_app_mode_name(target_mode));
    mode_change_success = set_current_app_mode(target_mode);
    if (!mode_change_success)
    {
      cli_printf("Failed to switch to %s.\r\n", get_app_mode_name(target_mode));
      return false;
    }
    cli_printf("Mode changed to: %s\r\n", get_app_mode_name(target_mode));
  }
  else {
    cli_printf("Mode remains: %s\r\n", get_app_mode_name(current_mode));
  }

  

  // Indicate command processed successfully
  return true; 
}
