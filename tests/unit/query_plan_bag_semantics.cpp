//
// Copyright 2017 Memgraph
// Created by Florijan Stamenkovic on 14.03.17.
//

#include <algorithm>
#include <iterator>
#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "communication/result_stream_faker.hpp"
#include "query/context.hpp"
#include "query/exceptions.hpp"
#include "query/plan/operator.hpp"

#include "query_plan_common.hpp"

using namespace query;
using namespace query::plan;

TEST(QueryPlan, Skip) {
  database::GraphDb db;
  auto dba = db.Access();

  AstStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n1");
  auto skip = std::make_shared<plan::Skip>(n.op_, LITERAL(2));

  query::DbAccessor execution_dba(&dba);
  auto context = MakeContext(storage, symbol_table, &execution_dba);
  EXPECT_EQ(0, PullAll(*skip, &context));

  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(0, PullAll(*skip, &context));

  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(0, PullAll(*skip, &context));

  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(1, PullAll(*skip, &context));

  for (int i = 0; i < 10; ++i) dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(11, PullAll(*skip, &context));
}

TEST(QueryPlan, Limit) {
  database::GraphDb db;
  auto dba = db.Access();

  AstStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n1");
  auto skip = std::make_shared<plan::Limit>(n.op_, LITERAL(2));

  query::DbAccessor execution_dba(&dba);
  auto context = MakeContext(storage, symbol_table, &execution_dba);
  EXPECT_EQ(0, PullAll(*skip, &context));

  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(1, PullAll(*skip, &context));

  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(2, PullAll(*skip, &context));

  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(2, PullAll(*skip, &context));

  for (int i = 0; i < 10; ++i) dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(2, PullAll(*skip, &context));
}

TEST(QueryPlan, CreateLimit) {
  // CREATE (n), (m)
  // MATCH (n) CREATE (m) LIMIT 1
  // in the end we need to have 3 vertices in the db
  database::GraphDb db;
  auto dba = db.Access();
  dba.InsertVertex();
  dba.InsertVertex();
  dba.AdvanceCommand();

  AstStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n1");
  NodeCreationInfo m;
  m.symbol = symbol_table.CreateSymbol("m", true);
  auto c = std::make_shared<CreateNode>(n.op_, m);
  auto skip = std::make_shared<plan::Limit>(c, LITERAL(1));

  query::DbAccessor execution_dba(&dba);
  auto context = MakeContext(storage, symbol_table, &execution_dba);
  EXPECT_EQ(1, PullAll(*skip, &context));
  dba.AdvanceCommand();
  EXPECT_EQ(3, CountIterable(dba.Vertices(false)));
}

TEST(QueryPlan, OrderBy) {
  database::GraphDb db;
  auto dba = db.Access();
  AstStorage storage;
  SymbolTable symbol_table;
  auto prop = dba.Property("prop");

  // contains a series of tests
  // each test defines the ordering a vector of values in the desired order
  auto Null = PropertyValue();
  std::vector<std::pair<Ordering, std::vector<PropertyValue>>> orderable{
      {Ordering::ASC,
       {PropertyValue(0), PropertyValue(0), PropertyValue(0.5),
        PropertyValue(1), PropertyValue(2), PropertyValue(12.6),
        PropertyValue(42), Null, Null}},
      {Ordering::ASC,
       {PropertyValue(false), PropertyValue(false), PropertyValue(true),
        PropertyValue(true), Null, Null}},
      {Ordering::ASC,
       {PropertyValue("A"), PropertyValue("B"), PropertyValue("a"),
        PropertyValue("a"), PropertyValue("aa"), PropertyValue("ab"),
        PropertyValue("aba"), Null, Null}},
      {Ordering::DESC,
       {Null, Null, PropertyValue(33), PropertyValue(33), PropertyValue(32.5),
        PropertyValue(32), PropertyValue(2.2), PropertyValue(2.1),
        PropertyValue(0)}},
      {Ordering::DESC, {Null, PropertyValue(true), PropertyValue(false)}},
      {Ordering::DESC, {Null, PropertyValue("zorro"), PropertyValue("borro")}}};

  for (const auto &order_value_pair : orderable) {
    std::vector<TypedValue> values;
    values.reserve(order_value_pair.second.size());
    for (const auto &v : order_value_pair.second) values.emplace_back(v);
    // empty database
    for (auto &vertex : dba.Vertices(false)) dba.DetachRemoveVertex(vertex);
    dba.AdvanceCommand();
    ASSERT_EQ(0, CountIterable(dba.Vertices(false)));

    // take some effort to shuffle the values
    // because we are testing that something not ordered gets ordered
    // and need to take care it does not happen by accident
    auto shuffled = values;
    auto order_equal = [&values, &shuffled]() {
      return std::equal(values.begin(), values.end(), shuffled.begin(),
                        TypedValue::BoolEqual{});
    };
    for (int i = 0; i < 50 && order_equal(); ++i) {
      std::random_shuffle(shuffled.begin(), shuffled.end());
    }
    ASSERT_FALSE(order_equal());

    // create the vertices
    for (const auto &value : shuffled)
      dba.InsertVertex().PropsSet(prop, PropertyValue(value));
    dba.AdvanceCommand();

    // order by and collect results
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto n_p = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop);
    auto order_by = std::make_shared<plan::OrderBy>(
        n.op_, std::vector<SortItem>{{order_value_pair.first, n_p}},
        std::vector<Symbol>{n.sym_});
    auto n_p_ne =
        NEXPR("n.p", n_p)->MapTo(symbol_table.CreateSymbol("n.p", true));
    auto produce = MakeProduce(order_by, n_p_ne);
    query::DbAccessor execution_dba(&dba);
    auto context = MakeContext(storage, symbol_table, &execution_dba);
    auto results = CollectProduce(*produce, &context);
    ASSERT_EQ(values.size(), results.size());
    for (int j = 0; j < results.size(); ++j)
      EXPECT_TRUE(TypedValue::BoolEqual{}(results[j][0], values[j]));
  }
}

