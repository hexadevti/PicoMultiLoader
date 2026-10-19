#include "config.h"

#include <stdio.h>
#include <string.h>
#include "ff.h"

#define MAX_KEYS  16
#define KEY_LEN   16
#define VALUE_LEN 160

typedef struct {
    char key[KEY_LEN];
    char value[VALUE_LEN];
} setting_t;

static setting_t s_items[MAX_KEYS];
static int s_count;
static bool s_loaded;

static void load(void)
{
    if (s_loaded) {
        return;
    }
    s_count = 0;
    FIL f;
    if (f_open(&f, CONFIG_FILE, FA_READ) != FR_OK) {
        s_loaded = true; // no file yet
        return;
    }
    char line[KEY_LEN + VALUE_LEN + 4];
    while (s_count < MAX_KEYS && f_gets(line, sizeof(line), &f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line || eq - line >= KEY_LEN) {
            continue;
        }
        *eq = '\0';
        strlcpy(s_items[s_count].key, line, KEY_LEN);
        strlcpy(s_items[s_count].value, eq + 1, VALUE_LEN);
        s_count++;
    }
    f_close(&f);
    s_loaded = true;
}

static setting_t *find(const char *key)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_items[i].key, key) == 0) {
            return &s_items[i];
        }
    }
    return NULL;
}

bool config_get(const char *key, char *buf, size_t len)
{
    load();
    setting_t *s = find(key);
    if (!s || len == 0) {
        return false;
    }
    snprintf(buf, len, "%s", s->value);
    return true;
}

bool config_set(const char *key, const char *value)
{
    load();
    setting_t *s = find(key);
    if (!value[0]) {
        if (s) {
            *s = s_items[--s_count];
        }
    } else {
        if (!s) {
            if (s_count >= MAX_KEYS || strlen(key) >= KEY_LEN) {
                return false;
            }
            s = &s_items[s_count++];
            strcpy(s->key, key);
        }
        snprintf(s->value, VALUE_LEN, "%s", value);
    }

    FIL f;
    if (f_open(&f, CONFIG_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    bool ok = f_puts("# PicoMultiLoader settings\n", &f) >= 0;
    for (int i = 0; ok && i < s_count; i++) {
        ok = f_printf(&f, "%s=%s\n", s_items[i].key, s_items[i].value) >= 0;
    }
    return f_close(&f) == FR_OK && ok;
}

void config_reload(void)
{
    s_loaded = false;
}
