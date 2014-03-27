#include <byteswap.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "mailbox.h"
#include "sha256.h"

#define NUM_QPUS 12
#define NUM_QPU_ELEMENTS 16

#define GPU_MEM_FLG 0xC // cached=0xC; direct=0x4
#define GPU_MEM_MAP 0x0 // cached=0x0; direct=0x20000000

#define MEM_SIZE 0x2000
#define MEM_ALIGN 0x1000

// per-qpu memory offsets
#define OFFSET_WORKING_VARS 0x0
#define OFFSET_MESSAGE_CHUNKS 0x200
#define OFFSET_UNIFORMS 0x1200

#define MSG_CHUNK_BYTES 64

// The smallest number of bytes we may need to append to the raw message.
// 1 byte for final bit 1
// 8 bytes for 64-bit message length
#define MIN_MSG_PAD_BYTES 9

static unsigned int program[] = {
    #include "sha256.hex"
};

static unsigned int initial_hash_values[] = {
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

static unsigned int round_constants[] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static int mb = 0;

#ifdef PROFILE
static double load_time = 0, compute_time = 0;
void add_perf_time(double* acc,
                   struct timespec* start_time,
                   struct timespec* end_time) {
  *acc += (end_time->tv_sec - start_time->tv_sec) +
    ((double)(end_time->tv_nsec - start_time->tv_nsec) / (double)(0x3fffffff));
}

void sha_dump_perf_stats() {
  printf("load time: %f  compute time: %f\n", load_time, compute_time);
}
#endif

typedef struct QPUInfo {
  void* arm_pointer;
  void* gpu_pointer;
  unsigned handle;
} QPUInfo;

void qpu_new_hashes(QPUInfo* qpu_info) {
  // initialize hash working variables
  int elem;
  for (elem = 0; elem < NUM_QPU_ELEMENTS; elem++) {
    memcpy(qpu_info->arm_pointer + OFFSET_WORKING_VARS +
           sizeof(initial_hash_values) * elem,
           initial_hash_values,
           sizeof(initial_hash_values));
  }
}

QPUInfo* qpu_init(int num_qpus, int qpu_num) {
  QPUInfo* qpu_info = (QPUInfo*)malloc(sizeof(QPUInfo));
  memset(qpu_info, 0, sizeof(QPUInfo));

  qpu_info->handle = mem_alloc(mb, MEM_SIZE, MEM_ALIGN, GPU_MEM_FLG);
  if (qpu_info->handle == 0) {
    fprintf(stderr, "mem_alloc failed\n");
    free(qpu_info);
    return 0;
  }

  qpu_info->gpu_pointer = (void*)mem_lock(mb, qpu_info->handle);
  qpu_info->arm_pointer =
    mapmem((unsigned)qpu_info->gpu_pointer + GPU_MEM_MAP, MEM_SIZE);

  // initialize uniforms
  unsigned* p = (unsigned*)(qpu_info->arm_pointer + OFFSET_UNIFORMS);
  *(p++) = (unsigned)(qpu_info->gpu_pointer + OFFSET_MESSAGE_CHUNKS);
  *(p++) = (unsigned)(qpu_info->gpu_pointer + OFFSET_WORKING_VARS);
  memcpy(p, round_constants, sizeof(round_constants));
  p += 64;
  *(p++) = (unsigned)qpu_num;
  *(p++) = (unsigned)num_qpus;

  qpu_new_hashes(qpu_info);

  return qpu_info;
}

void qpu_delete(QPUInfo* qpu_info) {
  unmapmem(qpu_info->arm_pointer, MEM_SIZE);
  mem_unlock(mb, qpu_info->handle);
  mem_free(mb, qpu_info->handle);
  free(qpu_info);
}

void qpu_dump_mem(QPUInfo* qpu_info) {
  int i;
  for (i = 0; i < MEM_SIZE / 4; i++) {
    if ((i % 8) == 0) {
      printf("\n%08x:", qpu_info->gpu_pointer + i * 4);
    }
    printf(" %08x", ((unsigned *)qpu_info->arm_pointer)[i]);
  }
  printf("\n");
}

// Returns:
//  < 0 if the message was completed more than one iteration ago
//  = 0 if this is the first iteration where the message is complete
//  > 0 if message chunks remain, and it should be called again later with
//      iteration + 1
int qpu_load_message_chunk(QPUInfo* qpu_info,
                           int elem,
                           int len,
                           void* msg,
                           int iteration) {
  int i;
  if (len + MIN_MSG_PAD_BYTES <= (iteration - 1) * MSG_CHUNK_BYTES) {
    // We completed handling this message more than one iteration ago.
    return -1;
  }
  if (len + MIN_MSG_PAD_BYTES <= iteration * MSG_CHUNK_BYTES) {
    // We completed handling this message during the last iteration.
    return 0;
  }

  unsigned* p = (unsigned*)(qpu_info->arm_pointer +
                            OFFSET_MESSAGE_CHUNKS +
                            elem * MSG_CHUNK_BYTES * 4);
  int msg_pos = iteration * MSG_CHUNK_BYTES;
  unsigned char* chunk_bytes = ((unsigned char*)msg) + msg_pos;
  for (i = 0; i < MSG_CHUNK_BYTES; ++i) {
    if ((i % 4) == 0 && msg_pos + 4 <= len) {
      // optimization for the common case
      *p = bswap_32(*(unsigned*)(chunk_bytes + i));
      i += 3;
      msg_pos += 3;
    } else if (msg_pos < len) {
      switch (i % 4) {
      case 0:
        *p = ((unsigned)*(chunk_bytes + i)) << 24;
        break;
      case 1:
        *p |= ((unsigned)*(chunk_bytes + i)) << 16;
        break;
      case 2:
        *p |= ((unsigned)*(chunk_bytes + i)) << 8;
        break;
      case 3:
        *p |= ((unsigned)*(chunk_bytes + i));
        break;
      }
    } else if (msg_pos == len) {
      switch (i % 4) {
      case 0:
        *p = 0x80000000;
        i += 3;
        msg_pos += 3;
        break;
      case 1:
        *p |= 0x00800000;
        *p &= 0xFF800000;
        i += 2;
        msg_pos += 2;
        break;
      case 2:
        *p |= 0x00008000;
        *p &= 0xFFFF8000;
        i += 1;
        msg_pos += 1;
        break;
      case 3:
        *p |= 0x00000080;
        *p &= 0xFFFFFF80;
        break;
      }
    } else if (i < MSG_CHUNK_BYTES - 8) {
      if ((i % 4) == 0) {
        // optimization for the common case
        memset(p, 0, MSG_CHUNK_BYTES - 8 - i);
        p += (MSG_CHUNK_BYTES - 8 - i) / sizeof(unsigned);
        i = MSG_CHUNK_BYTES - 9;
        continue;
      }
      switch (i % 4) {
      case 0:
        *p = 0;
        break;
      case 1:
        *p &= 0xFF000000;
        break;
      case 2:
        *p &= 0xFFFF0000;
        break;
      case 3:
        *p &= 0xFFFFFF00;
        break;
      }
    } else if (i == MSG_CHUNK_BYTES - 8) {
      // We have enough space to write the size - do it, and be done.
      *(p++) = (len >> 29);
      *(p++) = (len <<  3);
      return 1;
    } else {
      // i > MSG_CHUNK_BYTES - 8
      // We don't have enough space to write the size. We'll have to do it in
      // the next iteration.
      if ((i % 4) == 0) {
        // optimization for the common case
        memset(p, 0, MSG_CHUNK_BYTES - i);
        break;
      }
      switch (i % 4) {
      case 0:
        *p = 0;
        break;
      case 1:
        *p &= 0xFF000000;
        break;
      case 2:
        *p &= 0xFFFF0000;
        break;
      case 3:
        *p &= 0xFFFFFF00;
        break;
      }
    }
    if ((i+1) % 4 == 0) p++;
    ++msg_pos;
  }
  return 1;
}

struct SHAProgramInfo {
  int num_msgs;
  int num_qpus;
  QPUInfo** qpus;
  unsigned program_handle;
  void *program_arm, *program_gpu;
  unsigned** output;
};

void sha_program_delete(SHAProgramInfo* pi) {
  int i;
  if (pi->program_arm) {
    unmapmem(pi->program_arm, MEM_SIZE);
  }
  if (pi->program_handle) {
    mem_unlock(mb, pi->program_handle);
    mem_free(mb, pi->program_handle);
  }
  if (pi->qpus) {
    for (i = 0; i < pi->num_qpus; i++) {
      qpu_delete(pi->qpus[i]);
    }
    free(pi->qpus);
  }
  if (qpu_enable(mb, 0) != 0) {
    fprintf(stderr, "qpu_enable(mb, 0) failed\n");
  }
  if (pi->output) {
    for (i = 0; i < pi->num_msgs; i++) {
      free(pi->output[i]);
    }
    free(pi->output);
  }
  free(pi);
  mbox_close(mb);
}

SHAProgramInfo* sha_program_init(int num_msgs) {
  int i;

  if (mb == 0) {
    mb = mbox_open();
  }

  SHAProgramInfo* pi = malloc(sizeof(SHAProgramInfo));
  memset(pi, 0, sizeof(SHAProgramInfo));
  pi->num_msgs = num_msgs;

  pi->num_qpus = ((num_msgs-1) / NUM_QPU_ELEMENTS) + 1;
  if (pi->num_qpus > NUM_QPUS) {
    fprintf(stderr, "max num messages is %d\n", NUM_QPU_ELEMENTS * NUM_QPUS);
    sha_program_delete(pi);
    return 0;
  }

  if (qpu_enable(mb, 1) != 0) {
    fprintf(stderr, "qpu_enable(mb, 1) failed\n");
    sha_program_delete(pi);
    return 0;
  }

  pi->qpus = malloc(pi->num_qpus * sizeof(QPUInfo*));
  memset(pi->qpus, 0, pi->num_qpus * sizeof(QPUInfo*));
  for (i = 0; i < pi->num_qpus; i++) {
    pi->qpus[i] = qpu_init(pi->num_qpus, i);
    if (pi->qpus[i] == 0) {
      fprintf(stderr, "qpu_init() failed\n");
      sha_program_delete(pi);
      return 0;
    }
  }

  pi->program_handle = mem_alloc(mb, MEM_SIZE, MEM_ALIGN, GPU_MEM_FLG);
  if (pi->program_handle == 0) {
    fprintf(stderr, "mem_alloc() failed\n");
    sha_program_delete(pi);
    return 0;
  }
  pi->program_gpu = (void*)mem_lock(mb, pi->program_handle);
  pi->program_arm = mapmem((unsigned)pi->program_gpu + GPU_MEM_MAP, MEM_SIZE);

  // write program
  memcpy(pi->program_arm, program, sizeof(program));

  // write launch messages
  unsigned* p = (unsigned*)(pi->program_arm + sizeof(program));
  for (i = 0; i < pi->num_qpus; i++) {
    *(p++) = (unsigned)(pi->qpus[i]->gpu_pointer + OFFSET_UNIFORMS);
    *(p++) = (unsigned)pi->program_gpu;
  }

  pi->output = malloc(num_msgs * sizeof(unsigned*));
  return pi;
}

int sha_program_run(SHAProgramInfo* pi, int* msg_lens, void** msgs) {
  int i, iteration = 0, remaining = pi->num_msgs;

#ifdef PROFILE
  struct timespec start_time, end_time;
#endif

  for (i = 0; i < pi->num_qpus; i++) {
    qpu_new_hashes(pi->qpus[i]);
  }

  while (1) {
    remaining = 0;

#ifdef PROFILE
    clock_gettime(CLOCK_REALTIME, &start_time);
#endif
    for (i = 0; i < pi->num_msgs; i++) {
      int result = qpu_load_message_chunk(pi->qpus[i/NUM_QPU_ELEMENTS],
                                          i % NUM_QPU_ELEMENTS,
                                          msg_lens[i],
                                          msgs[i],
                                          iteration);
      if (result == 0) {
        pi->output[i] = malloc(8 * sizeof(unsigned));
        memcpy(pi->output[i],
               pi->qpus[i/NUM_QPU_ELEMENTS]->arm_pointer + OFFSET_WORKING_VARS +
               8 * sizeof(unsigned) * (i % NUM_QPU_ELEMENTS),
               8 * sizeof(unsigned));
      } else if (result == 1) {
        ++remaining;
      }
    }
#ifdef PROFILE
    clock_gettime(CLOCK_REALTIME, &end_time);
    add_perf_time(&load_time, &start_time, &end_time);
#endif

    if (remaining == 0) {
      return 0;
    }

#ifdef PROFILE
    clock_gettime(CLOCK_REALTIME, &start_time);
#endif
    unsigned r = execute_qpu(mb,
                             pi->num_qpus,
                             (unsigned)(pi->program_gpu + sizeof(program)),
                             1 /* noflush */,
                             3000 /* timeout (ms) */);
#ifdef PROFILE
    clock_gettime(CLOCK_REALTIME, &end_time);
    add_perf_time(&compute_time, &start_time, &end_time);
#endif

    if (r != 0) return r;
    iteration++;
  }
}

unsigned** sha_program_output(SHAProgramInfo* pi) {
  return pi->output;
}
