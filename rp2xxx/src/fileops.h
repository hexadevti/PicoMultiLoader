// fileops.h - copy, move and delete files and folders on the SD card (the file list's F2 menu)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Called while copying: the file being copied and the bytes copied of the whole job so far. */
typedef void (*fileops_progress_t)(const char *name, uint64_t done, uint64_t total);

/** True if something (file or folder) exists at `path` (FatFS path, "" or "/" = the root). */
bool fileops_exists(const char *path);

/** Copy a file, or a folder and everything in it, to `dst` (which must not exist yet). */
bool fileops_copy(const char *src, const char *dst, fileops_progress_t progress, char *err, size_t err_len);

/** Move/rename a file or folder to `dst` (which must not exist yet). */
bool fileops_move(const char *src, const char *dst, char *err, size_t err_len);

/** Delete a file, or a folder and everything in it. */
bool fileops_delete(const char *path, char *err, size_t err_len);
