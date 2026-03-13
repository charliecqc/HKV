#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <deque>
#include <vector>
#include <cstring>
#include <iostream>
#include "node.h"
#include "pmemBFPool.h"

// ---------------------------------------------------------------------------
// HotBloomCache v3 — Bounded DRAM cache with CLOCK eviction + SGP hot priority
//
// Combines two promotion sources:
//   1. Write-path (getBloomForWrite): promotes bloom to DRAM on insert/split.
//      Initial ref=1 (normal LRU, survives 1 CLOCK sweep without access).
//   2. Hot-region (promoteHot): promoted by SGP's maybeActivateHotRegion.
//      Initial ref=3 (higher priority, survives 3 sweeps without access).
//
// Eviction uses the CLOCK algorithm (approximate LRU):
//   - Global clockHand_ scans DirChunk slots sequentially.
//   - On each non-null slot: if ref > 0, decrement and skip; if ref == 0, evict.
//   - Unallocated chunks are skipped entirely for efficiency.
//
// Memory safety for eviction:
//   - Evict: CAS ptr→nullptr, sync DRAM→PMEM, retire ptr to delayed-free queue.
//   - Stale readers holding the old DRAM ptr read valid (synced) data.
//   - Delayed free (kRetireGrace generations) ensures no reader holds a stale
//     ptr when memory is actually freed.
// ---------------------------------------------------------------------------

#ifndef HOT_BLOOM_MAX_CACHED
#define HOT_BLOOM_MAX_CACHED 1500000UL
#endif

class HotBloomCache {
public:
    // ---- tunables ----
    static constexpr size_t kChunkSize   = 65536;  // 64K entries per DirChunk
    static constexpr size_t kMaxChunks   =
        (MAX_VALUE_NODES + kChunkSize - 1) / kChunkSize;
    static constexpr size_t kMaxCached   = HOT_BLOOM_MAX_CACHED;
    static constexpr size_t kRetireGrace = 2000;   // delayed-free generations

    // ---- per-slot metadata inside DirChunk ----
    struct alignas(64) DirChunk {
        std::atomic<BloomFilter*> ptrs[kChunkSize];
        std::atomic<uint8_t>      refs[kChunkSize];  // CLOCK reference counter
        std::atomic<uint8_t>      dirty[kChunkSize]; // 1 = modified via write path

        DirChunk() {
            for (size_t i = 0; i < kChunkSize; ++i) {
                ptrs[i].store(nullptr, std::memory_order_relaxed);
                refs[i].store(0, std::memory_order_relaxed);
                dirty[i].store(0, std::memory_order_relaxed);
            }
        }
    };

    // ---- construction / destruction ----
    explicit HotBloomCache(PmemBFPool* pmemPool)
        : pmemPool_(pmemPool), clockHand_(0)
    {
        for (auto& slot : chunks_)
            slot.store(nullptr, std::memory_order_relaxed);
    }

    ~HotBloomCache() {
        for (auto& slot : chunks_) {
            DirChunk* c = slot.load(std::memory_order_relaxed);
            if (!c) continue;
            for (size_t i = 0; i < kChunkSize; ++i) {
                BloomFilter* bf = c->ptrs[i].load(std::memory_order_relaxed);
                delete bf;
            }
            delete c;
        }
        std::lock_guard<std::mutex> lk(retireMu_);
        for (auto& r : retired_)
            delete r.ptr;
    }

    // ======================================================================
    //  Primary interface
    // ======================================================================

    /// Read-path: returns DRAM copy if cached, else PMEM.
    /// Does NOT touch ref — PMEM read ≈ DRAM read, so ref should only
    /// reflect write-hotness.  This ensures CLOCK evicts write-cold entries
    /// first, reserving DRAM for write-hot VNodes.
    BloomFilter* getBloom(size_t vnodeId) {
        size_t ci  = vnodeId / kChunkSize;
        size_t off = vnodeId % kChunkSize;

        DirChunk* c = chunks_[ci].load(std::memory_order_acquire);
        if (c) {
            BloomFilter* dram = c->ptrs[off].load(std::memory_order_acquire);
            if (dram) return dram;  // use DRAM copy but don't touch ref
        }
        return pmemPool_->at(vnodeId);
    }

