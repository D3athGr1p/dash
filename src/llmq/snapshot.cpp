// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/simplifiedmns.h>
#include <llmq/quorums.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/snapshot.h>

#include <evo/specialtx.h>

#include <serialize.h>
#include <version.h>

#include <base58.h>
#include <chainparams.h>
#include <univalue.h>
#include <validation.h>

namespace llmq
{

static const std::string DB_QUORUM_SNAPSHOT = "llmq_S";

std::unique_ptr<CQuorumSnapshotManager> quorumSnapshotManager;

void CQuorumSnapshot::ToJson(UniValue &obj) const
{
    //TODO Check this function if correct
    obj.setObject();
    UniValue activeQ(UniValue::VARR);
    for (const auto& h : activeQuorumMembers) {
        activeQ.push_back(h);
    }
    obj.pushKV("activeQuorumMembers", activeQ);
    obj.pushKV("mnSkipListMode", mnSkipListMode);
    UniValue skipList(UniValue::VARR);
    for (const auto& h : mnSkipList) {
        skipList.push_back(h);
    }
    obj.pushKV("mnSkipList", skipList);
}

void CQuorumRotationInfo::ToJson(UniValue &obj) const
{
    //TODO Check this function if correct
    obj.setObject();
    obj.pushKV("creationHeight", creationHeight);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << quorumSnaphotAtHMinusC;
    obj.pushKV("quorumSnaphotAtHMinusC", HexStr(ss));
    ss.clear();
    ss << quorumSnaphotAtHMinus2C;
    obj.pushKV("quorumSnaphotAtHMinus2C", HexStr(ss));
    ss.clear();
    ss << quorumSnaphotAtHMinus3C;
    obj.pushKV("quorumSnaphotAtHMinus3C", HexStr(ss));
    ss.clear();
    ss << mnListDiffTip;
    obj.pushKV("mnListDiffTip", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtHMinusC;
    obj.pushKV("mnListDiffAtHMinusC", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtHMinus2C;
    obj.pushKV("mnListDiffAtHMinus2C", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtHMinus3C;
    obj.pushKV("mnListDiffAtHMinus3C", HexStr(ss));
    ss.clear();
}

bool BuildQuorumRotationInfo(const CGetQuorumRotationInfo& request, CQuorumRotationInfo& response, std::string& errorRet)
{
    AssertLockHeld(cs_main);

    if (request.baseBlockHashesNb > 4) {
        errorRet = strprintf("invalid requested baseBlockHashesNb");
        return false;
    }

    if (request.baseBlockHashesNb != request.baseBlockHashes.size()) {
        errorRet = strprintf("missmatch requested baseBlockHashesNb and size(baseBlockHashes)");
        return false;
    }

    LOCK(deterministicMNManager->cs);

    //Quorum rotation is enabled only for InstantSend atm.
    Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeInstantSend;

    std::vector<const CBlockIndex*> baseBlockIndexes;
    if (request.baseBlockHashesNb == 0) {
        const CBlockIndex *blockIndex = ::ChainActive().Genesis();
        if (!blockIndex) {
            errorRet = strprintf("genesis block not found");
            return false;
        }
        baseBlockIndexes.push_back(blockIndex);
    }
    else {
        for (const auto& blockHash : request.baseBlockHashes){
            const CBlockIndex* blockIndex = LookupBlockIndex(blockHash);
            if (!blockIndex) {
                errorRet = strprintf("block %s not found", blockHash.ToString());
                return false;
            }
            if (!::ChainActive().Contains(blockIndex)){
                errorRet = strprintf("block %s is not in the active chain", blockHash.ToString());
                return false;
            }
            baseBlockIndexes.push_back(blockIndex);
        }
        std::sort(baseBlockIndexes.begin(), baseBlockIndexes.end(), [](const CBlockIndex* a, const CBlockIndex* b){
            return a->nHeight < b->nHeight;
        });
    }

    const CBlockIndex* tipBlockIndex = ::ChainActive().Tip();
    if (!tipBlockIndex) {
        errorRet = strprintf("tip block not found");
        return false;
    }
    //Build MN list Diff always with highest baseblock
    if (!BuildSimplifiedMNListDiff(baseBlockIndexes.back()->GetBlockHash(), tipBlockIndex->GetBlockHash(), response.mnListDiffTip, errorRet)) {
        return false;
    }

    const CBlockIndex* blockIndex = LookupBlockIndex(request.blockRequestHash);
    if (!blockIndex) {
        errorRet = strprintf("block not found");
        return false;
    }

    auto quorums = llmq::quorumBlockProcessor->GetMinedAndActiveCommitmentsUntilBlock(blockIndex);
    auto itQuorums = quorums.find(llmqType);
    if (itQuorums == quorums.end()) {
        errorRet = strprintf("No InstantSend quorum found");
        return false;
    }
    if (itQuorums->second.empty()){
        errorRet = strprintf("Empty list for InstantSend quorum");
        return false;
    }

    // Since the returned quorums are in reversed order, the most recent one is at index 0
    const CBlockIndex* hBlockIndex = LookupBlockIndex(itQuorums->second.at(0)->GetBlockHash());
    if (!hBlockIndex) {
        errorRet = strprintf("Can not find block H");
        return false;
    }
    response.creationHeight = hBlockIndex->nHeight;

    // H-C
    const CBlockIndex* hcBlockIndex = LookupBlockIndex(itQuorums->second.at(1)->GetBlockHash());
    if (!hcBlockIndex) {
        errorRet = strprintf("Can not find block H-C");
        return false;
    }

    if (!BuildSimplifiedMNListDiff(GetLastBaseBlockHash(baseBlockIndexes, hcBlockIndex), hcBlockIndex->GetBlockHash(), response.mnListDiffAtHMinusC, errorRet)) {
        return false;
    }

    auto snapshotHMinusC = quorumSnapshotManager->GetSnapshotForBlock(Params().GetConsensus().llmqTypeInstantSend, hcBlockIndex);
    if (!snapshotHMinusC.has_value()){
        errorRet = strprintf("Can not find quorum snapshot at H-C");
        return false;
    }
    response.quorumSnaphotAtHMinusC = std::move(snapshotHMinusC.value());

    // H-2C
    const CBlockIndex* h2cBlockIndex = LookupBlockIndex(itQuorums->second.at(2)->GetBlockHash());
    if (!h2cBlockIndex) {
        errorRet = strprintf("Can not find block H-2C");
        return false;
    }
    if (!BuildSimplifiedMNListDiff(GetLastBaseBlockHash(baseBlockIndexes, h2cBlockIndex), h2cBlockIndex->GetBlockHash(), response.mnListDiffAtHMinus2C, errorRet)) {
        return false;
    }

    auto snapshotHMinus2C = quorumSnapshotManager->GetSnapshotForBlock(Params().GetConsensus().llmqTypeInstantSend, hcBlockIndex);
    if (!snapshotHMinus2C.has_value()){
        errorRet = strprintf("Can not find quorum snapshot at H-C");
        return false;
    }
    response.quorumSnaphotAtHMinus2C = std::move(snapshotHMinus2C.value());

    // H-3C
    const CBlockIndex* h3cBlockIndex = LookupBlockIndex(itQuorums->second.at(3)->GetBlockHash());
    if (!h3cBlockIndex) {
        errorRet = strprintf("Can not find block H-3C");
        return false;
    }
    if (!BuildSimplifiedMNListDiff(GetLastBaseBlockHash(baseBlockIndexes, h3cBlockIndex), h3cBlockIndex->GetBlockHash(), response.mnListDiffAtHMinus3C, errorRet)) {
        return false;
    }

    auto snapshotHMinus3C = quorumSnapshotManager->GetSnapshotForBlock(Params().GetConsensus().llmqTypeInstantSend, h3cBlockIndex);
    if (!snapshotHMinus3C.has_value()){
        errorRet = strprintf("Can not find quorum snapshot at H-C");
        return false;
    }
    response.quorumSnaphotAtHMinus3C = std::move(snapshotHMinus3C.value());


    return true;
}

uint256 GetLastBaseBlockHash(const std::vector<const CBlockIndex*>& baseBlockIndexes, const CBlockIndex* blockIndex)
{
    uint256 hash;
    for (const auto baseBlock : baseBlockIndexes){
        if (baseBlock->nHeight > blockIndex->nHeight)
            break;
        hash = baseBlock->GetBlockHash();
    }
    return hash;
}

CQuorumSnapshotManager::CQuorumSnapshotManager(CEvoDB& _evoDb) :
        evoDb(_evoDb)
{
}

std::optional<CQuorumSnapshot>  CQuorumSnapshotManager::GetSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex)
{
    CQuorumSnapshot  snapshot = {};

    auto snapshotHash = ::SerializeHash(std::make_pair(llmqType, pindex->GetBlockHash()));

    LOCK(snapshotCacheCs);
    // try using cache before reading from disk
    auto it = quorumSnapshotCache.find(snapshotHash);
    if (it != quorumSnapshotCache.end()) {
        snapshot = it->second;
        return snapshot;
    }
    LOCK(evoDb.cs);
    if (evoDb.Read(std::make_pair(DB_QUORUM_SNAPSHOT, snapshotHash), snapshot)) {
        quorumSnapshotCache.emplace(snapshotHash, snapshot);
        return snapshot;
    }

    return std::nullopt;
}

void CQuorumSnapshotManager::StoreSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex, const CQuorumSnapshot& snapshot)
{
    auto snapshotHash = ::SerializeHash(std::make_pair(llmqType, pindex->GetBlockHash()));

    LOCK(snapshotCacheCs);
    LOCK(evoDb.cs);
    evoDb.Write(std::make_pair(DB_QUORUM_SNAPSHOT, snapshotHash), snapshot);
    quorumSnapshotCache.emplace(snapshotHash, snapshot);
}

} // namespace llmq