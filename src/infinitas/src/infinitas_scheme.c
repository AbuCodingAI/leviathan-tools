/**
 * Infinitas Browser - Custom Scheme Handler Implementation
 */

#include "infinitas_scheme.h"
#include "browser.h"
#include "renderer.h"
#include "settings.h"
#include "bookmarks.h"
#include "history.h"
#include "offstore.h"
#include "extension.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── shared page shell ─────────────────────────────────────── */

static const gchar PAGE_CSS[] =
    "*{margin:0;padding:0;box-sizing:border-box}"
    ":root{--bg:#0f0f13;--surf:#1a1a24;--bdr:#2a2a3a;--acc:#7c6af7;"
    "--grn:#4ade80;--red:#f87171;--txt:#e2e8f0;--mut:#64748b}"
    "body{background:var(--bg);color:var(--txt);font-family:system-ui,sans-serif;"
    "min-height:100vh}"
    ".hdr{background:var(--surf);border-bottom:1px solid var(--bdr);"
    "padding:16px 28px;display:flex;align-items:center;justify-content:space-between}"
    ".hdr h1{font-size:20px;font-weight:700}"
    ".hdr p{font-size:13px;color:var(--mut);margin-top:3px}"
    ".content{padding:28px;max-width:900px;margin:0 auto}"
    ".card{background:var(--surf);border:1px solid var(--bdr);border-radius:12px;"
    "padding:16px 20px;margin-bottom:12px;display:flex;align-items:center;gap:14px}"
    ".card-title{font-size:15px;font-weight:500;white-space:nowrap;overflow:hidden;"
    "text-overflow:ellipsis}"
    ".card-url{font-size:12px;color:var(--mut);white-space:nowrap;overflow:hidden;"
    "text-overflow:ellipsis}"
    ".card-url a{color:var(--mut);text-decoration:none}"
    ".card-url a:hover{color:var(--txt)}"
    ".card-meta{font-size:11px;color:var(--mut);white-space:nowrap}"
    ".flex1{flex:1;min-width:0}"
    ".btn{border:none;border-radius:8px;padding:7px 16px;font-size:13px;"
    "cursor:pointer;font-weight:500}"
    ".btn-del{background:#2a1a1a;color:var(--red)}"
    ".btn-del:hover{background:#3a1a1a}"
    ".btn-acc{background:var(--acc);color:#fff}"
    ".btn-acc:hover{filter:brightness(1.15)}"
    ".btn-muted{background:var(--bdr);color:var(--txt)}"
    ".btn-muted:hover{background:#3a3a4a}"
    ".empty{text-align:center;padding:60px 20px;color:var(--mut)}"
    ".empty h2{font-size:18px;margin-bottom:8px}"
    "::-webkit-scrollbar{width:5px}"
    "::-webkit-scrollbar-thumb{background:var(--bdr);border-radius:3px}";

/* ── async ping ──────────────────────────────────────────────────────────── */

typedef struct {
    WebKitURISchemeRequest *request;
    gchar *target;
    gchar *output;
    gboolean success;
} PingCtx;

