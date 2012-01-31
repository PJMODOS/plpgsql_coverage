#include "postgres.h"
#include "utils/guc.h"
#include "plpgsql.h"
#include "pgstat.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"

PG_MODULE_MAGIC;        /* Tell the server about our compile environment */

typedef struct {
    PLpgSQL_stmt *stmt;
    PgStat_Counter execCount;
    int num;
    int parent;
    int branch;
} StmtStats;

typedef struct
{
    int             stmtCount;
    StmtStats    ** stmtStats;
} CoverageCtx;

static const char * plugin_name  = "PLpgSQL_plugin";
static char * statsTableName = NULL;

/* Hooks */
void _PG_fini( void );

static void func_setup( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void func_end( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void stmt_begin( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );

/* Statement walker */
static void walkStmtList( PLpgSQL_execstate * estate, List *stmts, int parent, int branch );
static void walkStmtBlock( PLpgSQL_execstate * estate, PLpgSQL_stmt_block * stmt );
static void walkStmtIf( PLpgSQL_execstate * estate, PLpgSQL_stmt_if * stmt );
#if PG_VERSION_NUM >= 80400
static void walkStmtCase( PLpgSQL_execstate * estate, PLpgSQL_stmt_case * stmt );
#endif
static void walkStmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt, int parent, int branch );

/* Stats a allocation/search */
static StmtStats * newStmtStats( PLpgSQL_stmt * stmt, int num, int parent, int branch );
static StmtStats * getStmtStats( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );
static StmtStats * createStmtStats( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt, int parent, int branch );

/* Table updating */
static void ensureStatsTable( void );
static void updateStats( PLpgSQL_execstate * estate, PLpgSQL_function * func );

/* support for more plpgsql plugins */
static void (*old_func_setup) ( PLpgSQL_execstate * estate, PLpgSQL_function * func);
static void (*old_func_end) ( PLpgSQL_execstate * estate, PLpgSQL_function * func);
static void (*old_stmt_beg) ( PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt );

static PLpgSQL_plugin plugin_funcs = {
    func_setup,
    NULL,
    func_end,
    stmt_begin,
    NULL
};

/* Initialize hooks and config */
void _PG_init( void )
{
    PLpgSQL_plugin ** var_ptr = (PLpgSQL_plugin **) find_rendezvous_variable( plugin_name );

    if ((*var_ptr) != NULL) {
        old_func_setup = (*var_ptr)->func_setup;
        (*var_ptr)->func_setup = func_setup;
        old_func_end = (*var_ptr)->func_end;
        (*var_ptr)->func_end = func_end;
        old_stmt_beg = (*var_ptr)->stmt_beg;
        (*var_ptr)->stmt_beg = stmt_begin;
    } else {
        *var_ptr = &plugin_funcs;
    }

    DefineCustomStringVariable("plpgsql_coverage.stats_table_name",
                               "Table which will be used for storing of coverage data.",
                               "Including schema name",
                               &statsTableName,
#if PG_VERSION_NUM >= 80400
                               NULL,
#endif
                               PGC_USERSET,
#if PG_VERSION_NUM >= 80400
                               0,
#endif
#if PG_VERSION_NUM >= 90100
                               NULL,
#endif
                               NULL,
                               NULL );
}

/* Unload hook */
void _PG_fini( void )
{
    PLpgSQL_plugin ** var_ptr = (PLpgSQL_plugin **) find_rendezvous_variable( plugin_name );

    if (old_stmt_beg != NULL) {
        (*var_ptr)->func_setup = old_func_setup;
        (*var_ptr)->func_end = old_func_end;
        (*var_ptr)->stmt_beg = old_stmt_beg;
    } else {
        *var_ptr = NULL;
    }
}


/* Allocate new stats struct */
static StmtStats * newStmtStats( PLpgSQL_stmt * stmt, int num, int parent, int branch )
{
    StmtStats *ret = (StmtStats*)palloc(sizeof(StmtStats));
    ret->stmt = stmt;
    ret->execCount = 0;
    ret->num = num;
    ret->parent = parent;
    ret->branch = branch;

    return ret;
}

/* Find existing stats struct for a statement */
static StmtStats * getStmtStats( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
    CoverageCtx *ctx = (CoverageCtx *) estate->plugin_info;
    int i;

    // Try finding stats for a statement
    for (i = 0; i < ctx->stmtCount; i++)
    {
        if (ctx->stmtStats[i]->stmt == stmt)
            return ctx->stmtStats[i];
    }

    // Not found
    return NULL;
}

/* Find existing or allocate new stats struct for a statement */
static StmtStats * createStmtStats( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt, int parent, int branch )
{
    CoverageCtx *ctx = (CoverageCtx *) estate->plugin_info;
    StmtStats *stat;
    int i;

    // Try finding stats for a statement
    for (i = 0; i < ctx->stmtCount; i++)
    {
        if (ctx->stmtStats[i]->stmt == stmt)
            return ctx->stmtStats[i];
    }

    // Not found add stats to our statement stats list
    ctx->stmtCount++;
    stat = newStmtStats(stmt, i, parent, branch);

    if (ctx->stmtCount == 1)
        ctx->stmtStats = (StmtStats **)palloc(sizeof (StmtStats *) * ctx->stmtCount);
    else
        ctx->stmtStats = (StmtStats **)repalloc(ctx->stmtStats, sizeof (StmtStats *) * ctx->stmtCount);

    ctx->stmtStats[i] = stat;

    return ctx->stmtStats[i];
}


/* Process list of statements */
static void walkStmtList( PLpgSQL_execstate * estate, List *stmts, int parent, int branch )
{
    ListCell   *s;

    foreach(s, stmts)
        walkStmt(estate, (PLpgSQL_stmt *) lfirst(s), parent, branch);
}

/* Process statement block */
static void walkStmtBlock( PLpgSQL_execstate * estate, PLpgSQL_stmt_block * stmt )
{
    int branch = 0;

    walkStmtList(estate, stmt->body, getStmtStats(estate, (PLpgSQL_stmt *) stmt)->num, branch);

    if (stmt->exceptions)
    {
        ListCell   *e;

        foreach(e, stmt->exceptions->exc_list)
        {
            PLpgSQL_exception *exc = (PLpgSQL_exception *) lfirst(e);
            branch++;
            walkStmtList(estate, exc->action, getStmtStats(estate, (PLpgSQL_stmt *) stmt)->num, branch);
        }
    }
}

/* Process IF clause */
static void walkStmtIf( PLpgSQL_execstate * estate, PLpgSQL_stmt_if * stmt )
{
    walkStmtList(estate, stmt->true_body, getStmtStats(estate, (PLpgSQL_stmt *) stmt)->num, 0);

    if (stmt->false_body)
        walkStmtList(estate, stmt->false_body, getStmtStats(estate, (PLpgSQL_stmt *) stmt)->num, 1);

}

#if PG_VERSION_NUM >= 80400
/* Process CASE clause */
static void walkStmtCase( PLpgSQL_execstate * estate, PLpgSQL_stmt_case * stmt )
{
    ListCell   *l;
    int branch = 0;

    foreach(l, stmt->case_when_list)
    {
        PLpgSQL_case_when *cwt = (PLpgSQL_case_when *) lfirst(l);
        walkStmtList(estate, cwt->stmts, getStmtStats(estate, (PLpgSQL_stmt *) stmt)->num, branch);
        branch++;
    }

    if (stmt->have_else)
        walkStmtList(estate, stmt->else_stmts, getStmtStats(estate, (PLpgSQL_stmt *) stmt)->num, branch);
}
#endif

/*
 * Initializes statistics for each statement in the function
 */
static void walkStmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt, int parent, int branch )
{
    createStmtStats( estate, stmt, parent, branch );

    switch (stmt->cmd_type)
    {
        // BEGIN .. [EXCEPTION ..] END block, does not get "executed" but has body of statements
        case PLPGSQL_STMT_BLOCK:
            walkStmtBlock(estate, (PLpgSQL_stmt_block *) stmt);
            break;

        // IF .. [ ELSE .. ] END IF, the expression gets executed
        case PLPGSQL_STMT_IF:
            walkStmtIf(estate, (PLpgSQL_stmt_if *) stmt);
            break;

        // unconditional LOOP does not get "executed" but has statements inside
        case PLPGSQL_STMT_LOOP:
            walkStmtList(estate, ((PLpgSQL_stmt_loop *) stmt)->body, getStmtStats(estate, stmt)->num, 0);
            break;

        // conditional loops execute conditional expression
        case PLPGSQL_STMT_WHILE:
            walkStmtList(estate, ((PLpgSQL_stmt_while *) stmt)->body, getStmtStats(estate, stmt)->num, 0);
            break;

        case PLPGSQL_STMT_FORI:
            walkStmtList(estate, ((PLpgSQL_stmt_fori *) stmt)->body, getStmtStats(estate, stmt)->num, 0);
            break;

        case PLPGSQL_STMT_FORS:
            walkStmtList(estate, ((PLpgSQL_stmt_fors *) stmt)->body, getStmtStats(estate, stmt)->num, 0);
            break;

        case PLPGSQL_STMT_DYNFORS:
            walkStmtList(estate, ((PLpgSQL_stmt_dynfors *) stmt)->body, getStmtStats(estate, stmt)->num, 0);
            break;

#if PG_VERSION_NUM >= 80400
        case PLPGSQL_STMT_CASE:
            walkStmtCase(estate, (PLpgSQL_stmt_case *) stmt);
            break;

        case PLPGSQL_STMT_FORC:
            walkStmtList(estate, ((PLpgSQL_stmt_forc *) stmt)->body, getStmtStats(estate, stmt)->num, 0);
            break;
#endif

        // normal statement
        default:
            break;
    }
}

