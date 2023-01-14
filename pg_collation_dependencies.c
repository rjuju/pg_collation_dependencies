/*-------------------------------------------------------------------------
 *
 * pg_collation_dependencies.c: Find direct and indirect dependencies on
 *                              collations that can be corrupted in case of
 *                              underlying library upgrade.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (C) 2022-2023: Julien Rouhaud
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#if PG_VERSION_NUM < 120000
#include "access/htup_details.h"
#endif
#if PG_VERSION_NUM >= 120000
#include "access/relation.h"
#include "access/table.h"
#endif
#if PG_VERSION_NUM < 120000
#include "access/sysattr.h"
#endif
#if PG_VERSION_NUM < 140000
#include "catalog/indexing.h"
#endif
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

#define PG_COLL_DEP_COLS         1

#if PG_VERSION_NUM < 120000
#define table_open(o, l)	heap_open(o, l)
#define table_close(o, l)	heap_close(o, l)
#define Anum_pg_constraint_oid	ObjectIdAttributeNumber
#endif

#if PG_VERSION_NUM < 130000
#define IsOidList(l)			((l) == NIL || IsA((l), OidList))
#define list_sort(l, c)		list_qsort(l, c)
#ifdef USE_ASSERT_CHECKING
/*
 * Check that the specified List is valid (so far as we can tell).
 */
static void
check_list_invariants(const List *list)
{
	if (list == NIL)
		return;

	Assert(list->length > 0);
	Assert(list->head != NULL);
	Assert(list->tail != NULL);

	Assert(list->type == T_List ||
		   list->type == T_IntList ||
		   list->type == T_OidList);

	if (list->length == 1)
		Assert(list->head == list->tail);
	if (list->length == 2)
		Assert(list->head->next == list->tail);
	Assert(list->tail->next == NULL);
}
#else
#define check_list_invariants(l)
#endif							/* USE_ASSERT_CHECKING */

#endif							/* pg13- */

/*
 * Used when inspecting expressions.  Just stored all the seen collations.
 */
typedef struct pgcdWalkerContext
{
	List *collations;
} pgcdWalkerContext;

/*--- Functions --- */

