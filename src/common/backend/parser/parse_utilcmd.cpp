/* -------------------------------------------------------------------------
 *
 * parse_utilcmd.cpp
 *	  Perform parse analysis work for various utility commands
 *
 * Formerly we did this work during parse_analyze() in analyze.c.  However
 * that is fairly unsafe in the presence of querytree caching, since any
 * database state that we depend on in making the transformations might be
 * obsolete by the time the utility command is executed; and utility commands
 * have no infrastructure for holding locks or rechecking plan validity.
 * Hence these functions are now called at the start of execution of their
 * respective utility commands.
 *
 * NOTE: in general we must avoid scribbling on the passed-in raw parse
 * tree, since it might be in a plan cache.  The simplest solution is
 * a quick copyObject() call before manipulating the query tree.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *	src/common/backend/parser/parse_utilcmd.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/reloptions.h"
#include "access/gtm.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_partition_fn.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parse_oper.h"
#include "parser/parse_coerce.h"
#ifdef PGXC
#include "optimizer/pgxcship.h"
#include "pgstat.h"
#include "pgxc/groupmgr.h"
#include "pgxc/locator.h"
#include "pgxc/pgxc.h"
#include "optimizer/pgxcplan.h"
#include "optimizer/nodegroups.h"
#include "pgxc/execRemote.h"
#include "pgxc/redistrib.h"
#include "executor/nodeModifyTable.h"
#endif
#include "parser/parser.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/extended_statistics.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/partitionkey.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/numeric.h"
#include "utils/numeric_gs.h"
#include "mb/pg_wchar.h"
#include "gaussdb_version.h"

/* State shared by transformCreateStmt and its subroutines */
typedef struct {
    ParseState* pstate;             /* overall parser state */
    const char* stmtType;           /* "CREATE [FOREIGN] TABLE" or "ALTER TABLE" */
    RangeVar* relation;             /* relation to create */
    Relation rel;                   /* opened/locked rel, if ALTER */
    List* inhRelations;             /* relations to inherit from */
    bool isalter;                   /* true if altering existing table */
    bool ispartitioned;             /* true if it is for a partitioned table */
    bool hasoids;                   /* does relation have an OID column? */
    bool canInfomationalConstraint; /* If the value id true, it means that we can build informational constraint. */
    List* columns;                  /* ColumnDef items */
    List* ckconstraints;            /* CHECK constraints */
    List* clusterConstraints;       /* PARTIAL CLUSTER KEY constraints */
    List* fkconstraints;            /* FOREIGN KEY constraints */
    List* ixconstraints;            /* index-creating constraints */
    List* inh_indexes;              /* cloned indexes from INCLUDING INDEXES */
    List* blist;                    /* "before list" of things to do before creating the table */
    List* alist;                    /* "after list" of things to do after creating the table */
    PartitionState* csc_partTableState;
    List* reloptions;
    List* partitionKey; /* partitionkey for partiitoned table */
    IndexStmt* pkey;    /* PRIMARY KEY index, if any */
#ifdef PGXC
    List* fallback_dist_col;    /* suggested column to distribute on */
    DistributeBy* distributeby; /* original distribute by column of CREATE TABLE */
    PGXCSubCluster* subcluster; /* original subcluster option of CREATE TABLE */
#endif
    Node* node; /* @hdfs record a CreateStmt or AlterTableStmt object. */
    char* internalData;

    List* uuids;     /* used for create sequence */
    bool isResizing; /* true if the table is resizing */
    Oid  bucketOid;     /* bucket oid of the resizing table */
    List *relnodelist;  /* filenode of the resizing table */
    List *toastnodelist; /* toast node of the resizing table */
} CreateStmtContext;

/* State shared by transformCreateSchemaStmt and its subroutines */
typedef struct {
    const char* stmtType; /* "CREATE SCHEMA" or "ALTER SCHEMA" */
    char* schemaname;     /* name of schema */
    char* authid;         /* owner of schema */
    List* sequences;      /* CREATE SEQUENCE items */
    List* tables;         /* CREATE TABLE items */
    List* views;          /* CREATE VIEW items */
    List* indexes;        /* CREATE INDEX items */
    List* triggers;       /* CREATE TRIGGER items */
    List* grants;         /* GRANT items */
} CreateSchemaStmtContext;

#define ALTER_FOREIGN_TABLE "ALTER FOREIGN TABLE"
#define CREATE_FOREIGN_TABLE "CREATE FOREIGN TABLE"
#define ALTER_TABLE "ALTER TABLE"
#define CREATE_TABLE "CREATE TABLE"

/*
 * jduge a relation is valid to execute function transformTableLikeClause
 * if relation is table, view, composite type, or foreign table, then return true;
 * else return false.
 */
#define TRANSFORM_RELATION_LIKE_CLAUSE(rel_relkind)                                                                    \
    (((rel_relkind) != RELKIND_RELATION && (rel_relkind) != RELKIND_VIEW && (rel_relkind) != RELKIND_COMPOSITE_TYPE && \
         (rel_relkind) != RELKIND_FOREIGN_TABLE)                                                                       \
            ? false                                                                                                    \
            : true)

#define RELATION_ISNOT_REGULAR_PARTITIONED(relation)                                                              \
    (((relation)->rd_rel->relkind != RELKIND_RELATION && (relation)->rd_rel->relkind != RELKIND_FOREIGN_TABLE) || \
        RelationIsNonpartitioned((relation)))

static void transformColumnDefinition(CreateStmtContext* cxt, ColumnDef* column, bool preCheck);
static void transformTableConstraint(CreateStmtContext* cxt, Constraint* constraint);
static void transformTableLikeClause(
    CreateStmtContext* cxt, TableLikeClause* table_like_clause, bool preCheck, bool isFirstNode = false);
static void transformTableLikePartitionProperty(Relation relation, HeapTuple partitionTableTuple, List** partKeyColumns,
    List* partitionList, List** partitionDefinitions);
static IntervalPartitionDefState* TransformTableLikeIntervalPartitionDef(HeapTuple partitionTableTuple);
static void transformTableLikePartitionKeys(
    Relation relation, HeapTuple partitionTableTuple, List** partKeyColumns, List** partKeyPosList);
static void transformTableLikePartitionBoundaries(
    Relation relation, List* partKeyPosList, List* partitionList, List** partitionDefinitions);
static void transformOfType(CreateStmtContext* cxt, TypeName* ofTypename);
static IndexStmt* generateClonedIndexStmt(
    CreateStmtContext* cxt, Relation source_idx, const AttrNumber* attmap, int attmap_length, Relation rel);
static List* get_collation(Oid collation, Oid actual_datatype);
static List* get_opclass(Oid opclass, Oid actual_datatype);
static void checkPartitionValue(CreateStmtContext* cxt, CreateStmt* stmt);
static void checkClusterConstraints(CreateStmtContext* cxt);
static void checkPsortIndexCompatible(IndexStmt* stmt);
static void checkCBtreeIndexCompatible(IndexStmt* stmt);
static void checkCGinBtreeIndexCompatible(IndexStmt* stmt);

static void checkReserveColumn(CreateStmtContext* cxt);

static void transformIndexConstraints(CreateStmtContext* cxt);
static void checkConditionForTransformIndex(
    Constraint* constraint, CreateStmtContext* cxt, Oid index_oid, Relation index_rel);
static IndexStmt* transformIndexConstraint(Constraint* constraint, CreateStmtContext* cxt);
static void transformFKConstraints(CreateStmtContext* cxt, bool skipValidation, bool isAddConstraint);
static void transformConstraintAttrs(CreateStmtContext* cxt, List* constraintList);
static void transformColumnType(CreateStmtContext* cxt, ColumnDef* column);
static void setSchemaName(char* context_schema, char** stmt_schema_name);
/*
 * @hdfs
 * The following three functions are used for HDFS foreign talbe constraint.
 */
static void setInternalFlagIndexStmt(List* IndexList);
static void checkInformationalConstraint(Node* node, bool isForeignTbl);
static void checkConstraint(CreateStmtContext* cxt, Node* node);
static void setMemCheckFlagForIdx(List* IndexList);

/* check partition name */
static void check_partition_name_less_than(List* partitionList);
static void check_partition_name_start_end(List* partitionList);

/* for range partition: start/end syntax */
static void precheck_start_end_defstate(List* pos, Form_pg_attribute* attrs, RangePartitionStartEndDefState* defState);
static Datum get_partition_arg_value(Node* node, bool* isnull);
static Datum evaluate_opexpr(
    ParseState* pstate, List* oprname, Node* leftarg, Node* rightarg, Oid* restypid, int location);
static Const* coerce_partition_arg(ParseState* pstate, Node* node, Oid targetType);
static Oid choose_coerce_type(Oid leftid, Oid rightid);
static void get_rel_partition_info(Relation partTableRel, List** pos, Const** upBound);
static void get_src_partition_bound(Relation partTableRel, Oid srcPartOid, Const** lowBound, Const** upBound);
static Oid get_split_partition_oid(Relation partTableRel, SplitPartitionState* splitState);
static List* add_range_partition_def_state(List* xL, List* boundary, char* partName, const char* tblSpaceName);
static List* divide_start_end_every_internal(ParseState* pstate, char* partName, Form_pg_attribute attr,
    Const* startVal, Const* endVal, Node* everyExpr, int* numPart, int maxNum, bool isinterval, bool needCheck);
static List* DividePartitionStartEndInterval(ParseState* pstate, Form_pg_attribute attr, char* partName,
    Const* startVal, Const* endVal, Const* everyVal, Node* everyExpr, int* numPart, int maxNum);
static void TryReuseFilenode(Relation rel, CreateStmtContext *ctx, bool clonepart);
extern Node* makeAConst(Value* v, int location);

/*
 * transformCreateStmt -
 *	  parse analysis for CREATE TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed CreateStmt, but there may be additional actions
 * to be done before and after the actual DefineRelation() call.
 *
 * SQL92 allows constraints to be scattered all over, so thumb through
 * the columns and collect all constraints into one place.
 * If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 * then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
List* transformCreateStmt(CreateStmt* stmt, const char* queryString, const List* uuids, bool preCheck, bool isFirstNode)
{
    ParseState* pstate = NULL;
    CreateStmtContext cxt;
    List* result = NIL;
    List* saveAlist = NIL;
    ListCell* elements = NULL;
    Oid namespaceid;
    Oid existingRelid;

    /*
     * We must not scribble on the passed-in CreateStmt, so copy it.  (This is
     * overkill, but easy.)
     */
    stmt = (CreateStmt*)copyObject(stmt);
    
    if (uuids != NIL) {
        list_free_deep(stmt->uuids);
        stmt->uuids = (List*)copyObject(uuids);
    }
    
    if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP && stmt->relation->schemaname)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("temporary tables cannot specify a schema name")));

    /*
     * Look up the creation namespace.	This also checks permissions on the
     * target namespace, locks it against concurrent drops, checks for a
     * preexisting relation in that namespace with the same name, and updates
     * stmt->relation->relpersistence if the select namespace is temporary.
     */
    namespaceid = RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, &existingRelid);

    /*
     * If the relation already exists and the user specified "IF NOT EXISTS",
     * bail out with a NOTICE.
     */
    if (stmt->if_not_exists && OidIsValid(existingRelid)) {
        ereport(NOTICE,
            (errcode(ERRCODE_DUPLICATE_TABLE),
                errmsg("relation \"%s\" already exists, skipping", stmt->relation->relname)));
        return NIL;
    }

    /*
     * Transform node group name of table in logic cluster.
     * If not TO GROUP clause, add default node group to the CreateStmt;
     * If logic cluster is redistributing, modify node group to target node group
     * except delete delta table.
     */
    if (IS_PGXC_COORDINATOR && in_logic_cluster()) {
        char* groupName = NULL;
        if (stmt->subcluster == NULL && !IsA(stmt, CreateForeignTableStmt)) {
            groupName = PgxcGroupGetCurrentLogicCluster();
            if (groupName != NULL) {
                stmt->subcluster = makeNode(PGXCSubCluster);
                stmt->subcluster->clustertype = SUBCLUSTER_GROUP;
                stmt->subcluster->members = list_make1(makeString(groupName));
            }
        } else if (stmt->subcluster != NULL && stmt->subcluster->clustertype == SUBCLUSTER_GROUP) {
            Assert(stmt->subcluster->members->length == 1);
            groupName = strVal(linitial(stmt->subcluster->members));
            Assert(groupName != NULL);

            if (IsLogicClusterRedistributed(groupName)) {
                /* Specially handle delete delta table. */
                bool isDeleteDelta = false;
                if (!IsA(stmt, CreateForeignTableStmt) && stmt->relation->relpersistence == RELPERSISTENCE_UNLOGGED) {
                    isDeleteDelta = RelationIsDeleteDeltaTable(stmt->relation->relname);
                }

                /* Logic cluster is redistributing, modify node group to target node group */
                if (!isDeleteDelta) {
                    Value* val = (Value*)linitial(stmt->subcluster->members);
                    groupName = PgxcGroupGetStmtExecGroupInRedis();
                    if (groupName != NULL) {
                        pfree_ext(strVal(val));
                        strVal(val) = groupName;
                    }
                }
            }
        }
    }

    /*
     * If the target relation name isn't schema-qualified, make it so.  This
     * prevents some corner cases in which added-on rewritten commands might
     * think they should apply to other relations that have the same name and
     * are earlier in the search path.	But a local temp table is effectively
     * specified to be in pg_temp, so no need for anything extra in that case.
     */
    if (stmt->relation->schemaname == NULL && stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
        stmt->relation->schemaname = get_namespace_name(namespaceid, true);

    /* Set up pstate and CreateStmtContext */
    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = queryString;

    cxt.pstate = pstate;
    if (IsA(stmt, CreateForeignTableStmt))
        cxt.stmtType = CREATE_FOREIGN_TABLE;
    else
        cxt.stmtType = CREATE_TABLE;
    cxt.relation = stmt->relation;
    cxt.rel = NULL;
    cxt.inhRelations = stmt->inhRelations;
    cxt.subcluster = stmt->subcluster;
    cxt.isalter = false;
    cxt.columns = NIL;
    cxt.ckconstraints = NIL;
    cxt.fkconstraints = NIL;
    cxt.ixconstraints = NIL;
    cxt.clusterConstraints = NIL;
    cxt.inh_indexes = NIL;
    cxt.blist = NIL;
    cxt.alist = NIL;
    cxt.pkey = NULL;
    cxt.csc_partTableState = NULL;
    cxt.reloptions = NIL;
    cxt.hasoids = false;

#ifdef PGXC
    cxt.fallback_dist_col = NULL;
    cxt.distributeby = NULL;
#endif
    cxt.node = (Node*)stmt;
    cxt.internalData = stmt->internalData;
    cxt.isResizing = false;
    cxt.bucketOid = InvalidOid;
    cxt.relnodelist = NULL;
    cxt.toastnodelist = NULL;

    /* We have gen uuids, so use it */
    if (stmt->uuids != NIL)
        cxt.uuids = stmt->uuids;

    if (IS_PGXC_COORDINATOR && !IsConnFromCoord() && stmt->internalData != NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Do not support create table with INERNAL DATA clause.")));
    }

    if (IsA(stmt, CreateForeignTableStmt)) {
        CreateForeignTableStmt* fStmt = (CreateForeignTableStmt*)stmt;
        cxt.canInfomationalConstraint = CAN_BUILD_INFORMATIONAL_CONSTRAINT_BY_STMT(fStmt);
    } else {
        cxt.canInfomationalConstraint = false;
    }

    AssertEreport(stmt->ofTypename == NULL || stmt->inhRelations == NULL, MOD_OPT, "");

    if (stmt->ofTypename)
        transformOfType(&cxt, stmt->ofTypename);

    /*
     * Run through each primary element in the table creation clause. Separate
     * column defs from constraints, and do preliminary analysis.
     */
    foreach (elements, stmt->tableElts) {
        TableLikeClause* tblLlikeClause = NULL;
        Node* element = (Node*)lfirst(elements);
        cxt.uuids = stmt->uuids;

        switch (nodeTag(element)) {
            case T_ColumnDef:
                transformColumnDefinition(&cxt, (ColumnDef*)element, !isFirstNode && preCheck);
                break;

            case T_Constraint:
                transformTableConstraint(&cxt, (Constraint*)element);
                break;

            case T_TableLikeClause:
                tblLlikeClause = (TableLikeClause*)element;
#ifndef ENABLE_MULTIPLE_NODES
                if (tblLlikeClause->options & CREATE_TABLE_LIKE_DISTRIBUTION)
                    DISTRIBUTED_FEATURE_NOT_SUPPORTED();
#endif
                if (PointerIsValid(stmt->partTableState) && (tblLlikeClause->options & CREATE_TABLE_LIKE_PARTITION)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("unsupport \"like clause including partition\" for partitioned table"),
                            errdetail("use either \"like clause including partition\" or \"partition by\" clause")));
                }
                if (PointerIsValid(stmt->options) && (tblLlikeClause->options & CREATE_TABLE_LIKE_RELOPTIONS)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("unsupport \"like clause including reloptions\" together with \"with\""),
                            errdetail("use either \"like clause including reloptions\" or \"with\" clause")));
                }
#ifdef PGXC
                if (IS_PGXC_COORDINATOR && (tblLlikeClause->options & CREATE_TABLE_LIKE_DISTRIBUTION)) {
                    if (PointerIsValid(stmt->distributeby)) {
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                                errmsg(
                                    "unsupport \"like clause including distribution\" together with \"distribute by\""),
                                errdetail(
                                    "use either \"like clause including distribution\" or \"distribute by\" clause")));
                    }
                }
#endif
                transformTableLikeClause(&cxt, (TableLikeClause*)element, !isFirstNode && preCheck, isFirstNode);
                if (stmt->relation->relpersistence != RELPERSISTENCE_TEMP &&
                    tblLlikeClause->relation->relpersistence == RELPERSISTENCE_TEMP)
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("do not support create non-local-temp table like local temp table")));
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized node type: %d", (int)nodeTag(element))));
                break;
        }
    }

    // cxt.csc_partTableState is the partitionState generated
    // from like including partition clause
    if (cxt.csc_partTableState != NULL) {
        Assert(stmt->partTableState == NULL);
        stmt->partTableState = cxt.csc_partTableState;
    }
    /* check syntax for CREATE TABLE */
    checkPartitionSynax(stmt);

    /*
     * @hdfs
     * If the table is foreign table, must be gotten the ispartitioned value
     * from part_state struct.
     */
    if (IsA(stmt, CreateForeignTableStmt)) {
        CreateForeignTableStmt* ftblStmt = (CreateForeignTableStmt*)stmt;
        if (NULL != ftblStmt->part_state) {
            cxt.ispartitioned = true;
            cxt.partitionKey = ftblStmt->part_state->partitionKey;
        } else {
            cxt.ispartitioned = false;
        }
    } else {
        cxt.ispartitioned = PointerIsValid(stmt->partTableState);
        if (cxt.ispartitioned) {
            cxt.partitionKey = stmt->partTableState->partitionKey;
        }
    }

    checkPartitionValue(&cxt, stmt);

    /*
     * transform START/END into LESS/THAN:
     * Put this part behind checkPartitionValue(), since we assume start/end/every-paramters
     * have already been transformed from A_Const into Const.
     */
    if (stmt->partTableState && is_start_end_def_list(stmt->partTableState->partitionList)) {
        List* pos = NIL;
        TupleDesc desc;

        /* get partition key position */
        pos = GetPartitionkeyPos(stmt->partTableState->partitionKey, cxt.columns);

        /* get descriptor */
        desc = BuildDescForRelation(cxt.columns, (Node*)makeString(ORIENTATION_ROW));

        /* entry of transform */
        stmt->partTableState->partitionList = transformRangePartStartEndStmt(
            pstate, stmt->partTableState->partitionList, pos, desc->attrs, 0, NULL, NULL, true);
    }

    if (PointerIsValid(stmt->partTableState)) {
// only check partition name duplication on primary coordinator
#ifdef PGXC
        if ((IS_PGXC_COORDINATOR && !IsConnFromCoord()) || IS_SINGLE_NODE) {
#endif
            checkPartitionName(stmt->partTableState->partitionList);
#ifdef PGXC
        }
#endif
    }

    /* like clause-including reloptions: cxt.reloptions is produced by like including reloptions clause */
    /* output to stmt->options */
    if (cxt.reloptions != NIL) {
        stmt->options = list_concat(stmt->options, cxt.reloptions);
    }
    /* like clause-including oids: cxt.hasoids is produced by like including oids clause, output to stmt->options */
    if (cxt.hasoids) {
        stmt->options = lappend(stmt->options, makeDefElem("oids", (Node*)makeInteger(cxt.hasoids)));
    }
    cxt.hasoids = interpretOidsOption(stmt->options);

#ifdef PGXC
    if (cxt.distributeby != NULL) {
        stmt->distributeby = cxt.distributeby;
    } else {
        cxt.distributeby = stmt->distributeby;
    }

    if (stmt->distributeby != NULL) {
        if (stmt->distributeby->disttype == DISTTYPE_ROUNDROBIN) {
            if (IsA(stmt, CreateForeignTableStmt)) {
                if (IsSpecifiedFDW(((CreateForeignTableStmt*)stmt)->servername, DIST_FDW)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("For foreign table ROUNDROBIN distribution type is built-in support.")));
                }
            } else {
                FEATURE_NOT_PUBLIC_ERROR("Unsupport ROUNDROBIN distribute type");
            }
        } else if (stmt->distributeby->disttype == DISTTYPE_MODULO) {
            FEATURE_NOT_PUBLIC_ERROR("Unsupport MODULO distribute type");
        }
    }
#endif
    /*
     * transformIndexConstraints wants cxt.alist to contain only index
     * statements, so transfer anything we already have into saveAlist.
     */
    saveAlist = cxt.alist;
    cxt.alist = NIL;

    AssertEreport(stmt->constraints == NIL, MOD_OPT, "");

    /*
     * Postprocess constraints that give rise to index definitions.
     */
    transformIndexConstraints(&cxt);

    /*
     * @hdfs
     * If the table is HDFS foreign table, set internal_flag to true
     * in order to create informational constraint. The primary key and
     * unique informaiotnal constraints do not build a index, but informational
     * constraint is build in DefineIndex function.
     */
    if (cxt.alist != NIL) {
        if (cxt.canInfomationalConstraint)
            setInternalFlagIndexStmt(cxt.alist);
        else
            setMemCheckFlagForIdx(cxt.alist);
    }

    /*
     * Postprocess foreign-key constraints.
     */
    transformFKConstraints(&cxt, true, false);

    /*
     * Check partial cluster key constraints
     */
    checkClusterConstraints(&cxt);

    /*
     * Check reserve column
     */
    checkReserveColumn(&cxt);

    /*
     * Output results.
     */
    stmt->tableEltsDup = stmt->tableElts;
    stmt->tableElts = cxt.columns;
    stmt->constraints = cxt.ckconstraints;
    stmt->clusterKeys = cxt.clusterConstraints;
    stmt->oldBucket = cxt.bucketOid;
    stmt->oldNode  = cxt.relnodelist;
    stmt->oldToastNode = cxt.toastnodelist;
    if (stmt->internalData == NULL)
        stmt->internalData = cxt.internalData;

    result = lappend(cxt.blist, stmt);
    result = list_concat(result, cxt.alist);
    result = list_concat(result, saveAlist);

#ifdef PGXC
    /*
     * If the user did not specify any distribution clause and there is no
     * inherits clause, try and use PK or unique index
     */
    if ((!IsA(stmt, CreateForeignTableStmt) ||
            IsSpecifiedFDW(((CreateForeignTableStmt*)stmt)->servername, MOT_FDW)) &&
        !stmt->distributeby && !stmt->inhRelations && cxt.fallback_dist_col) {
        stmt->distributeby = (DistributeBy*)palloc0(sizeof(DistributeBy));
        stmt->distributeby->disttype = DISTTYPE_HASH;
        stmt->distributeby->colname = cxt.fallback_dist_col;
    }
#endif

    return result;
}

/*
 * createSeqOwnedByTable -
 *		create a sequence owned by table, need to add record to pg_depend.
 *		used in CREATE TABLE and CREATE TABLE ... LIKE
 */
