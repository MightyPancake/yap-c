#include "yap_c.h"
#include "yap/all.h"

#define empty_strbuf yap_strbuf_empty()

void save_c_code(yap_strbuf code){
	FILE* f = fopen("out.c", "w");
	if (!f){
		yap_log("Failed to open output file");
		return;
	}
	fwrite(yap_strbuf_data(&code), 1, code.len, f);
	fclose(f);
	yap_log("C code saved to out.c");
}

void yap_gen_code(yap_ctx* ctx){
    (void)ctx;
	yap_log("Hello from the backend!");
	yap_strbuf res;
	yap_strbuf_init(&res);
	// yap_tcc_example();
	for_darr(i, src, ctx->sources){
		yap_log("Running code gen for source #%ld: %s", i, src.path);
		for_darr(j, decl, ctx->source_codes[0].declarations){
			yap_strbuf decl_code = yap_gen_decl(ctx, decl);
			yap_log("decl #%ld:\n%s", j, yap_strbuf_data(&decl_code));
			// Append to result
			// TODO: We should probably accumulate total size and create the buffer once or write directly to file instead
			yap_strbuf_append(&res, yap_strbuf_data(&decl_code));
			yap_strbuf_append(&res, "\n\n");
			yap_strbuf_free(&decl_code);
		}
	}
	//TODO Write res to file
	save_c_code(res);
	yap_strbuf_free(&res);
}

yap_strbuf yap_gen_decl(yap_ctx* ctx, yap_decl decl){
	yap_strbuf res;
	switch(decl.kind){
		case yap_decl_func:
			yap_log("Gen for function declaration: %s", decl.func_decl.name);
			res = yap_gen_fn_decl(ctx, decl.func_decl);
			break;
		default:
			yap_log("Unhandled declaration kind in codegen: %d", decl.kind);
			break;
	}
	return res;
}

yap_strbuf yap_gen_name_type_id_combo(yap_ctx* ctx, char* name, yap_type_id id){
	yap_type* typ = yap_ctx_get_type(ctx, id);
	if (!typ){
		yap_log("Invalid type id %d in yap_gen_name_type_id_combo", id);
		return empty_strbuf;
	}
	return yap_gen_name_type_combo(ctx, name, *typ);
}

//TODO: FIX/FINISH
yap_strbuf yap_gen_name_type_combo(yap_ctx* ctx, char* name, yap_type typ){
	//TODO: Qualifiers
	// yap_strbuf qualifiers = yap_strbuf_newf("%s%s", typ.is_const?"const ":"", typ.is_volatile?"volatile ":""); //Add space here!!!
	yap_strbuf qualifiers = yap_strbuf_new();
	yap_strbuf left = yap_strbuf_new(); //This will be reversed
	yap_strbuf right = yap_strbuf_new(); //This will be in normal order
	// bool last_append_was_left = false;
	yap_type t = typ;
	uint depth = 0;
	//TODO: Finish other type kinds
	while(t.kind != yap_type_primitive){
		if (depth++ > 128){
			yap_log("Type depth exceeded 128");
			return (yap_strbuf){.data=NULL, .len=0, .cap=0};
		}
		switch(t.kind){
			case yap_type_ptr:
				// last_append_was_left = true;
				yap_strbuf_append(&left, "*");
				t = *yap_ctx_get_type(ctx, t.pointer_type);
				break;
			case yap_type_func:
				// last_append_was_left = false;
				yap_strbuf_append(&left, "*((");
				yap_strbuf_append(&right, ")(");
				for_darr(i, arg, t.func.args){
					if (i > 0) yap_strbuf_append(&right, ", ");
					yap_strbuf arg_str = yap_gen_name_type_combo(ctx, "", *yap_ctx_get_type(ctx, arg));
					yap_strbuf_append(&right, yap_strbuf_data(&arg_str));
					yap_strbuf_free(&arg_str);
				}
				yap_strbuf_append(&right, "))");
				t = *yap_ctx_get_type(ctx, t.func.return_type);
				break;
			default:
				yap_log("Unsupported type kind in yap_gen_name_type_combo: %d", t.kind);
				return empty_strbuf;
		}
	}
	char* reversed_left = malloc(left.len + 1);
	for (size_t i = 0; i < left.len; i++){
		reversed_left[i] = left.data[left.len - 1 - i];
	}
	reversed_left[left.len] = '\0';
	yap_strbuf_free(&left);
    char* prim_type_str = t.primitive.c_name;
	yap_strbuf res = yap_strbuf_newf("%s%s%s%s%s%s", yap_strbuf_data(&qualifiers), prim_type_str, name ? " " : "", reversed_left, name ? name : "", yap_strbuf_data(&right));
	yap_strbuf_free(&qualifiers);
	yap_strbuf_free(&right);
	free(reversed_left);
	return res;
}

yap_strbuf yap_gen_fn_decl(yap_ctx* ctx, yap_func_decl decl){
	(void)ctx;
	(void)decl;
	yap_strbuf res = yap_gen_name_type_id_combo(ctx, NULL, decl.ret_typ);
	yap_strbuf_appendf(&res, " %s(", decl.name);
	for_darr(i, arg, decl.args){
		if (i > 0) yap_strbuf_append(&res, ", ");
		yap_strbuf arg_buf = yap_gen_name_type_id_combo(ctx, arg.name, arg.type);
		yap_strbuf_append(&res, yap_strbuf_data(&arg_buf));
		yap_strbuf_free(&arg_buf);
	}
	yap_strbuf_append(&res, ")");
	yap_strbuf body_buf = yap_gen_block(ctx, decl.body);
	yap_strbuf_append(&res, yap_strbuf_data(&body_buf));
	yap_strbuf_free(&body_buf);
	return res;
}

