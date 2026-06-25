#include "yap_c.h"

static void tcc_error_callback(void* opaque, const char* msg){
    yap_ctx* ctx = (yap_ctx*)opaque;
    // TCC may emit warnings during the test phase (e.g. implicit printf).
    // Only push actual errors (containing "error:"), not warnings.
    if (strstr(msg, "error:") || strstr(msg, "Error:")){
        yap_log("TCC error: %s", msg);
        yap_emit_error_no_pos(ctx, "TCC: %s", msg);
    } else {
        yap_log("TCC: %s", msg);
    }
}

// Standalone smoke test: creates a throwaway TCC state, compiles+relocates+calls,
// then destroys it.  This verifies the full TCC pipeline without freezing the
// real build state.  Errors from the smoke test are not forwarded to ctx.
void yap_c_run_tcc_smoke_test(yap_ctx* ctx){
    (void)ctx;
    yap_log("Running TCC smoke test (separate state)");

    TCCState* test_tcc = tcc_new();
    if (!test_tcc){
        yap_log("Failed to create TCC state for smoke test");
        return;
    }
    tcc_set_output_type(test_tcc, TCC_OUTPUT_MEMORY);
    // Do NOT use tcc_error_callback — smoke test errors stay in this state

    // Configure the test state with paths (same as real init)
    char path[YAP_PATH_MAX];
    char* yap_home = yap_get_yap_home_path();
    const char* tcc_sys = getenv("TCC_LIB_PATH");
    if (!tcc_sys){
        FILE* tp = popen(
            "(find /nix/store/*tcc*/lib/tcc -name 'x86_64-libtcc1.a' 2>/dev/null;"
            " ls /usr/lib/tcc/libtcc1.a /usr/lib/x86_64-linux-gnu/tcc/libtcc1.a 2>/dev/null)"
            " | head -1", "r");
        if (tp){
            char found[YAP_PATH_MAX] = "";
            if (fgets(found, sizeof(found), tp) && found[0] == '/'){
                found[strcspn(found, "\n")] = '\0';
                char* slash = strrchr(found, '/');
                if (slash) *slash = '\0';
                tcc_set_lib_path(test_tcc, found);
                tcc_add_library_path(test_tcc, found);
            }
            pclose(tp);
        }
    }
    snprintf(path, sizeof(path), "%s/components/yap-c/tinycc", yap_home);
    tcc_add_library_path(test_tcc, path);

    // Add GCC include paths
    FILE* f = popen("echo | gcc -E -Wp,-v -x c - 2>&1", "r");
    if (f){
        char line[YAP_PATH_MAX];
        bool in_section = false;
        while (fgets(line, sizeof(line), f)){
            line[strcspn(line, "\n")] = '\0';
            if (strstr(line, "#include") && strstr(line, "search starts here")){ in_section = true; continue; }
            if (strstr(line, "End of search list")) break;
            if (in_section && line[0] == ' '){
                char* p = line; while (*p == ' ') p++;
                if (strlen(p) > 0 && p[0] == '/'){
                    tcc_add_include_path(test_tcc, p);
                    tcc_add_sysinclude_path(test_tcc, p);
                }
            }
        }
        pclose(f);
    }

    // Add GCC library paths (needed for tcc_relocate)
    f = popen("echo 'int main(){}' | gcc -x c - -Wl,--verbose 2>&1", "r");
    if (f){
        char line[YAP_PATH_MAX];
        while (fgets(line, sizeof(line), f)){
            const char* prefix = "SEARCH_DIR(\"";
            char* start = strstr(line, prefix);
            if (start){
                start += strlen(prefix);
                char* end = strchr(start, '"');
                if (end){ *end = '\0'; tcc_add_library_path(test_tcc, start); }
            }
            prefix = "attempt to open ";
            start = strstr(line, prefix);
            if (start){
                start += strlen(prefix);
                char* end = strstr(start, "/lib");
                if (end){
                    end += 4; char saved = *end; *end = '\0';
                    tcc_add_library_path(test_tcc, start);
                    *end = saved;
                }
            }
        }
        pclose(f);
    }
    free(yap_home);

    // Test: compile + relocate + call a function that uses printf
    const char* test_code =
        "#include <stdio.h>\n"
        "int __yap_smoke_func(int x) {\n"
        "    printf(\"Hello from TCC inside YAP! x=%d\\n\", x);\n"
        "    return x + 42;\n"
        "}\n";
    if (tcc_compile_string(test_tcc, test_code) == -1){
        yap_log("TCC smoke test: compile failed");
        tcc_delete(test_tcc);
        return;
    }

    // Relocate and call — pure function, no libc dependency
    if (tcc_relocate(test_tcc) != 0){
        yap_log("TCC smoke test: relocate failed (non-fatal on some systems)");
        tcc_delete(test_tcc);
        return;
    }
    int (*func)(int) = tcc_get_symbol(test_tcc, "__yap_smoke_func");
    if (func){
        int result = func(58);
        if (result == 100){
            yap_log("TCC smoke test PASSED (%d == 100)", result);
        } else {
            yap_log("TCC smoke test FAILED: got %d, expected 100", result);
        }
    } else {
        yap_log("TCC smoke test: get_symbol returned NULL");
    }
    tcc_delete(test_tcc);
}

