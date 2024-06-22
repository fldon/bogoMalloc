#include "memlib.h"
#include "DTools/MiscTools.h"
#include <cstdint>
#include <array>
#include <map>
#include <cassert>

/*Allocator, which uses its own "heap-like" structure in virtual memory (using the classes provided by memlib)
 *this is so that this allocator does not interfere with the malloc provided by cstdlib.h (as replacing it outright would require the replacement of other contents in that header)*/

void* mm_malloc(std::size_t size);

void mm_free(void *ptr);

/*Basic constants and macros*/

using WORD = uint32_t;
using DWORD = uint64_t;

constexpr std::size_t WSIZE = sizeof(WORD); /*Word and header/footer size in Bytes*/
constexpr std::size_t DSIZE = sizeof(DWORD); /*Double word size (bytes) */

constexpr std::size_t HEADERSIZE = WSIZE;
constexpr std::size_t FOOTERSIZE = HEADERSIZE;

constexpr long long SIZE_OF_ADDRESS = sizeof(intptr_t); //size of addresses in bytes

constexpr std::size_t MIN_BLOCK_SIZE = DSIZE + SIZE_OF_ADDRESS; //min block size in bytes INCLUDING header and footer (so min block does not include a payload!)

constexpr std::size_t OVERHEAD_SIZE = MIN_BLOCK_SIZE;

//The min block size is supposed to be DWORD-aligned! Currently, it is assumed that SIZE_OF_ADDRESS is also DWORD-aligned! If not, the above needs to be changed!

[[nodiscard]] inline constexpr std::size_t align_size_to_DWORD(const std::size_t size)
{
    std::size_t asize = 0;
    asize = DSIZE * ((size + (DSIZE-1)) / DSIZE);
    return asize;
}

//maximal block size in bytes. This is limited by the size field in the headers and footers. Currently those are 1 Word each. Includes size for header, footer and address of next block
constexpr std::size_t MAX_BLOCK_SIZE = align_size_to_DWORD(UINT32_MAX - DSIZE);

constexpr std::size_t MAX_BLOCK_ORDER = ceillog2(MAX_BLOCK_SIZE / MIN_BLOCK_SIZE);


/*
 * Structure of the explicit free lists:
 * HEADER (1 WORD)
 * NEXTBLK (size of an address)
 * FOOTER(1 WORD)
 *
 *
 * */

//TODO make this class handle all allocation and freeing. And just call the member functions of a global object of this class when using the static mm_malloc and mm_free functions
//This is a segregatet-fits allocator that uses segregatet lists of powers of 2 up to MAX_BLOCK_SIZE
//Note that this is NOT a buddy allocator
class MyAlloc
{
public:
    static constexpr std::size_t CHUNKSIZE = 1 << 12; /*Extend heap by this amount (Bytes)*/

    MyAlloc();

    void* malloc(std::size_t size);

    void free(void *ptr);

private:

    int mm_init();

    /*Pack a size and allocated bit into a word*/
    //TODO: should this return ulonglongs, or just words?
    inline WORD PACK(WORD size, WORD alloc)
    {
        assert(!(size & 0x7) && size % DSIZE == 0); //size needs to be DWORD aligned (which also means it needs to be at least DSIZE)
        WORD result = size | alloc;
        return result;
    }

    /* Read a word at address p */
    template<typename ptr>
    WORD GET(ptr p)
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

    /* Read the size and allocated fields from address p */
    template<typename PTR>
    WORD GET_SIZE(PTR p)
    {
        return GET(p) & ~0x7;
    }

    //get allocated bit of ptr p (should be hdr or ftr pointer)
    template<typename PTR>
    WORD GET_ALLOC(PTR p)
    {
        WORD test = GET(p);
        return GET(p) & 0x1;
    }

    /* Given block ptr bp, compute address of its header and footer*/
    template<typename PTR>
    BYTE* HDRP(PTR bp)
    {
        return reinterpret_cast<BYTE*>(bp) - HEADERSIZE;
    }

    template<typename PTR>
    BYTE* FTRP(PTR bp)
    {
        return reinterpret_cast<BYTE*>(bp) + GET_SIZE(HDRP(bp)) - HEADERSIZE - FOOTERSIZE;
    }


    // Given block ptr bp, compute address of next and previous blocks
    //TODO: this works with an IMPLICIT free list: change it to work with the explicit free lists I want to use!
/*
    template<typename PTR>
    BYTE* NEXT_BLKP(PTR bp)
    {
        return reinterpret_cast<BYTE*>(bp) + GET_SIZE(reinterpret_cast<BYTE*>(bp) - WSIZE);
    }
    template<typename PTR>
    BYTE* PREV_BLKP(PTR bp)
    {
        return reinterpret_cast<BYTE*>(bp) - GET_SIZE(reinterpret_cast<BYTE*>(bp) - DSIZE);
    }
*/

    // Given block ptr bp, compute address of next block in the explicit free list
    template<typename PTR>
    BYTE* NEXT_BLKP(PTR bp)
    {
        //Only free blocks have a next block!
        assert(!GET_ALLOC(HDRP(bp)));

        //Address of next block starts at first word after header
        BYTE *retval = *reinterpret_cast<BYTE* *> (HDRP(bp) + HEADERSIZE);

        //TODO: remove this, it is just for findig a bug
        assert((std::size_t)retval > (std::size_t)0x70000000000 || retval == nullptr);



        return retval;
    }

    /*!
     * \brief Given block ptr bp, compute address of next block in virtual address space. That block does not need to be in the explicit lists
     * \param bp
     * \return
     */
    template<typename PTR>
    BYTE* NEXT_BLKP_IMPL(PTR bp)
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
    BYTE* PREV_BLKP_IMPL(PTR bp)
    {
        std::size_t prev_blk_size = GET_SIZE(reinterpret_cast<BYTE*>(HDRP(bp)) - FOOTERSIZE); //get size from footer
        return reinterpret_cast<BYTE*>(HDRP(bp)) - prev_blk_size + HEADERSIZE;
    }


    /*!
 * \brief finds a fitting block with a free payload size of at least asize - might be more.
 * \param asize
 * \return a pointer to the beginning of the payload block or nullptr
 */
    [[nodiscard]] void* find_fit(std::size_t asize);

    /*!
     * \brief Finds a fitting block in the explicit free list given by ptr. Returns nullptr if no suitable block with payload size >= asize was found.
     * \param ptr
     * \return
     */
    BYTE* find_fit_in_list(BYTE* ptr, std::size_t asize);

    /*!
     * \brief "places" or reserves contents of size asize in the block given by bp. Splits bp down as far as possible while doing so.
     * \param bp
     * \param asize
     */
    void place(void *const bp, std::size_t asize);

    void* coalesce(void *bp);

    [[nodiscard]] BYTE* find_previous_block(void *bp);

    std::array<BYTE*, MAX_BLOCK_ORDER + 1> mFreelists{}; //Free list for every order of 2-powers of the min block size (smallest block is order 0) - contains Block ponters, NOT HEADER POINTERS!!
    std::map<std::size_t, std::size_t> freelist_sizes{}; //Contains the size in bytes for every item in mFreelists
    [[nodiscard]] constexpr std::size_t blocksize_idx(std::size_t asize) const; //Gives index in mFreelists array for the size group that asize fits into
    void remove_from_freelist(BYTE* bptr); //Remove block with block pointer bptr from the free list given by idx in mFreelists

    BYTE *heap_listp = nullptr; //Points to the beginning of the mapped area. TODO: this should at some point probabyl be a list: one pointer for each newly mmap-ed area (not used with sbrk)

};
