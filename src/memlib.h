#include <climits>
using BYTE = unsigned char;

static constexpr long long MAX_HEAP = (long long)3436 * 10000000 - 1; // Max number of BYTES in the heap(!) //Currently 32 GiB, more doesn't seem to work with one malloc call

void mem_init();

void* mem_sbrk(std::size_t incr);
