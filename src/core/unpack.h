/* Распаковка потока по первым байтам: 1F 8B — gzip (в том числе склеенный), PK — zip (первый файл),
 * FD 37 7A 58 5A — xz (не поддерживается: ошибка), иначе как есть. Целиком в память не читает. */
#ifndef PTV_UNPACK_H
#define PTV_UNPACK_H
#include <stddef.h>

typedef long (*rd_fn)(void *ctx, void *buf, size_t n);   /* >0 байты, 0 конец, <0 ошибка */
typedef struct up up;
up *up_new(rd_fn rd, void *ctx);
long up_read(up *u, void *buf, size_t n);                 /* как rd_fn */
const char *up_error(const up *u);                        /* "format" (xz), "gzip", "zip", "" */
const char *up_kind(const up *u);                         /* "plain", "gzip", "zip" */
void up_free(up *u);
#endif
