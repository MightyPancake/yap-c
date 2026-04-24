#include "yap_c.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void yap_strbuf_reserve(yap_strbuf* sb, size_t extra){
	size_t needed = sb->len + extra + 1;
	if (needed <= sb->cap){
		return;
	}

	size_t new_cap = sb->cap == 0 ? 32 : sb->cap;
	while (new_cap < needed){
		new_cap *= 2;
	}

	char* next = realloc(sb->data, new_cap);
	if (next == NULL){
		kenobi_panic("yap_strbuf: realloc failed");
	}

	sb->data = next;
	sb->cap = new_cap;
}

yap_strbuf yap_strbuf_empty(){
	return (yap_strbuf){.data=NULL, .len=0, .cap=0};
}

yap_strbuf yap_strbuf_new(){
	yap_strbuf sb;
	yap_strbuf_init(&sb);
	return sb;
}

yap_strbuf yap_strbuf_newf(const char* fmt, ...){
	yap_strbuf sb;
	yap_strbuf_init(&sb);

	va_list args;
	va_start(args, fmt);

	va_list args_len;
	va_copy(args_len, args);
	int needed = vsnprintf(NULL, 0, fmt, args_len);
	va_end(args_len);

	if (needed < 0){
		va_end(args);
		kenobi_panic("yap_strbuf: vsnprintf size failed");
	}

	size_t add = (size_t)needed;
	yap_strbuf_reserve(&sb, add);
	int written = vsnprintf(sb.data, sb.cap, fmt, args);
	va_end(args);

	if (written < 0 || (size_t)written != add){
		kenobi_panic("yap_strbuf: vsnprintf write failed");
	}

	sb.len = add;
	return sb;
}

void yap_strbuf_init(yap_strbuf* sb){
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
}

void yap_strbuf_initn(yap_strbuf* sb, const char* src, size_t n){
	yap_strbuf_init(sb);
	yap_strbuf_appendn(sb, src, n);
}

void yap_strbuf_inits(yap_strbuf* sb, const char* src){
	yap_strbuf_initn(sb, src, strlen(src));
}

void yap_strbuf_initf(yap_strbuf* sb, const char* fmt, ...){
	yap_strbuf_init(sb);

	va_list args;
	va_start(args, fmt);

	va_list args_len;
	va_copy(args_len, args);
	int needed = vsnprintf(NULL, 0, fmt, args_len);
	va_end(args_len);

	if (needed < 0){
		va_end(args);
		kenobi_panic("yap_strbuf: vsnprintf size failed");
	}

	size_t add = (size_t)needed;
	yap_strbuf_reserve(sb, add);
	int written = vsnprintf(sb->data, sb->cap, fmt, args);
	va_end(args);

	if (written < 0 || (size_t)written != add){
		kenobi_panic("yap_strbuf: vsnprintf write failed");
	}

	sb->len = add;
}

void yap_strbuf_clear(yap_strbuf* sb){
	sb->len = 0;
	if (sb->data != NULL){
		sb->data[0] = '\0';
	}
}

void yap_strbuf_free(yap_strbuf* sb){
	free(sb->data);
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
}

void yap_strbuf_appendn(yap_strbuf* sb, const char* src, size_t n){
	yap_strbuf_reserve(sb, n);
	memcpy(sb->data + sb->len, src, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
}

void yap_strbuf_append(yap_strbuf* sb, const char* src){
	yap_strbuf_appendn(sb, src, strlen(src));
}

void yap_strbuf_appendf(yap_strbuf* sb, const char* fmt, ...){
	va_list args;
	va_start(args, fmt);

	va_list args_len;
	va_copy(args_len, args);
	int needed = vsnprintf(NULL, 0, fmt, args_len);
	va_end(args_len);

	if (needed < 0){
		va_end(args);
		kenobi_panic("yap_strbuf: vsnprintf size failed");
	}

	size_t add = (size_t)needed;
	yap_strbuf_reserve(sb, add);
	int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
	va_end(args);

	if (written < 0 || (size_t)written != add){
		kenobi_panic("yap_strbuf: vsnprintf write failed");
	}

	sb->len += add;
}

char* yap_strbuf_take(yap_strbuf* sb){
	if (sb->data == NULL){
		sb->data = malloc(1);
		if (sb->data == NULL){
			kenobi_panic("yap_strbuf: malloc failed");
		}
		sb->data[0] = '\0';
		sb->cap = 1;
	}

	char* out = sb->data;
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
	return out;
}

const char* yap_strbuf_data(const yap_strbuf* sb){
	return sb->data == NULL ? "" : sb->data;
}