static void createSeqOwnedByTable(CreateStmtContext* cxt, ColumnDef* column, bool preCheck)
{
    Oid snamespaceid;
    char* snamespace = NULL;
    char* sname = NULL;
    char* qstring = NULL;
    A_Const* snamenode = NULL;
    TypeCast* castnode = NULL;
    FuncCall* funccallnode = NULL;
    CreateSeqStmt* seqstmt = NULL;
    AlterSeqStmt* altseqstmt = NULL;
    List* attnamelist = NIL;
    Constraint* constraint = NULL;

    /*
     * Determine namespace and name to use for the sequence.
     *
     * Although we use ChooseRelationName, it's not guaranteed that the
     * selected sequence name won't conflict; given sufficiently long
     * field names, two different serial columns in the same table could
     * be assigned the same sequence name, and we'd not notice since we
     * aren't creating the sequence quite yet.	In practice this seems
     * quite unlikely to be a problem, especially since few people would
     * need two serial columns in one table.
     */
    if (cxt->rel)
        snamespaceid = RelationGetNamespace(cxt->rel);
    else {
        snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
        RangeVarAdjustRelationPersistence(cxt->relation, snamespaceid);
    }
    snamespace = get_namespace_name(snamespaceid, true);
    sname = ChooseRelationName(cxt->relation->relname, column->colname, "seq", strlen("seq"), snamespaceid);

    if (!preCheck || IS_SINGLE_NODE)
        ereport(NOTICE,
            (errmsg("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
                cxt->stmtType,
                sname,
                cxt->relation->relname,
                column->colname)));

    /*
     * Build a CREATE SEQUENCE command to create the sequence object, and
     * add it to the list of things to be done before this CREATE/ALTER
     * TABLE.
     */
    seqstmt = makeNode(CreateSeqStmt);
    seqstmt->sequence = makeRangeVar(snamespace, sname, -1);
    seqstmt->options = NIL;
#ifdef PGXC
    seqstmt->is_serial = true;
#endif

    /* Assign UUID for create sequence */
    if (!IS_SINGLE_NODE)
        seqstmt->uuid = gen_uuid(cxt->uuids);
    else
        seqstmt->uuid = INVALIDSEQUUID;

    /*
     * If this is ALTER ADD COLUMN, make sure the sequence will be owned
     * by the table's owner.  The current user might be someone else
     * (perhaps a superuser, or someone who's only a member of the owning
     * role), but the SEQUENCE OWNED BY mechanisms will bleat unless table
     * and sequence have exactly the same owning role.
     */
    if (cxt->rel)
        seqstmt->ownerId = cxt->rel->rd_rel->relowner;
    else
        seqstmt->ownerId = InvalidOid;

    /*
     * When under analyzing, we may create temp sequence which has serial column,
     * but we cannot create temp sequence for now. Besides, create temp table (like t)
     * can be successfully created, but it should not happen. So here we set canCreateTempSeq
     * to true to handle this two cases.
     */
    if (u_sess->analyze_cxt.is_under_analyze || u_sess->attr.attr_common.enable_beta_features) {
        seqstmt->canCreateTempSeq = true;
    }

    cxt->blist = lappend(cxt->blist, seqstmt);

    /*
     * Build an ALTER SEQUENCE ... OWNED BY command to mark the sequence
     * as owned by this column, and add it to the list of things to be
     * done after this CREATE/ALTER TABLE.
     */
    altseqstmt = makeNode(AlterSeqStmt);
    altseqstmt->sequence = makeRangeVar(snamespace, sname, -1);
#ifdef PGXC
    altseqstmt->is_serial = true;
#endif
    attnamelist = list_make3(makeString(snamespace), makeString(cxt->relation->relname), makeString(column->colname));
    altseqstmt->options = list_make1(makeDefElem("owned_by", (Node*)attnamelist));

    cxt->alist = lappend(cxt->alist, altseqstmt);

    /*
     * Create appropriate constraints for SERIAL.  We do this in full,
     * rather than shortcutting, so that we will detect any conflicting
     * constraints the user wrote (like a different DEFAULT).
     *
     * Create an expression tree representing the function call
     * nextval('sequencename').  We cannot reduce the raw tree to cooked
     * form until after the sequence is created, but there's no need to do
     * so.
     */
    qstring = quote_qualified_identifier(snamespace, sname);
    snamenode = makeNode(A_Const);
    snamenode->val.type = T_String;
    snamenode->val.val.str = qstring;
    snamenode->location = -1;
    castnode = makeNode(TypeCast);
    castnode->typname = (TypeName*)SystemTypeName("regclass");
    castnode->arg = (Node*)snamenode;
    castnode->location = -1;
    funccallnode = makeNode(FuncCall);
    funccallnode->funcname = SystemFuncName("nextval");
    funccallnode->args = list_make1(castnode);
    funccallnode->agg_order = NIL;
    funccallnode->agg_star = false;
    funccallnode->agg_distinct = false;
    funccallnode->func_variadic = false;
    funccallnode->over = NULL;
    funccallnode->location = -1;

    constraint = makeNode(Constraint);
    constraint->contype = CONSTR_DEFAULT;
    constraint->location = -1;
    constraint->raw_expr = (Node*)funccallnode;
    constraint->cooked_expr = NULL;
    column->constraints = lappend(column->constraints, constraint);
    column->raw_default = constraint->raw_expr;

    constraint = makeNode(Constraint);
    constraint->contype = CONSTR_NOTNULL;
    constraint->location = -1;
    column->constraints = lappend(column->constraints, constraint);
}

/*
 * transformColumnDefinition -
 *		transform a single ColumnDef within CREATE TABLE
 *		Also used in ALTER TABLE ADD COLUMN
 */
static void transformColumnDefinition(CreateStmtContext* cxt, ColumnDef* column, bool preCheck)
{
    bool isSerial = false;
    bool sawNullable = false;
    bool sawDefault = false;
    Constraint* constraint = NULL;
    ListCell* clist = NULL;

    /* Check the constraint type. */
    checkConstraint(cxt, (Node*)column);

    cxt->columns = lappend(cxt->columns, (Node*)column);

    /* Check for SERIAL pseudo-types */
    isSerial = false;
    if (column->typname && list_length(column->typname->names) == 1 && !column->typname->pct_type) {
        char* typname = strVal(linitial(column->typname->names));

        if (strcmp(typname, "smallserial") == 0 || strcmp(typname, "serial2") == 0) {
            isSerial = true;
            column->typname->names = NIL;
            column->typname->typeOid = INT2OID;
        } else if (strcmp(typname, "serial") == 0 || strcmp(typname, "serial4") == 0) {
            isSerial = true;
            column->typname->names = NIL;
            column->typname->typeOid = INT4OID;
        } else if (strcmp(typname, "bigserial") == 0 || strcmp(typname, "serial8") == 0) {
            isSerial = true;
            column->typname->names = NIL;
            column->typname->typeOid = INT8OID;
        }

        if (isSerial) {
            /*
             * We have to reject "serial[]" explicitly, because once we've set
             * typeid, LookupTypeName won't notice arrayBounds.  We don't need any
             * special coding for serial(typmod) though.
             */
            if (column->typname->arrayBounds != NIL)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("array of serial is not implemented"),
                        parser_errposition(cxt->pstate, column->typname->location)));

            if (cxt->relation && cxt->relation->relpersistence == RELPERSISTENCE_TEMP)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("It's not supported to create serial column on temporary table")));

            if (0 == pg_strncasecmp(cxt->stmtType, ALTER_TABLE, strlen(cxt->stmtType)))
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("It's not supported to alter table add serial column")));
        }
    }

    /* Do necessary work on the column type declaration */
    if (column->typname)
        transformColumnType(cxt, column);

    /* Special actions for SERIAL pseudo-types */
    column->is_serial = isSerial;
    if (isSerial) {
        createSeqOwnedByTable(cxt, column, preCheck);
    }

    /* Process column constraints, if any... */
    transformConstraintAttrs(cxt, column->constraints);

    sawNullable = false;
    sawDefault = false;

    foreach (clist, column->constraints) {
        constraint = (Constraint*)lfirst(clist);

        switch (constraint->contype) {
            case CONSTR_NULL:
                if (sawNullable && column->is_not_null)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
                                column->colname,
                                cxt->relation->relname),
                            parser_errposition(cxt->pstate, constraint->location)));
                column->is_not_null = FALSE;
                sawNullable = true;
                break;

            case CONSTR_NOTNULL:
                if (sawNullable && !column->is_not_null)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
                                column->colname,
                                cxt->relation->relname),
                            parser_errposition(cxt->pstate, constraint->location)));
                column->is_not_null = TRUE;
                sawNullable = true;
                break;

            case CONSTR_DEFAULT:
                if (sawDefault)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
                                column->colname,
                                cxt->relation->relname),
                            parser_errposition(cxt->pstate, constraint->location)));
                column->raw_default = constraint->raw_expr;
                AssertEreport(constraint->cooked_expr == NULL, MOD_OPT, "");
                sawDefault = true;
                break;

            case CONSTR_CHECK:
                cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
                break;

            case CONSTR_PRIMARY:
            case CONSTR_UNIQUE:
                if (constraint->keys == NIL)
                    constraint->keys = list_make1(makeString(column->colname));
                cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
                break;

            case CONSTR_EXCLUSION:
                /* grammar does not allow EXCLUDE as a column constraint */
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("column exclusion constraints are not supported")));
                break;

            case CONSTR_FOREIGN:

                /*
                 * Fill in the current attribute's name and throw it into the
                 * list of FK constraints to be processed later.
                 */
                constraint->fk_attrs = list_make1(makeString(column->colname));
                cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
                break;

            case CONSTR_ATTR_DEFERRABLE:
            case CONSTR_ATTR_NOT_DEFERRABLE:
            case CONSTR_ATTR_DEFERRED:
            case CONSTR_ATTR_IMMEDIATE:
                /* transformConstraintAttrs took care of these */
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized constraint type: %d", constraint->contype)));
                break;
        }
    }

    /*
     * Generate ALTER FOREIGN TABLE ALTER COLUMN statement which adds
     * per-column foreign data wrapper options for this column.
     */
    if (column->fdwoptions != NIL) {
        AlterTableStmt* stmt = NULL;
        AlterTableCmd* cmd = NULL;

        cmd = makeNode(AlterTableCmd);
        cmd->subtype = AT_AlterColumnGenericOptions;
        cmd->name = column->colname;
        cmd->def = (Node*)column->fdwoptions;
        cmd->behavior = DROP_RESTRICT;
        cmd->missing_ok = false;

        stmt = makeNode(AlterTableStmt);
        stmt->relation = cxt->relation;
        stmt->cmds = NIL;
        stmt->relkind = OBJECT_FOREIGN_TABLE;
        stmt->cmds = lappend(stmt->cmds, cmd);

        cxt->alist = lappend(cxt->alist, stmt);
    }
}

/*
 * transformTableConstraint
 *		transform a Constraint node within CREATE TABLE or ALTER TABLE
 */
static void transformTableConstraint(CreateStmtContext* cxt, Constraint* constraint)
{
    switch (constraint->contype) {
        case CONSTR_PRIMARY:
        case CONSTR_UNIQUE:
        case CONSTR_EXCLUSION:
            cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
            break;

        case CONSTR_CHECK:
            cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
            break;

        case CONSTR_CLUSTER:
            cxt->clusterConstraints = lappend(cxt->clusterConstraints, constraint);
            break;

        case CONSTR_FOREIGN:
            cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
            break;

        case CONSTR_NULL:
        case CONSTR_NOTNULL:
        case CONSTR_DEFAULT:
        case CONSTR_ATTR_DEFERRABLE:
        case CONSTR_ATTR_NOT_DEFERRABLE:
        case CONSTR_ATTR_DEFERRED:
        case CONSTR_ATTR_IMMEDIATE:
            ereport(ERROR,
                (errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
                    errmsg("invalid context for constraint type %d", constraint->contype)));
            break;

        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized constraint type: %d", constraint->contype)));
            break;
    }

    /* Check the constraint type. */
    checkConstraint(cxt, (Node*)constraint);
}

/*
 * searchSeqidFromExpr
 *
 * search default expression for sequence oid.
 */
Oid searchSeqidFromExpr(Node* cooked_default)
{
    Const* first_arg = NULL;
    FuncExpr* nextvalExpr = NULL;

    if (IsA(cooked_default, FuncExpr)) {
        if (((FuncExpr*)cooked_default)->funcid == NEXTVALFUNCOID) {
            nextvalExpr = (FuncExpr*)cooked_default;
        } else {
            List* args = ((FuncExpr*)cooked_default)->args;
            if (args != NULL) {
                Node* nextval = (Node*)linitial(args);
                if (IsA(nextval, FuncExpr) && ((FuncExpr*)nextval)->funcid == NEXTVALFUNCOID) {
                    nextvalExpr = (FuncExpr*)nextval;
                }
            }
        }
    }
    if (nextvalExpr == NULL)
        return InvalidOid;

    first_arg = (Const*)linitial(nextvalExpr->args);
    Assert(IsA(first_arg, Const));

    return DatumGetObjectId(first_arg->constvalue);
}

/*
 * checkTableLikeSequence
 *
 * Analyze default expression of table column, if default is nextval function,
 * it means the first argument of nextval function is sequence, we need to
 * check whether the sequence exists in current datanode.
 * We check sequence oid only because stringToNode in transformTableLikeFromSerialData
 * has already checked sequence name (see _readFuncExpr in readfuncs.cpp).
 * Suppose create a table like this:  CREATE TABLE t1 (id serial, a int) TO NODE GROUP ng1;
 * a sequence named t1_id_seq will be created and the sequence exists in NodeGroup ng1.
 * If create table like t1 in another NodeGroup ng2, error will be reported because t1_id_seq
 * does not exists some datanodes of NodeGroup ng2.
 */
static void checkTableLikeSequence(Node* cooked_default)
{
    char* seqName = NULL;
    Oid seqId = searchSeqidFromExpr(cooked_default);
    if (!OidIsValid(seqId))
        return;

    seqName = get_rel_name(seqId);
    if (seqName == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("CREATE TABLE LIKE with column sequence "
                       "in different NodeGroup is not supported."),
                errdetail("Recommend to LIKE table with sequence in installation NodeGroup.")));
    }      
    pfree_ext(seqName);
}

/*
 * transformTableLikeFromSerialData
 *
 * Get meta info of a table from serialized data, the serialized data come from CN.
 * The function is used for CREATE TABLE ... LIKE across node group.
 */
static void transformTableLikeFromSerialData(CreateStmtContext* cxt, TableLikeClause* table_like_clause)
{
    ListCell* cell = NULL;
    TableLikeCtx* metaInfo = NULL;
    metaInfo = (TableLikeCtx*)stringToNode(cxt->internalData);

    table_like_clause->options = metaInfo->options;
    cxt->hasoids = metaInfo->hasoids;
    cxt->columns = metaInfo->columns;
    cxt->csc_partTableState = metaInfo->partition;
    cxt->inh_indexes = metaInfo->inh_indexes;
    cxt->clusterConstraints = metaInfo->cluster_keys;
    cxt->ckconstraints = metaInfo->ckconstraints;
    cxt->alist = metaInfo->comments;
    cxt->reloptions = metaInfo->reloptions;

    if (metaInfo->temp_table) {
        table_like_clause->relation->relpersistence = RELPERSISTENCE_TEMP;
        ExecSetTempObjectIncluded();
    }
    /* Special actions for SERIAL pseudo-types */
    foreach (cell, cxt->columns) {
        ColumnDef* column = (ColumnDef*)lfirst(cell);
        if (column->is_serial) {
            createSeqOwnedByTable(cxt, column, false);
        } else if (column->cooked_default != NULL) {
            checkTableLikeSequence(column->cooked_default);
        }
    }
}

/*
 * transformTableLikeClause
 *
 * Change the LIKE <srctable> portion of a CREATE TABLE statement into
 * column definitions which recreate the user defined column portions of
 * <srctable>.
 */
static void transformTableLikeClause(
    CreateStmtContext* cxt, TableLikeClause* table_like_clause, bool preCheck, bool isFirstNode)
{
    AttrNumber parentAttno;
    Relation relation;
    TupleDesc tupleDesc;
    TupleConstr* constr = NULL;
    AttrNumber* attmap = NULL;
    AclResult aclresult;
    char* comment = NULL;
    ParseCallbackState pcbstate;
    TableLikeCtx metaInfo;
    bool multiNodegroup = false;
    errno_t rc;

    setup_parser_errposition_callback(&pcbstate, cxt->pstate, table_like_clause->relation->location);

    /*
     * We may run into a case where LIKE clause happens between two tables with different
     * node groups, we don't check validation in coordinator nodes as in cluster expansion
     * scenarios we first dump/restore table's metadata in new added DNs without sync
     * pgxc_class, then invoke LIKE command. So we have to allow a case where source table's
     * nodegroup fully include target table's
     */
    if (IS_PGXC_DATANODE) {
        RangeVar* relvar = table_like_clause->relation;
        Oid relid = RangeVarGetRelidExtended(relvar, NoLock, true, false, false, true, NULL, NULL);
        if (relid == InvalidOid) {
            if (cxt->internalData != NULL) {
                cancel_parser_errposition_callback(&pcbstate);
                transformTableLikeFromSerialData(cxt, table_like_clause);
                return;
            }

            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("Table %s.%s does not exist in current datanode.", relvar->schemaname, relvar->relname)));
        }
    }

    relation = relation_openrv_extended(table_like_clause->relation, AccessShareLock, false, true);

    if (!TRANSFORM_RELATION_LIKE_CLAUSE(relation->rd_rel->relkind))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is not a table, view, composite type, or foreign table",
                    RelationGetRelationName(relation))));

    cancel_parser_errposition_callback(&pcbstate);

    // If specify 'INCLUDING ALL' for non-partitioned table, just remove the option 'INCLUDING PARTITION'.
    // Right shift 8 bits can handle both 'INCLUDING ALL' and 'INCLUDING ALL EXCLUDING option(s)'.
    // if add a new option, the number '8'(see marco 'MAX_TABLE_LIKE_OPTIONS') should be changed.
    if ((table_like_clause->options >> MAX_TABLE_LIKE_OPTIONS) && !RELATION_IS_PARTITIONED(relation) &&
        !RelationIsValuePartitioned(relation))
        table_like_clause->options = table_like_clause->options & ~CREATE_TABLE_LIKE_PARTITION;

    if (table_like_clause->options & CREATE_TABLE_LIKE_PARTITION) {
        if (RELATION_ISNOT_REGULAR_PARTITIONED(relation)) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("could not specify \"INCLUDING PARTITION\" for non-partitioned-table relation:\"%s\"",
                        RelationGetRelationName(relation))));
        }
        if (cxt->csc_partTableState != NULL) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("could not specify 2 or more \"INCLUDING PARTITION\" clauses, only one is allowed")));
        }
    }

    if (table_like_clause->options & CREATE_TABLE_LIKE_RELOPTIONS) {
        if (cxt->reloptions != NULL) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("could not specify 2 or more \"INCLUDING RELOPTIONS\" clauses, only one is allowed")));
        }
    }

    if (table_like_clause->options & CREATE_TABLE_LIKE_DISTRIBUTION) {
        if (cxt->distributeby != NULL) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("could not specify 2 or more \"INCLUDING DISTRIBUTION\" clauses, only one is allowed")));
        }
    }

    /* Initialize meta_info struct. */
    rc = memset_s(&metaInfo, sizeof(metaInfo), 0, sizeof(metaInfo));
    securec_check_ss(rc, "\0", "\0");

#ifdef PGXC
    /*
     * Check if relation is temporary and assign correct flag.
     * This will override transaction direct commit as no 2PC
     * can be used for transactions involving temporary objects.
     */
    if (IsTempTable(RelationGetRelid(relation))) {
        table_like_clause->relation->relpersistence = RELPERSISTENCE_TEMP;
        ExecSetTempObjectIncluded();
        metaInfo.temp_table = true;
    }

    /*
     * Block the creation of tables using views in their LIKE clause.
     * Views are not created on Datanodes, so this will result in an error
     * In order to fix this problem, it will be necessary to
     * transform the query string of CREATE TABLE into something not using
     * the view definition. Now Postgres-XC only uses the raw string...
     * There is some work done with event triggers in 9.3, so it might
     * be possible to use that code to generate the SQL query to be sent to
     * remote nodes. When this is done, this error will be removed.
     */
    if (relation->rd_rel->relkind == RELKIND_VIEW)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Postgres-XC does not support VIEW in LIKE clauses"),
                errdetail("The feature is not currently supported")));
#endif

    /*
     *  Judge whether create table ... like in multiple node group or not.
     *  If multi_nodegroup is true, table metainfo need to append to meta_info fields.
     *  At the end of transformTableLikeClause, meta_info need to serialize to string for datanodes.
     */
    multiNodegroup = false;
    if (IS_PGXC_COORDINATOR && !IsConnFromCoord()) {
        multiNodegroup = is_multi_nodegroup_createtbllike(cxt->subcluster, relation->rd_id);
    }

    /*
     * Check for privileges
     */
    if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE) {
        aclresult = pg_type_aclcheck(relation->rd_rel->reltype, GetUserId(), ACL_USAGE);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_TYPE, RelationGetRelationName(relation));
    } else {
        /*
         * Just return aclok when current user is superuser, although pg_class_aclcheck
         * also used superuser() function but it forbid the INSERT/DELETE/SELECT/UPDATE
         * for superuser in independent condition. Here CreateLike is no need to forbid.
         */
        if (superuser())
            aclresult = ACLCHECK_OK;
        else
            aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(), ACL_SELECT);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_CLASS, RelationGetRelationName(relation));
    }

    tupleDesc = RelationGetDescr(relation);
    constr = tupleDesc->constr;

    /*
     * Initialize column number map for map_variable_attnos().  We need this
     * since dropped columns in the source table aren't copied, so the new
     * table can have different column numbers.
     */
    attmap = (AttrNumber*)palloc0(sizeof(AttrNumber) * tupleDesc->natts);

    /*
     * Insert the copied attributes into the cxt for the new table definition.
     */
    for (parentAttno = 1; parentAttno <= tupleDesc->natts; parentAttno++) {
        Form_pg_attribute attribute = tupleDesc->attrs[parentAttno - 1];
        char* attributeName = NameStr(attribute->attname);
        ColumnDef* def = NULL;

        /*
         * Ignore dropped columns in the parent.  attmap entry is left zero.
         */
        if (attribute->attisdropped && !u_sess->attr.attr_sql.enable_cluster_resize)
            continue;

        if (u_sess->attr.attr_sql.enable_cluster_resize && attribute->attisdropped) {
            def = makeNode(ColumnDef);
            def->type = T_ColumnDef;
            def->colname = pstrdup(attributeName);
            def->dropped_attr = (Form_pg_attribute)palloc0(sizeof(FormData_pg_attribute));
            copyDroppedAttribute(def->dropped_attr, attribute);
        } else {
            /*
             * Create a new column, which is marked as NOT inherited.
             *
             * For constraints, ONLY the NOT NULL constraint is inherited by the
             * new column definition per SQL99.
             */
            def = makeNode(ColumnDef);
            def->colname = pstrdup(attributeName);
            def->typname = makeTypeNameFromOid(attribute->atttypid, attribute->atttypmod);
            def->kvtype = attribute->attkvtype;
            def->inhcount = 0;
            def->is_local = true;
            def->is_not_null = attribute->attnotnull;
            def->is_from_type = false;
            def->storage = 0;
            /* copy compression mode from source table */
            def->cmprs_mode = attribute->attcmprmode;
            def->raw_default = NULL;
            def->cooked_default = NULL;
            def->collClause = NULL;
            def->collOid = attribute->attcollation;
            def->constraints = NIL;
            def->dropped_attr = NULL;
        }

        /*
         * Add to column list
         */
        cxt->columns = lappend(cxt->columns, def);

        attmap[parentAttno - 1] = list_length(cxt->columns);

        /*
         * Copy default, if present and the default has been requested
         */
        if (attribute->atthasdef) {
            Node* this_default = NULL;
            AttrDefault* attrdef = NULL;
            int i;
            Oid seqId = InvalidOid;

            /* Find default in constraint structure */
            Assert(constr != NULL);
            attrdef = constr->defval;
            for (i = 0; i < constr->num_defval; i++) {
                if (attrdef[i].adnum == parentAttno) {
                    this_default = (Node*)stringToNode_skip_extern_fields(attrdef[i].adbin);
                    break;
                }
            }
            Assert(this_default != NULL);

            /*
             *  Whether default expr is serial type and the sequence is owned by the table.
             */
            seqId = searchSeqidFromExpr(this_default);
            if (OidIsValid(seqId)) {
                List* seqs = getOwnedSequences(relation->rd_id);
                if (seqs != NULL && list_member_oid(seqs, DatumGetObjectId(seqId))) {
                    /* is serial type */
                    def->is_serial = true;
                    /* Special actions for SERIAL pseudo-types */
                    createSeqOwnedByTable(cxt, def, preCheck);
                }
            }

            if (!def->is_serial && (table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS)) {
                /*
                 * If default expr could contain any vars, we'd need to fix 'em,
                 * but it can't; so default is ready to apply to child.
                 */
                def->cooked_default = this_default;
            }
        }

        /* Likewise, copy storage if requested */
        if (table_like_clause->options & CREATE_TABLE_LIKE_STORAGE)
            def->storage = attribute->attstorage;

        if (multiNodegroup) {
            /*need to copy ColumnDef deeply because we will modify it.*/
            ColumnDef* dup = (ColumnDef*)copyObject(def);
            if (def->is_serial) {
                /* Momory will be freed when ExecutorEnd  */
                dup->constraints = NULL;
                dup->raw_default = NULL;
            }
            metaInfo.columns = lappend(metaInfo.columns, dup);
        }

        /* Likewise, copy comment if requested */
        if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
            (comment = GetComment(attribute->attrelid, RelationRelationId, attribute->attnum)) != NULL) {
            CommentStmt* stmt = (CommentStmt*)makeNode(CommentStmt);

            stmt->objtype = OBJECT_COLUMN;
            stmt->objname = list_make3(
                makeString(cxt->relation->schemaname), makeString(cxt->relation->relname), makeString(def->colname));
            stmt->objargs = NIL;
            stmt->comment = comment;

            cxt->alist = lappend(cxt->alist, stmt);

            if (multiNodegroup) {
                /* don't need to copy CommentStmt deeply */
                metaInfo.comments = lappend(metaInfo.comments, stmt);
            }
        }
    }

    /*
     * Copy CHECK constraints if requested, being careful to adjust attribute
     * numbers so they match the child.
     */
    if ((table_like_clause->options & CREATE_TABLE_LIKE_CONSTRAINTS) && tupleDesc->constr) {
        int ccnum;

        /* check expr constraint */
        for (ccnum = 0; ccnum < tupleDesc->constr->num_check; ccnum++) {
            char* ccname = tupleDesc->constr->check[ccnum].ccname;
            char* ccbin = tupleDesc->constr->check[ccnum].ccbin;
            Constraint* n = makeNode(Constraint);
            Node* ccbin_node = NULL;
            bool found_whole_row = false;

            ccbin_node =
                map_variable_attnos((Node*)stringToNode(ccbin), 1, 0, attmap, tupleDesc->natts, &found_whole_row);

            /*
             * We reject whole-row variables because the whole point of LIKE
             * is that the new table's rowtype might later diverge from the
             * parent's.  So, while translation might be possible right now,
             * it wouldn't be possible to guarantee it would work in future.
             */
            if (found_whole_row)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot convert whole-row table reference"),
                        errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
                            ccname,
                            RelationGetRelationName(relation))));

            n->contype = CONSTR_CHECK;
            n->location = -1;
            n->conname = pstrdup(ccname);
            n->raw_expr = NULL;
            n->cooked_expr = nodeToString(ccbin_node);
            cxt->ckconstraints = lappend(cxt->ckconstraints, n);
            if (multiNodegroup) {
                /* don't need to copy Constraint deeply */
                metaInfo.ckconstraints = lappend(metaInfo.ckconstraints, n);
            }

            /* Copy comment on constraint */
            if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
                (comment = GetComment(get_relation_constraint_oid(RelationGetRelid(relation), n->conname, false),
                    ConstraintRelationId,
                    0)) != NULL) {
                CommentStmt* stmt = makeNode(CommentStmt);

                stmt->objtype = OBJECT_CONSTRAINT;
                stmt->objname = list_make3(
                    makeString(cxt->relation->schemaname), makeString(cxt->relation->relname), makeString(n->conname));
                stmt->objargs = NIL;
                stmt->comment = comment;

                cxt->alist = lappend(cxt->alist, stmt);
                if (multiNodegroup) {
                    /* don't need to copy CommentStmt deeply */
                    metaInfo.comments = lappend(metaInfo.comments, stmt);
                }
            }
        }

        /* paritial cluster key constraint like */
        if (tupleDesc->constr->clusterKeyNum > 0) {
            int pckNum;
            Constraint* n = makeNode(Constraint);

            for (pckNum = 0; pckNum < tupleDesc->constr->clusterKeyNum; pckNum++) {
                AttrNumber attrNum = tupleDesc->constr->clusterKeys[pckNum];
                Form_pg_attribute attribute = tupleDesc->attrs[attrNum - 1];
                char* attrName = NameStr(attribute->attname);

                n->contype = CONSTR_CLUSTER;
                n->location = -1;
                n->keys = lappend(n->keys, makeString(pstrdup(attrName)));
            }

            cxt->clusterConstraints = lappend(cxt->clusterConstraints, n);

            if (multiNodegroup) {
                /* don't need to copy Constraint deeply */
                metaInfo.cluster_keys = lappend(metaInfo.cluster_keys, n);
            }

            /* needn't copy comment on partial cluster key constraint
             * the constraint name was not like the source, refer to primary/unique constraint
             */
        }
    }

    /*
     * Likewise, copy partition definitions if requested. Then, copy index,
     * because partitioning might have effect on how to create indexes
     */
    if (table_like_clause->options & CREATE_TABLE_LIKE_PARTITION) {
        PartitionState* n = NULL;
        HeapTuple partitionTableTuple = NULL;
        Form_pg_partition partitionForm = NULL;
        List* partitionList = NIL;

        // read out partitioned table tuple, and partition tuple list
        partitionTableTuple =
            searchPgPartitionByParentIdCopy(PART_OBJ_TYPE_PARTED_TABLE, ObjectIdGetDatum(relation->rd_id));
        partitionList = searchPgPartitionByParentId(PART_OBJ_TYPE_TABLE_PARTITION, ObjectIdGetDatum(relation->rd_id));

        if (partitionTableTuple != NULL) {
            partitionForm = (Form_pg_partition)GETSTRUCT(partitionTableTuple);

            bool valuePartitionRel = (partitionForm->partstrategy == PART_STRATEGY_VALUE);

            /*
             * We only have to create PartitionState for a range partition table
             * with known partitions or a value partition table(HDFS).
             */
            if ((partitionList != NIL) || valuePartitionRel) {
                {
                    List* partKeyColumns = NIL;
                    List* partitionDefinitions = NIL;

                    transformTableLikePartitionProperty(
                        relation, partitionTableTuple, &partKeyColumns, partitionList, &partitionDefinitions);

                    // set PartitionState fields, 5 following
                    // (1)partition key
                    // (2)partition definition list
                    // (3)interval definition
                    // (4)partitionStrategy
                    // (5)rowMovement
                    n = makeNode(PartitionState);
                    n->partitionKey = partKeyColumns;
                    n->partitionList = partitionDefinitions;
                    n->partitionStrategy = partitionForm->partstrategy;
                    if (partitionForm->partstrategy == PART_STRATEGY_INTERVAL) {
                        n->intervalPartDef = TransformTableLikeIntervalPartitionDef(partitionTableTuple);
                    } else {
                        n->intervalPartDef = NULL;
                    }
                    n->rowMovement = relation->rd_rel->relrowmovement ? ROWMOVEMENT_ENABLE : ROWMOVEMENT_DISABLE;

                    // store the produced partition state in CreateStmtContext
                    cxt->csc_partTableState = n;

                    freePartList(partitionList);
                }
            }

            heap_freetuple_ext(partitionTableTuple);
        }
    }

    /*
     * Likewise, copy indexes if requested
     */
    if ((table_like_clause->options & CREATE_TABLE_LIKE_INDEXES) && relation->rd_rel->relhasindex) {
        List* parentIndexes = NIL;
        ListCell* l = NULL;

        parentIndexes = RelationGetIndexList(relation);

        foreach (l, parentIndexes) {
            Oid parentIndexOid = lfirst_oid(l);
            Relation parentIndex;
            IndexStmt* indexStmt = NULL;

            parentIndex = index_open(parentIndexOid, AccessShareLock);

            /* Build CREATE INDEX statement to recreate the parent_index */
            indexStmt = generateClonedIndexStmt(cxt, parentIndex, attmap, tupleDesc->natts, relation);

            /* Copy comment on index, if requested */
            if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) {
                comment = GetComment(parentIndexOid, RelationRelationId, 0);

                /*
                 * We make use of IndexStmt's idxcomment option, so as not to
                 * need to know now what name the index will have.
                 */
                indexStmt->idxcomment = comment;
            }

            /* Save it in the inh_indexes list for the time being */
            cxt->inh_indexes = lappend(cxt->inh_indexes, indexStmt);

            index_close(parentIndex, AccessShareLock);
        }
    }

    /*
     * Likewise, copy reloptions if requested
     */
    if (table_like_clause->options & CREATE_TABLE_LIKE_RELOPTIONS) {
        Datum reloptions = (Datum)0;
        bool isNull = false;
        HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relation->rd_id));
        if (!HeapTupleIsValid(tuple))
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed on source like relation %u for reloptions", relation->rd_id)));
        reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isNull);
        if (isNull)
            reloptions = (Datum)0;
        cxt->reloptions = untransformRelOptions(reloptions);

        /* remove on_commit_delete_rows option */
        if (cxt->relation->relpersistence != RELPERSISTENCE_TEMP &&
            cxt->relation->relpersistence != RELPERSISTENCE_GLOBAL_TEMP) {
            cxt->reloptions = RemoveRelOption(cxt->reloptions, "on_commit_delete_rows", NULL);
        }

        /* remove redis options first. */
        RemoveRedisRelOptionsFromList(&(cxt->reloptions));

        metaInfo.reloptions = cxt->reloptions;

        ReleaseSysCache(tuple);
    }
