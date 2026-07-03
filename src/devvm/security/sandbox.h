#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>

namespace devvm {

// Forward declaration
enum class TrustLevel : uint8_t;

namespace security {

// Linux syscall numbers (x86-64)
enum class SyscallID : int32_t {
  // I/O
  READ = 0,
  WRITE = 1,
  OPEN = 2,
  CLOSE = 3,
  STAT = 4,
  FSTAT = 5,
  LSTAT = 6,
  POLL = 7,
  LSEEK = 8,
  MMAP = 9,
  MPROTECT = 10,
  MUNMAP = 11,
  BRK = 12,

  // Process control
  SIGACTION = 13,
  SIGHALT = 14,
  SIGPAUSE = 15,
  SIGALTSTACK = 131,
  EXIT = 60,
  EXIT_GROUP = 231,
  PRCTL = 157,

  // Memory
  MREMAP = 25,
  MSYNC = 26,
  MINCORE = 27,
  MADVISE = 28,

  // Process
  FORK = 57,
  VFORK = 58,
  EXECVE = 59,
  CLONE = 56,
  WAIT4 = 114,
  GETPID = 39,
  GETPPID = 110,
  GETUID = 102,
  GETEUID = 107,
  GETGID = 104,
  GETEGID = 108,

  // Time
  GETTIMEOFDAY = 96,
  TIME = 201,
  CLOCK_GETTIME = 228,

  // Unknown/private
  UNKNOWN = -1,
};

// Syscall policy for each trust level
class SyscallPolicy {
 public:
  SyscallPolicy(TrustLevel trust_level);

  // Check if a syscall is allowed
  bool is_allowed(SyscallID syscall_id) const;

  // Get the trust level for this policy
  TrustLevel trust_level() const { return trust_level_; }

 private:
  TrustLevel trust_level_;
  std::set<int32_t> whitelist_;

  // Initialize whitelist based on trust level
  void init_whitelist();
};

// Syscall sandbox: filters and monitors syscalls during execution
class Sandbox {
 public:
  explicit Sandbox(TrustLevel trust_level);

  // Execute a syscall with sandboxing
  // Returns the syscall return value (simulated or filtered)
  int64_t execute_syscall(SyscallID syscall_id,
                          int64_t arg0, int64_t arg1, int64_t arg2,
                          int64_t arg3, int64_t arg4, int64_t arg5);

  // Get syscall statistics
  uint64_t syscall_count() const { return syscall_count_; }
  uint64_t blocked_count() const { return blocked_count_; }

 private:
  SyscallPolicy policy_;
  uint64_t syscall_count_ = 0;
  uint64_t blocked_count_ = 0;

  // Syscall handlers for specific syscalls
  int64_t handle_exit(int32_t code);
  int64_t handle_exit_group(int32_t code);
  int64_t handle_write(int32_t fd, const void* buf, size_t count);
  int64_t handle_read(int32_t fd, void* buf, size_t count);
  int64_t handle_mmap(void* addr, size_t len, int32_t prot, int32_t flags,
                      int32_t fd, int64_t offset);
  int64_t handle_mprotect(void* addr, size_t len, int32_t prot);
  int64_t handle_munmap(void* addr, size_t len);
  int64_t handle_brk(void* addr);
  int64_t handle_getpid();
  int64_t handle_getuid();
  int64_t handle_gettimeofday(void* tv, void* tz);
  int64_t handle_clock_gettime(int32_t clock_id, void* timespec);

  // Track allowed file descriptors
  std::set<int32_t> allowed_fds_ = {0, 1, 2};  // stdin, stdout, stderr
};

// Exception type for sandbox violations
class SandboxViolation : public std::runtime_error {
 public:
  explicit SandboxViolation(const std::string& msg) : std::runtime_error(msg) {}
  explicit SandboxViolation(SyscallID syscall_id)
      : std::runtime_error("Syscall " + std::to_string(static_cast<int32_t>(syscall_id)) +
                           " blocked by sandbox") {}
};

}  // namespace security
}  // namespace devvm
