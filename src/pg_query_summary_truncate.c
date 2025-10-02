#include "pg_query.h"
#include "pg_query_internal.h"
#include "pg_query_summary.h"

#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"

enum TruncationAttr {
	TRUNCATION_TARGET_LIST,
	TRUNCATION_WHERE_CLAUSE,
	TRUNCATION_VALUES_LISTS,
	TRUNCATION_COLS,
	TRUNCATION_CTE_QUERY,
};

typedef struct {
	List *truncations;
	int32_t depth;
} TruncationState;

typedef struct {
	enum TruncationAttr attr;
	Node *node;
	int32_t depth;
	int32_t length;
} PossibleTruncation;

static bool summary_truncation_options(Node *node, TruncationState *state);
static void sort_truncations(Node *node, TruncationState *state);
static void apply_truncations(Node *node, TruncationState *state);

static int32_t select_target_list_len(List *nodes);
static int32_t select_values_lists_len(List *nodes);
static int32_t update_target_list_len(List *nodes);
static int32_t where_clause_len(Node *node);
static int32_t cols_len(List *nodes);

/*
 * Given a walked parse tree and summary, store the truncated version in `summary`.
 *
 * Returns NULL on success.
 */
PgQueryError *pg_query_summary_truncate(Summary *summary, Node *node)
{
	TruncationState state = {NULL, 0};

	printf("\n\n! pg_query_summary_truncate()\n");
	summary_truncation_options(node, &state);
	return NULL;
}

static void add_truncation(TruncationState *state, enum TruncationAttr attr,
		Node *node, int32_t length)
{
	PossibleTruncation *truncation = palloc(sizeof(PossibleTruncation));

	truncation->attr = attr;
	truncation->node = node;
	truncation->depth = state->depth;
	truncation->length = length;

	state->truncations = lappend(state->truncations, truncation);
}

static void add_truncation_where_clause(TruncationState *state, Node *node, Node *whereClause)
{
	if (whereClause == NULL)
		return;

	add_truncation(state,
			TRUNCATION_WHERE_CLAUSE,
			node,
			where_clause_len(whereClause));
	printf("truncation => ??? / whereClause\n");
}

static bool
summary_truncation_options(Node *node, TruncationState *state)
{
	if (node == NULL)
		return false;
	printf("    summary_truncation_options()\n");

	switch (nodeTag(node))
	{
		case T_RawStmt:
			// For some reason our statement has been trapped in a RawStmt.
			// Free them.
			return summary_truncation_options(castNode(RawStmt, node)->stmt, state);

		case T_SelectStmt:
			{
				SelectStmt *stmt = castNode(SelectStmt, node);

				if (stmt->targetList != NULL) {
					add_truncation(state,
							TRUNCATION_TARGET_LIST,
							node,
							select_target_list_len(stmt->targetList));
					printf("truncation => SelectStmt/targetList\n");
				}

				add_truncation_where_clause(state, node, stmt->whereClause);

				if (stmt->valuesLists != NULL) {
					add_truncation(state,
							TRUNCATION_VALUES_LISTS,
							node,
							select_values_lists_len(stmt->valuesLists));
					printf("truncation => SelectStmt/valuesLists\n");
				}

				break;
			}

		case T_UpdateStmt:
			{
				UpdateStmt *stmt = castNode(UpdateStmt, node);

				if (stmt->targetList != NULL) {
					add_truncation(state,
							TRUNCATION_TARGET_LIST,
							node,
							update_target_list_len(stmt->targetList));
					printf("truncation => UpdateStmt/targetList\n");
				}

				add_truncation_where_clause(state, node, stmt->whereClause);

				break;
			}

		case T_DeleteStmt:
			{
				DeleteStmt *stmt = castNode(DeleteStmt, node);

				add_truncation_where_clause(state, node, stmt->whereClause);

				break;
			}

		case T_CopyStmt:
			{
				CopyStmt *stmt = castNode(CopyStmt, node);

				add_truncation_where_clause(state, node, stmt->whereClause);

				break;
			}

		case T_InsertStmt:
			{
				InsertStmt *stmt = castNode(InsertStmt, node);

				if (stmt->cols != NULL) {
					add_truncation(state, TRUNCATION_COLS, node, cols_len(stmt->cols));
					printf("truncation => InsertStmt/cols\n");
				}

				break;
			}

		case T_IndexStmt:
			{
				IndexStmt *stmt = castNode(IndexStmt, node);
				add_truncation_where_clause(state, node, stmt->whereClause);
				break;
			}

		case T_RuleStmt:
			{
				RuleStmt *stmt = castNode(RuleStmt, node);
				add_truncation_where_clause(state, node, stmt->whereClause);
				break;
			}

		case T_CommonTableExpr:
			{
				CommonTableExpr *stmt = castNode(CommonTableExpr, node);

				if (stmt->ctequery != NULL) {
					int32_t length = 2; // FIXME
					add_truncation(state,
							TRUNCATION_CTE_QUERY,
							node,
							length);
				}

				break;
			}

		case T_InferClause:
			{
				InferClause *stmt = castNode(InferClause, node);
				add_truncation_where_clause(state, node, stmt->whereClause);
				break;
			}

		case T_OnConflictClause:
			{
				OnConflictClause *stmt = castNode(OnConflictClause, node);

				if (stmt->targetList != NULL) {
					add_truncation(state,
							TRUNCATION_TARGET_LIST,
							node,
							update_target_list_len(stmt->targetList));
					printf("truncation => OnConflictClause/targetList\n");
				}

				add_truncation_where_clause(state, node, stmt->whereClause);

				break;
			}

		default:
			printf("    truncation/default (nodeTag = %i)\n", nodeTag(node));
			break;
	}

	if (!pg_query_raw_tree_walker_supports(node))
		return false;

	state->depth++;

	return raw_expression_tree_walker(node, summary_truncation_options, (void *) state);
}

static int32_t select_target_list_len(List *nodes) {
	//let fragment = dummy_select(nodes, None, vec![]).deparse()?;
	//Ok(fragment.len() as i32 - 7) // "SELECT "
	return 1; // FIXME
}

static int32_t select_values_lists_len(List *nodes) {
	//let fragment = dummy_select(vec![], None, nodes).deparse()?;
	//Ok(fragment.len() as i32 - 7) // "SELECT "
	return 2; // FIXME
}

static int32_t update_target_list_len(List *nodes) {
	//let fragment = dummy_update(nodes).deparse()?;
	//Ok(fragment.len() as i32 - 13) // "UPDATE x SET "
	return 3; // FIXME
}

static int32_t where_clause_len(Node *node) {
	//let fragment = dummy_select(vec![], Some(node), vec![]).deparse()?;
	//Ok(fragment.len() as i32 - 13) // "SELECT WHERE "
	return 4; // FIXME
}

static int32_t cols_len(List *nodes) {
	//let fragment = dummy_insert(nodes).deparse()?;
	//Ok(fragment.len() as i32 - 31) // "INSERT INTO x () DEFAULT VALUES"
	return 5;
}
