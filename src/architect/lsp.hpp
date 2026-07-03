// lsp.hpp - a minimal but genuine LSP (Language Server Protocol) client.
//
// Spawns a language server as a child process and talks JSON-RPC 2.0 over
// its stdio, with correct `Content-Length` framing. Implements the pieces
// Architect stage 1 needs: initialize/initialized handshake, didOpen,
// didChange and a live textDocument/hover round-trip.
//
// This translation unit is deliberately free of any GTK/WebKit dependency
// so the framing can be unit-tested with a mock server. Results are handed
// back through std::function callbacks that fire on the reader thread; the
// GUI layer is responsible for marshalling them onto the main loop.
#ifndef ARCHITECT_LSP_HPP
#define ARCHITECT_LSP_HPP

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <vector>
#include <utility>

#include "json.hpp"

// A single completion candidate returned by textDocument/completion.
struct CompletionItem {
    std::string label;        // text shown in the popup
    int         kind = 0;     // LSP CompletionItemKind (1..25), 0 if absent
    std::string detail;       // secondary text (type/signature), may be empty
    std::string insertText;   // text to insert; falls back to label if empty
};

// A single diagnostic from textDocument/publishDiagnostics.
struct Diagnostic {
    int         line = 0, character = 0;      // range.start (0-based)
    int         endLine = 0, endCharacter = 0; // range.end   (0-based)
    int         severity = 1;                  // 1 error 2 warn 3 info 4 hint
    std::string message;
    std::string source;                        // producing tool, may be empty
};

// A resolved source location (from definition / references replies). All the
// reply shapes (Location, Location[], LocationLink[]) collapse to this.
struct Location {
    std::string uri;
    int line = 0, character = 0;             // range.start (0-based)
    int endLine = 0, endCharacter = 0;       // range.end   (0-based)
};

// A single parameter within a SignatureInformation.
struct ParameterInfo {
    std::string label;                        // parameter label text
};

// One overload from textDocument/signatureHelp.
struct SignatureInfo {
    std::string label;                        // full signature text
    std::string documentation;                // may be empty
    std::vector<ParameterInfo> parameters;
    int activeParameter = -1;                  // per-signature active param, -1 if absent
};

// A single text edit within a WorkspaceEdit (rename result).
struct TextEditOp {
    int line = 0, character = 0;
    int endLine = 0, endCharacter = 0;
    std::string newText;
};

// A WorkspaceEdit flattened to per-URI edit lists (rename result).
struct WorkspaceEdit {
    // uri -> edits (unsorted; the applier is responsible for ordering).
    std::vector<std::pair<std::string, std::vector<TextEditOp>>> changes;
};

class LspClient {
public:
    LspClient() = default;
    ~LspClient();

    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;

    // Locate a server binary in PATH for the given languageId
    // ("c","cpp","python"). Returns the argv[0] to exec, or "" if none.
    static std::string detectServer(const std::string& languageId);

    // Spawn `command` (already resolved to an absolute or PATH-visible
    // program) and perform the initialize handshake against `rootPath`.
    // Returns false if the process could not be started.
    bool start(const std::string& command, const std::string& rootPath);

    bool isRunning() const { return running_.load(); }
    const std::string& command() const { return command_; }

    // Notifications ------------------------------------------------------
    void didOpen(const std::string& uri, const std::string& languageId,
                 const std::string& text);
    // Full-document sync: replaces the whole buffer.
    void didChange(const std::string& uri, const std::string& text);
    // Incremental sync: send only the replaced range + its new text. The
    // server-side version increments per URI like didChange.
    void didChangeRange(const std::string& uri, int startLine, int startChar,
                        int endLine, int endChar, const std::string& newText);

    // Request: hover at (line, character) 0-based. When the reply arrives
    // the onHover callback fires with the extracted markdown/plaintext
    // (empty string if the server returned no hover info).
    void hover(const std::string& uri, int line, int character);

    // Request: completion at (line, character) 0-based. On reply the
    // onCompletion callback fires with the parsed candidate list (handles
    // both CompletionItem[] and CompletionList{items:[...]} shapes).
    void completion(const std::string& uri, int line, int character);

