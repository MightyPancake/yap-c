#include "yap_c.h"

static void yap_c_inject_comptime_builders(TCCState* tcc);
static void ct_error(const char* msg); // defined below (introspection); used earlier by builder templates

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

/* yapi->var_value(name): reference to an already-in-scope var, read-only
 * (yapi->new_var/get_subject/add_param use ct_make_var directly instead, which
 * is lvalue-tagged, since those introduce a var that's meant to be fully usable). */
static void* ct_var_value(const char* name){
    yap_expr* e = ct_make_var(name);
    ((yap_expr*)e)->is_lvalue = false;
    return e;
}

static void* ct_make_new_var(void* type_id_ptr, const char* name){
    yap_expr* e = ct_make_var(name);
    ((yap_expr*)e)->type = (yap_type_id)(uintptr_t)type_id_ptr;
    return e;
}

/* yapi->assign(lval, op, rval): op is a char code, same domain as bin_op's op
 * ('+', '-', ... or '=' for plain assignment) -- not a cstring, since every
 * compound assignment in this language is mechanically "<binop-char>=", so a
 * single byte literal is enough to express all of them (e.g. '+' means
 * "+="), matching yapi.md's intent (a dedicated op type) without needing a
 * real yAssignOp enum type or string allocation from macro-author code. */
static void* ct_make_assign(void* lval, int op, void* rval){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_assignment;
    yap_expr* l = ct_alloc(sizeof(yap_expr)); *l = *(yap_expr*)lval;
    yap_expr* r = ct_alloc(sizeof(yap_expr)); *r = *(yap_expr*)rval;
    e->assignment = (yap_assignment){ .kind = yap_assignment_valid, .left = l, .right = r };
    if (op == '='){
        e->assignment.op[0] = '='; e->assignment.op[1] = '\0';
    } else {
        e->assignment.op[0] = (char)op; e->assignment.op[1] = '='; e->assignment.op[2] = '\0';
    }
    e->type = l->type;
    return e;
}

static void* ct_make_member(void* obj, const char* field){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_member_access;
    yap_expr* o = ct_alloc(sizeof(yap_expr)); *o = *(yap_expr*)obj;
    e->member_access = (yap_member_access){ .object = o, .member = ct_strdup(field) };
    e->is_lvalue = o->is_lvalue;
    e->is_comptime = o->is_comptime;
    return e;
}

/* yapi->index(obj, idx): array/slice/pointer indexing (obj:[idx] in surface
 * syntax) -- needed to build real array types (pointer-backed data + at()). */
static void* ct_make_index(void* obj_ptr, void* idx_ptr){
    yap_expr* obj = (yap_expr*)obj_ptr;
    yap_type_id element_type = 0;
    if (ct_ctx){
        yap_type* obj_type = yap_ctx_get_type(ct_ctx, obj->type);
        if (obj_type){
            if (obj_type->kind == yap_type_array) element_type = obj_type->array.element_type;
            else if (obj_type->kind == yap_type_slice) element_type = obj_type->slice.element_type;
            else if (obj_type->kind == yap_type_ptr) element_type = obj_type->pointer_type;
        }
    }
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_index_access;
    yap_expr* o = ct_alloc(sizeof(yap_expr)); *o = *obj;
    yap_expr* i = ct_alloc(sizeof(yap_expr)); *i = *(yap_expr*)idx_ptr;
    e->index_access = (yap_index_access){ .object = o, .index = i };
    e->type = element_type;
    e->is_lvalue = true;
    return e;
}

static void* ct_make_cast(void* expr_ptr, void* type_id_ptr){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_cast;
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = (yap_type_id)(uintptr_t)type_id_ptr;
    e->is_lvalue = src->is_lvalue;
    e->is_comptime = src->is_comptime;
    return e;
}

/* yapi->deref(ptr): unchecked pointer dereference (surface syntax 'ptr.'),
 * needed to reach fields through a pointer-receiver method's 'self'. */
static void* ct_make_deref(void* expr_ptr){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_type_id pointee = 0;
    if (ct_ctx){
        yap_type* t = yap_ctx_get_type(ct_ctx, src->type);
        if (t && t->kind == yap_type_ptr) pointee = t->pointer_type;
    }
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_deref;
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = pointee;
    e->is_lvalue = true;
    return e;
}

/* yapi->addr_of(expr): address-of (surface syntax 'expr@'). */
static void* ct_make_addr_of(void* expr_ptr){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_at_op;
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = ct_ctx ? yap_ctx_get_pointer_of_type_id(ct_ctx, src->type) : 0;
    e->is_lvalue = false;
    return e;
}

static void* ct_ptr_of(void* type_id_ptr){
    if (!ct_ctx) return NULL;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    return (void*)(uintptr_t)yap_ctx_get_pointer_of_type_id(ct_ctx, tid);
}

// yapi->func_typeN(ret, p1..pN): builds/dedups a function type id for precisely-typed callback params
static void* ct_fn_type_n(void* ret_ptr, unsigned int argc, void** params){
    if (!ct_ctx) return NULL;
    darr(yap_type_id) args = yap_ctx_darr_new(ct_ctx, yap_type_id, .cap=argc, .len=0);
    for (unsigned int i = 0; i < argc; i++)
        darr_push(args, (yap_type_id)(uintptr_t)params[i]);
    yap_type t = {
        .kind = yap_type_func,
        .func = { .args = args, .return_type = (yap_type_id)(uintptr_t)ret_ptr },
        .is_const = false
    };
    return (void*)(uintptr_t)yap_ctx_insert_type_if_not_exists(ct_ctx, t);
}
static void* ct_fn_type0(void* ret){ return ct_fn_type_n(ret, 0, NULL); }
static void* ct_fn_type1(void* ret, void* p1){ void* ps[1] = {p1}; return ct_fn_type_n(ret, 1, ps); }
static void* ct_fn_type2(void* ret, void* p1, void* p2){ void* ps[2] = {p1, p2}; return ct_fn_type_n(ret, 2, ps); }
static void* ct_fn_type3(void* ret, void* p1, void* p2, void* p3){ void* ps[3] = {p1, p2, p3}; return ct_fn_type_n(ret, 3, ps); }

/* yapi->sizeof(T): no dedicated AST node for a C sizeof expression, so this
 * builds a numeric-literal expr whose text is literally "sizeof(<c type>)" --
 * yap_gen_literal prints numeric literal text verbatim (build_state.c reuses
 * that codegen path as-is rather than adding a new expr kind for this). */
static void* ct_sizeof(void* type_id_ptr){
    if (!ct_ctx) return NULL;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    yap_loc loc = {0};
    yap_strbuf tstr = yap_gen_type_id(ct_ctx, loc, tid);
    char* text = ct_alloc(strlen(yap_strbuf_data(&tstr)) + 16);
    sprintf(text, "sizeof(%s)", yap_strbuf_data(&tstr));
    yap_strbuf_free(&tstr);
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_literal;
    e->literal = (yap_literal){ .kind = yap_literal_numerical, .text = text };
    e->type = ct_ctx->untyped_int_type_id;
    return e;
}

/* Comparison/relational ops yield bool regardless of operand types; arithmetic
 * ops yield the common type of their operands. */
