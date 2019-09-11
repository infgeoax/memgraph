#pragma once

#include <gflags/gflags.h>

#include "query/context.hpp"
#include "query/db_accessor.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/ast/cypher_main_visitor.hpp"
#include "query/frontend/stripped.hpp"
#include "query/interpret/frame.hpp"
#include "query/plan/operator.hpp"
#include "utils/skip_list.hpp"
#include "utils/spin_lock.hpp"
#include "utils/timer.hpp"

DECLARE_bool(query_cost_planner);
DECLARE_int32(query_plan_cache_ttl);

namespace auth {
class Auth;
}  // namespace auth

namespace integrations::kafka {
class Streams;
}  // namespace integrations::kafka

namespace query {

// TODO: Maybe this should move to query/plan/planner.
/// Interface for accessing the root operator of a logical plan.
class LogicalPlan {
 public:
  virtual ~LogicalPlan() {}

  virtual const plan::LogicalOperator &GetRoot() const = 0;
  virtual double GetCost() const = 0;
  virtual const SymbolTable &GetSymbolTable() const = 0;
  virtual const AstStorage &GetAstStorage() const = 0;
};

class Interpreter {
 private:
  class CachedPlan {
   public:
    CachedPlan(std::unique_ptr<LogicalPlan> plan);

    const auto &plan() const { return plan_->GetRoot(); }
    double cost() const { return plan_->GetCost(); }
    const auto &symbol_table() const { return plan_->GetSymbolTable(); }
    const auto &ast_storage() const { return plan_->GetAstStorage(); }

    bool IsExpired() const {
      return cache_timer_.Elapsed() >
             std::chrono::seconds(FLAGS_query_plan_cache_ttl);
    };

   private:
    std::unique_ptr<LogicalPlan> plan_;
    utils::Timer cache_timer_;
  };

  struct CachedQuery {
    AstStorage ast_storage;
    Query *query;
    std::vector<AuthQuery::Privilege> required_privileges;
  };

 public:
  /**
   * Wraps a `Query` that was created as a result of parsing a query string
   * along with its privileges.
   */
  struct ParsedQuery {
    Query *query;
    std::vector<AuthQuery::Privilege> required_privileges;
  };

  /**
   * Encapsulates all what's necessary for the interpretation of a query
   * into a single object that can be pulled (into the given Stream).
   */
  class Results {
    friend Interpreter;
    Results(DbAccessor *db_accessor, const query::Parameters &parameters,
            std::shared_ptr<CachedPlan> plan,
            std::vector<Symbol> output_symbols, std::vector<std::string> header,
            std::map<std::string, TypedValue> summary,
            std::vector<AuthQuery::Privilege> privileges,
            utils::MemoryResource *execution_memory,
            bool is_profile_query = false, bool should_abort_query = false)
        : ctx_{db_accessor},
          plan_(plan),
          cursor_(plan_->plan().MakeCursor(execution_memory)),
          frame_(plan_->symbol_table().max_position(), execution_memory),
          output_symbols_(std::move(output_symbols)),
          header_(std::move(header)),
          summary_(std::move(summary)),
          privileges_(std::move(privileges)),
          should_abort_query_(should_abort_query) {
      ctx_.is_profile_query = is_profile_query;
      ctx_.symbol_table = plan_->symbol_table();
      ctx_.evaluation_context.timestamp =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      ctx_.evaluation_context.parameters = parameters;
      ctx_.evaluation_context.properties =
          NamesToProperties(plan_->ast_storage().properties_, db_accessor);
      ctx_.evaluation_context.labels =
          NamesToLabels(plan_->ast_storage().labels_, db_accessor);
    }

   public:
    Results(const Results &) = delete;
    Results(Results &&) = default;
    Results &operator=(const Results &) = delete;
    Results &operator=(Results &&) = default;

    /**
     * Make the interpreter perform a single Pull. Results (if they exists) are
     * pushed into the given stream. On first Pull the header is written to the
     * stream, on last the summary.
     *
     * @param stream - The stream to push the header, results and summary into.
     * @return - If this Results is eligible for another Pull. If Pulling
     * after `false` has been returned, the behavior is undefined.
     * @tparam TStream - Stream type.
     */
    template <typename TStream>
    bool Pull(TStream &stream) {
      utils::Timer timer;
      // Setup temporary memory for a single Pull. Initial memory should come
      // from stack, 256 KiB should fit on the stack and should be more than
      // enough for a single Pull.
      constexpr size_t stack_size = 256 * 1024;
      char stack_data[stack_size];
      utils::MonotonicBufferResource memory(&stack_data[0], stack_size);
      ctx_.evaluation_context.memory = &memory;
      // We can now Pull a result.
      bool return_value = cursor_->Pull(frame_, ctx_);
      if (return_value && !output_symbols_.empty()) {
        // TODO: The streamed values should also probably use the above memory.
        std::vector<TypedValue> values;
        values.reserve(output_symbols_.size());
        for (const auto &symbol : output_symbols_) {
          values.emplace_back(frame_[symbol]);
        }
        stream.Result(values);
      }
      execution_time_ += timer.Elapsed().count();

      if (!return_value) {
        summary_["plan_execution_time"] = execution_time_;

        if (ctx_.is_profile_query) {
          summary_["profile"] =
              ProfilingStatsToJson(ctx_.stats, ctx_.profile_execution_time)
                  .dump();
        }

        cursor_->Shutdown();
      }

      return return_value;
    }

