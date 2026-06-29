#ifndef YAP_C_BUILD_STATE_H
#define YAP_C_BUILD_STATE_H

#include "yap/types.h"
#include <libtcc.h>

//Forward declare
typedef struct yap_ctx yap_ctx;
typedef struct yap_module yap_module;

//Incremental compilation build state (holds TCC state, counter, etc.)
typedef uint64_t yap_c_incremental_counter;

kenobi_new_struct_free(yap_c_build_state,
    TCCState* tcc;
    yap_c_incremental_counter counter;
);

//Standalone smoke test (throwaway state, full compile+relocate+call cycle)
void yap_c_run_tcc_smoke_test(yap_ctx* ctx);

//Initialize TCC state and attach build state to ctx (does NOT relocate)
void yap_c_init_tcc_state(yap_ctx* ctx);
void yap_c_free_tcc_state(yap_ctx* ctx);

//Feed a C string into the TCC state for incremental compilation
//Returns 0 on success, -1 on error
int yap_c_feed_c(yap_ctx* ctx, const char* c_code);

//Retrieve a compiled symbol (function pointer, variable, etc.) by name.
//If the symbol was declared after the last relocate, this will:
//  1. Destroy old TCCState
//  2. Create a fresh one
//  3. Re-feed ALL files from disk (types.h, prototypes.h, impl.c)
//  4. tcc_relocate() the new state
//  5. Return tcc_get_symbol()
//Returns NULL if not found.
void* yap_c_ensure_symbol(yap_ctx* ctx, const char* name);

// Force a full recompile by feeding all module files into a fresh TCCState
// and relocating. Returns 0 on success, -1 on error.
int yap_c_recompile_from_files(yap_ctx* ctx, yap_module* module);

void yap_tcc_check_main(yap_ctx* ctx);
void yap_c_set_comptime_ctx(yap_ctx* ctx);
void yap_c_set_macro_name(const char* name);
void yap_c_set_macro_loc(yap_source* src, yap_loc loc);
void yap_c_pop_macro_loc(void);

#endif //YAP_C_BUILD_STATE_H
