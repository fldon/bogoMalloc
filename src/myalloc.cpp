#include "myalloc.h"
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>
#include <algorithm>

/*!
 * \brief MyAlloc::MyAlloc
 */
MyAlloc::MyAlloc()
{
    mm_init();
}

/*!
 * \brief allocate one block of max_block_size and additional boundary blocks left and right
 * \return
 */
int MyAlloc::mm_init()
{
    return mm_request_more_memory();
}

/*!
 * \brief Requests a new slab of memory of SLAB_SIZE, initializes it, and puts it on the free list
 * \return
 */
int MyAlloc::mm_request_more_memory()
{
    //Cannot fit more slabs into the slab list
    if(m_slab_list_top_idx == m_slab_list.size())
    {
        return -1;
    }

    BYTE *new_mem_ptr = reinterpret_cast<BYTE*>(mem_map_slab());

    if(new_mem_ptr == reinterpret_cast<BYTE*>(-1))
        return -1;

    //Put newly mapped memory on top of list of slabs
    m_slab_list.at(m_slab_list_top_idx) = new_mem_ptr;
    ++m_slab_list_top_idx;

    //put boundary blocks left and right of free space
    PUT_WORD(new_mem_ptr, 0); //Alignment padding for header,footer,and epilogue blocks ----- This assumes that header and footer are 1 WORD in size!
    PUT_WORD(new_mem_ptr + WSIZE, PACK(OVERHEAD_SIZE, 1)); //left boundary header
    PUT_ADDRESS(new_mem_ptr + WSIZE + HEADERSIZE, nullptr); //Address of nonexistant next block for prologue block
    PUT_ADDRESS(new_mem_ptr + WSIZE + HEADERSIZE + SIZE_OF_ADDRESS, nullptr); //Address of nonexistant previous block for prologue block
    PUT_WORD(new_mem_ptr + WSIZE + HEADERSIZE + 2 * SIZE_OF_ADDRESS, PACK(OVERHEAD_SIZE, 1)); //left boundary footer
    PUT_WORD(new_mem_ptr + SLAB_SIZE - HEADERSIZE, PACK(0,1)); //Epilogue header


    constexpr std::size_t remaining_free_block_size = MAX_BLOCK_SIZE;

    BYTE* prevtop = m_free_lists.at(blocksize_to_freelist_idx(remaining_free_block_size));

    //Create one large free block out of the rest of the memory. so starting from Fourth block to the second-to-last block
    PUT_WORD(new_mem_ptr + LEFT_BOUNDARY_SIZE, PACK(remaining_free_block_size, 0)); //block header
    PUT_ADDRESS(new_mem_ptr + LEFT_BOUNDARY_SIZE + HEADERSIZE, prevtop); //address of nonexistant next block
    PUT_ADDRESS(new_mem_ptr + LEFT_BOUNDARY_SIZE + HEADERSIZE + SIZE_OF_ADDRESS, nullptr); //address of nonexistant previous block
    PUT_WORD(new_mem_ptr + SLAB_SIZE - HEADERSIZE - FOOTERSIZE, PACK(remaining_free_block_size, 0)); //block footer

    //insert the aforementioned large free block into the largest size class of the free list array
    BYTE* newtop = new_mem_ptr + LEFT_BOUNDARY_SIZE + HEADERSIZE; //Block address of large free block (NOT HEADER ADDRESS!!)

    assert(HDRP(newtop) == new_mem_ptr + LEFT_BOUNDARY_SIZE);
    assert (!GET_ALLOC(HDRP(newtop)));

    if(NEXT_BLKP(newtop) != nullptr)
    {
        PUT_ADDRESS(HDRP(NEXT_BLKP(newtop)) + HEADERSIZE + SIZE_OF_ADDRESS, newtop); //Put new block as previous for potentially existing block
    }

    m_free_lists.at(blocksize_to_freelist_idx(remaining_free_block_size)) = newtop;

    return 0;
}


/*!
 * \brief Maps memory for a slab of he size required by incr. Currently just uses sbrk. Is supposed to use mmap and save the mapped region pointers for unmapping later.
 * \param incr
 * \return
 */
