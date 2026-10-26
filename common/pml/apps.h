// apps.h - the apps on the Home screen: every top-level folder with an app.ini or a .uf2 in it, and
// the loose .uf2 files in the SD root. Favourites and recently run apps are kept as paths, one per
// line, in /pml/favourites.txt and /pml/recents.txt; ones that aren't top-level apps are listed too.
//
// app.ini (all keys optional; " ;" starts a comment):
//   [app]
//   name     = NES
//   author   = picocalc community
//   version  = 1.4.0
//   category = Emulators          ; Emulators | Tools | Dev | Media
//   icon     = icon.bmp           ; 32x32, 4-bit BMP: pixel value = palette role (look.h), 0 = clear
//   binary   = picocalc-nes.uf2   ; binary.rp2040 / binary.rp2350 win over it
//   board    = rp2350             ; p4 | rp2040 | rp2350 | any (a list is fine); other boards hide it
//   desc     = NES emulator with save states.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_MAX   64
#define APPS_POOL  2048
#define APP_NONE   0xFFFF // no string
#define PML_DIR    "/pml"

enum { APP_CAT_NONE, APP_CAT_EMULATORS, APP_CAT_TOOLS, APP_CAT_DEV, APP_CAT_MEDIA, APP_CATS };

enum {
    APP_DIR   = 1, // a folder (else a loose .uf2)
    APP_FAV   = 2,
    APP_EXTRA = 4, // not in the root: listed because it is a favourite or was run recently
};

typedef struct {
    uint16_t path;   // pool offsets: "/nes", "/uLisp.uf2"
    uint16_t title;
    uint16_t binary; // folders: the .uf2 in it to run; APP_NONE = none or several (opens in Files)
    uint16_t icon;   // folders: icon file if app.ini names one other than icon.bmp
    uint8_t cat;
    uint8_t flags;
    int8_t recent;   // 0 = run last, -1 = not recently
} app_t;

typedef struct {
    app_t list[APPS_MAX]; // sorted by title
    uint16_t count, used;
    char pool[APPS_POOL];
} apps_t;

typedef struct {
    char version[16];
    char author[32];
    char desc[96];
} app_info_t;

/** Category name ("Emulators"...), "" for none. */
const char *apps_cat_name(int cat);

/** Fill `a` from the card. */
void apps_scan(apps_t *a);

/** Fill `a` with what is in the folder `dir` ("" = the root, as apps_scan()): its .uf2 files, and
 *  its folders (any folder, flagged APP_EXTRA, except in the root). */
void apps_scan_dir(apps_t *a, const char *dir);

/** A pool string, "" for APP_NONE. */
const char *apps_str(const apps_t *a, uint16_t off);

/** Path of the .uf2 to run (the file itself, or the folder's binary); false if there is none. */
bool apps_binary_path(const apps_t *a, const app_t *e, char *out, size_t len);

/** Version, author and description from the app.ini; false if there is none. */
bool apps_info(const apps_t *a, const app_t *e, app_info_t *out);

/** Draw the app's 32x32 icon: its icon.bmp, else a generic one for its kind. */
void apps_draw_icon(const apps_t *a, const app_t *e, int x, int y);

/** True if `path` (a file or folder) is a favourite. */
bool apps_is_fav(const char *path);

/** Add or remove a favourite: 1 = now a favourite, 0 = not any more, -1 = could not save. */
int apps_toggle_fav(const char *path);

/** Put `path` first in the recently run list. */
void apps_add_recent(const char *path);

/** Forget the cached lists (the card was changed or swapped). */
void apps_reload(void);
