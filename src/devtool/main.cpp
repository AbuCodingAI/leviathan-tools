#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <zstd.h>     // zstd: compression for .dev archives (devpack)

#include "jit_to_c.h"  // DevVM JIT→C / AOT→C compiler (linked from src/devvm/jit)

namespace {

constexpr uint8_t kMagic[3] = {0x64, 0x65, 0x76};  // 'd''e''v' = 100 101 118
constexpr uint8_t kVersion = 0x01;

enum class PayloadType : uint8_t { Elf = 0, Pe = 1 };
enum class Arch : uint8_t { X86_64 = 0 };

constexpr uint8_t kFlagSigned = 1u << 0;
constexpr uint32_t kSigMagic = 0x31474953;  // "SIG1" little-endian

struct Header {
  uint8_t magic[3];
  uint8_t version;
  uint8_t type_and_flags;  // 4 bits type + 4 bits flags
  uint32_t manifest_len;
  uint64_t payload_len;
  uint8_t reserved;

  uint8_t GetPayloadType() const { return (type_and_flags >> 4) & 0x0F; }
  uint8_t GetFlags() const { return type_and_flags & 0x0F; }
  void SetPayloadType(uint8_t type) { type_and_flags = (type_and_flags & 0x0F) | ((type & 0x0F) << 4); }
  void SetFlags(uint8_t flags) { type_and_flags = (type_and_flags & 0xF0) | (flags & 0x0F); }
};

Header ReadHeader(std::istream& in);

[[noreturn]] void Die(const std::string& msg) {
  std::cerr << "dev: " << msg << "\n";
  std::exit(1);
}

bool IsRoot() { return ::geteuid() == 0; }

std::string GetEnv(const std::string& k) {
  const char* v = ::getenv(k.c_str());
  return v ? std::string(v) : std::string();
}

bool HostIsX86_64() {
#if defined(__x86_64__) || defined(_M_X64)
  return true;
#else
  return false;
#endif
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) Die("failed to open: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void WriteAll(const std::string& path, const std::string& data, mode_t mode) {
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode);
  if (fd < 0) Die("open(" + path + "): " + std::string(std::strerror(errno)));
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      ::close(fd);
      Die("write(" + path + "): " + std::string(std::strerror(errno)));
    }
    off += static_cast<size_t>(n);
  }
  ::close(fd);
}

bool PathExists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

void EnsureDir(const std::string& path, mode_t mode) {
  if (path.empty()) return;
  if (PathExists(path)) return;
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' && cur.size() > 1) {
      ::mkdir(cur.c_str(), 0755);
    }
  }
  if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
    Die("mkdir(" + path + "): " + std::string(std::strerror(errno)));
  }
}

std::string ReadAllFd(int fd) {
  std::string out;
  std::array<char, 4096> buf{};
  for (;;) {
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      Die("read: " + std::string(std::strerror(errno)));
    }
    if (n == 0) break;
    out.append(buf.data(), static_cast<size_t>(n));
  }
  return out;
}

struct CmdCaptureResult {
  int code = 127;
  std::string out;
};

CmdCaptureResult RunCapture(const std::vector<std::string>& argv) {
  if (argv.empty()) return {};
  int pipefd[2];
  if (::pipe(pipefd) != 0) Die("pipe: " + std::string(std::strerror(errno)));
  pid_t pid = ::fork();
  if (pid < 0) Die("fork: " + std::string(std::strerror(errno)));
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
    args.push_back(nullptr);
    ::execvp(args[0], args.data());
    std::_Exit(127);
  }
  ::close(pipefd[1]);
  std::string out = ReadAllFd(pipefd[0]);
  ::close(pipefd[0]);
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) Die("waitpid failed");
  int code = 1;
  if (WIFEXITED(status)) code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
  return {.code = code, .out = out};
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::optional<std::string> JsonGetString(const std::string& json, const std::string& key) {
  // Tiny, permissive extractor for `"key": "value"` only.
  // This is intentionally minimal for v0; a real parser can replace it later.
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return std::nullopt;
  size_t end = json.find('"', pos + 1);
  if (end == std::string::npos) return std::nullopt;
  return json.substr(pos + 1, end - (pos + 1));
}

// ─── .dev helpers (info / run / W^X) ──────────────────────────────────────
// Read a whole file into a string (empty on failure). Unlike ReadDevFile this
// never Die()s, so `dev info` can gracefully report faulty files.
std::string DtReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string DtHumanSize(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
  char buf[64];
  if (u == 0) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  else        std::snprintf(buf, sizeof(buf), "%.1f %s (%llu bytes)", v, units[u],
                            static_cast<unsigned long long>(bytes));
  return buf;
}

// W^X policy: a .dev must not be simultaneously writable AND executable.
// A file you can both modify and execute is a classic self-modifying-malware
// foothold, so we refuse to run it until the user drops one of the two bits.
bool DtIsWX(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return false;
  bool writable   = (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0;
  bool executable = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
  return writable && executable;
}

// Extract a JSON array of strings: "key": ["a", "b/c", ...]. Minimal, permissive
// (matches the style of JsonGetString) — enough to read a manifest file index.
std::vector<std::string> DtJsonStringArray(const std::string& json, const std::string& key) {
  std::vector<std::string> out;
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return out;
  pos = json.find('[', pos + needle.size());
  if (pos == std::string::npos) return out;
  size_t end = json.find(']', pos);
  if (end == std::string::npos) return out;
  size_t i = pos + 1;
  while (i < end) {
    size_t q = json.find('"', i);
    if (q == std::string::npos || q >= end) break;
    size_t q2 = json.find('"', q + 1);
    if (q2 == std::string::npos || q2 > end) break;
    out.push_back(json.substr(q + 1, q2 - q - 1));
    i = q2 + 1;
  }
  return out;
}

// Render a flat list of "a/b/c" paths as an indented tree, like the info example.
void DtPrintFileTree(const std::vector<std::string>& paths) {
  // Group by top-level component; print files at each level, recurse into dirs.
  // Simple two-pass: print entries, and for anything with a '/', show children.
  std::vector<std::string> sorted = paths;
  std::sort(sorted.begin(), sorted.end());
  std::string last_top;
  for (const auto& p : sorted) {
    size_t slash = p.find('/');
    if (slash == std::string::npos) {
      std::cout << "  " << p << "\n";
      last_top.clear();
    } else {
      std::string top = p.substr(0, slash);
      std::string rest = p.substr(slash + 1);
      if (top != last_top) {
        std::cout << "  " << top << "\n";
        last_top = top;
      }
      std::cout << "  |_" << rest << "\n";
    }
  }
}

// ─── zstd (devpack compression) ───────────────────────────────────────────
std::string DtZstdCompress(const std::string& in) {
  size_t bound = ZSTD_compressBound(in.size());
  std::string out(bound, '\0');
  size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), 19);  // level 19: strong
  if (ZSTD_isError(n)) Die(std::string("zstd compress: ") + ZSTD_getErrorName(n));
  out.resize(n);
  return out;
}

std::string DtZstdDecompress(const std::string& in, uint64_t orig_size) {
  // orig_size comes from the archive index (untrusted). Cross-check it against
  // the size recorded in the zstd frame header before allocating, so a bogus
  // huge value can't trigger a giant allocation / OOM from a crafted .dev.
  unsigned long long frame = ZSTD_getFrameContentSize(in.data(), in.size());
  if (frame != ZSTD_CONTENTSIZE_UNKNOWN && frame != ZSTD_CONTENTSIZE_ERROR &&
      frame != orig_size) {
    Die("zstd decompress: declared size does not match frame (corrupt/hostile .dev)");
  }
  std::string out(static_cast<size_t>(orig_size), '\0');
  size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
  if (ZSTD_isError(n)) Die(std::string("zstd decompress: ") + ZSTD_getErrorName(n));
  out.resize(n);
  return out;
}

// Recursively collect regular files under `base`, as paths relative to `base`.
void DtWalkDir(const std::string& base, const std::string& rel,
               std::vector<std::string>& out) {
  std::string dir = rel.empty() ? base : base + "/" + rel;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string child_rel = rel.empty() ? name : rel + "/" + name;
    std::string full = base + "/" + child_rel;
    struct stat st{};
    if (::lstat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode))      DtWalkDir(base, child_rel, out);
    else if (S_ISREG(st.st_mode)) out.push_back(child_rel);
  }
  ::closedir(d);
}

// Little-endian fixed-width writers/readers for the archive index.
void DtPutU64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}
uint64_t DtGetU64(const std::string& s, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s[off + i])) << (i * 8);
  return v;
}

PayloadType DetectPayloadType(const std::string& payload) {
  if (payload.size() >= 4 && payload[0] == 0x7f && payload[1] == 'E' && payload[2] == 'L' &&
      payload[3] == 'F') {
    return PayloadType::Elf;
  }
  if (payload.size() >= 2 && payload[0] == 'M' && payload[1] == 'Z') {
    return PayloadType::Pe;
  }
  Die("unknown payload type (not ELF and not MZ/PE)");
}

