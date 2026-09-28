/*
 * dsh-termux-sandbox — a lightweight, rootless, Android-native file-effect
 * sandbox runner for DeepSeek Harness on Termux.
 *
 * Why this exists: the Linux backends harness ships (bubblewrap, Landlock) are
 * both unavailable to Termux app processes. bubblewrap needs unprivileged user
 * namespaces (Android disables them) and Landlock needs a 5.13+ kernel with
 * CONFIG_SECURITY_LANDLOCK (typical Android kernels are older). Without a
 * backend every confined mode fails closed, so the Termux port could only run
 * `danger-full-access`.
 *
 * What it does: installs a seccomp filter in the wrapped process and serves the
 * write-intent syscalls through SECCOMP_RET_USER_NOTIF. Read-only syscalls never
 * notify the supervisor, so normal reading, globbing and pipelines keep native
 * speed; only paths that are about to be written are resolved and judged.
 *
 * The runner speaks the bwrap-compatible profile dialect that
 * `@deepseek-ai/dsh-sandbox-local` appends for `runnerCommand`:
 *
 *   dsh-termux-sandbox --ro-bind / / --dev /dev --unshare-pid --proc /proc \
 *     --die-with-parent [--tmpfs /tmp] [--bind ROOT ROOT] -- <command> [args...]
 *
 * Policy: device nodes (binder, null, tty, ...) and the process's own /proc
 * entries stay writable;
 * `--bind DIR` and `--tmpfs DIR` add writable roots (that is exactly the
 * workspace-write contract); everything else is read-only and denials surface
 * as EACCES, matching the Landlock denial dialect harness already classifies.
 *
 * Extra writable paths can be added without changing the profile through
 * DSH_TERMUX_SANDBOX_ALLOW, a colon-separated list of absolute paths. That is
 * the seam for workflow-specific scratch directories.
 *
 * Threat model: this is a cooperative file-effect boundary, not an adversarial
 * jail. It stops the model from writing outside the allowed roots and blocks
 * the obvious escape hatches (chmod/chown/ptrace/mount/...), but path checks
 * happen in userspace and are not race-free against a hostile process that
 * mutates symlinks concurrently. Values are checked by the kernel per syscall,
 * so a plain "denied" always means denied.
 *
 * Build (on device, no NDK):
 *   clang -O2 -Wall -Wextra -o dsh-termux-sandbox dsh-termux-sandbox.c
 */
#define _GNU_SOURCE

#if !defined(__aarch64__)
#error "dsh-termux-sandbox currently targets Termux on arm64 (aarch64)"
#endif

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

#define FATAL_PREFIX "dsh-termux-sandbox: fatal:"
#define MAX_ROOTS 64
#define MAX_PATH 4096

static const char *g_writable[MAX_ROOTS];
static int g_writable_count;
static int g_debug;
/** SIGCHLD is blocked and consumed through signalfd so the supervisor wakes the
 * instant the command exits instead of polling. */
static sigset_t g_child_mask;

static void fatal(const char *what) {
  fprintf(stderr, "%s %s: %s\n", FATAL_PREFIX, what, strerror(errno));
  exit(125);
}

