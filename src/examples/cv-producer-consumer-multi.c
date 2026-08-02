// Same pattern as cv-producer-consumer-safe.c, but with multiple producers
// and multiple consumers instead of just one of each -- added specifically
// to stress-test McMini's DMTCP+TSan mutex-annotation fix ("Fix false TSan
// races on mutex-protected globals") against 3+ threads sequentially
// cycling through the same mutex, a scenario no existing -safe target here
// exercises (they're all 1 producer + 1 consumer).
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_PRODUCERS 5
#define NUM_CONSUMERS 5
#define MAX_ITEMS 20
#define BUFFER_SIZE 3

pthread_mutex_t mutex;
pthread_cond_t cond;
int buffer[BUFFER_SIZE];
int in = 0, out = 0, count = 0;

void *producer(void *arg) {
  int id = *(int *)arg;
  for (int i = 0; i < MAX_ITEMS; i++) {
    sleep(1);
    pthread_mutex_lock(&mutex);
    while (count == BUFFER_SIZE) {
      pthread_cond_wait(&cond, &mutex);
    }
    buffer[in] = id * 1000 + i;
    printf("Producer %d: insert %d at %d\n", id, buffer[in], in);
    in = (in + 1) % BUFFER_SIZE;
    count++;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
  }
  return NULL;
}

void *consumer(void *arg) {
  int id = *(int *)arg;
  for (int i = 0; i < MAX_ITEMS; i++) {
    sleep(1);
    pthread_mutex_lock(&mutex);
    while (count == 0) {
      pthread_cond_wait(&cond, &mutex);
    }
    int item = buffer[out];
    printf("Consumer %d: remove %d from %d\n", id, item, out);
    out = (out + 1) % BUFFER_SIZE;
    count--;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
  }
  return NULL;
}

int main(void) {
  pthread_t producers[NUM_PRODUCERS], consumers[NUM_CONSUMERS];
  int producer_ids[NUM_PRODUCERS], consumer_ids[NUM_CONSUMERS];

  pthread_mutex_init(&mutex, NULL);
  pthread_cond_init(&cond, NULL);

  for (int i = 0; i < NUM_PRODUCERS; i++) {
    producer_ids[i] = i;
    pthread_create(&producers[i], NULL, producer, &producer_ids[i]);
  }
  for (int i = 0; i < NUM_CONSUMERS; i++) {
    consumer_ids[i] = i;
    pthread_create(&consumers[i], NULL, consumer, &consumer_ids[i]);
  }

  for (int i = 0; i < NUM_PRODUCERS; i++) {
    pthread_join(producers[i], NULL);
  }
  for (int i = 0; i < NUM_CONSUMERS; i++) {
    pthread_join(consumers[i], NULL);
  }

  printf("Done: %d items total\n", NUM_PRODUCERS * MAX_ITEMS);
  return 0;
}
