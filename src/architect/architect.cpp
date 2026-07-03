// architect.cpp - Architect, the LeviathanOS code editor (stage 1).
//
// A tiny IDE built on the same stack as the rest of the Leviathan office
// suite: GTK3 + WebKitGTK 4.1, with an HTML/JS/CSS front-end inside a
// WebKitWebView and a C++ backend. Stage 1 delivers:
//   * editor shell: line numbers, monospace editor, tab bar, file-tree
//   * real file I/O (open folder/file, save, save-as, new) over the
//     WebKit message bridge, with all content crossing base64-encoded
//   * a genuine LSP client (clangd / pylsp) with a live hover round-trip
//
// The C backend never splices raw file text or file names into HTML or
// JS source: every payload crosses the C<->JS bridge base64-encoded and
// is decoded with TextDecoder / atob on the far side, so no byte can
// break the page or inject script.
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <map>
#include <thread>
#include <atomic>
#include <regex>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <pty.h>       // forkpty (link -lutil)

#include "lsp.hpp"
#include "json.hpp"
#include "fuzzy.hpp"
#include "textguard.hpp"

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
// A running PTY-backed shell for the integrated terminal panel.
struct TermState {
    pid_t              pid = -1;
    int                fd  = -1;   // PTY master
    std::thread        reader;
    std::atomic<bool>  running{false};
};

// A color theme adopted from a VSCode extension (.vsix). We do NOT run any
// extension code -- we only read the static theme JSON it contributes and pull
// out the editor background/foreground plus a few token colors.
struct LoadedTheme {
    std::string id;        // stable id (extension::label)
    std::string name;      // display label
    std::string ext;       // owning extension name
    std::string bg, fg;    // editor.background / editor.foreground (#rrggbb)
    std::string kw, str, comment, num, pp; // token colors (may be empty)
};

// A loaded extension bundle (what Architect actually adopted from it).
struct LoadedExtension {
    std::string name;         // directory / package name
    std::string displayName;  // package.json displayName (fallback: name)
    std::string version;
    int themeCount = 0;
    int snippetCount = 0;     // counted for reporting only; not wired to insert
};

struct ArchitectApp {
    GtkWindow*     window   = nullptr;
    WebKitWebView* web_view = nullptr;

    // Extensions adopted from ~/.architect (themes/snippets only).
    std::vector<LoadedTheme>     themes;
    std::vector<LoadedExtension> extensions;
    std::string themeName;    // persisted selected theme id (empty = default)
    std::string extMsg;       // startup note (e.g. "unzip missing") for the status bar

    std::string projectRoot;          // opened folder (LSP root)
    // Concurrent language servers, keyed by server group ("cfamily","python").
    // Each file routes to the server for its language, so C/C++ (clangd) and
    // Python (pylsp) can be active at the same time.
    std::map<std::string, std::unique_ptr<LspClient>> lsps;
    bool webReady = false;

    // Fuzzy file finder / project search: cached recursive list of project
    // files (paths relative to projectRoot). Rebuilt when the folder changes.
    std::vector<std::string> projectFiles;
    std::string lastFormatPath;       // path awaiting a formatting reply

    // Integrated terminal (PTY-backed shell).
    TermState term;

    // Persisted session/settings (see ~/.config/architect/state.json).
    std::vector<std::string> openFiles;   // last session's open file paths
    int  fontSize = 13;                    // editor font size (px)
    int  tabWidth = 4;                     // editor tab width (columns)
    bool formatOnSave = false;             // run LSP formatting before saving
    bool restoredSession = false;          // guard: restore only once
};

// ---------------------------------------------------------------------------
// base64 (encode + decode)
// ---------------------------------------------------------------------------
static std::string b64_encode(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    size_t i = 0;
    while (i < len) {
        unsigned oa = data[i++];
        unsigned ob = i < len ? data[i++] : 0;
        unsigned oc = i < len ? data[i++] : 0;
        unsigned triple = (oa << 16) | (ob << 8) | oc;
        out.push_back(tbl[(triple >> 18) & 0x3F]);
        out.push_back(tbl[(triple >> 12) & 0x3F]);
        out.push_back(tbl[(triple >> 6) & 0x3F]);
        out.push_back(tbl[triple & 0x3F]);
    }
    size_t mod = len % 3;
    if (mod == 1) { out[out.size() - 1] = '='; out[out.size() - 2] = '='; }
    else if (mod == 2) { out[out.size() - 1] = '='; }
    return out;
}
static std::string b64_encode(const std::string& s) {
    return b64_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}
static std::string b64_decode(const std::string& in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((char)((buf >> bits) & 0xFF)); }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JS bridge helpers
// ---------------------------------------------------------------------------
struct IdleJs { WebKitWebView* view; std::string js; };

static gboolean idle_run_js(gpointer data) {
    IdleJs* m = static_cast<IdleJs*>(data);
    webkit_web_view_evaluate_javascript(m->view, m->js.c_str(), -1,
                                        nullptr, nullptr, nullptr, nullptr, nullptr);
    delete m;
    return G_SOURCE_REMOVE;
}

// Thread-safe: schedule JS to run on the GTK main loop.
static void post_js(ArchitectApp* app, const std::string& js) {
    g_idle_add(idle_run_js, new IdleJs{app->web_view, js});
}

// Deliver a JSON message to the front-end __recv() dispatcher. The JSON is
// base64-wrapped so no byte in a file name or path can break out.
static void send_json(ArchitectApp* app, const std::string& jsonBody) {
    std::string js = "__recv('" + b64_encode(jsonBody) + "');";
    post_js(app, js);
}

static void set_status(ArchitectApp* app, const std::string& text) {
    std::string body = "{\"cmd\":\"status\",\"text\":\"" + json::escape(text) + "\"}";
    send_json(app, body);
}

// ---------------------------------------------------------------------------
// Language detection
// ---------------------------------------------------------------------------
static bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           g_ascii_strcasecmp(s.c_str() + s.size() - suf.size(), suf.c_str()) == 0;
}
static std::string lang_for(const std::string& path) {
    if (ends_with(path, ".cpp") || ends_with(path, ".cc") || ends_with(path, ".cxx") ||
        ends_with(path, ".hpp") || ends_with(path, ".hh")  || ends_with(path, ".hxx") ||
        ends_with(path, ".c++"))
        return "cpp";
    if (ends_with(path, ".c")) return "c";
    if (ends_with(path, ".h")) return "c";
    if (ends_with(path, ".py")) return "python";
    return "";
}

// ---------------------------------------------------------------------------
// LSP wiring
// ---------------------------------------------------------------------------
static std::string path_to_uri(const std::string& path) { return "file://" + path; }

// Map a languageId to a server group. C and C++ share one clangd instance;
// Python gets its own pylsp. Empty string => no server for this language.
static std::string lsp_group(const std::string& lang) {
    if (lang == "c" || lang == "cpp") return "cfamily";
    if (lang == "python")             return "python";
    return "";
}

// Ensure a language server for `lang`'s group is running, rooted at the
// project folder, and return it (or nullptr if none is available). Multiple
// groups can run concurrently; each is created once and reused.
static LspClient* ensure_lsp(ArchitectApp* app, const std::string& lang) {
    std::string group = lsp_group(lang);
    if (group.empty()) return nullptr;

    auto it = app->lsps.find(group);
    if (it != app->lsps.end() && it->second && it->second->isRunning())
        return it->second.get();
    if (it != app->lsps.end()) app->lsps.erase(it);  // dead server: recreate

    std::string server = LspClient::detectServer(lang);
    if (server.empty()) {
        set_status(app, "No language server for '" + lang +
                        "' (install clangd or pylsp). Editing still works.");
        return nullptr;
    }

    auto client = std::make_unique<LspClient>();
    LspClient* lsp = client.get();

    // Callbacks fire on the reader thread -> marshal onto the main loop.
    WebKitWebView* view = app->web_view;
    lsp->onHover = [app, view](const std::string& md) {
        std::string body = "{\"cmd\":\"hover\",\"text\":\"" + json::escape(md) + "\"}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    lsp->onStatus = [app, view](const std::string& s) {
        std::string body = "{\"cmd\":\"status\",\"text\":\"" + json::escape(s) + "\"}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    lsp->onCompletion = [app, view](const std::vector<CompletionItem>& items) {
        std::string body = "{\"cmd\":\"completion\",\"items\":[";
        bool first = true;
        for (const auto& it : items) {
            if (!first) body += ",";
            first = false;
            std::string ins = it.insertText.empty() ? it.label : it.insertText;
            body += "{\"label\":\"" + json::escape(it.label) +
                    "\",\"kind\":" + std::to_string(it.kind) +
                    ",\"detail\":\"" + json::escape(it.detail) +
                    "\",\"insert\":\"" + json::escape(ins) + "\"}";
        }
        body += "]}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    lsp->onDiagnostics = [app, view](const std::string& uri,
                                          const std::vector<Diagnostic>& diags) {
        // Servers echo the URI we sent (file://<path>); map it back to a path
        // so the front-end can match diagnostics to the open tab.
        std::string path = uri;
        if (path.rfind("file://", 0) == 0) path = path.substr(7);
        std::string body = "{\"cmd\":\"diagnostics\",\"path\":\"" + json::escape(path) +
                           "\",\"items\":[";
        bool first = true;
        for (const auto& d : diags) {
            if (!first) body += ",";
            first = false;
            body += "{\"line\":" + std::to_string(d.line) +
                    ",\"character\":" + std::to_string(d.character) +
                    ",\"endLine\":" + std::to_string(d.endLine) +
                    ",\"endCharacter\":" + std::to_string(d.endCharacter) +
                    ",\"severity\":" + std::to_string(d.severity) +
                    ",\"message\":\"" + json::escape(d.message) +
                    "\",\"source\":\"" + json::escape(d.source) + "\"}";
        }
        body += "]}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    // uri (file://<path>) -> plain path, for matching front-end tabs.
    auto uri_to_path = [](const std::string& uri) {
        return uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    };
    auto locations_json = [uri_to_path](const std::string& cmd,
                                        const std::vector<Location>& locs) {
        std::string body = "{\"cmd\":\"" + cmd + "\",\"locations\":[";
        bool first = true;
        for (const auto& l : locs) {
            if (!first) body += ",";
            first = false;
            body += "{\"path\":\"" + json::escape(uri_to_path(l.uri)) +
                    "\",\"line\":" + std::to_string(l.line) +
                    ",\"character\":" + std::to_string(l.character) +
                    ",\"endLine\":" + std::to_string(l.endLine) +
                    ",\"endCharacter\":" + std::to_string(l.endCharacter) + "}";
        }
        body += "]}";
        return body;
    };
    lsp->onDefinition = [view, locations_json](const std::vector<Location>& locs) {
        std::string js = "__recv('" + b64_encode(locations_json("definition", locs)) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    lsp->onReferences = [view, locations_json](const std::vector<Location>& locs) {
        std::string js = "__recv('" + b64_encode(locations_json("references", locs)) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    lsp->onSignatureHelp = [view](const std::vector<SignatureInfo>& sigs,
                                       int actS, int actP) {
        std::string body = "{\"cmd\":\"signatureHelp\",\"activeSignature\":" +
                           std::to_string(actS) + ",\"activeParameter\":" +
                           std::to_string(actP) + ",\"signatures\":[";
        bool first = true;
        for (const auto& s : sigs) {
            if (!first) body += ",";
            first = false;
            body += "{\"label\":\"" + json::escape(s.label) +
                    "\",\"documentation\":\"" + json::escape(s.documentation) +
                    "\",\"activeParameter\":" + std::to_string(s.activeParameter) +
                    ",\"parameters\":[";
            bool pf = true;
            for (const auto& p : s.parameters) {
                if (!pf) body += ",";
                pf = false;
                body += "{\"label\":\"" + json::escape(p.label) + "\"}";
            }
            body += "]}";
        }
        body += "]}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    lsp->onRename = [view, uri_to_path](const WorkspaceEdit& we) {
        std::string body = "{\"cmd\":\"rename\",\"changes\":[";
        bool first = true;
        for (const auto& ch : we.changes) {
            if (!first) body += ",";
            first = false;
            body += "{\"path\":\"" + json::escape(uri_to_path(ch.first)) + "\",\"edits\":[";
            bool ef = true;
            for (const auto& e : ch.second) {
                if (!ef) body += ",";
                ef = false;
                body += "{\"line\":" + std::to_string(e.line) +
                        ",\"character\":" + std::to_string(e.character) +
                        ",\"endLine\":" + std::to_string(e.endLine) +
                        ",\"endCharacter\":" + std::to_string(e.endCharacter) +
                        ",\"newText\":\"" + json::escape(e.newText) + "\"}";
            }
            body += "]}";
        }
        body += "]}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };
    // Formatting reply: emit the edits tagged with the path we last asked to
    // format, so the front-end can apply them to the correct tab.
    lsp->onFormatting = [app, view](const std::vector<TextEditOp>& edits) {
        std::string body = "{\"cmd\":\"format\",\"path\":\"" +
                           json::escape(app->lastFormatPath) + "\",\"edits\":[";
        bool first = true;
        for (const auto& e : edits) {
            if (!first) body += ",";
            first = false;
            body += "{\"line\":" + std::to_string(e.line) +
                    ",\"character\":" + std::to_string(e.character) +
                    ",\"endLine\":" + std::to_string(e.endLine) +
                    ",\"endCharacter\":" + std::to_string(e.endCharacter) +
                    ",\"newText\":\"" + json::escape(e.newText) + "\"}";
        }
        body += "]}";
        std::string js = "__recv('" + b64_encode(body) + "');";
        g_idle_add(idle_run_js, new IdleJs{view, js});
    };

    std::string root = app->projectRoot.empty() ? std::string(".") : app->projectRoot;
    if (!lsp->start(server, root)) {
        set_status(app, "Failed to launch language server: " + server);
        return nullptr;
    }
    app->lsps[group] = std::move(client);
    return lsp;
}

// Return the running language server for `path`'s language, or nullptr.
static LspClient* lsp_for_path(ArchitectApp* app, const std::string& path) {
    std::string group = lsp_group(lang_for(path));
    if (group.empty()) return nullptr;
    auto it = app->lsps.find(group);
    if (it != app->lsps.end() && it->second && it->second->isRunning())
        return it->second.get();
    return nullptr;
}

// ---------------------------------------------------------------------------
// File-tree listing
// ---------------------------------------------------------------------------
static std::string list_dir_json(const std::string& path) {
    std::string body = "{\"cmd\":\"dir\",\"path\":\"" + json::escape(path) + "\",\"entries\":[";
    DIR* d = opendir(path.c_str());
    if (d) {
        struct Entry { std::string name, full; bool dir; };
        std::vector<Entry> entries;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            if (!name.empty() && name[0] == '.') continue; // hide dotfiles
            std::string full = path;
            if (!full.empty() && full.back() != '/') full += "/";
            full += name;
            struct stat st;
            bool isDir = (stat(full.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
            entries.push_back({name, full, isDir});
        }
        closedir(d);
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir > b.dir;      // dirs first
            return g_ascii_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        bool first = true;
        for (const auto& e : entries) {
            if (!first) body += ",";
            first = false;
            body += "{\"name\":\"" + json::escape(e.name) +
                    "\",\"path\":\"" + json::escape(e.full) +
                    "\",\"dir\":" + (e.dir ? "true" : "false") + "}";
        }
    }
    body += "]}";
    return body;
}

// ---------------------------------------------------------------------------
// File read / write
// ---------------------------------------------------------------------------
static bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
static bool write_file(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Settings / session persistence (~/.config/architect/state.json)
// ---------------------------------------------------------------------------
static std::string config_dir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0]) base = xdg;
    else {
        const char* home = getenv("HOME");
        base = home ? std::string(home) + "/.config" : std::string(".");
    }
    return base + "/architect";
}
static std::string config_path() { return config_dir() + "/state.json"; }

static void load_config(ArchitectApp* app) {
    std::string raw;
    if (!read_file(config_path(), raw)) return;
    json::Value root;
    if (!json::parse(raw, root) || !root.isObject()) return;
    if (const json::Value* r = root.find("root")) app->projectRoot = r->asString();
    if (const json::Value* f = root.find("fontSize")) {
        int v = (int)f->asInt(); if (v >= 8 && v <= 32) app->fontSize = v;
    }
    if (const json::Value* t = root.find("tabWidth")) {
        int v = (int)t->asInt(); if (v >= 1 && v <= 8) app->tabWidth = v;
    }
    if (const json::Value* f = root.find("formatOnSave"))
        app->formatOnSave = (f->type == json::Type::Bool) ? f->boolv : (f->asInt() != 0);
    if (const json::Value* th = root.find("theme")) app->themeName = th->asString();
    if (const json::Value* of = root.find("openFiles")) {
        if (of->isArray())
            for (const auto& e : of->arr)
                if (e.isString() && !e.str.empty()) app->openFiles.push_back(e.str);
    }
}

// Ensure the config directory exists (mkdir -p on the two-level path).
static void ensure_config_dir() {
    std::string dir = config_dir();
    // create parent (~/.config) then the leaf.
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos) {
        std::string parent = dir.substr(0, slash);
        if (!parent.empty()) mkdir(parent.c_str(), 0755);
    }
    mkdir(dir.c_str(), 0755);
}

static void save_config(ArchitectApp* app) {
    ensure_config_dir();
    std::string body = "{\"root\":\"" + json::escape(app->projectRoot) +
                       "\",\"fontSize\":" + std::to_string(app->fontSize) +
                       ",\"tabWidth\":" + std::to_string(app->tabWidth) +
                       ",\"formatOnSave\":" + (app->formatOnSave ? "true" : "false") +
                       ",\"theme\":\"" + json::escape(app->themeName) + "\"" +
                       ",\"openFiles\":[";
    bool first = true;
    for (const auto& p : app->openFiles) {
        if (!first) body += ",";
        first = false;
        body += "\"" + json::escape(p) + "\"";
    }
    body += "]}";
    write_file(config_path(), body);
}

// ---------------------------------------------------------------------------
// Extensions from ~/.architect (adopt VSCode color themes + snippets ONLY)
// ---------------------------------------------------------------------------
// A .vsix is just a zip; a .tar/.tar.gz is a tarball. On startup we extract each
// into ~/.architect/extensions/<name>/ (shelling out to the standard `unzip` /
// `tar`) and read only the STATIC things Architect can honestly use: the color
// themes a package contributes (editor bg/fg + a few token colors) and a count
// of snippet contributions. We do NOT run any extension JavaScript -- there is
// no VSCode extension host here, and we never claim there is.
static std::string architect_dir() {
    const char* home = getenv("HOME");
    return (home && home[0]) ? std::string(home) + "/.architect"
                             : std::string(".architect");
}

static bool have_cmd(const char* name) {
    std::string c = "command -v " + std::string(name) + " >/dev/null 2>&1";
    return system(c.c_str()) == 0;
}

// Single-quote a string for safe use in a /bin/sh command line.
static std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out.push_back(c); }
    out += "'";
    return out;
}

