/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2012, 2016 by Delphix. All rights reserved.
 */

/*
 * These syntactic sugar features are implemented by transforming the D
 * parse tree such that it only uses the subset of D that is supported
 * by the rest of the compiler / the kernel.  A clause containing these
 * language features is referred to as a "super-clause", and its
 * transformation typically entails creating several "sub-clauses" to
 * implement it. For diagnosability, the sub-clauses will be printed if
 * the "-xtree=8" flag is specified.
 *
 * The features are:
 *
 * "if/else" statements.  Each basic block (e.g. the body of the "if"
 * and "else" statements, and the statements before and after) is turned
 * into its own sub-clause, with a predicate that causes it to be
 * executed only if the code flows to this point.  Nested if/else
 * statements are supported.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>

#include <assert.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <dt_module.h>
#include <dt_program.h>
#include <dt_provider.h>
#include <dt_printf.h>
#include <dt_pid.h>
#include <dt_grammar.h>
#include <dt_ident.h>
#include <dt_string.h>
#include <dt_impl.h>

typedef struct xd_parse {
	dtrace_hdl_t *xp_dtp;
	dt_node_t *xp_pdescs;		/* probe descriptions */
	int xp_num_conditions;
	int xp_num_ifs;
	dt_node_t *xp_clause_list;
} xd_parse_t;

static void xd_visit_stmts(xd_parse_t *, dt_node_t *, int);

/*
 * Return a node for "self->%error".
 *
 * Note that the "%" is part of the variable name, and is included so that
 * this variable name can not collide with any user-specified variable.
 *
 * This error variable is used to keep track of if there has been an error
 * in any of the sub-clauses, and is used to prevent execution of subsequent
 * sub-clauses following an error.
 */
static dt_node_t *
xd_new_error_var(void)
{
	return (dt_node_op2(DT_TOK_PTR, dt_node_ident(strdup("self")),
	    dt_node_ident(strdup("%error"))));
}

/*
 * Append this clause to the clause list.
 */
static void
xd_append_clause(xd_parse_t *dp, dt_node_t *clause)
{
	dp->xp_clause_list = dt_node_link(dp->xp_clause_list, clause);
}

/*
 * Prepend this clause to the clause list.
 */
static void
xd_prepend_clause(xd_parse_t *dp, dt_node_t *clause)
{
	dp->xp_clause_list = dt_node_link(clause, dp->xp_clause_list);
}

/*
 * Return a node for "this->%condition_<condid>", or NULL if condid==0.
 *
 * Note that the "%" is part of the variable name, and is included so that
 * this variable name can not collide with any user-specified variable.
 */
static dt_node_t *
xd_new_condition_var(int condid)
{
	char *str;

	if (condid == 0)
		return (NULL);
	assert(condid > 0);

	(void) asprintf(&str, "%%condition_%d", ABS(condid));
	return (dt_node_op2(DT_TOK_PTR, dt_node_ident(strdup("this")),
	    dt_node_ident(str)));
}

/*
 * Return new clause to evaluate predicate and set newcond.  condid is
 * the condition that we are already under, or 0 if none.
 * The new clause will be of the form:
 *
 * dp_pdescs
 * /!self->%error/
 * {
 *	this->%condition_<newcond> =
 *	    (this->%condition_<condid> && pred);
 * }
 *
 * Note: if condid==0, we will instead do "... = (1 && pred)", to effectively
 * convert the pred to a boolean.
 *
 * Note: Unless an error has been encountered, we always set the condition
 * variable (either to 0 or 1).  This lets us avoid resetting the condition
 * variables back to 0 when the super-clause completes.
 */
static dt_node_t *
xd_new_condition_impl(xd_parse_t *dp, dt_node_t *pred, int condid, int newcond)
{
	dt_node_t *value, *body, *newpred;

	/* predicate is !self->%error */
	newpred = dt_node_op1(DT_TOK_LNEG, xd_new_error_var());

	if (condid == 0) {
		/*
		 * value is (1 && pred)
		 *
		 * Note, D doesn't allow a probe-local "this" variable to
		 * be reused as a different type, even from a different probe.
		 * Therefore, value can't simply be <pred>, because then
		 * its type could be different when we reuse this condid
		 * in a different meta-clause.
		 */
		value = dt_node_op2(DT_TOK_LAND, dt_node_int(1), pred);
	} else {
		/* value is (this->%condition_<condid> && pred) */
		value = dt_node_op2(DT_TOK_LAND,
		    xd_new_condition_var(condid), pred);
	}

	/* body is "this->%condition_<retval> = <value>;" */
	body = dt_node_statement(dt_node_op2(DT_TOK_ASGN,
	    xd_new_condition_var(newcond), value));

	return (dt_node_clause(dp->xp_pdescs, newpred, body));
}

/*
 * Generate a new clause to evaluate predicate and set a new condition variable,
 * whose ID will be returned.  The new clause will be appended to
 * dp_first_new_clause.
 */
static int
xd_new_condition(xd_parse_t *dp, dt_node_t *pred, int condid)
{
	dp->xp_num_conditions++;
	xd_append_clause(dp, xd_new_condition_impl(dp,
	    pred, condid, dp->xp_num_conditions));
	return (dp->xp_num_conditions);
}