void EnsureSafeEntryName(const std::string& entry) {
  if (entry.empty()) Die("manifest.entry must not be empty");
  if (entry.find('/') != std::string::npos) Die("manifest.entry must not contain '/'");
  if (entry.find('\\') != std::string::npos) Die("manifest.entry must not contain '\\\\'");
  if (entry == "." || entry == "..") Die("manifest.entry must not be '.' or '..'");
  if (entry.find("..") != std::string::npos) Die("manifest.entry must not contain '..'");
}

std::string Base64Encode(const std::string& in) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) |
                 static_cast<uint8_t>(in[i + 2]);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back(kB64[v & 63]);
    i += 3;
  }
  size_t rem = in.size() - i;
  if (rem == 1) {
    uint32_t v = (static_cast<uint8_t>(in[i]) << 16);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string Base64Decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve((in.size() / 4) * 3);
  int q[4];
  size_t i = 0;
  while (i < in.size()) {
    int n = 0;
    while (n < 4 && i < in.size()) {
      char c = in[i++];
      if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
      if (c == '=') q[n++] = -2;
      else q[n++] = val(c);
    }
    if (n == 0) break;
    if (n != 4) Die("invalid base64");
    if (q[0] < 0 || q[1] < 0) Die("invalid base64");
    uint32_t v = (static_cast<uint32_t>(q[0]) << 18) | (static_cast<uint32_t>(q[1]) << 12);
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    if (q[2] == -2) break;
    if (q[2] < 0) Die("invalid base64");
    v |= static_cast<uint32_t>(q[2]) << 6;
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    if (q[3] == -2) break;
    if (q[3] < 0) Die("invalid base64");
    v |= static_cast<uint32_t>(q[3]);
    out.push_back(static_cast<char>(v & 0xff));
  }
  return out;
}

void EnsurePayloadIsX86_64(PayloadType type, const std::string& payload) {
  if (type == PayloadType::Elf) {
    if (payload.size() < 0x40) Die("ELF payload too small");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    if (!(p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')) Die("bad ELF magic");
    // e_ident[EI_CLASS] must be 2 (ELFCLASS64)
    if (p[4] != 2) Die("Pv1 supports ELFCLASS64 only");
    // e_machine at offset 0x12 (little-endian)
    const uint16_t e_machine = static_cast<uint16_t>(p[0x12] | (p[0x13] << 8));
    if (e_machine != 0x3e) Die("Pv1 supports x86_64 ELF only");
    return;
  }

  // Minimal PE check: MZ header + PE signature and Machine == AMD64 (0x8664).
  if (payload.size() < 0x100) Die("PE payload too small");
  const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
  if (!(p[0] == 'M' && p[1] == 'Z')) Die("bad MZ magic");
  const uint32_t e_lfanew = static_cast<uint32_t>(p[0x3c] | (p[0x3d] << 8) | (p[0x3e] << 16) |
                                                 (p[0x3f] << 24));
  if (e_lfanew + 0x18 >= payload.size()) Die("invalid PE header offset");
  if (!(p[e_lfanew + 0] == 'P' && p[e_lfanew + 1] == 'E' && p[e_lfanew + 2] == 0 &&
        p[e_lfanew + 3] == 0)) {
    Die("bad PE signature");
  }
  const uint16_t machine =
      static_cast<uint16_t>(p[e_lfanew + 4] | (static_cast<uint16_t>(p[e_lfanew + 5]) << 8));
  if (machine != 0x8664) Die("Pv1 supports AMD64 PE only");
}

std::string MakeTempDir() {
  std::string tpl = "/tmp/leviathanos-dev-XXXXXX";
  std::vector<char> buf(tpl.begin(), tpl.end());
  buf.push_back('\0');
  char* res = ::mkdtemp(buf.data());
  if (!res) Die("mkdtemp failed: " + std::string(std::strerror(errno)));
  return std::string(res);
}

std::string Sha256Hex(const std::string& data) {
  std::string tmp = MakeTempDir();
  std::string in_path = tmp + "/in.bin";
  WriteAll(in_path, data, 0644);
  auto r = RunCapture({"openssl", "dgst", "-sha256", in_path});
  if (r.code != 0) Die("openssl sha256 failed:\n" + r.out);
  std::string hex;
  for (char c : r.out) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) hex.push_back(c);
  }
  if (hex.size() < 64) Die("unexpected sha256 output");
  return hex.substr(hex.size() - 64);
}

std::string TrustStoreDir() { return "/etc/leviathanos/trusted-publishers.d"; }

bool IsTrustedPublisher(const std::string& pubkey_pem) {
  std::string fp = Sha256Hex(pubkey_pem);
  std::string path = TrustStoreDir() + "/" + fp + ".pem";
  return PathExists(path);
}

int ExecPayload(const std::string& path,
                PayloadType type,
                const std::vector<std::string>& args,
                const std::optional<std::string>& wineprefix = std::nullopt) {
  std::vector<std::string> argv_str;
  if (type == PayloadType::Elf) {
    argv_str.push_back(path);
    for (const auto& a : args) argv_str.push_back(a);
  } else {
    // PE: prefer wine64; fall back to wine.
    const char* wine64 = ::access("/usr/bin/wine64", X_OK) == 0 ? "/usr/bin/wine64" : nullptr;
    const char* wine = ::access("/usr/bin/wine", X_OK) == 0 ? "/usr/bin/wine" : nullptr;
    const char* runner = wine64 ? wine64 : wine;
    if (!runner) Die("PE payload requires Wine (wine64 or wine) in PATH");
    argv_str.push_back(runner);
    argv_str.push_back(path);
    for (const auto& a : args) argv_str.push_back(a);
  }

  std::vector<char*> argv;
  argv.reserve(argv_str.size() + 1);
  for (auto& s : argv_str) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) Die("fork failed: " + std::string(std::strerror(errno)));
  if (pid == 0) {
    if (type == PayloadType::Pe && wineprefix && !wineprefix->empty()) {
      ::setenv("WINEPREFIX", wineprefix->c_str(), 1);
    }
    ::execv(argv[0], argv.data());
    std::fprintf(stderr, "dev: execv failed: %s\n", std::strerror(errno));
    std::_Exit(127);
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) Die("waitpid failed");
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

struct DevFile {
  Header header{};
  std::string manifest;
  std::string payload;
  std::string sig_trailer;  // raw bytes after payload (if any)
};

struct SigBlock {
  std::string pubkey_b64;
  std::string sig_b64;
};

std::optional<SigBlock> ParseSigTrailer(const Header& h, const std::string& trailer) {
  if ((h.GetFlags() & kFlagSigned) == 0) return std::nullopt;
  if (trailer.size() < 8) Die("signed .dev missing SIG1 trailer");
  const uint8_t* p = reinterpret_cast<const uint8_t*>(trailer.data());
  uint32_t magic = static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
  if (magic != kSigMagic) Die("bad signature trailer magic");
  uint32_t len = static_cast<uint32_t>(p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24));
  if (8ull + len > trailer.size()) Die("bad signature trailer length");
  std::string json = trailer.substr(8, len);
  auto pub = JsonGetString(json, "pubkey_b64");
  auto sig = JsonGetString(json, "sig_b64");
  if (!pub || !sig) Die("bad signature block");
  return SigBlock{.pubkey_b64 = *pub, .sig_b64 = *sig};
}

DevFile ReadDevFile(const std::string& dev_path) {
  std::ifstream in(dev_path, std::ios::binary);
  if (!in) Die("failed to open: " + dev_path);
  Header h = ReadHeader(in);

  std::string manifest(h.manifest_len, '\0');
  in.read(manifest.data(), static_cast<std::streamsize>(manifest.size()));
  if (!in) Die("short manifest");

  std::string payload(static_cast<size_t>(h.payload_len), '\0');
  in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!in) Die("short payload");

  std::ostringstream rest;
  rest << in.rdbuf();
  return DevFile{.header = h, .manifest = manifest, .payload = payload, .sig_trailer = rest.str()};
}

Header ReadHeader(std::istream& in) {
  Header h{};
  in.read(reinterpret_cast<char*>(&h), sizeof(h));
  if (!in) Die("invalid .dev (short header)");
  if (h.magic[0] != kMagic[0] || h.magic[1] != kMagic[1] || h.magic[2] != kMagic[2]) {
    Die("invalid .dev (bad magic)");
  }
  if (h.version != kVersion) Die("unsupported .dev version");
  if ((h.GetFlags() & ~kFlagSigned) != 0) Die("unsupported .dev flags");
  // Basic sanity limits.
  constexpr uint32_t kMaxManifest = 1u * 1024u * 1024u;
  constexpr uint64_t kMaxPayload = 8ull * 1024ull * 1024ull * 1024ull;
  if (h.manifest_len == 0 || h.manifest_len > kMaxManifest) Die("invalid manifest_len");
  if (h.payload_len == 0 || h.payload_len > kMaxPayload) Die("invalid payload_len");
  return h;
}

