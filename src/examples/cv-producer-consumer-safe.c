#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define MaxItems 1
#define BufferSize 2

int in = 0;
int out = 0;
int count = 0;
int buffer[BufferSize];
pthread_mutex_t mutex;
pthread_cond_t cond;
int DEBUG;

void *producer(void *pno)
{
    int item;
    for(int i = 0; i < MaxItems; i++) {
        sleep(3);
        item = rand(); // Produce an random item
        pthread_mutex_lock(&mutex);
        while (count == BufferSize) {
            pthread_cond_wait(&cond, &mutex);
        }
        buffer[in] = item;
        if (DEBUG) {
          printf("Producer %d: Insert Item %d at %d\n",
                 *((int *)pno),buffer[in],in);
        }
        in = (in+1)%BufferSize;
        count++;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

void *consumer(void *cno)
{
    for(int i = 0; i < MaxItems; i++) {
        sleep(3);
        pthread_mutex_lock(&mutex);
        while (count == 0) {
            pthread_cond_wait(&cond, &mutex);
        }
        int item = buffer[out];
        if (DEBUG) {
          printf("Consumer %d: Remove Item %d from %d\n",
                 *((int *)cno),item, out);
        }
        out = (out+1)%BufferSize;
        count--;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

int main(int argc, char* argv[])
{
    int NUM_PRODUCERS = 1;
    int NUM_CONSUMERS = 1;
    DEBUG = 1;

    pthread_t pro[NUM_PRODUCERS],con[NUM_CONSUMERS];

    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);

    // Separate arrays per role: reusing one shared array (sized to the
    // larger of the two counts) let main()'s second loop overwrite a
    // producer's argument slot while that producer thread could still be
    // reading it via pno -- a real, TSan-detectable data race.
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
        pthread_join(pro[i], NULL);
    }
    for(int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_join(con[i], NULL);
    }

    return 0;
}