    /// Write-path: promotes to DRAM if cache has room, else falls back to PMEM.
    /// Never triggers eviction — eviction is only done by the hot-region path
    /// (promoteHot) to avoid CLOCK overhead on every insert.
    BloomFilter* getBloomForWrite(size_t vnodeId) {
        size_t ci  = vnodeId / kChunkSize;
        size_t off = vnodeId % kChunkSize;

        // Fast path: already in DRAM
        DirChunk* c = chunks_[ci].load(std::memory_order_acquire);
        if (c) {
            BloomFilter* dram = c->ptrs[off].load(std::memory_order_acquire);
            if (dram) {
                uint8_t old = c->refs[off].load(std::memory_order_relaxed);
                if (old < 3)
                    c->refs[off].store(old + 1, std::memory_order_relaxed);
                c->dirty[off].store(1, std::memory_order_relaxed); // mark dirty
                return dram;
            }
        }
        // Promote without eviction — if cache full, fall through to PMEM
        {
            if (promoteInternal(vnodeId, 1, /*allowEviction=*/false)) {
                c = chunks_[ci].load(std::memory_order_acquire);
                if (c) {
                    BloomFilter* dram = c->ptrs[off].load(std::memory_order_acquire);
                    if (dram) {
                        c->dirty[off].store(1, std::memory_order_relaxed); // mark dirty
                        return dram;
                    }
                }
            }
        }
        // Cache full and this vnode isn't cached — use PMEM directly
        return pmemPool_->at(vnodeId);
    }

    /// True if this VNode's bloom is currently cached in DRAM.
    bool isCached(size_t vnodeId) const {
        size_t ci  = vnodeId / kChunkSize;
        size_t off = vnodeId % kChunkSize;
        DirChunk* c = chunks_[ci].load(std::memory_order_acquire);
        return c && c->ptrs[off].load(std::memory_order_acquire) != nullptr;
    }

    // ======================================================================
    //  Promotion interfaces
    // ======================================================================

    /// Normal promotion (ref=1). Called from getBloomForWrite.
    /// Never evicts — if cache is full, returns false.
    bool promote(size_t vnodeId) {
        return promoteInternal(vnodeId, 1, /*allowEviction=*/false);
    }

    /// Hot-region promotion (ref=3). Called from maybeActivateHotRegion.
    /// MAY evict cold entries — this is the ONLY path that triggers eviction.
    bool promoteHot(size_t vnodeId) {
        if (isCached(vnodeId)) {
            // Already cached — boost its ref to 3 (protects from eviction)
            size_t ci  = vnodeId / kChunkSize;
            size_t off = vnodeId % kChunkSize;
            DirChunk* c = chunks_[ci].load(std::memory_order_acquire);
            if (c) c->refs[off].store(3, std::memory_order_relaxed);
            return false;
        }
        return promoteInternal(vnodeId, 3, /*allowEviction=*/true);
    }

    /// Batch promote multiple VNode IDs with hot priority.
    size_t promoteMany(const std::vector<int>& vnodeIds) {
        size_t promoted = 0;
        for (int id : vnodeIds) {
            if (id >= 0 && promoteHot(static_cast<size_t>(id)))
                ++promoted;
        }
        return promoted;
    }

    /// Walk the VNode chain and promote with hot priority.
    void promoteVnodeChain(size_t startVnodeId, int maxDepth = 4) {
        if (startVnodeId >= MAX_VALUE_NODES) return;
        for (int d = 0; d < maxDepth; ++d) {
            promoteHot(startVnodeId);
            BloomFilter* bf = getBloom(startVnodeId);
            if (!bf) break;
            int next = read_consistent(bf->version, [&]() {
                return bf->next_id;
            });
            if (next < 0 || static_cast<size_t>(next) >= MAX_VALUE_NODES)
                break;
            startVnodeId = static_cast<size_t>(next);
        }
    }

    // ======================================================================
    //  Persistence / stats
    // ======================================================================

    /// Sync every DRAM copy back to PMEM (shutdown / checkpoint).
    void syncToPmem() {
        for (size_t ci = 0; ci < kMaxChunks; ++ci) {
            DirChunk* c = chunks_[ci].load(std::memory_order_relaxed);
            if (!c) continue;
            for (size_t i = 0; i < kChunkSize; ++i) {
                if (!c->dirty[i].load(std::memory_order_relaxed))
                    continue;  // skip clean entries
                BloomFilter* dram = c->ptrs[i].load(std::memory_order_relaxed);
                if (!dram) continue;
                size_t vnodeId = ci * kChunkSize + i;
                BloomFilter* pmem = pmemPool_->at(vnodeId);
                if (!pmem) continue;
                syncOneToPmem(dram, pmem);
                c->dirty[i].store(0, std::memory_order_relaxed);
            }
        }
    }

