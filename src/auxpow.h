// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014 Daniel Kraft
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_AUXPOW_H
#define BITCOIN_AUXPOW_H

#include "serialize.h"
#include "uint256.h"

#include "primitives/block.h"
#include "primitives/transaction.h"

#include <vector>

class CBlockIndex;

/* Because it is needed for auxpow, the definition of CMerkleTx is moved
   here from wallet.h.  The implementations of the various methods are
   not needed and stay in wallet.cpp.  */

/** A transaction with a merkle branch linking it to the block chain. */
class CMerkleTx : public CTransaction
{
private:
    int GetDepthInMainChainINTERNAL(const CBlockIndex* &pindexRet) const;

public:
    uint256 hashBlock;
    std::vector<uint256> vMerkleBranch;
    int nIndex;

    // memory only
    mutable bool fMerkleVerified;


    CMerkleTx()
    {
        Init();
    }

    CMerkleTx(const CTransaction& txIn) : CTransaction(txIn)
    {
        Init();
    }

    void Init()
    {
        hashBlock = uint256();
        nIndex = -1;
        fMerkleVerified = false;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(*(CTransaction*)this);
        nVersion = this->nVersion;
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
    }

    int SetMerkleBranch(const CBlock& block);


    /**
     * Return depth of transaction in blockchain:
     * -1  : not in blockchain, and not in memory pool (conflicted transaction)
     *  0  : in memory pool, waiting to be included in a block
     * >=1 : this many blocks deep in the main chain
     */
    int GetDepthInMainChain(const CBlockIndex* &pindexRet) const;
    int GetDepthInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChain(pindexRet); }
    bool IsInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChainINTERNAL(pindexRet) > 0; }
    int GetBlocksToMaturity() const;
    bool AcceptToMemoryPool(bool fLimitFree=true, bool fRejectInsaneFee=true);
};

/**
 * Data for the merge-mining auxpow.  This is a merkle tx (the parent block's
 * coinbase tx) that can be verified to be in the parent block, and this
 * transaction's input (the coinbase script) contains the reference
 * to the actual merge-mined block.
 */
class CAuxPow : public CMerkleTx
{

private:

  /** The merkle branch connecting the aux block to our coinbase.  */
  std::vector<uint256> vChainMerkleBranch;

  /** Merkle tree index of the aux block header in the coinbase.  */
  int nChainIndex;

  /** Parent block header (on which the real PoW is done).  */
  CBlockHeader parentBlock;

public:

  inline explicit CAuxPow (const CTransaction& txIn)
    : CMerkleTx (txIn)
  {}

  inline CAuxPow ()
    : CMerkleTx ()
  {}

  ADD_SERIALIZE_METHODS;

  template<typename Stream, typename Operation>
    inline void
    SerializationOp (Stream& s, Operation ser_action, int nType, int nVersion)
  {
    READWRITE (*static_cast<CMerkleTx*> (this));
    nVersion = this->nVersion;

    READWRITE (vChainMerkleBranch);
    READWRITE (nChainIndex);
    READWRITE (parentBlock);
  }

  /**
   * Check the auxpow, given the merge-mined block's hash and our chain ID.
   * Note that this does not verify the actual PoW on the parent block!  It
   * just confirms that all the merkle branches are valid.
   * @param hashAuxBlock Hash of the merge-mined block.
   * @param nChainId The auxpow chain ID of the block to check.
   * @return True if the auxpow is valid.
   */
  bool check (const uint256& hashAuxBlock, int nChainId) const;

  /**
   * Get the parent block's hash.  This is used to verify that it
   * satisfies the PoW requirement.
   * @return The parent block hash.
   */
  inline uint256
  getParentBlockHash () const
  {
    return parentBlock.GetHash ();
  }

};

/* Implement serialisation of auxpow in the block header.  Since this
   depends on CAuxPow being available, it can not be defined there.  */
template<typename Stream, typename Operation>
  void
  ReadWriteAuxPow (Stream& s, Operation ser_action,
                   boost::shared_ptr<CAuxPow>& auxpow,
                   int nType, const CBlockVersion& nBlockVersion)
{
  const int nVersion = nBlockVersion.GetBaseVersion ();

  if (nBlockVersion.IsAuxpow ())
    {
      if (ser_action.ForRead ())
        auxpow.reset (new CAuxPow());
      assert (auxpow);
      READWRITE (*auxpow);
    }
  else if (ser_action.ForRead ())
    auxpow.reset ();
}

#endif // BITCOIN_AUXPOW_H
