// lsp_test.cpp - unit test for the LSP client's Content-Length framing plus
// a genuine spawn/round-trip. When invoked with "--mock-server" this same
// binary acts as a tiny language server (JSON-RPC over stdio with correct
// framing); the test then launches that mock through LspClient and verifies a
// full initialize -> initialized -> hover exchange. No external server needed.
#include "../lsp.hpp"
#include "../json.hpp"
#include "../fuzzy.hpp"
#include "../textguard.hpp"

#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <limits.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok  : %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); g_fail++; } } while (0)

// ---------------------------------------------------------------------------
// Mock language server: reads framed JSON-RPC on stdin, replies on stdout.
// ---------------------------------------------------------------------------
static void write_frame(const std::string& body) {
    std::string wire = LspClient::frame(body);
    size_t off = 0;
    while (off < wire.size()) {
        ssize_t n = write(STDOUT_FILENO, wire.data() + off, wire.size() - off);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static int run_mock_server() {
    std::string buf;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t off = 0;
        std::string body;
        while (LspClient::parseFrame(buf, off, body)) {
            json::Value msg;
            if (json::parse(body, msg) && msg.isObject()) {
                const json::Value* method = msg.find("method");
                const json::Value* idv = msg.find("id");
                std::string m = method ? method->asString() : "";
                std::string id = idv && idv->isNumber()
                                     ? std::to_string((long)idv->num) : "";
                if (m == "initialize" && !id.empty()) {
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":{\"capabilities\":{\"hoverProvider\":true,"
                                "\"completionProvider\":{}}}}");
                } else if (m == "textDocument/hover" && !id.empty()) {
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":{\"contents\":{\"kind\":\"markdown\","
                                "\"value\":\"MOCK HOVER OK\"}}}");
                } else if (m == "textDocument/didOpen") {
                    // Server-initiated notification: publish a diagnostic for
                    // the just-opened document. Exercises the reader's
                    // notification-dispatch path.
                    write_frame("{\"jsonrpc\":\"2.0\",\"method\":"
                                "\"textDocument/publishDiagnostics\",\"params\":"
                                "{\"uri\":\"file:///tmp/x.c\",\"diagnostics\":[{"
                                "\"range\":{\"start\":{\"line\":2,\"character\":4},"
                                "\"end\":{\"line\":2,\"character\":9}},"
                                "\"severity\":1,\"source\":\"mock\","
                                "\"message\":\"MOCK DIAG\"}]}}");
                } else if (m == "textDocument/completion" && !id.empty()) {
                    // Reply in CompletionList{items:[...]} shape.
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":{\"isIncomplete\":false,\"items\":[{"
                                "\"label\":\"main\",\"kind\":3,\"detail\":\"int()\"},{"
                                "\"label\":\"malloc\",\"kind\":3}]}}");
                } else if (m == "textDocument/definition" && !id.empty()) {
                    // LocationLink[] shape (targetUri + targetSelectionRange).
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":[{\"targetUri\":\"file:///tmp/def.c\","
                                "\"targetRange\":{\"start\":{\"line\":10,\"character\":0},"
                                "\"end\":{\"line\":12,\"character\":1}},"
                                "\"targetSelectionRange\":{\"start\":{\"line\":10,\"character\":4},"
                                "\"end\":{\"line\":10,\"character\":9}}}]}");
                } else if (m == "textDocument/references" && !id.empty()) {
                    // Location[] shape, two hits in two files.
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":[{\"uri\":\"file:///tmp/a.c\","
                                "\"range\":{\"start\":{\"line\":1,\"character\":2},"
                                "\"end\":{\"line\":1,\"character\":7}}},"
                                "{\"uri\":\"file:///tmp/b.c\","
                                "\"range\":{\"start\":{\"line\":5,\"character\":0},"
                                "\"end\":{\"line\":5,\"character\":5}}}]}");
                } else if (m == "textDocument/signatureHelp" && !id.empty()) {
                    // One signature with two params, active param = 1.
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":{\"activeSignature\":0,\"activeParameter\":1,"
                                "\"signatures\":[{\"label\":\"int add(int a, int b)\","
                                "\"documentation\":\"adds two ints\","
                                "\"parameters\":[{\"label\":\"int a\"},{\"label\":\"int b\"}]}]}}");
                } else if (m == "textDocument/rename" && !id.empty()) {
                    // WorkspaceEdit in {changes:{uri:[edits]}} shape.
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                ",\"result\":{\"changes\":{\"file:///tmp/x.c\":[{"
                                "\"range\":{\"start\":{\"line\":0,\"character\":4},"
                                "\"end\":{\"line\":0,\"character\":8}},\"newText\":\"start\"},"
                                "{\"range\":{\"start\":{\"line\":3,\"character\":2},"
                                "\"end\":{\"line\":3,\"character\":6}},\"newText\":\"start\"}]}}}");
                } else if (m == "shutdown" && !id.empty()) {
                    write_frame("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":null}");
                } else if (m == "exit") {
                    return 0;
                }
                // notifications (initialized, didOpen, ...) need no reply
            }
        }
        if (off > 0) buf.erase(0, off);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Framing unit tests
