// fuzzy.hpp - a tiny, dependency-free fuzzy subsequence matcher/scorer.
//
// Used by Architect's fuzzy file finder (Ctrl+P): the C++ backend enumerates
// the project tree once, then ranks the cached path list against the user's
// query with fuzzy::match on every keystroke. Header-only and free of any
// GTK/WebKit dependency so it can be unit-tested standalone (see
// tests/lsp_test.cpp).
//
// Scoring model (higher = better; -1 means "no match"):
//   * every matched character                            +1
//   * a run of consecutive matches                       +2 per extra char
//   * a match at the start of a path segment/word        +5
//     (first char, or right after / \ _ - . or space)
//   * a match at offset 0                                +2
//   * a small penalty proportional to the candidate len  (prefer short/close)
// An empty pattern matches everything with a low constant score so the finder
// can show the whole list before the user types anything.
#ifndef ARCHITECT_FUZZY_HPP
#define ARCHITECT_FUZZY_HPP

#include <string>
#include <cctype>

namespace fuzzy {

inline bool isSep(char c) {
    return c == '/' || c == '\\' || c == '_' || c == '-' ||
           c == '.' || c == ' ';
}

// Case-insensitive fuzzy subsequence match. Returns a non-negative score when
// every character of `pat` appears in `str` in order, or -1 when it does not.
inline int match(const std::string& pat, const std::string& str) {
    if (pat.empty()) return 1;
    size_t pi = 0;
    int score = 0;
    int prevMatch = -2;   // index in str of the previously matched char
    bool prevSep = true;  // is the char before str[si] a separator / word start?

    for (size_t si = 0; si < str.size() && pi < pat.size(); ++si) {
        char pc = (char)std::tolower((unsigned char)pat[pi]);
        char sc = (char)std::tolower((unsigned char)str[si]);
        if (pc == sc) {
            score += 1;
            if ((int)si == prevMatch + 1) score += 2;  // consecutive run
            if (prevSep) score += 5;                   // start of a segment
            if (si == 0)  score += 2;                  // very first char
            prevMatch = (int)si;
            ++pi;
        }
        prevSep = isSep(str[si]);
    }

    if (pi < pat.size()) return -1;    // not all pattern chars consumed
    score -= (int)(str.size() / 32);   // gentle length penalty
    return score < 0 ? 0 : score;
}

} // namespace fuzzy

#endif // ARCHITECT_FUZZY_HPP
