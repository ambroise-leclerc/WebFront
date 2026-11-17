// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Extra manual diagnostic on ELF64/Linux: observe current EGL contexts without changing their arguments or result.
#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void (*Proc)(void);
static unsigned (*current)(void *, void *, void *, void *);
static Proc (*address)(const char *);

static unsigned observed_current(void *display, void *draw, void *read, void *context) {
    unsigned ok = current(display, draw, read, context);
    if (ok && context && address) {
        const unsigned char *(*get)(unsigned) = (void *)address("glGetString");
        if (get) {
            char executable[256];
            ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
            if (length < 0) length = 0;
            executable[length] = 0;
            const char *name = strrchr(executable, '/');
            fprintf(stderr, "WF528_CONTEXT pid=%ld process=%s vendor=%s renderer=%s version=%s\n",
                    (long)getpid(), name ? name + 1 : executable, get(0x1F00), get(0x1F01), get(0x1F02));
        }
    }
    return ok;
}

static Proc observed_address(const char *name) {
    Proc result = address(name);
    if (!strcmp(name, "eglMakeCurrent")) {
        current = (void *)result;
        return (Proc)observed_current;
    }
    return result;
}

unsigned la_version(unsigned version) {
    (void)version;
    return LAV_CURRENT;
}

unsigned la_objopen(struct link_map *map, Lmid_t namespace_id, uintptr_t *cookie) {
    (void)map;
    (void)namespace_id;
    (void)cookie;
    return LA_FLG_BINDTO | LA_FLG_BINDFROM;
}

uintptr_t la_symbind64(Elf64_Sym *symbol, unsigned index, uintptr_t *from, uintptr_t *to,
                      unsigned *flags, const char *name) {
    (void)index;
    (void)from;
    (void)to;
    (void)flags;
    if (!strcmp(name, "eglGetProcAddress")) {
        address = (void *)symbol->st_value;
        return (uintptr_t)observed_address;
    }
    if (!strcmp(name, "eglMakeCurrent")) {
        current = (void *)symbol->st_value;
        return (uintptr_t)observed_current;
    }
    return symbol->st_value;
}