#ifdef PGXC
    /*
     * Likewise, copy distribution if requested
     */
    if (table_like_clause->options & CREATE_TABLE_LIKE_DISTRIBUTION) {
        cxt->distributeby = (IS_PGXC_COORDINATOR) ? 
                            getTableDistribution(relation->rd_id) : 
                            getTableHBucketDistribution(relation);
    }
#endif

    /*
     * Likewise, copy oids if requested
     */
    if (table_like_clause->options & CREATE_TABLE_LIKE_OIDS) {
        cxt->hasoids = tupleDesc->tdhasoid;
    }

    if (multiNodegroup) {
        metaInfo.type = T_TableLikeCtx;
        metaInfo.options = table_like_clause->options;
        metaInfo.hasoids = cxt->hasoids;

        /* partition info and inh_indexes is only from transformTableLikeClause,
         *  so we don't need to copy them.
         */
        metaInfo.partition = cxt->csc_partTableState;
        metaInfo.inh_indexes = cxt->inh_indexes;

        cxt->internalData = nodeToString(&metaInfo);

        /* Momory of meta_info will be freed when ExecutorEnd  */
    }

    if (u_sess->attr.attr_sql.enable_cluster_resize) {
         cxt->isResizing = RelationInClusterResizing(relation);

         if (RELATION_OWN_BUCKET(relation) && RelationInClusterResizing(relation)) {
             cxt->bucketOid = relation->rd_bucketoid;
             TryReuseFilenode(relation, cxt, table_like_clause->options & CREATE_TABLE_LIKE_PARTITION);      
         }
    }

    /*
     * Close the parent rel, but keep our AccessShareLock on it until xact
     * commit.	That will prevent someone else from deleting or ALTERing the
     * parent before the child is committed.
     */
    if (IS_PGXC_COORDINATOR && !IsConnFromCoord() && !isFirstNode)
        heap_close(relation, AccessShareLock);
    else
        heap_close(relation, NoLock);
}

// this function is used to output 2 list,
// one for partitionkey, a list of column ref,
// another for partiton boundary, a list of
static void transformTableLikePartitionProperty(Relation relation, HeapTuple partitionTableTuple, List** partKeyColumns,
    List* partitionList, List** partitionDefinitions)
{
    List* partKeyPosList = NIL;
    transformTableLikePartitionKeys(relation, partitionTableTuple, partKeyColumns, &partKeyPosList);
    transformTableLikePartitionBoundaries(relation, partKeyPosList, partitionList, partitionDefinitions);
}

static IntervalPartitionDefState* TransformTableLikeIntervalPartitionDef(HeapTuple partitionTableTuple)
{
    IntervalPartitionDefState* intervalPartDef = makeNode(IntervalPartitionDefState);
    Relation partitionRel = relation_open(PartitionRelationId, RowExclusiveLock);
    char* intervalStr = ReadIntervalStr(partitionTableTuple, RelationGetDescr(partitionRel));
    Assert(intervalStr != NULL);
    intervalPartDef->partInterval = makeAConst(makeString(intervalStr), -1);
    oidvector* tablespaceIdVec = ReadIntervalTablespace(partitionTableTuple, RelationGetDescr(partitionRel));
    intervalPartDef->intervalTablespaces = NULL;
    if (tablespaceIdVec != NULL && tablespaceIdVec->dim1 > 0) {
        for (int i = 0; i < tablespaceIdVec->dim1; ++i) {
            char* tablespaceName = get_tablespace_name(tablespaceIdVec->values[i]);
            if (tablespaceName == NULL) {
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("tablespace with OID %u does not exist", tablespaceIdVec->values[i])));
            }
            intervalPartDef->intervalTablespaces =
                lappend(intervalPartDef->intervalTablespaces, makeString(tablespaceName));
        }
    }

    relation_close(partitionRel, RowExclusiveLock);
    return intervalPartDef;
}

static void transformTableLikePartitionKeys(
    Relation relation, HeapTuple partitionTableTuple, List** partKeyColumns, List** partKeyPosList)
{
    ColumnRef* c = NULL;
    Relation partitionRel = NULL;
    TupleDesc relationTupleDesc = NULL;
    Form_pg_attribute* relationAtts = NULL;
    int relationAttNumber = 0;
    Datum partkeyRaw = (Datum)0;
    ArrayType* partkeyColumns = NULL;
    int16* attnums = NULL;
    bool isNull = false;
    int nKeyColumn, i;

    /* open pg_partition catalog */
    partitionRel = relation_open(PartitionRelationId, RowExclusiveLock);

    /* Get the raw data which contain patition key's columns */
    partkeyRaw = heap_getattr(partitionTableTuple, Anum_pg_partition_partkey, RelationGetDescr(partitionRel), &isNull);
    /* if the raw value of partition key is null, then report error */
    if (isNull) {
        ereport(ERROR,
            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                errmsg("null partition key value for relation \"%s\"", RelationGetRelationName(relation))));
    }
    /*  convert Datum to ArrayType */
    partkeyColumns = DatumGetArrayTypeP(partkeyRaw);
    /* Get number of partition key columns from int2verctor */
    nKeyColumn = ARR_DIMS(partkeyColumns)[0];
    /* CHECK: the ArrayType of partition key is valid */
    if (ARR_NDIM(partkeyColumns) != 1 || nKeyColumn < 0 || ARR_HASNULL(partkeyColumns) ||
        ARR_ELEMTYPE(partkeyColumns) != INT2OID) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg("partition key column's number of relation \"%s\" is not a 1-D smallint array",
                    RelationGetRelationName(relation))));
    }

    AssertEreport(nKeyColumn <= RANGE_PARTKEYMAXNUM, MOD_OPT, "");
    /* Get int2 array of partition key column numbers */
    attnums = (int16*)ARR_DATA_PTR(partkeyColumns);
    /*
     * get the partition key number,
     * make ColumnRef node from name of partition key
     */
    relationTupleDesc = relation->rd_att;
    relationAttNumber = relationTupleDesc->natts;
    relationAtts = relationTupleDesc->attrs;

    for (i = 0; i < nKeyColumn; i++) {
        int attnum = (int)(attnums[i]);
        if (attnum >= 1 && attnum <= relationAttNumber) {
            c = makeNode(ColumnRef);
            c->fields = list_make1(makeString(pstrdup(NameStr(relationAtts[attnum - 1]->attname))));
            *partKeyColumns = lappend(*partKeyColumns, c);
            *partKeyPosList = lappend_int(*partKeyPosList, attnum - 1);
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                    errmsg("partition key column's number of %s not in the range of all its columns",
                        RelationGetRelationName(relation))));
        }
    }

    /* close pg_partition catalog */
    relation_close(partitionRel, RowExclusiveLock);
}

static void transformTableLikePartitionBoundaries(
    Relation relation, List* partKeyPosList, List* partitionList, List** partitionDefinitions)
{
    ListCell* partitionCell = NULL;
    List* orderedPartitionList = NIL;

    if (relation->partMap == NULL)
        return;

    // form into a new ordered list
    if (relation->partMap->type == PART_TYPE_RANGE || relation->partMap->type == PART_TYPE_INTERVAL) {
        RangePartitionMap* rangePartMap = (RangePartitionMap*)relation->partMap;
        int i;
        int rangePartitions = rangePartMap->rangeElementsNum;

        for (i = 0; i < rangePartitions; i++) {
            Oid partitionOid = rangePartMap->rangeElements[i].partitionOid;

            foreach (partitionCell, partitionList) {
                HeapTuple partitionTuple = (HeapTuple)lfirst(partitionCell);
                if (partitionOid == HeapTupleGetOid(partitionTuple)) {
                    orderedPartitionList = lappend(orderedPartitionList, partitionTuple);
                    break;
                }
            }
        }
    } else if (relation->partMap->type == PART_TYPE_LIST) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("\" including partition \" for list partitioned relation: \"%s\" not implemented yet",
                    RelationGetRelationName(relation))));
    }
    /* open pg_partition catalog */
    Relation partitionRel = relation_open(PartitionRelationId, AccessShareLock);

    foreach (partitionCell, orderedPartitionList) {
        HeapTuple partitionTuple = (HeapTuple)lfirst(partitionCell);
        Form_pg_partition partitionForm = (Form_pg_partition)GETSTRUCT(partitionTuple);
        /* no need to copy interval partition */
        if (partitionForm->partstrategy == PART_STRATEGY_INTERVAL) {
            continue;
        }

        bool attIsNull = false;
        Datum tableSpace = (Datum)0;
        Datum boundaries = (Datum)0;
        RangePartitionDefState* partitionNode = NULL;

        // in mppdb, we only support range partition by now(2014.05)
        // so here produce RangePartitionDefState node
        partitionNode = makeNode(RangePartitionDefState);

        // set RangePartitionDefState: 1.partition name
        partitionNode->partitionName = pstrdup(NameStr(partitionForm->relname));

        // set RangePartitionDefState: 2.partition tablespace
        tableSpace =
            heap_getattr(partitionTuple, Anum_pg_partition_reltablespace, RelationGetDescr(partitionRel), &attIsNull);
        if (attIsNull)
            partitionNode->tablespacename = NULL;
        else
            partitionNode->tablespacename = get_tablespace_name(DatumGetObjectId(tableSpace));

        // set RangePartitionDefState: 3.boundaries
        boundaries =
            heap_getattr(partitionTuple, Anum_pg_partition_boundaries, RelationGetDescr(partitionRel), &attIsNull);
        if (attIsNull) {
            partitionNode->boundary = NIL;
        } else {
            /* unstransform string items to Value list */
            List* boundaryValueList = NIL;
            List* resultBoundaryList = NIL;
            ListCell* boundaryCell = NULL;
            ListCell* partKeyCell = NULL;
            Value* boundaryValue = NULL;
            Datum boundaryDatum = (Datum)0;
            Node* boundaryNode = NULL;
            Form_pg_attribute* relationAtts = NULL;
            Form_pg_attribute att = NULL;
            int partKeyPos = 0;
            int16 typlen = 0;
            bool typbyval = false;
            char typalign;
            char typdelim;
            Oid typioparam = InvalidOid;
            Oid func = InvalidOid;
            Oid typid = InvalidOid;
            Oid typelem = InvalidOid;
            Oid typcollation = InvalidOid;
            int32 typmod = -1;

            boundaryValueList = untransformPartitionBoundary(boundaries);

            // transform Value(every is string Value) node into Const node.
            // (1)the first step is transform text into datum,
            // (2)then datum into corresponding int, float or string format
            // (3)the last step is make A_Const node using int, float or string
            relationAtts = relation->rd_att->attrs;
            forboth(boundaryCell, boundaryValueList, partKeyCell, partKeyPosList)
            {
                boundaryValue = (Value*)lfirst(boundaryCell);
                partKeyPos = (int)lfirst_int(partKeyCell);
                att = relationAtts[partKeyPos];

                /* get the oid/mod/collation/ of partition key */
                typid = att->atttypid;
                typmod = att->atttypmod;
                typcollation = att->attcollation;
                /* deal with null */
                if (!PointerIsValid(boundaryValue->val.str)) {
                    boundaryNode = (Node*)makeMaxConst(typid, typmod, typcollation);
                } else {
                    /* get the typein function's oid of current type */
                    get_type_io_data(typid, IOFunc_input, &typlen, &typbyval, &typalign, &typdelim, &typioparam, &func);
                    typelem = get_element_type(typid);

                    /* now call the typein function with collation,string, element_type, typemod
                     * as it's parameters.
                     */
                    boundaryDatum = OidFunctionCall3Coll(func,
                        typcollation,
                        CStringGetDatum(boundaryValue->val.str),
                        ObjectIdGetDatum(typelem),
                        Int32GetDatum(typmod));

                    // produce const node
                    boundaryNode =
                        (Node*)makeConst(typid, typmod, typcollation, typlen, boundaryDatum, false, typbyval);
                }
                resultBoundaryList = lappend(resultBoundaryList, boundaryNode);
            }
            partitionNode->boundary = resultBoundaryList;
        }

        // now, append the result RangePartitionDefState node to output list
        *partitionDefinitions = lappend(*partitionDefinitions, partitionNode);
    }
    /* close pg_partition catalog */
    relation_close(partitionRel, AccessShareLock);

    // free the new ordered list
    list_free_ext(orderedPartitionList);
}

static void transformOfType(CreateStmtContext* cxt, TypeName* ofTypename)
{
    HeapTuple tuple;
    TupleDesc tupdesc;
    int i;
    Oid ofTypeId;

    AssertArg(ofTypename);

    tuple = typenameType(NULL, ofTypename, NULL);
    check_of_type(tuple);
    ofTypeId = HeapTupleGetOid(tuple);
    ofTypename->typeOid = ofTypeId; /* cached for later */

    tupdesc = lookup_rowtype_tupdesc(ofTypeId, -1);
    for (i = 0; i < tupdesc->natts; i++) {
        Form_pg_attribute attr = tupdesc->attrs[i];
        ColumnDef* n = NULL;

        if (attr->attisdropped)
            continue;

        n = makeNode(ColumnDef);
        n->colname = pstrdup(NameStr(attr->attname));
        n->typname = makeTypeNameFromOid(attr->atttypid, attr->atttypmod);
        n->kvtype = ATT_KV_UNDEFINED;
        n->inhcount = 0;
        n->is_local = true;
        n->is_not_null = false;
        n->is_from_type = true;
        n->storage = 0;
        /* CREATE TYPE CANNOT provied compression feature, so the default is set. */
        n->cmprs_mode = ATT_CMPR_UNDEFINED;
        n->raw_default = NULL;
        n->cooked_default = NULL;
        n->collClause = NULL;
        n->collOid = attr->attcollation;
        n->constraints = NIL;
        cxt->columns = lappend(cxt->columns, n);
    }
    DecrTupleDescRefCount(tupdesc);

    ReleaseSysCache(tuple);
}

/*
 * Generate an IndexStmt node using information from an already existing index
 * "source_idx".  Attribute numbers should be adjusted according to attmap.
 */
static IndexStmt* generateClonedIndexStmt(
    CreateStmtContext* cxt, Relation source_idx, const AttrNumber* attmap, int attmap_length, Relation rel)
{
    Oid sourceRelid = RelationGetRelid(source_idx);
    Form_pg_attribute* attrs = RelationGetDescr(source_idx)->attrs;
    HeapTuple htIdxrel;
    HeapTuple htIdx;
    Form_pg_class idxrelrec;
    Form_pg_index idxrec;
    Form_pg_am amrec;
    oidvector* indcollation = NULL;
    oidvector* indclass = NULL;
    IndexStmt* index = NULL;
    List* indexprs = NIL;
    ListCell* indexprItem = NULL;
    Oid indrelid;
    int keyno;
    Oid keycoltype;
    Datum datum;
    bool isnull = false;
    bool isResize = false;

    /*
     * Fetch pg_class tuple of source index.  We can't use the copy in the
     * relcache entry because it doesn't include optional fields.
     */
    htIdxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(sourceRelid));
    if (!HeapTupleIsValid(htIdxrel))
        ereport(
            ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", sourceRelid)));
    idxrelrec = (Form_pg_class)GETSTRUCT(htIdxrel);

    /* Fetch pg_index tuple for source index from relcache entry */
    htIdx = source_idx->rd_indextuple;
    idxrec = (Form_pg_index)GETSTRUCT(htIdx);
    indrelid = idxrec->indrelid;

    /* Fetch pg_am tuple for source index from relcache entry */
    amrec = source_idx->rd_am;

    /* Extract indcollation from the pg_index tuple */
    datum = SysCacheGetAttr(INDEXRELID, htIdx, Anum_pg_index_indcollation, &isnull);
    Assert(!isnull);

    indcollation = (oidvector*)DatumGetPointer(datum);

    /* Extract indclass from the pg_index tuple */
    datum = SysCacheGetAttr(INDEXRELID, htIdx, Anum_pg_index_indclass, &isnull);
    Assert(!isnull);

    indclass = (oidvector*)DatumGetPointer(datum);

    /* Begin building the IndexStmt */
    index = makeNode(IndexStmt);
    index->relation = cxt->relation;
    index->accessMethod = pstrdup(NameStr(amrec->amname));
    if (OidIsValid(idxrelrec->reltablespace))
        index->tableSpace = get_tablespace_name(idxrelrec->reltablespace);
    else
        index->tableSpace = NULL;
    index->excludeOpNames = NIL;
    index->idxcomment = NULL;
    index->indexOid = InvalidOid;
    index->oldNode = InvalidOid;
    index->oldPSortOid = InvalidOid;
    index->unique = idxrec->indisunique;
    index->primary = idxrec->indisprimary;
    index->concurrent = false;
    // mark if the resulting indexStmt is a partitioned index
    index->isPartitioned = RelationIsPartitioned(source_idx);

    /*
     * If the src table is in resizing, means we are going to do create table like for tmp table,
     * then we preserve the index name by src index.
     * Otherwise, set idxname to NULL, let DefineIndex() choose a reasonable name.
     */
    if (PointerIsValid(rel) && RelationInClusterResizing(rel)) {
        /* Generate idxname based on src index name */
        errno_t rc;
        uint4 len = strlen(NameStr(source_idx->rd_rel->relname)) + 1;
        Assert(len <= NAMEDATALEN);

        index->idxname = (char*)palloc(len);
        rc = strncpy_s(index->idxname, len, NameStr(source_idx->rd_rel->relname), len - 1);
        securec_check(rc, "", "");
        isResize = true;
    } else {
        index->idxname = NULL;
    }

    /*
     * If the index is marked PRIMARY or has an exclusion condition, it's
     * certainly from a constraint; else, if it's not marked UNIQUE, it
     * certainly isn't.  If it is or might be from a constraint, we have to
     * fetch the pg_constraint record.
     */
    if (index->primary || index->unique || idxrec->indisexclusion) {
        Oid constraintId = get_index_constraint(sourceRelid);
        if (OidIsValid(constraintId)) {
            HeapTuple ht_constr;
            Form_pg_constraint conrec;

            ht_constr = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintId));
            if (!HeapTupleIsValid(ht_constr))
                ereport(ERROR,
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                        errmodule(MOD_OPT),
                        errmsg("cache lookup failed for constraint %u", constraintId)));
            conrec = (Form_pg_constraint)GETSTRUCT(ht_constr);

            index->isconstraint = true;
            index->deferrable = conrec->condeferrable;
            index->initdeferred = conrec->condeferred;

            /* If it's an exclusion constraint, we need the operator names */
            if (idxrec->indisexclusion) {
                Datum* elems = NULL;
                int nElems;
                int i;

                Assert(conrec->contype == CONSTRAINT_EXCLUSION);
                /* Extract operator OIDs from the pg_constraint tuple */
                datum = SysCacheGetAttr(CONSTROID, ht_constr, Anum_pg_constraint_conexclop, &isnull);
                if (isnull)
                    ereport(ERROR,
                        (errcode(ERRCODE_UNEXPECTED_NULL_VALUE),
                            errmodule(MOD_OPT),
                            errmsg("null conexclop for constraint %u", constraintId)));

                deconstruct_array(DatumGetArrayTypeP(datum), OIDOID, sizeof(Oid), true, 'i', &elems, NULL, &nElems);

                for (i = 0; i < nElems; i++) {
                    Oid operid = DatumGetObjectId(elems[i]);
                    HeapTuple opertup;
                    Form_pg_operator operform;
                    char* oprname = NULL;
                    char* nspname = NULL;
                    List* namelist = NIL;

                    opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operid));
                    if (!HeapTupleIsValid(opertup))
                        ereport(ERROR,
                            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                                errmodule(MOD_OPT),
                                errmsg("cache lookup failed for operator %u", operid)));

                    operform = (Form_pg_operator)GETSTRUCT(opertup);
                    oprname = pstrdup(NameStr(operform->oprname));
                    /* For simplicity we always schema-qualify the op name */
                    nspname = get_namespace_name(operform->oprnamespace, true);
                    namelist = list_make2(makeString(nspname), makeString(oprname));
                    index->excludeOpNames = lappend(index->excludeOpNames, namelist);
                    ReleaseSysCache(opertup);
                }
            }

            ReleaseSysCache(ht_constr);
        } else
            index->isconstraint = false;
    } else
        index->isconstraint = false;

    /* Get the index expressions, if any */
    datum = SysCacheGetAttr(INDEXRELID, htIdx, Anum_pg_index_indexprs, &isnull);
    if (!isnull) {
        char* exprsString = NULL;

        exprsString = TextDatumGetCString(datum);
        indexprs = (List*)stringToNode(exprsString);
    } else {
        indexprs = NIL;
}
    /* Build the list of IndexElem */
    index->indexParams = NIL;

    indexprItem = list_head(indexprs);
    for (keyno = 0; keyno < idxrec->indnatts; keyno++) {
        IndexElem* iparam = NULL;
        AttrNumber attnum = idxrec->indkey.values[keyno];
        uint16 opt = (uint16)source_idx->rd_indoption[keyno];

        iparam = makeNode(IndexElem);

        if (AttributeNumberIsValid(attnum)) {
            /* Simple index column */
            char* attname = NULL;

            attname = get_relid_attribute_name(indrelid, attnum);
            keycoltype = get_atttype(indrelid, attnum);

            iparam->name = attname;
            iparam->expr = NULL;
        } else {
            /* Expressional index */
            Node* indexkey = NULL;
            bool found_whole_row = false;

            if (indexprItem == NULL)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                        errmodule(MOD_OPT),
                        errmsg("too few entries in indexprs list")));

            indexkey = (Node*)lfirst(indexprItem);
            indexprItem = lnext(indexprItem);

            /* Adjust Vars to match new table's column numbering */
            indexkey = map_variable_attnos(indexkey, 1, 0, attmap, attmap_length, &found_whole_row);

            /* As in transformTableLikeClause, reject whole-row variables */
            if (found_whole_row)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot convert whole-row table reference"),
                        errdetail("Index \"%s\" contains a whole-row table reference.",
                            RelationGetRelationName(source_idx))));

            iparam->name = NULL;
            iparam->expr = indexkey;

            keycoltype = exprType(indexkey);
        }

        /* Copy the original index column name */
        iparam->indexcolname = pstrdup(NameStr(attrs[keyno]->attname));

        /* Add the collation name, if non-default */
        iparam->collation = get_collation(indcollation->values[keyno], keycoltype);

        /* Add the operator class name, if non-default */
        iparam->opclass = get_opclass(indclass->values[keyno], keycoltype);

        iparam->ordering = SORTBY_DEFAULT;
        iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

        /* Adjust options if necessary */
        if (amrec->amcanorder) {
            /*
             * If it supports sort ordering, copy DESC and NULLS opts. Don't
             * set non-default settings unnecessarily, though, so as to
             * improve the chance of recognizing equivalence to constraint
             * indexes.
             */
            if (((uint16)opt) & INDOPTION_DESC) {
                iparam->ordering = SORTBY_DESC;
                if ((((uint16)opt) & INDOPTION_NULLS_FIRST) == 0)
                    iparam->nulls_ordering = SORTBY_NULLS_LAST;
            } else {
                if (((uint16)opt) & INDOPTION_NULLS_FIRST)
                    iparam->nulls_ordering = SORTBY_NULLS_FIRST;
            }
        }

        index->indexParams = lappend(index->indexParams, iparam);
    }

    if (u_sess->attr.attr_sql.enable_cluster_resize && 
            isResize && RELATION_OWN_BUCKET(rel)) {
        if (!index->isPartitioned) {
            TryReuseIndex(source_idx->rd_id, index);
        } else {
            tryReusePartedIndex(source_idx->rd_id, index, rel);
        }
    }

    /* Copy reloptions if any */
    datum = SysCacheGetAttr(RELOID, htIdxrel, Anum_pg_class_reloptions, &isnull);
    if (!isnull)
        index->options = untransformRelOptions(datum);

    /* If it's a partial index, decompile and append the predicate */
    datum = SysCacheGetAttr(INDEXRELID, htIdx, Anum_pg_index_indpred, &isnull);
    if (!isnull) {
        char* pred_str = NULL;
        Node* pred_tree = NULL;
        bool found_whole_row = false;

        /* Convert text string to node tree */
        pred_str = TextDatumGetCString(datum);
        pred_tree = (Node*)stringToNode(pred_str);

        /* Adjust Vars to match new table's column numbering */
        pred_tree = map_variable_attnos(pred_tree, 1, 0, attmap, attmap_length, &found_whole_row);

        /* As in transformTableLikeClause, reject whole-row variables */
        if (found_whole_row)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot convert whole-row table reference"),
                    errdetail(
                        "Index \"%s\" contains a whole-row table reference.", RelationGetRelationName(source_idx))));

        index->whereClause = pred_tree;
    }

    /* Clean up */
    ReleaseSysCache(htIdxrel);

    return index;
}

