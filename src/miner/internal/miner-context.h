// Copyright (c) 2019 Duality Blockchain Solutions Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DYNAMIC_INTERNAL_MINER_CONTEXT_H
#define DYNAMIC_INTERNAL_MINER_CONTEXT_H

#include "miner/internal/hash-rate-counter.h"

#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include <atomic>

class CBlock;
class CChainParams;
class CConnman;
class CBlockIndex;
struct CBlockTemplate;

class MinerBase;
class MinerContext;
class MinersController;

/** Miner context shared_ptr */
using MinerContextRef = std::shared_ptr<MinerContext>;

struct MinerSharedContext {
public:
    const CChainParams& chainparams;
    CConnman& connman;

    MinerSharedContext(const CChainParams& chainparams_, CConnman& connman_)
        : chainparams(chainparams_), connman(connman_){};

    // Returns chain tip of current block template
    CBlockIndex* tip() const { return _chain_tip; }

    // Returns miner block flag
    // It's incremented every time new template is generated
    uint64_t block_flag() const { return _block_flag; }

    // Returns true if block was created
    bool has_block() const { return _block_template != nullptr; }

    // Returns miner block template
    std::shared_ptr<CBlockTemplate> block_template()
    {
        boost::shared_lock<boost::shared_mutex> guard(_mutex);
        return _block_template;
    }

protected:
    friend class MinerBase;
    friend class MinersController;

    // recreates miners block template
    void RecreateBlock();

private:
    // current block chain tip
    std::atomic<CBlockIndex*> _chain_tip{nullptr};
    // atomic flag incremented on recreated block
    std::atomic<std::uint64_t> _block_flag{0};
    // shared block template for miners
    std::shared_ptr<CBlockTemplate> _block_template{nullptr};
    // mutex protecting multiple threads recreating block
    mutable boost::shared_mutex _mutex;
};

using MinerSharedContextRef = std::shared_ptr<MinerSharedContext>;

/**
 * Miner context.
 */
class MinerContext
{
public:
    HashRateCounterRef counter;
    MinerSharedContextRef shared;

    MinerContext(const CChainParams& chainparams_, CConnman& connman_);
    MinerContext(MinerSharedContextRef shared_, HashRateCounterRef counter_);

    // Constructs child context
    explicit MinerContext(const MinerContext* ctx_)
        : MinerContext(ctx_->shared, ctx_->counter->MakeChild()){};

    // Creates child context for group or miner
    MinerContextRef MakeChild() const { return std::make_shared<MinerContext>(this); }

    // Connection manager
    CConnman& connman() const { return shared->connman; }

    // Chain parameters
    const CChainParams& chainparams() const { return shared->chainparams; }
};

#endif // DYNAMIC_INTERNAL_MINER_CONTEXT_H
