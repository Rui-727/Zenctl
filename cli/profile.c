/* profile.c - zenctl profile save / load / list / delete.
 *
 * Profiles are TOML snapshots of hardware state. v1 covers CPU
 * (governor + freq min/max), memory (swappiness + THP), and per-device
 * storage (scheduler + read_ahead_kb). Other domains are silently
 * skipped on save and ignored on load — partial profiles are valid.
 *
 *   zenctl profile save <name>     capture current state
 *   zenctl profile load <name>     restore state (--dry-run, --confirm)
 *   zenctl profile list            list saved profiles
 *   zenctl profile delete <name>   delete a profile
 *
 * Profile locations:
 *   /etc/zenctl/profiles/<name>.toml       (system; root writes)
 *   ~/.config/zenctl/profiles/<name>.toml  (user; unprivileged)
 *
 * On save the system path is used when running as root, else the user
 * path. On load the user path is tried first, then the system path.
 *
 * The TOML parser is a minimal line-based one: keys/values, [section]
 * headers, and quoted strings. No arrays of tables are needed for v1's
 * profile schema. The parser is permissive: malformed lines are
 * skipped with a warning rather than aborting the load.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/cpu.h"
#include "zenctl/mem.h"
#include "zenctl/storage.h"

#include "output.h"
#include "cmd/common.h"

#define SYSTEM_PROFILE_DIR "/etc/zenctl/profiles"

/* ── Path helpers ───────────────────────────────────────────────── */

/* Compose the user profile directory (~/.config/zenctl/profiles).
 * Returns a malloced string or NULL on failure. */
static char *user_profile_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/";
    char *p = malloc(strlen(home) + 32);
    if (!p) return NULL;
    snprintf(p, strlen(home) + 32, "%s/.config/zenctl/profiles", home);
    return p;
}

/* Build "<dir>/<name>.toml". Caller frees. */
static char *profile_path(const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    char *p = malloc(dl + 1 + nl + 6);
    if (!p) return NULL;
    snprintf(p, dl + 1 + nl + 6, "%s/%s.toml", dir, name);
    return p;
}

/* mkdir -p (best-effort). Returns 0 if dir exists or was created. */
static int mkpath(const char *dir)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", dir);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ── TOML parser ────────────────────────────────────────────────── */

typedef struct {
    char section[128];   /* current section path, e.g. "cpu" or "storage.sda" */
    char key[128];
    char value[512];     /* string content (no quotes) or int/bool as text */
    int  is_string;      /* 1 if value was quoted, 0 if bareword */
} toml_kv_t;

/* Strip trailing whitespace in-place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/* Parse a TOML file into a flat list of (section, key, value) triples.
 * Returns 0 on success (even if some lines were skipped), -1 on I/O
 * error. The caller frees *out with free(). */
static int toml_parse(const char *path, toml_kv_t **out, int *count,
                      zenctl_err_t *err)
{
    *out = NULL; *count = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        cli_make_err(err, ZENCTL_ERR_EIO, strerror(errno), path);
        return -1;
    }
    char section[128] = "";
    char line[1024];
    int cap = 0, n = 0;
    toml_kv_t *list = NULL;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        rstrip(p);
        if (*p == '\0' || *p == '#') continue;

        if (*p == '[') {
            /* Section header. The parser accepts both [section] and
             * [[array.of.tables]] (the latter is treated identically
             * for v1's schema — no profile section uses arrays). */
            char *start = p + 1;
            if (*start == '[') start++;
            char *end = strchr(start, ']');
            if (!end) continue;
            *end = '\0';
            snprintf(section, sizeof(section), "%s", start);
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* trim key */
        while (*key == ' ' || *key == '\t') key++;
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = '\0';
        /* trim value */
        while (*val == ' ' || *val == '\t') val++;
        int is_string = 0;
        if (*val == '"') {
            is_string = 1;
            val++;
            char *endq = strrchr(val, '"');
            if (endq) *endq = '\0';
        } else if (*val == '\'') {
            /* TOML literal string; treat same as basic for our purposes. */
            is_string = 1;
            val++;
            char *endq = strrchr(val, '\'');
            if (endq) *endq = '\0';
        }

        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            toml_kv_t *nl = realloc(list, nc * sizeof(*list));
            if (!nl) {
                free(list);
                fclose(f);
                cli_make_err(err, ZENCTL_ERR_NOMEM, "oom", "toml_parse");
                return -1;
            }
            list = nl; cap = nc;
        }
        snprintf(list[n].section, sizeof(list[n].section), "%s", section);
        snprintf(list[n].key,     sizeof(list[n].key),     "%s", key);
        snprintf(list[n].value,   sizeof(list[n].value),   "%s", val);
        list[n].is_string = is_string;
        n++;
    }
    fclose(f);
    *out = list;
    *count = n;
    return 0;
}

