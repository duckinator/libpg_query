#include "pg_query.h"
#include "pg_query_internal.h"
#include "pg_query_summary.h"

#include "nodes/nodeFuncs.h"

static bool pg_query_summary_truncate_impl(Node *node, Summary *summary);

/*
 * Given a walked parse tree and summary, store the truncated version in `summary`.
 *
 * Returns NULL on success.
 */
PgQueryError *pg_query_summary_truncate(Summary *summary, Node *node)
{
	pg_query_summary_truncate_impl(node, summary);
	return NULL;
}

static bool
pg_query_summary_truncate_impl(Node *node, Summary *summary)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		default:
			break;
	}

	if (!pg_query_raw_tree_walker_supports(node))
		return false;

	return raw_expression_tree_walker(node, pg_query_summary_truncate_impl, (void *) summary);
}