// Strip // and /* */ comments and trailing commas from JSONC. VSCode theme and
// package files are JSON-with-comments, which our strict parser would reject.
// String contents are preserved verbatim (comments/commas inside strings kept).
static std::string sanitize_jsonc(const std::string& in) {
    std::string a; a.reserve(in.size());
    bool inStr = false; char q = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (inStr) {
            a.push_back(c);
            if (c == '\\' && i + 1 < in.size()) { a.push_back(in[++i]); continue; }
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; q = c; a.push_back(c); continue; }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            i += 2; while (i < in.size() && in[i] != '\n') i++;
            if (i < in.size()) a.push_back('\n');
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2; while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) i++;
            i += 1; continue;
        }
        a.push_back(c);
    }
    // Remove trailing commas ( , before } or ] ), still respecting strings.
    std::string b; b.reserve(a.size()); inStr = false; q = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (inStr) {
            b.push_back(c);
            if (c == '\\' && i + 1 < a.size()) { b.push_back(a[++i]); continue; }
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; q = c; b.push_back(c); continue; }
        if (c == ',') {
            size_t j = i + 1;
            while (j < a.size() && (a[j] == ' ' || a[j] == '\t' ||
                                    a[j] == '\n' || a[j] == '\r')) j++;
            if (j < a.size() && (a[j] == '}' || a[j] == ']')) continue; // drop it
        }
        b.push_back(c);
    }
    return b;
}

// Pick the first non-empty color for any of `cands` from a scope->color map,
// trying exact keys first, then a scope-prefix match (VSCode scopes nest, e.g.
// "keyword.control" is a kind of "keyword").
static std::string pick_scope(const std::map<std::string, std::string>& m,
                              std::initializer_list<const char*> cands) {
    for (const char* c : cands) {
        auto it = m.find(c);
        if (it != m.end() && !it->second.empty()) return it->second;
    }
    for (const char* c : cands) {
        std::string cs = c;
        for (const auto& kv : m)
            if (!kv.second.empty() && kv.first.rfind(cs, 0) == 0) return kv.second;
    }
    return "";
}

// Locate the package.json manifest inside an extracted extension dir.
static std::string find_package_json(const std::string& dest) {
    const char* pref[] = {"/extension/package.json", "/package.json"};
    for (const char* p : pref) {
        std::string c = dest + p;
        struct stat st;
        if (stat(c.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return c;
    }
    std::function<std::string(const std::string&, int)> rec =
        [&](const std::string& d, int depth) -> std::string {
        if (depth > 3) return "";
        DIR* dd = opendir(d.c_str());
        if (!dd) return "";
        std::vector<std::string> subs;
        std::string found;
        struct dirent* e;
        while ((e = readdir(dd)) != nullptr) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string full = d + "/" + n;
            struct stat st;
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISREG(st.st_mode) && n == "package.json") { found = full; break; }
            if (S_ISDIR(st.st_mode)) subs.push_back(full);
        }
        closedir(dd);
        if (!found.empty()) return found;
        for (const auto& s : subs) {
            std::string r = rec(s, depth + 1);
            if (!r.empty()) return r;
        }
        return "";
    };
    return rec(dest, 0);
}

// Read one contributed theme file and distill it to a LoadedTheme.
static bool load_theme_file(const std::string& base, std::string rel,
                            const std::string& label, const std::string& extName,
                            LoadedTheme& out) {
    if (rel.rfind("./", 0) == 0) rel = rel.substr(2);
    std::string path = base + "/" + rel;
    std::string raw;
    if (!read_file(path, raw)) return false;
    json::Value th;
    if (!json::parse(sanitize_jsonc(raw), th) || !th.isObject()) return false;

    out.id = extName + "::" + label;
    out.name = label;
    out.ext = extName;
    if (const json::Value* colors = th.find("colors")) {
        if (colors->isObject()) {
            if (auto* v = colors->find("editor.background")) out.bg = v->asString();
            if (auto* v = colors->find("editor.foreground")) out.fg = v->asString();
        }
    }
    std::map<std::string, std::string> scopeMap;
    if (const json::Value* tc = th.find("tokenColors")) {
        if (tc->isArray()) {
            for (const auto& e : tc->arr) {
                if (!e.isObject()) continue;
                const json::Value* set = e.find("settings");
                if (!set || !set->isObject()) continue;
                const json::Value* fg = set->find("foreground");
                if (!fg || !fg->isString()) continue;
                std::string color = fg->asString();
                const json::Value* sc = e.find("scope");
                if (!sc) continue;
                auto add = [&](std::string s) {
                    size_t x = s.find_first_not_of(" \t\n\r");
                    size_t y = s.find_last_not_of(" \t\n\r");
                    if (x == std::string::npos) return;
                    s = s.substr(x, y - x + 1);
                    if (!s.empty() && scopeMap.find(s) == scopeMap.end())
                        scopeMap[s] = color;
                };
                if (sc->isString()) {
                    std::string s = sc->asString();  // may be a comma-separated list
                    size_t p = 0;
                    while (true) {
                        size_t c = s.find(',', p);
                        add(s.substr(p, c == std::string::npos ? std::string::npos : c - p));
                        if (c == std::string::npos) break;
                        p = c + 1;
                    }
                } else if (sc->isArray()) {
                    for (const auto& se : sc->arr)
                        if (se.isString()) add(se.str);
                }
            }
        }
    }
    out.kw      = pick_scope(scopeMap, {"keyword", "storage.type", "storage.modifier", "storage"});
    out.str     = pick_scope(scopeMap, {"string", "string.quoted", "string.quoted.double"});
    out.comment = pick_scope(scopeMap, {"comment", "comment.line", "comment.block"});
    out.num     = pick_scope(scopeMap, {"constant.numeric", "constant"});
    out.pp      = pick_scope(scopeMap, {"meta.preprocessor", "keyword.control.directive",
                                        "keyword.control.import", "entity.name.function.preprocessor"});
    // Usable only if it carries at least a background/foreground or a token color.
    return !(out.bg.empty() && out.fg.empty() && out.kw.empty() &&
             out.str.empty() && out.comment.empty() && out.num.empty() && out.pp.empty());
}

// Parse one extracted extension dir: its package.json + contributed themes.
static void load_extension_dir(ArchitectApp* app, const std::string& name,
                               const std::string& dest) {
    std::string pkgPath = find_package_json(dest);
    if (pkgPath.empty()) return;
    std::string raw;
    if (!read_file(pkgPath, raw)) return;
    json::Value pkg;
    if (!json::parse(sanitize_jsonc(raw), pkg) || !pkg.isObject()) return;
    std::string base = pkgPath.substr(0, pkgPath.find_last_of('/'));

    LoadedExtension ex;
    ex.name = name;
    if (auto* v = pkg.find("displayName")) ex.displayName = v->asString();
    if (ex.displayName.empty()) if (auto* v = pkg.find("name")) ex.displayName = v->asString();
    if (ex.displayName.empty()) ex.displayName = name;
    if (auto* v = pkg.find("version")) ex.version = v->asString();

    const json::Value* contrib = pkg.find("contributes");
    if (contrib && contrib->isObject()) {
        if (const json::Value* th = contrib->find("themes")) {
            if (th->isArray()) {
                for (const auto& t : th->arr) {
                    if (!t.isObject()) continue;
                    std::string label, rel;
                    if (auto* l = t.find("label")) label = l->asString();
                    if (auto* p = t.find("path"))  rel = p->asString();
                    if (rel.empty()) continue;
                    if (label.empty()) label = name;
                    LoadedTheme lt;
                    if (load_theme_file(base, rel, label, name, lt)) {
                        app->themes.push_back(lt);
                        ex.themeCount++;
                    }
                }
            }
        }
        if (const json::Value* sn = contrib->find("snippets"))
            if (sn->isArray()) ex.snippetCount = (int)sn->arr.size();
    }
    app->extensions.push_back(ex);
}

// Strip a known archive suffix to derive the extension directory name.
static std::string strip_archive_suffix(const std::string& n) {
    const char* sufs[] = {".tar.gz", ".vsix", ".tgz", ".tar", nullptr};
    for (int i = 0; sufs[i]; ++i)
        if (ends_with(n, sufs[i])) return n.substr(0, n.size() - strlen(sufs[i]));
    return n;
}

// Scan ~/.architect for *.vsix / *.tar / *.tar.gz, extract each once into
// extensions/<name>/, and load the themes it contributes.
static void scan_extensions(ArchitectApp* app) {
    std::string dir = architect_dir();
    mkdir(dir.c_str(), 0755);                       // create ~/.architect if missing
    std::string extDir = dir + "/extensions";
    mkdir(extDir.c_str(), 0755);

    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> archives;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string n = de->d_name;
        if (n == "." || n == ".." || n == "extensions") continue;
        if (ends_with(n, ".vsix") || ends_with(n, ".tar") ||
            ends_with(n, ".tar.gz") || ends_with(n, ".tgz"))
            archives.push_back(n);
    }
    closedir(d);
    if (archives.empty()) return;

    bool haveUnzip = have_cmd("unzip");
    bool haveTar   = have_cmd("tar");
    std::sort(archives.begin(), archives.end());
    int missing = 0;

    for (const auto& a : archives) {
        std::string base = strip_archive_suffix(a);
        std::string full = dir + "/" + a;
        std::string dest = extDir + "/" + base;

        // Extract only if not already extracted (fast subsequent launches).
        if (find_package_json(dest).empty()) {
            mkdir(dest.c_str(), 0755);
            std::string cmd;
            if (ends_with(a, ".vsix")) {
                if (!haveUnzip) { missing++; continue; }
                cmd = "unzip -o -q " + sh_quote(full) + " -d " + sh_quote(dest);
            } else if (ends_with(a, ".tar")) {
                if (!haveTar) { missing++; continue; }
                cmd = "tar xf " + sh_quote(full) + " -C " + sh_quote(dest);
            } else {  // .tar.gz / .tgz
                if (!haveTar) { missing++; continue; }
                cmd = "tar xzf " + sh_quote(full) + " -C " + sh_quote(dest);
            }
            cmd += " >/dev/null 2>&1";
            if (system(cmd.c_str()) != 0) { missing++; continue; }
        }
        load_extension_dir(app, base, dest);
    }

    if (missing > 0 && (!haveUnzip || !haveTar))
        app->extMsg = "Some extensions skipped: install 'unzip'/'tar' to load them.";
    else if (!app->extensions.empty())
        app->extMsg = "Loaded " + std::to_string(app->extensions.size()) +
                      " extension(s), " + std::to_string(app->themes.size()) + " theme(s).";
}

// Push the loaded theme/extension lists to the front-end, then re-apply the
// persisted theme selection (if any).
static void send_extensions(ArchitectApp* app) {
    std::string tb = "{\"cmd\":\"themes\",\"items\":[";
    bool first = true;
    for (const auto& t : app->themes) {
        if (!first) tb += ",";
        first = false;
        tb += "{\"id\":\"" + json::escape(t.id) + "\",\"name\":\"" + json::escape(t.name) +
              "\",\"ext\":\"" + json::escape(t.ext) + "\",\"bg\":\"" + json::escape(t.bg) +
              "\",\"fg\":\"" + json::escape(t.fg) + "\",\"kw\":\"" + json::escape(t.kw) +
              "\",\"str\":\"" + json::escape(t.str) + "\",\"cm\":\"" + json::escape(t.comment) +
              "\",\"num\":\"" + json::escape(t.num) + "\",\"pp\":\"" + json::escape(t.pp) + "\"}";
    }
    tb += "]}";
    send_json(app, tb);

    std::string eb = "{\"cmd\":\"extensions\",\"items\":[";
    first = true;
    for (const auto& e : app->extensions) {
        if (!first) eb += ",";
        first = false;
        eb += "{\"name\":\"" + json::escape(e.name) + "\",\"display\":\"" +
              json::escape(e.displayName) + "\",\"version\":\"" + json::escape(e.version) +
              "\",\"themes\":" + std::to_string(e.themeCount) +
              ",\"snippets\":" + std::to_string(e.snippetCount) + "}";
    }
    eb += "]}";
    send_json(app, eb);

    if (!app->themeName.empty())
        send_json(app, "{\"cmd\":\"applyTheme\",\"id\":\"" + json::escape(app->themeName) + "\"}");
    if (!app->extMsg.empty()) set_status(app, app->extMsg);
}

// Open a file into a tab and notify the LSP.
//
// Binary / huge-file guard: before streaming a whole file into the editor we
// stat its size and sniff only its head. A binary blob (NUL / garbled bytes) or
// anything larger than textguard::kMaxEditBytes is NEVER loaded into the
// textarea (that lags/hangs the app and shows garbage); instead we open a small,
// non-editable tab carrying only a one-line message. The decision is made here
// in the backend so gigabytes never cross the C<->JS bridge, and every open path
// (file dialog, file-tree click, Ctrl+P fuzzy-open, session restore) funnels
// through this one function so all of them are guarded.
static void open_file_into_tab(ArchitectApp* app, const std::string& path) {
    std::string name = path;
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string lang = lang_for(path);

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        set_status(app, "Open failed: " + path);
        return;
    }
    unsigned long long size = (unsigned long long)st.st_size;

    // Cheap head sniff (kSniffBytes) for the binary check.
    std::string head;
    if (FILE* hf = fopen(path.c_str(), "rb")) {
        textguard::sniff(hf, head);
        fclose(hf);
    }
    bool binary = textguard::looksBinary(head);
    bool tooBig = size > textguard::kMaxEditBytes;

    if (binary || tooBig) {
        // Emit a read-only tab holding only a short message. No file bytes cross
        // the bridge, and the LSP is not told about the file.
        std::string note = binary
            ? "\xE2\xAC\xA2 Binary file \xE2\x80\x94 not shown in the editor ("
              + textguard::humanSize(size) + ")"
            : "\xE2\xAC\xA2 File too large to edit (" + textguard::humanSize(size) + ")";
        std::string body = "{\"cmd\":\"file\",\"path\":\"" + json::escape(path) +
                           "\",\"name\":\"" + json::escape(name) +
                           "\",\"lang\":\"\",\"readonly\":true,\"content\":\"" +
                           b64_encode(note) + "\"}";
        send_json(app, body);
        set_status(app, (binary ? "Not opened (binary): " : "Not opened (too large): ") + name);
        return;
    }

    std::string content;
    if (!read_file(path, content)) {
        set_status(app, "Open failed: " + path);
        return;
    }

    std::string body = "{\"cmd\":\"file\",\"path\":\"" + json::escape(path) +
                       "\",\"name\":\"" + json::escape(name) +
                       "\",\"lang\":\"" + json::escape(lang) +
                       "\",\"content\":\"" + b64_encode(content) + "\"}";
    send_json(app, body);

    if (LspClient* lsp = ensure_lsp(app, lang))
        lsp->didOpen(path_to_uri(path), lang, content);
}

// ---------------------------------------------------------------------------
// GTK dialogs
// ---------------------------------------------------------------------------
static void open_folder_dialog(ArchitectApp* app) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open Project Folder", app->window,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (folder) {
            app->projectRoot = folder;
            app->projectFiles.clear();   // invalidate finder/search cache
            std::string name = folder;
            size_t slash = name.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < name.size())
                name = name.substr(slash + 1);
            std::string body = "{\"cmd\":\"folder\",\"root\":\"" + json::escape(app->projectRoot) +
                               "\",\"name\":\"" + json::escape(name) + "\"}";
            send_json(app, body);
            send_json(app, list_dir_json(app->projectRoot));
            g_free(folder);
        }
    }
    gtk_widget_destroy(dialog);
}

static void open_file_dialog(ArchitectApp* app) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open File", app->window, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (file) { open_file_into_tab(app, file); g_free(file); }
    }
    gtk_widget_destroy(dialog);
}

// Save-as: returns chosen path (empty if cancelled) and writes `data`.
static void save_as_dialog(ArchitectApp* app, const std::string& data,
                           const std::string& suggestedName) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Save File As", app->window, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),
        suggestedName.empty() ? "untitled.txt" : suggestedName.c_str());
    if (!app->projectRoot.empty())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                            app->projectRoot.c_str());
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (file) {
            std::string path = file;
            g_free(file);
            if (write_file(path, data)) {
                std::string name = path;
                size_t slash = name.find_last_of('/');
                if (slash != std::string::npos) name = name.substr(slash + 1);
                std::string body = "{\"cmd\":\"saved\",\"path\":\"" + json::escape(path) +
                                   "\",\"name\":\"" + json::escape(name) + "\",\"isNew\":true}";
                send_json(app, body);
                std::string lang = lang_for(path);
                if (LspClient* lsp = ensure_lsp(app, lang))
                    lsp->didOpen(path_to_uri(path), lang, data);
            } else {
                set_status(app, "Save failed: " + path);
            }
        }
    }
    gtk_widget_destroy(dialog);
}

// ---------------------------------------------------------------------------
// Project file enumeration (fuzzy finder + project-wide search)
// ---------------------------------------------------------------------------
static bool is_ignored_dir(const std::string& name) {
    static const char* kIgnore[] = {
        ".git", ".hg", ".svn", "node_modules", "build", "dist", "target",
        "out", "bin", "obj", ".cache", "__pycache__", ".idea", ".vscode",
        "vendor", ".venv", "venv", "ucfs", nullptr};
    for (int i = 0; kIgnore[i]; ++i)
        if (name == kIgnore[i]) return true;
    return false;
}

// Recursively collect regular files under `base`, as paths relative to `base`,
// skipping hidden entries and the ignore-listed directories. Bounded so a huge
// tree can never wedge the UI.
static void walk_project(const std::string& base, const std::string& rel,
                         std::vector<std::string>& out) {
    if (out.size() >= 30000) return;
    std::string full = rel.empty() ? base : base + "/" + rel;
    DIR* d = opendir(full.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (!name.empty() && name[0] == '.') continue;
        std::string childRel = rel.empty() ? name : rel + "/" + name;
        std::string childFull = base + "/" + childRel;
        struct stat st;
        if (lstat(childFull.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!is_ignored_dir(name)) subdirs.push_back(childRel);
        } else if (S_ISREG(st.st_mode)) {
            out.push_back(childRel);
        }
    }
    closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& s : subdirs) walk_project(base, s, out);
}

