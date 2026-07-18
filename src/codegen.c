#include "yap_c.h"
#include "yap/all.h"

#define empty_strbuf yap_strbuf_empty()

yap_strbuf yap_gen_index_access(yap_ctx* ctx, yap_loc loc, yap_expr expr);
static yap_strbuf yap_gen_func_body(yap_ctx* ctx, yap_loc loc, yap_block block, const char* prelude);

static int yap_c_resolve_opt_level(yap_ctx* ctx){
	int level = 0;
	if (!ctx->args) return level;
	for_darr(i, flag, ctx->args->backend_flags){
		if (!flag || flag[0] != 'O') continue;
		if (flag[1] < '0' || flag[1] > '3' || flag[2] != '\0'){
			yap_emit_error_no_pos(ctx, "Invalid backend flag '-b%s' (expected -bO0, -bO1, -bO2, or -bO3)", flag);
			continue;
		}
		level = flag[1] - '0';
	}
	return level;
}

static const char* yap_c_resolve_compiler(yap_ctx* ctx){
	const char* compiler = "gcc";
	if (!ctx->args) return compiler;
	for_darr(i, flag, ctx->args->backend_flags){
		if (!flag || strncmp(flag, "cc=", 3) != 0) continue;
		if (flag[3] == '\0'){
			yap_emit_error_no_pos(ctx, "Invalid backend flag '-b%s' (expected -bcc=<compiler>, e.g. -bcc=clang)", flag);
			continue;
		}
		compiler = flag + 3;
	}
	return compiler;
}

static bool yap_c_has_backend_flag(yap_ctx* ctx, const char* name){
	if (!ctx->args) return false;
	for_darr(i, flag, ctx->args->backend_flags){
		if (flag && strcmp(flag, name) == 0) return true;
	}
	return false;
}

static yap_strbuf yap_c_resolve_extra_cflags(yap_ctx* ctx){
	yap_strbuf extra = yap_strbuf_empty();
	if (!ctx->args) return extra;
	for_darr(i, flag, ctx->args->backend_flags){
		if (!flag || strncmp(flag, "f=", 2) != 0) continue;
		if (flag[2] == '\0'){
			yap_emit_error_no_pos(ctx, "Invalid backend flag '-b%s' (expected -bf=<cflag>, e.g. -bf=-Wall)", flag);
			continue;
		}
		yap_strbuf_appendf(&extra, " \"%s\"", flag + 2);
	}
	return extra;
}

static const yap_flag_desc yap_c_flag_descriptions[] = {
	{ "O0", "No optimization (default)" },
	{ "O1", "Light optimization" },
	{ "O2", "Moderate optimization" },
	{ "O3", "Aggressive optimization" },
	{ "c", "Stop after emitting C; copy the generated sources to ./out instead of compiling" },
	{ "cc=<compiler>", "Select the C compiler for the final build (gcc, clang, tcc supported; default gcc)" },
	{ "f=<cflag>", "Forward a raw flag directly to the underlying C compiler (repeatable), e.g. -bf=-Wall" },
};

const yap_flag_desc* yap_describe_flags(int* count){
	*count = sizeof(yap_c_flag_descriptions) / sizeof(yap_c_flag_descriptions[0]);
	return yap_c_flag_descriptions;
}

yap_ctx* yap_emit(yap_ctx* ctx){
	yap_log("Emission phase");

	yap_module* mod = yap_ctx_current_module(ctx);
	if (!mod){
		yap_log("No current module - nothing to emit");
		return ctx;
	}

	// Initialize module if not already done
	if (!mod->module_ctx)
		yap_c_init_module(mod);

	yap_module_c_code* mod_code = mod->module_ctx;

	// Flush all file handles before TCC or gcc touch the files
	if (mod_code->types_fp) fflush(mod_code->types_fp);
	if (mod_code->decls_fp) fflush(mod_code->decls_fp);
	if (mod_code->impl_fp)  fflush(mod_code->impl_fp);

	if (yap_c_has_backend_flag(ctx, "c")){
		yap_log("Stopping at emitted C (-bc): copying %s to ./out", mod_code->out_dir);
		if (yap_copy_dir_recursive(mod_code->out_dir, "out") != 0)
			yap_emit_error_no_pos(ctx, "Failed to copy emitted C files to ./out");
		else
			yap_log("Emitted C files copied to ./out");
		yap_c_free_module(mod);
		return ctx;
	}

	// TCC-based main check (verifies our recompile pipeline works); debug/log builds only
#ifdef YAP_LOG
	yap_tcc_check_main(ctx);
#endif

	// Collect module library flags for linking
	yap_strbuf lib_flags = yap_strbuf_empty();
	{
		void* item;
		size_t iter = 0;
		while (hashmap_iter(ctx->modules, &iter, &item)) {
			yap_module* m = item;
			if (!m->lib_paths) continue;
			for_darr(li, lp, m->lib_paths) {
				yap_strbuf_appendf(&lib_flags, " \"%s\"", lp);
			}
		}
	}

	char cmd[YAP_PATH_MAX * 4];
	const char *out_name = (ctx->args && ctx->args->output_file) ? ctx->args->output_file : "a.out";
	const char* compiler = yap_c_resolve_compiler(ctx);
	int opt_level = yap_c_resolve_opt_level(ctx);
	yap_strbuf extra_cflags = yap_c_resolve_extra_cflags(ctx);
	// -fno-semantic-interposition: under -fPIC, GCC's semantic interposition blocks inlining between generated (non-static) functions -- measured 1.3x-2.2x slowdown; no-op for tcc, supported by clang.
	snprintf(cmd, sizeof(cmd), "%s -fno-semantic-interposition -O%d%s %s/impl.c -o %s%s -lm 2>&1", compiler, opt_level,
		extra_cflags.data ? yap_strbuf_data(&extra_cflags) : "",
		mod_code->out_dir, out_name,
		lib_flags.data ? yap_strbuf_data(&lib_flags) : "");
	yap_strbuf_free(&extra_cflags);
	yap_strbuf_free(&lib_flags);
	yap_log("Compiling: %s", cmd);

	// Capture gcc/ld output instead of streaming it, so a successful non-debug build stays quiet; surfaced only on failure or under YAP_LOG.
	FILE* gcc_proc = popen(cmd, "r");
	yap_strbuf gcc_output = yap_strbuf_empty();
	int ret = -1;
	if (gcc_proc) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), gcc_proc)) > 0)
			yap_strbuf_appendn(&gcc_output, buf, n);
		ret = pclose(gcc_proc);
	}

