#include "memlib.h"
#include <cerrno>
#include <iostream>
#include <cstdlib> //TODO: should be later removed


//TODO: replace all of the sbrk and heap stuff with mutliple mmapped areas. Reason: one malloc call can only give you 32 gigs

static BYTE *mem_heap; //first byte of heap
static BYTE *mem_brk; //last byte of heap + 1
static BYTE *mem_max_addr; //Max legal heap addr + 1

/*!
 * \brief Initialize the memory model
 */
void mem_init()
{
    //TODO: should not use malloc, but instead mmap directly: just map a large virtual page in own zone that is a "second heap"
    mem_heap = reinterpret_cast<BYTE*> (malloc(MAX_HEAP));
    mem_brk = reinterpret_cast<BYTE*> (mem_heap);
    mem_max_addr = reinterpret_cast<BYTE*> (mem_heap + MAX_HEAP);
}

/*!
 * \brief extends the heap by incr bytes and returns the start address of the new area. Heap cannot be shrunk
 * TODO: potentially make heap shrinkable
 * */
void* mem_sbrk(std::size_t incr)
{
    BYTE *old_brk = mem_brk;

    if( (incr < 0 || ((mem_brk + incr) >= mem_max_addr)) )
    {
        errno = ENOMEM;
        std::cerr << "ERROR: mem_sbrk failed. Ran out of memory\n";
        return reinterpret_cast<void*> (-1);
    }
    mem_brk += incr;
    return reinterpret_cast<void*> (old_brk);
}

/*!
 * \brief Maps memory for a slab of he size required by incr. Currently just uses sbrk. Is supposed to use mmap.
 * \param incr
 * \return
 */
void* mem_map_slab(std::size_t incr)
{
    //TODO: Change to mmap
    return mem_sbrk(incr);
}