/* ── Save: capture live state into a TOML file ──────────────────── */

/* Write the [cpu] section. Reads CPU 0 (cpufreq policy is shared
 * across cores on most systems; the load path applies to all CPUs). */
static void save_cpu(FILE *f)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
    if (!cpu) return;

    char *gov = NULL; int64_t fmin = 0, fmax = 0;
    bool have_gov  = (zenctl_cpu_get_governor(cpu, &gov, NULL) == 0);
    bool have_fmin = (zenctl_cpu_get_freq_min(cpu, &fmin, NULL) == 0);
    bool have_fmax = (zenctl_cpu_get_freq_max(cpu, &fmax, NULL) == 0);

    if (have_gov || have_fmin || have_fmax) {
        fputs("\n[cpu]\n", f);
        if (have_gov)  fprintf(f, "governor = \"%s\"\n", gov);
        if (have_fmin) fprintf(f, "freq_min_hz = %lld\n", (long long)fmin);
        if (have_fmax) fprintf(f, "freq_max_hz = %lld\n", (long long)fmax);
    }
    free(gov);
    zenctl_cpu_close(cpu);
}

/* Write the [memory] section. */
static void save_memory(FILE *f)
{
    int swappiness = -1, overcommit = -1, vcp = -1;
    zenctl_thp_mode_t thp = 0;
    bool have_swap = (zenctl_mem_get_swappiness(&swappiness, NULL) == 0);
    bool have_thp  = (zenctl_mem_get_thp(&thp, NULL) == 0);
    bool have_oc   = (zenctl_mem_get_overcommit(&overcommit, NULL) == 0);
    bool have_vcp  = (zenctl_mem_get_vfs_cache_pressure(&vcp, NULL) == 0);

    if (!have_swap && !have_thp && !have_oc && !have_vcp) return;
    fputs("\n[memory]\n", f);
    if (have_swap) fprintf(f, "swappiness = %d\n", swappiness);
    if (have_thp) {
        const char *s = (thp == ZENCTL_THP_ALWAYS)  ? "always"  :
                        (thp == ZENCTL_THP_MADVISE) ? "madvise" :
                        (thp == ZENCTL_THP_NEVER)   ? "never"   : "always";
        fprintf(f, "thp = \"%s\"\n", s);
    }
    if (have_oc)  fprintf(f, "overcommit = %d\n", overcommit);
    if (have_vcp) fprintf(f, "vfs_cache_pressure = %d\n", vcp);
}

/* Filter predicate: skip pseudo block devices (loop, ram, md, zram). */
static bool is_real_block(const char *name)
{
    if (!name || !*name) return false;
    if (strncmp(name, "loop", 4) == 0) return false;
    if (strncmp(name, "ram",  3) == 0) return false;
    if (strncmp(name, "zram", 4) == 0) return false;
    if (strncmp(name, "md",   2) == 0 && isdigit((unsigned char)name[2])) return false;
    return true;
}

/* Write one [storage.<dev>] section per block device. */
static void save_storage(FILE *f)
{
    DIR *d = opendir("/sys/block");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_real_block(de->d_name)) continue;
        zenctl_err_t err;
        memset(&err, 0, sizeof(err));
        zenctl_storage_t *st = zenctl_storage_open(de->d_name, &err);
        if (!st) continue;

        char *sched = NULL; int64_t ra = 0;
        bool have_sched = (zenctl_storage_get_scheduler(st, &sched, NULL) == 0);
        bool have_ra    = (zenctl_storage_get_read_ahead(st, &ra, NULL) == 0);
        if (have_sched || have_ra) {
            fprintf(f, "\n[storage.%s]\n", de->d_name);
            if (have_sched) fprintf(f, "scheduler = \"%s\"\n", sched);
            if (have_ra)    fprintf(f, "read_ahead_kb = %lld\n", (long long)ra);
        }
        free(sched);
        zenctl_storage_close(st);
    }
    closedir(d);
}