void* MyAlloc::mem_map_slab()
{
    void *retval = mmap(NULL, SLAB_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, 0, 0);
    return retval;
}

/*!
    * \brief finds a fitting block with a free payload size of at least asize - might be more.
    * \param asize
    * \return a pointer to the beginning of the payload block or nullptr
    */
void *MyAlloc::find_fit(std::size_t asize)
{
    //return immediately if asize is larger than largest possible block size (including overhead and alignment reqs)
    if(asize >= MAX_BLOCK_SIZE)
        return nullptr;

    //Idea: go through all free lists: then check every header until one has the required size
    //if found: return pointer to the data block of the block with that header
    BYTE *ret = nullptr;

    //First: Check freelist that had the last free block added. In most cases this could have a block of enough size
    ret = find_fit_in_list(m_free_lists.at(m_last_freed_idx), asize);

    //go through freelists and check:s
    //1. if the size class of that list is large enough
    if(ret == nullptr)
    {
        for(std::size_t i = blocksize_to_freelist_idx(asize); i < m_free_lists.size(); ++i)
        {
            //2. go through that list if the size class and see if a fit can be found in that specific explicit free list
            ret = find_fit_in_list(m_free_lists.at(i), asize);
            if(ret != nullptr)
            {
                return ret;
            }
        }
    }
    return ret;
}

/*!
 * \brief Finds the first fit in the explicit free list given by ptr that can contain a block of asize (including overhead). Returns the block pointer, NOT the headerpointer!
 * \param ptr
 * \param asize
 * \return
 */
BYTE* MyAlloc::find_fit_in_list(BYTE* ptr, std::size_t asize)
{
    BYTE *currAddr = ptr;
    BYTE *ret = nullptr;
    while(currAddr != nullptr && GET_SIZE(HDRP(currAddr)) > 0) //size == 0 means next "block" is Epilogue block
    {
        std::size_t currSize = GET_SIZE(HDRP(currAddr));
        WORD currAlloc = GET_ALLOC(HDRP(currAddr));
        if(currSize >= asize && !currAlloc)
        {
            ret = currAddr;
            return ret;
        }
        currAddr = NEXT_BLKP(currAddr);
    }
    return ret;
}

/*!
     * \brief "places" or reserves contents of size asize in the block given by bp (which is known to have enough size to fit a block (including overhead) of asize).
     * Split block bp if remainder would be equal or larger than minimum block size.
     * \param bp
     * \param asize is the TOTAL size of the block with overhead.
     */
