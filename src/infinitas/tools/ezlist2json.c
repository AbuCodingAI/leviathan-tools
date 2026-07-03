/**
 * ezlist2json - build-time tool: EasyList text -> WebKit content-blocker JSON.
 *
 *   ezlist2json <easylist.txt> <adblock-rules.json> [max_rules]
 *
 * Runs offline; used by the Makefile to bake data/easylist.txt into
 * data/adblock-rules.json.  No WebKit dependency (glib only).
 */

#include "../src/ezlist.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <easylist.txt> <out.json> [max_rules]\n", argv[0]);
        return 2;
    }
    guint max_rules = (argc >= 4) ? (guint)strtoul(argv[3], NULL, 10) : 0;

    EzlistStats stats;
    GError *err = NULL;
    if (!ezlist_convert_file(argv[1], argv[2], max_rules, &stats, &err)) {
        fprintf(stderr, "ezlist2json: %s\n", err ? err->message : "conversion failed");
        if (err) g_error_free(err);
        return 1;
    }
    gchar *summary = ezlist_stats_to_string(&stats);
    fprintf(stderr, "%s\n  wrote %s\n", summary, argv[2]);
    g_free(summary);
    return 0;
}
