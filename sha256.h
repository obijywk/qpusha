#ifndef SHA256_H
#define SHA256_H

struct SHAProgramInfo;
typedef struct SHAProgramInfo SHAProgramInfo;

SHAProgramInfo* sha_program_init(int num_msgs);
void sha_program_delete(SHAProgramInfo* pi);
int sha_program_run(SHAProgramInfo* pi, int* msg_lens, void** msgs);
unsigned** sha_program_output(SHAProgramInfo* pi);

#ifdef PROFILE
void sha_dump_perf_stats();
#endif

#endif
