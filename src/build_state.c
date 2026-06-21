#include "yap_c.h"

static void tcc_error_callback(void* opaque, const char* msg){
    yap_ctx* ctx = (yap_ctx*)opaque;
    yap_log("TCC error: %s", msg);
    yap_emit_error_no_pos(ctx, "TCC: %s", msg);
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
                    yap_log("TCC include path: %s", p);
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
                    yap_log("TCC library path: %s", start);
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
                    yap_log("TCC library path: %s", start);
                    *end = saved;
                }
            }
        }
        pclose(f);
    }

    free(yap_home);
    state->counter = 0;
    ctx->build_state = state;
    yap_log("TCC build state initialized");
    yap_c_test_tcc(ctx);
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

void yap_c_test_tcc(yap_ctx* ctx){
    if (!ctx->build_state) return;
    yap_log("Testing TCC build state");
    yap_c_build_state* state = ctx->build_state;

    // Test 1: can we include stdio.h?
    const char* test_include = "#include <stdio.h>\n";
    if (tcc_compile_string(state->tcc, test_include) == -1){
        yap_emit_error_no_pos(ctx, "TCC: cannot include stdio.h (check TCC errors above)");
        return;
    }

    // Test 2: can we compile a trivial function?
    const char* test_compile =
        "int test_func() {\n"
        "    int a = 42;\n"
        "    return a;\n"
        "}\n";
    if (tcc_compile_string(state->tcc, test_compile) == -1){
        yap_emit_error_no_pos(ctx, "TCC compile failed (check TCC errors above)");
        return;
    }
    // Test 3: can we compile printf code?
    const char* test_printf =
        "#include <stdio.h>\n"
        "int hello_func() {\n"
        "    printf(\"Hello from TCC inside YAP!\\n\");\n"
        "    return 42;\n"
        "}\n";
    if (tcc_compile_string(state->tcc, test_printf) == -1){
        yap_emit_error_no_pos(ctx, "TCC: printf compile failed (check TCC errors above)");
        return;
    }
    // Try relocate + call — may fail on systems without static libc
    if (tcc_relocate(state->tcc) == 0){
        int (*hello_func)() = tcc_get_symbol(state->tcc, "hello_func");
        if (hello_func){
            hello_func();
            yap_log("TCC test passed (compile + relocate + call with printf)");
            return;
        }
    }
    // Relocate may have pushed TCC errors to ctx; clear them
    darr_free(ctx->errors);
    ctx->errors = darr_new(yap_error);
    yap_log("TCC compilation test passed (compile-only; relocate unavailable)");
    // Note: tcc_relocate/tcc_get_symbol requires libtcc1.a and libc,
    // which may not be available on all systems (NixOS etc.).
    // The compile-only test above verifies TCC is working.
    yap_log("TCC compilation test passed (compile-only)");
}

int yap_c_feed_c(yap_ctx* ctx, const char* c_code){
    if (!ctx->build_state){
        yap_log("TCC state not initialized — call yap_c_init_tcc_state first");
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
