// lsp.cpp - implementation of the minimal LSP client. See lsp.hpp.
#include "lsp.hpp"
#include "json.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// ---------------------------------------------------------------------------
// Framing helpers (also used directly by the unit test).
// ---------------------------------------------------------------------------
std::string LspClient::frame(const std::string& body) {
    std::string out = "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    return out;
}

bool LspClient::parseFrame(const std::string& buf, size_t& offset,
                           std::string& body) {
    // Find header terminator (\r\n\r\n) starting at offset.
    size_t headerEnd = buf.find("\r\n\r\n", offset);
    if (headerEnd == std::string::npos) return false;

    // Scan headers for Content-Length (case-insensitive key).
    size_t contentLength = std::string::npos;
    size_t p = offset;
    while (p < headerEnd) {
        size_t eol = buf.find("\r\n", p);
        if (eol == std::string::npos || eol > headerEnd) eol = headerEnd;
        std::string line = buf.substr(p, eol - p);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            // lowercase key
            for (char& c : key) c = (char)tolower((unsigned char)c);
            if (key == "content-length") {
                std::string val = line.substr(colon + 1);
                contentLength = (size_t)strtoul(val.c_str(), nullptr, 10);
            }
        }
        p = eol + 2;
    }
    if (contentLength == std::string::npos) return false;

    size_t bodyStart = headerEnd + 4;
    if (buf.size() < bodyStart + contentLength) return false; // incomplete

    body = buf.substr(bodyStart, contentLength);
    offset = bodyStart + contentLength;
    return true;
}