#ifdef YAP_LOG
	if (gcc_output.len) fputs(yap_strbuf_data(&gcc_output), stdout);
#else
	if (ret != 0 && gcc_output.len) fputs(yap_strbuf_data(&gcc_output), stdout);
#endif

	if (ret != 0) {
		yap_log("%s COMPILATION FAILED (exit code %d). Run: %s %s/impl.c -o %s", compiler, ret, compiler, mod_code->out_dir, out_name);
		yap_emit_error_no_pos(ctx, "%s compilation failed (exit code %d)", compiler, ret);
	} else {
		yap_log("Compilation succeeded, binary at %s", out_name);

		if (ctx->args && ctx->args->run){
			int run_ret = yap_c_run_from_files(ctx, mod);
			if (run_ret < 0)
				yap_emit_error_no_pos(ctx, "Failed to run program via TCC");
			else
				ctx->run_exit_code = run_ret;
		}
	}
	yap_strbuf_free(&gcc_output);

	// Close file handles and free module
	yap_c_free_module(mod);
	return ctx;
}

void yap_gen_source(yap_ctx* ctx, yap_source* src){
	(void)ctx;
	yap_log("Running codegen for source: %s", src->identity ? src->identity : "(unknown)");
	if (!src || !src->source_node){
		yap_log("Invalid source for codegen");
		return;
	}
}

void yap_gen_decl(yap_ctx* ctx, yap_decl decl){
	yap_module* module = yap_ctx_current_module(ctx);
	if (!module) return;

	// Lazy-init module context on first gen_decl call
	if (!module->module_ctx)
		yap_c_init_module(module);

	yap_module_c_code* mod_code = module->module_ctx;
	yap_strbuf res = empty_strbuf;
	yap_loc loc = decl.loc;

	// Advance the logical clock for this declaration
	mod_code->clock++;

	switch(decl.kind){
		case yap_decl_func_decl: {
			yap_log("Gen for function declaration (no body): %s", decl.func_decl.name);
			yap_strbuf proto = yap_gen_func_decl(ctx, loc, decl.func_decl, false, decl.module_prefix);
			if (proto.data && proto.len > 0 && mod_code->decls_fp){
				fputs(yap_strbuf_data(&proto), mod_code->decls_fp);
				fputc('\n', mod_code->decls_fp);
				fflush(mod_code->decls_fp);
			}
			yap_strbuf_free(&proto);
			break;
		}
		case yap_decl_func_def: {
			yap_log("Gen for function definition: %s", decl.func_decl.name);

			// Hoisted function literals are TU-internal: static linkage, no symbol table entry
			bool is_anon_func = decl.func_decl.name
				&& strncmp(decl.func_decl.name, "__anon_func_", 12) == 0;

			// Generate prototype → write to prototypes.h
			yap_strbuf proto = yap_gen_func_decl(ctx, loc, decl.func_decl, false, decl.module_prefix);
			if (proto.data && proto.len > 0 && mod_code->decls_fp){
				if (is_anon_func) fputs("static ", mod_code->decls_fp);
				fputs(yap_strbuf_data(&proto), mod_code->decls_fp);
				fputc('\n', mod_code->decls_fp);
				fflush(mod_code->decls_fp);
			}
			yap_strbuf_free(&proto);

			// Generate definition → write to impl.c
			res = yap_gen_func_definition(ctx, loc, decl);
			if (res.data && res.len > 0 && mod_code->impl_fp){
				if (is_anon_func) fputs("static ", mod_code->impl_fp);
				fputs(yap_strbuf_data(&res), mod_code->impl_fp);
				fputc('\n', mod_code->impl_fp);
				fflush(mod_code->impl_fp);
			}
			yap_strbuf_free(&res);
			break;
		}
		case yap_decl_named_type:
			yap_log("Gen for named type declaration: %s", decl.named_type_decl.name);
			res = yap_gen_type_decl(ctx, loc, decl);
			if (res.data && res.len > 0 && mod_code->types_fp){
				fputs(yap_strbuf_data(&res), mod_code->types_fp);
				fputc('\n', mod_code->types_fp);
				fflush(mod_code->types_fp);
			}
			yap_strbuf_free(&res);
			break;
		default:
			yap_log("Unhandled declaration kind in codegen: %d", decl.kind);
			break;
	}
}

yap_strbuf yap_gen_type_decl(yap_ctx* ctx, yap_loc src_loc, yap_decl decl){
	yap_named_type_decl ntd = decl.named_type_decl;
	if (ntd.kind == yap_named_type_decl_error){
		yap_log("Invalid named type declaration in codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Invalid named type declaration");
		return empty_strbuf;
	}
	if (ntd.name && ntd.name[0] == '_' && ntd.name[1] == '_'){
		yap_log("Skipping C-reserved type '%s' in codegen", ntd.name);
		return empty_strbuf;
	}
	switch (ntd.kind){
		case yap_named_type_decl_struct: {
			return yap_gen_struct_declaration(ctx, src_loc, decl);
		}
		case yap_named_type_decl_enum: {
			return yap_gen_enum_declaration(ctx, src_loc, decl);
		}
		case yap_named_type_decl_union: {
			return yap_gen_union_declaration(ctx, src_loc, decl);
		}
		case yap_named_type_decl_alias: {
			return yap_strbuf_newf("typedef struct %s %s;", ntd.name, ntd.name);
		}
		default:
			yap_log("Unhandled named type  kind in codegen: %ddeclaration", ntd.kind);
			yap_emit_error_at(ctx, src_loc, decl, "Unhandled named type declaration kind in codegen");
			return empty_strbuf;
	}
}