extern PGDLLEXPORT Datum	pg_collation_constraint_dependencies(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum	pg_collation_index_dependencies(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum	pg_collation_matview_dependencies(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_collation_constraint_dependencies);
PG_FUNCTION_INFO_V1(pg_collation_index_dependencies);
PG_FUNCTION_INFO_V1(pg_collation_matview_dependencies);

#if PG_VERSION_NUM < 150000
/* flag bits for InitMaterializedSRF() */
#define MAT_SRF_USE_EXPECTED_DESC	0x01	/* use expectedDesc as tupdesc. */
#define MAT_SRF_BLESS				0x02	/* "Bless" a tuple descriptor with
											 * BlessTupleDesc(). */
static void InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags);
#endif

#if PG_VERSION_NUM < 130000
/*
 * list_sort comparator for sorting a list into ascending OID order.
 */
static int
list_oid_cmp(const void *a, const void *b)
{
	Oid			v1 = lfirst_oid(*(ListCell **) a);
	Oid			v2 = lfirst_oid(*(ListCell **) b);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}

/*
 * Remove adjacent duplicates in a list of OIDs.
 *
 * It is caller's responsibility to have sorted the list to bring duplicates
 * together, perhaps via list_sort(list, list_oid_cmp).
 *
 * Note that this takes time proportional to the length of the list.
 */
static void
list_deduplicate_oid(List *list)
{
	int			len;

	Assert(IsOidList(list));
	len = list_length(list);
	if (len > 1)
	{
		ListCell   *lc = list->head;

		list->length = 1;

		while (lc->next)
		{
			ListCell   *next = lc->next;

			if (lc->data.oid_value == next->data.oid_value)
				lc->next = next->next;
			else
			{
				list->length++;
				lc = next;
			}
		}

		list->tail = lc;
	}
	check_list_invariants(list);
}
#endif

static bool pgcd_query_expression_walker(Node *node, pgcdWalkerContext *context);
static List *pgcd_get_rel_collations(Oid relid);
static List *pgcd_get_constraint_collations(Oid conid);
static List *pgcd_get_query_expression_collations(Node *expr);
static List *pgcd_get_range_type_collations(Oid rngid, bool ismultirange);
static List *pgcd_get_type_collations(Oid typid);
static List *pgcd_constraint_deps(Oid index_oid);
static List *pgcd_index_deps(Oid index_oid);
static List *pgcd_matview_deps(Oid matview_oid);

#if PG_VERSION_NUM < 150000
static void
InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags)
{
	bool		random_access;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	MemoryContext old_context,
				per_query_ctx;
	TupleDesc	stored_tupdesc;

	/* check to see if caller supports returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		((flags & MAT_SRF_USE_EXPECTED_DESC) != 0 && rsinfo->expectedDesc == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/*
	 * Store the tuplestore and the tuple descriptor in ReturnSetInfo.  This
	 * must be done in the per-query memory context.
	 */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	old_context = MemoryContextSwitchTo(per_query_ctx);

	/* build a tuple descriptor for our result type */
	if ((flags & MAT_SRF_USE_EXPECTED_DESC) != 0)
		stored_tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	else
	{
		if (get_call_result_type(fcinfo, NULL, &stored_tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
	}

	/* If requested, bless the tuple descriptor */
	if ((flags & MAT_SRF_BLESS) != 0)
		BlessTupleDesc(stored_tupdesc);

	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

	tupstore = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = stored_tupdesc;
	MemoryContextSwitchTo(old_context);
}
#endif

/*
 * Walker function to find collations in expressions.
 *
 * Don't try to be smart here for now, just remember all collations seen,
 * coming from explicit collation or underlying types even if there can be
 * false positive or redundant values.
 */
static bool
pgcd_query_expression_walker(Node *node, pgcdWalkerContext *context)
{
	if (!node)
		return false;

#define APPEND_COLL(l, o) { \
	if (OidIsValid(o)) \
		(l) = lappend_oid(l, o); \
}

#define APPEND_TYPE_COLLS(l, o) { \
	(l) = list_concat((l), pgcd_get_type_collations(o)); \
}

	switch (node->type)
	{
		case T_TableFunc:
		{
			TableFunc *func = (TableFunc *) node;
			ListCell  *lc;

			foreach(lc, func->colcollations)
				APPEND_COLL(context->collations, lfirst_oid(lc));

			foreach(lc, func->coltypes)
				APPEND_TYPE_COLLS(context->collations, lfirst_oid(lc));

			break;
		}
		case T_Var:
		{
			Var *var = (Var *) node;

			APPEND_COLL(context->collations, var->varcollid);
			APPEND_TYPE_COLLS(context->collations, var->vartype);

			break;
		}
		case T_Const:
		{
			Const *c = (Const *) node;

			APPEND_COLL(context->collations, c->constcollid);
			APPEND_TYPE_COLLS(context->collations, c->consttype);

			break;
		}
		case T_Param:
		{
			Param *param = (Param *) node;

			APPEND_COLL(context->collations, param->paramcollid);
			APPEND_TYPE_COLLS(context->collations, param->paramtype);

			break;
		}
#if PG_VERSION_NUM >= 120000
		case T_SubscriptingRef:
		{
			SubscriptingRef *ref = (SubscriptingRef *) node;

			APPEND_COLL(context->collations, ref->refcollid);
			APPEND_COLL(context->collations, ref->refcontainertype);
			APPEND_COLL(context->collations, ref->refelemtype);
#if PG_VERSION_NUM >= 140000
			APPEND_COLL(context->collations, ref->refrestype);
#endif

			break;
		}
#endif
		case T_FuncExpr:
		{
			FuncExpr *func = (FuncExpr *) node;

			APPEND_COLL(context->collations, func->funccollid);
			APPEND_COLL(context->collations, func->inputcollid);
			APPEND_TYPE_COLLS(context->collations, func->funcresulttype);

			break;
		}
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		{
			OpExpr *op = (OpExpr *) node;

			APPEND_COLL(context->collations, op->opcollid);
			APPEND_COLL(context->collations, op->inputcollid);
			APPEND_TYPE_COLLS(context->collations, op->opresulttype);

			break;
		}
		case T_ScalarArrayOpExpr:
		{
			ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) node;

			APPEND_COLL(context->collations, op->inputcollid);

			break;
		}
		case T_FieldSelect:
		{
			FieldSelect *f = (FieldSelect *) node;

			APPEND_COLL(context->collations, f->resultcollid);
			APPEND_TYPE_COLLS(context->collations, f->resulttype);

			break;
		}
		case T_RelabelType:
		{
			RelabelType *relabel = (RelabelType *) node;

			APPEND_COLL(context->collations, relabel->resultcollid);
			APPEND_TYPE_COLLS(context->collations, relabel->resulttype);

			break;
		}
		case T_CoerceViaIO:
		{
			CoerceViaIO *coerce = (CoerceViaIO *) node;

			APPEND_COLL(context->collations, coerce->resultcollid);
			APPEND_TYPE_COLLS(context->collations, coerce->resulttype);

			break;
		}
		case T_ArrayCoerceExpr:
		{
			ArrayCoerceExpr *coerce = (ArrayCoerceExpr *) node;

			APPEND_COLL(context->collations, coerce->resultcollid);
			APPEND_TYPE_COLLS(context->collations, coerce->resulttype);

			break;
		}
		case T_ConvertRowtypeExpr:
		{
			ConvertRowtypeExpr *crte = (ConvertRowtypeExpr *) node;

			APPEND_TYPE_COLLS(context->collations, crte->resulttype);

			break;
		}
		case T_CollateExpr:
		{
			CollateExpr *expr = (CollateExpr *) node;

			APPEND_COLL(context->collations, expr->collOid);

			break;
		}
		case T_CaseExpr:
		{
			CaseExpr *expr = (CaseExpr *) node;

			APPEND_COLL(context->collations, expr->casecollid);
			APPEND_TYPE_COLLS(context->collations, expr->casetype);

			break;
		}
		case T_CaseTestExpr:
		{
			CaseTestExpr *expr = (CaseTestExpr *) node;

			APPEND_COLL(context->collations, expr->collation);

			break;
		}
		case T_ArrayExpr:
		{
			ArrayExpr *expr = (ArrayExpr *) node;

			APPEND_COLL(context->collations, expr->array_collid);
			APPEND_TYPE_COLLS(context->collations, expr->array_typeid);

			break;
		}
		case T_RowExpr:
		{
			RowExpr *expr = (RowExpr *) node;

			if (expr->row_typeid != RECORDOID)
				APPEND_TYPE_COLLS(context->collations, expr->row_typeid);

			break;
		}
		case T_RowCompareExpr:
		{
			RowCompareExpr *expr = (RowCompareExpr *) node;
			ListCell	   *lc;

			foreach(lc, expr->inputcollids)
				APPEND_COLL(context->collations, lfirst_oid(lc));

			break;
		}
		case T_CoalesceExpr:
		{
			CoalesceExpr *expr = (CoalesceExpr *) node;

			APPEND_COLL(context->collations, expr->coalescecollid);
			APPEND_TYPE_COLLS(context->collations, expr->coalescetype);

			break;
		}
#if PG_VERSION_NUM < 160000
		case T_SQLValueFunction:
		{
			SQLValueFunction *expr = (SQLValueFunction *) node;

			APPEND_COLL(context->collations, expr->type);

			break;
		}
#endif
		case T_MinMaxExpr:
		{
			MinMaxExpr *expr = (MinMaxExpr *) node;

			APPEND_COLL(context->collations, expr->minmaxcollid);
			APPEND_COLL(context->collations, expr->inputcollid);
			APPEND_TYPE_COLLS(context->collations, expr->minmaxtype);

			break;
		}
		case T_CoerceToDomain:
		{
			CoerceToDomain *coerce = (CoerceToDomain *) node;

			APPEND_COLL(context->collations, coerce->resultcollid);

			APPEND_TYPE_COLLS(context->collations, coerce->resulttype);

			/*
			 * If the underlying expression is a direct scalar reference we can
			 * guarantee that the underlying collations won't be used, so
			 * ignore them.
			 */
			if (IsA(coerce->arg, Const) || IsA(coerce->arg, Var))
				return false;

			break;
		}
		case T_CoerceToDomainValue:
		{
			CoerceToDomainValue *coerce = (CoerceToDomainValue *) node;

			APPEND_COLL(context->collations, coerce->collation);
			APPEND_TYPE_COLLS(context->collations, coerce->typeId);

			break;
		}
		case T_Aggref:
		{
			Aggref *ref = (Aggref *) node;

			APPEND_COLL(context->collations, ref->aggcollid);
			APPEND_COLL(context->collations, ref->inputcollid);
			APPEND_TYPE_COLLS(context->collations, ref->aggtype);

			break;
		}
		case T_Query:
		{
			return query_tree_walker((Query *) node,
									 pgcd_query_expression_walker,
									 context, 0);
		}
		case T_RangeTblFunction:
		{
			RangeTblFunction *func = (RangeTblFunction *) node;
			ListCell	   *lc;

			foreach(lc, func->funccolcollations)
				APPEND_COLL(context->collations, lfirst_oid(lc));

			foreach(lc, func->funccoltypes)
				APPEND_TYPE_COLLS(context->collations, lfirst_oid(lc));

			break;
		}
		case T_SetOperationStmt:
		{
			SetOperationStmt *stmt = (SetOperationStmt *) node;
			ListCell	 *lc;

			foreach(lc, stmt->colCollations)
				APPEND_COLL(context->collations, lfirst_oid(lc));

			foreach(lc, stmt->colTypes)
				APPEND_TYPE_COLLS(context->collations, lfirst_oid(lc));

			break;
		}
		case T_WindowFunc:
		{
			WindowFunc *func = (WindowFunc *) node;

			APPEND_COLL(context->collations, func->wincollid);
			APPEND_COLL(context->collations, func->inputcollid);
			APPEND_TYPE_COLLS(context->collations, func->wintype);

			break;
		}
		case T_CommonTableExpr:
		{
			CommonTableExpr *expr = (CommonTableExpr *) node;
			ListCell		*lc;

			foreach(lc, expr->ctecolcollations)
				APPEND_COLL(context->collations, lfirst_oid(lc));
			foreach(lc, expr->ctecoltypes)
				APPEND_TYPE_COLLS(context->collations, lfirst_oid(lc));

			break;
		}
		/* Those nodes can appear but nothing specific to do. */
		case T_JoinExpr:
		case T_FromExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
		case T_SubLink:
		case T_TableSampleClause:
		case T_TargetEntry:
		case T_Alias:
		case T_RangeVar:
		case T_IntoClause:
		case T_NamedArgExpr:
		case T_BoolExpr:
		case T_CaseWhen:
		case T_XmlExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_List:
			/* Nothing to do, normal exprssion walker is enough. */
			break;
		/* The rest shouldn't be reachable in the supported objects. */
		default:
			elog(ERROR, "unexpected node type %d (%s)", node->type,
				 nodeToString(node));
			break;
	}

