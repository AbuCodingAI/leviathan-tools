#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH 1024

/* ------------------------------------------------------------------ *
 *  Scribe — LeviathanOS word processor
 *
 *  A contenteditable rich-text editor hosted in WebKit, driven from a
 *  small C backend. File text crosses the JS<->C bridge base64-encoded
 *  (UTF-8 safe via TextDecoder) so no byte can break the page or inject.
 * ------------------------------------------------------------------ */

typedef struct {
    GtkWindow *window;
    WebKitWebView *web_view;
    char current_file[MAX_PATH];
    char startup_file[MAX_PATH]; /* argv doc, injected once the page is ready */
    gchar *pending_html;         /* last editor HTML from JS (for save)        */
    gchar *pending_text;         /* last editor plain text from JS (for save)  */
    gboolean is_dirty;
    gboolean is_html_file;       /* current file keeps formatting (.html)      */
    gboolean initial_loaded;
    gchar *recovery_path;        /* autosave sidecar for crash recovery        */
} ScribeApp;

/* ------------------------------------------------------------------ *
 *  Front-end (HTML + JS).  Buttons carry data-* attributes and are
 *  wired by one delegated listener — keeps the C string literal free
 *  of nested-quote escaping.
 * ------------------------------------------------------------------ */
static const char *HTML_TEMPLATE =
"<!DOCTYPE html><html><head><meta charset='UTF-8'><style>"
"*{margin:0;padding:0;box-sizing:border-box;}"
"html,body{height:100%;}"
"body{font-family:monospace;background:#0a0f0a;color:#33ff33;display:flex;flex-direction:column;overflow:hidden;}"
"#toolbar{background:#0a140f;border-bottom:2px solid #33ff33;display:flex;flex-direction:column;gap:4px;padding:6px 8px;}"
".row{display:flex;gap:4px;flex-wrap:wrap;align-items:center;}"
".btn{padding:5px 9px;background:#030705;color:#33ff33;border:1px solid #1c5c2c;cursor:pointer;font-size:11px;font-weight:bold;font-family:monospace;transition:all .12s;user-select:none;}"
".btn:hover{background:#33ff33;color:#030705;border-color:#33ff33;}"
".btn:active{transform:scale(.96);}"
".btn.on{background:#33ff33;color:#030705;}"
".fmt{padding:5px 8px;min-width:26px;text-align:center;}"
"select.btn{padding:4px 4px;}"
"input[type=color]{width:26px;height:26px;padding:0;border:1px solid #1c5c2c;background:#030705;cursor:pointer;}"
".sep{width:1px;height:22px;background:#1c5c2c;margin:0 3px;}"
".lbl{font-size:10px;color:#5fbf6f;margin-right:2px;}"
"#page-area{flex:1;overflow:auto;padding:26px 10px;display:flex;justify-content:center;background:#0a0f0a;}"
"#paper{transition:transform .1s;}"
"#editor{width:816px;min-height:1056px;padding:72px 84px;background:#fff;color:#111;"
"font-family:'Times New Roman',serif;font-size:16px;line-height:1.6;"
"box-shadow:0 0 22px rgba(51,255,51,.22);border:1px solid #33ff33;}"
"#editor:focus{outline:none;}"
"#editor.mla{padding:96px;line-height:2;}"
"#editor h1{font-size:28px;margin:.4em 0;}#editor h2{font-size:22px;margin:.4em 0;}"
"#editor h3{font-size:18px;margin:.4em 0;}"
"#editor ul,#editor ol{margin-left:1.4em;}"
"#editor blockquote{border-left:3px solid #999;margin:.5em 0;padding-left:1em;color:#444;}"
"#editor table{border-collapse:collapse;margin:.5em 0;}"
"#editor td,#editor th{border:1px solid #888;padding:5px 9px;min-width:40px;}"
"#editor img{max-width:100%;}"
"#editor a{color:#0645ad;}"
"#find{display:none;gap:5px;align-items:center;background:#061006;padding:6px 8px;border-bottom:1px solid #1c5c2c;}"
"#find.on{display:flex;flex-wrap:wrap;}"
"#find input[type=text]{background:#030705;color:#33ff33;border:1px solid #1c5c2c;padding:5px 7px;font-family:monospace;font-size:11px;width:150px;}"
"#find label{font-size:10px;color:#5fbf6f;display:flex;align-items:center;gap:3px;}"
"#status{background:#0a140f;border-top:2px solid #33ff33;padding:5px 12px;display:flex;"
"justify-content:space-between;gap:10px;font-size:11px;color:#33ff33;font-family:monospace;}"
"#status .r{display:flex;gap:14px;}"
"#aboutov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;align-items:center;justify-content:center;}"
"#aboutov.on{display:flex;}"
"#aboutbox{background:#0a140f;border:2px solid #33ff33;color:#33ff33;max-width:480px;margin:16px;padding:20px;font-family:monospace;font-size:12px;line-height:1.7;box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#aboutbox .atitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#aboutbox .aok{margin-top:16px;text-align:right;}"
"</style></head><body>"
"<div id='toolbar'>"
"<div class='row'>"
"<button class='btn' data-act='open' title='Open (Ctrl+O)'>OPEN</button>"
"<button class='btn' data-act='save' title='Save (Ctrl+S)'>SAVE</button>"
"<button class='btn' data-act='saveas' title='Save As (Ctrl+Shift+S)'>SAVE AS</button>"
"<button class='btn' data-act='new' title='New (Ctrl+N)'>NEW</button>"
"<span class='sep'></span>"
"<button class='btn' data-act='pdf' title='Export to PDF'>PDF</button>"
"<button class='btn' data-act='print' title='Print (Ctrl+P)'>PRINT</button>"
"<span class='sep'></span>"
"<button class='btn' data-act='undo' title='Undo (Ctrl+Z)'>&#8630;</button>"
"<button class='btn' data-act='redo' title='Redo (Ctrl+Y)'>&#8631;</button>"
"<span class='sep'></span>"
"<button class='btn' data-act='find' title='Find &amp; Replace (Ctrl+F)'>FIND</button>"
"</div>"
"<div class='row'>"
"<select class='btn' id='fontsel' title='Font'>"
"<option value=''>FONT</option>"
"<option value=\"Times New Roman\">Times</option>"
"<option value='Georgia'>Georgia</option>"
"<option value='Arial'>Arial</option>"
"<option value='Helvetica'>Helvetica</option>"
"<option value='Verdana'>Verdana</option>"
"<option value='Courier New'>Courier</option>"
"</select>"
"<select class='btn' id='sizesel' title='Text size'>"
"<option value=''>SIZE</option>"
"<option value='1'>8</option><option value='2'>10</option><option value='3'>12</option>"
"<option value='4'>14</option><option value='5'>18</option><option value='6'>24</option>"
"<option value='7'>36</option>"
"</select>"
"<button class='btn fmt' data-cmd='bold' title='Bold (Ctrl+B)'><b>B</b></button>"
"<button class='btn fmt' data-cmd='italic' title='Italic (Ctrl+I)'><i>I</i></button>"
"<button class='btn fmt' data-cmd='underline' title='Underline (Ctrl+U)'><u>U</u></button>"
"<button class='btn fmt' data-cmd='strikeThrough' title='Strikethrough'><s>S</s></button>"
"<span class='lbl'>A</span><input type='color' id='fore' value='#111111' title='Text color'>"
"<span class='lbl'>HL</span><input type='color' id='hilite' value='#ffff00' title='Highlight'>"
"<span class='sep'></span>"
"<button class='btn fmt' data-align='justifyLeft' title='Align left'>&#8676;</button>"
"<button class='btn fmt' data-align='justifyCenter' title='Center'>&#8596;</button>"
"<button class='btn fmt' data-align='justifyRight' title='Align right'>&#8677;</button>"
"<button class='btn fmt' data-align='justifyFull' title='Justify'>&#8801;</button>"
"</div>"
"<div class='row'>"
"<select class='btn' id='stylesel' title='Paragraph style'>"
"<option value=''>STYLE</option>"
"<option value='h1'>Heading 1</option><option value='h2'>Heading 2</option>"
"<option value='h3'>Heading 3</option><option value='p'>Paragraph</option>"
"<option value='blockquote'>Quote</option><option value='pre'>Code</option>"
"</select>"
"<button class='btn fmt' data-cmd='insertUnorderedList' title='Bullet list'>&bull;</button>"
"<button class='btn fmt' data-cmd='insertOrderedList' title='Numbered list'>1.</button>"
"<button class='btn fmt' data-cmd='outdent' title='Decrease indent'>&#8592;|</button>"
"<button class='btn fmt' data-cmd='indent' title='Increase indent'>|&#8594;</button>"
"<span class='sep'></span>"
"<button class='btn' data-act='link' title='Insert link'>LINK</button>"
"<button class='btn' data-act='image' title='Insert image'>IMG</button>"
"<button class='btn' data-act='table' title='Insert table'>TABLE</button>"
"<button class='btn' data-act='hr' title='Horizontal line'>HR</button>"
"<button class='btn' data-act='clear' title='Clear formatting'>CLEAR</button>"
"<span class='sep'></span>"
"<span class='lbl'>SPACING</span>"
"<select class='btn' id='linesel' title='Line spacing'>"
"<option value='1.15'>1.0</option><option value='1.6' selected>1.5</option>"
"<option value='2'>2.0</option><option value='2.5'>2.5</option>"
"</select>"
"<span class='lbl'>PAGE</span>"
"<select class='btn' id='pagesel' title='Page size'>"
"<option value='letter'>Letter</option><option value='a4'>A4</option>"
"</select>"
"<button class='btn fmt' data-act='zoomout' title='Zoom out'>&minus;</button>"
"<button class='btn fmt' id='zlbl' data-act='zoomreset' title='Reset zoom'>100%</button>"
"<button class='btn fmt' data-act='zoomin' title='Zoom in'>+</button>"
"<button class='btn' data-act='mla' title='MLA formatting'>MLA</button>"
"<button class='btn' data-act='about' title='About Scribe'>?</button>"
"</div>"
"</div>"
"<div id='find'>"
"<span class='lbl'>FIND</span><input type='text' id='fq' placeholder='find...'>"
"<span class='lbl'>REPLACE</span><input type='text' id='rq' placeholder='replace...'>"
"<label><input type='checkbox' id='mcase'>Aa</label>"
"<button class='btn' data-act='findnext'>NEXT</button>"
"<button class='btn' data-act='replace'>REPLACE</button>"
"<button class='btn' data-act='replaceall'>ALL</button>"
"<button class='btn' data-act='findclose'>&times;</button>"
"</div>"
"<div id='page-area'><div id='paper'><div id='editor' contenteditable='true' spellcheck='true'></div></div></div>"
"<div id='status'><span id='msg'>&gt; READY</span>"
"<span class='r'><span id='rt'>0 min</span><span id='count'>0 words | 0 chars</span></span></div>"
"<input type='file' id='imgpick' accept='image/*' style='display:none'>"
"<script>"
"function ed(){return document.getElementById('editor');}"
"function $(i){return document.getElementById(i);}"
"var _dirty=false,_zoom=1;"
"try{document.execCommand('styleWithCSS',false,true);}catch(e){}"
"function setDirty(v){if(v!==_dirty){_dirty=v;window.webkit.messageHandlers.dirty.postMessage(v?'1':'0');}}"
"function setStatus(m){var s=$('msg');s.innerText='> '+m;clearTimeout(window._st);"
"window._st=setTimeout(function(){s.innerText='> READY';},2200);}"
"function updateCount(){var t=ed().innerText;var tt=t.replace(/\\s+/g,' ').trim();"
"var w=tt?tt.split(' ').length:0;"
"$('count').innerText=w+' words | '+t.length+' chars';"
"$('rt').innerText=Math.max(1,Math.ceil(w/200))+' min read';}"
"function touch(){setDirty(true);updateCount();}"
"function cmd(c){document.execCommand(c,false,null);ed().focus();touch();}"
"function withArg(c,a){document.execCommand(c,false,a);ed().focus();touch();}"
"function block(t){if(t)withArg('formatBlock',t);}"
"function insertHTML(h){document.execCommand('insertHTML',false,h);ed().focus();touch();}"
/* ---- named actions dispatched from data-act ---- */
"var ACT={"
"open:function(){window.webkit.messageHandlers.openFile.postMessage('');},"
"save:function(){doSave(false);},"
"saveas:function(){doSave(true);},"
"new:function(){if(_dirty&&!confirm('Discard unsaved changes and start a new document?'))return;"
"ed().innerHTML='';setDirty(false);updateCount();setStatus('NEW DOCUMENT');ed().focus();},"
"pdf:function(){window.webkit.messageHandlers.pdf.postMessage('');},"
"print:function(){window.webkit.messageHandlers.print.postMessage('');},"
"undo:function(){cmd('undo');},redo:function(){cmd('redo');},"
"find:function(){var f=$('find');f.classList.toggle('on');if(f.classList.contains('on')){$('fq').focus();}else{ed().focus();}},"
"findclose:function(){$('find').classList.remove('on');ed().focus();},"
"findnext:function(){var q=$('fq').value;if(!q)return;"
"var ok=window.find(q,$('mcase').checked,false,true,false,false,false);"
"setStatus(ok?'FOUND':'NOT FOUND');},"
"replace:function(){var q=$('fq').value,r=$('rq').value;if(!q)return;"
"var s=window.getSelection();"
"if(s.rangeCount&&s.toString()&&(($('mcase').checked&&s.toString()===q)||(!$('mcase').checked&&s.toString().toLowerCase()===q.toLowerCase()))){"
"document.execCommand('insertText',false,r);touch();}"
"window.find(q,$('mcase').checked,false,true,false,false,false);},"
"replaceall:function(){var q=$('fq').value,r=$('rq').value;if(!q)return;"
"var n=replaceAllText(q,r,$('mcase').checked);touch();setStatus('REPLACED '+n+' MATCH(ES)');},"
"link:function(){var u=prompt('Link URL:','https://');if(u)withArg('createLink',u);},"
"image:function(){$('imgpick').click();},"
"table:function(){var r=parseInt(prompt('Rows:','2'),10),c=parseInt(prompt('Columns:','2'),10);"
"if(!r||!c||r<1||c<1||r>100||c>50)return;var h='<table>';for(var i=0;i<r;i++){h+='<tr>';"
"for(var j=0;j<c;j++)h+='<td>&nbsp;</td>';h+='</tr>';}h+='</table><p><br></p>';insertHTML(h);},"
"hr:function(){cmd('insertHorizontalRule');},"
"clear:function(){cmd('removeFormat');block('p');},"
"zoomin:function(){setZoom(_zoom+0.1);},zoomout:function(){setZoom(_zoom-0.1);},"
"zoomreset:function(){setZoom(1);},"
"mla:function(){ed().classList.toggle('mla');var on=ed().classList.contains('mla');"
"if(on){$('linesel').value='2';ed().style.lineHeight='2';}setStatus(on?'MLA MODE ON':'MLA MODE OFF');},"
"about:function(){$('aboutov').classList.add('on');}"
"};"
"function setZoom(z){_zoom=Math.min(2.5,Math.max(0.5,z));"
"$('paper').style.transform='scale('+_zoom+')';$('paper').style.transformOrigin='top center';"
"$('zlbl').innerText=Math.round(_zoom*100)+'%';}"
/* safe replace: walk text nodes only, never touch markup */
"function replaceAllText(find,repl,mcase){if(!find)return 0;var count=0;"
"var re=new RegExp(find.replace(/[.*+?^${}()|[\\]\\\\]/g,'\\\\$&'),mcase?'g':'gi');"
"var w=document.createTreeWalker(ed(),NodeFilter.SHOW_TEXT,null,false);var ns=[];"
"while(w.nextNode())ns.push(w.currentNode);"
"ns.forEach(function(n){var m=n.nodeValue.match(re);if(m){count+=m.length;"
"n.nodeValue=n.nodeValue.replace(re,repl);}});return count;}"
"function doSave(forceAs){window.webkit.messageHandlers.save.postMessage("
"{html:ed().innerHTML,text:ed().innerText,as:forceAs?'1':'0'});}"
/* file text injected from C (base64, UTF-8 safe) */
"function b64bytes(b){var bin=atob(b);var a=new Uint8Array(bin.length);"
"for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);return a;}"
"function loadB64(b){try{ed().innerText=new TextDecoder('utf-8').decode(b64bytes(b));"
"setDirty(false);updateCount();setStatus('LOADED');}catch(e){setStatus('LOAD ERROR');}}"
"function loadHtmlB64(b){try{var html=new TextDecoder('utf-8').decode(b64bytes(b));"
"var m=html.match(/<body[^>]*>([\\s\\S]*?)<\\/body>/i);ed().innerHTML=m?m[1]:html;"
"setDirty(false);updateCount();setStatus('LOADED');}catch(e){setStatus('LOAD ERROR');}}"
/* toolbar wiring via delegation */
"document.querySelectorAll('[data-cmd]').forEach(function(b){b.onclick=function(){cmd(b.dataset.cmd);};});"
"document.querySelectorAll('[data-align]').forEach(function(b){b.onclick=function(){cmd(b.dataset.align);};});"
"document.querySelectorAll('[data-act]').forEach(function(b){b.onclick=function(){ACT[b.dataset.act]();};});"
"$('fontsel').onchange=function(){if(this.value)withArg('fontName',this.value);this.selectedIndex=0;};"
"$('sizesel').onchange=function(){if(this.value)withArg('fontSize',this.value);this.selectedIndex=0;};"
"$('stylesel').onchange=function(){block(this.value);this.selectedIndex=0;};"
"$('linesel').onchange=function(){ed().style.lineHeight=this.value;touch();};"
"$('pagesel').onchange=function(){if(this.value==='a4'){ed().style.width='794px';ed().style.minHeight='1123px';}"
"else{ed().style.width='816px';ed().style.minHeight='1056px';}};"
"$('fore').oninput=function(){withArg('foreColor',this.value);};"
"$('hilite').oninput=function(){withArg('hiliteColor',this.value)||withArg('backColor',this.value);};"
"$('imgpick').onchange=function(){var f=this.files[0];if(!f)return;var rd=new FileReader();"
"rd.onload=function(){withArg('insertImage',rd.result);};rd.readAsDataURL(f);this.value='';};"
"ed().addEventListener('input',touch);"
"document.addEventListener('keydown',function(e){if(!e.ctrlKey)return;var k=e.key.toLowerCase();"
"if(k==='s'){e.preventDefault();doSave(e.shiftKey);}"
"else if(k==='o'){e.preventDefault();ACT.open();}"
"else if(k==='n'){e.preventDefault();ACT['new']();}"
"else if(k==='p'){e.preventDefault();ACT.print();}"
"else if(k==='f'){e.preventDefault();ACT.find();}"
"else if(k==='b'){e.preventDefault();cmd('bold');}"
"else if(k==='i'){e.preventDefault();cmd('italic');}"
"else if(k==='u'){e.preventDefault();cmd('underline');}"
"else if(k==='y'){e.preventDefault();cmd('redo');}});"
"$('fq').addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();ACT.findnext();}});"
/* autosave crash-recovery every 15s while dirty */
"setInterval(function(){if(_dirty)window.webkit.messageHandlers.autosave.postMessage("
"{html:ed().innerHTML,text:ed().innerText});},15000);"
"updateCount();ed().focus();"
"</script>"
"<div id='aboutov'>"
"<div id='aboutbox'>"
"<div class='atitle'>Scribe &mdash; part of LeviathanOS</div>"
"A lightweight word processor. Free software under the GNU General Public "
"License, version 3.<br>This program comes with ABSOLUTELY NO WARRANTY.<br>"
"Full license: /usr/share/doc/leviathanos/LICENSE<br>"
"https://www.gnu.org/licenses/gpl-3.0.html"
"<div class='aok'><button class='btn' onclick=\"document.getElementById('aboutov').classList.remove('on')\">OK</button></div>"
"</div></div>"
"</body></html>";

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */
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

