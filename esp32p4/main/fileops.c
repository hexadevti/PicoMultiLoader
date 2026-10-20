// fileops.c - copy, move and delete files and folders on the SD card (POSIX calls on the VFS)
#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>

#define MAX_DEPTH 16
#define PATH_CAP  512
#define BUF_LEN   8192

typedef struct {
    uint8_t *buf;
    uint64_t done, total;
    fileops_progress_t progress;
} copy_job_t;

static const char *err_text(int e)
{
    switch (e) {
    case ENOENT:
        return "not found";
    case EEXIST:
        return "already exists";
    case ENOSPC:
        return "the SD card is full";
    case EACCES:
    case EPERM:
    case EROFS:
        return "access denied (a read-only file or card)";
    case ENAMETOOLONG:
        return "the name or path is too long";
    case ENOMEM:
        return "out of memory";
    case EIO:
        return "SD card error";
    default:
        return "file system error";
    }
}

static bool fail(char *err, size_t err_len, const char *what, int e)
{
    snprintf(err, err_len, "%s: %s", what, err_text(e));
    return false;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool fileops_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// Append "/name" to `path` (a PATH_CAP buffer); false if it doesn't fit.
static bool push(char *path, const char *name)
{
    size_t len = strlen(path);
    if (len + 1 + strlen(name) + 1 > PATH_CAP) {
        return false;
    }
    snprintf(path + len, PATH_CAP - len, "/%s", name);
    return true;
}

static bool dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

// Bytes in a file, or in everything under a folder; 0 or an errno.
static int tree_size(char *path, int depth, uint64_t *total)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        *total += st.st_size;
        return 0;
    }
    if (depth > MAX_DEPTH) {
        return ENAMETOOLONG;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return errno;
    }
    int e = 0;
    size_t len = strlen(path);
    struct dirent *de;
    while (!e && (de = readdir(dir)) != NULL) {
        if (dot_entry(de->d_name)) {
            continue;
        }
        e = push(path, de->d_name) ? tree_size(path, depth + 1, total) : ENAMETOOLONG;
        path[len] = '\0';
    }
    closedir(dir);
    return e;
}

static int copy_file(copy_job_t *job, const char *src, const char *dst, const struct stat *src_st)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        return errno;
    }
    if (fileops_exists(dst)) {
        fclose(in);
        return EEXIST;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        int e = errno;
        fclose(in);
        return e;
    }
    const char *name = base_name(src);
    job->progress(name, job->done, job->total);
    int e = 0;
    for (;;) {
        size_t got = fread(job->buf, 1, BUF_LEN, in);
        if (got == 0) {
            e = ferror(in) ? (errno ? errno : EIO) : 0;
            break;
        }
        if (fwrite(job->buf, 1, got, out) != got) {
            e = errno ? errno : ENOSPC;
            break;
        }
        job->done += got;
        job->progress(name, job->done, job->total);
    }
    fclose(in);
    if (fclose(out) != 0 && !e) {
        e = errno ? errno : EIO;
    }
    if (e) {
        unlink(dst); // don't leave half a file behind
        return e;
    }
    struct utimbuf times = {.actime = src_st->st_atime, .modtime = src_st->st_mtime};
    utime(dst, &times); // keep the original date
    return 0;
}

static int copy_tree(copy_job_t *job, char *src, char *dst, int depth)
{
    struct stat st;
    if (stat(src, &st) != 0) {
        return errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return copy_file(job, src, dst, &st);
    }
    if (depth > MAX_DEPTH) {
        return ENAMETOOLONG;
    }
    if (mkdir(dst, 0777) != 0) {
        return errno;
    }
    DIR *dir = opendir(src);
    if (!dir) {
        return errno;
    }
    int e = 0;
    size_t src_len = strlen(src), dst_len = strlen(dst);
    struct dirent *de;
    while (!e && (de = readdir(dir)) != NULL) {
        if (dot_entry(de->d_name)) {
            continue;
        }
        e = push(src, de->d_name) && push(dst, de->d_name) ? copy_tree(job, src, dst, depth + 1) : ENAMETOOLONG;
        src[src_len] = dst[dst_len] = '\0';
    }
    closedir(dir);
    return e;
}

bool fileops_copy(const char *src, const char *dst, fileops_progress_t progress, char *err, size_t err_len)
{
    copy_job_t job = {.buf = malloc(BUF_LEN), .progress = progress};
    char *s = malloc(PATH_CAP), *d = malloc(PATH_CAP);
    int e = ENOMEM;
    if (job.buf && s && d) {
        strlcpy(s, src, PATH_CAP);
        strlcpy(d, dst, PATH_CAP);
        e = tree_size(s, 0, &job.total);
        if (!e) {
            e = copy_tree(&job, s, d, 0);
        }
    }
    free(job.buf);
    free(s);
    free(d);
    return !e || fail(err, err_len, "Copy failed", e);
}

bool fileops_move(const char *src, const char *dst, char *err, size_t err_len)
{
    return rename(src, dst) == 0 || fail(err, err_len, "Move failed", errno);
}

static int remove_tree(char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : errno;
    }
    if (depth > MAX_DEPTH) {
        return ENAMETOOLONG;
    }
    size_t len = strlen(path);
    for (;;) {
        // reopen after each removal: deleting while iterating a FAT directory skips entries
        DIR *dir = opendir(path);
        if (!dir) {
            return errno;
        }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL && dot_entry(de->d_name)) {
        }
        bool more = de && push(path, de->d_name);
        bool too_long = de && !more;
        closedir(dir);
        if (too_long) {
            return ENAMETOOLONG;
        }
        if (!more) {
            break;
        }
        int e = remove_tree(path, depth + 1);
        path[len] = '\0';
        if (e) {
            return e;
        }
    }
    return rmdir(path) == 0 ? 0 : errno;
}

bool fileops_delete(const char *path, char *err, size_t err_len)
{
    char *p = malloc(PATH_CAP);
    int e = ENOMEM;
    if (p) {
        strlcpy(p, path, PATH_CAP);
        e = remove_tree(p, 0);
        free(p);
    }
    return !e || fail(err, err_len, "Delete failed", e);
}
