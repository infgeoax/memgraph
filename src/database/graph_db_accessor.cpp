#include "glog/logging.h"

#include "database/graph_db_accessor.hpp"
#include "database/state_delta.hpp"
#include "storage/edge.hpp"
#include "storage/edge_accessor.hpp"
#include "storage/vertex.hpp"
#include "storage/vertex_accessor.hpp"
#include "utils/atomic.hpp"
#include "utils/on_scope_exit.hpp"

GraphDbAccessor::GraphDbAccessor(GraphDb &db)
    : db_(db), transaction_(MasterEngine().Begin()) {}

GraphDbAccessor::~GraphDbAccessor() {
  if (!commited_ && !aborted_) {
    this->Abort();
  }
}

tx::transaction_id_t GraphDbAccessor::transaction_id() const {
  return transaction_->id_;
}

void GraphDbAccessor::AdvanceCommand() {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  MasterEngine().Advance(transaction_->id_);
}

void GraphDbAccessor::Commit() {
  DCHECK(!commited_ && !aborted_) << "Already aborted or commited transaction.";
  MasterEngine().Commit(*transaction_);
  commited_ = true;
}

void GraphDbAccessor::Abort() {
  DCHECK(!commited_ && !aborted_) << "Already aborted or commited transaction.";
  MasterEngine().Abort(*transaction_);
  aborted_ = true;
}

bool GraphDbAccessor::should_abort() const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return transaction_->should_abort();
}

durability::WriteAheadLog &GraphDbAccessor::wal() { return db_.wal_; }

VertexAccessor GraphDbAccessor::InsertVertex(
    std::experimental::optional<gid::Gid> gid) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";

  std::experimental::optional<uint64_t> next_id;
  if (gid) {
    CHECK(static_cast<int>(gid::WorkerId(*gid)) == db_.worker_id_)
        << "Attempting to set incompatible worker id";
    next_id = gid::LocalId(*gid);
  }

  auto id = db_.vertex_generator_.Next(next_id);
  auto vertex_vlist = new mvcc::VersionList<Vertex>(*transaction_, id);

  bool success = db_.vertices_.access().insert(id, vertex_vlist).second;
  CHECK(success) << "Attempting to insert a vertex with an existing ID: " << id;
  db_.wal_.Emplace(database::StateDelta::CreateVertex(transaction_->id_,
                                                      vertex_vlist->gid_));
  return VertexAccessor(vertex_vlist, *this);
}

std::experimental::optional<VertexAccessor> GraphDbAccessor::FindVertex(
    gid::Gid gid, bool current_state) {
  auto collection_accessor = db_.vertices_.access();
  auto found = collection_accessor.find(gid);
  if (found == collection_accessor.end()) return std::experimental::nullopt;
  VertexAccessor record_accessor(found->second, *this);
  if (!record_accessor.Visible(transaction(), current_state))
    return std::experimental::nullopt;
  return record_accessor;
}

std::experimental::optional<EdgeAccessor> GraphDbAccessor::FindEdge(
    gid::Gid gid, bool current_state) {
  auto collection_accessor = db_.edges_.access();
  auto found = collection_accessor.find(gid);
  if (found == collection_accessor.end()) return std::experimental::nullopt;
  EdgeAccessor record_accessor(found->second, *this);
  if (!record_accessor.Visible(transaction(), current_state))
    return std::experimental::nullopt;
  return record_accessor;
}

