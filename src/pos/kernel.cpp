// Copyright (c) 2019 Duality Blockchain Solutions Developers
// Copyright (c) 2015-2019 The PIVX developers
// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pos/kernel.h"

#include "arith_uint256.h"
#include "chainparams.h"
#include "db.h"
#include "policy/policy.h"
#include "pos/stakeinput.h"
#include "script/interpreter.h"
#include "timedata.h"
#include "uint256.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/assign/list_of.hpp>

// TODO (PoS): Add mainnet checkpoints after staking starts.
// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints = {};

/* NEW MODIFIER */

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel;
    ss << pindexPrev->nStakeModifier;

    return ss.GetHash();
}

bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, const unsigned int nBits, CStakeInput* stake, const unsigned int nTimeTx, uint256& hashProofOfStake, const bool fVerify)
{
    // Calculate the proof of stake hash
    if (!GetHashProofOfStake(pindexPrev, stake, nTimeTx, fVerify, hashProofOfStake)) {
        return error("%s : Failed to calculate the proof of stake hash", __func__);
    }

    const CAmount& nValueIn = stake->GetValue();
    const CDataStream& ssUniqueID = stake->GetUniqueness();

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    arith_uint256 bnWeight = arith_uint256(nValueIn) / 100;
    bnTarget *= bnWeight;

    // Check if proof-of-stake hash meets target protocol
    const bool res = (UintToArith256(hashProofOfStake) < bnTarget);

    if (fVerify || res) {
        LogPrint("staking", "%s : Proof Of Stake:"
                            "\nssUniqueID=%s"
                            "\nnTimeTx=%d"
                            "\nhashProofOfStake=%s"
                            "\nnBits=%d"
                            "\nweight=%d"
                            "\nbnTarget=%s (res: %d)\n\n",
            __func__, HexStr(ssUniqueID), nTimeTx, hashProofOfStake.GetHex(),
            nBits, nValueIn, bnTarget.GetHex(), res);
    }

    return res;
}

bool GetHashProofOfStake(const CBlockIndex* pindexPrev, CStakeInput* stake, const unsigned int nTimeTx, const bool fVerify, uint256& hashProofOfStakeRet) {
    // Grab the stake data
    CBlockIndex* pindexfrom = stake->GetIndexFrom();
    if (!pindexfrom) return error("%s : Failed to find the block index for stake origin", __func__);
    const CDataStream& ssUniqueID = stake->GetUniqueness();
    const unsigned int nTimeBlockFrom = pindexfrom->nTime;
    CDataStream modifier_ss(SER_GETHASH, 0);

    // Hash the modifier
    // Modifier v2
    modifier_ss << pindexPrev->nStakeModifier;

    CDataStream ss(modifier_ss);
    // Calculate hash
    ss << nTimeBlockFrom << ssUniqueID << nTimeTx;
    hashProofOfStakeRet = HashBlake2b(ss.begin(), ss.end());

    if (fVerify) {
        LogPrint("staking", "%s :{ nStakeModifier=%s\n"
                            "}\n",
            __func__, HexStr(modifier_ss));
    }
    return true;
}

bool Stake(const CBlockIndex* pindexPrev, CStakeInput* stakeInput, unsigned int nBits, unsigned int& nTimeTx, uint256& hashProofOfStake)
{
    int prevHeight = pindexPrev->nHeight;

    // get stake input pindex
    CBlockIndex* pindexFrom = stakeInput->GetIndexFrom();
    if (!pindexFrom || pindexFrom->nHeight < 1) return error("%s : no pindexfrom", __func__);

    const uint32_t nTimeBlockFrom = pindexFrom->nTime;
    const int nHeightBlockFrom = pindexFrom->nHeight;
    // check for maturity (min age) requirements
    if (Params().GetConsensus().nStakeMinAge > GetAdjustedTime() - nTimeBlockFrom)
        return error("%s : min age violation - height=%d - nTimeTx=%d, nTimeBlockFrom=%d, nHeightBlockFrom=%d",
                         __func__, prevHeight + 1, nTimeTx, nTimeBlockFrom, nHeightBlockFrom);

    // check for maturity (min depth) requirements
    const int nHeight = pindexPrev->nHeight + 1;
    if (nHeight < nHeightBlockFrom + Params().COINSTAKE_MIN_DEPTH())
        return error("%s : min depth violation, nHeight=%d, nHeightBlockFrom=%d", __func__, nHeight, nHeightBlockFrom);

    // iterate the hashing
    bool fSuccess = false;
    const unsigned int nHashDrift = 60;
    unsigned int nTryTime = nTimeTx - 1;
    // iterate from nTimeTx up to nTimeTx + nHashDrift
    // but not after the max allowed future blocktime drift (3 minutes for PoS)
    const unsigned int maxTime = std::min(nTimeTx + nHashDrift, Params().MaxFutureBlockTime(GetAdjustedTime(), true));

    while (nTryTime < maxTime)
    {
        //new block came in, move on
        if (chainActive.Height() != prevHeight)
            break;

        ++nTryTime;

        // if stake hash does not meet the target then continue to next iteration
        if (!CheckStakeKernelHash(pindexPrev, nBits, stakeInput, nTryTime, hashProofOfStake))
            continue;

        // if we made it this far, then we have successfully found a valid kernel hash
        fSuccess = true;
        nTimeTx = nTryTime;
        break;
    }

    mapHashedBlocks.clear();
    mapHashedBlocks[chainActive.Tip()->nHeight] = GetTime(); //store a time stamp of when we last hashed on this block
    return fSuccess;
}

