#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_open_count = 0;
static FILE *s_watched_fp = NULL;

static const char *get_conf_path(void) {
    const char *p = getenv("MODERNIME_FAULT_CONFIG_PATH");
    return p ? p : getenv("REVIEW_CONFIG_PATH");
}

static const char *get_conf_action(void) {
    const char *p = getenv("MODERNIME_FAULT_ACTION");
    return p ? p : getenv("REVIEW_CONFIG_ACTION");
}

static const char *get_conf_replacement(void) {
    const char *p = getenv("MODERNIME_FAULT_REPLACEMENT");
    return p ? p : getenv("REVIEW_REPLACEMENT");
}

static int is_unblocked(void) {
    return getenv("MODERNIME_FAULT_UNBLOCKED") != NULL || getenv("REVIEW_IO_UNBLOCKED") != NULL;
}

FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real_fopen)(const char *, const char *) = NULL;
    if (!real_fopen) {
        real_fopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    }

    const char *target = get_conf_path();
    if (target != NULL && strcmp(path, target) == 0) {
        ++s_open_count;
        const char *action = get_conf_action();
        if (action != NULL) {
            if (strcmp(action, "swap_on_hash") == 0 && s_open_count == 2) {
                const char *replacement = get_conf_replacement();
                if (replacement != NULL) {
                    rename(replacement, target);
                }
            }
            if (strcmp(action, "read_failure") == 0 && s_open_count == 1) {
                errno = EACCES;
                return NULL;
            }
            if (strcmp(action, "long_read_failure") == 0 && !is_unblocked()) {
                errno = EACCES;
                return NULL;
            }
        }
    }

    FILE *fp = real_fopen(path, mode);
    if (target != NULL && strcmp(path, target) == 0) {
        s_watched_fp = fp;
    }
    return fp;
}

int modernime_fault_open_count(void) {
    return s_open_count;
}

int review_open_count(void) {
    return s_open_count;
}

void modernime_fault_reset_count(void) {
    s_open_count = 0;
    s_watched_fp = NULL;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    static size_t (*real_fread)(void *, size_t, size_t, FILE *) = NULL;
    if (!real_fread) {
        real_fread = (size_t (*)(void *, size_t, size_t, FILE *))dlsym(RTLD_NEXT, "fread");
    }

    size_t got = real_fread(ptr, size, nmemb, stream);
    const char *action = get_conf_action();
    if (stream == s_watched_fp && action != NULL && strcmp(action, "swap_after_read") == 0 && s_open_count == 1) {
        const char *replacement = get_conf_replacement();
        const char *target = get_conf_path();
        if (replacement != NULL && target != NULL) {
            rename(replacement, target);
        }
    }
    return got;
}