// ---------------------------------------------------------------------------
static void test_framing() {
    printf("[framing]\n");

    // 1. frame() emits correct header + body.
    std::string body = "{\"jsonrpc\":\"2.0\"}";
    std::string wire = LspClient::frame(body);
    std::string expectHdr = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    CHECK(wire == expectHdr + body, "frame emits Content-Length header + CRLFCRLF + body");

    // 2. round-trip parse.
    size_t off = 0; std::string got;
    bool ok = LspClient::parseFrame(wire, off, got);
    CHECK(ok && got == body, "parseFrame recovers original body");
    CHECK(off == wire.size(), "parseFrame advances offset past the message");

    // 3. two concatenated messages parsed in sequence.
    std::string a = LspClient::frame("{\"a\":1}");
    std::string b = LspClient::frame("{\"b\":2}");
    std::string two = a + b;
    off = 0; std::string m1, m2;
    bool ok1 = LspClient::parseFrame(two, off, m1);
    bool ok2 = LspClient::parseFrame(two, off, m2);
    CHECK(ok1 && ok2 && m1 == "{\"a\":1}" && m2 == "{\"b\":2}",
          "two back-to-back frames parse in order");

    // 4. incomplete frame -> parseFrame returns false (needs more bytes).
    std::string partial = "Content-Length: 20\r\n\r\n{\"x\":1}"; // body shorter than 20
    off = 0; std::string p;
    CHECK(!LspClient::parseFrame(partial, off, p),
          "incomplete body reported as not-yet-available");

    // 5. header-only (no body yet) -> false.
    std::string hdronly = "Content-Length: 10\r\n\r\n";
    off = 0;
    CHECK(!LspClient::parseFrame(hdronly, off, p), "header without body is incomplete");

    // 6. case-insensitive header key and extra header lines.
    std::string body6 = "{\"z\":9}";
    std::string custom = "content-length: " + std::to_string(body6.size()) +
                         "\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n" + body6;
    off = 0; std::string g6;
    CHECK(LspClient::parseFrame(custom, off, g6) && g6 == body6,
          "case-insensitive Content-Length with extra headers");
}

