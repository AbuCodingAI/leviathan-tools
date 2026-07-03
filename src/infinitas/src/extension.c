/**
 * Infinitas Browser - Extension Manager
 * Chrome MV2/MV3 + native extension support
 */

#include "extension.h"
#include "browser.h"
#include "infinitas_scheme.h"
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>

/* ── chrome.* API shim (injected before every extension's content scripts) ─ */

/* Part 1: runtime, storage, tabs */
static const gchar CHROME_SHIM_P1[] =
"(function(EXT_ID){"
"if(window._inf_shim_done)return;"
"window._inf_shim_done=true;"
"var _ml=[],_pcb={};"
"function mkStore(area){"
"var pfx='_inf_'+EXT_ID+'_'+area+':';"
"return{"
"get:function(k,cb){"
"  var r={};"
"  if(k===null||k===undefined){"
"    for(var i=0;i<localStorage.length;i++){"
"      var ki=localStorage.key(i);"
"      if(ki&&ki.startsWith(pfx)){"
"        try{r[ki.slice(pfx.length)]=JSON.parse(localStorage.getItem(ki));}catch(e){}"
"      }}"
"  }else if(typeof k==='string'){"
"    var v=localStorage.getItem(pfx+k);"
"    r[k]=v!==null?JSON.parse(v):undefined;"
"  }else if(Array.isArray(k)){"
"    k.forEach(function(n){var v=localStorage.getItem(pfx+n);r[n]=v!==null?JSON.parse(v):undefined;});"
"  }else{"
"    Object.keys(k).forEach(function(n){"
"      var v=localStorage.getItem(pfx+n);"
"      r[n]=v!==null?JSON.parse(v):k[n];});"
"  }"
"  if(cb)cb(r);return Promise.resolve(r);},"
"set:function(items,cb){"
"  Object.keys(items).forEach(function(n){localStorage.setItem(pfx+n,JSON.stringify(items[n]));});"
"  if(cb)cb();return Promise.resolve();},"
"remove:function(k,cb){"
"  (Array.isArray(k)?k:[k]).forEach(function(n){localStorage.removeItem(pfx+n);});"
"  if(cb)cb();return Promise.resolve();},"
"clear:function(cb){"
"  var rem=[];for(var i=0;i<localStorage.length;i++){var ki=localStorage.key(i);"
"  if(ki&&ki.startsWith(pfx))rem.push(ki);}rem.forEach(function(ki){localStorage.removeItem(ki);});"
"  if(cb)cb();return Promise.resolve();},"
"onChanged:{addListener:function(){}}"
"}}"
"window.chrome=window.chrome||{};"
"var c=window.chrome;"
"c.runtime={"
"  id:EXT_ID,"
"  lastError:null,"
"  getURL:function(p){return'infinitas://ext/'+EXT_ID+'/'+p;},"
"  sendMessage:function(eid,msg,opts,cb){"
"    if(typeof eid!=='string'){cb=opts;opts=msg;msg=eid;eid=EXT_ID;}"
"    if(typeof opts==='function'){cb=opts;opts={};}"
"    var id='cb'+Date.now()+Math.random();"
"    if(cb)_pcb[id]=cb;"
"    window.webkit.messageHandlers.infinitas_ext_msg.postMessage("
"      {type:'msg',extId:EXT_ID,cbId:id,data:msg});},"
"  onMessage:{addListener:function(fn){_ml.push(fn);},"
"             removeListener:function(fn){var i=_ml.indexOf(fn);if(i>=0)_ml.splice(i,1);}},"
"  onInstalled:{addListener:function(){}},"
"  onStartup:{addListener:function(){}}"
"};"
"c.storage={local:mkStore('local'),sync:mkStore('sync'),session:mkStore('session'),"
"  onChanged:{addListener:function(){}}};"
;