void GraphDbAccessor::BuildIndex(const GraphDbTypes::Label &label,
                                 const GraphDbTypes::Property &property) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";

  db_.index_build_tx_in_progress_.access().insert(transaction_->id_);

  // on function exit remove the create index transaction from
  // build_tx_in_progress
  utils::OnScopeExit on_exit_1([this] {
    auto removed =
        db_.index_build_tx_in_progress_.access().remove(transaction_->id_);
    DCHECK(removed) << "Index creation transaction should be inside set";
  });

  const LabelPropertyIndex::Key key(label, property);
  if (db_.label_property_index_.CreateIndex(key) == false) {
    throw IndexExistsException(
        "Index is either being created by another transaction or already "
        "exists.");
  }

  // Everything that happens after the line above ended will be added to the
  // index automatically, but we still have to add to index everything that
  // happened earlier. We have to first wait for every transaction that
  // happend before, or a bit later than CreateIndex to end.
  {
    auto wait_transactions = db_.tx_engine_->GlobalActiveTransactions();
    auto active_index_creation_transactions =
        db_.index_build_tx_in_progress_.access();
    for (auto id : wait_transactions) {
      if (active_index_creation_transactions.contains(id)) continue;
      while (db_.tx_engine_->GlobalIsActive(id)) {
        // Active index creation set could only now start containing that id,
        // since that thread could have not written to the set set and to avoid
        // dead-lock we need to make sure we keep track of that
        if (active_index_creation_transactions.contains(id)) continue;
        // TODO reconsider this constant, currently rule-of-thumb chosen
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }

  // This accessor's transaction surely sees everything that happened before
  // CreateIndex.
  GraphDbAccessor dba(db_);

  // Add transaction to the build_tx_in_progress as this transaction doesn't
  // change data and shouldn't block other parallel index creations
  auto read_transaction_id = dba.transaction().id_;
  db_.index_build_tx_in_progress_.access().insert(read_transaction_id);
  // on function exit remove the read transaction from build_tx_in_progress
  utils::OnScopeExit on_exit_2([read_transaction_id, this] {
    auto removed =
        db_.index_build_tx_in_progress_.access().remove(read_transaction_id);
    DCHECK(removed) << "Index building (read) transaction should be inside set";
  });

  for (auto vertex : dba.Vertices(label, false)) {
    db_.label_property_index_.UpdateOnLabelProperty(vertex.address().local(),
                                                    vertex.current_);
  }
  // Commit transaction as we finished applying method on newest visible
  // records. Write that transaction's ID to the WAL as the index has been
  // built at this point even if this DBA's transaction aborts for some
  // reason.
  auto wal_build_index_tx_id = dba.transaction_id();
  dba.Commit();
  db_.wal_.Emplace(database::StateDelta::BuildIndex(
      wal_build_index_tx_id, LabelName(label), PropertyName(property)));

  // After these two operations we are certain that everything is contained in
  // the index under the assumption that this transaction contained no
  // vertex/edge insert/update before this method was invoked.
  db_.label_property_index_.IndexFinishedBuilding(key);
}

void GraphDbAccessor::UpdateLabelIndices(const GraphDbTypes::Label &label,
                                         const VertexAccessor &vertex_accessor,
                                         const Vertex *const vertex) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  DCHECK(vertex_accessor.is_local()) << "Only local vertices belong in indexes";
  auto *vlist_ptr = vertex_accessor.address().local();
  db_.labels_index_.Update(label, vlist_ptr, vertex);
  db_.label_property_index_.UpdateOnLabel(label, vlist_ptr, vertex);
}

void GraphDbAccessor::UpdatePropertyIndex(
    const GraphDbTypes::Property &property,
    const RecordAccessor<Vertex> &vertex_accessor, const Vertex *const vertex) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  DCHECK(vertex_accessor.is_local()) << "Only local vertices belong in indexes";
  db_.label_property_index_.UpdateOnProperty(
      property, vertex_accessor.address().local(), vertex);
}

int64_t GraphDbAccessor::VerticesCount() const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.vertices_.access().size();
}

int64_t GraphDbAccessor::VerticesCount(const GraphDbTypes::Label &label) const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.labels_index_.Count(label);
}

int64_t GraphDbAccessor::VerticesCount(
    const GraphDbTypes::Label &label,
    const GraphDbTypes::Property &property) const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  const LabelPropertyIndex::Key key(label, property);
  DCHECK(db_.label_property_index_.IndexExists(key)) << "Index doesn't exist.";
  return db_.label_property_index_.Count(key);
}

int64_t GraphDbAccessor::VerticesCount(const GraphDbTypes::Label &label,
                                       const GraphDbTypes::Property &property,
                                       const PropertyValue &value) const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  const LabelPropertyIndex::Key key(label, property);
  DCHECK(db_.label_property_index_.IndexExists(key)) << "Index doesn't exist.";
  return db_.label_property_index_.PositionAndCount(key, value).second;
}

