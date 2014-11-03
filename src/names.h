// Copyright (c) 2014 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef H_BITCOIN_NAMES
#define H_BITCOIN_NAMES

#include "serialize.h"
#include "uint256.h"

#include "core/transaction.h"

#include "script/script.h"

#include <list>
#include <map>
#include <string>
#include <utility>

class CBlockUndo;
class CCoinsView;
class CCoinsViewCache;
class CNameScript;
class CLevelDBBatch;
class CTxMemPool;
class CTxMemPoolEntry;
class CValidationState;

/* Some constants defining name limits.  */
static const unsigned MAX_VALUE_LENGTH = 1023;
static const unsigned MAX_NAME_LENGTH = 255;
static const unsigned MIN_FIRSTUPDATE_DEPTH = 12;
static const unsigned MAX_VALUE_LENGTH_UI = 520;

/**
 * Construct a valtype (e. g., name) from a string.
 * @param str The string input.
 * @return The corresponding valtype.
 */
inline valtype
ValtypeFromString (const std::string& str)
{
  return valtype (str.begin (), str.end ());
}

/**
 * Convert a valtype to a string.
 * @param val The valtype value.
 * @return Corresponding string.
 */
inline std::string
ValtypeToString (const valtype& val)
{
  return std::string (val.begin (), val.end ());
}

/* ************************************************************************** */
/* CNameData.  */

/**
 * Information stored for a name in the database.
 */
class CNameData
{

private:

  /** The name's value.  */
  valtype value;

  /** The transaction's height.  Used for expiry.  */
  unsigned nHeight;

  /** The name's last update outpoint.  */
  COutPoint prevout;

  /**
   * The name's address (as script).  This is kept here also, because
   * that information is useful to extract on demand (e. g., in name_show).
   */
  CScript addr;

public:

  ADD_SERIALIZE_METHODS;

  template<typename Stream, typename Operation>
    inline void SerializationOp (Stream& s, Operation ser_action,
                                 int nType, int nVersion)
  {
    READWRITE (value);
    READWRITE (nHeight);
    READWRITE (prevout);
    READWRITE (addr);
  }

  /* Compare for equality.  */
  friend inline bool
  operator== (const CNameData& a, const CNameData& b)
  {
    return a.value == b.value && a.nHeight == b.nHeight
            && a.prevout == b.prevout && a.addr == b.addr;
  }
  friend inline bool
  operator!= (const CNameData& a, const CNameData& b)
  {
    return !(a == b);
  }

  /**
   * Get the height.
   * @return The name's update height.
   */
  inline unsigned
  getHeight () const
  {
    return nHeight;
  }

  /**
   * Get the value.
   * @return The name's value.
   */
  inline const valtype&
  getValue () const
  {
    return value;
  }

  /**
   * Get the name's update outpoint.
   * @return The update outpoint.
   */
  inline const COutPoint&
  getUpdateOutpoint () const
  {
    return prevout;
  }

  /**
   * Get the address.
   * @return The name's address.
   */
  inline const CScript&
  getAddress () const
  {
    return addr;
  }

  /**
   * Check if the name is expired at the current chain height.
   * @return True iff the name is expired.
   */
  bool isExpired () const;

  /**
   * Check if the name is expired at the given height.
   * @param h The height at which to check.
   * @return True iff the name is expired at height h.
   */
  bool isExpired (unsigned h) const;

  /**
   * Set from a name update operation.
   * @param h The height (not available from script).
   * @param out The update outpoint.
   * @param script The name script.  Should be a name (first) update.
   */
  void fromScript (unsigned h, const COutPoint& out, const CNameScript& script);

};

/* ************************************************************************** */
/* CNameCache.  */

/**
 * Cache / record of updates to the name database.  In addition to
 * new names (or updates to them), this also keeps track of deleted names
 * (when rolling back changes).
 */
class CNameCache
{

public:

  /** Type for expire-index entries.  */
  typedef std::pair<unsigned, valtype> ExpireEntry;

private:

  /** New or updated names.  */
  std::map<valtype, CNameData> entries;
  /** Deleted names.  */
  std::set<valtype> deleted;

  /**
   * Changes to be performed to the expire index.  The entry is mapped
   * to either "true" (meaning to add it) or "false" (delete).
   */
  std::map<ExpireEntry, bool> expireIndex;

public:

  CNameCache ()
    : entries(), deleted(), expireIndex()
  {}

  inline void
  clear ()
  {
    entries.clear ();
    deleted.clear ();
    expireIndex.clear ();
  }

  /* See if the given name is marked as deleted.  */
  inline bool
  isDeleted (const valtype& name) const
  {
    return (deleted.count (name) > 0); 
  }

  /* Try to get a name's associated data.  This looks only
     in entries, and doesn't care about deleted data.  */
  bool get (const valtype& name, CNameData& data) const;

  /* Query the cached changes to the expire index.  In particular,
     for a given height and a given set of names that were indexed to
     this update height, apply possible changes to the set that
     are represented by the cached expire index changes.  */
  void updateNamesForHeight (unsigned nHeight, std::set<valtype>& names) const;

  /* Insert (or update) a name.  If it is marked as "deleted", this also
     removes the "deleted" mark.  */
  void set (const valtype& name, const CNameData& data);

  /* Delete a name.  If it is in the "entries" set also, remove it there.  */
  void remove (const valtype& name);

  /* Add an expire-index entry.  */
  void addExpireIndex (const valtype& name, unsigned height);