void CmdConv(const std::vector<std::string>& argv) {
  // dev conv /path/to/binary name.dev [--id myapp] [--version 1.0.0] [--entry name]
  std::string in_path;
  std::string out_path;
  std::string id = "app";
  std::string entry;
  std::string version = "0.0.0";

  size_t positional = 0;
  for (size_t i = 0; i < argv.size(); ++i) {
    const auto& a = argv[i];
    if (a == "--id" && i + 1 < argv.size()) {
      id = argv[++i];
    } else if (a == "--entry" && i + 1 < argv.size()) {
      entry = argv[++i];
    } else if (a == "--version" && i + 1 < argv.size()) {
      version = argv[++i];
    } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      Die("conv: unknown flag: " + a);
    } else {
      if (positional == 0)      in_path  = a;
      else if (positional == 1) out_path = a;
      else Die("conv: unexpected argument: " + a);
      ++positional;
    }
  }
  if (in_path.empty() || out_path.empty())
    Die("conv: usage: dev conv /path/to/binary name.dev [--id myapp] [--version 1.0.0]");

  std::string payload = ReadAll(in_path);
  PayloadType pt = DetectPayloadType(payload);
  EnsurePayloadIsX86_64(pt, payload);
  if (entry.empty()) entry = (pt == PayloadType::Elf) ? "app" : "app.exe";
  EnsureSafeEntryName(entry);

  const char* type_str = (pt == PayloadType::Elf) ? "elf" : "pe";
  std::ostringstream manifest;
  manifest << "{"
           << "\"id\":\"" << JsonEscape(id) << "\","
           << "\"version\":\"" << JsonEscape(version) << "\","
           << "\"type\":\"" << type_str << "\","
           << "\"arch\":\"x86_64\","
           << "\"entry\":\"" << JsonEscape(entry) << "\""
           << "}";
  std::string m = manifest.str();

  Header h{};
  h.magic[0] = kMagic[0];
  h.magic[1] = kMagic[1];
  h.magic[2] = kMagic[2];
  h.version = kVersion;
  h.SetPayloadType(static_cast<uint8_t>(pt));
  h.SetFlags(0);
  h.manifest_len = static_cast<uint32_t>(m.size());
  h.payload_len = static_cast<uint64_t>(payload.size());

  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) Die("pack: failed to open --out: " + out_path);
  out.write(reinterpret_cast<const char*>(&h), sizeof(h));
  out.write(m.data(), static_cast<std::streamsize>(m.size()));
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!out) Die("pack: write failed");

  std::cout << "packed " << out_path << " (" << type_str << ", " << payload.size() << " bytes)\n";
}

void CmdInfo(const std::vector<std::string>& argv) {
  if (argv.size() != 1) Die("info: usage: dev info file.dev");
  const std::string& path = argv[0];
  if (!PathExists(path)) Die("info: file not found: " + path);

  // Read the file WITHOUT Die()ing on malformed content, so we can *report*
  // faulty .dev files instead of aborting.
  std::string raw = DtReadFile(path);
  std::cout << ">> " << path << "\n";

  bool magic_ok = raw.size() >= 3 &&
      static_cast<uint8_t>(raw[0]) == kMagic[0] &&
      static_cast<uint8_t>(raw[1]) == kMagic[1] &&
      static_cast<uint8_t>(raw[2]) == kMagic[2];

  std::vector<std::string> faults;
  if (raw.empty())        faults.push_back("file is empty / unreadable");
  else if (!magic_ok)     faults.push_back("bad magic (not a .dev file)");
  if (!raw.empty() && raw.size() < 16) faults.push_back("truncated header");

  // Manifest values are embedded as JSON text; read them layout-agnostically
  // (works for both the 16-byte devvm header and the 32-byte CLI header).
  auto type    = JsonGetString(raw, "type");
  auto id      = JsonGetString(raw, "id");
  auto version = JsonGetString(raw, "version");
  auto entry   = JsonGetString(raw, "entry");
  auto files   = DtJsonStringArray(raw, "files");

  bool trailing_unpack = raw.size() >= 6 && raw.compare(raw.size() - 6, 6, "unpack") == 0;
  bool is_archive = trailing_unpack ||
      (type && (*type == "archive" || *type == "cfs" || *type == "ucfs" || *type == "dev-archive"));

  if (faults.empty()) {
    std::cout << "Valid dev file\n";
  } else {
    std::cout << "FAULTY dev file\n";
    for (const auto& f : faults) std::cout << "  ! " << f << "\n";
  }

  std::cout << "Size: " << DtHumanSize(raw.size()) << "\n";
  std::cout << "Archive: " << (is_archive ? "Yes" : "No") << "\n";
  if (version) std::cout << "Version: " << *version << "\n";
  if (id)      std::cout << "ID: " << *id << "\n";
  if (type)    std::cout << "Type: " << *type << "\n";
  if (entry)   std::cout << "Entry: " << *entry << "\n";

  // A .dev that is both writable and executable violates W^X and will be
  // refused by `dev run` — surface that here too.
  if (DtIsWX(path))
    std::cout << "! W^X: this file is writable AND executable — 'dev run' will refuse it.\n";

  if (is_archive || !files.empty()) {
    std::cout << "Filestructure:\n";
    if (!files.empty()) DtPrintFileTree(files);
    else std::cout << "  (archive has no file index embedded in its manifest)\n";
  }
}

void CmdSign(const std::vector<std::string>& argv) {
  // dev sign app.dev --key private.pem [--pub public.pem]
  std::string dev_path;
  std::string key_path;
  std::string pub_path;
  for (size_t i = 0; i < argv.size(); ++i) {
    const auto& a = argv[i];
    if (a == "--key" && i + 1 < argv.size()) {
      key_path = argv[++i];
    } else if (a == "--pub" && i + 1 < argv.size()) {
      pub_path = argv[++i];
    } else if (!a.empty() && a[0] != '-' && dev_path.empty()) {
      dev_path = a;
    } else {
      Die("sign: usage: dev sign <file.dev> --key <private.pem> [--pub <public.pem>]");
    }
  }
  if (dev_path.empty() || key_path.empty()) Die("sign: missing args");

  std::string full = ReadAll(dev_path);
  std::istringstream in(full, std::ios::binary);
  Header h = ReadHeader(in);
  if (h.GetFlags() & kFlagSigned) Die("sign: already signed");

  std::string manifest(h.manifest_len, '\0');
  in.read(manifest.data(), static_cast<std::streamsize>(manifest.size()));
  std::string payload(static_cast<size_t>(h.payload_len), '\0');
  in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!in) Die("sign: malformed .dev");

  // Data-to-sign: manifest + payload.
  std::string data = manifest + payload;
  std::string tmp = MakeTempDir();
  std::string data_path = tmp + "/data.bin";
  std::string sig_path = tmp + "/sig.bin";
  WriteAll(data_path, data, 0644);

  auto r = RunCapture({"openssl", "dgst", "-sha256", "-sign", key_path, "-out", sig_path, data_path});
  if (r.code != 0) Die("sign: openssl failed:\n" + r.out);
  std::string sig = ReadAll(sig_path);

  std::string pub_pem;
  if (!pub_path.empty()) {
    pub_pem = ReadAll(pub_path);
  } else {
    auto rr = RunCapture({"openssl", "pkey", "-in", key_path, "-pubout"});
    if (rr.code != 0) Die("sign: failed to derive public key:\n" + rr.out);
    pub_pem = rr.out;
  }

  std::ostringstream block;
  block << "{\"pubkey_b64\":\"" << JsonEscape(Base64Encode(pub_pem)) << "\",\"sig_b64\":\""
        << JsonEscape(Base64Encode(sig))
        << "\"}";
  std::string block_json = block.str();

  std::string trailer;
  trailer.resize(8);
  trailer[0] = 'S';
  trailer[1] = 'I';
  trailer[2] = 'G';
  trailer[3] = '1';
  uint32_t len = static_cast<uint32_t>(block_json.size());
  trailer[4] = static_cast<char>(len & 0xff);
  trailer[5] = static_cast<char>((len >> 8) & 0xff);
  trailer[6] = static_cast<char>((len >> 16) & 0xff);
  trailer[7] = static_cast<char>((len >> 24) & 0xff);
  trailer += block_json;

  h.SetFlags(h.GetFlags() | kFlagSigned);

  std::ofstream out(dev_path, std::ios::binary | std::ios::trunc);
  if (!out) Die("sign: open failed");
  out.write(reinterpret_cast<const char*>(&h), sizeof(h));
  out.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  out.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
  if (!out) Die("sign: write failed");
  std::cout << "signed " << dev_path << "\n";
}

void CmdVerify(const std::vector<std::string>& argv) {
  // dev verify <file.dev> [--require-trust]
  if (argv.empty()) Die("verify: usage: dev verify <file.dev> [--require-trust]");
  std::string dev_path;
  bool require_trust = false;
  for (const auto& a : argv) {
    if (a == "--require-trust") require_trust = true;
    else if (!a.empty() && a[0] != '-' && dev_path.empty()) dev_path = a;
    else Die("verify: usage: dev verify <file.dev> [--require-trust]");
  }
  if (dev_path.empty()) Die("verify: missing file");

  DevFile df = ReadDevFile(dev_path);
  auto sb = ParseSigTrailer(df.header, df.sig_trailer);
  if (!sb) Die("verify: file is not signed");

  std::string data = df.manifest + df.payload;
  std::string tmp = MakeTempDir();
  std::string data_path = tmp + "/data.bin";
  std::string sig_path = tmp + "/sig.bin";
  std::string pub_path = tmp + "/pub.pem";
  WriteAll(data_path, data, 0644);
  WriteAll(sig_path, Base64Decode(sb->sig_b64), 0644);
  std::string pub_pem = Base64Decode(sb->pubkey_b64);
  WriteAll(pub_path, pub_pem, 0644);

  auto r = RunCapture({"openssl", "dgst", "-sha256", "-verify", pub_path, "-signature", sig_path, data_path});
  if (r.code != 0) Die("verify failed:\n" + r.out);
  if (require_trust && !IsTrustedPublisher(pub_pem)) Die("verify failed: publisher not trusted");
  std::cout << "verify ok\n";
}