static void debugf(const char *fmt, ...) {
  if (!g_debug) return;
  va_list ap;
  va_start(ap, fmt);
  fputs("dsh-termux-sandbox: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

/* ------------------------------------------------------------------ policy */

/**
 * Canonicalize one directory that the profile declares writable. `create`
 * makes a missing directory (the `--tmpfs /tmp` promise on a host that has no
 * /tmp); otherwise the path is kept lexically when it does not exist yet.
 */
static char *canonical_dir(const char *path, int create) {
  char buf[PATH_MAX];
  if (realpath(path, buf) != NULL) return strdup(buf);
  if (create) {
    if (mkdir(path, 0700) == 0 && realpath(path, buf) != NULL) return strdup(buf);
  }
  if (path[0] != '/') return NULL;
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;
  char *copy = strndup(path, len);
  return copy;
}

/** True when `path` equals `root` or lives underneath it (`root` is canonical). */
static int path_under(const char *path, const char *root) {
  size_t n = strlen(root);
  if (n == 0) return 0;
  if (strcmp(path, root) == 0) return 1;
  if (strncmp(path, root, n) != 0) return 0;
  if (root[n - 1] == '/') return 1;
  return path[n] == '/';
}

static void add_writable(const char *path, int create) {
  if (g_writable_count >= MAX_ROOTS) {
    fprintf(stderr, "%s too many writable roots\n", FATAL_PREFIX);
    exit(125);
  }
  char *canon = canonical_dir(path, create);
  if (canon == NULL) {
    debugf("ignoring writable root %s (%s)", path, strerror(errno));
    return;
  }
  g_writable[g_writable_count++] = canon;
  debugf("writable root: %s", canon);
}

/** Decide one resolved absolute path. 0 allows, -EACCES denies. */
static int decide_path(const char *path) {
  /* Device nodes (null, tty, binder, ashmem, ...) are part of the platform
   * contract: terminal and IPC access need writable opens, and SELinux already
   * denies a Termux app every device that could reach block storage. */
  if (path_under(path, "/dev")) return 0;
  if (path_under(path, "/proc/self")) return 0;
  if (path_under(path, "/proc/thread-self")) return 0;
  /* Anonymous and socket-backed descriptors are memory or IPC, never durable
   * storage: memfd:, anon_inode:, socket: and pipe: have no filesystem home. */
  if (strncmp(path, "/memfd:", 7) == 0 || strncmp(path, "anon_inode:", 11) == 0
      || strncmp(path, "socket:", 7) == 0 || strncmp(path, "pipe:", 5) == 0) {
    return 0;
  }
  for (int i = 0; i < g_writable_count; i++) {
    if (path_under(path, g_writable[i])) return 0;
  }
  return -EACCES;
}

/* ------------------------------------------------------------- supervisor -- */

/** Read a NUL-terminated string out of another process. */
static int read_cstr(pid_t pid, uint64_t addr, char *out, size_t cap) {
  if (addr == 0 || cap == 0) return -1;

  struct iovec local = {.iov_base = out, .iov_len = cap - 1};
  struct iovec remote = {.iov_base = (void *)(uintptr_t)addr, .iov_len = cap - 1};
  ssize_t got = syscall(__NR_process_vm_readv, pid, &local, 1, &remote, 1, 0);
  if (got > 0) {
    out[got] = '\0';
    char *nul = memchr(out, '\0', (size_t)got);
    if (nul != NULL) *nul = '\0';
    return 0;
  }

  char proc[64];
  snprintf(proc, sizeof(proc), "/proc/%d/mem", (int)pid);
  int fd = open(proc, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  size_t i = 0;
  while (i + 1 < cap) {
    char chunk[256];
    ssize_t n = pread(fd, chunk, sizeof(chunk), (off_t)(addr + i));
    if (n <= 0) { close(fd); return -1; }
    for (ssize_t b = 0; b < n && i + 1 < cap; b++) {
      out[i++] = chunk[b];
      if (chunk[b] == '\0') { close(fd); return 0; }
    }
  }
  out[cap - 1] = '\0';
  close(fd);
  return 0;
}

/** Read one raw byte range out of another process. */
static int read_mem(pid_t pid, uint64_t addr, void *out, size_t len) {
  struct iovec local = {.iov_base = out, .iov_len = len};
  struct iovec remote = {.iov_base = (void *)(uintptr_t)addr, .iov_len = len};
  return syscall(__NR_process_vm_readv, pid, &local, 1, &remote, 1, 0) == (ssize_t)len ? 0 : -1;
}

static int read_fd_link(pid_t pid, int fd, char *out, size_t cap) {
  char link[64];
  snprintf(link, sizeof(link), "/proc/%d/fd/%d", (int)pid, fd);
  ssize_t n = readlink(link, out, cap - 1);
  if (n < 0) return -1;
  out[n] = '\0';
  return 0;
}

/**
 * Resolve what the tracee is about to touch into a canonical absolute path.
 * Relative paths resolve against the tracee's dirfd (or cwd), then the parent
 * directory is canonicalized so `..` and symlinked ancestors are honoured.
 */
static int resolve_path(pid_t pid, int dirfd, const char *raw, char *out, size_t cap) {
  char base[MAX_PATH];
  if (raw[0] == '/') {
    snprintf(base, sizeof(base), "%s", raw);
  } else {
    char dir[MAX_PATH];
    if (dirfd == AT_FDCWD) {
      snprintf(dir, sizeof(dir), "/proc/%d/cwd", (int)pid);
      ssize_t n = readlink(dir, base, sizeof(base) - 1);
      if (n < 0) return -1;
      base[n] = '\0';
    } else {
      if (read_fd_link(pid, dirfd, base, sizeof(base)) < 0) return -1;
    }
    size_t have = strlen(base);
    if (have + strlen(raw) + 2 >= sizeof(base)) return -1;
    if (have == 0 || base[have - 1] != '/') base[have++] = '/';
    snprintf(base + have, sizeof(base) - have, "%s", raw);
  }

  /* A path that already exists is resolved whole, so a symlink leaf cannot
   * smuggle a write out of the allowed roots. */
  char whole[PATH_MAX];
  if (realpath(base, whole) != NULL) {
    int n = snprintf(out, cap, "%s", whole);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
  }

  /* Otherwise canonicalize the parent so the leaf may be nonexistent
   * (O_CREAT); a nonexistent leaf is not a symlink. */
  char *slash = strrchr(base, '/');
  if (slash == NULL) return -1;
  char leaf[MAX_PATH];
  snprintf(leaf, sizeof(leaf), "%s", slash + 1);
  if (slash == base) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }

  char parent[PATH_MAX];
  /* /proc/<pid>/root prefixes the tracee's root; for the app's own children
   * that is the same root we see, so plain realpath is correct and cheaper. */
  if (realpath(base, parent) == NULL) return -1;
  int n = snprintf(out, cap, "%s%s%s", parent, parent[1] == '\0' ? "" : "/", leaf);
  return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/**
 * Serve notifications until the traced tree is gone.
 *
 * The wrapped command may spawn children that inherit the filter, so the
 * supervisor cannot simply exit when the direct child does. It keeps serving
 * and leaves after the command exits plus a quiet grace window with no further
 * notifications (DSH_TERMUX_SANDBOX_GRACE_MS, default 5000). A lingering
 * background child that writes after the window gets ENOSYS — a failure, never
 * an unchecked write, because the listener is gone.
 */
/**
 * Judge one filesystem target: a path pointer, or a descriptor when the
 * syscall names its target that way (utimensat(fd, NULL, ...), ftruncate,
 * fchmod, fchown, fallocate). Returns 1 to allow, 0 to deny. An unreadable or
 * unresolvable target is denied, never silently allowed.
 */
static int judge_target(pid_t pid, int dirfd, uint64_t pathp, const char *what) {
  char raw[MAX_PATH];
  char resolved[MAX_PATH];
  int have_target = 0;
  if (pathp != 0 && read_cstr(pid, pathp, raw, sizeof(raw)) == 0 && raw[0] != '\0') {
    have_target = resolve_path(pid, dirfd, raw, resolved, sizeof(resolved)) == 0;
  } else if (dirfd >= 0) {
    have_target = read_fd_link(pid, dirfd, resolved, sizeof(resolved)) == 0;
    if (have_target) {
      char *deleted = strstr(resolved, " (deleted)");
      if (deleted != NULL) *deleted = '\0';
    }
  }
  if (!have_target) {
    debugf("%s: target unreadable, denying", what);
    return 0;
  }
  int allow = decide_path(resolved) == 0;
  debugf("%s %s -> %s", what, allow ? "allow" : "deny", resolved);
  if (!allow) {
    fprintf(stderr, "dsh-termux-sandbox: denied write to %s\n", resolved);
  }
  return allow;
}

/**
 * Any live process still in the wrapped command's process group?
 *
 * The command gets its own process group so this scan answers "does the sandbox
 * still have work to supervise" — non-interactive shells keep their background
 * jobs in the group, so `cmd &` stays served. A process that calls setsid
 * escapes the scan; its later writes then fail with ENOSYS, never unchecked.
 */
static int pgid_alive(pid_t pgid) {
  DIR *dir = opendir("/proc");
  if (dir == NULL) return 1; /* fail closed: keep supervising */
  int alive = 0;
  struct dirent *entry;
  while (!alive && (entry = readdir(dir)) != NULL) {
    if (!isdigit((unsigned char)entry->d_name[0])) continue;
    pid_t pid = (pid_t)atoi(entry->d_name);
    if (pid == getpid() || pid <= 0) continue;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) continue;
    char buf[512];
    char *line = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (line == NULL) continue;
    /* Skip "pid (comm)" — comm may contain spaces and parentheses. */
    char *close = strrchr(line, ')');
    if (close == NULL || close[1] != ' ') continue;
    /* Field 3 is the single-character state, then ppid, then pgrp. */
    char state = '\0';
    int ppid = 0, pgrp = 0;
    if (sscanf(close + 2, "%c %d %d", &state, &ppid, &pgrp) != 3) continue;
    if (pgrp == (int)pgid) alive = 1;
  }
  closedir(dir);
  return alive;
}

static int supervise(int listener, pid_t child, int pgid_ok, int *status_out) {
  int child_done = 0;
  int grace_ms = 200;
  const char *configured = getenv("DSH_TERMUX_SANDBOX_GRACE_MS");
  if (configured != NULL && *configured != '\0') {
    int parsed = atoi(configured);
    if (parsed >= 0) grace_ms = parsed;
  }

  struct pollfd fds[2];
  fds[0].fd = listener;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  int signalfd_fd = signalfd(-1, &g_child_mask, SFD_NONBLOCK | SFD_CLOEXEC);
  fds[1].fd = signalfd_fd;
  fds[1].events = POLLIN;
  fds[1].revents = 0;
  int nfds = signalfd_fd >= 0 ? 2 : 1;

  for (;;) {
    /* Liveness first: reaping the child and checking its process group decides
     * whether any task still needs supervising, independent of notifications. */
    if (!child_done) {
      /* Drain the SIGCHLD signalfd so poll does not stay readable; the actual
       * reaping happens right here on the next check. */
      if (signalfd_fd >= 0) {
        struct signalfd_siginfo info;
        while (read(signalfd_fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) {}
      }
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child) {
        child_done = 1;
        *status_out = status;
        debugf("command exited, status=%d", status);
      }
    }
    if (child_done && (!pgid_ok || !pgid_alive(child))) {
      debugf("no tracees left (pgid_ok=%d); stopping supervisor", pgid_ok);
      /* Drain a notification that raced the last exit so no task is left
       * blocked on an answer that will never come. */
      struct pollfd drain = {.fd = listener, .events = POLLIN};
      if (poll(&drain, 1, 0) <= 0 || (drain.revents & POLLIN) == 0) return 0;
    }

    int ready = poll(fds, (nfds_t)nfds, child_done ? grace_ms : -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      debugf("poll failed: %s", strerror(errno));
      return 0;
    }
    if (ready == 0) continue;
    /* A dead notifying task turns the listener readable for POLLHUP while
     * NOTIF_RECV would block forever; only a real POLLIN has a request. */
    if ((fds[0].revents & POLLIN) == 0) continue;

    struct seccomp_notif req;
    memset(&req, 0, sizeof(req));
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req) < 0) {
      /* ENOENT: the notifying task died between poll and recv; keep serving. */
      if (errno == EINTR || errno == ENOENT) continue;
      debugf("NOTIF_RECV failed: %s", strerror(errno));
      return 0;
    }

    const int nr = (int)req.data.nr;
    const unsigned long long *a = req.data.args;
#ifdef __NR_openat2
    /* openat2 hides its flags behind a struct pointer, and the filter cannot
     * read it, so every openat2 lands here. A read-only open has no write
     * effect and is allowed without consulting the write policy; an
     * unreadable open_how is denied rather than assumed harmless. */
    if (nr == __NR_openat2) {
      struct { uint64_t flags; uint64_t mode; uint64_t resolve; } how;
      int readable = read_mem((pid_t)req.pid, a[2], &how, sizeof(how)) == 0;
      const uint64_t write_bits = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
      if (readable && (how.flags & write_bits) == 0) {
        struct seccomp_notif_resp resp = {0};
        resp.id = req.id;
        resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) < 0 && errno != ENOENT) {
          debugf("NOTIF_SEND failed: %s", strerror(errno));
        }
        continue;
      }
      if (!readable) {
        struct seccomp_notif_resp resp = {0};
        resp.id = req.id;
        resp.error = -EACCES;
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) < 0 && errno != ENOENT) {
          debugf("NOTIF_SEND failed: %s", strerror(errno));
        }
        continue;
      }
    }
