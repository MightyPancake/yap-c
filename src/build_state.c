#include "yap_c.h"

static void yap_c_inject_comptime_builders(TCCState* tcc);

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
    yap_c_inject_comptime_builders(state->tcc);
    yap_log("TCC build state initialized (NOT frozen — no relocate call)");
}

/* ----------------------------------------------------------------
 *  Comptime builder functions — called from TCC at compile time
 * ---------------------------------------------------------------- */

static yap_ctx* ct_ctx = NULL;
static uint64_t ct_expansion_counter = 0;
static const char* ct_current_macro_name = NULL;

#define CT_SOURCE_STACK_MAX 32
static yap_source* ct_source_stack[CT_SOURCE_STACK_MAX];
static yap_loc ct_loc_stack[CT_SOURCE_STACK_MAX];
static int ct_source_depth = 0;

void yap_c_set_comptime_ctx(yap_ctx* ctx){
    ct_ctx = ctx;
}

void yap_c_set_macro_name(const char* name){
    ct_current_macro_name = name;
}

void yap_c_set_macro_loc(yap_source* src, yap_loc loc){
    if (ct_source_depth < CT_SOURCE_STACK_MAX){
        ct_source_stack[ct_source_depth] = src;
        ct_loc_stack[ct_source_depth] = loc;
        ct_source_depth++;
    }
}

void yap_c_pop_macro_loc(void){
    if (ct_source_depth > 0) ct_source_depth--;
}

static void* ct_alloc(size_t sz){
    if (ct_ctx) return quake_alloc(&ct_ctx->arena, sz);
    return calloc(1, sz);
}

static char* ct_strdup(const char* s){
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = ct_alloc(len);
    memcpy(copy, s, len);
    return copy;
}

static void* ct_make_int(int value){
    char* text = ct_alloc(32);
    snprintf(text, 32, "%d", value);
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_literal;
    e->literal = (yap_literal){ .kind = yap_literal_numerical, .text = text };
    e->type = ct_ctx ? ct_ctx->untyped_int_type_id : 0;
    e->is_comptime = true;
    return e;
}

static void* ct_make_float(double value){
    char* text = ct_alloc(64);
    snprintf(text, 64, "%g", value);
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_literal;
    e->literal = (yap_literal){ .kind = yap_literal_numerical, .text = text };
    e->type = ct_ctx ? ct_ctx->untyped_float_type_id : 0;
    e->is_comptime = true;
    return e;
}

static void* ct_make_string(const char* value){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_literal;
    e->literal = (yap_literal){ .kind = yap_literal_string, .text = ct_strdup(value) };
    if (ct_ctx){
        yap_type_id byte_id = yap_ctx_get_type_id_by_name(ct_ctx, "byte");
        yap_type slice_t = { .kind = yap_type_slice, .slice = { .element_type = byte_id }, .is_const = false };
        e->type = yap_ctx_insert_type_if_not_exists(ct_ctx, slice_t);
    }
    e->is_comptime = true;
    return e;
}

static void* ct_make_bool(int value){
    char* text = ct_strdup(value ? "true" : "false");
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_literal;
    e->literal = (yap_literal){ .kind = yap_literal_bool, .text = text };
    e->type = ct_ctx ? ct_ctx->bool_type_id : 0;
    e->is_comptime = true;
    return e;
}

static void* ct_make_var(const char* name){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_var;
    e->var_name = ct_strdup(name);
    e->is_lvalue = true;
    return e;
}

static void* ct_make_bin(void* left, int op, void* right){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_bin;
    yap_expr* l = ct_alloc(sizeof(yap_expr)); *l = *(yap_expr*)left;
    yap_expr* r = ct_alloc(sizeof(yap_expr)); *r = *(yap_expr*)right;
    e->bin_expr = (yap_bin_expr){ .op = op, .left = l, .right = r };
    e->is_comptime = l->is_comptime && r->is_comptime;
    if (ct_ctx)
        e->type = yap_ctx_find_common_type(ct_ctx, l->type, r->type);
    return e;
}

static void* ct_make_func_call(void* func_expr, void** args, int argc){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_func_call;
    yap_expr* f = ct_alloc(sizeof(yap_expr)); *f = *(yap_expr*)func_expr;
    darr(yap_expr) params = darr_new(yap_expr);
    for (int i = 0; i < argc; i++){
        darr_push(params, *(yap_expr*)args[i]);
    }
    e->func_call = (yap_func_call){ .func_expr = f, .params = params };
    return e;
}

