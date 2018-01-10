#pragma once

#include <atomic>
#include <experimental/optional>
#include <unordered_map>

#include "communication/messaging/distributed.hpp"
#include "communication/rpc/rpc.hpp"
#include "durability/wal.hpp"
#include "threading/sync/spinlock.hpp"
#include "transactions/commit_log.hpp"
#include "transactions/engine.hpp"
#include "transactions/transaction.hpp"
#include "utils/exceptions.hpp"

namespace tx {

/** Indicates an error in transaction handling (currently
 * only command id overflow). */
class TransactionError : public utils::BasicException {
 public:
  using utils::BasicException::BasicException;
};

/** Single-node deployment transaction engine. Has complete functionality. */
class SingleNodeEngine : public Engine {
 public:
  /**
   * @param wal - Optional. If present, the Engine will write tx
   * Begin/Commit/Abort atomically (while under lock).
   */
  explicit SingleNodeEngine(durability::WriteAheadLog *wal = nullptr);

  /**
   * Begins a transaction and returns a pointer to
   * it's object.
   *
   * The transaction object is owned by this engine.
   * It will be released when the transaction gets
   * committted or aborted.
   */
  Transaction *Begin();

  /**
   * Advances the command on the transaction with the
   * given id.
   *
   * @param id - Transation id. That transaction must
   * be currently active.
   */
  void Advance(transaction_id_t id);

  /** Comits the given transaction. Deletes the transaction object, it's not
   * valid after this function executes. */
  void Commit(const Transaction &t);

  /** Aborts the given transaction. Deletes the transaction object, it's not
   * valid after this function executes. */
  void Abort(const Transaction &t);

  CommitLog::Info Info(transaction_id_t tx) const override;
  Snapshot GlobalGcSnapshot() override;
  Snapshot GlobalActiveTransactions() override;
  bool GlobalIsActive(transaction_id_t tx) const override;
  tx::transaction_id_t LocalLast() const override;
  void LocalForEachActiveTransaction(
      std::function<void(Transaction &)> f) override;

 protected:
  // Exposed for MasterEngine. Transaction for tx_id must be alive.
  Snapshot GetSnapshot(tx::transaction_id_t tx_id);

 private:
  std::atomic<transaction_id_t> counter_{0};
  CommitLog clog_;
  std::unordered_map<transaction_id_t, std::unique_ptr<Transaction>> store_;
  Snapshot active_;
  SpinLock lock_;
  // Optional. If present, the Engine will write tx Begin/Commit/Abort
  // atomically (while under lock).
  durability::WriteAheadLog *wal_{nullptr};
};
}  // namespace tx