#endif
    char what[32];
    snprintf(what, sizeof(what), "syscall %d", nr);

    /* Each syscall names its write target differently. rename moves between
     * two directories, so both ends must be writable; checking only the source
     * would let a rename carry a file out of the allowed roots. */
    int dirfd = AT_FDCWD;
    uint64_t pathp = 0;
    int dirfd2 = AT_FDCWD;
    uint64_t pathp2 = 0;
    switch (nr) {
      case __NR_symlinkat:
        dirfd = (int)a[1];
        pathp = a[2];
        break;
      case __NR_linkat:
        dirfd = (int)a[2];
        pathp = a[3];
        break;
      case __NR_renameat:
#ifdef __NR_renameat2
      case __NR_renameat2:
#endif
        dirfd = (int)a[0];
        pathp = a[1];
        dirfd2 = (int)a[2];
        pathp2 = a[3];
        break;
      case __NR_truncate:
        dirfd = AT_FDCWD;
        pathp = a[0];
        break;
      case __NR_ftruncate:
      case __NR_fchmod:
      case __NR_fchown:
      case __NR_fallocate:
        dirfd = (int)a[0];
        pathp = 0;
        break;
      default:
        dirfd = (int)a[0];
        pathp = a[1];
        break;
    }

    int allow = judge_target((pid_t)req.pid, dirfd, pathp, what);
    if (allow && pathp2 != 0) {
      snprintf(what, sizeof(what), "syscall %d (destination)", nr);
      allow = judge_target((pid_t)req.pid, dirfd2, pathp2, what);
    }

    struct seccomp_notif_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.id = req.id;
    if (allow) {
      resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
      resp.val = 0;
      resp.error = 0;
    } else {
      resp.error = -EACCES;
    }
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) < 0 && errno != ENOENT) {
      debugf("NOTIF_SEND failed: %s", strerror(errno));
    }

  }
}

