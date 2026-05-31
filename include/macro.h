#ifndef YAP_C_MACRO_H
#define YAP_C_MACRO_H

#include "yap/types.h"

yap_ctx* yap_register_macros(yap_ctx* ctx);

//Decided not to use libtcc, but wasm instead
//#include <libtcc.h>
// #include <wasm_store.h>

typedef uint64_t yap_c_macro_counter;

kenobi_new_struct_free(yap_c_macro_engine,
    char* internal_header;
    yap_c_macro_counter counter;
    const char* macro_tmp_path; //Where current macro status lives
    map macros; ///Map of macro name to macro definition
);

#endif //YAP_C_MACRO_H
