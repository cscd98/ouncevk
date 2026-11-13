/* SPDX-License-Identifier: MIT */

/*
 * Stub <spirv-tools/libspirv.h> for Horizon.
 * It declares the part of the SPIRV-Tools C API that Mesa uses.
 * SPIRV-Tools is not available on the target, so disassembly always fails.
 */

#ifndef SWITCH_CROSS_SPIRV_TOOLS_LIBSPIRV_H
#define SWITCH_CROSS_SPIRV_TOOLS_LIBSPIRV_H

#include <stddef.h>
#include <stdint.h>

typedef int spv_target_env;
typedef int spv_result_t;
typedef uint32_t spv_binary_to_text_options_t;

typedef struct spv_context_t *spv_context;
typedef struct spv_diagnostic_t *spv_diagnostic;

typedef struct spv_text_t {
    const char *str;
    size_t length;
} spv_text_t;

typedef const spv_text_t *spv_text;

/* Results. Callers check only for success. */
#define SPV_SUCCESS 0
#define SPV_UNSUPPORTED 1

/* Target environment. The value matches the SPIRV-Tools enumeration. */
#define SPV_ENV_UNIVERSAL_1_6 24

/* Disassembly option bits. */
#define SPV_BINARY_TO_TEXT_OPTION_COLOR (1u << 2)
#define SPV_BINARY_TO_TEXT_OPTION_INDENT (1u << 3)
#define SPV_BINARY_TO_TEXT_OPTION_SHOW_BYTE_OFFSET (1u << 4)
#define SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES (1u << 6)

#ifdef __cplusplus
extern "C" {
#endif

static inline spv_context spvContextCreate(spv_target_env environment)
{
    (void)environment;
    return NULL;
}

static inline void spvContextDestroy(spv_context context)
{
    (void)context;
}

static inline spv_result_t spvBinaryToText(spv_context context,
                                           const uint32_t *binary,
                                           size_t word_count,
                                           uint32_t options,
                                           spv_text *text,
                                           spv_diagnostic *diagnostic)
{
    (void)context;
    (void)binary;
    (void)word_count;
    (void)options;
    if (text != NULL)
        *text = NULL;
    if (diagnostic != NULL)
        *diagnostic = NULL;
    return SPV_UNSUPPORTED;
}

static inline void spvTextDestroy(spv_text text)
{
    (void)text;
}

static inline void spvDiagnosticPrint(spv_diagnostic diagnostic)
{
    (void)diagnostic;
}

static inline void spvDiagnosticDestroy(spv_diagnostic diagnostic)
{
    (void)diagnostic;
}

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_CROSS_SPIRV_TOOLS_LIBSPIRV_H */
