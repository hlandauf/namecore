// Copyright (c) 2012-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "coins.h"

#include "random.h"
#include "util.h"

#include <assert.h>

// calculate number of bytes for the bitmask, and its number of non-zero bytes
// each bit in the bitmask represents the availability of one output, but the
// availabilities of the first two outputs are encoded separately
void CCoins::CalcMaskSize(unsigned int &nBytes, unsigned int &nNonzeroBytes) const {
    unsigned int nLastUsedByte = 0;
    for (unsigned int b = 0; 2+b*8 < vout.size(); b++) {
        bool fZero = true;
        for (unsigned int i = 0; i < 8 && 2+b*8+i < vout.size(); i++) {
            if (!vout[2+b*8+i].IsNull()) {
                fZero = false;
                continue;
            }
        }
        if (!fZero) {
            nLastUsedByte = b + 1;
            nNonzeroBytes++;
        }
    }
    nBytes += nLastUsedByte;
}

bool CCoins::Spend(const COutPoint &out, CTxInUndo &undo) {
    if (out.n >= vout.size())
        return false;
    if (vout[out.n].IsNull())
        return false;
    undo = CTxInUndo(vout[out.n]);
    vout[out.n].SetNull();
    Cleanup();
    if (vout.size() == 0) {
        undo.nHeight = nHeight;
        undo.fCoinBase = fCoinBase;
        undo.nVersion = this->nVersion;
    }
    return true;
}

bool CCoins::Spend(int nPos) {
    CTxInUndo undo;
    COutPoint out(0, nPos);
    return Spend(out, undo);
}


bool CCoinsView::GetCoins(const uint256 &txid, CCoins &coins) const { return false; }
bool CCoinsView::HaveCoins(const uint256 &txid) const { return false; }
uint256 CCoinsView::GetBestBlock() const { return uint256(0); }
bool CCoinsView::GetName(const valtype &name, CNameData &data) const { return false; }
bool CCoinsView::GetNamesForHeight(unsigned nHeight, std::set<valtype>& names) const { return false; }
void CCoinsView::WalkNames(const valtype& start, CNameWalker& walker) const { assert (false); }
bool CCoinsView::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, const CNameCache &names) { return false; }
bool CCoinsView::GetStats(CCoinsStats &stats) const { return false; }
bool CCoinsView::ValidateNameDB() const { return false; }


CCoinsViewBacked::CCoinsViewBacked(CCoinsView *viewIn) : base(viewIn) { }
bool CCoinsViewBacked::GetCoins(const uint256 &txid, CCoins &coins) const { return base->GetCoins(txid, coins); }
bool CCoinsViewBacked::HaveCoins(const uint256 &txid) const { return base->HaveCoins(txid); }
uint256 CCoinsViewBacked::GetBestBlock() const { return base->GetBestBlock(); }
bool CCoinsViewBacked::GetName(const valtype &name, CNameData &data) const { return base->GetName(name, data); }
bool CCoinsViewBacked::GetNamesForHeight(unsigned nHeight, std::set<valtype>& names) const { return base->GetNamesForHeight(nHeight, names); }
void CCoinsViewBacked::WalkNames(const valtype& start, CNameWalker& walker) const { base->WalkNames(start, walker); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
bool CCoinsViewBacked::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, const CNameCache &names) { return base->BatchWrite(mapCoins, hashBlock, names); }
bool CCoinsViewBacked::GetStats(CCoinsStats &stats) const { return base->GetStats(stats); }
bool CCoinsViewBacked::ValidateNameDB() const { return base->ValidateNameDB(); }

CCoinsKeyHasher::CCoinsKeyHasher() : salt(GetRandHash()) {}

CCoinsViewCache::CCoinsViewCache(CCoinsView *baseIn) : CCoinsViewBacked(baseIn), hasModifier(false), hashBlock(0) { }

CCoinsViewCache::~CCoinsViewCache()
{
    assert(!hasModifier);
}

