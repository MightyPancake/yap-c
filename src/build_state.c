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

// Throwaway TCC state to verify the pipeline works; errors here are not forwarded to ctx.
void yap_c_run_tcc_smoke_test(yap_ctx* ctx){
    (void)ctx;
    yap_log("Running TCC smoke test (separate state)");

    TCCState* test_tcc = tcc_new();
    if (!test_tcc){
        yap_log("Failed to create TCC state for smoke test");
        return;
    }
    tcc_set_output_type(test_tcc, TCC_OUTPUT_MEMORY);
    // Do NOT use tcc_error_callback ; smoke test errors stay in this state

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

    // Relocate and call ; pure function, no libc dependency
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

    // Probe GCC for system include paths (via -E -Wp,-v)
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

    // Probe GCC linker for library search dirs; NixOS emits "attempt to open /path/...", Debian emits SEARCH_DIR("/path").
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
    yap_log("TCC build state initialized (NOT frozen ; no relocate call)");
}

/* ----------------------------------------------------------------
 *  Comptime builder functions ; called from TCC at compile time
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

// yapi->var_value: read-only ref to an in-scope var; resolves its type from global_scope so callers (e.g. func_call) get a real type instead of defaulting to 0.
static void* ct_var_value(const char* name){
    yap_expr* e = ct_make_var(name);
    ((yap_expr*)e)->is_lvalue = false;
    if (ct_ctx){
        const yap_var* var = yap_scope_get_var_recursive(ct_ctx->global_scope, (char*)name);
        if (var) ((yap_expr*)e)->type = var->type;
    }
    return e;
}

static void* ct_make_new_var(void* type_id_ptr, const char* name){
    yap_expr* e = ct_make_var(name);
    ((yap_expr*)e)->type = (yap_type_id)(uintptr_t)type_id_ptr;
    return e;
}

// yapi->assign: op is a char code (like bin_op's), not a cstring -- every compound assignment is mechanically "<char>=", so one byte covers all of them.
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

// yapi->opt_member ('obj?.field'): only meaningful on pointer-to-struct/union; never an lvalue -- a null pointer falls back to the member type's zero value at runtime.
static void* ct_make_opt_member(void* obj, const char* field){
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_optional_member_access;
    yap_expr* o = ct_alloc(sizeof(yap_expr)); *o = *(yap_expr*)obj;
    e->member_access = (yap_member_access){ .object = o, .member = ct_strdup(field) };
    if (ct_ctx){
        yap_type* obj_type = yap_ctx_get_type(ct_ctx, o->type);
        if (obj_type && obj_type->kind == yap_type_ptr)
            e->type = yap_ctx_find_member_type(ct_ctx, obj_type->pointer_type, field);
    }
    e->is_lvalue = false;
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

static void* ct_slice_of(void* type_id_ptr){
    if (!ct_ctx) return NULL;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    return (void*)(uintptr_t)yap_ctx_get_slice_of_type_id(ct_ctx, tid);
}

static void* ct_array_of(void* type_id_ptr, int size){
    if (!ct_ctx) return NULL;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    return (void*)(uintptr_t)yap_ctx_get_array_of_type_id(ct_ctx, tid, (size_t)size);
}

// yapi->type_of: reads expr's own .type -- only valid for exprs built via real semantic building. A yapi->member(...) result has .type unset until the spliced result is later built, so type_of(member(...)) always reads 0 here; use field_type instead.
static void* ct_type_of(void* expr_ptr){
    if (!expr_ptr) return NULL;
    yap_expr* e = (yap_expr*)expr_ptr;
    return (void*)(uintptr_t)e->type;
}

/* yapi->pointee_type(T): inverse of ptr_of -- the type T points to, given
 * T is itself a pointer type. */
static void* ct_pointee_type(void* type_id_ptr){
    if (!ct_ctx) return NULL;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    yap_type* t = yap_ctx_get_type(ct_ctx, tid);
    if (!t || t->kind != yap_type_ptr){
        ct_error("yapi->pointee_type: type is not a pointer");
        return NULL;
    }
    return (void*)(uintptr_t)t->pointer_type;
}

// yapi->field_type: struct/union field type looked up directly from T's definition, not via an expr (type_of on a yapi->member(...) result won't work -- see type_of's note).
static void* ct_field_type(void* type_id_ptr, const char* name){
    if (!ct_ctx || !name) return NULL;
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    yap_type* t = yap_ctx_get_type(ct_ctx, tid);
    if (!t){ ct_error("yapi->field_type: invalid type"); return NULL; }
    darr(yap_struct_field) fields = NULL;
    if (t->kind == yap_type_struct) fields = t->structure.fields;
    else if (t->kind == yap_type_union) fields = t->uni.variants;
    else { ct_error("yapi->field_type: type is not a struct or union"); return NULL; }
    for_darr(i, f, fields){
        if (f.kind == yap_struct_field_valid && f.name && strcmp(f.name, name) == 0)
            return (void*)(uintptr_t)f.type;
    }
    ct_error("yapi->field_type: no such field");
    return NULL;
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

// yapi->sizeof: no dedicated AST node, so builds a numeric-literal expr whose text is literally "sizeof(<c type>)" -- yap_gen_literal prints it verbatim.
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
    e->unary_op = '-';
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = src->type;
    e->is_comptime = src->is_comptime;
    return e;
}

/* yapi->not(e): prefix logical not ('!expr'). Result is always bool, same as
 * the real build path (yap_build_unary_expr). */
static void* ct_make_not(void* expr_ptr){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_unary;
    e->unary_op = '!';
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = ct_ctx ? ct_ctx->bool_type_id : 0;
    e->is_comptime = src->is_comptime;
    return e;
}

/* yapi->bnot(e): prefix bitwise not ('~expr'). Keeps the operand's type,
 * same as neg(). */
static void* ct_make_bnot(void* expr_ptr){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_unary;
    e->unary_op = '~';
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = src->type;
    e->is_comptime = src->is_comptime;
    return e;
}

// yapi->increment/decrement: result is never an lvalue; unchecked (like assign()) -- caller must pass an lvalue-tagged operand.
static void* ct_make_increment(void* expr_ptr, int prefix){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_increment;
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = src->type;
    e->prefix = prefix != 0;
    return e;
}

