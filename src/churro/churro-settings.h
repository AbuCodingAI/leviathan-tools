/*
 * churro-settings.h — shared config for all Churro widgets.
 *
 * Header-only so every widget (which the Makefile compiles as a standalone
 * translation unit) can read ~/.config/churro/settings.conf without linking.
 * Functions are `static inline` to avoid one-definition-rule / unused warnings.
 */
#ifndef CHURRO_SETTINGS_H
#define CHURRO_SETTINGS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    unsigned long dock_bg_color;
    unsigned long item_bg_color;
    unsigned long text_color;
    unsigned long accent_color;
    unsigned long border_color;
    int corner_radius;
    int dock_height;
    int icon_size;
    int opacity;
} ChurroSettings;

static inline ChurroSettings churro_default_settings(void) {
    ChurroSettings s;
    s.dock_bg_color  = 0x4a4a54;
    s.item_bg_color  = 0x5a5a64;
    s.text_color     = 0xaabbcc;
    s.accent_color   = 0x4a9aff;
    s.border_color   = 0x2a3a5a;
    s.corner_radius  = 24;
    s.dock_height    = 80;
    s.icon_size      = 48;
    s.opacity        = 95;
    return s;
}

static inline void churro_config_path(char *buf, size_t n) {
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/churro/settings.conf", home ? home : "/tmp");
}

/* Load settings from disk, falling back to defaults for any missing key. */
static inline void churro_load_settings(ChurroSettings *s) {
    *s = churro_default_settings();

    char path[512];
    churro_config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    unsigned int tmp;
    while (fgets(line, sizeof(line), f)) {
        if      (sscanf(line, "background_color=0x%x", &tmp) == 1) s->dock_bg_color = tmp;
        else if (sscanf(line, "item_color=0x%x",       &tmp) == 1) s->item_bg_color = tmp;
        else if (sscanf(line, "text_color=0x%x",       &tmp) == 1) s->text_color    = tmp;
        else if (sscanf(line, "accent_color=0x%x",     &tmp) == 1) s->accent_color  = tmp;
        else if (sscanf(line, "border_color=0x%x",     &tmp) == 1) s->border_color  = tmp;
        else if (sscanf(line, "corner_radius=%d", &s->corner_radius) == 1) { }
        else if (sscanf(line, "height=%d",        &s->dock_height)   == 1) { }
        else if (sscanf(line, "icon_size=%d",     &s->icon_size)     == 1) { }
        else if (sscanf(line, "opacity=%d",       &s->opacity)       == 1) { }
    }
    fclose(f);
}

static inline void churro_ensure_config_dir(void) {
    char dir[512];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/.config", home ? home : "/tmp");
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/churro", home ? home : "/tmp");
    mkdir(dir, 0755);
}

static inline void churro_save_settings(const ChurroSettings *s) {
    churro_ensure_config_dir();
    char path[512];
    churro_config_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "[Dock]\n");
    fprintf(f, "background_color=0x%06lx\n", s->dock_bg_color);
    fprintf(f, "item_color=0x%06lx\n",       s->item_bg_color);
    fprintf(f, "text_color=0x%06lx\n",       s->text_color);
    fprintf(f, "accent_color=0x%06lx\n",     s->accent_color);
    fprintf(f, "border_color=0x%06lx\n",     s->border_color);
    fprintf(f, "corner_radius=%d\n", s->corner_radius);
    fprintf(f, "height=%d\n",        s->dock_height);
    fprintf(f, "icon_size=%d\n",     s->icon_size);
    fprintf(f, "opacity=%d\n",       s->opacity);
    fclose(f);
}

#endif /* CHURRO_SETTINGS_H */
