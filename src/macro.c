#include "yap_c.h"
#include "yap/all.h"

yap_macro_val yap_eval_macro(yap_ctx* ctx, yap_macro_val* args, uint32_t arg_count){
    (void)ctx;
    (void)args;
    (void)arg_count;
    yap_log("Evaluating macro with %d arguments", arg_count);
    return (yap_macro_val){0};
}

void my_fn(){
  static int counter = 0;
  yap_log("Hello from my_fn! (called %d times)", ++counter);
}

void yap_tcc_example(){
  const char* example = "void my_fn(); int add(int a, int b) {my_fn(); return a + b; }";
  TCCState* tcc = tcc_new();
  if (!tcc){
    fprintf(stderr, "Could not create TCC state\n");
    yap_log("Could not create TCC state");
    return;
  }

  if (tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY) == -1){
    fprintf(stderr, "Failed to set TCC output type\n");
    yap_log("Failed to set TCC output type");
    tcc_delete(tcc);
    return;
  }

  tcc_add_symbol(tcc, "my_fn", (void*)my_fn);

  if (tcc_compile_string(tcc, example) == -1){
    fprintf(stderr, "Compilation failed\n");
    yap_log("Compilation failed");
    tcc_delete(tcc);
    return;
  }

  if (tcc_relocate(tcc) == -1){
    fprintf(stderr, "Relocation failed\n");
    yap_log("Relocation failed\n");
    tcc_delete(tcc);
    return;
  }

  int (*add)(int, int) = tcc_get_symbol(tcc, "add");
  if (!add){
    fprintf(stderr, "Could not get symbol 'add'\n");
    yap_log("Could not get symbol 'add'");
    tcc_delete(tcc);
    return;
  }

  int result = add(2, 3);
  yap_log("Result of add(2, 3): %d", result);

  tcc_delete(tcc);
  yap_log("Calling my_fn directly from host:");
  my_fn();
}

void yap_macro_init(yap_ctx* ctx){
  (void)ctx;
  //Initialize macro system here if needed
}

void yap_macro_warn(yap_ctx* ctx, const char* fmt, ...){
  if (!ctx || !fmt) return;

  va_list ap;
  va_start(ap, fmt);

  char* msg = NULL;
  int fmt_result = quake_vasprintf(&ctx->arena, &msg, fmt, ap);

  va_end(ap);

  if (fmt_result < 0 || !msg){
    msg = "(failed to format macro warning)";
  }

  yap_log("macro warning: %s", msg);
}

void yap_macro_error(yap_ctx* ctx, const char* fmt, ...){
  if (!ctx || !fmt) return;

  va_list ap;
  va_start(ap, fmt);

  char* msg = NULL;
  int fmt_result = quake_vasprintf(&ctx->arena, &msg, fmt, ap);

  va_end(ap);

  if (fmt_result < 0 || !msg){
    msg = "(failed to format macro error)";
  }

  yap_ctx_push_error(ctx, (yap_error){
    .kind = yap_error_no_pos,
    .src = NULL,
    .msg = msg
  });
}

bool yap_macro_emit_decl(yap_ctx* ctx, yap_decl decl){
  if (!ctx) return false;

  if (!ctx->current_module){
    yap_macro_error(ctx, "Cannot emit declaration outside module context");
    return false;
  }

  if (darr_len(ctx->source_codes) == 0){
    yap_macro_error(ctx, "Cannot emit declaration without active source code");
    return false;
  }

  yap_source_code* src_code = &darr_last(ctx->source_codes);
  if (!src_code->declarations){
    src_code->declarations = yap_ctx_darr_new(ctx, yap_decl);
  }

  darr_push(src_code->declarations, decl);
  return true;
}
