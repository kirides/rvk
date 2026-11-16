// Private heaps (src/core/private_heap.*, proxy/new_override.cpp; frame-ownership.md 16.26). rvk_tests links the operator replacement: every new / delete of the suite goes through it.

#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "core/private_heap.h"
#include "test.h"

using namespace rvk;

TEST(private_heap_alloc_free_alignment) {
    for (std::size_t align : {16u, 32u, 64u, 256u, 4096u}) {
        for (std::size_t size : {0u, 1u, 15u, 16u, 100u, 5000u, 3u << 20}) {
            void* p = pheap::alloc(size, align);
            CHECK(p != nullptr);
            CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
            CHECK(pheap::owns(p));
            CHECK(pheap::usableSize(p) >= size);
            std::memset(p, 0xAB, size);
            pheap::release(p, align > 16);
        }
    }
}

TEST(private_heap_foreign_pointers_go_to_the_crt) {
    void* m = std::malloc(100);
    CHECK(!pheap::owns(m));
    pheap::release(m, false);  // free()
    void* a = _aligned_malloc(100, 64);
    CHECK(!pheap::owns(a));
    pheap::release(a, true);  // _aligned_free
    pheap::release(nullptr, false);
    CHECK(!pheap::owns(nullptr));
    int local = 0;
    CHECK(!pheap::owns(&local));
}

TEST(private_heap_operator_new_forms) {
    auto* p1 = new int(7);
    auto* p2 = new int[100];
    auto* p3 = new (std::nothrow) int[10];
    struct alignas(64) Big { char c[64]; };
    auto* p4 = new Big;
    auto* p5 = new Big[4];
    CHECK(pheap::owns(p1) && pheap::owns(p2) && pheap::owns(p3) && pheap::owns(p4) && pheap::owns(p5));
    CHECK(reinterpret_cast<std::uintptr_t>(p4) % 64 == 0);
    delete p1;
    delete[] p2;
    delete[] p3;
    delete p4;
    delete[] p5;
    std::string s(1000, 'x');
    CHECK(pheap::owns(s.data()));
    std::vector<std::string> v(50, std::string(100, 'y'));
    CHECK(pheap::owns(v.data()));
}

TEST(private_heap_cross_thread_free_and_shards) {
    std::vector<void*> blocks(2000);
    std::thread a([&] {
        for (auto& b : blocks) b = new char[48];
    });
    a.join();
    std::thread b([&] {
        for (auto* p : blocks) {
            CHECK(pheap::owns(p));
            delete[] static_cast<char*>(p);
        }
    });
    b.join();
    // concurrent churn on several threads
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([] {
            std::vector<std::vector<int>> keep;
            for (int i = 0; i < 20000; ++i) {
                keep.emplace_back(static_cast<std::size_t>(i % 200 + 1), i);
                if (keep.size() > 64) keep.erase(keep.begin());
            }
        });
    }
    for (auto& t : ts) t.join();
}