static void run_js(ScribeApp *app, const char *js) {
    webkit_web_view_evaluate_javascript(app->web_view, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
}

static gboolean has_ext(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n >= e && g_ascii_strcasecmp(path + n - e, ext) == 0;
}

static void update_title(ScribeApp *app) {
    const char *base = app->current_file[0]
        ? (strrchr(app->current_file, '/') ? strrchr(app->current_file, '/') + 1
                                           : app->current_file)
        : "Untitled";
    char *title = g_strdup_printf("%s%s - Scribe",
                                  app->is_dirty ? "*" : "", base);
    gtk_window_set_title(app->window, title);
    g_free(title);
}

/* Inject a file's bytes into the editor (HTML preserves formatting; else plain). */
static void inject_file(ScribeApp *app, const gchar *data, gsize len) {
    char *b64 = b64_encode((const unsigned char *)data, len);
    if (!b64) return;
    char *js = g_strdup_printf(app->is_html_file ? "loadHtmlB64('%s');"
                                                 : "loadB64('%s');", b64);
    run_js(app, js);
    g_free(js);
    free(b64);
}

static void load_file(ScribeApp *app, const char *path) {
    gchar *data = NULL; gsize len = 0; GError *err = NULL;
    if (!g_file_get_contents(path, &data, &len, &err)) {
        run_js(app, "setStatus('OPEN FAILED');");
        if (err) g_error_free(err);
        return;
    }
    strncpy(app->current_file, path, MAX_PATH - 1);
    app->current_file[MAX_PATH - 1] = '\0';
    app->is_html_file = has_ext(path, ".html") || has_ext(path, ".htm");
    inject_file(app, data, len);
    g_free(data);
    app->is_dirty = FALSE;
    update_title(app);
}

/* ------------------------------------------------------------------ *
 *  Save
 * ------------------------------------------------------------------ */
static void save_to_current(ScribeApp *app) {
    if (!app->current_file[0]) return;
    FILE *f = fopen(app->current_file, "w");
    if (!f) { run_js(app, "setStatus('SAVE FAILED');"); return; }

    if (has_ext(app->current_file, ".html") || has_ext(app->current_file, ".htm")) {
        fputs("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
              "<style>body{font-family:'Times New Roman',serif;line-height:1.6;"
              "max-width:816px;margin:2em auto;padding:0 1em;}"
              "table{border-collapse:collapse;}td,th{border:1px solid #888;padding:5px 9px;}"
              "blockquote{border-left:3px solid #999;margin:.5em 0;padding-left:1em;color:#444;}"
              "img{max-width:100%;}</style></head><body>\n", f);
        if (app->pending_html) fputs(app->pending_html, f);
        fputs("\n</body></html>\n", f);
    } else {
        if (app->pending_text) fputs(app->pending_text, f);
    }
    fclose(f);

    /* successful save clears the crash-recovery sidecar */
    if (app->recovery_path) g_unlink(app->recovery_path);

    app->is_dirty = FALSE;
    update_title(app);
    run_js(app, "setDirty(false);setStatus('SAVED');");
}

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
    gtk_file_chooser_set_current_name(chooser,
        app->current_file[0] ? (strrchr(app->current_file,'/')?strrchr(app->current_file,'/')+1:app->current_file)
                             : "untitled.html");

    GtkFileFilter *fh = gtk_file_filter_new();
    gtk_file_filter_set_name(fh, "Rich Document (*.html) - keeps formatting");
    gtk_file_filter_add_pattern(fh, "*.html");
    gtk_file_filter_add_pattern(fh, "*.htm");
    gtk_file_chooser_add_filter(chooser, fh);

    GtkFileFilter *ft = gtk_file_filter_new();
    gtk_file_filter_set_name(ft, "Plain Text (*.txt)");
    gtk_file_filter_add_pattern(ft, "*.txt");
    gtk_file_chooser_add_filter(chooser, ft);

    g_signal_connect(dialog, "response", G_CALLBACK(on_save_as_response), app);
    gtk_widget_show(dialog);
}