/* Part 2: tabs, windows, action, rest */
static const gchar CHROME_SHIM_P2[] =
"c.tabs={"
"  query:function(o,cb){"
"    var t={id:1,index:0,windowId:1,highlighted:true,active:true,pinned:false,"
"      url:location.href,title:document.title,incognito:false};"
"    var r=(!o||o.active!==false)?[t]:[];"
"    if(cb)cb(r);return Promise.resolve(r);},"
"  create:function(p,cb){"
"    if(p&&p.url)window.webkit.messageHandlers.infinitas_open_tab.postMessage({url:p.url});"
"    var t={id:Date.now(),url:p?p.url:''};if(cb)cb(t);return Promise.resolve(t);},"
"  update:function(id,p,cb){"
"    if(p&&p.url)window.webkit.messageHandlers.infinitas_navigate.postMessage({url:p.url});"
"    if(cb)cb({id:id});return Promise.resolve({id:id});},"
"  sendMessage:function(id,msg,opts,cb){"
"    if(typeof opts==='function')cb=opts;"
"    window.dispatchEvent(new CustomEvent('_inf_msg',{detail:msg}));"
"    if(cb)cb(undefined);},"
"  getCurrent:function(cb){"
"    var t={id:1,url:location.href,title:document.title};"
"    if(cb)cb(t);return Promise.resolve(t);},"
"  onUpdated:{addListener:function(){}},"
"  onActivated:{addListener:function(){}},"
"  onCreated:{addListener:function(){}},"
"  onRemoved:{addListener:function(){}}"
"};"
"c.windows={"
"  getCurrent:function(o,cb){if(typeof o==='function')cb=o;"
"    var w={id:1,focused:true,type:'normal'};if(cb)cb(w);return Promise.resolve(w);},"
"  getAll:function(o,cb){if(typeof o==='function')cb=o;"
"    if(cb)cb([{id:1}]);return Promise.resolve([{id:1}]);},"
"  onFocusChanged:{addListener:function(){}}"
"};"
"c.action=c.browserAction={"
"  setIcon:function(o,cb){if(cb)cb();return Promise.resolve();},"
"  setBadgeText:function(o,cb){if(cb)cb();return Promise.resolve();},"
"  setBadgeBackgroundColor:function(o,cb){if(cb)cb();return Promise.resolve();},"
"  setTitle:function(o,cb){if(cb)cb();return Promise.resolve();},"
"  setPopup:function(o,cb){if(cb)cb();return Promise.resolve();},"
"  getTitle:function(o,cb){if(cb)cb('');return Promise.resolve('');},"
"  onClicked:{addListener:function(){}}"
"};"
"c.contextMenus={"
"  create:function(o,cb){if(cb)cb();return 'cm_'+Date.now();},"
"  removeAll:function(cb){if(cb)cb();return Promise.resolve();},"
"  onClicked:{addListener:function(){}}"
"};"
"c.notifications={"
"  create:function(id,o,cb){"
"    if(typeof id==='object'){cb=o;o=id;id='n'+Date.now();}"
"    if(window.Notification&&Notification.permission==='granted'&&o)"
"      new Notification(o.title||'',{body:o.message||''});"
"    if(cb)cb(id);return Promise.resolve(id);},"
"  clear:function(id,cb){if(cb)cb(true);return Promise.resolve(true);},"
"  onClicked:{addListener:function(){}},"
"  onClosed:{addListener:function(){}}"
"};"
"c.i18n={"
"  getMessage:function(k){return k;},"
"  getUILanguage:function(){return navigator.language||'en';},"
"  detectLanguage:function(t,cb){if(cb)cb({isReliable:false,languages:[]});}"
"};"
"c.extension={getURL:c.runtime.getURL,getBackgroundPage:function(){return null;}};"
"c.permissions={"
"  contains:function(p,cb){if(cb)cb(true);return Promise.resolve(true);},"
"  request:function(p,cb){if(cb)cb(true);return Promise.resolve(true);},"
"  getAll:function(cb){if(cb)cb({permissions:[],origins:[]});}"
"};"
"c.alarms={create:function(){},clear:function(){},getAll:function(cb){if(cb)cb([]);},"
"  onAlarm:{addListener:function(){}}};"
"c.commands={onCommand:{addListener:function(){}},getAll:function(cb){if(cb)cb([]);}};"
"c.scripting={"
"  executeScript:function(o,cb){"
"    if(o&&o.func)try{o.func();}catch(e){}"
"    if(cb)cb([]);return Promise.resolve([]);},"
"  insertCSS:function(o,cb){"
"    if(o&&o.css){var s=document.createElement('style');"
"      s.textContent=o.css;document.head.appendChild(s);}"
"    if(cb)cb();return Promise.resolve();},"
"  removeCSS:function(o,cb){if(cb)cb();return Promise.resolve()}"
"};"
"c.declarativeNetRequest={"
"  updateDynamicRules:function(o,cb){if(cb)cb();return Promise.resolve();},"
"  getDynamicRules:function(cb){if(cb)cb([]);return Promise.resolve([]);},"
"  onRuleMatchedDebug:{addListener:function(){}}"
"};"
"c.webNavigation={"
"  onCompleted:{addListener:function(){}},"
"  onBeforeNavigate:{addListener:function(){}},"
"  onCommitted:{addListener:function(){}}"
"};"
"c.identity={"
"  getAuthToken:function(o,cb){if(cb)cb(undefined);},"
"  launchWebAuthFlow:function(o,cb){if(cb)cb(undefined);}"
"};"
"c.cookies={"
"  get:function(o,cb){if(cb)cb(null);},"
"  getAll:function(o,cb){if(cb)cb([]);},"
"  set:function(o,cb){if(cb)cb(null);},"
"  remove:function(o,cb){if(cb)cb(null);}"
"};"
"if(!window.browser)window.browser=window.chrome;"
/* Cross-script message dispatch */
"window.addEventListener('_inf_msg',function(e){"
"  _ml.forEach(function(fn){try{fn(e.detail,{id:EXT_ID},function(){});}catch(e){}});"
"});"
/* Reply handler for sendMessage callbacks */
"window._inf_reply=function(cbId,data){"
"  if(_pcb[cbId]){_pcb[cbId](data);delete _pcb[cbId];}"
"};"
"})";  /* end IIFE — EXT_ID is passed as argument below */

