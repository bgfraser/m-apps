/* SPDX-License-Identifier: MIT */
/* libFuzzer: kit/load.c's META.TXT reader (the launcher's names) on any bytes. */
#include "load.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* kit/load.c's file loading, which this doesn't use. */
bool fat_read(const struct fat_entry *file, uint32_t offset, void *to, uint32_t n) {
    (void)file, (void)offset, (void)to, (void)n;
    return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    char *copy = malloc(n ? n : 1);
    memcpy(copy, data, n);
    /* As the launcher has it before: the folder's name, and the rest empty. Unused bytes are left as noise, so a value
     * that isn't ended shows. */
    struct app_text text;
    memset(&text, 0x5a, sizeof text);
    memcpy(text.name, "HELLO", 6);
    text.about[0] = text.version[0] = 0;
    app_meta_parse(copy, (uint32_t)n, &text);
    /* Each a C string within its field. (A value spilling into the next field would stay inside the struct, where the
     * sanitizers can't see it; that needs a model of the parser, which this doesn't have.) */
    assert(memchr(text.name, 0, sizeof text.name) && memchr(text.about, 0, sizeof text.about) &&
           memchr(text.version, 0, sizeof text.version));
    free(copy);
    return 0;
}