yap_strbuf yap_gen_struct_declaration(yap_ctx* ctx, yap_loc src_loc, yap_decl decl){
	yap_named_type_decl ntd = decl.named_type_decl;
	yap_type* t = yap_ctx_get_type(ctx, ntd.type_id);
	if (!t){
		yap_log("Invalid type id in named type declaration codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Invalid type id in named type declaration");
		return empty_strbuf;
	}
	if (t->kind != yap_type_struct){
		yap_log("Expected struct type in named struct declaration codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Expected struct type in named struct declaration codegen");
		return empty_strbuf;
	}
	yap_struct_type st = t->structure;
	yap_strbuf res = yap_strbuf_newf("typedef struct %s %s;\nstruct %s {\n", ntd.name, ntd.name, ntd.name);
	for_darr(i, field, st.fields){
		yap_type* field_type = yap_ctx_get_type(ctx, field.type);
			if (!field_type){
				yap_log("Invalid field type id in named struct declaration codegen");
				yap_emit_error_at(ctx, src_loc, decl, "Invalid field type id in named struct declaration codegen");
			yap_strbuf_free(&res);
			return empty_strbuf;
		}
		yap_strbuf field_buf = yap_gen_name_type_combo(ctx, field.name, *field_type);
		yap_strbuf_appendf(&res, "%s;\n", yap_strbuf_data(&field_buf));
		yap_strbuf_free(&field_buf);
	}
	yap_strbuf_appendf(&res, "};");
	return res;
}

yap_strbuf yap_gen_enum_declaration(yap_ctx* ctx, yap_loc src_loc, yap_decl decl){
	yap_named_type_decl ntd = decl.named_type_decl;
	yap_type* t = yap_ctx_get_type(ctx, ntd.type_id);
	if (!t){
		yap_log("Invalid type id in named enum declaration codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Invalid type id in named enum declaration");
		return empty_strbuf;
	}
	if (t->kind != yap_type_enum){
		yap_log("Expected enum type in named enum declaration codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Expected enum type in named enum declaration codegen");
		return empty_strbuf;
	}
	yap_enum_type et = t->enumeration;
	yap_strbuf res = yap_strbuf_newf("typedef enum %s %s;\nenum %s {\n", ntd.name, ntd.name, ntd.name);
	for_darr(i, variant, et.variants){
		if (i > 0) yap_strbuf_append(&res, ",\n");
		yap_strbuf_appendf(&res, "    %s", variant.name);
		if (variant.value){
			yap_strbuf value_buf = yap_gen_expr(ctx, src_loc, *variant.value);
			yap_strbuf_appendf(&res, " = %s", yap_strbuf_data(&value_buf));
			yap_strbuf_free(&value_buf);
		}
	}
	yap_strbuf_appendf(&res, "\n};");
	return res;
}

yap_strbuf yap_gen_union_declaration(yap_ctx* ctx, yap_loc src_loc, yap_decl decl){
	yap_named_type_decl ntd = decl.named_type_decl;
	yap_type* t = yap_ctx_get_type(ctx, ntd.type_id);
	if (!t){
		yap_log("Invalid type id in named union declaration codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Invalid type id in named union declaration");
		return empty_strbuf;
	}
	if (t->kind != yap_type_union){
		yap_log("Expected union type in named union declaration codegen");
		yap_emit_error_at(ctx, src_loc, decl, "Expected union type in named union declaration codegen");
		return empty_strbuf;
	}
	yap_union_type ut = t->uni;
	yap_strbuf res = yap_strbuf_newf("typedef union %s %s;\nunion %s {\n", ntd.name, ntd.name, ntd.name);
	for_darr(i, variant, ut.variants){
		yap_type* variant_type = yap_ctx_get_type(ctx, variant.type);
			if (!variant_type){
				yap_log("Invalid variant type id in named union declaration codegen");
				yap_emit_error_at(ctx, src_loc, decl, "Invalid variant type id in named union declaration codegen");
			yap_strbuf_free(&res);
			return empty_strbuf;
		}
		yap_strbuf variant_buf = yap_gen_name_type_combo(ctx, variant.name, *variant_type);
		yap_strbuf_appendf(&res, "    %s;\n", yap_strbuf_data(&variant_buf));
		yap_strbuf_free(&variant_buf);
	}
	yap_strbuf_appendf(&res, "};");
	return res;
}

yap_strbuf yap_gen_name_type_combo(yap_ctx* ctx, const char* name, yap_type typ){
	yap_strbuf res;
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
			res = yap_gen_name_type_combo(ctx, yap_strbuf_data(&decorated_name), *return_type);
			yap_strbuf_free(&decorated_name);
			return res;
		}
		case yap_type_struct:
			yap_struct_type st = typ.structure;
			if (st.name){
				const char* name_sep = (name && name[0]) ? " " : "";
				return yap_strbuf_newf("%s%s%s%s", const_prefix, st.name, name_sep, name ? name : "");
			}else{
				res = yap_strbuf_newf("%sstruct {\n", const_prefix);
				for_darr(i, field, st.fields){
					yap_type* field_type = yap_ctx_get_type(ctx, field.type);
					if (!field_type){
						yap_log("Invalid field type id in yap_gen_name_type_combo struct generation");
						yap_strbuf_free(&res);
						return empty_strbuf;
					}
					yap_strbuf field_buf = yap_gen_name_type_combo(ctx, field.name, *field_type);
					yap_strbuf_append(&res, yap_strbuf_data(&field_buf));
					yap_strbuf_append(&res, ";\n");
					yap_strbuf_free(&field_buf);
				}
				yap_strbuf_append(&res, "}");
				if (name && name[0]) yap_strbuf_appendf(&res, " %s", name);
				return res;
			}
			break;
		case yap_type_union: {
			yap_union_type ut = typ.uni;
			if (ut.name){
				const char* name_sep = (name && name[0]) ? " " : "";
				return yap_strbuf_newf("%s%s%s%s", const_prefix, ut.name, name_sep, name ? name : "");
			}
			res = yap_strbuf_newf("%sunion {\n", const_prefix);
			for_darr(i, variant, ut.variants){
				yap_type* variant_type = yap_ctx_get_type(ctx, variant.type);
				if (!variant_type){
					yap_log("Invalid variant type id in yap_gen_name_type_combo union generation");
					yap_strbuf_free(&res);
					return empty_strbuf;
				}
				yap_strbuf variant_buf = yap_gen_name_type_combo(ctx, variant.name, *variant_type);
				yap_strbuf_append(&res, yap_strbuf_data(&variant_buf));
				yap_strbuf_append(&res, ";\n");
				yap_strbuf_free(&variant_buf);
			}
			yap_strbuf_append(&res, "}");
			if (name && name[0]) yap_strbuf_appendf(&res, " %s", name);
			return res;
		}
		case yap_type_enum: {
			yap_enum_type et = typ.enumeration;
			if (et.name){
				const char* name_sep = (name && name[0]) ? " " : "";
				return yap_strbuf_newf("%s%s%s%s", const_prefix, et.name, name_sep, name ? name : "");
			}
			res = yap_strbuf_newf("%senum { ", const_prefix);
			for_darr(i, variant, et.variants){
				if (i > 0) yap_strbuf_append(&res, ", ");
				yap_strbuf_appendf(&res, "%s", variant.name);
			}
			yap_strbuf_append(&res, " }");
			if (name && name[0]) yap_strbuf_appendf(&res, " %s", name);
			return res;
		}
		case yap_type_array: {
			yap_type* elem = yap_ctx_get_type(ctx, typ.array.element_type);
			if (!elem) return empty_strbuf;
			yap_strbuf decorated = yap_strbuf_newf("%s[%zu]", name ? name : "", typ.array.size);
			res = yap_gen_name_type_combo(ctx, yap_strbuf_data(&decorated), *elem);
			yap_strbuf_free(&decorated);
			return res;
		}
		case yap_type_slice: {
			yap_type* elem = yap_ctx_get_type(ctx, typ.slice.element_type);
			if (!elem) return empty_strbuf;
			/* yExprList as a declared param type gets codegen'd twice (proto + def); reuse the stable yExprList typedef instead of an anonymous struct, since two separately-emitted anon structs aren't compatible C types even with identical fields. */
			if (typ.slice.element_type == ctx->yexpr_type_id){
				return (name && name[0]) ? yap_strbuf_newf("yExprList %s", name) : yap_strbuf_newf("yExprList");
			}
			yap_strbuf elem_str = yap_gen_name_type_combo(ctx, NULL, *elem);
			res = yap_strbuf_newf("struct { %s* data; unsigned long len; }", yap_strbuf_data(&elem_str));
			yap_strbuf_free(&elem_str);
			if (name && name[0]) yap_strbuf_appendf(&res, " %s", name);
			return res;
		}
		case yap_type_hole:
			yap_log("Internal error: unfilled type hole '$%s' reached codegen (should have been substituted via :fill_type() before :finish())", typ.hole_name);
			return empty_strbuf;
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

