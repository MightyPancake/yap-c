#include "yap_c.h"
#include "yap/all.h"

#define empty_strbuf yap_strbuf_empty()

void save_c_code(yap_strbuf code){
	FILE* f = fopen("out.c", "w");
	if (!f){
		yap_log("Failed to open output file");
		return;
	}
	const char preambule[] = "#include <stdint.h>\n\n";
	fwrite(preambule, 1, sizeof(preambule)-1, f);
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
	for_darr_elems(src, ctx->sources){
		size_t i = (size_t)(src - ctx->sources);
		yap_log("Running code gen for source #%ld: %s", i, src->path);
		if (i >= darr_len(ctx->source_codes)){
			yap_log("Missing source_code entry for source #%ld", i);
			continue;
		}
		for_darr(j, decl, ctx->source_codes[i].declarations){
			yap_strbuf decl_code = yap_gen_decl(ctx, src, decl);
			yap_log("decl #%ld:\n%s", j, yap_strbuf_data(&decl_code));
			// Append to result
			// TODO: We should probably accumulate total size and create the buffer once or write directly to file instead
			yap_strbuf_append(&res, yap_strbuf_data(&decl_code));
			yap_strbuf_append(&res, "\n\n");
			yap_strbuf_free(&decl_code);
		}
	}
	if (yap_ctx_dispatch_errors(ctx)){
		yap_strbuf_free(&res);
		return;
	}

	//TODO Write res to file
	save_c_code(res);
	yap_strbuf_free(&res);
}

yap_strbuf yap_gen_decl(yap_ctx* ctx, yap_source* src, yap_decl decl){
	yap_strbuf res;
	switch(decl.kind){
		case yap_decl_func:
			yap_log("Gen for function declaration: %s", decl.func_decl.name);
			res = yap_gen_fn_decl(ctx, src, decl.func_decl);
			break;
		default:
			yap_log("Unhandled declaration kind in codegen: %d", decl.kind);
			break;
	}
	return res;
}

yap_strbuf yap_gen_name_type_combo(yap_ctx* ctx, const char* name, yap_type typ){
	const char* const_prefix = typ.is_const ? "const " : "";
	switch (typ.kind){
		case yap_type_primitive: {
			const char* name_sep = (name && name[0]) ? " " : "";
			return yap_strbuf_newf("%s%s%s%s", const_prefix, typ.primitive.c_name, name_sep, name ? name : "");
		}
		case yap_type_ptr: {
			yap_type* sub = yap_ctx_get_type(ctx, typ.pointer_type);
			if (!sub){
				yap_log("Invalid pointer subtype in yap_gen_name_type_combo");
				return empty_strbuf;
			}
			yap_strbuf decorated_name = yap_strbuf_newf("*%s%s%s", typ.is_const ? " const" : "", (name && name[0]) ? " " : "", name ? name : "");
			yap_strbuf res = yap_gen_name_type_combo(ctx, yap_strbuf_data(&decorated_name), *sub);
			yap_strbuf_free(&decorated_name);
			return res;
		}
		case yap_type_func: {
			yap_type* return_type = yap_ctx_get_type(ctx, typ.func.return_type);
			if (!return_type){
				yap_log("Invalid function return type in yap_gen_name_type_combo");
				return empty_strbuf;
			}
			yap_strbuf args = yap_strbuf_new();
			for_darr(i, arg, typ.func.args){
				if (i > 0) yap_strbuf_append(&args, ", ");
				yap_type* arg_type = yap_ctx_get_type(ctx, arg);
				if (!arg_type){
					yap_log("Invalid function argument type in yap_gen_name_type_combo");
					yap_strbuf_free(&args);
					return empty_strbuf;
				}
				yap_strbuf arg_str = yap_gen_name_type_combo(ctx, NULL, *arg_type);
				if (!arg_str.data){
					yap_strbuf_free(&args);
					return empty_strbuf;
				}
				yap_strbuf_append(&args, yap_strbuf_data(&arg_str));
				yap_strbuf_free(&arg_str);
			}
			yap_strbuf decorated_name = yap_strbuf_newf("(*%s%s%s)(%s)", const_prefix, (name && name[0]) ? " " : "", name ? name : "", yap_strbuf_data(&args));
			yap_strbuf_free(&args);
			yap_strbuf res = yap_gen_name_type_combo(ctx, yap_strbuf_data(&decorated_name), *return_type);
			yap_strbuf_free(&decorated_name);
			return res;
		}
		default:
			yap_log("Unsupported type kind in yap_gen_name_type_combo: %d", typ.kind);
			return empty_strbuf;
	}
}