/*
 * get_collation		- fetch qualified name of a collation
 *
 * If collation is InvalidOid or is the default for the given actual_datatype,
 * then the return value is NIL.
 */
static List* get_collation(Oid collation, Oid actual_datatype)
{
    List* result = NIL;
    HeapTuple htColl;
    Form_pg_collation collRec;
    char* nspName = NULL;
    char* collName = NULL;

    if (!OidIsValid(collation))
        return NIL; /* easy case */
    if (collation == get_typcollation(actual_datatype))
        return NIL; /* just let it default */

    htColl = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
    if (!HeapTupleIsValid(htColl))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmodule(MOD_OPT),
                errmsg("cache lookup failed for collation %u", collation)));

    collRec = (Form_pg_collation)GETSTRUCT(htColl);

    /* For simplicity, we always schema-qualify the name */
    nspName = get_namespace_name(collRec->collnamespace, true);
    collName = pstrdup(NameStr(collRec->collname));
    result = list_make2(makeString(nspName), makeString(collName));

    ReleaseSysCache(htColl);
    return result;
}

/*
 * get_opclass			- fetch qualified name of an index operator class
 *
 * If the opclass is the default for the given actual_datatype, then
 * the return value is NIL.
 */
static List* get_opclass(Oid opclass, Oid actual_datatype)
{
    List* result = NIL;
    HeapTuple htOpc;
    Form_pg_opclass opcRec;
    htOpc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
    if (!HeapTupleIsValid(htOpc))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmodule(MOD_OPT),
                errmsg("cache lookup failed for opclass %u", opclass)));
    opcRec = (Form_pg_opclass)GETSTRUCT(htOpc);
    if (GetDefaultOpClass(actual_datatype, opcRec->opcmethod) != opclass) {
        /* For simplicity, we always schema-qualify the name */
        char* nspName = get_namespace_name(opcRec->opcnamespace, true);
        char* opcName = pstrdup(NameStr(opcRec->opcname));

        result = list_make2(makeString(nspName), makeString(opcName));
    }

    ReleaseSysCache(htOpc);
    return result;
}

/*
 * transformIndexConstraints
 *		Handle UNIQUE, PRIMARY KEY, EXCLUDE constraints, which create indexes.
 *		We also merge in any index definitions arising from
 *		LIKE ... INCLUDING INDEXES.
 */
static void transformIndexConstraints(CreateStmtContext* cxt)
{
    IndexStmt* index = NULL;
    List* indexlist = NIL;
    ListCell* lc = NULL;

    /*
     * Run through the constraints that need to generate an index. For PRIMARY
     * KEY, mark each column as NOT NULL and create an index. For UNIQUE or
     * EXCLUDE, create an index as for PRIMARY KEY, but do not insist on NOT
     * NULL.
     */
    foreach (lc, cxt->ixconstraints) {
        Constraint* constraint = (Constraint*)lfirst(lc);

        AssertEreport(IsA(constraint, Constraint), MOD_OPT, "");
        AssertEreport(constraint->contype == CONSTR_PRIMARY || constraint->contype == CONSTR_UNIQUE ||
                          constraint->contype == CONSTR_EXCLUSION,
            MOD_OPT,
            "");
        if (cxt->ispartitioned && !cxt->isalter) {
            AssertEreport(PointerIsValid(cxt->partitionKey), MOD_OPT, "");

            /*
             * @hdfs
             * Columns of PRIMARY KEY/UNIQUE could be any columns on HDFS partition table.
             * If the partition foreign table will support real index, the following code must
             * be modified.
             */
            if (IsA(cxt->node, CreateForeignTableStmt) &&
                isObsOrHdfsTableFormSrvName(((CreateForeignTableStmt*)cxt->node)->servername)) {
                /* Do nothing */
            } else if (constraint->contype == CONSTR_EXCLUSION) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Partitioned table does not support EXCLUDE index")));
            } else {
                ListCell* ixcell = NULL;
                ListCell* pkcell = NULL;

                foreach (pkcell, cxt->partitionKey) {
                    ColumnRef* colref = (ColumnRef*)lfirst(pkcell);
                    char* pkname = ((Value*)linitial(colref->fields))->val.str;
                    bool found = false;

                    foreach (ixcell, constraint->keys) {
                        char* ikname = strVal(lfirst(ixcell));

                        /*
                         * Indexkey column for PRIMARY KEY/UNIQUE constraint Must
                         * contain partitionKey
                         */
                        if (!strcmp(pkname, ikname)) {
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("Invalid PRIMARY KEY/UNIQUE constraint for partitioned table"),
                                errdetail("Columns of PRIMARY KEY/UNIQUE constraint Must contain PARTITION KEY")));
                    }
                }
            }
        }

        index = transformIndexConstraint(constraint, cxt);

        indexlist = lappend(indexlist, index);
    }

    /* Add in any indexes defined by LIKE ... INCLUDING INDEXES */
    foreach (lc, cxt->inh_indexes) {
        index = (IndexStmt*)lfirst(lc);
        if (index->primary) {
            if (cxt->pkey != NULL)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("multiple primary keys for table \"%s\" are not allowed", cxt->relation->relname)));
            cxt->pkey = index;
        }

        indexlist = lappend(indexlist, index);
    }

    /*
     * Scan the index list and remove any redundant index specifications. This
     * can happen if, for instance, the user writes UNIQUE PRIMARY KEY. A
     * strict reading of SQL92 would suggest raising an error instead, but
     * that strikes me as too anal-retentive. - tgl 2001-02-14
     *
     * XXX in ALTER TABLE case, it'd be nice to look for duplicate
     * pre-existing indexes, too.
     */
    AssertEreport(cxt->alist == NIL, MOD_OPT, "");
    if (cxt->pkey != NULL) {
        /* Make sure we keep the PKEY index in preference to others... */
        cxt->alist = list_make1(cxt->pkey);
    }

    foreach (lc, indexlist) {
        bool keep = true;
        ListCell* k = NULL;

        index = (IndexStmt*)lfirst(lc);
        /* if it's pkey, it's already in cxt->alist */
        if (index == cxt->pkey)
            continue;

        /*
         * For create table like, if the table is resizing, don't remove redundant index,
         * because we need to keep the index totally same with origin table's indices.
         */
        if (cxt->isResizing) {
            cxt->alist = lappend(cxt->alist, index);
            continue;
        }

        foreach (k, cxt->alist) {
            IndexStmt* priorindex = (IndexStmt*)lfirst(k);

            if (equal(index->indexParams, priorindex->indexParams) &&
                equal(index->whereClause, priorindex->whereClause) &&
                equal(index->excludeOpNames, priorindex->excludeOpNames) &&
                strcmp(index->accessMethod, priorindex->accessMethod) == 0 &&
                index->deferrable == priorindex->deferrable && index->initdeferred == priorindex->initdeferred) {
                priorindex->unique = priorindex->unique || index->unique;

                /*
                 * If the prior index is as yet unnamed, and this one is
                 * named, then transfer the name to the prior index. This
                 * ensures that if we have named and unnamed constraints,
                 * we'll use (at least one of) the names for the index.
                 */
                if (priorindex->idxname == NULL)
                    priorindex->idxname = index->idxname;
                keep = false;
                break;
            }
        }

        if (keep)
            cxt->alist = lappend(cxt->alist, index);
    }
}

/*
 * If it's ALTER TABLE ADD CONSTRAINT USING INDEX,
 * verify the index is usable.
 */
static void checkConditionForTransformIndex(
    Constraint* constraint, CreateStmtContext* cxt, Oid index_oid, Relation index_rel)
{
    if (constraint == NULL || cxt == NULL || index_rel == NULL)
        return;

    char* indexName = constraint->indexname;
    Form_pg_index indexForm = index_rel->rd_index;
    Relation heapRel = cxt->rel;

    /* Check that it does not have an associated constraint already */
    if (OidIsValid(get_index_constraint(index_oid)))
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("index \"%s\" is already associated with a constraint", indexName),
                parser_errposition(cxt->pstate, constraint->location)));

    /* Perform validity checks on the index */
    if (indexForm->indrelid != RelationGetRelid(heapRel))
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("index \"%s\" does not belong to table \"%s\"", indexName, RelationGetRelationName(heapRel)),
                parser_errposition(cxt->pstate, constraint->location)));

    if (!IndexIsValid(indexForm))
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("index \"%s\" is not valid", indexName),
                parser_errposition(cxt->pstate, constraint->location)));

    if (!indexForm->indisunique)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is not a unique index", indexName),
                errdetail("Cannot create a primary key or unique constraint using such an index."),
                parser_errposition(cxt->pstate, constraint->location)));

    if (RelationGetIndexExpressions(index_rel) != NIL)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("index \"%s\" contains expressions", indexName),
                errdetail("Cannot create a primary key or unique constraint using such an index."),
                parser_errposition(cxt->pstate, constraint->location)));

    if (RelationGetIndexPredicate(index_rel) != NIL)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is a partial index", indexName),
                errdetail("Cannot create a primary key or unique constraint using such an index."),
                parser_errposition(cxt->pstate, constraint->location)));

    /*
     * It's probably unsafe to change a deferred index to non-deferred. (A
     * non-constraint index couldn't be deferred anyway, so this case
     * should never occur; no need to sweat, but let's check it.)
     */
    if (!indexForm->indimmediate && !constraint->deferrable)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is a deferrable index", indexName),
                errdetail("Cannot create a non-deferrable constraint using a deferrable index."),
                parser_errposition(cxt->pstate, constraint->location)));

    /*
     * Insist on it being a btree.	That's the only kind that supports
     * uniqueness at the moment anyway; but we must have an index that
     * exactly matches what you'd get from plain ADD CONSTRAINT syntax,
     * else dump and reload will produce a different index (breaking
     * pg_upgrade in particular).
     */
    if (index_rel->rd_rel->relam != get_am_oid(DEFAULT_INDEX_TYPE, false))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("index \"%s\" is not a btree", indexName),
                parser_errposition(cxt->pstate, constraint->location)));
}

/*
 * transformIndexConstraint
 *		Transform one UNIQUE, PRIMARY KEY, or EXCLUDE constraint for
 *		transformIndexConstraints.
 */
static IndexStmt* transformIndexConstraint(Constraint* constraint, CreateStmtContext* cxt)
{
    IndexStmt* index = NULL;
    ListCell* lc = NULL;

    index = makeNode(IndexStmt);

    index->unique = (constraint->contype != CONSTR_EXCLUSION);
    index->primary = (constraint->contype == CONSTR_PRIMARY);
    if (index->primary) {
        if (cxt->pkey != NULL) {
            if (0 == pg_strncasecmp(cxt->stmtType, CREATE_FOREIGN_TABLE, strlen(cxt->stmtType)) ||
                0 == pg_strncasecmp(cxt->stmtType, ALTER_FOREIGN_TABLE, strlen(cxt->stmtType))) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg(
                            "Multiple primary keys for foreign table \"%s\" are not allowed.", cxt->relation->relname),
                        parser_errposition(cxt->pstate, constraint->location)));
            } else {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("multiple primary keys for table \"%s\" are not allowed", cxt->relation->relname),
                        parser_errposition(cxt->pstate, constraint->location)));
            }
        }
        cxt->pkey = index;

        /*
         * In ALTER TABLE case, a primary index might already exist, but
         * DefineIndex will check for it.
         */
    }
    index->isconstraint = true;
    index->deferrable = constraint->deferrable;
    index->initdeferred = constraint->initdeferred;

    if (constraint->conname != NULL)
        index->idxname = pstrdup(constraint->conname);
    else
        index->idxname = NULL; /* DefineIndex will choose name */

    index->relation = cxt->relation;
    index->accessMethod = const_cast<char*>(constraint->access_method ? constraint->access_method : DEFAULT_INDEX_TYPE);
    index->options = constraint->options;
    index->tableSpace = constraint->indexspace;
    index->whereClause = constraint->where_clause;
    index->indexParams = NIL;
    index->excludeOpNames = NIL;
    index->idxcomment = NULL;
    index->indexOid = InvalidOid;
    index->oldNode = InvalidOid;
    index->oldPSortOid = InvalidOid;
    index->concurrent = false;

    /*
     * @hdfs
     * The foreign table dose not have index. the HDFS foreign table has informational
     * constraint which is not a index.
     * If the partition foreign table will support real index, the following code must
     * be modified.
     */
    if (0 == pg_strncasecmp(cxt->stmtType, CREATE_FOREIGN_TABLE, strlen(cxt->stmtType)) ||
        0 == pg_strncasecmp(cxt->stmtType, ALTER_FOREIGN_TABLE, strlen(cxt->stmtType))) {
        index->isPartitioned = false;
    } else {
        index->isPartitioned = cxt->ispartitioned;
    }

    index->inforConstraint = constraint->inforConstraint;

    /*
     * If it's ALTER TABLE ADD CONSTRAINT USING INDEX, look up the index and
     * verify it's usable, then extract the implied column name list.  (We
     * will not actually need the column name list at runtime, but we need it
     * now to check for duplicate column entries below.)
     */
    if (constraint->indexname != NULL) {
        char* indexName = constraint->indexname;
        Relation heapRel = cxt->rel;
        Oid indexOid;
        Relation indexRel;
        Form_pg_index indexForm;
        oidvector* indclass = NULL;
        Datum indclassDatum;
        bool isnull = true;
        int i;

        /* Grammar should not allow this with explicit column list */
        AssertEreport(constraint->keys == NIL, MOD_OPT, "");

        /* Grammar should only allow PRIMARY and UNIQUE constraints */
        AssertEreport(constraint->contype == CONSTR_PRIMARY || constraint->contype == CONSTR_UNIQUE, MOD_OPT, "");

        /* Must be ALTER, not CREATE, but grammar doesn't enforce that */
        if (!cxt->isalter)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot use an existing index in CREATE TABLE"),
                    parser_errposition(cxt->pstate, constraint->location)));

        /* Look for the index in the same schema as the table */
        indexOid = get_relname_relid(indexName, RelationGetNamespace(heapRel));
        if (!OidIsValid(indexOid))
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("index \"%s\" does not exist", indexName),
                    parser_errposition(cxt->pstate, constraint->location)));

        /* Open the index (this will throw an error if it is not an index) */
        indexRel = index_open(indexOid, AccessShareLock);
        indexForm = indexRel->rd_index;

        /* check the conditons for this function,
         * and verify the index is usable
         */
        checkConditionForTransformIndex(constraint, cxt, indexOid, indexRel);

        /* Must get indclass the hard way */
        indclassDatum = SysCacheGetAttr(INDEXRELID, indexRel->rd_indextuple, Anum_pg_index_indclass, &isnull);
        AssertEreport(!isnull, MOD_OPT, "");
        indclass = (oidvector*)DatumGetPointer(indclassDatum);

        for (i = 0; i < indexForm->indnatts; i++) {
            int2 attnum = indexForm->indkey.values[i];
            Form_pg_attribute attform;
            char* attname = NULL;
            Oid defopclass;

            /*
             * We shouldn't see attnum == 0 here, since we already rejected
             * expression indexes.	If we do, SystemAttributeDefinition will
             * throw an error.
             */
            if (attnum > 0) {
                AssertEreport(attnum <= heapRel->rd_att->natts, MOD_OPT, "");
                attform = heapRel->rd_att->attrs[attnum - 1];
            } else
                attform = SystemAttributeDefinition(attnum, heapRel->rd_rel->relhasoids,  
                                  RELATION_HAS_BUCKET(heapRel));
            attname = pstrdup(NameStr(attform->attname));

            /*
             * Insist on default opclass and sort options.	While the index
             * would still work as a constraint with non-default settings, it
             * might not provide exactly the same uniqueness semantics as
             * you'd get from a normally-created constraint; and there's also
             * the dump/reload problem mentioned above.
             */
            defopclass = GetDefaultOpClass(attform->atttypid, indexRel->rd_rel->relam);
            if (indclass->values[i] != defopclass || indexRel->rd_indoption[i] != 0)
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("index \"%s\" does not have default sorting behavior", indexName),
                        errdetail("Cannot create a primary key or unique constraint using such an index."),
                        parser_errposition(cxt->pstate, constraint->location)));

            constraint->keys = lappend(constraint->keys, makeString(attname));
        }

        /* Close the index relation but keep the lock */
        relation_close(indexRel, NoLock);

        index->indexOid = indexOid;
    }

    /*
     * If it's an EXCLUDE constraint, the grammar returns a list of pairs of
     * IndexElems and operator names.  We have to break that apart into
     * separate lists.
     */
    if (constraint->contype == CONSTR_EXCLUSION) {
        foreach (lc, constraint->exclusions) {
            List* pair = (List*)lfirst(lc);
            IndexElem* elem = NULL;
            List* opname = NIL;

            Assert(list_length(pair) == 2);
            elem = (IndexElem*)linitial(pair);
            Assert(IsA(elem, IndexElem));
            opname = (List*)lsecond(pair);
            Assert(IsA(opname, List));

            index->indexParams = lappend(index->indexParams, elem);
            index->excludeOpNames = lappend(index->excludeOpNames, opname);
        }

        return index;
    }

    /*
     * For UNIQUE and PRIMARY KEY, we just have a list of column names.
     *
     * Make sure referenced keys exist.  If we are making a PRIMARY KEY index,
     * also make sure they are NOT NULL, if possible. (Although we could leave
     * it to DefineIndex to mark the columns NOT NULL, it's more efficient to
     * get it right the first time.)
     */
    foreach (lc, constraint->keys) {
        char* key = strVal(lfirst(lc));
        bool found = false;
        ColumnDef* column = NULL;
        ListCell* columns = NULL;
        IndexElem* iparam = NULL;

        foreach (columns, cxt->columns) {
            column = (ColumnDef*)lfirst(columns);
            AssertEreport(IsA(column, ColumnDef), MOD_OPT, "");
            if (strcmp(column->colname, key) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            /* found column in the new table; force it to be NOT NULL */
            if (constraint->contype == CONSTR_PRIMARY && !constraint->inforConstraint->nonforced)
                column->is_not_null = TRUE;
        } else if (SystemAttributeByName(key, cxt->hasoids) != NULL) {
            /*
             * column will be a system column in the new table, so accept it.
             * System columns can't ever be null, so no need to worry about
             * PRIMARY/NOT NULL constraint.
             */
            found = true;
        } else if (cxt->inhRelations != NIL) {
            /* try inherited tables */
            ListCell* inher = NULL;

            foreach (inher, cxt->inhRelations) {
                RangeVar* inh = (RangeVar*)lfirst(inher);
                Relation rel;
                int count;

                AssertEreport(IsA(inh, RangeVar), MOD_OPT, "");
                rel = heap_openrv(inh, AccessShareLock);
                if (rel->rd_rel->relkind != RELKIND_RELATION)
                    ereport(ERROR,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("inherited relation \"%s\" is not a table", inh->relname)));
                for (count = 0; count < rel->rd_att->natts; count++) {
                    Form_pg_attribute inhattr = rel->rd_att->attrs[count];
                    char* inhname = NameStr(inhattr->attname);

                    if (inhattr->attisdropped)
                        continue;
                    if (strcmp(key, inhname) == 0) {
                        found = true;

                        /*
                         * We currently have no easy way to force an inherited
                         * column to be NOT NULL at creation, if its parent
                         * wasn't so already. We leave it to DefineIndex to
                         * fix things up in this case.
                         */
                        break;
                    }
                }
                heap_close(rel, NoLock);
                if (found)
                    break;
            }
        }

        /*
         * In the ALTER TABLE case, don't complain about index keys not
         * created in the command; they may well exist already. DefineIndex
         * will complain about them if not, and will also take care of marking
         * them NOT NULL.
         */
        if (!found && !cxt->isalter)
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" named in key does not exist", key),
                    parser_errposition(cxt->pstate, constraint->location)));

        /* Check for PRIMARY KEY(foo, foo) */
        foreach (columns, index->indexParams) {
            iparam = (IndexElem*)lfirst(columns);
            if (iparam->name && strcmp(key, iparam->name) == 0) {
                if (index->primary)
                    ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_COLUMN),
                            errmsg("column \"%s\" appears twice in primary key constraint", key),
                            parser_errposition(cxt->pstate, constraint->location)));
                else
                    ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_COLUMN),
                            errmsg("column \"%s\" appears twice in unique constraint", key),
                            parser_errposition(cxt->pstate, constraint->location)));
            }
        }

#ifdef PGXC
        /*
         * Set fallback distribution column.
         * If not set, set it to first column in index.
         * If primary key, we prefer that over a unique constraint.
         */
        if (index->indexParams == NIL && (index->primary || cxt->fallback_dist_col == NULL)) {
            if (cxt->fallback_dist_col != NULL) {
                list_free_deep(cxt->fallback_dist_col);
                cxt->fallback_dist_col = NULL;
            }
            cxt->fallback_dist_col = lappend(cxt->fallback_dist_col, makeString(pstrdup(key)));
        }
#endif

        /* OK, add it to the index definition */
        iparam = makeNode(IndexElem);
        iparam->name = pstrdup(key);
        iparam->expr = NULL;
        iparam->indexcolname = NULL;
        iparam->collation = NIL;
        iparam->opclass = NIL;
        iparam->ordering = SORTBY_DEFAULT;
        iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
        index->indexParams = lappend(index->indexParams, iparam);
    }

    return index;
}

/*
 * transformFKConstraints
 *		handle FOREIGN KEY constraints
 */
static void transformFKConstraints(CreateStmtContext* cxt, bool skipValidation, bool isAddConstraint)
{
    ListCell* fkclist = NULL;

    if (cxt->fkconstraints == NIL)
        return;

    /*
     * If CREATE TABLE or adding a column with NULL default, we can safely
     * skip validation of FK constraints, and nonetheless mark them valid.
     * (This will override any user-supplied NOT VALID flag.)
     */
    if (skipValidation) {
        foreach (fkclist, cxt->fkconstraints) {
            Constraint* constraint = (Constraint*)lfirst(fkclist);

            constraint->skip_validation = true;
            constraint->initially_valid = true;
#ifdef PGXC
            /*
             * Set fallback distribution column.
             * If not yet set, set it to first column in FK constraint
             * if it references a partitioned table
             */
            if (IS_PGXC_COORDINATOR && cxt->fallback_dist_col == NIL && list_length(constraint->pk_attrs) != 0) {
                if (list_length(constraint->pk_attrs) != list_length(constraint->fk_attrs)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_FOREIGN_KEY),
                            errmsg("number of referencing and referenced columns for foreign key disagree")));
                }

                Oid pk_rel_id = RangeVarGetRelid(constraint->pktable, NoLock, false);
                RelationLocInfo* locInfo = GetRelationLocInfo(pk_rel_id);

                if (locInfo != NULL && locInfo->partAttrNum != NIL) {
                    int i = 0;
                    char* colstr = NULL;
                    ListCell *cell = NULL; 
                    ListCell *pkCell = NULL;
                    AttrNumber attnum;
                    AttrNumber pkAttnum;

                    foreach (cell, locInfo->partAttrNum) {
                        attnum = lfirst_int(cell);
                        /* This table is replication */
                        if (attnum == 0) {
                            break;
                        }

                        i = 0;
                        foreach (pkCell, constraint->pk_attrs) {
                            pkAttnum = get_attnum(pk_rel_id, strVal(lfirst(pkCell)));
                            if (attnum == pkAttnum) {
                                break;
                            }
                            i++;
                        }
                        if (pkCell == NULL) {
                            list_free_deep(cxt->fallback_dist_col);
                            cxt->fallback_dist_col = NULL;
                            break;
                        } else {
                            colstr = pstrdup(strVal(list_nth(constraint->fk_attrs, i)));
                            cxt->fallback_dist_col = lappend(cxt->fallback_dist_col, makeString(colstr));
                        }
                    }
                }
            }