void yap_c_init_tcc_state(yap_ctx* ctx){
    if (ctx->build_state) return;

    yap_c_build_state* state = mem_one(yap_c_build_state);
    state->tcc = tcc_new();
    if (!state->tcc){
        yap_log("Failed to create TCC state");
        free(state);
        return;
    }
    tcc_set_output_type(state->tcc, TCC_OUTPUT_MEMORY);
    tcc_set_error_func(state->tcc, ctx, tcc_error_callback);

    // Resolve paths relative to yap home
    char path[YAP_PATH_MAX];
    char* yap_home = yap_get_yap_home_path();

    // TCC internal headers + runtime libs.
    // Probe for libtcc1.a: Nix store, then Debian/Ubuntu, then bundled
    const char* tcc_sys = getenv("TCC_LIB_PATH");
    if (!tcc_sys){
        FILE* tp = popen(
            "(find /nix/store/*tcc*/lib/tcc -name 'x86_64-libtcc1.a' 2>/dev/null;"
            " ls /usr/lib/tcc/libtcc1.a /usr/lib/x86_64-linux-gnu/tcc/libtcc1.a 2>/dev/null)"
            " | head -1", "r");
        if (tp){
            char found[YAP_PATH_MAX] = "";
            if (fgets(found, sizeof(found), tp) && found[0] == '/'){
                found[strcspn(found, "\n")] = '\0';
                char* libdir = found;
                char* slash = strrchr(libdir, '/');
                if (slash) *slash = '\0';
                tcc_set_lib_path(state->tcc, libdir);
                tcc_add_library_path(state->tcc, libdir);
                yap_log("TCC system lib path: %s", libdir);
                tcc_sys = found;
            }
            pclose(tp);
        }
    }
    if (!tcc_sys){
        // Bundled tinycc
        snprintf(path, sizeof(path), "%s/components/yap-c/tinycc", yap_home);
        tcc_set_lib_path(state->tcc, path);
        tcc_add_library_path(state->tcc, path);
    } else {
        // Add bundled tinycc for headers too
        snprintf(path, sizeof(path), "%s/components/yap-c/tinycc", yap_home);
        tcc_add_library_path(state->tcc, path);
    }

    // Probe GCC for system include + library paths
    // We parse include paths from -E -Wp,-v, and library paths from -Wl,--verbose
    // Probe GCC for system include paths
    FILE* f = popen("echo | gcc -E -Wp,-v -x c - 2>&1", "r");
    if (f){
        char line[YAP_PATH_MAX];
        bool in_section = false;
        while (fgets(line, sizeof(line), f)){
            line[strcspn(line, "\n")] = '\0';
            if (strstr(line, "#include") && strstr(line, "search starts here")){
                in_section = true;
                continue;
            }
            if (strstr(line, "End of search list")) break;
            if (in_section && line[0] == ' '){
                char* p = line;
                while (*p == ' ') p++;
                if (strlen(p) > 0 && p[0] == '/'){
                    tcc_add_include_path(state->tcc, p);
                    tcc_add_sysinclude_path(state->tcc, p);
                    //yap_log("TCC include path: %s", p);
                }
            }
        }
        pclose(f);
    }

    // Probe GCC linker for library search dirs.
    // On NixOS, the format is "attempt to open /path/libc.so ..."
    // On Debian, it's "SEARCH_DIR(\"/path\")"
    f = popen("echo 'int main(){}' | gcc -x c - -Wl,--verbose 2>&1", "r");
    if (f){
        char line[YAP_PATH_MAX];
        while (fgets(line, sizeof(line), f)){
            // Try SEARCH_DIR format first
            const char* prefix = "SEARCH_DIR(\"";
            char* start = strstr(line, prefix);
            if (start){
                start += strlen(prefix);
                char* end = strchr(start, '"');
                if (end){
                    *end = '\0';
                    tcc_add_library_path(state->tcc, start);
                    //yap_log("TCC library path: %s", start);
                }
            }
            // Try "attempt to open /path/lib..." format (NixOS)
            prefix = "attempt to open ";
            start = strstr(line, prefix);
            if (start){
                start += strlen(prefix);
                char* end = strstr(start, "/lib");
                if (end){
                    end += 4; // skip "/lib"
                    char saved = *end;
                    *end = '\0';
                    // Don't add duplicates
                    tcc_add_library_path(state->tcc, start);
                    //yap_log("TCC library path: %s", start);
                    *end = saved;
                }
            }
        }
        pclose(f);
    }

    free(yap_home);
    state->counter = 0;
    ctx->build_state = state;
    yap_log("TCC build state initialized (NOT frozen — no relocate call)");
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

int yap_c_feed_c(yap_ctx* ctx, const char* c_code){
    if (!ctx->build_state){
        yap_log("TCC state not initialized - call yap_c_init_tcc_state first");
        return -1;
    }
    yap_c_build_state* state = ctx->build_state;
    if (tcc_compile_string(state->tcc, c_code) == -1){
        yap_log("TCC compile failed for: %s", c_code);
        return -1;
    }
    state->counter++;
    yap_log("TCC fed %zu bytes (counter=%lu)", strlen(c_code), state->counter);
    return 0;
}

void* yap_c_get_symbol(yap_ctx* ctx, const char* name){
    if (!ctx->build_state){
        yap_log("TCC state not initialized");
        return NULL;
    }
    yap_c_build_state* state = ctx->build_state;
    void* sym = tcc_get_symbol(state->tcc, name);
    if (!sym)
        yap_log("Symbol '%s' not found in TCC state", name);
    else
        yap_log("Symbol '%s' resolved to %p", name, sym);
    return sym;
}

// Feed a file directly into TCC (no intermediate buffer)
static int feed_file_to_tcc(yap_ctx* ctx, const char* path){
    yap_c_build_state* state = ctx->build_state;
    if (!state || !state->tcc) return -1;
    if (tcc_add_file(state->tcc, path) == -1){
        yap_log("TCC compile failed for file: %s", path);
        return -1;
    }
    state->counter++;
    return 0;
}

int yap_c_recompile_from_files(yap_ctx* ctx, yap_module* module){
    if (!ctx || !module || !module->module_ctx) return -1;
    yap_module_c_code* mod_code = module->module_ctx;

    // Flush all open file handles before reading them back
    if (mod_code->types_fp) fflush(mod_code->types_fp);
    if (mod_code->decls_fp) fflush(mod_code->decls_fp);
    if (mod_code->impl_fp)  fflush(mod_code->impl_fp);

    // Destroy old TCC state
    yap_c_free_tcc_state(ctx);

    // Create fresh TCC state
    yap_c_init_tcc_state(ctx);
    if (!ctx->build_state){
        yap_log("Failed to re-init TCC state during recompile");
        return -1;
    }

    // Feed all three files in order
    char path[YAP_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/types.h", mod_code->out_dir);
    if (feed_file_to_tcc(ctx, path) != 0){
        yap_log("Failed to feed %s to TCC", path);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/prototypes.h", mod_code->out_dir);
    if (feed_file_to_tcc(ctx, path) != 0){
        yap_log("Failed to feed %s to TCC", path);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/impl.c", mod_code->out_dir);
    if (feed_file_to_tcc(ctx, path) != 0){
        yap_log("Failed to feed %s to TCC", path);
        return -1;
    }

    // Add module libraries before relocating
    yap_c_build_state* state = ctx->build_state;
    {
        void* item;
        size_t iter = 0;
        while (hashmap_iter(ctx->modules, &iter, &item)) {
            yap_module* m = item;
            if (!m->lib_paths) continue;
            for_darr(li, lp, m->lib_paths) {
                yap_log("TCC: adding module lib '%s'", lp);
                if (tcc_add_file(state->tcc, lp) == -1)
                    yap_log("TCC: failed to add library '%s'", lp);
            }
        }
    }

    // Relocate the new state
    if (tcc_relocate(state->tcc) != 0){
        yap_log("TCC relocate failed during recompile");
        darr_free(ctx->errors);
        ctx->errors = darr_new(yap_error);
        return -1;
    }

    yap_log("Recompile succeeded (counter=%lu)", state->counter);
    return 0;
}

void* yap_c_ensure_symbol(yap_ctx* ctx, const char* name){
    if (!ctx || !ctx->current_module){
        yap_log("No active module for ensure_symbol");
        return NULL;
    }
    yap_module* module = ctx->current_module;

    // Try current state first (may have been relocated already with this symbol)
    void* sym = yap_c_get_symbol(ctx, name);
    if (sym) return sym;

    // Symbol not available — recompile from files, then try again
    yap_log("Symbol '%s' not in current TCC state, recompiling...", name);
    if (yap_c_recompile_from_files(ctx, module) != 0){
        yap_log("Recompile failed — cannot resolve '%s'", name);
        return NULL;
    }

    sym = yap_c_get_symbol(ctx, name);
    if (!sym)
        yap_log("Symbol '%s' still not found after recompile", name);
    return sym;
}

void yap_tcc_check_main(yap_ctx* ctx){
    if (!ctx || !ctx->current_module) return;

    yap_log("TCC-checking for 'main' symbol...");
    int rc = yap_c_recompile_from_files(ctx, ctx->current_module);
    if (rc != 0){
        yap_ctx_push_error(ctx, (yap_error){
            .kind = yap_error_no_pos,
            .msg  = strus_copy("TCC recompile failed; cannot verify main")
        });
        return;
    }

    void* sym = yap_c_get_symbol(ctx, "main");
    if (sym){
        yap_log("TCC-check: 'main' found at %p so TCC likely works!", sym);
    } else {
        yap_ctx_push_error(ctx, (yap_error){
            .kind = yap_error_no_pos,
            .msg  = strus_copy("No 'main' function found in compiled code")
        });
    }
}
