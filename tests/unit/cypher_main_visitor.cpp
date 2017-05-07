#include <algorithm>
#include <climits>
#include <string>
#include <unordered_map>
#include <vector>

#include "antlr4-runtime.h"
#include "dbms/dbms.hpp"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "query/context.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/ast/cypher_main_visitor.hpp"
#include "query/frontend/opencypher/parser.hpp"
#include "query/typed_value.hpp"

namespace {

using namespace query;
using namespace query::frontend;
using query::TypedValue;
using testing::Pair;
using testing::ElementsAre;
using testing::UnorderedElementsAre;

class AstGenerator {
 public:
  AstGenerator(const std::string &query)
      : dbms_(),
        db_accessor_(dbms_.active()),
        context_(Config{}, *db_accessor_),
        query_string_(query),
        parser_(query),
        visitor_(context_),
        query_([&]() {
          visitor_.visit(parser_.tree());
          return visitor_.query();
        }()) {}

  Dbms dbms_;
  std::unique_ptr<GraphDbAccessor> db_accessor_;
  Context context_;
  std::string query_string_;
  ::frontend::opencypher::Parser parser_;
  CypherMainVisitor visitor_;
  Query *query_;
};

TEST(CypherMainVisitorTest, SyntaxException) {
  ASSERT_THROW(AstGenerator("CREATE ()-[*1...2]-()"), SyntaxException);
}

TEST(CypherMainVisitorTest, SyntaxExceptionOnTrailingText) {
  ASSERT_THROW(AstGenerator("RETURN 2 + 2 mirko"), SyntaxException);
}

TEST(CypherMainVisitorTest, PropertyLookup) {
  AstGenerator ast_generator("RETURN n.x");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *property_lookup = dynamic_cast<PropertyLookup *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(property_lookup->expression_);
  auto identifier = dynamic_cast<Identifier *>(property_lookup->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "n");
  ASSERT_EQ(property_lookup->property_,
            ast_generator.db_accessor_->property("x"));
}

TEST(CypherMainVisitorTest, LabelsTest) {
  AstGenerator ast_generator("RETURN n:x:y");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *labels_test = dynamic_cast<LabelsTest *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(labels_test->expression_);
  auto identifier = dynamic_cast<Identifier *>(labels_test->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "n");
  ASSERT_THAT(labels_test->labels_,
              ElementsAre(ast_generator.db_accessor_->label("x"),
                          ast_generator.db_accessor_->label("y")));
}

TEST(CypherMainVisitorTest, EscapedLabel) {
  AstGenerator ast_generator("RETURN n:`l-$\"'ab``e````l`");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *labels_test = dynamic_cast<LabelsTest *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto identifier = dynamic_cast<Identifier *>(labels_test->expression_);
  ASSERT_EQ(identifier->name_, "n");
  ASSERT_THAT(labels_test->labels_,
              ElementsAre(ast_generator.db_accessor_->label("l-$\"'ab`e``l")));
}

TEST(CypherMainVisitorTest, ReturnNoDistinctNoBagSemantics) {
  AstGenerator ast_generator("RETURN x");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_FALSE(return_clause->body_.all_identifiers);
  ASSERT_EQ(return_clause->body_.order_by.size(), 0U);
  ASSERT_EQ(return_clause->body_.named_expressions.size(), 1U);
  ASSERT_FALSE(return_clause->body_.limit);
  ASSERT_FALSE(return_clause->body_.skip);
  ASSERT_FALSE(return_clause->body_.distinct);
}

TEST(CypherMainVisitorTest, ReturnDistinct) {
  AstGenerator ast_generator("RETURN DISTINCT x");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_TRUE(return_clause->body_.distinct);
}

TEST(CypherMainVisitorTest, ReturnLimit) {
  AstGenerator ast_generator("RETURN x LIMIT 5");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_TRUE(return_clause->body_.limit);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(return_clause->body_.limit);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 5);
}

TEST(CypherMainVisitorTest, ReturnSkip) {
  AstGenerator ast_generator("RETURN x SKIP 5");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_TRUE(return_clause->body_.skip);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(return_clause->body_.skip);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 5);
}

TEST(CypherMainVisitorTest, ReturnOrderBy) {
  AstGenerator ast_generator("RETURN x, y, z ORDER BY z ASC, x, y DESC");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_EQ(return_clause->body_.order_by.size(), 3U);
  std::vector<std::pair<Ordering, std::string>> ordering;
  for (const auto &sort_item : return_clause->body_.order_by) {
    auto *identifier = dynamic_cast<Identifier *>(sort_item.second);
    ordering.emplace_back(sort_item.first, identifier->name_);
  }
  ASSERT_THAT(ordering, UnorderedElementsAre(Pair(Ordering::ASC, "z"),
                                             Pair(Ordering::ASC, "x"),
                                             Pair(Ordering::DESC, "y")));
}

