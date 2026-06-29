#ifndef YAP_C_TYPES_H
#define YAP_C_TYPES_H

#include "yap/all.h"
#include <stdio.h>
#include <stdint.h>

// Logical clock: each declaration gets a timestamp.
// When we need a symbol, we compare the symbol's timestamp to the
// last relocate timestamp to know if recompilation is needed.
typedef uint64_t yap_c_timestamp;

//Module
kenobi_new_struct_free(yap_module_c_code,
    // File-based emission: keep handles open for the lifetime of the module
    char   out_dir[1024];   // temp build directory
    FILE*  types_fp;        // types.h   (always open for append)
    FILE*  decls_fp;        // prototypes.h (always open for append)
    FILE*  impl_fp;         // impl.c    (always open for append)
    FILE*  comptime_fp;     // comptime.c (comptime-only functions, TCC reads but GCC doesn't)

    // Logical clock: increments on every declaration
    yap_c_timestamp clock;

    // Per-declaration timestamp tracking (for incremental recompile)
    // Maps declaration name -> timestamp it was emitted at
    darr(yap_c_timestamp) decl_timestamps;  // indexed by a decl counter
    uint32_t decl_count;                    // total declarations emitted
);

#endif //YAP_C_TYPES_H