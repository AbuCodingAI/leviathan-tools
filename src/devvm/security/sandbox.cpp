#include "sandbox.h"
#include "../core/devvm.h"

#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

namespace devvm {
namespace security {

// ============================================================================
// SyscallPolicy: Define allowed syscalls per trust level
// ============================================================================

SyscallPolicy::SyscallPolicy(TrustLevel trust_level) : trust_level_(trust_level) {
  init_whitelist();
}

void SyscallPolicy::init_whitelist() {
  switch (trust_level_) {
    case TrustLevel::UNTRUSTED:
      // Minimal syscalls for untrusted code
      whitelist_.insert(static_cast<int32_t>(SyscallID::EXIT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::EXIT_GROUP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::WRITE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::READ));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MPROTECT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MUNMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::BRK));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETTIMEOFDAY));
      whitelist_.insert(static_cast<int32_t>(SyscallID::CLOCK_GETTIME));
      break;

    case TrustLevel::SIGNED:
      // Signed code gets more syscalls
      whitelist_.insert(static_cast<int32_t>(SyscallID::EXIT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::EXIT_GROUP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::READ));
      whitelist_.insert(static_cast<int32_t>(SyscallID::WRITE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::OPEN));
      whitelist_.insert(static_cast<int32_t>(SyscallID::CLOSE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::STAT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::FSTAT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::LSEEK));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MPROTECT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MUNMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::BRK));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MSYNC));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MADVISE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETPID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETUID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETEUID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETGID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETEGID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETTIMEOFDAY));
      whitelist_.insert(static_cast<int32_t>(SyscallID::TIME));
      whitelist_.insert(static_cast<int32_t>(SyscallID::CLOCK_GETTIME));
      whitelist_.insert(static_cast<int32_t>(SyscallID::PRCTL));
      break;

    case TrustLevel::VERIFIED:
      // Verified code (OS-verified) can use almost all syscalls
      // Blocked: fork, execve, clone, vulnerable syscalls
      whitelist_.insert(static_cast<int32_t>(SyscallID::READ));
      whitelist_.insert(static_cast<int32_t>(SyscallID::WRITE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::OPEN));
      whitelist_.insert(static_cast<int32_t>(SyscallID::CLOSE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::STAT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::FSTAT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::LSTAT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::POLL));
      whitelist_.insert(static_cast<int32_t>(SyscallID::LSEEK));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MPROTECT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MUNMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::BRK));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MREMAP));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MSYNC));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MINCORE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::MADVISE));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETPID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETUID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETEUID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETGID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETEGID));
      whitelist_.insert(static_cast<int32_t>(SyscallID::GETTIMEOFDAY));
      whitelist_.insert(static_cast<int32_t>(SyscallID::TIME));
      whitelist_.insert(static_cast<int32_t>(SyscallID::CLOCK_GETTIME));
      whitelist_.insert(static_cast<int32_t>(SyscallID::PRCTL));
      whitelist_.insert(static_cast<int32_t>(SyscallID::EXIT));
      whitelist_.insert(static_cast<int32_t>(SyscallID::EXIT_GROUP));
      // Still block: fork, execve, clone (process creation/execution)
      break;
  }
}

bool SyscallPolicy::is_allowed(SyscallID syscall_id) const {
  return whitelist_.count(static_cast<int32_t>(syscall_id)) > 0;
}

// ============================================================================
// Sandbox: Execute syscalls with sandboxing
// ============================================================================

Sandbox::Sandbox(TrustLevel trust_level) : policy_(trust_level) {}