TEST(QueryPlan, OrderByMultiple) {
  database::GraphDb db;
  auto dba = db.Access();
  AstStorage storage;
  SymbolTable symbol_table;

  auto p1 = dba.Property("p1");
  auto p2 = dba.Property("p2");

  // create a bunch of vertices that in two properties
  // have all the variations (with repetition) of N values.
  // ensure that those vertices are not created in the
  // "right" sequence, but randomized
  const int N = 20;
  std::vector<std::pair<int, int>> prop_values;
  for (int i = 0; i < N * N; ++i) prop_values.emplace_back(i % N, i / N);
  std::random_shuffle(prop_values.begin(), prop_values.end());
  for (const auto &pair : prop_values) {
    auto v = dba.InsertVertex();
    v.PropsSet(p1, PropertyValue(pair.first));
    v.PropsSet(p2, PropertyValue(pair.second));
  }
  dba.AdvanceCommand();

  // order by and collect results
  auto n = MakeScanAll(storage, symbol_table, "n");
  auto n_p1 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), p1);
  auto n_p2 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), p2);
  // order the results so we get
  // (p1: 0, p2: N-1)
  // (p1: 0, p2: N-2)
  // ...
  // (p1: N-1, p2:0)
  auto order_by = std::make_shared<plan::OrderBy>(n.op_,
                                                  std::vector<SortItem>{
                                                      {Ordering::ASC, n_p1},
                                                      {Ordering::DESC, n_p2},
                                                  },
                                                  std::vector<Symbol>{n.sym_});
  auto n_p1_ne =
      NEXPR("n.p1", n_p1)->MapTo(symbol_table.CreateSymbol("n.p1", true));
  auto n_p2_ne =
      NEXPR("n.p2", n_p2)->MapTo(symbol_table.CreateSymbol("n.p2", true));
  auto produce = MakeProduce(order_by, n_p1_ne, n_p2_ne);
  query::DbAccessor execution_dba(&dba);
  auto context = MakeContext(storage, symbol_table, &execution_dba);
  auto results = CollectProduce(*produce, &context);
  ASSERT_EQ(N * N, results.size());
  for (int j = 0; j < N * N; ++j) {
    ASSERT_EQ(results[j][0].type(), TypedValue::Type::Int);
    EXPECT_EQ(results[j][0].ValueInt(), j / N);
    ASSERT_EQ(results[j][1].type(), TypedValue::Type::Int);
    EXPECT_EQ(results[j][1].ValueInt(), N - 1 - j % N);
  }
}

TEST(QueryPlan, OrderByExceptions) {
  database::GraphDb db;
  auto dba = db.Access();
  AstStorage storage;
  SymbolTable symbol_table;
  auto prop = dba.Property("prop");

  // a vector of pairs of typed values that should result
  // in an exception when trying to order on them
  std::vector<std::pair<PropertyValue, PropertyValue>> exception_pairs{
      {PropertyValue(42), PropertyValue(true)},
      {PropertyValue(42), PropertyValue("bla")},
      {PropertyValue(42),
       PropertyValue(std::vector<PropertyValue>{PropertyValue(42)})},
      {PropertyValue(true), PropertyValue("bla")},
      {PropertyValue(true),
       PropertyValue(std::vector<PropertyValue>{PropertyValue(true)})},
      {PropertyValue("bla"),
       PropertyValue(std::vector<PropertyValue>{PropertyValue("bla")})},
      // illegal comparisons of same-type values
      {PropertyValue(std::vector<PropertyValue>{PropertyValue(42)}),
       PropertyValue(std::vector<PropertyValue>{PropertyValue(42)})}};

  for (const auto &pair : exception_pairs) {
    // empty database
    for (auto &vertex : dba.Vertices(false)) dba.DetachRemoveVertex(vertex);
    dba.AdvanceCommand();
    ASSERT_EQ(0, CountIterable(dba.Vertices(false)));

    // make two vertices, and set values
    dba.InsertVertex().PropsSet(prop, pair.first);
    dba.InsertVertex().PropsSet(prop, pair.second);
    dba.AdvanceCommand();
    ASSERT_EQ(2, CountIterable(dba.Vertices(false)));
    for (const auto &va : dba.Vertices(false))
      ASSERT_NE(va.PropsAt(prop).type(), PropertyValue::Type::Null);

    // order by and expect an exception
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto n_p = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop);
    auto order_by = std::make_shared<plan::OrderBy>(
        n.op_, std::vector<SortItem>{{Ordering::ASC, n_p}},
        std::vector<Symbol>{});
    query::DbAccessor execution_dba(&dba);
    auto context = MakeContext(storage, symbol_table, &execution_dba);
    EXPECT_THROW(PullAll(*order_by, &context), QueryRuntimeException);
  }
}
