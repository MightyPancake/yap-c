#ifndef YAP_C_STRBUF_H
#define YAP_C_STRBUF_H

#include <stddef.h>

typedef struct {
	char* data;
	size_t len;
	size_t cap;
} yap_strbuf;

yap_strbuf yap_strbuf_newf(const char* fmt, ...);
yap_strbuf yap_strbuf_empty();
yap_strbuf yap_strbuf_new();
void yap_strbuf_init(yap_strbuf* sb);
void yap_strbuf_initn(yap_strbuf* sb, const char* src, size_t n);
void yap_strbuf_inits(yap_strbuf* sb, const char* src);
void yap_strbuf_initf(yap_strbuf* sb, const char* fmt, ...);
void yap_strbuf_clear(yap_strbuf* sb);
void yap_strbuf_free(yap_strbuf* sb);
void yap_strbuf_appendn(yap_strbuf* sb, const char* src, size_t n);
void yap_strbuf_append(yap_strbuf* sb, const char* src);
void yap_strbuf_appendf(yap_strbuf* sb, const char* fmt, ...);
char* yap_strbuf_take(yap_strbuf* sb);
const char* yap_strbuf_data(const yap_strbuf* sb);

#endif //YAP_C_STRBUF_H