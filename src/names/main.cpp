// Copyright (c) 2014 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "names/main.h"

#include "chainparams.h"
#include "coins.h"
#include "leveldbwrapper.h"
#include "../main.h"
#include "script/names.h"
#include "txmempool.h"
#include "undo.h"
#include "util.h"

/**
 * Check whether a name at nPrevHeight is expired at nHeight.  Also
 * heights of MEMPOOL_HEIGHT are supported.  For nHeight == MEMPOOL_HEIGHT,
 * we check at the current best tip's height.
 * @param nPrevHeight The name's output.
 * @param nHeight The height at which it should be updated.
 * @return True iff the name is expired.
 */
static bool
isExpired (unsigned nPrevHeight, unsigned nHeight)
{
  assert (nHeight != MEMPOOL_HEIGHT);
  if (nPrevHeight == MEMPOOL_HEIGHT)
    return false;

  return nPrevHeight + Params ().NameExpirationDepth (nHeight) <= nHeight;
}

/* ************************************************************************** */
/* CNameData.  */

bool
CNameData::isExpired () const
{
  return isExpired (chainActive.Height ());
}

bool
CNameData::isExpired (unsigned h) const
{
  return ::isExpired (nHeight, h);
}

/* ************************************************************************** */
/* CNameCache.  */

/* Write all cached changes to a database batch update object.  */
void
CNameCache::writeBatch (CLevelDBBatch& batch) const
{
  for (std::map<valtype, CNameData>::const_iterator i = entries.begin ();
       i != entries.end (); ++i)
    batch.Write (std::make_pair ('n', i->first), i->second);

  for (std::set<valtype>::const_iterator i = deleted.begin ();
       i != deleted.end (); ++i)
    batch.Erase (std::make_pair ('n', *i));

  assert (fNameHistory || history.empty ());
  for (std::map<valtype, CNameHistory>::const_iterator i = history.begin ();
       i != history.end (); ++i)
    if (i->second.empty ())
      batch.Erase (std::make_pair ('h', i->first));
    else
      batch.Write (std::make_pair ('h', i->first), i->second);

  for (std::map<ExpireEntry, bool>::const_iterator i = expireIndex.begin ();
       i != expireIndex.end (); ++i)
    if (i->second)
      batch.Write (std::make_pair ('x', i->first));
    else
      batch.Erase (std::make_pair ('x', i->first));
}

/* ************************************************************************** */
/* CNameTxUndo.  */

void
CNameTxUndo::fromOldState (const valtype& nm, const CCoinsView& view)
{
  name = nm;
  isNew = !view.GetName (name, oldData);
}

void
CNameTxUndo::apply (CCoinsViewCache& view) const
{
  if (isNew)
    view.DeleteName (name);
  else
    view.SetName (name, oldData, true);
}

/* ************************************************************************** */
/* CNameMemPool.  */

void
CNameMemPool::addUnchecked (const uint256& hash, const CTxMemPoolEntry& entry)
{
  AssertLockHeld (pool.cs);

  if (entry.isNameNew ())
    {
      const valtype& newHash = entry.getNameNewHash ();
      const std::map<valtype, uint256>::const_iterator mit
        = mapNameNews.find (newHash);
      if (mit != mapNameNews.end ())
        assert (mit->second == hash);
      else
        mapNameNews.insert (std::make_pair (newHash, hash));
    }

  if (entry.isNameRegistration ())
    {
      const valtype& name = entry.getName ();
      assert (mapNameRegs.count (name) == 0);
      mapNameRegs.insert (std::make_pair (name, hash));
    }

  if (entry.isNameUpdate ())
    {
      const valtype& name = entry.getName ();
      assert (mapNameUpdates.count (name) == 0);
      mapNameUpdates.insert (std::make_pair (name, hash));
    }
}