static void on_save_message(WebKitUserContentManager *manager,
                            WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (!jsc_value_is_object(value)) return;

    JSCValue *h = jsc_value_object_get_property(value, "html");
    JSCValue *t = jsc_value_object_get_property(value, "text");
    JSCValue *a = jsc_value_object_get_property(value, "as");
    char *html = jsc_value_to_string(h);
    char *text = jsc_value_to_string(t);
    char *as   = jsc_value_to_string(a);
    gboolean force_as = as && as[0] == '1';

    g_free(app->pending_html); g_free(app->pending_text);
    app->pending_html = g_strdup(html ? html : "");
    app->pending_text = g_strdup(text ? text : "");

    g_free(html); g_free(text); g_free(as);
    g_object_unref(h); g_object_unref(t); g_object_unref(a);

    if (app->current_file[0] && !force_as) save_to_current(app);
    else open_save_as_dialog(app);
}

/* ------------------------------------------------------------------ *
 *  Autosave (crash recovery)
 * ------------------------------------------------------------------ */
static void on_autosave_message(WebKitUserContentManager *manager,
                                WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (!jsc_value_is_object(value) || !app->recovery_path) return;
    JSCValue *h = jsc_value_object_get_property(value, "html");
    char *html = jsc_value_to_string(h);
    if (html) {
        FILE *f = fopen(app->recovery_path, "w");
        if (f) {
            fputs("<!DOCTYPE html><meta charset='UTF-8'><body>\n", f);
            fputs(html, f);
            fclose(f);
        }
        g_free(html);
    }
    g_object_unref(h);
}