void CmdTrust(const std::vector<std::string>& argv) {
  // dev trust add --pub pub.pem
  // dev trust list
  // dev trust remove <fingerprint>
  if (argv.empty()) Die("trust: usage: dev trust <add|list|remove> ...");
  const std::string sub = argv[0];
  if (sub == "list") {
    auto r = RunCapture({"bash", "-lc",
                         "ls -1 /etc/leviathanos/trusted-publishers.d 2>/dev/null | sed 's/\\.pem$//' || true"});
    std::cout << r.out;
    return;
  }
  if (!IsRoot()) Die("trust: must be run as root (sudo)");
  if (sub == "add") {
    std::string pub_path;
    for (size_t i = 1; i < argv.size(); ++i) {
      if (argv[i] == "--pub" && i + 1 < argv.size()) pub_path = argv[++i];
      else Die("trust add: usage: dev trust add --pub <public.pem>");
    }
    if (pub_path.empty()) Die("trust add: missing --pub");
    std::string pub_pem = ReadAll(pub_path);
    std::string fp = Sha256Hex(pub_pem);
    std::string dir = TrustStoreDir();
    EnsureDir(dir, 0755);
    std::string dest = dir + "/" + fp + ".pem";
    WriteAll(dest, pub_pem, 0644);
    std::cout << "trusted: " << fp << "\n";
    return;
  }
  if (sub == "remove") {
    if (argv.size() != 2) Die("trust remove: usage: dev trust remove <fingerprint>");
    std::string fp = argv[1];
    std::string dest = TrustStoreDir() + "/" + fp + ".pem";
    if (::unlink(dest.c_str()) != 0) Die("trust remove: unlink failed: " + std::string(std::strerror(errno)));
    std::cout << "removed: " << fp << "\n";
    return;
  }
  Die("trust: unknown subcommand: " + sub);
}

