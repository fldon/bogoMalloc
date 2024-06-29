#include "DTools/MiscTools.h"
#include "DTools/DTSingleton.h"
#include <cstdint>
#include <array>
#include <cassert>

/*Allocator, which uses its own mmapp-ed memory arenas to administrate the virtual memory. This way it does not interfere with malloc
All memory blocks are DWORD-aligned */

/*
 * Structure of the explicit free lists:
 * HEADER (1 WORD)
 * NEXTBLK (SIZE_OF_ADDRESS)
 * FOOTER(1 WORD)
 *
 *
 * */

/*Basic constants and macros*/
using BYTE = unsigned char;

static constexpr long long MAX_HEAP = ((long long)3436 * 10000000 * 32) - 1; // Max number of BYTES in the heap(!) Currently 32 * 32 GiB, should be increased as much as reasonably possible

//TODO: Maybe use DWORDS everywhere. After all, nobody uses 32 bit machines anymore and it would mean that the max block size would increase
using WORD = uint32_t;
using DWORD = uint64_t;

constexpr std::size_t WSIZE = sizeof(WORD); /*Word and header/footer size in Bytes*/
constexpr std::size_t DSIZE = sizeof(DWORD); /*Double word size (bytes) */

constexpr std::size_t HEADERSIZE = WSIZE;
constexpr std::size_t FOOTERSIZE = HEADERSIZE;

constexpr long long SIZE_OF_ADDRESS = sizeof(intptr_t); //size of addresses in bytes

constexpr std::size_t MIN_BLOCK_SIZE = HEADERSIZE + FOOTERSIZE + SIZE_OF_ADDRESS; //min block size in bytes INCLUDING header and footer (so min block does not include a payload!)

constexpr std::size_t OVERHEAD_SIZE = MIN_BLOCK_SIZE;

//The min block size is supposed to be DWORD-aligned! Currently, it is assumed that SIZE_OF_ADDRESS is also DWORD-aligned! If not, the above needs to be changed!

/*!
 * \brief Returns a size >= the input size that is DWORD-aligned
 * \param size
 * \return
 */
[[nodiscard]] inline constexpr std::size_t align_size_to_DWORD(const std::size_t size)
{
    //TODO: Do I need to check for case of size < DSIZE specifically?
    std::size_t asize = 0;
    asize = DSIZE * ((size + (DSIZE-1)) / DSIZE);
    return asize;
}

//Size of newly allocated slabs of memory by mmap
constexpr std::size_t SLAB_SIZE = align_size_to_DWORD(UINT32_MAX - DSIZE);

constexpr std::size_t LEFT_BOUNDARY_SIZE = OVERHEAD_SIZE + WSIZE;

constexpr std::size_t ADMIN_OVERHEAD_SIZE = LEFT_BOUNDARY_SIZE + HEADERSIZE; //size of padding, left boundary and right boundary of a slab together

//maximal block size in bytes. This is limited by the size field in the headers and footers. Currently those are 1 Word each. Includes size for header, footer and address of next block
constexpr std::size_t MAX_BLOCK_SIZE = SLAB_SIZE - ADMIN_OVERHEAD_SIZE;

//Gives index in Freelists array for the size group that asize fits into
[[nodiscard]] static constexpr std::size_t blocksize_to_freelist_idx(std::size_t asize)
{
    assert(asize <= MAX_BLOCK_SIZE);

    unsigned int idx = dtools::ceillog2(asize /  MIN_BLOCK_SIZE);

    return idx;
}

//Gives size of size group for idx in Freelist array
[[nodiscard]] static constexpr std::size_t freelist_idx_to_blocksize(std::size_t idx)
{
    return MIN_BLOCK_SIZE << idx;
}

//If this is changed, also change blocksize_to_idx and idx_to_blocksize in MyAlloc. Not very good design...
constexpr std::size_t MAX_BLOCK_ORDER = blocksize_to_freelist_idx(MAX_BLOCK_SIZE);





/*!
 * \brief This is a segregated-fits allocator that uses segregated lists of powers of 2 up to MAX_BLOCK_SIZE.
 */
class MyAlloc : public dtools::DTSingleton<MyAlloc>
{
public:
    MyAlloc();

    [[nodiscard]] void* malloc(std::size_t size);

    void free(void *ptr);

private:
    int mm_init();

    [[nodiscard]] void* mem_map_slab();

    void mem_unmap_slab(void *start_of_slab);

    int mm_request_more_memory();

    /*Pack a size and allocated bit into a word*/
    [[nodiscard]] inline WORD PACK(WORD size, WORD alloc)
    {
        assert(!(size & 0x7) && size % DSIZE == 0); //size needs to be DWORD aligned (which also means it needs to be at least DSIZE)
        WORD result = size | alloc;
        return result;
    }

