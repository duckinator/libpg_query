#include "pg_query.h"
#include "pg_query_internal.h"
#include "pg_query_summary.h"

#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"

#include "mb/pg_wchar.h"

/* FIXME:
 * Temporarily copied from https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/nodes/list.c#L1674
 * list_sort() needs to be imported properly.
 * check_list_invariants() is already imported, but duplicated for now because it's static.
 */

static void
check_list_invariants(const List *list)
{
	if (list == NIL)
		return;

	Assert(list->length > 0);
	Assert(list->length <= list->max_length);
	Assert(list->elements != NULL);

	Assert(list->type == T_List ||
		   list->type == T_IntList ||
		   list->type == T_OidList ||
		   list->type == T_XidList);
}

void
list_sort(List *list, list_sort_comparator cmp)
{
	typedef int (*qsort_comparator) (const void *a, const void *b);
	int			len;

	check_list_invariants(list);

	/* Nothing to do if there's less than two elements */
	len = list_length(list);
	if (len > 1)
		qsort(list->elements, len, sizeof(ListCell), (qsort_comparator) cmp);
}

/* FIXME: Temporarily copied from https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/utils/mb/mbutils.c#L1120-L1158
 * pg_mbcharcliplen() needs to be imported properly.
 * it depends on cliplen().
 */

static int
cliplen(const char *str, int len, int limit)
{
	int			l = 0;

	len = Min(len, limit);
	while (l < len && str[l])
		l++;
	return l;
}

int
pg_mbcharcliplen(const char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			nch = 0;
	int			l;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return cliplen(mbstr, len, limit);

	while (len > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);
		nch++;
		if (nch > limit)
			break;
		clen += l;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/* FIXME: temporarily copied from https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/utils/mb/mbutils.c#L1036-L1051 */

int
pg_mbstrlen(const char *mbstr)
{
	int			len = 0;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return strlen(mbstr);

	while (*mbstr)
	{
		mbstr += pg_mblen(mbstr);
		len++;
	}
	return len;
}

/* ========================= */


enum TruncationAttr {
	TRUNCATION_TARGET_LIST,
	TRUNCATION_WHERE_CLAUSE,
	TRUNCATION_VALUES_LISTS,
	TRUNCATION_COLS,
	TRUNCATION_CTE_QUERY,
};

typedef struct {
	PgQueryError *error;
	List *truncations;
	int32_t depth;
} TruncationState;

typedef struct {
	enum TruncationAttr attr;
	Node *node;
	int32_t depth;
	int32_t length;
} PossibleTruncation;

static bool summary_truncation_options(Node *tree, TruncationState *state);
//static void sort_truncations(Node *node, TruncationState *state);
static PgQueryError *apply_truncations(Summary *summary, Node *tree, TruncationState *state, int truncate_limit);
static int cmp_possible_truncation_depth(const ListCell *a, const ListCell *b);

static int32_t select_target_list_len(List *nodes);
static int32_t select_values_lists_len(List *nodes);
static int32_t update_target_list_len(List *nodes);
static int32_t where_clause_len(Node *node);
static int32_t cols_len(List *nodes);

static ColumnRef *dummy_column(void);
static ResTarget *dummy_target(void);
static Node *dummy_select(List *targetList, Node *whereClause, List *valuesLists);
static Node *dummy_insert(List *cols);
static Node *dummy_update(List *targetList);

/*
 * Given a walked parse tree and summary, store the truncated version in `summary`.
 *
 * Returns NULL on success.
 */
PgQueryError *pg_query_summary_truncate(Summary *summary, Node *tree, int truncate_limit)
{
	TruncationState state = {NULL, NULL, 0};

	summary_truncation_options(tree, &state);

	if (state.error)
		return state.error;

	list_sort(state.truncations, cmp_possible_truncation_depth);
	return apply_truncations(summary, tree, &state, truncate_limit);
}

static void truncate_mbstr(char *mbstr, size_t max_chars)
{
	// Determine the number of characters in mbstr.
	int n_chars = pg_mbstrlen(mbstr);

	// If we don't need to truncate the string, return immediately.
	if (n_chars <= max_chars)
		return;

	// Determine how many bytes hold `max_chars - 3`.
	int n_bytes = pg_mbcharcliplen(mbstr, n_chars, max_chars - 3);

	// Actually truncate it.
	strncpy(mbstr + n_bytes, "...", 4);
	mbstr[n_bytes + 3] = '\0';
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
}

static bool
summary_truncation_options(Node *node, TruncationState *state)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_RawStmt:
			// For some reason our statement has been trapped in a RawStmt.
			// Free them.
			return summary_truncation_options(castNode(RawStmt, node)->stmt, state);

		case T_SelectStmt:
			{
				SelectStmt *stmt = castNode(SelectStmt, node);

				if (stmt->targetList != NULL)
					add_truncation(state,
							TRUNCATION_TARGET_LIST,
							node,
							select_target_list_len(stmt->targetList));

				add_truncation_where_clause(state, node, stmt->whereClause);

				if (stmt->valuesLists != NULL)
					add_truncation(state,
							TRUNCATION_VALUES_LISTS,
							node,
							select_values_lists_len(stmt->valuesLists));

				break;
			}

		case T_UpdateStmt:
			{
				UpdateStmt *stmt = castNode(UpdateStmt, node);

				if (stmt->targetList != NULL)
					add_truncation(state,
							TRUNCATION_TARGET_LIST,
							node,
							update_target_list_len(stmt->targetList));

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

				if (stmt->cols != NULL)
					add_truncation(state, TRUNCATION_COLS, node, cols_len(stmt->cols));

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
					PgQueryDeparseResult result = pg_query_deparse(node);

					if (result.error) {
						state->error = result.error;
						return false;
					}

					int32_t length = strlen(result.query);

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

				if (stmt->targetList != NULL)
					add_truncation(state,
							TRUNCATION_TARGET_LIST,
							node,
							update_target_list_len(stmt->targetList));

				add_truncation_where_clause(state, node, stmt->whereClause);

				break;
			}

		default:
			break;
	}

	if (!pg_query_raw_tree_walker_supports(node))
		return false;

	state->depth++;

	return raw_expression_tree_walker(node, summary_truncation_options, (void *) state);
}