void MyAlloc::place(void *const bp, std::size_t asize)
{
    //Assume at this point: asize is DWORD-aligned!
    assert(asize % DSIZE == 0);

    std::size_t non_split_size = GET_SIZE(HDRP(bp));

    //Assume at this point: block can hold asize
    assert(non_split_size >= asize);

    //remove old pre-split block from explicit free list (happens whether I split it or not, which is why I do it here)
    remove_from_freelist(reinterpret_cast<BYTE*>(bp));

    //check if remainder of size after placing asize is larger or equal to min block size
    //If not: Do nothing
    //If yes: change Size in header, create extra footer block for first block, create extra header block and address block for second block, change size in footer of second block
    if(non_split_size >= asize + OVERHEAD_SIZE)
    {
        //split asize and bsize, so that both are DWORD aligned and at least min size
        //for this, we need to check:
        //Is the resulting bsize DWORD-aligned? - The answer should always be yes, because asize is guaranteed to be DWORD-aligned, and so is MIN_BLOCK_SIZE
        //If yes, do the split
        //If no,  don't split after all
        //There is no point in trying larger asize parameters, because the alignment is taken care of elsewhere and we couldn't handle un-even blocksizes anyway
        
        std::size_t bsize = non_split_size - asize; //Size remaining for second block (includes size needed for overhead)

        assert(bsize % DSIZE == 0);

        //if split: create and/or change header and footer for A and B block (and address block for b block)
        //No address block for a block because it is no longer free!
        PUT_WORD(reinterpret_cast<WORD*>(HDRP(bp)), PACK(asize,1));
        PUT_WORD(FTRP(bp), PACK(asize, 1)); //footer of first block with asize-size

        //sanity check
        assert(GET_SIZE(HDRP(bp)) == asize);

        //Split off free block to the right
        //BYTE* splitblockp = reinterpret_cast<BYTE*>(HDRP(bp) + GET_SIZE(HDRP(bp)) + HEADERSIZE);
        BYTE *splitblockp = NEXT_BLKP_IMPL(bp);
        BYTE* prevtop_b = m_free_lists.at(blocksize_to_freelist_idx(bsize));
        PUT_WORD(HDRP(splitblockp), PACK(bsize,0)); //Header of split block
        PUT_ADDRESS(HDRP(splitblockp) + HEADERSIZE, prevtop_b); //next Address block of split block
        PUT_ADDRESS(HDRP(splitblockp) + HEADERSIZE + SIZE_OF_ADDRESS, nullptr); //address of nonexistant previous block
        PUT_WORD(HDRP(splitblockp) + HEADERSIZE + SIZE_OF_ADDRESS, PACK(bsize, 0)); //Footer of split block


        assert(NEXT_BLKP_IMPL(bp) ==  splitblockp);

        if(prevtop_b != nullptr)
        {
            PUT_ADDRESS(HDRP(prevtop_b) + HEADERSIZE + SIZE_OF_ADDRESS, splitblockp); //Put new split block as previous for potentially existing block
        }

        //insert new free split-block into correct explicit free list
        m_free_lists.at(blocksize_to_freelist_idx(bsize)) = reinterpret_cast<BYTE*> (splitblockp);
    }
    else
    {
        PUT_WORD(reinterpret_cast<WORD*>(HDRP(bp)), PACK(non_split_size,1));
        PUT_WORD(FTRP(bp), PACK(non_split_size, 1));
    }
    consecutive_frees = 0;
}

/*!
 * \brief MyAlloc::malloc
 * \param size
 * \return
 */
void* MyAlloc::malloc(std::size_t size)
{
    //find fit (using find_fit, duh) and create block out of found block (split beforehand inside place function)
    //also remove the block from the free list after allocating it
    //If no fit can be found, allocate a new memory slab and put that new block on the free list

    /* Ignore spurious requests */
    if(size == 0 || size > MAX_BLOCK_SIZE)
        return nullptr;

    void *retp = nullptr;

    /* Adjust block size to include overhead and DWORD alignment requirements */
    std::size_t asize = align_size_to_DWORD(OVERHEAD_SIZE + size);


    /*Search the free lists for a fit */
    retp = find_fit(asize);
    if(retp != nullptr)
    {
        place(retp, asize);
        return retp;
    }

    //handle getting more memory in case no fit was found
    if(mm_request_more_memory() == -1)
    {
        return nullptr;
    }
    //try again if memory could be requested
    retp = malloc(size);

    //At this point this should not be possible; request was reasonable and we got a full new slab
    assert(retp != nullptr);

    return retp;
}

/*!
 * \brief free block and coalesce as far as possible by checking repeatedly to the left and right for free blocks: then place in appropiate size class in free list
 * \param ptr
 */
void MyAlloc::free(void *bp)
{
    //set header and footer to size of block and alloc bit set to 0:
    std::size_t size = GET_SIZE(HDRP(bp));
    PUT_WORD(HDRP(bp), PACK(size, 0));
    PUT_WORD(FTRP(bp), PACK(size, 0));

    //Coalesce as far as possible
    //if((total_frees % CHECK_UNMAP_TOTAL_NUM == 0) || consecutive_frees % CHECK_UNMAP_CONSEQ_NUM == 0)
    bp = coalesce(bp);

    //insert newly-freed block into correct explicit free list, and insert correct address block into freed block
    BYTE* prevtop = m_free_lists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bp))));
    PUT_ADDRESS(HDRP(bp) + HEADERSIZE, prevtop); //address of potentially nonenxistant next block
    PUT_ADDRESS(HDRP(bp) + HEADERSIZE + SIZE_OF_ADDRESS, prevtop); //address of potentially nonenxistant next block

    if(prevtop != nullptr)
    {
        PUT_ADDRESS(HDRP(prevtop) + HEADERSIZE + SIZE_OF_ADDRESS, bp); //Put new free block as previous for potentially existing block
    }

    m_free_lists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bp)))) = reinterpret_cast<BYTE*>(bp);

    m_last_freed_idx = blocksize_to_freelist_idx(GET_SIZE(HDRP(bp)));

    ++total_frees;
    ++consecutive_frees;

    //Unmapping policy: Enough consecutive free calls or free calls in total
    if((total_frees % CHECK_UNMAP_TOTAL_NUM == 0) || consecutive_frees % CHECK_UNMAP_CONSEQ_NUM == 0)
    {
        void *slabp = get_slab_for_block(bp);
        unmap_slab_if_unused(slabp);
    }
}

