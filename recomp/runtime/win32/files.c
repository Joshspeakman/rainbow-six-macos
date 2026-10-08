/* Files: Windows paths -> host paths (case-insensitive), file handles. */
#include "w32.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef __linux__
#define st_birthtimespec st_ctim
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#endif

char w32_drive_c[1024];                /* host directory for C:\ (other than the game folder) */
char w32_cwd[1024] = "C:\\GOG Games\\Dungeon Siege";
static const char GAME_PREFIX[] = "c:\\gog games\\dungeon siege";

/* find `name` in host directory `dir` ignoring case; writes the real name */
static int ci_lookup(const char *dir, const char *name, char *out, size_t cap)
{
    char p[2048]; struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!lstat(p, &st)) { snprintf(out, cap, "%s", name); return 1; }
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, cap, "%s", e->d_name); ok = 1; break; }
    closedir(d); return ok;
}
char w32_game_overlay[1024];          /* writes to the game folder land here ("" = write in place) */
int w32_import_from_ds1;               /* see w32_host_path */
int w32_loa_docs;                      /* the expansion is played: Documents\\Dungeon Siege is Documents\\Dungeon Siege LOA
                                        * (separate from w32_game_layer, which w32_host_dirs switches off pass by pass) */
char w32_game_layer[1024];            /* read-only layer between the overlay and the game folder: the expansion's data
                                       * (Resources/, Maps/) when Legends of Aranna is played; "" = none */
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static void mkdirs(const char *p)          /* create every missing parent of p */
{
    char t[2048]; snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
}
static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"); if (!a) return -1;
    mkdirs(to); FILE *b = fopen(to, "wb"); if (!b) { fclose(a); return -1; }
    char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, a))) fwrite(buf, 1, n, b);
    fclose(a); fclose(b); return 0;
}
/* resolve path components below `root` case-insensitively; missing components are kept as given */
static void resolve(const char *root, char **parts, int from, int np, char *out, size_t cap)
{
    char cur[2048]; snprintf(cur, sizeof cur, "%s", root);
    for (int j = from; j < np; j++) {
        char real[512]; size_t l = strlen(cur);
        if (ci_lookup(cur, parts[j], real, sizeof real)) snprintf(cur + l, sizeof cur - l, "/%s", real);
        else snprintf(cur + l, sizeof cur - l, "/%s", parts[j]);
    }
    snprintf(out, cap, "%s", cur);
}
/* Windows path -> host path. mode 0: read (overlay if present, else base); 1: create/write (overlay, copied up). */
int w32_host_path(const char *win, char *out, size_t cap, int mode)
{
    char full[2048], norm[2048];
    if (((win[0] | 32) >= 'a' && (win[0] | 32) <= 'z') && win[1] == ':') snprintf(full, sizeof full, "%s", win);
    else if (win[0] == '\\' || win[0] == '/') snprintf(full, sizeof full, "C:%s", win);
    else snprintf(full, sizeof full, "%s\\%s", w32_cwd, win);
    if ((full[0] | 32) != 'c') return -1;
    char *parts[256]; int np = 0; size_t k;
    for (char *s = full; *s; s++) if (*s == '/') *s = '\\';
    char *save, *tok = strtok_r(full + 2, "\\", &save);
    while (tok) {
        if (!strcmp(tok, "..")) { if (np) np--; }
        else if (strcmp(tok, ".") && *tok) { if (np == 256) return -1; parts[np++] = tok; }   /* too deep: no such path */
        tok = strtok_r(0, "\\", &save);
    }
    norm[0] = 'c'; norm[1] = ':'; norm[2] = 0; k = 2;
    for (int j = 0; j < np; j++) k += (size_t)snprintf(norm + k, sizeof norm - k, "\\%s", parts[j]);
    size_t gl = strlen(GAME_PREFIX);
    /* Legends of Aranna keeps its settings, characters and saves in Documents\Dungeon Siege LOA, as the original does
     * (the base engine names the folder "Dungeon Siege"); its "Import DS Character" list reads Dungeon Siege's own saves */
    extern int w32_import_from_ds1;
    if (w32_loa_docs && np >= 4 && !strcasecmp(parts[0], "Users") && !strcasecmp(parts[2], "Documents") && !strcasecmp(parts[3], "Dungeon Siege") &&
        !(w32_import_from_ds1 && np >= 5 && !strcasecmp(parts[4], "Save")))
        parts[3] = (char *)"Dungeon Siege LOA";
    if (!(np >= 2 && !strncasecmp(norm, GAME_PREFIX, gl) && (norm[gl] == 0 || norm[gl] == '\\'))) {
        resolve(w32_drive_c, parts, 0, np, out, cap);
        if (mode) mkdirs(out);
        return 0;
    }
    char base[2048]; resolve(w32_game_dir, parts, 2, np, base, sizeof base);
    if (!*w32_game_overlay) { snprintf(out, cap, "%s", base); return 0; }
    char ov[2048]; resolve(w32_game_overlay, parts, 2, np, ov, sizeof ov);
    if (exists(ov)) { snprintf(out, cap, "%s", ov); return 0; }
    if (*w32_game_layer) {                       /* the expansion layer: read where it has the file */
        char ly[2048]; resolve(w32_game_layer, parts, 2, np, ly, sizeof ly);
        struct stat lst;
        if (!stat(ly, &lst) && (!mode || S_ISDIR(lst.st_mode))) { if (!mode) { snprintf(out, cap, "%s", ly); return 0; } }
        else if (!stat(ly, &lst) && mode) { snprintf(base, sizeof base, "%s", ly); }   /* written: copied up from the layer */
    }
    if (!mode) { snprintf(out, cap, "%s", base); return 0; }
    struct stat st;
    if (!stat(base, &st) && S_ISREG(st.st_mode)) copy_file(base, ov); else mkdirs(ov);
    snprintf(out, cap, "%s", ov);
    return 0;
}
/* every host directory behind a game-folder directory (overlay, expansion layer, game folder); returns how many exist */
int w32_host_dirs(const char *win, char dirs[3][2048])
{
    char save_ov[1024], save_ly[1024]; int n = 0;
    snprintf(save_ov, sizeof save_ov, "%s", w32_game_overlay); snprintf(save_ly, sizeof save_ly, "%s", w32_game_layer);
    /* one pass per layer: overlay only, layer only, game folder only (the others switched off) */
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0 && !*save_ov) continue;
        if (pass == 1 && !*save_ly) continue;
        if (pass == 0) { w32_game_layer[0] = 0; }
        if (pass == 1) { snprintf(w32_game_overlay, sizeof w32_game_overlay, "%s", save_ly); w32_game_layer[0] = 0; }
        if (pass == 2) { w32_game_overlay[0] = 0; w32_game_layer[0] = 0; }
        char d[2048];
        if (!w32_host_path(win, d, sizeof d, 0) && exists(d)) {
            int dup = 0; for (int j = 0; j < n; j++) if (!strcmp(dirs[j], d)) dup = 1;
            if (!dup) snprintf(dirs[n++], 2048, "%s", d);
        }
        snprintf(w32_game_overlay, sizeof w32_game_overlay, "%s", save_ov); snprintf(w32_game_layer, sizeof w32_game_layer, "%s", save_ly);
    }
    if (!*save_ov && !n) { if (!w32_host_path(win, dirs[0], 2048, 0) && exists(dirs[0])) n = 1; }
    return n;
}