yap_strbuf yap_gen_type(yap_ctx* ctx, yap_loc loc, yap_type type){
	(void)loc;
	return yap_gen_name_type_combo(ctx, "", type);
}

yap_strbuf yap_gen_type_id(yap_ctx* ctx, yap_loc loc, yap_type_id id){
	(void)loc;
	return yap_gen_name_type_id_combo(ctx, "", id);
}

yap_strbuf yap_gen_func_decl(yap_ctx* ctx, yap_loc loc, yap_func_decl decl, bool gen_definition, const char* module_prefix){
	(void)decl;
	bool is_main = strcmp(decl.name, "main") == 0;
	const char* emit_name = decl.name;
	const char* prefix = module_prefix;
	if (!prefix){
		yap_module* cur_mod = yap_ctx_current_module(ctx);
		if (cur_mod) prefix = cur_mod->prefix;
	}
	if (prefix && prefix[0] && !is_main) {
		emit_name = yap_ctx_strus_newf(ctx, "%s%s", prefix, decl.name);
	}
	yap_strbuf res = yap_gen_name_type_id_combo(ctx, NULL, decl.ret_typ);
	yap_strbuf_appendf(&res, " %s(", emit_name);
	// main's C entry point always takes (argc, argv); a yap 'byte@[] args' param is bound from them in the body prelude instead.
	if (is_main){
		yap_strbuf_append(&res, "int argc, char** argv");
	}else{
		for_darr(i, arg, decl.args){
			if (i > 0) yap_strbuf_append(&res, ", ");
			yap_strbuf arg_buf = yap_gen_name_type_id_combo(ctx, arg.name, arg.type);
			yap_strbuf_append(&res, yap_strbuf_data(&arg_buf));
			yap_strbuf_free(&arg_buf);
		}
	}
	yap_strbuf_append(&res, ")");
	if (gen_definition){
		yap_strbuf prelude_buf = empty_strbuf;
		bool has_prelude = is_main && darr_len(decl.args) == 1;
		if (has_prelude){
			yap_func_arg arg0 = decl.args[0];
			yap_strbuf arg_type_buf = yap_gen_name_type_id_combo(ctx, arg0.name, arg0.type);
			prelude_buf = yap_strbuf_newf("%s = { .data = argv, .len = (unsigned long)argc };\n",
				yap_strbuf_data(&arg_type_buf));
			yap_strbuf_free(&arg_type_buf);
		}
		yap_strbuf body_buf = yap_gen_func_body(ctx, loc, decl.body,
			has_prelude ? yap_strbuf_data(&prelude_buf) : NULL);
		if (has_prelude) yap_strbuf_free(&prelude_buf);
		yap_strbuf_append(&res, yap_strbuf_data(&body_buf));
		yap_strbuf_free(&body_buf);
	}else{
		yap_strbuf_append(&res, ";");
	}
	return res;
}

yap_strbuf yap_gen_func_definition(yap_ctx* ctx, yap_loc loc, yap_decl decl){
	return yap_gen_func_decl(ctx, loc, decl.func_decl, true, decl.module_prefix);
}