static void* ct_make_decrement(void* expr_ptr, int prefix){
    yap_expr* src = (yap_expr*)expr_ptr;
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind = yap_expr_decrement;
    yap_expr* sub = ct_alloc(sizeof(yap_expr)); *sub = *src;
    e->subexpr = sub;
    e->type = src->type;
    e->prefix = prefix != 0;
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
    // Pre-sized to argc and filled via .src in one shot -- darr_push always realloc()s, which is unsafe on arena-backed (quake_alloc) storage.
    darr(yap_expr) params = ct_ctx
        ? yap_ctx_darr_new(ct_ctx, yap_expr, .cap=argc, .len=argc, .src=src)
        : darr_new(yap_expr, .cap=argc, .len=argc, .src=src);
    e->func_call = (yap_func_call){ .func_expr = f, .params = params };
    if (ct_ctx){
        yap_type* func_type = yap_ctx_get_type(ct_ctx, f->type);
        if (func_type && func_type->kind == yap_type_func)
            e->type = func_type->func.return_type;
    }
    return e;
}

// yapi->call0..call3: ergonomic fixed-arity shortcuts; arbitrary arity goes through call_args_new/push + yapi->call (ct_make_func_call).
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

// yapi->call_args_new/push: growable yCallArgs list for >3-arg calls, distinct from yExprList (the fixed slice type). Grows via fresh ct_alloc+memcpy (never realloc), safe to mix with arena-backed storage -- same technique as ct_stmt_list_push.
static void* ct_call_args_new(void){
    yap_expr_list* l = ct_alloc(sizeof(yap_expr_list));
    *l = (yap_expr_list){0};
    return l;
}

static void* ct_call_args_push(void* list, void* expr){
    yap_expr_list* l = (yap_expr_list*)list;
    if (!l || !expr) return l;
    if (l->count >= l->cap){
        unsigned int newcap = l->cap ? l->cap * 2 : 4;
        yap_expr* items = ct_alloc(sizeof(yap_expr) * newcap);
        if (l->items) memcpy(items, l->items, sizeof(yap_expr) * l->count);
        l->items = items;
        l->cap = newcap;
    }
    l->items[l->count++] = *(yap_expr*)expr;
    return l;
}

/* ----------------------------------------------------------------
 *  Comptime handle lists
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

// yapi->var_decl: bare declaration only; declare-then-init is two statements now: var_decl(...) then expr_statement(assign(new_var(...), "=", value)).
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

// yapi->if_stmt/if_else_stmt: mirror the two real if AST kinds rather than one builder with a nullable else branch.
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

/* yapi->for_stmt(init, cond, update, body): mirrors yap_for (init;cond;update)
 * body). init may be a var_decl/expr_stmt/etc.; body is typically a block. */
static void* ct_make_for_stmt(void* init_stmt, void* cond, void* update, void* body_stmt){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_for;
    yap_statement* init_cpy = ct_alloc(sizeof(yap_statement)); *init_cpy = *(yap_statement*)init_stmt;
    yap_statement* body_cpy = ct_alloc(sizeof(yap_statement)); *body_cpy = *(yap_statement*)body_stmt;
    s->for_stmt = (yap_for){
        .init = init_cpy,
        .condition = *(yap_expr*)cond,
        .update = *(yap_expr*)update,
        .body = body_cpy
    };
    return s;
}

// yapi->break_stmt/continue_stmt: no payload; unchecked whether the splice site is actually inside a loop, same trust-the-caller stance as the other builders.
static void* ct_make_break_stmt(void){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_break;
    return s;
}