yap_strbuf yap_gen_name_type_id_combo(yap_ctx* ctx, const char* name, yap_type_id id){
	yap_type* typ = yap_ctx_get_type(ctx, id);
	if (!typ){
		yap_log("Invalid type id %d in yap_gen_name_type_id_combo", id);
		return empty_strbuf;
	}
	return yap_gen_name_type_combo(ctx, name, *typ);
}

yap_strbuf yap_gen_type(yap_ctx* ctx, yap_source* src, yap_type type){
	(void)src;
	return yap_gen_name_type_combo(ctx, "", type);
}

yap_strbuf yap_gen_type_id(yap_ctx* ctx, yap_source* src, yap_type_id id){
	(void)src;
	return yap_gen_name_type_id_combo(ctx, "", id);
}

yap_strbuf yap_gen_fn_decl(yap_ctx* ctx, yap_source* src, yap_func_decl decl){
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
	yap_strbuf body_buf = yap_gen_block(ctx, src, decl.body);
	yap_strbuf_append(&res, yap_strbuf_data(&body_buf));
	yap_strbuf_free(&body_buf);
	return res;
}

yap_strbuf yap_gen_block(yap_ctx* ctx, yap_source* src, yap_block block){
	if (block.kind != yap_block_valid || !block.statements){
		yap_log("Invalid block passed to codegen; skipping block generation");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("{\n");
	for_darr(i, stmt, block.statements){
		yap_strbuf stmt_buf = yap_gen_statement(ctx, src, stmt);
		yap_strbuf_appendf(&res, "%s\n", yap_strbuf_data(&stmt_buf));
		yap_strbuf_free(&stmt_buf);
	}
	yap_strbuf_append(&res, "}");
	return res;
}

yap_strbuf yap_gen_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	switch(stmt.kind){
		case yap_statement_error: break; //TODO
		case yap_statement_empty:
			return yap_gen_empty_statement(ctx, stmt);
		case yap_statement_expr:
			return yap_gen_expr_statement(ctx, src, stmt);
		case yap_statement_var_decl:
			return yap_gen_var_decl(ctx, src, stmt.var_decl);
		case yap_statement_return:
			return yap_gen_return_statement(ctx, src, stmt);
		case yap_statement_if:
			return yap_gen_if_statement(ctx, src, stmt);
		case yap_statement_if_else:
			return yap_gen_if_else_statement(ctx, src, stmt);
		case yap_statement_while:
			return yap_gen_while(ctx, src, stmt);
		case yap_statement_for: break;
			return yap_gen_for(ctx, src, stmt);
		case yap_statement_break: break;
			return yap_gen_break(ctx, src, stmt);
		case yap_statement_continue: break;
			return yap_gen_continue(ctx, src, stmt);
		case yap_statement_block:
			if (stmt.block.kind != yap_block_valid || !stmt.block.statements){
				yap_log("Invalid block statement encountered during codegen");
				return empty_strbuf;
			}
			return yap_gen_block(ctx, src, stmt.block);
		default: break;
	}
	yap_log("Unsupported statement kind in yap_gen_statement: %d", stmt.kind);
	return empty_strbuf;
}

yap_strbuf yap_gen_break(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	(void)ctx;
	(void)src;
	(void)stmt;
	return yap_strbuf_newf("break;");
}

yap_strbuf yap_gen_continue(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	(void)ctx;
	(void)src;
	(void)stmt;
	return yap_strbuf_newf("continue;");
}

