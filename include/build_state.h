#ifndef YAP_C_BUILD_STATE_H
#define YAP_C_BUILD_STATE_H

#include "yap/types.h"
#include <libtcc.h>

//Forward declare
typedef struct yap_ctx yap_ctx;

//Incremental compilation build state (holds TCC state, counter, etc.)
typedef uint64_t yap_c_incremental_counter;

kenobi_new_struct_free(yap_c_build_state,
    TCCState* tcc;
    yap_c_incremental_counter counter;
);

//Initialize TCC state and attach build state to ctx
void yap_c_init_tcc_state(yap_ctx* ctx);
void yap_c_free_tcc_state(yap_ctx* ctx);

//Small tcc test
void yap_c_test_tcc(yap_ctx* ctx);

//Feed a C string into the TCC state for incremental compilation
//Returns 0 on success, -1 on error
int yap_c_feed_c(yap_ctx* ctx, const char* c_code);

//Retrieve a compiled symbol (function pointer, variable, etc.) by name
//Returns NULL if not found
void* yap_c_get_symbol(yap_ctx* ctx, const char* name);

#endif //YAP_C_BUILD_STATE_H