static void* ct_make_continue_stmt(void){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_continue;
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

// yapi->block_expr: value-yielding counterpart to block, builds a GNU statement-expression node so a macro can declare+mutate a temporary and still return one yExpr. Last statement must be an expr statement; its type/lvalue/comptime-ness becomes the block's.
static void* ct_make_block_expr(void* stmts_list){
    yap_stmt_list* sl = (yap_stmt_list*)stmts_list;
    unsigned int count = sl ? sl->count : 0;
    if (count == 0){ ct_error("yapi->block_expr: statement list is empty"); return NULL; }
    yap_statement last = sl->items[count - 1];
    if (last.kind != yap_statement_expr){
        ct_error("yapi->block_expr: last statement must be an expression statement");
        return NULL;
    }
    darr(yap_statement) statements = ct_ctx
        ? yap_ctx_darr_new(ct_ctx, yap_statement, .cap=count, .len=count, .src=sl->items)
        : darr_new(yap_statement, .cap=count, .len=count, .src=sl->items);
    yap_block* blk = ct_alloc(sizeof(yap_block));
    *blk = (yap_block){ .kind = yap_block_valid, .statements = statements };
    yap_expr* e = ct_alloc(sizeof(yap_expr));
    *e = (yap_expr){0};
    e->kind        = yap_expr_block;
    e->block       = blk;
    e->type        = last.expr.type;
    e->is_lvalue   = last.expr.is_lvalue;
    e->is_comptime = last.expr.is_comptime;
    return e;
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
 *  Incremental: struct_t/enum_t/union_t create an empty builder; add_field/add_variant fill it; finish(name) locks it, hashes its layout, dedups against an existing same-name-and-hash type, and emits it if new.
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

// yapi->yEnumT_add_variant_value: like add_variant but with an explicit discriminant value ('Name = value') instead of auto-increment.
static void* ct_enum_add_variant_value(void* b_ptr, const char* variant_name, void* value_ptr){
    ct_type_builder* b = b_ptr;
    if (!b) return b_ptr;
    if (b->locked){ ct_error("Cannot add a variant to a type template after finish()"); return b_ptr; }
    yap_expr* val = ct_alloc(sizeof(yap_expr)); *val = *(yap_expr*)value_ptr;
    yap_enum_variant v = { .name = ct_strdup(variant_name), .value = val };
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
        // Explicit empty prefix: c_name is already fully resolved; NULL would fall back to current_module->prefix, which reflects the last-imported module, not where this macro is invoked from -- risking a stale prefix reapplied on an already-correct name.
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
 *  fn_t (plain function) / new_method (method, subject auto-injected as first param "self") build the same template; finish(name) hashes the generated C code for dedup, then emits a top-level function -- mangled "name_hash", or "SubjectType_name" for methods.
 * ---------------------------------------------------------------- */

typedef struct {
    bool is_method;
    yap_type_id subject_type_id; // non-pointer subject type (valid if is_method); used for owner-name mangling even if the actual first param is pointer-typed (ref method)
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

// Like ct_new_method but subject is auto-injected as 'T@' (pointer) for mutating methods; call site auto-takes-the-address of an lvalue receiver, so callers still write 'recv:name(args)' with no '&'. get_subject() returns 'self' (typed T@); deref it to reach fields.
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

// Named struct/union/enum (and primitives, for builtin comptime types) are the only eligible method subjects -- mirrors yap_named_type_owner_name in build.c, duplicated since that's static in a different shared library.
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
        // Method finish(name) mangles to "OwnerType_name" (no hash), matching yap_func_decl_emit_name's convention for user-declared methods, so it dispatches via the same 'recv:name(args)' path. Dedup rides on the owner type's finish()/existed() gate, not a body hash.
        const char* owner = ct_owner_type_name(b->subject_type_id);
        if (!owner) owner = "?";
        emit_name = ct_alloc(strlen(owner) + strlen(name) + 2);
        sprintf(emit_name, "%s_%s", owner, name);
    } else {
        // Plain fn_t(): hash the generated C code (signature + body), same reuse-codegen approach ct_type_finish uses for field layout.
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

// yFn:ref(): a yFn handle already IS its emitted C name, so this is just ct_var_value under the real tag -- gives a callable reference with .type resolved via scope lookup.
static void* ct_fn_ref(void* fn_handle){
    return ct_var_value((const char*)fn_handle);
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

// yapi->register_macro_method: associates receiver type `owner` with macro method `name`, backed by `backing_fn_name` (may be an alias). Stored in ct_ctx->macro_methods, separate from real per-instantiation methods and ordinary bare-name macros, so dispatch looks up (receiver.type, name) directly instead of guessing by parameter shape.
static void ct_register_macro_method(void* owner_type_ptr, const char* method_name, const char* backing_fn_name){
    if (!ct_ctx || !method_name || !backing_fn_name) return;
    yap_type_id owner = (yap_type_id)(uintptr_t)owner_type_ptr;

    const yap_var* found = NULL;
    char* emit_name = NULL;
    // A top-level fn outside any 'module X {}' wrapper lands in ctx->global_scope itself, not a module's .scope -- check that first before scanning modules.
    {
        const yap_var* var = yap_scope_get_var(ct_ctx->global_scope, (char*)backing_fn_name);
        if (var){ found = var; emit_name = ct_strdup(var->name); }
    }
    size_t iter = 0;
    void* item;
    while (!found && hashmap_iter(ct_ctx->modules, &iter, &item)){
        yap_module* mod = (yap_module*)item;
        if (!mod->scope) continue;
        const yap_var* var = yap_scope_get_var(mod->scope, (char*)backing_fn_name);
        if (!var) continue;
        found = var;
        emit_name = (mod->prefix && mod->prefix[0])
            ? yap_ctx_strus_newf(ct_ctx, "%s%s", mod->prefix, var->name)
            : ct_strdup(var->name);
    }
    if (!found){
        char* msg = ct_alloc(strlen(backing_fn_name) + 64);
        sprintf(msg, "yapi->register_macro_method: no macro function '%s' found", backing_fn_name);
        ct_error(msg);
        return;
    }

    darr_push(ct_ctx->macro_methods, ((yap_macro_method_entry){
        .owner_type = owner,
        .name = ct_strdup(method_name),
        .emit_name = emit_name,
        .func_type = found->type
    }));
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

// yapi->type_hole ($T): unlike expr/stmt holes (AST nodes filled later), a type IS a type_id, so the hole must be a real interned type_id up front. Interned by hole_name, so repeated $T in one template dedupes to the same type_id -- one :fill_type() closes every occurrence.
static void* ct_make_type_hole(const char* name){
    if (!ct_ctx) return NULL;
    yap_type hole_type = (yap_type){0};
    hole_type.kind = yap_type_hole;
    hole_type.hole_name = ct_strdup(name);
    return (void*)(uintptr_t)yap_ctx_insert_type_if_not_exists(ct_ctx, hole_type);
}

// yapi->ident_hole ($name in a var_decl): yIdent is just a const char*, so the hole IS the string value -- sentinel-prefixed with '$', a byte no real identifier can start with, so ct_is_ident_hole can tell a hole from an ordinary name by inspection alone.
static void* ct_make_ident_hole(const char* name){
    size_t len = name ? strlen(name) : 0;
    char* s = ct_alloc(len + 2);
    s[0] = '$';
    if (len) memcpy(s + 1, name, len);
    s[len + 1] = '\0';
    return s;
}

// True if `ident` is a ct_make_ident_hole result; *out_name points into `ident` (no copy).
static bool ct_is_ident_hole(const char* ident, const char** out_name){
    if (!ident || ident[0] != '$') return false;
    if (out_name) *out_name = ident + 1;
    return true;
}

// Deep-clones an expr, substituting exactly one of: a blueprint hole named `name` -> expr_val, a cast's type-hole -> *type_val, or an ident-hole var-ref -> ident_val (var_decl's own name is fixed by ct_clone_stmt; this handles later plain references to the same $name). name=NULL means plain clone; cloning lets a stored blueprint be filled repeatedly without mutation.
static yap_expr* ct_clone_expr(yap_expr* e, const char* name, yap_expr* expr_val, yap_type_id* type_val, const char* ident_val){
    if (!e) return NULL;
    if (e->kind == yap_expr_blueprint_hole){
        if (expr_val && name && e->var_name && strcmp(e->var_name, name) == 0)
            return ct_clone_expr(expr_val, NULL, NULL, NULL, NULL);
        yap_expr* h = ct_alloc(sizeof(yap_expr)); *h = *e;
        h->var_name = ct_strdup(e->var_name);
        return h;
    }
    yap_expr* n = ct_alloc(sizeof(yap_expr)); *n = *e;
    switch (e->kind){
        case yap_expr_bin:
            n->bin_expr.left  = ct_clone_expr(e->bin_expr.left,  name, expr_val, type_val, ident_val);
            n->bin_expr.right = ct_clone_expr(e->bin_expr.right, name, expr_val, type_val, ident_val);
            // Recompute now that holes are filled, so the result carries the real operand type (e.g. i32), not yExpr.
            n->type = ct_bin_result_type(n->bin_expr.op, n->bin_expr.left, n->bin_expr.right);
            break;
        case yap_expr_unary:
            n->subexpr = ct_clone_expr(e->subexpr, name, expr_val, type_val, ident_val);
            // '-'/'~' follow the (now-filled) operand's type; '!' is always
            // bool regardless of operand, same as ct_make_not/yap_build_unary_expr.
            n->type = (e->unary_op == '!') ? e->type : n->subexpr->type;
            break;
        case yap_expr_paren:
            n->subexpr = ct_clone_expr(e->subexpr, name, expr_val, type_val, ident_val);
            n->type = n->subexpr->type; // follows the (now-filled) operand
            break;
        case yap_expr_cast:
            n->subexpr = ct_clone_expr(e->subexpr, name, expr_val, type_val, ident_val);
            // Unlike deref/at_op/etc, a cast's OWN .type is user-specified and may itself be a lazy $T type-hole, so it needs the same hole-check/substitute treatment as var_decl's .var.type.
            if (type_val && ct_ctx){
                yap_type* et = yap_ctx_get_type(ct_ctx, e->type);
                if (et && et->kind == yap_type_hole && name && et->hole_name && strcmp(et->hole_name, name) == 0)
                    n->type = *type_val;
            }
            break;
        case yap_expr_deref:
        case yap_expr_at_op:
        case yap_expr_increment:
        case yap_expr_decrement:
            n->subexpr = ct_clone_expr(e->subexpr, name, expr_val, type_val, ident_val);
            break;
        case yap_expr_assignment:
            n->assignment.left  = ct_clone_expr(e->assignment.left,  name, expr_val, type_val, ident_val);
            n->assignment.right = ct_clone_expr(e->assignment.right, name, expr_val, type_val, ident_val);
            break;
        case yap_expr_member_access:
            n->member_access.object = ct_clone_expr(e->member_access.object, name, expr_val, type_val, ident_val);
            n->member_access.member = ct_strdup(e->member_access.member);
            // Recompute is_lvalue after fill: ct_make_member baked in false at construction time when the object was still an unfilled hole (e.g. $self.field where $self fills to a deref result) -- without this, codegen rejects the assignment as not-an-lvalue.
            n->is_lvalue = n->member_access.object->is_lvalue;
            break;
        case yap_expr_index_access:
            n->index_access.object = ct_clone_expr(e->index_access.object, name, expr_val, type_val, ident_val);
            n->index_access.index  = ct_clone_expr(e->index_access.index,  name, expr_val, type_val, ident_val);
            // Recompute element type after fill: ct_make_index resolved .type from the object's type at construction time, which silently came out 0 if the object was still an unfilled hole. is_lvalue is unconditionally true here, so it doesn't need recomputing.
            if (ct_ctx){
                yap_type* obj_type = yap_ctx_get_type(ct_ctx, n->index_access.object->type);
                if (obj_type){
                    if (obj_type->kind == yap_type_array) n->type = obj_type->array.element_type;
                    else if (obj_type->kind == yap_type_slice) n->type = obj_type->slice.element_type;
                    else if (obj_type->kind == yap_type_ptr) n->type = obj_type->pointer_type;
                }
            }
            break;
        case yap_expr_func_call: {
            n->func_call.func_expr = ct_clone_expr(e->func_call.func_expr, name, expr_val, type_val, ident_val);
            unsigned int argc = darr_len(e->func_call.params);
            darr(yap_expr) new_params = ct_ctx
                ? yap_ctx_darr_new(ct_ctx, yap_expr, .cap=argc, .len=argc)
                : darr_new(yap_expr, .cap=argc, .len=argc);
            for (unsigned int i = 0; i < argc; i++){
                yap_expr* cloned = ct_clone_expr(&e->func_call.params[i], name, expr_val, type_val, ident_val);
                new_params[i] = *cloned;
            }
            n->func_call.params = new_params;
            /* callee may have been a hole (e.g. a yFn value); recompute the
             * call's result type now that it's filled, same as bin/ternary. */
            if (ct_ctx){
                yap_type* func_type = yap_ctx_get_type(ct_ctx, n->func_call.func_expr->type);
                if (func_type && func_type->kind == yap_type_func)
                    n->type = func_type->func.return_type;
            }
            break;
        }
        case yap_expr_ternary:
            n->ternary.condition = ct_clone_expr(e->ternary.condition, name, expr_val, type_val, ident_val);
            n->ternary.then_expr = ct_clone_expr(e->ternary.then_expr, name, expr_val, type_val, ident_val);
            n->ternary.else_expr = ct_clone_expr(e->ternary.else_expr, name, expr_val, type_val, ident_val);
            if (ct_ctx) // branches may have been holes; recompute from filled operands
                n->type = yap_ctx_find_common_type(ct_ctx, n->ternary.then_expr->type, n->ternary.else_expr->type);
            break;
        case yap_expr_var: {
            n->var_name = ct_strdup(e->var_name);
            if (ident_val){
                const char* hole_nm = NULL;
                if (ct_is_ident_hole(e->var_name, &hole_nm) && name && hole_nm && strcmp(hole_nm, name) == 0)
                    n->var_name = ct_strdup(ident_val);
            }
            break;
        }
        case yap_expr_literal:
            n->literal.text = ct_strdup(e->literal.text);
            break;
        default:
            // block, module_access, etc.: shallow copy shares children -- fine since fill is non-mutating and these never carry unfilled holes in the first-cut feature set.
            break;
    }
    return n;
}

// yExprBlueprint:fill: returns a fresh tree with holes named `name` replaced by `value`; self is left intact so fills can be chained, then :finish().
static void* ct_bp_fill(void* self, const char* name, void* value){
    return ct_clone_expr((yap_expr*)self, name, (yap_expr*)value, NULL, NULL);
}

// yExprBlueprint:fill_type: substitutes a cast's lazy $T type-hole named `name` with a concrete type_id.
static void* ct_bp_fill_type(void* self, const char* name, void* type_id_ptr){
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    return ct_clone_expr((yap_expr*)self, name, NULL, &tid, NULL);
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
        case yap_expr_unary: case yap_expr_paren:
        case yap_expr_deref: case yap_expr_at_op:
        case yap_expr_increment: case yap_expr_decrement:
            h = ct_first_unfilled_hole(e->subexpr);
            break;
        case yap_expr_cast:
            h = ct_first_unfilled_hole(e->subexpr);
            // A cast's own .type may itself be a lazy $T type-hole, unlike deref/at_op/etc whose .type is derived, not user-specified.
            if (!h && ct_ctx){
                yap_type* et = yap_ctx_get_type(ct_ctx, e->type);
                if (et && et->kind == yap_type_hole) h = et->hole_name ? et->hole_name : "?";
            }
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
        case yap_expr_func_call:
            h = ct_first_unfilled_hole(e->func_call.func_expr);
            for (unsigned int i = 0; !h && i < darr_len(e->func_call.params); i++)
                h = ct_first_unfilled_hole(&e->func_call.params[i]);
            break;
        case yap_expr_var: {
            // A plain var-ref whose name is an ident-hole is unfilled until :fill_ident()/:fill_var().
            const char* nm = NULL;
            if (ct_is_ident_hole(e->var_name, &nm)) h = nm ? nm : "?";
            break;
        }
        default: break;
    }
    return h;
}

// yExprBlueprint:finish: verifies every hole was filled (comptime error if not; the codegen guard is only a last-resort backstop), then hands back the template as a plain yExpr.
static void* ct_bp_finish(void* self){
    const char* hole = ct_first_unfilled_hole((yap_expr*)self);
    if (hole){
        char msg[160];
        snprintf(msg, sizeof(msg), "blueprint :finish() called with unfilled hole '%s' ; add :fill_expr/:fill_type(c\"%s\", ...) first", hole, hole);
        ct_error(msg);
    }
    return self;
}

/* ----------------------------------------------------------------
 *  Statement blueprints (yStmtBlueprint)
 *  Mirrors ct_clone_expr/ct_first_unfilled_hole but recurses over statement kinds, deferring embedded exprs to the expr-level helpers.
 * ---------------------------------------------------------------- */
// yapi->hole_stmt: statement-position hole; reuses the .expr union slot to carry the name as a blueprint_hole expr.
static void* ct_make_stmt_hole(const char* name){
    yap_statement* s = ct_alloc(sizeof(yap_statement));
    *s = (yap_statement){0};
    s->kind = yap_statement_hole;
    s->expr = (yap_expr){0};
    s->expr.kind = yap_expr_blueprint_hole;
    s->expr.var_name = ct_strdup(name);
    return s;
}

// Clones a yStatement, replacing holes named `name`; exactly one of expr_val/stmt_val/type_val/ident_val is non-NULL, selecting expr holes (fill_expr), statement holes (fill_stmt), var_decl/cast type-holes (fill_type), or var_decl ident-holes (fill_ident) -- others are left intact for a later chained fill.
static yap_statement* ct_clone_stmt(yap_statement* s, const char* name, yap_expr* expr_val, yap_statement* stmt_val, yap_type_id* type_val, const char* ident_val){
    if (!s) return NULL;
    if (s->kind == yap_statement_hole){
        if (stmt_val && name && s->expr.var_name && strcmp(s->expr.var_name, name) == 0)
            return ct_clone_stmt(stmt_val, NULL, NULL, NULL, NULL, NULL); // deep-clone the fill value
        yap_statement* h = ct_alloc(sizeof(yap_statement)); *h = *s;
        h->expr.var_name = ct_strdup(s->expr.var_name);
        return h;
    }
    yap_statement* n = ct_alloc(sizeof(yap_statement));
    *n = *s;
    // Each hole kind self-gates on its own value parameter inside ct_clone_expr (blueprint_hole checks expr_val, cast checks type_val, var-ref checks ident_val); `name` just disambiguates which occurrence, so it's passed through as-is -- no per-kind zeroing (the old expr-only `en` variable this replaced would wrongly suppress type/ident-hole matches when expr_val is NULL).
    switch (s->kind){
        case yap_statement_expr:
            n->expr = *ct_clone_expr(&s->expr, name, expr_val, type_val, ident_val);
            break;
        case yap_statement_return:
            n->return_stmt.value = *ct_clone_expr(&s->return_stmt.value, name, expr_val, type_val, ident_val);
            break;
        case yap_statement_var_decl: {
            if (type_val){
                yap_type* vt = ct_ctx ? yap_ctx_get_type(ct_ctx, s->var_decl.var.type) : NULL;
                if (vt && vt->kind == yap_type_hole && name && vt->hole_name && strcmp(vt->hole_name, name) == 0)
                    n->var_decl.var.type = *type_val;
            }
            if (ident_val){
                const char* hole_nm = NULL;
                if (ct_is_ident_hole(s->var_decl.var.name, &hole_nm) && name && hole_nm && strcmp(hole_nm, name) == 0)
                    n->var_decl.var.name = ct_strdup(ident_val);
            }
            if (s->var_decl.has_init)
                n->var_decl.init = *ct_clone_expr(&s->var_decl.init, name, expr_val, type_val, ident_val);
            break;
        }
        case yap_statement_if:
            n->if_stmt.condition   = *ct_clone_expr(&s->if_stmt.condition, name, expr_val, type_val, ident_val);
            n->if_stmt.then_branch = ct_clone_stmt(s->if_stmt.then_branch, name, expr_val, stmt_val, type_val, ident_val);
            break;
        case yap_statement_if_else:
            n->if_else_stmt.condition   = *ct_clone_expr(&s->if_else_stmt.condition, name, expr_val, type_val, ident_val);
            n->if_else_stmt.then_branch = ct_clone_stmt(s->if_else_stmt.then_branch, name, expr_val, stmt_val, type_val, ident_val);
            n->if_else_stmt.else_branch = ct_clone_stmt(s->if_else_stmt.else_branch, name, expr_val, stmt_val, type_val, ident_val);
            break;
        case yap_statement_while:
            n->while_stmt.condition = *ct_clone_expr(&s->while_stmt.condition, name, expr_val, type_val, ident_val);
            n->while_stmt.body      = ct_clone_stmt(s->while_stmt.body, name, expr_val, stmt_val, type_val, ident_val);
            break;
        case yap_statement_block: {
            unsigned int cnt = darr_len(s->block.statements);
            darr(yap_statement) stmts = ct_ctx
                ? yap_ctx_darr_new(ct_ctx, yap_statement, .cap=cnt, .len=0)
                : darr_new(yap_statement, .cap=cnt, .len=0);
            for_darr(i, st, s->block.statements){
                yap_statement* c = ct_clone_stmt(&st, name, expr_val, stmt_val, type_val, ident_val);
                darr_push(stmts, *c);
            }
            n->block.statements = stmts;
            break;
        }
        default: break;
    }
    return n;
}

static const char* ct_first_unfilled_hole_stmt(yap_statement* s){
    if (!s) return NULL;
    if (s->kind == yap_statement_hole) return s->expr.var_name ? s->expr.var_name : "?";
    const char* h = NULL;
    switch (s->kind){
        case yap_statement_expr:     h = ct_first_unfilled_hole(&s->expr); break;
        case yap_statement_return:   h = ct_first_unfilled_hole(&s->return_stmt.value); break;
        case yap_statement_var_decl: {
            yap_type* vt = ct_ctx ? yap_ctx_get_type(ct_ctx, s->var_decl.var.type) : NULL;
            if (vt && vt->kind == yap_type_hole) h = vt->hole_name ? vt->hole_name : "?";
            if (!h){
                const char* nm = NULL;
                if (ct_is_ident_hole(s->var_decl.var.name, &nm)) h = nm ? nm : "?";
            }
            if (!h && s->var_decl.has_init) h = ct_first_unfilled_hole(&s->var_decl.init);
            break;
        }
        case yap_statement_if:
            h = ct_first_unfilled_hole(&s->if_stmt.condition);
            if (!h) h = ct_first_unfilled_hole_stmt(s->if_stmt.then_branch);
            break;
        case yap_statement_if_else:
            h = ct_first_unfilled_hole(&s->if_else_stmt.condition);
            if (!h) h = ct_first_unfilled_hole_stmt(s->if_else_stmt.then_branch);
            if (!h) h = ct_first_unfilled_hole_stmt(s->if_else_stmt.else_branch);
            break;
        case yap_statement_while:
            h = ct_first_unfilled_hole(&s->while_stmt.condition);
            if (!h) h = ct_first_unfilled_hole_stmt(s->while_stmt.body);
            break;
        case yap_statement_block:
            for_darr(i, st, s->block.statements){ h = ct_first_unfilled_hole_stmt(&st); if (h) break; }
            break;
        default: break;
    }
    return h;
}

/* yStmtBlueprint:fill_expr(name, value) ; replace expr holes named `name`. */
static void* ct_bp_stmt_fill_expr(void* self, const char* name, void* value){
    return ct_clone_stmt((yap_statement*)self, name, (yap_expr*)value, NULL, NULL, NULL);
}

/* yStmtBlueprint:fill_stmt(name, value) ; replace statement holes named `name`. */
static void* ct_bp_stmt_fill_stmt(void* self, const char* name, void* value){
    return ct_clone_stmt((yap_statement*)self, name, NULL, (yap_statement*)value, NULL, NULL);
}

/* yStmtBlueprint:fill_type(name, type) ; replace a var_decl type-hole (or a
 * nested cast's type-hole) named `name` with the concrete type_id. */
static void* ct_bp_stmt_fill_type(void* self, const char* name, void* type_id_ptr){
    yap_type_id tid = (yap_type_id)(uintptr_t)type_id_ptr;
    return ct_clone_stmt((yap_statement*)self, name, NULL, NULL, &tid, NULL);
}

// yStmtBlueprint:fill_ident: replaces a var_decl ident-hole (the declaration itself); does not touch later plain references to the same name (that's a separate expr-hole -- see fill_var).
static void* ct_bp_stmt_fill_ident(void* self, const char* name, const char* ident){
    return ct_clone_stmt((yap_statement*)self, name, NULL, NULL, NULL, ident);
}

// yStmtBlueprint:fill_var: declare-and-reference sugar. A var_decl's name-hole and a later plain reference sharing the same spelling are different hole kinds (ident-hole vs expr-hole); fill_ident alone only closes the declaration. This closes both via two sequential clones -- fill_ident then fill_expr with a fresh reference -- not a new substitution kind.
static void* ct_bp_stmt_fill_var(void* self, const char* name, void* type_id_ptr, const char* ident){
    void* declared = ct_bp_stmt_fill_ident(self, name, ident);
    yap_expr* ref = ct_make_new_var(type_id_ptr, ident);
    return ct_clone_stmt((yap_statement*)declared, name, ref, NULL, NULL, NULL);
}

/* yStmtBlueprint:finish() ; verify all holes filled, hand back a plain yStmt. */
static void* ct_bp_stmt_finish(void* self){
    const char* hole = ct_first_unfilled_hole_stmt((yap_statement*)self);
    if (hole){
        char msg[168];
        snprintf(msg, sizeof(msg), "stmt blueprint :finish() called with unfilled hole '%s' ; add :fill_expr/:fill_stmt/:fill_type/:fill_ident(c\"%s\", ...) first", hole, hole);
        ct_error(msg);
    }
    return self;
}

const char* ct_builder_decls =
    // yExprList named once here so codegen reuses this stable name instead of emitting a fresh anonymous struct (incompatible across separate prototype/definition emissions). Layout must exactly match yap_gen_name_type_combo's slice codegen and build.c's yap_yexpr_slice.
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
    "extern void* yapi_not(void* expr);\n"
    "extern void* yapi_bnot(void* expr);\n"
    "extern void* yapi_ternary(void* cond, void* then_expr, void* else_expr);\n"
    "extern void* yapi_assign(void* lval, int op, void* rval);\n"
    "extern void* yapi_member(void* obj, const char* field);\n"
    "extern void* yapi_opt_member(void* obj, const char* field);\n"
    "extern void* yapi_index(void* obj, void* idx);\n"
    "extern void* yapi_cast(void* expr, void* type_id);\n"
    "extern void* yapi_deref(void* expr);\n"
    "extern void* yapi_addr_of(void* expr);\n"
    "extern void* yapi_increment(void* expr, int prefix);\n"
    "extern void* yapi_decrement(void* expr, int prefix);\n"
    "extern void* yapi_ptr_of(void* type_id);\n"
    "extern void* yapi_slice_of(void* type_id);\n"
    "extern void* yapi_array_of(void* type_id, int size);\n"
    "extern void* yapi_type_of(void* expr);\n"
    "extern void* yapi_pointee_type(void* type_id);\n"
    "extern void* yapi_field_type(void* type_id, const char* name);\n"
    "extern void* yapi_sizeof(void* type_id);\n"
    "extern void* yapi_call0(void* func);\n"
    "extern void* yapi_call1(void* func, void* a);\n"
    "extern void* yapi_call2(void* func, void* a, void* b);\n"
    "extern void* yapi_call3(void* func, void* a, void* b, void* c);\n"
    "extern void* yapi_call_args_new(void);\n"
    "extern void* yapi_call_args_push(void* list, void* expr);\n"
    "extern void* yapi_call(void* func, void* args_list);\n"
    "extern int yapi_kind(void* expr);\n"
    "extern int yapi_is_comptime(void* expr);\n"
    "extern void* yapi_var_decl(void* type_id, const char* ident);\n"
    "extern void* yapi_expr_stmt(void* expr);\n"
    "extern void* yapi_return_stmt(void* expr);\n"
    "extern void* yapi_if_stmt(void* cond, void* then_stmt);\n"
    "extern void* yapi_if_else_stmt(void* cond, void* then_stmt, void* else_stmt);\n"
    "extern void* yapi_while_stmt(void* cond, void* body_stmt);\n"
    "extern void* yapi_for_stmt(void* init_stmt, void* cond, void* update, void* body_stmt);\n"
    "extern void* yapi_break_stmt(void);\n"
    "extern void* yapi_continue_stmt(void);\n"
    "extern void* yapi_block(void* stmts_list);\n"
    "extern void* yapi_block_expr(void* stmts_list);\n"
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
    "extern void yapi_register_macro_method(void* owner_type, const char* name, const char* backing_fn_name);\n"
    "extern void* yapi_hole(const char* name);\n"
    "extern void* yapi_hole_stmt(const char* name);\n"
    "extern void* yapi_type_hole(const char* name);\n"
    "extern const char* yapi_ident_hole(const char* name);\n"  /* returns yIdent */
    "extern void* yStructT_add_field(void* b, void* type_id, const char* name);\n"
    "extern void* yStructT_finish(void* b, const char* name);\n"
    "extern int yStructT_existed(void* b);\n"
    "extern void* yStructT_type(void* b);\n"
    "extern void* yEnumT_add_variant(void* b, const char* name);\n"
    "extern void* yEnumT_add_variant_value(void* b, const char* name, void* value);\n"
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
    "extern void* yFn_ref(void* fn);\n"
    "extern void* yType_new_method(void* type_id);\n"
    "extern void* yType_new_ref_method(void* type_id);\n"
    "extern void* yExprBlueprint_fill_expr(void* self, const char* name, void* value);\n"
    "extern void* yExprBlueprint_fill_type(void* self, const char* name, void* type_id);\n"
    "extern void* yExprBlueprint_finish(void* self);\n"
    "extern void* yStmtBlueprint_fill_expr(void* self, const char* name, void* value);\n"
    "extern void* yStmtBlueprint_fill_stmt(void* self, const char* name, void* value);\n"
    "extern void* yStmtBlueprint_fill_type(void* self, const char* name, void* type_id);\n"
    "extern void* yStmtBlueprint_fill_ident(void* self, const char* name, const char* ident);\n"
    "extern void* yStmtBlueprint_fill_var(void* self, const char* name, void* type_id, const char* ident);\n"
    "extern void* yStmtBlueprint_finish(void* self);\n"
    "#else\n"
    "static inline void* yapi_int(int v){(void)v;return 0;}\n"
    "static inline void* yapi_float(double v){(void)v;return 0;}\n"
    "static inline void* yapi_string(const char* v){(void)v;return 0;}\n"
    "static inline void* yapi_bool(int v){(void)v;return 0;}\n"
    "static inline void* yapi_var_value(const char* v){(void)v;return 0;}\n"
    "static inline void* yapi_new_var(void* t,const char* n){(void)t;(void)n;return 0;}\n"
    "static inline void* yapi_bin_op(void* l,int o,void* r){(void)l;(void)o;(void)r;return 0;}\n"
    "static inline void* yapi_neg(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_not(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_bnot(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_ternary(void* c,void* t,void* f){(void)c;(void)t;(void)f;return 0;}\n"
    "static inline void* yapi_assign(void* l,int o,void* r){(void)l;(void)o;(void)r;return 0;}\n"
    "static inline void* yapi_member(void* o,const char* f){(void)o;(void)f;return 0;}\n"
    "static inline void* yapi_opt_member(void* o,const char* f){(void)o;(void)f;return 0;}\n"
    "static inline void* yapi_index(void* o,void* i){(void)o;(void)i;return 0;}\n"
    "static inline void* yapi_cast(void* e,void* t){(void)e;(void)t;return 0;}\n"
    "static inline void* yapi_deref(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_addr_of(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_increment(void* e,int p){(void)e;(void)p;return 0;}\n"
    "static inline void* yapi_decrement(void* e,int p){(void)e;(void)p;return 0;}\n"
    "static inline void* yapi_ptr_of(void* t){(void)t;return 0;}\n"
    "static inline void* yapi_slice_of(void* t){(void)t;return 0;}\n"
    "static inline void* yapi_array_of(void* t,int s){(void)t;(void)s;return 0;}\n"
    "static inline void* yapi_type_of(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_pointee_type(void* t){(void)t;return 0;}\n"
    "static inline void* yapi_field_type(void* t,const char* n){(void)t;(void)n;return 0;}\n"
    "static inline void* yapi_sizeof(void* t){(void)t;return 0;}\n"
    "static inline void* yapi_call0(void* f){(void)f;return 0;}\n"
    "static inline void* yapi_call1(void* f,void* a){(void)f;(void)a;return 0;}\n"
    "static inline void* yapi_call2(void* f,void* a,void* b){(void)f;(void)a;(void)b;return 0;}\n"
    "static inline void* yapi_call3(void* f,void* a,void* b,void* c){(void)f;(void)a;(void)b;(void)c;return 0;}\n"
    "static inline void* yapi_call_args_new(void){return 0;}\n"
    "static inline void* yapi_call_args_push(void* l,void* e){(void)l;(void)e;return 0;}\n"
    "static inline void* yapi_call(void* f,void* a){(void)f;(void)a;return 0;}\n"
    "static inline int yapi_kind(void* e){(void)e;return 0;}\n"
    "static inline int yapi_is_comptime(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_var_decl(void* t,const char* n){(void)t;(void)n;return 0;}\n"
    "static inline void* yapi_expr_stmt(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_return_stmt(void* e){(void)e;return 0;}\n"
    "static inline void* yapi_if_stmt(void* c,void* t){(void)c;(void)t;return 0;}\n"
    "static inline void* yapi_if_else_stmt(void* c,void* t,void* e){(void)c;(void)t;(void)e;return 0;}\n"
    "static inline void* yapi_while_stmt(void* c,void* b){(void)c;(void)b;return 0;}\n"
    "static inline void* yapi_for_stmt(void* i,void* c,void* u,void* b){(void)i;(void)c;(void)u;(void)b;return 0;}\n"
    "static inline void* yapi_break_stmt(void){return 0;}\n"
    "static inline void* yapi_continue_stmt(void){return 0;}\n"
    "static inline void* yapi_block(void* s){(void)s;return 0;}\n"
    "static inline void* yapi_block_expr(void* s){(void)s;return 0;}\n"
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
    "static inline void yapi_register_macro_method(void* t,const char* n,const char* f){(void)t;(void)n;(void)f;}\n"
    "static inline void* yapi_hole(const char* n){(void)n;return 0;}\n"
    "static inline void* yapi_hole_stmt(const char* n){(void)n;return 0;}\n"
    "static inline void* yapi_type_hole(const char* n){(void)n;return 0;}\n"
    "static inline const char* yapi_ident_hole(const char* n){(void)n;return \"\";}\n"
    "static inline void* yStructT_add_field(void* b,void* t,const char* n){(void)b;(void)t;(void)n;return 0;}\n"
    "static inline void* yStructT_finish(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline int yStructT_existed(void* b){(void)b;return 0;}\n"
    "static inline void* yStructT_type(void* b){(void)b;return 0;}\n"
    "static inline void* yEnumT_add_variant(void* b,const char* n){(void)b;(void)n;return 0;}\n"
    "static inline void* yEnumT_add_variant_value(void* b,const char* n,void* v){(void)b;(void)n;(void)v;return 0;}\n"
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
    "static inline void* yFn_ref(void* f){(void)f;return 0;}\n"
    "static inline void* yType_new_method(void* t){(void)t;return 0;}\n"
    "static inline void* yType_new_ref_method(void* t){(void)t;return 0;}\n"
    "static inline void* yExprBlueprint_fill_expr(void* s,const char* n,void* v){(void)s;(void)n;(void)v;return 0;}\n"
    "static inline void* yExprBlueprint_fill_type(void* s,const char* n,void* t){(void)s;(void)n;(void)t;return 0;}\n"
    "static inline void* yExprBlueprint_finish(void* s){(void)s;return 0;}\n"
    "static inline void* yStmtBlueprint_fill_expr(void* s,const char* n,void* v){(void)s;(void)n;(void)v;return 0;}\n"
    "static inline void* yStmtBlueprint_fill_stmt(void* s,const char* n,void* v){(void)s;(void)n;(void)v;return 0;}\n"
    "static inline void* yStmtBlueprint_fill_type(void* s,const char* n,void* t){(void)s;(void)n;(void)t;return 0;}\n"
    "static inline void* yStmtBlueprint_fill_ident(void* s,const char* n,const char* i){(void)s;(void)n;(void)i;return 0;}\n"
    "static inline void* yStmtBlueprint_fill_var(void* s,const char* n,void* t,const char* i){(void)s;(void)n;(void)t;(void)i;return 0;}\n"
    "static inline void* yStmtBlueprint_finish(void* s){(void)s;return 0;}\n"
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
    tcc_add_symbol(tcc, "yapi_not",         ct_make_not);
    tcc_add_symbol(tcc, "yapi_bnot",        ct_make_bnot);
    tcc_add_symbol(tcc, "yapi_ternary",     ct_make_ternary);
    tcc_add_symbol(tcc, "yapi_assign",      ct_make_assign);
    tcc_add_symbol(tcc, "yapi_member",      ct_make_member);
    tcc_add_symbol(tcc, "yapi_opt_member",  ct_make_opt_member);
    tcc_add_symbol(tcc, "yapi_index",       ct_make_index);
    tcc_add_symbol(tcc, "yapi_cast",        ct_make_cast);
    tcc_add_symbol(tcc, "yapi_deref",       ct_make_deref);
    tcc_add_symbol(tcc, "yapi_addr_of",     ct_make_addr_of);
    tcc_add_symbol(tcc, "yapi_increment",   ct_make_increment);
    tcc_add_symbol(tcc, "yapi_decrement",   ct_make_decrement);
    tcc_add_symbol(tcc, "yapi_ptr_of",      ct_ptr_of);
    tcc_add_symbol(tcc, "yapi_slice_of",    ct_slice_of);
    tcc_add_symbol(tcc, "yapi_array_of",    ct_array_of);
    tcc_add_symbol(tcc, "yapi_type_of",      ct_type_of);
    tcc_add_symbol(tcc, "yapi_pointee_type", ct_pointee_type);
    tcc_add_symbol(tcc, "yapi_field_type",   ct_field_type);
    tcc_add_symbol(tcc, "yapi_sizeof",      ct_sizeof);
    tcc_add_symbol(tcc, "yapi_call0",       ct_call0);
    tcc_add_symbol(tcc, "yapi_call1",       ct_call1);
    tcc_add_symbol(tcc, "yapi_call2",       ct_call2);
    tcc_add_symbol(tcc, "yapi_call3",       ct_call3);
    tcc_add_symbol(tcc, "yapi_call_args_new",  ct_call_args_new);
    tcc_add_symbol(tcc, "yapi_call_args_push", ct_call_args_push);
    tcc_add_symbol(tcc, "yapi_call",           ct_make_func_call);
    tcc_add_symbol(tcc, "yapi_kind",         ct_expr_kind);
    tcc_add_symbol(tcc, "yapi_is_comptime",  ct_expr_is_comptime);
    tcc_add_symbol(tcc, "yapi_var_decl",       ct_make_var_decl);
    tcc_add_symbol(tcc, "yapi_expr_stmt",      ct_make_expr_stmt);
    tcc_add_symbol(tcc, "yapi_return_stmt",    ct_make_return_stmt);
    tcc_add_symbol(tcc, "yapi_if_stmt",        ct_make_if_stmt);
    tcc_add_symbol(tcc, "yapi_if_else_stmt",   ct_make_if_else_stmt);
    tcc_add_symbol(tcc, "yapi_while_stmt",     ct_make_while_stmt);
    tcc_add_symbol(tcc, "yapi_for_stmt",       ct_make_for_stmt);
    tcc_add_symbol(tcc, "yapi_break_stmt",     ct_make_break_stmt);
    tcc_add_symbol(tcc, "yapi_continue_stmt",  ct_make_continue_stmt);
    tcc_add_symbol(tcc, "yapi_block",          ct_make_block);
    tcc_add_symbol(tcc, "yapi_block_expr",     ct_make_block_expr);
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
    tcc_add_symbol(tcc, "yapi_register_macro_method", ct_register_macro_method);
    tcc_add_symbol(tcc, "yapi_hole",         ct_make_hole);
    tcc_add_symbol(tcc, "yapi_hole_stmt",    ct_make_stmt_hole);
    tcc_add_symbol(tcc, "yapi_type_hole",    ct_make_type_hole);
    tcc_add_symbol(tcc, "yapi_ident_hole",   ct_make_ident_hole);

    tcc_add_symbol(tcc, "yStructT_add_field", ct_struct_add_field);
    tcc_add_symbol(tcc, "yStructT_finish",    ct_type_finish);
    tcc_add_symbol(tcc, "yStructT_existed",   ct_type_existed);
    tcc_add_symbol(tcc, "yStructT_type",      ct_type_type);
    tcc_add_symbol(tcc, "yEnumT_add_variant", ct_enum_add_variant);
    tcc_add_symbol(tcc, "yEnumT_add_variant_value", ct_enum_add_variant_value);
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
    tcc_add_symbol(tcc, "yFn_ref",              ct_fn_ref);
    tcc_add_symbol(tcc, "yType_new_method",     ct_new_method);
    tcc_add_symbol(tcc, "yType_new_ref_method", ct_new_ref_method);
    tcc_add_symbol(tcc, "yExprBlueprint_fill_expr",   ct_bp_fill);
    tcc_add_symbol(tcc, "yExprBlueprint_fill_type",   ct_bp_fill_type);
    tcc_add_symbol(tcc, "yExprBlueprint_finish", ct_bp_finish);
    tcc_add_symbol(tcc, "yStmtBlueprint_fill_expr",  ct_bp_stmt_fill_expr);
    tcc_add_symbol(tcc, "yStmtBlueprint_fill_stmt",  ct_bp_stmt_fill_stmt);
    tcc_add_symbol(tcc, "yStmtBlueprint_fill_type",  ct_bp_stmt_fill_type);
    tcc_add_symbol(tcc, "yStmtBlueprint_fill_ident", ct_bp_stmt_fill_ident);
    tcc_add_symbol(tcc, "yStmtBlueprint_fill_var",   ct_bp_stmt_fill_var);
    tcc_add_symbol(tcc, "yStmtBlueprint_finish",     ct_bp_stmt_finish);
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

    // Symbol not available ; recompile from files, then try again
    yap_log("Symbol '%s' not in current TCC state, recompiling...", name);
    if (yap_c_recompile_from_files(ctx, module) != 0){
        yap_log("Recompile failed ; cannot resolve '%s'", name);
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
