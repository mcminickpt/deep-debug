// Standalone host-side test harness. Not an mcmini model-checking target.
//
// Mirrors dmtcp-callback.c's ACTUAL fast-path resumption mechanism (each
// thread calls getcontext() directly, as a plain function call -- no signal
// handler, unlike the multithreaded-fork-tsan-2.0 standalone package) to
// exercise R2 (TSan fork hooks around _Fork()), R3 (__clone instead of the
// public clone()), and R4 (fresh TSan fiber for the forking thread AND each
// recreated thread) under real ThreadSanitizer.
//
// BUGGY MODE (for RED evidence): compile with -DMTF_BUGGY to use the public
// clone() (no R3) and skip the forking-thread fiber switch (no R4 remainder),
// reproducing the failure these two fixes exist to prevent.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef __x86_64__
#include <asm/prctl.h>
#include <sys/prctl.h>
#else
#error "This standalone harness only covers __x86_64__; see dmtcp-callback.c for other architectures."
#endif

#ifndef NUM_WORKERS
#define NUM_WORKERS 3
#endif
#define ITERS 100000

// ---- TSan externs, copied verbatim from src/lib/dmtcp-callback.c ----
extern void *__tsan_create_fiber(unsigned flags) __attribute__((weak));
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags)
    __attribute__((weak));
extern void __sanitizer_syscall_pre_impl_fork(void) __attribute__((weak));
extern void __sanitizer_syscall_post_impl_fork(long res) __attribute__((weak));

// libc's internal clone (NOT intercepted by libtsan, unlike public clone()).
extern int __clone(int (*fn)(void *), void *child_stack, int flags, void *arg,
                    ... /* pid_t *ptid, void *newtls, pid_t *ctid */);

// ---- struct threadinfo + TLS/descriptor helpers, copied verbatim from
// src/lib/dmtcp-callback.c (x86_64 branch only) for fidelity to production,
// including the "syscall(SYS_arch_prctl, 2, ARCH_SET_FS, ...)" call already
// proven correct by the multithreaded-fork-tsan-2.0 standalone package. ----
struct threadinfo {
  ucontext_t context;
  unsigned long fs;
  unsigned long gs;
  pthread_t pthread_descriptor;
};
static struct threadinfo threadInfos[NUM_WORKERS];
static atomic_int threadIdx = 0;

static void getTLSPointer(struct threadinfo *ti) {
  assert(syscall(SYS_arch_prctl, ARCH_GET_FS, &ti->fs) == 0);
  assert(syscall(SYS_arch_prctl, ARCH_GET_GS, &ti->gs) == 0);
}
static void setTLSPointer(struct threadinfo *ti) {
  assert(syscall(SYS_arch_prctl, 2, ARCH_SET_FS, ti->fs) != 0);
  assert(syscall(SYS_arch_prctl, 2, ARCH_SET_GS, ti->gs) != 0);
}
static int pthreadDescriptorTidOffset(void) { return 720; }
static pid_t patchThreadDescriptor(pthread_t pthreadSelf) {
  int offset = pthreadDescriptorTidOffset();
  pid_t oldtid = *(pid_t *)((char *)pthreadSelf + offset);
  *(pid_t *)((char *)pthreadSelf + offset) = syscall(SYS_gettid);
  return oldtid;
}

// ---- Harness state ----
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter = 0;
static sem_t sem_checkin, sem_release, sem_done, sem_park;

static void sem_wait_retry(sem_t *s) {
  while (sem_wait(s) != 0) /* EINTR */;
}

// ---- child_setcontext_fast(), copied verbatim in structure from
// src/lib/dmtcp-callback.c ----
static int child_setcontext_fast(void *arg) {
  struct threadinfo *ti = arg;
  setTLSPointer(ti);
  patchThreadDescriptor(ti->pthread_descriptor);
  setcontext(&ti->context); // never returns
  return 0;
}