static gboolean deliver_ping(gpointer user_data) {
    PingCtx *ctx = user_data;

    const gchar *color = ctx->success ? "#4ade80" : "#f87171";
    const gchar *label = ctx->success ? "Reachable" : "Unreachable";

    gchar *escaped = g_markup_escape_text(ctx->output ? ctx->output : "", -1);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Ping %s</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{background:#0f0f13;color:#e2e8f0;font-family:system-ui,sans-serif;"
        "display:flex;align-items:center;justify-content:center;"
        "min-height:100vh;padding:32px}"
        ".card{background:#1a1a24;border:1px solid #2a2a3a;border-radius:14px;"
        "padding:32px;max-width:640px;width:100%%}"
        ".host{font-size:28px;font-weight:700;letter-spacing:-0.5px;margin-bottom:6px}"
        ".status{font-size:14px;font-weight:600;margin-bottom:24px}"
        "pre{background:#0f0f13;border:1px solid #2a2a3a;border-radius:8px;"
        "padding:18px;font-size:13px;line-height:1.6;color:#94a3b8;"
        "overflow-x:auto;white-space:pre-wrap;word-break:break-all}"
        "a{color:#7c6af7;text-decoration:none;font-size:13px;margin-top:18px;"
        "display:inline-block}"
        "</style></head><body>"
        "<div class='card'>"
        "<div class='host'>%s</div>"
        "<div class='status' style='color:%s'>%s</div>"
        "<pre>%s</pre>"
        "<a href='javascript:history.back()'>\xe2\x86\x90 Back</a>"
        "</div></body></html>",
        ctx->target, ctx->target, color, label, escaped);
    g_free(escaped);

    GInputStream *s = g_memory_input_stream_new_from_data(html, -1, g_free);
    webkit_uri_scheme_request_finish(ctx->request, s, -1, "text/html");
    g_object_unref(s);
    g_object_unref(ctx->request);
    g_free(ctx->target);
    g_free(ctx->output);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static gpointer ping_thread(gpointer user_data) {
    PingCtx *ctx = user_data;

    gchar *argv[] = {
        "ping", "-c", "4", "-W", "10", ctx->target, NULL
    };
    gchar *out = NULL, *err = NULL;
    gint status = 0;

    gboolean ok = g_spawn_sync(NULL, argv, NULL,
                                G_SPAWN_SEARCH_PATH,
                                NULL, NULL,
                                &out, &err, &status, NULL);

    if (!ok) {
        ctx->output  = g_strdup("Error: 'ping' command not found on this system.");
        ctx->success = FALSE;
    } else {
        GString *combined = g_string_new(out ? out : "");
        if (err && *err) g_string_append(combined, err);
        ctx->output  = g_string_free(combined, FALSE);
        ctx->success = (status == 0);
    }
    g_free(out);
    g_free(err);

    g_idle_add(deliver_ping, ctx);
    return NULL;
}

static void handle_ping(WebKitURISchemeRequest *request, const gchar *target) {
    PingCtx *ctx = g_new0(PingCtx, 1);
    ctx->request = g_object_ref(request);
    ctx->target  = g_strdup(target);
    /* Keep the request pending — deliver_ping will call finish() from the main
     * thread via g_idle_add once the ping completes (≤10 s). */
    g_thread_new("infinitas-ping", ping_thread, ctx);
}

/* ── extensions page ─────────────────────────────────────────────────────── */

static gchar* gen_extensions_page(InfinitasBrowser *browser) {
    if (!browser || !browser->extensions) {
        return g_strdup(
            "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
            "<body style='background:#0f0f13;color:#e2e8f0;font-family:system-ui;"
            "padding:32px'>No extension manager available.</body></html>");
    }

    GString *body = g_string_new("");
    ExtensionManager *em = browser->extensions;

    if (em->extensions->len == 0) {
        g_string_append(body,
            "<div class='empty'><h2>No extensions installed</h2>"
            "<p>Drop an unpacked Chrome extension folder into<br>"
            "<code style='color:#7c6af7'>~/.infinitas/extensions/</code>"
            " and restart Infinitas.</p></div>");
    } else {
        for (guint i = 0; i < em->extensions->len; i++) {
            InfinitasExtension *e = g_ptr_array_index(em->extensions, i);
            gchar *name = e->name ? g_markup_escape_text(e->name, -1) : g_strdup("Unknown");
            gchar *desc = e->description ? g_markup_escape_text(e->description, -1) : g_strdup("");
            gchar *ver  = e->version ? g_markup_escape_text(e->version, -1) : g_strdup("?");

            const gchar *mv_label = e->manifest_version >= 3 ? "MV3" :
                                    e->manifest_version == 2 ? "MV2" : "Native";

            gchar *icon_html = g_strdup("");
            if (e->icon_path && g_file_test(e->icon_path, G_FILE_TEST_EXISTS)) {
                gchar *icon_url = g_strdup_printf("infinitas://ext/%s/", e->id);
                /* find relative icon path */
                const gchar *rel = e->icon_path + strlen(e->path);
                if (*rel == '/') rel++;
                gchar *full_icon = g_strdup_printf("infinitas://ext/%s/%s", e->id, rel);
                g_free(icon_url);
                g_free(icon_html);
                icon_html = g_strdup_printf(
                    "<img src='%s' width='32' height='32' "
                    "style='border-radius:6px;flex-shrink:0' onerror=\"this.style.display='none'\">",
                    full_icon);
                g_free(full_icon);
            }

            g_string_append_printf(body,
                "<div class='card'>"
                "<div style='display:flex;gap:14px;align-items:flex-start'>"
                "%s"
                "<div style='flex:1;min-width:0'>"
                "<div class='card-title'>%s"
                "  <span style='font-size:11px;background:#1a1a3a;color:#7c6af7;"
                "    padding:2px 7px;border-radius:4px;margin-left:6px;"
                "    font-weight:600;vertical-align:middle'>%s</span>"
                "  <span style='font-size:11px;color:#64748b;margin-left:4px'>v%s</span>"
                "</div>"
                "<div style='font-size:13px;color:#64748b;margin-top:4px;line-height:1.5'>%s</div>"
                "<div style='margin-top:10px;font-size:12px;color:#475569'>"
                "  <strong style='color:#94a3b8'>ID:</strong> %s &nbsp; "
                "  <strong style='color:#94a3b8'>Scripts:</strong> %u JS, %u CSS &nbsp;"
                "  %s"
                "</div></div></div></div>",
                icon_html, name, mv_label, ver, desc, e->id,
                e->content_scripts ? e->content_scripts->len : 0,
                e->content_css     ? e->content_css->len     : 0,
                e->has_action ? "<span style='color:#4ade80'>&#10003; toolbar button</span>" : "");

            g_free(name); g_free(desc); g_free(ver); g_free(icon_html);
        }
    }

    gchar *body_str = g_string_free(body, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Extensions \xe2\x80\x94 Infinitas</title>"
        "<style>%s"
        ".card{display:block;padding:20px 22px;margin-bottom:14px;"
        "background:var(--surf);border:1px solid var(--bdr);border-radius:14px}"
        "code{background:#0f0f13;padding:2px 6px;border-radius:4px;font-size:13px}"
        "</style></head><body>"
        "<div class='hdr'>"
        "<div><h1>Extensions</h1>"
        "<p>Chrome MV2/MV3 and native extensions &nbsp;&bull;&nbsp; "
        "Drop folders in <code>~/.infinitas/extensions/</code></p></div>"
        "</div>"
        "<div class='content'>%s</div>"
        "</body></html>",
        PAGE_CSS, body_str);
    g_free(body_str);
    return html;
}

/* ── plugin path registry ────────────────────────────────────────────────── */

typedef struct {
    gchar  *prefix;
    gchar* (*handler)(const gchar *subpath, gpointer userdata);
    gpointer userdata;
} PluginPathEntry;

static GPtrArray *g_plugin_paths = NULL;

void infinitas_register_plugin_path(const gchar *prefix,
                                     gchar* (*handler)(const gchar*, gpointer),
                                     gpointer userdata) {
    if (!g_plugin_paths)
        g_plugin_paths = g_ptr_array_new();
    PluginPathEntry *e = g_new(PluginPathEntry, 1);
    e->prefix  = g_strdup(prefix);
    e->handler = handler;
    e->userdata = userdata;
    g_ptr_array_add(g_plugin_paths, e);
    g_print("[SCHEME] Plugin registered path: infinitas://%s\n", prefix);
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void finish_with_html(WebKitURISchemeRequest *req, gchar *html) {
    GInputStream *s = g_memory_input_stream_new_from_data(html, -1, g_free);
    webkit_uri_scheme_request_finish(req, s, -1, "text/html");
    g_object_unref(s);
}

static void redirect_to(WebKitURISchemeRequest *req, const gchar *dest) {
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv='refresh' content='0;url=%s'>"
        "</head><body></body></html>", dest);
    finish_with_html(req, html);
}

/* ── new tab page ────────────────────────────────────────────────────────── */

/* ── Infinitas Inner ──────────────────────────────────────────────────────
 * Small, subtle, once-a-year celebratory easter eggs for the new-tab page.
 * NOT Google Doodles: a tiny element tucked in a corner that only appears on
 * the exact matching day. Pure self-contained HTML/CSS/JS (emoji + inline SVG
 * + CSS animation) — no network, no external assets. This whole blob is passed
 * to g_strdup_printf as a plain "%s" argument, so its literal '%' (CSS/JS) are
 * NOT format specifiers and need no doubling.
 * Uses the same dark palette vars as the page: --txt --mut --acc --surf --bdr. */
static const gchar II_EGG[] =
"<style>"
"#ii-egg,.ii-egg{position:fixed;z-index:6;user-select:none;line-height:1}"
".ii-corner{right:16px;bottom:14px;font-size:20px;opacity:.5;cursor:pointer;"
"transition:transform .25s,opacity .25s}"
".ii-corner:hover{opacity:1;transform:scale(1.2)}"
".ii-sway{animation:ii-sway 3.5s ease-in-out infinite}"
".ii-twk{animation:ii-twk 2.5s ease-in-out infinite}"
".ii-bob{animation:ii-bob 2.8s ease-in-out infinite}"
".ii-flk{animation:ii-flk 2s steps(2) infinite}"
".ii-whisper{position:fixed;right:16px;bottom:44px;font-size:11px;color:var(--mut);"
"opacity:0;animation:ii-whisper 6s ease-in-out infinite;pointer-events:none;z-index:6}"
".ii-hunt{opacity:.32;cursor:pointer;transition:opacity .2s}"
".ii-hunt:hover{opacity:.7}"
".ii-spin-on{animation:ii-spin 1.1s cubic-bezier(.2,.7,.2,1)}"
".ii-crack-on{animation:ii-crack .5s forwards}"
".ii-twk-on{animation:ii-twk .6s 2}"
".ii-note{position:fixed;left:50%;bottom:74px;transform:translateX(-50%);max-width:430px;"
"background:var(--surf);border:1px solid var(--bdr);border-radius:12px;padding:16px 20px;"
"font-size:13px;line-height:1.65;color:var(--txt);box-shadow:0 10px 34px rgba(0,0,0,.55);"
"display:none;z-index:20;cursor:pointer}"
".ii-note .sig{display:block;margin-top:10px;color:var(--acc);font-size:12px}"
".ii-cf{position:fixed;top:-12px;width:8px;height:8px;border-radius:1px;opacity:.9;z-index:19;"
"pointer-events:none;animation:ii-fall linear forwards}"
"@keyframes ii-sway{0%,100%{transform:rotate(-7deg)}50%{transform:rotate(7deg)}}"
"@keyframes ii-twk{0%,100%{opacity:.5}50%{opacity:1;filter:drop-shadow(0 0 5px #ffd76a)}}"
"@keyframes ii-bob{0%,100%{transform:translateY(0) rotate(-3deg)}50%{transform:translateY(-5px) rotate(3deg)}}"
"@keyframes ii-flk{0%,100%{opacity:.85}45%{opacity:.3}55%{opacity:.7}75%{opacity:.4}}"
"@keyframes ii-whisper{0%,100%{opacity:0}30%,70%{opacity:.8}}"
"@keyframes ii-spin{from{transform:rotate(0)}to{transform:rotate(1800deg)}}"
"@keyframes ii-crack{0%{transform:scale(1) rotate(0)}40%{transform:scale(1.25) rotate(-12deg)}"
"100%{transform:scale(0) rotate(22deg);opacity:0}}"
"@keyframes ii-fall{to{transform:translateY(112vh) rotate(560deg);opacity:.2}}"
"</style>"
"<div id='ii-egg'></div><div class='ii-note' id='ii-note'></div>"
"<script>(function(){"
"var E=document.getElementById('ii-egg');if(!E)return;"
"var NOTE=document.getElementById('ii-note');"
"var d=new Date(),m=d.getMonth()+1,day=d.getDate(),Y=d.getFullYear();"
"function pad(n){return(n<10?'0':'')+n;}"
"var T=pad(m)+'-'+pad(day);"
/* nth weekday of a month: dow 0=Sun; used for Thanksgiving/Columbus Day. */
"function nthDow(y,mo,dow,n){var f=new Date(y,mo-1,1).getDay();return 1+((dow-f+7)%7)+(n-1)*7;}"
/* Genuine Easter Sunday via the Anonymous Gregorian computus algorithm. */
"function easter(y){var a=y%19,b=Math.floor(y/100),c=y%100,dd=Math.floor(b/4),ee=b%4,"
"ff=Math.floor((b+8)/25),gg=Math.floor((b-ff+1)/3),hh=(19*a+b-dd-gg+15)%30,"
"ii=Math.floor(c/4),kk=c%4,ll=(32+2*ee+2*ii-hh-kk)%7,mm=Math.floor((a+11*hh+22*ll)/451),"
"mo=Math.floor((hh+ll-7*mm+114)/31),da=((hh+ll-7*mm+114)%31)+1;return pad(mo)+'-'+pad(da);}"
/* Approximate Gregorian dates for lunar / Hebrew-calendar holidays, 2025-2030.
   These are published approximate moon-sighting / Hebrew-calendar dates and
   should be refined later with a real astronomical / Hebrew-calendar routine. */
"var RAMADAN={2025:'03-01',2026:'02-18',2027:'02-08',2028:'01-28',2029:'01-16',2030:'01-05'};"
"var EIDFITR={2025:'03-31',2026:'03-20',2027:'03-10',2028:'02-26',2029:'02-14',2030:'02-04'};"
"var EIDADHA={2025:'06-07',2026:'05-27',2027:'05-17',2028:'05-05',2029:'04-24',2030:'04-13'};"
"var HANUKKAH={2025:'12-14',2026:'12-04',2027:'12-24',2028:'12-12',2029:'12-01',2030:'12-20'};"
"var egg=null,BD=null;"
"try{BD=localStorage.getItem('infinitas_birthday');}catch(_){}"
/* TODO(google-birthday-sync): once Google OAuth / People API is wired up,
   populate localStorage['infinitas_birthday'] (format MM-DD) from the signed-in
   Google account birthday, the same way Gmail surfaces it. Full OAuth is out of
   scope for now, so setting the key manually is the current "basics" path. */
"if(BD&&BD===T)egg='bday';"
"else if(T==='07-04')egg='july4';"
"else if(T==='12-25')egg='xmas';"
"else if(T==='01-01')egg='nye';"
"else if(T==='10-31')egg='hween';"
"else if(m===11&&day===nthDow(Y,11,4,4))egg='thanks';"
"else if(m===10&&day===nthDow(Y,10,1,2))egg='columbus';"
"else if(T===easter(Y))egg='easter';"
"else if(RAMADAN[Y]===T||EIDFITR[Y]===T||EIDADHA[Y]===T)egg='crescent';"
"else if(HANUKKAH[Y]===T)egg='dreidel';"
"if(!egg)return;"
"function confetti(n){var cs=['#7c6af7','#4ade80','#f87171','#ffd76a','#38bdf8'];"
"for(var i=0;i<n;i++){var p=document.createElement('div');p.className='ii-cf';"
"p.style.left=(Math.random()*100)+'vw';p.style.background=cs[i%5];"
"p.style.animationDuration=(2+Math.random()*2)+'s';p.style.animationDelay=(Math.random()*1.4)+'s';"
"document.body.appendChild(p);(function(x){setTimeout(function(){x.remove();},4600);})(p);}}"
/* July 4 — a tiny subtle fireworks emoji; click adds a couple of pops. */
"if(egg==='july4'){E.className='ii-corner ii-twk';E.textContent='\xF0\x9F\x8E\x86';"
"E.onclick=function(){confetti(26);};}"
/* Christmas — a twinkling ornament + a present, gentle sway. */
"else if(egg==='xmas'){E.className='ii-corner ii-sway';"
"E.innerHTML='<span class=\"ii-twk\">\xF0\x9F\x94\xB4</span>\xF0\x9F\x8E\x81';"
"E.onclick=function(){confetti(14);};}"
/* New Year — small fireworks + a whispered greeting. */
"else if(egg==='nye'){E.className='ii-corner ii-twk';E.textContent='\xF0\x9F\x8E\x86';"
"var w=document.createElement('div');w.className='ii-whisper';w.textContent='happy new year';"
"document.body.appendChild(w);E.onclick=function(){confetti(30);};}"
/* Halloween — a flickering pumpkin; click toggles a ghost. */
"else if(egg==='hween'){E.className='ii-corner ii-flk';E.textContent='\xF0\x9F\x8E\x83';"
"E.onclick=function(){E.textContent=E.textContent==='\xF0\x9F\x8E\x83'?'\xF0\x9F\x91\xBB':'\xF0\x9F\x8E\x83';};}"
/* Thanksgiving — an autumn leaf; click reveals a turkey. */
"else if(egg==='thanks'){E.className='ii-corner ii-sway';E.textContent='\xF0\x9F\x8D\x82';"
"E.onclick=function(){E.textContent=E.textContent==='\xF0\x9F\x8D\x82'?'\xF0\x9F\xA6\x83':'\xF0\x9F\x8D\x82';};}"
/* Columbus Day — a little ship that bobs. */
"else if(egg==='columbus'){E.className='ii-corner ii-bob';E.textContent='\xE2\x9B\xB5';}"
/* Ramadan + both Eids — a crescent moon to spot; click twinkles. */
"else if(egg==='crescent'){E.className='ii-corner';E.textContent='\xF0\x9F\x8C\x99';E.style.opacity='.4';"
"E.onclick=function(){E.classList.remove('ii-twk-on');void E.offsetWidth;E.classList.add('ii-twk-on');"
"var s=document.createElement('div');s.className='ii-egg';s.textContent='\xE2\x9C\xA8';s.style.right='42px';"
"s.style.bottom='34px';s.style.fontSize='12px';document.body.appendChild(s);"
"setTimeout(function(){s.remove();},900);};}"
/* Hanukkah — a dreidel (inline SVG) that spins on click. Nothing else. */
"else if(egg==='dreidel'){E.className='ii-corner';E.style.transformOrigin='50% 42%';"
"E.innerHTML='<svg width=\"22\" height=\"26\" viewBox=\"0 0 22 26\">"
"<rect x=\"9.5\" y=\"0\" width=\"3\" height=\"5\" fill=\"#caa15a\"/>"
"<path d=\"M11 5 L20 12 L11 21 L2 12 Z\" fill=\"#7c6af7\" stroke=\"#b9adff\" stroke-width=\"1\"/>"
"<path d=\"M11 21 L13 25 L9 25 Z\" fill=\"#caa15a\"/>"
"<text x=\"11\" y=\"15\" font-size=\"8\" text-anchor=\"middle\" fill=\"#fff\">\xD7\xA0</text></svg>';"
"E.onclick=function(){E.classList.remove('ii-spin-on');void E.offsetWidth;E.classList.add('ii-spin-on');};}"
/* Easter — the literal easter egg hunt: 3 tiny eggs tucked around the page;
   find and click each to crack it. Finding all three pops confetti. */
"else if(egg==='easter'){"
"var spots=[['13vw','32vh'],['83vw','60vh'],['47vw','85vh']];"
"var cols=['#f7a6c4','#a6d3f7','#c8f7a6'];var found=0;"
"spots.forEach(function(p,idx){var g=document.createElement('div');g.className='ii-egg ii-hunt';"
"g.style.left=p[0];g.style.top=p[1];"
"g.innerHTML='<svg width=\"16\" height=\"22\" viewBox=\"0 0 16 22\">"
"<ellipse cx=\"8\" cy=\"12\" rx=\"7\" ry=\"10\" fill=\"'+cols[idx]+'\" stroke=\"#0003\"/>"
"<path d=\"M1 12 h14\" stroke=\"#ffffff99\" stroke-width=\"1.4\"/></svg>';"
"g.onclick=function(){g.classList.add('ii-crack-on');found++;"
"setTimeout(function(){g.remove();},520);if(found===spots.length){confetti(20);}};"
"document.body.appendChild(g);});}"
/* Birthday — a small cake; click for confetti + the Creator's note (verbatim). */
"else if(egg==='bday'){E.className='ii-corner';E.textContent='\xF0\x9F\x8E\x82';"
"NOTE.innerHTML=\"It isn't like other days, something feels off, it feels special, "
"it feels ecstatic, it's your birthday, it's your special day and nothing should take from that, "
"may you celebrate this birthday and many more to come<span class='sig'>\xE2\x80\x94 The Leviathan Creator</span>\";"
"NOTE.onclick=function(){NOTE.style.display='none';};"
"E.onclick=function(){confetti(40);NOTE.style.display='block';};}"
"})();</script>";

static gchar* gen_tab_page(InfinitasBrowser *browser) {
    GPtrArray *recent = browser ? history_get_all(browser->history, 8) : NULL;

    GString *hist_html = g_string_new("");
    if (recent && recent->len > 0) {
        g_string_append(hist_html,
            "<div style='margin-top:32px'>"
            "<h2 style='font-size:14px;color:var(--mut);letter-spacing:.5px;"
            "text-transform:uppercase;margin-bottom:12px'>Recent</h2>"
            "<div style='display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px'>");
        for (guint i = 0; i < recent->len; i++) {
            HistoryEntry *e = g_ptr_array_index(recent, i);
            gchar *t = e->title ? g_markup_escape_text(e->title, -1) : g_strdup("Untitled");
            gchar *u = e->url  ? g_markup_escape_text(e->url,   -1) : g_strdup("");
            g_string_append_printf(hist_html,
                "<a href='%s' style='text-decoration:none'>"
                "<div style='background:var(--surf);border:1px solid var(--bdr);"
                "border-radius:10px;padding:12px 14px;cursor:pointer;transition:border-color .15s'>"
                "<div style='font-size:13px;font-weight:500;white-space:nowrap;"
                "overflow:hidden;text-overflow:ellipsis;color:var(--txt)'>%s</div>"
                "<div style='font-size:11px;color:var(--mut);margin-top:4px;"
                "white-space:nowrap;overflow:hidden;text-overflow:ellipsis'>%s</div>"
                "</div></a>",
                u, t, u);
            g_free(t); g_free(u);
        }
        g_string_append(hist_html, "</div></div>");
    } else {
        g_string_append(hist_html,
            "<p style='color:var(--mut);margin-top:32px;font-size:14px'>"
            "No recent pages yet.</p>");
    }
    if (recent) g_ptr_array_free(recent, TRUE);

    gchar *hist_str = g_string_free(hist_html, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>New Tab</title><style>"
        "%s"
        "body{display:flex;align-items:center;justify-content:center;"
        "flex-direction:column;padding:40px 24px}"
        ".logo{font-size:42px;font-weight:800;letter-spacing:-2px;color:var(--txt);"
        "margin-bottom:8px}"
        ".tagline{font-size:14px;color:var(--mut);margin-bottom:36px}"
        ".search-wrap{width:100%%;max-width:560px;position:relative}"
        ".search-wrap input{width:100%%;padding:14px 20px;font-size:16px;"
        "background:var(--surf);border:1px solid var(--bdr);border-radius:24px;"
        "color:var(--txt);outline:none;transition:border-color .15s}"
        ".search-wrap input:focus{border-color:var(--acc)}"
        ".quick{display:flex;gap:12px;margin-top:28px;flex-wrap:wrap;justify-content:center}"
        ".quick a{display:flex;flex-direction:column;align-items:center;gap:6px;"
        "padding:14px 18px;background:var(--surf);border:1px solid var(--bdr);"
        "border-radius:12px;text-decoration:none;color:var(--mut);font-size:12px;"
        "min-width:80px;transition:color .15s,border-color .15s}"
        ".quick a:hover{color:var(--txt);border-color:var(--acc)}"
        ".quick .icon{font-size:22px}"
        ".recent-wrap{width:100%%;max-width:840px;margin-top:8px}"
        "</style></head><body>"
        "<div class='logo'>Infinitas</div>"
        "<div class='tagline'>Your browser. Your rules.</div>"
        "<div class='search-wrap'>"
        "<input type='text' placeholder='\xf0\x9f\x94\x8d Search or enter address\xe2\x80\xa6'"
        " onkeydown='if(event.key===\"Enter\"){"
        "var v=this.value.trim();"
        "location.href=(v.includes(\".\")||v.includes(\"://\"))?"
        "(v.startsWith(\"http\")?v:\"https://\"+v):"
        "\"https://www.google.com/search?q=\"+encodeURIComponent(v);}'>"
        "</div>"
        "<div class='quick'>"
        "<a href='infinitas://bookmarks'><span class='icon'>\xe2\xad\x90</span>Bookmarks</a>"
        "<a href='infinitas://history'><span class='icon'>\xf0\x9f\x95\x90</span>History</a>"
        "<a href='infinitas://offline'><span class='icon'>\xf0\x9f\x93\xa6</span>Offline</a>"
        "<a href='infinitas://fonts'><span class='icon'>\xf0\x9f\x94\xa4</span>Fonts</a>"
        "<a href='infinitas://settings'><span class='icon'>\xe2\x9a\x99</span>Settings</a>"
        "<a href='infinitas://help'><span class='icon'>\xe2\x9d\x93</span>Help</a>"
        "<a href='infinitas://about'><span class='icon'>\xe2\x84\xb9</span>About</a>"
        "</div>"
        "<div class='recent-wrap'>%s</div>"
        "%s"
        "</body></html>",
        PAGE_CSS, hist_str, II_EGG);
    g_free(hist_str);
    return html;
}

/* ── bookmarks page ──────────────────────────────────────────────────────── */

static gchar* gen_bookmarks_page(InfinitasBrowser *browser) {
    GString *body = g_string_new("");
    if (browser) {
        GPtrArray *bms = bookmarks_get_all(browser->bookmarks);
        if (!bms || bms->len == 0) {
            g_string_append(body,
                "<div class='empty'><h2>No bookmarks yet</h2>"
                "<p>Press Ctrl+D on any page to save it here.</p></div>");
        } else {
            for (guint i = 0; i < bms->len; i++) {
                Bookmark *b = g_ptr_array_index(bms, i);
                gchar *t = b->title ? g_markup_escape_text(b->title, -1) : g_strdup("Untitled");
                gchar *u = b->url   ? g_markup_escape_text(b->url,   -1) : g_strdup("");
                g_string_append_printf(body,
                    "<div class='card'>"
                    "<div class='flex1'>"
                    "<div class='card-title'>%s</div>"
                    "<div class='card-url'><a href='%s'>%s</a></div>"
                    "</div>"
                    "<span class='card-meta'>%s</span>"
                    "<a href='%s'><button class='btn btn-muted'>Open</button></a>"
                    "<a href='infinitas://delete-bookmark/%d'>"
                    "<button class='btn btn-del'>Remove</button></a>"
                    "</div>",
                    t, u, u,
                    b->created ? b->created : "",
                    u, b->id);
                g_free(t); g_free(u);
            }
        }
        if (bms) g_ptr_array_free(bms, TRUE);
    }

    gchar *body_str = g_string_free(body, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Bookmarks \xe2\x80\x94 Infinitas</title>"
        "<style>%s</style></head><body>"
        "<div class='hdr'><div><h1>Bookmarks</h1>"
        "<p>Ctrl+D on any page to add</p></div></div>"
        "<div class='content'>%s</div>"
        "</body></html>",
        PAGE_CSS, body_str);
    g_free(body_str);
    return html;
}

/* ── history page ────────────────────────────────────────────────────────── */

/* Sum resident memory (KB) of the browser + its WebKit web/network processes. */
static long get_webkit_memory_kb(void) {
    long total = 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[300], buf[256] = {0};
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *cf = fopen(path, "r");
        if (!cf) continue;
        if (!fgets(buf, sizeof(buf), cf)) buf[0] = '\0';
        fclose(cf);
        if (!strstr(buf, "WebKit") && !strstr(buf, "infinitas")) continue;
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        FILE *sf = fopen(path, "r");
        if (!sf) continue;
        char line[256];
        while (fgets(line, sizeof(line), sf)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                long kb = 0; sscanf(line + 6, "%ld", &kb); total += kb; break;
            }
        }
        fclose(sf);
    }
    closedir(d);
    return total;
}