/* ── helpers ─────────────────────────────────────────────────────────────── */

static gchar* read_file(const gchar *path) {
    gchar *content = NULL;
    g_file_get_contents(path, &content, NULL, NULL);
    return content;
}

static void ext_free(gpointer p) {
    InfinitasExtension *e = p;
    if (!e) return;
    g_free(e->id); g_free(e->name); g_free(e->version);
    g_free(e->description); g_free(e->path);
    g_free(e->content_match); g_free(e->bg_script);
    g_free(e->action_title); g_free(e->action_popup);
    g_free(e->icon_path);
    if (e->content_scripts) g_ptr_array_free(e->content_scripts, TRUE);
    if (e->content_css)     g_ptr_array_free(e->content_css, TRUE);
    if (e->url_patterns) g_strfreev(e->url_patterns);
    g_free(e);
}

/* Convert a Chrome match pattern to a WebKit-compatible glob.
   Returns g_strdup'd string or NULL for <all_urls> / wildcards. */
static gchar* chrome_pattern_to_webkit(const gchar *p) {
    if (!p) return NULL;
    if (!g_strcmp0(p, "<all_urls>")) return NULL;
    if (!g_strcmp0(p, "*://*/*"))    return NULL;
    /* Chrome uses scheme://host/path — WebKit uses the same glob syntax */
    return g_strdup(p);
}

/* Find the best icon path (prefer 128, then 48, then 16, then any) */
static gchar* find_best_icon(const gchar *ext_dir, json_object *icons_obj) {
    if (!icons_obj || !json_object_is_type(icons_obj, json_type_object))
        return NULL;

    const gchar *sizes[] = {"128", "64", "48", "32", "16", NULL};
    for (int i = 0; sizes[i]; i++) {
        json_object *jv;
        if (json_object_object_get_ex(icons_obj, sizes[i], &jv)) {
            gchar *p = g_build_filename(ext_dir, json_object_get_string(jv), NULL);
            if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
            g_free(p);
        }
    }
    return NULL;
}

