#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>

#define MaxItems 1
#define BufferSize 2

// Unlike sleep(), a busy-wait can't be cut short by DMTCP's own
// pre-checkpoint signal (which interrupts blocking syscalls, returning
// early with time remaining) -- needed below so main's own delay reliably
// lasts the full duration instead of racing main's return/exit() against
// an in-progress checkpoint write.
static void busy_wait_seconds(int seconds) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < seconds);
}

// Same as producer-consumer-safe.c, except main never calls pthread_join():
// each worker signals `done` once its real work finishes, then parks forever
// on an unposted semaphore, and main returns as soon as both `done`s are
// posted -- exercising a real, alive-but-parked (never joined/exited) thread
// still present when the process ends, matching multithreaded-fork-tsan-2.0's
// test_park.c ("thread resurrection ... without pthread_join/pthread_exit").
//
// NOTE: main() returning here (rather than e.g. pthread_exit()) triggers
// POSIX exit() semantics -- the whole process tears down immediately,
// including TSan's own exit-time Finalize()/ThreadCount() bookkeeping, which
// reuses main()'s now-dead stack slots (confirmed live via a gdb hardware
// watchpoint: the exact address that held prod_ids[0] gets overwritten by
// TSan's internal "alive thread count" a moment later). This can show up as
// an apparently-garbled producer/consumer id in RECORD-mode's printf output
// on a rare timing -- harmless (pure RECORD-mode artifact, before any
// checkpoint/restart), not a libmcmini/DMTCP bug, and not the subject of
// this file's actual test (multithreaded_fork's recreated-but-unjoined
// thread handling under --multithreaded-fork restart).
sem_t empty;
sem_t full;
sem_t producer_done;
sem_t consumer_done;
sem_t park; // never posted
int in = 0;
int out = 0;
int buffer[BufferSize];
pthread_mutex_t mutex;
int DEBUG;

static void sem_wait_retry(sem_t *s) { while (sem_wait(s) != 0) /* EINTR */; }

void *producer(void *pno)
{
    int item;
    for(int i = 0; i < MaxItems; i++) {
        sleep(3);
        item = rand(); // Produce an random item
        sem_wait(&empty);
        pthread_mutex_lock(&mutex);
        buffer[in] = item;
        if (DEBUG) {
          printf("Producer %d: Insert Item %d at %d\n",
                 *((int *)pno),buffer[in],in);
        }
        in = (in+1)%BufferSize;
        pthread_mutex_unlock(&mutex);
        sem_post(&full);
    }
    sem_post(&producer_done);
    sem_wait_retry(&park); // park forever; never exit/join
    return NULL;
}

void *consumer(void *cno)
{
    for(int i = 0; i < MaxItems; i++) {
        sleep(3);
        sem_wait(&full);
        pthread_mutex_lock(&mutex);
        int item = buffer[out];
        if (DEBUG) {
          printf("Consumer %d: Remove Item %d from %d\n",
                 *((int *)cno),item, out);
        }
        out = (out+1)%BufferSize;
        pthread_mutex_unlock(&mutex);
        sem_post(&empty);
    }
    sem_post(&consumer_done);
    sem_wait_retry(&park); // park forever; never exit/join
    return NULL;
}

int main(int argc, char* argv[])
{
    int NUM_PRODUCERS = 1;
    int NUM_CONSUMERS = 1;
    DEBUG = 1;

    pthread_t pro[NUM_PRODUCERS],con[NUM_CONSUMERS];

    pthread_mutex_init(&mutex, NULL);
    sem_init(&empty,0,BufferSize);
    sem_init(&full,0,0);
    sem_init(&producer_done,0,0);
    sem_init(&consumer_done,0,0);
    sem_init(&park,0,0);

    int prod_ids[NUM_PRODUCERS];
    int cons_ids[NUM_CONSUMERS];

    for(int i = 0; i < NUM_PRODUCERS; i++) {
        prod_ids[i] = i+1;
        pthread_create(&pro[i], NULL, producer, (void *)&prod_ids[i]);
    }
    for(int i = 0; i < NUM_CONSUMERS; i++) {
        cons_ids[i] = i+1;
        pthread_create(&con[i], NULL, consumer, (void *)&cons_ids[i]);
    }

    for(int i = 0; i < NUM_PRODUCERS; i++) {
        sem_wait(&producer_done);
    }
    for(int i = 0; i < NUM_CONSUMERS; i++) {
        sem_wait(&consumer_done);
    }
    // Widen the window where both workers are safely parked (on `park`)
    // but main hasn't yet returned/exited -- gives a checkpoint interval
    // (mcmini -i N) a reliable target to land on, instead of racing main's
    // own exit() against the workers' last bit of real work (see this
    // file's top-of-file comment on the TSan-Finalize()/stack-reuse
    // artifact that showed up when that race was too tight).
    busy_wait_seconds(10);
    // Deliberately no pthread_join(): producer/consumer are still alive,
    // parked on `park`, when the process ends.

    // Explicit exit() call, NOT `return 0;`: a plain return makes glibc's
    // own __libc_start_call_main call __GI_exit -- a glibc-internal alias
    // resolved at glibc's own compile time, invisible to ANY interposition
    // technique (--wrap, LD_PRELOAD, or a strong-symbol override). Confirmed
    // live via gdb: that's exactly what happens on a plain return. Since
    // main's own restart-quiescence check-in (mc_transparent_exit(), which
    // exit() routes to) never runs in that case, the model checker never
    // sees main check in after a restart -- it just watches the whole
    // process really exit out from under it. An explicit exit() call is a
    // normal, interposable function call, so it checks in correctly.
    exit(0);
}