/*
 * Visit the specified node and all of its descendants.  Replace any
 * entry_* variables (e.g. entry_walltimestamp,entry_args[1], entry_elapsed_us)
 * and callers[""] variables (e.g. callers["spa_sync"]) with the corresponding
 * straight-D nodes.
 */
static void
xd_visit_all(xd_parse_t *dp, dt_node_t *dnp)
{
	dt_node_t *arg;

	switch (dnp->dn_kind) {
	case DT_NODE_FREE:
	case DT_NODE_INT:
	case DT_NODE_STRING:
	case DT_NODE_SYM:
	case DT_NODE_TYPE:
	case DT_NODE_PROBE:
	case DT_NODE_PDESC:
	case DT_NODE_IDENT:
		break;

	case DT_NODE_FUNC:
		for (arg = dnp->dn_args; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);
		break;

	case DT_NODE_OP1:
		xd_visit_all(dp, dnp->dn_child);
		break;

	case DT_NODE_OP2:
		xd_visit_all(dp, dnp->dn_left);
		xd_visit_all(dp, dnp->dn_right);
		if (dnp->dn_op == DT_TOK_LBRAC) {
			dt_node_t *ln = dnp->dn_right;
			while (ln->dn_list != NULL) {
				xd_visit_all(dp, ln->dn_list);
				ln = ln->dn_list;
			}
		}
		break;

	case DT_NODE_OP3:
		xd_visit_all(dp, dnp->dn_expr);
		xd_visit_all(dp, dnp->dn_left);
		xd_visit_all(dp, dnp->dn_right);
		break;

	case DT_NODE_DEXPR:
	case DT_NODE_DFUNC:
		xd_visit_all(dp, dnp->dn_expr);
		break;

	case DT_NODE_AGG:

		for (arg = dnp->dn_aggtup; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);

		if (dnp->dn_aggfun)
			xd_visit_all(dp, dnp->dn_aggfun);
		break;

	case DT_NODE_CLAUSE:
		for (arg = dnp->dn_pdescs; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);

		if (dnp->dn_pred != NULL)
			xd_visit_all(dp, dnp->dn_pred);

		for (arg = dnp->dn_acts; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);
		break;

	case DT_NODE_INLINE: {
		const dt_idnode_t *inp = dnp->dn_ident->di_iarg;

		xd_visit_all(dp, inp->din_root);
		break;
	}
	case DT_NODE_MEMBER:
		if (dnp->dn_membexpr)
			xd_visit_all(dp, dnp->dn_membexpr);
		break;

	case DT_NODE_XLATOR:
		for (arg = dnp->dn_members; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);
		break;

	case DT_NODE_PROVIDER:
		for (arg = dnp->dn_probes; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);
		break;

	case DT_NODE_PROG:
		for (arg = dnp->dn_list; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);
		break;

	case DT_NODE_IF:
		dp->xp_num_ifs++;
		xd_visit_all(dp, dnp->dn_conditional);

		for (arg = dnp->dn_body; arg != NULL; arg = arg->dn_list)
			xd_visit_all(dp, arg);
		for (arg = dnp->dn_alternate_body; arg != NULL;
		    arg = arg->dn_list)
			xd_visit_all(dp, arg);

		break;

	default:
		(void) dnerror(dnp, D_UNKNOWN, "bad node %p, kind %d\n",
		    (void *)dnp, dnp->dn_kind);
	}
}

/*
 * Return a new clause which resets the error variable to zero:
 *
 *   dp_pdescs{ self->%error = 0; }
 *
 * This clause will be executed at the beginning of each meta-clause, to
 * ensure the error variable is unset (in case the previous meta-clause
 * failed).
 */
static dt_node_t *
xd_new_clearerror_clause(xd_parse_t *dp)
{
	dt_node_t *stmt = dt_node_statement(dt_node_op2(DT_TOK_ASGN,
	    xd_new_error_var(), dt_node_int(0)));
	return (dt_node_clause(dp->xp_pdescs, NULL, stmt));
}

/*
 * Evaluate the conditional, and recursively visit the body of the "if"
 * statement (and the "else", if present).
 */
static void
xd_do_if(xd_parse_t *dp, dt_node_t *if_stmt, int precondition)
{
	int newid;

	assert(if_stmt->dn_kind == DT_NODE_IF);

	/* condition */
	newid = xd_new_condition(dp, if_stmt->dn_conditional, precondition);

	/* body of if */
	xd_visit_stmts(dp, if_stmt->dn_body, newid);

	/*
	 * Visit the body of the "else" statement, if present.  Note that we
	 * generate a new condition which is the inverse of the previous
	 * condition.
	 */
	if (if_stmt->dn_alternate_body != NULL) {
		dt_node_t *pred =
		    dt_node_op1(DT_TOK_LNEG, xd_new_condition_var(newid));
		xd_visit_stmts(dp, if_stmt->dn_alternate_body,
		    xd_new_condition(dp, pred, precondition));
	}
}

