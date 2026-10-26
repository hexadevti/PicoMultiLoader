// fileops.c - copy, move and delete files and folders on the SD card (FatFS)
#include "fileops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdcard.h"

#define MAX_DEPTH 16
#define PATH_CAP  512

// The FILs and the copy buffer live on the heap: the main stack is only a few KB.
typedef struct {
    FIL in, out;
    uint8_t *buf;
    UINT buf_len;
    uint64_t done, total;
    fileops_progress_t progress;
} copy_job_t;

static const char *fr_text(FRESULT fr)
{
    switch (fr) {
    case FR_NO_FILE:
    case FR_NO_PATH:
        return "not found";
    case FR_EXIST:
        return "already exists";
    case FR_DENIED:
        return "access denied (SD card full, or a read-only file)";
    case FR_WRITE_PROTECTED:
        return "the SD card is write-protected";
    case FR_INVALID_NAME:
        return "the name or path is too long";
    case FR_NOT_READY:
    case FR_DISK_ERR:
        return "SD card error";
    case FR_NOT_ENOUGH_CORE:
        return "out of memory";
    default:
        return "file system error";
    }
}

static bool fail(char *err, size_t err_len, const char *what, FRESULT fr)
{
    snprintf(err, err_len, "%s: %s", what, fr_text(fr));
    return false;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool fileops_exists(const char *path)
{
    if (!path[0] || strcmp(path, "/") == 0) {
        return true;
    }
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
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

// Bytes in a file, or in everything under a folder.
static FRESULT tree_size(char *path, int depth, uint64_t *total)
{
    FILINFO *fi = malloc(sizeof(FILINFO));
    if (!fi) {
        return FR_NOT_ENOUGH_CORE;
    }
    FRESULT fr = f_stat(path, fi);
    if (fr != FR_OK || !(fi->fattrib & AM_DIR)) {
        *total += fr == FR_OK ? fi->fsize : 0;
        free(fi);
        return fr;
    }
    DIR dir;
    if (depth > MAX_DEPTH || (fr = f_opendir(&dir, path)) != FR_OK) {
        free(fi);
        return depth > MAX_DEPTH ? FR_INVALID_NAME : fr;
    }
    size_t len = strlen(path);
    while ((fr = f_readdir(&dir, fi)) == FR_OK && fi->fname[0]) {
        if (!(fi->fattrib & AM_DIR)) {
            *total += fi->fsize;
        } else if (push(path, fi->fname)) {
            fr = tree_size(path, depth + 1, total);
            path[len] = '\0';
            if (fr != FR_OK) {
                break;
            }
        }
    }
    f_closedir(&dir);
    free(fi);
    return fr;
}

static FRESULT copy_file(copy_job_t *job, const char *src, const char *dst, const FILINFO *src_info)
{
    FRESULT fr = f_open(&job->in, src, FA_READ);
    if (fr != FR_OK) {
        return fr;
    }
    if ((fr = f_open(&job->out, dst, FA_WRITE | FA_CREATE_NEW)) != FR_OK) {
        f_close(&job->in);
        return fr;
    }
    const char *name = base_name(src);
    job->progress(name, job->done, job->total);
    for (;;) {
        UINT got = 0, put = 0;
        if ((fr = f_read(&job->in, job->buf, job->buf_len, &got)) != FR_OK || got == 0) {
            break;
        }
        if ((fr = f_write(&job->out, job->buf, got, &put)) != FR_OK) {
            break;
        }
        if (put < got) { // the card is full
            fr = FR_DENIED;
            break;
        }
        job->done += got;
        job->progress(name, job->done, job->total);
    }
    f_close(&job->in);
    FRESULT cr = f_close(&job->out);
    if (fr == FR_OK) {
        fr = cr;
    }
    if (fr != FR_OK) {
        f_unlink(dst); // don't leave half a file behind
        return fr;
    }
    f_utime(dst, src_info); // keep the original date
    return FR_OK;
}

static FRESULT copy_tree(copy_job_t *job, char *src, char *dst, int depth)
{
    FILINFO *fi = malloc(sizeof(FILINFO));
    if (!fi) {
        return FR_NOT_ENOUGH_CORE;
    }
    FRESULT fr = f_stat(src, fi);
    if (fr == FR_OK && !(fi->fattrib & AM_DIR)) {
        fr = copy_file(job, src, dst, fi);
        free(fi);
        return fr;
    }
    DIR dir;
    if (fr != FR_OK || depth > MAX_DEPTH || (fr = f_mkdir(dst)) != FR_OK || (fr = f_opendir(&dir, src)) != FR_OK) {
        free(fi);
        return fr != FR_OK ? fr : FR_INVALID_NAME;
    }
    size_t src_len = strlen(src), dst_len = strlen(dst);
    while ((fr = f_readdir(&dir, fi)) == FR_OK && fi->fname[0]) {
        if (!push(src, fi->fname) || !push(dst, fi->fname)) {
            fr = FR_INVALID_NAME;
        } else {
            fr = copy_tree(job, src, dst, depth + 1);
        }
        src[src_len] = dst[dst_len] = '\0';
        if (fr != FR_OK) {
            break;
        }
    }
    f_closedir(&dir);
    free(fi);
    return fr;
}

bool fileops_copy(const char *src, const char *dst, fileops_progress_t progress, char *err, size_t err_len)
{
    copy_job_t *job = calloc(1, sizeof(copy_job_t));
    char *s = malloc(PATH_CAP), *d = malloc(PATH_CAP);
    if (job) {
        job->buf_len = 4096;
        job->buf = malloc(job->buf_len);
        if (!job->buf) {
            job->buf_len = 512;
            job->buf = malloc(job->buf_len);
        }
    }
    FRESULT fr = FR_NOT_ENOUGH_CORE;
    if (job && job->buf && s && d) {
        strlcpy(s, src, PATH_CAP);
        strlcpy(d, dst, PATH_CAP);
        job->progress = progress;
        fr = tree_size(s, 0, &job->total);
        if (fr == FR_OK) {
            fr = copy_tree(job, s, d, 0);
        }
    }
    if (job) {
        free(job->buf);
    }
    free(job);
    free(s);
    free(d);
    return fr == FR_OK || fail(err, err_len, "Copy failed", fr);
}

bool fileops_move(const char *src, const char *dst, char *err, size_t err_len)
{
    FRESULT fr = f_rename(src, dst);
    return fr == FR_OK || fail(err, err_len, "Move failed", fr);
}

static FRESULT remove_tree(char *path, int depth)
{
    FILINFO *fi = malloc(sizeof(FILINFO));
    if (!fi) {
        return FR_NOT_ENOUGH_CORE;
    }
    FRESULT fr = f_stat(path, fi);
    if (fr != FR_OK || !(fi->fattrib & AM_DIR)) {
        free(fi);
        return fr != FR_OK ? fr : f_unlink(path);
    }
    size_t len = strlen(path);
    while (depth <= MAX_DEPTH) {
        // reopen after each removal: deleting while iterating a FAT directory skips entries
        DIR dir;
        if ((fr = f_opendir(&dir, path)) != FR_OK) {
            break;
        }
        fr = f_readdir(&dir, fi);
        f_closedir(&dir);
        if (fr != FR_OK || !fi->fname[0]) {
            break;
        }
        if (!push(path, fi->fname)) {
            fr = FR_INVALID_NAME;
            break;
        }
        fr = remove_tree(path, depth + 1);
        path[len] = '\0';
        if (fr != FR_OK) {
            break;
        }
    }
    free(fi);
    if (fr == FR_OK && depth > MAX_DEPTH) {
        fr = FR_INVALID_NAME;
    }
    return fr != FR_OK ? fr : f_unlink(path);
}

bool fileops_delete(const char *path, char *err, size_t err_len)
{
    char *p = malloc(PATH_CAP);
    FRESULT fr = FR_NOT_ENOUGH_CORE;
    if (p) {
        strlcpy(p, path, PATH_CAP);
        fr = remove_tree(p, 0);
        free(p);
    }
    return fr == FR_OK || fail(err, err_len, "Delete failed", fr);
}