/* ------------------------------------------------------------------- filter */

static uint32_t open_write_mask(void) {
  uint32_t mask = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
#ifdef __O_TMPFILE
  mask |= __O_TMPFILE;
#endif
  return mask;
}

static size_t build_filter(struct sock_filter *prog, size_t cap) {
  size_t n = 0;
#define EMIT(f) do { if (n >= cap) { fprintf(stderr, "%s filter overflow\n", FATAL_PREFIX); exit(125); } prog[n++] = (f); } while (0)
#define STMT(code_, k_) ((struct sock_filter){.code = (code_), .jt = 0, .jf = 0, .k = (k_)})
#define JMP(code_, k_, jt_, jf_) ((struct sock_filter){.code = (code_), .jt = (jt_), .jf = (jf_), .k = (k_)})

  EMIT(STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, arch)));
  EMIT(JMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0));
  EMIT(STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  EMIT(STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, nr)));

  /* openat is the hot path: notify only when the flags ask for a write. */
  {
    const size_t body = 4;
    EMIT(JMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, (uint8_t)body));
    EMIT(STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, args[2])));
    EMIT(STMT(BPF_ALU | BPF_AND | BPF_K, open_write_mask()));
    EMIT(JMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    EMIT(STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF));
  }

  /* Path-taking syscalls the supervisor judges. */
  static const int notify_nr[] = {
    __NR_unlinkat, __NR_renameat, __NR_mkdirat, __NR_symlinkat, __NR_linkat,
    __NR_mknodat, __NR_fchmodat, __NR_fchownat, __NR_utimensat,
    /* Descriptor-targeted writes resolve through /proc/<pid>/fd so the same
     * policy judges them; a plain denial here would break touch, cp -p, git
     * and friends inside a writable workspace. */
    __NR_ftruncate, __NR_fchmod, __NR_fchown, __NR_fallocate,
#ifdef __NR_renameat2
    __NR_renameat2,
#endif
#ifdef __NR_openat2
    __NR_openat2, /* flags live in a struct; always judged by the supervisor */
#endif
#ifdef __NR_fchmodat2
    __NR_fchmodat2,
#endif
  };
  for (size_t i = 0; i < sizeof(notify_nr) / sizeof(notify_nr[0]); i++) {
    EMIT(JMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)notify_nr[i], 0, 1));
    EMIT(STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF));
  }
  EMIT(JMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_truncate, 0, 1));
  EMIT(STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF));

  /* Descriptor-based writes and sandbox escape hatches: deny outright. */
  static const int deny_nr[] = {
    __NR_ptrace, __NR_process_vm_writev, __NR_mount, __NR_umount2,
    __NR_pivot_root, __NR_setns, __NR_unshare, __NR_bpf, __NR_keyctl,
    __NR_add_key, __NR_request_key, __NR_init_module, __NR_finit_module,
    __NR_delete_module, __NR_reboot, __NR_kexec_load, __NR_swapon,
    __NR_swapoff, __NR_sethostname, __NR_setdomainname,
    __NR_name_to_handle_at, __NR_open_by_handle_at, __NR_io_uring_setup,
#ifdef __NR_fsopen
    __NR_fsopen, __NR_fsmount, __NR_fspick, __NR_open_tree,
    __NR_move_mount, __NR_mount_setattr,
#endif
#ifdef __NR_quotactl
    __NR_quotactl,
#endif
  };
  for (size_t i = 0; i < sizeof(deny_nr) / sizeof(deny_nr[0]); i++) {
    EMIT(JMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)deny_nr[i], 0, 1));
    EMIT(STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA)));
  }

  EMIT(STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
#undef EMIT
#undef STMT
#undef JMP
  return n;
}