IMPL(kernel32, GetFileType)
{
    HObj *o = h_get(ARG(0), H_FILE);
    if (!o) { w32_set_last_error(c, 6); RET(0, 1); }
    RET(1, 1);                                              /* FILE_TYPE_DISK */
}

/* ---- path queries ---- */
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fnmatch.h>
extern char w32_cwd[1024];
static uint32_t copy_out(Ctx *c, const char *s, uint32_t buf, uint32_t cap)   /* returns length, or needed size */
{
    uint32_t n = (uint32_t)strlen(s);
    if (n + 1 > cap) return n + 1;
    memcpy(GP(buf), s, n + 1); (void)c; return n;
}
static void full_win_path(const char *in, char *out, size_t cap)
{
    char tmp[2048];
    if (((in[0] | 32) >= 'a' && (in[0] | 32) <= 'z') && in[1] == ':') snprintf(tmp, sizeof tmp, "%s", in);
    else if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof tmp, "C:%s", in);
    else snprintf(tmp, sizeof tmp, "%s\\%s", w32_cwd, in);
    char *parts[256]; int np = 0;
    for (char *s = tmp; *s; s++) if (*s == '/') *s = '\\';
    char drive = (char)(tmp[0] & ~32), *save, *tok = strtok_r(tmp + 2, "\\", &save);
    while (tok) { if (!strcmp(tok, "..")) { if (np) np--; } else if (strcmp(tok, ".") && *tok && np < 256) parts[np++] = tok; tok = strtok_r(0, "\\", &save); }
    size_t k = (size_t)snprintf(out, cap, "%c:", drive);
    for (int j = 0; j < np && k < cap; j++) k += (size_t)snprintf(out + k, cap - k, "\\%s", parts[j]);   /* stops when full */
    if (k >= cap) return;
    if (!np) snprintf(out + k, cap - k, "\\");
    size_t l = strlen(in);
    if (l && (in[l - 1] == '\\' || in[l - 1] == '/') && np) snprintf(out + strlen(out), cap - strlen(out), "\\");
}
IMPL(kernel32, GetLongPathNameA) { RET(copy_out(c, GS(ARG(0)), ARG(1), ARG(2)), 3); }
IMPL(kernel32, GetFullPathNameA)
{
    char full[2048]; full_win_path(GS(ARG(0)), full, sizeof full);
    uint32_t buf = ARG(2), fp = ARG(3), r = copy_out(c, full, buf, ARG(1));
    if (fp && r < ARG(1)) { const char *s = strrchr(full, '\\'); rt_w32(G_MEM, fp, s && s[1] ? buf + (uint32_t)(s + 1 - full) : 0); }
    RET(r, 4);
}
IMPL(kernel32, GetCurrentDirectoryA) { RET(copy_out(c, w32_cwd, ARG(1), ARG(0)), 2); }
IMPL(kernel32, GetTempPathA) { RET(copy_out(c, "C:\\Temp\\", ARG(1), ARG(0)), 2); }
static int host(Ctx *c, uint32_t win, char *out, int create)
{
    if (w32_host_path(GS(win), out, 2048, create)) { w32_set_last_error(c, 3); return -1; }
    return 0;
}
IMPL(kernel32, GetFileAttributesA)
{
    char p[2048]; struct stat st;
    if (host(c, ARG(0), p, 0) || stat(p, &st)) { if (getenv("W32_DIRLOG")) fprintf(stderr, "w32: GetFileAttributes(%s) missing\n", GS(ARG(0))); w32_set_last_error(c, 2); RET(0xffffffffu, 1); }
    if (getenv("W32_DIRLOG")) fprintf(stderr, "w32: GetFileAttributes(%s) -> %s\n", GS(ARG(0)), p);
    RET(S_ISDIR(st.st_mode) ? 0x10 : (st.st_mode & S_IWUSR) ? 0x20 : 0x21, 1);
}
static void ft_of(uint32_t at, time_t sec, long nsec)    /* Unix time -> FILETIME (100 ns since 1601) */
{
    uint64_t t = ((uint64_t)sec + 11644473600ull) * 10000000ull + (uint64_t)nsec / 100;
    rt_w32(G_MEM, at, (uint32_t)t); rt_w32(G_MEM, at + 4, (uint32_t)(t >> 32));
}
/* GetFileAttributesExA(name, GetFileExInfoStandard, WIN32_FILE_ATTRIBUTE_DATA *) */
IMPL(kernel32, GetFileAttributesExA)
{
    char p[2048]; struct stat st; uint32_t d = ARG(2);
    if (host(c, ARG(0), p, 0) || stat(p, &st)) { w32_set_last_error(c, 2); RET(0, 3); }
    rt_w32(G_MEM, d, S_ISDIR(st.st_mode) ? 0x10 : (st.st_mode & S_IWUSR) ? 0x20 : 0x21);
#ifdef __APPLE__
    ft_of(d + 4, st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec); ft_of(d + 12, st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec);
    ft_of(d + 20, st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec);
#else
    ft_of(d + 4, st.st_ctim.tv_sec, st.st_ctim.tv_nsec); ft_of(d + 12, st.st_atim.tv_sec, st.st_atim.tv_nsec); ft_of(d + 20, st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
#endif
    uint64_t sz = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    rt_w32(G_MEM, d + 28, (uint32_t)(sz >> 32)); rt_w32(G_MEM, d + 32, (uint32_t)sz);
    RET(1, 3);
}
IMPL(kernel32, SetFileAttributesA) { char p[2048]; struct stat st; RET(!host(c, ARG(0), p, 0) && !stat(p, &st), 2); }
IMPL(kernel32, CreateDirectoryA)
{
    char p[2048]; if (host(c, ARG(0), p, 1)) RET(0, 2);
    if (getenv("W32_DIRLOG")) fprintf(stderr, "w32: CreateDirectory(%s) -> %s\n", GS(ARG(0)), p);
    if (mkdir(p, 0755)) { w32_set_last_error(c, errno == EEXIST ? 183 : 3); RET(0, 2); }
    RET(1, 2);
}
/* deleting or moving only ever touches the data folder: the game folder and the expansion layer are never changed */
static int writable_path(const char *p)
{
    extern char w32_drive_c[1024];
    size_t a = strlen(w32_drive_c), b = strlen(w32_game_overlay);
    if (!*w32_game_overlay) return 1;                     /* no overlay: the game runs from a folder it may change */
    return (a && !strncmp(p, w32_drive_c, a)) || (b && !strncmp(p, w32_game_overlay, b));
}
IMPL(kernel32, RemoveDirectoryA) { char p[2048]; if (host(c, ARG(0), p, 0) || !writable_path(p) || rmdir(p)) { w32_set_last_error(c, 2); RET(0, 1); } RET(1, 1); }
IMPL(kernel32, DeleteFileA) { char p[2048]; if (host(c, ARG(0), p, 0) || !writable_path(p) || unlink(p)) { w32_set_last_error(c, 2); RET(0, 1); } RET(1, 1); }
IMPL(kernel32, MoveFileA)
{
    char a[2048], b[2048]; struct stat st;
    if (host(c, ARG(0), a, 0) || host(c, ARG(1), b, 1)) RET(0, 2);
    if (!writable_path(a)) { w32_set_last_error(c, 5); RET(0, 2); }
    if (!stat(b, &st)) { w32_set_last_error(c, 183); RET(0, 2); }
    if (rename(a, b)) { w32_set_last_error(c, 2); RET(0, 2); }
    RET(1, 2);
}
IMPL(kernel32, CopyFileA)                     /* (src, dst, fail if it exists): written into the overlay like any write */
{
    char a[2048], b[2048]; struct stat st;
    if (host(c, ARG(0), a, 0) || host(c, ARG(1), b, 1)) RET(0, 3);
    if (!writable_path(b)) { w32_set_last_error(c, 5); RET(0, 3); }
    if (ARG(2) && !stat(b, &st)) { w32_set_last_error(c, 80); RET(0, 3); }     /* ERROR_FILE_EXISTS */
    int in = open(a, O_RDONLY); if (in < 0) { w32_set_last_error(c, 2); RET(0, 3); }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); w32_set_last_error(c, 5); RET(0, 3); }
    char buf[65536]; ssize_t n; int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
    if (n < 0) ok = 0;
    close(in); close(out); RET(ok, 3);
}
IMPL(kernel32, GetDriveTypeA) { RET(3, 1); }                              /* DRIVE_FIXED */
IMPL(kernel32, GetLogicalDriveStringsA)
{
    if (ARG(0) < 5) RET(5, 2);
    memcpy(GP(ARG(1)), "C:\\\0\0", 5); RET(4, 2);
}
IMPL(kernel32, GetVolumeInformationA)
{
    uint32_t name = ARG(1), nsz = ARG(2), serial = ARG(3), maxlen = ARG(4), flags = ARG(5), fs = ARG(6), fsz = ARG(7);
    if (name && nsz) copy_out(c, "MAC", name, nsz);
    if (serial) rt_w32(G_MEM, serial, 0x1234abcd);
    if (maxlen) rt_w32(G_MEM, maxlen, 255);
    if (flags) rt_w32(G_MEM, flags, 0x3);
    if (fs && fsz) copy_out(c, "NTFS", fs, fsz);
    RET(1, 8);
}
IMPL(kernel32, GetDiskFreeSpaceExA)
{
    for (int k = 1; k <= 3; k++) if (ARG(k)) rt_w64(G_MEM, ARG(k), 64ull << 30);
    RET(1, 4);
}
IMPL(kernel32, GetTempFileNameA)
{
    static uint32_t n = 1; uint32_t u = ARG(2) ? ARG(2) : n++;
    char s[2048]; snprintf(s, sizeof s, "%s\\%.3s%X.tmp", GS(ARG(0)), GS(ARG(1)), u & 0xffff);
    copy_out(c, s, ARG(3), 260);
    if (!ARG(2)) { char p[2048]; if (!w32_host_path(s, p, sizeof p, 1)) { int fd = open(p, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd); } }
    RET(u, 4);
}