#endif
        }
    }

    /*
     * For CREATE TABLE or ALTER TABLE ADD COLUMN, gin up an ALTER TABLE ADD
     * CONSTRAINT command to execute after the basic command is complete. (If
     * called from ADD CONSTRAINT, that routine will add the FK constraints to
     * its own subcommand list.)
     *
     * Note: the ADD CONSTRAINT command must also execute after any index
     * creation commands.  Thus, this should run after
     * transformIndexConstraints, so that the CREATE INDEX commands are
     * already in cxt->alist.
     */
    if (!isAddConstraint) {
        AlterTableStmt* alterstmt = makeNode(AlterTableStmt);

        alterstmt->relation = cxt->relation;
        alterstmt->cmds = NIL;
        alterstmt->relkind = OBJECT_TABLE;

        foreach (fkclist, cxt->fkconstraints) {
            Constraint* constraint = (Constraint*)lfirst(fkclist);
            AlterTableCmd* altercmd = makeNode(AlterTableCmd);

            altercmd->subtype = AT_ProcessedConstraint;
            altercmd->name = NULL;
            altercmd->def = (Node*)constraint;
            alterstmt->cmds = lappend(alterstmt->cmds, altercmd);
        }

        cxt->alist = lappend(cxt->alist, alterstmt);
    }
}

/*
 * transformIndexStmt - parse analysis for CREATE INDEX and ALTER TABLE
 *
 * Note: this is a no-op for an index not using either index expressions or
 * a predicate expression.	There are several code paths that create indexes
 * without bothering to call this, because they know they don't have any
 * such expressions to deal with.
 */
IndexStmt* transformIndexStmt(Oid relid, IndexStmt* stmt, const char* queryString)
{
    Relation rel;
    ParseState* pstate = NULL;
    RangeTblEntry* rte = NULL;
    ListCell* l = NULL;

    /*
     * We must not scribble on the passed-in IndexStmt, so copy it.  (This is
     * overkill, but easy.)
     */
    stmt = (IndexStmt*)copyObject(stmt);

    /* Set up pstate */
    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = queryString;

    /*
     * Put the parent table into the rtable so that the expressions can refer
     * to its fields without qualification.  Caller is responsible for locking
     * relation, but we still need to open it.
     */
    rel = relation_open(relid, NoLock);
    rte = addRangeTableEntry(pstate, stmt->relation, NULL, false, true, true, false, true);

    if (RelationIsTsStore(rel)) {
        /* timeseries store does not support index for now */
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("timeseries store does not support add index ")));
    }

    if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE && isMOTFromTblOid(RelationGetRelid(rel))) {
        stmt->internal_flag = true;
    }

    bool isColStore = RelationIsColStore(rel);
    if (stmt->accessMethod == NULL) {
        if (!isColStore) {
            /* row store using btree index by default */
            stmt->accessMethod = DEFAULT_INDEX_TYPE;
        } else {
            /* column store using psort index by default */
            stmt->accessMethod = DEFAULT_CSTORE_INDEX_TYPE;
        }
    } else {
        bool isDfsStore = RelationIsDfsStore(rel);
        const bool isPsortMothed = (0 == pg_strcasecmp(stmt->accessMethod, DEFAULT_CSTORE_INDEX_TYPE));

        /* check if this is the cstore btree index */
        bool isCBtreeMethod = false;
        if (isColStore && ((0 == pg_strcasecmp(stmt->accessMethod, DEFAULT_INDEX_TYPE)) ||
                              (0 == pg_strcasecmp(stmt->accessMethod, CSTORE_BTREE_INDEX_TYPE)))) {
            stmt->accessMethod = CSTORE_BTREE_INDEX_TYPE;
            isCBtreeMethod = true;
        }

        /* check if this is the cstore gin btree index */
        bool isCGinBtreeMethod = false;
        if (isColStore && ((0 == pg_strcasecmp(stmt->accessMethod, DEFAULT_GIN_INDEX_TYPE)) ||
                              (0 == pg_strcasecmp(stmt->accessMethod, CSTORE_GINBTREE_INDEX_TYPE)))) {
            stmt->accessMethod = CSTORE_GINBTREE_INDEX_TYPE;
            isCGinBtreeMethod = true;
        }

        if (isCGinBtreeMethod && is_feature_disabled(MULTI_VALUE_COLUMN)) {
            /* cgin index is disabled */
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Unsupport cgin index in this version")));
        }

        if (!isColStore && (0 != pg_strcasecmp(stmt->accessMethod, DEFAULT_INDEX_TYPE)) &&
            (0 != pg_strcasecmp(stmt->accessMethod, DEFAULT_GIN_INDEX_TYPE)) &&
            (0 != pg_strcasecmp(stmt->accessMethod, DEFAULT_GIST_INDEX_TYPE))) {
            /* row store only support btree/gin/gist index */
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"%s\" does not support row store", stmt->accessMethod)));
        }
        if (isColStore && (!isPsortMothed && !isCBtreeMethod && !isCGinBtreeMethod)) {
            /* column store support psort/cbtree/gin index */
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"%s\" does not support column store", stmt->accessMethod)));
        } else if (isColStore && isCGinBtreeMethod && isDfsStore) {
            /* dfs store does not support cginbtree index currently */
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"%s\" does not support dfs store", stmt->accessMethod)));
        }
    }

    /* no to join list, yes to namespaces */
    addRTEtoQuery(pstate, rte, false, true, true);

    /* take care of the where clause */
    if (stmt->whereClause) {
        stmt->whereClause = transformWhereClause(pstate, stmt->whereClause, "WHERE");
        /* we have to fix its collations too */
        assign_expr_collations(pstate, stmt->whereClause);
    }

    /* take care of any index expressions */
    foreach (l, stmt->indexParams) {
        IndexElem* ielem = (IndexElem*)lfirst(l);

        if (ielem->expr) {
            /* Extract preliminary index col name before transforming expr */
            if (ielem->indexcolname == NULL)
                ielem->indexcolname = FigureIndexColname(ielem->expr);

            /* Now do parse transformation of the expression */
            ielem->expr = transformExpr(pstate, ielem->expr);

            /* We have to fix its collations too */
            assign_expr_collations(pstate, ielem->expr);

            /*
             * We check only that the result type is legitimate; this is for
             * consistency with what transformWhereClause() checks for the
             * predicate.  DefineIndex() will make more checks.
             */
            if (expression_returns_set(ielem->expr))
                ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("index expression cannot return a set")));
        }
    }

    /*
     * Check that only the base rel is mentioned.
     */
    if (list_length(pstate->p_rtable) != 1)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("index expressions and predicates can refer only to the table being indexed")));

    free_parsestate(pstate);

    /* Close relation */
    heap_close(rel, NoLock);

    /* check psort index compatible */
    if (0 == pg_strcasecmp(stmt->accessMethod, DEFAULT_CSTORE_INDEX_TYPE)) {
        checkPsortIndexCompatible(stmt);
    }

    /* check psort index compatible */
    if (0 == pg_strcasecmp(stmt->accessMethod, CSTORE_BTREE_INDEX_TYPE)) {
        checkCBtreeIndexCompatible(stmt);
    }

    /* check cgin btree index compatible */
    if (0 == pg_strcasecmp(stmt->accessMethod, CSTORE_GINBTREE_INDEX_TYPE)) {
        checkCGinBtreeIndexCompatible(stmt);
    }

    return stmt;
}

/*
 * transformRuleStmt -
 *	  transform a CREATE RULE Statement. The action is a list of parse
 *	  trees which is transformed into a list of query trees, and we also
 *	  transform the WHERE clause if any.
 *
 * actions and whereClause are output parameters that receive the
 * transformed results.
 *
 * Note that we must not scribble on the passed-in RuleStmt, so we do
 * copyObject() on the actions and WHERE clause.
 */
void transformRuleStmt(RuleStmt* stmt, const char* queryString, List** actions, Node** whereClause)
{
    Relation rel;
    ParseState* pstate = NULL;
    RangeTblEntry* oldrte = NULL;
    RangeTblEntry* newrte = NULL;

    /*
     * To avoid deadlock, make sure the first thing we do is grab
     * AccessExclusiveLock on the target relation.	This will be needed by
     * DefineQueryRewrite(), and we don't want to grab a lesser lock
     * beforehand.
     */
    rel = heap_openrv(stmt->relation, AccessExclusiveLock);

    /* Set up pstate */
    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = queryString;

    /*
     * NOTE: 'OLD' must always have a varno equal to 1 and 'NEW' equal to 2.
     * Set up their RTEs in the main pstate for use in parsing the rule
     * qualification.
     */
    oldrte = addRangeTableEntryForRelation(pstate, rel, makeAlias("old", NIL), false, false);
    newrte = addRangeTableEntryForRelation(pstate, rel, makeAlias("new", NIL), false, false);
    /* Must override addRangeTableEntry's default access-check flags */
    oldrte->requiredPerms = 0;
    newrte->requiredPerms = 0;

    /*
     * They must be in the namespace too for lookup purposes, but only add the
     * one(s) that are relevant for the current kind of rule.  In an UPDATE
     * rule, quals must refer to OLD.field or NEW.field to be unambiguous, but
     * there's no need to be so picky for INSERT & DELETE.  We do not add them
     * to the joinlist.
     */
    switch (stmt->event) {
        case CMD_SELECT:
            addRTEtoQuery(pstate, oldrte, false, true, true);
            break;
        case CMD_UPDATE:
            addRTEtoQuery(pstate, oldrte, false, true, true);
            addRTEtoQuery(pstate, newrte, false, true, true);
            break;
        case CMD_INSERT:
            addRTEtoQuery(pstate, newrte, false, true, true);
            break;
        case CMD_DELETE:
            addRTEtoQuery(pstate, oldrte, false, true, true);
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_CASE_NOT_FOUND),
                    errmodule(MOD_OPT),
                    errmsg("unrecognized event type: %d", (int)stmt->event)));

            break;
    }

    /* take care of the where clause */
    *whereClause = transformWhereClause(pstate, (Node*)copyObject(stmt->whereClause), "WHERE");
    /* we have to fix its collations too */
    assign_expr_collations(pstate, *whereClause);

    if (list_length(pstate->p_rtable) != 2) /* naughty, naughty... */
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("rule WHERE condition cannot contain references to other relations")));

    /* aggregates not allowed (but subselects are okay) */
    if (pstate->p_hasAggs)
        ereport(
            ERROR, (errcode(ERRCODE_GROUPING_ERROR), errmsg("cannot use aggregate function in rule WHERE condition")));
    if (pstate->p_hasWindowFuncs)
        ereport(
            ERROR, (errcode(ERRCODE_WINDOWING_ERROR), errmsg("cannot use window function in rule WHERE condition")));

    /*
     * 'instead nothing' rules with a qualification need a query rangetable so
     * the rewrite handler can add the negated rule qualification to the
     * original query. We create a query with the new command type CMD_NOTHING
     * here that is treated specially by the rewrite system.
     */
    if (stmt->actions == NIL) {
        Query* nothingQry = makeNode(Query);

        nothingQry->commandType = CMD_NOTHING;
        nothingQry->rtable = pstate->p_rtable;
        nothingQry->jointree = makeFromExpr(NIL, NULL); /* no join wanted */

        *actions = list_make1(nothingQry);
    } else {
        ListCell* l = NULL;
        List* newactions = NIL;

        /*
         * transform each statement, like parse_sub_analyze()
         */
        foreach (l, stmt->actions) {
            Node* action = (Node*)lfirst(l);
            ParseState* sub_pstate = make_parsestate(NULL);
            Query *sub_qry = NULL;
            Query *top_subqry = NULL;
            bool hasOld = false;
            bool hasNew = false;

#ifdef PGXC
            if (IsA(action, NotifyStmt))
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                        errmsg("Rule may not use NOTIFY, it is not yet supported")));
#endif
            /*
             * Since outer ParseState isn't parent of inner, have to pass down
             * the query text by hand.
             */
            sub_pstate->p_sourcetext = queryString;

            /*
             * Set up OLD/NEW in the rtable for this statement.  The entries
             * are added only to relnamespace, not varnamespace, because we
             * don't want them to be referred to by unqualified field names
             * nor "*" in the rule actions.  We decide later whether to put
             * them in the joinlist.
             */
            oldrte = addRangeTableEntryForRelation(sub_pstate, rel, makeAlias("old", NIL), false, false);
            newrte = addRangeTableEntryForRelation(sub_pstate, rel, makeAlias("new", NIL), false, false);
            oldrte->requiredPerms = 0;
            newrte->requiredPerms = 0;
            addRTEtoQuery(sub_pstate, oldrte, false, true, false);
            addRTEtoQuery(sub_pstate, newrte, false, true, false);

            /* Transform the rule action statement */
            top_subqry = transformStmt(sub_pstate, (Node*)copyObject(action));
            /*
             * We cannot support utility-statement actions (eg NOTIFY) with
             * nonempty rule WHERE conditions, because there's no way to make
             * the utility action execute conditionally.
             */
            if (top_subqry->commandType == CMD_UTILITY && *whereClause != NULL)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                        errmsg("rules with WHERE conditions can only have SELECT, INSERT, UPDATE, or DELETE actions")));

            /*
             * If the action is INSERT...SELECT, OLD/NEW have been pushed down
             * into the SELECT, and that's what we need to look at. (Ugly
             * kluge ... try to fix this when we redesign querytrees.)
             */
            sub_qry = getInsertSelectQuery(top_subqry, NULL);
            /*
             * If the sub_qry is a setop, we cannot attach any qualifications
             * to it, because the planner won't notice them.  This could
             * perhaps be relaxed someday, but for now, we may as well reject
             * such a rule immediately.
             */
            if (sub_qry->setOperations != NULL && *whereClause != NULL)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

            /*
             * Validate action's use of OLD/NEW, qual too
             */
            hasOld = rangeTableEntry_used((Node*)sub_qry, PRS2_OLD_VARNO, 0) ||
                      rangeTableEntry_used(*whereClause, PRS2_OLD_VARNO, 0);
            hasNew = rangeTableEntry_used((Node*)sub_qry, PRS2_NEW_VARNO, 0) ||
                      rangeTableEntry_used(*whereClause, PRS2_NEW_VARNO, 0);

            switch (stmt->event) {
                case CMD_SELECT:
                    if (hasOld)
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("ON SELECT rule cannot use OLD")));
                    if (hasNew)
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("ON SELECT rule cannot use NEW")));
                    break;
                case CMD_UPDATE:
                    /* both are OK */
                    break;
                case CMD_INSERT:
                    if (hasOld)
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("ON INSERT rule cannot use OLD")));
                    break;
                case CMD_DELETE:
                    if (hasNew)
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("ON DELETE rule cannot use NEW")));
                    break;
                default:
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                            errmodule(MOD_OPT),
                            errmsg("unrecognized event type: %d", (int)stmt->event)));
                    break;
            }

            /*
             * OLD/NEW are not allowed in WITH queries, because they would
             * amount to outer references for the WITH, which we disallow.
             * However, they were already in the outer rangetable when we
             * analyzed the query, so we have to check.
             *
             * Note that in the INSERT...SELECT case, we need to examine the
             * CTE lists of both top_subqry and sub_qry.
             *
             * Note that we aren't digging into the body of the query looking
             * for WITHs in nested sub-SELECTs.  A WITH down there can
             * legitimately refer to OLD/NEW, because it'd be an
             * indirect-correlated outer reference.
             */
            if (rangeTableEntry_used((Node*)top_subqry->cteList, PRS2_OLD_VARNO, 0) ||
                rangeTableEntry_used((Node*)sub_qry->cteList, PRS2_OLD_VARNO, 0))
                ereport(
                    ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot refer to OLD within WITH query")));
            if (rangeTableEntry_used((Node*)top_subqry->cteList, PRS2_NEW_VARNO, 0) ||
                rangeTableEntry_used((Node*)sub_qry->cteList, PRS2_NEW_VARNO, 0))
                ereport(
                    ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot refer to NEW within WITH query")));

            /*
             * For efficiency's sake, add OLD to the rule action's jointree
             * only if it was actually referenced in the statement or qual.
             *
             * For INSERT, NEW is not really a relation (only a reference to
             * the to-be-inserted tuple) and should never be added to the
             * jointree.
             *
             * For UPDATE, we treat NEW as being another kind of reference to
             * OLD, because it represents references to *transformed* tuples
             * of the existing relation.  It would be wrong to enter NEW
             * separately in the jointree, since that would cause a double
             * join of the updated relation.  It's also wrong to fail to make
             * a jointree entry if only NEW and not OLD is mentioned.
             */
            if (hasOld || (hasNew && stmt->event == CMD_UPDATE)) {
                /*
                 * If sub_qry is a setop, manipulating its jointree will do no
                 * good at all, because the jointree is dummy. (This should be
                 * a can't-happen case because of prior tests.)
                 */
                if (sub_qry->setOperations != NULL)
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
                /* hack so we can use addRTEtoQuery() */
                sub_pstate->p_rtable = sub_qry->rtable;
                sub_pstate->p_joinlist = sub_qry->jointree->fromlist;
                addRTEtoQuery(sub_pstate, oldrte, true, false, false);
                sub_qry->jointree->fromlist = sub_pstate->p_joinlist;
            }

            newactions = lappend(newactions, top_subqry);

            free_parsestate(sub_pstate);
        }

        *actions = newactions;
    }

    free_parsestate(pstate);

    /* Close relation, but keep the exclusive lock */
    heap_close(rel, NoLock);
}

/*
 * transformAlterTableStmt -
 *		parse analysis for ALTER TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed AlterTableStmt, but there may be additional actions
 * to be done before and after the actual AlterTable() call.
 */
List* transformAlterTableStmt(Oid relid, AlterTableStmt* stmt, const char* queryString)
{
    Relation rel;
    ParseState* pstate = NULL;
    CreateStmtContext cxt;
    List* result = NIL;
    List* saveAlist = NIL;
    ListCell *lcmd = NULL;
    ListCell *l = NULL;
    List* newcmds = NIL;
    bool skipValidation = true;
    AlterTableCmd* newcmd = NULL;
    Node* rangePartDef = NULL;
    AddPartitionState* addDefState = NULL;
    SplitPartitionState* splitDefState = NULL;
    ListCell* cell = NULL;

    /*
     * We must not scribble on the passed-in AlterTableStmt, so copy it. (This
     * is overkill, but easy.)
     */
    stmt = (AlterTableStmt*)copyObject(stmt);
    /* Caller is responsible for locking the relation */
    rel = relation_open(relid, NoLock);
    if (IS_FOREIGNTABLE(rel)) {
        /*
         * In the security mode, the useft privilege of a user must be
         * checked before the user alters a foreign table.
         */
        if (isSecurityMode && !have_useft_privilege()) {
            ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to alter foreign table in security mode")));
        }
    }

    /* Set up pstate and CreateStmtContext */
    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = queryString;

    cxt.pstate = pstate;
    if (stmt->relkind == OBJECT_FOREIGN_TABLE) {
        cxt.stmtType = ALTER_FOREIGN_TABLE;
    } else {
        cxt.stmtType = ALTER_TABLE;
    }
    cxt.relation = stmt->relation;
    cxt.relation->relpersistence = RelationGetRelPersistence(rel);
    cxt.rel = rel;
    cxt.inhRelations = NIL;
    cxt.isalter = true;
    cxt.hasoids = false; /* need not be right */
    cxt.columns = NIL;
    cxt.ckconstraints = NIL;
    cxt.fkconstraints = NIL;
    cxt.ixconstraints = NIL;
    cxt.clusterConstraints = NIL;
    cxt.inh_indexes = NIL;
    cxt.blist = NIL;
    cxt.alist = NIL;
    cxt.pkey = NULL;
    cxt.ispartitioned = RelationIsPartitioned(rel);

#ifdef PGXC
    cxt.fallback_dist_col = NULL;
    cxt.distributeby = NULL;
    cxt.subcluster = NULL;
#endif
    cxt.node = (Node*)stmt;
    cxt.isResizing = false;
    cxt.bucketOid = InvalidOid;
    cxt.relnodelist = NULL;
    cxt.toastnodelist = NULL;

    if (RelationIsForeignTable(rel)) {
        cxt.canInfomationalConstraint = CAN_BUILD_INFORMATIONAL_CONSTRAINT_BY_RELID(RelationGetRelid(rel));
    } else {
        cxt.canInfomationalConstraint = false;
    }

    /*
     * The only subtypes that currently require parse transformation handling
     * are ADD COLUMN and ADD CONSTRAINT.  These largely re-use code from
     * CREATE TABLE.
     */
    foreach (lcmd, stmt->cmds) {
        AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lcmd);

        elog(ES_LOGLEVEL, "[transformAlterTableStmt] cmd subtype: %d", cmd->subtype);

        switch (cmd->subtype) {
            case AT_AddColumn:
            case AT_AddColumnToView: {
                ColumnDef* def = (ColumnDef*)cmd->def;

                AssertEreport(IsA(def, ColumnDef), MOD_OPT, "");
                transformColumnDefinition(&cxt, def, false);

                /*
                 * If the column has a non-null default, we can't skip
                 * validation of foreign keys.
                 */
                if (def->raw_default != NULL)
                    skipValidation = false;

                /*
                 * All constraints are processed in other ways. Remove the
                 * original list
                 */
                def->constraints = NIL;

                newcmds = lappend(newcmds, cmd);
                break;
            }
            case AT_AddConstraint:

                /*
                 * The original AddConstraint cmd node doesn't go to newcmds
                 */
                if (IsA(cmd->def, Constraint)) {
                    transformTableConstraint(&cxt, (Constraint*)cmd->def);
                    if (((Constraint*)cmd->def)->contype == CONSTR_FOREIGN) {
                        skipValidation = false;
                    }
                } else
                    ereport(ERROR,
                        (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                            errmodule(MOD_OPT),
                            errmsg("unrecognized node type: %d", (int)nodeTag(cmd->def))));
                break;

            case AT_ProcessedConstraint:

                /*
                 * Already-transformed ADD CONSTRAINT, so just make it look
                 * like the standard case.
                 */
                cmd->subtype = AT_AddConstraint;
                newcmds = lappend(newcmds, cmd);
                break;
            case AT_AddPartition:
                /* transform the boundary of range partition,
                 * this step transform it from A_Const into Const */
                addDefState = (AddPartitionState*)cmd->def;
                if (!PointerIsValid(addDefState)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_UNEXPECTED_NULL_VALUE), errmsg("missing definition of adding partition")));
                }
                /* A_Const -->Const */
                foreach (cell, addDefState->partitionList) {
                    rangePartDef = (Node*)lfirst(cell);
                    transformRangePartitionValue(pstate, rangePartDef, true);
                }

                /* transform START/END into LESS/THAN:
                 * Put this part behind the transformRangePartitionValue().
                 */
                if (addDefState->isStartEnd) {
                    List* pos = NIL;
                    int32 partNum;
                    Const* lowBound = NULL;

                    if (!RELATION_IS_PARTITIONED(rel))
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OPERATION),
                                errmodule(MOD_OPT),
                                errmsg("can not add partition against NON-PARTITIONED table")));

                    /* get partition number */
                    partNum = getNumberOfPartitions(rel);
                    if (partNum >= MAX_PARTITION_NUM)
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OPERATION),
                                errmodule(MOD_OPT),
                                errmsg("the current relation have already reached max number of partitions")));

                    /* get partition info */
                    get_rel_partition_info(rel, &pos, &lowBound);

                    /* entry of transform */
                    addDefState->partitionList = transformRangePartStartEndStmt(
                        pstate, addDefState->partitionList, pos, rel->rd_att->attrs, partNum, lowBound, NULL, true);
                }

                newcmds = lappend(newcmds, cmd);
                break;

            case AT_DropPartition:
            case AT_TruncatePartition:
            case AT_ExchangePartition:
                /* transform the boundary of range partition,
                 * this step transform it from A_Const into Const */
                rangePartDef = (Node*)cmd->def;
                if (PointerIsValid(rangePartDef)) {
                    transformRangePartitionValue(pstate, rangePartDef, false);
                }

                newcmds = lappend(newcmds, cmd);
                break;

            case AT_SplitPartition:
                /* transform the boundary of range partition: from A_Const into Const */
                splitDefState = (SplitPartitionState*)cmd->def;
                if (!PointerIsValid(splitDefState->split_point)) {
                    foreach (cell, splitDefState->dest_partition_define_list) {
                        rangePartDef = (Node*)lfirst(cell);
                        transformRangePartitionValue(pstate, rangePartDef, true);
                    }
                }
                if (splitDefState->partition_for_values)
                    splitDefState->partition_for_values =
                        transformRangePartitionValueInternal(pstate, splitDefState->partition_for_values, true, true);

                /* transform the start/end into less/than */
                if (is_start_end_def_list(splitDefState->dest_partition_define_list)) {
                    List* pos = NIL;
                    int32 partNum;
                    Const* lowBound = NULL;
                    Const* upBound = NULL;
                    Oid srcPartOid = InvalidOid;

                    if (!RELATION_IS_PARTITIONED(rel))
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                                errmodule(MOD_OPT),
                                errmsg("can not split partition against NON-PARTITIONED table")));

                    /* get partition number */
                    partNum = getNumberOfPartitions(rel);

                    /* get partition info */
                    get_rel_partition_info(rel, &pos, NULL);

                    /* get source partition bound */
                    srcPartOid = get_split_partition_oid(rel, splitDefState);
                    if (!OidIsValid(srcPartOid)) {
                        ereport(ERROR,
                            (errcode(ERRCODE_UNDEFINED_TABLE),
                                errmsg("split partition \"%s\" does not exist.", splitDefState->src_partition_name)));
                    }
                    get_src_partition_bound(rel, srcPartOid, &lowBound, &upBound);

                    /* entry of transform */
                    splitDefState->dest_partition_define_list = transformRangePartStartEndStmt(pstate,
                        splitDefState->dest_partition_define_list,
                        pos,
                        rel->rd_att->attrs,
                        partNum - 1,
                        lowBound,
                        upBound,
                        true);
                }

                newcmds = lappend(newcmds, cmd);
                break;

            default:
                newcmds = lappend(newcmds, cmd);
                break;
        }
    }

    /*
     * transformIndexConstraints wants cxt.alist to contain only index
     * statements, so transfer anything we already have into save_alist
     * immediately.
     */
    saveAlist = cxt.alist;
    cxt.alist = NIL;

    /* Postprocess index and FK constraints */
    transformIndexConstraints(&cxt);

    transformFKConstraints(&cxt, skipValidation, true);

    /*
     * Check partial cluster key constraints
     */
    checkClusterConstraints(&cxt);

    /*
     * Check reserve column
     */
    checkReserveColumn(&cxt);

    if (stmt->relkind == OBJECT_FOREIGN_TABLE && cxt.alist != NIL) {
        Oid relationId;
        relationId = RelationGetRelid(rel);

        if (isMOTFromTblOid(relationId) || CAN_BUILD_INFORMATIONAL_CONSTRAINT_BY_RELID(relationId)) {
            setInternalFlagIndexStmt(cxt.alist);
        }
    }

    /*
     * Push any index-creation commands into the ALTER, so that they can be
     * scheduled nicely by tablecmds.c.  Note that tablecmds.c assumes that
     * the IndexStmt attached to an AT_AddIndex or AT_AddIndexConstraint
     * subcommand has already been through transformIndexStmt.
     */
    foreach (l, cxt.alist) {
        IndexStmt* idxstmt = (IndexStmt*)lfirst(l);
        AssertEreport(IsA(idxstmt, IndexStmt), MOD_OPT, "");
        idxstmt = transformIndexStmt(relid, idxstmt, queryString);
        newcmd = makeNode(AlterTableCmd);
        newcmd->subtype = OidIsValid(idxstmt->indexOid) ? AT_AddIndexConstraint : AT_AddIndex;
        newcmd->def = (Node*)idxstmt;
        newcmds = lappend(newcmds, newcmd);
    }
    cxt.alist = NIL;

    /* Append any CHECK or FK constraints to the commands list */
    foreach (l, cxt.ckconstraints) {
        newcmd = makeNode(AlterTableCmd);
        newcmd->subtype = AT_AddConstraint;
        newcmd->def = (Node*)lfirst(l);
        newcmds = lappend(newcmds, newcmd);
    }
    foreach (l, cxt.fkconstraints) {
        newcmd = makeNode(AlterTableCmd);
        newcmd->subtype = AT_AddConstraint;
        newcmd->def = (Node*)lfirst(l);
        newcmds = lappend(newcmds, newcmd);
    }

    foreach (l, cxt.clusterConstraints) {
        newcmd = makeNode(AlterTableCmd);
        newcmd->subtype = AT_AddConstraint;
        newcmd->def = (Node*)lfirst(l);
        newcmds = lappend(newcmds, newcmd);
    }
    /* Close rel */
    relation_close(rel, NoLock);

    /*
     * Output results.
     */
    stmt->cmds = newcmds;

    result = lappend(cxt.blist, stmt);
    result = list_concat(result, cxt.alist);
    result = list_concat(result, saveAlist);

    return result;
}