    /** Calls Pull() until exhausted. */
    template <typename TStream>
    void PullAll(TStream &stream) {
      while (Pull(stream)) continue;
    }

    const std::vector<std::string> &header() const & { return header_; }
    std::vector<std::string> &&header() && { return std::move(header_); }
    const std::map<std::string, TypedValue> &summary() const & {
      return summary_;
    }
    std::map<std::string, TypedValue> &&summary() && {
      return std::move(summary_);
    }

    const std::vector<AuthQuery::Privilege> &privileges() {
      return privileges_;
    }

    bool ShouldAbortQuery() const { return should_abort_query_; }

   private:
    ExecutionContext ctx_;
    std::shared_ptr<CachedPlan> plan_;
    query::plan::UniqueCursorPtr cursor_;
    Frame frame_;
    std::vector<Symbol> output_symbols_;

    std::vector<std::string> header_;
    std::map<std::string, TypedValue> summary_;

    double execution_time_{0};

    std::vector<AuthQuery::Privilege> privileges_;

    bool should_abort_query_;
  };

  Interpreter();
  Interpreter(const Interpreter &) = delete;
  Interpreter &operator=(const Interpreter &) = delete;
  Interpreter(Interpreter &&) = delete;
  Interpreter &operator=(Interpreter &&) = delete;

  virtual ~Interpreter() {}

  /**
   * Generates an Results object for the parameters. The resulting object
   * can be Pulled with its results written to an arbitrary stream.
   */
  virtual Results operator()(const std::string &query, DbAccessor *db_accessor,
                             const std::map<std::string, PropertyValue> &params,
                             bool in_explicit_transaction,
                             utils::MemoryResource *execution_memory);

  auth::Auth *auth_ = nullptr;
  integrations::kafka::Streams *kafka_streams_ = nullptr;

 protected:
  std::pair<frontend::StrippedQuery, ParsedQuery> StripAndParseQuery(
      const std::string &, Parameters *, AstStorage *ast_storage,
      const std::map<std::string, PropertyValue> &);

  // high level tree -> logical plan
  // AstStorage and SymbolTable may be modified during planning. The created
  // LogicalPlan must take ownership of AstStorage and SymbolTable.
  virtual std::unique_ptr<LogicalPlan> MakeLogicalPlan(CypherQuery *,
                                                       AstStorage,
                                                       const Parameters &,
                                                       DbAccessor *);

  virtual void PrettyPrintPlan(const DbAccessor &,
                               const plan::LogicalOperator *, std::ostream *);

  virtual std::string PlanToJson(const DbAccessor &,
                                 const plan::LogicalOperator *);

 private:
  struct QueryCacheEntry {
    bool operator==(const QueryCacheEntry &other) const {
      return first == other.first;
    }
    bool operator<(const QueryCacheEntry &other) const {
      return first < other.first;
    }
    bool operator==(const HashType &other) const { return first == other; }
    bool operator<(const HashType &other) const { return first < other; }

    HashType first;
    // TODO: Maybe store the query string here and use it as a key with the hash
    // so that we eliminate the risk of hash collisions.
    CachedQuery second;
  };

  struct PlanCacheEntry {
    bool operator==(const PlanCacheEntry &other) const {
      return first == other.first;
    }
    bool operator<(const PlanCacheEntry &other) const {
      return first < other.first;
    }
    bool operator==(const HashType &other) const { return first == other; }
    bool operator<(const HashType &other) const { return first < other; }

    HashType first;
    // TODO: Maybe store the query string here and use it as a key with the hash
    // so that we eliminate the risk of hash collisions.
    std::shared_ptr<CachedPlan> second;
  };

  utils::SkipList<QueryCacheEntry> ast_cache_;
  utils::SkipList<PlanCacheEntry> plan_cache_;

  // Antlr has singleton instance that is shared between threads. It is
  // protected by locks inside of antlr. Unfortunately, they are not protected
  // in a very good way. Once we have antlr version without race conditions we
  // can remove this lock. This will probably never happen since antlr
  // developers introduce more bugs in each version. Fortunately, we have cache
  // so this lock probably won't impact performance much...
  utils::SpinLock antlr_lock_;
  bool is_tsc_available_;

  // high level tree -> CachedPlan
  std::shared_ptr<CachedPlan> CypherQueryToPlan(HashType query_hash,
                                                CypherQuery *query,
                                                AstStorage ast_storage,
                                                const Parameters &parameters,
                                                DbAccessor *db_accessor);

  // stripped query -> high level tree
  ParsedQuery ParseQuery(const std::string &stripped_query,
                         const std::string &original_query,
                         const frontend::ParsingContext &context,
                         AstStorage *ast_storage);
};

}  // namespace query