/* ── Chrome match patterns → NULL-terminated gchar** for WebKit ─────────── */

static gchar** patterns_from_array(const gchar *ext_dir, json_object *arr) {
    (void)ext_dir;
    if (!arr || !json_object_is_type(arr, json_type_array)) return NULL;

    int n = (int)json_object_array_length(arr);
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);

    for (int i = 0; i < n; i++) {
        const gchar *pat = json_object_get_string(json_object_array_get_idx(arr, i));
        gchar *wk = chrome_pattern_to_webkit(pat);
        if (!wk) { /* means "all URLs" — no filter */
            g_ptr_array_free(list, TRUE);
            return NULL;
        }
        g_ptr_array_add(list, wk);
    }

    if (list->len == 0) { g_ptr_array_free(list, TRUE); return NULL; }
    g_ptr_array_add(list, NULL);
    gchar **result = (gchar**)g_ptr_array_free(list, FALSE);
    return result;
}

/* ── load one extension directory ───────────────────────────────────────── */

static InfinitasExtension* load_ext(const gchar *dir, const gchar *name) {
    gchar *mp = g_build_filename(dir, "manifest.json", NULL);
    if (!g_file_test(mp, G_FILE_TEST_EXISTS)) { g_free(mp); return NULL; }

    gchar *raw = read_file(mp); g_free(mp);
    if (!raw) return NULL;

    json_object *root = json_tokener_parse(raw); g_free(raw);
    if (!root) return NULL;

    InfinitasExtension *e = g_new0(InfinitasExtension, 1);
    e->id              = g_strdup(name);
    e->path            = g_strdup(dir);
    e->content_scripts = g_ptr_array_new_with_free_func(g_free);
    e->content_css     = g_ptr_array_new_with_free_func(g_free);
    e->inject_at       = WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

    json_object *jv;
    if (json_object_object_get_ex(root, "manifest_version", &jv))
        e->manifest_version = json_object_get_int(jv);
    if (json_object_object_get_ex(root, "name",        &jv)) e->name        = g_strdup(json_object_get_string(jv));
    if (json_object_object_get_ex(root, "version",     &jv)) e->version     = g_strdup(json_object_get_string(jv));
    if (json_object_object_get_ex(root, "description", &jv)) e->description = g_strdup(json_object_get_string(jv));

    /* icons */
    json_object *icons_obj;
    if (json_object_object_get_ex(root, "icons", &icons_obj))
        e->icon_path = find_best_icon(dir, icons_obj);

    /* content_scripts */
    json_object *cs_arr;
    if (json_object_object_get_ex(root, "content_scripts", &cs_arr) &&
        json_object_is_type(cs_arr, json_type_array)) {

        int ncs = (int)json_object_array_length(cs_arr);
        for (int i = 0; i < ncs; i++) {
            json_object *cs = json_object_array_get_idx(cs_arr, i);

            /* run_at timing */
            json_object *run_at_jv;
            if (json_object_object_get_ex(cs, "run_at", &run_at_jv)) {
                const gchar *ra = json_object_get_string(run_at_jv);
                if (!g_strcmp0(ra, "document_start"))
                    e->inject_at = WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START;
                else
                    e->inject_at = WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
            }

            /* URL patterns */
            json_object *matches_jv;
            if (!e->url_patterns &&
                json_object_object_get_ex(cs, "matches", &matches_jv))
                e->url_patterns = patterns_from_array(dir, matches_jv);

            /* JS files */
            json_object *js_arr;
            if (json_object_object_get_ex(cs, "js", &js_arr) &&
                json_object_is_type(js_arr, json_type_array)) {
                int njs = (int)json_object_array_length(js_arr);
                for (int j = 0; j < njs; j++) {
                    const gchar *fn = json_object_get_string(json_object_array_get_idx(js_arr, j));
                    gchar *fp = g_build_filename(dir, fn, NULL);
                    gchar *src = read_file(fp); g_free(fp);
                    if (src) g_ptr_array_add(e->content_scripts, src);
                }
            }

            /* CSS files */
            json_object *css_arr;
            if (json_object_object_get_ex(cs, "css", &css_arr) &&
                json_object_is_type(css_arr, json_type_array)) {
                int ncss = (int)json_object_array_length(css_arr);
                for (int j = 0; j < ncss; j++) {
                    const gchar *fn = json_object_get_string(json_object_array_get_idx(css_arr, j));
                    gchar *fp = g_build_filename(dir, fn, NULL);
                    gchar *src = read_file(fp); g_free(fp);
                    if (src) g_ptr_array_add(e->content_css, src);
                }
            }
        }
    }

    /* background script (MV2: background.scripts[], MV3: background.service_worker) */
    json_object *bg;
    if (json_object_object_get_ex(root, "background", &bg)) {
        json_object *sw;
        if (json_object_object_get_ex(bg, "service_worker", &sw)) {
            gchar *bp = g_build_filename(dir, json_object_get_string(sw), NULL);
            e->bg_script = read_file(bp); g_free(bp);
        } else {
            json_object *scripts_arr;
            if (json_object_object_get_ex(bg, "scripts", &scripts_arr) &&
                json_object_is_type(scripts_arr, json_type_array)) {
                GString *combined = g_string_new("");
                int n = (int)json_object_array_length(scripts_arr);
                for (int i = 0; i < n; i++) {
                    const gchar *fn = json_object_get_string(json_object_array_get_idx(scripts_arr, i));
                    gchar *fp = g_build_filename(dir, fn, NULL);
                    gchar *src = read_file(fp); g_free(fp);
                    if (src) { g_string_append(combined, src); g_string_append_c(combined, '\n'); g_free(src); }
                }
                e->bg_script = g_string_free(combined, FALSE);
            }
        }
    }

    /* browser_action (MV2) or action (MV3) */
    json_object *action_obj = NULL;
    if (!json_object_object_get_ex(root, "action", &action_obj))
        json_object_object_get_ex(root, "browser_action", &action_obj);
    if (action_obj) {
        e->has_action = TRUE;
        json_object *title_jv, *popup_jv;
        if (json_object_object_get_ex(action_obj, "default_title", &title_jv))
            e->action_title = g_strdup(json_object_get_string(title_jv));
        if (json_object_object_get_ex(action_obj, "default_popup", &popup_jv)) {
            gchar *pp = g_build_filename(dir, json_object_get_string(popup_jv), NULL);
            e->action_popup = read_file(pp); g_free(pp);
        }
        /* If no icon from top-level, try action's default_icon */
        if (!e->icon_path) {
            json_object *dicon;
            if (json_object_object_get_ex(action_obj, "default_icon", &dicon)) {
                if (json_object_is_type(dicon, json_type_string)) {
                    e->icon_path = g_build_filename(dir, json_object_get_string(dicon), NULL);
                } else if (json_object_is_type(dicon, json_type_object)) {
                    e->icon_path = find_best_icon(dir, dicon);
                }
            }
        }
    }

    json_object_put(root);
    if (!e->name) e->name = g_strdup(name);

    g_print("[EXT] Loaded: %s %s (MV%d) from %s\n",
            e->name, e->version ? e->version : "", e->manifest_version, dir);
    return e;
}