// ---------------------------------------------------------------------------
// Server discovery
// ---------------------------------------------------------------------------
static bool inPath(const std::string& prog) {
    if (prog.find('/') != std::string::npos)
        return access(prog.c_str(), X_OK) == 0;
    const char* path = getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t colon = p.find(':', start);
        std::string dir = p.substr(start, colon == std::string::npos
                                              ? std::string::npos
                                              : colon - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + prog;
            if (access(full.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

std::string LspClient::detectServer(const std::string& languageId) {
    std::vector<std::string> candidates;
    if (languageId == "c" || languageId == "cpp") {
        candidates = {"clangd", "clangd-18", "clangd-17", "clangd-16",
                      "clangd-15", "clangd-14", "ccls"};
    } else if (languageId == "python") {
        candidates = {"pylsp", "pyls", "jedi-language-server", "pyright-langserver"};
    }
    for (const auto& c : candidates)
        if (inPath(c)) return c;
    return "";
}

// ---------------------------------------------------------------------------
// Process lifecycle
// ---------------------------------------------------------------------------
bool LspClient::start(const std::string& command, const std::string& rootPath) {
    command_ = command;
    rootPath_ = rootPath;

    int toChild[2];   // parent writes toChild[1], child reads toChild[0]
    int fromChild[2]; // child writes fromChild[1], parent reads fromChild[0]
    if (pipe(toChild) != 0) return false;
    if (pipe(fromChild) != 0) {
        close(toChild[0]); close(toChild[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return false;
    }

    if (pid == 0) {
        // Child: wire pipes to stdin/stdout, keep stderr for diagnostics.
        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);

        // Split command on spaces so callers may pass simple arguments.
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : command) {
            if (ch == ' ') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
            else cur.push_back(ch);
        }
        if (!cur.empty()) parts.push_back(cur);
        if (parts.empty()) _exit(127);

        std::vector<char*> argv;
        for (auto& s : parts) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    }

    // Parent.
    close(toChild[0]);
    close(fromChild[1]);
    in_fd_  = toChild[1];
    out_fd_ = fromChild[0];
    child_  = pid;
    running_ = true;

    reader_ = std::thread(&LspClient::readerLoop, this);

    // initialize handshake.
    int id = nextId();
    initializeId_ = id;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_[id] = "initialize";
    }
    std::string rootUri = "file://" + rootPath_;
    std::string params =
        "{\"processId\":" + std::to_string((long)getpid()) +
        ",\"rootUri\":\"" + json::escape(rootUri) + "\"" +
        ",\"rootPath\":\"" + json::escape(rootPath_) + "\"" +
        ",\"capabilities\":{\"textDocument\":{"
        "\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},"
        "\"completion\":{\"completionItem\":{\"snippetSupport\":false},"
        "\"contextSupport\":true},"
        "\"publishDiagnostics\":{\"relatedInformation\":false},"
        "\"formatting\":{\"dynamicRegistration\":false},"
        "\"synchronization\":{\"didSave\":true,\"dynamicRegistration\":false}"
        "}},\"trace\":\"off\"}";
    sendRequest(id, "initialize", params);
    status("Starting language server: " + command_);
    return true;
}

LspClient::~LspClient() { shutdown(); }

void LspClient::shutdown() {
    if (!running_.exchange(false)) {
        if (reader_.joinable()) reader_.join();
        return;
    }
    // Best-effort polite shutdown, then terminate.
    if (in_fd_ >= 0) {
        std::string body = "{\"jsonrpc\":\"2.0\",\"id\":9999,\"method\":\"shutdown\"}";
        std::string wire = frame(body);
        (void)!write(in_fd_, wire.data(), wire.size());
        std::string exitBody = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";
        std::string exitWire = frame(exitBody);
        (void)!write(in_fd_, exitWire.data(), exitWire.size());
        close(in_fd_);
        in_fd_ = -1;
    }
    if (out_fd_ >= 0) { close(out_fd_); out_fd_ = -1; }
    if (child_ > 0) {
        // Give it a moment, then reap; kill if still alive.
        for (int i = 0; i < 20; i++) {
            int st;
            pid_t r = waitpid(child_, &st, WNOHANG);
            if (r == child_ || r < 0) { child_ = -1; break; }
            usleep(10000);
        }
        if (child_ > 0) { kill(child_, SIGTERM); waitpid(child_, nullptr, 0); child_ = -1; }
    }
    if (reader_.joinable()) reader_.join();
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------
void LspClient::writeMessage(const std::string& body) {
    std::string wire = frame(body);
    std::lock_guard<std::mutex> lk(write_mtx_);
    if (in_fd_ < 0) return;
    size_t off = 0;
    while (off < wire.size()) {
        ssize_t n = write(in_fd_, wire.data() + off, wire.size() - off);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            running_ = false;
            break;
        }
        off += (size_t)n;
    }
}

void LspClient::sendRequest(int id, const std::string& method,
                            const std::string& params) {
    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                       ",\"method\":\"" + method + "\",\"params\":" + params + "}";
    writeMessage(body);
}

void LspClient::sendNotification(const std::string& method,
                                 const std::string& params) {
    std::string body = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method +
                       "\",\"params\":" + params + "}";
    writeMessage(body);
}

// ---------------------------------------------------------------------------
// LSP operations
// ---------------------------------------------------------------------------
// Return the next monotonically-increasing version for `uri`. When `reset`
// is true (didOpen) the counter restarts at 1; otherwise it increments.
int LspClient::nextVersion(const std::string& uri, bool reset) {
    std::lock_guard<std::mutex> lk(ver_mtx_);
    int v = reset ? 1 : versions_[uri] + 1;
    versions_[uri] = v;
    return v;
}

void LspClient::didOpen(const std::string& uri, const std::string& languageId,
                        const std::string& text) {
    if (!running_) return;
    int v = nextVersion(uri, true);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\",\"languageId\":\"" + json::escape(languageId) +
        "\",\"version\":" + std::to_string(v) +
        ",\"text\":\"" + json::escape(text) + "\"}}";
    sendNotification("textDocument/didOpen", params);
}

void LspClient::didChange(const std::string& uri, const std::string& text) {
    if (!running_) return;
    // Full-document sync (TextDocumentSyncKind.Full), per-URI version.
    int v = nextVersion(uri, false);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\",\"version\":" + std::to_string(v) +
        "},\"contentChanges\":[{\"text\":\"" + json::escape(text) + "\"}]}";
    sendNotification("textDocument/didChange", params);
}

void LspClient::didChangeRange(const std::string& uri, int startLine, int startChar,
                               int endLine, int endChar, const std::string& newText) {
    if (!running_) return;
    // Incremental sync (TextDocumentSyncKind.Incremental): a single content
    // change carrying the replaced range plus the text that now occupies it.
    int v = nextVersion(uri, false);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\",\"version\":" + std::to_string(v) +
        "},\"contentChanges\":[{\"range\":{\"start\":{\"line\":" +
        std::to_string(startLine) + ",\"character\":" + std::to_string(startChar) +
        "},\"end\":{\"line\":" + std::to_string(endLine) + ",\"character\":" +
        std::to_string(endChar) + "}},\"text\":\"" + json::escape(newText) + "\"}]}";
    sendNotification("textDocument/didChange", params);
}

void LspClient::completion(const std::string& uri, int line, int character) {
    if (!running_) { if (onCompletion) onCompletion({}); return; }
    int id = nextId();
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_[id] = "completion";
    }
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\"},\"position\":{\"line\":" + std::to_string(line) +
        ",\"character\":" + std::to_string(character) +
        "},\"context\":{\"triggerKind\":1}}";
    sendRequest(id, "textDocument/completion", params);
}

void LspClient::hover(const std::string& uri, int line, int character) {
    if (!running_) { if (onHover) onHover(""); return; }
    int id = nextId();
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_[id] = "hover";
    }
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\"},\"position\":{\"line\":" + std::to_string(line) +
        ",\"character\":" + std::to_string(character) + "}}";
    sendRequest(id, "textDocument/hover", params);
}

// Shared params builder for the position-based navigation requests.
static std::string posParams(const std::string& uri, int line, int character) {
    return "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
           "\"},\"position\":{\"line\":" + std::to_string(line) +
           ",\"character\":" + std::to_string(character) + "}}";
}

void LspClient::definition(const std::string& uri, int line, int character) {
    if (!running_) { if (onDefinition) onDefinition({}); return; }
    int id = nextId();
    { std::lock_guard<std::mutex> lk(pending_mtx_); pending_[id] = "definition"; }
    sendRequest(id, "textDocument/definition", posParams(uri, line, character));
}

void LspClient::references(const std::string& uri, int line, int character) {
    if (!running_) { if (onReferences) onReferences({}); return; }
    int id = nextId();
    { std::lock_guard<std::mutex> lk(pending_mtx_); pending_[id] = "references"; }
    // includeDeclaration so the defining site shows up in the list too.
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\"},\"position\":{\"line\":" + std::to_string(line) +
        ",\"character\":" + std::to_string(character) +
        "},\"context\":{\"includeDeclaration\":true}}";
    sendRequest(id, "textDocument/references", params);
}

void LspClient::signatureHelp(const std::string& uri, int line, int character) {
    if (!running_) { if (onSignatureHelp) onSignatureHelp({}, -1, -1); return; }
    int id = nextId();
    { std::lock_guard<std::mutex> lk(pending_mtx_); pending_[id] = "signatureHelp"; }
    sendRequest(id, "textDocument/signatureHelp", posParams(uri, line, character));
}

void LspClient::rename(const std::string& uri, int line, int character,
                       const std::string& newName) {
    if (!running_) { if (onRename) onRename(WorkspaceEdit{}); return; }
    int id = nextId();
    { std::lock_guard<std::mutex> lk(pending_mtx_); pending_[id] = "rename"; }
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\"},\"position\":{\"line\":" + std::to_string(line) +
        ",\"character\":" + std::to_string(character) +
        "},\"newName\":\"" + json::escape(newName) + "\"}";
    sendRequest(id, "textDocument/rename", params);
}

void LspClient::formatting(const std::string& uri, int tabSize, bool insertSpaces) {
    if (!running_) { if (onFormatting) onFormatting({}); return; }
    int id = nextId();
    { std::lock_guard<std::mutex> lk(pending_mtx_); pending_[id] = "formatting"; }
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + json::escape(uri) +
        "\"},\"options\":{\"tabSize\":" + std::to_string(tabSize) +
        ",\"insertSpaces\":" + (insertSpaces ? "true" : "false") +
        ",\"trimTrailingWhitespace\":true,\"insertFinalNewline\":true,"
        "\"trimFinalNewlines\":true}}";
    sendRequest(id, "textDocument/formatting", params);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
void LspClient::readerLoop() {
    std::string buf;
    char tmp[8192];
    while (running_) {
        ssize_t n = read(out_fd_, tmp, sizeof(tmp));
        if (n > 0) {
            buf.append(tmp, (size_t)n);
            size_t offset = 0;
            std::string body;
            while (parseFrame(buf, offset, body)) {
                handleMessage(body);
            }
            if (offset > 0) buf.erase(0, offset);
        } else if (n == 0) {
            break; // EOF: server closed stdout
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    running_ = false;
    status("Language server stopped.");
}

// Extract the human-readable text out of a hover "result".
// Handles: {contents:"str"}, {contents:{kind,value}},
// {contents:{language,value}} and {contents:[ ... ]}.
static std::string extractHover(const json::Value& result) {
    const json::Value* c = result.find("contents");
    if (!c) return "";
    if (c->isString()) return c->str;
    if (c->isObject()) {
        if (const json::Value* v = c->find("value")) return v->asString();
        return "";
    }
    if (c->isArray()) {
        std::string out;
        for (const auto& item : c->arr) {
            if (!out.empty()) out += "\n\n";
            if (item.isString()) out += item.str;
            else if (item.isObject()) {
                if (const json::Value* v = item.find("value")) out += v->asString();
            }
        }
        return out;
    }
    return "";
}

// Parse a textDocument/completion result (CompletionItem[] or
// CompletionList{items:[...]}) into a flat vector.
static std::vector<CompletionItem> extractCompletion(const json::Value& result) {
    std::vector<CompletionItem> out;
    const json::Value* arr = nullptr;
    if (result.isArray()) arr = &result;
    else if (result.isObject()) {
        const json::Value* items = result.find("items");
        if (items && items->isArray()) arr = items;
    }
    if (!arr) return out;
    for (const auto& e : arr->arr) {
        if (!e.isObject()) continue;
        CompletionItem ci;
        if (const json::Value* l = e.find("label"))  ci.label = l->asString();
        if (const json::Value* k = e.find("kind"))   ci.kind = (int)k->asInt();
        if (const json::Value* d = e.find("detail")) ci.detail = d->asString();
        if (const json::Value* t = e.find("insertText")) ci.insertText = t->asString();
        // A textEdit, when present, is authoritative for the inserted text.
        if (const json::Value* te = e.find("textEdit")) {
            if (const json::Value* nt = te->find("newText")) ci.insertText = nt->asString();
        }
        if (ci.label.empty()) ci.label = ci.insertText;
        if (ci.label.empty()) continue;
        out.push_back(std::move(ci));
        if (out.size() >= 200) break; // keep the popup light
    }
    return out;
}

// Parse a publishDiagnostics params object into (uri, diagnostics).
static void extractDiagnostics(const json::Value& params, std::string& uri,
                               std::vector<Diagnostic>& out) {
    if (const json::Value* u = params.find("uri")) uri = u->asString();
    const json::Value* arr = params.find("diagnostics");
    if (!arr || !arr->isArray()) return;
    for (const auto& d : arr->arr) {
        if (!d.isObject()) continue;
        Diagnostic dg;
        if (const json::Value* r = d.find("range")) {
            if (const json::Value* st = r->find("start")) {
                if (const json::Value* l = st->find("line"))      dg.line = (int)l->asInt();
                if (const json::Value* c = st->find("character")) dg.character = (int)c->asInt();
            }
            if (const json::Value* en = r->find("end")) {
                if (const json::Value* l = en->find("line"))      dg.endLine = (int)l->asInt();
                if (const json::Value* c = en->find("character")) dg.endCharacter = (int)c->asInt();
            }
        }
        if (const json::Value* s = d.find("severity")) dg.severity = (int)s->asInt();
        if (const json::Value* m = d.find("message"))  dg.message = m->asString();
        if (const json::Value* s = d.find("source"))   dg.source = s->asString();
        out.push_back(std::move(dg));
    }
}

// Pull a {line,character} pair out of a position object.
static void readPos(const json::Value* pos, int& line, int& ch) {
    if (!pos) return;
    if (const json::Value* l = pos->find("line"))      line = (int)l->asInt();
    if (const json::Value* c = pos->find("character")) ch   = (int)c->asInt();
}

// Fill a Location's range fields from a range object.
static void readRange(const json::Value* range, Location& loc) {
    if (!range) return;
    readPos(range->find("start"), loc.line, loc.character);
    readPos(range->find("end"),   loc.endLine, loc.endCharacter);
}

// Parse a definition/references result: Location, Location[], or
// LocationLink[] (targetUri/targetSelectionRange|targetRange). Any null or
// unexpected shape yields an empty vector.
std::vector<Location> LspClient::parseLocations(const json::Value& result) {
    std::vector<Location> out;
    auto readOne = [&](const json::Value& e) {
        if (!e.isObject()) return;
        Location loc;
        if (const json::Value* u = e.find("uri")) {           // Location
            loc.uri = u->asString();
            readRange(e.find("range"), loc);
        } else if (const json::Value* tu = e.find("targetUri")) { // LocationLink
            loc.uri = tu->asString();
            const json::Value* r = e.find("targetSelectionRange");
            if (!r) r = e.find("targetRange");
            readRange(r, loc);
        } else {
            return;
        }
        if (!loc.uri.empty()) out.push_back(std::move(loc));
    };
    if (result.isArray()) {
        for (const auto& e : result.arr) readOne(e);
    } else if (result.isObject()) {
        readOne(result);
    }
    return out;
}

// Parse a signatureHelp result into overloads + active indices.
std::vector<SignatureInfo> LspClient::parseSignatureHelp(const json::Value& result,
                                                         int& activeSignature,
                                                         int& activeParameter) {
    std::vector<SignatureInfo> out;
    activeSignature = 0;
    activeParameter = -1;
    if (!result.isObject()) return out;
    if (const json::Value* a = result.find("activeSignature")) activeSignature = (int)a->asInt();
    if (const json::Value* a = result.find("activeParameter")) activeParameter = (int)a->asInt();
    const json::Value* sigs = result.find("signatures");
    if (!sigs || !sigs->isArray()) return out;
    for (const auto& s : sigs->arr) {
        if (!s.isObject()) continue;
        SignatureInfo si;
        if (const json::Value* l = s.find("label")) si.label = l->asString();
        if (const json::Value* d = s.find("documentation")) {
            if (d->isString()) si.documentation = d->str;
            else if (d->isObject()) { if (const json::Value* v = d->find("value")) si.documentation = v->asString(); }
        }
        if (const json::Value* ap = s.find("activeParameter")) si.activeParameter = (int)ap->asInt();
        if (const json::Value* ps = s.find("parameters")) {
            if (ps->isArray()) {
                for (const auto& p : ps->arr) {
                    ParameterInfo pi;
                    if (p.isString()) pi.label = p.str;
                    else if (p.isObject()) {
                        const json::Value* lab = p.find("label");
                        if (lab) {
                            if (lab->isString()) pi.label = lab->str;
                            else if (lab->isArray() && lab->arr.size() == 2) {
                                // [start,end] offsets into the signature label.
                                int a = (int)lab->arr[0].asInt();
                                int b = (int)lab->arr[1].asInt();
                                if (a >= 0 && b >= a && b <= (int)si.label.size())
                                    pi.label = si.label.substr(a, b - a);
                            }
                        }
                    }
                    si.parameters.push_back(std::move(pi));
                }
            }
        }
        out.push_back(std::move(si));
    }
    return out;
}

// Parse a bare TextEdit[] (the textDocument/formatting result shape).
std::vector<TextEditOp> LspClient::parseTextEdits(const json::Value& result) {
    std::vector<TextEditOp> ops;
    if (!result.isArray()) return ops;
    for (const auto& e : result.arr) {
        if (!e.isObject()) continue;
        TextEditOp op;
        Location tmp;
        readRange(e.find("range"), tmp);
        op.line = tmp.line; op.character = tmp.character;
        op.endLine = tmp.endLine; op.endCharacter = tmp.endCharacter;
        if (const json::Value* nt = e.find("newText")) op.newText = nt->asString();
        ops.push_back(std::move(op));
    }
    return ops;
}

// Parse a rename WorkspaceEdit: either {changes:{uri:[edits]}} or
// {documentChanges:[{textDocument:{uri},edits:[...]}]}.
WorkspaceEdit LspClient::parseWorkspaceEdit(const json::Value& result) {
    WorkspaceEdit we;
    if (!result.isObject()) return we;
    auto readEdits = [](const json::Value& editsArr) {
        std::vector<TextEditOp> ops;
        if (!editsArr.isArray()) return ops;
        for (const auto& e : editsArr.arr) {
            if (!e.isObject()) continue;
            TextEditOp op;
            Location tmp;
            readRange(e.find("range"), tmp);
            op.line = tmp.line; op.character = tmp.character;
            op.endLine = tmp.endLine; op.endCharacter = tmp.endCharacter;
            if (const json::Value* nt = e.find("newText")) op.newText = nt->asString();
            ops.push_back(std::move(op));
        }
        return ops;
    };
    if (const json::Value* ch = result.find("changes")) {
        if (ch->isObject()) {
            for (const auto& kv : ch->obj)
                we.changes.emplace_back(kv.first, readEdits(kv.second));
        }
    }
    if (const json::Value* dc = result.find("documentChanges")) {
        if (dc->isArray()) {
            for (const auto& d : dc->arr) {
                if (!d.isObject()) continue;
                const json::Value* td = d.find("textDocument");
                std::string uri;
                if (td) { if (const json::Value* u = td->find("uri")) uri = u->asString(); }
                const json::Value* edits = d.find("edits");
                if (!uri.empty() && edits) we.changes.emplace_back(uri, readEdits(*edits));
            }
        }
    }
    return we;
}

void LspClient::handleMessage(const std::string& body) {
    json::Value msg;
    if (!json::parse(body, msg) || !msg.isObject()) return;

    const json::Value* idv = msg.find("id");
    const json::Value* method = msg.find("method");

    // Server-initiated request or notification (has method).
    if (method && method->isString()) {
        // Notifications we care about are dispatched here; unknown
        // server->client requests are ignored (no response sent).
        if (method->str == "textDocument/publishDiagnostics") {
            const json::Value* params = msg.find("params");
            if (params && params->isObject()) {
                std::string uri;
                std::vector<Diagnostic> diags;
                extractDiagnostics(*params, uri, diags);
                if (onDiagnostics) onDiagnostics(uri, diags);
            }
        }
        return;
    }

    // Response to one of our requests (has id, no method).
    if (!idv || !idv->isNumber()) return;
    int id = (int)idv->num;

    std::string kind;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        auto it = pending_.find(id);
        if (it != pending_.end()) { kind = it->second; pending_.erase(it); }
    }

    if (kind == "initialize") {
        // Complete the handshake.
        sendNotification("initialized", "{}");
        status("Language server ready (" + command_ + ")");
        return;
    }
    if (kind == "hover") {
        const json::Value* result = msg.find("result");
        std::string text;
        if (result && result->isObject())
            text = extractHover(*result);
        if (onHover) onHover(text);
        return;
    }
    if (kind == "completion") {
        const json::Value* result = msg.find("result");
        std::vector<CompletionItem> items;
        if (result) items = extractCompletion(*result);
        if (onCompletion) onCompletion(items);
        return;
    }
    if (kind == "definition") {
        const json::Value* result = msg.find("result");
        std::vector<Location> locs;
        if (result) locs = parseLocations(*result);
        if (onDefinition) onDefinition(locs);
        return;
    }
    if (kind == "references") {
        const json::Value* result = msg.find("result");
        std::vector<Location> locs;
        if (result) locs = parseLocations(*result);
        if (onReferences) onReferences(locs);
        return;
    }
    if (kind == "signatureHelp") {
        const json::Value* result = msg.find("result");
        std::vector<SignatureInfo> sigs;
        int actS = -1, actP = -1;
        if (result && result->isObject()) sigs = parseSignatureHelp(*result, actS, actP);
        if (onSignatureHelp) onSignatureHelp(sigs, actS, actP);
        return;
    }
    if (kind == "rename") {
        const json::Value* result = msg.find("result");
        WorkspaceEdit we;
        if (result) we = parseWorkspaceEdit(*result);
        if (onRename) onRename(we);
        return;
    }
    if (kind == "formatting") {
        const json::Value* result = msg.find("result");
        std::vector<TextEditOp> edits;
        if (result) edits = parseTextEdits(*result);
        if (onFormatting) onFormatting(edits);
        return;
    }
}
