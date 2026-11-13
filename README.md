kvblocks - paged kv cache blocks for llm inference [educational project]

disclaimer:
this is a toy, not a real inference engine. it manages blocks of
integers that stand in for tokens, there is no model and no gpu here.
i built it to understand how paged attention works, not to serve
anything. dont use it for real inference.

why i made this:
i kept reading about vllm and paged attention and the block manager
was the part i couldnt picture. every blog post says "blocks are
shared with refcounts and evicted lru" and then moves on. so i wrote
one. fixed-size blocks, sequences that hold block ids, copy on write
when a forked sequence writes to a shared block, and a prefix cache
so a second sequence with the same prompt reuses the blocks instead
of pretending to recompute them. i avoided pulling in a real
tokenizer or a model on purpose, tokens are just ints here, the
point is the allocator, not the model. c++ because the real ones are
c++ and i wanted to feel the ownership stuff instead of hiding
behind python dicts.

the guide:
build it:
  make

run the demo:
  ./kvblocks demo

run the bench:
  ./kvblocks bench 10000

run the tests:
  make test

the demo makes two sequences share a system prompt, forks one, and
shows the prefix cache reusing blocks. the manager itself is in
src/block.h and src/block.cpp, the cli is src/main.cpp.

what it does:
- fixed-size blocks, you pick how many blocks and how many tokens
  each one holds
- append allocates a new block when the last one is full, returns
  false instead of crashing when it runs out
- fork shares blocks by refcount, writing to a shared block copies
  it first so the original never changes
- full blocks stay on a free list after their sequence is freed, so
  attach_prefix can hand them to a new sequence with the same tokens
- eviction is lru, the oldest cached block goes first
- stats counts allocations, evictions, prefix hits and misses

things i didnt do yet:
- no persistence, the cache dies with the process
- no per-block timestamps, eviction is just free-list order
- no sliding window attention or block compression, blocks are dumb
  chunks of ints