/* Hooks */

/* Called on function startup
 * build list of statements present in the function*/
static void func_setup( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
    CoverageCtx *ctx = (CoverageCtx *) palloc(sizeof(CoverageCtx));

    if (old_func_setup != NULL)
        old_func_setup(estate, func);

    ctx->stmtCount = 0;
    estate->plugin_info = ctx;
    walkStmt(estate, (PLpgSQL_stmt *) func->action, 0, 0);
}

/* Called on function execution end
 * saves stats */
static void func_end( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
    if (old_func_end != NULL)
        old_func_end(estate, func);

    if (statsTableName == NULL)
        return;

    updateStats(estate, func);
}

/* Called on each statement execution
 * updates stats for given statement */
static void stmt_begin( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
    StmtStats    * stats;

    if (old_stmt_beg != NULL)
        old_stmt_beg(estate, stmt);

    stats = getStmtStats( estate, stmt );
    stats->execCount++;
}


/* Check if our stats table exists, if not create it */
static void ensureStatsTable( void )
{
    RangeVar   *relVar;
    StringInfoData  cmd;
    const char     *createTableString =
            " CREATE TABLE %s ( "
            "  func_oid OID, "
            "  line_number INT, "
            "  stmt_number INT, "
            "  stmt_type TEXT, "
            "  exec_count INT8, "
            "  stmt_parent INT, "
            "  stmt_branch INT "
            " );";
    const char  *createIndexString =
            " CREATE UNIQUE INDEX coverage_uniq ON %s( func_oid, line_number );";

    relVar = makeRangeVarFromNameList(stringToQualifiedNameList(statsTableName));

    if (OidIsValid( RangeVarGetRelid( relVar, true )))
        return;

    initStringInfo( &cmd );
    appendStringInfo( &cmd, createTableString, statsTableName );
    SPI_exec( cmd.data, 0 );

    initStringInfo( &cmd );
    appendStringInfo( &cmd, createIndexString, statsTableName );
    SPI_exec( cmd.data, 0 );
}


