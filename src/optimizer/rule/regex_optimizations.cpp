#include "duckdb/optimizer/rule/regex_optimizations.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "re2/re2.h"
#include "re2/regexp.h"

namespace duckdb {

RegexOptimizationRule::RegexOptimizationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function = make_uniq<SpecificFunctionMatcher>("regexp_matches");
	func->policy = SetMatcher::Policy::ORDERED;
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	root = std::move(func);
}

unique_ptr<Expression> RegexOptimizationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                    bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant_expr = bindings[2].get().Cast<BoundConstantExpression>();
	D_ASSERT(root.children.size() == 2);

	if (constant_expr.value.IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(root.return_type));
	}

	// the constant_expr is a scalar expression that we have to fold
	if (!constant_expr.IsFoldable()) {
		return nullptr;
	}

	auto constant_value = ExpressionExecutor::EvaluateScalar(GetContext(), constant_expr);
	D_ASSERT(constant_value.type() == constant_expr.return_type);
	auto patt_str = StringValue::Get(constant_value);

	duckdb_re2::RE2 pattern(patt_str);
	if (!pattern.ok()) {
		return nullptr; // this should fail somewhere else
	}

	if (pattern.Regexp()->op() == duckdb_re2::kRegexpLiteralString ||
	    pattern.Regexp()->op() == duckdb_re2::kRegexpLiteral) {

		string min;
		string max;
		pattern.PossibleMatchRange(&min, &max, patt_str.size() + 1);
		if (min != max) {
			return nullptr;
		}
		auto parameter = make_uniq<BoundConstantExpression>(Value(std::move(min)));
		auto contains = make_uniq<BoundFunctionExpression>(root.return_type, ContainsFun::GetFunction(),
		                                                   std::move(root.children), nullptr);
		contains->children[1] = std::move(parameter);

		return std::move(contains);
	} else {
		string prefix("tmp_prefix");
		bool fold_case = true;
		auto regexp = pattern.Regexp();

		pattern.Regexp()->RequiredPrefix(&prefix, &fold_case, &regexp);
		// prefix now has the prefic I need.
		// foldcase is now false
		// dunno is the regexp that comes after the thing
		if (!prefix.empty()) {
			auto prefix_expression = make_unique<BoundFunctionExpression>(root->return_type, PrefixFun::GetFunction(), std::move(root->children), nullptr);
			prefix_expression->children[1] = make_unique<BoundConstantExpression>(Value(std::move(prefix)));
//			if (regexp->op() == duckdb_re2::kRegexpEmptyMatch) {
//				auto next = make_unique<BoundFunctionExpression>(root->return_type, RegexpFun::));
//				next->children[1] = Value(std::move(regexp->ToString())));
//				prefix_expression->children[2] = std::move(next);
//			}
			return std::move(prefix_expression);
		}

		string min;
		string max;
		pattern.PossibleMatchRange(&min, &max, patt_str.size());

		auto a = "wait here";
	}

	return nullptr;
}

} // namespace duckdb