CCoinsMap::const_iterator CCoinsViewCache::FetchCoins(const uint256 &txid) const {
    CCoinsMap::iterator it = cacheCoins.find(txid);
    if (it != cacheCoins.end())
        return it;
    CCoins tmp;
    if (!base->GetCoins(txid, tmp))
        return cacheCoins.end();
    CCoinsMap::iterator ret = cacheCoins.insert(std::make_pair(txid, CCoinsCacheEntry())).first;
    tmp.swap(ret->second.coins);
    if (ret->second.coins.IsPruned()) {
        // The parent only has an empty entry for this txid; we can consider our
        // version as fresh.
        ret->second.flags = CCoinsCacheEntry::FRESH;
    }
    return ret;
}

bool CCoinsViewCache::GetCoins(const uint256 &txid, CCoins &coins) const {
    CCoinsMap::const_iterator it = FetchCoins(txid);
    if (it != cacheCoins.end()) {
        coins = it->second.coins;
        return true;
    }
    return false;
}

CCoinsModifier CCoinsViewCache::ModifyCoins(const uint256 &txid) {
    assert(!hasModifier);
    hasModifier = true;
    std::pair<CCoinsMap::iterator, bool> ret = cacheCoins.insert(std::make_pair(txid, CCoinsCacheEntry()));
    if (ret.second) {
        if (!base->GetCoins(txid, ret.first->second.coins)) {
            // The parent view does not have this entry; mark it as fresh.
            ret.first->second.coins.Clear();
            ret.first->second.flags = CCoinsCacheEntry::FRESH;
        } else if (ret.first->second.coins.IsPruned()) {
            // The parent view only has a pruned entry for this; mark it as fresh.
            ret.first->second.flags = CCoinsCacheEntry::FRESH;
        }
    }
    // Assume that whenever ModifyCoins is called, the entry will be modified.
    ret.first->second.flags |= CCoinsCacheEntry::DIRTY;
    return CCoinsModifier(*this, ret.first);
}

const CCoins* CCoinsViewCache::AccessCoins(const uint256 &txid) const {
    CCoinsMap::const_iterator it = FetchCoins(txid);
    if (it == cacheCoins.end()) {
        return NULL;
    } else {
        return &it->second.coins;
    }
}

bool CCoinsViewCache::HaveCoins(const uint256 &txid) const {
    CCoinsMap::const_iterator it = FetchCoins(txid);
    // We're using vtx.empty() instead of IsPruned here for performance reasons,
    // as we only care about the case where an transaction was replaced entirely
    // in a reorganization (which wipes vout entirely, as opposed to spending
    // which just cleans individual outputs).
    return (it != cacheCoins.end() && !it->second.coins.vout.empty());
}

uint256 CCoinsViewCache::GetBestBlock() const {
    if (hashBlock == uint256(0))
        hashBlock = base->GetBestBlock();
    return hashBlock;
}

void CCoinsViewCache::SetBestBlock(const uint256 &hashBlockIn) {
    hashBlock = hashBlockIn;
}

bool CCoinsViewCache::GetName(const valtype &name, CNameData& data) const {
    if (cacheNames.isDeleted(name))
        return false;
    if (cacheNames.get(name, data))
        return true;

    /* Note: This does not attempt to cache name queries.  The cache
       only keeps track of changes!  */

    return base->GetName(name, data);
}

bool CCoinsViewCache::GetNamesForHeight(unsigned nHeight, std::set<valtype>& names) const {
    /* Query the base view first, and then apply the cached changes (if
       there are any).  */

    if (!base->GetNamesForHeight(nHeight, names))
        return false;

    cacheNames.updateNamesForHeight(nHeight, names);
    return true;
}

void CCoinsViewCache::WalkNames(const valtype& start, CNameWalker& walker) const {
    /* Before using this function, callers should flush the cache.  We don't
       want to deal with combining the backing data store with the cached
       updates.  This is not worth the effort compared to just flushing
       every time a name_scan or name_filter is required.  */
    assert (cacheNames.empty ());

    base->WalkNames (start, walker);
}

void CCoinsViewCache::SetName(const valtype &name, const CNameData& data) {
    CNameData oldData;
    if (GetName(name, oldData))
        cacheNames.removeExpireIndex(name, oldData.getHeight());

    cacheNames.set(name, data);
    cacheNames.addExpireIndex(name, data.getHeight());
}

