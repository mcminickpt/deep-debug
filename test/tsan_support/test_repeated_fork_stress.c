// Standalone host-side stress test. Not an mcmini model-checking target.
//
// Extends test_fastpath_fork_clone_fiber.c's architecture to stress-test R2's
// TSan fork-hook bracketing (__sanitizer_syscall_pre/post_impl_fork around
// _Fork()) across MANY repeated fast_multithreaded_fork() calls from the same
// long-lived process -- mirroring mc_template_thread_loop_forever()'s real
// while(1) structure (src/lib/template/loop.c:207-239), which never resets
// threadIdx between calls because threadInfos[] is a snapshot taken once and
// reused for every subsequent branch fork.
//
// Unlike test_fastpath_fork_clone_fiber.c (whose original threads run their
// workload once after being released, modeling a direct-restart-into-branch
// scenario), this harness's original ("template") threads check in once and
// park FOREVER -- matching DMTCP_RESTART_INTO_TEMPLATE's real "threads must
// wait forever" semantics, since the same getcontext() snapshot must remain
// valid and reusable across every one of the NUM_FORKS repeated forks below.
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
#ifndef NUM_FORKS
#define NUM_FORKS 10
#endif
#define ITERS 100000

// ---- TSan externs, copied verbatim from src/lib/dmtcp-callback.c ----
extern void *__tsan_create_fiber(unsigned flags) __attribute__((weak));
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags) __attribute__((weak));
extern void __sanitizer_syscall_pre_impl_fork(void) __attribute__((weak));
extern void __sanitizer_syscall_post_impl_fork(long res) __attribute__((weak));
extern int __clone(int (*fn)(void *), void *child_stack, int flags, void *arg,
                    ... /* pid_t *ptid, void *newtls, pid_t *ctid */);

// ---- struct threadinfo + TLS/descriptor helpers, copied verbatim from
// src/lib/dmtcp-callback.c (x86_64 branch only), matching Phase 1-2's
// accepted-duplication precedent (linking the real file is impractical). ----
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

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter = 0;
static sem_t sem_checkin, sem_park_forever, sem_done;

static void sem_wait_retry(sem_t *s) {
  while (sem_wait(s) != 0) /* EINTR */;
}

static int child_setcontext_fast(void *arg) {
  struct threadinfo *ti = arg;
  setTLSPointer(ti);
  patchThreadDescriptor(ti->pthread_descriptor);
  setcontext(&ti->context);
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
    __clone(child_setcontext_fast, stack, clone_flags,
            (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
  }
}

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
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
    restart_child_threads_fast();
  }
  return childpid;
}

// Mirrors the ORIGINAL (template-process) thread's role: check in once, then
// park FOREVER -- see the file header comment for why this differs from
// test_fastpath_fork_clone_fiber.c's "release and run once" model.
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
    sem_post(&sem_checkin);
    sem_wait_retry(&sem_park_forever); // never released
    return NULL; // not reached
  }

  // Resumed via __clone()+setcontext in a forked branch child.
  if (__tsan_switch_to_fiber != NULL) {
    __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
  }
  for (int i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&mtx);
    shared_counter++;
    pthread_mutex_unlock(&mtx);
  }
  sem_post(&sem_done);
  sem_wait_retry(&sem_park_forever); // park; process _exit()s shortly
  return NULL;
}

int main(void) {
  sem_init(&sem_checkin, 0, 0);
  sem_init(&sem_park_forever, 0, 0);
  sem_init(&sem_done, 0, 0);

  pthread_t th[NUM_WORKERS];
  for (int i = 0; i < NUM_WORKERS; i++) {
    pthread_create(&th[i], NULL, worker, NULL);
  }
  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_wait(&sem_checkin);
  }

  int failures = 0;
  for (int iter = 0; iter < NUM_FORKS; iter++) {
    pid_t pid = fast_multithreaded_fork();
    if (pid == 0) {
      for (int i = 0; i < NUM_WORKERS; i++) {
        sem_wait(&sem_done);
      }
      int ok = (shared_counter == NUM_WORKERS * ITERS);
      fprintf(stderr, "[CHILD iter=%d pid=%d] shared_counter=%ld (expected %d) %s\n",
              iter, getpid(), shared_counter, NUM_WORKERS * ITERS, ok ? "OK" : "MISMATCH");
      _exit(ok ? 0 : 1);
    } else {
      int status;
      waitpid(pid, &status, 0);
      int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      fprintf(stderr, "[PARENT iter=%d] child pid=%d exited=%d code=%d signaled=%d %s\n",
              iter, pid, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status),
              ok ? "OK" : "FAIL");
      if (!ok) failures++;
    }
  }

  fprintf(stderr, "%s: %d/%d iterations passed\n",
          failures == 0 ? "ALL PASSED" : "SOME FAILED", NUM_FORKS - failures, NUM_FORKS);
  return failures == 0 ? 0 : 1;
}