    /* Read a word at address p */
    template<typename ptr>
    [[nodiscard]] WORD GET(ptr p)
    {
        return *reinterpret_cast<WORD*> (p);
    }

    template<typename ptr, typename T>
    void PUT_WORD(ptr p, T val)
    {
        *(reinterpret_cast<WORD*> (p)) = val;
    }

    template<typename ptr, typename T>
    void PUT_ADDRESS(ptr p, T val)
    {
        *(reinterpret_cast<DWORD*> (p)) = reinterpret_cast<DWORD>(val);
    }

    /* Read the size and allocated fields from address p
       The rightmost 3 bits are reserved for flags, by virtue of the size being DWORD aligned.*/
    template<typename PTR>
    [[nodiscard]] WORD GET_SIZE(PTR p)
    {
        std::size_t retval = GET(p) & ~0x7;
        return retval;
    }

    //get allocated bit of ptr p (should be hdr or ftr pointer)
    template<typename PTR>
    [[nodiscard]] WORD GET_ALLOC(PTR p)
    {
        return GET(p) & 0x1;
    }

    /* Given block ptr bp, compute address of its header and footer*/
    template<typename PTR>
    [[nodiscard]] BYTE* HDRP(PTR bp)
    {
        return reinterpret_cast<BYTE*>(bp) - HEADERSIZE;
    }

    template<typename PTR>
    [[nodiscard]] BYTE* FTRP(PTR bp)
    {
        return reinterpret_cast<BYTE*>(bp) + GET_SIZE(HDRP(bp)) - HEADERSIZE - FOOTERSIZE;
    }

    // Given block ptr bp, compute address of next block in the explicit free list
    template<typename PTR>
    [[nodiscard]] BYTE* NEXT_BLKP(PTR bp)
    {
        //Only free blocks have a next block!
        assert(!GET_ALLOC(HDRP(bp)));

        //Address of next block starts at first word after header
        BYTE *retval = *reinterpret_cast<BYTE* *> (HDRP(bp) + HEADERSIZE);
        return retval;
    }

    /*!
     * \brief Given block ptr bp, compute address of next block in virtual address space. That block does not need to be in the explicit lists
     * \param bp
     * \return
     */
    template<typename PTR>
    [[nodiscard]] BYTE* NEXT_BLKP_IMPL(PTR bp)
    {
        std::size_t blocksize = GET_SIZE(HDRP(bp));

        BYTE *resptr = reinterpret_cast<BYTE*>(HDRP(bp)) + blocksize + HEADERSIZE;

        //Sanity check: if this assertion fails, the block bp was not initialized correctly before this function was called
        assert(resptr != bp);

        return resptr;
    }

    /*!
     * \brief Given block ptr bp, compute address of previous block in virtual address space. That block does not need to be in the explicit lists
     * \param bp
     * \return
     */
    template<typename PTR>
    [[nodiscard]] BYTE* PREV_BLKP_IMPL(PTR bp)
    {
        std::size_t prev_blk_size = GET_SIZE(reinterpret_cast<BYTE*>(HDRP(bp)) - FOOTERSIZE); //get size from footer
        return reinterpret_cast<BYTE*>(HDRP(bp)) - prev_blk_size + HEADERSIZE;
    }

    [[nodiscard]] void* find_fit(std::size_t asize);

    BYTE* find_fit_in_list(BYTE* ptr, std::size_t asize);

    void place(void *const bp, std::size_t asize);

    [[nodiscard]] void* coalesce(void *bp);

    [[nodiscard]] BYTE* find_previous_block(void *bp);

    [[nodiscard]] void* get_slab_for_block(void *blockpointer);
    void unmap_slab_if_unused(void *slab_ptr);



    void remove_from_freelist(BYTE* bptr); //Remove block with block pointer bptr from the free list given by idx in mFreelists

    std::array<BYTE*, MAX_BLOCK_ORDER + 1> m_free_lists{}; //Free list for every order of 2-powers of the min block size (smallest block is order 0) - contains Block ponters, NOT HEADER POINTERS!!

    std::array<BYTE*, MAX_HEAP / SLAB_SIZE> m_slab_list{}; //List of pointers to first byte of every newly mapped slab of memory. Used for unmapping during coalescing.
    std::array<BYTE*, MAX_HEAP / SLAB_SIZE>::size_type m_slab_list_top_idx{0};


    int consecutive_frees{0};
    unsigned int total_frees{0};
    static constexpr int CHECK_UNMAP_CONSEQ_NUM = 10;
    static constexpr int CHECK_UNMAP_TOTAL_NUM = 100;
};

//Free functions internally use the singleton-object
[[nodiscard]] inline void* mm_malloc(std::size_t size)
{
    return MyAlloc::get_object()->malloc(size);
}

inline void mm_free(void *ptr)
{
    MyAlloc::get_object()->free(ptr);
}
