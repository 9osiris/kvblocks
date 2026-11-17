#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace kvblocks {

// one fixed-size chunk of cached key/value tokens
struct Block {
    int id = -1;
    int ref_count = 0;
    int filled = 0;
    uint64_t hash = 0;
    bool cached = false;
    // tick of the last time this block was touched, drives lru eviction
    uint64_t last_used = 0;
    std::vector<int> tokens;
};

// one sequence holds an ordered list of block ids
struct Sequence {
    std::vector<int> blocks;
};

// running counters for the manager
struct Stats {
    int total_blocks = 0;
    int used_blocks = 0;
    int free_blocks = 0;
    int cached_blocks = 0;
    long allocations = 0;
    long evictions = 0;
    long prefix_hits = 0;
    long prefix_misses = 0;
    // ticks since the oldest cached block was last touched, 0 if none cached
    long oldest_cached_age = 0;
};

// paged kv cache, vllm style. blocks are fixed-size, sequences share
// them by refcount, full blocks hang around on a free list so a new
// sequence with the same prefix can reuse them instead of recomputing.
class BlockManager {
public:
    BlockManager(int num_blocks, int block_size);

    int num_blocks() const { return num_blocks_; }
    int block_size() const { return block_size_; }

    // start a new empty sequence, returns its id
    int new_sequence();

    // append one token, allocates a new block when the last one is full.
    // false when out of blocks or the sequence does not exist.
    bool append(int seq_id, int token);

    // fork a sequence. both sides share blocks until one of them writes,
    // then the writer gets its own copy. returns the new id, -1 if unknown.
    int fork(int seq_id);

    // drop a sequence, its blocks go back on the free list. false if unknown.
    bool free_sequence(int seq_id);

    // reuse cached full blocks for a matching prefix. only works on an
    // empty sequence. returns how many blocks were reused, -1 if unknown
    // or the sequence already has blocks.
    int attach_prefix(int seq_id, const std::vector<int>& tokens);

    // tokens in a sequence, in order. empty when unknown.
    std::vector<int> sequence_tokens(int seq_id) const;
    int sequence_len(int seq_id) const;
    int sequence_blocks(int seq_id) const;

    Stats stats() const;

private:
    int num_blocks_;
    int block_size_;
    int next_seq_id_ = 1;
    long allocations_ = 0;
    long evictions_ = 0;
    long prefix_hits_ = 0;
    long prefix_misses_ = 0;
    // monotonic tick, bumped on every touch so tests stay deterministic
    uint64_t clock_ = 0;
    std::vector<Block> blocks_;
    // free blocks split in two: uncached ones cost nothing to reuse,
    // cached ones are ordered by last_used so the front is the lru victim
    std::deque<int> free_list_;
    std::deque<int> cached_list_;
    std::unordered_map<uint64_t, int> hash_to_block_;
    std::unordered_map<int, Sequence> sequences_;

    uint64_t hash_tokens(uint64_t prev, const std::vector<int>& tokens) const;
    void touch(Block& block);
    int allocate_block();
    void release_block(int block_id);
    void drop_from_cached_list(int block_id);
};

} // namespace kvblocks