// Populate app->projectFiles from disk if not already cached.
static void ensure_project_files(ArchitectApp* app) {
    if (!app->projectFiles.empty() || app->projectRoot.empty()) return;
    walk_project(app->projectRoot, "", app->projectFiles);
}

// ---------------------------------------------------------------------------
// Integrated terminal: a PTY-backed shell bridged into the WebView.
// ---------------------------------------------------------------------------
// Push a JSON message from a worker thread onto the GTK main loop.
static void post_json_threadsafe(ArchitectApp* app, const std::string& body) {
    std::string js = "__recv('" + b64_encode(body) + "');";
    g_idle_add(idle_run_js, new IdleJs{app->web_view, js});
}

static void term_reader_loop(ArchitectApp* app) {
    char buf[8192];
    while (app->term.running.load()) {
        ssize_t n = read(app->term.fd, buf, sizeof(buf));
        if (n > 0) {
            std::string data(buf, (size_t)n);
            post_json_threadsafe(app,
                "{\"cmd\":\"termout\",\"data\":\"" + b64_encode(data) + "\"}");
        } else if (n == 0) {
            break;               // shell exited, PTY closed
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    app->term.running.store(false);
    post_json_threadsafe(app, "{\"cmd\":\"termexit\"}");
}

static void term_start(ArchitectApp* app) {
    if (app->term.running.load()) return;
    if (app->term.reader.joinable()) app->term.reader.join();  // reap prior run

    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0) { set_status(app, "Terminal: forkpty failed"); return; }

    if (pid == 0) {
        // Child: become an interactive login-ish shell in the project folder.
        const char* sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/bash";
        setenv("TERM", "xterm-256color", 1);
        if (!app->projectRoot.empty())
            if (chdir(app->projectRoot.c_str()) != 0) { /* ignore */ }
        execl(sh, sh, "-i", (char*)nullptr);
        _exit(127);
    }

    app->term.fd = master;
    app->term.pid = pid;
    app->term.running.store(true);
    app->term.reader = std::thread(term_reader_loop, app);
    set_status(app, "Terminal ready");
}

static void term_stop(ArchitectApp* app) {
    bool wasRunning = app->term.running.exchange(false);
    if (app->term.fd >= 0) { close(app->term.fd); app->term.fd = -1; }
    if (app->term.pid > 0) {
        kill(app->term.pid, SIGHUP);
        waitpid(app->term.pid, nullptr, 0);
        app->term.pid = -1;
    }
    if (app->term.reader.joinable()) app->term.reader.join();
    (void)wasRunning;
}

// ---------------------------------------------------------------------------
// Message handlers (JS -> C)
// ---------------------------------------------------------------------------
static std::string jsc_str(JSCValue* v) {
    if (!v || !jsc_value_is_string(v)) return "";
    char* s = jsc_value_to_string(v);
    std::string out = s ? s : "";
    g_free(s);
    return out;
}
static std::string obj_str(JSCValue* obj, const char* prop) {
    JSCValue* v = jsc_value_object_get_property(obj, prop);
    std::string out = jsc_str(v);
    g_object_unref(v);
    return out;
}
static int obj_int(JSCValue* obj, const char* prop) {
    JSCValue* v = jsc_value_object_get_property(obj, prop);
    int out = jsc_value_is_number(v) ? (int)jsc_value_to_int32(v) : 0;
    g_object_unref(v);
    return out;
}

static void on_openFolder(WebKitUserContentManager*, WebKitJavascriptResult*, gpointer p) {
    open_folder_dialog(static_cast<ArchitectApp*>(p));
}
static void on_openFile(WebKitUserContentManager*, WebKitJavascriptResult*, gpointer p) {
    open_file_dialog(static_cast<ArchitectApp*>(p));
}
static void on_listDir(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    std::string path = jsc_str(webkit_javascript_result_get_js_value(r));
    if (!path.empty()) send_json(app, list_dir_json(path));
}
static void on_openPath(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    std::string path = jsc_str(webkit_javascript_result_get_js_value(r));
    if (!path.empty()) open_file_into_tab(app, path);
}
static void on_save(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string path = obj_str(v, "path");
    std::string data = b64_decode(obj_str(v, "content"));
    std::string name = obj_str(v, "name");
    if (path.empty()) { save_as_dialog(app, data, name); return; }
    if (write_file(path, data)) {
        std::string body = "{\"cmd\":\"saved\",\"path\":\"" + json::escape(path) + "\"}";
        send_json(app, body);
        if (LspClient* lsp = ensure_lsp(app, lang_for(path)))
            lsp->didChange(path_to_uri(path), data);
    } else {
        set_status(app, "Save failed: " + path);
    }
}
static void on_saveAs(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string data = b64_decode(obj_str(v, "content"));
    std::string name = obj_str(v, "name");
    save_as_dialog(app, data, name);
}
static void on_change(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string path = obj_str(v, "path");
    std::string data = b64_decode(obj_str(v, "content"));
    if (path.empty()) return;
    if (LspClient* lsp = lsp_for_path(app, path))
        lsp->didChange(path_to_uri(path), data);
}
static void on_complete(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string path = obj_str(v, "path");
    int line = obj_int(v, "line");
    int character = obj_int(v, "character");
    LspClient* lsp = lsp_for_path(app, path);
    if (!lsp) {
        // No server: reply with an empty list so the popup just stays closed.
        send_json(app, "{\"cmd\":\"completion\",\"items\":[]}");
        return;
    }
    lsp->completion(path_to_uri(path), line, character);
}
static void on_hover(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string path = obj_str(v, "path");
    int line = obj_int(v, "line");
    int character = obj_int(v, "character");
    if (lang_for(path).empty()) { set_status(app, "Hover: not a code file"); return; }
    LspClient* lsp = lsp_for_path(app, path);
    if (!lsp) {
        set_status(app, "Hover: no language server running");
        send_json(app, "{\"cmd\":\"hover\",\"text\":\"\"}");
        return;
    }
    lsp->hover(path_to_uri(path), line, character);
}
// Shared guard: extract path/line/character and return the LSP client that
// should service the request (or nullptr if none is running for that file).
static LspClient* lsp_ready_at(ArchitectApp* app, JSCValue* v, std::string& path,
                               int& line, int& character) {
    if (!jsc_value_is_object(v)) return nullptr;
    path = obj_str(v, "path");
    line = obj_int(v, "line");
    character = obj_int(v, "character");
    return lsp_for_path(app, path);
}

static void on_definition(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    std::string path; int line, ch;
    LspClient* lsp = lsp_ready_at(app, v, path, line, ch);
    if (!lsp) {
        set_status(app, "Definition: no language server");
        send_json(app, "{\"cmd\":\"definition\",\"locations\":[]}");
        return;
    }
    lsp->definition(path_to_uri(path), line, ch);
}
static void on_references(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    std::string path; int line, ch;
    LspClient* lsp = lsp_ready_at(app, v, path, line, ch);
    if (!lsp) {
        set_status(app, "References: no language server");
        send_json(app, "{\"cmd\":\"references\",\"locations\":[]}");
        return;
    }
    lsp->references(path_to_uri(path), line, ch);
}
static void on_signatureHelp(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    std::string path; int line, ch;
    LspClient* lsp = lsp_ready_at(app, v, path, line, ch);
    if (!lsp) {
        send_json(app, "{\"cmd\":\"signatureHelp\",\"signatures\":[]}");
        return;
    }
    lsp->signatureHelp(path_to_uri(path), line, ch);
}
static void on_rename(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    std::string path; int line, ch;
    LspClient* lsp = lsp_ready_at(app, v, path, line, ch);
    if (!lsp) {
        set_status(app, "Rename: no language server");
        send_json(app, "{\"cmd\":\"rename\",\"changes\":[]}");
        return;
    }
    std::string newName = b64_decode(obj_str(v, "newName"));
    if (newName.empty()) return;
    lsp->rename(path_to_uri(path), line, ch, newName);
}
// Format the current document via textDocument/formatting. The reply is
// tagged with `lastFormatPath` so the front-end applies edits to the tab.
static void on_format(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string path = obj_str(v, "path");
    LspClient* lsp = lsp_for_path(app, path);
    if (!lsp) {
        set_status(app, "Format: no language server for this file");
        send_json(app, "{\"cmd\":\"format\",\"path\":\"" + json::escape(path) +
                       "\",\"edits\":[]}");
        return;
    }
    app->lastFormatPath = path;
    lsp->formatting(path_to_uri(path), app->tabWidth, true);
}
// Incremental didChange: front-end sends the replaced range + new text.
static void on_changeRange(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string path = obj_str(v, "path");
    if (path.empty()) return;
    LspClient* lsp = lsp_for_path(app, path);
    if (!lsp) return;
    int sl = obj_int(v, "startLine"), sc = obj_int(v, "startChar");
    int el = obj_int(v, "endLine"), ec = obj_int(v, "endChar");
    std::string text = b64_decode(obj_str(v, "text"));
    lsp->didChangeRange(path_to_uri(path), sl, sc, el, ec, text);
}
// Persist session + settings. Files arrive as base64 of newline-joined paths.
static void on_saveState(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    int fs = obj_int(v, "fontSize"); if (fs >= 8 && fs <= 32) app->fontSize = fs;
    int tw = obj_int(v, "tabWidth"); if (tw >= 1 && tw <= 8) app->tabWidth = tw;
    app->formatOnSave = obj_int(v, "formatOnSave") != 0;
    app->themeName = b64_decode(obj_str(v, "theme"));  // "" = default theme
    std::string joined = b64_decode(obj_str(v, "files"));
    app->openFiles.clear();
    size_t start = 0;
    while (start <= joined.size()) {
        size_t nl = joined.find('\n', start);
        std::string one = joined.substr(start, nl == std::string::npos
                                                   ? std::string::npos : nl - start);
        if (!one.empty()) app->openFiles.push_back(one);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    save_config(app);
}
static void on_log(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer) {
    std::string s = jsc_str(webkit_javascript_result_get_js_value(r));
    g_print("[architect/js] %s\n", s.c_str());
}

// Fuzzy file finder (Ctrl+P): rank the cached project file list against the
// query with fuzzy::match and return the best matches (full + relative path).
static void on_fuzzyFind(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string q = b64_decode(obj_str(v, "q"));
    ensure_project_files(app);

    std::vector<std::pair<int, const std::string*>> scored;
    scored.reserve(app->projectFiles.size());
    for (const auto& rel : app->projectFiles) {
        int s = fuzzy::match(q, rel);
        if (s >= 0) scored.push_back({s, &rel});
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second->size() < b.second->size();
              });

    std::string body = "{\"cmd\":\"fuzzy\",\"items\":[";
    bool first = true;
    size_t limit = 300;
    for (const auto& sc : scored) {
        if (limit-- == 0) break;
        if (!first) body += ",";
        first = false;
        std::string full = app->projectRoot + "/" + *sc.second;
        body += "{\"rel\":\"" + json::escape(*sc.second) +
                "\",\"path\":\"" + json::escape(full) + "\"}";
    }
    body += "]}";
    send_json(app, body);
}

// Project-wide search (Ctrl+Shift+F): walk the cached file list and collect
// matches (plain substring or regex), grouped per file with line:col.
static void on_projectSearch(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string q = b64_decode(obj_str(v, "q"));
    bool useRegex = obj_int(v, "regex") != 0;
    bool matchCase = obj_int(v, "case") != 0;
    if (q.empty()) { send_json(app, "{\"cmd\":\"search\",\"items\":[]}"); return; }
    ensure_project_files(app);

    std::regex re;
    bool reOk = false;
    if (useRegex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!matchCase) flags |= std::regex::icase;
            re = std::regex(q, flags);
            reOk = true;
        } catch (...) {
            send_json(app, "{\"cmd\":\"search\",\"items\":[],\"error\":\"bad regex\"}");
            return;
        }
    }
    std::string needle = q;
    if (!matchCase && !useRegex)
        for (char& c : needle) c = (char)tolower((unsigned char)c);

    std::string body = "{\"cmd\":\"search\",\"items\":[";
    bool firstFile = true;
    int totalMatches = 0;
    const int kMaxMatches = 2000;

    for (const auto& rel : app->projectFiles) {
        if (totalMatches >= kMaxMatches) break;
        std::string full = app->projectRoot + "/" + rel;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || st.st_size > 4 * 1024 * 1024) continue;
        std::string content;
        if (!read_file(full, content)) continue;
        if (content.find('\0') != std::string::npos) continue;  // skip binary

        std::string fileBody;
        bool firstHit = true;
        int lineNo = 0;
        size_t pos = 0;
        while (pos <= content.size() && totalMatches < kMaxMatches) {
            size_t nl = content.find('\n', pos);
            std::string line = content.substr(pos, nl == std::string::npos
                                                       ? std::string::npos : nl - pos);
            // Find matches within this line.
            auto addHit = [&](int col, size_t /*len*/) {
                if (!firstHit) fileBody += ",";
                firstHit = false;
                std::string disp = line;
                if (disp.size() > 200) disp = disp.substr(0, 200);
                fileBody += "{\"line\":" + std::to_string(lineNo) +
                            ",\"col\":" + std::to_string(col) +
                            ",\"text\":\"" + b64_encode(disp) + "\"}";
                totalMatches++;
            };
            if (useRegex && reOk) {
                auto begin = std::sregex_iterator(line.begin(), line.end(), re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end && totalMatches < kMaxMatches; ++it) {
                    if (it->length() == 0) continue;
                    addHit((int)it->position(), (size_t)it->length());
                }
            } else {
                std::string hay = line;
                if (!matchCase)
                    for (char& c : hay) c = (char)tolower((unsigned char)c);
                size_t from = 0, at;
                while ((at = hay.find(needle, from)) != std::string::npos &&
                       totalMatches < kMaxMatches) {
                    addHit((int)at, needle.size());
                    from = at + (needle.empty() ? 1 : needle.size());
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
            lineNo++;
        }
        if (!firstHit) {
            if (!firstFile) body += ",";
            firstFile = false;
            body += "{\"path\":\"" + json::escape(full) +
                    "\",\"rel\":\"" + json::escape(rel) +
                    "\",\"matches\":[" + fileBody + "]}";
        }
    }
    body += "]}";
    send_json(app, body);
}

static void on_termStart(WebKitUserContentManager*, WebKitJavascriptResult*, gpointer p) {
    term_start(static_cast<ArchitectApp*>(p));
}
static void on_termInput(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    std::string data = b64_decode(obj_str(v, "data"));
    if (app->term.running.load() && app->term.fd >= 0 && !data.empty()) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = write(app->term.fd, data.data() + off, data.size() - off);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
            off += (size_t)n;
        }
    }
}
static void on_termResize(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_object(v)) return;
    int cols = obj_int(v, "cols"), rows = obj_int(v, "rows");
    if (app->term.running.load() && app->term.fd >= 0 && cols > 0 && rows > 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        ioctl(app->term.fd, TIOCSWINSZ, &ws);
    }
}

// ---------------------------------------------------------------------------
// Front-end (HTML/CSS/JS)
// ---------------------------------------------------------------------------
static const char* HTML =
"<!DOCTYPE html><html><head><meta charset='UTF-8'><style>"
"*{margin:0;padding:0;box-sizing:border-box;}"
"html,body{height:100%;overflow:hidden;}"
"body{font-family:'DejaVu Sans Mono',monospace;background:#0a0f0a;color:#33ff33;"
"display:flex;flex-direction:column;font-size:13px;}"
"#top{background:#0a140f;border-bottom:2px solid #33ff33;padding:6px 8px;display:flex;"
"gap:4px;align-items:center;flex-wrap:wrap;}"
".btn{padding:4px 9px;background:#030705;color:#33ff33;border:1px solid #1c5c2c;"
"cursor:pointer;font-size:11px;font-weight:bold;font-family:inherit;border-radius:2px;"
"line-height:1.5;white-space:nowrap;letter-spacing:.3px;}"
".btn:hover{background:#33ff33;color:#030705;border-color:#33ff33;}"
".sep{width:1px;height:18px;background:#1c5c2c;margin:0 3px;flex:none;}"
"#title{font-weight:bold;letter-spacing:1px;color:#8fffa0;margin-right:8px;}"
"#main{flex:1;display:flex;min-height:0;}"
"#side{width:220px;min-width:120px;background:#071007;border-right:1px solid #1c5c2c;"
"overflow:auto;padding:4px 0;font-size:12px;}"
"#side .root{padding:4px 8px;color:#8fffa0;font-weight:bold;border-bottom:1px solid #123;}"
".node{padding:2px 6px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
".node:hover{background:#0f2a12;}"
".node.dir{color:#7fd0ff;}"
".node.file{color:#bfe6c0;}"
"#center{flex:1;display:flex;flex-direction:column;min-width:0;}"
"#tabs{display:flex;background:#050a05;border-bottom:1px solid #1c5c2c;overflow-x:auto;}"
".tab{padding:5px 10px;border-right:1px solid #1c5c2c;cursor:pointer;white-space:nowrap;"
"font-size:12px;color:#9fdfa0;display:flex;gap:6px;align-items:center;}"
".tab.active{background:#0f2a12;color:#33ff33;}"
".tab .x{color:#6fa06f;font-weight:bold;}"
".tab .x:hover{color:#ff6b6b;}"
".tab .dot{color:#ffcc44;}"
"#edwrap{flex:1;display:flex;min-height:0;overflow:hidden;background:#0a0f0a;position:relative;}"
"#gutter{background:#071007;color:#4f8f5f;text-align:right;padding:8px 6px 8px 8px;"
"user-select:none;overflow:hidden;line-height:1.5;white-space:pre;border-right:1px solid #123;}"
/* editor is a transparent textarea overlaid on a highlighted <pre> layer */
"#hlwrap{flex:1;position:relative;min-width:0;overflow:hidden;background:#0a0f0a;}"
"#highlight{position:absolute;top:0;left:0;right:0;bottom:0;margin:0;padding:8px;"
"line-height:1.5;font-family:inherit;font-size:13px;white-space:pre;overflow:hidden;"
"color:#d6ffd6;tab-size:4;pointer-events:none;z-index:1;}"
"#diaglayer{position:absolute;top:0;left:0;right:0;bottom:0;overflow:hidden;"
"pointer-events:none;z-index:2;}"
"#diagin{position:absolute;top:0;left:0;}"
"#editor{position:absolute;top:0;left:0;width:100%;height:100%;background:transparent;"
"color:transparent;caret-color:#d6ffd6;border:none;outline:none;resize:none;padding:8px;"
"line-height:1.5;font-family:inherit;font-size:13px;white-space:pre;"
"overflow-x:auto;overflow-y:auto;tab-size:4;z-index:3;}"
"#editor::selection{background:#1c5c2c;color:transparent;}"
/* token colors */
".tk{color:#61afef;}.ts{color:#e6c07b;}.tc{color:#5c7a5c;font-style:italic;}"
".tn{color:#d19a66;}.tp{color:#c678dd;}"
/* diagnostic squiggles */
".sq{position:absolute;height:3px;pointer-events:none;}"
".sq.s1{background:repeating-linear-gradient(135deg,#ff5555 0,#ff5555 2px,transparent 2px,transparent 4px);}"
".sq.s2{background:repeating-linear-gradient(135deg,#ffcc44 0,#ffcc44 2px,transparent 2px,transparent 4px);}"
".sq.s3{background:repeating-linear-gradient(135deg,#44aaff 0,#44aaff 2px,transparent 2px,transparent 4px);}"
".sq.s4{background:repeating-linear-gradient(135deg,#888 0,#888 2px,transparent 2px,transparent 4px);}"
/* completion popup */
"#acbox{position:absolute;z-index:5;display:none;min-width:180px;max-width:440px;"
"max-height:210px;overflow:auto;background:#071007;border:1px solid #33ff33;"
"border-radius:3px;font-size:12px;box-shadow:0 4px 14px #000;}"
".acrow{padding:2px 8px;display:flex;gap:8px;align-items:baseline;white-space:nowrap;"
"cursor:pointer;color:#bfe6c0;}"
".acrow.sel{background:#134a1c;color:#eaffea;}"
".ackind{color:#7fd0ff;font-size:10px;min-width:38px;}"
".acdetail{color:#6f9f6f;font-size:11px;overflow:hidden;text-overflow:ellipsis;}"
"#hover{height:140px;border-top:2px solid #33ff33;background:#060d06;overflow:auto;"
"padding:8px 10px;font-size:12px;color:#cfe;white-space:pre-wrap;display:none;}"
"#hover.on{display:block;}"
"#hover .h{color:#8fffa0;font-weight:bold;margin-bottom:4px;}"
/* problems panel */
"#problems{height:150px;border-top:2px solid #ffcc44;background:#0d0d06;overflow:auto;"
"padding:2px 0 6px 0;font-size:12px;display:none;}"
"#problems.on{display:block;}"
"#problems .ph{color:#ffcc44;font-weight:bold;padding:3px 10px;position:sticky;top:0;"
"background:#0d0d06;}"
"#problist .pit{padding:3px 10px;cursor:pointer;white-space:pre-wrap;color:#cfe6cf;"
"border-left:3px solid transparent;}"
"#problist .pit:hover{background:#141a10;}"
"#problist .pit.s1{border-left-color:#ff5555;}"
"#problist .pit.s2{border-left-color:#ffcc44;}"
"#problist .pit.s3{border-left-color:#44aaff;}"
"#problist .pit.s4{border-left-color:#888;}"
"#problist .empty{padding:6px 10px;color:#5c7a5c;}"
".psev{font-weight:bold;color:#ff8a8a;}.ploc{color:#7fd0ff;}"
"#status{background:#0a140f;border-top:1px solid #1c5c2c;padding:5px 12px;display:flex;"
"justify-content:space-between;font-size:11px;color:#8fffa0;letter-spacing:.3px;}"
"#status #pos{color:#7fd0ff;}"
/* find & replace bar: anchored to the top-right of the editor area (edwrap is
   position:relative) so it sits below the tab bar, never over the toolbar */