static yap_strbuf yap_gen_func_body(yap_ctx* ctx, yap_loc loc, yap_block block, const char* prelude){
	if (block.kind != yap_block_valid || !block.statements){
		yap_log("Invalid block passed to codegen; skipping block generation");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("{\n");
	if (prelude) yap_strbuf_append(&res, prelude);
	for_darr(i, stmt, block.statements){
		yap_strbuf stmt_buf = yap_gen_statement(ctx, loc, stmt);
		yap_strbuf_appendf(&res, "%s\n", yap_strbuf_data(&stmt_buf));
		yap_strbuf_free(&stmt_buf);
	}
	yap_strbuf_append(&res, "}");
	return res;
}

yap_strbuf yap_gen_block(yap_ctx* ctx, yap_loc loc, yap_block block){
	return yap_gen_func_body(ctx, loc, block, NULL);
}

yap_strbuf yap_gen_statement(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	switch(stmt.kind){
		case yap_statement_error: break; //TODO
		case yap_statement_empty:
			return yap_gen_empty_statement(ctx, stmt);
		case yap_statement_expr:
			return yap_gen_expr_statement(ctx, loc, stmt);
		case yap_statement_var_decl:
			return yap_gen_var_decl(ctx, loc, stmt.var_decl);
		case yap_statement_return:
			return yap_gen_return_statement(ctx, loc, stmt);
		case yap_statement_if:
			return yap_gen_if_statement(ctx, loc, stmt);
		case yap_statement_if_else:
			return yap_gen_if_else_statement(ctx, loc, stmt);
		case yap_statement_while:
			return yap_gen_while(ctx, loc, stmt);
		case yap_statement_for:
			return yap_gen_for(ctx, loc, stmt);
		case yap_statement_break:
			return yap_gen_break(ctx, loc, stmt);
		case yap_statement_continue:
			return yap_gen_continue(ctx, loc, stmt);
		case yap_statement_block:
			if (stmt.block.kind != yap_block_valid || !stmt.block.statements){
				yap_log("Invalid block statement encountered during codegen");
				return empty_strbuf;
			}
			return yap_gen_block(ctx, loc, stmt.block);
		default: break;
	}
	yap_log("Unsupported statement kind in yap_gen_statement: %d", stmt.kind);
	return empty_strbuf;
}

yap_strbuf yap_gen_break(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	(void)ctx;
	(void)loc;
	(void)stmt;
	return yap_strbuf_newf("break;");
}

yap_strbuf yap_gen_continue(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	(void)ctx;
	(void)loc;
	(void)stmt;
	return yap_strbuf_newf("continue;");
}

yap_strbuf yap_gen_for(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	yap_for for_stmt = stmt.for_stmt;
	yap_strbuf init = yap_gen_statement(ctx, loc, *for_stmt.init);
	if (!init.data){
			yap_emit_error_at(ctx, loc, *(for_stmt.init), "%s", "Failed to generate code for for loop initializer");
		return empty_strbuf;
	}
	yap_strbuf condition = yap_gen_expr(ctx, loc, for_stmt.condition);
	if (!condition.data){
		yap_emit_error_at(ctx, loc, stmt.for_stmt.condition, "%s", "Failed to generate expression for for loop condition");
		yap_strbuf_free(&init);
		return empty_strbuf;
	}
	yap_strbuf update = yap_gen_expr(ctx, loc, for_stmt.update);
	if (!update.data){
		yap_emit_error_at(ctx, loc, for_stmt.update, "%s", "Failed to generate expression for for loop update");
		yap_strbuf_free(&init);
		yap_strbuf_free(&condition);
		return empty_strbuf;
	}
	yap_strbuf body = yap_gen_statement(ctx, loc, *(for_stmt.body));
	if (!body.data){
		yap_emit_error_at(ctx, loc, *stmt.for_stmt.body, "%s", "Failed to generate code for for loop body");
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

yap_strbuf yap_gen_while(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	yap_strbuf condition_buf = yap_gen_expr(ctx, loc, stmt.while_stmt.condition);
	if (!condition_buf.data){
		yap_emit_error_at(ctx, loc, stmt.while_stmt.condition, "%s", "Failed to generate expression for while loop condition");
		return empty_strbuf;
	}
	yap_strbuf body_buf = yap_gen_statement(ctx, loc, *stmt.while_stmt.body);
	if (!body_buf.data){
		yap_emit_error_at(ctx, loc, *stmt.while_stmt.body, "%s", "Failed to generate code for while loop body");
		yap_strbuf_free(&condition_buf);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("while (%s)\n%s", yap_strbuf_data(&condition_buf), yap_strbuf_data(&body_buf));
	yap_strbuf_free(&condition_buf);
	yap_strbuf_free(&body_buf);
	return res;
}

yap_strbuf yap_gen_if_statement(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	yap_if if_stmt = stmt.if_stmt;
	yap_strbuf cond_buf = yap_gen_expr(ctx, loc, if_stmt.condition);
	if (!cond_buf.data){
		yap_emit_error_at(ctx, loc, if_stmt.condition, "%s", "Failed to generate expression for if statement condition");
		return empty_strbuf;
	}
	yap_statement then_branch = *(if_stmt.then_branch);
	yap_strbuf then_buf = yap_gen_statement(ctx, loc, then_branch);
	if (!then_buf.data){
		yap_emit_error_at(ctx, loc, then_branch, "%s", "Failed to generate code for if statement then branch");
		yap_strbuf_free(&cond_buf);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("if (%s)\n%s", yap_strbuf_data(&cond_buf), yap_strbuf_data(&then_buf));
	yap_strbuf_free(&cond_buf);
	yap_strbuf_free(&then_buf);
	return res;
}

yap_strbuf yap_gen_if_else_statement(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	yap_if_else if_stmt = stmt.if_else_stmt;
	yap_strbuf cond_buf = yap_gen_expr(ctx, loc, if_stmt.condition);
		if (!cond_buf.data){
		yap_emit_error_at(ctx, loc, if_stmt.condition, "%s", "Failed to generate expression for if statement condition");
		return empty_strbuf;
	}
	//Then branch
	yap_statement then_branch = *(if_stmt.then_branch);
	yap_strbuf then_buf = yap_gen_statement(ctx, loc, then_branch);
	if (!then_buf.data){
		yap_emit_error_at(ctx, loc, then_branch, "%s", "Failed to generate code for if statement then branch");
		yap_strbuf_free(&cond_buf);
		return empty_strbuf;
	}

	//Else branch
	yap_statement else_branch = *(if_stmt.else_branch);
	yap_strbuf else_buf = yap_gen_statement(ctx, loc, else_branch);
	if (!else_buf.data){
		yap_emit_error_at(ctx, loc, else_branch, "%s", "Failed to generate code for if statement else branch");
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

yap_strbuf yap_gen_return_statement(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	yap_return_statement ret = stmt.return_stmt;
	yap_expr expr = ret.value;
	if (expr.type == ctx->void_type_id){
		return yap_strbuf_newf("return;");
	}
	yap_strbuf expr_buf = yap_gen_expr(ctx, loc, expr);
	if (expr_buf.data == NULL){
		yap_emit_error_at(ctx, loc, expr, "%s", "Failed to generate expression for return statement");
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

yap_strbuf yap_gen_expr_statement(yap_ctx* ctx, yap_loc loc, yap_statement stmt){
	yap_strbuf expr_buf = yap_gen_expr(ctx, loc, stmt.expr);
	if (expr_buf.data == NULL){
		yap_emit_error_at(ctx, loc, stmt.expr, "%s", "Failed to generate expression for expression statement");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s;", yap_strbuf_data(&expr_buf));
	yap_strbuf_free(&expr_buf);
	return res;
}

yap_strbuf yap_gen_var_decl(yap_ctx* ctx, yap_loc loc, yap_var_decl var_decl){
	yap_var var = var_decl.var;
	yap_strbuf name_type_combo = yap_gen_name_type_id_combo(ctx, var.name, var.type);
	if (!var_decl.has_init){
		yap_strbuf res = yap_strbuf_newf("%s;", yap_strbuf_data(&name_type_combo));
		yap_strbuf_free(&name_type_combo);
		return res;
	}
	yap_strbuf expr = yap_gen_expr(ctx, loc, var_decl.init);
	if (!expr.data){
		yap_emit_error_at(ctx, loc, var_decl.init, "%s", "Failed to generate expression for variable declaration");
		yap_strbuf_free(&name_type_combo);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s = %s;", yap_strbuf_data(&name_type_combo), yap_strbuf_data(&expr));
	yap_strbuf_free(&name_type_combo);
	yap_strbuf_free(&expr);
	return res;
}

yap_strbuf yap_gen_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	switch(expr.kind){
		case yap_expr_literal:
			return yap_gen_literal(ctx, loc, expr);
		case yap_expr_var:
			return yap_gen_var_access(ctx, loc, expr);
		case yap_expr_bin:
			return yap_gen_binary_expr(ctx, loc, expr);
		case yap_expr_unary:
			return yap_gen_unary_expr(ctx, loc, expr);
		case yap_expr_assignment:
			return yap_gen_assignment(ctx, loc, expr);
		case yap_expr_func_call:
			return yap_gen_func_call(ctx, loc, expr);
		case yap_expr_cast:
			return yap_gen_cast_expr(ctx, loc, expr);
		case yap_expr_at_op:
			return yap_gen_at_op(ctx, loc, expr);
		case yap_expr_paren:
			return yap_gen_paren_expr(ctx, loc, expr);
		case yap_expr_increment:
			return yap_gen_increment(ctx, loc, expr);
		case yap_expr_decrement:
			return yap_gen_decrement(ctx, loc, expr);
		case yap_expr_ternary:
			return yap_gen_ternary_expr(ctx, loc, expr);
		case yap_expr_block:
			return yap_gen_block_expr(ctx, loc, expr);
		case yap_expr_member_access:
			return yap_gen_member_access(ctx, loc, expr);
		case yap_expr_optional_member_access:
			return yap_gen_optional_member_access(ctx, loc, expr);
		case yap_expr_deref:
			return yap_gen_deref(ctx, loc, expr);
		case yap_expr_index_access:
			return yap_gen_index_access(ctx, loc, expr);
		case yap_expr_blueprint_hole:
			yap_emit_error_at(ctx, loc, expr, "blueprint hole '%s' was never filled ; use :fill(c\"%s\", expr) then :finish() before emitting",
				expr.var_name ? expr.var_name : "?", expr.var_name ? expr.var_name : "?");
			return empty_strbuf;
		default:
			    yap_emit_error_at(ctx, loc, expr, "%s", "Unsupported expression kind in codegen");
			return empty_strbuf;
	}
}

yap_strbuf yap_gen_ternary_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_ternary_expr ternary = expr.ternary;
	yap_strbuf condition = yap_gen_expr(ctx, loc, *ternary.condition);
	if (!condition.data){
		yap_emit_error_at(ctx, loc, *ternary.condition, "%s", "Failed to generate expression for ternary operator condition");
		return empty_strbuf;
	}
	yap_strbuf then_branch = yap_gen_expr(ctx, loc, *ternary.then_expr);
	if (!then_branch.data){
		yap_emit_error_at(ctx, loc, *ternary.then_expr, "%s", "Failed to generate expression for ternary operator then branch");
		yap_strbuf_free(&condition);
		return empty_strbuf;
	}
	yap_strbuf else_branch = yap_gen_expr(ctx, loc, *ternary.else_expr);
	if (!else_branch.data){
		yap_emit_error_at(ctx, loc, *ternary.else_expr, "%s", "Failed to generate expression for ternary operator else branch");
		yap_strbuf_free(&condition);
		yap_strbuf_free(&then_branch);
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("(%s ? %s : %s)", yap_strbuf_data(&condition), yap_strbuf_data(&then_branch), yap_strbuf_data(&else_branch));
	yap_strbuf_free(&condition);
	yap_strbuf_free(&then_branch);
	yap_strbuf_free(&else_branch);
	return res;
}

yap_strbuf yap_gen_increment(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *expr.subexpr);
	yap_strbuf res = expr.prefix
		? yap_strbuf_newf("(++%s)", yap_strbuf_data(&subexpr))
		: yap_strbuf_newf("(%s++)", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_decrement(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *expr.subexpr);
	yap_strbuf res = expr.prefix
		? yap_strbuf_newf("(--%s)", yap_strbuf_data(&subexpr))
		: yap_strbuf_newf("(%s--)", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_paren_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *expr.subexpr);
	yap_strbuf res = yap_strbuf_newf("(%s)", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_unary_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *expr.subexpr);
	char op = expr.unary_op ? expr.unary_op : '-'; // default covers any legacy/builder-made unary that predates unary_op
	yap_strbuf res = yap_strbuf_newf("(%c(%s))", op, yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_block_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf body = yap_gen_block(ctx, loc, *expr.block);
	if (!body.data){
		yap_emit_error_at(ctx, loc, expr, "%s", "Failed to generate code for block expression");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("(%s)", yap_strbuf_data(&body));
	yap_strbuf_free(&body);
	return res;
}

yap_strbuf yap_gen_at_op(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *expr.subexpr);
	yap_strbuf res = yap_strbuf_newf("(&(%s))", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_deref(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *expr.subexpr);
	if (!subexpr.data){
		yap_emit_error_at(ctx, loc, *expr.subexpr, "%s", "Failed to generate expression for dereference operand");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("(*(%s))", yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	return res;
}

yap_strbuf yap_gen_cast_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_strbuf subexpr = yap_gen_expr(ctx, loc, *(expr.subexpr));
	yap_strbuf typ = yap_gen_type_id(ctx, loc, expr.type);

	/* Plain C 'char' has platform-defined signedness; route byte (yap's only char-backed, declared-unsigned primitive) through 'unsigned char' first so widening always zero-extends regardless of platform. */
	yap_type* src_type = yap_ctx_get_type(ctx, yap_ctx_coerce_type_id_to_id(ctx, expr.subexpr->type));
	yap_type* dst_type = yap_ctx_get_type(ctx, yap_ctx_coerce_type_id_to_id(ctx, expr.type));
	bool needs_unsigned_char_route =
		src_type && dst_type &&
		src_type->kind == yap_type_primitive && dst_type->kind == yap_type_primitive &&
		!src_type->primitive.is_signed &&
		strcmp(src_type->primitive.c_name, "char") == 0 &&
		dst_type->primitive.bytes > src_type->primitive.bytes;

	yap_strbuf res = needs_unsigned_char_route
		? yap_strbuf_newf("((%s)(unsigned char)(%s))", yap_strbuf_data(&typ), yap_strbuf_data(&subexpr))
		: yap_strbuf_newf("((%s)(%s))", yap_strbuf_data(&typ), yap_strbuf_data(&subexpr));
	yap_strbuf_free(&subexpr);
	yap_strbuf_free(&typ);
	return res;
}

yap_strbuf yap_gen_func_call(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_func_call func_call = expr.func_call;
	yap_strbuf res = yap_gen_expr(ctx, loc, *(func_call.func_expr));
	yap_strbuf_append(&res, "(");
	for_darr(i, arg, func_call.params){
		if (i > 0) yap_strbuf_append(&res, ", ");
		yap_strbuf arg_buf = yap_gen_expr(ctx, loc, arg);
		if (!arg_buf.data){
			yap_emit_error_at(ctx, loc, arg, "%s", "Failed to generate expression for function call argument");
			yap_strbuf_free(&res);
			return empty_strbuf;
		}
		yap_strbuf_append(&res, yap_strbuf_data(&arg_buf));
		yap_strbuf_free(&arg_buf);
	}
	yap_strbuf_append(&res, ")");
	return res;
}

yap_strbuf yap_gen_member_access(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_member_access ma = expr.member_access;
	yap_strbuf obj = yap_gen_expr(ctx, loc, *ma.object);
	if (!obj.data){
		yap_emit_error_at(ctx, loc, *ma.object, "%s", "Failed to generate expression for member access object");
		return empty_strbuf;
	}
	yap_strbuf res = yap_strbuf_newf("%s.%s", yap_strbuf_data(&obj), ma.member);
	yap_strbuf_free(&obj);
	return res;
}

static unsigned long yap_c_optchain_counter = 0;

yap_strbuf yap_gen_optional_member_access(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_member_access ma = expr.member_access;
	yap_strbuf obj = yap_gen_expr(ctx, loc, *ma.object);
	if (!obj.data){
		yap_emit_error_at(ctx, loc, *ma.object, "%s", "Failed to generate expression for optional member access object");
		return empty_strbuf;
	}

	char tmp_name[32];
	snprintf(tmp_name, sizeof(tmp_name), "__yap_opt%lu", yap_c_optchain_counter++);

	yap_strbuf tmp_decl = yap_gen_name_type_id_combo(ctx, tmp_name, ma.object->type);
	yap_strbuf field_type = yap_gen_type_id(ctx, loc, expr.type);
	if (!tmp_decl.data || !field_type.data){
		yap_emit_error_at(ctx, loc, expr, "%s", "Failed to generate type for optional member access");
		yap_strbuf_free(&obj);
		yap_strbuf_free(&tmp_decl);
		yap_strbuf_free(&field_type);
		return empty_strbuf;
	}

	yap_strbuf res = yap_strbuf_newf("({ %s = %s; %s ? %s->%s : (%s){0}; })",
		yap_strbuf_data(&tmp_decl), yap_strbuf_data(&obj),
		tmp_name, tmp_name, ma.member,
		yap_strbuf_data(&field_type));
	yap_strbuf_free(&obj);
	yap_strbuf_free(&tmp_decl);
	yap_strbuf_free(&field_type);
	return res;
}

yap_strbuf yap_gen_index_access(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_index_access ia = expr.index_access;
	yap_strbuf obj = yap_gen_expr(ctx, loc, *ia.object);
	yap_strbuf idx = yap_gen_expr(ctx, loc, *ia.index);
	yap_strbuf res;
	yap_type* obj_type = yap_ctx_get_type(ctx, ia.object->type);
	if (obj_type && obj_type->kind == yap_type_slice)
		res = yap_strbuf_newf("%s.data[%s]", yap_strbuf_data(&obj), yap_strbuf_data(&idx));
	else
		res = yap_strbuf_newf("%s[%s]", yap_strbuf_data(&obj), yap_strbuf_data(&idx));
	yap_strbuf_free(&obj);
	yap_strbuf_free(&idx);
	return res;
}

yap_strbuf yap_gen_binary_expr(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_bin_expr bin = expr.bin_expr;
	yap_strbuf left = yap_gen_expr(ctx, loc, *bin.left);
	yap_strbuf right = yap_gen_expr(ctx, loc, *bin.right);
	if (left.data == NULL || right.data == NULL){
		yap_emit_error_at(ctx, loc, expr, "%s", "Failed to generate expression for binary expression");
		yap_strbuf_free(&left);
		yap_strbuf_free(&right);
		return empty_strbuf;
	}
	const char* op_str;
	char op_buf[2] = { (char)bin.op, '\0' };
	switch (bin.op) {
		case yap_bin_expr_eq:  op_str = "=="; break;
		case yap_bin_expr_neq: op_str = "!="; break;
		case yap_bin_expr_lt:  op_str = "<";  break;
		case yap_bin_expr_gt:  op_str = ">";  break;
		case yap_bin_expr_le:  op_str = "<="; break;
		case yap_bin_expr_ge:  op_str = ">="; break;
		case yap_bin_expr_and: op_str = "&&"; break;
		case yap_bin_expr_or:  op_str = "||"; break;
		case yap_bin_expr_shl: op_str = "<<"; break;
		case yap_bin_expr_shr: op_str = ">>"; break;
		default: op_str = op_buf; // &, |, ^ (and arithmetic) match their own single-char C spelling
	}
	yap_strbuf res = yap_strbuf_newf("%s %s %s", yap_strbuf_data(&left), op_str, yap_strbuf_data(&right));
	yap_strbuf_free(&left);
	yap_strbuf_free(&right);
	return res;
}

yap_strbuf yap_gen_var_access(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	(void)ctx;
	(void)loc;
	return yap_strbuf_newf("%s", expr.var_name);
}

yap_strbuf yap_gen_assignment(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_assignment assignment = expr.assignment;
	yap_code_range range = expr.loc.range;
	yap_expr l = *assignment.left;
	yap_expr r = *assignment.right;
	(void)r;
	if (!l.is_lvalue){
		yap_emit_error_rangef(ctx, loc.src, range, "%s", "Left side of assignment is not an lvalue");
		return yap_strbuf_empty();
	}
	yap_strbuf left = yap_gen_expr(ctx, loc, *assignment.left);
	yap_strbuf right = yap_gen_expr(ctx, loc, *assignment.right);
	if (left.data == NULL || right.data == NULL){
		yap_emit_error_rangef(ctx, loc.src, range, "%s", "Failed to generate expression for assignment");
		yap_strbuf_free(&left);
		yap_strbuf_free(&right);
		return empty_strbuf;
	}

	yap_strbuf res = yap_strbuf_newf("%s %s %s", yap_strbuf_data(&left), assignment.op, yap_strbuf_data(&right));
	yap_strbuf_free(&left);
	yap_strbuf_free(&right);
	return res;
}

yap_strbuf yap_gen_blob_literal(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	yap_blob blob = expr.literal.blob;
	yap_type* target = yap_ctx_get_type(ctx, expr.type);
	if (!target){
		yap_emit_error_at(ctx, loc, expr, "%s", "Invalid type for blob literal");
		return empty_strbuf;
	}

	if (target->kind == yap_type_struct){
		yap_strbuf type_str = yap_gen_type_id(ctx, loc, expr.type);
		yap_strbuf res = yap_strbuf_newf("((%s){", yap_strbuf_data(&type_str));
		yap_strbuf_free(&type_str);
		for (unsigned int i = 0; i < blob.field_count; i++){
			if (i > 0) yap_strbuf_append(&res, ", ");
			char* name = blob.names[i];
			yap_strbuf val = yap_gen_expr(ctx, loc, blob.elements[i]);
			if (name)
				yap_strbuf_appendf(&res, ".%s = %s", name, yap_strbuf_data(&val));
			else
				yap_strbuf_append(&res, yap_strbuf_data(&val));
			yap_strbuf_free(&val);
		}
		yap_strbuf_append(&res, "})");
		return res;
	}

	if (target->kind == yap_type_array){
		yap_strbuf res = yap_strbuf_newf("{");
		for (unsigned int i = 0; i < blob.field_count; i++){
			if (i > 0) yap_strbuf_append(&res, ", ");
			yap_strbuf val = yap_gen_expr(ctx, loc, blob.elements[i]);
			yap_strbuf_append(&res, yap_strbuf_data(&val));
			yap_strbuf_free(&val);
		}
		yap_strbuf_append(&res, "}");
		return res;
	}

	if (target->kind == yap_type_slice){
		yap_type* elem_type = yap_ctx_get_type(ctx, target->slice.element_type);
		if (!elem_type){
			yap_emit_error_at(ctx, loc, expr, "%s", "Invalid slice element type for blob");
			return empty_strbuf;
		}
		yap_strbuf elem_str = yap_gen_name_type_combo(ctx, NULL, *elem_type);
		yap_strbuf res = yap_strbuf_newf("{ .data = (%s[]){",
			yap_strbuf_data(&elem_str));
		yap_strbuf_free(&elem_str);
		for (unsigned int i = 0; i < blob.field_count; i++){
			if (i > 0) yap_strbuf_append(&res, ", ");
			yap_strbuf val = yap_gen_expr(ctx, loc, blob.elements[i]);
			yap_strbuf_append(&res, yap_strbuf_data(&val));
			yap_strbuf_free(&val);
		}
		yap_strbuf_appendf(&res, "}, .len = %u }", blob.field_count);
		return res;
	}

	yap_emit_error_at(ctx, loc, expr, "%s", "Blob literal has unresolved type");
	return empty_strbuf;
}

// literal.text holds decoded bytes; re-escape for C. Octal (not \x) since C hex escapes are unbounded-length.
static yap_strbuf yap_escape_c_string_bytes(const char* text, size_t len){
	yap_strbuf res = yap_strbuf_new();
	for (size_t i = 0; i < len; i++){
		unsigned char c = (unsigned char)text[i];
		switch (c){
			case '\\': yap_strbuf_append(&res, "\\\\"); break;
			case '"':  yap_strbuf_append(&res, "\\\""); break;
			case '\n': yap_strbuf_append(&res, "\\n"); break;
			case '\t': yap_strbuf_append(&res, "\\t"); break;
			case '\r': yap_strbuf_append(&res, "\\r"); break;
			default:
				if (c < 0x20 || c == 0x7f)
					yap_strbuf_appendf(&res, "\\%03o", c);
				else
					yap_strbuf_appendn(&res, (const char*)&c, 1);
		}
	}
	return res;
}

yap_strbuf yap_gen_literal(yap_ctx* ctx, yap_loc loc, yap_expr expr){
	(void)ctx;
	yap_literal literal = expr.literal;
	switch(literal.kind){
		case yap_literal_numerical:
			//TODO: Finish? Errors
			return yap_strbuf_newf("%s", literal.text);
		case yap_literal_bool:
			return yap_strbuf_newf("%s", literal.text);
		case yap_literal_string: {
			size_t len = strlen(literal.text);
			yap_strbuf escaped = yap_escape_c_string_bytes(literal.text, len);
			yap_strbuf res = yap_strbuf_newf("((struct { char* data; unsigned long len; }){ .data = \"%s\", .len = %zu })", yap_strbuf_data(&escaped), len);
			yap_strbuf_free(&escaped);
			return res;
		}
		case yap_literal_cstring: {
			size_t len = strlen(literal.text);
			yap_strbuf escaped = yap_escape_c_string_bytes(literal.text, len);
			yap_strbuf res = yap_strbuf_newf("\"%s\"", yap_strbuf_data(&escaped));
			yap_strbuf_free(&escaped);
			return res;
		}
		case yap_literal_byte:
			return yap_strbuf_newf("%s", literal.text);
		case yap_literal_null:
			return yap_strbuf_newf("%s", "NULL");
		case yap_literal_blob:
			return yap_gen_blob_literal(ctx, loc, expr);
		default:
			    yap_emit_error_at(ctx, loc, expr, "%s", "Unsupported literal kind in codegen");
			return empty_strbuf;
	}
}