/*
 * Preprocess a list of column constraint clauses
 * to attach constraint attributes to their primary constraint nodes
 * and detect inconsistent/misplaced constraint attributes.
 *
 * NOTE: currently, attributes are only supported for FOREIGN KEY, UNIQUE,
 * EXCLUSION, and PRIMARY KEY constraints, but someday they ought to be
 * supported for other constraint types.
 */
static void transformConstraintAttrs(CreateStmtContext* cxt, List* constraintList)
{
    Constraint* lastprimarycon = NULL;
    bool sawDeferrability = false;
    bool sawInitially = false;
    ListCell* clist = NULL;

#define SUPPORTS_ATTRS(node)                                                                     \
    ((node) != NULL && ((node)->contype == CONSTR_PRIMARY || (node)->contype == CONSTR_UNIQUE || \
                           (node)->contype == CONSTR_EXCLUSION || (node)->contype == CONSTR_FOREIGN))

    foreach (clist, constraintList) {
        Constraint* con = (Constraint*)lfirst(clist);

        if (!IsA(con, Constraint))
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE), errmsg("unrecognized node type: %d", (int)nodeTag(con))));
        switch (con->contype) {
            case CONSTR_ATTR_DEFERRABLE:
                if (!SUPPORTS_ATTRS(lastprimarycon))
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("misplaced DEFERRABLE clause"),
                            parser_errposition(cxt->pstate, con->location)));
                if (sawDeferrability)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
                            parser_errposition(cxt->pstate, con->location)));
                sawDeferrability = true;
                lastprimarycon->deferrable = true;
                break;

            case CONSTR_ATTR_NOT_DEFERRABLE:
                if (!SUPPORTS_ATTRS(lastprimarycon))
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("misplaced NOT DEFERRABLE clause"),
                            parser_errposition(cxt->pstate, con->location)));
                if (sawDeferrability)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
                            parser_errposition(cxt->pstate, con->location)));
                sawDeferrability = true;
                lastprimarycon->deferrable = false;
                if (sawInitially && lastprimarycon->initdeferred)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
                            parser_errposition(cxt->pstate, con->location)));
                break;

            case CONSTR_ATTR_DEFERRED:
                if (!SUPPORTS_ATTRS(lastprimarycon))
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("misplaced INITIALLY DEFERRED clause"),
                            parser_errposition(cxt->pstate, con->location)));
                if (sawInitially)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
                            parser_errposition(cxt->pstate, con->location)));
                sawInitially = true;
                lastprimarycon->initdeferred = true;

                /*
                 * If only INITIALLY DEFERRED appears, assume DEFERRABLE
                 */
                if (!sawDeferrability)
                    lastprimarycon->deferrable = true;
                else if (!lastprimarycon->deferrable)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
                            parser_errposition(cxt->pstate, con->location)));
                break;

            case CONSTR_ATTR_IMMEDIATE:
                if (!SUPPORTS_ATTRS(lastprimarycon))
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("misplaced INITIALLY IMMEDIATE clause"),
                            parser_errposition(cxt->pstate, con->location)));
                if (sawInitially)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
                            parser_errposition(cxt->pstate, con->location)));
                sawInitially = true;
                lastprimarycon->initdeferred = false;
                break;

            default:
                /* Otherwise it's not an attribute */
                lastprimarycon = con;
                /* reset flags for new primary node */
                sawDeferrability = false;
                sawInitially = false;
                break;
        }
    }
}

/*
 * Special handling of type definition for a column
 */
static void transformColumnType(CreateStmtContext* cxt, ColumnDef* column)
{
    /*
     * All we really need to do here is verify that the type is valid,
     * including any collation spec that might be present.
     */
    Type ctype = typenameType(cxt->pstate, column->typname, NULL);

    if (column->collClause) {
        Form_pg_type typtup = (Form_pg_type)GETSTRUCT(ctype);

        LookupCollation(cxt->pstate, column->collClause->collname, column->collClause->location);
        /* Complain if COLLATE is applied to an uncollatable type */
        if (!OidIsValid(typtup->typcollation))
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("collations are not supported by type %s", format_type_be(HeapTupleGetOid(ctype))),
                    parser_errposition(cxt->pstate, column->collClause->location)));
    }

    ReleaseSysCache(ctype);
}

/*
 * transformCreateSchemaStmt -
 *	  analyzes the CREATE SCHEMA statement
 *
 * Split the schema element list into individual commands and place
 * them in the result list in an order such that there are no forward
 * references (e.g. GRANT to a table created later in the list). Note
 * that the logic we use for determining forward references is
 * presently quite incomplete.
 *
 * SQL92 also allows constraints to make forward references, so thumb through
 * the table columns and move forward references to a posterior alter-table
 * command.
 *
 * The result is a list of parse nodes that still need to be analyzed ---
 * but we can't analyze the later commands until we've executed the earlier
 * ones, because of possible inter-object references.
 *
 * Note: this breaks the rules a little bit by modifying schema-name fields
 * within passed-in structs.  However, the transformation would be the same
 * if done over, so it should be all right to scribble on the input to this
 * extent.
 */
List* transformCreateSchemaStmt(CreateSchemaStmt* stmt)
{
    CreateSchemaStmtContext cxt;
    List* result = NIL;
    ListCell* elements = NULL;

    cxt.stmtType = "CREATE SCHEMA";
    cxt.schemaname = stmt->schemaname;
    cxt.authid = stmt->authid;
    cxt.sequences = NIL;
    cxt.tables = NIL;
    cxt.views = NIL;
    cxt.indexes = NIL;
    cxt.triggers = NIL;
    cxt.grants = NIL;

    /*
     * Run through each schema element in the schema element list. Separate
     * statements by type, and do preliminary analysis.
     */
    foreach (elements, stmt->schemaElts) {
        Node* element = (Node*)lfirst(elements);

        switch (nodeTag(element)) {
            case T_CreateSeqStmt: {
                CreateSeqStmt* elp = (CreateSeqStmt*)element;

                setSchemaName(cxt.schemaname, &elp->sequence->schemaname);
                cxt.sequences = lappend(cxt.sequences, element);
            } break;

            case T_CreateStmt: {
                CreateStmt* elp = (CreateStmt*)element;

                setSchemaName(cxt.schemaname, &elp->relation->schemaname);

                cxt.tables = lappend(cxt.tables, element);
            } break;

            case T_ViewStmt: {
                ViewStmt* elp = (ViewStmt*)element;

                setSchemaName(cxt.schemaname, &elp->view->schemaname);

                cxt.views = lappend(cxt.views, element);
            } break;

            case T_IndexStmt: {
                IndexStmt* elp = (IndexStmt*)element;

                setSchemaName(cxt.schemaname, &elp->relation->schemaname);
                cxt.indexes = lappend(cxt.indexes, element);
            } break;

            case T_CreateTrigStmt: {
                CreateTrigStmt* elp = (CreateTrigStmt*)element;

                setSchemaName(cxt.schemaname, &elp->relation->schemaname);
                cxt.triggers = lappend(cxt.triggers, element);
            } break;

            case T_GrantStmt:
                cxt.grants = lappend(cxt.grants, element);
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized node type: %d", (int)nodeTag(element))));
        }
    }

    result = NIL;
    result = list_concat(result, cxt.sequences);
    result = list_concat(result, cxt.tables);
    result = list_concat(result, cxt.views);
    result = list_concat(result, cxt.indexes);
    result = list_concat(result, cxt.triggers);
    result = list_concat(result, cxt.grants);

    return result;
}

/*
 * setSchemaName
 *		Set or check schema name in an element of a CREATE SCHEMA command
 */
static void setSchemaName(char* context_schema, char** stmt_schema_name)
{
    if (*stmt_schema_name == NULL)
        *stmt_schema_name = context_schema;
    else if (strcmp(context_schema, *stmt_schema_name) != 0)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_SCHEMA_DEFINITION),
                errmsg("CREATE specifies a schema (%s) "
                       "different from the one being created (%s)",
                    *stmt_schema_name,
                    context_schema)));
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check synax for range partition defination
 * Description	:
 * Notes		:
 */
void checkPartitionSynax(CreateStmt* stmt)
{
    ListCell* cell = NULL;
    bool value_partition = false;

    /* unsupport inherits clause */
    if (stmt->inhRelations) {
        if (stmt->partTableState) {
            ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("unsupport inherits clause for partitioned table")));
        } else {
            foreach (cell, stmt->inhRelations) {
                RangeVar* inh = (RangeVar*)lfirst(cell);
                Relation rel;

                AssertEreport(IsA(inh, RangeVar), MOD_OPT, "");
                rel = heap_openrv(inh, AccessShareLock);
                /* @hdfs
                 * Deal with error mgs for foreign table, the foreign table
                 * is not inherited
                 */
                if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE) {
                    ereport(ERROR,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("inherited relation \"%s\" is a foreign table", inh->relname),
                            errdetail("can not inherit from a foreign table")));
                } else if (rel->rd_rel->relkind != RELKIND_RELATION) {
                    ereport(ERROR,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("inherited relation \"%s\" is not a table", inh->relname)));
                }
                if (RELATION_IS_PARTITIONED(rel)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                            errmodule(MOD_OPT),
                            errmsg("inherited relation \"%s\" is a partitioned table", inh->relname),
                            errdetail("can not inherit from partitioned table")));
                }
                heap_close(rel, NoLock);
            }
        }
    }
    /* is it a partitoned table? */
    if (!stmt->partTableState) {
        return;
    }

    /* check syntax for value-partitioned table */
    if (stmt->partTableState->partitionStrategy == PART_STRATEGY_VALUE) {
        value_partition = true;

        /* do partition-key null check as part of sytax check */
        if (list_length(stmt->partTableState->partitionKey) == 0) {
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("Value-based partition table should have one column at least")));
        }

        /*
         * for value partitioned table we only do a simple sanity check to
         * ensure that any uncessary fileds are set with NULL
         */
        if (stmt->partTableState->intervalPartDef || stmt->partTableState->partitionList) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPERATION),
                    errmsg("Value-Based partition table creation encounters unexpected data in unnecessary fields"),
                    errdetail("save context and get assistance from DB Dev team")));
        }
    }

    /* unsupport om commit clause */
    if (stmt->oncommit) {
        ereport(
            ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("ON COMMIT option is not supported for partitioned table")));
    }

    /* unsupport typed table */
    if (stmt->ofTypename) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("Typed table can't not be partitioned")));
    }

    /* unsupport typed table */
    if (stmt->relation->relpersistence != RELPERSISTENCE_PERMANENT) {
        ereport(ERROR,
            (errcode(ERRCODE_SYNTAX_ERROR),
                errmsg("unsupported feature with temporary/unlogged table for partitioned table")));
    }

    /* unsupport oids option */
    foreach (cell, stmt->options) {
        DefElem* def = (DefElem*)lfirst(cell);

        if (!def->defnamespace && !pg_strcasecmp(def->defname, "oids")) {
            ereport(
                ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("OIDS option is not supported for partitioned table")));
        }
    }

    /* check partition key number for none value-partition table */
    if (!value_partition && stmt->partTableState->partitionKey->length > MAX_PARTITIONKEY_NUM) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("too many partition keys for partitioned table"),
                errhint("Partittion key columns can not be more than %d", MAX_PARTITIONKEY_NUM)));
    }

    /* check range partition number for none value-partition table */
    if (!value_partition && stmt->partTableState->partitionList->length > MAX_PARTITION_NUM) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("too many partitions for partitioned table"),
                errhint("Number of partitions can not be more than %d", MAX_PARTITION_NUM)));
    }

    /* check interval synax */
    if (stmt->partTableState->intervalPartDef) {
        if (stmt->partTableState->partitionKey->length > 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("Range partitioned table with INTERVAL clause has more than one column"),
                    errhint("Only support one partition key for interval partition")));
        }
        if (!IsA(stmt->partTableState->intervalPartDef->partInterval, A_Const) ||
            ((A_Const*)stmt->partTableState->intervalPartDef->partInterval)->val.type != T_String) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                    // errmsg("invalid input syntax for type %s: \"%s\"", datatype, str)));
                    errmsg("invalid input syntax for type interval")));
        }
        int32 typmod = -1;
        Interval* interval = NULL;
        A_Const* node = (A_Const*)stmt->partTableState->intervalPartDef->partInterval;
        interval = char_to_interval(node->val.val.str, typmod);
        pfree(interval);
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check partition value
 * Description	:
 * Notes		: partition key value must be const or const-evaluable expression
 */
static void checkPartitionValue(CreateStmtContext* cxt, CreateStmt* stmt)
{
    PartitionState* partdef = NULL;
    ListCell* cell = NULL;

    partdef = stmt->partTableState;
    if (partdef == NULL) {
        return;
    }
    /* transform expression in partition definition and evaluate the expression */
    foreach (cell, partdef->partitionList) {
        Node* state = (Node*)lfirst(cell);
        transformRangePartitionValue(cxt->pstate, state, true);
    }
}

/*
 * check_partition_name_less_than
 *  check partition name with less/than stmt.
 *
 * [IN] partitionList: partition list
 *
 * RETURN: void
 */
static void check_partition_name_less_than(List* partitionList)
{
    ListCell* cell = NULL;
    ListCell* lc = NULL;
    char* curPartname = NULL;
    char* refPartname = NULL;

    foreach (cell, partitionList) {
        lc = cell;
        refPartname = ((RangePartitionDefState*)lfirst(cell))->partitionName;
        while ((lc = lnext(lc)) != NULL) {
            curPartname = ((RangePartitionDefState*)lfirst(lc))->partitionName;

            if (!strcmp(refPartname, curPartname)) {
                ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("duplicate partition name: \"%s\"", refPartname)));
            }
        }
    }
}

/*
 * check_partition_name_start_end
 *  check partition name with start/end stmt.
 *
 * [IN] partitionList: partition list
 *
 * RETURN: void
 */
static void check_partition_name_start_end(List* partitionList)
{
    ListCell* cell = NULL;
    ListCell* lc = NULL;
    RangePartitionStartEndDefState* defState = NULL;
    RangePartitionStartEndDefState* lastState = NULL;

    foreach (cell, partitionList) {
        lc = cell;
        lastState = (RangePartitionStartEndDefState*)lfirst(cell);

        while ((lc = lnext(lc)) != NULL) {
            defState = (RangePartitionStartEndDefState*)lfirst(lc);
            if (!strcmp(lastState->partitionName, defState->partitionName))
                ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT),
                        errmsg("duplicate partition name: \"%s\"", defState->partitionName)));
        }
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check partition name
 * Description	: duplicate partition name is not allowed
 * Notes		:
 */
void checkPartitionName(List* partitionList)
{
    ListCell* cell = NULL;

    cell = list_head(partitionList);
    if (cell != NULL) {
        Node* state = (Node*)lfirst(cell);

        if (IsA(state, RangePartitionDefState))
            check_partition_name_less_than(partitionList);
        else
            check_partition_name_start_end(partitionList);
    }
}

/*
 * Check partial cluster key constraints
 */
static void checkClusterConstraints(CreateStmtContext* cxt)
{
    AssertEreport(cxt != NULL, MOD_OPT, "");

    if (cxt->clusterConstraints == NIL) {
        return;
    }

    ListCell* lc = NULL;
    ListCell* lc1 = NULL;
    ListCell* lc2 = NULL;

    foreach (lc, cxt->clusterConstraints) {
        Constraint* constraint = (Constraint*)lfirst(lc);

        // for each keys find out whether have same key
        foreach (lc1, constraint->keys) {
            char* key1 = strVal(lfirst(lc1));
            lc2 = lnext(lc1);

            for (; lc2 != NULL; lc2 = lnext(lc2)) {
                char* key2 = strVal(lfirst(lc2));
                if (0 == strcasecmp(key1, key2)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_COLUMN),
                            errmsg("column \"%s\" appears twice in partial cluster key constraint", key1),
                            parser_errposition(cxt->pstate, constraint->location)));
                }
            }
        }
    }
}

/*
 * Check reserve column
 */
static void checkReserveColumn(CreateStmtContext* cxt)
{
    AssertEreport(cxt != NULL, MOD_OPT, "");

    if (cxt->columns == NIL) {
        return;
    }

    List* columns = cxt->columns;
    ListCell* lc = NULL;

    foreach (lc, columns) {
        ColumnDef* col = (ColumnDef*)lfirst(lc);
        AssertEreport(col != NULL, MOD_OPT, "");

        if (CHCHK_PSORT_RESERVE_COLUMN(col->colname)) {
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_COLUMN),
                    errmsg("column name \"%s\" conflicts with a system column name", col->colname)));
        }
    }
}

static void checkPsortIndexCompatible(IndexStmt* stmt)
{
    if (stmt->whereClause) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("access method \"psort\" does not support WHERE clause")));
    }

    /* psort index can not support index expressions */
    ListCell* lc = NULL;
    foreach (lc, stmt->indexParams) {
        IndexElem* ielem = (IndexElem*)lfirst(lc);

        if (ielem->expr) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"psort\" does not support index expressions")));
        }
    }
}

static void checkCBtreeIndexCompatible(IndexStmt* stmt)
{
    if (stmt->whereClause) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("access method \"cbtree\" does not support WHERE clause")));
    }

    /* psort index can not support index expressions */
    ListCell* lc = NULL;
    foreach (lc, stmt->indexParams) {
        IndexElem* ielem = (IndexElem*)lfirst(lc);

        if (ielem->expr) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"cbtree\" does not support index expressions")));
        }
    }
}

static void checkCGinBtreeIndexCompatible(IndexStmt* stmt)
{
    Assert(stmt);

    if (stmt->whereClause) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("access method \"cgin\" does not support WHERE clause")));
    }

    /* cgin index can not support null text search parser */
    ListCell* l = NULL;
    foreach (l, stmt->indexParams) {
        IndexElem* ielem = (IndexElem*)lfirst(l);
        Node* expr = ielem->expr;

        if (expr != NULL) {
            Assert(IsA(expr, FuncExpr));
            if (IsA(expr, FuncExpr)) {
                FuncExpr* funcexpr = (FuncExpr*)expr;
                Node* firstarg = (Node*)lfirst(funcexpr->args->head);

                if (IsA(firstarg, Const)) {
                    Const* constarg = (Const*)firstarg;
                    if (constarg->constisnull)
                        ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("access method \"cgin\" does not support null text search parser")));
                }
            }
        }
    }
}

void transformRangePartitionValue(ParseState* pstate, Node* rangePartDef, bool needCheck)
{
    Assert(rangePartDef); /* never null */

    switch (rangePartDef->type) {
        case T_RangePartitionDefState: {
            RangePartitionDefState* state = (RangePartitionDefState*)rangePartDef;
            /* only one boundary need transform */
            state->boundary = transformRangePartitionValueInternal(pstate, state->boundary, needCheck, true);
            break;
        }
        case T_RangePartitionStartEndDefState: {
            RangePartitionStartEndDefState* state = (RangePartitionStartEndDefState*)rangePartDef;

            /* transform each point, null-case is also covered */
            state->startValue = transformRangePartitionValueInternal(pstate, state->startValue, needCheck, true);
            state->endValue = transformRangePartitionValueInternal(pstate, state->endValue, needCheck, true);
            state->everyValue = transformRangePartitionValueInternal(pstate, state->everyValue, needCheck, true);
            break;
        }
        default:
            Assert(false); /* never happen */
    }
}

List* transformRangePartitionValueInternal(ParseState* pstate, List* boundary, bool needCheck, bool needFree)
{
    List* newMaxValueList = NIL;
    ListCell* valueCell = NULL;
    Node* maxElem = NULL;
    Node* result = NULL;

    /* scan max value of partition key of per partition */
    foreach (valueCell, boundary) {
        maxElem = (Node*)lfirst(valueCell);
        result = transformIntoConst(pstate, maxElem);
        if (PointerIsValid(result) && needCheck && ((Const*)result)->constisnull && !((Const*)result)->ismaxvalue) {
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("Partition key value can not be null"),
                    errdetail("partition bound element must be one of: string, datetime or interval literal, number, "
                              "or MAXVALUE, and not null")));
        }
        newMaxValueList = lappend(newMaxValueList, result);
    }

    if (needFree && boundary != NIL)
        list_free_ext(boundary); /* avoid mem leak */

    return newMaxValueList;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Input		:
 * Output	:
 * Return		:
 * Notes		:
 */
Node* transformIntoConst(ParseState* pstate, Node* maxElem)
{
    Node* result = NULL;
    FuncExpr* funcexpr = NULL;
    /* transform expression first */
    maxElem = transformExpr(pstate, maxElem);

    /* then, evaluate expression */
    switch (nodeTag(maxElem)) {
        case T_Const:
            result = maxElem;
            break;
        /* MaxValue for Date must be a function expression(to_date) */
        case T_FuncExpr: {
            funcexpr = (FuncExpr*)maxElem;
            result = (Node*)evaluate_expr(
                (Expr*)funcexpr, exprType((Node*)funcexpr), exprTypmod((Node*)funcexpr), funcexpr->funccollid);
            /*
             * if the function expression cannot be evaluated and output a const,
             * than report error
             */
            if (T_Const != nodeTag((Node*)result)) {
                ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("partition key value must be const or const-evaluable expression")));
            }
        } break;
        default: {
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("partition key value must be const or const-evaluable expression")));
        } break;
    }
    return result;
}
/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
Oid generateClonedIndex(Relation source_idx, Relation source_relation, char* tempIndexName, Oid targetTblspcOid,
    bool skip_build, bool partitionedIndex)
{
    CreateStmtContext cxt;
    IndexStmt* index_stmt = NULL;
    AttrNumber* attmap = NULL;
    int attmap_length, i;
    Oid heap_relid;
    Relation heapRel;
    TupleDesc tupleDesc;
    Oid sourceRelid = RelationGetRelid(source_idx);
    Oid ret;

    /* get the relation that the index is created on */
    heap_relid = IndexGetRelation(sourceRelid, false);
    heapRel = relation_open(heap_relid, AccessShareLock);

    /* create cxt.relation */
    cxt.relation = makeRangeVar(
        get_namespace_name(RelationGetNamespace(source_relation), true), RelationGetRelationName(source_relation), -1);

    /* initialize attribute array */
    tupleDesc = RelationGetDescr(heapRel);
    attmap_length = tupleDesc->natts;
    attmap = (AttrNumber*)palloc0(sizeof(AttrNumber) * attmap_length);
    for (i = 0; i < attmap_length; i++)
        attmap[i] = i + 1;

    /* generate an index statement */
    index_stmt = generateClonedIndexStmt(&cxt, source_idx, attmap, attmap_length, NULL);

    if (tempIndexName != NULL)
        index_stmt->idxname = tempIndexName;

    if (OidIsValid(targetTblspcOid)) {
        /* generateClonedIndexStmt() maybe set tablespace name, so free it first. */
        if (index_stmt->tableSpace) {
            pfree_ext(index_stmt->tableSpace);
        }
        /* set target tablespace's name into index_stmt */
        index_stmt->tableSpace = get_tablespace_name(targetTblspcOid);
    }

    /* set is partitioned field */
    index_stmt->isPartitioned = partitionedIndex;

    /* don't do mem check, since there's no distribution info for new added temp table */
    index_stmt->skip_mem_check = true;

    /* Run parse analysis ... */
    index_stmt = transformIndexStmt(RelationGetRelid(source_relation), index_stmt, NULL);

    /* ... and do it */
    WaitState oldStatus = pgstat_report_waitstatus(STATE_CREATE_INDEX);
    ret = DefineIndex(RelationGetRelid(source_relation),
        index_stmt,
        InvalidOid, /* no predefined OID */
        false,      /* is_alter_table */
        true,       /* check_rights */
        skip_build, /* skip_build */
        false);     /* quiet */
    (void)pgstat_report_waitstatus(oldStatus);

    /* clean up */
    pfree_ext(attmap);
    relation_close(heapRel, AccessShareLock);

    return ret;
}