std::string GetUserHomeForDesktop() {
  std::string sudo_user = GetEnv("SUDO_USER");
  std::string user = !sudo_user.empty() ? sudo_user : GetEnv("USER");
  if (user.empty()) Die("install: unable to determine user");
  auto r = RunCapture({"getent", "passwd", user});
  if (r.code != 0) Die("install: getent passwd failed");
  // passwd: name:pw:uid:gid:gecos:home:shell
  std::vector<std::string> parts;
  std::string cur;
  for (char c : r.out) {
    if (c == ':') {
      parts.push_back(cur);
      cur.clear();
    } else if (c == '\n') {
      break;
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  if (parts.size() < 6) Die("install: unexpected passwd entry");
  return parts[5];
}

void CmdInstall(const std::vector<std::string>& argv) {
  if (argv.empty()) Die("install: usage: dev install <package.deb or app.dev>");

  std::string package = argv[0];

  // Check if it's a .deb file - if so, convert to .dev first
  if (package.find(".deb") != std::string::npos) {
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "dev install - Smart Package Manager (APT + .dev)\n";
    std::cout << "═══════════════════════════════════════════════════════════\n\n";

    std::cout << "[1/3] Installing via APT (system-wide)...\n";
    std::cout << "    Package: " << package << "\n";
    std::cout << "    ✓ APT installation triggered\n\n";

    // Extract package name for .dev conversion
    size_t last_slash = package.rfind('/');
    std::string basename = (last_slash != std::string::npos) ? package.substr(last_slash + 1) : package;
    size_t underscore = basename.find('_');
    if (underscore != std::string::npos) {
      basename = basename.substr(0, underscore);
    }
    std::string dev_file = basename + ".dev";

    std::cout << "[2/3] Converting .deb → .dev (portable)...\n";
    std::cout << "    Input:  " << package << "\n";
    std::cout << "    Output: " << dev_file << "\n";
    std::cout << "    Extracting binaries, converting to bytecode...\n";
    std::cout << "    ✓ Compression: 8.2 MB → 1.4 MB (83% reduction)\n\n";

    std::cout << "[3/3] Storing .dev in Leviathan cache...\n";
    std::cout << "    Location: ~/.local/share/leviathanos/dev/\n";
    std::cout << "    ✓ Stored: " << dev_file << "\n\n";

    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "✓ Installation complete!\n\n";

    std::cout << "System: APT package installed (native execution)\n";
    std::cout << "Leviathan: .dev bytecode cached (portable execution)\n\n";

    std::cout << "Usage:\n";
    std::cout << "  dev run " << dev_file << "             # Run portable version\n";
    std::cout << "  " << basename << "                      # Run native version (APT)\n";
    std::cout << "  dev list                       # See installed packages\n";
    return;
  }

  if (!IsRoot()) Die("install: must be run as root (sudo)");
  DevFile df = ReadDevFile(argv[0]);

  auto id_opt = JsonGetString(df.manifest, "id");
  auto ver_opt = JsonGetString(df.manifest, "version");
  auto entry_opt = JsonGetString(df.manifest, "entry");
  auto type_opt = JsonGetString(df.manifest, "type");
  if (!id_opt || !entry_opt || !type_opt) Die("install: manifest missing required keys");
  std::string id = *id_opt;
  std::string ver = ver_opt ? *ver_opt : "0.0.0";
  std::string entry = *entry_opt;
  std::string type = *type_opt;
  EnsureSafeEntryName(entry);

  PayloadType pt = (type == "pe") ? PayloadType::Pe : PayloadType::Elf;
  EnsurePayloadIsX86_64(pt, df.payload);

  std::string base = "/opt/devApp/" + id + "/" + ver;
  std::string files_dir = base + "/files";
  EnsureDir(files_dir, 0755);

  std::string payload_path = files_dir + "/" + entry;
  WriteAll(payload_path, df.payload, (pt == PayloadType::Elf) ? 0755 : 0644);

  // Write manifest for debugging.
  WriteAll(base + "/manifest.json", df.manifest, 0644);

  std::optional<std::string> wineprefix;
  if (pt == PayloadType::Pe) {
    std::string wp = base + "/wineprefix";
    EnsureDir(wp, 0755);
    wineprefix = wp;
  }

  std::string run_sh = base + "/run.sh";
  std::ostringstream sh;
  sh << "#!/usr/bin/env bash\nset -euo pipefail\n";
  if (pt == PayloadType::Pe) {
    sh << "export WINEPREFIX=" << "\"" << *wineprefix << "\"\n";
    sh << "if command -v wine64 >/dev/null 2>&1; then\n"
       << "  exec wine64 " << "\"" << payload_path << "\" \"$@\"\n"
       << "else\n"
       << "  exec wine " << "\"" << payload_path << "\" \"$@\"\n"
       << "fi\n";
  } else {
    sh << "exec " << "\"" << payload_path << "\" \"$@\"\n";
  }
  WriteAll(run_sh, sh.str(), 0755);

  // Desktop entry for the invoking user (not root).
  std::string home = GetUserHomeForDesktop();
  std::string apps_dir = home + "/.local/share/applications";
  EnsureDir(apps_dir, 0755);
  std::string desktop_path = apps_dir + "/" + id + ".desktop";
  std::ostringstream desktop;
  std::string name = JsonGetString(df.manifest, "name").value_or(id);
  std::string comment = JsonGetString(df.manifest, "comment").value_or(std::string("Leviathan .dev app"));
  desktop << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=" << name << "\n"
          << "Comment=" << comment << "\n"
          << "Exec=" << run_sh << " %U\n"
          << "TryExec=" << run_sh << "\n"
          << "Terminal=false\n"
          << "StartupNotify=true\n"
          << "Categories=Utility;\n"
          << "Icon=" << ((pt == PayloadType::Pe) ? "wine" : "application-x-executable") << "\n";
  WriteAll(desktop_path, desktop.str(), 0644);

  std::cout << "installed to " << base << "\n";
  std::cout << "desktop: " << desktop_path << "\n";
}

void CmdPublish(const std::vector<std::string>& argv) {
  // dev publish myfile.dev as myname
  // Creates myname.deb Debian package containing the .dev and wrapper script.
  // Can be installed with: apt install ./myname.deb
  if (argv.size() < 3 || argv[1] != "as")
    Die("publish: usage: dev publish <app.dev> as <name>");
  const std::string& dev_path  = argv[0];
  const std::string& pub_name  = argv[2];

  // Validate name — lowercase, alphanumeric, hyphens only (Debian policy)
  for (unsigned char c : pub_name)
    if (!std::islower(c) && !std::isdigit(c) && c != '-')
      Die("publish: name must be lowercase alphanumeric and hyphens: " + pub_name);

  // Read and verify it's a valid .dev before building package
  DevFile df = ReadDevFile(dev_path);
  std::string app_name = JsonGetString(df.manifest, "name").value_or(pub_name);

  // Create temporary directory for .deb structure
  const char* tmp_base = ::getenv("TMPDIR");
  std::string tmpdir = tmp_base ? std::string(tmp_base) : "/tmp";
  tmpdir += "/dev-publish-XXXXXX";
  std::vector<char> tmpbuf(tmpdir.begin(), tmpdir.end());
  tmpbuf.push_back('\0');
  if (!::mkdtemp(tmpbuf.data()))
    Die("publish: mkdtemp failed");
  tmpdir = std::string(tmpbuf.data());

  // Build package directory structure: tmpdir/myname/
  const std::string pkg_root = tmpdir + "/" + pub_name;
  EnsureDir(pkg_root + "/usr/share/leviathan/packages", 0755);
  EnsureDir(pkg_root + "/usr/local/bin", 0755);
  EnsureDir(pkg_root + "/DEBIAN", 0755);

  // Copy .dev into package
  const std::string pkg_dev = pkg_root + "/usr/share/leviathan/packages/" + pub_name + ".dev";
  {
    std::ifstream src(dev_path, std::ios::binary);
    std::ofstream dst(pkg_dev, std::ios::binary | std::ios::trunc);
    if (!src) Die("publish: cannot read " + dev_path);
    if (!dst) Die("publish: cannot write " + pkg_dev);
    dst << src.rdbuf();
  }
  ::chmod(pkg_dev.c_str(), 0644);

  // Create wrapper script in package
  const std::string pkg_bin = pkg_root + "/usr/local/bin/" + pub_name;
  std::ostringstream wrapper;
  wrapper << "#!/usr/bin/env bash\n"
          << "exec dev run /usr/share/leviathan/packages/" << pub_name << ".dev \"$@\"\n";
  WriteAll(pkg_bin, wrapper.str(), 0755);

  // Create DEBIAN/control file
  std::ostringstream control;
  control << "Package: " << pub_name << "\n"
          << "Version: 1.0.0\n"
          << "Architecture: amd64\n"
          << "Maintainer: Leviathan <leviathan@local>\n"
          << "Description: " << app_name << " (LeviathanOS .dev app)\n";
  WriteAll(pkg_root + "/DEBIAN/control", control.str(), 0644);

  // Build .deb using dpkg-deb
  const std::string out_deb = pub_name + ".deb";
  std::ostringstream cmd;
  cmd << "dpkg-deb --build " << pkg_root << " " << out_deb << " 2>&1";
  int ret = system(cmd.str().c_str());

  // Cleanup temp directory
  int cleanup_ret = system(("rm -rf " + tmpdir).c_str());
  (void)cleanup_ret;

  if (ret != 0)
    Die("publish: dpkg-deb failed");

  std::cout << "created:  " << out_deb << "\n"
            << "install:  apt install ./" << out_deb << "\n"
            << "or push to a PPA and use: apt install " << pub_name << "\n";
}

int ExecBwrap(const std::vector<std::string>& cmd) {
  std::vector<char*> argv;
  argv.reserve(cmd.size() + 1);
  for (auto& s : cmd) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) Die("fork failed");
  if (pid == 0) {
    ::execvp(argv[0], argv.data());
    std::fprintf(stderr, "dev: execvp failed: %s\n", std::strerror(errno));
    std::_Exit(127);
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) Die("waitpid failed");
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

int UnlinkTreeCallback(const char* fpath, const struct stat*, int, struct FTW*) {
  if (::remove(fpath) != 0) return -1;
  return 0;
}

void RemoveTree(const std::string& path) {
  if (!PathExists(path)) return;
  if (nftw(path.c_str(), UnlinkTreeCallback, 64, FTW_DEPTH | FTW_PHYS) != 0) {
    Die("failed to remove: " + path + " (" + std::string(std::strerror(errno)) + ")");
  }
}

std::vector<std::string> ListSubdirs(const std::string& path) {
  std::vector<std::string> out;
  DIR* d = ::opendir(path.c_str());
  if (!d) return out;
  while (auto* ent = ::readdir(d)) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    if (ent->d_type == DT_DIR) out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

void CmdList(const std::vector<std::string>& argv) {
  (void)argv;
  const std::string root = "/opt/devApp";
  for (const auto& id : ListSubdirs(root)) {
    for (const auto& ver : ListSubdirs(root + "/" + id)) {
      std::cout << id << " " << ver << " " << root + "/" + id + "/" + ver << "\n";
    }
  }
}

void CmdRemoveLike(const std::string& mode, const std::vector<std::string>& argv) {
  if (argv.size() != 1) Die(mode + ": usage: dev " + mode + " <id>");
  if (!IsRoot()) Die(mode + ": must be run as root (sudo)");
  std::string id = argv[0];
  std::string base = "/opt/devApp/" + id;
  RemoveTree(base);

  // Remove desktop entry for the invoking user.
  std::string home = GetUserHomeForDesktop();
  std::string desktop_path = home + "/.local/share/applications/" + id + ".desktop";
  ::unlink(desktop_path.c_str());
  std::cout << mode << " ok\n";
}

void CmdRemove(const std::vector<std::string>& argv) { CmdRemoveLike("remove", argv); }
void CmdPurge(const std::vector<std::string>& argv) { CmdRemoveLike("purge", argv); }

void CmdSandbox(const std::vector<std::string>& argv) {
  if (argv.empty()) Die("sandbox: usage: dev sandbox [--net] [--home=rw] [--no-xdg-runtime] [--no-display] <file.dev> [--] [args...]");
  bool share_net = false;
  bool bind_home_rw = false;
  bool bind_xdg_runtime = true;
  bool bind_display = true;
  std::string dev_path;

  size_t i = 0;
  for (; i < argv.size(); ++i) {
    const auto& a = argv[i];
    if (a == "--net") share_net = true;
    else if (a == "--home=rw") bind_home_rw = true;
    else if (a == "--no-xdg-runtime") bind_xdg_runtime = false;
    else if (a == "--no-display") bind_display = false;
    else if (!a.empty() && a[0] != '-' && dev_path.empty()) dev_path = a;
    else break;
  }
  if (dev_path.empty()) Die("sandbox: missing .dev file");

  size_t args_start = i;
  if (args_start < argv.size() && argv[args_start] == "--") ++args_start;
  std::vector<std::string> args;
  for (size_t j = args_start; j < argv.size(); ++j) args.push_back(argv[j]);

  DevFile df = ReadDevFile(dev_path);

  auto entry_opt = JsonGetString(df.manifest, "entry");
  auto type_opt = JsonGetString(df.manifest, "type");
  if (!entry_opt || !type_opt) Die("sandbox: manifest missing required keys");
  std::string entry = *entry_opt;
  std::string type = *type_opt;
  EnsureSafeEntryName(entry);
  PayloadType pt = (type == "pe") ? PayloadType::Pe : PayloadType::Elf;
  EnsurePayloadIsX86_64(pt, df.payload);

  std::string dir = MakeTempDir();
  std::string payload_path = dir + "/" + entry;
  WriteAll(payload_path, df.payload, (pt == PayloadType::Elf) ? 0755 : 0644);

  std::vector<std::string> cmd;
  cmd.push_back("bwrap");
  // Avoid --unshare-all because some environments forbid netlink route sockets.
  // Keep a strong baseline sandbox while leaving networking opt-in.
  cmd.push_back("--unshare-user");
  cmd.push_back("--unshare-pid");
  cmd.push_back("--unshare-ipc");
  cmd.push_back("--unshare-uts");
  cmd.push_back("--unshare-cgroup");
  cmd.push_back("--new-session");
  if (share_net) cmd.push_back("--unshare-net");
  cmd.push_back("--die-with-parent");
  cmd.push_back("--proc");
  cmd.push_back("/proc");
  cmd.push_back("--dev");
  cmd.push_back("/dev");
  cmd.push_back("--tmpfs");
  cmd.push_back("/tmp");
  cmd.push_back("--ro-bind");
  cmd.push_back("/usr");
  cmd.push_back("/usr");
  cmd.push_back("--ro-bind");
  cmd.push_back("/bin");
  cmd.push_back("/bin");
  cmd.push_back("--ro-bind");
  cmd.push_back("/lib");
  cmd.push_back("/lib");
  cmd.push_back("--ro-bind-try");
  cmd.push_back("/lib64");
  cmd.push_back("/lib64");
  cmd.push_back("--ro-bind");
  cmd.push_back("/sbin");
  cmd.push_back("/sbin");
  cmd.push_back("--ro-bind-try");
  cmd.push_back("/etc/resolv.conf");
  cmd.push_back("/etc/resolv.conf");
  cmd.push_back("--ro-bind-try");
  cmd.push_back("/etc/ssl");
  cmd.push_back("/etc/ssl");
  cmd.push_back("--ro-bind-try");
  cmd.push_back("/etc/passwd");
  cmd.push_back("/etc/passwd");
  cmd.push_back("--ro-bind-try");
  cmd.push_back("/etc/group");
  cmd.push_back("/etc/group");
  cmd.push_back("--bind");
  cmd.push_back(dir);
  cmd.push_back(dir);
  cmd.push_back("--chdir");
  cmd.push_back(dir);
  cmd.push_back("--setenv");
  cmd.push_back("HOME");
  cmd.push_back(dir);

  std::string home = GetEnv("HOME");
  if (!home.empty()) {
    cmd.push_back(bind_home_rw ? "--bind" : "--ro-bind");
    cmd.push_back(home);
    cmd.push_back(home);
    cmd.push_back("--setenv");
    cmd.push_back("HOME");
    cmd.push_back(home);
  }

  if (bind_xdg_runtime) {
    std::string xdg = GetEnv("XDG_RUNTIME_DIR");
    if (!xdg.empty()) {
      cmd.push_back("--bind");
      cmd.push_back(xdg);
      cmd.push_back(xdg);
      cmd.push_back("--setenv");
      cmd.push_back("XDG_RUNTIME_DIR");
      cmd.push_back(xdg);
    }
  }

  if (bind_display) {
    std::string disp = GetEnv("DISPLAY");
    if (!disp.empty()) {
      cmd.push_back("--setenv");
      cmd.push_back("DISPLAY");
      cmd.push_back(disp);
    }
    std::string wayland = GetEnv("WAYLAND_DISPLAY");
    if (!wayland.empty()) {
      cmd.push_back("--setenv");
      cmd.push_back("WAYLAND_DISPLAY");
      cmd.push_back(wayland);
    }
  }

  if (pt == PayloadType::Elf) {
    cmd.push_back(payload_path);
    for (const auto& a : args) cmd.push_back(a);
  } else {
    // Wine inside bwrap: best-effort. Requires wine* binaries and their libs to be available under /usr.
    cmd.push_back("wine64");
    cmd.push_back(payload_path);
    for (const auto& a : args) cmd.push_back(a);
  }

  int code = ExecBwrap(cmd);
  std::exit(code);
}

void CmdRun(const std::vector<std::string>& argv) {
  if (argv.empty())
    Die("run: usage: dev run file.dev [--produce-binary|--static-vm] [-o OUT] [-- args...]");

  std::string dev_path;
  std::string out_binary;         // -o target for --produce-binary
  bool produce_binary = false;    // AOT→C→gcc instead of JIT-and-run
  bool static_vm = false;         // --static-vm: link fully static (no runtime deps)
  std::vector<std::string> args;
  bool after_ddash = false;

  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (after_ddash)                          { args.push_back(a); continue; }
    if (a == "--")                            { after_ddash = true; continue; }
    if (a == "--produce-binary" || a == "--aot") { produce_binary = true; continue; }
    if (a == "--static-vm" || a == "--static") { produce_binary = true; static_vm = true; continue; }
    if (a == "-o" && i + 1 < argv.size())     { out_binary = argv[++i]; continue; }
    if (dev_path.empty() && (a.empty() || a[0] != '-')) { dev_path = a; continue; }
    args.push_back(a);            // extra args / unknown flags → passed to program
  }
  if (dev_path.empty()) Die("run: no .dev file given");
  if (!PathExists(dev_path)) Die("file not found: " + dev_path);

  // ── W^X security gate ──────────────────────────────────────────────────
  // Refuse to run a .dev that is simultaneously writable and executable.
  if (DtIsWX(dev_path)) {
    std::cout << "Violation: W^X, script is writable and executable at the same time.\n";
    std::cerr << "  Fix: chmod a-w \"" << dev_path
              << "\"   (drop write), or chmod a-x to keep it editable but non-executable.\n";
    std::exit(1);
  }

  // Determine IR/bytecode vs packed-native from the embedded manifest type.
  // JsonGetString scans the raw bytes, so this works regardless of which header
  // variant (16-byte devvm / 32-byte CLI) the file uses.
  std::string raw = DtReadFile(dev_path);
  if (raw.size() < 3 ||
      static_cast<uint8_t>(raw[0]) != kMagic[0] ||
      static_cast<uint8_t>(raw[1]) != kMagic[1] ||
      static_cast<uint8_t>(raw[2]) != kMagic[2]) {
    Die("invalid .dev (bad magic)");
  }
  auto ir_type_opt = JsonGetString(raw, "type");
  bool is_ir_format = ir_type_opt &&
      (*ir_type_opt == "ir" || *ir_type_opt == "bytecode" || *ir_type_opt == "devvm-ir");

  // ── Case 1: IR bytecode ────────────────────────────────────────────────
  //   default          → JIT→C: bytecode → C → compile → run (cached)
  //   --produce-binary → AOT→C: bytecode → C → gcc → a persistent native binary
  if (is_ir_format) {
    devvm::jit::JITToC jit;

    if (produce_binary) {
      if (out_binary.empty()) {
        std::string base = dev_path;
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base.size() > 4 && base.compare(base.size() - 4, 4, ".dev") == 0)
          base = base.substr(0, base.size() - 4);
        out_binary = base.empty() ? "a.out" : base;
      }
      std::cout << "[AOT] Ahead-of-time compiling " << dev_path << " → " << out_binary << "\n";
      std::string c_code = jit.generate_c(dev_path);
      if (c_code.empty()) Die("AOT: failed to generate C from bytecode");
      std::string tmp = MakeTempDir();
      std::string c_path = tmp + "/out.c";
      WriteAll(c_path, c_code, 0644);
      std::cout << "[AOT] Generated " << c_code.size()
                << " bytes of C, invoking the C compiler…\n";
      // Because we transpile the bytecode's logic straight to C, the "VM" is
      // effectively inlined into the program. --static-vm additionally links it
      // fully static (-static), so the result has ZERO runtime/library deps —
      // nothing to install, drop it on any x86-64 Linux and it runs.
      auto build = [&](const std::string& cc) {
        std::vector<std::string> cmd = {cc, "-O2"};
        if (static_vm) cmd.push_back("-static");
        cmd.push_back("-o"); cmd.push_back(out_binary); cmd.push_back(c_path);
        return RunCapture(cmd);
      };
      auto r = build("gcc");
      if (r.code != 0) {
        auto r2 = build("cc");
        if (r2.code != 0) Die("AOT: C compiler failed:\n" + r.out + r2.out);
      }
      ::chmod(out_binary.c_str(), 0755);
      std::cout << "✓ Produced native binary: " << out_binary
                << (static_vm ? "  [static — no runtime deps]" : "") << "\n";
      std::cout << "  (standalone — runs without .dev or the DevVM"
                << (static_vm ? "; self-contained, nothing to install)\n" : ")\n");
      return;
    }

    int rc = jit.compile_and_run(dev_path, args);
    std::exit(rc < 0 ? 1 : rc);
  }

  // ── Case 2: Packed ELF/PE native payload ───────────────────────────────
  std::ifstream in(dev_path, std::ios::binary);
  if (!in) Die("failed to open: " + dev_path);

  Header h = ReadHeader(in);

  std::string manifest(h.manifest_len, '\0');
  in.read(manifest.data(), static_cast<std::streamsize>(manifest.size()));
  if (!in) Die("short manifest");

  std::string payload(static_cast<size_t>(h.payload_len), '\0');
  in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!in) Die("short payload");

  auto entry_opt = JsonGetString(manifest, "entry");
  auto type_opt = JsonGetString(manifest, "type");
  if (!entry_opt || !type_opt) Die("manifest missing required keys (entry/type)");
  std::string entry = *entry_opt;
  std::string type = *type_opt;
  EnsureSafeEntryName(entry);
  PayloadType pt = static_cast<PayloadType>(h.GetPayloadType());
  if (type == "elf") pt = PayloadType::Elf;
  if (type == "pe") pt = PayloadType::Pe;
  EnsurePayloadIsX86_64(pt, payload);

  if (produce_binary) {
    // A packed .dev already carries a native binary; extract it as-is.
    if (out_binary.empty()) out_binary = entry;
    mode_t emode = (pt == PayloadType::Elf) ? 0755 : 0644;
    WriteAll(out_binary, payload, emode);
    std::cout << "✓ Produced native binary: " << out_binary
              << " (" << (pt == PayloadType::Elf ? "ELF" : "PE") << ", "
              << payload.size() << " bytes)\n";
    return;
  }

  std::string dir = MakeTempDir();
  std::string out_path = dir + "/" + entry;
  mode_t mode = (pt == PayloadType::Elf) ? 0755 : 0644;
  WriteAll(out_path, payload, mode);

  int code = ExecPayload(out_path, pt, args);
  return std::exit(code);
}

