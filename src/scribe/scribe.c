#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTENT 65536
#define MAX_PATH 512

typedef struct {
    GtkWindow *window;
    WebKitWebView *web_view;
    char current_file[MAX_PATH];
    char content[MAX_CONTENT];   /* text loaded from argv, injected once page is ready */
    gchar *pending_html;         /* last editor HTML received from JS (for save)        */
    gchar *pending_text;         /* last editor plain text received from JS (for save)  */
    gboolean is_dirty;           /* mirror of the JS dirty flag                         */
    gboolean initial_loaded;     /* argv content injected already                       */
} ScribeApp;

/* ------------------------------------------------------------------ *
 *  HTML / JS front-end.  The C backend never injects raw file text
 *  into HTML; content crosses the bridge base64-encoded and is
 *  decoded (UTF-8 safe) with TextDecoder, so no character can break
 *  the page or cause injection.
 * ------------------------------------------------------------------ */
static const char *HTML_TEMPLATE =
"<!DOCTYPE html><html><head><meta charset='UTF-8'><style>"
"*{margin:0;padding:0;box-sizing:border-box;}"
"html,body{height:100%;}"
"body{font-family:monospace;background:#0a0f0a;color:#33ff33;display:flex;flex-direction:column;}"
"#toolbar{background:#0a140f;padding:8px;border-bottom:2px solid #33ff33;display:flex;gap:5px;flex-wrap:wrap;align-items:center;}"
".btn{padding:5px 11px;background:#030705;color:#33ff33;border:1px solid #33ff33;cursor:pointer;font-size:11px;font-weight:bold;font-family:monospace;transition:all .15s;}"
".btn:hover{background:#33ff33;color:#030705;}"
".btn:active{transform:scale(.97);}"
".fmt{padding:4px 8px;font-size:11px;}"
"select.btn{padding:4px 6px;}"
".sep{width:1px;height:20px;background:#1c5c2c;margin:0 3px;}"
"#page-area{flex:1;overflow:auto;padding:30px 10px;display:flex;justify-content:center;background:#0a0f0a;}"
"#editor{width:100%;max-width:816px;min-height:1056px;height:max-content;padding:72px 84px;"
"background:#fff;color:#111;font-family:'Times New Roman',serif;font-size:16px;line-height:1.8;"
"box-shadow:0 0 22px rgba(51,255,51,.25);border:1px solid #33ff33;}"
"#editor:focus{outline:none;}"
"#editor.mla{padding:96px;line-height:2;}"
"#editor h1{font-size:28px;margin:.4em 0;}"
"#editor h2{font-size:22px;margin:.4em 0;}"
"#editor ul,#editor ol{margin-left:1.4em;}"
"#status{background:#0a140f;border-top:2px solid #33ff33;padding:5px 12px;display:flex;"
"justify-content:space-between;font-size:11px;color:#33ff33;font-family:monospace;}"
"#aboutov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;align-items:center;justify-content:center;}"
"#aboutov.on{display:flex;}"
"#aboutbox{background:#0a140f;border:2px solid #33ff33;color:#33ff33;max-width:480px;margin:16px;padding:20px;font-family:monospace;font-size:12px;line-height:1.7;box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#aboutbox .atitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#aboutbox .aok{margin-top:16px;text-align:right;}"
"</style></head><body>"
"<div id='toolbar'>"
"<button class='btn' onclick='openFile()' title='Ctrl+O'>OPEN</button>"
"<button class='btn' onclick='saveFile()' title='Ctrl+S'>SAVE</button>"
"<button class='btn' onclick='newFile()' title='Ctrl+N'>NEW</button>"
"<span class='sep'></span>"
"<button class='btn fmt' onclick='cmd(\"bold\")' title='Ctrl+B'><b>B</b></button>"
"<button class='btn fmt' onclick='cmd(\"italic\")' title='Ctrl+I'><i>I</i></button>"
"<button class='btn fmt' onclick='cmd(\"underline\")' title='Ctrl+U'><u>U</u></button>"
"<span class='sep'></span>"
"<select class='btn' onchange='block(this.value);this.selectedIndex=0;'>"
"<option value=''>STYLE</option>"
"<option value='h1'>Heading 1</option>"
"<option value='h2'>Heading 2</option>"
"<option value='p'>Paragraph</option>"
"</select>"
"<select class='btn' onchange='fontSize(this.value);this.selectedIndex=0;'>"
"<option value=''>SIZE</option>"
"<option value='2'>Small</option>"
"<option value='3'>Normal</option>"
"<option value='5'>Large</option>"
"<option value='7'>Huge</option>"
"</select>"
"<button class='btn fmt' onclick='cmd(\"insertUnorderedList\")' title='Bullet list'>&bull; LIST</button>"
"<button class='btn fmt' onclick='cmd(\"insertOrderedList\")' title='Numbered list'>1. LIST</button>"
"<span class='sep'></span>"
"<button class='btn' onclick='findReplace()' title='Ctrl+F'>FIND</button>"
"<button class='btn' onclick='toggleMLA()'>MLA</button>"
"<span class='sep'></span>"
"<button class='btn' onclick='showAbout()' title='About Scribe'>ABOUT</button>"
"</div>"
"<div id='page-area'><div id='editor' contenteditable='true'></div></div>"
"<div id='status'><span id='msg'>&gt; READY</span><span id='count'>0 words | 0 chars</span></div>"
"<script>"
"function ed(){return document.getElementById('editor');}"
"var _dirty=false;"
"function setDirty(v){if(v!==_dirty){_dirty=v;window.webkit.messageHandlers.dirty.postMessage(v?'1':'0');}}"
"function setStatus(m){var s=document.getElementById('msg');s.innerText='> '+m;"
"clearTimeout(window._st);window._st=setTimeout(function(){s.innerText='> READY';},2000);}"
"function updateCount(){var t=ed().innerText;var tt=t.replace(/\\s+/g,' ').trim();"
"var w=tt?tt.split(' ').length:0;"
"document.getElementById('count').innerText=w+' words | '+t.length+' chars';}"
"function cmd(c){document.execCommand(c,false,null);ed().focus();setDirty(true);updateCount();}"
"function block(t){if(t){document.execCommand('formatBlock',false,t);ed().focus();setDirty(true);}}"
"function fontSize(s){if(s){document.execCommand('fontSize',false,s);ed().focus();setDirty(true);}}"
"function openFile(){window.webkit.messageHandlers.openFile.postMessage('');}"
"function saveFile(){window.webkit.messageHandlers.save.postMessage({html:ed().innerHTML,text:ed().innerText});}"
"function newFile(){if(_dirty&&!confirm('Discard unsaved changes and start a new document?'))return;"
"ed().innerHTML='';setDirty(false);updateCount();setStatus('NEW DOCUMENT');ed().focus();}"
"var mla=false;"
"function toggleMLA(){mla=!mla;ed().classList.toggle('mla',mla);setStatus(mla?'MLA MODE ON':'MLA MODE OFF');}"
"function findReplace(){var f=prompt('Find:');if(!f)return;var r=prompt('Replace \"'+f+'\" with:');"
"if(r===null)return;var e=ed();var n=e.innerHTML.split(f).length-1;e.innerHTML=e.innerHTML.split(f).join(r);"
"setDirty(true);updateCount();setStatus('REPLACED '+n+' MATCH(ES)');}"
/* called from C after loading a file's text (base64, UTF-8 safe) */
"function loadB64(b){try{var bin=atob(b);var a=new Uint8Array(bin.length);"
"for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);"
"ed().innerText=new TextDecoder('utf-8').decode(a);setDirty(false);updateCount();"
"setStatus('LOADED');}catch(e){setStatus('LOAD ERROR');}}"
"ed().addEventListener('input',function(){setDirty(true);updateCount();});"
"document.addEventListener('keydown',function(e){if(!e.ctrlKey)return;var k=e.key.toLowerCase();"
"if(k==='s'){e.preventDefault();saveFile();}"
"else if(k==='o'){e.preventDefault();openFile();}"
"else if(k==='n'){e.preventDefault();newFile();}"
"else if(k==='b'){e.preventDefault();cmd('bold');}"
"else if(k==='i'){e.preventDefault();cmd('italic');}"
"else if(k==='u'){e.preventDefault();cmd('underline');}"
"else if(k==='f'){e.preventDefault();findReplace();}});"
"function showAbout(){document.getElementById('aboutov').classList.add('on');}"
"function closeAbout(){document.getElementById('aboutov').classList.remove('on');}"
"updateCount();ed().focus();"
"</script>"
"<div id='aboutov' onclick='if(event.target===this)closeAbout()'>"
"<div id='aboutbox'>"
"<div class='atitle'>Scribe &mdash; part of LeviathanOS</div>"
"Free software under the GNU General Public License, version 3.<br>"
"This program comes with ABSOLUTELY NO WARRANTY.<br>"
"Full license: /usr/share/doc/leviathanos/LICENSE<br>"
"https://www.gnu.org/licenses/gpl-3.0.html"
"<div class='aok'><button class='btn' onclick='closeAbout()'>OK</button></div>"
"</div></div>"
"</body></html>";

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */

/* Standard base64 encoder. Returns a malloc'd NUL-terminated string. */
static char *b64_encode(const unsigned char *data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i, j;
    for (i = 0, j = 0; i < len;) {
        unsigned oa = i < len ? data[i++] : 0;
        unsigned ob = i < len ? data[i++] : 0;
        unsigned oc = i < len ? data[i++] : 0;
        unsigned triple = (oa << 16) | (ob << 8) | oc;
        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = tbl[(triple >> 6) & 0x3F];
        out[j++] = tbl[triple & 0x3F];
    }
    switch (len % 3) {
        case 1: out[olen - 1] = '='; out[olen - 2] = '='; break;
        case 2: out[olen - 1] = '='; break;
        default: break;
    }
    out[olen] = '\0';
    return out;
}

/* Inject plain text into the editor safely via base64 + TextDecoder. */
static void load_text_into_editor(ScribeApp *app, const char *text) {
    char *b64 = b64_encode((const unsigned char *)text, strlen(text));
    if (!b64) return;
    char *js = g_strdup_printf("loadB64('%s');", b64);
    webkit_web_view_evaluate_javascript(app->web_view, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
    g_free(js);
    free(b64);
}

static void run_js(ScribeApp *app, const char *js) {
    webkit_web_view_evaluate_javascript(app->web_view, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
}

static gboolean has_ext(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n >= e && g_ascii_strcasecmp(path + n - e, ext) == 0;
}

/* Write the pending content to app->current_file, choosing HTML vs. plain
 * text from the extension. HTML files are wrapped in a minimal document. */
static void save_to_current(ScribeApp *app) {
    if (!app->current_file[0]) return;
    FILE *f = fopen(app->current_file, "w");
    if (!f) {
        run_js(app, "setStatus('SAVE FAILED');");
        return;
    }
    if (has_ext(app->current_file, ".html") || has_ext(app->current_file, ".htm")) {
        fputs("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
              "<style>body{font-family:'Times New Roman',serif;line-height:1.8;"
              "max-width:800px;margin:2em auto;}</style></head><body>\n", f);
        if (app->pending_html) fputs(app->pending_html, f);
        fputs("\n</body></html>\n", f);
    } else {
        if (app->pending_text) fputs(app->pending_text, f);
    }
    fclose(f);
    g_print("Saved to %s\n", app->current_file);
    run_js(app, "setDirty(false);setStatus('SAVED');");
}

/* ------------------------------------------------------------------ *
 *  Save flow
 * ------------------------------------------------------------------ */
static void on_save_as_response(GtkDialog *dialog, gint response, ScribeApp *app) {
    if (response == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            strncpy(app->current_file, filename, MAX_PATH - 1);
            app->current_file[MAX_PATH - 1] = '\0';
            g_free(filename);
            save_to_current(app);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_save_as_dialog(ScribeApp *app) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save Document As", app->window, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
    gtk_file_chooser_set_current_name(chooser, "untitled.txt");

    GtkFileFilter *ft = gtk_file_filter_new();
    gtk_file_filter_set_name(ft, "Plain Text (*.txt)");
    gtk_file_filter_add_pattern(ft, "*.txt");
    gtk_file_chooser_add_filter(chooser, ft);

    GtkFileFilter *fh = gtk_file_filter_new();
    gtk_file_filter_set_name(fh, "HTML Document (*.html)");
    gtk_file_filter_add_pattern(fh, "*.html");
    gtk_file_filter_add_pattern(fh, "*.htm");
    gtk_file_chooser_add_filter(chooser, fh);

    g_signal_connect(dialog, "response", G_CALLBACK(on_save_as_response), app);
    gtk_widget_show(dialog);
}

/* Receives {html, text} object from JS when the user saves. */
static void on_save_message(WebKitUserContentManager *manager,
                            WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (jsc_value_is_object(value)) {
        JSCValue *h = jsc_value_object_get_property(value, "html");
        JSCValue *t = jsc_value_object_get_property(value, "text");
        char *html = jsc_value_to_string(h);
        char *text = jsc_value_to_string(t);

        g_free(app->pending_html);
        g_free(app->pending_text);
        app->pending_html = g_strdup(html ? html : "");
        app->pending_text = g_strdup(text ? text : "");

        g_free(html);
        g_free(text);
        g_object_unref(h);
        g_object_unref(t);

        if (app->current_file[0])
            save_to_current(app);
        else
            open_save_as_dialog(app);
    }
}

/* ------------------------------------------------------------------ *
 *  Open flow
 * ------------------------------------------------------------------ */
static void on_file_open_response(GtkDialog *dialog, gint response, ScribeApp *app) {
    if (response == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            FILE *f = fopen(filename, "r");
            if (f) {
                strncpy(app->current_file, filename, MAX_PATH - 1);
                app->current_file[MAX_PATH - 1] = '\0';
                size_t n = fread(app->content, 1, MAX_CONTENT - 1, f);
                app->content[n] = '\0';
                fclose(f);
                load_text_into_editor(app, app->content);
            } else {
                run_js(app, "setStatus('OPEN FAILED');");
            }
            g_free(filename);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void on_open_message(WebKitUserContentManager *manager,
                            WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager; (void)js_result;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open Document", app->window, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Text / HTML Files");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_filter_add_pattern(filter, "*.html");
    gtk_file_filter_add_pattern(filter, "*.htm");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    g_signal_connect(dialog, "response", G_CALLBACK(on_file_open_response), app);
    gtk_widget_show(dialog);
}

/* ------------------------------------------------------------------ *
 *  Dirty tracking + close guard
 * ------------------------------------------------------------------ */
static void on_dirty_message(WebKitUserContentManager *manager,
                             WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (jsc_value_is_string(value)) {
        char *s = jsc_value_to_string(value);
        app->is_dirty = (s && s[0] == '1');
        g_free(s);
    }
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)widget; (void)event;
    ScribeApp *app = (ScribeApp *)data;
    if (app->is_dirty) {
        GtkWidget *d = gtk_message_dialog_new(
            app->window, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_YES_NO,
            "You have unsaved changes. Quit without saving?");
        gint r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_YES)
            return TRUE;  /* veto the close */
    }
    return FALSE;
}

/* Inject argv content once the initial page has finished loading. */
static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event,
                            ScribeApp *app) {
    (void)web_view;
    if (event == WEBKIT_LOAD_FINISHED && !app->initial_loaded) {
        app->initial_loaded = TRUE;
        if (app->current_file[0])
            load_text_into_editor(app, app->content);
    }
}

/* ------------------------------------------------------------------ *
 *  UI setup
 * ------------------------------------------------------------------ */
static void setup_ui(ScribeApp *app) {
    app->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(app->window, "Scribe - Word Processor");
    gtk_window_set_default_size(app->window, 960, 720);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);

    app->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(app->web_view), TRUE, TRUE, 0);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_web_view_set_settings(app->web_view, settings);

    WebKitUserContentManager *manager =
        webkit_web_view_get_user_content_manager(app->web_view);
    webkit_user_content_manager_register_script_message_handler(manager, "save");
    webkit_user_content_manager_register_script_message_handler(manager, "openFile");
    webkit_user_content_manager_register_script_message_handler(manager, "dirty");
    g_signal_connect(manager, "script-message-received::save",
                     G_CALLBACK(on_save_message), app);
    g_signal_connect(manager, "script-message-received::openFile",
                     G_CALLBACK(on_open_message), app);
    g_signal_connect(manager, "script-message-received::dirty",
                     G_CALLBACK(on_dirty_message), app);

    g_signal_connect(app->web_view, "load-changed",
                     G_CALLBACK(on_load_changed), app);

    webkit_web_view_load_html(app->web_view, HTML_TEMPLATE, NULL);

    gtk_widget_show_all(GTK_WIDGET(app->window));
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    ScribeApp app = {0};
    setup_ui(&app);

    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (f) {
            strncpy(app.current_file, argv[1], MAX_PATH - 1);
            app.current_file[MAX_PATH - 1] = '\0';
            size_t n = fread(app.content, 1, MAX_CONTENT - 1, f);
            app.content[n] = '\0';
            fclose(f);
            /* actual injection happens in on_load_changed once ready */
        } else {
            g_printerr("Scribe: could not open '%s'\n", argv[1]);
        }
    }

    gtk_main();

    g_free(app.pending_html);
    g_free(app.pending_text);
    return 0;
}
