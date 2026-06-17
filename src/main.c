#include "yap_c.h"

//This walks the AST looks for macro defintions and registers them in the context
yap_ctx* yap_register_macros(yap_ctx* ctx){
    // yap_log("\n\nPhase 2: Macro registration\n");
    return ctx;
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