#include "../src/block.h"

#include <cstdio>
#include <vector>

using namespace kvblocks;

static int passed = 0;
static int failed = 0;

static void check(bool cond, const char* name) {
    if (cond) {
        passed++;
    } else {
        failed++;
        printf("FAIL: %s\n", name);
    }
}

static void fill(BlockManager& m, int seq, std::vector<int> toks) {
    for (int t : toks) {
        check(m.append(seq, t), "append in fill");
    }
}

int main() {
    // fresh manager state
    {
        BlockManager m(8, 4);
        Stats s = m.stats();
        check(s.total_blocks == 8, "total blocks is 8");
        check(s.free_blocks == 8, "all blocks free at start");
        check(s.used_blocks == 0, "no blocks used at start");
        check(s.cached_blocks == 0, "nothing cached at start");
    }

    // append across a block boundary
    {
        BlockManager m(8, 4);
        int s = m.new_sequence();
        fill(m, s, {1, 2, 3, 4, 5});
        check(m.sequence_len(s) == 5, "len is 5 after 5 appends");
        check(m.sequence_blocks(s) == 2, "5 tokens span 2 blocks");
        check(m.sequence_tokens(s) == std::vector<int>({1, 2, 3, 4, 5}),
              "tokens come back in order");
        check(m.stats().used_blocks == 2, "2 blocks used");
    }

    // freeing returns blocks
    {
        BlockManager m(8, 4);
        int s = m.new_sequence();
        fill(m, s, {1, 2, 3, 4, 5, 6, 7, 8, 9});
        check(m.stats().used_blocks == 3, "3 blocks used before free");
        check(m.free_sequence(s), "free returns true");
        check(m.stats().used_blocks == 0, "0 blocks used after free");
        check(m.stats().free_blocks == 8, "all blocks free again");
        check(!m.free_sequence(s), "double free returns false");
        check(!m.free_sequence(999), "freeing unknown seq returns false");
    }

    // partial blocks are not cached, full ones are
    {
        BlockManager m(8, 4);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3}); // partial only
        m.free_sequence(a);
        check(m.stats().cached_blocks == 0, "partial block is not cached");
        int b = m.new_sequence();
        fill(m, b, {1, 2, 3, 4, 5}); // one full, one partial
        m.free_sequence(b);
        check(m.stats().cached_blocks == 1, "one full block cached");
    }

    // fork shares blocks, copy on write keeps the original intact
    {
        BlockManager m(8, 4);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4, 5});
        int c = m.fork(a);
        check(c != -1, "fork returns a new id");
        check(m.sequence_tokens(c) == m.sequence_tokens(a), "fork starts identical");
        check(m.stats().used_blocks == 2, "fork shares blocks, no new ones");
        check(m.append(c, 6), "append to fork works");
        check(m.sequence_tokens(a) == std::vector<int>({1, 2, 3, 4, 5}),
              "original unchanged after fork append");
        check(m.sequence_tokens(c) == std::vector<int>({1, 2, 3, 4, 5, 6}),
              "fork has the new token");
        check(m.stats().used_blocks == 3, "copy on write added one block");
        check(m.fork(999) == -1, "forking unknown seq returns -1");
    }

    // prefix cache hit and miss
    {
        BlockManager m(8, 4);
        int a = m.new_sequence();
        fill(m, a, {10, 20, 30, 40, 50, 60, 70, 80});
        m.free_sequence(a);
        check(m.stats().cached_blocks == 2, "two full blocks cached");

        int b = m.new_sequence();
        int reused = m.attach_prefix(b, {10, 20, 30, 40, 50, 60, 70, 80});
        check(reused == 2, "prefix reuses both blocks");
        check(m.sequence_tokens(b) == std::vector<int>({10, 20, 30, 40, 50, 60, 70, 80}),
              "reused prefix tokens match");
        check(m.stats().cached_blocks == 0, "cache empty after reuse");
        check(m.stats().prefix_hits == 2, "two prefix hits counted");

        int d = m.new_sequence();
        int missed = m.attach_prefix(d, {1, 2, 3, 4, 5, 6, 7, 8});
        check(missed == 0, "different prefix reuses nothing");
        check(m.stats().prefix_misses == 1, "one prefix miss counted");

        check(m.attach_prefix(b, {10, 20, 30, 40}) == -1,
              "attach on non-empty seq returns -1");
        check(m.attach_prefix(999, {10, 20, 30, 40}) == -1,
              "attach on unknown seq returns -1");
    }

    // a partial prefix only reuses the full blocks
    {
        BlockManager m(8, 4);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4, 5, 6, 7, 8});
        m.free_sequence(a);
        int b = m.new_sequence();
        int reused = m.attach_prefix(b, {1, 2, 3, 4, 5, 6});
        check(reused == 1, "partial prefix reuses only the full block");
        check(m.sequence_len(b) == 4, "only the full block lands");
    }

    // eviction reclaims the oldest cached block first
    {
        BlockManager m(4, 2);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4}); // two full blocks, both cached on free
        m.free_sequence(a);
        int b = m.new_sequence();
        fill(m, b, {5, 6, 7, 8}); // uses the two never-touched blocks
        m.free_sequence(b);
        check(m.stats().cached_blocks == 4, "all four blocks cached");
        int c = m.new_sequence();
        fill(m, c, {9, 10, 11, 12}); // evicts the oldest cached pair
        check(m.stats().cached_blocks == 2, "oldest cached blocks evicted");
        check(m.stats().evictions == 2, "two evictions counted");
        int d = m.new_sequence();
        int gone = m.attach_prefix(d, {1, 2, 3, 4});
        check(gone == 0, "evicted prefix is gone");
        int e = m.new_sequence();
        int kept = m.attach_prefix(e, {5, 6, 7, 8});
        check(kept == 2, "newer cached prefix survives");
    }

    // out of memory fails cleanly instead of crashing
    {
        BlockManager m(2, 2);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4});
        check(!m.append(a, 5), "append fails when out of blocks");
        check(m.sequence_len(a) == 4, "failed append adds nothing");
        check(!m.append(999, 1), "append to unknown seq returns false");
        check(m.sequence_len(999) == 0, "unknown seq len is 0");
        check(m.sequence_blocks(999) == -1, "unknown seq blocks is -1");
    }

    // allocation takes an empty block before evicting a cached prefix
    {
        BlockManager m(4, 2);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4}); // two full blocks, cached on free
        m.free_sequence(a);
        check(m.stats().cached_blocks == 2, "two blocks cached");

        int b = m.new_sequence();
        fill(m, b, {9}); // one token, partial block
        m.free_sequence(b); // partial block comes back uncached
        check(m.stats().cached_blocks == 2, "partial free adds no cache");

        int c = m.new_sequence();
        check(m.append(c, 7), "append works");
        check(m.stats().evictions == 0, "no eviction while an empty block is free");
        int d = m.new_sequence();
        int reused = m.attach_prefix(d, {1, 2, 3, 4});
        check(reused == 2, "cached prefix survives the allocation");
    }

    // reusing a prefix refreshes its recency, so it outlives older ones
    {
        BlockManager m(4, 2);
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4});
        m.free_sequence(a);
        int b = m.new_sequence();
        fill(m, b, {5, 6, 7, 8});
        m.free_sequence(b);
        check(m.stats().cached_blocks == 4, "all four blocks cached");

        int c = m.new_sequence();
        check(m.attach_prefix(c, {1, 2, 3, 4}) == 2, "first prefix reuses two");
        m.free_sequence(c); // its blocks go back as the newest cached pair

        int d = m.new_sequence();
        fill(m, d, {9, 10, 11, 12}); // evicts the oldest cached pair
        check(m.stats().evictions == 2, "two evictions counted");
        int e = m.new_sequence();
        check(m.attach_prefix(e, {1, 2, 3, 4}) == 2, "reused prefix survives");
        int f = m.new_sequence();
        check(m.attach_prefix(f, {5, 6, 7, 8}) == 0, "older prefix is gone");
    }

    // stats report the age of the oldest cached block in ticks
    {
        BlockManager m(4, 2);
        check(m.stats().oldest_cached_age == 0, "no cached age when empty");
        int a = m.new_sequence();
        fill(m, a, {1, 2, 3, 4});
        m.free_sequence(a);
        check(m.stats().oldest_cached_age == 1, "oldest cached block is 1 tick old");
        int b = m.new_sequence();
        fill(m, b, {5, 6, 7, 8});
        m.free_sequence(b);
        check(m.stats().oldest_cached_age == 3, "oldest cached age grows");
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