"#findbar{position:absolute;top:6px;right:14px;z-index:6;background:#071007;"
"border:1px solid #33ff33;border-radius:3px;padding:7px;display:none;flex-direction:column;"
"gap:5px;box-shadow:0 6px 18px rgba(0,0,0,.6);font-size:12px;}"
"#findbar.on{display:flex;}"
"#findbar .row{display:flex;gap:4px;align-items:center;}"
"#findbar input{background:#030705;color:#d6ffd6;border:1px solid #1c5c2c;"
"font-family:inherit;font-size:12px;padding:2px 4px;width:180px;outline:none;}"
"#findbar input:focus{border-color:#33ff33;}"
"#findbar button{background:#030705;color:#33ff33;border:1px solid #1c5c2c;"
"cursor:pointer;font-family:inherit;font-size:11px;padding:2px 6px;}"
"#findbar button:hover{background:#33ff33;color:#030705;}"
"#findbar button.tog.on{background:#134a1c;color:#eaffea;}"
"#findcount{color:#7fd0ff;font-size:11px;min-width:56px;text-align:center;}"
/* bracket match highlight */
"#brklayer{position:absolute;top:0;left:0;right:0;bottom:0;overflow:hidden;"
"pointer-events:none;z-index:1;}"
"#brkin{position:absolute;top:0;left:0;}"
".brk{position:absolute;border:1px solid #33ff33;background:rgba(51,255,51,.15);"
"box-sizing:border-box;}"
/* find current-match highlight */
"#fmlayer{position:absolute;top:0;left:0;right:0;bottom:0;overflow:hidden;"
"pointer-events:none;z-index:2;}"
"#fmin{position:absolute;top:0;left:0;}"
".fmk{position:absolute;background:rgba(255,204,68,.35);box-sizing:border-box;}"
/* signature help popup */
"#sigbox{position:absolute;z-index:7;display:none;max-width:520px;background:#071007;"
"border:1px solid #7fd0ff;padding:5px 9px;font-size:12px;color:#cfe6cf;"
"box-shadow:0 4px 14px #000;white-space:pre-wrap;}"
"#sigbox .ap{color:#ffcc44;font-weight:bold;text-decoration:underline;}"
"#sigbox .sd{color:#6f9f6f;font-size:11px;margin-top:3px;}"
/* references panel (reuses problems styling) */
"#refs{height:150px;border-top:2px solid #7fd0ff;background:#060d0d;overflow:auto;"
"padding:2px 0 6px 0;font-size:12px;display:none;}"
"#refs.on{display:block;}"
"#refs .ph{color:#7fd0ff;font-weight:bold;padding:3px 10px;position:sticky;top:0;background:#060d0d;}"
"#reflist .rit{padding:3px 10px;cursor:pointer;white-space:pre-wrap;color:#cfe6cf;"
"border-left:3px solid #7fd0ff;}"
"#reflist .rit:hover{background:#0f1a1a;}"
"#reflist .empty{padding:6px 10px;color:#5c7a5c;}"
".rloc{color:#7fd0ff;}"
/* settings menu */
"#setmenu{position:absolute;z-index:8;display:none;background:#071007;border:1px solid #33ff33;"
"padding:8px;font-size:12px;box-shadow:0 4px 14px #000;flex-direction:column;gap:6px;}"
"#setmenu.on{display:flex;}"
"#setmenu .srow{display:flex;gap:6px;align-items:center;justify-content:space-between;}"
"#setmenu .srow span{color:#bfe6c0;}"
"#setmenu button{background:#030705;color:#33ff33;border:1px solid #1c5c2c;cursor:pointer;"
"font-family:inherit;font-size:12px;padding:1px 8px;min-width:24px;}"
"#setmenu button:hover{background:#33ff33;color:#030705;}"
"#setmenu .sval{color:#eaffea;min-width:24px;text-align:center;}"
"#aboutov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;align-items:center;justify-content:center;}"
"#aboutov.on{display:flex;}"
"#aboutbox{background:#0a140f;border:2px solid #33ff33;color:#33ff33;max-width:480px;margin:16px;padding:20px;font-family:'DejaVu Sans Mono',monospace;font-size:12px;line-height:1.7;box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#aboutbox .atitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#aboutbox .aok{margin-top:16px;text-align:right;}"
/* extensions list overlay */
"#extov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;"
"align-items:center;justify-content:center;}"
"#extov.on{display:flex;}"
"#extbox{background:#0a140f;border:2px solid #33ff33;border-radius:4px;color:#cfe6cf;"
"max-width:520px;width:88%;max-height:70%;overflow:auto;margin:16px;padding:18px 20px;"
"font-family:'DejaVu Sans Mono',monospace;font-size:12px;line-height:1.6;"
"box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#extbox .etitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#extbox .erow{padding:5px 0;border-bottom:1px solid #123;}"
"#extbox .ehdr{color:#8fffa0;font-weight:bold;margin:12px 0 4px;}"
"#extbox .esub{color:#6f9f6f;font-size:11px;}"
"#extbox .eok{margin-top:16px;text-align:right;}"
/* command palette / fuzzy finder overlay */
"#qov{position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:9000;display:none;justify-content:center;align-items:flex-start;}"
"#qov.on{display:flex;}"
"#qbox{margin-top:56px;width:620px;max-width:90%;background:#071007;border:1px solid #33ff33;border-radius:4px;overflow:hidden;"
"box-shadow:0 8px 30px #000;display:flex;flex-direction:column;max-height:64%;}"
"#qin{background:#030705;color:#d6ffd6;border:none;border-bottom:1px solid #1c5c2c;outline:none;"
"font-family:inherit;font-size:14px;padding:8px 10px;}"
"#qlist{overflow:auto;}"
".qrow{padding:5px 10px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;"
"color:#bfe6c0;display:flex;gap:8px;align-items:baseline;}"
".qrow.sel{background:#134a1c;color:#eaffea;}"
".qrow .qsub{color:#6f9f6f;font-size:11px;overflow:hidden;text-overflow:ellipsis;}"
".qrow .qkey{color:#7fd0ff;font-size:10px;margin-left:auto;}"
"#qempty{padding:8px 10px;color:#5c7a5c;}"
/* project-wide search panel */
"#search{height:200px;border-top:2px solid #c678dd;background:#0c0810;overflow:hidden;"
"display:none;flex-direction:column;font-size:12px;}"
"#search.on{display:flex;}"
"#search .sbar{display:flex;gap:4px;align-items:center;padding:5px 8px;background:#0c0810;"
"border-bottom:1px solid #2a1c3a;}"
"#search .sbar input{background:#030705;color:#d6ffd6;border:1px solid #1c5c2c;font-family:inherit;"
"font-size:12px;padding:2px 4px;outline:none;}"
"#searchq{flex:1;}"
"#search .sbar button{background:#030705;color:#c678dd;border:1px solid #2a1c3a;cursor:pointer;"
"font-family:inherit;font-size:11px;padding:2px 6px;}"
"#search .sbar button.on{background:#3a2a4a;color:#eaffea;}"
"#searchres{overflow:auto;}"
"#searchres .sfile{color:#c678dd;font-weight:bold;padding:4px 8px 2px;}"
"#searchres .smatch{padding:1px 8px 1px 22px;cursor:pointer;white-space:pre;color:#cfe6cf;"
"overflow:hidden;text-overflow:ellipsis;}"
"#searchres .smatch:hover{background:#171021;}"
"#searchres .sloc{color:#7fd0ff;}"
"#searchres .empty{padding:8px;color:#5c7a5c;}"
/* integrated terminal panel */
"#terminal{height:230px;border-top:2px solid #44aaff;background:#050608;display:none;"
"flex-direction:column;min-height:60px;}"
"#terminal.on{display:flex;}"
"#terminal .thead{display:flex;align-items:center;gap:8px;padding:2px 8px;background:#08101a;"
"border-bottom:1px solid #12283a;color:#7fd0ff;font-size:11px;font-weight:bold;}"
"#terminal .thead .tx{margin-left:auto;cursor:pointer;color:#6fa06f;}"
"#terminal .thead .tx:hover{color:#ff6b6b;}"
"#termscreen{flex:1;overflow:auto;padding:4px 8px;font-family:inherit;font-size:12px;"
"line-height:1.35;white-space:pre-wrap;word-break:break-all;color:#cfe6cf;outline:none;}"
/* terminal ANSI colors */
".b{font-weight:bold;}"
".tf30{color:#5c6370;}.tf31{color:#e06c75;}.tf32{color:#98c379;}.tf33{color:#e5c07b;}"
".tf34{color:#61afef;}.tf35{color:#c678dd;}.tf36{color:#56b6c2;}.tf37{color:#dcdfe4;}"
".tf90{color:#7f848e;}.tf91{color:#ff7b86;}.tf92{color:#b5e890;}.tf93{color:#ffd88a;}"
".tf94{color:#8fc7ff;}.tf95{color:#e0a0ff;}.tf96{color:#7fe0ec;}.tf97{color:#ffffff;}"
"</style></head><body>"
"<div id='top'>"
"<span id='title'>&#9670; ARCHITECT</span>"
"<button class='btn' onclick='M.openFolder.postMessage(\"\")'>FOLDER</button>"
"<button class='btn' onclick='M.openFile.postMessage(\"\")'>OPEN</button>"
"<button class='btn' onclick='newFile()' title='Ctrl+N'>NEW</button>"
"<button class='btn' onclick='save()' title='Ctrl+S'>SAVE</button>"
"<button class='btn' onclick='saveAs()' title='Ctrl+Shift+S'>SAVE AS</button>"
"<span class='sep'></span>"
"<button class='btn' onclick='doHover()' title='Hover at caret (F1)'>HOVER</button>"
"<button class='btn' onclick='requestCompletion(true)' title='Autocomplete (Ctrl+Space)'>SUGGEST</button>"
"<button class='btn' onclick='doDefinition()' title='Go to definition (F12)'>DEF</button>"
"<button class='btn' onclick='doReferences()' title='Find references (Shift+F12)'>REFS</button>"
"<button class='btn' onclick='doRename()' title='Rename symbol (F2)'>RENAME</button>"
"<button class='btn' onclick='toggleProblems()'><span id='probbtn'>PROBLEMS</span></button>"
"<button class='btn' onclick='doFormat()' title='Format Document (Shift+Alt+F)'>FMT</button>"
"<span class='sep'></span>"
"<button class='btn' onclick='openPalette()' title='Command Palette (Ctrl+Shift+P)'>CMD</button>"
"<button class='btn' onclick='openFinder()' title='Go to File (Ctrl+P)'>GOTO</button>"
"<button class='btn' onclick='toggleSearch(true)' title='Search in Files (Ctrl+Shift+F)'>SEARCH</button>"
"<button class='btn' onclick='toggleTerminal()' title='Terminal (Ctrl+`)'>TERM</button>"
"<button class='btn' onclick='devRun(\"run\")' title='dev run current file'>&#9654; RUN</button>"
"<span class='sep'></span>"
"<button class='btn' onclick='toggleFind()' title='Find (Ctrl+F)'>FIND</button>"
"<button class='btn' onclick='toggleSettings(event)' title='Settings'>SET</button>"
"<button class='btn' onclick='showAbout()' title='About Architect'>ABOUT</button>"
"</div>"
"<div id='setmenu'>"
"<div class='srow'><span>Font size</span><button onclick='changeFont(-1)'>-</button>"
"<span class='sval' id='fsval'>13</span><button onclick='changeFont(1)'>+</button></div>"
"<div class='srow'><span>Tab width</span><button onclick='changeTab(-1)'>-</button>"
"<span class='sval' id='twval'>4</span><button onclick='changeTab(1)'>+</button></div>"
"<div class='srow'><span>Format on save</span>"
"<button id='fosbtn' onclick='toggleFormatOnSave()'>OFF</button></div>"
"</div>"
"<div id='main'>"
"<div id='side'><div class='root' id='rootlabel'>NO FOLDER</div><div id='tree'></div></div>"
"<div id='center'>"
"<div id='tabs'></div>"
"<div id='edwrap'>"
"<div id='gutter'>1</div>"
"<div id='hlwrap'>"
"<pre id='highlight'></pre>"
"<div id='brklayer'><div id='brkin'></div></div>"
"<div id='fmlayer'><div id='fmin'></div></div>"
"<div id='diaglayer'><div id='diagin'></div></div>"
"<textarea id='editor' spellcheck='false' wrap='off'></textarea>"
"<div id='acbox'></div>"
"<div id='sigbox'></div>"
"<div id='findbar'>"
"<div class='row'><input id='findin' placeholder='Find' spellcheck='false'>"
"<span id='findcount'>0/0</span>"
"<button onclick='findNext(1)' title='Next (Enter)'>&#9660;</button>"
"<button onclick='findNext(-1)' title='Previous (Shift+Enter)'>&#9650;</button>"
"<button class='tog' id='casebtn' onclick='toggleCase()' title='Match case'>Aa</button>"
"<button onclick='toggleFind()' title='Close (Esc)'>&#10005;</button></div>"
"<div class='row'><input id='replin' placeholder='Replace' spellcheck='false'>"
"<button onclick='replaceOne()'>Repl</button>"
"<button onclick='replaceAll()'>All</button></div>"
"</div>"
"</div>"
"</div>"
"<div id='hover'></div>"
"<div id='problems'><div class='ph'>PROBLEMS</div><div id='problist'></div></div>"
"<div id='refs'><div class='ph'>REFERENCES</div><div id='reflist'></div></div>"
"<div id='search'>"
"<div class='sbar'>"
"<input id='searchq' placeholder='Search in files' spellcheck='false'>"
"<button id='searchcase' onclick='toggleSearchOpt(\"case\")' title='Match case'>Aa</button>"
"<button id='searchre' onclick='toggleSearchOpt(\"re\")' title='Regular expression'>.*</button>"
"<button onclick='runProjectSearch()' title='Search (Enter)'>GO</button>"
"<button onclick='toggleSearch()' title='Close'>&#10005;</button>"
"</div><div id='searchres'></div></div>"
"<div id='terminal'>"
"<div class='thead'>&#9636; TERMINAL<span class='tx' onclick='toggleTerminal()'>&#10005;</span></div>"
"<div id='termscreen' tabindex='0'></div>"
"</div>"
"</div>"
"</div>"
"<div id='status'><span id='msg'>&gt; READY</span><span id='pos'>Ln 1, Col 1</span></div>"
"<script>"
"var M=window.webkit.messageHandlers;"
"function log(s){M.log.postMessage(String(s));}"
"function b64e(str){var b=new TextEncoder().encode(str);var s='';"
"for(var i=0;i<b.length;i++)s+=String.fromCharCode(b[i]);return btoa(s);}"
"function b64d(b){var bin=atob(b);var a=new Uint8Array(bin.length);"
"for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);"
"return new TextDecoder('utf-8').decode(a);}"
"var ed=document.getElementById('editor');"
"var gutter=document.getElementById('gutter');"
"var hl_el=document.getElementById('highlight');"
"var diagin=document.getElementById('diagin');"
"var acbox=document.getElementById('acbox');"
"var tabsEl=document.getElementById('tabs');"
"var sigbox=document.getElementById('sigbox');"
"var brkin=document.getElementById('brkin');"
"var tabs=[];var active=-1;var untitledN=0;var diagStore={};"
"var g_fontSize=13,g_tabWidth=4,g_formatOnSave=false;"
/* ---- editor metrics (monospace) ---- */
"var PADX=8,PADY=8,CHW=8,LNH=19.5;"
"function measure(){var cs=getComputedStyle(ed);"
"var s=document.createElement('span');s.style.position='absolute';s.style.visibility='hidden';"
"s.style.whiteSpace='pre';s.style.fontFamily=cs.fontFamily;s.style.fontSize=cs.fontSize;"
"s.textContent='0000000000';document.body.appendChild(s);"
"CHW=s.getBoundingClientRect().width/10;document.body.removeChild(s);"
"var lh=parseFloat(cs.lineHeight);if(!isNaN(lh))LNH=lh;"
"PADX=parseFloat(cs.paddingLeft)||8;PADY=parseFloat(cs.paddingTop)||8;}"
/* ---- syntax highlighter (offline tokenizer) ---- */
"var Q1=String.fromCharCode(39),Q2=String.fromCharCode(34),Q3=String.fromCharCode(96);"
"function kwset(s){var o={};var a=s.split(' ');for(var i=0;i<a.length;i++)if(a[i])o[a[i]]=1;return o;}"
"var CKW='auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Bool _Complex sizeof include define ifdef ifndef endif pragma undef';"
"var CPPKW=CKW+' bool catch class constexpr constinit consteval decltype delete dynamic_cast explicit export false friend mutable namespace new noexcept nullptr operator private protected public reinterpret_cast requires static_assert static_cast template this throw true try typeid typename using virtual wchar_t concept co_await co_return co_yield';"
"var LANGS={"
"'c':{line:'//',block:['/*','*/'],str:[Q2,Q1],pp:true,kw:kwset(CKW)},"
"'cpp':{line:'//',block:['/*','*/'],str:[Q2,Q1],pp:true,kw:kwset(CPPKW)},"
"'python':{line:'#',str:[Q2,Q1],triple:true,kw:kwset('False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield match case self')},"
"'js':{line:'//',block:['/*','*/'],str:[Q2,Q1,Q3],kw:kwset('async await break case catch class const continue debugger default delete do else export extends false finally for function get if import in instanceof let new null of return set static super switch this throw true try typeof var void while with yield')},"
"'sh':{line:'#',str:[Q2,Q1],kw:kwset('if then else elif fi case esac for while until do done in function select return break continue local export readonly declare unset shift eval exec source alias set trap echo cd test')},"
"'json':{str:[Q2],kw:kwset('true false null')}};"
"function hlLangFor(name){name=(name||'').toLowerCase();"
"function e(x){return name.length>=x.length&&name.slice(-x.length)===x;}"
"if(e('.json'))return 'json';"
"if(e('.cpp')||e('.cc')||e('.cxx')||e('.hpp')||e('.hh')||e('.hxx')||e('.c++'))return 'cpp';"
"if(e('.c')||e('.h'))return 'c';"
"if(e('.py'))return 'python';"
"if(e('.js')||e('.mjs')||e('.cjs')||e('.jsx'))return 'js';"
"if(e('.sh')||e('.bash')||e('.zsh')||e('.ksh'))return 'sh';"
"return '';}"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
"function hl(code,lang){var L=LANGS[lang];if(!L)return esc(code);"
"var out='';var i=0,n=code.length;var kw=L.kw;var atLine=true;"
"function ids(c){return /[A-Za-z_$]/.test(c);}function idc(c){return /[A-Za-z0-9_$]/.test(c);}"
"while(i<n){var c=code[i];var j;"
"if(L.pp&&c=='#'&&atLine){j=code.indexOf('\\n',i);if(j<0)j=n;"
"out+='<span class=\"tp\">'+esc(code.slice(i,j))+'</span>';i=j;continue;}"
"if(L.line&&code.substr(i,L.line.length)==L.line){j=code.indexOf('\\n',i);if(j<0)j=n;"
"out+='<span class=\"tc\">'+esc(code.slice(i,j))+'</span>';i=j;continue;}"
"if(L.block&&code.substr(i,2)==L.block[0]){j=code.indexOf(L.block[1],i+2);"
"if(j<0)j=n;else j+=L.block[1].length;"
"out+='<span class=\"tc\">'+esc(code.slice(i,j))+'</span>';i=j;atLine=(code[i-1]=='\\n');continue;}"
"if(L.triple&&(code.substr(i,3)==Q2+Q2+Q2||code.substr(i,3)==Q1+Q1+Q1)){"
"var tq=code.substr(i,3);j=code.indexOf(tq,i+3);if(j<0)j=n;else j+=3;"
"out+='<span class=\"ts\">'+esc(code.slice(i,j))+'</span>';i=j;atLine=(code[i-1]=='\\n');continue;}"
"if(L.str.indexOf(c)>=0){var q=c;j=i+1;"
"while(j<n){if(code[j]=='\\\\'){j+=2;continue;}if(code[j]==q){j++;break;}if(code[j]=='\\n'){break;}j++;}"
"out+='<span class=\"ts\">'+esc(code.slice(i,j))+'</span>';i=j;atLine=false;continue;}"
"if((c>='0'&&c<='9')||(c=='.'&&code[i+1]>='0'&&code[i+1]<='9')){"
"j=i+1;while(j<n&&/[0-9a-zA-Z_.]/.test(code[j]))j++;"
"out+='<span class=\"tn\">'+esc(code.slice(i,j))+'</span>';i=j;atLine=false;continue;}"
"if(ids(c)){j=i+1;while(j<n&&idc(code[j]))j++;var w=code.slice(i,j);"
"if(kw[w])out+='<span class=\"tk\">'+esc(w)+'</span>';else out+=esc(w);i=j;atLine=false;continue;}"
"out+=esc(c);if(c=='\\n')atLine=true;else if(c!=' '&&c!='\\t')atLine=false;i++;}"
"return out;}"
"function renderHighlight(){var t=cur();var code=ed.value;"
"if(code.length>120000){hl_el.textContent=code;return;}"
"hl_el.innerHTML=hl(code,t?t.hl:'');}"
"function syncScroll(){var st=ed.scrollTop,sl=ed.scrollLeft;"
"hl_el.scrollTop=st;hl_el.scrollLeft=sl;gutter.scrollTop=st;"
"var tf='translate('+(-sl)+'px,'+(-st)+'px)';"
"diagin.style.transform=tf;brkin.style.transform=tf;"
"var fm=document.getElementById('fmin');if(fm)fm.style.transform=tf;}"
/* ---- tabs / editor ---- */
"function setStatus(m){var s=document.getElementById('msg');s.innerText='> '+m;}"
"function cur(){return active>=0?tabs[active]:null;}"
"function renderTabs(){tabsEl.innerHTML='';tabs.forEach(function(t,i){"
"var d=document.createElement('div');d.className='tab'+(i==active?' active':'');"
"var dot=t.dirty?'<span class=\"dot\">*</span>':'';"
"d.innerHTML=dot+'<span></span><span class=\"x\">x</span>';"
"d.children[dot?1:0].innerText=t.name;"
"d.onclick=function(e){if(e.target.className=='x'){closeTab(i);e.stopPropagation();}else activate(i);};"
"tabsEl.appendChild(d);});}"
"function activate(i){if(active>=0&&tabs[active]&&!tabs[active].readonly)tabs[active].text=ed.value;"
"active=i;var t=tabs[i];ed.value=t.text;ed.disabled=!!t.readonly;ed.readOnly=!!t.readonly;renderTabs();"
"renderHighlight();updateGutter();updatePos();renderDiagnostics();syncScroll();acHide();"
"setStatus((t.readonly?'VIEW ':'EDITING ')+t.name);ed.focus();}"
"function closeTab(i){if(tabs[i].dirty&&!confirm('Discard unsaved changes in '+tabs[i].name+'?'))return;"
"tabs.splice(i,1);if(active>=tabs.length)active=tabs.length-1;"
"if(active<0){ed.value='';ed.disabled=true;}else{ed.value=tabs[active].text;}"
"renderTabs();renderHighlight();updateGutter();renderDiagnostics();acHide();saveSession();}"
"function openTab(path,name,text,lang,ro){ro=!!ro;"
"for(var i=0;i<tabs.length;i++)if(tabs[i].path&&tabs[i].path==path){tabs[i].text=text;tabs[i].sentText=text;tabs[i].readonly=ro;tabs[i].lang=ro?'':lang;tabs[i].hl=ro?'':hlLangFor(name);activate(i);return;}"
"tabs.push({path:path,name:name,text:text,sentText:text,lang:ro?'':lang,hl:ro?'':hlLangFor(name),dirty:false,readonly:ro});activate(tabs.length-1);saveSession();}"
"function newFile(){untitledN++;tabs.push({path:'',name:'untitled-'+untitledN+'.txt',text:'',sentText:'',lang:'',hl:'',dirty:false});"
"activate(tabs.length-1);}"
"function markDirty(){var t=cur();if(t&&!t.dirty){t.dirty=true;renderTabs();}}"
"function updateGutter(){var n=ed.value.split('\\n').length;var s='';"
"for(var i=1;i<=n;i++)s+=i+'\\n';gutter.textContent=s;gutter.scrollTop=ed.scrollTop;}"
"function caretLineCol(){var p=ed.selectionStart;var before=ed.value.substr(0,p);"
"var nl=before.lastIndexOf('\\n');var line=(before.match(/\\n/g)||[]).length;"
"var col=p-(nl+1);return{line:line,col:col};}"
"function updatePos(){var lc=caretLineCol();"
"document.getElementById('pos').innerText='Ln '+(lc.line+1)+', Col '+(lc.col+1);}"
"var changeTimer=null;"
/* offset(0-based) -> {line,ch} in string s */
"function offToLC(s,off){var line=0,last=0;for(var i=0;i<off&&i<s.length;i++){"
"if(s[i]=='\\n'){line++;last=i+1;}}return{line:line,ch:off-last};}"
/* minimal common-prefix/suffix diff between old and new text */
"function diffRange(o,n){var oL=o.length,nL=n.length;var p=0;var m=Math.min(oL,nL);"
"while(p<m&&o[p]==n[p])p++;var s=0;while(s<(m-p)&&o[oL-1-s]==n[nL-1-s])s++;"
"return{startOff:p,endOff:oL-s,repl:n.slice(p,nL-s)};}"
/* Incremental didChange: send only the replaced range + its new text. */
"function scheduleChange(){var t=cur();if(!t||!t.path||!t.lang)return;"
"clearTimeout(changeTimer);changeTimer=setTimeout(function(){"
"var t2=cur();if(!t2||!t2.path||!t2.lang)return;"
"var o=t2.sentText,n=t2.text;if(o===n)return;var d=diffRange(o,n);"
"var st=offToLC(o,d.startOff),en=offToLC(o,d.endOff);t2.sentText=n;"
"M.changeRange.postMessage({path:t2.path,startLine:st.line,startChar:st.ch,"
"endLine:en.line,endChar:en.ch,text:b64e(d.repl)});},400);}"
"ed.addEventListener('input',function(){var t=cur();if(!t)return;t.text=ed.value;"
"markDirty();renderHighlight();updateGutter();updatePos();syncScroll();scheduleChange();scheduleAC();"
"updateBracket();if(findState.open)runFind();"
"var pch=ed.value[ed.selectionStart-1];"
"if(pch=='('||pch==','){scheduleSig();}else if(pch==')'){sigHide();}});"
"ed.addEventListener('scroll',function(){syncScroll();if(sigbox.style.display=='block')requestSig();});"
"ed.addEventListener('keyup',function(){updatePos();updateBracket();});"
"ed.addEventListener('click',function(e){updatePos();acHide();sigHide();updateBracket();"
"if(e.ctrlKey){e.preventDefault();doDefinition();}});"
"function realSave(){var t=cur();if(!t||t.readonly)return;t.text=ed.value;"
"M.save.postMessage({path:t.path,name:t.name,content:b64e(t.text)});}"
"var pendingFormatSave=null;"
"function save(){var t=cur();if(!t)return;if(t.readonly){setStatus('Read-only: file not shown in editor');return;}t.text=ed.value;"
"if(g_formatOnSave&&t.path&&t.lang){pendingFormatSave=t.path;doFormat();}else realSave();}"
"function saveAs(){var t=cur();if(!t)return;if(t.readonly){setStatus('Read-only: nothing to save');return;}t.text=ed.value;"
"M.saveAs.postMessage({name:t.name,content:b64e(t.text)});}"
"function doHover(){var t=cur();if(!t||!t.path||!t.lang){return;}"
"var lc=caretLineCol();M.hover.postMessage({path:t.path,line:lc.line,character:lc.col});}"
/* ---- autocomplete ---- */
"var acItems=[],acSel=0,acOpen=false,acTimer=null;"
"var KINDS={1:'text',2:'meth',3:'fn',4:'ctor',5:'field',6:'var',7:'class',8:'iface',9:'mod',"
"10:'prop',11:'unit',12:'val',13:'enum',14:'kw',15:'snip',16:'color',17:'file',18:'ref',"
"19:'dir',20:'enum',21:'const',22:'struct',23:'event',24:'op',25:'type'};"
"function acHide(){acbox.style.display='none';acOpen=false;acItems=[];}"
"function acRefresh(){var r=acbox.children;for(var i=0;i<r.length;i++)"
"r[i].className='acrow'+(i==acSel?' sel':'');"
"if(r[acSel])r[acSel].scrollIntoView({block:'nearest'});}"
"function acShow(items){acItems=items||[];acSel=0;if(!acItems.length){acHide();return;}"
"acbox.innerHTML='';"
"for(var i=0;i<acItems.length;i++){(function(it,idx){"
"var row=document.createElement('div');row.className='acrow'+(idx==0?' sel':'');"
"var k=document.createElement('span');k.className='ackind';k.textContent=KINDS[it.kind]||'';"
"var l=document.createElement('span');l.className='aclabel';l.textContent=it.label;"
"row.appendChild(k);row.appendChild(l);"
"if(it.detail){var d=document.createElement('span');d.className='acdetail';d.textContent=it.detail;row.appendChild(d);}"
"row.onmousedown=function(ev){ev.preventDefault();acSel=idx;acAccept();};"
"acbox.appendChild(row);})(acItems[i],i);}"
"var lc=caretLineCol();"
"var x=PADX+lc.col*CHW-ed.scrollLeft;var y=PADY+(lc.line+1)*LNH-ed.scrollTop;"
"acbox.style.left=Math.max(0,x)+'px';acbox.style.top=y+'px';"
"acbox.style.display='block';acOpen=true;}"
"function acAccept(){var it=acItems[acSel];if(!it){acHide();return;}var t=cur();if(!t){acHide();return;}"
"var p=ed.selectionStart;var before=ed.value.slice(0,p);"
"var m=before.match(/[A-Za-z0-9_$]*$/);var pre=m?m[0]:'';var start=p-pre.length;"
"var ins=it.insert||it.label;"
"ed.value=ed.value.slice(0,start)+ins+ed.value.slice(p);"
"var np=start+ins.length;ed.selectionStart=ed.selectionEnd=np;"
"t.text=ed.value;markDirty();renderHighlight();updateGutter();updatePos();syncScroll();"
"scheduleChange();acHide();ed.focus();}"
"function requestCompletion(force){var t=cur();"
"if(!t||!t.path||!t.lang){if(force)setStatus('Completion: no language server for this file');return;}"
"var lc=caretLineCol();M.complete.postMessage({path:t.path,line:lc.line,character:lc.col});}"
"function scheduleAC(){clearTimeout(acTimer);var t=cur();if(!t||!t.path||!t.lang){acHide();return;}"
"var p=ed.selectionStart;var ch=ed.value[p-1];"
"if(!ch||!/[A-Za-z0-9_$.>:]/.test(ch)){acHide();return;}"
"acTimer=setTimeout(function(){requestCompletion(false);},260);}"
"var QUOTES={};QUOTES[Q1]=1;QUOTES[Q2]=1;QUOTES[Q3]=1;"
"ed.addEventListener('keydown',function(e){"
"if(acOpen){"
"if(e.key=='ArrowDown'){acSel=(acSel+1)%acItems.length;acRefresh();e.preventDefault();return;}"
"if(e.key=='ArrowUp'){acSel=(acSel-1+acItems.length)%acItems.length;acRefresh();e.preventDefault();return;}"
"if(e.key=='Enter'||e.key=='Tab'){acAccept();e.preventDefault();return;}"
"if(e.key=='Escape'){acHide();e.preventDefault();return;}}"
"if(e.ctrlKey&&(e.code=='Space'||e.key==' ')){e.preventDefault();requestCompletion(true);return;}"
"if(e.ctrlKey||e.altKey||e.metaKey)return;"
/* Tab: insert indent, or indent/dedent the selected lines */
"if(e.key=='Tab'){var v=ed.value,s=ed.selectionStart,en=ed.selectionEnd,u=indentUnit();"
"if(s==en&&!e.shiftKey){ed.value=v.slice(0,s)+u+v.slice(s);ed.selectionStart=ed.selectionEnd=s+u.length;"
"e.preventDefault();afterEdit();return;}"
"var ls=v.lastIndexOf('\\n',s-1)+1;var le=v.indexOf('\\n',en);if(le<0)le=v.length;"
"var lines=v.slice(ls,le).split('\\n');"
"var nl=lines.map(function(ln){if(e.shiftKey){if(ln.slice(0,u.length)==u)return ln.slice(u.length);"
"var m=ln.match(/^\\s/);return m?ln.slice(1):ln;}return u+ln;});"
"var rep=nl.join('\\n');ed.value=v.slice(0,ls)+rep+v.slice(le);"
"ed.selectionStart=ls;ed.selectionEnd=ls+rep.length;e.preventDefault();afterEdit();return;}"
/* Enter: auto-indent (carry leading whitespace; extra after { ( [ or :) */
"if(e.key=='Enter'&&ed.selectionStart==ed.selectionEnd){"
"var v=ed.value,p=ed.selectionStart;var ls=v.lastIndexOf('\\n',p-1)+1;"
"var ind=(v.slice(ls,p).match(/^[ \\t]*/)||[''])[0];"
"var pc=v[p-1],nc=v[p];var extra=(pc=='{'||pc==':'||pc=='('||pc=='[')?indentUnit():'';"
"if((pc=='{'&&nc=='}')||(pc=='('&&nc==')')||(pc=='['&&nc==']')){"
"var ins='\\n'+ind+extra+'\\n'+ind;ed.value=v.slice(0,p)+ins+v.slice(p);"
"ed.selectionStart=ed.selectionEnd=p+1+ind.length+extra.length;}"
"else{var ins2='\\n'+ind+extra;ed.value=v.slice(0,p)+ins2+v.slice(p);"
"ed.selectionStart=ed.selectionEnd=p+ins2.length;}"
"e.preventDefault();afterEdit();return;}"
/* Backspace between an empty pair deletes both delimiters */
"if(e.key=='Backspace'&&ed.selectionStart==ed.selectionEnd){"
"var v=ed.value,p=ed.selectionStart;var a=v[p-1],b=v[p];"
"if((a=='('&&b==')')||(a=='['&&b==']')||(a=='{'&&b=='}')||(QUOTES[a]&&a==b)){"
"ed.value=v.slice(0,p-1)+v.slice(p+1);ed.selectionStart=ed.selectionEnd=p-1;"
"e.preventDefault();afterEdit();return;}}"
/* Auto-close brackets/quotes; wrap selection; type-through closers */
"if(e.key.length==1){var ch=e.key;var v=ed.value,s=ed.selectionStart,en=ed.selectionEnd;"
"if(s!=en&&(PAIRS[ch]||QUOTES[ch])){var cl=PAIRS[ch]||ch;"
"ed.value=v.slice(0,s)+ch+v.slice(s,en)+cl+v.slice(en);"
"ed.selectionStart=s+1;ed.selectionEnd=en+1;e.preventDefault();afterEdit();return;}"
"if(s==en){"
"if((ch==')'||ch==']'||ch=='}')&&v[s]==ch){ed.selectionStart=ed.selectionEnd=s+1;"
"e.preventDefault();updatePos();updateBracket();return;}"
"if(PAIRS[ch]){ed.value=v.slice(0,s)+ch+PAIRS[ch]+v.slice(s);ed.selectionStart=ed.selectionEnd=s+1;"
"e.preventDefault();afterEdit();return;}"
"if(QUOTES[ch]){if(v[s]==ch){ed.selectionStart=ed.selectionEnd=s+1;"
"e.preventDefault();updatePos();updateBracket();return;}"
"var nx=v[s]||'';var pv=v[s-1]||'';"
"if((nx===''||nx==' '||nx=='\\t'||nx=='\\n'||nx==')'||nx==']'||nx=='}'||nx==','||nx==';')&&!/[A-Za-z0-9_]/.test(pv)){"
"ed.value=v.slice(0,s)+ch+ch+v.slice(s);ed.selectionStart=ed.selectionEnd=s+1;"
"e.preventDefault();afterEdit();return;}}}}});"
/* ---- diagnostics ---- */
"function posToIndex(line,col){var v=ed.value;var idx=0,ln=0;"
"for(var i=0;i<v.length&&ln<line;i++){if(v[i]=='\\n'){ln++;idx=i+1;}}return idx+col;}"
"function jumpTo(line,col){var idx=posToIndex(line,col);ed.focus();"
"ed.selectionStart=ed.selectionEnd=idx;ed.scrollTop=Math.max(0,line*LNH-ed.clientHeight/2);"
"syncScroll();updatePos();}"
"function toggleProblems(){var p=document.getElementById('problems');"
"p.className=(p.className=='on')?'':'on';}"
"function renderDiagnostics(){diagin.innerHTML='';var pl=document.getElementById('problist');pl.innerHTML='';"
"var t=cur();var list=(t&&t.path)?diagStore[t.path]:null;var count=list?list.length:0;"
"document.getElementById('probbtn').innerText='PROBLEMS'+(count?' ('+count+')':'');"
"if(!list||!count){pl.innerHTML='<div class=\"empty\">No problems.</div>';return;}"
"var lines=ed.value.split('\\n');"
"list.forEach(function(d){var sev=d.severity||1;"
"var w=1;if(d.endLine==d.line)w=Math.max(1,d.endCharacter-d.character);"
"else w=Math.max(1,((lines[d.line]||'').length-d.character));"
"var sq=document.createElement('div');sq.className='sq s'+sev;"
"sq.style.left=(PADX+d.character*CHW)+'px';sq.style.top=(PADY+(d.line+1)*LNH-3)+'px';"
"sq.style.width=(w*CHW)+'px';diagin.appendChild(sq);"
"var e=document.createElement('div');e.className='pit s'+sev;"
"var st=['','E','W','I','H'][sev]||'?';"
"var tag=document.createElement('span');tag.className='psev';tag.textContent=st;"
"var loc=document.createElement('span');loc.className='ploc';loc.textContent=' '+(d.line+1)+':'+(d.character+1)+'  ';"
"e.appendChild(tag);e.appendChild(loc);"
"e.appendChild(document.createTextNode(d.message+(d.source?'  ['+d.source+']':'')));"
"e.onclick=function(){jumpTo(d.line,d.character);};pl.appendChild(e);});}"
/* ---- settings / session ---- */
"function saveSession(){var fs=[];for(var i=0;i<tabs.length;i++)if(tabs[i].path)fs.push(tabs[i].path);"
"M.saveState.postMessage({files:b64e(fs.join(String.fromCharCode(10))),fontSize:g_fontSize,"
"tabWidth:g_tabWidth,formatOnSave:g_formatOnSave?1:0,theme:b64e(g_themeName||'')});}"
"function applySettings(){var px=g_fontSize+'px';ed.style.fontSize=px;hl_el.style.fontSize=px;"
"gutter.style.fontSize=px;sigbox.style.fontSize=px;ed.style.tabSize=g_tabWidth;hl_el.style.tabSize=g_tabWidth;"
"document.getElementById('fsval').innerText=g_fontSize;document.getElementById('twval').innerText=g_tabWidth;"
"var fb=document.getElementById('fosbtn');if(fb){fb.innerText=g_formatOnSave?'ON':'OFF';fb.className=g_formatOnSave?'on':'';}"
"measure();renderHighlight();updateGutter();updatePos();syncScroll();updateBracket();}"
"function changeFont(d){g_fontSize=Math.max(8,Math.min(32,g_fontSize+d));applySettings();saveSession();}"
"function changeTab(d){g_tabWidth=Math.max(1,Math.min(8,g_tabWidth+d));applySettings();saveSession();}"
"function toggleSettings(ev){var m=document.getElementById('setmenu');"
"if(m.className=='on'){m.className='';return;}"
"var b=ev&&ev.currentTarget?ev.currentTarget:null;var r=b?b.getBoundingClientRect():{left:60,bottom:34};"
"m.style.left=r.left+'px';m.style.top=(r.bottom+2)+'px';m.className='on';}"
/* ---- generic edit commit ---- */
"function afterEdit(){var t=cur();if(!t)return;t.text=ed.value;markDirty();"
"renderHighlight();updateGutter();updatePos();syncScroll();scheduleChange();updateBracket();}"
"function indentUnit(){var s='';for(var i=0;i<g_tabWidth;i++)s+=' ';return s;}"
"function lineBounds(pos){var v=ed.value;var st=v.lastIndexOf('\\n',pos-1)+1;"
"var en=v.indexOf('\\n',pos);if(en<0)en=v.length;return{start:st,end:en};}"
/* ---- LSP navigation ---- */
"var pendingJump=null;"
"function doDefinition(){var t=cur();if(!t||!t.path||!t.lang){setStatus('Definition: not a code file');return;}"
"var lc=caretLineCol();M.definition.postMessage({path:t.path,line:lc.line,character:lc.col});}"
"function doReferences(){var t=cur();if(!t||!t.path||!t.lang){setStatus('References: not a code file');return;}"
"var lc=caretLineCol();M.references.postMessage({path:t.path,line:lc.line,character:lc.col});}"
"function doRename(){var t=cur();if(!t||!t.path||!t.lang){setStatus('Rename: not a code file');return;}"
"var nn=prompt('Rename symbol to:');if(!nn)return;var lc=caretLineCol();"
"M.rename.postMessage({path:t.path,line:lc.line,character:lc.col,newName:b64e(nn)});}"
"function gotoLocation(loc){if(!loc)return;"
"for(var i=0;i<tabs.length;i++){if(tabs[i].path==loc.path){activate(i);jumpTo(loc.line,loc.character);return;}}"
"pendingJump=loc;M.openPath.postMessage(loc.path);}"
"function toggleRefs(){var p=document.getElementById('refs');p.className=(p.className=='on')?'':'on';}"
"function showReferences(locs){var rl=document.getElementById('reflist');rl.innerHTML='';"
"var p=document.getElementById('refs');"
"if(!locs||!locs.length){rl.innerHTML='<div class=\"empty\">No references.</div>';p.className='on';setStatus('No references');return;}"
"locs.forEach(function(l){var e=document.createElement('div');e.className='rit';"
"var nm=l.path.split('/').pop();"
"var loc=document.createElement('span');loc.className='rloc';loc.textContent=nm+':'+(l.line+1)+':'+(l.character+1);"
"e.appendChild(loc);e.onclick=function(){gotoLocation(l);};rl.appendChild(e);});"
"p.className='on';setStatus(locs.length+' reference(s)');}"
/* ---- rename (apply WorkspaceEdit) ---- */
"var pendingRename={};"
"function lcToOff(str,line,col){var idx=0,ln=0;for(var i=0;i<str.length&&ln<line;i++){"
"if(str[i]=='\\n'){ln++;idx=i+1;}}return idx+col;}"
"function applyEditsToText(txt,edits){var wo=edits.map(function(e){"
"return{s:lcToOff(txt,e.line,e.character),e:lcToOff(txt,e.endLine,e.endCharacter),nt:e.newText};});"
"wo.sort(function(a,b){return b.s-a.s;});"
"for(var i=0;i<wo.length;i++){var w=wo[i];txt=txt.slice(0,w.s)+w.nt+txt.slice(w.e);}return txt;}"
"function applyRename(changes){if(!changes||!changes.length){setStatus('Rename: no changes');return;}"
"var files=0,edc=0;changes.forEach(function(ch){var found=false;"
"for(var i=0;i<tabs.length;i++){if(tabs[i].path==ch.path){"
"tabs[i].text=applyEditsToText(tabs[i].text,ch.edits);tabs[i].dirty=true;found=true;files++;edc+=ch.edits.length;"
"if(i==active){ed.value=tabs[i].text;renderHighlight();updateGutter();updatePos();syncScroll();scheduleChange();}"
"break;}}"
"if(!found){pendingRename[ch.path]=ch.edits;M.openPath.postMessage(ch.path);}});"
"renderTabs();setStatus('Renamed in '+files+' open file(s), '+edc+' edit(s)');}"
/* ---- signature help ---- */
"var sigTimer=null;"
"function sigHide(){sigbox.style.display='none';}"
"function requestSig(){var t=cur();if(!t||!t.path||!t.lang){sigHide();return;}"
"var lc=caretLineCol();M.signatureHelp.postMessage({path:t.path,line:lc.line,character:lc.col});}"
"function scheduleSig(){clearTimeout(sigTimer);sigTimer=setTimeout(requestSig,180);}"
"function showSig(msg){var sigs=msg.signatures;if(!sigs||!sigs.length){sigHide();return;}"
"var ai=msg.activeSignature||0;if(ai<0||ai>=sigs.length)ai=0;var s=sigs[ai];"
"var ap=(s.activeParameter!=null&&s.activeParameter>=0)?s.activeParameter:msg.activeParameter;"
"var lab=s.label||'';var html;"
"if(ap!=null&&ap>=0&&s.parameters&&s.parameters[ap]&&s.parameters[ap].label){"
"var pl=s.parameters[ap].label;var idx=lab.indexOf(pl);"
"if(idx>=0)html=esc(lab.slice(0,idx))+'<span class=\"ap\">'+esc(pl)+'</span>'+esc(lab.slice(idx+pl.length));"
"else html=esc(lab);}else html=esc(lab);"
"sigbox.innerHTML=html+(s.documentation?'<div class=\"sd\">'+esc(s.documentation)+'</div>':'');"
"sigbox.style.display='block';var lc=caretLineCol();"
"var x=PADX+lc.col*CHW-ed.scrollLeft;var y=PADY+lc.line*LNH-ed.scrollTop-6-sigbox.offsetHeight;"
"sigbox.style.left=Math.max(0,x)+'px';sigbox.style.top=Math.max(0,y)+'px';}"
/* ---- bracket matching ---- */
"var PAIRS={'(':')','[':']','{':'}'};var CLOSERS={')':'(',']':'[','}':'{'};"
"function findMatch(pos){var v=ed.value;var ch=v[pos];"
"if(PAIRS[ch]){var o=ch,cl=PAIRS[ch],d=1;for(var i=pos+1;i<v.length;i++){"
"if(v[i]==o)d++;else if(v[i]==cl){d--;if(d==0)return i;}}return -1;}"
"if(CLOSERS[ch]){var c2=ch,o2=CLOSERS[ch],d2=1;for(var j=pos-1;j>=0;j--){"
"if(v[j]==c2)d2++;else if(v[j]==o2){d2--;if(d2==0)return j;}}return -1;}"
"return -1;}"
"function boxAt(off,cls){var lc=offToLC(ed.value,off);var d=document.createElement('div');d.className=cls;"
"d.style.left=(PADX+lc.ch*CHW)+'px';d.style.top=(PADY+lc.line*LNH)+'px';"
"d.style.width=CHW+'px';d.style.height=LNH+'px';return d;}"
"function updateBracket(){brkin.innerHTML='';if(ed.selectionStart!=ed.selectionEnd)return;"
"var pos=ed.selectionStart;var v=ed.value;var cand=-1;"
"if(pos>0&&(PAIRS[v[pos-1]]||CLOSERS[v[pos-1]]))cand=pos-1;"
"else if((PAIRS[v[pos]]||CLOSERS[v[pos]]))cand=pos;"
"if(cand<0)return;var m=findMatch(cand);if(m<0)return;"
"brkin.appendChild(boxAt(cand,'brk'));brkin.appendChild(boxAt(m,'brk'));}"
/* ---- find & replace ---- */
"var findState={open:false,matchCase:false,matches:[],idx:-1};"
"function toggleFind(showRepl){var fb=document.getElementById('findbar');"
"if(findState.open){fb.className='';findState.open=false;clearFindMark();ed.focus();return;}"
"fb.className='on';findState.open=true;var fi=document.getElementById('findin');"
"var sel=ed.value.slice(ed.selectionStart,ed.selectionEnd);"
"if(sel&&sel.indexOf('\\n')<0)fi.value=sel;runFind();fi.focus();fi.select();}"
"function toggleCase(){findState.matchCase=!findState.matchCase;"
"document.getElementById('casebtn').className='tog'+(findState.matchCase?' on':'');runFind();}"
"function runFind(){var q=document.getElementById('findin').value;findState.matches=[];"
"if(q){var hay=findState.matchCase?ed.value:ed.value.toLowerCase();"
"var nd=findState.matchCase?q:q.toLowerCase();var from=0,at;"
"while((at=hay.indexOf(nd,from))>=0){findState.matches.push(at);from=at+(nd.length||1);}}"
"if(!findState.matches.length){findState.idx=-1;clearFindMark();updateFindCount();return;}"
"if(findState.idx<0||findState.idx>=findState.matches.length){"
"var cp=ed.selectionStart;findState.idx=0;"
"for(var i=0;i<findState.matches.length;i++){if(findState.matches[i]>=cp){findState.idx=i;break;}}}"
"showMatch();}"
"function findNext(dir){if(!findState.matches.length){runFind();return;}"
"findState.idx=(findState.idx+dir+findState.matches.length)%findState.matches.length;showMatch();}"
"function clearFindMark(){var fm=document.getElementById('fmin');if(fm)fm.innerHTML='';}"
"function showMatch(){var q=document.getElementById('findin').value;"
"if(findState.idx<0||!findState.matches.length){updateFindCount();return;}"
"var at=findState.matches[findState.idx];ed.selectionStart=at;ed.selectionEnd=at+q.length;"
"var lc=offToLC(ed.value,at);ed.scrollTop=Math.max(0,lc.line*LNH-ed.clientHeight/2);syncScroll();"
"var fm=document.getElementById('fmin');fm.innerHTML='';var box=document.createElement('div');box.className='fmk';"
"box.style.left=(PADX+lc.ch*CHW)+'px';box.style.top=(PADY+lc.line*LNH)+'px';"
"box.style.width=(Math.max(1,q.length)*CHW)+'px';box.style.height=LNH+'px';fm.appendChild(box);"
"updateFindCount();updatePos();}"
"function updateFindCount(){document.getElementById('findcount').innerText="
"(findState.matches.length?(findState.idx+1)+'/'+findState.matches.length:'0/0');}"
"function replaceOne(){if(findState.idx<0||!findState.matches.length)return;"
"var q=document.getElementById('findin').value;var rep=document.getElementById('replin').value;"
"var at=findState.matches[findState.idx];"
"ed.value=ed.value.slice(0,at)+rep+ed.value.slice(at+q.length);"
"ed.selectionStart=ed.selectionEnd=at+rep.length;afterEdit();runFind();"
"if(findState.open)document.getElementById('findin').focus();}"
"function replaceAll(){if(!findState.matches.length)return;"
"var q=document.getElementById('findin').value;var rep=document.getElementById('replin').value;"
"var v=ed.value,out='',from=0;var ms=findState.matches;"
"for(var i=0;i<ms.length;i++){out+=v.slice(from,ms[i])+rep;from=ms[i]+q.length;}out+=v.slice(from);"
"ed.value=out;ed.selectionStart=ed.selectionEnd=out.length;afterEdit();"
"setStatus('Replaced '+ms.length+' occurrence(s)');runFind();"
"if(findState.open)document.getElementById('findin').focus();}"
"(function(){var fi=document.getElementById('findin');var ri=document.getElementById('replin');"
"fi.addEventListener('input',function(){findState.idx=-1;runFind();});"
"fi.addEventListener('keydown',function(e){"
"if(e.key=='Enter'){e.preventDefault();findNext(e.shiftKey?-1:1);}"
"else if(e.key=='Escape'){e.preventDefault();toggleFind();}});"
"ri.addEventListener('keydown',function(e){"
"if(e.key=='Enter'){e.preventDefault();replaceOne();}"
"else if(e.key=='Escape'){e.preventDefault();toggleFind();}});})();"
/* ---- editor edit operations ---- */
"function commentToggle(){var t=cur();if(!t)return;var L=LANGS[t.hl];"
"var pfx=(L&&L.line)?L.line:'//';var v=ed.value;"
"var s=ed.selectionStart,e=ed.selectionEnd;"
"var ls=v.lastIndexOf('\\n',s-1)+1;var le=v.indexOf('\\n',e);if(le<0)le=v.length;"
"if(e>s&&v[e-1]=='\\n'&&e-1>=ls){}"
"var block=v.slice(ls,le);var lines=block.split('\\n');"
"var allC=true;for(var i=0;i<lines.length;i++){var ln=lines[i];"
"if(ln.trim()===''){continue;}if(ln.replace(/^\\s*/,'').indexOf(pfx)!==0){allC=false;break;}}"
"var nl=lines.map(function(ln){if(ln.trim()===''){return ln;}"
"if(allC){var idx=ln.indexOf(pfx);var after=ln.slice(idx+pfx.length);"
"if(after[0]==' ')after=after.slice(1);return ln.slice(0,idx)+after;}"
"else{var m=ln.match(/^\\s*/)[0];return m+pfx+' '+ln.slice(m.length);}});"
"var rep=nl.join('\\n');ed.value=v.slice(0,ls)+rep+v.slice(le);"
"ed.selectionStart=ls;ed.selectionEnd=ls+rep.length;afterEdit();}"
"function dupLine(){var v=ed.value;var s=ed.selectionStart,e=ed.selectionEnd;"
"var ls=v.lastIndexOf('\\n',s-1)+1;var le=v.indexOf('\\n',e);if(le<0)le=v.length;"
"var block=v.slice(ls,le);"
"ed.value=v.slice(0,le)+'\\n'+block+v.slice(le);"
"var np=s+block.length+1;ed.selectionStart=ed.selectionEnd=np;afterEdit();}"
"function moveLine(dir){var v=ed.value;var s=ed.selectionStart,e=ed.selectionEnd;"
"var ls=v.lastIndexOf('\\n',s-1)+1;var le=v.indexOf('\\n',e);if(le<0)le=v.length;"
"if(dir<0){if(ls==0)return;var ps=v.lastIndexOf('\\n',ls-2)+1;"
"var prev=v.slice(ps,ls);var block=v.slice(ls,le);"
"var nv=v.slice(0,ps)+block+'\\n'+prev.slice(0,prev.length-1)+v.slice(le);"
"ed.value=nv;var d=ps-ls;ed.selectionStart=s+d;ed.selectionEnd=e+d;afterEdit();}"
"else{if(le>=v.length)return;var ne=v.indexOf('\\n',le+1);if(ne<0)ne=v.length;"
"var next=v.slice(le+1,ne);var block=v.slice(ls,le);"
"var nv=v.slice(0,ls)+next+'\\n'+block+v.slice(ne);"
"ed.value=nv;var d=next.length+1;ed.selectionStart=s+d;ed.selectionEnd=e+d;afterEdit();}}"
"document.addEventListener('keydown',function(e){"
"var k=e.key.toLowerCase();"
"if(e.ctrlKey&&e.shiftKey&&k=='p'){e.preventDefault();openPalette();return;}"
"if(e.ctrlKey&&!e.shiftKey&&k=='p'){e.preventDefault();openFinder();return;}"
"if(e.ctrlKey&&e.shiftKey&&k=='f'){e.preventDefault();toggleSearch(true);return;}"
"if(e.ctrlKey&&(e.key=='`'||k=='`')){e.preventDefault();toggleTerminal();return;}"
"if(e.altKey&&e.shiftKey&&k=='f'){e.preventDefault();doFormat();return;}"
"if(e.ctrlKey&&!e.shiftKey&&k=='s'){e.preventDefault();save();return;}"
"if(e.ctrlKey&&e.shiftKey&&k=='s'){e.preventDefault();saveAs();return;}"
"if(e.ctrlKey&&k=='n'){e.preventDefault();newFile();return;}"
"if(e.ctrlKey&&k=='f'){e.preventDefault();toggleFind();return;}"
"if(e.ctrlKey&&k=='h'){e.preventDefault();if(!findState.open)toggleFind();"
"document.getElementById('replin').focus();return;}"
"if(e.ctrlKey&&k=='/'){e.preventDefault();commentToggle();return;}"
"if(e.ctrlKey&&e.shiftKey&&k=='d'){e.preventDefault();dupLine();return;}"
"if(e.altKey&&e.key=='ArrowUp'){e.preventDefault();moveLine(-1);return;}"
"if(e.altKey&&e.key=='ArrowDown'){e.preventDefault();moveLine(1);return;}"
"if(e.key=='F1'){e.preventDefault();doHover();return;}"
"if(e.key=='F2'){e.preventDefault();doRename();return;}"
"if(e.key=='F12'){e.preventDefault();if(e.shiftKey)doReferences();else doDefinition();return;}"
"if(e.key=='Escape'){"
"var ex=document.getElementById('extov');if(ex.className.indexOf('on')>=0){closeExt();return;}"
"if(findState.open){toggleFind();return;}sigHide();"
"var sm=document.getElementById('setmenu');if(sm.className=='on')sm.className='';}});"
/* ---- file tree ---- */
"var treeEl=document.getElementById('tree');var dirData={};"
"function renderTree(){treeEl.innerHTML='';renderLevel(window.__root,treeEl,0);}"
"function renderLevel(path,container,depth){var entries=dirData[path];if(!entries)return;"
"entries.forEach(function(e){var node=document.createElement('div');"
"node.className='node '+(e.dir?'dir':'file');node.style.paddingLeft=(6+depth*12)+'px';"
"node.innerText=(e.dir?'\\u25B8 ':'  ')+e.name;"
"container.appendChild(node);"
"if(e.dir){var sub=document.createElement('div');sub.style.display='none';container.appendChild(sub);"
"node.onclick=function(){if(sub.style.display=='none'){sub.style.display='block';"
"node.innerText='\\u25BE '+e.name;if(!dirData[e.path]){pendingParent[e.path]={sub:sub,depth:depth+1};"
"M.listDir.postMessage(e.path);}else renderLevel(e.path,sub,depth+1);}"
"else{sub.style.display='none';node.innerText='\\u25B8 '+e.name;}};}"
"else{node.onclick=function(){M.openPath.postMessage(e.path);};}});}"
"var pendingParent={};"
/* ---- receive from C ---- */
"window.__recv=function(b){var msg=JSON.parse(b64d(b));var c=msg.cmd;"
"if(c=='folder'){window.__root=msg.root;dirData={};treeEl.innerHTML='';"
"document.getElementById('rootlabel').innerText=msg.name.toUpperCase();"
"setStatus('FOLDER: '+msg.root);}"
"else if(c=='dir'){dirData[msg.path]=msg.entries;"
"if(msg.path==window.__root){renderTree();}"
"else if(pendingParent[msg.path]){var pp=pendingParent[msg.path];"
"renderLevel(msg.path,pp.sub,pp.depth);delete pendingParent[msg.path];}}"
"else if(c=='file'){openTab(msg.path,msg.name,b64d(msg.content),msg.lang,msg.readonly);"
"if(pendingRename[msg.path]){var t=cur();if(t&&t.path==msg.path){"
"t.text=applyEditsToText(t.text,pendingRename[msg.path]);t.dirty=true;ed.value=t.text;"
"renderHighlight();updateGutter();updatePos();syncScroll();scheduleChange();renderTabs();}delete pendingRename[msg.path];}"
"if(pendingJump&&pendingJump.path==msg.path){jumpTo(pendingJump.line,pendingJump.character);pendingJump=null;}}"
"else if(c=='saved'){var t=cur();if(t){if(msg.path)t.path=msg.path;"
"if(msg.name)t.name=msg.name;if(msg.lang!==undefined)t.lang=msg.lang;"
"t.hl=hlLangFor(t.name);t.dirty=false;t.sentText=t.text;renderTabs();renderHighlight();saveSession();}setStatus('SAVED');}"
"else if(c=='status'){setStatus(msg.text);}"
"else if(c=='hover'){showHover(msg.text);}"
"else if(c=='completion'){acShow(msg.items);}"
"else if(c=='diagnostics'){diagStore[msg.path]=msg.items;renderDiagnostics();}"
"else if(c=='settings'){if(msg.fontSize)g_fontSize=msg.fontSize;if(msg.tabWidth)g_tabWidth=msg.tabWidth;"
"if(msg.formatOnSave!==undefined)g_formatOnSave=!!msg.formatOnSave;applySettings();}"
"else if(c=='definition'){var L=msg.locations;if(!L||!L.length){setStatus('No definition found');}"
"else if(L.length==1){gotoLocation(L[0]);}else{showReferences(L);}}"
"else if(c=='references'){showReferences(msg.locations);}"
"else if(c=='signatureHelp'){showSig(msg);}"
"else if(c=='rename'){applyRename(msg.changes);}"
"else if(c=='format'){applyFormat(msg);}"
"else if(c=='fuzzy'){if(qp.mode=='file')qpRenderFiles(msg.items);}"
"else if(c=='search'){showSearch(msg);}"
"else if(c=='termout'){termFeed(b64d(msg.data));}"
"else if(c=='termexit'){term.started=false;termFeed('\\r\\n[process exited - reopen terminal to restart]\\r\\n');}"
"else if(c=='themes'){g_themes=msg.items||[];rebuildThemeCmds();}"
"else if(c=='extensions'){g_exts=msg.items||[];}"
"else if(c=='applyTheme'){applyThemeById(msg.id);}};"
"function showHover(text){var h=document.getElementById('hover');"
"if(!text){h.className='';h.innerHTML='';setStatus('No hover info');return;}"
"h.className='on';var e=text.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');"
"h.innerHTML='<div class=\"h\">HOVER</div>'+e;}"
"function showAbout(){document.getElementById('aboutov').classList.add('on');}"
"function closeAbout(){document.getElementById('aboutov').classList.remove('on');}"
/* ==== IDE: formatting ==== */
"function doFormat(){var t=cur();if(!t||!t.path||!t.lang){setStatus('Format: not a code file');return;}"
"t.text=ed.value;setStatus('Formatting...');M.format.postMessage({path:t.path});}"
"function applyFormat(msg){var path=msg.path,edits=msg.edits;"
"var done=function(){if(pendingFormatSave&&pendingFormatSave==path){pendingFormatSave=null;realSave();}};"
"if(!edits||!edits.length){setStatus('Format: no changes');done();return;}"
"for(var i=0;i<tabs.length;i++){if(tabs[i].path==path){"
"tabs[i].text=applyEditsToText(tabs[i].text,edits);tabs[i].dirty=true;"
"if(i==active){var sp=ed.selectionStart;ed.value=tabs[i].text;"
"ed.selectionStart=ed.selectionEnd=Math.min(sp,ed.value.length);"
"renderHighlight();updateGutter();updatePos();syncScroll();scheduleChange();}"
"renderTabs();break;}}"
"setStatus('Formatted ('+edits.length+' edit(s))');done();}"
"function toggleFormatOnSave(){g_formatOnSave=!g_formatOnSave;"
"var fb=document.getElementById('fosbtn');fb.innerText=g_formatOnSave?'ON':'OFF';"
"fb.className=g_formatOnSave?'on':'';saveSession();setStatus('Format on save: '+(g_formatOnSave?'ON':'OFF'));}"
/* ==== IDE: command palette + fuzzy finder ==== */
"var qp={mode:'',items:[],sel:0};"
"function fuzzyScore(pat,str){pat=pat.toLowerCase();str=str.toLowerCase();"
"var pi=0,score=0,prev=-2,sep=true;"
"for(var si=0;si<str.length&&pi<pat.length;si++){var c=str[si];"
"if(pat[pi]==c){score++;if(si==prev+1)score+=2;if(sep)score+=5;if(si==0)score+=2;prev=si;pi++;}"
"sep=(c=='/'||c=='_'||c=='-'||c=='.'||c==' '||c=='\\\\');}"
"if(pi<pat.length)return -1;return score;}"
"var CMDS=["
"{t:'Open Folder',k:'',run:function(){M.openFolder.postMessage('');}},"
"{t:'Open File',k:'',run:function(){M.openFile.postMessage('');}},"
"{t:'New File',k:'Ctrl+N',run:newFile},"
"{t:'Save',k:'Ctrl+S',run:save},"
"{t:'Save As',k:'Ctrl+Shift+S',run:saveAs},"
"{t:'Format Document',k:'Shift+Alt+F',run:doFormat},"
"{t:'Toggle Format on Save',k:'',run:toggleFormatOnSave},"
"{t:'Go to File',k:'Ctrl+P',run:openFinder},"
"{t:'Search in Files',k:'Ctrl+Shift+F',run:function(){toggleSearch(true);}},"
"{t:'Find',k:'Ctrl+F',run:function(){toggleFind();}},"
"{t:'Replace',k:'Ctrl+H',run:function(){if(!findState.open)toggleFind();document.getElementById('replin').focus();}},"
"{t:'Toggle Terminal',k:'Ctrl+`',run:toggleTerminal},"
"{t:'Toggle Problems',k:'',run:toggleProblems},"
"{t:'Go to Definition',k:'F12',run:doDefinition},"
"{t:'Find References',k:'Shift+F12',run:doReferences},"
"{t:'Rename Symbol',k:'F2',run:doRename},"
"{t:'Hover / Quick Info',k:'F1',run:doHover},"
"{t:'Autocomplete',k:'Ctrl+Space',run:function(){requestCompletion(true);}},"
"{t:'Toggle Line Comment',k:'Ctrl+/',run:commentToggle},"
"{t:'Duplicate Line',k:'Ctrl+Shift+D',run:dupLine},"
"{t:'Move Line Up',k:'Alt+Up',run:function(){moveLine(-1);}},"
"{t:'Move Line Down',k:'Alt+Down',run:function(){moveLine(1);}},"
"{t:'Dev: Run File',k:'',run:function(){devRun('run');}},"
"{t:'Dev: File Info',k:'',run:function(){devRun('info');}},"
"{t:'Dev: Run (produce binary)',k:'',run:devRunBinary},"
"{t:'Increase Font Size',k:'',run:function(){changeFont(1);}},"
"{t:'Decrease Font Size',k:'',run:function(){changeFont(-1);}},"
"{t:'Settings',k:'',run:function(){toggleSettings(null);}},"
"{t:'About Architect',k:'',run:showAbout}"
"];"
/* ==== IDE: extension themes (adopted from user extensions dir) ==== */
"var g_themes=[],g_exts=[],g_themeName='';"
"var themeStyle=document.createElement('style');document.head.appendChild(themeStyle);"
/* safeColor: only allow hex colors into the CSS override (blocks CSS
   injection from a third-party theme file) */