/*
 * @hdfs
 * Brief        : set informational constraint flag in IndexStmt.
 * Description  : Set indexStmt's internal_flag. This flag will be set to false
 *                if indexStmt is built by "Creat index", otherwise be set to true.
 * Input        : the IndexStmt list.
 * Output       : none.
 * Return Value : none.
 * Notes        : This function is only used for HDFS foreign table.
 */
static void setInternalFlagIndexStmt(List* IndexList)
{
    ListCell* lc = NULL;

    Assert(IndexList != NIL);
    foreach (lc, IndexList) {
        IndexStmt* index = NULL;
        index = (IndexStmt*)lfirst(lc);
        index->internal_flag = true;
    }
}

/*
 * Brief        : Check the foreign table constraint type.
 * Description  : This function checks HDFS foreign table constraint type. The supported constraint
 *                types and some useful comment are:
 *                1. Only the primary key, unique, not null and null will be supported.
 *                2. Only "NOT ENFORCED" clause is supported for HDFS foreign table informational constraint.
 *                3. Multi-column combined informational constraint is forbidden.
 * Input        : node, the node needs to be checked.
 * Output       : none.
 * Return Value : none.
 * Notes        : none.
 */
void checkInformationalConstraint(Node* node, bool isForeignTbl)
{
    if (node == NULL) {
        return;
    }

    Constraint* constr = (Constraint*)node;

    /* Common table unsupport not force Constraint. */
    if (!isForeignTbl) {
        if (constr->inforConstraint && constr->inforConstraint->nonforced) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("It is not allowed to support \"NOT ENFORCED\" informational constraint.")));
        }
        return;
    }

    if (constr->contype == CONSTR_NULL || constr->contype == CONSTR_NOTNULL) {
        return;
    } else if (constr->contype == CONSTR_PRIMARY || constr->contype == CONSTR_UNIQUE) {
        /* HDFS foreign table only support not enforced informational primary key and unique Constraint. */
        if (constr->inforConstraint == NULL || !constr->inforConstraint->nonforced) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("The foreign table only support \"NOT ENFORCED\" informational constraint.")));
        }
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Only the primary key, unique, not null and null be supported.")));
    }

    if (constr->keys != NIL && list_length(constr->keys) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Multi-column combined informational constraint is forbidden.")));
    }
}

/*
 * @Description: Check Constraint.
 * @in cxt: CreateStmtContext or AlterTableStmt struct.
 * @in node: Constraint or ColumnDef.
 */
static void checkConstraint(CreateStmtContext* cxt, Node* node)
{
    bool canBuildInfoConstraint = cxt->canInfomationalConstraint;

    /* Judge constraint is valid. */
    if (IsA(node, Constraint)) {
        checkInformationalConstraint(node, canBuildInfoConstraint);
    } else if (IsA(node, ColumnDef)) {
        List* constList = ((ColumnDef*)node)->constraints;
        ListCell* cell = NULL;

        foreach (cell, constList) {
            Node* element = (Node*)lfirst(cell);

            if (IsA(element, Constraint)) {
                checkInformationalConstraint(element, canBuildInfoConstraint);
            }
        }
    }
}

/*
 * @Description: set skip mem check flag for index stmt. If the
 *	index is created just after table creation, we will not do
 *	memory check and adaption
 * @in IndexList: index list after table creation
 */
static void setMemCheckFlagForIdx(List* IndexList)
{
    ListCell* lc = NULL;

    Assert(IndexList != NIL);
    foreach (lc, IndexList) {
        IndexStmt* index = NULL;
        index = (IndexStmt*)lfirst(lc);
        index->skip_mem_check = true;
    }
}

/*
 * add_range_partition_def_state
 * 	add one partition def state into a List
 *
 * [IN] xL: List to be appended
 * [IN] boundary: a list of the end point (list_length must be 1)
 * [IN] partName: partition name
 * [IN] tblSpaceName: tablespace name
 *
 * RETURN: the partitionDefState List
 */
static List* add_range_partition_def_state(List* xL, List* boundary, char* partName, const char* tblSpaceName)
{
    RangePartitionDefState* addState = makeNode(RangePartitionDefState);
    addState->boundary = boundary;
    addState->partitionName = pstrdup(partName);
    addState->tablespacename = pstrdup(tblSpaceName);
    addState->curStartVal = NULL;
    addState->partitionInitName = NULL;

    return lappend(xL, addState);
}

/*
 * get_range_partition_name_prefix
 * 	get partition name's prefix
 *
 * [out] namePrefix: an array of length NAMEDATALEN to store name prefix
 * [IN] srcName: src name
 * [IN] printNotice: print notice or not, default false
 *
 * RETURN: void
 */
void get_range_partition_name_prefix(char* namePrefix, char* srcName, bool printNotice)
{
    errno_t ret = EOK;
    int len;

    /* namePrefix is an array of length NAMEDATALEN, so it's safe to store string */
    Assert(namePrefix && srcName);
    ret = sprintf_s(namePrefix, NAMEDATALEN, "%s", srcName);
    securec_check_ss(ret, "\0", "\0");

    len = strlen(srcName);
    if (len > LEN_PARTITION_PREFIX) {
        int k = pg_mbcliplen(namePrefix, len, LEN_PARTITION_PREFIX);
        namePrefix[k] = '\0';
        if (printNotice)
            ereport(NOTICE,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("Partition name's prefix \"%s\" will be truncated to \"%s\"", srcName, namePrefix)));
    }
}

/* get_rel_partition_info
 * 	get detail info of a partition rel
 *
 * [IN] partTableRel: partition relation
 * [OUT] pos: position of the partition key
 * [OUT] upBound: up boundary of last partition
 *
 * RETURN: void
 */
static void get_rel_partition_info(Relation partTableRel, List** pos, Const** upBound)
{
    RangePartitionMap* partMap = NULL;
    int2vector* partitionKey = NULL;
    int partKeyNum;

    if (!RELATION_IS_PARTITIONED(partTableRel)) {
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("CAN NOT get detail info from a NON-PARTITIONED relation.")));
    }

    if (pos == NULL && upBound == NULL)
        return; /* nothing to do */

    partMap = (RangePartitionMap*)partTableRel->partMap;
    partitionKey = partMap->partitionKey;
    partKeyNum = partMap->partitionKey->dim1;

    /* get position of the partition key */
    if (pos != NULL) {
        List* m_pos = NULL;
        for (int i = 0; i < partKeyNum; i++)
            m_pos = lappend_int(m_pos, partitionKey->values[i] - 1);

        *pos = m_pos;
    }

    /* get up boundary of the last partition */
    if (upBound != NULL) {
        int partNum = getNumberOfPartitions(partTableRel);
        *upBound = (Const*)copyObject(partMap->rangeElements[partNum - 1].boundary[0]);
    }
}

/* get_src_partition_bound
 * 	get detail info of a partition rel
 *
 * [IN] partTableRel: partition relation
 * [IN] srcPartOid: src partition oid
 * [OUT] lowBound: low boundary of the src partition
 * [OUT] upBound: up boundary of the src partition
 *
 * RETURN: void
 */
static void get_src_partition_bound(Relation partTableRel, Oid srcPartOid, Const** lowBound, Const** upBound)
{
    RangePartitionMap* partMap = NULL;
    int srcPartSeq;

    if (!RELATION_IS_PARTITIONED(partTableRel)) {
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("CAN NOT get detail info from a NON-PARTITIONED relation.")));
    }

    if (lowBound == NULL && upBound == NULL)
        return; /* nothing to do */

    if (srcPartOid == InvalidOid)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("CAN NOT get detail info from a partitioned relation WITHOUT specified partition.")));

    partMap = (RangePartitionMap*)partTableRel->partMap;

    srcPartSeq = partOidGetPartSequence(partTableRel, srcPartOid) - 1;
    if (lowBound != NULL) {
        if (srcPartSeq > 0)
            *lowBound = (Const*)copyObject(partMap->rangeElements[srcPartSeq - 1].boundary[0]);
        else
            *lowBound = NULL;
    }

    if (upBound != NULL)
        *upBound = (Const*)copyObject(partMap->rangeElements[srcPartSeq].boundary[0]);
}

/* get_split_partition_oid
 * 	get oid of the split partition
 *
 * [IN] partTableRel: partition relation
 * [IN] splitState: split partition state
 *
 * RETURN: oid of the partition to be splitted
 */
static Oid get_split_partition_oid(Relation partTableRel, SplitPartitionState* splitState)
{
    RangePartitionMap* partMap = NULL;
    Oid srcPartOid = InvalidOid;

    if (!RELATION_IS_PARTITIONED(partTableRel)) {
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("CAN NOT get partition oid from a NON-PARTITIONED relation.")));
    }

    partMap = (RangePartitionMap*)partTableRel->partMap;

    if (PointerIsValid(splitState->src_partition_name)) {
        srcPartOid = partitionNameGetPartitionOid(RelationGetRelid(partTableRel),
            splitState->src_partition_name,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            true,
            false,
            NULL,
            NULL,
            NoLock);
    } else {
        Assert(PointerIsValid(splitState->partition_for_values));
        splitState->partition_for_values = transformConstIntoTargetType(
            partTableRel->rd_att->attrs, partMap->partitionKey, splitState->partition_for_values);
        srcPartOid = partitionValuesGetPartitionOid(
            partTableRel, splitState->partition_for_values, AccessExclusiveLock, true, true, false);
    }

    return srcPartOid;
}

#define precheck_point_value_internal(a)                                                                              \
    do {                                                                                                              \
        Node* pexpr = (Node*)linitial(a);                      /* original value */                                   \
        Const* pval = GetPartitionValue(pos, attrs, a, false); /* cast(ori)::int */                                   \
        if (!pval->ismaxvalue) {                                                                                      \
            Const* c = (Const*)evaluate_expr((Expr*)pexpr, exprType(pexpr), exprTypmod(pexpr), exprCollation(pexpr)); \
            if (partitonKeyCompare(&pval, &c, 1) != 0)                                                                \
                ereport(ERROR,                                                                                        \
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),                                                       \
                        errmsg("start/end/every value must be an const-integer for partition \"%s\"",                 \
                            defState->partitionName)));                                                               \
        }                                                                                                             \
    } while (0)

/*
 * precheck_start_end_defstate
 *    precheck start/end value of a range partition defstate
 */
static void precheck_start_end_defstate(List* pos, Form_pg_attribute* attrs, RangePartitionStartEndDefState* defState)
{
    ListCell* cell = NULL;

    if (pos == NULL || attrs == NULL || defState == NULL)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("unexpected parameter for precheck start/end defstate.")));

    Assert(pos->length == 1); /* already been checked in caller */
    foreach (cell, pos) {
        int i = lfirst_int(cell);

        switch (attrs[i]->atttypid) {
            case INT2OID:
            case INT4OID:
            case INT8OID:
                if (defState->startValue)
                    precheck_point_value_internal(defState->startValue);
                if (defState->endValue)
                    precheck_point_value_internal(defState->endValue);
                if (defState->everyValue)
                    precheck_point_value_internal(defState->everyValue);
                break;

            default:
                break; /* don't check */
        }
    }

    return;
}

/* is_start_end_def_list
 * 	check the partition state and return the type of state
 * 	true: start/end stmt; false: less/than stmt
 *
 * [IN] state: partition state
 *
 * RETURN: if it is start/end stmt
 */
bool is_start_end_def_list(List* def_list)
{
    ListCell* cell = NULL;

    if (def_list == NULL)
        return false;

    /* count start/end clause */
    foreach (cell, def_list) {
        Node* defState = (Node*)lfirst(cell);

        if (!IsA(defState, RangePartitionStartEndDefState))
            return false; /* not in start/end syntax, stop here */
    }

    return true;
}

/*
 * get_partition_arg_value
 * 	Get the actual value from the expression. There are only a limited range
 * 	of cases we must cover because the parser guarantees constant input.
 *
 * [IN] node: input node expr
 * [out] isnull: indicate the NULL of result datum
 *
 * RETURN: a datum produced by node
 */
static Datum get_partition_arg_value(Node* node, bool* isnull)
{
    Const* c = NULL;

    c = (Const*)evaluate_expr((Expr*)node, exprType(node), exprTypmod(node), exprCollation(node));
    if (!IsA(c, Const))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("partition parameter is not constant.")));

    *isnull = c->constisnull;
    return c->constvalue;
}

/*
 * evaluate_opexpr
 * Evaluate a basic operator expression from a partitioning specification.
 * The expression will only be an op expr but the sides might contain
 * a coercion function. The underlying value will be a simple constant,
 * however.
 *
 * If restypid is non-NULL and *restypid is set to InvalidOid, we tell the
 * caller what the return type of the operator is. If it is anything but
 * InvalidOid, coerce the operation's result to that type.
 *
 * [IN] pstate: parser state
 * [IN] oprname: name of opreator which can be <, +, -, etc.
 * [IN] leftarg: left arg
 * [IN] rightarg: right arg
 * [IN/OUT] restypid: result type id, if given coerce to it, otherwise return the compute result-type.
 * [IN] location: location of the expr, not necessary
 *
 * RETURN: a datum produced by the expr: "leftarg oprname rightarg"
 */
static Datum evaluate_opexpr(
    ParseState* pstate, List* oprname, Node* leftarg, Node* rightarg, Oid* restypid, int location)
{
    Datum res = 0;
    Datum lhs = 0;
    Datum rhs = 0;
    OpExpr* opexpr = NULL;
    bool byval = false;
    int16 len;
    Oid oprcode;
    Type typ;
    bool isnull = false;

    opexpr = (OpExpr*)make_op(pstate, oprname, leftarg, rightarg, location);

    oprcode = get_opcode(opexpr->opno);
    if (oprcode == InvalidOid) /* should not fail */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmodule(MOD_OPT),
                errmsg("cache lookup failed for operator %u", opexpr->opno)));

    opexpr->opfuncid = oprcode;

    /* compute result */
    lhs = get_partition_arg_value((Node*)linitial(opexpr->args), &isnull);
    if (!isnull) {
        rhs = get_partition_arg_value((Node*)lsecond(opexpr->args), &isnull);
        if (!isnull)
            res = OidFunctionCall2(opexpr->opfuncid, lhs, rhs);
    }

    /* If the caller supplied a target result type, coerce if necesssary */
    if (PointerIsValid(restypid)) {
        if (OidIsValid(*restypid)) {
            if (*restypid != opexpr->opresulttype) {
                Expr* e = NULL;
                int32 typmod;
                Const* c = NULL;
                bool isnull = false;

                typ = typeidType(opexpr->opresulttype);
                c = makeConst(opexpr->opresulttype,
                    ((Form_pg_type)GETSTRUCT(typ))->typtypmod,
                    ((Form_pg_type)GETSTRUCT(typ))->typcollation,
                    typeLen(typ),
                    res,
                    false,
                    typeByVal(typ));
                ReleaseSysCache(typ);

                typ = typeidType(*restypid);
                typmod = ((Form_pg_type)GETSTRUCT(typ))->typtypmod;
                ReleaseSysCache(typ);

                /* coerce from oprresulttype to resttypid */
                e = (Expr*)coerce_type(NULL,
                    (Node*)c,
                    opexpr->opresulttype,
                    *restypid,
                    typmod,
                    COERCION_ASSIGNMENT,
                    COERCE_IMPLICIT_CAST,
                    -1);

                res = get_partition_arg_value((Node*)e, &isnull);
            }
        } else {
            *restypid = opexpr->opresulttype;
        }
    } else {
        return res;
    }

    /* copy result, done */
    Assert(OidIsValid(*restypid));
    typ = typeidType(*restypid);
    byval = typeByVal(typ);
    len = typeLen(typ);
    ReleaseSysCache(typ);

    res = datumCopy(res, byval, len);
    return res;
}

/*
 * coerce_partition_arg
 * 	coerce a partition parameter (start/end/every) to targetType
 *
 * [IN] pstate: parse state
 * [IN] node: Node to be coerced
 * [IN] targetType: target type
 *
 * RETURN: a const
 */
static Const* coerce_partition_arg(ParseState* pstate, Node* node, Oid targetType)
{
    Datum res;
    Oid curtyp;
    Const* c = NULL;
    Type typ = typeidType(targetType);
    int32 typmod = ((Form_pg_type)GETSTRUCT(typ))->typtypmod;
    int16 typlen = ((Form_pg_type)GETSTRUCT(typ))->typlen;
    bool typbyval = ((Form_pg_type)GETSTRUCT(typ))->typbyval;
    Oid typcollation = ((Form_pg_type)GETSTRUCT(typ))->typcollation;
    bool isnull = false;

    ReleaseSysCache(typ);

    curtyp = exprType(node);
    Assert(OidIsValid(curtyp));

    if (curtyp != targetType && OidIsValid(targetType)) {
        node = coerce_type(pstate, node, curtyp, targetType, typmod, COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);

        if (!PointerIsValid(node))
            ereport(
                ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("could not coerce partitioning parameter.")));
    }
    res = get_partition_arg_value(node, &isnull);
    c = makeConst(targetType, typmod, typcollation, typlen, res, isnull, typbyval);

    return c;
}

/*
 * choose_coerce_type
 * 	choose a coerce type
 * Note: this function may help us to fix ambiguous problem
 */
static Oid choose_coerce_type(Oid leftid, Oid rightid)
{
    if (leftid == FLOAT8OID && rightid == NUMERICOID)
        return NUMERICOID; /* make_op chooses function float8pl to compute "float8 + numeric" */
    else
        return InvalidOid; /* let make_op decide */
}

/*
 * divide_start_end_every_internal
 * 	internal implementaion for dividing an interval indicated by any-datatype
 * 	for example:
 * 	-- start(1) end(100) every(30)
 * 	-- start(123.01) end(345.09) every(111.99)
 * 	-- start('12-01-2012') end('12-05-2018') every('1 year')
 *
 * If (end-start) is divided by every with a remainder, then last partition is smaller
 * than others.
 *
 * [IN] pstate: parse state
 * [IN] partName: partition name
 * [IN] attr: pg_attribute of the target type
 * [IN] startVal: start value
 * [IN] endVal: end value
 * [IN] everyVal: interval value
 * [OUT] numPart: number of partitions
 * [IN] maxNum: max partition number allowed
 * [IN] isinterval: if EVERY is a interval value
 *
 * RETURN: end points of all sub-intervals
 */
static List* divide_start_end_every_internal(ParseState* pstate, char* partName, Form_pg_attribute attr,
    Const* startVal, Const* endVal, Node* everyExpr, int* numPart, int maxNum, bool isinterval, bool needCheck)
{
    List* result = NIL;
    List* oprPl = NIL;
    List* oprLt = NIL;
    List* oprLe = NIL;
    List* oprMul = NIL;
    List* oprEq = NIL;
    Datum res;
    Const* pnt = NULL;
    Oid restypid;
    Const* curpnt = NULL;
    int32 nPart;
    bool isEnd = false;
    Const* everyVal = NULL;
    Oid targetType;
    bool targetByval = false;
    int16 targetLen;
    int32 targetTypmod;
    Oid targetCollation;

    Assert(maxNum > 0 && maxNum <= MAX_PARTITION_NUM);

    oprPl = list_make1(makeString("+"));
    oprLt = list_make1(makeString("<"));
    oprLe = list_make1(makeString("<="));
    oprMul = list_make1(makeString("*"));
    oprEq = list_make1(makeString("="));

    /*
     * cast everyExpr to targetType
     * Note: everyExpr goes through transformExpr and transformIntoConst already.
     */
    everyVal = (Const*)GetTargetValue(attr, (Const*)everyExpr, isinterval);

    /* first compare start/end value */
    res = evaluate_opexpr(pstate, oprLe, (Node*)endVal, (Node*)startVal, NULL, -1);
    if (DatumGetBool(res))
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("start value must be less than end value for partition \"%s\".", partName)));

    /* get target type info */
    targetType = attr->atttypid;
    targetByval = attr->attbyval;
    targetCollation = attr->attcollation;
    if (targetType == DATEOID || targetType == TIMESTAMPOID || targetType == TIMESTAMPTZOID)
        targetTypmod = -1; /* avoid accuracy-problem of date */
    else
        targetTypmod = attr->atttypmod;
    targetLen = attr->attlen;

    /* build result */
    curpnt = startVal;
    nPart = 0;
    isEnd = false;
    while (nPart < maxNum) {
        /* compute currentPnt + everyval */
        res = evaluate_opexpr(pstate, oprPl, (Node*)curpnt, (Node*)everyVal, &targetType, -1);
        pnt = makeConst(targetType, targetTypmod, targetCollation, targetLen, res, false, targetByval);
        pnt = (Const*)GetTargetValue(attr, (Const*)pnt, false);

        /* necessary check in first pass */
        if (nPart == 0) {
            /*
             * check ambiguous partition rule
             *
             * 1. start(1) end (1.00007) every(0.00001)  -- for float4 datatype
             *      cast(1 + 0.00001 as real)  !=  (1 + 0.00001)::numeric
             * This rule is ambiguous, error out.
             */
            if (needCheck) {
                Const* c = NULL;
                Const* uncast = NULL;
                Type typ;

                /* get every value, uncast */
                restypid = exprType(everyExpr);
                c = coerce_partition_arg(pstate, everyExpr, restypid);

                /* calculate start+every cast to proper type */
                restypid = choose_coerce_type(targetType, restypid);
                res = evaluate_opexpr(pstate, oprPl, (Node*)startVal, (Node*)c, &restypid, -1);
                typ = typeidType(restypid);
                uncast = makeConst(restypid,
                    ((Form_pg_type)GETSTRUCT(typ))->typtypmod,
                    ((Form_pg_type)GETSTRUCT(typ))->typcollation,
                    typeLen(typ),
                    res,
                    false,
                    typeByVal(typ));
                ReleaseSysCache(typ);

                res = evaluate_opexpr(pstate, oprEq, (Node*)pnt, (Node*)uncast, NULL, -1);
                if (!DatumGetBool(res))
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("ambiguous partition rule is raised by EVERY parameter in partition \"%s\".",
                                partName)));
            }

            /* check partition step */
            res = evaluate_opexpr(pstate, oprLe, (Node*)pnt, (Node*)startVal, NULL, -1);
            if (DatumGetBool(res))
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("partition step is too small for partition \"%s\".", partName)));
        }

        /* check to determine if it is the final partition */
        res = evaluate_opexpr(pstate, oprLe, (Node*)pnt, (Node*)endVal, NULL, -1);
        if (DatumGetBool(res)) {
            result = lappend(result, pnt);
            nPart++;
            res = evaluate_opexpr(pstate, oprLt, (Node*)pnt, (Node*)endVal, NULL, -1);
            if (!DatumGetBool(res)) {
                /* case-1: final partition just matches endVal  */
                isEnd = true;
                break;
            }
        } else if (nPart == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("partition step is too big for partition \"%s\".", partName)));
        } else {
            /* case-2: final partition is smaller than others */
            pfree_ext(pnt);
            pnt = (Const*)copyObject(endVal);
            result = lappend(result, pnt);
            nPart++;
            isEnd = true;
            break;
        }

        curpnt = pnt;
    }

    if (!isEnd) {
        /* too many partitions, report error */
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("too many partitions after split partition \"%s\".", partName),
                errhint("number of partitions can not be more than %d, MINVALUE will be auto-included if not assigned.",
                    MAX_PARTITION_NUM)));
    }

    /* done */
    Assert(result && result->length == nPart);
    if (numPart != NULL)
        *numPart = nPart;

    return result;
}

/*
 * DividePartitionStartEndInterval
 * 	divide the partition interval of start/end into specified sub-intervals
 *
 * [IN] pstate: parse state
 * [IN] attr: pg_attribute
 * [IN] partName: partition name
 * [IN] startVal: start value
 * [IN] endVal: end value
 * [IN] everyVal: interval value
 * [OUT] numPart: number of partitions
 * [IN] maxNum: max partition number allowed
 *
 * RETURN: end points of all sub-intervals
 */
static List* DividePartitionStartEndInterval(ParseState* pstate, Form_pg_attribute attr, char* partName,
    Const* startVal, Const* endVal, Const* everyVal, Node* everyExpr, int* numPart, int maxNum)
{
    List* result = NIL;

    Assert(maxNum > 0 && maxNum <= MAX_PARTITION_NUM);
    Assert(attr != NULL);

    /* maxvalue is not allowed in start/end stmt */
    Assert(startVal && IsA(startVal, Const) && !startVal->ismaxvalue);
    Assert(endVal && IsA(endVal, Const) && !endVal->ismaxvalue);
    Assert(everyVal && IsA(everyVal, Const) && !everyVal->ismaxvalue);

    /* Form each partition node const */
    switch (attr->atttypid) {
        case NUMERICOID: {
            Numeric v1 = DatumGetNumeric(startVal->constvalue);
            Numeric v2 = DatumGetNumeric(endVal->constvalue);
            Numeric d = DatumGetNumeric(everyVal->constvalue);
            /* NAN is not allowed */
            if (NUMERIC_IS_NAN(v1) || NUMERIC_IS_NAN(v2) || NUMERIC_IS_NAN(d))
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("partition \"%s\" is invalid.", partName),
                        errhint("NaN can not appear in a (START, END, EVERY) clause.")));

            result = divide_start_end_every_internal(
                pstate, partName, attr, startVal, endVal, everyExpr, numPart, maxNum, false, true);
            break;
        }

        case FLOAT4OID: {
            float4 v1 = DatumGetFloat4(startVal->constvalue);
            float4 v2 = DatumGetFloat4(endVal->constvalue);
            float4 d = DatumGetFloat4(everyVal->constvalue);
            /* INF is not allowed */
            if (isinf(d) || isinf(v1) || isinf(v2))
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("partition \"%s\" is invalid.", partName),
                        errhint("INF can not appear in a (START, END, EVERY) clause.")));

            result = divide_start_end_every_internal(
                pstate, partName, attr, startVal, endVal, everyExpr, numPart, maxNum, false, true);
            break;
        }

        case FLOAT8OID: {
            float8 v1 = DatumGetFloat8(startVal->constvalue);
            float8 v2 = DatumGetFloat8(endVal->constvalue);
            float8 d = DatumGetFloat8(everyVal->constvalue);
            /* INF is not allowed */
            if (isinf(d) || isinf(v1) || isinf(v2))
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("partition \"%s\" is invalid.", partName),
                        errhint("INF can not appear in a (START, END, EVERY) clause.")));

            result = divide_start_end_every_internal(
                pstate, partName, attr, startVal, endVal, everyExpr, numPart, maxNum, false, true);
            break;
        }

        case INT2OID:
        case INT4OID:
        case INT8OID: {
            result = divide_start_end_every_internal(
                pstate, partName, attr, startVal, endVal, everyExpr, numPart, maxNum, false, false);
            break;
        }

        case DATEOID:
        case TIMESTAMPOID: {
            Timestamp t1 = DatumGetTimestamp(startVal->constvalue);
            Timestamp t2 = DatumGetTimestamp(endVal->constvalue);
            if (TIMESTAMP_NOT_FINITE(t1) || TIMESTAMP_NOT_FINITE(t2))
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("partition \"%s\" is invalid.", partName),
                        errhint("INF can not appear in a (START, END, EVERY) clause.")));
            result = divide_start_end_every_internal(
                pstate, partName, attr, startVal, endVal, everyExpr, numPart, maxNum, true, false);
            break;
        }

        case TIMESTAMPTZOID: {
            TimestampTz t1 = DatumGetTimestampTz(startVal->constvalue);
            TimestampTz t2 = DatumGetTimestampTz(endVal->constvalue);
            if (TIMESTAMP_NOT_FINITE(t1) || TIMESTAMP_NOT_FINITE(t2))
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("partition \"%s\" is invalid.", partName),
                        errhint("INF can not appear in a (START, END, EVERY) clause.")));
            result = divide_start_end_every_internal(
                pstate, partName, attr, startVal, endVal, everyExpr, numPart, maxNum, true, false);
            break;
        }

        default:
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("unsupported datatype served as a partition key in the start/end clause."),
                    errhint("Valid datatypes are: smallint, int, bigint, float4/real, float8/double, numeric, date and "
                            "timestamp [with time zone].")));
    }

    return result;
}