TEST(CypherMainVisitorTest, ReturnNamedIdentifier) {
  AstGenerator ast_generator("RETURN var AS var5");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_FALSE(return_clause->body_.all_identifiers);
  auto *named_expr = return_clause->body_.named_expressions[0];
  ASSERT_EQ(named_expr->name_, "var5");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier->name_, "var");
}

TEST(CypherMainVisitorTest, ReturnAsterisk) {
  AstGenerator ast_generator("RETURN *");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_TRUE(return_clause->body_.all_identifiers);
  ASSERT_EQ(return_clause->body_.named_expressions.size(), 0U);
}

TEST(CypherMainVisitorTest, IntegerLiteral) {
  AstGenerator ast_generator("RETURN 42");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 42);
}

TEST(CypherMainVisitorTest, IntegerLiteralTooLarge) {
  ASSERT_THROW(AstGenerator("RETURN 10000000000000000000000000"),
               SemanticException);
}

TEST(CypherMainVisitorTest, BooleanLiteralTrue) {
  AstGenerator ast_generator("RETURN TrUe");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<bool>(), true);
}

TEST(CypherMainVisitorTest, BooleanLiteralFalse) {
  AstGenerator ast_generator("RETURN faLSE");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<bool>(), false);
}

TEST(CypherMainVisitorTest, NullLiteral) {
  AstGenerator ast_generator("RETURN nULl");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.type(), TypedValue::Type::Null);
}

TEST(CypherMainVisitorTest, ParenthesizedExpression) {
  AstGenerator ast_generator("RETURN (2)");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, OrOperator) {
  AstGenerator ast_generator("RETURN true Or false oR n");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *or_operator2 = dynamic_cast<OrOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(or_operator2);
  auto *or_operator1 = dynamic_cast<OrOperator *>(or_operator2->expression1_);
  ASSERT_TRUE(or_operator1);
  auto *operand1 = dynamic_cast<PrimitiveLiteral *>(or_operator1->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<bool>(), true);
  auto *operand2 = dynamic_cast<PrimitiveLiteral *>(or_operator1->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<bool>(), false);
  auto *operand3 = dynamic_cast<Identifier *>(or_operator2->expression2_);
  ASSERT_TRUE(operand3);
  ASSERT_EQ(operand3->name_, "n");
}

TEST(CypherMainVisitorTest, XorOperator) {
  AstGenerator ast_generator("RETURN true xOr false");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *xor_operator = dynamic_cast<XorOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand1 = dynamic_cast<PrimitiveLiteral *>(xor_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<bool>(), true);
  auto *operand2 = dynamic_cast<PrimitiveLiteral *>(xor_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<bool>(), false);
}

TEST(CypherMainVisitorTest, AndOperator) {
  AstGenerator ast_generator("RETURN true and false");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *and_operator = dynamic_cast<AndOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand1 = dynamic_cast<PrimitiveLiteral *>(and_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<bool>(), true);
  auto *operand2 = dynamic_cast<PrimitiveLiteral *>(and_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<bool>(), false);
}