void
CNameMemPool::remove (const CTxMemPoolEntry& entry)
{
  AssertLockHeld (pool.cs);

  if (entry.isNameRegistration ())
    {
      const std::map<valtype, uint256>::iterator mit
        = mapNameRegs.find (entry.getName ());
      assert (mit != mapNameRegs.end ());
      mapNameRegs.erase (mit);
    }
  if (entry.isNameUpdate ())
    {
      const std::map<valtype, uint256>::iterator mit
        = mapNameUpdates.find (entry.getName ());
      assert (mit != mapNameUpdates.end ());
      mapNameUpdates.erase (mit);
    }
}

void
CNameMemPool::removeConflicts (const CTransaction& tx,
                               std::list<CTransaction>& removed)
{
  AssertLockHeld (pool.cs);

  if (!tx.IsNamecoin ())
    return;

  BOOST_FOREACH (const CTxOut& txout, tx.vout)
    {
      const CNameScript nameOp(txout.scriptPubKey);
      if (nameOp.isNameOp () && nameOp.getNameOp () == OP_NAME_FIRSTUPDATE)
        {
          const valtype& name = nameOp.getOpName ();
          const std::map<valtype, uint256>::const_iterator mit
            = mapNameRegs.find (name);
          if (mit != mapNameRegs.end ())
            {
              const std::map<uint256, CTxMemPoolEntry>::const_iterator mit2
                = pool.mapTx.find (mit->second);
              assert (mit2 != pool.mapTx.end ());
              pool.remove (mit2->second.GetTx (), removed, true);
            }
        }
    }
}

void
CNameMemPool::removeUnexpireConflicts (const std::set<valtype>& unexpired,
                                       std::list<CTransaction>& removed)
{
  AssertLockHeld (pool.cs);

  BOOST_FOREACH (const valtype& name, unexpired)
    {
      const std::map<valtype, uint256>::const_iterator mit
        = mapNameRegs.find (name);
      if (mit != mapNameRegs.end ())
        {
          const std::map<uint256, CTxMemPoolEntry>::const_iterator mit2
            = pool.mapTx.find (mit->second);
          assert (mit2 != pool.mapTx.end ());
          pool.remove (mit2->second.GetTx (), removed, true);
        }
    }
}

void
CNameMemPool::removeExpireConflicts (const std::set<valtype>& expired,
                                     std::list<CTransaction>& removed)
{
  AssertLockHeld (pool.cs);

  BOOST_FOREACH (const valtype& name, expired)
    {
      const std::map<valtype, uint256>::const_iterator mit
        = mapNameUpdates.find (name);
      if (mit != mapNameUpdates.end ())
        {
          const std::map<uint256, CTxMemPoolEntry>::const_iterator mit2
            = pool.mapTx.find (mit->second);
          assert (mit2 != pool.mapTx.end ());
          pool.remove (mit2->second.GetTx (), removed, true);
        }
    }
}

void
CNameMemPool::check (const CCoinsView& coins) const
{
  AssertLockHeld (pool.cs);

  const uint256 blockHash = coins.GetBestBlock ();
  int nHeight;
  if (blockHash.IsNull())
    nHeight = 0;
  else
    nHeight = mapBlockIndex.find (blockHash)->second->nHeight;

  std::set<valtype> nameRegs;
  std::set<valtype> nameUpdates;
  BOOST_FOREACH (const PAIRTYPE(const uint256, CTxMemPoolEntry)& entry,
                 pool.mapTx)
    {
      if (entry.second.isNameNew ())
        {
          const valtype& newHash = entry.second.getNameNewHash ();
          const std::map<valtype, uint256>::const_iterator mit
            = mapNameNews.find (newHash);

          assert (mit != mapNameNews.end ());
          assert (mit->second == entry.first);
        }

      if (entry.second.isNameRegistration ())
        {
          const valtype& name = entry.second.getName ();

          const std::map<valtype, uint256>::const_iterator mit
            = mapNameRegs.find (name);
          assert (mit != mapNameRegs.end ());
          assert (mit->second == entry.first);

          assert (nameRegs.count (name) == 0);
          nameRegs.insert (name);

          /* The old name should be expired already.  Note that we use
             nHeight+1 for the check, because that's the height at which
             the mempool tx will actually be mined.  */
          CNameData data;
          if (coins.GetName (name, data))
            assert (data.isExpired (nHeight + 1));
        }

      if (entry.second.isNameUpdate ())
        {
          const valtype& name = entry.second.getName ();

          const std::map<valtype, uint256>::const_iterator mit
            = mapNameUpdates.find (name);
          assert (mit != mapNameUpdates.end ());
          assert (mit->second == entry.first);

          assert (nameUpdates.count (name) == 0);
          nameUpdates.insert (name);

          /* As above, use nHeight+1 for the expiration check.  */
          CNameData data;
          if (!coins.GetName (name, data))
            assert (false);
          assert (!data.isExpired (nHeight + 1));
        }
    }

  assert (nameRegs.size () == mapNameRegs.size ());
  assert (nameUpdates.size () == mapNameUpdates.size ());
}

