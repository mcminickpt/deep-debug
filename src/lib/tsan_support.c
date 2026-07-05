#include "mcmini/spy/checkpointing/tsan_support.h"

#include <stdio.h>

int thread_blocks_signal(pid_t tid, int signo) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int)tid);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    return 0;
  }

  char line[256];
  unsigned long long sigblk = 0;
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "SigBlk: %llx", &sigblk) == 1) {
      found = 1;
      break;
    }
  }
  fclose(f);

  if (!found) {
    return 0;
  }
  return (int)((sigblk >> (signo - 1)) & 1ULL);
}