TEST(CypherMainVisitorTest, AdditionSubtractionOperators) {
  AstGenerator ast_generator("RETURN 1 - 2 + 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *addition_operator = dynamic_cast<AdditionOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(addition_operator);
  auto *subtraction_operator =
      dynamic_cast<SubtractionOperator *>(addition_operator->expression1_);
  ASSERT_TRUE(subtraction_operator);
  auto *operand1 =
      dynamic_cast<PrimitiveLiteral *>(subtraction_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 1);
  auto *operand2 =
      dynamic_cast<PrimitiveLiteral *>(subtraction_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 2);
  auto *operand3 =
      dynamic_cast<PrimitiveLiteral *>(addition_operator->expression2_);
  ASSERT_TRUE(operand3);
  ASSERT_EQ(operand3->value_.Value<int64_t>(), 3);
}

TEST(CypherMainVisitorTest, MulitplicationOperator) {
  AstGenerator ast_generator("RETURN 2 * 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *mult_operator = dynamic_cast<MultiplicationOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand1 =
      dynamic_cast<PrimitiveLiteral *>(mult_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 =
      dynamic_cast<PrimitiveLiteral *>(mult_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

TEST(CypherMainVisitorTest, DivisionOperator) {
  AstGenerator ast_generator("RETURN 2 / 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *div_operator = dynamic_cast<DivisionOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand1 = dynamic_cast<PrimitiveLiteral *>(div_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<PrimitiveLiteral *>(div_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

TEST(CypherMainVisitorTest, ModOperator) {
  AstGenerator ast_generator("RETURN 2 % 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *mod_operator = dynamic_cast<ModOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand1 = dynamic_cast<PrimitiveLiteral *>(mod_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<PrimitiveLiteral *>(mod_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

#define CHECK_COMPARISON(TYPE, VALUE1, VALUE2)                             \
  do {                                                                     \
    auto *and_operator = dynamic_cast<AndOperator *>(_operator);           \
    ASSERT_TRUE(and_operator);                                             \
    _operator = and_operator->expression1_;                                \
    auto *cmp_operator = dynamic_cast<TYPE *>(and_operator->expression2_); \
    ASSERT_TRUE(cmp_operator);                                             \
    auto *operand1 =                                                       \
        dynamic_cast<PrimitiveLiteral *>(cmp_operator->expression1_);      \
    ASSERT_EQ(operand1->value_.Value<int64_t>(), VALUE1);                  \
    auto *operand2 =                                                       \
        dynamic_cast<PrimitiveLiteral *>(cmp_operator->expression2_);      \
    ASSERT_EQ(operand2->value_.Value<int64_t>(), VALUE2);                  \
  } while (0)

TEST(CypherMainVisitorTest, ComparisonOperators) {
  AstGenerator ast_generator("RETURN 2 = 3 != 4 <> 5 < 6 > 7 <= 8 >= 9");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  Expression *_operator =
      return_clause->body_.named_expressions[0]->expression_;
  CHECK_COMPARISON(GreaterEqualOperator, 8, 9);
  CHECK_COMPARISON(LessEqualOperator, 7, 8);
  CHECK_COMPARISON(GreaterOperator, 6, 7);
  CHECK_COMPARISON(LessOperator, 5, 6);
  CHECK_COMPARISON(NotEqualOperator, 4, 5);
  CHECK_COMPARISON(NotEqualOperator, 3, 4);
  auto *cmp_operator = dynamic_cast<EqualOperator *>(_operator);
  ASSERT_TRUE(cmp_operator);
  auto *operand1 = dynamic_cast<PrimitiveLiteral *>(cmp_operator->expression1_);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<PrimitiveLiteral *>(cmp_operator->expression2_);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

#undef CHECK_COMPARISON

TEST(CypherMainVisitorTest, ListIndexingOperator) {
  AstGenerator ast_generator("RETURN [1,2,3] [ 2 ]");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *list_index_op = dynamic_cast<ListIndexingOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(list_index_op);
  auto *list = dynamic_cast<ListLiteral *>(list_index_op->expression1_);
  EXPECT_TRUE(list);
  auto *index = dynamic_cast<PrimitiveLiteral *>(list_index_op->expression2_);
  ASSERT_EQ(index->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, ListSlicingOperatorNoBounds) {
  ASSERT_THROW(AstGenerator("RETURN [1,2,3] [ .. ]"), SemanticException);
}

TEST(CypherMainVisitorTest, ListSlicingOperator) {
  AstGenerator ast_generator("RETURN [1,2,3] [ .. 2 ]");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *list_slicing_op = dynamic_cast<ListSlicingOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(list_slicing_op);
  auto *list = dynamic_cast<ListLiteral *>(list_slicing_op->list_);
  EXPECT_TRUE(list);
  EXPECT_FALSE(list_slicing_op->lower_bound_);
  auto *upper_bound =
      dynamic_cast<PrimitiveLiteral *>(list_slicing_op->upper_bound_);
  EXPECT_EQ(upper_bound->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, InListOperator) {
  AstGenerator ast_generator("RETURN 5 IN [1,2]");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *in_list_operator = dynamic_cast<InListOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(in_list_operator);
  auto *literal =
      dynamic_cast<PrimitiveLiteral *>(in_list_operator->expression1_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 5);
  auto *list = dynamic_cast<ListLiteral *>(in_list_operator->expression2_);
  ASSERT_TRUE(list);
}

TEST(CypherMainVisitorTest, IsNull) {
  AstGenerator ast_generator("RETURN 2 iS NulL");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *is_type_operator = dynamic_cast<IsNullOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand1 =
      dynamic_cast<PrimitiveLiteral *>(is_type_operator->expression_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, IsNotNull) {
  AstGenerator ast_generator("RETURN 2 iS nOT NulL");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *not_operator = dynamic_cast<NotOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *is_type_operator =
      dynamic_cast<IsNullOperator *>(not_operator->expression_);
  auto *operand1 =
      dynamic_cast<PrimitiveLiteral *>(is_type_operator->expression_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, NotOperator) {
  AstGenerator ast_generator("RETURN not true");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *not_operator = dynamic_cast<NotOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  auto *operand = dynamic_cast<PrimitiveLiteral *>(not_operator->expression_);
  ASSERT_TRUE(operand);
  ASSERT_EQ(operand->value_.Value<bool>(), true);
}

TEST(CypherMainVisitorTest, UnaryMinusPlusOperators) {
  AstGenerator ast_generator("RETURN -+5");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *unary_minus_operator = dynamic_cast<UnaryMinusOperator *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(unary_minus_operator);
  auto *unary_plus_operator =
      dynamic_cast<UnaryPlusOperator *>(unary_minus_operator->expression_);
  ASSERT_TRUE(unary_plus_operator);
  auto *operand =
      dynamic_cast<PrimitiveLiteral *>(unary_plus_operator->expression_);
  ASSERT_TRUE(operand);
  ASSERT_EQ(operand->value_.Value<int64_t>(), 5);
}

TEST(CypherMainVisitorTest, Aggregation) {
  AstGenerator ast_generator(
      "RETURN COUNT(a), MIN(b), MAX(c), SUM(d), AVG(e), COUNT(*)");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_EQ(return_clause->body_.named_expressions.size(), 6U);
  Aggregation::Op ops[] = {Aggregation::Op::COUNT, Aggregation::Op::MIN,
                           Aggregation::Op::MAX, Aggregation::Op::SUM,
                           Aggregation::Op::AVG};
  std::string ids[] = {"a", "b", "c", "d", "e"};
  for (int i = 0; i < 5; ++i) {
    auto *aggregation = dynamic_cast<Aggregation *>(
        return_clause->body_.named_expressions[i]->expression_);
    ASSERT_TRUE(aggregation);
    ASSERT_EQ(aggregation->op_, ops[i]);
    auto *identifier = dynamic_cast<Identifier *>(aggregation->expression_);
    ASSERT_TRUE(identifier);
    ASSERT_EQ(identifier->name_, ids[i]);
  }
  auto *aggregation = dynamic_cast<Aggregation *>(
      return_clause->body_.named_expressions[5]->expression_);
  ASSERT_TRUE(aggregation);
  ASSERT_EQ(aggregation->op_, Aggregation::Op::COUNT);
  ASSERT_FALSE(aggregation->expression_);
}

TEST(CypherMainVisitorTest, UndefinedFunction) {
  ASSERT_THROW(AstGenerator("RETURN "
                            "IHopeWeWillNeverHaveAwesomeMemgraphProcedureWithS"
                            "uchALongAndAwesomeNameSinceThisTestWouldFail(1)"),
               SemanticException);
}

TEST(CypherMainVisitorTest, Function) {
  AstGenerator ast_generator("RETURN abs(n, 2)");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_EQ(return_clause->body_.named_expressions.size(), 1);
  auto *function = dynamic_cast<Function *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(function);
  ASSERT_TRUE(function->function_);
}

TEST(CypherMainVisitorTest, StringLiteralDoubleQuotes) {
  AstGenerator ast_generator("RETURN \"mi'rko\"");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), "mi'rko");
}

TEST(CypherMainVisitorTest, StringLiteralSingleQuotes) {
  AstGenerator ast_generator("RETURN 'mi\"rko'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), "mi\"rko");
}

TEST(CypherMainVisitorTest, StringLiteralEscapedChars) {
  AstGenerator ast_generator(
      "RETURN '\\\\\\'\\\"\\b\\B\\f\\F\\n\\N\\r\\R\\t\\T'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), "\\'\"\b\b\f\f\n\n\r\r\t\t");
}

TEST(CypherMainVisitorTest, StringLiteralEscapedUtf16) {
  AstGenerator ast_generator("RETURN '\\u221daaa\\U221daaa'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), u8"\u221daaa\u221daaa");
}

TEST(CypherMainVisitorTest, StringLiteralEscapedUtf32) {
  AstGenerator ast_generator("RETURN '\\u0001F600aaaa\\U0001F600aaaaaaaa'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(),
            u8"\U0001F600aaaa\U0001F600aaaaaaaa");
}

TEST(CypherMainVisitorTest, DoubleLiteral) {
  AstGenerator ast_generator("RETURN 3.5");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<double>(), 3.5);
}

TEST(CypherMainVisitorTest, DoubleLiteralExponent) {
  AstGenerator ast_generator("RETURN 5e-1");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<PrimitiveLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<double>(), 0.5);
}

TEST(CypherMainVisitorTest, ListLiteral) {
  AstGenerator ast_generator("RETURN [3, [], 'johhny']");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *list_literal = dynamic_cast<ListLiteral *>(
      return_clause->body_.named_expressions[0]->expression_);
  ASSERT_TRUE(list_literal);
  ASSERT_EQ(3, list_literal->elements_.size());
  auto *elem_0 = dynamic_cast<PrimitiveLiteral *>(list_literal->elements_[0]);
  ASSERT_TRUE(elem_0);
  EXPECT_EQ(elem_0->value_.type(), TypedValue::Type::Int);
  auto *elem_1 = dynamic_cast<ListLiteral *>(list_literal->elements_[1]);
  ASSERT_TRUE(elem_1);
  EXPECT_EQ(0, elem_1->elements_.size());
  auto *elem_2 = dynamic_cast<PrimitiveLiteral *>(list_literal->elements_[2]);
  ASSERT_TRUE(elem_2);
  EXPECT_EQ(elem_2->value_.type(), TypedValue::Type::String);
}

TEST(CypherMainVisitorTest, NodePattern) {
  AstGenerator ast_generator(
      "MATCH (:label1:label2:label3 {a : 5, b : 10}) RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_FALSE(match->optional_);
  EXPECT_FALSE(match->where_);
  ASSERT_EQ(match->patterns_.size(), 1U);
  ASSERT_TRUE(match->patterns_[0]);
  ASSERT_EQ(match->patterns_[0]->atoms_.size(), 1U);
  auto node = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node);
  ASSERT_TRUE(node->identifier_);
  EXPECT_EQ(node->identifier_->name_,
            CypherMainVisitor::kAnonPrefix + std::to_string(1));
  EXPECT_FALSE(node->identifier_->user_declared_);
  EXPECT_THAT(node->labels_, UnorderedElementsAre(
                                 ast_generator.db_accessor_->label("label1"),
                                 ast_generator.db_accessor_->label("label2"),
                                 ast_generator.db_accessor_->label("label3")));
  std::unordered_map<GraphDbTypes::Property, int64_t> properties;
  for (auto x : node->properties_) {
    auto *literal = dynamic_cast<PrimitiveLiteral *>(x.second);
    ASSERT_TRUE(literal);
    ASSERT_TRUE(literal->value_.type() == TypedValue::Type::Int);
    properties[x.first] = literal->value_.Value<int64_t>();
  }
  EXPECT_THAT(properties,
              UnorderedElementsAre(
                  Pair(ast_generator.db_accessor_->property("a"), 5),
                  Pair(ast_generator.db_accessor_->property("b"), 10)));
}

TEST(CypherMainVisitorTest, NodePatternIdentifier) {
  AstGenerator ast_generator("MATCH (var) RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_FALSE(match->optional_);
  EXPECT_FALSE(match->where_);
  auto node = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node);
  ASSERT_TRUE(node->identifier_);
  EXPECT_EQ(node->identifier_->name_, "var");
  EXPECT_TRUE(node->identifier_->user_declared_);
  EXPECT_THAT(node->labels_, UnorderedElementsAre());
  EXPECT_THAT(node->properties_, UnorderedElementsAre());
}

TEST(CypherMainVisitorTest, RelationshipPatternNoDetails) {
  AstGenerator ast_generator("MATCH ()--() RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_FALSE(match->optional_);
  EXPECT_FALSE(match->where_);
  ASSERT_EQ(match->patterns_.size(), 1U);
  ASSERT_TRUE(match->patterns_[0]);
  ASSERT_EQ(match->patterns_[0]->atoms_.size(), 3U);
  auto *node1 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node1);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_TRUE(edge);
  auto *node2 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[2]);
  ASSERT_TRUE(node2);
  EXPECT_EQ(edge->direction_, EdgeAtom::Direction::BOTH);
  ASSERT_TRUE(edge->identifier_);
  EXPECT_THAT(edge->identifier_->name_,
              CypherMainVisitor::kAnonPrefix + std::to_string(2));
  EXPECT_FALSE(edge->identifier_->user_declared_);
}

// PatternPart in braces.
TEST(CypherMainVisitorTest, PatternPartBraces) {
  AstGenerator ast_generator("MATCH ((()--())) RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_FALSE(match->where_);
  ASSERT_EQ(match->patterns_.size(), 1U);
  ASSERT_TRUE(match->patterns_[0]);
  ASSERT_EQ(match->patterns_[0]->atoms_.size(), 3U);
  auto *node1 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node1);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_TRUE(edge);
  auto *node2 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[2]);
  ASSERT_TRUE(node2);
  EXPECT_EQ(edge->direction_, EdgeAtom::Direction::BOTH);
  ASSERT_TRUE(edge->identifier_);
  EXPECT_THAT(edge->identifier_->name_,
              CypherMainVisitor::kAnonPrefix + std::to_string(2));
  EXPECT_FALSE(edge->identifier_->user_declared_);
}

TEST(CypherMainVisitorTest, RelationshipPatternDetails) {
  AstGenerator ast_generator(
      "MATCH ()<-[:type1|type2 {a : 5, b : 10}]-() RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_FALSE(match->optional_);
  EXPECT_FALSE(match->where_);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_TRUE(edge);
  EXPECT_EQ(edge->direction_, EdgeAtom::Direction::LEFT);
  EXPECT_THAT(
      edge->edge_types_,
      UnorderedElementsAre(ast_generator.db_accessor_->edge_type("type1"),
                           ast_generator.db_accessor_->edge_type("type2")));
  std::unordered_map<GraphDbTypes::Property, int64_t> properties;
  for (auto x : edge->properties_) {
    auto *literal = dynamic_cast<PrimitiveLiteral *>(x.second);
    ASSERT_TRUE(literal);
    ASSERT_TRUE(literal->value_.type() == TypedValue::Type::Int);
    properties[x.first] = literal->value_.Value<int64_t>();
  }
  EXPECT_THAT(properties,
              UnorderedElementsAre(
                  Pair(ast_generator.db_accessor_->property("a"), 5),
                  Pair(ast_generator.db_accessor_->property("b"), 10)));
}

TEST(CypherMainVisitorTest, RelationshipPatternVariable) {
  AstGenerator ast_generator("MATCH ()-[var]->() RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_FALSE(match->optional_);
  EXPECT_FALSE(match->where_);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_TRUE(edge);
  EXPECT_EQ(edge->direction_, EdgeAtom::Direction::RIGHT);
  ASSERT_TRUE(edge->identifier_);
  EXPECT_THAT(edge->identifier_->name_, "var");
  EXPECT_TRUE(edge->identifier_->user_declared_);
}

// // Relationship with unbounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternUnbounded) {
//   ParserTables parser("CREATE ()-[*]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 1,
//                        LLONG_MAX);
// }
//
// // Relationship with lower bounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternLowerBounded) {
//   ParserTables parser("CREATE ()-[*5..]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 5,
//                        LLONG_MAX);
// }
//
// // Relationship with upper bounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternUpperBounded) {
//   ParserTables parser("CREATE ()-[*..10]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 1, 10);
// }
//
// // Relationship with lower and upper bounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternLowerUpperBounded) {
//   ParserTables parser("CREATE ()-[*5..10]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 5, 10);
// }
//
// // Relationship with fixed number of edges.
// TEST(CypherMainVisitorTest, RelationshipPatternFixedRange) {
//   ParserTables parser("CREATE ()-[*10]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 10, 10);
// }
//
//
// // PatternPart with variable.
// TEST(CypherMainVisitorTest, PatternPartVariable) {
//   ParserTables parser("CREATE var=()--()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 1U);
//   ASSERT_EQ(parser.pattern_parts_.size(), 1U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   ASSERT_EQ(parser.nodes_.size(), 2U);
//   ASSERT_EQ(parser.pattern_parts_.begin()->second.nodes.size(), 2U);
//   ASSERT_EQ(parser.pattern_parts_.begin()->second.relationships.size(), 1U);
//   ASSERT_NE(parser.identifiers_map_.find("var"),
//   parser.identifiers_map_.end());
//   auto output_identifier = parser.identifiers_map_["var"];
//   ASSERT_NE(parser.pattern_parts_.find(output_identifier),
//             parser.pattern_parts_.end());
// }

TEST(CypherMainVisitorTest, ReturnUnanemdIdentifier) {
  AstGenerator ast_generator("RETURN var");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_TRUE(return_clause);
  ASSERT_EQ(return_clause->body_.named_expressions.size(), 1U);
  auto *named_expr = return_clause->body_.named_expressions[0];
  ASSERT_TRUE(named_expr);
  ASSERT_EQ(named_expr->name_, "var");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "var");
  ASSERT_TRUE(identifier->user_declared_);
}

TEST(CypherMainVisitorTest, Create) {
  AstGenerator ast_generator("CREATE (n)");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *create = dynamic_cast<Create *>(query->clauses_[0]);
  ASSERT_TRUE(create);
  ASSERT_EQ(create->patterns_.size(), 1U);
  ASSERT_TRUE(create->patterns_[0]);
  ASSERT_EQ(create->patterns_[0]->atoms_.size(), 1U);
  auto node = dynamic_cast<NodeAtom *>(create->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node);
  ASSERT_TRUE(node->identifier_);
  ASSERT_EQ(node->identifier_->name_, "n");
}

TEST(CypherMainVisitorTest, Delete) {
  AstGenerator ast_generator("DELETE n, m");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *del = dynamic_cast<Delete *>(query->clauses_[0]);
  ASSERT_TRUE(del);
  ASSERT_FALSE(del->detach_);
  ASSERT_EQ(del->expressions_.size(), 2U);
  auto *identifier1 = dynamic_cast<Identifier *>(del->expressions_[0]);
  ASSERT_TRUE(identifier1);
  ASSERT_EQ(identifier1->name_, "n");
  auto *identifier2 = dynamic_cast<Identifier *>(del->expressions_[1]);
  ASSERT_TRUE(identifier2);
  ASSERT_EQ(identifier2->name_, "m");
}

TEST(CypherMainVisitorTest, DeleteDetach) {
  AstGenerator ast_generator("DETACH DELETE n");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *del = dynamic_cast<Delete *>(query->clauses_[0]);
  ASSERT_TRUE(del);
  ASSERT_TRUE(del->detach_);
  ASSERT_EQ(del->expressions_.size(), 1U);
  auto *identifier1 = dynamic_cast<Identifier *>(del->expressions_[0]);
  ASSERT_TRUE(identifier1);
  ASSERT_EQ(identifier1->name_, "n");
}

TEST(CypherMainVisitorTest, OptionalMatchWhere) {
  AstGenerator ast_generator("OPTIONAL MATCH (n) WHERE m RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  EXPECT_TRUE(match->optional_);
  ASSERT_TRUE(match->where_);
  auto *identifier = dynamic_cast<Identifier *>(match->where_->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "m");
}

TEST(CypherMainVisitorTest, Set) {
  AstGenerator ast_generator("SET a.x = b, c = d, e += f, g : h : i ");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 4U);

  {
    auto *set_property = dynamic_cast<SetProperty *>(query->clauses_[0]);
    ASSERT_TRUE(set_property);
    ASSERT_TRUE(set_property->property_lookup_);
    auto *identifier1 =
        dynamic_cast<Identifier *>(set_property->property_lookup_->expression_);
    ASSERT_TRUE(identifier1);
    ASSERT_EQ(identifier1->name_, "a");
    ASSERT_EQ(set_property->property_lookup_->property_,
              ast_generator.db_accessor_->property("x"));
    auto *identifier2 = dynamic_cast<Identifier *>(set_property->expression_);
    ASSERT_EQ(identifier2->name_, "b");
  }

  {
    auto *set_properties_assignment =
        dynamic_cast<SetProperties *>(query->clauses_[1]);
    ASSERT_TRUE(set_properties_assignment);
    ASSERT_FALSE(set_properties_assignment->update_);
    ASSERT_TRUE(set_properties_assignment->identifier_);
    ASSERT_EQ(set_properties_assignment->identifier_->name_, "c");
    auto *identifier =
        dynamic_cast<Identifier *>(set_properties_assignment->expression_);
    ASSERT_EQ(identifier->name_, "d");
  }

  {
    auto *set_properties_update =
        dynamic_cast<SetProperties *>(query->clauses_[2]);
    ASSERT_TRUE(set_properties_update);
    ASSERT_TRUE(set_properties_update->update_);
    ASSERT_TRUE(set_properties_update->identifier_);
    ASSERT_EQ(set_properties_update->identifier_->name_, "e");
    auto *identifier =
        dynamic_cast<Identifier *>(set_properties_update->expression_);
    ASSERT_EQ(identifier->name_, "f");
  }

  {
    auto *set_labels = dynamic_cast<SetLabels *>(query->clauses_[3]);
    ASSERT_TRUE(set_labels);
    ASSERT_TRUE(set_labels->identifier_);
    ASSERT_EQ(set_labels->identifier_->name_, "g");
    ASSERT_THAT(set_labels->labels_,
                UnorderedElementsAre(ast_generator.db_accessor_->label("h"),
                                     ast_generator.db_accessor_->label("i")));
  }
}

TEST(CypherMainVisitorTest, Remove) {
  AstGenerator ast_generator("REMOVE a.x, g : h : i");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);

  {
    auto *remove_property = dynamic_cast<RemoveProperty *>(query->clauses_[0]);
    ASSERT_TRUE(remove_property);
    ASSERT_TRUE(remove_property->property_lookup_);
    auto *identifier1 = dynamic_cast<Identifier *>(
        remove_property->property_lookup_->expression_);
    ASSERT_TRUE(identifier1);
    ASSERT_EQ(identifier1->name_, "a");
    ASSERT_EQ(remove_property->property_lookup_->property_,
              ast_generator.db_accessor_->property("x"));
  }
  {
    auto *remove_labels = dynamic_cast<RemoveLabels *>(query->clauses_[1]);
    ASSERT_TRUE(remove_labels);
    ASSERT_TRUE(remove_labels->identifier_);
    ASSERT_EQ(remove_labels->identifier_->name_, "g");
    ASSERT_THAT(remove_labels->labels_,
                UnorderedElementsAre(ast_generator.db_accessor_->label("h"),
                                     ast_generator.db_accessor_->label("i")));
  }
}

TEST(CypherMainVisitorTest, With) {
  AstGenerator ast_generator("WITH n AS m RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_TRUE(with);
  ASSERT_FALSE(with->body_.distinct);
  ASSERT_FALSE(with->body_.limit);
  ASSERT_FALSE(with->body_.skip);
  ASSERT_EQ(with->body_.order_by.size(), 0U);
  ASSERT_FALSE(with->where_);
  ASSERT_EQ(with->body_.named_expressions.size(), 1U);
  auto *named_expr = with->body_.named_expressions[0];
  ASSERT_EQ(named_expr->name_, "m");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier->name_, "n");
}

TEST(CypherMainVisitorTest, WithNonAliasedExpression) {
  ASSERT_THROW(AstGenerator("WITH n.x RETURN 1"), SemanticException);
}

TEST(CypherMainVisitorTest, WithNonAliasedVariable) {
  AstGenerator ast_generator("WITH n RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_TRUE(with);
  ASSERT_EQ(with->body_.named_expressions.size(), 1U);
  auto *named_expr = with->body_.named_expressions[0];
  ASSERT_EQ(named_expr->name_, "n");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier->name_, "n");
}

TEST(CypherMainVisitorTest, WithDistinct) {
  AstGenerator ast_generator("WITH DISTINCT n AS m RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_TRUE(with->body_.distinct);
  ASSERT_FALSE(with->where_);
  ASSERT_EQ(with->body_.named_expressions.size(), 1U);
  auto *named_expr = with->body_.named_expressions[0];
  ASSERT_EQ(named_expr->name_, "m");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier->name_, "n");
}

TEST(CypherMainVisitorTest, WithBag) {
  AstGenerator ast_generator("WITH n as m ORDER BY m SKIP 1 LIMIT 2 RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_FALSE(with->body_.distinct);
  ASSERT_FALSE(with->where_);
  ASSERT_EQ(with->body_.named_expressions.size(), 1U);
  // No need to check contents of body. That is checked in RETURN clause tests.
  ASSERT_EQ(with->body_.order_by.size(), 1U);
  ASSERT_TRUE(with->body_.limit);
  ASSERT_TRUE(with->body_.skip);
}

TEST(CypherMainVisitorTest, WithWhere) {
  AstGenerator ast_generator("WITH n AS m WHERE k RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_TRUE(with);
  ASSERT_TRUE(with->where_);
  auto *identifier = dynamic_cast<Identifier *>(with->where_->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "k");
  ASSERT_EQ(with->body_.named_expressions.size(), 1U);
  auto *named_expr = with->body_.named_expressions[0];
  ASSERT_EQ(named_expr->name_, "m");
  auto *identifier2 = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier2->name_, "n");
}

TEST(CypherMainVisitorTest, ClausesOrdering) {
  // Obviously some of the ridiculous combinations don't fail here, but they
  // will fail in semantic analysis or they make perfect sense as a part of
  // bigger query.
  AstGenerator("RETURN 1");
  ASSERT_THROW(AstGenerator("RETURN 1 RETURN 1"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 MATCH (n) RETURN n"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 DELETE n"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 MERGE (n)"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 WITH n AS m RETURN 1"),
               SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 AS n UNWIND n AS x RETURN x"),
               SemanticException);

  AstGenerator("CREATE (n)");
  ASSERT_THROW(AstGenerator("SET n:x MATCH (n) RETURN n"), SemanticException);
  AstGenerator("REMOVE n.x SET n.x = 1");
  AstGenerator("REMOVE n:L RETURN n");
  AstGenerator("SET n.x = 1 WITH n AS m RETURN m");

  ASSERT_THROW(AstGenerator("MATCH (n)"), SemanticException);
  AstGenerator("MATCH (n) MATCH (n) RETURN n");
  AstGenerator("MATCH (n) SET n = m");
  AstGenerator("MATCH (n) RETURN n");
  AstGenerator("MATCH (n) WITH n AS m RETURN m");

  ASSERT_THROW(AstGenerator("WITH 1 AS n"), SemanticException);
  AstGenerator("WITH 1 AS n WITH n AS m RETURN m");
  AstGenerator("WITH 1 AS n RETURN n");
  AstGenerator("WITH 1 AS n SET n += m");
  AstGenerator("WITH 1 AS n MATCH (n) RETURN n");

  ASSERT_THROW(AstGenerator("UNWIND [1,2,3] AS x"), SemanticException);
  ASSERT_THROW(AstGenerator("CREATE (n) UNWIND [1,2,3] AS x RETURN x"),
               SemanticException);
  AstGenerator("UNWIND [1,2,3] AS x CREATE (n) RETURN x");
  AstGenerator("CREATE (n) WITH n UNWIND [1,2,3] AS x RETURN x");
}

TEST(CypherMainVisitorTest, Merge) {
  AstGenerator ast_generator(
      "MERGE (a) -[:r]- (b) ON MATCH SET a.x = b.x "
      "ON CREATE SET b :label ON MATCH SET b = a");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *merge = dynamic_cast<Merge *>(query->clauses_[0]);
  ASSERT_TRUE(merge);
  EXPECT_TRUE(dynamic_cast<Pattern *>(merge->pattern_));
  ASSERT_EQ(merge->on_match_.size(), 2U);
  EXPECT_TRUE(dynamic_cast<SetProperty *>(merge->on_match_[0]));
  EXPECT_TRUE(dynamic_cast<SetProperties *>(merge->on_match_[1]));
  ASSERT_EQ(merge->on_create_.size(), 1U);
  EXPECT_TRUE(dynamic_cast<SetLabels *>(merge->on_create_[0]));
}

TEST(CypherMainVisitorTest, Unwind) {
  AstGenerator ast_generator("UNWIND [1,2,3] AS elem RETURN elem");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *unwind = dynamic_cast<Unwind *>(query->clauses_[0]);
  ASSERT_TRUE(unwind);
  auto *ret = dynamic_cast<Return *>(query->clauses_[1]);
  EXPECT_TRUE(ret);
  ASSERT_TRUE(unwind->named_expression_);
  EXPECT_EQ(unwind->named_expression_->name_, "elem");
  auto *expr = unwind->named_expression_->expression_;
  ASSERT_TRUE(expr);
  ASSERT_TRUE(dynamic_cast<ListLiteral *>(expr));
}

TEST(CypherMainVisitorTest, UnwindWithoutAsError) {
  EXPECT_THROW(AstGenerator("UNWIND [1,2,3] RETURN 42"), SyntaxException);
}
}