static int ct_expr_kind(void* expr){
    return ((yap_expr*)expr)->kind;
}

static int ct_expr_is_comptime(void* expr){
    return ((yap_expr*)expr)->is_comptime;
}

/* ----------------------------------------------------------------
 *  Statement builders
 * ---------------------------------------------------------------- */

static void* ct_make_var_decl(const char* name, void* type_id_ptr, void* init_expr){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_var_decl;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    s->var_decl = (yap_var_decl){
        .kind = yap_var_decl_valid,
        .var = { .name = name ? ct_strdup(name) : NULL, .type = tid },
        .has_init = (init_expr != NULL),
        .init = init_expr ? *(yap_expr*)init_expr : (yap_expr){0},
    };
    return s;
}

static void* ct_make_expr_stmt(void* expr){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_expr;
    s->expr = *(yap_expr*)expr;
    return s;
}

static void* ct_make_block(void** stmts, int count){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_block;
    darr(yap_statement) statements = darr_new(yap_statement);
    for (int i = 0; i < count; i++){
        darr_push(statements, *(yap_statement*)stmts[i]);
    }
    s->block = (yap_block){ .kind = yap_block_valid, .statements = statements };
    return s;
}

static void* ct_uniq(void){
    char* name = ct_alloc(128);
    snprintf(name, 128, "__uniq_%s_%llu",
        ct_current_macro_name ? ct_current_macro_name : "anon",
        (unsigned long long)ct_expansion_counter);
    ct_expansion_counter++;
    return ct_make_var(name);
}

static const char* ct_uniq_name(void){
    char* name = ct_alloc(128);
    snprintf(name, 128, "__uniq_%s_%llu",
        ct_current_macro_name ? ct_current_macro_name : "anon",
        (unsigned long long)ct_expansion_counter);
    ct_expansion_counter++;
    return name;
}

/* ----------------------------------------------------------------
 *  Type emission builders
 * ---------------------------------------------------------------- */

typedef enum { CT_KIND_STRUCT, CT_KIND_ENUM, CT_KIND_UNION } ct_type_builder_kind;

typedef struct {
    ct_type_builder_kind kind;
    char* name;
    union {
        darr(yap_struct_field) fields;
        darr(yap_enum_variant) variants;
    };
} ct_type_builder;

static void* ct_struct_new(const char* name){
    ct_type_builder* b = ct_alloc(sizeof(ct_type_builder));
    b->kind = CT_KIND_STRUCT;
    b->name = ct_strdup(name);
    b->fields = darr_new(yap_struct_field);
    return b;
}

static void* ct_enum_new(const char* name){
    ct_type_builder* b = ct_alloc(sizeof(ct_type_builder));
    b->kind = CT_KIND_ENUM;
    b->name = ct_strdup(name);
    b->variants = darr_new(yap_enum_variant);
    return b;
}

static void* ct_union_new(const char* name){
    ct_type_builder* b = ct_alloc(sizeof(ct_type_builder));
    b->kind = CT_KIND_UNION;
    b->name = ct_strdup(name);
    b->fields = darr_new(yap_struct_field);
    return b;
}

static void* ct_struct_field(void* b_ptr, const char* field_name, void* type_id_ptr){
    ct_type_builder* b = b_ptr;
    yap_type_id type_id = (yap_type_id)(uintptr_t)type_id_ptr;
    yap_struct_field f = {
        .kind = yap_struct_field_valid,
        .name = ct_strdup(field_name),
        .type = type_id,
        .default_value = NULL,
    };
    darr_push(b->fields, f);
    return b_ptr;
}

static void* ct_enum_variant(void* b_ptr, const char* variant_name, void* value_ptr){
    ct_type_builder* b = b_ptr;
    yap_expr* val = NULL;
    if (value_ptr) val = (yap_expr*)value_ptr;
    yap_enum_variant v = {
        .name = ct_strdup(variant_name),
        .value = val,
    };
    darr_push(b->variants, v);
    return b_ptr;
}

static void* ct_union_variant(void* b_ptr, const char* variant_name, void* type_id_ptr){
    return ct_struct_field(b_ptr, variant_name, type_id_ptr);
}