bool
CNameMemPool::checkTx (const CTransaction& tx) const
{
  AssertLockHeld (pool.cs);

  if (!tx.IsNamecoin ())
    return true;

  /* In principle, multiple name_updates could be performed within the
     mempool at once (building upon each other).  This is disallowed, though,
     since the current mempool implementation does not like it.  (We keep
     track of only a single update tx for each name.)  */

  BOOST_FOREACH (const CTxOut& txout, tx.vout)
    {
      const CNameScript nameOp(txout.scriptPubKey);
      if (!nameOp.isNameOp ())
        continue;

      switch (nameOp.getNameOp ())
        {
        case OP_NAME_NEW:
          {
            const valtype& newHash = nameOp.getOpHash ();
            std::map<valtype, uint256>::const_iterator mi;
            mi = mapNameNews.find (newHash);
            if (mi != mapNameNews.end () && mi->second != tx.GetHash ())
              return false;
            break;
          }

        case OP_NAME_FIRSTUPDATE:
          {
            const valtype& name = nameOp.getOpName ();
            if (registersName (name))
              return false;
            break;
          }

        case OP_NAME_UPDATE:
          {
            const valtype& name = nameOp.getOpName ();
            if (updatesName (name))
              return false;
            break;
          }

        default:
          assert (false);
        }
    }

  return true;
}

/* ************************************************************************** */

