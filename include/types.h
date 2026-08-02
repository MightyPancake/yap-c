#ifndef YAP_C_TYPES_H
#define YAP_C_TYPES_H

#include "yap/all.h"
#include <stdio.h>
#include <stdint.h>

// Logical clock: each declaration gets a timestamp, compared against the last relocate's timestamp to detect if recompilation is needed.
typedef uint64_t yap_c_timestamp;

// A function definition this module emitted: its yap-level name (as written in source) and
// the actual emitted C symbol name (after module-prefix mangling). Used to resolve
// '-bexport=<name>' backend flags to real C symbols, and to validate the name was real.
typedef struct {
    char* yap_name;
    char* c_name;
} yap_c_emitted_func;

//Module
kenobi_new_struct_free(yap_module_c_code,
    // File-based emission: keep handles open for the lifetime of the module
    char   out_dir[1024];   // temp build directory
    FILE*  types_fp;        // types.h   (always open for append)
    FILE*  decls_fp;        // prototypes.h (always open for append)
    FILE*  impl_fp;         // impl.c    (always open for append)

    // Logical clock: increments on every declaration
    yap_c_timestamp clock;

    // Per-declaration timestamp tracking (for incremental recompile)
    // Maps declaration name -> timestamp it was emitted at
    darr(yap_c_timestamp) decl_timestamps;  // indexed by a decl counter
    uint32_t decl_count;                    // total declarations emitted

    // Function definitions emitted so far, and whether one of them is 'main' -- both used
    // by the emcc '-bexport='/no-main wiring in yap_emit.
    darr(yap_c_emitted_func) emitted_funcs;
    bool has_main;

    // Hashes of element-type C strings for slice typedefs already written to types.h.
    // Prevents duplicate anonymous struct definitions for the same slice element type.
    darr(uint64_t) emitted_slice_hashes;
);

#endif //YAP_C_TYPES_H