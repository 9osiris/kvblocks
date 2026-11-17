#include "block.h"

#include <algorithm>

namespace kvblocks {

// fnv-1a, chained so a block hash covers the whole prefix before it
static const uint64_t kFnvOffset = 14695981039346656037ULL;
static const uint64_t kFnvPrime = 1099511628211ULL;

BlockManager::BlockManager(int num_blocks, int block_size)
    : num_blocks_(num_blocks), block_size_(block_size) {
    blocks_.resize(num_blocks_);
    for (int i = 0; i < num_blocks_; i++) {
        blocks_[i].id = i;
        blocks_[i].tokens.reserve(block_size_);
        free_list_.push_back(i);
    }
}

uint64_t BlockManager::hash_tokens(uint64_t prev, const std::vector<int>& tokens) const {
    uint64_t h = prev;
    for (int t : tokens) {
        // mix the token in 4 bytes at a time, same as fnv over the raw ints
        for (int b = 0; b < 4; b++) {
            h ^= (uint64_t)((t >> (b * 8)) & 0xff);
            h *= kFnvPrime;
        }
    }
    return h;
}

int BlockManager::new_sequence() {
    int id = next_seq_id_++;
    sequences_[id] = Sequence();
    return id;
}

void BlockManager::touch(Block& block) {
    block.last_used = ++clock_;
}

int BlockManager::allocate_block() {
    int id = -1;
    if (!free_list_.empty()) {
        // an empty or partial block costs nothing to reuse, take one
        // before evicting any cached prefix
        id = free_list_.front();
        free_list_.pop_front();
    } else if (!cached_list_.empty()) {
        // every free block holds a prefix, evict the least recently used
        id = cached_list_.front();
        cached_list_.pop_front();
    } else {
        return -1;
    }
    Block& block = blocks_[id];
    if (block.cached) {
        hash_to_block_.erase(block.hash);
        block.cached = false;
        block.hash = 0;
        evictions_++;
    }
    block.tokens.clear();
    block.filled = 0;
    block.ref_count = 1;
    allocations_++;
    return id;
}

void BlockManager::drop_from_cached_list(int block_id) {
    auto it = std::find(cached_list_.begin(), cached_list_.end(), block_id);
    if (it != cached_list_.end()) {
        cached_list_.erase(it);
    }
}

void BlockManager::release_block(int block_id) {
    Block& block = blocks_[block_id];
    if (block.filled == block_size_) {
        // full blocks stay hashed so a later prefix can find them
        block.cached = true;
        hash_to_block_[block.hash] = block_id;
    } else {
        block.tokens.clear();
        block.filled = 0;
        block.hash = 0;
    }
    touch(block);
    // clock only grows, so the back stays the most recently used
    if (block.cached) {
        cached_list_.push_back(block_id);
    } else {
        free_list_.push_back(block_id);
    }
}

bool BlockManager::append(int seq_id, int token) {
    auto it = sequences_.find(seq_id);
    if (it == sequences_.end()) {
        return false;
    }
    Sequence& seq = it->second;

    int last = -1;
    if (!seq.blocks.empty()) {
        last = seq.blocks.back();
    }
    if (last == -1 || blocks_[last].filled == block_size_) {
        int fresh = allocate_block();
        if (fresh == -1) {
            return false;
        }
        seq.blocks.push_back(fresh);
        last = fresh;
    } else if (blocks_[last].ref_count > 1) {
        // copy on write, the other sequences keep the shared block
        int fresh = allocate_block();
        if (fresh == -1) {
            return false;
        }
        Block& old_block = blocks_[last];
        Block& new_block = blocks_[fresh];
        new_block.tokens = old_block.tokens;
        new_block.filled = old_block.filled;
        old_block.ref_count--;
        if (old_block.ref_count == 0) {
            release_block(last);
        }
        seq.blocks.back() = fresh;
        last = fresh;
    }

    Block& block = blocks_[last];
    block.tokens.push_back(token);
    block.filled++;
    if (block.filled == block_size_) {
        uint64_t prev = kFnvOffset;
        if (seq.blocks.size() > 1) {
            prev = blocks_[seq.blocks[seq.blocks.size() - 2]].hash;
        }
        block.hash = hash_tokens(prev, block.tokens);
    }
    return true;
}

int BlockManager::fork(int seq_id) {
    auto it = sequences_.find(seq_id);
    if (it == sequences_.end()) {
        return -1;
    }
    int id = next_seq_id_++;
    Sequence copy = it->second;
    for (int block_id : copy.blocks) {
        blocks_[block_id].ref_count++;
    }
    sequences_[id] = copy;
    return id;
}

bool BlockManager::free_sequence(int seq_id) {
    auto it = sequences_.find(seq_id);
    if (it == sequences_.end()) {
        return false;
    }
    for (int block_id : it->second.blocks) {
        Block& block = blocks_[block_id];
        block.ref_count--;
        if (block.ref_count == 0) {
            release_block(block_id);
        }
    }
    sequences_.erase(it);
    return true;
}

int BlockManager::attach_prefix(int seq_id, const std::vector<int>& tokens) {
    auto it = sequences_.find(seq_id);
    if (it == sequences_.end() || !it->second.blocks.empty()) {
        return -1;
    }
    Sequence& seq = it->second;
    uint64_t prev = kFnvOffset;
    int reused = 0;
    size_t pos = 0;
    while (pos + (size_t)block_size_ <= tokens.size()) {
        std::vector<int> chunk(tokens.begin() + pos, tokens.begin() + pos + block_size_);
        uint64_t h = hash_tokens(prev, chunk);
        auto hit = hash_to_block_.find(h);
        if (hit == hash_to_block_.end()) {
            prefix_misses_++;
            break;
        }
        int block_id = hit->second;
        Block& block = blocks_[block_id];
        if (block.ref_count != 0 || !block.cached) {
            prefix_misses_++;
            break;
        }
        hash_to_block_.erase(hit);
        block.cached = false;
        drop_from_cached_list(block_id);
        touch(block);
        block.ref_count = 1;
        seq.blocks.push_back(block_id);
        prev = h;
        pos += block_size_;
        reused++;
        prefix_hits_++;
    }
    return reused;
}

std::vector<int> BlockManager::sequence_tokens(int seq_id) const {
    std::vector<int> out;
    auto it = sequences_.find(seq_id);
    if (it == sequences_.end()) {
        return out;
    }
    for (int block_id : it->second.blocks) {
        const Block& block = blocks_[block_id];
        out.insert(out.end(), block.tokens.begin(), block.tokens.end());
    }
    return out;
}

int BlockManager::sequence_len(int seq_id) const {
    return (int)sequence_tokens(seq_id).size();
}

int BlockManager::sequence_blocks(int seq_id) const {
    auto it = sequences_.find(seq_id);
    if (it == sequences_.end()) {
        return -1;
    }
    return (int)it->second.blocks.size();
}

Stats BlockManager::stats() const {
    Stats s;
    s.total_blocks = num_blocks_;
    s.free_blocks = (int)(free_list_.size() + cached_list_.size());
    s.cached_blocks = (int)hash_to_block_.size();
    for (const Block& block : blocks_) {
        if (block.ref_count > 0) {
            s.used_blocks++;
        }
    }
    s.allocations = allocations_;
    s.evictions = evictions_;
    s.prefix_hits = prefix_hits_;
    s.prefix_misses = prefix_misses_;
    bool any_cached = false;
    uint64_t oldest = 0;
    for (const Block& block : blocks_) {
        if (block.cached && (!any_cached || block.last_used < oldest)) {
            any_cached = true;
            oldest = block.last_used;
        }
    }
    s.oldest_cached_age = any_cached ? (long)(clock_ - oldest) : 0;
    return s;
}

} // namespace kvblocks