bool
CheckNameTransaction (const CTransaction& tx, unsigned nHeight,
                      const CCoinsView& view,
                      CValidationState& state, unsigned flags)
{
  const std::string strTxid = tx.GetHash ().GetHex ();
  const char* txid = strTxid.c_str ();
  const bool fMempool = (flags & SCRIPT_VERIFY_NAMES_MEMPOOL);

  /* Ignore historic bugs.  */
  CChainParams::BugType type;
  if (Params ().IsHistoricBug (tx.GetHash (), nHeight, type))
    return true;

  /* As a first step, try to locate inputs and outputs of the transaction
     that are name scripts.  At most one input and output should be
     a name operation.  */

  int nameIn = -1;
  CNameScript nameOpIn;
  CCoins coinsIn;
  for (unsigned i = 0; i < tx.vin.size (); ++i)
    {
      const COutPoint& prevout = tx.vin[i].prevout;
      CCoins coins;
      if (!view.GetCoins (prevout.hash, coins))
        return error ("%s: failed to fetch input coins for %s", __func__, txid);

      const CNameScript op(coins.vout[prevout.n].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameIn != -1)
            return state.Invalid (error ("%s: multiple name inputs into"
                                         " transaction %s", __func__, txid));
          nameIn = i;
          nameOpIn = op;
          coinsIn = coins;
        }
    }

  int nameOut = -1;
  CNameScript nameOpOut;
  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameOut != -1)
            return state.Invalid (error ("%s: multiple name outputs from"
                                         " transaction %s", __func__, txid));
          nameOut = i;
          nameOpOut = op;
        }
    }

  /* Check that no name inputs/outputs are present for a non-Namecoin tx.
     If that's the case, all is fine.  For a Namecoin tx instead, there
     should be at least an output (for NAME_NEW, no inputs are expected).  */

  if (!tx.IsNamecoin ())
    {
      if (nameIn != -1)
        return state.Invalid (error ("%s: non-Namecoin tx %s has name inputs",
                                     __func__, txid));
      if (nameOut != -1)
        return state.Invalid (error ("%s: non-Namecoin tx %s at height %u"
                                     " has name outputs",
                                     __func__, txid, nHeight));

      return true;
    }

  assert (tx.IsNamecoin ());
  if (nameOut == -1)
    return state.Invalid (error ("%s: Namecoin tx %s has no name outputs",
                                 __func__, txid));

  /* Reject "greedy names".  */
  if (tx.vout[nameOut].nValue < Params().MinNameCoinAmount(nHeight))
    return state.Invalid (error ("%s: greedy name", __func__));

  /* Handle NAME_NEW now, since this is easy and different from the other
     operations.  */

  if (nameOpOut.getNameOp () == OP_NAME_NEW)
    {
      if (nameIn != -1)
        return state.Invalid (error ("CheckNameTransaction: NAME_NEW with"
                                     " previous name input"));

      if (nameOpOut.getOpHash ().size () != 20)
        return state.Invalid (error ("CheckNameTransaction: NAME_NEW's hash"
                                     " has wrong size"));

      return true;
    }

  /* Now that we have ruled out NAME_NEW, check that we have a previous
     name input that is being updated.  */

  assert (nameOpOut.isAnyUpdate ());
  if (nameIn == -1)
    return state.Invalid (error ("CheckNameTransaction: update without"
                                 " previous name input"));
  const valtype& name = nameOpOut.getOpName ();

  if (name.size () > MAX_NAME_LENGTH)
    return state.Invalid (error ("CheckNameTransaction: name too long"));
  if (nameOpOut.getOpValue ().size () > MAX_VALUE_LENGTH)
    return state.Invalid (error ("CheckNameTransaction: value too long"));

  /* Process NAME_UPDATE next.  */

  if (nameOpOut.getNameOp () == OP_NAME_UPDATE)
    {
      if (!nameOpIn.isAnyUpdate ())
        return state.Invalid (error ("CheckNameTransaction: NAME_UPDATE with"
                                     " prev input that is no update"));

      if (name != nameOpIn.getOpName ())
        return state.Invalid (error ("%s: NAME_UPDATE name mismatch to prev tx"
                                     " found in %s", __func__, txid));

      /* This is actually redundant, since expired names are removed
         from the UTXO set and thus not available to be spent anyway.
         But it does not hurt to enforce this here, too.  It is also
         exercised by the unit tests.  */
      if (isExpired (coinsIn.nHeight, nHeight))
        return state.Invalid (error ("CheckNameTransaction: trying to update"
                                     " expired name"));

      return true;
    }

  /* Finally, NAME_FIRSTUPDATE.  */

  assert (nameOpOut.getNameOp () == OP_NAME_FIRSTUPDATE);
  if (nameOpIn.getNameOp () != OP_NAME_NEW)
    return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                 " with non-NAME_NEW prev tx"));

  /* Maturity of NAME_NEW is checked only if we're not adding
     to the mempool.  */
  if (!fMempool)
    {
      assert (static_cast<unsigned> (coinsIn.nHeight) != MEMPOOL_HEIGHT);
      if (coinsIn.nHeight + MIN_FIRSTUPDATE_DEPTH > nHeight)
        return state.Invalid (error ("CheckNameTransaction: NAME_NEW"
                                     " is not mature for FIRST_UPDATE"));
    }

  if (nameOpOut.getOpRand ().size () > 20)
    return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                 " rand too large, %d bytes",
                                 nameOpOut.getOpRand ().size ()));

  {
    valtype toHash(nameOpOut.getOpRand ());
    toHash.insert (toHash.end (), name.begin (), name.end ());
    const uint160 hash = Hash160 (toHash);
    if (hash != uint160 (nameOpIn.getOpHash ()))
      return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                   " hash mismatch"));
  }

  CNameData oldName;
  if (view.GetName (name, oldName) && !oldName.isExpired (nHeight))
    return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                 " on an unexpired name"));

  /* We don't have to specifically check that miners don't create blocks with
     conflicting NAME_FIRSTUPDATE's, since the mining's CCoinsViewCache
     takes care of this with the check above already.  */

  return true;
}