"function safeColor(c){return(typeof c=='string'&&/^#[0-9a-fA-F]{3,8}$/.test(c))?c:'';}"
"function findTheme(id){for(var i=0;i<g_themes.length;i++)if(g_themes[i].id==id)return g_themes[i];return null;}"
"function applyThemeObj(t){if(!t)return;var bg=safeColor(t.bg),fg=safeColor(t.fg);"
"var hw=document.getElementById('hlwrap'),ew=document.getElementById('edwrap');"
"if(bg){hw.style.background=bg;ew.style.background=bg;gutter.style.background=bg;}"
"if(fg){hl_el.style.color=fg;ed.style.caretColor=fg;}"
"var css='';if(safeColor(t.kw))css+='.tk{color:'+safeColor(t.kw)+';}';"
"if(safeColor(t.str))css+='.ts{color:'+safeColor(t.str)+';}';"
"if(safeColor(t.cm))css+='.tc{color:'+safeColor(t.cm)+';}';"
"if(safeColor(t.num))css+='.tn{color:'+safeColor(t.num)+';}';"
"if(safeColor(t.pp))css+='.tp{color:'+safeColor(t.pp)+';}';"
"themeStyle.textContent=css;g_themeName=t.id;renderHighlight();syncScroll();"
"setStatus('Theme: '+t.name);saveSession();}"
"function applyThemeById(id){var t=findTheme(id);if(t)applyThemeObj(t);}"
"function resetTheme(){themeStyle.textContent='';"
"var hw=document.getElementById('hlwrap'),ew=document.getElementById('edwrap');"
"hw.style.background='';ew.style.background='';gutter.style.background='';"
"hl_el.style.color='';ed.style.caretColor='';g_themeName='';"
"renderHighlight();syncScroll();setStatus('Theme: default (dark green)');saveSession();}"
"function rebuildThemeCmds(){for(var i=CMDS.length-1;i>=0;i--)if(CMDS[i].xtheme)CMDS.splice(i,1);"
"CMDS.push({t:'Theme: Default (dark green)',k:'',xtheme:true,run:resetTheme});"
"g_themes.forEach(function(t){CMDS.push({t:'Theme: '+t.name+(t.ext?'  ['+t.ext+']':''),"
"k:'',xtheme:true,run:function(){applyThemeById(t.id);}});});"
"CMDS.push({t:'Extensions: Show Loaded',k:'',xtheme:true,run:showExtensions});}"
"function showExtensions(){var box=document.getElementById('extbody');var h='';"
"if(!g_exts.length){h='<div>No extensions loaded.</div>"
"<div class=\"esub\">Drop a .vsix or .tar into ~/.architect and restart Architect.</div>';}"
"else{g_exts.forEach(function(e){h+='<div class=\"erow\"><b>'+esc(e.display)+'</b>'+"
"(e.version?' <span class=\"esub\">v'+esc(e.version)+'</span>':'')+"
"'<div class=\"esub\">'+e.themes+' theme(s), '+e.snippets+' snippet(s)'+"
"(e.snippets?' (snippets listed, not inserted)':'')+'</div></div>';});}"
"if(g_themes.length){h+='<div class=\"ehdr\">Loaded themes (Command Palette \\u2192 Theme:)</div>';"
"g_themes.forEach(function(t){h+='<div class=\"erow\">'+esc(t.name)+"
"' <span class=\"esub\">['+esc(t.ext)+']</span></div>';});}"
"box.innerHTML=h;document.getElementById('extov').classList.add('on');}"
"function closeExt(){document.getElementById('extov').classList.remove('on');}"
"function qClose(){document.getElementById('qov').classList.remove('on');qp.mode='';ed.focus();}"
"function qpNav(d){if(!qp.items.length)return;qp.sel=(qp.sel+d+qp.items.length)%qp.items.length;"
"var rows=document.getElementById('qlist').children;"
"for(var i=0;i<rows.length;i++)rows[i].className='qrow'+(i==qp.sel?' sel':'');"
"if(rows[qp.sel])rows[qp.sel].scrollIntoView({block:'nearest'});}"
"function qpAccept(){var it=qp.items[qp.sel];qClose();if(!it)return;"
"if(qp.mode=='cmd')it.run();else if(qp.mode=='file')M.openPath.postMessage(it.path);}"
"function openPalette(){qp.mode='cmd';document.getElementById('qov').classList.add('on');"
"var qin=document.getElementById('qin');qin.value='';qin.placeholder='Type a command...';"
"qpRenderCmd('');qin.focus();}"
"function qpRenderCmd(q){var list=[];for(var i=0;i<CMDS.length;i++){var s=q?fuzzyScore(q,CMDS[i].t):0;"
"if(s>=0)list.push({c:CMDS[i],s:s});}list.sort(function(a,b){return b.s-a.s;});"
"qp.items=list.map(function(x){return x.c;});qp.sel=0;"
"var lst=document.getElementById('qlist');lst.innerHTML='';"
"if(!qp.items.length){lst.innerHTML='<div id=\"qempty\">No commands</div>';return;}"
"qp.items.forEach(function(c,i){var d=document.createElement('div');d.className='qrow'+(i==0?' sel':'');"
"var t=document.createElement('span');t.textContent=c.t;d.appendChild(t);"
"if(c.k){var kk=document.createElement('span');kk.className='qkey';kk.textContent=c.k;d.appendChild(kk);}"
"d.onmousedown=function(e){e.preventDefault();qp.sel=i;qpAccept();};lst.appendChild(d);});}"
"function openFinder(){qp.mode='file';document.getElementById('qov').classList.add('on');"
"var qin=document.getElementById('qin');qin.value='';qin.placeholder='Go to file...';"
"qp.items=[];qp.sel=0;document.getElementById('qlist').innerHTML='';"
"M.fuzzyFind.postMessage({q:b64e('')});qin.focus();}"
"function qpRenderFiles(items){qp.items=items||[];qp.sel=0;"
"var lst=document.getElementById('qlist');lst.innerHTML='';"
"if(!qp.items.length){lst.innerHTML='<div id=\"qempty\">No files</div>';return;}"
"qp.items.forEach(function(it,i){var d=document.createElement('div');d.className='qrow'+(i==0?' sel':'');"
"var nm=it.rel.split('/').pop();"
"var t=document.createElement('span');t.textContent=nm;d.appendChild(t);"
"var sub=document.createElement('span');sub.className='qsub';sub.textContent=it.rel;d.appendChild(sub);"
"d.onmousedown=function(e){e.preventDefault();qp.sel=i;qpAccept();};lst.appendChild(d);});}"
"(function(){var qin=document.getElementById('qin');"
"qin.addEventListener('input',function(){if(qp.mode=='cmd')qpRenderCmd(qin.value);"
"else if(qp.mode=='file')M.fuzzyFind.postMessage({q:b64e(qin.value)});});"
"qin.addEventListener('keydown',function(e){"
"if(e.key=='ArrowDown'){e.preventDefault();qpNav(1);}"
"else if(e.key=='ArrowUp'){e.preventDefault();qpNav(-1);}"
"else if(e.key=='Enter'){e.preventDefault();qpAccept();}"
"else if(e.key=='Escape'){e.preventDefault();qClose();}"
"e.stopPropagation();});})();"
/* ==== IDE: project-wide search ==== */
"var searchOpt={case:false,re:false};"
"function toggleSearch(force){var p=document.getElementById('search');"
"if(p.className=='on'&&!force){p.className='';return;}p.className='on';"
"setTimeout(function(){var q=document.getElementById('searchq');q.focus();q.select();},30);}"
"function toggleSearchOpt(w){searchOpt[w]=!searchOpt[w];"
"var id=w=='case'?'searchcase':'searchre';"
"document.getElementById(id).className=searchOpt[w]?'on':'';runProjectSearch();}"
"function runProjectSearch(){var q=document.getElementById('searchq').value;"
"var res=document.getElementById('searchres');"
"if(!q){res.innerHTML='<div class=\"empty\">Type to search across the project.</div>';return;}"
"res.innerHTML='<div class=\"empty\">Searching...</div>';"
"M.projectSearch.postMessage({q:b64e(q),regex:searchOpt.re?1:0,case:searchOpt.case?1:0});}"
"function showSearch(msg){var res=document.getElementById('searchres');res.innerHTML='';"
"if(msg.error){res.innerHTML='<div class=\"empty\">Error: '+esc(msg.error)+'</div>';return;}"
"var items=msg.items||[];if(!items.length){res.innerHTML='<div class=\"empty\">No results.</div>';return;}"
"var total=0;items.forEach(function(f){var head=document.createElement('div');head.className='sfile';"
"head.textContent=f.rel+'  ('+f.matches.length+')';res.appendChild(head);"
"f.matches.forEach(function(m){total++;var d=document.createElement('div');d.className='smatch';"
"var loc=document.createElement('span');loc.className='sloc';loc.textContent=(m.line+1)+':'+(m.col+1)+'  ';"
"d.appendChild(loc);d.appendChild(document.createTextNode(b64d(m.text)));"
"d.onclick=function(){gotoLocation({path:f.path,line:m.line,character:m.col});};res.appendChild(d);});});"
"setStatus('Search: '+total+' match(es) in '+items.length+' file(s)');}"
"(function(){var q=document.getElementById('searchq');"
"q.addEventListener('keydown',function(e){if(e.key=='Enter'){e.preventDefault();runProjectSearch();}"
"else if(e.key=='Escape'){e.preventDefault();toggleSearch();ed.focus();}e.stopPropagation();});})();"
/* ==== IDE: integrated terminal (minimal VT over a PTY) ==== */
"var term={lines:[[]],row:0,col:0,fg:'',bold:false,cls:'',started:false,open:false};"
"function termReset(){term.lines=[[]];term.row=0;term.col=0;term.fg='';term.bold=false;term.cls='';}"
"function termClsUpd(){var c='';if(term.bold)c='b';if(term.fg){if(c)c+=' ';c+='tf'+term.fg;}term.cls=c;}"
"function termRow(r){while(term.lines.length<=r)term.lines.push([]);}"
"function termPut(ch){termRow(term.row);var line=term.lines[term.row];"
"while(line.length<term.col)line.push([' ','']);line[term.col]=[ch,term.cls];term.col++;}"
"function termCsi(cmd,params){var ps=params.split(';').map(function(x){return x===''?0:(parseInt(x,10)||0);});"
"var n=ps[0]||0;"
"if(cmd=='m'){for(var i=0;i<ps.length;i++){var c=ps[i];"
"if(c==0){term.fg='';term.bold=false;}else if(c==1){term.bold=true;}else if(c==22){term.bold=false;}"
"else if(c==39){term.fg='';}else if((c>=30&&c<=37)||(c>=90&&c<=97))term.fg=''+c;}termClsUpd();}"
"else if(cmd=='K'){termRow(term.row);var l=term.lines[term.row];"
"if(n==0){if(l.length>term.col)l.length=term.col;}"
"else if(n==1){for(var k=0;k<term.col&&k<l.length;k++)l[k]=[' ',''];}else if(n==2)l.length=0;}"
"else if(cmd=='J'){if(n==2||n==3){termReset();}else if(n==0){term.lines.length=term.row+1;"
"var l2=term.lines[term.row];if(l2.length>term.col)l2.length=term.col;}}"
"else if(cmd=='C'){term.col+=(n||1);}"
"else if(cmd=='D'){term.col=Math.max(0,term.col-(n||1));}"
"else if(cmd=='G'){term.col=Math.max(0,(n||1)-1);}"
"else if(cmd=='H'||cmd=='f'){term.col=Math.max(0,(ps[1]||1)-1);}}"
"function termFeed(s){var i=0,n=s.length;"
"while(i<n){var c=s[i];"
"if(c=='\\x1b'){var nx=s[i+1];"
"if(nx=='['){var j=i+2,p='';while(j<n&&!/[A-Za-z@]/.test(s[j])){p+=s[j];j++;}"
"if(j<n){termCsi(s[j],p);i=j+1;}else i=n;continue;}"
"else if(nx==']'){var j2=i+2;while(j2<n&&s[j2]!='\\x07'&&!(s[j2]=='\\x1b'&&s[j2+1]=='\\\\'))j2++;"
"if(s[j2]=='\\x1b')j2++;i=j2+1;continue;}else{i+=2;continue;}}"
"var code=c.charCodeAt(0);"
"if(c=='\\n'){term.row++;term.col=0;termRow(term.row);}"
"else if(c=='\\r'){term.col=0;}"
"else if(code==8){if(term.col>0)term.col--;}"
"else if(code==9){var sp=8-(term.col%8);for(var t2=0;t2<sp;t2++)termPut(' ');}"
"else if(code>=32){termPut(c);}"
"i++;}"
"if(term.lines.length>5000){var drop=term.lines.length-5000;term.lines.splice(0,drop);"
"term.row-=drop;if(term.row<0)term.row=0;}"
"termRender();}"
"function termRender(){var el=document.getElementById('termscreen');var out='';"
"for(var r=0;r<term.lines.length;r++){var line=term.lines[r];var cur=null,buf='',seg='';"
"for(var c=0;c<line.length;c++){var cell=line[c],ch=cell[0],cl=cell[1];"
"if(cl!==cur){if(buf){seg+=cur?'<span class=\"'+cur+'\">'+esc(buf)+'</span>':esc(buf);buf='';}cur=cl;}buf+=ch;}"
"if(buf)seg+=cur?'<span class=\"'+cur+'\">'+esc(buf)+'</span>':esc(buf);out+=seg+'\\n';}"
"el.innerHTML=out;el.scrollTop=el.scrollHeight;}"
"function termSendResize(){var el=document.getElementById('termscreen');"
"var cols=Math.max(20,Math.floor(el.clientWidth/CHW));"
"var rows=Math.max(5,Math.floor(el.clientHeight/LNH));"
"M.termResize.postMessage({cols:cols,rows:rows});}"
"function toggleTerminal(){var p=document.getElementById('terminal');"
"if(term.open){p.classList.remove('on');term.open=false;return;}"
"p.classList.add('on');term.open=true;"
"if(!term.started){term.started=true;termReset();termRender();M.termStart.postMessage('');}"
"setTimeout(function(){document.getElementById('termscreen').focus();termSendResize();},60);}"
"function termKey(e){if(!term.started)return;var k=e.key,b='';"
"if(k.length==1){if(e.ctrlKey){var cc=k.toLowerCase().charCodeAt(0);"
"if(cc>=97&&cc<=122)b=String.fromCharCode(cc-96);else if(k==' ')b='\\x00';else b=k;}else b=k;}"
"else if(k=='Enter')b='\\r';else if(k=='Backspace')b='\\x7f';else if(k=='Tab')b='\\t';"
"else if(k=='Escape')b='\\x1b';else if(k=='ArrowUp')b='\\x1b[A';else if(k=='ArrowDown')b='\\x1b[B';"
"else if(k=='ArrowRight')b='\\x1b[C';else if(k=='ArrowLeft')b='\\x1b[D';"
"else if(k=='Home')b='\\x1b[H';else if(k=='End')b='\\x1b[F';else if(k=='Delete')b='\\x1b[3~';"
"else if(k=='PageUp')b='\\x1b[5~';else if(k=='PageDown')b='\\x1b[6~';else return;"
"e.preventDefault();e.stopPropagation();M.termInput.postMessage({data:b64e(b)});}"
"(function(){var el=document.getElementById('termscreen');"
"el.addEventListener('keydown',termKey);"
"window.addEventListener('resize',function(){if(term.open)termSendResize();});})();"
"function runInTerminal(cmd){var fresh=!term.started;"
"if(!term.open)toggleTerminal();"
"else if(!term.started){term.started=true;termReset();termRender();M.termStart.postMessage('');}"
"setTimeout(function(){M.termInput.postMessage({data:b64e(cmd+String.fromCharCode(10))});},fresh?450:30);}"
"function shq(s){var q=String.fromCharCode(39);"
"return q+String(s).split(q).join(q+String.fromCharCode(92)+q+q)+q;}"
"function devRun(mode){var t=cur();if(!t||!t.path){setStatus('Dev: save the file first');return;}"
"runInTerminal('dev '+mode+' '+shq(t.path));}"
"function devRunBinary(){var t=cur();if(!t||!t.path){setStatus('Dev: save the file first');return;}"
"runInTerminal('dev run '+shq(t.path)+' --produce-binary');}"
"ed.disabled=true;measure();applySettings();renderHighlight();updateGutter();"
"</script>"
"<div id='qov' onclick='if(event.target===this)qClose()'>"
"<div id='qbox'><input id='qin' spellcheck='false'><div id='qlist'></div></div>"
"</div>"
"<div id='aboutov' onclick='if(event.target===this)closeAbout()'>"
"<div id='aboutbox'>"
"<div class='atitle'>Architect &mdash; part of LeviathanOS</div>"
"Free software under the GNU General Public License, version 3.<br>"
"This program comes with ABSOLUTELY NO WARRANTY.<br>"
"Full license: /usr/share/doc/leviathanos/LICENSE<br>"
"https://www.gnu.org/licenses/gpl-3.0.html"
"<div class='aok'><button class='btn' onclick='closeAbout()'>OK</button></div>"
"</div></div>"
"<div id='extov' onclick='if(event.target===this)closeExt()'>"
"<div id='extbox'>"
"<div class='etitle'>&#9670; EXTENSIONS &mdash; adopted from ~/.architect</div>"
"<div id='extbody'></div>"
"<div class='esub' style='margin-top:10px;'>Architect adopts VSCode color themes"
" (and lists snippet contributions) from .vsix/.tar bundles. It does not run"
" VSCode extension code.</div>"
"<div class='eok'><button class='btn' onclick='closeExt()'>OK</button></div>"
"</div></div>"
"</body></html>";

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
struct StartupArg { ArchitectApp* app; std::string path; };