/* "What's Running" — task-manager view of open tabs + memory. */
static gchar* gen_running_page(InfinitasBrowser *browser) {
    GString *body = g_string_new("");
    gint active = browser ? browser_get_active_tab(browser) : -1;
    if (browser && browser->tabs && browser->tabs->len > 0) {
        for (guint i = 0; i < browser->tabs->len; i++) {
            WebRenderer *r = g_ptr_array_index(browser->tabs, i);
            if (!r) continue;
            gchar *t = g_markup_escape_text(r->page_title ? r->page_title : "New Tab", -1);
            const gchar *url = (r->hibernated && r->hibernated_url) ? r->hibernated_url
                             : (r->current_url ? r->current_url : "");
            gchar *u = g_markup_escape_text(url, -1);
            gboolean audio = r->view && webkit_web_view_is_playing_audio(r->view);
            const char *state = r->hibernated ? "\xF0\x9F\x92\xA4 Sleeping"
                              : ((gint)i == active ? "Active" : "Running");
            g_string_append_printf(body,
                "<div class='card'>"
                "<div class='flex1'><div class='card-title'>%s%s</div>"
                "<div class='card-url'>%s</div></div>"
                "<span class='card-meta'>%s</span>"
                "<a href='%s'><button class='btn btn-muted'>Open</button></a>"
                "</div>",
                audio ? "\xF0\x9F\x94\x8A " : "", t, u, state, u);
            g_free(t); g_free(u);
        }
    } else {
        g_string_append(body, "<div class='empty'><h2>No tabs open</h2></div>");
    }
    long mem_kb = get_webkit_memory_kb();
    gchar *body_str = g_string_free(body, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>What's Running \xe2\x80\x94 Infinitas</title>"
        "<meta http-equiv='refresh' content='3'>"
        "<style>%s</style></head><body>"
        "<div class='hdr'><div><h1>What's Running</h1>"
        "<p>%u tab%s &middot; %ld MB memory &middot; \xF0\x9F\x92\xA4 idle tabs auto-sleep</p></div></div>"
        "<div class='content'>%s</div></body></html>",
        PAGE_CSS,
        browser && browser->tabs ? browser->tabs->len : 0,
        (browser && browser->tabs && browser->tabs->len == 1) ? "" : "s",
        mem_kb / 1024, body_str);
    g_free(body_str);
    return html;
}

static gchar* gen_history_page(InfinitasBrowser *browser) {
    GString *body = g_string_new("");
    if (browser) {
        GPtrArray *hist = history_get_all(browser->history, 500);
        if (!hist || hist->len == 0) {
            g_string_append(body,
                "<div class='empty'><h2>No history yet</h2>"
                "<p>Pages you visit will appear here.</p></div>");
        } else {
            for (guint i = 0; i < hist->len; i++) {
                HistoryEntry *e = g_ptr_array_index(hist, i);
                gchar *t = e->title ? g_markup_escape_text(e->title, -1) : g_strdup("Untitled");
                gchar *u = e->url   ? g_markup_escape_text(e->url,   -1) : g_strdup("");
                g_string_append_printf(body,
                    "<div class='card'>"
                    "<div class='flex1'>"
                    "<div class='card-title'>%s</div>"
                    "<div class='card-url'><a href='%s'>%s</a></div>"
                    "</div>"
                    "<span class='card-meta'>%s &middot; %d visit%s</span>"
                    "<a href='%s'><button class='btn btn-muted'>Open</button></a>"
                    "<a href='infinitas://delete-history/%d'>"
                    "<button class='btn btn-del'>Remove</button></a>"
                    "</div>",
                    t, u, u,
                    e->timestamp ? e->timestamp : "",
                    e->visit_count, e->visit_count == 1 ? "" : "s",
                    u, e->id);
                g_free(t); g_free(u);
            }
        }
        if (hist) g_ptr_array_free(hist, TRUE);
    }

    gchar *body_str = g_string_free(body, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>History \xe2\x80\x94 Infinitas</title>"
        "<style>%s</style></head><body>"
        "<div class='hdr'><div><h1>History</h1>"
        "<p>Your recent browsing activity</p></div>"
        "<a href='infinitas://clear-history'>"
        "<button class='btn btn-del'>Clear All</button></a></div>"
        "<div class='content'>%s</div>"
        "</body></html>",
        PAGE_CSS, body_str);
    g_free(body_str);
    return html;
}

/* ── help page ───────────────────────────────────────────────────────────── */