void
ApplyNameTransaction (const CTransaction& tx, unsigned nHeight,
                      CCoinsViewCache& view, CBlockUndo& undo)
{
  assert (nHeight != MEMPOOL_HEIGHT);

  /* Handle historic bugs that should *not* be applied.  Names that are
     outputs should be marked as unspendable in this case.  Otherwise,
     we get an inconsistency between the UTXO set and the name database.  */
  CChainParams::BugType type;
  const uint256 txHash = tx.GetHash ();
  if (Params ().IsHistoricBug (txHash, nHeight, type)
      && type != CChainParams::BUG_FULLY_APPLY)
    {
      if (type == CChainParams::BUG_FULLY_IGNORE)
        {
          CCoinsModifier coins = view.ModifyCoins (txHash);
          for (unsigned i = 0; i < tx.vout.size (); ++i)
            {
              const CNameScript op(tx.vout[i].scriptPubKey);
              if (op.isNameOp () && op.isAnyUpdate ())
                {
                  if (!coins->IsAvailable (i) || !coins->Spend (i))
                    LogPrintf ("ERROR: %s : spending buggy name output failed",
                               __func__);
                }
            }
        }

      return;
    }

  /* This check must be done *after* the historic bug fixing above!  Some
     of the names that must be handled above are actually produced by
     transactions *not* marked as Namecoin tx.  */
  if (!tx.IsNamecoin ())
    return;

  /* Changes are encoded in the outputs.  We don't have to do any checks,
     so simply apply all these.  */

  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp () && op.isAnyUpdate ())
        {
          const valtype& name = op.getOpName ();
          if (fDebug)
            LogPrintf ("Updating name at height %d: %s\n",
                       nHeight, ValtypeToString (name).c_str ());

          CNameTxUndo opUndo;
          opUndo.fromOldState (name, view);
          undo.vnameundo.push_back (opUndo);

          CNameData data;
          data.fromScript (nHeight, COutPoint (tx.GetHash (), i), op);
          view.SetName (name, data, false);
        }
    }
}

bool
ExpireNames (unsigned nHeight, CCoinsViewCache& view, CBlockUndo& undo,
             std::set<valtype>& names)
{
  names.clear ();

  /* The genesis block contains no name expirations.  */
  if (nHeight == 0)
    return true;

  /* Otherwise, find out at which update heights names have expired
     since the last block.  If the expiration depth changes, this could
     be multiple heights at once.  */

  const unsigned expDepthOld = Params ().NameExpirationDepth (nHeight - 1);
  const unsigned expDepthNow = Params ().NameExpirationDepth (nHeight);

  if (expDepthNow > nHeight)
    return true;

  /* Both are inclusive!  The last expireTo was nHeight - 1 - expDepthOld,
     now we start at this value + 1.  */
  const unsigned expireFrom = nHeight - expDepthOld;
  const unsigned expireTo = nHeight - expDepthNow;

  /* It is possible that expireFrom = expireTo + 1, in case that the
     expiration period is raised together with the block height.  In this
     case, no names expire in the current step.  This case means that
     the absolute expiration height "n - expirationDepth(n)" is
     flat -- which is fine.  */
  assert (expireFrom <= expireTo + 1);

  /* Find all names that expire at those depths.  Note that GetNamesForHeight
     clears the output set, to we union all sets here.  */
  for (unsigned h = expireFrom; h <= expireTo; ++h)
    {
      std::set<valtype> newNames;
      view.GetNamesForHeight (h, newNames);
      names.insert (newNames.begin (), newNames.end ());
    }

  /* Expire all those names.  */
  for (std::set<valtype>::const_iterator i = names.begin ();
       i != names.end (); ++i)
    {
      const std::string nameStr = ValtypeToString (*i);

      CNameData data;
      if (!view.GetName (*i, data))
        return error ("%s : name '%s' not found in the database",
                      __func__, nameStr.c_str ());
      if (!data.isExpired (nHeight))
        return error ("%s : name '%s' is not actually expired",
                      __func__, nameStr.c_str ());

      /* Special rule:  When d/postmortem expires (the name used by
         libcoin in the name-stealing demonstration), it's coin
         is already spent.  Ignore.  */
      if (nHeight == 175868 && nameStr == "d/postmortem")
        continue;

      const COutPoint& out = data.getUpdateOutpoint ();
      CCoinsModifier coins = view.ModifyCoins (out.hash);

      if (!coins->IsAvailable (out.n))
        return error ("%s : name coin for '%s' is not available",
                      __func__, nameStr.c_str ());
      const CNameScript nameOp(coins->vout[out.n].scriptPubKey);
      if (!nameOp.isNameOp () || !nameOp.isAnyUpdate ()
          || nameOp.getOpName () != *i)
        return error ("%s : name coin to be expired is wrong script", __func__);

      CTxInUndo txUndo;
      if (!coins->Spend (out.n, &txUndo))
        return error ("%s : failed to spend name coin for '%s'",
                      __func__, nameStr.c_str ());
      undo.vexpired.push_back (txUndo);
    }

  return true;
}