/* Insert or Update stats in our table */
static void updateStats( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
    CoverageCtx * ctx = (CoverageCtx *) estate->plugin_info;
    int i;
    void *execPlan;
    Datum values[7];
    Oid argTypes[7] = {OIDOID, INT4OID, INT4OID, TEXTOID, INT8OID, INT4OID, INT4OID};
    Oid selectArgTypes[1] = { OIDOID };
    Datum selectValues[1] = { ObjectIdGetDatum( func->fn_oid ) };
    char *insertStmt = "INSERT INTO %s VALUES($1,$2,$3,$4,$5,$6,$7)";
    char *updateStmt = "UPDATE %s SET stmt_type = $4, exec_count = exec_count+$5 WHERE func_oid = $1 AND line_number = $2 AND stmt_number = $3";
    char *selectStmt = "SELECT func_oid FROM %s WHERE func_oid = $1 LIMIT 1";
    char *stmt;
    bool exists;

    // Make sure table exists
    ensureStatsTable();

    /* Check if stats for function are already in table so we know
     * if we want to update them or insert new ones */
    stmt =  palloc( strlen( selectStmt ) + strlen( statsTableName ));
    sprintf( stmt, selectStmt, statsTableName );

    execPlan = SPI_prepare( stmt, 1, selectArgTypes );

    if( SPI_execp( execPlan, selectValues, NULL, 1 ) != SPI_OK_SELECT )
    {
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                errmsg("plpgsql_coverage: error querying coverage data")));
    }

    exists = (SPI_processed > 0);

    pfree( stmt );
    SPI_pfree( execPlan );

    // Prepare insert or update plans depending of existance of stats for this functiion in the table
    if (exists)
    {
        stmt =  palloc( strlen( updateStmt ) + strlen( statsTableName ));
        sprintf( stmt, updateStmt, statsTableName );
    }
    else
    {
        stmt =  palloc( strlen( insertStmt ) + strlen( statsTableName ));
        sprintf( stmt, insertStmt, statsTableName );
    }

    execPlan = SPI_prepare( stmt, 7, argTypes );

    /* Update/Insert stats for every statement */
    for (i = 0; i < ctx->stmtCount; i++)
    {
        StmtStats *stats = ctx->stmtStats[i];
        int spiResult;

        size_t stmtTypeLen = strlen( plpgsql_stmt_typename(stats->stmt) );
        char * stmtType = palloc( VARHDRSZ + stmtTypeLen );

        memcpy( VARDATA( stmtType ), plpgsql_stmt_typename(stats->stmt), stmtTypeLen );
        SET_VARSIZE( stmtType , VARHDRSZ + stmtTypeLen );

        values[0] = ObjectIdGetDatum( func->fn_oid );
        values[1] = Int32GetDatum((int32) stats->stmt->lineno );
        values[2] = Int32GetDatum((int32) stats->num );
        values[3] = PointerGetDatum( stmtType );
        values[4] = Int64GetDatum( stats->execCount );
        values[5] = Int32GetDatum((int32) stats->parent );
        values[6] = Int32GetDatum((int32) stats->branch );

        spiResult = SPI_execp( execPlan, values, NULL, 1 );

        if ((exists && spiResult != SPI_OK_UPDATE) || (!exists && spiResult != SPI_OK_INSERT))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                    errmsg("plpgsql_coverage: error updating coverage data")));
        }

        pfree( stmtType );
    }

    pfree( stmt );
    SPI_pfree( execPlan );
}