// ---------------------------------------------------------------------------
// Real spawn + JSON-RPC round-trip against the mock server.
// ---------------------------------------------------------------------------
static void test_roundtrip(const char* selfPath) {
    printf("[round-trip]\n");
    LspClient client;

    std::atomic<bool> ready{false};
    std::atomic<bool> gotHover{false};
    std::atomic<bool> gotDiag{false};
    std::atomic<bool> gotComp{false};
    std::string hoverText;
    std::string diagUri, diagMsg;
    int diagLine = -1, diagSeverity = -1;
    size_t compCount = 0;
    std::string compFirst;
    int compFirstKind = -1;

    client.onStatus = [&](const std::string& s) {
        if (s.find("ready") != std::string::npos) ready = true;
    };
    client.onHover = [&](const std::string& md) {
        hoverText = md;
        gotHover = true;
    };
    client.onDiagnostics = [&](const std::string& uri,
                               const std::vector<Diagnostic>& ds) {
        diagUri = uri;
        if (!ds.empty()) {
            diagMsg = ds[0].message;
            diagLine = ds[0].line;
            diagSeverity = ds[0].severity;
        }
        gotDiag = true;
    };
    client.onCompletion = [&](const std::vector<CompletionItem>& items) {
        compCount = items.size();
        if (!items.empty()) { compFirst = items[0].label; compFirstKind = items[0].kind; }
        gotComp = true;
    };

    std::string cmd = std::string(selfPath) + " --mock-server";
    bool started = client.start(cmd, "/tmp");
    CHECK(started, "spawned mock server and sent initialize");

    // Wait for initialize response -> "ready".
    for (int i = 0; i < 200 && !ready; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(ready.load(), "initialize handshake completed (initialized sent)");

    client.didOpen("file:///tmp/x.c", "c", "int main(){return 0;}\n");
    client.hover("file:///tmp/x.c", 0, 4);

    for (int i = 0; i < 200 && !gotHover; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotHover.load(), "hover response received");
    CHECK(hoverText == "MOCK HOVER OK", "hover contents extracted correctly");

    // didOpen triggered a server publishDiagnostics notification: the reader
    // must dispatch it (not just id-bearing responses).
    for (int i = 0; i < 200 && !gotDiag; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotDiag.load(), "publishDiagnostics notification dispatched");
    CHECK(diagUri == "file:///tmp/x.c", "diagnostic uri parsed");
    CHECK(diagMsg == "MOCK DIAG" && diagLine == 2 && diagSeverity == 1,
          "diagnostic message/line/severity parsed");

    // Completion round-trip (CompletionList{items:[...]} shape).
    client.completion("file:///tmp/x.c", 0, 4);
    for (int i = 0; i < 200 && !gotComp; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotComp.load(), "completion response received");
    CHECK(compCount == 2 && compFirst == "main" && compFirstKind == 3,
          "completion items parsed (label+kind)");

    // Definition round-trip (LocationLink[] shape).
    std::atomic<bool> gotDef{false};
    std::vector<Location> defLocs;
    client.onDefinition = [&](const std::vector<Location>& l) { defLocs = l; gotDef = true; };
    client.definition("file:///tmp/x.c", 0, 4);
    for (int i = 0; i < 200 && !gotDef; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotDef.load(), "definition response received");
    CHECK(defLocs.size() == 1 && defLocs[0].uri == "file:///tmp/def.c" &&
          defLocs[0].line == 10 && defLocs[0].character == 4,
          "definition LocationLink parsed (targetSelectionRange preferred)");

    // References round-trip (Location[] across two files).
    std::atomic<bool> gotRefs{false};
    std::vector<Location> refLocs;
    client.onReferences = [&](const std::vector<Location>& l) { refLocs = l; gotRefs = true; };
    client.references("file:///tmp/x.c", 0, 4);
    for (int i = 0; i < 200 && !gotRefs; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotRefs.load(), "references response received");
    CHECK(refLocs.size() == 2 && refLocs[0].uri == "file:///tmp/a.c" &&
          refLocs[1].uri == "file:///tmp/b.c" && refLocs[1].line == 5,
          "references list parsed (two locations)");

    // Signature help round-trip.
    std::atomic<bool> gotSig{false};
    std::vector<SignatureInfo> sigs;
    int sigActS = -1, sigActP = -1;
    client.onSignatureHelp = [&](const std::vector<SignatureInfo>& s, int a, int b) {
        sigs = s; sigActS = a; sigActP = b; gotSig = true;
    };
    client.signatureHelp("file:///tmp/x.c", 0, 8);
    for (int i = 0; i < 200 && !gotSig; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotSig.load(), "signatureHelp response received");
    CHECK(sigs.size() == 1 && sigs[0].label == "int add(int a, int b)" &&
          sigs[0].parameters.size() == 2 && sigs[0].parameters[1].label == "int b" &&
          sigActP == 1,
          "signatureHelp parsed (label, params, activeParameter)");

    // Rename round-trip (WorkspaceEdit {changes:{uri:[edits]}}).
    std::atomic<bool> gotRename{false};
    WorkspaceEdit renameEdit;
    client.onRename = [&](const WorkspaceEdit& we) { renameEdit = we; gotRename = true; };
    client.rename("file:///tmp/x.c", 0, 4, "start");
    for (int i = 0; i < 200 && !gotRename; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gotRename.load(), "rename response received");
    CHECK(renameEdit.changes.size() == 1 &&
          renameEdit.changes[0].first == "file:///tmp/x.c" &&
          renameEdit.changes[0].second.size() == 2 &&
          renameEdit.changes[0].second[0].newText == "start" &&
          renameEdit.changes[0].second[1].line == 3,
          "rename WorkspaceEdit parsed (uri + two text edits)");

    // Incremental didChange must not throw and keeps the server alive.
    client.didChangeRange("file:///tmp/x.c", 0, 4, 0, 8, "start");
    CHECK(client.isRunning(), "incremental didChangeRange sent, server still running");

    client.shutdown();
    CHECK(!client.isRunning(), "clean shutdown");
}

// ---------------------------------------------------------------------------
// Direct unit tests of the reply parsers (exercise the alternate shapes the
// round-trip mock doesn't cover: bare Location object, LocationLink[] with
// only targetRange, documentChanges rename, and param labels as [start,end]
// offsets). These assert the exact structs the callbacks would deliver.
// ---------------------------------------------------------------------------
static json::Value J(const std::string& s) {
    json::Value v; json::parse(s, v); return v;
}

static void test_parsers() {
    printf("[parsers]\n");

    // Definition: a single bare Location object (not an array).
    {
        auto locs = LspClient::parseLocations(J(
            "{\"uri\":\"file:///p/q.c\",\"range\":{\"start\":{\"line\":7,"
            "\"character\":3},\"end\":{\"line\":7,\"character\":9}}}"));
        CHECK(locs.size() == 1 && locs[0].uri == "file:///p/q.c" &&
              locs[0].line == 7 && locs[0].character == 3 && locs[0].endCharacter == 9,
              "parseLocations handles a single Location object");
    }
    // Definition: LocationLink[] falling back to targetRange when no
    // targetSelectionRange is present.
    {
        auto locs = LspClient::parseLocations(J(
            "[{\"targetUri\":\"file:///r.c\",\"targetRange\":{"
            "\"start\":{\"line\":2,\"character\":0},"
            "\"end\":{\"line\":4,\"character\":1}}}]"));
        CHECK(locs.size() == 1 && locs[0].uri == "file:///r.c" && locs[0].line == 2,
              "parseLocations handles LocationLink[] via targetRange fallback");
    }
    // Empty / null result yields no locations, never crashes.
    {
        auto locs = LspClient::parseLocations(J("null"));
        CHECK(locs.empty(), "parseLocations tolerates a null result");
    }
    // signatureHelp with parameter labels given as [start,end] offsets.
    {
        int as = -1, ap = -1;
        auto sigs = LspClient::parseSignatureHelp(J(
            "{\"activeSignature\":0,\"activeParameter\":0,\"signatures\":[{"
            "\"label\":\"foo(int x, int y)\",\"parameters\":["
            "{\"label\":[4,9]},{\"label\":[11,16]}]}]}"), as, ap);
        CHECK(sigs.size() == 1 && sigs[0].parameters.size() == 2 &&
              sigs[0].parameters[0].label == "int x" &&
              sigs[0].parameters[1].label == "int y" && ap == 0,
              "parseSignatureHelp resolves [start,end] offset parameter labels");
    }
    // Rename via documentChanges[] instead of changes{}.
    {
        auto we = LspClient::parseWorkspaceEdit(J(
            "{\"documentChanges\":[{\"textDocument\":{\"uri\":\"file:///d.c\","
            "\"version\":2},\"edits\":[{\"range\":{\"start\":{\"line\":1,"
            "\"character\":0},\"end\":{\"line\":1,\"character\":3}},"
            "\"newText\":\"baz\"}]}]}"));
        CHECK(we.changes.size() == 1 && we.changes[0].first == "file:///d.c" &&
              we.changes[0].second.size() == 1 &&
              we.changes[0].second[0].newText == "baz" &&
              we.changes[0].second[0].line == 1,
              "parseWorkspaceEdit handles documentChanges[] shape");
    }
    // Malformed / empty edit does not crash and yields an empty edit set.
    {
        auto we = LspClient::parseWorkspaceEdit(J("{}"));
        CHECK(we.changes.empty(), "parseWorkspaceEdit tolerates empty result");
    }
    // Formatting result: a bare TextEdit[] parsed into ordered edits.
    {
        auto edits = LspClient::parseTextEdits(J(
            "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
            "\"end\":{\"line\":0,\"character\":4}},\"newText\":\"    \"},"
            "{\"range\":{\"start\":{\"line\":2,\"character\":0},"
            "\"end\":{\"line\":2,\"character\":0}},\"newText\":\"x\"}]"));
        CHECK(edits.size() == 2 && edits[0].endCharacter == 4 &&
              edits[0].newText == "    " && edits[1].line == 2 &&
              edits[1].newText == "x",
              "parseTextEdits parses a TextEdit[] formatting result");
    }
    // Non-array / null formatting result yields no edits, never crashes.
    {
        auto edits = LspClient::parseTextEdits(J("null"));
        CHECK(edits.empty(), "parseTextEdits tolerates a null result");
    }
}

// ---------------------------------------------------------------------------
// Fuzzy matcher (Ctrl+P finder ranking): subsequence detection + scoring.
// ---------------------------------------------------------------------------
static void test_fuzzy() {
    printf("[fuzzy]\n");

    // Non-subsequence -> -1.
    CHECK(fuzzy::match("xyz", "src/architect.cpp") < 0,
          "non-subsequence rejected (-1)");
    // Subsequence -> non-negative.
    CHECK(fuzzy::match("arch", "src/architect.cpp") >= 0,
          "subsequence accepted (>=0)");
    // Empty pattern matches anything (lets the finder show the whole list).
    CHECK(fuzzy::match("", "anything") >= 0, "empty pattern matches all");
    // Case-insensitive.
    CHECK(fuzzy::match("ARCH", "architect") >= 0, "match is case-insensitive");

    // A contiguous match scores higher than a scattered mid-word one.
    int contig = fuzzy::match("arch", "architect.cpp");
    int scattered = fuzzy::match("arch", "axrxcxhxyzzz");
    CHECK(contig > scattered, "contiguous match outranks scattered match");

    // Matching at a path-segment boundary beats matching mid-word.
    int boundary = fuzzy::match("lsp", "src/lsp.cpp");
    int midword  = fuzzy::match("lsp", "collapsedxx");
    CHECK(boundary > midword, "segment-start match outranks mid-word match");

    // Exact filename beats a longer path containing the same subsequence.
    int shortHit = fuzzy::match("main", "main.c");
    int longHit  = fuzzy::match("main", "deep/nested/dir/mainframe_helper.cpp");
    CHECK(shortHit > longHit, "shorter, tighter candidate ranks higher");
}

// ---------------------------------------------------------------------------
// Binary / huge-file guard (textguard): the detector that keeps binaries and
// gigabyte ISOs out of the editor textarea.
// ---------------------------------------------------------------------------
static void test_textguard() {
    printf("[textguard]\n");

    // Plain ASCII / UTF-8 source is text.
    CHECK(!textguard::looksBinary("int main(){return 0;}\n"),
          "ascii source is not binary");
    CHECK(!textguard::looksBinary("h\xC3\xA9llo /* \xE2\x9C\x93 unicode */\n"),
          "utf-8 (high-bit) bytes are treated as text");
    CHECK(!textguard::looksBinary(""), "empty file is not binary (editable)");
    CHECK(!textguard::looksBinary("\t\r\n  spaced\ttext\n"),
          "whitespace control bytes are text");
    CHECK(!textguard::looksBinary("\x1b[31mred\x1b[0m ansi log\n"),
          "ESC (ANSI) bytes are text");

    // A NUL byte anywhere in the head marks the file binary (classic test).
    {
        std::string s = "ELF"; s.push_back('\0'); s += "\x01\x02\x03rest";
        CHECK(textguard::looksBinary(s), "a NUL byte marks the file binary");
    }
    // A high ratio of other control bytes marks it binary.
    {
        std::string s;
        for (int i = 0; i < 100; i++) s.push_back((char)(i % 7 + 1)); // 0x01..0x07
        CHECK(textguard::looksBinary(s), ">30% control bytes marks binary");
    }
    // A few stray control bytes in mostly-text does NOT trip the ratio.
    {
        std::string s(100, 'x'); s[10] = '\x01'; s[20] = '\x02';
        CHECK(!textguard::looksBinary(s), "a couple of control bytes stay text");
    }

    // Size threshold + human-readable size string.
    CHECK(textguard::kMaxEditBytes == 2u * 1024u * 1024u, "edit-size cap is 2 MB");
    CHECK(3u * 1024u * 1024u > textguard::kMaxEditBytes, "3 MB exceeds the cap");
    CHECK(textguard::humanSize(2u * 1024u * 1024u) == "2.0 MB", "humanSize renders MB");
    CHECK(textguard::humanSize(512u * 1024u) == "512 KB", "humanSize renders KB");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--mock-server") == 0)
        return run_mock_server();

    char real[PATH_MAX];
    const char* self = realpath(argv[0], real) ? real : argv[0];

    printf("== Architect LSP framing + round-trip tests ==\n");
    test_framing();
    test_parsers();
    test_fuzzy();
    test_textguard();
    test_roundtrip(self);

    printf("\n%s\n", g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