#undef APPEND_COLL
#undef APPEND_TYPE_COLLS

	return expression_tree_walker(node, pgcd_query_expression_walker, context);
}

/*
 * Get full list of collation dependencies for the given composite type or
 * relation.
 *
 * This only looks at the column list, so it's not usable for more complex
 * objects like materialized views.
 */
static List *
pgcd_get_rel_collations(Oid relid)
{
	List	   *res = NIL;
	Relation	typRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	typRel = table_open(AttributeRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
			Anum_pg_attribute_attrelid,
			BTEqualStrategyNumber, F_OIDEQ,
			ObjectIdGetDatum(relid));

	scan = systable_beginscan(typRel, AttributeRelidNumIndexId, true, NULL, 1,
							  key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attribute pg_att = (Form_pg_attribute) GETSTRUCT(tup);

		Assert(AttributeNumberIsValid(pg_att->attnum));

		/* System columns are guaranteed to not rely on any collation. */
		if (pg_att->attnum < 0)
		{
			/* Composite types don't have system columns. */
			Assert(get_rel_relkind(relid) != RELKIND_COMPOSITE_TYPE);
			continue;
		}

		/* If the attribute has a collation, use it. */
		if (OidIsValid(pg_att->attcollation))
			res = lappend_oid(res, pg_att->attcollation);

		/* And recurse in case there's nested types. */
		res = list_concat(res, pgcd_get_type_collations(pg_att->atttypid));
	}

	systable_endscan(scan);
	table_close(typRel, NoLock);

	return res;
}

