#ifndef LOG_H
#define LOG_H

    /* * --------------------------------------------------------------------------
    * Global Logger Redirection & SDK Override
    * --------------------------------------------------------------------------
    * This header is forced into every compilation unit via CMake (-include).
    * It redirects standard SDK PRINTF calls to a non-blocking FreeRTOS queue.
    */

    /* Protect against Assembly files (e.g., startup.S) seeing C syntax */
    #if !defined(__ASSEMBLER__)
        /* * --------------------------------------------------------------------------
        * The "Include-Undef-Redefine" Pattern
        * --------------------------------------------------------------------------
        * 1. Include SDK header FIRST to let it define its macros (avoiding warnings).
        * 2. Undefine PRINTF to remove the blocking debug console implementation.
        * 3. Redefine PRINTF to point to our async logger.
        * Because fsl_debug_console.h has include guards, it won't be loaded again.
        */
        #include "fsl_debug_console.h"

        #ifdef PRINTF
            #undef PRINTF
        #endif

        #define PRINTF log_print

        /* --------------------------------------------------------------------------
        * Logging API
        * -------------------------------------------------------------------------- */

        void log_init_q(void);
        void log_task(void *pvParameters);
        int log_print(const char *format, ...);


    #endif /* !defined(__ASSEMBLER__) */

#endif // LOG_H