yap_strbuf yap_gen_for(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	yap_for for_stmt = stmt.for_stmt;
	yap_strbuf init = yap_gen_statement(ctx, src, *for_stmt.init);
	if (!init.data){
		yap_emit_error_at(ctx, src, *(for_stmt.init), "%s", "Failed to generate code for for loop initializer");
		return empty_strbuf;
	}
	yap_strbuf condition = yap_gen_expr(ctx, src, for_stmt.condition);
	if (!condition.data){
		yap_emit_error_at(ctx, src, stmt.for_stmt.condition, "%s", "Failed to generate expression for for loop condition");
		yap_strbuf_free(&init);
		return empty_strbuf;
	}
	yap_strbuf update = yap_gen_expr(ctx, src, for_stmt.update);
	if (!update.data){
		yap_emit_error_at(ctx, src, for_stmt.update, "%s", "Failed to generate expression for for loop update");
		yap_strbuf_free(&init);
		yap_strbuf_free(&condition);
		return empty_strbuf;
	}
	yap_strbuf body = yap_gen_statement(ctx, src, *(for_stmt.body));
	if (!body.data){
		yap_emit_error_at(ctx, src, *stmt.for_stmt.body, "%s", "Failed to generate code for for loop body");
		yap_strbuf_free(&init);
		yap_strbuf_free(&condition);
		yap_strbuf_free(&update);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("for (%s %s; %s)\n%s", yap_strbuf_data(&init), yap_strbuf_data(&condition), yap_strbuf_data(&update), yap_strbuf_data(&body));
	yap_strbuf_free(&init);
	yap_strbuf_free(&condition);
	yap_strbuf_free(&update);
	yap_strbuf_free(&body);
	return res;
}

yap_strbuf yap_gen_while(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	yap_strbuf condition_buf = yap_gen_expr(ctx, src, stmt.while_stmt.condition);
	if (!condition_buf.data){
		yap_emit_error_at(ctx, src, stmt.while_stmt.condition, "%s", "Failed to generate expression for while loop condition");
		return empty_strbuf;
	}
	yap_strbuf body_buf = yap_gen_statement(ctx, src, *stmt.while_stmt.body);
	if (!body_buf.data){
		yap_emit_error_at(ctx, src, *stmt.while_stmt.body, "%s", "Failed to generate code for while loop body");
		yap_strbuf_free(&condition_buf);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("while (%s)\n%s", yap_strbuf_data(&condition_buf), yap_strbuf_data(&body_buf));
	yap_strbuf_free(&condition_buf);
	yap_strbuf_free(&body_buf);
	return res;
}

yap_strbuf yap_gen_if_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	yap_if if_stmt = stmt.if_stmt;
	yap_strbuf cond_buf = yap_gen_expr(ctx, src, if_stmt.condition);
	if (!cond_buf.data){
		yap_emit_error_at(ctx, src, if_stmt.condition, "%s", "Failed to generate expression for if statement condition");
		return empty_strbuf;
	}
	yap_statement then_branch = *(if_stmt.then_branch);
	yap_strbuf then_buf = yap_gen_statement(ctx, src, then_branch);
	if (!then_buf.data){
		yap_emit_error_at(ctx, src, then_branch, "%s", "Failed to generate code for if statement then branch");
		yap_strbuf_free(&cond_buf);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("if (%s)\n%s", yap_strbuf_data(&cond_buf), yap_strbuf_data(&then_buf));
	yap_strbuf_free(&cond_buf);
	yap_strbuf_free(&then_buf);
	return res;
}

yap_strbuf yap_gen_if_else_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	yap_if_else if_stmt = stmt.if_else_stmt;
	yap_strbuf cond_buf = yap_gen_expr(ctx, src, if_stmt.condition);
	if (!cond_buf.data){
		yap_emit_error_at(ctx, src, if_stmt.condition, "%s", "Failed to generate expression for if statement condition");
		return empty_strbuf;
	}
	//Then branch
	yap_statement then_branch = *(if_stmt.then_branch);
	yap_strbuf then_buf = yap_gen_statement(ctx, src, then_branch);
	if (!then_buf.data){
		yap_emit_error_at(ctx, src, then_branch, "%s", "Failed to generate code for if statement then branch");
		yap_strbuf_free(&cond_buf);
		return empty_strbuf;
	}

	//Else branch
	yap_statement else_branch = *(if_stmt.else_branch);
	yap_strbuf else_buf = yap_gen_statement(ctx, src, else_branch);
	if (!else_buf.data){
		yap_emit_error_at(ctx, src, else_branch, "%s", "Failed to generate code for if statement else branch");
		yap_strbuf_free(&cond_buf);
		yap_strbuf_free(&then_buf);
		yap_strbuf_free(&else_buf);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("if (%s)\n%s\nelse\n%s", yap_strbuf_data(&cond_buf), yap_strbuf_data(&then_buf), yap_strbuf_data(&else_buf));
	yap_strbuf_free(&cond_buf);
	yap_strbuf_free(&then_buf);
	yap_strbuf_free(&else_buf);
	return res;
}

yap_strbuf yap_gen_return_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	yap_return_statement ret = stmt.return_stmt;
	yap_expr expr = ret.value;
	if (expr.type == ctx->void_type_id){
		return yap_strbuf_newf("return;");
	}
	yap_strbuf expr_buf = yap_gen_expr(ctx, src, expr);
	if (expr_buf.data == NULL){
		yap_emit_error_at(ctx, src, expr, "%s", "Failed to generate expression for return statement");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("return %s;", yap_strbuf_data(&expr_buf));
	yap_strbuf_free(&expr_buf);
	return res;
}

yap_strbuf yap_gen_empty_statement(yap_ctx* ctx, yap_statement stmt){
	(void)ctx;
	(void)stmt;
	return yap_strbuf_newf(";");
}

yap_strbuf yap_gen_expr_statement(yap_ctx* ctx, yap_source* src, yap_statement stmt){
	yap_strbuf expr_buf = yap_gen_expr(ctx, src, stmt.expr);
	if (expr_buf.data == NULL){
		yap_emit_error_at(ctx, src, stmt.expr, "%s", "Failed to generate expression for expression statement");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s;", yap_strbuf_data(&expr_buf));
	yap_strbuf_free(&expr_buf);
	return res;
}

yap_strbuf yap_gen_var_decl(yap_ctx* ctx, yap_source* src, yap_var_decl var_decl){
	yap_var var = var_decl.var;
	yap_strbuf expr = yap_gen_expr(ctx, src, var_decl.init);
	if (!expr.data){
		yap_emit_error_at(ctx, src, var_decl.init, "%s", "Failed to generate expression for variable declaration");
		return empty_strbuf;
	}
	yap_strbuf name_type_combo = yap_gen_name_type_id_combo(ctx, var.name, var.type);
	yap_strbuf res = yap_strbuf_newf("%s = %s;", yap_strbuf_data(&name_type_combo), yap_strbuf_data(&expr));
	yap_strbuf_free(&name_type_combo);
	yap_strbuf_free(&expr);
	return res;
}

yap_strbuf yap_gen_expr(yap_ctx* ctx, yap_source* src, yap_expr expr){
	switch(expr.kind){
		case yap_expr_literal:
			return yap_gen_literal(ctx, src, expr);
		case yap_expr_var:
			return yap_gen_var_access(ctx, src, expr);
		case yap_expr_bin:
			return yap_gen_binary_expr(ctx, src, expr);
		case yap_expr_assignment:
			return yap_gen_assignment(ctx, src, expr);
		case yap_expr_func_call:
			return yap_gen_func_call(ctx, src, expr);
		case yap_expr_cast:
			return yap_gen_cast_expr(ctx, src, expr);
		case yap_expr_at_op:
			return yap_gen_at_op(ctx, src, expr);
		case yap_expr_paren:
			return yap_gen_paren_expr(ctx, src, expr);
		default:
			yap_emit_error_at(ctx, src, expr, "%s", "Unsupported expression kind in codegen");
			return empty_strbuf;
	}
}

yap_strbuf yap_gen_paren_expr(yap_ctx* ctx, yap_source* src, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, src, *expr.subexpr);
	yap_strbuf res = yap_strbuf_newf("(%s)", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_at_op(yap_ctx* ctx, yap_source* src, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, src, *expr.subexpr);
	yap_strbuf res = yap_strbuf_newf("(&(%s))", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_cast_expr(yap_ctx* ctx, yap_source* src, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, src, *(expr.subexpr));
	yap_strbuf typ = yap_gen_type_id(ctx, src, expr.type);
	yap_strbuf res = yap_strbuf_newf("((%s)(%s))", yap_strbuf_data(&typ), yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	yap_strbuf_free(&typ);
	return res;
}

yap_strbuf yap_gen_func_call(yap_ctx* ctx, yap_source* src, yap_expr expr){
	yap_func_call func_call = expr.func_call;
	yap_strbuf res = yap_gen_expr(ctx, src, *(func_call.func_expr));
	yap_strbuf_append(&res, "(");
	for_darr(i, arg, func_call.params){
		if (i > 0) yap_strbuf_append(&res, ", ");
		yap_strbuf arg_buf = yap_gen_expr(ctx, src, arg);
		if (!arg_buf.data){
			yap_emit_error_at(ctx, src, arg, "%s", "Failed to generate expression for function call argument");
			yap_strbuf_free(&res);
			return empty_strbuf;
		}
		yap_strbuf_append(&res, yap_strbuf_data(&arg_buf));
		yap_strbuf_free(&arg_buf);
	}
	yap_strbuf_append(&res, ")");
	return res;
}

yap_strbuf yap_gen_binary_expr(yap_ctx* ctx, yap_source* src, yap_expr expr){
	yap_bin_expr bin = expr.bin_expr;
	yap_strbuf left = yap_gen_expr(ctx, src, *bin.left);
	yap_strbuf right = yap_gen_expr(ctx, src, *bin.right);
	if (left.data == NULL || right.data == NULL){
		yap_emit_error_at(ctx, src, expr, "%s", "Failed to generate expression for binary expression");
		yap_strbuf_free(&left);
		yap_strbuf_free(&right);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s %c %s", yap_strbuf_data(&left), bin.op, yap_strbuf_data(&right));
	yap_strbuf_free(&left);
	yap_strbuf_free(&right);
	return res;
}

yap_strbuf yap_gen_var_access(yap_ctx* ctx, yap_source* src, yap_expr expr){
	(void)ctx;
	(void)src;
	return yap_strbuf_newf("%s", expr.var_name);
}

yap_strbuf yap_gen_assignment(yap_ctx* ctx, yap_source* src, yap_expr expr){
	yap_assignment assignment = expr.assignment;
	yap_code_range range = expr.range;
	yap_expr l = *assignment.left;
	yap_expr r = *assignment.right;
	(void)r;
	if (!l.is_lvalue){
		yap_emit_error_rangef(ctx, src, range, "%s", "Left side of assignment is not an lvalue");
		return yap_strbuf_empty();
	}
	yap_strbuf left = yap_gen_expr(ctx, src, *assignment.left);
	yap_strbuf right = yap_gen_expr(ctx, src, *assignment.right);
	if (left.data == NULL || right.data == NULL){
		yap_emit_error_rangef(ctx, src, range, "%s", "Failed to generate expression for assignment");
		yap_strbuf_free(&left);
		yap_strbuf_free(&right);
		return empty_strbuf;
	}

	yap_strbuf res;
	if (assignment.op == '='){
		res = yap_strbuf_newf("%s = %s", yap_strbuf_data(&left), yap_strbuf_data(&right));
	} else {
		 res = yap_strbuf_newf("%s %c= %s", yap_strbuf_data(&left), assignment.op, yap_strbuf_data(&right));
	}
	yap_strbuf_free(&left);
	yap_strbuf_free(&right);
	return res;
}

yap_strbuf yap_gen_literal(yap_ctx* ctx, yap_source* src, yap_expr expr){
	(void)ctx;
	yap_literal literal = expr.literal;
	switch(literal.kind){
		case yap_literal_numerical:
			//TODO: Finish? Errors
			return yap_strbuf_newf("%s", literal.text);
		default:
			yap_emit_error_at(ctx, src, expr, "%s", "Unsupported literal kind in codegen");
			return empty_strbuf;
	}
}