/*
 * Get full list of collation dependencies for the given constraint.
 */
static List *
pgcd_get_constraint_collations(Oid conid)
{
	List			   *res = NIL;
	Datum				datum;
	bool				isnull;
	bool				found_conbin = false;
	Relation			conRel;
	ScanKeyData			key[1];
	SysScanDesc			scan;
	HeapTuple			tup;

	conRel = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_constraint_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conid));

	scan = systable_beginscan(conRel, ConstraintOidIndexId, true, NULL, 1, key);

	tup = systable_getnext(scan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find constraint %u", conid);

	/* Get the collations from the stored expression, if any. */
	datum = SysCacheGetAttr(CONSTROID, tup, Anum_pg_constraint_conbin, &isnull);
	if (!isnull)
	{
		char	   *expr;
		Node	   *node;

		found_conbin = true;

		expr = TextDatumGetCString(datum);
		node = stringToNode(expr);

		res = pgcd_get_query_expression_collations(node);
	}

	/* Get the collations for the underlying keys, if any. */
	datum = SysCacheGetAttr(CONSTROID, tup, Anum_pg_constraint_conkey, &isnull);
	if (!isnull)
	{
		Form_pg_constraint pg_constraint;
		Relation	rel;
		ArrayType  *arr;
		AttrNumber *conkeys;
		int			numkeys;

		pg_constraint = (Form_pg_constraint) GETSTRUCT(tup);

		Assert(OidIsValid(pg_constraint->conrelid));

		rel = relation_open(pg_constraint->conrelid, AccessShareLock);

		arr = DatumGetArrayTypeP(datum);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != INT2OID)
			elog(ERROR, "conkey is not a 1-D smallint array");

		numkeys = ARR_DIMS(arr)[0];
		conkeys = (AttrNumber *) ARR_DATA_PTR(arr);
		for (int i = 0; i < numkeys; i++)
		{
			Oid		attnum = conkeys[i];
			Oid		atttypid;

			/*
			 * Constraints on whole-row don't have a valid attnum, we can
			 * simply skip those as the underlying collation(s) have already
			 * been detected while processing the underlying Vars in the
			 * associated expression..
			 */
			if (!AttributeNumberIsValid(attnum))
			{
				/* We should have seen an expression. */
				Assert(found_conbin);
				continue;
			}

			atttypid = TupleDescAttr(rel->rd_att, attnum - 1)->atttypid;

			res = list_concat(res, pgcd_get_type_collations(atttypid));
		}

		relation_close(rel, NoLock);
	}

	systable_endscan(scan);
	table_close(conRel, NoLock);

	return res;
}

