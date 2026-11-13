#include "block.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace kvblocks;

static void print_stats(const BlockManager& m) {
    Stats s = m.stats();
    printf("blocks: %d total, %d used, %d free, %d cached\n",
           s.total_blocks, s.used_blocks, s.free_blocks, s.cached_blocks);
    printf("allocations: %ld, evictions: %ld, prefix hits: %ld, misses: %ld\n",
           s.allocations, s.evictions, s.prefix_hits, s.prefix_misses);
}

static void print_seq(const BlockManager& m, int seq, const char* name) {
    std::vector<int> toks = m.sequence_tokens(seq);
    printf("%s: %d tokens in %d blocks:", name, (int)toks.size(), m.sequence_blocks(seq));
    for (int t : toks) {
        printf(" %d", t);
    }
    printf("\n");
}

// two sequences share a system prompt, one forks, one reuses the prefix
static int demo() {
    BlockManager m(8, 4);
    printf("kvblocks demo: 8 blocks, 4 tokens each\n\n");

    int a = m.new_sequence();
    int prompt[] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int t : prompt) {
        m.append(a, t);
    }
    m.append(a, 9);
    m.append(a, 10);
    print_seq(m, a, "seq a (prompt + 9 10)");
    print_stats(m);

    // freeing a parks its two full blocks in the prefix cache
    m.free_sequence(a);
    printf("\nseq a freed, its full blocks are cached now:\n");
    print_stats(m);

    // same prompt again, the first two blocks come straight from cache
    int b = m.new_sequence();
    int reused = m.attach_prefix(b, {1, 2, 3, 4, 5, 6, 7, 8});
    printf("\nseq b attached %d cached prefix blocks\n", reused);
    m.append(b, 42);
    m.append(b, 43);
    print_seq(m, b, "seq b (same prompt + 42 43)");
    print_stats(m);

    // fork b, writing to the fork must not touch the original
    int c = m.fork(b);
    m.append(c, 99);
    printf("\n");
    print_seq(m, b, "seq b after fork");
    print_seq(m, c, "seq c (forked, + 99)");
    print_stats(m);

    m.free_sequence(b);
    m.free_sequence(c);
    printf("\nafter freeing everything:\n");
    print_stats(m);
    return 0;
}

// hammer the allocator: new seq, fill it, free it, repeat
static int bench(int rounds) {
    BlockManager m(256, 16);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; i++) {
        int s = m.new_sequence();
        for (int t = 0; t < 64; t++) {
            if (!m.append(s, t)) {
                printf("bench ran out of blocks on round %d\n", i);
                return 1;
            }
        }
        m.free_sequence(s);
    }
    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    printf("bench: %d sequences x 64 tokens in %.1f ms (%.0f seq/s)\n",
           rounds, ms, rounds / (ms / 1000.0));
    print_stats(m);
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "demo";
    if (cmd == "demo") {
        return demo();
    }
    if (cmd == "bench") {
        int rounds = argc > 2 ? std::stoi(argv[2]) : 10000;
        return bench(rounds);
    }
    printf("usage: kvblocks [demo|bench [rounds]]\n");
    return 2;
}
