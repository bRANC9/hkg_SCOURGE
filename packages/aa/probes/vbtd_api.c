/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * vbtd_api — does the vendor Bluetooth stack *export* the calls an Android Auto
 * SDP record needs, or does the name merely appear somewhere in the file?
 *
 * Why this exists
 * ---------------
 * The head unit has no kernel Bluetooth stack: `vbtd` is the stack, running in
 * userspace on /dev/ttyS0 (docs/22 §5b). It exports `SDP_CreateRecord`,
 * `SDP_AddAttribute`, `SDP_AddServiceClassIdList` and `RFCOMM_CreateConnection`,
 * which is what makes the on-unit route possible — the record is published
 * inside the vendor stack's own database, through the LD_PRELOAD shim in
 * tools/bttap.
 *
 * The naive check ("search the binary for the string") is a false-positive
 * machine: an *imported* symbol's name lives in the same .dynstr table, so
 * `strstr(file, "RFCOMM_CreateConnection")` is true for a stack that only
 * *calls* it. This tool walks the ELF .dynsym instead and requires the symbol to
 * be defined (st_shndx != SHN_UNDEF), which is exactly the condition for
 * `dlsym` to find it.
 *
 * No `nm` on the unit, no BlueZ headers, no allocation beyond one read of the
 * file (bounded at 64 MiB).
 *
 * Usage:
 *   vbtd_api /app/bin/vbtd                      # report for the symbols we need
 *   vbtd_api /app/bin/vbtd --json /tmp/bt.json  # same, machine-readable
 *   vbtd_api --help
 *
 * Exit codes: 0 = every wanted symbol is exported, 1 = some are missing or only
 * imported, 2 = the file is not a usable ELF64 x86-64 object, 3 = usage error.
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Never read more than this: the vendor daemons are a few hundred KiB, and a
 * wrong path must not turn into a multi-gigabyte read on the unit. */
#define MAX_IMAGE (64u * 1024u * 1024u)

enum want_state {
    WANT_ABSENT = 0,
    WANT_EXPORTED,
    WANT_IMPORTED
};

struct wanted {
    const char *name;
    const char *why;
    enum want_state state;
    unsigned matches;
};

/* What the AA bootstrap needs, split the way the verdict needs it. */
static struct wanted g_wanted[] = {
    {"SDP_CreateRecord", "SDP: a record handle for the AA service", WANT_ABSENT, 0u},
    {"SDP_AddServiceClassIdList", "SDP: the AA UUID, alone", WANT_ABSENT, 0u},
    {"SDP_AddAttribute", "SDP: protocol descriptor, name, browse group", WANT_ABSENT, 0u},
    {"RFCOMM_CreateConnection", "RFCOMM: the server the phone connects to", WANT_ABSENT, 0u},
};

static const size_t g_wanted_count = sizeof g_wanted / sizeof g_wanted[0];

static const char *state_name(enum want_state state)
{
    switch (state) {
    case WANT_EXPORTED:
        return "exported";
    case WANT_IMPORTED:
        return "imported";
    default:
        return "absent";
    }
}

static int read_image(const char *path, unsigned char **out, size_t *out_len, char *err, size_t err_len)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, err_len, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, err_len, "cannot stat %s: %s", path, strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (st.st_size <= 0 || (unsigned long long)st.st_size > MAX_IMAGE) {
        snprintf(err, err_len, "%s: implausible size %lld", path, (long long)st.st_size);
        (void)close(fd);
        return -1;
    }
    const size_t len = (size_t)st.st_size;
    unsigned char *buf = malloc(len);
    if (buf == NULL) {
        snprintf(err, err_len, "out of memory for %zu bytes", len);
        (void)close(fd);
        return -1;
    }
    size_t got = 0u;
    while (got < len) {
        const ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) {
            snprintf(err, err_len, "short read on %s at %zu/%zu", path, got, len);
            free(buf);
            (void)close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    (void)close(fd);
    *out = buf;
    *out_len = len;
    return 0;
}

static void note_symbol(const char *name, unsigned char sym_type, unsigned char sym_bind,
                        unsigned short shndx)
{
    (void)sym_type;
    (void)sym_bind;
    for (size_t i = 0u; i < g_wanted_count; i++) {
        if (strcmp(g_wanted[i].name, name) != 0)
            continue;
        g_wanted[i].matches++;
        /* Defined objects are what dlsym can find; an undefined one is only a
         * name this binary imports from somewhere else. */
        const enum want_state state = (shndx == SHN_UNDEF) ? WANT_IMPORTED : WANT_EXPORTED;
        if (state == WANT_EXPORTED || g_wanted[i].state == WANT_ABSENT)
            g_wanted[i].state = state;
        return;
    }
}

/* Bounded length of a string table entry.  Written by hand instead of using
 * strnlen: the unit build is musl with a strict feature-macro set, and a missing
 * prototype there is a warning we would rather not carry. */
static size_t bounded_len(const char *text, size_t max)
{
    size_t n = 0u;
    while (n < max && text[n] != '\0')
        n++;
    return n;
}

/* Walk SHT_DYNSYM.  Returns 0 on success, -1 when the object has no dynamic
 * symbol table at all (a static binary, or not an ELF we understand). */