bool initStakeInput(const CBlock block, std::unique_ptr<CStakeInput>& stake, int nPreviousBlockHeight) {
    CTransactionRef ptx = block.vtx[1];
    if (!ptx->IsCoinStake())
        return error("%s : called on non-coinstake %s", __func__, ptx->GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = ptx->vin[0];

    //Construct the stakeinput object
    // First try finding the previous transaction in database
    uint256 hashBlock;
    CTransactionRef ptxPrev;
    if (!GetTransaction(txin.prevout.hash, ptxPrev, Params().GetConsensus(), hashBlock, true))
        return error("%s : INFO: read txPrev failed, tx id prev: %s, block id %s",
                     __func__, txin.prevout.hash.GetHex(), block.GetHash().GetHex());

    //verify signature and script
    if (!VerifyScript(txin.scriptSig, ptxPrev->vout[txin.prevout.n].scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, TransactionSignatureChecker(ptx.get(), 0)))
        return error("%s : VerifySignature failed on coinstake %s", __func__, ptx->GetHash().ToString().c_str());

    const CTransaction& txPrev = *ptxPrev.get();
    CDynamicStake* stakeInput = new CDynamicStake();
    stakeInput->SetInput(txPrev, txin.prevout.n);
    stake = std::unique_ptr<CStakeInput>(stakeInput);

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlock& block, uint256& hashProofOfStake, std::unique_ptr<CStakeInput>& stake, const int& nPreviousBlockHeight)
{
    // Initialize the stake object
    if(!initStakeInput(block, stake, nPreviousBlockHeight))
        return error("%s : stake input object initialization failed", __func__);

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    CBlockIndex* pindexPrev = mapBlockIndex[block.hashPrevBlock];
    CBlockIndex* pindexfrom = stake->GetIndexFrom();
    if (!pindexfrom)
        return error("%s : Failed to find the block index for stake origin", __func__);

    const unsigned int nTxTime = block.nTime;
    const int nBlockFromHeight = pindexfrom->nHeight;
    const unsigned int nBlockFromTime = pindexfrom->nTime;
    //check for maturity (min depth) requirements
    if (!Params().HasStakeMinDepth(nPreviousBlockHeight + 1, nBlockFromHeight))
        return error("%s : min depth violation - height=%d - nHeightBlockFrom=%d", __func__, nPreviousBlockHeight, nBlockFromHeight);

    //check for maturity (min age) requirements
    if (!Params().HasStakeMinAge(nTxTime, nBlockFromTime))
        return error("%s : min age violation - nTimeTx=%d, nTimeBlockFrom=%d", __func__, nTxTime, nBlockFromTime);

    if (!CheckStakeKernelHash(pindexPrev, block.nBits, stake.get(), nTxTime, hashProofOfStake, true))
        return error("%s : INFO: check kernel failed on coinstake %s, hashProof=%s", __func__,
                     block.vtx[1]->GetHash().GetHex(), hashProofOfStake.GetHex());

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    // v0.3 protocol
    return (nTimeBlock == nTimeTx);
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    if (Params().NetworkIDString() != CBaseChainParams::MAIN) return true; // Testnet has no checkpoints
    if (mapStakeModifierCheckpoints.count(nHeight)) {
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];
    }
    return true;
}