int64_t GraphDbAccessor::VerticesCount(
    const GraphDbTypes::Label &label, const GraphDbTypes::Property &property,
    const std::experimental::optional<utils::Bound<PropertyValue>> lower,
    const std::experimental::optional<utils::Bound<PropertyValue>> upper)
    const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  const LabelPropertyIndex::Key key(label, property);
  DCHECK(db_.label_property_index_.IndexExists(key)) << "Index doesn't exist.";
  CHECK(lower || upper) << "At least one bound must be provided";
  CHECK(!lower || lower.value().value().type() != PropertyValue::Type::Null)
      << "Null value is not a valid index bound";
  CHECK(!upper || upper.value().value().type() != PropertyValue::Type::Null)
      << "Null value is not a valid index bound";

  if (!upper) {
    auto lower_pac =
        db_.label_property_index_.PositionAndCount(key, lower.value().value());
    int64_t size = db_.label_property_index_.Count(key);
    return std::max(0l,
                    size - lower_pac.first -
                        (lower.value().IsInclusive() ? 0l : lower_pac.second));

  } else if (!lower) {
    auto upper_pac =
        db_.label_property_index_.PositionAndCount(key, upper.value().value());
    return upper.value().IsInclusive() ? upper_pac.first + upper_pac.second
                                       : upper_pac.first;

  } else {
    auto lower_pac =
        db_.label_property_index_.PositionAndCount(key, lower.value().value());
    auto upper_pac =
        db_.label_property_index_.PositionAndCount(key, upper.value().value());
    auto result = upper_pac.first - lower_pac.first;
    if (lower.value().IsExclusive()) result -= lower_pac.second;
    if (upper.value().IsInclusive()) result += upper_pac.second;
    return std::max(0l, result);
  }
}

bool GraphDbAccessor::RemoveVertex(VertexAccessor &vertex_accessor) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";

  if (!vertex_accessor.is_local()) {
    LOG(ERROR) << "Remote vertex deletion not implemented";
    // TODO support distributed
    // call remote RemoveVertex(gid), return it's result. The result can be
    // (true, false), or an error can occur (serialization, timeout). In case
    // of error the remote worker will be asking for a transaction abort,
    // not sure what to do here.
    return false;
  }
  vertex_accessor.SwitchNew();
  // it's possible the vertex was removed already in this transaction
  // due to it getting matched multiple times by some patterns
  // we can only delete it once, so check if it's already deleted
  if (vertex_accessor.current().is_expired_by(*transaction_)) return true;
  if (vertex_accessor.out_degree() > 0 || vertex_accessor.in_degree() > 0)
    return false;

  auto *vlist_ptr = vertex_accessor.address().local();
  db_.wal_.Emplace(
      database::StateDelta::RemoveVertex(transaction_->id_, vlist_ptr->gid_));
  vlist_ptr->remove(vertex_accessor.current_, *transaction_);
  return true;
}

void GraphDbAccessor::DetachRemoveVertex(VertexAccessor &vertex_accessor) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  if (!vertex_accessor.is_local()) {
    LOG(ERROR) << "Remote vertex deletion not implemented";
    // TODO support distributed
    // call remote DetachRemoveVertex(gid). It can either succeed or an error
    // can occur. See discussion in the RemoveVertex method above.
  }
  vertex_accessor.SwitchNew();
  for (auto edge_accessor : vertex_accessor.in())
    RemoveEdge(edge_accessor, true, false);
  vertex_accessor.SwitchNew();
  for (auto edge_accessor : vertex_accessor.out())
    RemoveEdge(edge_accessor, false, true);

  vertex_accessor.SwitchNew();
  // it's possible the vertex was removed already in this transaction
  // due to it getting matched multiple times by some patterns
  // we can only delete it once, so check if it's already deleted
  if (!vertex_accessor.current().is_expired_by(*transaction_))
    vertex_accessor.address().local()->remove(vertex_accessor.current_,
                                              *transaction_);
}

EdgeAccessor GraphDbAccessor::InsertEdge(
    VertexAccessor &from, VertexAccessor &to, GraphDbTypes::EdgeType edge_type,
    std::experimental::optional<gid::Gid> gid) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  // An edge is created on the worker of it's "from" vertex.
  if (!from.is_local()) {
    LOG(ERROR) << "Remote edge insertion not implemented.";
    // TODO call remote InsertEdge(...)->gid. Possible outcomes are successful
    // creation or an error (serialization, timeout). If successful, create an
    // EdgeAccessor and return it. The remote InsertEdge(...) will be calling
    // remote Connect(...) if "to" is not local to it.
  }
  std::experimental::optional<uint64_t> next_id;
  if (gid) {
    CHECK(static_cast<int>(gid::WorkerId(*gid)) == db_.worker_id_)
        << "Attempting to set incompatible worker id";
    next_id = gid::LocalId(*gid);
  }

  auto id = db_.edge_generator_.Next(next_id);
  auto edge_vlist = new mvcc::VersionList<Edge>(
      *transaction_, id, from.address(), to.address(), edge_type);
  // We need to insert edge_vlist to edges_ before calling update since update
  // can throw and edge_vlist will not be garbage collected if it is not in
  // edges_ skiplist.
  bool success = db_.edges_.access().insert(id, edge_vlist).second;
  CHECK(success) << "Attempting to insert an edge with an existing ID: " << id;

  // ensure that the "from" accessor has the latest version
  from.SwitchNew();
  from.update().out_.emplace(to.address(), edge_vlist, edge_type);

  // It is possible that the "to" accessor is remote.
  if (to.is_local()) {
    // ensure that the "to" accessor has the latest version (Switch new)
    // WARNING: must do that after the above "from.update()" for cases when
    // we are creating a cycle and "from" and "to" are the same vlist
    to.SwitchNew();
    to.update().in_.emplace(from.address(), edge_vlist, edge_type);
  } else {
    LOG(ERROR) << "Connecting to a remote vertex not implemented.";
    // TODO call remote Connect(from_gid, edge_gid, to_gid, edge_type). Possible
    // outcomes are success or error (serialization, timeout).
  }
  db_.wal_.Emplace(database::StateDelta::CreateEdge(
      transaction_->id_, edge_vlist->gid_, from.gid(), to.gid(),
      EdgeTypeName(edge_type)));
  return EdgeAccessor(edge_vlist, *this, from.address(), to.address(),
                      edge_type);
}

