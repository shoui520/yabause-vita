#ifndef TEST_VITASHARK_H
#define TEST_VITASHARK_H
#include <stdint.h>
typedef enum { SHARK_OPT_SLOW, SHARK_OPT_SAFE, SHARK_OPT_DEFAULT, SHARK_OPT_FAST, SHARK_OPT_UNSAFE } shark_opt;
typedef enum { SHARK_LOG_INFO, SHARK_LOG_WARNING, SHARK_LOG_ERROR } shark_log_level;
void shark_install_log_cb(void (*callback)(const char *, shark_log_level, int));
void vglSetupRuntimeShaderCompiler(shark_opt, int32_t, int32_t, int32_t);
#endif