/* Build the full TOML document and write it to `path`. */
static int profile_write(const char *path, const char *name, bool json,
                         bool dry_run, zenctl_err_t *err)
{
    if (dry_run) {
        char buf[256];
        snprintf(buf, sizeof(buf), "would save profile '%s' to %s", name, path);
        cli_dryrun(json, buf);
        return 0;
    }
    /* Ensure parent directory exists. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkpath(dir); }

    FILE *f = fopen(path, "w");
    if (!f) {
        cli_make_err(err, ZENCTL_ERR_EIO, strerror(errno), path);
        return -1;
    }
    fprintf(f, "# zenctl profile\n");
    fprintf(f, "schema_version = 1\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "created_at = %lld\n", (long long)time(NULL));

    save_cpu(f);
    save_memory(f);
    save_storage(f);
    fclose(f);
    return 0;
}

static int profile_save_cmd(const char *name, bool json, bool dry_run)
{
    if (!name || !*name) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "profile name required", "profile save");
        return cli_err(json, &err);
    }
    /* Reject names that could escape the profiles directory. */
    if (strchr(name, '/') || strcmp(name, "..") == 0 ||
        strstr(name, "../") != NULL) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "invalid profile name", "profile save");
        return cli_err(json, &err);
    }

    const char *dir = SYSTEM_PROFILE_DIR;
    char *udir = NULL;
    if (!cli_is_root()) {
        udir = user_profile_dir();
        if (!udir) {
            zenctl_err_t err;
            cli_make_err(&err, ZENCTL_ERR_NOMEM, "oom", "profile save");
            return cli_err(json, &err);
        }
        dir = udir;
    }
    char *path = profile_path(dir, name);
    free(udir);
    if (!path) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_NOMEM, "oom", "profile save");
        return cli_err(json, &err);
    }

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = profile_write(path, name, json, dry_run, &err);
    if (rc == 0) {
        if (json) {
            out_json_ok_begin();
            bool first = true;
            out_json_field_string(&first, "name", name);
            out_json_field_string(&first, "path", path);
            out_json_ok_end();
        } else {
            out_kv("saved", name);
            out_kv("path", path);
        }
    } else {
        rc = cli_err(json, &err);
    }
    free(path);
    return rc;
}

/* ── Load: apply a profile ──────────────────────────────────────── */

/* Apply one [cpu] key. */
static int load_cpu_kv(const char *key, const char *val, int is_string,
                       bool dry_run, bool confirm, bool json)
{
    /* Determine CPU count from /sys/devices/system/cpu/cpuN. */
    int ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu < 1) ncpu = 1;

    if (strcmp(key, "governor") == 0) {
        if (!is_string) { /* ignore */ return 0; }
        if (dry_run) {
            char b[128]; snprintf(b, sizeof(b), "cpu[all]: governor=%s", val);
            cli_dryrun(json, b);
            return 0;
        }
        zenctl_err_t err; int rc = 0;
        for (int i = 0; i < ncpu; i++) {
            zenctl_cpu_t *c = zenctl_cpu_open(i, NULL);
            if (!c) continue;
            if (zenctl_cpu_set_governor(c, val, &err) != 0) rc = -1;
            zenctl_cpu_close(c);
        }
        return rc;
    }
    if (strcmp(key, "freq_min_hz") == 0 || strcmp(key, "freq_max_hz") == 0) {
        if (is_string) return 0;
        long long hz;
        if (cli_parse_int(val, &hz) != 0 || hz < 0) return 0;
        if (dry_run) {
            char b[128];
            snprintf(b, sizeof(b), "cpu[all]: %s=%lld", key, hz);
            cli_dryrun(json, b);
            return 0;
        }
        zenctl_err_t err; int rc = 0;
        for (int i = 0; i < ncpu; i++) {
            zenctl_cpu_t *c = zenctl_cpu_open(i, NULL);
            if (!c) continue;
            int r = (strcmp(key, "freq_min_hz") == 0)
                ? zenctl_cpu_set_freq_min(c, hz, &err)
                : zenctl_cpu_set_freq_max(c, hz, &err);
            if (r != 0) rc = -1;
            zenctl_cpu_close(c);
        }
        (void)confirm; /* freq clamping isn't destructive */
        return rc;
    }
    return 0;  /* unknown key — skip */
}