int64_t GraphDbAccessor::EdgesCount() const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.edges_.access().size();
}

void GraphDbAccessor::RemoveEdge(EdgeAccessor &edge_accessor,
                                 bool remove_from_from, bool remove_from_to) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  if (!edge_accessor.is_local()) {
    LOG(ERROR) << "Remote edge deletion not implemented";
    // TODO support distributed
    // call remote RemoveEdge(gid, true, true). It can either succeed or an
    // error can occur. See discussion in the RemoveVertex method above.
  }
  // it's possible the edge was removed already in this transaction
  // due to it getting matched multiple times by some patterns
  // we can only delete it once, so check if it's already deleted
  edge_accessor.SwitchNew();
  if (edge_accessor.current().is_expired_by(*transaction_)) return;
  if (remove_from_from)
    edge_accessor.from().update().out_.RemoveEdge(edge_accessor.address());
  if (remove_from_to)
    edge_accessor.to().update().in_.RemoveEdge(edge_accessor.address());
  edge_accessor.address().local()->remove(edge_accessor.current_,
                                          *transaction_);
  db_.wal_.Emplace(
      database::StateDelta::RemoveEdge(transaction_->id_, edge_accessor.gid()));
}

GraphDbTypes::Label GraphDbAccessor::Label(const std::string &label_name) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.labels_->value_to_id(label_name);
}

const std::string &GraphDbAccessor::LabelName(
    const GraphDbTypes::Label label) const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.labels_->id_to_value(label);
}

GraphDbTypes::EdgeType GraphDbAccessor::EdgeType(
    const std::string &edge_type_name) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.edge_types_->value_to_id(edge_type_name);
}

const std::string &GraphDbAccessor::EdgeTypeName(
    const GraphDbTypes::EdgeType edge_type) const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.edge_types_->id_to_value(edge_type);
}

GraphDbTypes::Property GraphDbAccessor::Property(
    const std::string &property_name) {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.properties_->value_to_id(property_name);
}

const std::string &GraphDbAccessor::PropertyName(
    const GraphDbTypes::Property property) const {
  DCHECK(!commited_ && !aborted_) << "Accessor committed or aborted";
  return db_.properties_->id_to_value(property);
}

int64_t GraphDbAccessor::Counter(const std::string &name) {
  return db_.counters_.access()
      .emplace(name, std::make_tuple(name), std::make_tuple(0))
      .first->second.fetch_add(1);
}

void GraphDbAccessor::CounterSet(const std::string &name, int64_t value) {
  auto name_counter_pair = db_.counters_.access().emplace(
      name, std::make_tuple(name), std::make_tuple(value));
  if (!name_counter_pair.second) name_counter_pair.first->second.store(value);
}

std::vector<std::string> GraphDbAccessor::IndexInfo() const {
  std::vector<std::string> info;
  for (GraphDbTypes::Label label : db_.labels_index_.Keys()) {
    info.emplace_back(":" + LabelName(label));
  }
  for (LabelPropertyIndex::Key key : db_.label_property_index_.Keys()) {
    info.emplace_back(fmt::format(":{}({})", LabelName(key.label_),
                                  PropertyName(key.property_)));
  }
  return info;
}
auto &GraphDbAccessor::remote_vertices() { return remote_vertices_; }
auto &GraphDbAccessor::remote_edges() { return remote_edges_; }

template <>
GraphDbAccessor::RemoteCache<Vertex> &GraphDbAccessor::remote_elements() {
  return remote_vertices();
}

template <>
GraphDbAccessor::RemoteCache<Edge> &GraphDbAccessor::remote_elements() {
  return remote_edges();
}
