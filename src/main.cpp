#include <iostream>
#include <vector>
#include <chrono>
#include "myalloc.h"
#include "DTools/MiscTools.h"

using namespace std;

int main()
{
    constexpr std::size_t NUM_OF_ALLOCS_TO_TEST = 150000;
    constexpr int MAX_MALLOC_SIZE = 61440;
    constexpr int NUM_OF_TESTS = 1000;


    //Test MyAlloc
    //Don't test for external fragmentation, too much work


    //Test for running time and for Internal fragmentation after allocating a large amount of blocks with random sizes
    //Do this test 100 times, take average vales
    //Do the same for malloc and compare
    std::chrono::duration<double> total_seconds_myalloc{0};
    std::chrono::duration<double> total_seconds_myfree{0};

    double vm_usage_myalloc{0};
    double resident_set_myalloc{0};

    std::chrono::duration<double> total_seconds_malloc{0};
    std::chrono::duration<double> total_seconds_free{0};

    double vm_usage_malloc{0};
    double resident_set_malloc{0};

    for(int i = 0; i < NUM_OF_TESTS; ++i)
    {
        std::vector<void*> ptrs{NUM_OF_ALLOCS_TO_TEST, 0};
        const auto startAlloc{std::chrono::steady_clock::now()};
        for(std::size_t i = 0; i < NUM_OF_ALLOCS_TO_TEST; ++i)
        {
            std::size_t blocksize = rand() % MAX_MALLOC_SIZE + 1;
            void *currAlloc = mm_malloc(blocksize);
            assert(currAlloc != nullptr);

            ptrs.at(i) = currAlloc;
        }

        const auto endAlloc{std::chrono::steady_clock::now()};

        dtools::process_mem_usage(vm_usage_myalloc, resident_set_myalloc);

        const auto startFree{std::chrono::steady_clock::now()};
        for(auto &ptr : ptrs)
        {
            mm_free(ptr);
        }

        const auto endFree{std::chrono::steady_clock::now()};

        const std::chrono::duration<double> elapsed_seconds_alloc{endAlloc - startAlloc};
        total_seconds_myalloc += elapsed_seconds_alloc;

        const std::chrono::duration<double> elapsed_seconds_free{endFree - startFree};
        total_seconds_myfree += elapsed_seconds_free;
    }

    //test malloc
    for(int i = 0; i < NUM_OF_TESTS; ++i)
    {
        std::vector<void*> ptrs{NUM_OF_ALLOCS_TO_TEST, 0};
        const auto startAlloc{std::chrono::steady_clock::now()};
        for(std::size_t i = 0; i < NUM_OF_ALLOCS_TO_TEST; ++i)
        {
            std::size_t blocksize = rand() % MAX_MALLOC_SIZE + 1;
            void *currAlloc = malloc(blocksize);
            assert(currAlloc != nullptr);

            ptrs.at(i) = currAlloc;
        }

        const auto endAlloc{std::chrono::steady_clock::now()};

        dtools::process_mem_usage(vm_usage_malloc, resident_set_malloc);


        const auto startFree{std::chrono::steady_clock::now()};
        for(auto &ptr : ptrs)
        {
            free(ptr);
        }

        const auto endFree{std::chrono::steady_clock::now()};

        const std::chrono::duration<double> elapsed_seconds_alloc{endAlloc - startAlloc};
        total_seconds_malloc += elapsed_seconds_alloc;

        const std::chrono::duration<double> elapsed_seconds_free{endFree - startFree};
        total_seconds_free += elapsed_seconds_free;
    }

    std::cout << "Memory usage at full allocation(resident set): " << "MyAlloc: " << resident_set_myalloc << "\n";
    std::cout << "Memory usage at full allocation(virtual memory): " << "MyAlloc: " << vm_usage_myalloc << "\n";
    std::cout << "Memory usage at full allocation(resident set): " << "malloc: " << resident_set_malloc << "\n";
    std::cout << "Memory usage at full allocation(virtual memory): " << "mAlloc: " << vm_usage_malloc << "\n";
    std::cout << "Average alloc time:\n" << "MyAlloc: " << total_seconds_myalloc.count() << ", malloc: " << total_seconds_malloc.count() << "\n";
    std::cout << "Average free time:\n" << "MyAlloc: " << total_seconds_myfree.count() << ", malloc: " << total_seconds_free.count() << "\n";
}