/*!
 * \brief Coalesce the left and right blocks of bp if they are free. Returns block pointer to the resulting free block.
 * Removes coalesced blocks from free lists, but does NOT insert the resulting block
 * \param bp
 */
void* MyAlloc::coalesce(void *bp)
{
    //Coalesce blocks left and right according to the 4 cases in the book
    //However, the header of the resulting block must be followed by an (empty) address block
    WORD prev_alloc = GET_ALLOC(HDRP(PREV_BLKP_IMPL(bp)));
    WORD next_alloc = GET_ALLOC(HDRP(NEXT_BLKP_IMPL(bp)));
    std::size_t size = GET_SIZE(HDRP(bp));

    if(prev_alloc && next_alloc) //case 1, no coalescing possible
    {
        return bp;
    }

    if (prev_alloc && !next_alloc) //case 2, right block coalesced
    {
        //remove right free block from the correct free list
        BYTE *right_block = reinterpret_cast<BYTE*>(NEXT_BLKP_IMPL(bp));
        remove_from_freelist(right_block);

        //Change sizes in header and footer
        size += GET_SIZE(HDRP(NEXT_BLKP_IMPL(bp)));
        PUT_WORD(HDRP(bp), PACK(size, 0));
        PUT_WORD(FTRP(bp), PACK(size, 0));
    }

    else if (!prev_alloc && next_alloc) //case 3, left block coalesced
    {
        //remove left free block from the correct free list
        remove_from_freelist(reinterpret_cast<BYTE*>(PREV_BLKP_IMPL(bp)));

        //Move bp to start of left block, and change sizes in header and footer
        size += GET_SIZE(HDRP(PREV_BLKP_IMPL(bp)));
        PUT_WORD(FTRP(bp), PACK(size, 0));
        PUT_WORD(HDRP(PREV_BLKP_IMPL(bp)), PACK(size, 0));
        bp = PREV_BLKP_IMPL(bp); //This still works at this point because the left footer wasn't touched
    }

    else //case 4
    {
        //remove left and right free block from the correct free list
        remove_from_freelist(reinterpret_cast<BYTE*>(NEXT_BLKP_IMPL(bp)));
        remove_from_freelist(reinterpret_cast<BYTE*>(PREV_BLKP_IMPL(bp)));

        size += GET_SIZE(HDRP(NEXT_BLKP_IMPL(bp)))
                + GET_SIZE(HDRP(PREV_BLKP_IMPL(bp)));
        //Move bp to start of left block, and use footer of right block
        PUT_WORD(HDRP(PREV_BLKP_IMPL(bp)), PACK(size, 0));
        PUT_WORD(FTRP(NEXT_BLKP_IMPL(bp)), PACK(size, 0));
        bp = PREV_BLKP_IMPL(bp); //This still works at this point because the left footer wasn't touched
    }
    return bp;
}

/*
constexpr std::size_t MyAlloc::blocksize_to_freelist_idx(std::size_t asize) const
{
    unsigned int reduced_log_idx = dtools::ceillog2(asize /  MIN_BLOCK_SIZE);

    assert(reduced_log_idx <= MAX_BLOCK_ORDER);

    return reduced_log_idx;
}

constexpr std::size_t MyAlloc::freelist_idx_to_blocksize(std::size_t idx) const
{
    return MIN_BLOCK_SIZE << idx;
}
*/

