#include "yap_c.h"
#include "yap/all.h"

void yap_gen_code(yap_ctx* ctx){
    (void)ctx;
	yap_log("Hello from the backend!");
	// yap_tcc_example();
	for_darr(i, decl, ctx->source_codes[0].declarations){
		yap_strbuf decl_code = yap_gen_decl(ctx, decl);
		yap_log("decl #%ld:\n%s", i, decl_code.data);
		yap_strbuf_free(&decl_code);
	}
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
		return (yap_strbuf){.data=NULL, .len=0, .cap=0};
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
	bool last_append_was_left = false;
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
					yap_strbuf_append(&right, arg_str.data);
					yap_strbuf_free(&arg_str);
				}
				yap_strbuf_append(&right, "))");
				t = *yap_ctx_get_type(ctx, t.func.return_type);
				break;
			default:
				yap_log("Unsupported type kind in yap_gen_name_type_combo: %d", t.kind);
				return (yap_strbuf){.data=NULL, .len=0, .cap=0};
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
		yap_strbuf_append(&res, arg_buf.data);
		yap_strbuf_free(&arg_buf);
	}
	yap_strbuf_append(&res, "){\n    //TODO: Generate function body\n}");
	return res;
}