bool
UnexpireNames (unsigned nHeight, const CBlockUndo& undo, CCoinsViewCache& view,
               std::set<valtype>& names)
{
  names.clear ();

  /* The genesis block contains no name expirations.  */
  if (nHeight == 0)
    return true;

  std::vector<CTxInUndo>::const_reverse_iterator i;
  for (i = undo.vexpired.rbegin (); i != undo.vexpired.rend (); ++i)
    {
      const CNameScript nameOp(i->txout.scriptPubKey);
      if (!nameOp.isNameOp () || !nameOp.isAnyUpdate ())
        return error ("%s : wrong script to be unexpired", __func__);

      const valtype& name = nameOp.getOpName ();
      if (names.count (name) > 0)
        return error ("%s : name '%s' unexpired twice",
                      __func__, ValtypeToString (name).c_str ());
      names.insert (name);

      CNameData data;
      if (!view.GetName (nameOp.getOpName (), data))
        return error ("%s : no data for name '%s' to be unexpired",
                      __func__, ValtypeToString (name).c_str ());
      if (!data.isExpired (nHeight) || data.isExpired (nHeight - 1))
        return error ("%s : name '%s' to be unexpired is not expired in the DB"
                      " or it was already expired before the current height",
                      __func__, ValtypeToString (name).c_str ());

      if (!ApplyTxInUndo (*i, view, data.getUpdateOutpoint ()))
        return error ("%s : failed to undo name coin spending", __func__);
    }

  return true;
}

void
CheckNameDB (bool disconnect)
{
  const int option = GetArg ("-checknamedb", Params ().DefaultCheckNameDB ());

  if (option == -1)
    return;

  assert (option >= 0);
  if (option != 0)
    {
      if (disconnect || chainActive.Height () % option != 0)
        return;
    }

  pcoinsTip->Flush ();
  const bool ok = pcoinsTip->ValidateNameDB ();

  /* The DB is inconsistent (mismatch between UTXO set and names DB) between
     (roughly) blocks 139,000 and 180,000.  This is caused by libcoin's
     "name stealing" bug.  For instance, d/postmortem is removed from
     the UTXO set shortly after registration (when it is used to steal
     names), but it remains in the name DB until it expires.  */
  if (!ok)
    {
      const unsigned nHeight = chainActive.Height ();
      LogPrintf ("ERROR: %s : name database is inconsistent\n", __func__);
      if (nHeight >= 139000 && nHeight <= 180000)
        LogPrintf ("This is expected due to 'name stealing'.\n");
      else
        assert (false);
    }
}