static gchar* gen_help_page(void) {
    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Help &mdash; Infinitas</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        ":root{--bg:#0f0f13;--surf:#1a1a24;--bdr:#2a2a3a;--acc:#7c6af7;"
        "--txt:#e2e8f0;--mut:#64748b;--grn:#4ade80}"
        "body{background:var(--bg);color:var(--txt);font-family:system-ui,sans-serif;"
        "min-height:100vh}"
        ".hdr{background:var(--surf);border-bottom:1px solid var(--bdr);"
        "padding:20px 32px;display:flex;align-items:center;gap:16px}"
        ".hdr h1{font-size:22px;font-weight:700}"
        ".hdr p{font-size:13px;color:var(--mut);margin-top:3px}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));"
        "gap:16px;padding:32px;max-width:1100px;margin:0 auto}"
        ".card{background:var(--surf);border:1px solid var(--bdr);border-radius:14px;"
        "padding:20px 22px;transition:border-color .15s}"
        ".card:hover{border-color:var(--acc)}"
        ".card a{text-decoration:none;color:inherit}"
        ".card-url{font-family:monospace;font-size:14px;color:var(--acc);"
        "font-weight:600;margin-bottom:8px}"
        ".card-title{font-size:16px;font-weight:600;margin-bottom:6px}"
        ".card-desc{font-size:13px;color:var(--mut);line-height:1.6}"
        ".tag{display:inline-block;font-size:10px;padding:2px 7px;"
        "border-radius:4px;margin-bottom:10px;font-weight:600;letter-spacing:.4px}"
        ".tag-page{background:#1e1a3a;color:var(--acc)}"
        ".tag-action{background:#1a2e1a;color:var(--grn)}"
        ".tag-redirect{background:#2a1a1a;color:#f87171}"
        ".shortcuts{margin:0 32px 32px;max-width:1100px}"
        ".shortcuts h2{font-size:16px;font-weight:600;margin-bottom:14px;color:var(--mut);"
        "text-transform:uppercase;letter-spacing:.6px}"
        ".kb-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}"
        ".kb{background:var(--surf);border:1px solid var(--bdr);border-radius:10px;"
        "padding:12px 16px;display:flex;justify-content:space-between;align-items:center}"
        ".kb-key{font-family:monospace;background:#0f0f13;border:1px solid var(--bdr);"
        "border-radius:5px;padding:3px 8px;font-size:12px;color:var(--acc)}"
        ".kb-desc{font-size:13px;color:var(--mut)}"
        "::-webkit-scrollbar{width:5px}"
        "::-webkit-scrollbar-thumb{background:var(--bdr);border-radius:3px}"
        "</style></head><body>"
        "<div class='hdr'>"
        "<div>"
        "<h1>Infinitas Pages</h1>"
        "<p>All built-in <code style='color:var(--acc)'>infinitas://</code> addresses</p>"
        "</div>"
        "</div>"

        "<div class='grid'>"

        /* newtab */
        "<div class='card'><a href='infinitas://newtab'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://newtab</div>"
        "<div class='card-title'>New Tab</div>"
        "<div class='card-desc'>Start page with search bar, quick links, and recent history.</div>"
        "</a></div>"

        /* bookmarks */
        "<div class='card'><a href='infinitas://bookmarks'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://bookmarks</div>"
        "<div class='card-title'>Bookmarks</div>"
        "<div class='card-desc'>View and manage all your bookmarks. Press Ctrl+D on any page to add one.</div>"
        "</a></div>"

        /* history */
        "<div class='card'><a href='infinitas://history'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://history</div>"
        "<div class='card-title'>History</div>"
        "<div class='card-desc'>Full browsing history with timestamps, visit counts, and per-item removal.</div>"
        "</a></div>"

        /* offline */
        "<div class='card'><a href='infinitas://offline'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://offline</div>"
        "<div class='card-title'>Offline Archive</div>"
        "<div class='card-desc'>Starred pages saved as encrypted .infx binaries. Served automatically when offline.</div>"
        "</a></div>"

        /* fonts */
        "<div class='card'><a href='infinitas://fonts'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://fonts</div>"
        "<div class='card-title'>Font Manager</div>"
        "<div class='card-desc'>Browse all system fonts with a live preview. Apply one globally to all pages.</div>"
        "</a></div>"

        /* settings */
        "<div class='card'><a href='infinitas://settings'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://settings</div>"
        "<div class='card-title'>Settings</div>"
        "<div class='card-desc'>Browser preferences: home page, search engine, privacy options.</div>"
        "</a></div>"

        /* help */
        "<div class='card'><a href='infinitas://help'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://help</div>"
        "<div class='card-title'>Help</div>"
        "<div class='card-desc'>This page. All built-in infinitas:// addresses and keyboard shortcuts.</div>"
        "</a></div>"

        /* about */
        "<div class='card'><a href='infinitas://about'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://about</div>"
        "<div class='card-title'>About / License</div>"
        "<div class='card-desc'>Program info and the GNU GPLv3 license notice.</div>"
        "</a></div>"

        /* mail */
        "<div class='card'><a href='infinitas://mail'>"
        "<div class='tag tag-redirect'>REDIRECT</div>"
        "<div class='card-url'>infinitas://mail</div>"
        "<div class='card-title'>Mail</div>"
        "<div class='card-desc'>Redirects to Gmail (mail.google.com).</div>"
        "</a></div>"

        /* incognito */
        "<div class='card'><a href='infinitas://incognito'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://incognito</div>"
        "<div class='card-title'>Incognito</div>"
        "<div class='card-desc'>Information about private browsing mode.</div>"
        "</a></div>"

        /* skribe */
        "<div class='card'><a href='infinitas://skribe'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://skribe</div>"
        "<div class='card-title'>Skribe</div>"
        "<div class='card-desc'>Built-in PDF reader. Drop a PDF file to open it.</div>"
        "</a></div>"

        /* apps */
        "<div class='card'><a href='infinitas://apps'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://apps</div>"
        "<div class='card-title'>Apps</div>"
        "<div class='card-desc'>Installed web apps and PWAs.</div>"
        "</a></div>"

        /* pwa */
        "<div class='card'><a href='infinitas://pwa'>"
        "<div class='tag tag-page'>PAGE</div>"
        "<div class='card-url'>infinitas://pwa</div>"
        "<div class='card-title'>PWA Manager</div>"
        "<div class='card-desc'>Manage Progressive Web Apps installed in Infinitas.</div>"
        "</a></div>"

        /* clear-history */
        "<div class='card'>"
        "<div class='tag tag-action'>ACTION</div>"
        "<div class='card-url'>infinitas://clear-history</div>"
        "<div class='card-title'>Clear History</div>"
        "<div class='card-desc'>Deletes all browsing history and redirects back to the history page.</div>"
        "</div>"

        /* delete-bookmark */
        "<div class='card'>"
        "<div class='tag tag-action'>ACTION</div>"
        "<div class='card-url'>infinitas://delete-bookmark/&lt;id&gt;</div>"
        "<div class='card-title'>Delete Bookmark</div>"
        "<div class='card-desc'>Removes a bookmark by numeric ID. Used by the Bookmarks page.</div>"
        "</div>"

        /* apply-font */
        "<div class='card'>"
        "<div class='tag tag-action'>ACTION</div>"
        "<div class='card-url'>infinitas://apply-font/&lt;name&gt;</div>"
        "<div class='card-title'>Apply Font</div>"
        "<div class='card-desc'>Sets a font globally across all tabs. Used by the Font Manager.</div>"
        "</div>"

        /* java */
        "<div class='card'>"
        "<div class='tag tag-action'>ACTION</div>"
        "<div class='card-url'>infinitas://java/launch?code=&amp;&hellip;</div>"
        "<div class='card-title'>Java Applet Launcher</div>"
        "<div class='card-desc'>Spawns a JVM subprocess and opens the applet in a native window. "
        "Requires the Java plugin installed in ~/.infinitas/plugins/.</div>"
        "</div>"

        "</div>" /* end .grid */

        /* keyboard shortcuts */
        "<div class='shortcuts'>"
        "<h2>Keyboard Shortcuts</h2>"
        "<div class='kb-grid'>"
        "<div class='kb'><span class='kb-desc'>New tab</span><span class='kb-key'>Ctrl+T</span></div>"
        "<div class='kb'><span class='kb-desc'>Close tab</span><span class='kb-key'>Ctrl+W</span></div>"
        "<div class='kb'><span class='kb-desc'>Focus address bar</span><span class='kb-key'>Ctrl+L</span></div>"
        "<div class='kb'><span class='kb-desc'>Reload</span><span class='kb-key'>F5</span></div>"
        "<div class='kb'><span class='kb-desc'>Hard reload / stop</span><span class='kb-key'>Ctrl+Shift+R</span></div>"
        "<div class='kb'><span class='kb-desc'>Bookmark page</span><span class='kb-key'>Ctrl+D</span></div>"
        "<div class='kb'><span class='kb-desc'>Developer console</span><span class='kb-key'>Ctrl+Shift+I</span></div>"
        "<div class='kb'><span class='kb-desc'>Fullscreen</span><span class='kb-key'>F11</span></div>"
        "</div>"
        "</div>"

        "</body></html>");
}

/* ── about / license page ────────────────────────────────────────────────── */

static gchar* gen_about_page(void) {
    return g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>About \xe2\x80\x94 Infinitas</title>"
        "<style>%s"
        ".about-card{background:var(--surf);border:1px solid var(--bdr);"
        "border-radius:14px;padding:28px 30px;line-height:1.7}"
        ".about-card h2{font-size:20px;font-weight:700;margin-bottom:14px}"
        ".about-card p{font-size:14px;color:var(--txt);margin-bottom:10px}"
        ".about-card .mut{color:var(--mut);font-size:13px}"
        ".about-card code{background:#0f0f13;padding:2px 7px;border-radius:5px;"
        "font-size:13px;color:var(--acc)}"
        ".about-card a{color:var(--acc);text-decoration:none}"
        ".about-card a:hover{text-decoration:underline}"
        "</style></head><body>"
        "<div class='hdr'><div><h1>About</h1>"
        "<p>License and program information</p></div></div>"
        "<div class='content'>"
        "<div class='about-card'>"
        "<h2>Infinitas \xe2\x80\x94 part of LeviathanOS</h2>"
        "<p>Free software under the GNU General Public License, version 3.</p>"
        "<p>This program comes with ABSOLUTELY NO WARRANTY.</p>"
        "<p class='mut'>Full license: <code>/usr/share/doc/leviathanos/LICENSE</code></p>"
        "<p><a href='https://www.gnu.org/licenses/gpl-3.0.html'>"
        "https://www.gnu.org/licenses/gpl-3.0.html</a></p>"
        "</div>"
        "</div>"
        "</body></html>",
        PAGE_CSS);
}

/* ── offline page ────────────────────────────────────────────────────────── */

/* ── Infinitas Inner: "No Internet" egg ───────────────────────────────────
 * A literal spacebar-spamming game injected into the offline page. Spam the
 * spacebar: it counts presses, a little character reacts, a combo meter fills,
 * and the best score persists in localStorage. Self-contained, lightweight.
 * Passed to g_strdup_printf as a plain "%s" arg, so its '%' are not specifiers. */
static const gchar OFFLINE_GAME[] =
"<style>"
"#sg-wrap{text-align:center;padding:8px 28px 48px}"
"#sg-card{display:inline-block;min-width:280px;background:var(--surf);"
"border:1px solid var(--bdr);border-radius:14px;padding:22px 26px}"
"#sg-char{font-size:52px;line-height:1;transition:transform .08s}"
"#sg-count{font-size:30px;font-weight:800;color:var(--acc);margin-top:6px}"
"#sg-hi{font-size:12px;color:var(--mut);margin-top:2px}"
"#sg-meter{height:8px;background:var(--bdr);border-radius:4px;margin-top:14px;overflow:hidden}"
"#sg-fill{height:100%;width:0;background:var(--acc);transition:width .12s}"
"#sg-hint{font-size:12px;color:var(--mut);margin-top:14px}"
".sg-hit{transform:scale(1.35) rotate(8deg)!important}"
"</style>"
"<div id='sg-wrap'><div id='sg-card'>"
"<div id='sg-char'>\xF0\x9F\x9A\x80</div><div id='sg-count'>0</div><div id='sg-hi'>Best: 0</div>"
"<div id='sg-meter'><div id='sg-fill'></div></div>"
"<div id='sg-hint'>No internet? Spam the spacebar.</div>"
"</div></div>"
"<script>(function(){"
"var c=0,hi=0,decay;"
"try{hi=parseInt(localStorage.getItem('infinitas_spacegame')||'0',10)||0;}catch(_){}"
"var ch=document.getElementById('sg-char'),ct=document.getElementById('sg-count'),"
"hd=document.getElementById('sg-hi'),fl=document.getElementById('sg-fill');"
"if(!ch)return;hd.textContent='Best: '+hi;"
"var faces=['\xF0\x9F\x9A\x80','\xF0\x9F\x9B\xB8','\xE2\x9C\xA8','\xF0\x9F\x92\xAB','\xF0\x9F\x8C\x9F'];"
"function hit(){c++;ct.textContent=c;ch.textContent=faces[c%faces.length];"
"ch.classList.add('sg-hit');setTimeout(function(){ch.classList.remove('sg-hit');},80);"
"fl.style.width=Math.min(100,(c%25)*4)+'%';"
"if(c>hi){hi=c;hd.textContent='Best: '+hi;"
"try{localStorage.setItem('infinitas_spacegame',String(hi));}catch(_){}}"
"clearTimeout(decay);decay=setTimeout(function(){c=0;ct.textContent='0';"
"fl.style.width='0';ch.textContent='\xF0\x9F\x98\xB4';},2500);}"
"document.addEventListener('keydown',function(e){if(e.code==='Space'||e.key===' '){"
"e.preventDefault();hit();}});"
"})();</script>";

static gchar* gen_offline_page(InfinitasBrowser *browser) {
    GString *body = g_string_new("");
    if (browser && browser->offstore) {
        GPtrArray *list = offstore_list(browser->offstore);
        if (!list || list->len == 0) {
            g_string_append(body,
                "<div class='empty'><h2>No offline pages saved</h2>"
                "<p>Bookmark (&#9733; or Ctrl+D) any page to save an encrypted offline copy.</p>"
                "</div>");
        } else {
            for (guint i = 0; i < list->len; i++) {
                OfflineEntry *e = g_ptr_array_index(list, i);
                gchar *t = e->title ? g_markup_escape_text(e->title, -1) : g_strdup("Untitled");
                gchar *u = e->url   ? g_markup_escape_text(e->url,   -1) : g_strdup("");
                g_string_append_printf(body,
                    "<div class='card'>"
                    "<div class='flex1'>"
                    "<div class='card-title'>%s</div>"
                    "<div class='card-url'><a href='%s'>%s</a></div>"
                    "</div>"
                    "<span class='card-meta'>%s</span>"
                    "<a href='%s'><button class='btn btn-muted'>Open</button></a>"
                    "</div>",
                    t, u, u,
                    e->saved_at ? e->saved_at : "",
                    u);
                g_free(t); g_free(u);
            }
        }
        if (list) g_ptr_array_free(list, TRUE);
    }
    gchar *body_str = g_string_free(body, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Offline Pages \xe2\x80\x94 Infinitas</title>"
        "<style>%s</style></head><body>"
        "<div class='hdr'><div>"
        "<h1>&#x1F4E6; Offline Archive</h1>"
        "<p>Starred pages \xe2\x80\x94 encrypted, available without internet</p>"
        "</div></div>"
        "<div class='content'>%s</div>%s</body></html>",
        PAGE_CSS, body_str, OFFLINE_GAME);
    g_free(body_str);
    return html;
}

/* ── system font list ────────────────────────────────────────────────────── */

static gchar* get_system_fonts_json(void) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray  *list = g_ptr_array_new_with_free_func(g_free);

    FILE *fp = popen("fc-list --format='%{family[0]}\\n' 2>/dev/null | sort -u", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
            char *comma = strchr(line, ','); if (comma) *comma = '\0';
            gchar *t = g_strstrip(g_strdup(line));
            if (*t && !g_hash_table_contains(seen, t)) {
                g_hash_table_insert(seen, g_strdup(t), NULL);
                g_ptr_array_add(list, t);
            } else { g_free(t); }
        }
        pclose(fp);
    }

    const gchar *extras[] = { "Comic Sans MS", "Comic Sans", "Comic Neue", NULL };
    for (int i = 0; extras[i]; i++) {
        if (!g_hash_table_contains(seen, extras[i])) {
            g_hash_table_insert(seen, g_strdup(extras[i]), NULL);
            g_ptr_array_add(list, g_strdup(extras[i]));
        }
    }
    g_ptr_array_sort(list, (GCompareFunc)g_ascii_strcasecmp);

    GString *json = g_string_new("[");
    for (guint i = 0; i < list->len; i++) {
        gchar *esc = g_strescape(g_ptr_array_index(list, i), "");
        if (i) g_string_append_c(json, ',');
        g_string_append_printf(json, "\"%s\"", esc);
        g_free(esc);
    }
    g_string_append_c(json, ']');
    g_hash_table_destroy(seen);
    g_ptr_array_free(list, TRUE);
    return g_string_free(json, FALSE);
}

/* ── font manager page (split to keep string literals short) ─────────────── */