typedef struct {
    void* type;
    int was_emitted;
} ct_type_emission;

static ct_type_emission ct_emit_type(void* sb_ptr){
    if (!ct_ctx) return (ct_type_emission){0};
    ct_type_builder* b = sb_ptr;

    // Build layout string for hashing
    char* layout_parts[64];
    int part_count = 0;
    size_t layout_len = 0;

    if (b->kind == CT_KIND_STRUCT || b->kind == CT_KIND_UNION){
        for_darr(i, f, b->fields){
            yap_type* ft = yap_ctx_get_type(ct_ctx, f.type);
            char* ft_str = ft ? yap_ctx_type_to_mangle_string(ct_ctx, *ft) : "?";
            char* p = ct_alloc(strlen(f.name) + strlen(ft_str) + 3);
            sprintf(p, "%s:%s,", f.name, ft_str);
            if (ft) free(ft_str);
            layout_parts[part_count++] = p;
            layout_len += strlen(p);
            if (part_count >= 64) break;
        }
    } else {
        for_darr(i, v, b->variants){
            char* p = ct_alloc(strlen(v.name) + 2);
            sprintf(p, "%s,", v.name);
            layout_parts[part_count++] = p;
            layout_len += strlen(p);
            if (part_count >= 64) break;
        }
    }

    char* layout = ct_alloc(layout_len + 1);
    layout[0] = '\0';
    for (int pi = 0; pi < part_count; pi++) strcat(layout, layout_parts[pi]);

    uint64_t hash = hashmap_murmur(layout, strlen(layout), 0, 0);
    char* c_name = ct_alloc(strlen(b->name) + 20);
    sprintf(c_name, "%s_%llx", b->name, (unsigned long long)hash);

    yap_type_id existing = yap_ctx_get_type_id_by_name(ct_ctx, c_name);
    if (existing){
        yap_log("emit_type: '%s' already exists (dedup hit, hash=%llx), id=%u",
            b->name, (unsigned long long)hash, existing);
        if (b->kind == CT_KIND_ENUM) darr_free(b->variants);
        else darr_free(b->fields);
        return (ct_type_emission){ .type = (void*)(uintptr_t)existing, .was_emitted = 0 };
    }

    yap_type t = {0};
    yap_named_type_decl_kind decl_kind;

    if (b->kind == CT_KIND_STRUCT){
        darr(yap_struct_field) af = yap_ctx_darr_new(ct_ctx, yap_struct_field, .cap=darr_len(b->fields), .len=0);
        for_darr(i, f, b->fields) darr_push(af, f);
        darr_free(b->fields);
        t.kind = yap_type_struct;
        t.structure.fields = af; t.structure.c_name = c_name; t.structure.name = c_name;
        decl_kind = yap_named_type_decl_struct;
    } else if (b->kind == CT_KIND_UNION){
        darr(yap_struct_field) af = yap_ctx_darr_new(ct_ctx, yap_struct_field, .cap=darr_len(b->fields), .len=0);
        for_darr(i, f, b->fields) darr_push(af, f);
        darr_free(b->fields);
        t.kind = yap_type_union;
        t.uni.variants = af; t.uni.c_name = c_name; t.uni.name = c_name;
        decl_kind = yap_named_type_decl_union;
    } else {
        darr(yap_enum_variant) av = yap_ctx_darr_new(ct_ctx, yap_enum_variant, .cap=darr_len(b->variants), .len=0);
        for_darr(i, v, b->variants) darr_push(av, v);
        darr_free(b->variants);
        t.kind = yap_type_enum;
        t.enumeration.variants = av; t.enumeration.c_name = c_name; t.enumeration.name = c_name;
        decl_kind = yap_named_type_decl_enum;
    }

    yap_type_id id = yap_ctx_push_named_type(ct_ctx, c_name, c_name, t);

    yap_decl decl = {
        .kind = yap_decl_named_type,
        .named_type_decl = { .name = c_name, .c_name = c_name, .kind = decl_kind, .type_id = id },
    };
    if (ct_ctx->gen_decl)
        ct_ctx->gen_decl(ct_ctx, decl);

    yap_log("emit_type: emitted '%s' as '%s' (hash=%llx), id=%u",
        b->name, c_name, (unsigned long long)hash, id);
    return (ct_type_emission){ .type = (void*)(uintptr_t)id, .was_emitted = 1 };
}

