// textguard.hpp - cheap, dependency-free guards for deciding whether a file is
// safe to load into the editor textarea.
//
// Opening a binary blob or a multi-gigabyte ISO into a <textarea> streams the
// whole thing across the C<->JS bridge and jams megabytes of garbled bytes into
// the DOM, which lags or hangs the whole app. The backend uses these helpers to
// make that decision cheaply BEFORE reading a whole file: stat the size, sniff
// only the first few KB for binary content, and refuse anything that fails.
//
// Header-only and free of any GTK/WebKit dependency so the detection logic can
// be unit-tested standalone (see tests/lsp_test.cpp).
#ifndef ARCHITECT_TEXTGUARD_HPP
#define ARCHITECT_TEXTGUARD_HPP

#include <string>
#include <cstddef>
#include <cstdio>

namespace textguard {

// Files larger than this are never loaded into the editor (bytes). 2 MB is
// comfortably above any hand-edited source file yet small enough that loading
// it never stalls the UI.
static const size_t kMaxEditBytes = 2u * 1024u * 1024u;

// How many leading bytes to sniff for binary content. A NUL (or a high ratio of
// other non-text control bytes) inside this window marks the file as binary.
static const size_t kSniffBytes = 8192;

// Classify a byte buffer (typically the first kSniffBytes of a file) as binary.
//   * any NUL byte                      -> binary (the classic text/binary test)
//   * >30% "other" control bytes        -> binary (garbled / non-text)
// Bytes >= 0x80 are treated as text: they are valid UTF-8 lead/continuation
// bytes and appear in every non-ASCII source file. Only low control bytes that
// are not ordinary whitespace (tab/LF/CR/FF) or ESC count against the file.
inline bool looksBinary(const std::string& head) {
    if (head.empty()) return false;   // an empty file is editable (text)
    size_t nonText = 0;
    for (unsigned char c : head) {
        if (c == 0x00) return true;                     // NUL => binary
        if (c == 0x09 || c == 0x0A || c == 0x0D || c == 0x0C) continue; // ws
        if (c == 0x1B) continue;                        // ESC (ANSI) is fine
        if (c < 0x20) ++nonText;                        // other C0 controls
        // c >= 0x20 (incl. 0x80..0xFF UTF-8) counts as text
    }
    return nonText * 100 > head.size() * 30;            // >30% => binary
}

// Convenience: read up to kSniffBytes from an already-open FILE* into `head`.
// Leaves the stream position advanced; callers that also need the full content
// should re-open or rewind.
inline void sniff(FILE* f, std::string& head) {
    char buf[kSniffBytes];
    size_t n = std::fread(buf, 1, sizeof(buf), f);
    head.assign(buf, n);
}

// Human-readable size for the "not shown" message: MB with one decimal for
// anything >= 1 MB, otherwise whole KB.
inline std::string humanSize(unsigned long long bytes) {
    char buf[32];
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (mb >= 1.0) std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    else           std::snprintf(buf, sizeof(buf), "%.0f KB", (double)bytes / 1024.0);
    return buf;
}

} // namespace textguard

#endif // ARCHITECT_TEXTGUARD_HPP
