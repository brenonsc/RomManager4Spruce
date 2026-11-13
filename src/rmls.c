// rmls - lists the games in a ROM folder fast (one readdir, no stat per file).
//   rmls list DIR     the games in DIR, sorted (byte order, like ls in the C locale)
//   rmls count ROMS   "FOLDER|games" for every folder of ROMS that has any
// Skipped, as launch.sh's ROM_IGNORE does: hidden files, folders, Imgs, list caches
// (.db, .xml, .json, .txt, miyoocache.bin) and pictures (.png, .jpg).
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int ends(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcasecmp(s + n - m, suf) == 0;
}

static int is_game(const char *dir, struct dirent *e) {
    const char *n = e->d_name;
    if (n[0] == '.' || strcasecmp(n, "Imgs") == 0 || strcasecmp(n, "miyoocache.bin") == 0) return 0;
    if (ends(n, ".xml") || ends(n, ".db") || ends(n, ".png") || ends(n, ".jpg") || ends(n, ".txt") || ends(n, ".json"))
        return 0;
    if (e->d_type == DT_DIR) return 0;
    if (e->d_type == DT_UNKNOWN) { // some filesystems don't say: ask
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", dir, n);
        DIR *d = opendir(p);
        if (d) { closedir(d); return 0; }
    }
    return 1;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 1;
    size_t n = 0, cap = 256;
    char **v = malloc(cap * sizeof *v);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_game(dir, e)) continue;
        if (n == cap) v = realloc(v, (cap *= 2) * sizeof *v);
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(v, n, sizeof *v, cmp);
    for (size_t i = 0; i < n; i++) puts(v[i]);
    return 0;
}

static int count(const char *roms) {
    DIR *d = opendir(roms);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", roms, e->d_name);
        DIR *s = opendir(p);
        if (!s) continue;
        int c = 0;
        struct dirent *f;
        while ((f = readdir(s))) c += is_game(p, f);
        closedir(s);
        if (c > 0) printf("%s|%d\n", e->d_name, c);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "list") == 0) return list(argv[2]);
    if (argc == 3 && strcmp(argv[1], "count") == 0) return count(argv[2]);
    fprintf(stderr, "usage: rmls list DIR | rmls count ROMS\n");
    return 2;
}
