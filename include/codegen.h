#ifndef YAP_C_BACKEND_H
#define YAP_C_BACKEND_H

void yap_gen_code(yap_ctx* ctx);
yap_strbuf yap_gen_decl(yap_ctx* ctx, yap_decl decl);
yap_strbuf yap_gen_fn_decl(yap_ctx* ctx, yap_func_decl decl);

//Types
yap_strbuf yap_gen_name_type_combo(yap_ctx* ctx, char* name, yap_type t);

#endif
