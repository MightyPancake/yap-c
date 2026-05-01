#ifndef YAP_C_BACKEND_H
#define YAP_C_BACKEND_H

void yap_gen_code(yap_ctx* ctx);
yap_strbuf yap_gen_decl(yap_ctx* ctx, yap_source* src, yap_decl decl);
yap_strbuf yap_gen_fn_decl(yap_ctx* ctx, yap_source* src, yap_func_decl decl);

//Statements
yap_strbuf yap_gen_empty_statement(yap_ctx* ctx, yap_statement stmt);
yap_strbuf yap_gen_expr_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_block(yap_ctx* ctx, yap_source* src, yap_block block);
yap_strbuf yap_gen_var_decl(yap_ctx* ctx, yap_source* src, yap_var_decl var_decl);
yap_strbuf yap_gen_return_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_if_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_if_else_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_while(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_for(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_break(yap_ctx* ctx, yap_source* src, yap_statement stmt);
yap_strbuf yap_gen_continue(yap_ctx* ctx, yap_source* src, yap_statement stmt);

//Expressions
yap_strbuf yap_gen_expr(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_literal(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_var_access(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_binary_expr(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_assignment(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_func_call(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_cast_expr(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_at_op(yap_ctx* ctx, yap_source* src, yap_expr expr);
yap_strbuf yap_gen_paren_expr(yap_ctx* ctx, yap_source* src, yap_expr expr);

//Types
yap_strbuf yap_gen_name_type_combo(yap_ctx* ctx, const char* name, yap_type t);
yap_strbuf yap_gen_name_type_id_combo(yap_ctx* ctx, const char* name, yap_type_id id);
yap_strbuf yap_gen_type(yap_ctx* ctx, yap_source* src, yap_type type);
yap_strbuf yap_gen_type_id(yap_ctx* ctx, yap_source* src, yap_type_id id);

#endif