static bool ct_is_cmp_op(int op){
    return op == yap_bin_expr_eq  || op == yap_bin_expr_neq
        || op == yap_bin_expr_lt  || op == yap_bin_expr_gt
        || op == yap_bin_expr_le  || op == yap_bin_expr_ge;
}

static yap_type_id ct_bin_result_type(int op, yap_expr* l, yap_expr* r){
    if (!ct_ctx) return 0;
    if (ct_is_cmp_op(op)) return ct_ctx->bool_type_id;
    return yap_ctx_find_common_type(ct_ctx, l->type, r->type);
}

static void* ct_make_bin(void* left, int op, void* right){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_bin;
    yap_expr* l = ct_alloc(sizeof(yap_expr)); *l = *(yap_expr*)left;
    yap_expr* r = ct_alloc(sizeof(yap_expr)); *r = *(yap_expr*)right;
    e->bin_expr = (yap_bin_expr){ .op = op, .left = l, .right = r };
    e->is_comptime = l->is_comptime && r->is_comptime;
    e->type = ct_bin_result_type(op, l, r);
    return e;
}

/* yapi->neg(e): prefix unary minus. */
static void* ct_make_neg(void* expr_ptr){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_unary;
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = src->type;
    e->is_comptime = src->is_comptime;
    return e;
}

/* yapi->ternary(cond, then, else): a ? b : c. Result type is the common type of
 * the two branches. */
static void* ct_make_ternary(void* cond, void* then_e, void* else_e){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_ternary;
    yap_expr* c = ct_alloc(sizeof(yap_expr)); *c = *(yap_expr*)cond;
    yap_expr* t = ct_alloc(sizeof(yap_expr)); *t = *(yap_expr*)then_e;
    yap_expr* f = ct_alloc(sizeof(yap_expr)); *f = *(yap_expr*)else_e;
    e->ternary = (yap_ternary_expr){ .condition = c, .then_expr = t, .else_expr = f };
    e->is_comptime = c->is_comptime && t->is_comptime && f->is_comptime;
    if (ct_ctx) e->type = yap_ctx_find_common_type(ct_ctx, t->type, f->type);
    return e;
}

static void* ct_make_func_call(void* func_expr, void* args_list){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_func_call;
    yap_expr* f = ct_alloc(sizeof(yap_expr)); *f = *(yap_expr*)func_expr;
    yap_expr_list* al = (yap_expr_list*)args_list;
    unsigned int argc = al ? al->count : 0;
    void* src = al ? al->items : NULL;
    /* Pre-sized to argc and filled via .src in one shot (no darr_push) so
     * this never grows — darr_push always realloc()s, which would be unsafe
     * to mix with arena-backed (quake_alloc) storage. */
    darr(yap_expr) params = ct_ctx
        ? yap_ctx_darr_new(ct_ctx, yap_expr, .cap=argc, .len=argc, .src=src)
        : darr_new(yap_expr, .cap=argc, .len=argc, .src=src);
    e->func_call = (yap_func_call){ .func_expr = f, .params = params };
    return e;
}

/* yapi->call0..call3: fixed-arity call-expression builders (direct args, no
 * list-building needed) -- covers every real call site (print()'s dispatch
 * calls and arr.yap's own calls into its C backend are all <=2 args), so
 * there's no macro-author-facing list-building API at all; ct_make_func_call
 * above (which still takes a yap_expr_list*) stays as the shared internal
 * implementation these funnel into with a stack-built list. */
static void* ct_call_n(void* func_expr, unsigned int argc, yap_expr** args){
    yap_expr_list list = {0};
    list.items = ct_alloc(sizeof(yap_expr) * (argc ? argc : 1));
    list.count = argc;
    list.cap   = argc;
    for (unsigned int i = 0; i < argc; i++) list.items[i] = *args[i];
    return ct_make_func_call(func_expr, &list);
}

static void* ct_call0(void* func_expr){
    return ct_call_n(func_expr, 0, NULL);
}

static void* ct_call1(void* func_expr, void* a){
    yap_expr* args[1] = { a };
    return ct_call_n(func_expr, 1, args);
}

static void* ct_call2(void* func_expr, void* a, void* b){
    yap_expr* args[2] = { a, b };
    return ct_call_n(func_expr, 2, args);
}

static void* ct_call3(void* func_expr, void* a, void* b, void* c){
    yap_expr* args[3] = { a, b, c };
    return ct_call_n(func_expr, 3, args);
}

/* ----------------------------------------------------------------
 *  Comptime handle lists — backing yStmtList, the macro-side vehicle for
 *  building a growing, unbounded number of yStmt values (needed for
 *  building a function body one statement at a time; yExprList's macro-
 *  author-facing builders were removed above in favor of call0..call3 since
 *  every real call site only ever needed a small fixed number of args).
 * ---------------------------------------------------------------- */

static void* ct_stmt_list_new(void){
    yap_stmt_list* l = ct_alloc(sizeof(yap_stmt_list));
    *l = (yap_stmt_list){0};
    return l;
}