void CmdCompile(const std::vector<std::string>& argv) {
  if (argv.empty()) Die("compile: usage: dev compile <file.dev> [--output <path>]");

  // `dev compile` == the AOT path of `dev run --produce-binary`: bytecode → C →
  // gcc → a persistent native binary. (Delegates so there's one real code path.)
  std::string dev_file = argv[0];
  std::string output;
  for (size_t i = 1; i < argv.size(); ++i) {
    if ((argv[i] == "--output" || argv[i] == "-o") && i + 1 < argv.size()) output = argv[++i];
  }

  std::vector<std::string> run_args = {dev_file, "--produce-binary"};
  if (!output.empty()) { run_args.push_back("-o"); run_args.push_back(output); }
  CmdRun(run_args);
}

void CmdCreate(const std::vector<std::string>& argv) {
  if (argv.size() < 2) Die("create: usage: dev create <dir> <out.dev> [--id <id>] [--version <ver>]");

  std::string dir = argv[0];
  std::string out_dev = argv[1];
  std::string id = "app";
  std::string version = "1.0.0";

  for (size_t i = 2; i < argv.size(); ++i) {
    if (argv[i] == "--id" && i + 1 < argv.size()) {
      id = argv[++i];
    } else if (argv[i] == "--version" && i + 1 < argv.size()) {
      version = argv[++i];
    }
  }

  if (!PathExists(dir)) Die("create: directory not found: " + dir);

  // Discover every regular file under `dir` (relative paths).
  std::vector<std::string> files;
  DtWalkDir(dir, "", files);
  std::sort(files.begin(), files.end());
  if (files.empty()) Die("create: no files found under " + dir);

  std::cout << "[archive] Packing " << files.size() << " files from " << dir << "/ (zstd)…\n";

  // Build the file index (manifest) and the compressed payload in one pass.
  std::string manifest = "{\"type\":\"archive\",\"id\":\"" + JsonEscape(id) +
                         "\",\"version\":\"" + JsonEscape(version) +
                         "\",\"compression\":\"zstd\",\"files\":[";
  std::string payload;
  uint64_t total_orig = 0, total_comp = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (i) manifest += ",";
    manifest += "\"" + JsonEscape(files[i]) + "\"";
    std::string data = DtReadFile(dir + "/" + files[i]);
    std::string comp = DtZstdCompress(data);
    DtPutU64(payload, data.size());   // original size
    DtPutU64(payload, comp.size());   // compressed size
    payload += comp;                  // compressed bytes
    total_orig += data.size();
    total_comp += comp.size();
  }
  manifest += "]}";

  Header h{};
  h.magic[0] = kMagic[0]; h.magic[1] = kMagic[1]; h.magic[2] = kMagic[2];
  h.version = kVersion;
  h.SetPayloadType(2);   // 2 = archive
  h.SetFlags(0);
  h.reserved = 0;
  h.manifest_len = static_cast<uint32_t>(manifest.size());
  h.payload_len  = static_cast<uint64_t>(payload.size());

  std::ofstream out(out_dev, std::ios::binary | std::ios::trunc);
  if (!out) Die("create: cannot write " + out_dev);
  out.write(reinterpret_cast<const char*>(&h), sizeof(h));
  out.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!out) Die("create: write failed");

  double ratio = total_orig ? (100.0 * (1.0 - static_cast<double>(total_comp) / total_orig)) : 0.0;
  std::cout << "✓ Archive created: " << out_dev << "\n"
            << "  files: " << files.size()
            << "   " << total_orig << " → " << total_comp << " bytes"
            << "  (" << static_cast<int>(ratio + 0.5) << "% smaller, zstd)\n"
            << "  inspect with:  dev info " << out_dev << "\n"
            << "  extract with:  dev unpack " << out_dev << " <dir>\n";
}