/* Apply one [memory] key. */
static int load_memory_kv(const char *key, const char *val, int is_string,
                          bool dry_run, bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    if (strcmp(key, "swappiness") == 0) {
        if (is_string) return 0;
        long long v; if (cli_parse_int(val, &v) != 0) return 0;
        if (dry_run) { char b[64]; snprintf(b, sizeof(b), "memory: swappiness=%lld", v); cli_dryrun(json, b); return 0; }
        return zenctl_mem_set_swappiness((int)v, &err) == 0 ? 0 : -1;
    }
    if (strcmp(key, "overcommit") == 0) {
        if (is_string) return 0;
        long long v; if (cli_parse_int(val, &v) != 0) return 0;
        if (dry_run) { char b[64]; snprintf(b, sizeof(b), "memory: overcommit=%lld", v); cli_dryrun(json, b); return 0; }
        return zenctl_mem_set_overcommit((int)v, &err) == 0 ? 0 : -1;
    }
    if (strcmp(key, "vfs_cache_pressure") == 0) {
        if (is_string) return 0;
        long long v; if (cli_parse_int(val, &v) != 0) return 0;
        if (dry_run) { char b[64]; snprintf(b, sizeof(b), "memory: vfs_cache_pressure=%lld", v); cli_dryrun(json, b); return 0; }
        return zenctl_mem_set_vfs_cache_pressure((int)v, &err) == 0 ? 0 : -1;
    }
    if (strcmp(key, "thp") == 0) {
        if (!is_string) return 0;
        zenctl_thp_mode_t m;
        if      (strcmp(val, "always")  == 0) m = ZENCTL_THP_ALWAYS;
        else if (strcmp(val, "madvise") == 0) m = ZENCTL_THP_MADVISE;
        else if (strcmp(val, "never")   == 0) m = ZENCTL_THP_NEVER;
        else return 0;
        if (dry_run) { char b[64]; snprintf(b, sizeof(b), "memory: thp=%s", val); cli_dryrun(json, b); return 0; }
        return zenctl_mem_set_thp(m, &err) == 0 ? 0 : -1;
    }
    return 0;
}

/* Apply one [storage.<dev>] key. */
static int load_storage_kv(const char *dev, const char *key, const char *val,
                           int is_string, bool dry_run, bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) return 0;  /* device gone; skip silently */

    int rc = 0;
    if (strcmp(key, "scheduler") == 0) {
        if (!is_string) goto out;
        if (dry_run) {
            char b[128]; snprintf(b, sizeof(b), "storage.%s: scheduler=%s", dev, val);
            cli_dryrun(json, b);
            goto out;
        }
        rc = (zenctl_storage_set_scheduler(st, val, &err) == 0) ? 0 : -1;
    } else if (strcmp(key, "read_ahead_kb") == 0) {
        if (is_string) goto out;
        long long v; if (cli_parse_int(val, &v) != 0) goto out;
        if (dry_run) {
            char b[128]; snprintf(b, sizeof(b), "storage.%s: read_ahead_kb=%lld", dev, v);
            cli_dryrun(json, b);
            goto out;
        }
        rc = (zenctl_storage_set_read_ahead(st, v, &err) == 0) ? 0 : -1;
    }
out:
    zenctl_storage_close(st);
    return rc;
}

/* Apply a parsed profile. Returns 0 if every applicable key succeeded;
 * -1 if any write failed (caller decides whether that's fatal). */
static int profile_apply(const toml_kv_t *kv, int count, bool json,
                         bool dry_run, bool confirm)
{
    int failures = 0;
    for (int i = 0; i < count; i++) {
        const char *sec = kv[i].section;
        if (sec[0] == '\0') continue;  /* top-level (schema_version etc.) */
        int rc = 0;
        if (strcmp(sec, "cpu") == 0) {
            rc = load_cpu_kv(kv[i].key, kv[i].value, kv[i].is_string,
                             dry_run, confirm, json);
        } else if (strcmp(sec, "memory") == 0) {
            rc = load_memory_kv(kv[i].key, kv[i].value, kv[i].is_string,
                                dry_run, json);
        } else if (strncmp(sec, "storage.", 8) == 0) {
            rc = load_storage_kv(sec + 8, kv[i].key, kv[i].value,
                                 kv[i].is_string, dry_run, json);
        }
        /* Unknown sections are silently ignored — partial profiles are
         * valid and we don't want to break on a [gpu] block we don't
         * yet know how to apply. */
        if (rc != 0) failures++;
    }
    return failures ? -1 : 0;
}