/*
 * Get full list of collation dependencies for the given expression.
 */
static List *
pgcd_get_query_expression_collations(Node *expr)
{
	pgcdWalkerContext context;

	context.collations = NIL;
	query_or_expression_tree_walker(expr, pgcd_query_expression_walker,
									(void *) &context, 0);

	return context.collations;
}

/*
 * Get full list of collation dependencies for the given (multi)range type.
 */
static List *
pgcd_get_range_type_collations(Oid rngid, bool ismultirange)
{
	List		   *res = NIL;
	Form_pg_range	pg_range;
	Relation		rngRel;
	ScanKeyData		key[1];
	SysScanDesc		scan;
	HeapTuple		tup;

	rngRel = table_open(RangeRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
#if PG_VERSION_NUM >= 140000
				ismultirange ? Anum_pg_range_rngmultitypid :
#endif
				Anum_pg_range_rngtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rngid));

	scan = systable_beginscan(rngRel,
#if PG_VERSION_NUM >= 140000
							  ismultirange ? RangeMultirangeTypidIndexId :
#endif
							  RangeTypidIndexId,
							  true, NULL, 1, key);

	tup = systable_getnext(scan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find range %u", rngid);

	pg_range = (Form_pg_range) GETSTRUCT(tup);

	/* Remember the range collation if any. */
	if (OidIsValid(pg_range->rngcollation))
		res = lappend_oid(res, pg_range->rngcollation);

	/* And recurse in case there's nested types. */
	res = list_concat(res, pgcd_get_type_collations(pg_range->rngsubtype));

	systable_endscan(scan);
	table_close(rngRel, NoLock);

	return res;
}