/* ------------------------------------------------------------------ *
 *  Open
 * ------------------------------------------------------------------ */
static void on_file_open_response(GtkDialog *dialog, gint response, ScribeApp *app) {
    if (response == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) { load_file(app, filename); g_free(filename); }
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
    gtk_file_filter_set_name(filter, "Documents (*.txt, *.html)");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_filter_add_pattern(filter, "*.html");
    gtk_file_filter_add_pattern(filter, "*.htm");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    g_signal_connect(dialog, "response", G_CALLBACK(on_file_open_response), app);
    gtk_widget_show(dialog);
}

/* ------------------------------------------------------------------ *
 *  Print / PDF export
 * ------------------------------------------------------------------ */
static void on_print_finished(WebKitPrintOperation *op, gpointer data) {
    (void)data;
    g_object_unref(op);
}

static void on_print_message(WebKitUserContentManager *manager,
                             WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager; (void)js_result;
    WebKitPrintOperation *op = webkit_print_operation_new(app->web_view);
    webkit_print_operation_run_dialog(op, app->window);
    g_object_unref(op);
}

static void on_pdf_message(WebKitUserContentManager *manager,
                           WebKitJavascriptResult *js_result, ScribeApp *app) {
    (void)manager; (void)js_result;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Export to PDF", app->window, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Export", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "document.pdf");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (fn) {
            WebKitPrintOperation *op = webkit_print_operation_new(app->web_view);
            GtkPrintSettings *st = gtk_print_settings_new();
            char *uri = g_strdup_printf("file://%s", fn);
            gtk_print_settings_set(st, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
            gtk_print_settings_set(st, "output-file-format", "pdf");
            webkit_print_operation_set_print_settings(op, st);
            g_signal_connect(op, "finished", G_CALLBACK(on_print_finished), NULL);
            webkit_print_operation_print(op);
            g_free(uri); g_object_unref(st); g_free(fn);
            run_js(app, "setStatus('EXPORTED PDF');");
        }
    }
    gtk_widget_destroy(dialog);
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
        update_title(app);
    }
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)widget; (void)event;
    ScribeApp *app = (ScribeApp *)data;
    if (app->is_dirty) {
        GtkWidget *d = gtk_message_dialog_new(
            app->window, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_YES_NO, "You have unsaved changes. Quit without saving?");
        gint r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_YES) return TRUE;  /* veto the close */
    }
    if (app->recovery_path) g_unlink(app->recovery_path);
    return FALSE;
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event,
                            ScribeApp *app) {
    (void)web_view;
    if (event == WEBKIT_LOAD_FINISHED && !app->initial_loaded) {
        app->initial_loaded = TRUE;
        if (app->startup_file[0]) load_file(app, app->startup_file);
    }
}

