#include "myalloc.h"
#include <stdexcept>


void* mm_malloc(std::size_t size)
{
    return MyAlloc::get_object()->malloc(size);
}

void mm_free(void *ptr)
{
    MyAlloc::get_object()->free(ptr);
}


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
    mem_init();
    return mm_request(MAX_BLOCK_SIZE);
}

int MyAlloc::mm_request(std::size_t request_size)
{
    BYTE *new_mem_ptr = reinterpret_cast<BYTE*>(mem_map_slab(request_size));
    //TODO: for now, just allocates one large block of max blocksize. Later, this should be replaced by mmap calls that do the same thing each time we need a new block.
    if(new_mem_ptr == (BYTE*) - 1)
        return -1;


    constexpr std::size_t ADMIN_OVERHEAD_SIZE = WSIZE + OVERHEAD_SIZE + HEADERSIZE; //size of padding, left boundary and right boundary together
    constexpr std::size_t LEFT_BOUNDARY_SIZE = OVERHEAD_SIZE + WSIZE;

    //requested slab size does not make any sense; it's too small
    if(request_size <= ADMIN_OVERHEAD_SIZE)
    {
        return -1;
    }

    //put boundary blocks left and right of free space
    PUT_WORD(new_mem_ptr, 0); //Alignment padding for header,footer,and epilogue blocks ----- This assumes that header and footer are 1 WORD in size!
    PUT_WORD(new_mem_ptr + WSIZE, PACK(OVERHEAD_SIZE, 1)); //left boundary header
    PUT_ADDRESS(new_mem_ptr + WSIZE + HEADERSIZE, nullptr); //Address of nonexistant next block for prologue block TODO: check that PUT actually can put a whole DWORD/address here!
    PUT_WORD(new_mem_ptr + WSIZE + HEADERSIZE + SIZE_OF_ADDRESS, PACK(OVERHEAD_SIZE, 1)); //left boundary footer
    PUT_WORD(new_mem_ptr + request_size - HEADERSIZE, PACK(0,1)); //Epilogue header


    std::size_t remaining_free_block_size = request_size - ADMIN_OVERHEAD_SIZE;

    BYTE* prevtop = mFreelists.at(blocksize_to_freelist_idx(remaining_free_block_size));

    //Create one large free block out of the rest of the memory. so starting from Fourth block to the second-to-last block
    PUT_WORD(new_mem_ptr + LEFT_BOUNDARY_SIZE, PACK(remaining_free_block_size, 0)); //block header
    PUT_ADDRESS(new_mem_ptr + LEFT_BOUNDARY_SIZE + HEADERSIZE, prevtop); //address of nonexistant next block
    PUT_WORD(new_mem_ptr + request_size - HEADERSIZE - FOOTERSIZE, PACK(remaining_free_block_size, 0)); //block footer

    //insert the aforementioned large free block into the largest size class of the free list array
    BYTE* newtop = new_mem_ptr + LEFT_BOUNDARY_SIZE + HEADERSIZE; //Block address of large free block (NOT HEADER ADDRESS!!)

    assert(HDRP(newtop) == new_mem_ptr + LEFT_BOUNDARY_SIZE);
    assert (!GET_ALLOC(HDRP(newtop)));

    mFreelists.at(blocksize_to_freelist_idx(remaining_free_block_size)) = newtop;

    return 0;
}


/*!
 * \brief Maps memory for a slab of he size required by incr. Currently just uses sbrk. Is supposed to use mmap and save the mapped region pointers for unmapping later.
 * \param incr
 * \return
 */
void* MyAlloc::mem_map_slab(std::size_t incr)
{
    //TODO: Change to mmap
    return mem_sbrk(incr);
}


