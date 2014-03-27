#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include "sha256.h"

#define NUM_HASHES 192

int main(int argc, char *argv[]) {
  unsigned msg_lens[NUM_HASHES];
  char* msgs[NUM_HASHES];
  int i, j;

  for (i = 0; i < NUM_HASHES; i++) {
    msg_lens[i] = i;
    msgs[i] = malloc(i);
    for (j = 0; j < i; j++) {
      msgs[i][j] = 'a' + (j % 26);
    }
  }

  SHAProgramInfo* pi = sha_program_init(NUM_HASHES);
  for (i = 0; i < 5000; i++) {
    if (sha_program_run(pi, msg_lens, (void**)msgs) != 0) {
      fprintf(stderr, "qpu program run failed\n");
    } else {
      if (i == 0) {
        unsigned** output = sha_program_output(pi);
        for (i = 0; i < NUM_HASHES; i++) {
          printf("%d ", i);
          for (j = 0; j < 8; j++) {
            printf("%08x", *(output[i] + j));
          }
          printf("\n");
        }
      }
    }
  }

#ifdef PROFILE
  sha_dump_perf_stats();
#endif

  sha_program_delete(pi);
  return 0;
}