static void* ct_type_id(const char* name){
    if (!ct_ctx) return NULL;
    return (void*)(uintptr_t)yap_ctx_get_type_id_by_name(ct_ctx, (char*)name);
}

/* ----------------------------------------------------------------
 *  Phase 8: Compiler state queries
 * ---------------------------------------------------------------- */

static int ct_type_exists(const char* name){
    if (!ct_ctx) return 0;
    return yap_ctx_get_type_id_by_name(ct_ctx, (char*)name) != 0;
}

static int ct_func_exists(const char* name){
    if (!ct_ctx) return 0;
    const yap_var* var = yap_scope_get_var_recursive(ct_ctx->global_scope, (char*)name);
    if (!var) return 0;
    yap_type* t = yap_ctx_get_type(ct_ctx, var->type);
    return t && t->kind == yap_type_func;
}

/* ----------------------------------------------------------------
 *  Phase 9: Introspection and debugging
 * ---------------------------------------------------------------- */

static void ct_log(const char* msg){
    if (msg) fprintf(stderr, "%s\n", msg);
}

static void ct_error(const char* msg){
    if (!ct_ctx || !msg) return;
    if (ct_source_depth > 0){
        yap_source* src = ct_source_stack[ct_source_depth - 1];
        yap_loc loc = ct_loc_stack[ct_source_depth - 1];
        loc.src = src;
        yap_ctx_push_error(ct_ctx, (yap_error){
            .kind = yap_error_pos,
            .src = src,
            .loc = loc,
            .msg = strus_copy((char*)msg),
        });
    } else {
        yap_emit_error_no_pos(ct_ctx, "comptime error: %s", msg);
    }
}

static void ct_warn(const char* msg){
    if (msg) fprintf(stderr, "[WARNING] %s\n", msg);
}

const char* ct_builder_decls =
    "#ifndef __YAP_TYPE_EMISSION_DEFINED\n"
    "#define __YAP_TYPE_EMISSION_DEFINED\n"
    "typedef struct yTypeEmission { void* type; int was_emitted; } yTypeEmission;\n"
    "#endif\n"
    "#ifdef __TINYC__\n"
    "extern void* yapi_int(int value);\n"
    "extern void* yapi_float(double value);\n"
    "extern void* yapi_string(const char* value);\n"
    "extern void* yapi_bool(int value);\n"
    "extern void* yapi_var(const char* ident);\n"
    "extern void* yapi_bin(void* left, int op, void* right);\n"
    "extern void* yapi_call(void* func, void** args, int argc);\n"
    "extern int yapi_kind(void* expr);\n"
    "extern int yapi_is_comptime(void* expr);\n"
    "extern void* yapi_var_decl(const char* name, void* type_id, void* init);\n"
    "extern void* yapi_expr_stmt(void* expr);\n"
    "extern void* yapi_block(void** stmts, int count);\n"
    "extern void* yapi_uniq(void);\n"
    "extern const char* yapi_uniq_name(void);\n"  /* returns yIdent */
    "extern void* yapi_struct_new(const char* name);\n"
    "extern void* yapi_struct_field(void* sb, const char* name, void* type_id);\n"
    "extern void* yapi_enum_new(const char* name);\n"
    "extern void* yapi_enum_variant(void* eb, const char* name, void* value);\n"
    "extern void* yapi_union_new(const char* name);\n"
    "extern void* yapi_union_variant(void* ub, const char* name, void* type_id);\n"
    "extern yTypeEmission yapi_emit_type(void* sb);\n"
    "extern void* yapi_type_id(const char* name);\n"
    "extern int yapi_type_exists(const char* name);\n"
    "extern int yapi_func_exists(const char* name);\n"
    "extern void yapi_log(const char* msg);\n"
    "extern void yapi_error(const char* msg);\n"
    "extern void yapi_warn(const char* msg);\n"
    "#else\n"
    "static inline void* yapi_int(int v){(void)v;return 0;}\n"
    "static inline void* yapi_float(double v){(void)v;return 0;}\n"
    "static inline void* yapi_string(const char* v){(void)v;return 0;}\n"
    "static inline void* yapi_bool(int v){(void)v;return 0;}\n"
    "static inline void* yapi_var(const char* v){(void)v;return 0;}\n"
    "static inline void* yapi_bin(void* l,int o,void* r){(void)l;(void)o;(void)r;return 0;}\n"
    "static inline void* yapi_call(void* f,void** a,int c){(void)f;(void)a;(void)c;return 0;}\n"
    "static inline int yapi_kind(void* e){(void)e;return 0;}\n"
    "static inline int yapi_is_comptime(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_var_decl(const char* n,void* t,void* i){(void)n;(void)t;(void)i;return 0;}\n"
    "static inline void* yapi_expr_stmt(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_block(void** s,int c){(void)s;(void)c;return 0;}\n"
    "static inline void* yapi_uniq(void){return 0;}\n"
    "static inline const char* yapi_uniq_name(void){return \"\";}\n"
    "static inline void* yapi_struct_new(const char* n){(void)n;return 0;}\n"
    "static inline void* yapi_struct_field(void* s,const char* n,void* t){(void)s;(void)n;(void)t;return 0;}\n"
    "static inline void* yapi_enum_new(const char* n){(void)n;return 0;}\n"
    "static inline void* yapi_enum_variant(void* e,const char* n,void* v){(void)e;(void)n;(void)v;return 0;}\n"
    "static inline void* yapi_union_new(const char* n){(void)n;return 0;}\n"
    "static inline void* yapi_union_variant(void* u,const char* n,void* t){(void)u;(void)n;(void)t;return 0;}\n"
    "static inline yTypeEmission yapi_emit_type(void* s){(void)s;return (yTypeEmission){0};}\n"
    "static inline void* yapi_type_id(const char* n){(void)n;return 0;}\n"
    "static inline int yapi_type_exists(const char* n){(void)n;return 0;}\n"
    "static inline int yapi_func_exists(const char* n){(void)n;return 0;}\n"
    "static inline void yapi_log(const char* m){(void)m;}\n"
    "static inline void yapi_error(const char* m){(void)m;}\n"
    "static inline void yapi_warn(const char* m){(void)m;}\n"
    "#endif\n";