    // Request: go-to-definition at (line, character). On reply onDefinition
    // fires with the resolved locations (handles Location, Location[] and
    // LocationLink[] shapes; empty vector if none).
    void definition(const std::string& uri, int line, int character);

    // Request: find references at (line, character). On reply onReferences
    // fires with all resolved locations.
    void references(const std::string& uri, int line, int character);

    // Request: signature help at (line, character). On reply onSignatureHelp
    // fires with the overloads plus the active signature/parameter indices.
    void signatureHelp(const std::string& uri, int line, int character);

    // Request: rename the symbol at (line, character) to newName. On reply
    // onRename fires with the WorkspaceEdit (empty changes if unsupported).
    void rename(const std::string& uri, int line, int character,
                const std::string& newName);

    // Request: format the whole document (textDocument/formatting). On reply
    // onFormatting fires with the TextEdit[] to apply (empty if unsupported).
    void formatting(const std::string& uri, int tabSize, bool insertSpaces);

    // Callbacks (invoked from the reader thread) -------------------------
    std::function<void(const std::string& markdown)> onHover;
    std::function<void(const std::string& message)>  onStatus;
    std::function<void(const std::vector<CompletionItem>&)> onCompletion;
    // Server-initiated: publishDiagnostics for `uri`.
    std::function<void(const std::string& uri,
                       const std::vector<Diagnostic>&)>    onDiagnostics;
    std::function<void(const std::vector<Location>&)>      onDefinition;
    std::function<void(const std::vector<Location>&)>      onReferences;
    // activeSignature / activeParameter are the top-level indices from the
    // SignatureHelp reply.
    std::function<void(const std::vector<SignatureInfo>&,
                       int activeSignature, int activeParameter)> onSignatureHelp;
    std::function<void(const WorkspaceEdit&)>              onRename;
    // textDocument/formatting reply: the edits to apply to the whole buffer.
    std::function<void(const std::vector<TextEditOp>&)>   onFormatting;

    void shutdown();

    // Exposed for unit testing: parse the `result` payloads of each request
    // shape. These are the exact functions the reader thread uses to turn a
    // server reply into the structs delivered through the callbacks.
    static std::vector<Location>  parseLocations(const json::Value& result);
    static std::vector<SignatureInfo> parseSignatureHelp(const json::Value& result,
                                                         int& activeSignature,
                                                         int& activeParameter);
    static WorkspaceEdit          parseWorkspaceEdit(const json::Value& result);
    // Parse a textDocument/formatting result (a bare TextEdit[]).
    static std::vector<TextEditOp> parseTextEdits(const json::Value& result);

    // Exposed for unit testing: build/parse a single framed message.
    static std::string frame(const std::string& body);      // body -> wire bytes
    // Parse one message from `buf` starting at offset. On success returns
    // true, sets `body` and advances `offset` past the consumed bytes.
    // Returns false if a complete frame is not yet available.
    static bool parseFrame(const std::string& buf, size_t& offset,
                           std::string& body);

private:
    int  nextId() { return ++id_; }
    void writeMessage(const std::string& body);   // adds framing, writes to child
    void sendRequest(int id, const std::string& method, const std::string& params);
    void sendNotification(const std::string& method, const std::string& params);
    void readerLoop();
    void handleMessage(const std::string& body);
    void status(const std::string& m) { if (onStatus) onStatus(m); }

    std::string command_;
    std::string rootPath_;
    pid_t child_ = -1;
    int   in_fd_  = -1;   // we write to child's stdin
    int   out_fd_ = -1;   // we read from child's stdout
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<int>  id_{0};
    std::mutex write_mtx_;

    // pending request id -> kind ("hover"/"completion"), routes replies.
    std::mutex pending_mtx_;
    std::map<int, std::string> pending_;
    int initializeId_ = -1;

    // Per-document version counters so didChange versions increase
    // monotonically per URI (diagnostics map back to the right file).
    std::mutex ver_mtx_;
    std::map<std::string, int> versions_;
    int nextVersion(const std::string& uri, bool reset);
};

#endif // ARCHITECT_LSP_HPP