int64_t Sandbox::execute_syscall(SyscallID syscall_id,
                                  int64_t arg0, int64_t arg1, int64_t arg2,
                                  int64_t arg3, int64_t arg4, int64_t arg5) {
  syscall_count_++;

  // Check if syscall is allowed
  if (!policy_.is_allowed(syscall_id)) {
    blocked_count_++;
    throw SandboxViolation(syscall_id);
  }

  // Handle specific syscalls
  switch (syscall_id) {
    case SyscallID::EXIT:
      return handle_exit(static_cast<int32_t>(arg0));

    case SyscallID::EXIT_GROUP:
      return handle_exit_group(static_cast<int32_t>(arg0));

    case SyscallID::WRITE:
      return handle_write(static_cast<int32_t>(arg0),
                          reinterpret_cast<const void*>(arg1),
                          static_cast<size_t>(arg2));

    case SyscallID::READ:
      return handle_read(static_cast<int32_t>(arg0),
                         reinterpret_cast<void*>(arg1),
                         static_cast<size_t>(arg2));

    case SyscallID::MMAP:
      return reinterpret_cast<int64_t>(handle_mmap(
          reinterpret_cast<void*>(arg0), static_cast<size_t>(arg1),
          static_cast<int32_t>(arg2), static_cast<int32_t>(arg3),
          static_cast<int32_t>(arg4), arg5));

    case SyscallID::MPROTECT:
      return handle_mprotect(reinterpret_cast<void*>(arg0),
                             static_cast<size_t>(arg1),
                             static_cast<int32_t>(arg2));

    case SyscallID::MUNMAP:
      return handle_munmap(reinterpret_cast<void*>(arg0),
                           static_cast<size_t>(arg1));

    case SyscallID::BRK:
      return reinterpret_cast<int64_t>(handle_brk(reinterpret_cast<void*>(arg0)));

    case SyscallID::GETPID:
      return handle_getpid();

    case SyscallID::GETUID:
      return handle_getuid();

    case SyscallID::GETTIMEOFDAY:
      return handle_gettimeofday(reinterpret_cast<void*>(arg0),
                                 reinterpret_cast<void*>(arg1));

    case SyscallID::CLOCK_GETTIME:
      return handle_clock_gettime(static_cast<int32_t>(arg0),
                                  reinterpret_cast<void*>(arg1));

    default:
      // Return -1 for unimplemented but allowed syscalls
      return -1;
  }
}

int64_t Sandbox::handle_exit(int32_t code) {
  // Signal exit (doesn't actually exit, just returns code)
  return code;
}

int64_t Sandbox::handle_exit_group(int32_t code) {
  return code;
}

int64_t Sandbox::handle_write(int32_t fd, const void* buf, size_t count) {
  // Only allow writes to stdout/stderr
  if (fd != 1 && fd != 2) {
    return -1;  // EBADF
  }
  if (allowed_fds_.find(fd) == allowed_fds_.end()) {
    return -1;
  }
  // Safe cast and write
  if (::write(fd, buf, count) < 0) {
    return -1;
  }
  return static_cast<int64_t>(count);
}

int64_t Sandbox::handle_read(int32_t fd, void* buf, size_t count) {
  // Only allow reads from stdin
  if (fd != 0) {
    return -1;  // EBADF
  }
  if (allowed_fds_.find(fd) == allowed_fds_.end()) {
    return -1;
  }
  ssize_t result = ::read(fd, buf, count);
  return (result < 0) ? -1 : result;
}

int64_t Sandbox::handle_mmap(void* addr, size_t len, int32_t prot, int32_t flags,
                              int32_t fd, int64_t offset) {
  // Only allow memory mapping with specific protections
  // No executable allocations for UNTRUSTED
  if (policy_.trust_level() == TrustLevel::UNTRUSTED) {
    if (prot & 0x4) {  // PROT_EXEC
      return -1;       // Deny executable mappings
    }
  }

  // Only allow anonymous or read-only file mappings
  void* result = ::mmap(addr, len, prot, flags, fd, offset);
  return (result == MAP_FAILED) ? -1 : reinterpret_cast<int64_t>(result);
}

int64_t Sandbox::handle_mprotect(void* addr, size_t len, int32_t prot) {
  // Restrict PROT_EXEC for untrusted
  if (policy_.trust_level() == TrustLevel::UNTRUSTED) {
    if (prot & 0x4) {  // PROT_EXEC
      return -1;
    }
  }
  return (::mprotect(addr, len, prot) == 0) ? 0 : -1;
}

int64_t Sandbox::handle_munmap(void* addr, size_t len) {
  return (::munmap(addr, len) == 0) ? 0 : -1;
}

int64_t Sandbox::handle_brk(void* addr) {
  // brk is used for heap expansion; allow it
  int result = ::brk(addr);
  return (result == -1) ? -1 : 0;
}

int64_t Sandbox::handle_getpid() {
  return ::getpid();
}

int64_t Sandbox::handle_getuid() {
  return ::getuid();
}

int64_t Sandbox::handle_gettimeofday(void* tv, void* tz) {
  // Safe wrapper around gettimeofday
  if (::gettimeofday(reinterpret_cast<struct timeval*>(tv),
                     reinterpret_cast<struct timezone*>(tz)) == 0) {
    return 0;
  }
  return -1;
}

int64_t Sandbox::handle_clock_gettime(int32_t clock_id, void* timespec) {
  // Safe wrapper around clock_gettime
  if (::clock_gettime(clock_id, reinterpret_cast<struct timespec*>(timespec)) == 0) {
    return 0;
  }
  return -1;
}

}  // namespace security
}  // namespace devvm