/* ---- file handles ---- */
typedef struct File { int fd; char path[1024]; int is_dir; char win[520]; } File;   /* win: the name it was opened by */
static uint64_t ft_from_ts(struct timespec t)
{
    if (w32_deterministic) return 132000000000000000ull;          /* harness: file times must not differ between runs */
    return ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100;
}
IMPL(kernel32, CreateFileA)
{
    uint32_t access = ARG(1), disp = ARG(4), flags = ARG(5); char p[2048]; struct stat st;
    /* only opens that write (or create/truncate) go to the data folder's overlay of the game folder; plain reads use
     * the file where it is, so the game's archives are never copied */
    int writes = (access & (0x40000000u | 0x10000000u)) || disp == 1 || disp == 2 || disp == 5;
    if (host(c, ARG(0), p, writes)) RET(0xffffffffu, 7);
    if (!writes && stat(p, &st) && disp == 4 && host(c, ARG(0), p, 1)) RET(0xffffffffu, 7);   /* OPEN_ALWAYS creating it */
    int exists = !stat(p, &st), isdir = exists && S_ISDIR(st.st_mode);
    if (isdir && !(flags & 0x02000000)) { w32_set_last_error(c, 5); RET(0xffffffffu, 7); }
    int of = (access & 0x40000000) ? ((access & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (isdir) of = O_RDONLY;
    switch (disp) {
    case 1: if (exists) { w32_set_last_error(c, 80); RET(0xffffffffu, 7); } of |= O_CREAT | O_EXCL; break;
    case 2: of |= O_CREAT | O_TRUNC; break;
    case 3: if (!exists) { w32_set_last_error(c, 2); RET(0xffffffffu, 7); } break;
    case 4: of |= O_CREAT; break;
    case 5: if (!exists) { w32_set_last_error(c, 2); RET(0xffffffffu, 7); } of |= O_TRUNC; break;
    }
    if ((of & O_CREAT) && !(access & 0x40000000)) of = (of & ~O_ACCMODE) | O_RDWR;
    int fd = open(p, of, 0644);
    if (fd < 0) { w32_set_last_error(c, errno == ENOENT ? 3 : 5); RET(0xffffffffu, 7); }
    File *f = calloc(1, sizeof *f); f->fd = fd; f->is_dir = isdir; snprintf(f->path, sizeof f->path, "%s", p); snprintf(f->win, sizeof f->win, "%s", GS(ARG(0)));
    w32_set_last_error(c, (exists && (disp == 2 || disp == 4)) ? 183 : 0);
    uint32_t h = h_new(H_FILE, f);
    if (getenv("W32_FILELOG")) fprintf(stderr, "w32: CreateFileA(%s) -> %x fd %d (%s)\n", GS(ARG(0)), h, fd, p);
    RET(h, 7);
}
typedef struct Mapping Mapping;
static int mapping_named(const Mapping *m);
void w32_mapping_free(void *p);
void w32_file_closed(HObj *o)
{
    if (o->type == H_FILE) { File *f = o->p; close(f->fd); free(f); }
    else if (o->type == H_MAPPING && !mapping_named(o->p)) w32_mapping_free(o->p);   /* views stay valid (mmap keeps the file) */
}
IMPL(kernel32, ReadFile)
{
    HObj *o = h_get(ARG(0), H_FILE); uint32_t buf = ARG(1), n = ARG(2), rd = ARG(3);
    if (!o) { w32_set_last_error(c, 6); RET(0, 5); }
    ssize_t r = read(((File *)o->p)->fd, GP(buf), n);
    if (rd) rt_w32(G_MEM, rd, r > 0 ? (uint32_t)r : 0);
    RET(r >= 0, 5);
}
IMPL(kernel32, WriteFile)
{
    HObj *o = h_get(ARG(0), H_FILE); uint32_t buf = ARG(1), n = ARG(2), wr = ARG(3);
    if (!o) { w32_set_last_error(c, 6); RET(0, 5); }
    ssize_t r = write(((File *)o->p)->fd, GP(buf), n);
    if (wr) rt_w32(G_MEM, wr, r > 0 ? (uint32_t)r : 0);
    RET(r >= 0, 5);
}
IMPL(kernel32, SetFilePointer)
{
    HObj *o = h_get(ARG(0), H_FILE); uint32_t hi = ARG(2), method = ARG(3);
    if (!o) { w32_set_last_error(c, 6); RET(0xffffffffu, 4); }
    int64_t off = hi ? (int64_t)(((uint64_t)rt_r32(G_MEM, hi) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1);
    off_t r = lseek(((File *)o->p)->fd, off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { w32_set_last_error(c, 131); RET(0xffffffffu, 4); }
    if (hi) rt_w32(G_MEM, hi, (uint32_t)((uint64_t)r >> 32));
    w32_set_last_error(c, 0); RET((uint32_t)r, 4);
}
IMPL(kernel32, GetFileSize)
{
    HObj *o = h_get(ARG(0), H_FILE); struct stat st;
    if (!o || fstat(((File *)o->p)->fd, &st)) { w32_set_last_error(c, 6); RET(0xffffffffu, 2); }
    if (getenv("W32_FILELOG")) fprintf(stderr, "w32: GetFileSize(%x) fd %d %s -> %lld\n", ARG(0), ((File *)o->p)->fd, ((File *)o->p)->path, (long long)st.st_size);
    if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET((uint32_t)st.st_size, 2);
}
IMPL(kernel32, GetFileSizeEx)                         /* (handle, LARGE_INTEGER *size) */
{
    HObj *o = h_get(ARG(0), H_FILE); struct stat st;
    if (!o || fstat(((File *)o->p)->fd, &st)) { w32_set_last_error(c, 6); RET(0, 2); }
    rt_w64(G_MEM, ARG(1), (uint64_t)st.st_size); RET(1, 2);
}
IMPL(kernel32, SetFilePointerEx)                      /* (handle, LARGE_INTEGER distance (2 dwords), LARGE_INTEGER *new position, method) */
{
    HObj *o = h_get(ARG(0), H_FILE); int64_t off = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1)); uint32_t np = ARG(3), method = ARG(4);
    if (!o) { w32_set_last_error(c, 6); RET(0, 5); }
    off_t r = lseek(((File *)o->p)->fd, off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { w32_set_last_error(c, 131); RET(0, 5); }
    if (np) rt_w64(G_MEM, np, (uint64_t)r);
    RET(1, 5);
}
IMPL(kernel32, GetFileTime)
{
    HObj *o = h_get(ARG(0), H_FILE); struct stat st;
    if (!o || fstat(((File *)o->p)->fd, &st)) { w32_set_last_error(c, 6); RET(0, 4); }
    if (ARG(1)) rt_w64(G_MEM, ARG(1), ft_from_ts(st.st_birthtimespec));
    if (ARG(2)) rt_w64(G_MEM, ARG(2), ft_from_ts(st.st_atimespec));
    if (ARG(3)) rt_w64(G_MEM, ARG(3), ft_from_ts(st.st_mtimespec));
    RET(1, 4);
}
IMPL(kernel32, FlushFileBuffers) { RET(h_get(ARG(0), H_FILE) != 0, 1); }

/* ---- directory enumeration (case-insensitive name order, "." and ".." first, like NTFS) ---- */
typedef struct Find { char dir[3][1024]; int ndir; char **names; int n, pos, docs; } Find;   /* docs: see FindFirstFileA */
static int ci_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
static void fill_find(Find *fd, const char *name, uint32_t out);
static void find_fill(Find *fd, const char *name, uint32_t out)          /* the entry, under the name the game sees */
{
    fill_find(fd, name, out);
    if (fd->docs && !strcasecmp(name, "Dungeon Siege LOA")) snprintf((char *)GP(out + 44), 260, "Dungeon Siege");   /* cFileName */
    if (getenv("W32_DIRLOG")) fprintf(stderr, "w32:   entry %s (attr %x)\n", (char *)GP(out + 44), rt_r32(G_MEM, out));
}
static void fill_find(Find *fd, const char *name, uint32_t out)
{
    char p[2048]; struct stat st; memset(GP(out), 0, 320);
    for (int k = 0; k < fd->ndir; k++) { snprintf(p, sizeof p, "%s/%s", fd->dir[k], name); if (!stat(p, &st)) break; }
    if (!stat(p, &st)) {
        rt_w32(G_MEM, out, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        rt_w64(G_MEM, out + 4, ft_from_ts(st.st_birthtimespec)); rt_w64(G_MEM, out + 12, ft_from_ts(st.st_atimespec));
        rt_w64(G_MEM, out + 20, ft_from_ts(st.st_mtimespec));
        rt_w32(G_MEM, out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); rt_w32(G_MEM, out + 32, S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size);
    }
    snprintf((char *)GP(out + 44), 260, "%s", name);
}
int w32_hide_seefar;    /* set by the host when its draw distance setting is on */
IMPL(kernel32, FindFirstFileA)
{
    char win[2048], p[2048]; snprintf(win, sizeof win, "%s", GS(ARG(0)));
    for (char *s = win; *s; s++) if (*s == '/') *s = '\\';
    char *sl = strrchr(win, '\\'); const char *pat = sl ? sl + 1 : win;
    char dirwin[2048]; if (sl) snprintf(dirwin, sizeof dirwin, "%.*s", (int)(sl - win), win); else snprintf(dirwin, sizeof dirwin, ".");
    if (!*dirwin) snprintf(dirwin, sizeof dirwin, "\\");
    char dirs[3][2048]; int nd = w32_host_dirs(dirwin, dirs);
    if (!nd) { w32_set_last_error(c, 3); RET(0xffffffffu, 2); }
    (void)p;
    Find *fd = calloc(1, sizeof *fd); fd->ndir = nd; for (int k = 0; k < nd; k++) snprintf(fd->dir[k], sizeof fd->dir[k], "%s", dirs[k]);
    int cap = 64; fd->names = malloc(cap * sizeof *fd->names);
    const char *pt = !strcmp(pat, "*.*") ? "*" : pat;
    /* with the expansion, Documents\Dungeon Siege is Documents\Dungeon Siege LOA (see w32_host_path): a listing of
     * Documents shows that folder under the name "Dungeon Siege" (the game looks for its folder that way) and leaves
     * out the base game's own */
    size_t dl = strlen(dirwin);
    int docs = w32_loa_docs && dl >= 10 && !strcasecmp(dirwin + dl - 10, "\\Documents");
    int is_root = strlen(dirwin) <= 3;
    for (int di = 0; di < nd; di++) {
        DIR *d = opendir(dirs[di]); if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (is_root && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) continue;
            if (e->d_name[0] == '.' && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) continue;    /* host dotfiles */
            if (docs && !strcasecmp(e->d_name, "Dungeon Siege")) continue;
            const char *shown = docs && !strcasecmp(e->d_name, "Dungeon Siege LOA") ? "Dungeon Siege" : e->d_name;
            if (fnmatch(pt, shown, FNM_CASEFOLD)) continue;
            if (w32_hide_seefar && !fnmatch("sf_seefar*.dsres", e->d_name, FNM_CASEFOLD)) continue;   /* built-in draw distance replaces it */
            int dup = 0; for (int j = 0; j < fd->n; j++) if (!strcasecmp(fd->names[j], e->d_name)) { dup = 1; break; }
            if (dup) continue;
            if (fd->n == cap) { cap *= 2; fd->names = realloc(fd->names, cap * sizeof *fd->names); }
            fd->names[fd->n++] = strdup(e->d_name);
        }
        closedir(d);
    }
    qsort(fd->names, (size_t)fd->n, sizeof *fd->names, ci_cmp);       /* "." and ".." sort first */
    if (getenv("W32_DIRLOG")) fprintf(stderr, "w32: FindFirstFile(%s) pattern %s: %d\n", win, pt, fd->n);
    if (!fd->n) { free(fd->names); free(fd); w32_set_last_error(c, 2); RET(0xffffffffu, 2); }
    fd->docs = docs;
    find_fill(fd, fd->names[0], ARG(1)); fd->pos = 1;
    RET(h_new(H_FIND, fd), 2);
}
IMPL(kernel32, FindNextFileA)
{
    HObj *o = h_get(ARG(0), H_FIND); if (!o) { w32_set_last_error(c, 6); RET(0, 2); }
    Find *fd = o->p;
    if (fd->pos >= fd->n) { w32_set_last_error(c, 18); RET(0, 2); }
    find_fill(fd, fd->names[fd->pos++], ARG(1)); RET(1, 2);
}
IMPL(kernel32, FindClose)
{
    HObj *o = h_get(ARG(0), H_FIND); if (!o) RET(0, 1);
    Find *fd = o->p; for (int k = 0; k < fd->n; k++) free(fd->names[k]);
    free(fd->names); free(fd); o->p = 0; h_close(ARG(0)); RET(1, 1);
}

/* ---- file mappings: the host file is mapped straight into guest memory ---- */
struct Mapping { int fd; uint32_t size, writable; char name[64]; };
static Mapping *mnamed[32]; static uint32_t mnamed_h[32]; static int nmnamed;
static int mapping_named(const Mapping *m) { for (int k = 0; k < nmnamed; k++) if (mnamed[k] == m) return 1; return 0; }   /* kept: OpenFileMapping */
void w32_mapping_free(void *p) { Mapping *m = p; if (m->fd >= 0) close(m->fd); free(m); }
IMPL(kernel32, CreateFileMappingA)
{
    uint32_t fh = ARG(0), prot = ARG(2), lo = ARG(4), name = ARG(5);
    Mapping *m = calloc(1, sizeof *m); m->writable = (prot & 0x0c) != 0;
    if (fh == 0xffffffffu) { m->fd = -1; m->size = lo; }              /* pagefile-backed (shared memory) */
    else {
        HObj *o = h_get(fh, H_FILE); struct stat st;
        if (!o) { free(m); w32_set_last_error(c, 6); RET(0, 6); }
        m->fd = dup(((File *)o->p)->fd);
        if (m->fd < 0 || fstat(m->fd, &st)) { if (m->fd >= 0) close(m->fd); free(m); w32_set_last_error(c, 4); RET(0, 6); }   /* out of descriptors */
        m->size = lo ? lo : (uint32_t)st.st_size;
        if (m->writable && (uint64_t)st.st_size < m->size) ftruncate(m->fd, m->size);
    }
    uint32_t h = h_new(H_MAPPING, m);
    if (name && nmnamed < 32) { snprintf(m->name, sizeof m->name, "%s", GS(name)); mnamed[nmnamed] = m; mnamed_h[nmnamed++] = h; }
    w32_set_last_error(c, 0); RET(h, 6);
}
IMPL(kernel32, OpenFileMappingA)
{
    for (int k = 0; k < nmnamed; k++) if (!strcmp(mnamed[k]->name, GS(ARG(2)))) RET(mnamed_h[k], 3);
    w32_set_last_error(c, 2); RET(0, 3);
}
typedef struct View { uint32_t addr, size; } View;
static View views[1024]; static int nviews;
static uint32_t map_view(Ctx *c, uint32_t h, uint32_t access, uint32_t offlo, uint32_t n, uint32_t want)
{
    HObj *o = h_get(h, H_MAPPING); if (!o) { w32_set_last_error(c, 6); return 0; }
    Mapping *m = o->p;
    if (!n) n = m->size - offlo;
    uint32_t sz = (n + 0xfff) & ~0xfffu;
    uint32_t a = vm_alloc(want, sz, 0x3000, (access & 2) ? 4 : 2);
    if (!a) { w32_set_last_error(c, 8); return 0; }
    if (m->fd >= 0) {
        int wr = (access & 2) && m->writable;
        void *r = mmap(G_MEM + a, sz, PROT_READ | PROT_WRITE, MAP_FIXED | (wr ? MAP_SHARED : MAP_PRIVATE), m->fd, offlo);
        if (r == MAP_FAILED) { vm_free(a, 0, 0x8000); w32_set_last_error(c, 5); return 0; }
    }
    if (nviews < 1024) { views[nviews].addr = a; views[nviews++].size = sz; }
    return a;
}
IMPL(kernel32, MapViewOfFile) { RET(map_view(c, ARG(0), ARG(1), ARG(3), ARG(4), 0), 5); }
IMPL(kernel32, MapViewOfFileEx) { RET(map_view(c, ARG(0), ARG(1), ARG(3), ARG(4), ARG(5)), 6); }
IMPL(kernel32, UnmapViewOfFile)
{
    uint32_t a = ARG(0);
    for (int k = 0; k < nviews; k++) if (views[k].addr == a) {
        msync(G_MEM + a, views[k].size, MS_ASYNC);
        vm_free(a, 0, 0x8000);                                  /* replaces the file pages with fresh zero pages */
        views[k] = views[--nviews]; RET(1, 1);
    }
    RET(0, 1);
}
int w32_file_fd(uint32_t h) { HObj *o = h_get(h, H_FILE); return o ? ((File *)o->p)->fd : -1; }
const char *w32_file_win_path(uint32_t h) { HObj *o = h_get(h, H_FILE); return o ? ((File *)o->p)->win : 0; }
