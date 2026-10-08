/* Reading one file out of a Dungeon Siege Tank archive (.dsres/.dsmap) from native code, and a small parser for the
 * game's .gas text format: used by features whose data the game keeps in archives the native side needs to see
 * (the expansion's overhead map). Format: header (directory set, file set, data offset at +12), directories and
 * files as records with length-prefixed names, file data stored plain or as zlib chunks. */
#include "tank.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static uint32_t u32(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static uint16_t u16(const uint8_t *d, size_t o) { uint16_t v; memcpy(&v, d + o, 2); return v; }

int tank_read(const char *archive, const char *path, uint8_t **out, size_t *outlen)
{
    int fd = open(archive, O_RDONLY); if (fd < 0) return -1;
    struct stat st; if (fstat(fd, &st) || st.st_size < 32) { close(fd); return -1; }
    size_t n = (size_t)st.st_size; const uint8_t *d = mmap(0, n, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (d == MAP_FAILED) return -1;
    int rc = -1;
    uint32_t dirset = u32(d, 12), fileset = u32(d, 16), dataoff = u32(d, 24);
    #define IN(o, len) ((uint64_t)(o) + (uint64_t)(len) <= n)          /* every offset checked: a damaged archive is refused */
    if (!IN(dirset, 4) || !IN(fileset, 4)) goto done;
    /* the directory names, for building each file's path */
    uint32_t ndir = u32(d, dirset); if (ndir > 100000 || !IN(dirset + 4, 4ull * ndir)) goto done;
    uint32_t *doff = calloc(ndir, 4), *dpar = calloc(ndir, 4); const char **dname = calloc(ndir, sizeof *dname); uint16_t *dlen = calloc(ndir, 2);
    for (uint32_t k = 0; k < ndir; k++) {
        uint32_t off = u32(d, dirset + 4 + 4 * k); uint64_t o = (uint64_t)dirset + off; doff[k] = off;
        if (!IN(o, 18) || !IN(o + 18, u16(d, o + 16))) { dlen[k] = 0; dname[k] = ""; continue; }
        dpar[k] = u32(d, o); dlen[k] = u16(d, o + 16); dname[k] = (const char *)d + o + 18;
    }
    uint32_t nf = u32(d, fileset); if (!IN(fileset + 4, 4ull * nf)) nf = 0;
    for (uint32_t k = 0; k < nf && rc; k++) {
        uint64_t o64 = (uint64_t)fileset + u32(d, fileset + 4 + 4 * k);
        if (!IN(o64, 30) || !IN(o64 + 30, u16(d, o64 + 28))) continue;
        uint32_t o = (uint32_t)o64;
        uint32_t parent = u32(d, o), size = u32(d, o + 4), foff = u32(d, o + 8); uint16_t fmt = u16(d, o + 24), nlen = u16(d, o + 28);
        /* full path: walk the parents */
        char full[1024]; size_t len = 0; char parts[32][256]; int np = 0;
        snprintf(parts[np++], 256, "%.*s", nlen, (const char *)d + o + 30);
        for (uint32_t p = parent, guard = 0; guard < 31; guard++) {
            uint32_t j = 0; while (j < ndir && doff[j] != p) j++;
            if (j == ndir || !dlen[j]) break;
            snprintf(parts[np++], 256, "%.*s", dlen[j], dname[j]);
            if (dpar[j] == p) break;
            p = dpar[j];
        }
        full[0] = 0;
        for (int i = np - 1; i >= 0 && len < sizeof full; i--) len += (size_t)snprintf(full + len, sizeof full - len, "%s%s", parts[i], i ? "/" : "");
        if (len >= sizeof full || strcasecmp(full, path)) continue;
        if (size > (256u << 20)) continue;
        uint8_t *buf = calloc(1, size ? size : 1); if (!buf) continue;
        if (fmt == 0) { if (IN((uint64_t)dataoff + foff, size)) memcpy(buf, d + dataoff + foff, size); }
        else {
            uint32_t o2 = (o + 30 + nlen + 1 + 3) & ~3u; if (!IN(o2, 8)) { free(buf); continue; }
            uint32_t chunk = u32(d, o2 + 4), nch = chunk ? (size + chunk - 1) / chunk : 0, w = 0;
            o2 += 8;
            if (!IN(o2, 16ull * nch)) nch = 0;
            for (uint32_t ch = 0; ch < nch && w <= size; ch++) {
                uint32_t usz = u32(d, o2 + 16 * ch), csz = u32(d, o2 + 16 * ch + 4), extra = u32(d, o2 + 16 * ch + 8), coff = u32(d, o2 + 16 * ch + 12);
                uint64_t at = (uint64_t)dataoff + foff + coff;
                if (!IN(at, (uint64_t)csz + extra)) break;
                const uint8_t *raw = d + at;
                if (csz < usz) { uLongf dl = size - w; uncompress(buf + w, &dl, raw, csz); w += (uint32_t)dl; }
                else { uint32_t c = usz < size - w ? usz : size - w; memcpy(buf + w, raw, c); w += c; }
                if (extra && (uint64_t)w + extra <= size) { memcpy(buf + w, raw + csz, extra); w += extra; }
            }
        }
        *out = buf; *outlen = size; rc = 0;
    }
    free(doff); free(dpar); free(dname); free(dlen);
    #undef IN
done:
    munmap((void *)d, n);
    return rc;
}

/* ---- .gas: [name] or [t:type,n:name] blocks with { key = value; } entries; comments // and block comments */
static void skip_ws(const char **p)
{
    for (;;) {
        while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
        if ((*p)[0] == '/' && (*p)[1] == '/') { while (**p && **p != '\n') (*p)++; continue; }
        if ((*p)[0] == '/' && (*p)[1] == '*') { const char *e = strstr(*p + 2, "*/"); *p = e ? e + 2 : *p + strlen(*p); continue; }
        break;
    }
}
static GasBlock *parse_block(const char **p, const char *name, size_t nlen)
{
    GasBlock *b = calloc(1, sizeof *b); snprintf(b->name, sizeof b->name, "%.*s", (int)nlen, name);
    for (;;) {
        skip_ws(p);
        if (!**p || **p == '}') { if (**p) (*p)++; return b; }
        if (**p == '[') {
            const char *s = ++*p; while (**p && **p != ']') (*p)++;
            size_t l = (size_t)(*p - s); if (**p) (*p)++;
            const char *nm = s; const char *n2 = memchr(s, ',', l);            /* [t:type,n:name] -> name */
            if (n2 && l > (size_t)(n2 - s) + 3 && n2[1] == 'n' && n2[2] == ':') { nm = n2 + 3; l -= (size_t)(nm - s); }
            skip_ws(p); if (**p == '{') (*p)++;
            GasBlock *c = parse_block(p, nm, l);
            if (b->nchild == b->capchild) { b->capchild = b->capchild ? b->capchild * 2 : 8; b->child = realloc(b->child, sizeof *b->child * (size_t)b->capchild); }
            b->child[b->nchild++] = c;
            continue;
        }
        /* key [with a type prefix like "f " or "x "] = value ; */
        const char *ks = *p; while (**p && **p != '=' && **p != ';' && **p != '}' && **p != '\n') (*p)++;
        if (**p != '=') { if (**p == ';' || **p == '\n') (*p)++; continue; }
        const char *ke = *p; while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        const char *sp = ks; for (const char *q = ks; q < ke; q++) if (*q == ' ' || *q == '\t') sp = q + 1;
        (*p)++; skip_ws(p);
        const char *vs = *p; int quoted = **p == '"';
        if (quoted) { vs = ++*p; while (**p && **p != '"') (*p)++; }
        else while (**p && **p != ';' && **p != '\n' && **p != '}') (*p)++;
        const char *ve = *p; while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        if (quoted && **p == '"') (*p)++;
        if (b->nkey == b->capkey) {
            b->capkey = b->capkey ? b->capkey * 2 : 8;
            b->key = realloc(b->key, sizeof *b->key * (size_t)b->capkey); b->val = realloc(b->val, sizeof *b->val * (size_t)b->capkey);
        }
        snprintf(b->key[b->nkey], sizeof b->key[0], "%.*s", (int)(ke - sp), sp);
        snprintf(b->val[b->nkey], sizeof b->val[0], "%.*s", (int)(ve - vs), vs);
        b->nkey++;
        while (**p && **p != ';' && **p != '\n' && **p != '}') (*p)++;
        if (**p == ';') (*p)++;
    }
}
GasBlock *gas_parse(const char *text) { const char *p = text; return parse_block(&p, "", 0); }
void gas_free(GasBlock *b) { if (!b) return; for (int i = 0; i < b->nchild; i++) gas_free(b->child[i]); free(b->child); free(b->key); free(b->val); free(b); }
GasBlock *gas_child(GasBlock *b, const char *name)
{
    if (!b) return 0;
    for (int i = 0; i < b->nchild; i++) if (!strcasecmp(b->child[i]->name, name)) return b->child[i];
    return 0;
}
const char *gas_get(GasBlock *b, const char *key, const char *dflt)
{
    if (b) for (int i = 0; i < b->nkey; i++) if (!strcasecmp(b->key[i], key)) return b->val[i];
    return dflt;
}