static const gchar FONTS_P1[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
"<title>Font Manager \xe2\x80\x94 Infinitas</title><style>"
"*{margin:0;padding:0;box-sizing:border-box}"
":root{--bg:#0f0f13;--surf:#1a1a24;--bdr:#2a2a3a;"
"--acc:#7c6af7;--grn:#4ade80;--txt:#e2e8f0;--mut:#64748b}"
"body{background:var(--bg);color:var(--txt);font-family:system-ui,sans-serif;"
"height:100vh;display:flex;flex-direction:column;overflow:hidden}"
".hdr{padding:16px 24px;background:var(--surf);border-bottom:1px solid var(--bdr);flex-shrink:0}"
".hdr h1{font-size:18px;font-weight:700}"
".hdr p{font-size:13px;color:var(--mut);margin-top:3px}"
".bar{padding:10px 24px;background:var(--surf);border-bottom:1px solid var(--bdr);"
"display:flex;gap:10px;align-items:center;flex-shrink:0}"
"input[type=text],input[type=number]{background:var(--bg);border:1px solid var(--bdr);"
"border-radius:8px;color:var(--txt);padding:7px 11px;font-size:14px;outline:none}"
"input:focus{border-color:var(--acc)}"
"#srch{flex:0 0 200px}#prev{flex:1}#sz{width:60px;text-align:center}"
".lbl{font-size:13px;color:var(--mut);white-space:nowrap}"
".grid{flex:1;overflow-y:auto;padding:18px 24px;"
"display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));"
"gap:12px;align-content:start}"
".card{background:var(--surf);border:2px solid var(--bdr);border-radius:12px;"
"padding:14px 16px;cursor:pointer;transition:border-color .15s,background .15s}"
".card:hover{border-color:var(--acc);background:#1e1e2e}"
".card.sel{border-color:var(--grn)}"
".cname{font-size:11px;color:var(--mut);text-transform:uppercase;"
"letter-spacing:.7px;margin-bottom:7px;white-space:nowrap;"
"overflow:hidden;text-overflow:ellipsis}"
".cprev{color:var(--txt);line-height:1.35;overflow:hidden;"
"display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical}"
".tag{display:inline-block;background:#1a3a2a;color:var(--grn);"
"font-size:9px;padding:1px 5px;border-radius:4px;margin-left:5px;"
"vertical-align:middle;font-family:system-ui!important}"
".foot{padding:11px 24px;background:var(--surf);border-top:1px solid var(--bdr);"
"display:flex;align-items:center;justify-content:space-between;flex-shrink:0}"
".info{font-size:14px;color:var(--mut)}.info strong{color:var(--txt)}"
".cnt{font-size:12px;color:var(--mut)}"
".btn{background:var(--acc);color:#fff;border:none;border-radius:8px;"
"padding:9px 22px;font-size:14px;font-weight:600;cursor:pointer}"
".btn:hover:not(:disabled){filter:brightness(1.15)}"
".btn:disabled{opacity:.4;cursor:default}"
"::-webkit-scrollbar{width:5px}"
"::-webkit-scrollbar-thumb{background:var(--bdr);border-radius:3px}"
"</style></head><body>"
"<div class='hdr'><h1>Font Manager</h1>"
"<p>System fonts \xe2\x80\x94 click to preview, then Apply to change all pages</p></div>"
"<div class='bar'>"
"<input id='srch' type='text' placeholder='\xf0\x9f\x94\x8d Search\xe2\x80\xa6' oninput='filt()'>"
"<input id='prev' type='text' value='The quick brown fox jumps over the lazy dog 0123'>"
"<span class='lbl'>Size</span><input id='sz' type='number' value='20' min='8' max='72'>"
"</div>"
"<div class='grid' id='grid'></div>"
"<div class='foot'>"
"<div><span class='info'>Selected: <strong id='seln'>None</strong></span>"
"<span class='cnt' id='cnt'></span></div>"
"<button class='btn' id='abtn' disabled onclick='apply()'>Apply Font</button>"
"</div><script>"
"const DYSLEX=['Comic Sans MS','Comic Sans','Comic Neue','OpenDyslexic',"
"'Dyslexie','Lexie Readable','Sylexiad','Read Regular','Tiresias'];"
"const fonts=";

static const gchar FONTS_P2[] =
";"
"let sel=null,vis=[...fonts];"
"const grid=document.getElementById('grid');"
"const prev=document.getElementById('prev');"
"const sz=document.getElementById('sz');"
"const seln=document.getElementById('seln');"
"const abtn=document.getElementById('abtn');"
"const cnt=document.getElementById('cnt');"
"function isDx(f){return DYSLEX.some(d=>f.toLowerCase().includes(d.toLowerCase()));}"
"function txt(){return prev.value||'The quick brown fox';}"
"function size(){return Math.max(8,Math.min(72,parseInt(sz.value)||20));}"
"function render(){"
"cnt.textContent=' ('+vis.length+' fonts)';"
"grid.innerHTML=vis.map(f=>{"
"const tag=isDx(f)?'<span class=\"tag\">dyslexia-friendly</span>':'';"
"const s=f===sel?' sel':'';"
"return `<div class=\"card${s}\" data-f=\"${f.replace(/\"/g,'&quot;')}\" onclick=\"pick(this)\">`"
"+`<div class=\"cname\">${f}${tag}</div>`"
"+`<div class=\"cprev\" style=\"font-family:'${f}',sans-serif;font-size:${size()}px\">${txt()}</div>`"
"+'</div>';"
"}).join('');"
"if(sel){const c=grid.querySelector('.card.sel');"
"if(c)c.scrollIntoView({behavior:'smooth',block:'nearest'});}}"
"function filt(){"
"const q=document.getElementById('srch').value.toLowerCase();"
"vis=fonts.filter(f=>f.toLowerCase().includes(q));render();}"
"function pick(card){sel=card.dataset.f;seln.textContent=sel;abtn.disabled=false;render();}"
"function apply(){if(!sel)return;"
"location.href='infinitas://apply-font/'+encodeURIComponent(sel);}"
"prev.oninput=render;sz.oninput=render;render();"
"</script></body></html>";

gchar* infinitas_get_fonts_page(void) {
    gchar *json = get_system_fonts_json();
    GString *out = g_string_new(FONTS_P1);
    g_string_append(out, json);
    g_string_append(out, FONTS_P2);
    g_free(json);
    return g_string_free(out, FALSE);
}

/* ── apply-font handler ──────────────────────────────────────────────────── */

static void handle_apply_font(WebKitURISchemeRequest *req,
                               const gchar *encoded, InfinitasBrowser *browser) {
    gchar *family = g_uri_unescape_string(encoded, NULL);
    if (!family || !*family) {
        g_free(family);
        finish_with_html(req, g_strdup("<html><body>Error: no font name</body></html>"));
        return;
    }
    if (browser) {
        for (guint i = 0; i < browser->tabs->len; i++)
            renderer_apply_font(g_ptr_array_index(browser->tabs, i), family);
        settings_set_string(browser->settings, "font_family", family);
    }
    gchar *esc  = g_markup_escape_text(family, -1);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<script>setTimeout(()=>location.href='infinitas://fonts',900);</script>"
        "</head><body style='background:#0f0f13;color:#4ade80;font-family:system-ui;"
        "display:flex;align-items:center;justify-content:center;height:100vh;font-size:20px'>"
        "&#10003; Font applied: <strong style='font-family:\"%s\";margin-left:8px'>%s</strong>"
        "</body></html>", family, esc);
    g_free(esc); g_free(family);
    finish_with_html(req, html);
}

/* ── query string helper ─────────────────────────────────────────────────── */

static gchar* query_get(const gchar *query, const gchar *key) {
    if (!query || !key) return NULL;
    gchar *pattern = g_strdup_printf("%s=", key);
    const gchar *p = strstr(query, pattern);
    g_free(pattern);
    if (!p) return NULL;
    p += strlen(key) + 1;
    const gchar *end = strchr(p, '&');
    gchar *encoded = end ? g_strndup(p, end - p) : g_strdup(p);
    gchar *decoded = g_uri_unescape_string(encoded, NULL);
    g_free(encoded);
    return decoded;
}

/* ── PNO — Plentiful Notes for Orchestra ─────────────────────────────────── */

static const gchar PNO_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<title>PNO \xe2\x80\x94 Plentiful Notes for Orchestra</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
":root{--bg:#0f0f13;--surf:#1a1a24;--bdr:#2a2a3a;--acc:#7c6af7;"
"--mut:#64748b;--txt:#e2e8f0;--grn:#4ade80}"
"body{background:var(--bg);color:var(--txt);font-family:system-ui,sans-serif;"
"height:100vh;display:flex;flex-direction:column;overflow:hidden}"
/* drop zone */
"#drop{flex:1;display:flex;flex-direction:column;align-items:center;"
"justify-content:center;gap:16px;cursor:pointer;"
"border:2px dashed var(--bdr);border-radius:18px;margin:32px;"
"transition:border-color .2s,background .2s}"
"#drop.over{border-color:var(--acc);background:#1a1a2e}"
"#drop .icon{font-size:64px;line-height:1}"
"#drop h2{font-size:20px;font-weight:600}"
"#drop p{font-size:13px;color:var(--mut)}"
"#drop input{display:none}"
/* player layout */
"#player{display:none;flex:1;flex-direction:row;overflow:hidden}"
/* playlist */
"#plist{width:260px;background:var(--surf);border-right:1px solid var(--bdr);"
"display:flex;flex-direction:column;overflow:hidden}"
"#plist-header{padding:14px 18px;font-size:12px;color:var(--mut);"
"text-transform:uppercase;letter-spacing:.5px;border-bottom:1px solid var(--bdr);"
"display:flex;justify-content:space-between;align-items:center}"
"#plist-items{overflow-y:auto;flex:1}"
".pitem{padding:10px 18px;cursor:pointer;font-size:13px;border-bottom:1px solid #14141a;"
"white-space:nowrap;overflow:hidden;text-overflow:ellipsis;transition:background .1s}"
".pitem:hover{background:#22223a}"
".pitem.active{background:#1e1a3a;color:var(--acc)}"
".pitem .dur{font-size:11px;color:var(--mut);float:right;margin-top:1px}"
/* main controls */
"#ctrl{flex:1;display:flex;flex-direction:column;align-items:center;"
"justify-content:center;gap:24px;padding:32px}"
"#track-name{font-size:22px;font-weight:700;text-align:center;"
"max-width:480px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
"#track-sub{font-size:13px;color:var(--mut)}"
/* piano decoration */
"#keys{display:flex;gap:2px;margin:4px 0}"
".wk{width:18px;height:60px;background:#e2e8f0;border-radius:0 0 4px 4px;"
"border:1px solid #94a3b8}"
".bk{width:12px;height:38px;background:#1a1a24;border-radius:0 0 3px 3px;"
"margin:0 -7px;z-index:1;position:relative;border:1px solid #2a2a3a}"
/* seek */
"#seek-wrap{width:100%;max-width:500px;display:flex;align-items:center;gap:10px}"
"#seek{flex:1;-webkit-appearance:none;height:4px;border-radius:2px;"
"background:var(--bdr);outline:none;cursor:pointer}"
"#seek::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;"
"border-radius:50%;background:var(--acc);cursor:pointer}"
".time{font-size:12px;color:var(--mut);min-width:38px;text-align:center}"
/* buttons */
"#btns{display:flex;align-items:center;gap:20px}"
".cbtn{background:none;border:none;color:var(--txt);cursor:pointer;font-size:22px;"
"padding:6px;border-radius:8px;transition:color .15s,background .15s;line-height:1}"
".cbtn:hover{color:var(--acc)}"
"#play-btn{font-size:36px;background:var(--acc);color:#fff;border-radius:50%;"
"width:60px;height:60px;display:flex;align-items:center;justify-content:center;"
"transition:filter .15s}"
"#play-btn:hover{filter:brightness(1.15)}"
".cbtn.active{color:var(--acc)}"
/* volume */
"#vol-wrap{display:flex;align-items:center;gap:10px;font-size:18px}"
"#vol{width:90px;-webkit-appearance:none;height:3px;border-radius:2px;"
"background:var(--bdr);outline:none;cursor:pointer}"
"#vol::-webkit-slider-thumb{-webkit-appearance:none;width:12px;height:12px;"
"border-radius:50%;background:var(--mut);cursor:pointer}"
/* add more btn */
"#add-more{position:fixed;bottom:20px;right:20px;background:var(--surf);"
"border:1px solid var(--bdr);color:var(--txt);border-radius:10px;"
"padding:8px 16px;font-size:13px;cursor:pointer;display:none;"
"transition:border-color .15s}"
"#add-more:hover{border-color:var(--acc)}"
"::-webkit-scrollbar{width:4px}"
"::-webkit-scrollbar-thumb{background:var(--bdr);border-radius:2px}"
"</style></head><body>"

/* drop zone */
"<div id='drop' onclick='document.getElementById(\"fi\").click()'"
" ondragover='ev(event)' ondrop='drop(event)'>"
"<div class='icon'>\xf0\x9f\x8e\xb9</div>"
"<h2>Drop audio here</h2>"
"<p>or click to browse &mdash; MP3, FLAC, WAV, OGG, M4A\xe2\x80\xa6</p>"
"<p style='font-size:11px;color:#475569;margin-top:8px'>"
"Plentiful Notes for Orchestra</p>"
"<input id='fi' type='file' accept='audio/*' multiple onchange='fromInput(this)'>"
"</div>"