static int scan_dynsym(const unsigned char *image, size_t len, char *err, size_t err_len)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (len < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        snprintf(err, err_len, "not an ELF object");
        return -1;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        snprintf(err, err_len, "not a little-endian ELF64 object");
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        snprintf(err, err_len, "not an x86-64 object (machine %u)", (unsigned)eh->e_machine);
        return -1;
    }
    if (eh->e_shoff == 0u || eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shnum == 0u) {
        snprintf(err, err_len, "no section headers (stripped static binary?)");
        return -1;
    }
    if (eh->e_shoff + (uint64_t)eh->e_shentsize * eh->e_shnum > (uint64_t)len) {
        snprintf(err, err_len, "section headers point past the end of the file");
        return -1;
    }
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(image + eh->e_shoff);
    int found = 0;
    for (unsigned i = 0u; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM)
            continue;
        if (sh[i].sh_link >= eh->e_shnum) {
            snprintf(err, err_len, ".dynsym has a bogus string-table link");
            return -1;
        }
        const Elf64_Shdr *str = &sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > (uint64_t)len ||
            str->sh_offset + str->sh_size > (uint64_t)len) {
            snprintf(err, err_len, ".dynsym or its string table is out of bounds");
            return -1;
        }
        const Elf64_Sym *syms = (const Elf64_Sym *)(image + sh[i].sh_offset);
        const char *names = (const char *)(image + str->sh_offset);
        const size_t count = (size_t)(sh[i].sh_size / sizeof(Elf64_Sym));
        for (size_t n = 0u; n < count; n++) {
            const uint32_t off = syms[n].st_name;
            if (off == 0u || (size_t)off >= (size_t)str->sh_size)
                continue;
            /* st_name is an offset into the string table; the table is bounded
             * above, and strnlen keeps a malformed one from running away. */
            const size_t max = (size_t)str->sh_size - (size_t)off;
            const size_t name_len = bounded_len(names + off, max);
            if (name_len == 0u || name_len == max)
                continue;
            note_symbol(names + off, ELF64_ST_TYPE(syms[n].st_info),
                        ELF64_ST_BIND(syms[n].st_info), syms[n].st_shndx);
        }
        found = 1;
    }
    if (!found) {
        snprintf(err, err_len, "no dynamic symbol table (statically linked)");
        return -1;
    }
    return 0;
}

static int all_exported(void)
{
    for (size_t i = 0u; i < g_wanted_count; i++) {
        if (g_wanted[i].state != WANT_EXPORTED)
            return 0;
    }
    return 1;
}

/* The two flags the status JSON carries (docs/22 §5c): an SDP API needs all
 * three SDP calls defined, the RFCOMM API one. */
static int sdp_api_ready(void)
{
    for (size_t i = 0u; i < g_wanted_count; i++) {
        if (strncmp(g_wanted[i].name, "SDP_", 4) != 0)
            continue;
        if (g_wanted[i].state != WANT_EXPORTED)
            return 0;
    }
    return 1;
}

static int rfcomm_api_ready(void)
{
    for (size_t i = 0u; i < g_wanted_count; i++) {
        if (strcmp(g_wanted[i].name, "RFCOMM_CreateConnection") == 0)
            return g_wanted[i].state == WANT_EXPORTED;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *json_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s <elf> [--json <out>]\n", argv[0]);
            printf("reports whether the AA-critical SDP/RFCOMM symbols are exported\n");
            return 0;
        }
        if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
            continue;
        }
        if (path == NULL) {
            path = argv[i];
            continue;
        }
        fprintf(stderr, "unexpected argument: %s\n", argv[i]);
        return 3;
    }
    if (path == NULL) {
        fprintf(stderr, "usage: %s <elf> [--json <out>]\n", argv[0]);
        return 3;
    }

    unsigned char *image = NULL;
    size_t len = 0u;
    char err[256] = "";
    if (read_image(path, &image, &len, err, sizeof err) != 0) {
        fprintf(stderr, "vbtd_api: %s\n", err);
        return 2;
    }
    if (scan_dynsym(image, len, err, sizeof err) != 0) {
        fprintf(stderr, "vbtd_api: %s: %s\n", path, err);
        free(image);
        return 2;
    }
    free(image);

    printf("vbtd_api: %s\n", path);
    for (size_t i = 0u; i < g_wanted_count; i++) {
        printf("  %-28s %-9s  (%s)\n", g_wanted[i].name, state_name(g_wanted[i].state),
               g_wanted[i].why);
    }
    printf("sdp_api=%s rfcomm_api=%s\n", sdp_api_ready() ? "true" : "false",
           rfcomm_api_ready() ? "true" : "false");

    if (json_path != NULL) {
        FILE *out = fopen(json_path, "w");
        if (out == NULL) {
            fprintf(stderr, "vbtd_api: cannot write %s: %s\n", json_path, strerror(errno));
            return 2;
        }
        fprintf(out, "{\"path\":\"%s\",\"sdp_api\":%s,\"rfcomm_api\":%s,\"symbols\":{",
                path, sdp_api_ready() ? "true" : "false", rfcomm_api_ready() ? "true" : "false");
        for (size_t i = 0u; i < g_wanted_count; i++) {
            fprintf(out, "%s\"%s\":\"%s\"", i == 0u ? "" : ",", g_wanted[i].name,
                    state_name(g_wanted[i].state));
        }
        fprintf(out, "}}\n");
        (void)fclose(out);
        printf("json=%s\n", json_path);
    }
    return all_exported() ? 0 : 1;
}