static void restart_child_threads_fast(void) {
  int maxThreadIdx = atomic_load(&threadIdx);
  for (int i = 0; i < maxThreadIdx; i++) {
    int clone_flags = (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM
                       | CLONE_SIGHAND | CLONE_THREAD
                       | CLONE_SETTLS | CLONE_PARENT_SETTID
                       | CLONE_CHILD_CLEARTID);
    void *stack = malloc(0x10000) + 0x10000 - 128; // 64 KB, intentionally leaked
    int offset = pthreadDescriptorTidOffset();
    pid_t *ctid = (pid_t *)((char *)threadInfos[i].pthread_descriptor + offset);
    pid_t *ptid = ctid;
#ifdef MTF_BUGGY
    // R3 NOT applied: public clone() is intercepted by libtsan and treated
    // as a fork, corrupting its thread-slot state for this CLONE_THREAD clone.
    clone(child_setcontext_fast, stack, clone_flags,
          (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
#else
    // R3: libc's raw __clone, not intercepted by libtsan.
    __clone(child_setcontext_fast, stack, clone_flags,
            (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
#endif
  }
}

// ---- fast_multithreaded_fork(), copied in structure from
// src/lib/dmtcp-callback.c ----
static pid_t fast_multithreaded_fork(void) {
  pid_t _Fork();
  if (__sanitizer_syscall_pre_impl_fork != NULL) {
    __sanitizer_syscall_pre_impl_fork();
  }
  int childpid = _Fork();
  if (__sanitizer_syscall_post_impl_fork != NULL) {
    __sanitizer_syscall_post_impl_fork(childpid);
  }
  if (childpid == 0) { // child process
#ifndef MTF_BUGGY
    // R4 remainder: the forking thread keeps its inherited (fork-copied) TSan
    // ThreadState, whose shadow call stack starts at the parent's fork-time
    // depth and can overflow as this thread keeps running. Fresh fiber too.
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
#endif
    restart_child_threads_fast();
  }
  return childpid;
}

// ---- worker thread: mirrors thread_handle_after_dmtcp_restart()'s
// getcontext-direct-call mechanism (no signal handler). ----
static void *worker(void *arg) {
  (void)arg;
  pid_t orig_pid = getpid();
  int idx = atomic_fetch_add(&threadIdx, 1);
  struct threadinfo *ti = &threadInfos[idx];
  memset(ti, 0, sizeof(*ti));
  ti->pthread_descriptor = pthread_self();
  getTLSPointer(ti);

  int rc = getcontext(&ti->context);
  assert(rc == 0);

  if (getpid() == orig_pid) {
    // Still in the original process (pre-fork): check in, then block until
    // released (mirrors production's cond_wait parking; a plain semaphore is
    // sufficient here since R1's mode-machinery is out of scope for R2/R3/R4).
    sem_post(&sem_checkin);
    sem_wait_retry(&sem_release);
  } else {
    // Resumed via __clone()+setcontext in the forked child: give this
    // TSan-invisible OS thread a valid ThreadState before any instrumented
    // call below. (Already-proven R4 half; unchanged by MTF_BUGGY.)
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
  }

  // TSan-intercepted work: locked shared write, exercised both by the
  // original threads (parent) and the recreated threads (child).
  for (int i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&mtx);
    shared_counter++;
    pthread_mutex_unlock(&mtx);
  }
  sem_post(&sem_done);
  sem_wait_retry(&sem_park); // park forever; R5 join/exit shims out of scope
  return NULL;
}

int main(void) {
  sem_init(&sem_checkin, 0, 0);
  sem_init(&sem_release, 0, 0);
  sem_init(&sem_done, 0, 0);
  sem_init(&sem_park, 0, 0);

  pthread_t th[NUM_WORKERS];
  for (int i = 0; i < NUM_WORKERS; i++) {
    pthread_create(&th[i], NULL, worker, NULL);
  }
  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_wait(&sem_checkin);
  }

  fprintf(stderr, "[main pid=%d] all workers checked in; forking...\n", getpid());
  pid_t pid = fast_multithreaded_fork();
  const char *who = (pid == 0) ? "CHILD" : "PARENT";
  fprintf(stderr, "[%s pid=%d] returned from fast_multithreaded_fork\n", who, getpid());

  if (pid > 0) {
    // PARENT: release the still-parked original workers.
    for (int i = 0; i < NUM_WORKERS; i++) {
      sem_post(&sem_release);
    }
  }
  // CHILD: recreated threads proceed on their own (see worker()'s else branch).

  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_wait(&sem_done);
  }
  fprintf(stderr, "[%s pid=%d] shared_counter=%ld (expected %d)\n",
          who, getpid(), shared_counter, NUM_WORKERS * ITERS);

  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    fprintf(stderr, "[PARENT] child: exited=%d code=%d signaled=%d\n",
            WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status));
  }
  fprintf(stderr, "[%s pid=%d] done, _exit(0)\n", who, getpid());
  _exit(0);
}
