#include "yap_c.h"

void yap_c_init_tcc_state(yap_ctx* ctx){
    if (ctx->build_state) return; //Already initialized

    yap_c_build_state* state = mem_one(yap_c_build_state);
    state->tcc = tcc_new();
    if (!state->tcc){
        yap_log("Failed to create TCC state");
        free(state);
        return;
    }
    tcc_set_output_type(state->tcc, TCC_OUTPUT_MEMORY);
    state->counter = 0;

    ctx->build_state = state;
    yap_log("TCC build state initialized");
}

void yap_c_free_tcc_state(yap_ctx* ctx){
    if (!ctx->build_state) return;
    yap_c_build_state* state = ctx->build_state;
    if (state->tcc){
        tcc_delete(state->tcc);
        state->tcc = NULL;
    }
    free(state);
    ctx->build_state = NULL;
    yap_log("TCC build state freed");
}