/* ── public API ──────────────────────────────────────────────────────────── */

ExtensionManager* extension_manager_new(const gchar *ext_dir) {
    ExtensionManager *em = g_new0(ExtensionManager, 1);
    em->extensions = g_ptr_array_new_with_free_func(ext_free);
    em->ext_dir    = g_strdup(ext_dir);
    g_mkdir_with_parents(ext_dir, 0755);
    return em;
}

void extension_manager_free(ExtensionManager *em) {
    if (!em) return;
    g_ptr_array_free(em->extensions, TRUE);
    g_free(em->ext_dir);
    g_free(em);
}

void extension_manager_load_all(ExtensionManager *em) {
    GDir *dir = g_dir_open(em->ext_dir, 0, NULL);
    if (!dir) return;
    const gchar *n;
    while ((n = g_dir_read_name(dir))) {
        gchar *path = g_build_filename(em->ext_dir, n, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            InfinitasExtension *e = load_ext(path, n);
            if (e) g_ptr_array_add(em->extensions, e);
        }
        g_free(path);
    }
    g_dir_close(dir);
}

/* ── resource serving ────────────────────────────────────────────────────── */

static GHashTable *g_ext_resource_dirs = NULL; /* id → path */

static gchar* ext_resource_handler(const gchar *subpath, gpointer userdata) {
    (void)userdata;
    /* subpath = "<ext_id>/<file_path>" */
    const gchar *slash = strchr(subpath, '/');
    if (!slash) return NULL;

    gchar *ext_id = g_strndup(subpath, (gsize)(slash - subpath));
    const gchar *file  = slash + 1;

    if (!g_ext_resource_dirs) { g_free(ext_id); return NULL; }

    const gchar *ext_dir = g_hash_table_lookup(g_ext_resource_dirs, ext_id);
    g_free(ext_id);
    if (!ext_dir) return NULL;

    gchar *fpath = g_build_filename(ext_dir, file, NULL);
    gchar *content = read_file(fpath);
    g_free(fpath);
    return content; /* may be NULL — handler returns NULL = not found */
}

