#include <iostream>
#include <vector>
#include "myalloc.h"

using namespace std;

int main()
{

    void *mem_to_fill = mm_malloc(sizeof(int));
    *(int*)mem_to_fill = 5;
    std::cout << *(int*)mem_to_fill;

    mm_free(mem_to_fill);


    constexpr std::size_t NUM_OF_ALLOCS_TO_TEST = 1000000;
    constexpr int MAX_MALLOC_SIZE = 21440;


    //Test MyAlloc
    std::vector<void*> ptrs{NUM_OF_ALLOCS_TO_TEST, 0};
    for(std::size_t i = 0; i < NUM_OF_ALLOCS_TO_TEST; ++i)
    {
        //std::size_t blocksize = rand() % MAX_MALLOC_SIZE + 1;
        std::size_t blocksize = MAX_MALLOC_SIZE;
        void *currAlloc = mm_malloc(blocksize);
        assert(currAlloc != nullptr);



        //Write the allocated pages full
        for(std::size_t j = 0; j < blocksize/sizeof(WORD); j++)
        {
            //*(reinterpret_cast<WORD*> (currAlloc) + j) = rand();
        }


        ptrs.at(i) = currAlloc;
    }

    for(auto &ptr : ptrs)
    {
        mm_free(ptr);
    }
}