static void on_load_changed(WebKitWebView*, WebKitLoadEvent ev, gpointer p) {
    StartupArg* sa = static_cast<StartupArg*>(p);
    ArchitectApp* app = sa->app;
    if (ev == WEBKIT_LOAD_FINISHED && !app->webReady) {
        app->webReady = true;

        // Push persisted editor settings to the front-end first.
        std::string sbody = "{\"cmd\":\"settings\",\"fontSize\":" +
                            std::to_string(app->fontSize) + ",\"tabWidth\":" +
                            std::to_string(app->tabWidth) + ",\"formatOnSave\":" +
                            (app->formatOnSave ? "true" : "false") + "}";
        send_json(app, sbody);

        // Push adopted extension themes + the extensions list to the front-end.
        send_extensions(app);

        if (!sa->path.empty()) {
            struct stat st;
            if (stat(sa->path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                app->projectRoot = sa->path;
                std::string name = sa->path;
                size_t slash = name.find_last_of('/');
                if (slash != std::string::npos && slash + 1 < name.size())
                    name = name.substr(slash + 1);
                std::string body = "{\"cmd\":\"folder\",\"root\":\"" + json::escape(sa->path) +
                                   "\",\"name\":\"" + json::escape(name) + "\"}";
                send_json(app, body);
                send_json(app, list_dir_json(sa->path));
            } else {
                // Treat as a file; open its directory as the project too.
                std::string dir = sa->path;
                size_t slash = dir.find_last_of('/');
                if (slash != std::string::npos) {
                    app->projectRoot = dir.substr(0, slash);
                }
                open_file_into_tab(app, sa->path);
            }
        } else if (!app->restoredSession) {
            // No CLI argument: reopen the last folder + files from the session.
            app->restoredSession = true;
            if (!app->projectRoot.empty()) {
                struct stat st;
                if (stat(app->projectRoot.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    std::string name = app->projectRoot;
                    size_t slash = name.find_last_of('/');
                    if (slash != std::string::npos && slash + 1 < name.size())
                        name = name.substr(slash + 1);
                    std::string body = "{\"cmd\":\"folder\",\"root\":\"" +
                                       json::escape(app->projectRoot) + "\",\"name\":\"" +
                                       json::escape(name) + "\"}";
                    send_json(app, body);
                    send_json(app, list_dir_json(app->projectRoot));
                } else {
                    app->projectRoot.clear();
                }
            }
            for (const auto& f : app->openFiles) {
                struct stat st;
                if (stat(f.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                    open_file_into_tab(app, f);
            }
        }
    }
}

static gboolean on_delete(GtkWidget*, GdkEvent*, gpointer p) {
    ArchitectApp* app = static_cast<ArchitectApp*>(p);
    for (auto& kv : app->lsps) if (kv.second) kv.second->shutdown();
    term_stop(app);
    return FALSE;
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    ArchitectApp* app = new ArchitectApp();
    load_config(app);       // restore persisted settings + last session
    scan_extensions(app);   // adopt themes/snippets from ~/.architect (*.vsix/*.tar)
    app->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(app->window, "Architect - LeviathanOS Code Editor");
    gtk_window_set_default_size(app->window, 1100, 760);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    app->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_container_add(GTK_CONTAINER(app->window), GTK_WIDGET(app->web_view));

    WebKitSettings* settings = webkit_settings_new();
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
    webkit_web_view_set_settings(app->web_view, settings);

    WebKitUserContentManager* mgr =
        webkit_web_view_get_user_content_manager(app->web_view);
    struct { const char* name; GCallback cb; } handlers[] = {
        {"openFolder", G_CALLBACK(on_openFolder)},
        {"openFile",   G_CALLBACK(on_openFile)},
        {"listDir",    G_CALLBACK(on_listDir)},
        {"openPath",   G_CALLBACK(on_openPath)},
        {"save",       G_CALLBACK(on_save)},
        {"saveAs",     G_CALLBACK(on_saveAs)},
        {"change",     G_CALLBACK(on_change)},
        {"changeRange",G_CALLBACK(on_changeRange)},
        {"hover",      G_CALLBACK(on_hover)},
        {"complete",   G_CALLBACK(on_complete)},
        {"definition", G_CALLBACK(on_definition)},
        {"references", G_CALLBACK(on_references)},
        {"signatureHelp", G_CALLBACK(on_signatureHelp)},
        {"rename",     G_CALLBACK(on_rename)},
        {"format",     G_CALLBACK(on_format)},
        {"fuzzyFind",  G_CALLBACK(on_fuzzyFind)},
        {"projectSearch", G_CALLBACK(on_projectSearch)},
        {"termStart",  G_CALLBACK(on_termStart)},
        {"termInput",  G_CALLBACK(on_termInput)},
        {"termResize", G_CALLBACK(on_termResize)},
        {"saveState",  G_CALLBACK(on_saveState)},
        {"log",        G_CALLBACK(on_log)},
    };
    for (auto& h : handlers) {
        webkit_user_content_manager_register_script_message_handler(mgr, h.name);
        std::string sig = std::string("script-message-received::") + h.name;
        g_signal_connect(mgr, sig.c_str(), h.cb, app);
    }

    StartupArg* sa = new StartupArg{app, argc > 1 ? argv[1] : ""};
    g_signal_connect(app->web_view, "load-changed",
                     G_CALLBACK(on_load_changed), sa);

    webkit_web_view_load_html(app->web_view, HTML, nullptr);
    gtk_widget_show_all(GTK_WIDGET(app->window));

    gtk_main();

    for (auto& kv : app->lsps) if (kv.second) kv.second->shutdown();
    term_stop(app);
    delete app;
    delete sa;
    return 0;
}
