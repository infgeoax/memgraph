// tests in this suite deal with edge cases in logical operator behavior
// that's not easily testable with single-phase testing. instead, for
// easy testing and latter readability they are tested end-to-end.

#include <optional>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "communication/result_stream_faker.hpp"
#include "database/single_node/graph_db.hpp"
#include "database/single_node/graph_db_accessor.hpp"
#include "query/interpreter.hpp"

DECLARE_bool(query_cost_planner);

class QueryExecution : public testing::Test {
 protected:
  std::optional<database::GraphDb> db_;
  std::optional<database::GraphDbAccessor> dba_;

  void SetUp() {
    db_.emplace();
    dba_.emplace(db_->Access());
  }

  void TearDown() {
    dba_ = std::nullopt;
    db_ = std::nullopt;
  }

  /** Commits the current transaction and refreshes the dba_
   * variable to hold a new accessor with a new transaction */
  void Commit() {
    dba_->Commit();
    dba_ = db_->Access();
  }

  /** Executes the query and returns the results.
   * Does NOT commit the transaction */
  auto Execute(const std::string &query) {
    query::DbAccessor query_dba(&*dba_);
    ResultStreamFaker<query::TypedValue> stream;
    auto results = query::Interpreter()(query, &query_dba, {}, false,
                                        utils::NewDeleteResource());
    stream.Header(results.header());
    results.PullAll(stream);
    stream.Summary(results.summary());
    return stream.GetResults();
  }
};

TEST_F(QueryExecution, MissingOptionalIntoExpand) {
  // validating bug where expanding from Null (due to a preceeding optional
  // match) exhausts the expansion cursor, even if it's input is still not
  // exhausted
  Execute(
      "CREATE (a:Person {id: 1}), (b:Person "
      "{id:2})-[:Has]->(:Dog)-[:Likes]->(:Food )");
  Commit();
  ASSERT_EQ(Execute("MATCH (n) RETURN n").size(), 4);

  auto Exec = [this](bool desc, const std::string &edge_pattern) {
    // this test depends on left-to-right query planning
    FLAGS_query_cost_planner = false;
    return Execute(std::string("MATCH (p:Person) WITH p ORDER BY p.id ") +
                   (desc ? "DESC " : "") +
                   "OPTIONAL MATCH (p)-->(d:Dog) WITH p, d "
                   "MATCH (d)" +
                   edge_pattern +
                   "(f:Food) "
                   "RETURN p, d, f")
        .size();
  };

  std::string expand = "-->";
  std::string variable = "-[*1]->";
  std::string bfs = "-[*bfs..1]->";

  EXPECT_EQ(Exec(false, expand), 1);
  EXPECT_EQ(Exec(true, expand), 1);
  EXPECT_EQ(Exec(false, variable), 1);
  EXPECT_EQ(Exec(true, bfs), 1);
  EXPECT_EQ(Exec(true, bfs), 1);
}

TEST_F(QueryExecution, EdgeUniquenessInOptional) {
  // Validating that an edge uniqueness check can't fail when the edge is Null
  // due to optonal match. Since edge-uniqueness only happens in one OPTIONAL
  // MATCH, we only need to check that scenario.
  Execute("CREATE (), ()-[:Type]->()");
  Commit();
  ASSERT_EQ(Execute("MATCH (n) RETURN n").size(), 3);
  EXPECT_EQ(Execute("MATCH (n) OPTIONAL MATCH (n)-[r1]->(), (n)-[r2]->() "
                    "RETURN n, r1, r2")
                .size(),
            3);
}