  /* Remove an expire-index entry.  */
  void removeExpireIndex (const valtype& name, unsigned height);

  /* Apply all the changes in the passed-in record on top of this one.  */
  void apply (const CNameCache& cache);

  /* Write all cached changes to a database batch update object.  */
  void writeBatch (CLevelDBBatch& batch) const;

};

/* ************************************************************************** */
/* CNameTxUndo.  */

/**
 * Undo information for one name operation.  This contains either the
 * information that the name was newly created (and should thus be
 * deleted entirely) or that it was updated including the old value.
 */
class CNameTxUndo
{

private:

  /** The name this concerns.  */
  valtype name;

  /** Whether this was an entirely new name (no update).  */
  bool isNew;

  /** The old name value that was overwritten by the operation.  */
  CNameData oldData;

public:

  ADD_SERIALIZE_METHODS;

  template<typename Stream, typename Operation>
    inline void SerializationOp (Stream& s, Operation ser_action,
                                 int nType, int nVersion)
  {
    READWRITE (name);
    READWRITE (isNew);
    if (!isNew)
      READWRITE (oldData);
  }

  /**
   * Set the data for an update/registration of the given name.  The CCoinsView
   * is used to find out all the necessary information.
   * @param nm The name that is being updated.
   * @param view The (old!) chain state.
   */
  void fromOldState (const valtype& nm, const CCoinsView& view);

  /**
   * Apply the undo to the chain state given.
   * @param view The chain state to update ("undo").
   */
  void apply (CCoinsViewCache& view) const;

};

/* ************************************************************************** */
/* CNameMemPool.  */

/**
 * Handle the name component of the transaction mempool.  This keeps track
 * of name operations that are in the mempool and ensures that all transactions
 * kept are consistent.  E. g., no two transactions are allowed to register
 * the same name, and name registration transactions are removed if a
 * conflicting registration makes it into a block.
 */
class CNameMemPool
{

private:

  /** The parent mempool object.  Used to, e. g., remove conflicting tx.  */
  CTxMemPool& pool;

  /**
   * Keep track of names that are registered by transactions in the pool.
   * Map name to registering transaction.
   */
  std::map<valtype, uint256> mapNameRegs;

public:

  /**
   * Construct with reference to parent mempool.
   * @param p The parent pool.
   */
  explicit inline CNameMemPool (CTxMemPool& p)
    : pool(p), mapNameRegs()
  {}

  /**
   * Check whether a particular name is being registered by
   * some transaction in the mempool.  Does not lock, this is
   * done by the parent mempool (which calls through afterwards).
   * @param name The name to check for.
   * @return True iff there's a matching name registration in the pool.
   */
  inline bool
  registersName (const valtype& name) const
  {
    return mapNameRegs.count (name) > 0;
  }

  /**
   * Clear all data.
   */
  inline void
  clear ()
  {
    mapNameRegs.clear ();
  }

  /**
   * Add an entry without checking it.  It should have been checked
   * already.  If this conflicts with the mempool, it may throw.
   * @param hash The tx hash.
   * @param entry The new mempool entry.
   */
  void addUnchecked (const uint256& hash, const CTxMemPoolEntry& entry);

  /**
   * Remove the given mempool entry.  It is assumed that it is present.
   * @param entry The entry to remove.
   */
  void remove (const CTxMemPoolEntry& entry);

  /**
   * Remove conflicts for the given tx, based on name operations.  I. e.,
   * if the tx registers a name that conflicts with another registration
   * in the mempool, detect this and remove the mempool tx accordingly.
   * @param tx The transaction for which we look for conflicts.
   * @param removed Put removed tx here.
   */
  void removeConflicts (const CTransaction& tx,
                        std::list<CTransaction>& removed);

  /**
   * Perform sanity checks.  Throws if it fails.
   * @param coins The coins view this represents.
   */
  void check (const CCoinsView& coins) const;

  /**
   * Check if a tx can be added (based on name criteria) without
   * causing a conflict.
   * @param tx The transaction to check.
   * @return True if it doesn't conflict.
   */
  bool checkTx (const CTransaction& tx) const;

};

/* ************************************************************************** */

/**
 * Check a transaction according to the additional Namecoin rules.  This
 * ensures that all name operations (if any) are valid and that it has
 * name operations iff it is marked as Namecoin tx by its version.
 * @param tx The transaction to check.
 * @param nHeight Height at which the tx will be.  May be MEMPOOL_HEIGHT.
 * @param view The current chain state.
 * @param state Resulting validation state.
 * @return True in case of success.
 */
bool CheckNameTransaction (const CTransaction& tx, unsigned nHeight,
                           const CCoinsView& view,
                           CValidationState& state);

/**
 * Apply the changes of a name transaction to the name database.
 * @param tx The transaction to apply.
 * @param nHeight Height at which the tx is.  Used for CNameData.
 * @param view The chain state to update.
 * @param undo Record undo information here.
 * @return True in case of success.
 */
void ApplyNameTransaction (const CTransaction& tx, unsigned nHeight,
                           CCoinsViewCache& view, CBlockUndo& undo);

/**
 * Check the name database consistency.  This calls CCoinsView::ValidateNameDB,
 * but only if applicable depending on the -checknamedb setting.  If it fails,
 * this throws an assertion failure.
 * @param disconnect Whether we are disconnecting blocks.
 */
void CheckNameDB (bool disconnect);

#endif // H_BITCOIN_NAMES
