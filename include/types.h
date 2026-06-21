#ifndef YAP_C_TYPES_H
#define YAP_C_TYPES_H

#include "yap/all.h"

//Module
kenobi_new_struct_free(yap_module_c_code,
    //Code
    darr(yap_strbuf) types; //Type declarations go here
    darr(yap_strbuf) decls; //Declarations go here
    darr(yap_strbuf) impl; //Implementation goes here

    //Incremental compilation
    //TODO
);

#endif //YAP_C_TYPES_H