/* player */
"<div id='player'>"
"<div id='plist'>"
"<div id='plist-header'>"
"<span id='pcount'>0 tracks</span>"
"<button class='cbtn' onclick='document.getElementById(\"fi2\").click()' "
"title='Add more' style='font-size:16px'>\xe2\x8a\x95</button>"
"<input id='fi2' type='file' accept='audio/*' multiple style='display:none' "
"onchange='fromInput(this)'>"
"</div>"
"<div id='plist-items'></div>"
"</div>"
"<div id='ctrl'>"
"<div id='track-name'>No track</div>"
"<div id='track-sub' style='color:var(--mut);font-size:13px'>PNO</div>"
/* piano keys decoration */
"<div id='keys'>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div><div class='bk'></div>"
"<div class='wk'></div>"
"</div>"
"<div id='seek-wrap'>"
"<span class='time' id='cur'>0:00</span>"
"<input id='seek' type='range' value='0' min='0' max='1000' step='1' "
"oninput='seek(this.value)'>"
"<span class='time' id='dur'>0:00</span>"
"</div>"
"<div id='btns'>"
"<button class='cbtn' id='shuf-btn' onclick='toggleShuffle()' title='Shuffle'>\xe2\x87\x8c</button>"
"<button class='cbtn' onclick='prev()' title='Previous'>\xe2\x8f\xae</button>"
"<button class='cbtn' id='play-btn' onclick='togglePlay()'>&#x25B6;</button>"
"<button class='cbtn' onclick='next()' title='Next'>\xe2\x8f\xad</button>"
"<button class='cbtn' id='loop-btn' onclick='toggleLoop()' title='Loop'>\xe2\x87\x84</button>"
"</div>"
"<div id='vol-wrap'>"
"\xf0\x9f\x94\x89"
"<input id='vol' type='range' value='100' min='0' max='100' "
"oninput='setVol(this.value)'>"
"\xf0\x9f\x94\x8a"
"</div>"
"</div>"
"</div>"

"<script>"
"var tracks=[],cur=0,shuf=false,looping=false;"
"var audio=new Audio();"
"audio.volume=1;"

/* format seconds → m:ss */
"function fmt(s){s=Math.floor(s||0);"
"return Math.floor(s/60)+':'+(s%60<10?'0':'')+s%60;}"

/* drag/drop */
"function ev(e){e.preventDefault();document.getElementById('drop').classList.add('over');}"
"function drop(e){e.preventDefault();"
"document.getElementById('drop').classList.remove('over');"
"addFiles(e.dataTransfer.files);}"
"function fromInput(el){addFiles(el.files);el.value='';}"

"function addFiles(files){"
"var added=0;"
"for(var i=0;i<files.length;i++){"
"var f=files[i];"
"if(f.type.startsWith('audio/')||/\\.(mp3|flac|wav|ogg|m4a|aac|opus|wma)$/i.test(f.name)){"
"tracks.push({name:f.name.replace(/\\.[^.]+$/,''),url:URL.createObjectURL(f)});"
"added++;}"
"}"
"if(added){renderList();showPlayer();if(tracks.length===added)playAt(0);}}"

"function showPlayer(){"
"document.getElementById('drop').style.display='none';"
"var p=document.getElementById('player');"
"p.style.display='flex';"
"}"

"function renderList(){"
"var el=document.getElementById('plist-items');"
"el.innerHTML='';"
"tracks.forEach(function(t,i){"
"var d=document.createElement('div');"
"d.className='pitem'+(i===cur?' active':'');"
"d.textContent=t.name;"
"d.onclick=function(){playAt(i);};"
"el.appendChild(d);"
"});"
"document.getElementById('pcount').textContent=tracks.length+' track'+(tracks.length===1?'':'s');"
"}"

"function playAt(i){"
"cur=i;audio.src=tracks[i].url;audio.play();"
"document.getElementById('track-name').textContent=tracks[i].name;"
"document.getElementById('play-btn').innerHTML='&#x23F8;';"
"renderList();"
"}"

"function togglePlay(){"
"if(audio.paused){audio.play();document.getElementById('play-btn').innerHTML='&#x23F8;';}"
"else{audio.pause();document.getElementById('play-btn').innerHTML='&#x25B6;';}"
"}"
"function prev(){playAt(cur>0?cur-1:tracks.length-1);}"
"function next(){"
"var n=shuf?Math.floor(Math.random()*tracks.length):(cur+1)%tracks.length;"
"playAt(n);}"
"function toggleShuffle(){"
"shuf=!shuf;"
"document.getElementById('shuf-btn').classList.toggle('active',shuf);}"
"function toggleLoop(){"
"looping=!looping;audio.loop=looping;"
"document.getElementById('loop-btn').classList.toggle('active',looping);}"
"function seek(v){if(audio.duration)audio.currentTime=audio.duration*v/1000;}"
"function setVol(v){audio.volume=v/100;}"

"audio.addEventListener('timeupdate',function(){"
"if(!audio.duration)return;"
"document.getElementById('seek').value=Math.round(audio.currentTime/audio.duration*1000);"
"document.getElementById('cur').textContent=fmt(audio.currentTime);"
"document.getElementById('dur').textContent=fmt(audio.duration);"
"});"
"audio.addEventListener('ended',function(){if(!looping)next();});"
"audio.addEventListener('play',function(){"
"document.getElementById('play-btn').innerHTML='&#x23F8;';});"
"audio.addEventListener('pause',function(){"
"document.getElementById('play-btn').innerHTML='&#x25B6;';});"
"</script></body></html>";

/* ── protocol management page ────────────────────────────────────────────── */

static gchar* gen_protocol_page(InfinitasBrowser *browser) {
    GString *rows = g_string_new("");

    if (browser && browser->protocols) {
        GPtrArray *list = protocol_list(browser->protocols);
        for (guint i = 0; i < list->len; i++) {
            InfinitasProtocol *p = g_ptr_array_index(list, i);
            gchar *name   = g_markup_escape_text(p->name,   -1);
            gchar *target = g_markup_escape_text(p->target, -1);
            const gchar *type_color =
                p->type == PROTOCOL_REROUTE  ? "#7c6af7" :
                p->type == PROTOCOL_MASK     ? "#4ade80"  : "#f87171";
            const gchar *type_label =
                p->type == PROTOCOL_REROUTE  ? "REROUTE"  :
                p->type == PROTOCOL_MASK     ? "MASK"     : "OPEN APP";
            g_string_append_printf(rows,
                "<tr>"
                "<td style='font-family:monospace;color:#7c6af7'>infinitas://%s</td>"
                "<td><span style='background:#1a1a2e;color:%s;padding:2px 8px;"
                "border-radius:4px;font-size:11px;font-weight:700'>%s</span></td>"
                "<td style='color:#94a3b8;font-size:13px;max-width:360px;"
                "overflow:hidden;text-overflow:ellipsis'>%s</td>"
                "<td><a href='infinitas://delete-protocol/%s'>"
                "<button class='btn btn-del'>Remove</button></a></td>"
                "</tr>",
                name, type_color, type_label, target, name);
            g_free(name); g_free(target);
        }
        protocol_list_free(list);
    }

    gchar *rows_str = g_string_free(rows, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Protocol Manager \xe2\x80\x94 Infinitas</title>"
        "<style>%s"
        "table{width:100%%;border-collapse:collapse}"
        "th{text-align:left;font-size:11px;color:var(--mut);text-transform:uppercase;"
        "letter-spacing:.5px;padding:0 12px 10px 12px;border-bottom:1px solid var(--bdr)}"
        "td{padding:12px;border-bottom:1px solid #1e1e28;font-size:14px;vertical-align:middle}"
        "tr:last-child td{border-bottom:none}"
        ".form-row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:16px}"
        "input,select{background:#0f0f13;border:1px solid var(--bdr);border-radius:8px;"
        "color:var(--txt);padding:8px 12px;font-size:14px}"
        "input:focus,select:focus{outline:none;border-color:var(--acc)}"
        "input.name-in{width:130px}input.target-in{flex:1;min-width:200px}"
        ".type-badge{font-size:11px;font-weight:700;padding:2px 8px;border-radius:4px}"
        ".desc-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:12px}"
        ".desc-card{background:#0f0f13;border:1px solid var(--bdr);border-radius:8px;"
        "padding:12px;font-size:12px;color:var(--mut)}"
        ".desc-card strong{display:block;margin-bottom:4px}"
        "</style></head><body>"
        "<div class='hdr'>"
        "<div><h1>&#x1F517; Protocol Manager</h1>"
        "<p>Define custom <code style='color:var(--acc)'>infinitas://</code> shortcuts</p></div>"
        "</div>"
        "<div class='content'>"
        "<div class='desc-grid'>"
        "<div class='desc-card'>"
        "<strong style='color:#7c6af7'>REROUTE</strong>"
        "Browser navigates to target. Address bar updates.</div>"
        "<div class='desc-card'>"
        "<strong style='color:#4ade80'>MASK</strong>"
        "Target content shown, but address bar keeps <code>infinitas://name</code>.</div>"
        "<div class='desc-card'>"
        "<strong style='color:#f87171'>OPEN APP</strong>"
        "OS-level launch via xdg-open. Opens file or app.</div>"
        "</div>"
        "<div style='background:var(--surf);border:1px solid var(--bdr);border-radius:12px;"
        "padding:20px;margin:20px 0'>"
        "<h2 style='font-size:14px;color:var(--mut);margin-bottom:16px;"
        "text-transform:uppercase;letter-spacing:.5px'>Add Protocol</h2>"
        "<div class='form-row'>"
        "<span style='color:var(--acc);font-family:monospace;font-size:14px'>infinitas://</span>"
        "<input class='name-in' id='n' placeholder='name' oninput=\"document.getElementById('prev').textContent='infinitas://'+this.value\">"
        "<select id='t'>"
        "<option value='reroute'>reroute</option>"
        "<option value='mask'>mask</option>"
        "<option value='open_app'>open app</option>"
        "</select>"
        "<input class='target-in' id='g' placeholder='https://example.com or infinitas://tab'>"
        "<button class='btn btn-acc' onclick=\""
        "var n=document.getElementById('n').value.trim(),"
        "t=document.getElementById('t').value,"
        "g=document.getElementById('g').value.trim();"
        "if(n&&g)location.href='infinitas://define-protocol?name='+encodeURIComponent(n)"
        "+'&type='+encodeURIComponent(t)+'&target='+encodeURIComponent(g);"
        "\">Add</button>"
        "</div>"
        "<div style='font-size:12px;color:var(--mut);margin-top:8px'>"
        "Preview: <code id='prev' style='color:var(--acc)'>infinitas://</code></div>"
        "</div>"
        "<div style='background:var(--surf);border:1px solid var(--bdr);border-radius:12px;overflow:hidden'>"
        "<table><thead><tr>"
        "<th>Shortcut</th><th>Type</th><th>Target</th><th></th>"
        "</tr></thead><tbody>%s</tbody></table></div>"
        "</div></body></html>",
        PAGE_CSS, rows_str);
    g_free(rows_str);
    return html;
}

static void handle_define_protocol(WebKitURISchemeRequest *req, const gchar *query,
                                    InfinitasBrowser *browser) {
    gchar *name   = query_get(query, "name");
    gchar *type   = query_get(query, "type");
    gchar *target = query_get(query, "target");

    if (name && *name && target && *target && browser && browser->protocols) {
        /* Sanitize name: lowercase, strip non-alnum/hyphen */
        for (gchar *p = name; *p; p++) {
            if (g_ascii_isalpha(*p)) *p = g_ascii_tolower(*p);
            else if (!g_ascii_isdigit(*p) && *p != '-') *p = '-';
        }
        /* Auto-add https:// if no scheme given */
        gchar *full_target;
        if (!strstr(target, "://"))
            full_target = g_strdup_printf("https://%s", target);
        else
            full_target = g_strdup(target);
        protocol_add(browser->protocols, name,
                     protocol_type_from_string(type), full_target);
        g_free(full_target);
    }
    g_free(name); g_free(type); g_free(target);
    redirect_to(req, "infinitas://protocol");
}

/* ── PWA management ──────────────────────────────────────────────────────── */

static sqlite3* pwa_db(void) {
    static sqlite3 *db = NULL;
    if (db) return db;
    gchar *path = g_build_filename(g_get_home_dir(), ".infinitas", "pwas.db", NULL);
    if (sqlite3_open(path, &db) == SQLITE_OK)
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS pwas("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT,url TEXT UNIQUE,desktop_path TEXT,"
            "installed_at TEXT DEFAULT(datetime('now')));",
            NULL, NULL, NULL);
    g_free(path);
    return db;
}