/* ------------------------------------------------------------------ *
 *  UI setup
 * ------------------------------------------------------------------ */
static void reg(WebKitUserContentManager *m, const char *name,
                GCallback cb, ScribeApp *app) {
    webkit_user_content_manager_register_script_message_handler(m, name);
    char *sig = g_strdup_printf("script-message-received::%s", name);
    g_signal_connect(m, sig, cb, app);
    g_free(sig);
}

static void setup_ui(ScribeApp *app) {
    app->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(app->window, "Scribe - Word Processor");
    gtk_window_set_default_size(app->window, 1120, 800);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);

    app->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(app->web_view), TRUE, TRUE, 0);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_web_view_set_settings(app->web_view, settings);

    /* built-in spell checking */
    WebKitWebContext *ctx = webkit_web_view_get_context(app->web_view);
    webkit_web_context_set_spell_checking_enabled(ctx, TRUE);
    const char *langs[] = { "en_US", NULL };
    webkit_web_context_set_spell_checking_languages(ctx, langs);

    WebKitUserContentManager *m =
        webkit_web_view_get_user_content_manager(app->web_view);
    reg(m, "save",     G_CALLBACK(on_save_message),     app);
    reg(m, "openFile", G_CALLBACK(on_open_message),     app);
    reg(m, "dirty",    G_CALLBACK(on_dirty_message),    app);
    reg(m, "print",    G_CALLBACK(on_print_message),    app);
    reg(m, "pdf",      G_CALLBACK(on_pdf_message),      app);
    reg(m, "autosave", G_CALLBACK(on_autosave_message), app);

    g_signal_connect(app->web_view, "load-changed",
                     G_CALLBACK(on_load_changed), app);

    webkit_web_view_load_html(app->web_view, HTML_TEMPLATE, NULL);
    gtk_widget_show_all(GTK_WIDGET(app->window));
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    ScribeApp app = {0};
    app.recovery_path = g_build_filename(g_get_user_data_dir(),
                                         "scribe-recovery.html", NULL);
    setup_ui(&app);

    if (argc > 1) {
        strncpy(app.startup_file, argv[1], MAX_PATH - 1);
        app.startup_file[MAX_PATH - 1] = '\0';
        /* injection happens in on_load_changed once the page is ready */
    }

    gtk_main();

    g_free(app.pending_html);
    g_free(app.pending_text);
    g_free(app.recovery_path);
    return 0;
}