static int cmp_possible_truncation_depth(const ListCell *a, const ListCell *b)
{
	const PossibleTruncation *pt_a = lfirst(a);
	const PossibleTruncation *pt_b = lfirst(b);

	return pt_a->depth - pt_b->depth;
}

static void expand_ellipses(char *str)
{
	// https://www.compart.com/en/unicode/U+2026
	// pattern is a double quote, ellipses, and another double quote.
	char pattern[5] = {'"', 0xE2, 0x80, 0xA6, '"'};

	for (size_t i = 0; i < strlen(str); i++) {
		if (memcmp(str + i, pattern, 5) == 0) {
			memcpy(str + i, "...", 3);
			strcpy(str + i + 3, str + i + 5);
		}
	}
}

// This is equivalent to a kludge in the Rust implementation.
// Not sure why the `SELECT ... AS ...` shows up in both Rust and C,
// but not the Ruby implementation.
static void remove_unneeded_as(char *str)
{
	char *pattern = "SELECT ... AS ...";
	size_t plen = strlen(pattern);

	size_t slen = strlen("SELECT ...");

	for (size_t i = 0; i < strlen(str); i++) {
		if (memcmp(str + i, pattern, plen) == 0) {
			strcpy(str + i + slen, str + i + plen);
		}
	}
}