    size_t cachedCount() const {
        return population_.load(std::memory_order_relaxed);
    }

    size_t dramBytes() const {
        size_t bytes = 0;
        for (size_t ci = 0; ci < kMaxChunks; ++ci) {
            DirChunk* c = chunks_[ci].load(std::memory_order_relaxed);
            if (c) bytes += sizeof(DirChunk);
        }
        bytes += population_.load(std::memory_order_relaxed) * sizeof(BloomFilter);
        return bytes;
    }

    size_t maxCapacity()   const { return kMaxCached; }
    size_t evictionCount() const { return evictions_.load(std::memory_order_relaxed); }

    // ======================================================================
    //  Utility: collect VNode IDs reachable from a level-0 Inode
    // ======================================================================

    static std::vector<int> collectVnodeIds(const Inode* inode) {
        std::vector<int> ids;
        if (!inode || inode->hdr.level != 0) return ids;
        for (int i = 0; i <= inode->hdr.last_index; ++i) {
            int vid = static_cast<int>(inode->gp_values[i]);
            if (vid >= 0 && vid < static_cast<int>(MAX_VALUE_NODES))
                ids.push_back(vid);
        }
        {
#if ENABLE_SGP
            uint32_t vis = inode->sgpVisible.load(std::memory_order_acquire);
            for (int i = 0; i <= inode->hdr.last_sgp; ++i) {
                if ((vis >> i) & 1u) {
                    int vid = static_cast<int>(inode->sgp_values[i]);
                    if (vid >= 0 && vid < static_cast<int>(MAX_VALUE_NODES))
                        ids.push_back(vid);
                }
            }
#endif
        }
        return ids;
    }

private:
    // ---- directory ----
    std::array<std::atomic<DirChunk*>, kMaxChunks> chunks_;
    PmemBFPool* pmemPool_;
    std::mutex  allocMu_;

    // ---- population tracking ----
    std::atomic<size_t> population_{0};
    std::atomic<size_t> evictions_{0};

    // ---- CLOCK eviction state ----
    std::atomic<size_t> clockHand_;

    // ---- delayed free ----
    struct Retired { BloomFilter* ptr; size_t gen; };
    std::mutex              retireMu_;
    std::deque<Retired>     retired_;
    std::atomic<size_t>     retireGen_{0};

