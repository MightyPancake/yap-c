#include "yap_c.h"

void yap_backend_init(yap_ctx* ctx){
    yap_c_init_tcc_state(ctx);
    yap_log("Backend (yap-c) initialized");
}

void yap_backend_free(yap_ctx* ctx){
    yap_c_free_tcc_state(ctx);
    yap_log("Backend (yap-c) freed");
}

void yap_c_init_module(yap_module* module){
    yap_module_c_code* mod_code = mem_one(yap_module_c_code);
    //Init code arrays
    mod_code->types = darr_new(yap_strbuf);
    mod_code->decls = darr_new(yap_strbuf);
    mod_code->impl = darr_new(yap_strbuf);

    //Attach to module
    module->module_ctx = mod_code;
}

void yap_c_free_module(yap_module* module){
    if (!module || !module->module_ctx) return;
    yap_module_c_code* mod_code = module->module_ctx;
    //Free code arrays
    for_darr(i, type_code, mod_code->types){
        yap_strbuf_free(&type_code);
    }
    darr_free(mod_code->types);
    for_darr(i, decl_code, mod_code->decls){
        yap_strbuf_free(&decl_code);
    }
    darr_free(mod_code->decls);
    for_darr(i, impl_code, mod_code->impl){
        yap_strbuf_free(&impl_code);
    }
    darr_free(mod_code->impl);

    //Free module ctx
    free(mod_code);
}