/*Implements a first-fit search of the explicit free lists. Returns the Block pointer to a free block if found (NOT THE HEADER POINTER!)*/
void *MyAlloc::find_fit(std::size_t asize)
{
    //return immediately if asize is larger than largest possible block size (including overhead and alignment reqs)
    if(asize >= MAX_BLOCK_SIZE)
        return nullptr;

   //Idea: go through all free lists: then check every header until one has the required size
   //if found: return pointer to the data block of the block with that header
    BYTE *ret = nullptr;

    //go through freelists and check:
    //1. if the size class of that list is large enough
    for(std::size_t i = 0; i < mFreelists.size(); ++i)
    {
        if(freelist_idx_to_blocksize(i) >= asize) //Is size class big enough to fit asize?
        {
            //2. go through that list if the size class and see if a fit can be found in that specific explicit free list
            ret = find_fit_in_list(mFreelists.at(i), asize);
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

/*place requested block of given size at beginning of free block bp (which is known to have enough size to fit a block (including overhead) of asize)
 * asize is the TOTAL size of the block with overhead
 * Split block bp if remainder would be equal or larger than minimum block size
 * */
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
        BYTE* prevtop_b = mFreelists.at(blocksize_to_freelist_idx(bsize));
        PUT_WORD(HDRP(splitblockp), PACK(bsize,0)); //Header of split block
        PUT_ADDRESS(HDRP(splitblockp) + HEADERSIZE, prevtop_b); //next Address block of split block
        PUT_WORD(HDRP(splitblockp) + HEADERSIZE + SIZE_OF_ADDRESS, PACK(bsize, 0)); //Footer of split block


        assert(NEXT_BLKP_IMPL(bp) ==  splitblockp);

        //insert new free split-block into correct explicit free list
        mFreelists.at(blocksize_to_freelist_idx(bsize)) = reinterpret_cast<BYTE*> (splitblockp);
    }
    else
    {
        PUT_WORD(reinterpret_cast<WORD*>(HDRP(bp)), PACK(non_split_size,1));
        PUT_WORD(FTRP(bp), PACK(non_split_size, 1));
    }
}

/*!
 * \brief MyAlloc::malloc
 * \param size
 * \return
 */
void* MyAlloc::malloc(std::size_t size)
{
    //TODO: find fit (using find_fit, duh) and create block out of found block (split beforehand inside place function)
    //Obviously, also remove the block from the free list after allocating it
    //If no fit can be found, allocate more memory using the memlib and put that new block on the free list (essentially the same as mm_init)

    /* Ignore spurious requests */
    if(size == 0 || size > MAX_BLOCK_SIZE)
        return nullptr;

    BYTE *retp = nullptr;

    /* Adjust block size to include overhead and DWORD alignment requirements */
    //TODO: Do I need to check for case of size < DSIZE specifically?
    std::size_t asize;
    //asize = DSIZE * ((OVERHEAD_SIZE + size + (DSIZE-1)) / DSIZE);
    asize = align_size_to_DWORD(OVERHEAD_SIZE + size);


    /*Search the free lists for a fit */
    retp = reinterpret_cast<BYTE*>(find_fit(asize));
    if(retp != nullptr)
    {
        place(retp, asize);
        return retp;
    }

    //handle getting more memory in case no fit was found
    if(mm_request(MAX_BLOCK_SIZE) == -1)
    {
        return nullptr;
    }
    //try again if memory could be requested
    return malloc(size);
}

/*!
 * \brief free block and coalesce as far as possible by checking repeatedly to the left and right for free blocks: then place in appropiate size class in free list
 * \param ptr
 */
void MyAlloc::free(void *bp)
{
    //TODO: Add some way to check if the slab in which the currently freed block exists, can be unmapped

    //set header and footer to size of block and alloc bit set to 0:
    std::size_t size = GET_SIZE(HDRP(bp));
    PUT_WORD(HDRP(bp), PACK(size, 0));
    PUT_WORD(FTRP(bp), PACK(size, 0));
    //Coalesce as far as possible
    bp = coalesce(bp);

    //TODO: remove this, it's just for findign a bug
    assert((std::size_t)bp > (std::size_t)0x70000000000);

    //insert newly-freed block into correct explicit free list, and insert correct address block into freed block
    BYTE* prevtop = mFreelists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bp))));
    PUT_ADDRESS(HDRP(bp) + HEADERSIZE, prevtop); //address of potentially nonenxistant next block
    mFreelists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bp)))) = reinterpret_cast<BYTE*>(bp);
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
    WORD prev_alloc = GET_ALLOC(FTRP(PREV_BLKP_IMPL(bp)));
    WORD next_alloc = GET_ALLOC(FTRP(NEXT_BLKP_IMPL(bp)));
    std::size_t size = GET_SIZE(HDRP(bp));

    if(prev_alloc && next_alloc) //case 1, no coalescing possible
    {
        return bp;
    }

    else if (prev_alloc && !next_alloc) //case 2, right block coalesced
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

/*!
 * Gives index in mFreelists array for the size group that asize fits into
 * \param asize
 * \return
 */
constexpr std::size_t MyAlloc::blocksize_to_freelist_idx(std::size_t asize) const
{
    unsigned int reduced_log_idx = ceillog2(asize /  MIN_BLOCK_SIZE);

    assert(reduced_log_idx <= MAX_BLOCK_ORDER);

    return reduced_log_idx;
}

constexpr std::size_t MyAlloc::freelist_idx_to_blocksize(std::size_t idx) const
{
        return MIN_BLOCK_SIZE << idx;
}

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
        PUT_ADDRESS(HDRP(currptr) + HEADERSIZE, NEXT_BLKP(NEXT_BLKP(currptr))); //address of potentially nonenxistant next block
        return;
    }
    //If bptr is the first block in the list (because no previous block was found), change list entry to next block
    BYTE *newtop = NEXT_BLKP(bptr);

    std::size_t idx = blocksize_to_freelist_idx(GET_SIZE(HDRP(bptr)));
    mFreelists.at(idx) = newtop;
}

/*!
 * \brief returns the block in one of the free lists that points to bp. Returns 0 if bp is the first in the list, throws exception if block is not free
 * \param bp
 * \return
 */
BYTE* MyAlloc::find_previous_block(void *bp)
{
    BYTE *currptr = mFreelists.at(blocksize_to_freelist_idx(GET_SIZE(HDRP(bp))));
    if(currptr == bp)
    {
        return nullptr;
    }
    while(currptr != nullptr && NEXT_BLKP(currptr) != nullptr)
    {
        if(NEXT_BLKP(currptr) == bp)
        {
            return currptr;
        }
        currptr = NEXT_BLKP(currptr);
    }
    throw std::runtime_error("Block to be found is not in free list");
}