/*!
 * \brief Remove block with block pointer bptr from the appropriate explicit free list for its size
 * \param idx
 * \param bptr
 */
void MyAlloc::remove_from_freelist(BYTE* bptr)
{   
    BYTE *currptr = find_previous_block(bptr);
    if(currptr != nullptr)
    {
        //make next of currptr point to next of its next instead
        BYTE *nextnextPtr = NEXT_BLKP(NEXT_BLKP(currptr));
        PUT_ADDRESS(HDRP(currptr) + HEADERSIZE, nextnextPtr); //address of potentially nonenxistant next block
        if(nextnextPtr != nullptr)
        {
            PUT_ADDRESS(HDRP(nextnextPtr) + HEADERSIZE + SIZE_OF_ADDRESS, currptr); //address of next next block
        }
        return;
    }
    //If bptr is the first block in the list (because no previous block was found), change list entry to next block
    m_free_lists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bptr)))) = NEXT_BLKP(bptr);
}

/*!
 * \brief returns the block in one of the free lists that points to bp. Returns 0 if bp is the first in the list, throws exception if block is not free
 * \param bp
 * \return
 */
BYTE* MyAlloc::find_previous_block(void *bp) const
{
    if(m_free_lists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bp)))) == bp)
    {
        return nullptr;
    }

    BYTE *prevptr = PREV_BLKP(bp);
    if(prevptr == nullptr)
    {
        throw std::runtime_error("Block to be found is not in free list");
    }
    return prevptr;
}

/*!
 * \brief Returns ptr to the start of the memory slab that the block pointed to by the input parameter is located inside.
 * Throws if the pointer is outside of any of the mapped slabs.
 * Behavior is undefined is the slab was previously removed from slab list - in which case calling this function must be a mistake.
 * \param blockpointer
 * \return
 */
void* MyAlloc::get_slab_for_block(void *blockpointer)
{
    //take the maximum slab ptr in the slab list that is smaller than bp. Assuming nothing went wrong earlier, this MUST be the correct slab.
    //Slab MUST NOT be removed from list before doing this otherwise behavior is undefined!
    if(m_slab_list.empty())
    {
        throw std::runtime_error("No slab was mapped. Function call must be erroneous as there can exist no valid blockpointer.");
    }
    BYTE *slab_candidate = nullptr;
    for(BYTE* &slabptr : m_slab_list)
    {
        //Assume that the slab list is filled starting from the front: all nullptrs are at the back! That means we are done!
        if(slabptr == nullptr)
        {
            break;
        }
        if(slabptr > slab_candidate && slabptr < blockpointer)
        {
            slab_candidate = slabptr;
        }
    }

    if(slab_candidate == nullptr)
    {
        throw std::runtime_error("Supplied block pointer does not lie in any mapped slab range and cannot be valid!");
    }

    return slab_candidate;
}

/*!
 * \brief Checks if the slab pointed to by the input parameter contains only free blocks. If so, removes all free blocks from the lists and unmaps the slab.
 * Behavior is undefined if slab_ptr does not point to the start of a slab!
 * \param slab_ptr
 */
void MyAlloc::unmap_slab_if_unused(void *slab_ptr)
{
    //Assume that a completely free slab MUST contain ONE SINGLE block of maximum size (after all, every free-call coalesces them!)
    //Then only check for blocksize of first block: if maximum, remove that block and unmap the slab
    BYTE *first_bp = reinterpret_cast<BYTE*>(slab_ptr) + LEFT_BOUNDARY_SIZE + HEADERSIZE;

    if(GET_SIZE(HDRP(first_bp)) == MAX_BLOCK_SIZE)
    {
        remove_from_freelist(first_bp);
        if(munmap(slab_ptr, SLAB_SIZE) != 0)
        {
            throw std::runtime_error("Munmap error!");
        }
        //remove slab_ptr from m_slab_list
        auto removedIt = std::remove(m_slab_list.begin(), m_slab_list.end(), slab_ptr);

        //There had to be a slab in the map to remove at this point, otherwise a wizard is at work
        assert(removedIt != m_slab_list.end());

        *removedIt = nullptr;
        --m_slab_list_top_idx;
    }
}