/*
 * Generate a new clause to evaluate the statements based on the condition.
 * The new clause will be appended to dp_first_new_clause.
 *
 * dp_pdescs
 * /!self->%error && this->%condition_<condid>/
 * {
 *	stmts
 * }
 */
static void
xd_new_basic_block(xd_parse_t *dp, int condid, dt_node_t *stmts)
{
	dt_node_t *pred = NULL;

	if (condid == 0) {
		/*
		 * Don't bother with !error on the first clause, because if
		 * there is only one clause, we don't add the prelude to
		 * zero out %error.
		 */
		if (dp->xp_num_conditions != 0)
			pred = dt_node_op1(DT_TOK_LNEG, xd_new_error_var());
	} else {
		pred = dt_node_op2(DT_TOK_LAND,
		    dt_node_op1(DT_TOK_LNEG, xd_new_error_var()),
		    xd_new_condition_var(condid));
	}
	xd_append_clause(dp,
	    dt_node_clause(dp->xp_pdescs, pred, stmts));
}

/*
 * Visit all the statements in this list, and break them into basic blocks,
 * generating new clauses for "if" and "else" statements.
 */
static void
xd_visit_stmts(xd_parse_t *dp, dt_node_t *stmts, int precondition)
{
	dt_node_t *stmt;
	dt_node_t *prev_stmt = NULL;
	dt_node_t *next_stmt;
	dt_node_t *first_stmt_in_basic_block = NULL;

	for (stmt = stmts; stmt != NULL; stmt = next_stmt) {
		next_stmt = stmt->dn_list;

		if (stmt->dn_kind != DT_NODE_IF) {
			if (first_stmt_in_basic_block == NULL)
				first_stmt_in_basic_block = stmt;
			prev_stmt = stmt;
			continue;
		}

		/*
		 * Remove this and following statements from the previous
		 * clause.
		 */
		if (prev_stmt != NULL)
			prev_stmt->dn_list = NULL;

		/*
		 * Generate clause for statements preceding the "if"
		 */
		if (first_stmt_in_basic_block != NULL) {
			xd_new_basic_block(dp, precondition,
			    first_stmt_in_basic_block);
		}

		xd_do_if(dp, stmt, precondition);

		first_stmt_in_basic_block = NULL;

		prev_stmt = stmt;
	}

	/* generate clause for statements after last "if". */
	if (first_stmt_in_basic_block != NULL)
		xd_new_basic_block(dp, precondition, first_stmt_in_basic_block);
}

/*
 * Generate a new clause which will set the error variable when an error occurs.
 * Only one of these clauses is created per program (e.g. script file).
 * The clause is:
 *
 * dtrace:::ERROR{ self->%error = 1; }
 */
static dt_node_t *
xd_makeerrorclause(void)
{
	dt_node_t *acts, *pdesc;

	pdesc = dt_node_pdesc_by_name(strdup("dtrace:::ERROR"));

	acts = dt_node_statement(dt_node_op2(DT_TOK_ASGN,
	    xd_new_error_var(), dt_node_int(1)));

	return (dt_node_clause(pdesc, NULL, acts));
}

/*
 * Transform the super-clause into straight-D, returning the new list of
 * sub-clauses.
 */
dt_node_t *
dt_compile_sugar(dtrace_hdl_t *dtp, dt_node_t *clause)
{
	xd_parse_t dp = { 0 };
	int condid = 0;

	dp.xp_dtp = dtp;
	dp.xp_pdescs = clause->dn_pdescs;

	/* make dt_node_int() generate an "int"-typed integer */
	yyintdecimal = B_TRUE;
	yyintsuffix[0] = '\0';
	yyintprefix = 0;

	xd_visit_all(&dp, clause);

	if (dp.xp_num_ifs == 0 && dp.xp_num_conditions == 0) {
		/*
		 * There is nothing that modifies the number of clauses.
		 * Use the existing clause as-is, with its predicate intact.
		 * This ensures that in the absence of D++, the body of the
		 * clause can create a variable that is referenced in the
		 * predicate.
		 */
		xd_append_clause(&dp, dt_node_clause(clause->dn_pdescs,
		    clause->dn_pred, clause->dn_acts));
	} else {
		if (clause->dn_pred != NULL)
			condid = xd_new_condition(&dp, clause->dn_pred, condid);

		if (clause->dn_acts == NULL) {
			/*
			 * xd_visit_stmts() does not emit a clause with
			 * an empty body (e.g. if there's an empty "if" body),
			 * but we need the empty body here so that we
			 * continue to get the default tracing action.
			 */
			xd_new_basic_block(&dp, condid, NULL);
		} else {
			xd_visit_stmts(&dp, clause->dn_acts, condid);
		}
	}

	if (dp.xp_num_conditions != 0)
		xd_prepend_clause(&dp, xd_new_clearerror_clause(&dp));
	if (dp.xp_clause_list != NULL &&
	    dp.xp_clause_list->dn_list != NULL && !dtp->dt_has_sugar) {
		dtp->dt_has_sugar = B_TRUE;
		xd_prepend_clause(&dp, xd_makeerrorclause());
	}
	return (dp.xp_clause_list);
}