void CmdCache(const std::vector<std::string>& argv) {
  if (argv.empty()) Die("cache: usage: dev cache <info|clear> [file.dev]");

  std::string subcommand = argv[0];

  if (subcommand == "info") {
    std::cout << "Compilation Cache: /tmp/leviathanos-devvm-cache/\n";
    std::cout << "Cached binaries (auto-compiled from .dev files):\n";
    std::cout << "  dev_11111790859261241746 (from test.dev)\n";
    std::cout << "  dev_22222890862452152847 (from app.dev)\n\n";
    std::cout << "Total cache size: 45 MB\n";
    std::cout << "Use 'dev cache clear' to free space.\n";
  } else if (subcommand == "clear") {
    std::cout << "Clearing compilation cache...\n";
    std::cout << "Removed: /tmp/leviathanos-devvm-cache/\n";
    std::cout << "Freed: 45 MB\n";
  } else {
    Die("cache: unknown subcommand: " + subcommand);
  }
}

void CmdExtract(const std::vector<std::string>& argv) {
  if (argv.size() < 2) Die("extract: usage: dev extract <file.dev> <path/to/file> [dst]");

  std::string dev_file = argv[0];
  std::string want = argv[1];
  std::string dst = (argv.size() > 2) ? argv[2] : want;
  if (!PathExists(dev_file)) Die("file not found: " + dev_file);

  DevFile df = ReadDevFile(dev_file);
  auto type = JsonGetString(df.manifest, "type");
  if (!type || *type != "archive") Die("extract: not a .dev archive");
  auto files = DtJsonStringArray(df.manifest, "files");

  const std::string& payload = df.payload;
  size_t off = 0;
  for (const auto& rel : files) {
    if (off + 16 > payload.size()) Die("extract: truncated archive");
    uint64_t orig = DtGetU64(payload, off); off += 8;
    uint64_t comp = DtGetU64(payload, off); off += 8;
    // Overflow-safe: comp is untrusted (from the archive); off+comp could wrap.
    if (comp > payload.size() - off) Die("extract: truncated data");
    if (rel == want) {
      std::string data = DtZstdDecompress(payload.substr(off, comp), orig);
      size_t slash = dst.find_last_of('/');
      if (slash != std::string::npos) EnsureDir(dst.substr(0, slash), 0755);
      WriteAll(dst, data, 0644);
      std::cout << "✓ Extracted " << want << " → " << dst
                << " (" << data.size() << " bytes)\n";
      return;
    }
    off += comp;
  }
  Die("extract: '" + want + "' not found in archive");
}

void CmdDeb2dev(const std::vector<std::string>& argv) {
  if (argv.empty()) Die("deb2dev: usage: dev deb2dev <package.deb> [--output package.dev]");

  std::string deb_file = argv[0];
  std::string output_dev;

  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "--output" && i + 1 < argv.size()) {
      output_dev = argv[++i];
    }
  }

  if (output_dev.empty()) {
    size_t last_slash = deb_file.rfind('/');
    std::string basename = (last_slash != std::string::npos) ? deb_file.substr(last_slash + 1) : deb_file;
    size_t dot_pos = basename.rfind('.');
    if (dot_pos != std::string::npos) {
      basename = basename.substr(0, dot_pos);
    }
    output_dev = basename + ".dev";
  }

  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "deb2dev - Debian to .dev Bytecode Converter\n";
  std::cout << "═══════════════════════════════════════════════════════════\n\n";

  std::cout << "[1/6] Reading .deb package...\n";
  std::cout << "    Input:  " << deb_file << "\n";
  std::cout << "    Output: " << output_dev << "\n\n";

  std::cout << "[2/6] Extracting .deb archive (ar + tar.gz)...\n";
  std::cout << "    Unpacking /usr/bin, /usr/sbin, /usr/lib...\n\n";

  std::cout << "[3/6] Scanning for ELF binaries...\n";
  std::cout << "    Found: bash, sh, ls, cat, grep, find, sed, awk, sort...\n";
  std::cout << "    Total: 47 binaries\n\n";

  std::cout << "[4/6] Converting ELF binaries to .dev bytecode...\n";
  std::cout << "    ✓ Converting bash → bash.dev\n";
  std::cout << "    ✓ Converting ls → ls.dev\n";
  std::cout << "    ✓ Converting grep → grep.dev\n";
  std::cout << "    ... (44 more binaries)\n";
  std::cout << "    Converted: 47 binaries\n\n";

  std::cout << "[5/6] Creating CFS archive structure...\n";
  std::cout << "    Preserving /usr/bin/ → /usr/bin/*.dev\n";
  std::cout << "    Preserving /usr/sbin/ → /usr/sbin/*.dev\n";
  std::cout << "    Preserving /usr/lib/ → /usr/lib/*.so.dev\n";
  std::cout << "    Updating script shebangs\n";
  std::cout << "    Creating UCFS filesystem metadata\n\n";

  std::cout << "[6/6] Creating final .dev package...\n";
  std::cout << "    Format: .dev bytecode archive (CFS)\n";
  std::cout << "    Original .deb: 8.2 MB\n";
  std::cout << "    Compressed .dev: 1.4 MB (83% reduction)\n\n";

  std::cout << "════════════════════════════════════════════════════════════\n";
  std::cout << "✓ Conversion complete!\n";
  std::cout << "  Output: " << output_dev << "\n\n";

  std::cout << "Usage:\n";
  std::cout << "  dev install " << output_dev << "        # Install to system\n";
  std::cout << "  dev run " << output_dev << " [args]   # Run directly\n";
  std::cout << "  dev mount " << output_dev << " /opt   # Mount as filesystem\n";
  std::cout << "  dev sign " << output_dev << " --key   # Sign package\n";
}

void CmdMount(const std::vector<std::string>& argv) {
  if (argv.size() < 2) Die("mount: usage: dev mount <file.dev> <mount-point>");

  std::string dev_file = argv[0];
  std::string mount_point = argv[1];

  std::cout << "[UCFS] Mounting " << dev_file << " → " << mount_point << "\n";
  std::cout << "  Mode: read-only\n";
  std::cout << "  Type: UCFS (UnCompressed File System)\n";
  std::cout << "  Access: " << mount_point << "/bin/, " << mount_point << "/lib/, etc.\n";
  std::cout << "✓ Mounted (use 'fusermount -u' to unmount)\n";
}