    // ------------------------------------------------------------------
    //  Core: promote, optionally with eviction when at capacity
    //
    //  allowEviction=false  →  write-path: bail out if cache full (no CLOCK overhead)
    //  allowEviction=true   →  prediction-path: evict a cold entry to make room
    // ------------------------------------------------------------------
    bool promoteInternal(size_t vnodeId, uint8_t initialRef,
                         bool allowEviction = false) {
        if (vnodeId >= MAX_VALUE_NODES) return false;
        if (isCached(vnodeId)) return false;

        BloomFilter* pmem = pmemPool_->at(vnodeId);
        if (!pmem) return false;

        // Make room if at capacity
        if (population_.load(std::memory_order_relaxed) >= kMaxCached) {
            if (!allowEviction || !evictOne()) {
                return false;   // cache full — write-path falls back to PMEM
            }
        }

        // Allocate DRAM copy (reuse from retired pool if possible)
        BloomFilter* dram = allocateDram();

        // Consistent snapshot from PMEM
        read_consistent(pmem->version, [&]() -> int {
            dram->next_id = pmem->next_id;
            dram->min_key = pmem->min_key;
            std::memcpy(dram->fingerprints,
                        pmem->fingerprints,
                        sizeof(dram->fingerprints));
            return 0;
        });

        uint64_t ver = pmem->version.load(std::memory_order_acquire);
        while (ver & 1u)
            ver = pmem->version.load(std::memory_order_acquire);
        dram->version.store(ver, std::memory_order_relaxed);

        // Install in directory
        DirChunk* c = ensureChunk(vnodeId / kChunkSize);
        size_t off  = vnodeId % kChunkSize;

        BloomFilter* expected = nullptr;
        if (!c->ptrs[off].compare_exchange_strong(expected, dram,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
            // Concurrent promotion — boost existing entry's ref
            c->refs[off].store(initialRef, std::memory_order_relaxed);
            freeDram(dram);
            return false;
        }

        c->refs[off].store(initialRef, std::memory_order_relaxed);
        population_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // ------------------------------------------------------------------
    //  CLOCK eviction: scan DirChunks globally for a victim (ref==0)
    // ------------------------------------------------------------------
    bool evictOne() {
        // Scan up to 4M slots (covers 2× full vnodeId space)
        const size_t maxScan = std::min<size_t>(MAX_VALUE_NODES * 2, 4000000);

        for (size_t attempt = 0; attempt < maxScan; ++attempt) {
            size_t vid = clockHand_.fetch_add(1, std::memory_order_relaxed)
                         % MAX_VALUE_NODES;
            size_t ci  = vid / kChunkSize;
            size_t off = vid % kChunkSize;

            DirChunk* c = chunks_[ci].load(std::memory_order_acquire);
            if (!c) {
                // Skip rest of this unallocated chunk
                size_t remaining = kChunkSize - off - 1;
                if (remaining > 0)
                    clockHand_.fetch_add(remaining, std::memory_order_relaxed);
                continue;
            }

            BloomFilter* dram = c->ptrs[off].load(std::memory_order_acquire);
            if (!dram) continue;  // not cached

            // Check reference counter
            uint8_t ref = c->refs[off].load(std::memory_order_relaxed);
            if (ref > 0) {
                c->refs[off].store(ref - 1, std::memory_order_relaxed);
                continue;  // give it another chance
            }

            // ref == 0 → evict.  CAS ptr→nullptr to claim this slot.
            BloomFilter* expected = dram;
            if (!c->ptrs[off].compare_exchange_strong(expected, nullptr,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
                continue;  // concurrent modification
            }

            // Sync DRAM→PMEM only if dirty (written via getBloomForWrite)
            if (c->dirty[off].load(std::memory_order_relaxed)) {
                BloomFilter* pmem = pmemPool_->at(vid);
                if (pmem) syncOneToPmem(dram, pmem);
                c->dirty[off].store(0, std::memory_order_relaxed);
            }

            retireBloom(dram);
            population_.fetch_sub(1, std::memory_order_relaxed);
            evictions_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;  // all entries referenced — could not evict
    }

    // ------------------------------------------------------------------
    //  Helpers
    // ------------------------------------------------------------------

    /// Copy one DRAM bloom → PMEM bloom.
    static void syncOneToPmem(BloomFilter* dram, BloomFilter* pmem) {
        pmem->version.store(
            dram->version.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        pmem->next_id = dram->next_id;
        pmem->min_key = dram->min_key;
        std::memcpy(pmem->fingerprints,
                    dram->fingerprints,
                    sizeof(dram->fingerprints));
    }

    /// Lazily allocate a DirChunk.
    DirChunk* ensureChunk(size_t ci) {
        DirChunk* c = chunks_[ci].load(std::memory_order_acquire);
        if (c) return c;
        std::lock_guard<std::mutex> lk(allocMu_);
        c = chunks_[ci].load(std::memory_order_relaxed);
        if (!c) {
            c = new DirChunk();
            chunks_[ci].store(c, std::memory_order_release);
        }
        return c;
    }

    /// Allocate a DRAM BloomFilter — reuse from retired pool when possible.
    BloomFilter* allocateDram() {
        {
            std::lock_guard<std::mutex> lk(retireMu_);
            size_t gen = retireGen_.load(std::memory_order_relaxed);
            if (!retired_.empty() &&
                retired_.front().gen + kRetireGrace <= gen) {
                BloomFilter* reused = retired_.front().ptr;
                retired_.pop_front();
                return reused;
            }
        }
        return new BloomFilter();
    }

    /// Retire a DRAM bloom for delayed free.
    void retireBloom(BloomFilter* ptr) {
        std::lock_guard<std::mutex> lk(retireMu_);
        size_t gen = retireGen_.fetch_add(1, std::memory_order_relaxed);
        retired_.push_back({ptr, gen});

        // Free entries past the grace period
        while (!retired_.empty() &&
               retired_.front().gen + kRetireGrace <= gen) {
            delete retired_.front().ptr;
            retired_.pop_front();
        }
    }

    /// Free a never-published DRAM bloom (e.g., failed CAS).
    void freeDram(BloomFilter* ptr) { delete ptr; }
};