static gchar* gen_pwa_page(void) {
    GString *rows = g_string_new("");
    /* Scan the actual .desktop launchers — the real source of truth, so the
     * manager shows every Infinitas PWA regardless of how it was created. */
    gchar *app_dir = g_build_filename(g_get_home_dir(),
        ".local", "share", "applications", NULL);
    GDir *dir = g_dir_open(app_dir, 0, NULL);
    int count = 0;
    if (dir) {
        const gchar *fname;
        while ((fname = g_dir_read_name(dir))) {
            if (!g_str_has_prefix(fname, "infinitas-") ||
                !g_str_has_suffix(fname, ".desktop")) continue;

            gchar *full = g_build_filename(app_dir, fname, NULL);
            GKeyFile *kf = g_key_file_new();
            gchar *app_name = NULL, *exec = NULL;
            if (g_key_file_load_from_file(kf, full, G_KEY_FILE_NONE, NULL)) {
                app_name = g_key_file_get_string(kf, "Desktop Entry", "Name", NULL);
                exec     = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
            }
            /* slug = filename without "infinitas-" prefix and ".desktop" suffix */
            gchar *slug = g_strdup(fname + strlen("infinitas-"));
            gchar *dot = g_strrstr(slug, ".desktop");
            if (dot) *dot = '\0';

            gchar *en = g_markup_escape_text(app_name ? app_name : slug, -1);
            gchar *ee = g_markup_escape_text(exec ? exec : "", -1);
            g_string_append_printf(rows,
                "<div class='card'><div class='flex1'>"
                "<div class='card-title'>%s</div>"
                "<div class='card-url'>%s</div></div>"
                "<a href='infinitas://uninstall-pwa/%s'>"
                "<button class='btn btn-del'>Uninstall</button></a></div>",
                en, ee, slug);
            count++;

            g_free(en); g_free(ee); g_free(slug);
            g_free(app_name); g_free(exec);
            g_key_file_free(kf); g_free(full);
        }
        g_dir_close(dir);
    }
    g_free(app_dir);

    if (count == 0)
        g_string_append(rows,
            "<div class='empty'><h2>No apps installed</h2>"
            "<p>Visit any website and click <strong>&#x2295; Install App</strong> "
            "to install it as an app.</p></div>");

    gchar *rows_str = g_string_free(rows, FALSE);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>PWA Manager \xe2\x80\x94 Infinitas</title>"
        "<style>%s</style></head><body>"
        "<div class='hdr'><div>"
        "<h1>&#x1F4E6; PWA Manager</h1>"
        "<p>Installed web apps \xe2\x80\x94 launch from your app menu</p>"
        "</div></div>"
        "<div class='content'>%s</div></body></html>",
        PAGE_CSS, rows_str);
    g_free(rows_str);
    return html;
}

static void handle_install_pwa(WebKitURISchemeRequest *req, const gchar *query,
                                InfinitasBrowser *browser) {
    gchar *url   = query_get(query, "url");
    gchar *title = query_get(query, "title");

    if (!url || !*url) {
        g_free(url); g_free(title);
        redirect_to(req, "infinitas://pwa");
        return;
    }
    if (!title || !*title) { g_free(title); title = g_strdup("Web App"); }

    /* Build slug: lowercase, non-alnum → hyphens, collapse, strip ends */
    gchar *slug_raw = g_ascii_strdown(title, -1);
    GString *slug_buf = g_string_new("");
    gboolean hyph = FALSE;
    for (gchar *p = slug_raw; *p; p++) {
        if (g_ascii_isalnum(*p)) { g_string_append_c(slug_buf, *p); hyph = FALSE; }
        else if (!hyph && slug_buf->len > 0) { g_string_append_c(slug_buf, '-'); hyph = TRUE; }
    }
    /* Strip trailing hyphen */
    if (slug_buf->len > 0 && slug_buf->str[slug_buf->len-1] == '-')
        g_string_truncate(slug_buf, slug_buf->len - 1);
    g_free(slug_raw);
    gchar *slug = g_string_free(slug_buf, FALSE);
    if (!*slug) { g_free(slug); slug = g_strdup("webapp"); }

    /* Find the running binary — try PATH first, then /proc/self/exe */
    gchar *bin = g_find_program_in_path("infinitas");
    if (!bin) {
        bin = g_file_read_link("/proc/self/exe", NULL);
        if (!bin) bin = g_strdup("infinitas");
    }

    /* Ensure apps dir exists */
    gchar *app_dir = g_build_filename(
        g_get_home_dir(), ".local", "share", "applications", NULL);
    g_mkdir_with_parents(app_dir, 0755);

    gchar *desktop_name = g_strdup_printf("infinitas-%s.desktop", slug);
    gchar *desktop_path = g_build_filename(app_dir, desktop_name, NULL);
    g_free(desktop_name);

    /* App icon: prefer the real favicon URL the page reported (downloaded
     * directly — reliable, since the tab has already navigated to this
     * scheme). Fall back to the live view's cached favicon, then the logo. */
    gchar *icon_value = g_strdup("infinitas");
    gchar *icon_url = query_get(query, "icon");
    gchar *icon_dir = g_build_filename(g_get_home_dir(), ".local", "share", "icons", NULL);
    g_mkdir_with_parents(icon_dir, 0755);
    gchar *icon_path = g_strdup_printf("%s/infinitas-pwa-%s.png", icon_dir, slug);
    gboolean got_icon = FALSE;

    if (icon_url && g_str_has_prefix(icon_url, "http")) {
        /* Download the favicon straight from its URL. */
        gchar *dl_argv[] = { "curl", "-sfL", "--max-time", "8",
                             "-o", icon_path, icon_url, NULL };
        gint status = 0;
        if (g_spawn_sync(NULL, dl_argv, NULL, G_SPAWN_SEARCH_PATH,
                         NULL, NULL, NULL, NULL, &status, NULL) &&
            status == 0 &&
            g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
            got_icon = TRUE;
        }
    }
    /* Fallback: the live view's cached favicon texture. */
    if (!got_icon && browser) {
        WebRenderer *r = browser_get_active_renderer(browser);
        if (r && r->view) {
            GdkTexture *fav = webkit_web_view_get_favicon(r->view);
            if (fav && gdk_texture_save_to_png(fav, icon_path)) got_icon = TRUE;
        }
    }
    if (got_icon) { g_free(icon_value); icon_value = g_strdup(icon_path); }
    g_free(icon_url); g_free(icon_dir); g_free(icon_path);

    gboolean wrote = FALSE;
    FILE *f = fopen(desktop_path, "w");
    if (f) {
        /* Quote the Exec path in case it contains spaces */
        fprintf(f,
            "[Desktop Entry]\n"
            "Version=1.0\n"
            "Name=%s\n"
            "Exec=\"%s\" --pwa=%s:%s\n"
            "Icon=%s\n"
            "Type=Application\n"
            "Categories=Network;WebBrowser;\n"
            "Comment=Installed via Infinitas\n"
            "StartupNotify=true\n",
            title, bin, title, url, icon_value);
        fclose(f);
        chmod(desktop_path, 0755);
        wrote = TRUE;

        /* Refresh desktop database (sync so it's done before success page) */
        gchar *db_argv[] = { "update-desktop-database", app_dir, NULL };
        g_spawn_sync(NULL, db_argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, NULL, NULL);
    }
    g_free(app_dir);
    g_free(bin);

    /* Store in DB */
    if (wrote) {
        sqlite3 *db = pwa_db();
        if (db) {
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db,
                    "INSERT OR REPLACE INTO pwas(name,url,desktop_path) VALUES(?,?,?);",
                    -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, title,        -1, NULL);
                sqlite3_bind_text(stmt, 2, url,          -1, NULL);
                sqlite3_bind_text(stmt, 3, desktop_path, -1, NULL);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }

    gchar *et = g_markup_escape_text(title, -1);
    gchar *eu = g_markup_escape_text(url,   -1);
    const gchar *status_color = wrote ? "#4ade80" : "#f87171";
    const gchar *status_msg   = wrote
        ? "Find it in your app menu."
        : "Could not write .desktop file \xe2\x80\x94 check permissions.";
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<script>setTimeout(()=>location.href='infinitas://pwa',1800);</script>"
        "</head><body style='background:#0f0f13;font-family:system-ui;"
        "display:flex;flex-direction:column;align-items:center;justify-content:center;"
        "height:100vh;gap:12px;text-align:center'>"
        "<div style='font-size:48px'>%s</div>"
        "<h2 style='font-size:22px;color:%s'>%s: %s</h2>"
        "<p style='color:#64748b;font-size:14px'>%s</p>"
        "<p style='color:#64748b;font-size:13px'>%s</p>"
        "</body></html>",
        wrote ? "&#x1F4E6;" : "&#x26A0;",
        status_color,
        wrote ? "Installed" : "Error",
        et, eu, status_msg);
    g_free(et); g_free(eu); g_free(slug); g_free(icon_value);
    g_free(desktop_path); g_free(url); g_free(title);
    finish_with_html(req, html);
}

/* ── main handler ────────────────────────────────────────────────────────── */

/* ── password manager (infinitas://passwords) ────────────────────────────── */

static gchar* gen_passwords_page(InfinitasBrowser *browser) {
    GString *rows = g_string_new("");
    guint count = 0;

    if (browser && browser->passwords) {
        GPtrArray *list = passwords_list(browser->passwords);
        for (guint i = 0; i < list->len; i++) {
            PasswordEntry *e = g_ptr_array_index(list, i);
            gchar *origin = e->origin   ? g_markup_escape_text(e->origin, -1)   : g_strdup("");
            gchar *user   = e->username ? g_markup_escape_text(e->username, -1) : g_strdup("");
            g_string_append_printf(rows,
                "<div class='row'>"
                "<div class='meta'><div class='org'>%s</div>"
                "<div class='usr'>%s</div></div>"
                "<div class='pw'>\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2</div>"
                "<a class='del' href='infinitas://delete-password/%d'>Delete</a>"
                "</div>",
                origin, (user && *user) ? user : "(no username)", e->id);
            g_free(origin); g_free(user);
            count++;
        }
        g_ptr_array_free(list, TRUE);
    }

    if (count == 0)
        g_string_append(rows,
            "<div class='empty'>No saved passwords yet.<br>"
            "<span>When you sign in to a site, Infinitas will offer to save it here.</span></div>");

    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Passwords</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{background:#0f0f13;color:#e2e8f0;font-family:system-ui,sans-serif;min-height:100vh}"
        ".hdr{background:#1a1a24;border-bottom:1px solid #2a2a3a;padding:18px 28px}"
        ".hdr h1{font-size:20px;font-weight:700}"
        ".hdr p{font-size:13px;color:#64748b;margin-top:4px}"
        ".wrap{max-width:820px;margin:24px auto;padding:0 20px}"
        ".note{background:#1a1a24;border:1px solid #2a2a3a;border-radius:10px;"
        "padding:12px 16px;font-size:13px;color:#94a3b8;margin-bottom:18px}"
        ".note b{color:#e2e8f0}"
        ".row{display:flex;align-items:center;gap:14px;background:#1a1a24;"
        "border:1px solid #2a2a3a;border-radius:10px;padding:12px 16px;margin-bottom:8px}"
        ".meta{flex:1;min-width:0}"
        ".org{font-size:14px;font-weight:600;word-break:break-all}"
        ".usr{font-size:13px;color:#94a3b8;margin-top:2px;word-break:break-all}"
        ".pw{color:#64748b;letter-spacing:2px;font-size:15px}"
        ".del{color:#f87171;text-decoration:none;font-size:13px;padding:6px 12px;"
        "border:1px solid #3a2a2a;border-radius:8px}"
        ".del:hover{background:#2a1a1a}"
        ".empty{text-align:center;color:#64748b;padding:60px 20px;font-size:15px}"
        ".empty span{font-size:13px;color:#475569}"
        "</style></head><body>"
        "<div class='hdr'><h1>\xF0\x9F\x94\x91 Passwords</h1>"
        "<p>Saved sign-ins are stored encrypted on this device. "
        "Card data is never saved.</p></div>"
        "<div class='wrap'>"
        "<div class='note'><b>Privacy:</b> passwords are hidden and kept "
        "encrypted under <code>~/.infinitas</code>. Credit-card details are "
        "intentionally never stored.</div>"
        "%s</div></body></html>", rows->str);

    g_string_free(rows, TRUE);
    return html;
}