yap_strbuf yap_gen_block(yap_ctx* ctx, yap_block block){
	yap_strbuf res = yap_strbuf_newf("{\n");
	for_darr(i, stmt, block.statements){
		yap_strbuf stmt_buf = yap_gen_statement(ctx, stmt);
		yap_strbuf_appendf(&res, "%s\n", yap_strbuf_data(&stmt_buf));
		yap_strbuf_free(&stmt_buf);
	}
	yap_strbuf_append(&res, "}");
	return res;
}

yap_strbuf yap_gen_statement(yap_ctx* ctx, yap_statement stmt){
	switch(stmt.kind){
		case yap_statement_error: break; //TODO
		case yap_statement_empty:
			return yap_gen_empty_statement(ctx, stmt);
		case yap_statement_expr:
			return yap_gen_expr_statement(ctx, stmt);
		case yap_statement_var_decl:
			return yap_gen_var_decl(ctx, stmt.var_decl);
		case yap_statement_return: break; //TODO
		case yap_statement_if: break; //TODO
		case yap_statement_if_else: break; //TODO
		case yap_statement_while: break; //TODO
		case yap_statement_for: break; //TODO
		case yap_statement_break: break; //TODO
		case yap_statement_continue: break; //TODO
		case yap_statement_block:
			return yap_gen_block(ctx, stmt.block);
		default: break;
	}
	yap_log("Unsupported statement kind in yap_gen_statement: %d", stmt.kind);
	return empty_strbuf;
}

yap_strbuf yap_gen_empty_statement(yap_ctx* ctx, yap_statement stmt){
	(void)ctx;
	(void)stmt;
	return yap_strbuf_newf(";");
}

yap_strbuf yap_gen_expr_statement(yap_ctx* ctx, yap_statement stmt){
	yap_strbuf expr_buf = yap_gen_expr(ctx, stmt.expr);
	if (expr_buf.data == NULL){
		yap_log("Failed to generate expression for expression statement");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s;", yap_strbuf_data(&expr_buf));
	yap_strbuf_free(&expr_buf);
	return res;
}

yap_strbuf yap_gen_var_decl(yap_ctx* ctx, yap_var_decl var_decl){
	yap_var var = var_decl.var;
	yap_strbuf expr = yap_gen_expr(ctx, var_decl.init);
	if (!expr.data){
		yap_log("Failed to generate expression for variable declaration");
		return empty_strbuf;
	}
	yap_strbuf name_type_combo = yap_gen_name_type_id_combo(ctx, var.name, var.type);
	yap_strbuf res = yap_strbuf_newf("%s = %s;", yap_strbuf_data(&name_type_combo), yap_strbuf_data(&expr));
	yap_strbuf_free(&name_type_combo);
	yap_strbuf_free(&expr);
	return res;
}

yap_strbuf yap_gen_expr(yap_ctx* ctx, yap_expr expr){
	switch(expr.kind){
		case yap_expr_literal:
			return yap_gen_literal(ctx, expr.literal);
		case yap_expr_var:
			return yap_gen_var_access(ctx, expr);
		case yap_expr_bin:
			return yap_gen_binary_expr(ctx, expr);
		case yap_expr_assignment:
			return yap_gen_assignment(ctx, expr.assignment);
		default:
			yap_log("Unsupported expression kind in yap_gen_expr: %d", expr.kind);
			return empty_strbuf;
	}
}

yap_strbuf yap_gen_binary_expr(yap_ctx* ctx, yap_expr expr){
	yap_bin_expr bin = expr.bin_expr;
	yap_strbuf left = yap_gen_expr(ctx, *bin.left);
	yap_strbuf right = yap_gen_expr(ctx, *bin.right);
	if (left.data == NULL || right.data == NULL){
		yap_log("Failed to generate expression for binary expression");
		yap_strbuf_free(&left);
		yap_strbuf_free(&right);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s %c %s", yap_strbuf_data(&left), bin.op, yap_strbuf_data(&right));
	yap_strbuf_free(&left);
	yap_strbuf_free(&right);
	return res;
}

yap_strbuf yap_gen_var_access(yap_ctx* ctx, yap_expr expr){
	(void)ctx;
	return yap_strbuf_newf("%s", expr.var_name);
}

yap_strbuf yap_gen_assignment(yap_ctx* ctx, yap_assignment assignment){
	yap_expr l = *assignment.left;
	yap_expr r = *assignment.right;
	if (!l.is_lvalue){
		yap_log("Left side of assignment is not an lvalue");
		return yap_strbuf_empty();
	}
	yap_strbuf left = yap_gen_expr(ctx, *assignment.left);
	yap_strbuf right = yap_gen_expr(ctx, *assignment.right);
	if (left.data == NULL || right.data == NULL){
		yap_log("Failed to generate expression for assignment");
		yap_strbuf_free(&left);
		yap_strbuf_free(&right);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s %c= %s", yap_strbuf_data(&left), assignment.op, yap_strbuf_data(&right));
	yap_strbuf_free(&left);
	yap_strbuf_free(&right);
	return res;
}

yap_strbuf yap_gen_literal(yap_ctx* ctx, yap_literal literal){
	(void)ctx;
	switch(literal.kind){
		case yap_literal_numerical:
			//TODO: Finish? Errors
			return yap_strbuf_newf("%s", literal.text);
		default:
			yap_log("Unsupported literal kind in yap_gen_literal: %d", literal.kind);
			return empty_strbuf;
	}
}