/*
 * Get full list of collation dependencies for the given type.
 */
static List *
pgcd_get_type_collations(Oid typid)
{
	Form_pg_type typtup;
	HeapTuple	tp;
	List	   *res = NIL;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	/*
	 * Caller should have a lock on the owning object, so the type can't be
	 * dropped concurrently.
	 */
	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "could not find type %u", typid);

	typtup = (Form_pg_type) GETSTRUCT(tp);

	/*
	 * If the recorded collation is valid, just use it.  Otherwise inspect the
	 * type to see if there's any underlying collation.
	 */
	if (OidIsValid(typtup->typcollation))
		res = lappend_oid(res, typtup->typcollation);
	else if (OidIsValid(typtup->typelem))
	{
		/* Subscripting, get the info for the underlying type. */
		res = list_concat(res, pgcd_get_type_collations(typtup->typelem));
	}
	else if (OidIsValid(typtup->typbasetype))
	{
		/* Domain, inspect the base type. */
		res = list_concat(res, pgcd_get_type_collations(typtup->typbasetype));
	}
	else if (OidIsValid(typtup->typrelid))
	{
		/* Composite type or plain rel, lookup the underlying relation. */
		res = list_concat(res,
						  pgcd_get_rel_collations(typtup->typrelid));

	}
	else if (typtup->typtype == TYPTYPE_RANGE
#if PG_VERSION_NUM >= 140000
			 || typtup->typtype == TYPTYPE_MULTIRANGE
#endif
			 )
	{
		bool ismultirange = (typtup->typtype != TYPTYPE_RANGE);
		Oid conid;

#if PG_VERSION_NUM >= 120000
		conid = typtup->oid;
#else
		conid = HeapTupleGetOid(tp);
#endif

		res = list_concat(res,
						  pgcd_get_range_type_collations(conid,
														 ismultirange));
	}

	/*
	 * Scan pg_depend to find any constraint for that type.
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);

		if (pg_depend->classid != ConstraintRelationId)
			continue;

		res = list_concat(res,
						  pgcd_get_constraint_collations(pg_depend->objid));
	}

	systable_endscan(depScan);

	table_close(depRel, NoLock);

	ReleaseSysCache(tp);
	return res;
}

/*
 * Get full list of collation dependencies for the given constraint.
 *
 * This takes care of removing any duplicated collation.
 */
static List *
pgcd_constraint_deps(Oid constraint_oid)
{
	List	   *res;

	res = pgcd_get_constraint_collations(constraint_oid);
#if PG_VERSION_NUM < 130000
	res =
#endif
	list_sort(res, list_oid_cmp);
	list_deduplicate_oid(res);

	return res;
}

/*
 * Get full list of collation dependencies for the given index.
 *
 * This takes care of looking into index expressions and predicates and
 * removing any duplicated collation.
 */