void infinitas_scheme_handle(WebKitURISchemeRequest *request, gpointer user_data) {
    InfinitasBrowser *browser = (InfinitasBrowser *)user_data;
    if (!request) return;

    const gchar *uri  = webkit_uri_scheme_request_get_uri(request);
    const gchar *path = strstr(uri, "://");
    if (path) path += 3; else path = uri;

    /* ── internal page routing ── */
    if (g_str_has_prefix(path, "tab") &&
        (path[3] == '\0' || path[3] == '/')) {
        finish_with_html(request, gen_tab_page(browser));

    } else if (g_str_has_prefix(path, "bookmarks")) {
        finish_with_html(request, gen_bookmarks_page(browser));

    } else if (g_str_has_prefix(path, "history")) {
        finish_with_html(request, gen_history_page(browser));

    } else if (g_str_has_prefix(path, "running")) {
        finish_with_html(request, gen_running_page(browser));

    } else if (g_str_has_prefix(path, "fonts")) {
        finish_with_html(request, infinitas_get_fonts_page());

    } else if (g_str_has_prefix(path, "settings")) {
        finish_with_html(request, infinitas_get_settings_page());

    } else if (g_str_has_prefix(path, "passwords")) {
        finish_with_html(request, gen_passwords_page(browser));

    } else if (g_str_has_prefix(path, "delete-password/")) {
        gint id = atoi(path + strlen("delete-password/"));
        if (browser && browser->passwords)
            passwords_delete_by_id(browser->passwords, id);
        redirect_to(request, "infinitas://passwords");

    } else if (g_str_has_prefix(path, "apps")) {
        finish_with_html(request, infinitas_get_apps_page());

    } else if (g_str_has_prefix(path, "uninstall-pwa/")) {
        /* Delete the launcher + its captured favicon, clear any DB row. */
        const gchar *raw = path + strlen("uninstall-pwa/");
        GString *clean = g_string_new("");   /* sanitise: only [a-z0-9-] */
        for (const gchar *p = raw; *p; p++)
            if (g_ascii_isalnum(*p) || *p == '-') g_string_append_c(clean, *p);
        gchar *slug = g_string_free(clean, FALSE);
        if (*slug) {
            gchar *app_dir = g_build_filename(g_get_home_dir(),
                ".local", "share", "applications", NULL);
            gchar *desktop = g_strdup_printf("%s/infinitas-%s.desktop", app_dir, slug);
            gchar *icon = g_strdup_printf("%s/.local/share/icons/infinitas-pwa-%s.png",
                g_get_home_dir(), slug);
            remove(desktop);
            remove(icon);
            sqlite3 *db = pwa_db();
            if (db) {
                sqlite3_stmt *st;
                if (sqlite3_prepare_v2(db, "DELETE FROM pwas WHERE desktop_path=?;",
                        -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, desktop, -1, NULL);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
            gchar *db_argv[] = { "update-desktop-database", app_dir, NULL };
            g_spawn_sync(NULL, db_argv, NULL, G_SPAWN_SEARCH_PATH,
                         NULL, NULL, NULL, NULL, NULL, NULL);
            g_free(app_dir); g_free(desktop); g_free(icon);
        }
        g_free(slug);
        redirect_to(request, "infinitas://pwa");

    } else if (g_str_has_prefix(path, "pwa")) {
        finish_with_html(request, gen_pwa_page());

    } else if (g_str_has_prefix(path, "skribe")) {
        finish_with_html(request, infinitas_get_skribe_page());

    } else if (g_str_has_prefix(path, "incognito")) {
        finish_with_html(request, infinitas_get_incognito_page());

    } else if (g_str_has_prefix(path, "apply-font/")) {
        handle_apply_font(request, path + strlen("apply-font/"), browser);

    } else if (g_str_has_prefix(path, "delete-bookmark/")) {
        gint id = atoi(path + strlen("delete-bookmark/"));
        if (browser) bookmarks_remove(browser->bookmarks, id);
        redirect_to(request, "infinitas://bookmarks");

    } else if (g_str_has_prefix(path, "delete-history/")) {
        gint id = atoi(path + strlen("delete-history/"));
        if (browser) history_delete_entry(browser->history, id);
        redirect_to(request, "infinitas://history");

    } else if (g_str_has_prefix(path, "clear-history")) {
        if (browser) history_clear(browser->history);
        redirect_to(request, "infinitas://history");

    } else if (g_str_has_prefix(path, "help")) {
        finish_with_html(request, gen_help_page());

    } else if (g_str_has_prefix(path, "about")) {
        finish_with_html(request, gen_about_page());

    } else if (g_str_has_prefix(path, "offline")) {
        finish_with_html(request, gen_offline_page(browser));

    } else if (g_str_has_prefix(path, "signin") &&
               (path[6] == '\0' || path[6] == '/')) {
        redirect_to(request, "https://accounts.google.com");

    } else if (g_str_has_prefix(path, "account") &&
               (path[7] == '\0' || path[7] == '/')) {
        redirect_to(request, "https://myaccount.google.com");

    } else if (g_str_has_prefix(path, "signout")) {
        redirect_to(request, "https://accounts.google.com/logout");

    } else if (g_str_has_prefix(path, "mail")) {
        redirect_to(request, "https://mail.google.com");

    } else if (g_str_has_prefix(path, "pno") &&
               (path[3] == '\0' || path[3] == '/')) {
        finish_with_html(request, g_strdup(PNO_PAGE));

    } else if (g_str_has_prefix(path, "ping/")) {
        handle_ping(request, path + strlen("ping/"));

    } else if (g_str_has_prefix(path, "extensions")) {
        finish_with_html(request, gen_extensions_page(browser));

    } else if (g_str_has_prefix(path, "protocol") &&
               (path[8] == '\0' || path[8] == '/')) {
        finish_with_html(request, gen_protocol_page(browser));

    } else if (g_str_has_prefix(path, "define-protocol?")) {
        handle_define_protocol(request, path + strlen("define-protocol?"), browser);

    } else if (g_str_has_prefix(path, "delete-protocol/")) {
        if (browser && browser->protocols)
            protocol_delete(browser->protocols, path + strlen("delete-protocol/"));
        redirect_to(request, "infinitas://protocol");

    } else if (g_str_has_prefix(path, "pwa") &&
               (path[3] == '\0' || path[3] == '/')) {
        finish_with_html(request, gen_pwa_page());

    } else if (g_str_has_prefix(path, "install-pwa?")) {
        handle_install_pwa(request, path + strlen("install-pwa?"), browser);

    } else {
        /* check user-defined protocols */
        gboolean handled = FALSE;
        if (browser && browser->protocols) {
            gchar *lookup_name = g_strdup(path);
            gchar *slash = strchr(lookup_name, '/');
            if (slash) *slash = '\0';
            InfinitasProtocol *p = protocol_lookup(browser->protocols, lookup_name);
            g_free(lookup_name);
            if (p) {
                handled = TRUE;
                if (p->type == PROTOCOL_REROUTE) {
                    redirect_to(request, p->target);
                } else if (p->type == PROTOCOL_MASK) {
                    /* Show target content under the current infinitas:// URL */
                    const gchar *inner = g_str_has_prefix(p->target, "infinitas://")
                        ? p->target + strlen("infinitas://") : NULL;
                    gchar *iframe = inner
                        ? g_strdup_printf(
                            "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                            "<style>*{margin:0;padding:0}iframe{width:100vw;height:100vh;border:none}</style>"
                            "</head><body><iframe src='infinitas://%s'></iframe></body></html>", inner)
                        : g_strdup_printf(
                            "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                            "<style>*{margin:0;padding:0}iframe{width:100vw;height:100vh;border:none}</style>"
                            "</head><body><iframe src='%s'></iframe></body></html>", p->target);
                    finish_with_html(request, iframe);
                } else { /* OPEN_APP */
                    gchar *argv_[] = { "xdg-open", p->target, NULL };
                    g_spawn_async(NULL, argv_, NULL, G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, NULL);
                    redirect_to(request, "infinitas://tab");
                }
                g_free(p->name); g_free(p->target); g_free(p);
            }
        }
        /* check plugin-registered paths */
        if (g_plugin_paths) {
            for (guint i = 0; i < g_plugin_paths->len; i++) {
                PluginPathEntry *e = g_ptr_array_index(g_plugin_paths, i);
                if (g_str_has_prefix(path, e->prefix)) {
                    const gchar *sub = path + strlen(e->prefix);
                    gchar *html = e->handler(sub, e->userdata);
                    if (html) { finish_with_html(request, html); handled = TRUE; break; }
                }
            }
        }
        if (!handled) {
            finish_with_html(request, g_strdup(
                "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
                "<body style='background:#0f0f13;color:#e2e8f0;font-family:system-ui;"
                "display:flex;align-items:center;justify-content:center;height:100vh'>"
                "<h1>404 &mdash; Page not found</h1></body></html>"));
        }
    }
}

/* ── static page implementations ────────────────────────────────────────── */

gchar* infinitas_get_settings_page(void) {
    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>Settings \xe2\x80\x94 Infinitas</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:system-ui;background:#0f0f13;color:#e2e8f0;min-height:100vh}"
        ".hdr{background:#1a1a24;border-bottom:1px solid #2a2a3a;padding:16px 28px}"
        ".hdr h1{font-size:20px;font-weight:700}"
        ".content{padding:28px;max-width:640px;margin:0 auto}"
        ".section{background:#1a1a24;border:1px solid #2a2a3a;border-radius:12px;"
        "padding:20px;margin-bottom:20px}"
        ".section h2{font-size:14px;color:#7c6af7;text-transform:uppercase;"
        "letter-spacing:.5px;margin-bottom:16px}"
        ".row{display:flex;justify-content:space-between;align-items:center;"
        "padding:10px 0;border-bottom:1px solid #1a1a2a}"
        ".row:last-child{border-bottom:none}"
        "label{font-size:14px;color:#e2e8f0}"
        "input[type=text],select{background:#0f0f13;border:1px solid #2a2a3a;"
        "border-radius:8px;color:#e2e8f0;padding:7px 12px;font-size:14px;width:220px}"
        "button{background:#7c6af7;color:#fff;border:none;border-radius:8px;"
        "padding:10px 22px;font-size:14px;cursor:pointer}"
        "</style></head><body>"
        "<div class='hdr'><h1>Settings</h1></div>"
        "<div class='content'>"
        "<div class='section'><h2>General</h2>"
        "<div class='row'><label>Home Page</label>"
        "<input type='text' value='infinitas://newtab'></div>"
        "<div class='row'><label>Search Engine</label>"
        "<select><option>Google</option></select></div></div>"
        "<div class='section'><h2>Privacy</h2>"
        "<div class='row'><label>Enable JavaScript</label>"
        "<input type='checkbox' checked></div>"
        "<div class='row'><label>Enable Cookies</label>"
        "<input type='checkbox' checked></div></div>"
        "<button>Save Settings</button>"
        "</div></body></html>");
}

gchar* infinitas_get_apps_page(void) {
    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Apps</title>"
        "<style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:system-ui;background:#0f0f13;color:#e2e8f0}"
        ".hdr{background:#1a1a24;border-bottom:1px solid #2a2a3a;padding:16px 28px}"
        ".hdr h1{font-size:20px;font-weight:700}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));"
        "gap:16px;padding:28px;max-width:900px;margin:0 auto}"
        ".card{text-align:center;padding:20px;background:#1a1a24;"
        "border:1px solid #2a2a3a;border-radius:12px;cursor:pointer}"
        ".card:hover{border-color:#7c6af7}"
        ".icon{font-size:44px;margin-bottom:10px}"
        ".name{font-size:13px;color:#94a3b8}"
        "</style></head><body>"
        "<div class='hdr'><h1>Apps</h1></div>"
        "<div class='grid'>"
        "<div class='card'><div class='icon'>&#x1F4C4;</div><div class='name'>Files</div></div>"
        "<div class='card'><div class='icon'>&#x1F3A8;</div><div class='name'>Draw</div></div>"
        "<div class='card'><div class='icon'>&#x1F4DD;</div><div class='name'>Notes</div></div>"
        "</div></body></html>");
}

gchar* infinitas_get_pwa_page(void) {
    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>PWA Manager</title>"
        "<style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:system-ui;background:#0f0f13;color:#e2e8f0}"
        ".hdr{background:#1a1a24;border-bottom:1px solid #2a2a3a;padding:16px 28px}"
        ".hdr h1{font-size:20px;font-weight:700}"
        ".empty{text-align:center;padding:80px 20px;color:#64748b}"
        ".empty h2{font-size:18px;margin-bottom:8px}"
        "</style></head><body>"
        "<div class='hdr'><h1>PWA Manager</h1></div>"
        "<div class='empty'><h2>No apps installed</h2>"
        "<p>Install websites as apps from the browser menu.</p></div>"
        "</body></html>");
}

gchar* infinitas_get_skribe_page(void) {
    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Skribe</title>"
        "<style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:system-ui;background:#1a1a2e;color:#e2e8f0;"
        "height:100vh;display:flex;flex-direction:column}"
        ".tb{background:#16213e;padding:10px 20px;display:flex;gap:12px;align-items:center}"
        ".tb button{background:#0f3460;color:#e2e8f0;border:none;padding:8px 16px;"
        "border-radius:8px;cursor:pointer}"
        ".tb label{background:#7c6af7;color:#fff;padding:8px 16px;border-radius:8px;cursor:pointer}"
        ".main{flex:1;display:flex;align-items:center;justify-content:center;background:#0f0f1a}"
        ".drop{border:2px dashed #2a2a4a;border-radius:12px;padding:60px 40px;"
        "text-align:center;color:#64748b}"
        ".drop h2{font-size:20px;margin-bottom:8px}"
        "</style></head><body>"
        "<div class='tb'>"
        "<button>\xe2\x86\x90 Prev</button>"
        "<button>Next \xe2\x86\x92</button>"
        "<label>Upload PDF<input type='file' accept='application/pdf' style='display:none'></label>"
        "</div>"
        "<div class='main'><div class='drop'>"
        "<h2>Drop a PDF here</h2><p>or use Upload PDF above</p>"
        "</div></div></body></html>");
}

gchar* infinitas_get_incognito_page(void) {
    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Incognito</title>"
        "<style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:system-ui;background:#0d0d1a;color:#e2e8f0;min-height:100vh}"
        ".hdr{background:#12122a;border-bottom:1px solid #1e1e3a;padding:20px 28px;text-align:center}"
        ".hdr h1{font-size:22px;font-weight:700}"
        ".content{max-width:640px;margin:32px auto;padding:0 24px}"
        ".card{background:#12122a;border:1px solid #1e1e3a;border-radius:12px;"
        "padding:24px;margin-bottom:16px}"
        ".card h2{font-size:16px;margin-bottom:10px;color:#a78bfa}"
        ".card p,.card li{font-size:14px;color:#94a3b8;line-height:1.7}"
        "ul{padding-left:18px}"
        "</style></head><body>"
        "<div class='hdr'><h1>&#x1F576; Incognito Mode</h1></div>"
        "<div class='content'>"
        "<div class='card'><h2>What\xe2\x80\x99s private</h2>"
        "<p>History, cookies, and form data won\xe2\x80\x99t be saved after this session.</p></div>"
        "<div class='card'><h2>What\xe2\x80\x99s not private</h2>"
        "<ul><li>Your ISP can still see your traffic</li>"
        "<li>Websites you visit can log your IP address</li>"
        "<li>Downloads and bookmarks are saved</li></ul></div>"
        "</div></body></html>");
}