static void* ct_stmt_list_push(void* list, void* stmt){
    yap_stmt_list* l = (yap_stmt_list*)list;
    if (!l || !stmt) return l;
    if (l->count >= l->cap){
        unsigned int newcap = l->cap ? l->cap * 2 : 4;
        yap_statement* items = ct_alloc(sizeof(yap_statement) * newcap);
        if (l->items) memcpy(items, l->items, sizeof(yap_statement) * l->count);
        l->items = items;
        l->cap = newcap;
    }
    l->items[l->count++] = *(yap_statement*)stmt;
    return l;
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

/* yapi->var_decl(type, name): a bare declaration, no initializer -- composing a
 * declare-then-init is now two statements: var_decl(...) followed by
 * expr_statement(assign(new_var(...), "=", value)). */
static void* ct_make_var_decl(void* type_id_ptr, const char* name){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_var_decl;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    s->var_decl = (yap_var_decl){
        .kind = yap_var_decl_valid,
        .var = { .name = name ? ct_strdup(name) : NULL, .type = tid },
        .has_init = false,
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

static void* ct_make_return_stmt(void* expr){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_return;
    s->return_stmt = (yap_return_statement){ .value = *(yap_expr*)expr };
    return s;
}

/* yapi->if_stmt(cond, then) / yapi->if_else_stmt(cond, then, else): mirror the
 * two real if-statement AST kinds (yap_statement_if / yap_statement_if_else)
 * rather than a single builder with an optional/nullable else branch. */
static void* ct_make_if_stmt(void* cond, void* then_stmt){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_if;
    yap_statement* then_cpy = ct_alloc(sizeof(yap_statement)); *then_cpy = *(yap_statement*)then_stmt;
    s->if_stmt = (yap_if){ .condition = *(yap_expr*)cond, .then_branch = then_cpy };
    return s;
}

static void* ct_make_if_else_stmt(void* cond, void* then_stmt, void* else_stmt){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_if_else;
    yap_statement* then_cpy = ct_alloc(sizeof(yap_statement)); *then_cpy = *(yap_statement*)then_stmt;
    yap_statement* else_cpy = ct_alloc(sizeof(yap_statement)); *else_cpy = *(yap_statement*)else_stmt;
    s->if_else_stmt = (yap_if_else){ .condition = *(yap_expr*)cond, .then_branch = then_cpy, .else_branch = else_cpy };
    return s;
}

static void* ct_make_while_stmt(void* cond, void* body_stmt){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_while;
    yap_statement* body_cpy = ct_alloc(sizeof(yap_statement)); *body_cpy = *(yap_statement*)body_stmt;
    s->while_stmt = (yap_while){ .condition = *(yap_expr*)cond, .body = body_cpy };
    return s;
}

static void* ct_make_block(void* stmts_list){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_block;
    yap_stmt_list* sl = (yap_stmt_list*)stmts_list;
    unsigned int count = sl ? sl->count : 0;
    void* src = sl ? sl->items : NULL;
    darr(yap_statement) statements = ct_ctx
        ? yap_ctx_darr_new(ct_ctx, yap_statement, .cap=count, .len=count, .src=src)
        : darr_new(yap_statement, .cap=count, .len=count, .src=src);
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
 *  Type template builders (yapi.md): yStructT / yEnumT / yUnionT
 *
 *  Incremental templates: struct_t()/enum_t()/union_t() create an empty
 *  builder (no name yet); add_field()/add_variant() fill it; finish(name)
 *  locks it, hashes its layout, dedups against an existing same-name-and-
 *  hash type, and emits it if new. existed()/type() read back the result.
 * ---------------------------------------------------------------- */

typedef enum { CT_KIND_STRUCT, CT_KIND_ENUM, CT_KIND_UNION } ct_type_builder_kind;

typedef struct {
    ct_type_builder_kind kind;
    bool locked;
    bool was_existed;
    yap_type_id result_id;
    union {
        darr(yap_struct_field) fields;   // struct/union
        darr(yap_enum_variant) variants; // enum
    };
} ct_type_builder;

static void* ct_struct_new(void){
    ct_type_builder* b = ct_alloc(sizeof(ct_type_builder));
    *b = (ct_type_builder){0};
    b->kind = CT_KIND_STRUCT;
    b->fields = darr_new(yap_struct_field);
    return b;
}

static void* ct_enum_new(void){
    ct_type_builder* b = ct_alloc(sizeof(ct_type_builder));
    *b = (ct_type_builder){0};
    b->kind = CT_KIND_ENUM;
    b->variants = darr_new(yap_enum_variant);
    return b;
}

static void* ct_union_new(void){
    ct_type_builder* b = ct_alloc(sizeof(ct_type_builder));
    *b = (ct_type_builder){0};
    b->kind = CT_KIND_UNION;
    b->fields = darr_new(yap_struct_field);
    return b;
}

// Returns the builder itself (self) so type${ } / hand-written code can chain
// st:add_field(...):add_field(...); the return is ignored when called as a stmt.
static void* ct_struct_add_field(void* b_ptr, void* type_id_ptr, const char* field_name){
    ct_type_builder* b = b_ptr;
    if (!b) return b_ptr;
    if (b->locked){ ct_error("Cannot add a field to a type template after finish()"); return b_ptr; }
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

static void* ct_enum_add_variant(void* b_ptr, const char* variant_name){
    ct_type_builder* b = b_ptr;
    if (!b) return b_ptr;
    if (b->locked){ ct_error("Cannot add a variant to a type template after finish()"); return b_ptr; }
    yap_enum_variant v = { .name = ct_strdup(variant_name), .value = NULL };
    darr_push(b->variants, v);
    return b_ptr;
}

static void* ct_type_finish(void* b_ptr, const char* name){
    if (!ct_ctx || !b_ptr || !name) return NULL;
    ct_type_builder* b = b_ptr;
    if (b->locked) return (void*)(uintptr_t)b->result_id; // idempotent re-finish

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
    char* c_name = ct_alloc(strlen(name) + 20);
    sprintf(c_name, "%s_%llx", name, (unsigned long long)hash);

    yap_type_id existing = yap_ctx_get_type_id_by_name(ct_ctx, c_name);
    if (existing){
        yap_log("finish: '%s' already exists (dedup hit, hash=%llx), id=%u",
            name, (unsigned long long)hash, existing);
        if (b->kind == CT_KIND_ENUM) darr_free(b->variants);
        else darr_free(b->fields);
        b->locked = true;
        b->was_existed = true;
        b->result_id = existing;
        return (void*)(uintptr_t)existing;
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
        /* Explicit empty prefix: c_name is already fully resolved (name_hash),
         * and gen_decl/codegen falls back to ctx->current_module->prefix when
         * module_prefix is NULL -- current_module reflects whatever module was
         * last switched to during import processing, not where this macro is
         * being *invoked* from, so leaving it NULL risks a stale/wrong prefix
         * getting silently re-applied on top of an already-correct name. */
        .module_prefix = "",
    };
    if (ct_ctx->gen_decl)
        ct_ctx->gen_decl(ct_ctx, decl);

    yap_log("finish: emitted '%s' as '%s' (hash=%llx), id=%u",
        name, c_name, (unsigned long long)hash, id);
    b->locked = true;
    b->was_existed = false;
    b->result_id = id;
    return (void*)(uintptr_t)id;
}

static int ct_type_existed(void* b_ptr){
    ct_type_builder* b = b_ptr;
    if (!b || !b->locked){ ct_error("existed() called before finish()"); return 0; }
    return b->was_existed ? 1 : 0;
}

static void* ct_type_type(void* b_ptr){
    ct_type_builder* b = b_ptr;
    if (!b || !b->locked){ ct_error("type() called before finish()"); return NULL; }
    return (void*)(uintptr_t)b->result_id;
}

/* ----------------------------------------------------------------
 *  Func template builder (yapi.md): yFnT
 *
 *  fn_t() (plain function) / yType:new_method() (method, subject
 *  auto-injected as first param under the fixed internal name "self",
 *  reachable only via get_subject()) both build the same template.
 *  finish(name) hashes the *generated C code* (signature + yap_gen_block'd
 *  body) for dedup -- reusing real codegen instead of a bespoke AST
 *  serializer -- then emits a top-level function exactly like a normal
 *  'fn' declaration would (mangled "name_hash", methods further mangled
 *  "SubjectType_name" the same way user-declared methods are, since
 *  new_method()'s subject param makes this indistinguishable from one at
 *  the call site).
 * ---------------------------------------------------------------- */

typedef struct {
    bool is_method;
    yap_type_id subject_type_id; // the *non-pointer* subject type, valid if is_method --
                                  // used for owner-name mangling regardless of whether
                                  // the actual first param ended up pointer-typed (ref method)
    darr(yap_func_arg) params;   // subject pre-pushed here if is_method
    yap_type_id return_type;
    yap_statement body;
    bool has_body;
    bool locked;
    bool was_existed;
    char* result_name;           // emit_name after finish; doubles as the yFn handle
} ct_func_builder;

static void* ct_fn_t(void){
    ct_func_builder* b = ct_alloc(sizeof(ct_func_builder));
    *b = (ct_func_builder){0};
    b->params = darr_new(yap_func_arg);
    b->return_type = ct_ctx ? ct_ctx->void_type_id : 0;
    return b;
}

static void* ct_new_method(void* type_id_ptr){
    ct_func_builder* b = ct_fn_t();
    b->is_method = true;
    yap_type_id subj = (yap_type_id)(uintptr_t)type_id_ptr;
    b->subject_type_id = subj;
    yap_func_arg self_arg = {
        .kind = yap_func_arg_valid,
        .name = "self",
        .type = subj,
        .default_value = (yap_expr){0},
    };
    darr_push(b->params, self_arg);
    return b;
}

/* Like ct_new_method, but the subject is auto-injected as 'T@' (a pointer to
 * the subject type) rather than 'T' by value -- for methods that need to
 * mutate the caller's instance in place (e.g. a growable array's push()).
 * The call site (yap_build_method_callee / yap_build_func_call_expr in
 * build.c) auto-takes-the-address of an lvalue receiver to match, so this is
 * still called as an ordinary 'recv:name(args)' -- no '&' needed by the
 * macro's callers. get_subject() still returns 'self' directly (now typed
 * T@); dereference it with yapi->deref() to reach fields. */
static void* ct_new_ref_method(void* type_id_ptr){
    ct_func_builder* b = ct_fn_t();
    b->is_method = true;
    yap_type_id subj = (yap_type_id)(uintptr_t)type_id_ptr;
    b->subject_type_id = subj;
    yap_type_id subj_ptr = ct_ctx ? yap_ctx_get_pointer_of_type_id(ct_ctx, subj) : 0;
    yap_func_arg self_arg = {
        .kind = yap_func_arg_valid,
        .name = "self",
        .type = subj_ptr,
        .default_value = (yap_expr){0},
    };
    darr_push(b->params, self_arg);
    return b;
}

static void* ct_func_add_param(void* b_ptr, void* type_id_ptr, const char* name){
    ct_func_builder* b = b_ptr;
    if (!b) return NULL;
    if (b->locked){ ct_error("Cannot add a param to a func template after finish()"); return NULL; }
    char* pname = ct_strdup(name);
    yap_func_arg arg = {
        .kind = yap_func_arg_valid,
        .name = pname,
        .type = (yap_type_id)(uintptr_t)type_id_ptr,
        .default_value = (yap_expr){0},
    };
    darr_push(b->params, arg);
    return ct_make_var(pname);
}

static void ct_func_set_return_type(void* b_ptr, void* type_id_ptr){
    ct_func_builder* b = b_ptr;
    if (!b) return;
    if (b->locked){ ct_error("Cannot set the return type of a func template after finish()"); return; }
    b->return_type = (yap_type_id)(uintptr_t)type_id_ptr;
}

static void ct_func_set_body(void* b_ptr, void* stmt_ptr){
    ct_func_builder* b = b_ptr;
    if (!b || !stmt_ptr) return;
    if (b->locked){ ct_error("Cannot set the body of a func template after finish()"); return; }
    b->body = *(yap_statement*)stmt_ptr;
    b->has_body = true;
}

static void* ct_func_get_subject(void* b_ptr){
    ct_func_builder* b = b_ptr;
    if (!b || !b->is_method){ ct_error("get_subject() called on a non-method func template"); return NULL; }
    return ct_make_var("self");
}

/* Named struct/union/enum types (and, for our builtin comptime types like
 * yStructT itself, primitives) are the only eligible method subjects -- same
 * rule as yap_named_type_owner_name in build.c, duplicated here since that's
 * static in a different shared library. */
static const char* ct_owner_type_name(yap_type_id tid){
    if (!ct_ctx) return NULL;
    yap_type* t = yap_ctx_get_type(ct_ctx, tid);
    if (!t) return NULL;
    if (t->kind == yap_type_struct) return t->structure.name;
    if (t->kind == yap_type_union) return t->uni.name;
    if (t->kind == yap_type_enum) return t->enumeration.name;
    if (t->kind == yap_type_primitive) return t->primitive.name;
    return NULL;
}

static void* ct_func_finish(void* b_ptr, const char* name){
    if (!ct_ctx || !b_ptr || !name) return NULL;
    ct_func_builder* b = b_ptr;
    if (b->locked) return b->result_name; // idempotent re-finish

    yap_block body_block;
    if (b->has_body && b->body.kind == yap_statement_block){
        body_block = b->body.block;
    } else if (b->has_body){
        darr(yap_statement) stmts = yap_ctx_darr_new(ct_ctx, yap_statement, .cap=1, .len=1, .src=&b->body);
        body_block = (yap_block){ .kind = yap_block_valid, .statements = stmts };
    } else {
        body_block = (yap_block){ .kind = yap_block_valid, .statements = yap_ctx_darr_new(ct_ctx, yap_statement, .cap=0, .len=0) };
    }

    darr(yap_func_arg) af = yap_ctx_darr_new(ct_ctx, yap_func_arg, .cap=darr_len(b->params), .len=0);
    for_darr(i, p, b->params) darr_push(af, p);
    darr_free(b->params);

    char* emit_name;
    if (b->is_method){
        /* yapi.md: "Running finish("hello") on a method results in a mangled
         * name <mangled_subject_type> + "_" + "hello"" -- no hash, exactly the
         * "%s_%s" convention yap_func_decl_emit_name uses for a user-declared
         * 'subj_type subj_name:name(...)' method, so it's reachable via the
         * same 'recv:name(args)' dispatch (yap_build_method_callee). Dedup for
         * methods rides on the *owner type's* finish()/existed() gate (see the
         * arr()/at() example in yapi.md), not a body hash here. */
        const char* owner = ct_owner_type_name(b->subject_type_id);
        if (!owner) owner = "?";
        emit_name = ct_alloc(strlen(owner) + strlen(name) + 2);
        sprintf(emit_name, "%s_%s", owner, name);
    } else {
        // Plain fn_t(): hash the generated C code (signature + body), same
        // reuse-codegen approach as ct_type_finish reuses field layout --
        // "hashes the func code" per yapi.md.
        yap_strbuf hash_buf = yap_strbuf_new();
        yap_type* rt = yap_ctx_get_type(ct_ctx, b->return_type);
        char* rt_str = rt ? yap_ctx_type_to_mangle_string(ct_ctx, *rt) : "?";
        yap_strbuf_appendf(&hash_buf, "%s(", rt_str);
        if (rt) free(rt_str);
        for_darr(i, p, af){
            yap_type* pt = yap_ctx_get_type(ct_ctx, p.type);
            char* pt_str = pt ? yap_ctx_type_to_mangle_string(ct_ctx, *pt) : "?";
            yap_strbuf_appendf(&hash_buf, "%s,", pt_str);
            if (pt) free(pt_str);
        }
        yap_strbuf_append(&hash_buf, ")");
        yap_loc dummy_loc = {0};
        yap_strbuf body_buf = yap_gen_block(ct_ctx, dummy_loc, body_block);
        yap_strbuf_append(&hash_buf, yap_strbuf_data(&body_buf));
        yap_strbuf_free(&body_buf);

        uint64_t hash = hashmap_murmur(yap_strbuf_data(&hash_buf), strlen(yap_strbuf_data(&hash_buf)), 0, 0);
        yap_strbuf_free(&hash_buf);

        emit_name = ct_alloc(strlen(name) + 20);
        sprintf(emit_name, "%s_%llx", name, (unsigned long long)hash);
    }

    const yap_var* existing = yap_scope_get_var_recursive(ct_ctx->global_scope, emit_name);
    if (existing){
        yap_log("finish: func '%s' already exists (dedup hit), name=%s", name, emit_name);
        b->locked = true;
        b->was_existed = true;
        b->result_name = emit_name;
        return emit_name;
    }

    darr(yap_type_id) arg_type_ids = yap_ctx_darr_new(ct_ctx, yap_type_id, .cap=darr_len(af), .len=0);
    for_darr(i, p, af) darr_push(arg_type_ids, p.type);
    yap_type ft = { .kind = yap_type_func, .func = { .args = arg_type_ids, .return_type = b->return_type } };
    yap_type_id ftid = yap_ctx_insert_type_if_not_exists(ct_ctx, ft);
    yap_scope_set_var(ct_ctx->global_scope, (yap_var){ .name = emit_name, .type = ftid });

    yap_decl decl = {
        .kind = yap_decl_func_def,
        .func_decl = (yap_func_decl){
            .name = emit_name,
            .args = af,
            .ret_typ = b->return_type,
            .body = body_block,
        },
        // See ct_type_finish for why this must be explicit, not left to
        // fall back to (possibly stale) ctx->current_module->prefix.
        .module_prefix = "",
    };
    if (ct_ctx->gen_decl)
        ct_ctx->gen_decl(ct_ctx, decl);

    yap_log("finish: emitted func '%s' as '%s'", name, emit_name);
    b->locked = true;
    b->was_existed = false;
    b->result_name = emit_name;
    return emit_name;
}

static int ct_func_existed(void* b_ptr){
    ct_func_builder* b = b_ptr;
    if (!b || !b->locked){ ct_error("existed() called before finish()"); return 0; }
    return b->was_existed ? 1 : 0;
}

static void* ct_func_func(void* b_ptr){
    ct_func_builder* b = b_ptr;
    if (!b || !b->locked){ ct_error("func() called before finish()"); return NULL; }
    return b->result_name;
}

static void* ct_type_lookup(const char* name){
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

/* yapi->hole(name): a blueprint placeholder. Typed as a yExpr so it can sit
 * anywhere in a builder-constructed template; yapi->fill replaces it later. */
static void* ct_make_hole(const char* name){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_blueprint_hole;
    e->var_name = ct_strdup(name);
    e->type = ct_ctx ? ct_ctx->yexpr_type_id : 0;
    e->is_comptime = true;
    return e;
}

/* Deep-clone a comptime expr, replacing every blueprint hole named `name` with
 * a (deep) copy of `value`. Pass name=NULL for a plain deep clone. Cloning is
 * required so a stored blueprint can be filled repeatedly without mutation.
 * Kinds a first-cut blueprint template can contain (literal/var/bin/hole) plus
 * common value kinds are cloned structurally; anything else is shallow-copied,
 * which is safe because fill never mutates a node's children. */
static yap_expr* ct_clone_expr(yap_expr* e, const char* name, yap_expr* value){
    if (!e) return NULL;
    if (e->kind == yap_expr_blueprint_hole){
        if (name && e->var_name && strcmp(e->var_name, name) == 0)
            return ct_clone_expr(value, NULL, NULL);
        yap_expr* h = ct_alloc(sizeof(yap_expr)); *h = *e;
        h->var_name = ct_strdup(e->var_name);
        return h;
    }
    yap_expr* n = ct_alloc(sizeof(yap_expr)); *n = *e;
    switch (e->kind){
        case yap_expr_bin:
            n->bin_expr.left  = ct_clone_expr(e->bin_expr.left,  name, value);
            n->bin_expr.right = ct_clone_expr(e->bin_expr.right, name, value);
            /* The template's bin type was computed while an operand was still a
             * hole (typed yExpr) — recompute it now that holes are filled so the
             * result carries the real operand type (e.g. i32), not yExpr. */
            n->type = ct_bin_result_type(n->bin_expr.op, n->bin_expr.left, n->bin_expr.right);
            break;
        case yap_expr_unary:
        case yap_expr_paren:
            n->subexpr = ct_clone_expr(e->subexpr, name, value);
            n->type = n->subexpr->type; // follows the (now-filled) operand
            break;
        case yap_expr_cast:
        case yap_expr_deref:
        case yap_expr_at_op:
        case yap_expr_increment:
        case yap_expr_decrement:
            n->subexpr = ct_clone_expr(e->subexpr, name, value);
            break;
        case yap_expr_assignment:
            n->assignment.left  = ct_clone_expr(e->assignment.left,  name, value);
            n->assignment.right = ct_clone_expr(e->assignment.right, name, value);
            break;
        case yap_expr_member_access:
            n->member_access.object = ct_clone_expr(e->member_access.object, name, value);
            n->member_access.member = ct_strdup(e->member_access.member);
            break;
        case yap_expr_index_access:
            n->index_access.object = ct_clone_expr(e->index_access.object, name, value);
            n->index_access.index  = ct_clone_expr(e->index_access.index,  name, value);
            break;
        case yap_expr_ternary:
            n->ternary.condition = ct_clone_expr(e->ternary.condition, name, value);
            n->ternary.then_expr = ct_clone_expr(e->ternary.then_expr, name, value);
            n->ternary.else_expr = ct_clone_expr(e->ternary.else_expr, name, value);
            if (ct_ctx) // branches may have been holes; recompute from filled operands
                n->type = yap_ctx_find_common_type(ct_ctx, n->ternary.then_expr->type, n->ternary.else_expr->type);
            break;
        case yap_expr_var:
            n->var_name = ct_strdup(e->var_name);
            break;
        case yap_expr_literal:
            n->literal.text = ct_strdup(e->literal.text);
            break;
        default:
            /* func_call, block, module_access, etc.: shallow copy shares
             * children, which is fine since fill is non-mutating and such nodes
             * never carry unfilled holes in the first-cut feature set. */
            break;
    }
    return n;
}

/* yExprBlueprint:fill(name, value) method: a blueprint with holes named `name`
 * replaced by `value`. Returns a fresh tree (self is left intact for further
 * fills) that is still a yExprBlueprint — chain more fills, then :finish(). */
static void* ct_bp_fill(void* self, const char* name, void* value){
    return ct_clone_expr((yap_expr*)self, name, (yap_expr*)value);
}

/* First unfilled hole in a comptime expr, or NULL if fully filled. */
static const char* ct_first_unfilled_hole(yap_expr* e){
    if (!e) return NULL;
    if (e->kind == yap_expr_blueprint_hole) return e->var_name ? e->var_name : "?";
    const char* h = NULL;
    switch (e->kind){
        case yap_expr_bin:
            h = ct_first_unfilled_hole(e->bin_expr.left);
            if (!h) h = ct_first_unfilled_hole(e->bin_expr.right);
            break;
        case yap_expr_unary: case yap_expr_paren: case yap_expr_cast:
        case yap_expr_deref: case yap_expr_at_op:
        case yap_expr_increment: case yap_expr_decrement:
            h = ct_first_unfilled_hole(e->subexpr);
            break;
        case yap_expr_assignment:
            h = ct_first_unfilled_hole(e->assignment.left);
            if (!h) h = ct_first_unfilled_hole(e->assignment.right);
            break;
        case yap_expr_member_access:
            h = ct_first_unfilled_hole(e->member_access.object);
            break;
        case yap_expr_index_access:
            h = ct_first_unfilled_hole(e->index_access.object);
            if (!h) h = ct_first_unfilled_hole(e->index_access.index);
            break;
        case yap_expr_ternary:
            h = ct_first_unfilled_hole(e->ternary.condition);
            if (!h) h = ct_first_unfilled_hole(e->ternary.then_expr);
            if (!h) h = ct_first_unfilled_hole(e->ternary.else_expr);
            break;
        default: break;
    }
    return h;
}

/* yExprBlueprint:finish() method: verify every hole was filled, then hand back
 * the template as a plain yExpr. An unfilled hole is a comptime error (the
 * codegen guard is only a last-resort backstop). */
static void* ct_bp_finish(void* self){
    const char* hole = ct_first_unfilled_hole((yap_expr*)self);
    if (hole){
        char msg[160];
        snprintf(msg, sizeof(msg), "blueprint :finish() called with unfilled hole '%s' — add :fill(c\"%s\", ...) first", hole, hole);
        ct_error(msg);
    }
    return self;
}

const char* ct_builder_decls =
    /* Named once here so codegen (yap_gen_name_type_combo's yap_type_slice
     * case, components/yap-c/src/codegen.c) can reuse this stable name
     * instead of emitting a fresh anonymous struct everywhere yExprList is
     * used as a declared parameter type -- anonymous structs aren't
     * compatible types across separate prototype/definition emissions for
     * the same function. Layout must exactly match yap_gen_name_type_combo's
     * generic slice codegen ('T* data; unsigned long len;') and build.c's
     * yap_yexpr_slice (same layout, used to build these values). */
    "#ifndef __YAP_EXPRLIST_DEFINED\n"
    "#define __YAP_EXPRLIST_DEFINED\n"
    "typedef struct { void** data; unsigned long len; } yExprList;\n"
    "#endif\n"
    "#ifdef __TINYC__\n"
    "extern void* yapi_int(int value);\n"
    "extern void* yapi_float(double value);\n"
    "extern void* yapi_string(const char* value);\n"
    "extern void* yapi_bool(int value);\n"
    "extern void* yapi_var_value(const char* ident);\n"
    "extern void* yapi_new_var(void* type_id, const char* ident);\n"
    "extern void* yapi_bin_op(void* left, int op, void* right);\n"
    "extern void* yapi_neg(void* expr);\n"
    "extern void* yapi_ternary(void* cond, void* then_expr, void* else_expr);\n"
    "extern void* yapi_assign(void* lval, int op, void* rval);\n"
    "extern void* yapi_member(void* obj, const char* field);\n"
    "extern void* yapi_index(void* obj, void* idx);\n"
    "extern void* yapi_cast(void* expr, void* type_id);\n"
    "extern void* yapi_deref(void* expr);\n"
    "extern void* yapi_addr_of(void* expr);\n"
    "extern void* yapi_ptr_of(void* type_id);\n"
    "extern void* yapi_sizeof(void* type_id);\n"
    "extern void* yapi_call0(void* func);\n"
    "extern void* yapi_call1(void* func, void* a);\n"
    "extern void* yapi_call2(void* func, void* a, void* b);\n"
    "extern void* yapi_call3(void* func, void* a, void* b, void* c);\n"
    "extern int yapi_kind(void* expr);\n"
    "extern int yapi_is_comptime(void* expr);\n"
    "extern void* yapi_var_decl(void* type_id, const char* ident);\n"
    "extern void* yapi_expr_stmt(void* expr);\n"
    "extern void* yapi_return_stmt(void* expr);\n"
    "extern void* yapi_if_stmt(void* cond, void* then_stmt);\n"
    "extern void* yapi_if_else_stmt(void* cond, void* then_stmt, void* else_stmt);\n"
    "extern void* yapi_while_stmt(void* cond, void* body_stmt);\n"
    "extern void* yapi_block(void* stmts_list);\n"
    "extern void* yapi_uniq(void);\n"
    "extern const char* yapi_uniq_name(void);\n"  /* returns yIdent */
    "extern void* yapi_stmt_list_new(void);\n"
    "extern void* yapi_stmt_list_push(void* list, void* stmt);\n"
    "extern void* yapi_struct_t(void);\n"
    "extern void* yapi_enum_t(void);\n"
    "extern void* yapi_union_t(void);\n"
    "extern void* yapi_fn_t(void);\n"
    "extern void* yapi_type(const char* name);\n"
    "extern void* yapi_fn_type0(void* ret);\n"
    "extern void* yapi_fn_type1(void* ret, void* p1);\n"
    "extern void* yapi_fn_type2(void* ret, void* p1, void* p2);\n"
    "extern void* yapi_fn_type3(void* ret, void* p1, void* p2, void* p3);\n"
    "extern int yapi_type_exists(const char* name);\n"
    "extern int yapi_func_exists(const char* name);\n"
    "extern void yapi_log(const char* msg);\n"
    "extern void yapi_error(const char* msg);\n"
    "extern void yapi_warn(const char* msg);\n"
    "extern void* yapi_hole(const char* name);\n"
    "extern void* yStructT_add_field(void* b, void* type_id, const char* name);\n"
    "extern void* yStructT_finish(void* b, const char* name);\n"
    "extern int yStructT_existed(void* b);\n"
    "extern void* yStructT_type(void* b);\n"
    "extern void* yEnumT_add_variant(void* b, const char* name);\n"
    "extern void* yEnumT_finish(void* b, const char* name);\n"
    "extern int yEnumT_existed(void* b);\n"
    "extern void* yEnumT_type(void* b);\n"
    "extern void* yUnionT_add_field(void* b, void* type_id, const char* name);\n"
    "extern void* yUnionT_finish(void* b, const char* name);\n"
    "extern int yUnionT_existed(void* b);\n"
    "extern void* yUnionT_type(void* b);\n"
    "extern void* yFnT_add_param(void* b, void* type_id, const char* name);\n"
    "extern void yFnT_set_return_type(void* b, void* type_id);\n"
    "extern void yFnT_set_body(void* b, void* stmt);\n"
    "extern void* yFnT_finish(void* b, const char* name);\n"
    "extern int yFnT_existed(void* b);\n"
    "extern void* yFnT_func(void* b);\n"
    "extern void* yFnT_get_subject(void* b);\n"
    "extern void* yType_new_method(void* type_id);\n"
    "extern void* yType_new_ref_method(void* type_id);\n"
    "extern void* yExprBlueprint_fill_expr(void* self, const char* name, void* value);\n"
    "extern void* yExprBlueprint_finish(void* self);\n"
    "#else\n"
    "static inline void* yapi_int(int v){(void)v;return 0;}\n"
    "static inline void* yapi_float(double v){(void)v;return 0;}\n"
    "static inline void* yapi_string(const char* v){(void)v;return 0;}\n"
    "static inline void* yapi_bool(int v){(void)v;return 0;}\n"
    "static inline void* yapi_var_value(const char* v){(void)v;return 0;}\n"
    "static inline void* yapi_new_var(void* t,const char* n){(void)t;(void)n;return 0;}\n"
    "static inline void* yapi_bin_op(void* l,int o,void* r){(void)l;(void)o;(void)r;return 0;}\n"
    "static inline void* yapi_neg(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_ternary(void* c,void* t,void* f){(void)c;(void)t;(void)f;return 0;}\n"
    "static inline void* yapi_assign(void* l,int o,void* r){(void)l;(void)o;(void)r;return 0;}\n"
    "static inline void* yapi_member(void* o,const char* f){(void)o;(void)f;return 0;}\n"
    "static inline void* yapi_index(void* o,void* i){(void)o;(void)i;return 0;}\n"
    "static inline void* yapi_cast(void* e,void* t){(void)e;(void)t;return 0;}\n"
    "static inline void* yapi_deref(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_addr_of(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_ptr_of(void* t){(void)t;return 0;}\n"
    "static inline void* yapi_sizeof(void* t){(void)t;return 0;}\n"
    "static inline void* yapi_call0(void* f){(void)f;return 0;}\n"
    "static inline void* yapi_call1(void* f,void* a){(void)f;(void)a;return 0;}\n"
    "static inline void* yapi_call2(void* f,void* a,void* b){(void)f;(void)a;(void)b;return 0;}\n"
    "static inline void* yapi_call3(void* f,void* a,void* b,void* c){(void)f;(void)a;(void)b;(void)c;return 0;}\n"
    "static inline int yapi_kind(void* e){(void)e;return 0;}\n"
    "static inline int yapi_is_comptime(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_var_decl(void* t,const char* n){(void)t;(void)n;return 0;}\n"
    "static inline void* yapi_expr_stmt(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_return_stmt(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_if_stmt(void* c,void* t){(void)c;(void)t;return 0;}\n"
    "static inline void* yapi_if_else_stmt(void* c,void* t,void* e){(void)c;(void)t;(void)e;return 0;}\n"
    "static inline void* yapi_while_stmt(void* c,void* b){(void)c;(void)b;return 0;}\n"
    "static inline void* yapi_block(void* s){(void)s;return 0;}\n"
    "static inline void* yapi_uniq(void){return 0;}\n"
    "static inline const char* yapi_uniq_name(void){return \"\";}\n"
    "static inline void* yapi_stmt_list_new(void){return 0;}\n"
    "static inline void* yapi_stmt_list_push(void* l,void* s){(void)l;(void)s;return 0;}\n"
    "static inline void* yapi_struct_t(void){return 0;}\n"
    "static inline void* yapi_enum_t(void){return 0;}\n"
    "static inline void* yapi_union_t(void){return 0;}\n"
    "static inline void* yapi_fn_t(void){return 0;}\n"
    "static inline void* yapi_type(const char* n){(void)n;return 0;}\n"
    "static inline void* yapi_fn_type0(void* r){(void)r;return 0;}\n"
    "static inline void* yapi_fn_type1(void* r,void* a){(void)r;(void)a;return 0;}\n"
    "static inline void* yapi_fn_type2(void* r,void* a,void* b){(void)r;(void)a;(void)b;return 0;}\n"
    "static inline void* yapi_fn_type3(void* r,void* a,void* b,void* c){(void)r;(void)a;(void)b;(void)c;return 0;}\n"
    "static inline int yapi_type_exists(const char* n){(void)n;return 0;}\n"
    "static inline int yapi_func_exists(const char* n){(void)n;return 0;}\n"
    "static inline void yapi_log(const char* m){(void)m;}\n"
    "static inline void yapi_error(const char* m){(void)m;}\n"
    "static inline void yapi_warn(const char* m){(void)m;}\n"
    "static inline void* yapi_hole(const char* n){(void)n;return 0;}\n"
    "static inline void* yStructT_add_field(void* b,void* t,const char* n){(void)b;(void)t;(void)n;return 0;}\n"
    "static inline void* yStructT_finish(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline int yStructT_existed(void* b){(void)b;return 0;}\n"
    "static inline void* yStructT_type(void* b){(void)b;return 0;}\n"
    "static inline void* yEnumT_add_variant(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline void* yEnumT_finish(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline int yEnumT_existed(void* b){(void)b;return 0;}\n"
    "static inline void* yEnumT_type(void* b){(void)b;return 0;}\n"
    "static inline void* yUnionT_add_field(void* b,void* t,const char* n){(void)b;(void)t;(void)n;return 0;}\n"
    "static inline void* yUnionT_finish(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline int yUnionT_existed(void* b){(void)b;return 0;}\n"
    "static inline void* yUnionT_type(void* b){(void)b;return 0;}\n"
    "static inline void* yFnT_add_param(void* b,void* t,const char* n){(void)b;(void)t;(void)n;return 0;}\n"
    "static inline void yFnT_set_return_type(void* b,void* t){(void)b;(void)t;}\n"
    "static inline void yFnT_set_body(void* b,void* s){(void)b;(void)s;}\n"
    "static inline void* yFnT_finish(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline int yFnT_existed(void* b){(void)b;return 0;}\n"
    "static inline void* yFnT_func(void* b){(void)b;return 0;}\n"
    "static inline void* yFnT_get_subject(void* b){(void)b;return 0;}\n"
    "static inline void* yType_new_method(void* t){(void)t;return 0;}\n"
    "static inline void* yType_new_ref_method(void* t){(void)t;return 0;}\n"
    "static inline void* yExprBlueprint_fill_expr(void* s,const char* n,void* v){(void)s;(void)n;(void)v;return 0;}\n"
    "static inline void* yExprBlueprint_finish(void* s){(void)s;return 0;}\n"
    "#endif\n";

static void yap_c_inject_comptime_builders(TCCState* tcc){
    tcc_add_symbol(tcc, "yapi_int",         ct_make_int);
    tcc_add_symbol(tcc, "yapi_float",       ct_make_float);
    tcc_add_symbol(tcc, "yapi_string",      ct_make_string);
    tcc_add_symbol(tcc, "yapi_bool",        ct_make_bool);
    tcc_add_symbol(tcc, "yapi_var_value",   ct_var_value);
    tcc_add_symbol(tcc, "yapi_new_var",     ct_make_new_var);
    tcc_add_symbol(tcc, "yapi_bin_op",      ct_make_bin);
    tcc_add_symbol(tcc, "yapi_neg",         ct_make_neg);
    tcc_add_symbol(tcc, "yapi_ternary",     ct_make_ternary);
    tcc_add_symbol(tcc, "yapi_assign",      ct_make_assign);
    tcc_add_symbol(tcc, "yapi_member",      ct_make_member);
    tcc_add_symbol(tcc, "yapi_index",       ct_make_index);
    tcc_add_symbol(tcc, "yapi_cast",        ct_make_cast);
    tcc_add_symbol(tcc, "yapi_deref",       ct_make_deref);
    tcc_add_symbol(tcc, "yapi_addr_of",     ct_make_addr_of);
    tcc_add_symbol(tcc, "yapi_ptr_of",      ct_ptr_of);
    tcc_add_symbol(tcc, "yapi_sizeof",      ct_sizeof);
    tcc_add_symbol(tcc, "yapi_call0",       ct_call0);
    tcc_add_symbol(tcc, "yapi_call1",       ct_call1);
    tcc_add_symbol(tcc, "yapi_call2",       ct_call2);
    tcc_add_symbol(tcc, "yapi_call3",       ct_call3);
    tcc_add_symbol(tcc, "yapi_kind",         ct_expr_kind);
    tcc_add_symbol(tcc, "yapi_is_comptime",  ct_expr_is_comptime);
    tcc_add_symbol(tcc, "yapi_var_decl",       ct_make_var_decl);
    tcc_add_symbol(tcc, "yapi_expr_stmt",      ct_make_expr_stmt);
    tcc_add_symbol(tcc, "yapi_return_stmt",    ct_make_return_stmt);
    tcc_add_symbol(tcc, "yapi_if_stmt",        ct_make_if_stmt);
    tcc_add_symbol(tcc, "yapi_if_else_stmt",   ct_make_if_else_stmt);
    tcc_add_symbol(tcc, "yapi_while_stmt",     ct_make_while_stmt);
    tcc_add_symbol(tcc, "yapi_block",          ct_make_block);
    tcc_add_symbol(tcc, "yapi_uniq",           ct_uniq);
    tcc_add_symbol(tcc, "yapi_uniq_name",      ct_uniq_name);
    tcc_add_symbol(tcc, "yapi_stmt_list_new",  ct_stmt_list_new);
    tcc_add_symbol(tcc, "yapi_stmt_list_push", ct_stmt_list_push);
    tcc_add_symbol(tcc, "yapi_struct_t",      ct_struct_new);
    tcc_add_symbol(tcc, "yapi_enum_t",        ct_enum_new);
    tcc_add_symbol(tcc, "yapi_union_t",       ct_union_new);
    tcc_add_symbol(tcc, "yapi_fn_t",        ct_fn_t);
    tcc_add_symbol(tcc, "yapi_type",          ct_type_lookup);
    tcc_add_symbol(tcc, "yapi_fn_type0",    ct_fn_type0);
    tcc_add_symbol(tcc, "yapi_fn_type1",    ct_fn_type1);
    tcc_add_symbol(tcc, "yapi_fn_type2",    ct_fn_type2);
    tcc_add_symbol(tcc, "yapi_fn_type3",    ct_fn_type3);
    tcc_add_symbol(tcc, "yapi_type_exists",  ct_type_exists);
    tcc_add_symbol(tcc, "yapi_func_exists",  ct_func_exists);
    tcc_add_symbol(tcc, "yapi_log",          ct_log);
    tcc_add_symbol(tcc, "yapi_error",        ct_error);
    tcc_add_symbol(tcc, "yapi_warn",         ct_warn);
    tcc_add_symbol(tcc, "yapi_hole",         ct_make_hole);

    tcc_add_symbol(tcc, "yStructT_add_field", ct_struct_add_field);
    tcc_add_symbol(tcc, "yStructT_finish",    ct_type_finish);
    tcc_add_symbol(tcc, "yStructT_existed",   ct_type_existed);
    tcc_add_symbol(tcc, "yStructT_type",      ct_type_type);
    tcc_add_symbol(tcc, "yEnumT_add_variant", ct_enum_add_variant);
    tcc_add_symbol(tcc, "yEnumT_finish",      ct_type_finish);
    tcc_add_symbol(tcc, "yEnumT_existed",     ct_type_existed);
    tcc_add_symbol(tcc, "yEnumT_type",        ct_type_type);
    tcc_add_symbol(tcc, "yUnionT_add_field",  ct_struct_add_field);
    tcc_add_symbol(tcc, "yUnionT_finish",     ct_type_finish);
    tcc_add_symbol(tcc, "yUnionT_existed",    ct_type_existed);
    tcc_add_symbol(tcc, "yUnionT_type",       ct_type_type);
    tcc_add_symbol(tcc, "yFnT_add_param",       ct_func_add_param);
    tcc_add_symbol(tcc, "yFnT_set_return_type", ct_func_set_return_type);
    tcc_add_symbol(tcc, "yFnT_set_body",        ct_func_set_body);
    tcc_add_symbol(tcc, "yFnT_finish",          ct_func_finish);
    tcc_add_symbol(tcc, "yFnT_existed",         ct_func_existed);
    tcc_add_symbol(tcc, "yFnT_func",            ct_func_func);
    tcc_add_symbol(tcc, "yFnT_get_subject",     ct_func_get_subject);
    tcc_add_symbol(tcc, "yType_new_method",     ct_new_method);
    tcc_add_symbol(tcc, "yType_new_ref_method", ct_new_ref_method);
    tcc_add_symbol(tcc, "yExprBlueprint_fill_expr",   ct_bp_fill);
    tcc_add_symbol(tcc, "yExprBlueprint_finish", ct_bp_finish);
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

static int feed_module_files_to_tcc(yap_ctx* ctx, yap_module* module){
    yap_module_c_code* mod_code = module->module_ctx;

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

    yap_c_build_state* state = ctx->build_state;
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

    if (feed_module_files_to_tcc(ctx, module) != 0)
        return -1;

    // Relocate the new state
    yap_c_build_state* state = ctx->build_state;
    if (tcc_relocate(state->tcc) != 0){
        yap_log("TCC relocate failed during recompile");
        darr_free(ctx->errors);
        ctx->errors = darr_new(yap_error);
        return -1;
    }

    yap_log("Recompile succeeded (counter=%lu)", state->counter);
    return 0;
}

int yap_c_run_from_files(yap_ctx* ctx, yap_module* module){
    if (!ctx || !module || !module->module_ctx) return -1;
    yap_module_c_code* mod_code = module->module_ctx;

    if (mod_code->types_fp) fflush(mod_code->types_fp);
    if (mod_code->decls_fp) fflush(mod_code->decls_fp);
    if (mod_code->impl_fp)  fflush(mod_code->impl_fp);

    yap_c_free_tcc_state(ctx);
    yap_c_init_tcc_state(ctx);
    if (!ctx->build_state){
        yap_log("Failed to re-init TCC state for run");
        return -1;
    }

    if (feed_module_files_to_tcc(ctx, module) != 0)
        return -1;

    yap_c_build_state* state = ctx->build_state;
    yap_log("Running program in-memory via TCC...");
    int ret = tcc_run(state->tcc, 0, NULL);
    yap_log("Program finished (exit code %d)", ret);
    return ret;
}

void* yap_c_ensure_symbol(yap_ctx* ctx, const char* name){
    yap_module* module = ctx ? yap_ctx_current_module(ctx) : NULL;
    if (!module){
        yap_log("No active module for ensure_symbol");
        return NULL;
    }

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
    yap_module* module = ctx ? yap_ctx_current_module(ctx) : NULL;
    if (!module) return;

    yap_log("TCC-checking for 'main' symbol...");
    int rc = yap_c_recompile_from_files(ctx, module);
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