#define add_last_single_start_partition                                                                              \
    do {                                                                                                             \
        Const* laststart = NULL;                                                                                     \
        /* last DefState is a single START, so add the last partition here */                                        \
        Assert(lastState->startValue && !lastState->endValue);                                                       \
        pnt = (Const*)copyObject(startVal);                                                                          \
        boundary = list_make1(pnt);                                                                                  \
        laststart = GetPartitionValue(pos, attrs, lastState->startValue, false);                                     \
        if (partitonKeyCompare(&laststart, &startVal, 1) >= 0)                                                       \
            ereport(ERROR,                                                                                           \
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),                                                          \
                    errmsg("start value of partition \"%s\" is too low.", defState->partitionName),                  \
                    errhint("partition gap or overlapping is not allowed.")));                                       \
        if (lowBound == NULL && curDefState == 1) {                                                                  \
            /* last single START is the first DefState and MINVALUE is included */                                   \
            get_range_partition_name_prefix(namePrefix, lastState->partitionName);                                   \
            ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, 1);                                     \
            securec_check_ss(ret, "\0", "\0");                                                                       \
            newPartList = add_range_partition_def_state(newPartList, boundary, partName, lastState->tableSpaceName); \
            totalPart++;                                                                                             \
        } else {                                                                                                     \
            newPartList = add_range_partition_def_state(                                                             \
                newPartList, boundary, lastState->partitionName, lastState->tableSpaceName);                         \
            totalPart++;                                                                                             \
        }                                                                                                            \
        pfree_ext(laststart);                                                                                        \
    } while (0)

/*
 * transformRangePartStartEndStmt
 * 	entry of transform range partition which is defined by "start/end" syntax
 *
 * [IN] pstate: parse state
 * [IN] partitionList: partition list to be rewrote
 * [IN] attrs: pg_attribute item
 * [IN] pos: position of partition key in ColDef
 * [IN] existPartNum: number of partitions already exists
 * [IN] lowBound: low-boundary of all paritions
 * [IN] upBound: up-boundary of all partitions
 * [IN] needFree: free input partitionList or not, true: free, false: not
 *
 * lowBound/upBound rules:
 *
 *                                lowBound                       upBound
 *   not-NULL:    check SP == lowBound      check EP == upBound
 *                                                        (START) include upBound
 *
 *         NULL:         include MINVALUE      (START) include MAXVALUE
 * SP: first start point of the def; EP: final end point of the def
 * (START) include xxx: for a single start as final clause, include xxx.
 *
 * -- CREATE TABLE PARTITION: lowBound=NULL, upBound=NULL
 * -- ADD PARTITION: lowBound=ExistUpBound, upBound=NULL
 * -- SPLIT PARTITION: lowBound=CurrentPartLowBound, upBound=CurrentPartUpBound
 *
 * RETURN: a new partition list (wrote by "less/than" syntax).
 */
List* transformRangePartStartEndStmt(ParseState* pstate, List* partitionList, List* pos, Form_pg_attribute* attrs,
    int32 existPartNum, Const* lowBound, Const* upBound, bool needFree)
{
    ListCell* cell = NULL;
    int i, j;
    Oid targetType = InvalidOid;
    List* newPartList = NIL;
    char partName[NAMEDATALEN] = {0};
    char namePrefix[NAMEDATALEN] = {0};
    errno_t ret = EOK;
    Const* startVal = NULL;
    Const* endVal = NULL;
    Const* everyVal = NULL;
    Const* lastVal = NULL;
    int totalPart = 0;
    int numPart = 0;
    List* resList = NIL;
    List* boundary = NIL;
    Const* pnt = NULL;
    RangePartitionStartEndDefState* defState = NULL;
    RangePartitionStartEndDefState* lastState = NULL;
    int curDefState;
    int kc;
    ListCell* lc = NULL;
    char* curName = NULL;
    char* preName = NULL;
    bool isinterval = false;
    Form_pg_attribute attr = NULL;

    if (partitionList == NULL || list_length(partitionList) == 0 || attrs == NULL || pos == NULL)
        return partitionList; /* untouched */

    Assert(existPartNum >= 0 && existPartNum <= MAX_PARTITION_NUM);

    /* only one partition key is allowed */
    if (pos->length != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("partitioned table has too many partition keys."),
                errhint("start/end syntax requires a partitioned table with only one partition key.")));
    }

    /*
     * Now, it is start/end stmt, check following key-points:
     *
     * - mixture of "start/end" and "less/than" is forbidden
     * - only one partition key is given
     * - datatype of partition key
     * - continuity of partitions
     * - number of partitions <= MAX_PARTITION_NUM
     * - validation of partition namePrefix
     */
    foreach (cell, partitionList) {
        RangePartitionStartEndDefState* defState = (RangePartitionStartEndDefState*)lfirst(cell);
        if ((defState->startValue && defState->startValue->length != 1) ||
            (defState->endValue && defState->endValue->length != 1) ||
            (defState->everyValue && defState->everyValue->length != 1))
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("too many partition keys for partition \"%s\".", defState->partitionName),
                    errhint("only one partition key is allowed in start/end clause.")));
    }

    /* check partition name */
    check_partition_name_start_end(partitionList);

    /* check: datatype of partition key */
    foreach (cell, pos) {
        i = lfirst_int(cell);
        attr = attrs[i];
        targetType = attr->atttypid;

        switch (targetType) {
            case INT2OID:
            case INT4OID:
            case INT8OID:
            case NUMERICOID:
            case FLOAT4OID:
            case FLOAT8OID:
                isinterval = false;
                break;

            case DATEOID:
            case TIMESTAMPOID:
            case TIMESTAMPTZOID:
                isinterval = true;
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("datatype of column \"%s\" is unsupported for partition key in start/end clause.",
                            NameStr(attrs[i]->attname)),
                        errhint("Valid datatypes are: smallint, int, bigint, float4/real, float8/double, numeric, date "
                                "and timestamp [with time zone].")));
                break;
        }
    }

    /* check exist partition number */
    if (existPartNum >= MAX_PARTITION_NUM)
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg("can not add more partitions as partition number is already at its maximum.")));

    /*
     * Start transform (including check)
     *
     * Recall the syntax:
     *   start_end_item [, ...]
     *
     * where start_end_item:
     * 	{ start(a) end (b) [every(d)] }  |  start(a)  |  end (b)
     */
    curDefState = 0;
    totalPart = existPartNum;
    lastState = NULL;
    defState = NULL;
    lastVal = NULL;
    foreach (cell, partitionList) {
        lastState = defState;
        defState = (RangePartitionStartEndDefState*)lfirst(cell);
        Assert(defState);

        /* precheck defstate */
        precheck_start_end_defstate(pos, attrs, defState);

        /* type-1: start + end + every */
        if (defState->startValue && defState->endValue && defState->everyValue) {
            Node* everyExpr = (Node*)linitial(defState->everyValue);
            startVal = GetPartitionValue(pos, attrs, defState->startValue, false);
            endVal = GetPartitionValue(pos, attrs, defState->endValue, false);
            everyVal = GetPartitionValue(pos, attrs, defState->everyValue, isinterval);
            /* check value */
            if (startVal->ismaxvalue || endVal->ismaxvalue || everyVal->ismaxvalue)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("partition \"%s\" is invalid.", defState->partitionName),
                        errhint("MAXVALUE can not appear in a (START, END, EVERY) clause.")));
            if (partitonKeyCompare(&startVal, &endVal, 1) >= 0)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg(
                            "start value must be less than end value for partition \"%s\".", defState->partitionName)));
            if (lastVal != NULL) {
                if (lastVal->ismaxvalue)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("partition \"%s\" is not allowed behind MAXVALUE.", defState->partitionName)));
                kc = partitonKeyCompare(&lastVal, &startVal, 1);
                if (kc > 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("start value of partition \"%s\" is too low.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
                if (kc < 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("start value of partition \"%s\" is too high.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
            }

            /* build necessary MINVALUE, check lowBound,  append for last single START, etc. */
            if (lastVal == NULL) {
                if (lastState != NULL) {
                    /* last DefState is a single START */
                    add_last_single_start_partition;
                } else {
                    /* this is the first DefState (START, END, EVERY) */
                    if (lowBound == NULL) {
                        /* this is the first DefState (START, END, EVERY), add MINVALUE */
                        pnt = (Const*)copyObject(startVal);
                        boundary = list_make1(pnt);
                        get_range_partition_name_prefix(namePrefix, defState->partitionName);
                        ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, 0);
                        securec_check_ss(ret, "\0", "\0");
                        newPartList =
                            add_range_partition_def_state(newPartList, boundary, partName, defState->tableSpaceName);
                        totalPart++;
                    } else {
                        /* this is the first DefState (START, END, EVERY), but do not include MINVALUE */
                        /* check SP: case for ADD_PARTITION, SPLIT_PARTITION */
                        /* ignore: case for ADD_PARTITION, check SP: SPLIT_PARTITION */
                        if (NULL != lowBound && NULL != upBound) {
                            if (partitonKeyCompare(&lowBound, &startVal, 1) != 0)
                                ereport(ERROR,
                                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                                        errmsg(
                                            "start value of partition \"%s\" NOT EQUAL up-boundary of last partition.",
                                            defState->partitionName)));
                        }
                    }
                }
            }

            /* add current DefState */
            get_range_partition_name_prefix(namePrefix, defState->partitionName, true);
            Assert(totalPart < MAX_PARTITION_NUM);
            Assert(everyExpr);
            resList = DividePartitionStartEndInterval(pstate,
                attr,
                defState->partitionName,
                startVal,
                endVal,
                everyVal,
                everyExpr,
                &numPart,
                MAX_PARTITION_NUM - totalPart);
            Assert(resList && numPart == resList->length);

            j = 1;
            foreach (lc, resList) {
                ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, j);
                securec_check_ss(ret, "\0", "\0");
                boundary = list_make1(lfirst(lc));
                newPartList = add_range_partition_def_state(newPartList, boundary, partName, defState->tableSpaceName);

                if (j == 1) {
                    ((RangePartitionDefState*)llast(newPartList))->curStartVal = (Const*)copyObject(startVal);
                    ((RangePartitionDefState*)llast(newPartList))->partitionInitName = pstrdup(defState->partitionName);
                }

                j++;
            }
            list_free_ext(resList); /* can not be freed deeply */

            totalPart += numPart;

            /* update lastVal */
            pfree_ext(everyVal);
            if (NULL != lastVal)
                pfree_ext(lastVal);
            lastVal = endVal;
        } else if (defState->startValue && defState->endValue) {
            startVal = GetPartitionValue(pos, attrs, defState->startValue, false);
            endVal = GetPartitionValue(pos, attrs, defState->endValue, false);
            Assert(startVal != NULL && endVal != NULL);

            /* check value */
            if (startVal->ismaxvalue)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("start value can not be MAXVALUE for partition \"%s\".", defState->partitionName)));

            if (partitonKeyCompare(&startVal, &endVal, 1) >= 0)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg(
                            "start value must be less than end value for partition \"%s\".", defState->partitionName)));

            if (lastVal != NULL) {
                if (lastVal->ismaxvalue)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("partition \"%s\" is not allowed behind MAXVALUE.", defState->partitionName)));

                kc = partitonKeyCompare(&lastVal, &startVal, 1);
                if (kc > 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("start value of partition \"%s\" is too low.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
                if (kc < 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("start value of partition \"%s\" is too high.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
            }

            /* build less than defstate */
            if (lastVal != NULL) {
                /* last DefState is (START END EVERY) or (START END) or (END) */
                pnt = (Const*)copyObject(endVal);
                boundary = list_make1(pnt);
                newPartList = add_range_partition_def_state(
                    newPartList, boundary, defState->partitionName, defState->tableSpaceName);
                totalPart++;
            } else {
                if (lastState != NULL) {
                    /* last DefState is a single START */
                    add_last_single_start_partition;

                    /* add current DefState */
                    pnt = (Const*)copyObject(endVal);
                    boundary = list_make1(pnt);
                    newPartList = add_range_partition_def_state(
                        newPartList, boundary, defState->partitionName, defState->tableSpaceName);
                    totalPart++;
                } else if (lowBound == NULL) {
                    /* this is the first DefState (START, END), and MINVALUE will be included */
                    get_range_partition_name_prefix(namePrefix, defState->partitionName);

                    /* MINVALUE */
                    pnt = (Const*)copyObject(startVal);
                    boundary = list_make1(pnt);
                    ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, 0);
                    securec_check_ss(ret, "\0", "\0");
                    newPartList = add_range_partition_def_state(newPartList, boundary, partName, defState->tableSpaceName);
                    totalPart++;

                    pnt = (Const*)copyObject(endVal);
                    boundary = list_make1(pnt);
                    ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, 1);
                    securec_check_ss(ret, "\0", "\0");
                    newPartList = add_range_partition_def_state(newPartList, boundary, partName, defState->tableSpaceName);
                    totalPart++;
                } else {
                    /* this is first DefState (START, END), but do not include MINVALUE */
                    /* check SP: case for ADD_PARTITION, SPLIT_PARTITION */
                    /* ignore: case for ADD_PARTITION, check SP: SPLIT_PARTITION */
                    if (NULL != lowBound && NULL != upBound) {
                        if (partitonKeyCompare(&lowBound, &startVal, 1) != 0)
                            ereport(ERROR,
                                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                                    errmsg("start value of partition \"%s\" NOT EQUAL up-boundary of last partition.",
                                        defState->partitionName)));
                    }

                    /* add endVal as a pnt */
                    pnt = (Const*)copyObject(endVal);
                    boundary = list_make1(pnt);
                    newPartList = add_range_partition_def_state(
                        newPartList, boundary, defState->partitionName, defState->tableSpaceName);
                    if (NULL != newPartList) {
                        ((RangePartitionDefState*)llast(newPartList))->curStartVal = (Const*)copyObject(startVal);
                    }

                    totalPart++;
                }
            }

            if (NULL != lastVal)
                pfree_ext(lastVal);
            lastVal = endVal;
        } else if (defState->startValue) {
            startVal = GetPartitionValue(pos, attrs, defState->startValue, false);
            Assert(startVal != NULL);

            /* check value */
            if (startVal->ismaxvalue)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("start value can not be MAXVALUE for partition \"%s\".", defState->partitionName)));

            if (lastVal != NULL) {
                if (lastVal->ismaxvalue)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("partition \"%s\" is not allowed behind MAXVALUE.", defState->partitionName)));

                kc = partitonKeyCompare(&lastVal, &startVal, 1);
                if (kc > 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("start value of partition \"%s\" is too low.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
                if (kc < 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("start value of partition \"%s\" is too high.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
            }

            /* build less than defstate */
            if (lastVal == NULL) {
                if (lastState != NULL) {
                    /* last DefState is a single START */
                    add_last_single_start_partition;
                } else {
                    /* this is the first DefState */
                    if (lowBound == NULL) {
                        /* this is the first DefState, and MINVALUE will be included */
                        get_range_partition_name_prefix(namePrefix, defState->partitionName);
                        pnt = (Const*)copyObject(startVal);
                        boundary = list_make1(pnt);
                        ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, 0);
                        securec_check_ss(ret, "\0", "\0");

                        /* add MINVALUE here, the other partition will be added in next DefState because the endVal is
                         * unknown right now */
                        newPartList =
                            add_range_partition_def_state(newPartList, boundary, partName, defState->tableSpaceName);
                        totalPart++;
                    } else {
                        /* this is the first DefState, do not include MINVALUE */
                        /* check SP: case for ADD_PARTITION, SPLIT_PARTITION */
                        /* ignore: case for ADD_PARTITION, check SP: SPLIT_PARTITION */
                        if (NULL != lowBound && NULL != upBound) {
                            if (partitonKeyCompare(&lowBound, &startVal, 1) != 0)
                                ereport(ERROR,
                                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                                        errmsg(
                                            "start value of partition \"%s\" NOT EQUAL up-boundary of last partition.",
                                            defState->partitionName)));
                        }
                    }
                }
            }

            if (NULL != lastVal)
                pfree_ext(lastVal);
            lastVal = NULL;
        } else if (defState->endValue) {
            endVal = GetPartitionValue(pos, attrs, defState->endValue, false);
            Assert(endVal != NULL);

            /* check value */
            if (lastVal != NULL) {
                if (lastVal->ismaxvalue)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("partition \"%s\" is not allowed behind MAXVALUE.", defState->partitionName)));

                if (partitonKeyCompare(&lastVal, &endVal, 1) >= 0) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("end value of partition \"%s\" is too low.", defState->partitionName),
                            errhint("partition gap or overlapping is not allowed.")));
                }
            }

            /* build a less than defState: we need a last partition, or it is a first partition here */
            if (lastVal == NULL) {
                if (lastState != NULL) {
                    /* last def is a single START, invalid definition */
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("partition \"%s\" is an invalid definition clause.", defState->partitionName),
                            errhint("Do not use a single END after a single START.")));
                } else {
                    /* this is the first def state END, check lowBound if any */
                    /* case for ADD_PARTITION, SPLIT_PARTITION */
                    if (lowBound && partitonKeyCompare(&lowBound, &endVal, 1) >= 0)
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                                errmsg(
                                    "end value of partition \"%s\" MUST be greater than up-boundary of last partition.",
                                    defState->partitionName)));
                }
            }
            pnt = (Const*)copyObject(endVal);
            boundary = list_make1(pnt);
            newPartList =
                add_range_partition_def_state(newPartList, boundary, defState->partitionName, defState->tableSpaceName);
            totalPart++;

            if (lastVal != NULL) {
                pfree_ext(lastVal); 
            }
            lastVal = endVal;
            startVal = NULL;
        } else {
            Assert(false); /* unexpected syntax */
        }

        /* -- */
        /* check partition numbers */
        if (totalPart >= MAX_PARTITION_NUM) {
            if (totalPart == MAX_PARTITION_NUM && !lnext(cell)) {
                break;
            } else {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("too many partitions after split partition \"%s\".", defState->partitionName),
                        errhint("number of partitions can not be more than %d, MINVALUE will be auto-included if not "
                                "assigned.",
                            MAX_PARTITION_NUM)));
            }
        }

        curDefState++;
    }

    /* Final stage: add upBound for a single START at last */
    if (!defState->endValue) {
        /* this is a single START */
        Assert(defState->startValue);

        /* first check upBound */
        if (upBound == NULL) {
            /* no upBound, means up-Boundary is MAXVALUE: case for CREATE, ADD_PARTITION */
            pnt = makeNode(Const);
            pnt->ismaxvalue = true;
            boundary = list_make1(pnt);
        } else {
            /* have upBound: case for SPLIT PARTITION */
            if (partitonKeyCompare(&upBound, &startVal, 1) <= 0)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("start value of partition \"%s\" MUST be less than up-boundary of the partition to be "
                               "splitted.",
                            defState->partitionName)));

            pnt = (Const*)copyObject(upBound);
            boundary = list_make1(pnt);
        }

        /* second check lowBound */
        if (lowBound == NULL && curDefState == 1) {
            /* we have no lowBound, and this is a first def, so MINVALUE already been added */
            get_range_partition_name_prefix(namePrefix, defState->partitionName);
            ret = sprintf_s(partName, sizeof(partName), "%s_%d", namePrefix, 1);
            securec_check_ss(ret, "\0", "\0");
            newPartList = add_range_partition_def_state(newPartList, boundary, partName, defState->tableSpaceName);
        } else {
            newPartList =
                add_range_partition_def_state(newPartList, boundary, defState->partitionName, defState->tableSpaceName);
            if (NULL != newPartList && NULL != defState->startValue) {
                ((RangePartitionDefState*)llast(newPartList))->curStartVal =
                    (Const*)copyObject(linitial(defState->startValue));
            }
        }
        totalPart++;
    } else {
        /* final def has endVal, just check upBound if any, case for SPLIT_PARTITION */
        if (upBound && partitonKeyCompare(&upBound, &endVal, 1) != 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("end value of partition \"%s\" NOT EQUAL up-boundary of the partition to be splitted.",
                        defState->partitionName)));
        }
    }

    /* necessary check */
    if (totalPart > MAX_PARTITION_NUM) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("too many partitions after split partition \"%s\".", defState->partitionName),
                errhint("number of partitions can not be more than %d, MINVALUE will be auto-included if not assigned.",
                    MAX_PARTITION_NUM)));
    }

    /* since splitting partition is done, check partition name again */
    foreach (cell, newPartList) {
        lc = cell;
        preName = ((RangePartitionDefState*)lfirst(cell))->partitionName;
        while (NULL != (lc = lnext(lc))) {
            curName = ((RangePartitionDefState*)lfirst(lc))->partitionName;
            if (!strcmp(curName, preName)) {
                ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT),
                        errmsg("duplicate partition name: \"%s\".", curName),
                        errhint("partitions defined by (START, END, EVERY) are named as \"partitionName_x\" where x is "
                                "an integer and starts from 0 or 1.")));
            }
        }
    }

    /* it's ok, done */
    Assert(newPartList && newPartList->length == totalPart - existPartNum);
    if (needFree) {
        list_free_deep(partitionList); /* deep free is ok */
        partitionList = NULL;
    }

    if (NULL != startVal)
        pfree_ext(startVal);
    if (NULL != lastVal)
        pfree_ext(lastVal);

    return newPartList;
}

/*
 * Check if CreateStmt contains TableLikeClause, and the table to be defined is
 * on different nodegrop with the parent table.
 *
 * CreateStmt: the Stmt need check.
 */
bool check_contains_tbllike_in_multi_nodegroup(CreateStmt* stmt)
{
    ListCell* elements = NULL;
    Relation relation = NULL;
    foreach (elements, stmt->tableElts) {
        if (IsA(lfirst(elements), TableLikeClause)) {
            TableLikeClause* clause = (TableLikeClause*)lfirst(elements);
            relation = relation_openrv(clause->relation, AccessShareLock);

            if (is_multi_nodegroup_createtbllike(stmt->subcluster, relation->rd_id)) {
                heap_close(relation, AccessShareLock);
                return true;
            }

            heap_close(relation, AccessShareLock);
        }
    }

    return false;
}

/*
 * Check if the parent table and the table to be define in the same cluseter.
 * oid : the parent Table OID
 * subcluster: the new table where to create
 */
bool is_multi_nodegroup_createtbllike(PGXCSubCluster* subcluster, Oid oid)
#ifdef ENABLE_MULTIPLE_NODES
{
    Oid likeGroupOid;
    bool multiNodegroup = false;
    char* groupName = NULL;
    Oid newGroupOid = ng_get_installation_group_oid();

    if (subcluster != NULL) {
        ListCell* lc = NULL;
        foreach (lc, subcluster->members) {
            groupName = strVal(lfirst(lc));
        }
        if (groupName != NULL)
            newGroupOid = get_pgxc_groupoid(groupName);
    }

    likeGroupOid = get_pgxc_class_groupoid(oid);
    multiNodegroup = (newGroupOid != likeGroupOid);

    return multiNodegroup;
}
#else
{
    DISTRIBUTED_FEATURE_NOT_SUPPORTED();
    return false;
}
#endif

static void TryReuseFilenode(Relation rel, CreateStmtContext *ctx, bool clonepart)
{
    Form_pg_partition partForm = NULL;
    HeapTuple partTuple = NULL;
    List *partitionList = NULL;
    ListCell *cell = NULL;
    Relation toastRel;      

    if (!RelationIsPartitioned(rel)) {
        ctx->relnodelist = lappend_oid(ctx->relnodelist, rel->rd_rel->relfilenode);
        if (OidIsValid(rel->rd_rel->reltoastrelid)) {
            toastRel = heap_open(rel->rd_rel->reltoastrelid, NoLock);
            ctx->toastnodelist = lappend_oid(ctx->toastnodelist, rel->rd_rel->reltoastrelid);
            ctx->toastnodelist = lappend_oid(ctx->toastnodelist, toastRel->rd_rel->reltoastidxid);
            heap_close(toastRel, NoLock);
        }
    } else if (clonepart) {
        partitionList = searchPgPartitionByParentId(PART_OBJ_TYPE_TABLE_PARTITION, ObjectIdGetDatum(rel->rd_id));
        foreach (cell, partitionList) {
            partTuple = (HeapTuple)lfirst(cell);            
            partForm = (Form_pg_partition) GETSTRUCT(partTuple);
            ctx->relnodelist = lappend_oid(ctx->relnodelist, HeapTupleGetOid(partTuple));

            if (OidIsValid(partForm->reltoastrelid)) {
                toastRel = heap_open(partForm->reltoastrelid, NoLock);
                ctx->toastnodelist = lappend_oid(ctx->toastnodelist, partForm->reltoastrelid);
                ctx->toastnodelist = lappend_oid(ctx->toastnodelist, toastRel->rd_rel->reltoastidxid);
                heap_close(toastRel, NoLock);
            }
        }
        freePartList(partitionList);
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
            errmsg("Not specify \"INCLUDING PARTITION\" for partitioned-table relation:\"%s\"",
            RelationGetRelationName(rel))));
    }
}