static PgQueryError *apply_truncations(Summary *summary, Node *tree, TruncationState *state, int truncation_limit)
{
	List *truncations = state->truncations;

	PgQueryDeparseResult result = pg_query_deparse(tree);
	char *output = result.query;

	if (result.error)
		return result.error;

	if (strlen(result.query) <= truncation_limit) {
		summary->truncated_query = output;
		return NULL;
	}

	while (list_length(state->truncations) > 0) {
		// Get the first item from the list.
		PossibleTruncation *truncation = linitial(state->truncations);
		// ... and then remove it from the list.
		state->truncations = list_delete_nth_cell(state->truncations, 0);

		Node *node = truncation->node;
		enum TruncationAttr attr = truncation->attr;

		if (truncation->length <= 3) {
			// If "truncating" would make it longer, refuse to truncate.
		} else if (IsA(node, SelectStmt) && attr == TRUNCATION_TARGET_LIST) {
			SelectStmt *stmt = castNode(SelectStmt, node);
			stmt->targetList = list_make1(dummy_target());
			printf("Select/targetList\n");
		}
		else if (IsA(node, SelectStmt) && attr == TRUNCATION_WHERE_CLAUSE) {
			SelectStmt *stmt = castNode(SelectStmt, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("Select/whereClause\n");
		}
		else if (IsA(node, SelectStmt) && attr == TRUNCATION_VALUES_LISTS) {
			SelectStmt *stmt = castNode(SelectStmt, node);
			stmt->valuesLists = list_make1(dummy_column());
			printf("Select/valuesLists\n");
		}
		else if (IsA(node, UpdateStmt) && attr == TRUNCATION_TARGET_LIST) {
			UpdateStmt *stmt = castNode(UpdateStmt, node);
			stmt->targetList = list_make1(dummy_target());
			printf("UpdateStmt/targetList\n");
		}
		else if (IsA(node, UpdateStmt) && attr == TRUNCATION_WHERE_CLAUSE) {
			UpdateStmt *stmt = castNode(UpdateStmt, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("UpdateStmt/whereClause\n");
		}
		else if (IsA(node, DeleteStmt) && attr == TRUNCATION_WHERE_CLAUSE) {
			DeleteStmt *stmt = castNode(DeleteStmt, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("Delete/whereClause\n");
		}
		else if (IsA(node, CopyStmt) && attr == TRUNCATION_WHERE_CLAUSE) {
			CopyStmt *stmt = castNode(CopyStmt, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("Copy/whereClause\n");
		}
		else if (IsA(node, InsertStmt) && attr == TRUNCATION_COLS) {
			InsertStmt *stmt = castNode(InsertStmt, node);
			stmt->cols = list_make1(dummy_target());
			printf("Insert/cols\n");
		}
		else if (IsA(node, IndexStmt) && attr == TRUNCATION_WHERE_CLAUSE) {
			IndexStmt *stmt = castNode(IndexStmt, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("Index/whereClause\n");
		}
		else if (IsA(node, RuleStmt) && attr == TRUNCATION_WHERE_CLAUSE) {
			RuleStmt *stmt = castNode(RuleStmt, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("Rule/whereClause\n");
		}
		else if (IsA(node, CommonTableExpr) && attr == TRUNCATION_CTE_QUERY) {
			// Equivalent to: https://github.com/pganalyze/pg_query.rs/blob/66eb7becea1a40e315fee3f90197a35a89d20c25/src/truncate.rs#L230-L237
			CommonTableExpr *stmt = castNode(CommonTableExpr, node);
			Node *old = stmt->ctequery;
			stmt->ctequery = dummy_select(NULL, (Node *) dummy_column(), NULL);
			if (old) {
				for (size_t i = 0; i < list_length(state->truncations);) {
					PossibleTruncation *t = (PossibleTruncation *) list_nth_cell(state->truncations, i);
					if (t->node == old)
						state->truncations = list_delete_nth_cell(state->truncations, i);
					else
						i++;
				}
			}
			printf("CTE/cteQuery\n");
		}
		else if (IsA(node, InferClause) && attr == TRUNCATION_WHERE_CLAUSE) {
			InferClause *stmt = castNode(InferClause, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("InferClause/whereClause\n");
		}
		else if (IsA(node, OnConflictClause) && attr == TRUNCATION_TARGET_LIST) {
			OnConflictClause *stmt = castNode(OnConflictClause, node);
			stmt->targetList = list_make1(dummy_target());
			printf("OnConflictClause/targetList\n");
		}
		else if (IsA(node, OnConflictClause) && attr == TRUNCATION_WHERE_CLAUSE) {
			OnConflictClause *stmt = castNode(OnConflictClause, node);
			stmt->whereClause = (Node *) dummy_column();
			printf("OnConflictClause/whereClause\n");
		}
		else
		{
			fprintf(stderr, "ERROR: unimplemented truncation");
			exit(1);
		}

		PgQueryDeparseResult result = pg_query_deparse(tree);

		if (result.error)
			return result.error;

		output = result.query;
		expand_ellipses(output);
		remove_unneeded_as(output);

		if (strlen(output) <= truncation_limit) {
			summary->truncated_query = output;
			return NULL;
		}
	}

	truncate_mbstr(output, truncation_limit);
	summary->truncated_query = output;
	return NULL;
}

static ColumnRef *dummy_column(void)
{
	ColumnRef *colref = makeNode(ColumnRef);

	colref->fields = list_make1(makeString(pstrdup("…")));
	colref->location = 0;

	return colref;
}

static ResTarget *dummy_target(void)
{
	ResTarget *target = makeNode(ResTarget);

	target->name = pstrdup("…");
	target->location = 0; // TODO: docs for ResTarget say "-1 if unknown" -- would that be more correct? (see also, dummy_column())
	target->val = (Node *) dummy_column();

	return target;
}

static Node *dummy_select(List *targetList, Node *whereClause, List *valuesLists)
{
	SelectStmt *stmt = makeNode(SelectStmt);

	stmt->targetList = targetList;
	stmt->whereClause = whereClause;
	stmt->valuesLists = valuesLists;

	return (Node *) stmt;
}

static Node *dummy_insert(List *cols)
{
	RangeVar *rv = makeNode(RangeVar);
	rv->relname = pstrdup("x");
	rv->inh = true;
	rv->relpersistence = 'p';
	rv->location = 0;

	InsertStmt *stmt = makeNode(InsertStmt);
	stmt->relation = rv;
	stmt->cols = cols;
	stmt->override = 1;

	return (Node *) stmt;
}

static Node *dummy_update(List *targetList)
{
	RangeVar *rv = makeNode(RangeVar);
	rv->relname = pstrdup("x");
	rv->inh = true;
	rv->relpersistence = 'p';
	rv->location = 0;

	UpdateStmt *stmt = makeNode(UpdateStmt);
	stmt->relation = rv;
	stmt->targetList = targetList;

	return (Node *) stmt;
}

static void print_pg_query_error(PgQueryError *error, const char *func)
{
	fprintf(stderr, "ERROR from %s:\n  %s:%s:%i: %s\n",
			func, error->funcname, error->filename, error->lineno, error->message);
}

static int32_t select_target_list_len(List *nodes) {
	PgQueryDeparseResult result = pg_query_deparse_node(dummy_select(nodes, NULL, NULL));

	if (result.error) {
		print_pg_query_error(result.error, __func__);
		pg_query_free_deparse_result(result);
		return -1;
	}

	int32_t length = (int32_t)strlen(result.query) - 7; // "SELECT "

	pg_query_free_deparse_result(result);

	return length;
}

static int32_t select_values_lists_len(List *nodes) {
	PgQueryDeparseResult result = pg_query_deparse_node(dummy_select(NULL, NULL, nodes));

	if (result.error) {
		print_pg_query_error(result.error, __func__);
		pg_query_free_deparse_result(result);
		return -1;
	}

	int32_t length = (int32_t)strlen(result.query) - 9; // "VALUES ()"

	pg_query_free_deparse_result(result);

	return length;
}

static int32_t update_target_list_len(List *nodes) {
	PgQueryDeparseResult result = pg_query_deparse_node(dummy_update(nodes));

	if (result.error) {
		print_pg_query_error(result.error, __func__);
		pg_query_free_deparse_result(result);
		return -1;
	}

	int32_t length = (int32_t)strlen(result.query) - 13; // "UPDATE x SET "

	pg_query_free_deparse_result(result);

	return length;
}

static int32_t where_clause_len(Node *node) {
	PgQueryDeparseResult result = pg_query_deparse_node(dummy_select(NULL, node, NULL));

	if (result.error) {
		print_pg_query_error(result.error, __func__);
		pg_query_free_deparse_result(result);
		return -1;
	}

	int32_t length = (int32_t)strlen(result.query) - 13; // "SELECT WHERE "

	pg_query_free_deparse_result(result);

	return length;
}

static int32_t cols_len(List *nodes) {
	PgQueryDeparseResult result = pg_query_deparse_node(dummy_insert(nodes));

	if (result.error) {
		print_pg_query_error(result.error, __func__);
		pg_query_free_deparse_result(result);
		return -1;
	}

	int32_t length = (int32_t)strlen(result.query) - 31; // "INSERT INTO x () DEFAULT VALUES"

	pg_query_free_deparse_result(result);

	return length;
}