void CCoinsViewCache::DeleteName(const valtype &name) {
    CNameData oldData;
    if (GetName(name, oldData))
        cacheNames.removeExpireIndex(name, oldData.getHeight());
    else
        LogPrintf("%s : no existing entry for name '%s'",
                  __func__, ValtypeToString(name).c_str());

    cacheNames.remove(name);
}

bool CCoinsViewCache::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlockIn, const CNameCache &names) {
    assert(!hasModifier);
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) { // Ignore non-dirty entries (optimization).
            CCoinsMap::iterator itUs = cacheCoins.find(it->first);
            if (itUs == cacheCoins.end()) {
                if (!it->second.coins.IsPruned()) {
                    // The parent cache does not have an entry, while the child
                    // cache does have (a non-pruned) one. Move the data up, and
                    // mark it as fresh (if the grandparent did have it, we
                    // would have pulled it in at first GetCoins).
                    assert(it->second.flags & CCoinsCacheEntry::FRESH);
                    CCoinsCacheEntry& entry = cacheCoins[it->first];
                    entry.coins.swap(it->second.coins);
                    entry.flags = CCoinsCacheEntry::DIRTY | CCoinsCacheEntry::FRESH;
                }
            } else {
                if ((itUs->second.flags & CCoinsCacheEntry::FRESH) && it->second.coins.IsPruned()) {
                    // The grandparent does not have an entry, and the child is
                    // modified and being pruned. This means we can just delete
                    // it from the parent.
                    cacheCoins.erase(itUs);
                } else {
                    // A normal modification.
                    itUs->second.coins.swap(it->second.coins);
                    itUs->second.flags |= CCoinsCacheEntry::DIRTY;
                }
            }
        }
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }
    hashBlock = hashBlockIn;
    cacheNames.apply(names);
    return true;
}

bool CCoinsViewCache::Flush() {
    bool fOk = base->BatchWrite(cacheCoins, hashBlock, cacheNames);
    cacheCoins.clear();
    cacheNames.clear();
    return fOk;
}

unsigned int CCoinsViewCache::GetCacheSize() const {
    // Do not take name operations into account here.
    return cacheCoins.size();
}

const CTxOut &CCoinsViewCache::GetOutputFor(const CTxIn& input) const
{
    const CCoins* coins = AccessCoins(input.prevout.hash);
    assert(coins && coins->IsAvailable(input.prevout.n));
    return coins->vout[input.prevout.n];
}

CAmount CCoinsViewCache::GetValueIn(const CTransaction& tx) const
{
    if (tx.IsCoinBase())
        return 0;

    CAmount nResult = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
        nResult += GetOutputFor(tx.vin[i]).nValue;

    return nResult;
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            const COutPoint &prevout = tx.vin[i].prevout;
            const CCoins* coins = AccessCoins(prevout.hash);
            if (!coins || !coins->IsAvailable(prevout.n)) {
                return false;
            }
        }
    }
    return true;
}

double CCoinsViewCache::GetPriority(const CTransaction &tx, int nHeight) const
{
    if (tx.IsCoinBase())
        return 0.0;
    double dResult = 0.0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        const CCoins* coins = AccessCoins(txin.prevout.hash);
        assert(coins);
        if (!coins->IsAvailable(txin.prevout.n)) continue;
        if (coins->nHeight < nHeight) {
            dResult += coins->vout[txin.prevout.n].nValue * (nHeight-coins->nHeight);
        }
    }
    return tx.ComputePriority(dResult);
}

CCoinsModifier::CCoinsModifier(CCoinsViewCache& cache_, CCoinsMap::iterator it_) : cache(cache_), it(it_) {}

CCoinsModifier::~CCoinsModifier()
{
    assert(cache.hasModifier);
    cache.hasModifier = false;
    it->second.coins.Cleanup();
    if ((it->second.flags & CCoinsCacheEntry::FRESH) && it->second.coins.IsPruned()) {
        cache.cacheCoins.erase(it);
    }
}
