#ifndef YAP_C_MACRO_H
#define YAP_C_MACRO_H

#include "yap/types.h"

#include <libtcc.h>

void yap_tcc_example();

void yap_macro_warn(yap_ctx* ctx, const char* fmt, ...);
void yap_macro_error(yap_ctx* ctx, const char* fmt, ...);
bool yap_macro_emit_decl(yap_ctx* ctx, yap_decl decl);

#endif //YAP_C_MACRO_H