static int profile_load_cmd(const char *name, bool json, bool dry_run, bool confirm)
{
    if (!name || !*name) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "profile name required", "profile load");
        return cli_err(json, &err);
    }

    /* Find the profile: user dir first, then system dir. */
    char *udir = user_profile_dir();
    char *upath = udir ? profile_path(udir, name) : NULL;
    free(udir);
    char *spath = profile_path(SYSTEM_PROFILE_DIR, name);

    const char *path = NULL;
    if (upath && access(upath, R_OK) == 0) path = upath;
    else if (spath && access(spath, R_OK) == 0) path = spath;

    if (!path) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_ENOENT,
                     "profile not found", name);
        free(upath); free(spath);
        return cli_err(json, &err);
    }

    /* Writes during load require root (unless --dry-run). */
    if (!dry_run && cli_require_root(json)) {
        free(upath); free(spath);
        return -1;
    }

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    toml_kv_t *kv = NULL; int n = 0;
    if (toml_parse(path, &kv, &n, &err) != 0) {
        free(upath); free(spath);
        return cli_err(json, &err);
    }

    int rc = profile_apply(kv, n, json, dry_run, confirm);
    if (rc == 0) {
        if (json) {
            out_json_ok_begin();
            bool first = true;
            out_json_field_string(&first, "loaded", name);
            out_json_field_string(&first, "path", path);
            out_json_field_int(&first, "entries", n);
            out_json_ok_end();
        } else {
            out_kv("loaded", name);
            out_kv("path", path);
            out_kv_int("entries", n);
        }
    } else {
        if (json) {
            out_json_error(ZENCTL_ERR_EIO,
                           "profile load completed with errors");
        } else {
            out_err_code(ZENCTL_ERR_EIO,
                         "profile load completed with errors");
        }
    }
    free(kv);
    free(upath); free(spath);
    return rc;
}

/* ── List ───────────────────────────────────────────────────────── */

/* Append names from one profile directory into a growing array. */
static int list_dir_profiles(const char *dir, char ***list, int *n, int *cap)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 6) continue;
        if (strcmp(de->d_name + l - 5, ".toml") != 0) continue;
        if (de->d_name[0] == '.') continue;
        if (*n == *cap) {
            int nc = *cap ? *cap * 2 : 16;
            char **na = realloc(*list, nc * sizeof(char *));
            if (!na) { closedir(d); return -1; }
            *list = na; *cap = nc;
        }
        char *copy = strndup(de->d_name, l - 5);
        if (!copy) { closedir(d); return -1; }
        (*list)[(*n)++] = copy;
    }
    closedir(d);
    return 0;
}

static int cmp_strp(const void *a, const void *b)
{
    const char *const *pa = a; const char *const *pb = b;
    return strcmp(*pa, *pb);
}

static int profile_list_cmd(bool json)
{
    char **list = NULL; int n = 0, cap = 0;
    char *udir = user_profile_dir();
    if (udir) { list_dir_profiles(udir, &list, &n, &cap); free(udir); }
    list_dir_profiles(SYSTEM_PROFILE_DIR, &list, &n, &cap);

    /* de-duplicate (user wins) */
    qsort(list, n, sizeof(char *), cmp_strp);
    int out_n = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && strcmp(list[i], list[i - 1]) == 0) {
            free(list[i]); list[i] = NULL;
        } else {
            list[out_n++] = list[i];
        }
    }

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < out_n; i++) {
            if (i) fputc(',', stdout);
            out_json_escape(list[i]);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        for (int i = 0; i < out_n; i++) puts(list[i]);
    }
    for (int i = 0; i < out_n; i++) free(list[i]);
    free(list);
    return 0;
}

/* ── Delete ─────────────────────────────────────────────────────── */

static int profile_delete_cmd(const char *name, bool json)
{
    if (!name || !*name) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "profile name required", "profile delete");
        return cli_err(json, &err);
    }
    char *udir = user_profile_dir();
    char *upath = udir ? profile_path(udir, name) : NULL;
    free(udir);
    char *spath = profile_path(SYSTEM_PROFILE_DIR, name);

    const char *path = NULL;
    if (upath && access(upath, W_OK) == 0) path = upath;
    else if (spath && access(spath, W_OK) == 0) path = spath;

    if (!path) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_ENOENT,
                     "profile not found or not writable", name);
        free(upath); free(spath);
        return cli_err(json, &err);
    }
    if (unlink(path) != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EIO, strerror(errno), path);
        free(upath); free(spath);
        return cli_err(json, &err);
    }
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "deleted", name);
        out_json_field_string(&first, "path", path);
        out_json_ok_end();
    } else {
        out_kv("deleted", name);
        out_kv("path", path);
    }
    free(upath); free(spath);
    return 0;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_profile(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl profile <save|load|list|delete> <name>");
    const char *sub = argv[0];
    if (strcmp(sub, "list") == 0) return profile_list_cmd(json);
    if (argc < 2)
        return cli_usage(json, "zenctl profile <save|load|delete> <name>");
    const char *name = argv[1];
    if (strcmp(sub, "save")   == 0) return profile_save_cmd(name, json, dry_run);
    if (strcmp(sub, "load")   == 0) return profile_load_cmd(name, json, dry_run, confirm);
    if (strcmp(sub, "delete") == 0) return profile_delete_cmd(name, json);
    return cli_usage(json, "zenctl profile <save|load|list|delete> <name>");
}
