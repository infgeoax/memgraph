#include <limits>
#include <mutex>

#include "glog/logging.h"

#include "database/state_delta.hpp"
#include "transactions/engine_rpc_messages.hpp"
#include "transactions/engine_single_node.hpp"

namespace tx {

SingleNodeEngine::SingleNodeEngine(durability::WriteAheadLog *wal)
    : wal_(wal) {}

Transaction *SingleNodeEngine::Begin() {
  std::lock_guard<SpinLock> guard(lock_);

  transaction_id_t id{++counter_};
  auto t = new Transaction(id, active_, *this);
  active_.insert(id);
  store_.emplace(id, t);
  if (wal_) {
    wal_->Emplace(database::StateDelta::TxBegin(id));
  }
  return t;
}

command_id_t SingleNodeEngine::Advance(transaction_id_t id) {
  std::lock_guard<SpinLock> guard(lock_);

  auto it = store_.find(id);
  DCHECK(it != store_.end())
      << "Transaction::advance on non-existing transaction";

  Transaction *t = it->second.get();
  if (t->cid_ == std::numeric_limits<command_id_t>::max())
    throw TransactionError(
        "Reached maximum number of commands in this "
        "transaction.");

  return ++(t->cid_);
}

command_id_t SingleNodeEngine::UpdateCommand(transaction_id_t id) {
  std::lock_guard<SpinLock> guard(lock_);
  auto it = store_.find(id);
  DCHECK(it != store_.end())
      << "Transaction::advance on non-existing transaction";
  return it->second->cid_;
}

void SingleNodeEngine::Commit(const Transaction &t) {
  auto tx_id = t.id_;
  {
    std::lock_guard<SpinLock> guard(lock_);
    clog_.set_committed(tx_id);
    active_.remove(tx_id);
    if (wal_) {
      wal_->Emplace(database::StateDelta::TxCommit(tx_id));
    }
    store_.erase(store_.find(tx_id));
  }
  NotifyListeners(tx_id);
}

void SingleNodeEngine::Abort(const Transaction &t) {
  auto tx_id = t.id_;
  {
    std::lock_guard<SpinLock> guard(lock_);
    clog_.set_aborted(tx_id);
    active_.remove(tx_id);
    if (wal_) {
      wal_->Emplace(database::StateDelta::TxAbort(tx_id));
    }
    store_.erase(store_.find(tx_id));
  }
  NotifyListeners(tx_id);
}

CommitLog::Info SingleNodeEngine::Info(transaction_id_t tx) const {
  return clog_.fetch_info(tx);
}

Snapshot SingleNodeEngine::GlobalGcSnapshot() {
  std::lock_guard<SpinLock> guard(lock_);

  // No active transactions.
  if (active_.size() == 0) {
    auto snapshot_copy = active_;
    snapshot_copy.insert(counter_ + 1);
    return snapshot_copy;
  }

  // There are active transactions.
  auto snapshot_copy = store_.find(active_.front())->second->snapshot();
  snapshot_copy.insert(active_.front());
  return snapshot_copy;
}

Snapshot SingleNodeEngine::GlobalActiveTransactions() {
  std::lock_guard<SpinLock> guard(lock_);
  Snapshot active_transactions = active_;
  return active_transactions;
}

tx::transaction_id_t SingleNodeEngine::LocalLast() const {
  return counter_.load();
}

void SingleNodeEngine::LocalForEachActiveTransaction(
    std::function<void(Transaction &)> f) {
  std::lock_guard<SpinLock> guard(lock_);
  for (auto transaction : active_) {
    f(*store_.find(transaction)->second);
  }
}

tx::Transaction *SingleNodeEngine::RunningTransaction(
    tx::transaction_id_t tx_id) {
  std::lock_guard<SpinLock> guard(lock_);
  auto found = store_.find(tx_id);
  CHECK(found != store_.end())
      << "Can't return snapshot for an inactive transaction";
  return found->second.get();
}
}  // namespace tx
