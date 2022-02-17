#include "cpp11.hpp"
#include "duckdb.hpp"
#include "typesr.hpp"
#include "rapi.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"

#include "duckdb/main/relation/filter_relation.hpp"
#include "duckdb/main/relation/projection_relation.hpp"
#include "duckdb/main/relation/aggregate_relation.hpp"
#include "duckdb/main/relation/order_relation.hpp"

using namespace duckdb;
using namespace cpp11;

template <typename T, typename... Args>
external_pointer<T> make_external(const string &rclass, Args &&...args) {
	auto extptr = external_pointer<T>(new T(std::forward<Args>(args)...));
	((sexp)extptr).attr("class") = rclass;
	return (extptr);
}

// DuckDB Expressions

[[cpp11::register]] SEXP rapi_expr_reference(std::string ref) {
	if (ref.size() == 0) {
		stop("expr_reference: Zero length name");
	}
	return make_external<ColumnRefExpression>("duckdb_expr", ref);
}

[[cpp11::register]] SEXP  rapi_expr_constant(sexp val) {
	if (LENGTH(val) != 1) {
		stop("expr_constant: Need value of length one");
	}
	return make_external<ConstantExpression>("duckdb_expr", RApiTypes::SexpToValue(val, 0));
}

[[cpp11::register]] SEXP  rapi_expr_function(std::string name, list args) {
	if (name.size() == 0) {
		stop("expr_function: Zero length name");
	}
	vector<unique_ptr<ParsedExpression>> children;
	for (auto arg : args) {
		children.push_back(expr_extptr_t(arg)->Copy());
	}
	auto operator_type = OperatorToExpressionType(name);
	if (operator_type != ExpressionType::INVALID && children.size() == 2) {
		return make_external<ComparisonExpression>("duckdb_expr", operator_type, move(children[0]), move(children[1]));
	} else if (name == "||") {
		return make_external<ConjunctionExpression>("duckdb_expr", ExpressionType::CONJUNCTION_OR, move(children[0]),
		                                            move(children[1]));
	}
	if (name == "mean") {
		name = "avg";
	} else if (name == "n") {
		name = "count_star";
	}
	return make_external<FunctionExpression>("duckdb_expr", name, move(children));
}

[[cpp11::register]] std::string  rapi_expr_tostring(duckdb::expr_extptr_t expr) {
	return expr->ToString();
}

// DuckDB Relations

[[cpp11::register]] SEXP  rapi_rel_from_df(duckdb::conn_eptr_t con, data_frame df) {
	if (!con->conn) {
		stop("rel_from_df: Invalid connection");
	}
	if (df.size() == 0) {
		stop("rel_from_df: Invalid data frame");
	}

	named_parameter_map_t other_params;
	other_params["experimental"] = Value::BOOLEAN(true);
	auto rel = con->conn->TableFunction("r_dataframe_scan", {Value::POINTER((uintptr_t)(SEXP)df)}, other_params);
	auto res = sexp(make_external<RelationWrapper>("duckdb_relation", move(rel)));
	res.attr("df") = df;
	return res;
}

[[cpp11::register]] SEXP  rapi_rel_filter(duckdb::rel_extptr_t rel, list exprs) {
	unique_ptr<ParsedExpression> filter_expr;
	if (exprs.size() == 0) { // nop
		warning("rel_filter without filter expressions has no effect");
		return rel;
	} else if (exprs.size() == 1) {
		filter_expr = ((expr_extptr_t)exprs[0])->Copy();
	} else {
		vector<unique_ptr<ParsedExpression>> filters;
		for (expr_extptr_t expr : exprs) {
			filters.push_back(expr->Copy());
		}
		filter_expr = make_unique<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, move(filters));
	}
	auto res = std::make_shared<FilterRelation>(rel->rel, move(filter_expr));
	return make_external<RelationWrapper>("duckdb_relation", res);
}

[[cpp11::register]] SEXP  rapi_rel_project(duckdb::rel_extptr_t rel, list exprs) {
	if (exprs.size() == 0) {
		warning("rel_project without projection expressions has no effect");
		return rel;
	}
	vector<unique_ptr<ParsedExpression>> projections;
	vector<string> aliases;

	for (expr_extptr_t expr : exprs) {
		aliases.push_back((expr->ToString()));
		projections.push_back(expr->Copy());
	}

	auto res = std::make_shared<ProjectionRelation>(rel->rel, move(projections), move(aliases));
	return make_external<RelationWrapper>("duckdb_relation", res);
}

[[cpp11::register]] SEXP  rapi_rel_aggregate(duckdb::rel_extptr_t rel, list groups, list aggregates) {
	vector<unique_ptr<ParsedExpression>> res_groups, res_aggregates;

	// TODO deal with empty groups
	vector<string> aliases;

	for (expr_extptr_t expr : groups) {
		res_groups.push_back(expr->Copy());
		res_aggregates.push_back(expr->Copy());
	}

	int aggr_idx = 0; // has to be int for - reasons
	auto aggr_names = aggregates.names();

	for (expr_extptr_t expr_p : aggregates) {
		auto expr = expr_p->Copy();
		if (aggr_names.size() > aggr_idx) {
			expr->alias = aggr_names[aggr_idx];
		}
		res_aggregates.push_back(move(expr));
		aggr_idx++;
	}

	auto res = std::make_shared<AggregateRelation>(rel->rel, move(res_aggregates), move(res_groups));
	return make_external<RelationWrapper>("duckdb_relation", res);
}

[[cpp11::register]] SEXP rapi_rel_order(duckdb::rel_extptr_t rel, list orders) {
	vector<OrderByNode> res_orders;

	for (expr_extptr_t expr : orders) {
		res_orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, expr->Copy());
	}

	auto res = std::make_shared<OrderRelation>(rel->rel, move(res_orders));
	return make_external<RelationWrapper>("duckdb_relation", res);
}

static SEXP result_to_df(unique_ptr<QueryResult> res) {
	if (res->type == QueryResultType::STREAM_RESULT) {
		res = ((StreamQueryResult &)*res).Materialize();
	}
	D_ASSERT(res->type == QueryResultType::MATERIALIZED_RESULT);
	auto mat_res = (MaterializedQueryResult *)res.get();

	writable::integers row_names;
	row_names.push_back(NA_INTEGER);
	row_names.push_back(-mat_res->collection.Count());

	// TODO this thing we can probably statically cache
	writable::strings classes;
	classes.push_back("tbl_df");
	classes.push_back("tbl");
	classes.push_back("data.frame");

	auto df = sexp(duckdb_execute_R_impl(mat_res));
	df.attr("class") = classes;
	df.attr("row.names") = row_names;
	return df;
}

[[cpp11::register]] SEXP  rapi_rel_to_df(duckdb::rel_extptr_t rel) {
	return result_to_df(rel->rel->Execute());
}

[[cpp11::register]] std::string  rapi_rel_tostring(duckdb::rel_extptr_t rel) {
	return rel->rel->ToString();
}

[[cpp11::register]] SEXP  rapi_rel_explain(duckdb::rel_extptr_t rel) {
	return result_to_df(rel->rel->Explain());
}

[[cpp11::register]] SEXP  rapi_rel_sql(duckdb::rel_extptr_t rel, std::string sql) {
	auto res = rel->rel->Query("_", sql);
	if (!res->success) {
		stop(res->error);
	}
	return result_to_df(move(res));
}

[[cpp11::register]] SEXP  rapi_rel_names(duckdb::rel_extptr_t rel) {
	auto ret = writable::strings();
	for (auto &col : rel->rel->Columns()) {
		ret.push_back(col.name);
	}
	return (ret);
}