static List *
pgcd_index_deps(Oid index_oid)
{
	List	   *res = NIL;
	LockRelId	indexrelid = {index_oid, MyDatabaseId};
	LockRelId	indrelid = {InvalidOid, MyDatabaseId};
	Datum		datum;
	bool		indexprs_isnull;
	List	   *indexprs;
	ListCell   *indexpr_item;
	HeapTuple	tup;
	Form_pg_index rd_index;

	LockRelationOid(indexrelid.relId, AccessShareLock);

	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not open index %u", index_oid);

	rd_index = (Form_pg_index) GETSTRUCT(tup);

	indrelid.relId = rd_index->indrelid;
	LockRelationOid(indrelid.relId, AccessShareLock);

	datum = SysCacheGetAttr(INDEXRELID, tup, Anum_pg_index_indexprs,
							&indexprs_isnull);

	if (!indexprs_isnull)
	{
		char	   *expr;

		expr = TextDatumGetCString(datum);
		indexprs = (List *) stringToNode(expr);
	}
	else
		indexprs = NIL;

	indexpr_item = list_head(indexprs);
	for (int i = 0; i < rd_index->indnkeyatts; i++)
	{
		int indkey = rd_index->indkey.values[i];

		if (AttributeNumberIsValid(indkey))
		{
			Oid typid = get_atttype(rd_index->indrelid, indkey);
			Datum	datum;
			bool	isnull, foundcoll = false;

			/* Get the explicit collation if any */
			datum = SysCacheGetAttr(INDEXRELID, tup,
									Anum_pg_index_indcollation, &isnull);

			if (!isnull)
			{
				oidvector *indcollation = (oidvector *) DatumGetPointer(datum);

				if (OidIsValid(indcollation->values[i]))
				{
					foundcoll = true;
					res = lappend_oid(res, indcollation->values[i]);
				}
			}

			/*
			 * Extract any collation(s) from the underlying type only if there
			 * wasn't explicit collation, as otherwise the index wouldn't
			 * depend on it.
			 */
			if (!foundcoll)
				res = list_concat(res, pgcd_get_type_collations(typid));
		}
		else
		{
			Node	   *indexkey;

			Assert(!indexprs_isnull);

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");

			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(
#if PG_VERSION_NUM >= 130000
								 indexprs,
#endif
								 indexpr_item);

			res = list_concat(res, pgcd_get_query_expression_collations(indexkey));
		}
	}

	datum = SysCacheGetAttr(INDEXRELID, tup,
							Anum_pg_index_indpred, &indexprs_isnull);
	if (!indexprs_isnull)
	{
		Node	   *indpred;
		char	   *expr;

		expr = TextDatumGetCString(datum);
		indpred = (Node *) stringToNode(expr);

		res = list_concat(res, pgcd_get_query_expression_collations(indpred));
	}

#if PG_VERSION_NUM < 130000
	res =
#endif
	list_sort(res, list_oid_cmp);
	list_deduplicate_oid(res);

	ReleaseSysCache(tup);

	return res;
}

/*
 * Get full list of collation dependencies for the given materialized view.
 *
 * This takes care of removing any duplicated collation.
 */
static List *
pgcd_matview_deps(Oid matview_oid)
{
	List	   *res = NIL;
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;

	matviewRel = table_open(matview_oid, AccessShareLock);

	/* Make sure it is a materialized view. */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 */
	dataQuery = linitial_node(Query, actions);

	res = list_concat(res, pgcd_get_query_expression_collations((Node *) dataQuery));

	table_close(matviewRel, NoLock);

#if PG_VERSION_NUM < 130000
	res =
#endif
	list_sort(res, list_oid_cmp);
	list_deduplicate_oid(res);

	return res;
}

/*
 * SRF returning all found collation dependencies for the given dependency.
 */
Datum
pg_collation_constraint_dependencies(PG_FUNCTION_ARGS)
{
	Oid				constraint_oid = PG_GETARG_OID(0);
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell	   *lc;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	foreach(lc, pgcd_constraint_deps(constraint_oid))
	{
		Datum			values[PG_COLL_DEP_COLS];
		bool			nulls[PG_COLL_DEP_COLS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = ObjectIdGetDatum(lfirst_oid(lc));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}

/*
 * SRF returning all found collation dependencies for the given index.
 */
Datum
pg_collation_index_dependencies(PG_FUNCTION_ARGS)
{
	Oid				index_oid = PG_GETARG_OID(0);
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell	   *lc;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	foreach(lc, pgcd_index_deps(index_oid))
	{
		Datum			values[PG_COLL_DEP_COLS];
		bool			nulls[PG_COLL_DEP_COLS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = ObjectIdGetDatum(lfirst_oid(lc));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}

/*
 * SRF returning all found collation dependencies for the given materialized
 * view.
 */
Datum
pg_collation_matview_dependencies(PG_FUNCTION_ARGS)
{
	Oid				matview_oid = PG_GETARG_OID(0);
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell	   *lc;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	foreach(lc, pgcd_matview_deps(matview_oid))
	{
		Datum			values[PG_COLL_DEP_COLS];
		bool			nulls[PG_COLL_DEP_COLS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = ObjectIdGetDatum(lfirst_oid(lc));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}
