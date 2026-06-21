#ifndef YAP_C_H
#define YAP_C_H

#include "utils/utils.h"
#define YAP_LOG_TAG "YAP-C"
#define YAP_LOG_TAG_COLOR aesc_yellow
#define YAP_LOG_MSG_COLOR aesc_white

#include "yap/all.h"

//String builder
#include "strbuf.h"

//Types
#include "types.h"

//Incremental compilation build state (TCC state, counter, etc.)
#include "build_state.h"

//Codegen
#include "codegen.h"

#endif //YAP_C_H