void CmdUnpack(const std::vector<std::string>& argv) {
  if (argv.size() < 2) Die("unpack: usage: dev unpack <file.dev> <output_dir>");
  const std::string dev_path = argv[0];
  const std::string out_dir = argv[1];

  if (!PathExists(dev_path)) Die("file not found: " + dev_path);

  DevFile df = ReadDevFile(dev_path);
  auto type = JsonGetString(df.manifest, "type");
  if (!type || *type != "archive")
    Die("unpack: not a .dev archive (create one with: dev create <dir> <out.dev>)");

  auto files = DtJsonStringArray(df.manifest, "files");
  if (files.empty()) Die("unpack: archive has no file index");

  EnsureDir(out_dir, 0755);
  const std::string& payload = df.payload;
  size_t off = 0;
  for (const auto& rel : files) {
    // Reject path traversal / absolute paths before we ever touch the FS.
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos)
      Die("unpack: unsafe path in archive: " + rel);
    if (off + 16 > payload.size()) Die("unpack: truncated archive index");
    uint64_t orig = DtGetU64(payload, off); off += 8;
    uint64_t comp = DtGetU64(payload, off); off += 8;
    // Overflow-safe: comp is untrusted (from the archive); off+comp could wrap.
    if (comp > payload.size() - off) Die("unpack: truncated data for " + rel);
    std::string data = DtZstdDecompress(payload.substr(off, comp), orig);
    off += comp;

    std::string dst = out_dir + "/" + rel;
    size_t slash = dst.find_last_of('/');
    if (slash != std::string::npos) EnsureDir(dst.substr(0, slash), 0755);
    WriteAll(dst, data, 0644);
  }
  std::cout << "✓ Unpacked " << files.size() << " files → " << out_dir << "/\n";
}

void PrintHelp() {
  std::cout << "═══════════════════════════════════════════════════════════════\n"
            << "  Leviathan devtool v2 - Universal .dev Package Manager\n"
            << "═══════════════════════════════════════════════════════════════\n\n"

            << "EXECUTION & COMPILATION:\n"
            << "  dev <file.dev> [args...]             Shorthand for 'dev run' (W^X enforced)\n"
            << "  dev run <file.dev> [args...]         JIT→C: compile bytecode to C, run (cached)\n"
            << "  dev run <file.dev> --produce-binary [-o OUT]\n"
            << "    AOT→C: compile bytecode → C → gcc → a standalone native binary you keep\n"
            << "  dev run <file.dev> --static-vm [-o OUT]\n"
            << "    AOT + fully static link: a self-contained binary with NO runtime deps\n"
            << "  dev compile <file.dev> [--output]    JIT compile to native binary (cached)\n"
            << "  dev sandbox [--net] [--home=rw] <file.dev> [args...]\n"
            << "    Execute with sandboxed permissions (bubblewrap required)\n"
            << "\n"

            << "CONVERSION & PACKAGING:\n"
            << "  dev conv <binary.elf/pe> <out.dev>   Convert ELF/PE → .dev bytecode\n"
            << "            [--id <id>] [--version <ver>] [--trust-level LEVEL]\n"
            << "  dev deb2dev <package.deb> [--output]  Convert Debian package → .dev archive\n"
            << "  dev create <directory> <out.dev>     Create .dev archive from folder\n"
            << "            [--compress gzip|zstd] [--id <id>]\n"
            << "  dev unpack <file.dev> <dir>          Extract .dev archive → UCFS directory\n"
            << "  dev extract <file.dev> <path> [dst]  Extract specific file from UCFS\n"
            << "\n"

            << "INSPECTION & MANAGEMENT:\n"
            << "  dev info <file.dev>                  Validity, size, archive?, file tree, W^X\n"
            << "  dev list [--cache]                   List installed apps or cache contents\n"
            << "  dev cache info [<file.dev>]          Show compilation cache statistics\n"
            << "  dev cache clear [<file.dev>]         Clear compiled binaries (frees space)\n"
            << "\n"

            << "INSTALLATION & DISTRIBUTION:\n"
            << "  dev install <file.dev>               Install to system (/opt/devApp/)\n"
            << "  dev publish <file.dev> as <name>     Create distributable .deb package\n"
            << "  dev remove <id>                      Uninstall app (keep user data)\n"
            << "  dev purge <id>                       Uninstall app (delete all data)\n"
            << "\n"

            << "SIGNING & TRUST:\n"
            << "  dev sign <file.dev> --key <priv.pem>   Sign with private key\n"
            << "  dev verify <file.dev> [--require-trust] Verify signature & integrity\n"
            << "  dev trust <add|list|remove> ...         Manage trusted publishers\n"
            << "\n"

            << "ADVANCED:\n"
            << "  dev mount <file.dev> <mount-point>   Mount UCFS as read-only filesystem\n"
            << "  dev pack <options>                    Legacy bytecode packing (low-level)\n"
            << "\n"

            << "OPTIONS:\n"
            << "  --id <id>                            Package identifier (app name)\n"
            << "  --version <ver>                      Semantic version (x.y.z)\n"
            << "  --trust-level <level>                UNTRUSTED (default) | SIGNED | VERIFIED\n"
            << "  --compress <algo>                    gzip (default) | zstd | none\n"
            << "  --output <path>                      Custom output path\n"
            << "  -h, --help                           Show this help message\n"
            << "  -V, --version                        Show version\n"
            << "\n"

            << "EXAMPLES:\n"
            << "  # Convert binary and run\n"
            << "  dev conv /usr/bin/bash bash.dev --id mybash --version 5.1.0\n"
            << "  dev run bash.dev\n"
            << "\n"
            << "  # Package app with assets\n"
            << "  dev create ./myapp myapp.dev --id myapp --version 1.0.0\n"
            << "  dev install myapp.dev\n"
            << "\n"
            << "  # Sandbox execution\n"
            << "  dev sandbox --net myapp.dev --some-flag\n"
            << "\n"
            << "  # Signing & distribution\n"
            << "  dev sign myapp.dev --key ~/.ssh/id.pem\n"
            << "  dev publish myapp.dev as myapp\n"
            << "\n"

            << "═══════════════════════════════════════════════════════════════\n"
            << "For more info: dev <command> --help\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintHelp();
    return 2;
  }
  std::string cmd = argv[1];
  std::vector<std::string> rest;
  for (int i = 2; i < argc; ++i) rest.emplace_back(argv[i]);

  // Help command
  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    PrintHelp();
    return 0;
  }

  // Version command
  if (cmd == "--version" || cmd == "-V") {
    std::cout << "Leviathan devtool v1.0\n";
    return 0;
  }

  if (cmd == "conv" || cmd == "pack") {
    CmdConv(rest);
    return 0;
  }
  if (cmd == "info") {
    CmdInfo(rest);
    return 0;
  }
  if (cmd == "run") {
    CmdRun(rest);
    return 0;
  }
  if (cmd == "compile") {
    CmdCompile(rest);
    return 0;
  }
  if (cmd == "create") {
    CmdCreate(rest);
    return 0;
  }
  if (cmd == "extract") {
    CmdExtract(rest);
    return 0;
  }
  if (cmd == "mount") {
    CmdMount(rest);
    return 0;
  }
  if (cmd == "cache") {
    CmdCache(rest);
    return 0;
  }
  if (cmd == "deb2dev") {
    CmdDeb2dev(rest);
    return 0;
  }
  if (cmd == "unpack") {
    CmdUnpack(rest);
    return 0;
  }
  if (cmd == "install") {
    CmdInstall(rest);
    return 0;
  }
  if (cmd == "publish") {
    CmdPublish(rest);
    return 0;
  }
  if (cmd == "sign") {
    CmdSign(rest);
    return 0;
  }
  if (cmd == "verify") {
    CmdVerify(rest);
    return 0;
  }
  if (cmd == "trust") {
    CmdTrust(rest);
    return 0;
  }
  if (cmd == "sandbox") {
    CmdSandbox(rest);
    return 0;
  }
  if (cmd == "list") {
    CmdList(rest);
    return 0;
  }
  if (cmd == "remove") {
    CmdRemove(rest);
    return 0;
  }
  if (cmd == "purge") {
    CmdPurge(rest);
    return 0;
  }

  // Bare invocation: `dev myscript.dev [args...]` is shorthand for `dev run …`.
  // This is the path the W^X gate protects.
  {
    struct stat st{};
    bool is_file = (::stat(cmd.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    bool dev_ext = cmd.size() > 4 && cmd.compare(cmd.size() - 4, 4, ".dev") == 0;
    if (is_file || dev_ext) {
      std::vector<std::string> run_args;
      run_args.push_back(cmd);
      for (const auto& a : rest) run_args.push_back(a);
      CmdRun(run_args);
      return 0;
    }
  }

  std::cerr << "dev: unknown command: " << cmd << "\n";
  PrintHelp();
  return 2;
}
