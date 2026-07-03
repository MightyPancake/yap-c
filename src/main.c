#include "yap_c.h"

extern const char* ct_builder_decls;

void yap_backend_init(yap_ctx* ctx){
#ifdef YAP_LOG
    yap_c_run_tcc_smoke_test(ctx);
#endif
    yap_c_init_tcc_state(ctx);
    yap_c_set_comptime_ctx(ctx);
    yap_log("Backend (yap-c) initialized");
}

void yap_backend_free(yap_ctx* ctx){
    // yap_emit() normally frees a module's codegen state (module_ctx) once
    // it finishes emitting it. If compilation stops early (e.g. a semantic
    // error caught before Phase 3 ever runs yap_emit), any module that had
    // at least one declaration lazily init its module_ctx via yap_gen_decl
    // never gets that cleanup call. Catch those here; a no-op for modules
    // yap_emit already freed since yap_c_free_module guards on module_ctx.
    void* item;
    size_t iter = 0;
    while (hashmap_iter(ctx->modules, &iter, &item)) {
        yap_module* m = item;
        yap_c_free_module(m);
    }
    yap_c_free_tcc_state(ctx);
}

void yap_c_init_module(yap_module* module){
    yap_module_c_code* mod_code = mem_one(yap_module_c_code);

    // Create temp build directory for this module
    char* tmpdir = yap_make_temp_dir();
    if (tmpdir){
        snprintf(mod_code->out_dir, sizeof(mod_code->out_dir), "%s", tmpdir);
        free(tmpdir);
    } else {
        snprintf(mod_code->out_dir, sizeof(mod_code->out_dir), "/tmp/yap_build");
        yap_rmdir_recursive(mod_code->out_dir);
        yap_mkdir(mod_code->out_dir);
    }

    // Open the three emission files for append (keep handles alive)
    char path[YAP_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/types.h", mod_code->out_dir);
    mod_code->types_fp = fopen(path, "w");
    if (mod_code->types_fp) {
        fputs(
            "#pragma once\n"
            "#include <stdint.h>\n"
            "#include <stdbool.h>\n"
            "#include <stddef.h>\n\n",
            mod_code->types_fp
        );
        fputs(ct_builder_decls, mod_code->types_fp);
        fputc('\n', mod_code->types_fp);
        fflush(mod_code->types_fp);
    } else {
        yap_log("Failed to open %s", path);
    }

    snprintf(path, sizeof(path), "%s/prototypes.h", mod_code->out_dir);
    mod_code->decls_fp = fopen(path, "w");
    if (mod_code->decls_fp) {
        fputs(
            "#pragma once\n"
            "#include \"types.h\"\n\n",
            mod_code->decls_fp
        );
        fflush(mod_code->decls_fp);
    } else {
        yap_log("Failed to open %s", path);
    }

    snprintf(path, sizeof(path), "%s/impl.c", mod_code->out_dir);
    mod_code->impl_fp = fopen(path, "w");
    if (mod_code->impl_fp){
        // Write preamble immediately
        fputs(
            "#line 0 \"yap_c_output.c\"\n"
            "#include <stdint.h>\n"
            "#include <stdbool.h>\n"
            "#include <stddef.h>\n"
            "#include \"types.h\"\n"
            "#include \"prototypes.h\"\n\n",
            mod_code->impl_fp
        );
        fflush(mod_code->impl_fp);
    } else {
        yap_log("Failed to open %s", path);
    }

    // Init clock and timestamp tracking
    mod_code->clock = 0;
    mod_code->decl_timestamps = darr_new(yap_c_timestamp);
    mod_code->decl_count = 0;

    module->module_ctx = mod_code;
    yap_log("Module init: files in %s", mod_code->out_dir);
}

void yap_c_free_module(yap_module* module){
    if (!module || !module->module_ctx) return;
    yap_module_c_code* mod_code = module->module_ctx;

    // Close file handles
    if (mod_code->types_fp)    fclose(mod_code->types_fp);
    if (mod_code->decls_fp)    fclose(mod_code->decls_fp);
    if (mod_code->impl_fp)     fclose(mod_code->impl_fp);

    // Free timestamp tracking
    darr_free(mod_code->decl_timestamps);

    // Clean up temp build directory
    if (mod_code->out_dir[0])
        yap_rmdir_recursive(mod_code->out_dir);

    free(mod_code);
    module->module_ctx = NULL;
}