static void yap_c_inject_comptime_builders(TCCState* tcc){
    tcc_add_symbol(tcc, "yapi_int",         ct_make_int);
    tcc_add_symbol(tcc, "yapi_float",       ct_make_float);
    tcc_add_symbol(tcc, "yapi_string",      ct_make_string);
    tcc_add_symbol(tcc, "yapi_bool",        ct_make_bool);
    tcc_add_symbol(tcc, "yapi_var",         ct_make_var);
    tcc_add_symbol(tcc, "yapi_bin",         ct_make_bin);
    tcc_add_symbol(tcc, "yapi_call",        ct_make_func_call);
    tcc_add_symbol(tcc, "yapi_kind",         ct_expr_kind);
    tcc_add_symbol(tcc, "yapi_is_comptime",  ct_expr_is_comptime);
    tcc_add_symbol(tcc, "yapi_var_decl",       ct_make_var_decl);
    tcc_add_symbol(tcc, "yapi_expr_stmt",      ct_make_expr_stmt);
    tcc_add_symbol(tcc, "yapi_block",          ct_make_block);
    tcc_add_symbol(tcc, "yapi_uniq",           ct_uniq);
    tcc_add_symbol(tcc, "yapi_uniq_name",      ct_uniq_name);
    tcc_add_symbol(tcc, "yapi_struct_new",     ct_struct_new);
    tcc_add_symbol(tcc, "yapi_struct_field",  ct_struct_field);
    tcc_add_symbol(tcc, "yapi_enum_new",      ct_enum_new);
    tcc_add_symbol(tcc, "yapi_enum_variant",  ct_enum_variant);
    tcc_add_symbol(tcc, "yapi_union_new",     ct_union_new);
    tcc_add_symbol(tcc, "yapi_union_variant",  ct_union_variant);
    tcc_add_symbol(tcc, "yapi_emit_type",     ct_emit_type);
    tcc_add_symbol(tcc, "yapi_type_id",       ct_type_id);
    tcc_add_symbol(tcc, "yapi_type_exists",  ct_type_exists);
    tcc_add_symbol(tcc, "yapi_func_exists",  ct_func_exists);
    tcc_add_symbol(tcc, "yapi_log",          ct_log);
    tcc_add_symbol(tcc, "yapi_error",        ct_error);
    tcc_add_symbol(tcc, "yapi_warn",         ct_warn);
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