static int install_filter(void) {
  static struct sock_filter prog[512];
  size_t len = build_filter(prog, sizeof(prog) / sizeof(prog[0]));
  struct sock_fprog fprog = {.len = (unsigned short)len, .filter = prog};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) return -1;
  return (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &fprog);
}

static int send_fd(int sock, int fd) {
  char buf[1] = {'x'};
  struct iovec io = {.iov_base = buf, .iov_len = sizeof(buf)};
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {0};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
  return (int)sendmsg(sock, &msg, 0);
}

static int recv_fd(int sock) {
  char buf[1];
  struct iovec io = {.iov_base = buf, .iov_len = sizeof(buf)};
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {0};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  if (recvmsg(sock, &msg, 0) < 0) return -1;
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == NULL) return -1;
  int fd = -1;
  memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
  return fd;
}

/* ---------------------------------------------------------------- launcher */

static void usage(void) {
  fprintf(stderr, "%s bad invocation\n", FATAL_PREFIX);
  fputs("usage: dsh-termux-sandbox [bwrap profile args] -- <command> [args...]\n", stderr);
}

int main(int argc, char **argv) {
  g_debug = getenv("DSH_TERMUX_SANDBOX_DEBUG") != NULL;

  int i = 1;
  int die_with_parent = 0;
  for (; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--") == 0) { i++; break; }
    if (strcmp(arg, "--ro-bind") == 0) { i += 2; continue; }
    if (strcmp(arg, "--bind") == 0 || strcmp(arg, "--dev-bind") == 0) {
      if (i + 2 >= argc) { usage(); return 2; }
      /* --bind SRC DST makes DST writable; --ro-bind is intentionally ignored. */
      add_writable(argv[i + 2], 0);
      i += 2;
      continue;
    }
    if (strcmp(arg, "--tmpfs") == 0) {
      if (i + 1 >= argc) { usage(); return 2; }
      /* --tmpfs is the profile's promise that this directory is a writable
       * scratch area; materialize it when the host has none. */
      add_writable(argv[i + 1], 1);
      /* On Android the filesystem root is read-only for apps, so bwrap's
       * /tmp cannot be created. Termux tools honour TMPDIR, which is the
       * host path that actually backs "a writable temp area" here. */
      if (strcmp(argv[i + 1], "/tmp") == 0) {
        const char *host_tmp = getenv("TMPDIR");
        const char *termux_prefix = getenv("PREFIX");
        if (host_tmp == NULL || host_tmp[0] != '/') host_tmp = termux_prefix;
        if (host_tmp != NULL && host_tmp[0] == '/') {
          char tmp_path[PATH_MAX];
          if (termux_prefix != NULL && strcmp(host_tmp, termux_prefix) == 0) {
            snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", host_tmp);
            add_writable(tmp_path, 1);
          } else {
            add_writable(host_tmp, 1);
          }
        }
      }
      i += 1;
      continue;
    }
    if (strcmp(arg, "--dev") == 0 || strcmp(arg, "--proc") == 0) { i += 1; continue; }
    if (strcmp(arg, "--die-with-parent") == 0) { die_with_parent = 1; continue; }
    if (strcmp(arg, "--unshare-pid") == 0 || strcmp(arg, "--unshare-user") == 0
        || strcmp(arg, "--share-net") == 0 || strcmp(arg, "--new-session") == 0) {
      continue;
    }
    /* Unknown flags are ignored so a newer profile dialect still runs; the
     * policy above stays fail-closed because only explicit --bind/--tmpfs
     * roots become writable. */
    debugf("ignoring unsupported profile argument: %s", arg);
  }
  if (i >= argc) { usage(); return 2; }

  const char *extra = getenv("DSH_TERMUX_SANDBOX_ALLOW");
  if (extra != NULL && *extra != '\0') {
    char *copy = strdup(extra);
    if (copy == NULL) fatal("strdup");
    for (char *tok = strtok(copy, ":"); tok != NULL; tok = strtok(NULL, ":")) {
      if (*tok != '/') continue;
      add_writable(tok, 0);
    }
  }

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) fatal("socketpair");

  /* Block SIGCHLD up front: the supervisor consumes it through signalfd, and
   * the wrapped command gets the mask restored before it execs. */
  sigemptyset(&g_child_mask);
  sigaddset(&g_child_mask, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &g_child_mask, NULL) < 0) fatal("sigprocmask");

  pid_t child = fork();
  if (child < 0) fatal("fork");
  if (child == 0) {
    close(sv[0]);
    /* Own process group: the supervisor uses it to know when the command tree
     * — including background jobs a non-interactive shell leaves behind — is
     * really gone. */
    setpgid(0, 0);
    if (die_with_parent) prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    int listener = install_filter();
    if (listener < 0) {
      fprintf(stderr, "%s cannot install seccomp filter: %s\n", FATAL_PREFIX, strerror(errno));
      _exit(125);
    }
    if (send_fd(sv[1], listener) < 0) {
      fprintf(stderr, "%s cannot hand listener to supervisor: %s\n", FATAL_PREFIX, strerror(errno));
      _exit(125);
    }
    close(sv[1]);
    close(listener);
    /* The wrapped command must not inherit our blocked SIGCHLD. */
    sigprocmask(SIG_UNBLOCK, &g_child_mask, NULL);
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "%s cannot exec %s: %s\n", FATAL_PREFIX, argv[i], strerror(errno));
    _exit(127);
  }

  close(sv[1]);
  /* Racing the child's own setpgid is expected; EACCES means it already
   * exec'd, which only happens after it moved itself into the new group. */
  int pgid_ok = setpgid(child, child) == 0 || errno == EACCES;
  int listener = recv_fd(sv[0]);
  if (listener < 0) {
    fprintf(stderr, "%s supervisor did not receive the listener\n", FATAL_PREFIX);
    return 125;
  }
  debugf("supervising pid %d", (int)child);
  int status = -1;
  supervise(listener, child, pgid_ok, &status);
  if (status == -1) waitpid(child, &status, 0);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 125;
}