void extension_manager_register_resources(ExtensionManager *em) {
    if (!g_ext_resource_dirs)
        g_ext_resource_dirs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      g_free, g_free);
    for (guint i = 0; i < em->extensions->len; i++) {
        InfinitasExtension *e = g_ptr_array_index(em->extensions, i);
        g_hash_table_insert(g_ext_resource_dirs,
                             g_strdup(e->id), g_strdup(e->path));
    }
    /* Register scheme: infinitas://ext/<id>/<path> */
    infinitas_register_plugin_path("ext/", ext_resource_handler, NULL);
}

/* ── inject scripts into a new WebKit view ──────────────────────────────── */

void extension_manager_inject(ExtensionManager *em,
                               WebKitWebView *view,
                               WebKitUserContentManager *mgr) {
    (void)view;

    for (guint i = 0; i < em->extensions->len; i++) {
        InfinitasExtension *e = g_ptr_array_index(em->extensions, i);

        /* Build and inject the chrome.* shim for this extension */
        gchar *shim = g_strdup_printf("%s%s('%s');",
                                       CHROME_SHIM_P1, CHROME_SHIM_P2, e->id);
        WebKitUserScript *shim_script = webkit_user_script_new(
            shim,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            NULL, NULL);
        webkit_user_content_manager_add_script(mgr, shim_script);
        webkit_user_script_unref(shim_script);
        g_free(shim);

        /* Inject CSS */
        for (guint j = 0; j < e->content_css->len; j++) {
            const gchar *src = g_ptr_array_index(e->content_css, j);
            WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new(
                src,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_STYLE_LEVEL_USER,
                (const gchar* const*)e->url_patterns, NULL);
            webkit_user_content_manager_add_style_sheet(mgr, sheet);
            webkit_user_style_sheet_unref(sheet);
        }

        /* Inject JS content scripts */
        for (guint j = 0; j < e->content_scripts->len; j++) {
            const gchar *src = g_ptr_array_index(e->content_scripts, j);
            WebKitUserScript *script = webkit_user_script_new(
                src,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                e->inject_at,
                (const gchar* const*)e->url_patterns, NULL);
            webkit_user_content_manager_add_script(mgr, script);
            webkit_user_script_unref(script);
        }
    }
}

/* ── message handlers ────────────────────────────────────────────────────── */

