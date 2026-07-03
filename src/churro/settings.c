/*
 * Churro Settings — CLI + interactive editor for ~/.config/churro/settings.conf
 *
 * The struct, defaults and load/save live in churro-settings.h so that every
 * widget shares exactly one config format. Changing values here (or editing the
 * conf file directly) drives the look of the dock and the other widgets.
 */
#include "churro-settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_current(const ChurroSettings *s) {
    printf("  Dock Background: 0x%06lx\n", s->dock_bg_color);
    printf("  Item Background: 0x%06lx\n", s->item_bg_color);
    printf("  Text Color:      0x%06lx\n", s->text_color);
    printf("  Accent Color:    0x%06lx\n", s->accent_color);
    printf("  Border Color:    0x%06lx\n", s->border_color);
    printf("  Corner Radius:   %d\n",      s->corner_radius);
    printf("  Dock Height:     %d\n",      s->dock_height);
    printf("  Icon Size:       %d\n",      s->icon_size);
    printf("  Opacity:         %d%%\n",    s->opacity);
}

static void print_menu(void) {
    printf("\n=== Churro Settings ===\n");
    printf("1. View current settings\n");
    printf("2. Change dock background color\n");
    printf("3. Change item background color\n");
    printf("4. Change text color\n");
    printf("5. Change accent color\n");
    printf("6. Change border color\n");
    printf("7. Change corner radius\n");
    printf("8. Change dock height\n");
    printf("9. Change icon size\n");
    printf("10. Change opacity\n");
    printf("11. Reset to defaults\n");
    printf("0. Exit\n");
    printf("Choice: ");
}

static int read_int(void) {
    int v = 0;
    if (scanf("%d", &v) != 1) { v = 0; }
    return v;
}

static unsigned long read_hex(void) {
    unsigned int v = 0;
    if (scanf("%x", &v) != 1) { v = 0; }
    return v;
}

int main(int argc, char *argv[]) {
    ChurroSettings settings;
    churro_load_settings(&settings);

    /* Create the file on first run so widgets have something to read. */
    {
        char path[512];
        churro_config_path(path, sizeof(path));
        if (access(path, F_OK) != 0) churro_save_settings(&settings);
    }

    if (argc > 1) {
        if (strcmp(argv[1], "--list") == 0) {
            printf("Churro Settings:\n");
            print_current(&settings);
            return 0;
        } else if (strcmp(argv[1], "--set") == 0 && argc > 3) {
            char *key = argv[2];
            char *value = argv[3];
            unsigned int c;

            if (strcmp(key, "dock_bg") == 0 && sscanf(value, "0x%x", &c) == 1)      settings.dock_bg_color = c;
            else if (strcmp(key, "item_bg") == 0 && sscanf(value, "0x%x", &c) == 1) settings.item_bg_color = c;
            else if (strcmp(key, "text_color") == 0 && sscanf(value, "0x%x", &c)==1) settings.text_color = c;
            else if (strcmp(key, "accent_color") == 0 && sscanf(value,"0x%x",&c)==1) settings.accent_color = c;
            else if (strcmp(key, "border_color") == 0 && sscanf(value,"0x%x",&c)==1) settings.border_color = c;
            else if (strcmp(key, "corner_radius") == 0) settings.corner_radius = atoi(value);
            else if (strcmp(key, "dock_height") == 0)   settings.dock_height = atoi(value);
            else if (strcmp(key, "icon_size") == 0)     settings.icon_size = atoi(value);
            else if (strcmp(key, "opacity") == 0)       settings.opacity = atoi(value);

            churro_save_settings(&settings);
            printf("[Settings] Updated %s = %s\n", key, value);
            return 0;
        } else if (strcmp(argv[1], "--reset") == 0) {
            settings = churro_default_settings();
            churro_save_settings(&settings);
            printf("[Settings] Reset to defaults\n");
            return 0;
        }
    }

    /* Interactive mode */
    while (1) {
        print_menu();
        int choice = read_int();
        getchar();

        if (choice == 0) break;

        switch (choice) {
        case 1:  printf("\nCurrent Settings:\n"); print_current(&settings); break;
        case 2:  printf("Enter dock background color (hex, e.g., 4a4a54): "); settings.dock_bg_color = read_hex(); break;
        case 3:  printf("Enter item background color (hex, e.g., 5a5a64): "); settings.item_bg_color = read_hex(); break;
        case 4:  printf("Enter text color (hex, e.g., aabbcc): ");            settings.text_color = read_hex(); break;
        case 5:  printf("Enter accent color (hex, e.g., 4a9aff): ");          settings.accent_color = read_hex(); break;
        case 6:  printf("Enter border color (hex, e.g., 2a3a5a): ");          settings.border_color = read_hex(); break;
        case 7:  printf("Enter corner radius (8-32): ");   settings.corner_radius = read_int(); break;
        case 8:  printf("Enter dock height (60-120): ");   settings.dock_height = read_int(); break;
        case 9:  printf("Enter icon size (32-64): ");      settings.icon_size = read_int(); break;
        case 10: printf("Enter opacity (0-100): ");        settings.opacity = read_int(); break;
        case 11: settings = churro_default_settings(); printf("Reset to defaults\n"); break;
        default: printf("Invalid choice\n"); continue;
        }

        churro_save_settings(&settings);
        printf("Settings saved!\n");
    }

    return 0;
}