static void on_navigate_msg(WebKitUserContentManager *m, JSCValue *val,
                             gpointer browser_ptr) {
    (void)m;
    if (!jsc_value_is_object(val)) return;
    JSCValue *u = jsc_value_object_get_property(val, "url");
    if (u && jsc_value_is_string(u)) {
        gchar *url = jsc_value_to_string(u);
        if (url) { browser_navigate((InfinitasBrowser*)browser_ptr, url); g_free(url); }
        g_object_unref(u);
    }
}

static void on_open_tab_msg(WebKitUserContentManager *m, JSCValue *val,
                              gpointer browser_ptr) {
    (void)m;
    if (!jsc_value_is_object(val)) return;
    JSCValue *u = jsc_value_object_get_property(val, "url");
    if (u && jsc_value_is_string(u)) {
        gchar *url = jsc_value_to_string(u);
        if (url) { browser_create_tab((InfinitasBrowser*)browser_ptr, url); g_free(url); }
        g_object_unref(u);
    }
}

static void on_ext_msg(WebKitUserContentManager *m, JSCValue *val,
                        gpointer browser_ptr) {
    (void)m; (void)val; (void)browser_ptr;
}

void extension_manager_register_handlers(ExtensionManager *em,
                                          WebKitUserContentManager *mgr,
                                          gpointer browser) {
    (void)em;
    webkit_user_content_manager_register_script_message_handler(mgr, "infinitas_navigate", NULL);
    g_signal_connect(mgr, "script-message-received::infinitas_navigate",
                     G_CALLBACK(on_navigate_msg), browser);

    webkit_user_content_manager_register_script_message_handler(mgr, "infinitas_open_tab", NULL);
    g_signal_connect(mgr, "script-message-received::infinitas_open_tab",
                     G_CALLBACK(on_open_tab_msg), browser);

    webkit_user_content_manager_register_script_message_handler(mgr, "infinitas_ext_msg", NULL);
    g_signal_connect(mgr, "script-message-received::infinitas_ext_msg",
                     G_CALLBACK(on_ext_msg), browser);
}

/* ── toolbar buttons ─────────────────────────────────────────────────────── */

static void on_ext_btn(GtkButton *btn, gpointer ext_ptr) {
    InfinitasExtension *e = ext_ptr;
    if (!e->action_popup) return;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), e->action_title ? e->action_title : e->name);
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 480);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
    if (GTK_IS_WINDOW(root))
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(root));

    WebKitWebView *view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    /* set base URI so relative URLs in popup resolve to extension dir */
    gchar *base = g_strdup_printf("infinitas://ext/%s/", e->id);
    webkit_web_view_load_html(view, e->action_popup, base);
    g_free(base);

    gtk_window_set_child(GTK_WINDOW(win), GTK_WIDGET(view));
    gtk_window_present(GTK_WINDOW(win));
}

void extension_manager_add_toolbar_buttons(ExtensionManager *em,
                                            GtkWidget *toolbar,
                                            gpointer browser) {
    (void)browser;
    for (guint i = 0; i < em->extensions->len; i++) {
        InfinitasExtension *e = g_ptr_array_index(em->extensions, i);
        if (!e->has_action) continue;

        GtkWidget *btn;
        if (e->icon_path && g_file_test(e->icon_path, G_FILE_TEST_EXISTS)) {
            GdkTexture *tex = gdk_texture_new_from_filename(e->icon_path, NULL);
            if (tex) {
                GtkWidget *img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
                gtk_image_set_pixel_size(GTK_IMAGE(img), 18);
                g_object_unref(tex);
                btn = gtk_button_new();
                gtk_button_set_child(GTK_BUTTON(btn), img);
            } else {
                btn = gtk_button_new_with_label(e->name);
            }
        } else {
            btn = gtk_button_new_with_label(e->name);
        }

        gtk_widget_set_tooltip_text(btn, e->action_title ? e->action_title : e->name);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_ext_btn), e);
        gtk_box_append(GTK_BOX(toolbar), btn);
    }
}
