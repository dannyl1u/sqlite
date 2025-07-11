/*
** 2014 Jun 09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This is an SQLite module implementing full-text search.
*/


#include "fts5Int.h"

/*
** This variable is set to false when running tests for which the on disk
** structures should not be corrupt. Otherwise, true. If it is false, extra
** assert() conditions in the fts5 code are activated - conditions that are
** only true if it is guaranteed that the fts5 database is not corrupt.
*/
#ifdef SQLITE_DEBUG
int sqlite3_fts5_may_be_corrupt = 1;
#endif


typedef struct Fts5Auxdata Fts5Auxdata;
typedef struct Fts5Auxiliary Fts5Auxiliary;
typedef struct Fts5Cursor Fts5Cursor;
typedef struct Fts5FullTable Fts5FullTable;
typedef struct Fts5Sorter Fts5Sorter;
typedef struct Fts5TokenizerModule Fts5TokenizerModule;

/*
** NOTES ON TRANSACTIONS: 
**
** SQLite invokes the following virtual table methods as transactions are 
** opened and closed by the user:
**
**     xBegin():    Start of a new transaction.
**     xSync():     Initial part of two-phase commit.
**     xCommit():   Final part of two-phase commit.
**     xRollback(): Rollback the transaction.
**
** Anything that is required as part of a commit that may fail is performed
** in the xSync() callback. Current versions of SQLite ignore any errors 
** returned by xCommit().
**
** And as sub-transactions are opened/closed:
**
**     xSavepoint(int S):  Open savepoint S.
**     xRelease(int S):    Commit and close savepoint S.
**     xRollbackTo(int S): Rollback to start of savepoint S.
**
** During a write-transaction the fts5_index.c module may cache some data 
** in-memory. It is flushed to disk whenever xSync(), xRelease() or
** xSavepoint() is called. And discarded whenever xRollback() or xRollbackTo() 
** is called.
**
** Additionally, if SQLITE_DEBUG is defined, an instance of the following
** structure is used to record the current transaction state. This information
** is not required, but it is used in the assert() statements executed by
** function fts5CheckTransactionState() (see below).
*/
struct Fts5TransactionState {
  int eState;                     /* 0==closed, 1==open, 2==synced */
  int iSavepoint;                 /* Number of open savepoints (0 -> none) */
};

/*
** A single object of this type is allocated when the FTS5 module is 
** registered with a database handle. It is used to store pointers to
** all registered FTS5 extensions - tokenizers and auxiliary functions.
*/
struct Fts5Global {
  fts5_api api;                   /* User visible part of object (see fts5.h) */
  sqlite3 *db;                    /* Associated database connection */ 
  i64 iNextId;                    /* Used to allocate unique cursor ids */
  Fts5Auxiliary *pAux;            /* First in list of all aux. functions */
  Fts5TokenizerModule *pTok;      /* First in list of all tokenizer modules */
  Fts5TokenizerModule *pDfltTok;  /* Default tokenizer module */
  Fts5Cursor *pCsr;               /* First in list of all open cursors */
  u32 aLocaleHdr[4];
};

/*
** Size of header on fts5_locale() values. And macro to access a buffer
** containing a copy of the header from an Fts5Config pointer.
*/
#define FTS5_LOCALE_HDR_SIZE ((int)sizeof( ((Fts5Global*)0)->aLocaleHdr ))
#define FTS5_LOCALE_HDR(pConfig) ((const u8*)(pConfig->pGlobal->aLocaleHdr))

#define FTS5_INSTTOKEN_SUBTYPE 73

/*
** Each auxiliary function registered with the FTS5 module is represented
** by an object of the following type. All such objects are stored as part
** of the Fts5Global.pAux list.
*/
struct Fts5Auxiliary {
  Fts5Global *pGlobal;            /* Global context for this function */
  char *zFunc;                    /* Function name (nul-terminated) */
  void *pUserData;                /* User-data pointer */
  fts5_extension_function xFunc;  /* Callback function */
  void (*xDestroy)(void*);        /* Destructor function */
  Fts5Auxiliary *pNext;           /* Next registered auxiliary function */
};

/*
** Each tokenizer module registered with the FTS5 module is represented
** by an object of the following type. All such objects are stored as part
** of the Fts5Global.pTok list.
**
** bV2Native:
**  True if the tokenizer was registered using xCreateTokenizer_v2(), false
**  for xCreateTokenizer(). If this variable is true, then x2 is populated
**  with the routines as supplied by the caller and x1 contains synthesized
**  wrapper routines. In this case the user-data pointer passed to 
**  x1.xCreate should be a pointer to the Fts5TokenizerModule structure,
**  not a copy of pUserData.
**
**  Of course, if bV2Native is false, then x1 contains the real routines and
**  x2 the synthesized ones. In this case a pointer to the Fts5TokenizerModule
**  object should be passed to x2.xCreate.
**
**  The synthesized wrapper routines are necessary for xFindTokenizer(_v2)
**  calls.
*/
struct Fts5TokenizerModule {
  char *zName;                    /* Name of tokenizer */
  void *pUserData;                /* User pointer passed to xCreate() */
  int bV2Native;                  /* True if v2 native tokenizer */
  fts5_tokenizer x1;              /* Tokenizer functions */
  fts5_tokenizer_v2 x2;           /* V2 tokenizer functions */
  void (*xDestroy)(void*);        /* Destructor function */
  Fts5TokenizerModule *pNext;     /* Next registered tokenizer module */
};

struct Fts5FullTable {
  Fts5Table p;                    /* Public class members from fts5Int.h */
  Fts5Storage *pStorage;          /* Document store */
  Fts5Global *pGlobal;            /* Global (connection wide) data */
  Fts5Cursor *pSortCsr;           /* Sort data from this cursor */
  int iSavepoint;                 /* Successful xSavepoint()+1 */

#ifdef SQLITE_DEBUG
  struct Fts5TransactionState ts;
#endif
};

struct Fts5MatchPhrase {
  Fts5Buffer *pPoslist;           /* Pointer to current poslist */
  int nTerm;                      /* Size of phrase in terms */
};

/*
** pStmt:
**   SELECT rowid, <fts> FROM <fts> ORDER BY +rank;
**
** aIdx[]:
**   There is one entry in the aIdx[] array for each phrase in the query,
**   the value of which is the offset within aPoslist[] following the last 
**   byte of the position list for the corresponding phrase.
*/
struct Fts5Sorter {
  sqlite3_stmt *pStmt;
  i64 iRowid;                     /* Current rowid */
  const u8 *aPoslist;             /* Position lists for current row */
  int nIdx;                       /* Number of entries in aIdx[] */
  int aIdx[FLEXARRAY];            /* Offsets into aPoslist for current row */
};

/* Size (int bytes) of an Fts5Sorter object with N indexes */
#define SZ_FTS5SORTER(N) (offsetof(Fts5Sorter,nIdx)+((N+2)/2)*sizeof(i64))

/*
** Virtual-table cursor object.
**
** iSpecial:
**   If this is a 'special' query (refer to function fts5SpecialMatch()), 
**   then this variable contains the result of the query. 
**
** iFirstRowid, iLastRowid:
**   These variables are only used for FTS5_PLAN_MATCH cursors. Assuming the
**   cursor iterates in ascending order of rowids, iFirstRowid is the lower
**   limit of rowids to return, and iLastRowid the upper. In other words, the
**   WHERE clause in the user's query might have been:
**
**       <tbl> MATCH <expr> AND rowid BETWEEN $iFirstRowid AND $iLastRowid
**
**   If the cursor iterates in descending order of rowid, iFirstRowid
**   is the upper limit (i.e. the "first" rowid visited) and iLastRowid
**   the lower.
*/
struct Fts5Cursor {
  sqlite3_vtab_cursor base;       /* Base class used by SQLite core */
  Fts5Cursor *pNext;              /* Next cursor in Fts5Cursor.pCsr list */
  int *aColumnSize;               /* Values for xColumnSize() */
  i64 iCsrId;                     /* Cursor id */

  /* Zero from this point onwards on cursor reset */
  int ePlan;                      /* FTS5_PLAN_XXX value */
  int bDesc;                      /* True for "ORDER BY rowid DESC" queries */
  i64 iFirstRowid;                /* Return no rowids earlier than this */
  i64 iLastRowid;                 /* Return no rowids later than this */
  sqlite3_stmt *pStmt;            /* Statement used to read %_content */
  Fts5Expr *pExpr;                /* Expression for MATCH queries */
  Fts5Sorter *pSorter;            /* Sorter for "ORDER BY rank" queries */
  int csrflags;                   /* Mask of cursor flags (see below) */
  i64 iSpecial;                   /* Result of special query */

  /* "rank" function. Populated on demand from vtab.xColumn(). */
  char *zRank;                    /* Custom rank function */
  char *zRankArgs;                /* Custom rank function args */
  Fts5Auxiliary *pRank;           /* Rank callback (or NULL) */
  int nRankArg;                   /* Number of trailing arguments for rank() */
  sqlite3_value **apRankArg;      /* Array of trailing arguments */
  sqlite3_stmt *pRankArgStmt;     /* Origin of objects in apRankArg[] */

  /* Auxiliary data storage */
  Fts5Auxiliary *pAux;            /* Currently executing extension function */
  Fts5Auxdata *pAuxdata;          /* First in linked list of saved aux-data */

  /* Cache used by auxiliary API functions xInst() and xInstCount() */
  Fts5PoslistReader *aInstIter;   /* One for each phrase */
  int nInstAlloc;                 /* Size of aInst[] array (entries / 3) */
  int nInstCount;                 /* Number of phrase instances */
  int *aInst;                     /* 3 integers per phrase instance */
};

/*
** Bits that make up the "idxNum" parameter passed indirectly by 
** xBestIndex() to xFilter().
*/
#define FTS5_BI_MATCH        0x0001         /* <tbl> MATCH ? */
#define FTS5_BI_RANK         0x0002         /* rank MATCH ? */
#define FTS5_BI_ROWID_EQ     0x0004         /* rowid == ? */
#define FTS5_BI_ROWID_LE     0x0008         /* rowid <= ? */
#define FTS5_BI_ROWID_GE     0x0010         /* rowid >= ? */

#define FTS5_BI_ORDER_RANK   0x0020
#define FTS5_BI_ORDER_ROWID  0x0040
#define FTS5_BI_ORDER_DESC   0x0080

/*
** Values for Fts5Cursor.csrflags
*/
#define FTS5CSR_EOF               0x01
#define FTS5CSR_REQUIRE_CONTENT   0x02
#define FTS5CSR_REQUIRE_DOCSIZE   0x04
#define FTS5CSR_REQUIRE_INST      0x08
#define FTS5CSR_FREE_ZRANK        0x10
#define FTS5CSR_REQUIRE_RESEEK    0x20
#define FTS5CSR_REQUIRE_POSLIST   0x40

#define BitFlagAllTest(x,y) (((x) & (y))==(y))
#define BitFlagTest(x,y)    (((x) & (y))!=0)


/*
** Macros to Set(), Clear() and Test() cursor flags.
*/
#define CsrFlagSet(pCsr, flag)   ((pCsr)->csrflags |= (flag))
#define CsrFlagClear(pCsr, flag) ((pCsr)->csrflags &= ~(flag))
#define CsrFlagTest(pCsr, flag)  ((pCsr)->csrflags & (flag))

struct Fts5Auxdata {
  Fts5Auxiliary *pAux;            /* Extension to which this belongs */
  void *pPtr;                     /* Pointer value */
  void(*xDelete)(void*);          /* Destructor */
  Fts5Auxdata *pNext;             /* Next object in linked list */
};

#ifdef SQLITE_DEBUG
#define FTS5_BEGIN      1
#define FTS5_SYNC       2
#define FTS5_COMMIT     3
#define FTS5_ROLLBACK   4
#define FTS5_SAVEPOINT  5
#define FTS5_RELEASE    6
#define FTS5_ROLLBACKTO 7
static void fts5CheckTransactionState(Fts5FullTable *p, int op, int iSavepoint){
  switch( op ){
    case FTS5_BEGIN:
      assert( p->ts.eState==0 );
      p->ts.eState = 1;
      p->ts.iSavepoint = -1;
      break;

    case FTS5_SYNC:
      assert( p->ts.eState==1 || p->ts.eState==2 );
      p->ts.eState = 2;
      break;

    case FTS5_COMMIT:
      assert( p->ts.eState==2 );
      p->ts.eState = 0;
      break;

    case FTS5_ROLLBACK:
      assert( p->ts.eState==1 || p->ts.eState==2 || p->ts.eState==0 );
      p->ts.eState = 0;
      break;

    case FTS5_SAVEPOINT:
      assert( p->ts.eState>=1 );
      assert( iSavepoint>=0 );
      assert( iSavepoint>=p->ts.iSavepoint );
      p->ts.iSavepoint = iSavepoint;
      break;
      
    case FTS5_RELEASE:
      assert( p->ts.eState>=1 );
      assert( iSavepoint>=0 );
      assert( iSavepoint<=p->ts.iSavepoint );
      p->ts.iSavepoint = iSavepoint-1;
      break;

    case FTS5_ROLLBACKTO:
      assert( p->ts.eState>=1 );
      assert( iSavepoint>=-1 );
      /* The following assert() can fail if another vtab strikes an error
      ** within an xSavepoint() call then SQLite calls xRollbackTo() - without
      ** having called xSavepoint() on this vtab.  */
      /* assert( iSavepoint<=p->ts.iSavepoint ); */
      p->ts.iSavepoint = iSavepoint;
      break;
  }
}
#else
# define fts5CheckTransactionState(x,y,z)
#endif

/*
** Return true if pTab is a contentless table. If parameter bIncludeUnindexed
** is true, this includes contentless tables that store UNINDEXED columns
** only.
*/
static int fts5IsContentless(Fts5FullTable *pTab, int bIncludeUnindexed){
  int eContent = pTab->p.pConfig->eContent;
  return (
    eContent==FTS5_CONTENT_NONE 
    || (bIncludeUnindexed && eContent==FTS5_CONTENT_UNINDEXED)
  );
}

/*
** Delete a virtual table handle allocated by fts5InitVtab(). 
*/
static void fts5FreeVtab(Fts5FullTable *pTab){
  if( pTab ){
    sqlite3Fts5IndexClose(pTab->p.pIndex);
    sqlite3Fts5StorageClose(pTab->pStorage);
    sqlite3Fts5ConfigFree(pTab->p.pConfig);
    sqlite3_free(pTab);
  }
}

/*
** The xDisconnect() virtual table method.
*/
static int fts5DisconnectMethod(sqlite3_vtab *pVtab){
  fts5FreeVtab((Fts5FullTable*)pVtab);
  return SQLITE_OK;
}

/*
** The xDestroy() virtual table method.
*/
static int fts5DestroyMethod(sqlite3_vtab *pVtab){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  int rc = sqlite3Fts5DropAll(pTab->pConfig);
  if( rc==SQLITE_OK ){
    fts5FreeVtab((Fts5FullTable*)pVtab);
  }
  return rc;
}

/*
** This function is the implementation of both the xConnect and xCreate
** methods of the FTS3 virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fts5")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> "column name" and other module argument fields.
*/
static int fts5InitVtab(
  int bCreate,                    /* True for xCreate, false for xConnect */
  sqlite3 *db,                    /* The SQLite database connection */
  void *pAux,                     /* Hash table containing tokenizers */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVTab,          /* Write the resulting vtab structure here */
  char **pzErr                    /* Write any error message here */
){
  Fts5Global *pGlobal = (Fts5Global*)pAux;
  const char **azConfig = (const char**)argv;
  int rc = SQLITE_OK;             /* Return code */
  Fts5Config *pConfig = 0;        /* Results of parsing argc/argv */
  Fts5FullTable *pTab = 0;        /* New virtual table object */

  /* Allocate the new vtab object and parse the configuration */
  pTab = (Fts5FullTable*)sqlite3Fts5MallocZero(&rc, sizeof(Fts5FullTable));
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5ConfigParse(pGlobal, db, argc, azConfig, &pConfig, pzErr);
    assert( (rc==SQLITE_OK && *pzErr==0) || pConfig==0 );
  }
  if( rc==SQLITE_OK ){
    pConfig->pzErrmsg = pzErr;
    pTab->p.pConfig = pConfig;
    pTab->pGlobal = pGlobal;
    if( bCreate || sqlite3Fts5TokenizerPreload(&pConfig->t) ){
      rc = sqlite3Fts5LoadTokenizer(pConfig);
    }
  }

  /* Open the index sub-system */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5IndexOpen(pConfig, bCreate, &pTab->p.pIndex, pzErr);
  }

  /* Open the storage sub-system */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5StorageOpen(
        pConfig, pTab->p.pIndex, bCreate, &pTab->pStorage, pzErr
    );
  }

  /* Call sqlite3_declare_vtab() */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5ConfigDeclareVtab(pConfig);
  }

  /* Load the initial configuration */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5ConfigLoad(pTab->p.pConfig, pTab->p.pConfig->iCookie-1);
  }

  if( rc==SQLITE_OK && pConfig->eContent==FTS5_CONTENT_NORMAL ){
    rc = sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, (int)1);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_vtab_config(db, SQLITE_VTAB_INNOCUOUS);
  }

  if( pConfig ) pConfig->pzErrmsg = 0;
  if( rc!=SQLITE_OK ){
    fts5FreeVtab(pTab);
    pTab = 0;
  }else if( bCreate ){
    fts5CheckTransactionState(pTab, FTS5_BEGIN, 0);
  }
  *ppVTab = (sqlite3_vtab*)pTab;
  return rc;
}

/*
** The xConnect() and xCreate() methods for the virtual table. All the
** work is done in function fts5InitVtab().
*/
static int fts5ConnectMethod(
  sqlite3 *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  return fts5InitVtab(0, db, pAux, argc, argv, ppVtab, pzErr);
}
static int fts5CreateMethod(
  sqlite3 *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  return fts5InitVtab(1, db, pAux, argc, argv, ppVtab, pzErr);
}

/*
** The different query plans.
*/
#define FTS5_PLAN_MATCH          1       /* (<tbl> MATCH ?) */
#define FTS5_PLAN_SOURCE         2       /* A source cursor for SORTED_MATCH */
#define FTS5_PLAN_SPECIAL        3       /* An internal query */
#define FTS5_PLAN_SORTED_MATCH   4       /* (<tbl> MATCH ? ORDER BY rank) */
#define FTS5_PLAN_SCAN           5       /* No usable constraint */
#define FTS5_PLAN_ROWID          6       /* (rowid = ?) */

/*
** Set the SQLITE_INDEX_SCAN_UNIQUE flag in pIdxInfo->flags. Unless this
** extension is currently being used by a version of SQLite too old to
** support index-info flags. In that case this function is a no-op.
*/
static void fts5SetUniqueFlag(sqlite3_index_info *pIdxInfo){
#if SQLITE_VERSION_NUMBER>=3008012
#ifndef SQLITE_CORE
  if( sqlite3_libversion_number()>=3008012 )
#endif
  {
    pIdxInfo->idxFlags |= SQLITE_INDEX_SCAN_UNIQUE;
  }
#endif
}

static int fts5UsePatternMatch(
  Fts5Config *pConfig, 
  struct sqlite3_index_constraint *p
){
  assert( FTS5_PATTERN_GLOB==SQLITE_INDEX_CONSTRAINT_GLOB );
  assert( FTS5_PATTERN_LIKE==SQLITE_INDEX_CONSTRAINT_LIKE );
  if( pConfig->t.ePattern==FTS5_PATTERN_GLOB && p->op==FTS5_PATTERN_GLOB ){
    return 1;
  }
  if( pConfig->t.ePattern==FTS5_PATTERN_LIKE 
   && (p->op==FTS5_PATTERN_LIKE || p->op==FTS5_PATTERN_GLOB)
  ){
    return 1;
  }
  return 0;
}

/*
** Implementation of the xBestIndex method for FTS5 tables. Within the 
** WHERE constraint, it searches for the following:
**
**   1. A MATCH constraint against the table column.
**   2. A MATCH constraint against the "rank" column.
**   3. A MATCH constraint against some other column.
**   4. An == constraint against the rowid column.
**   5. A < or <= constraint against the rowid column.
**   6. A > or >= constraint against the rowid column.
**
** Within the ORDER BY, the following are supported:
**
**   5. ORDER BY rank [ASC|DESC]
**   6. ORDER BY rowid [ASC|DESC]
**
** Information for the xFilter call is passed via both the idxNum and 
** idxStr variables. Specifically, idxNum is a bitmask of the following
** flags used to encode the ORDER BY clause:
**
**     FTS5_BI_ORDER_RANK
**     FTS5_BI_ORDER_ROWID
**     FTS5_BI_ORDER_DESC
**
** idxStr is used to encode data from the WHERE clause. For each argument
** passed to the xFilter method, the following is appended to idxStr:
**
**   Match against table column:            "m"
**   Match against rank column:             "r"
**   Match against other column:            "M<column-number>"
**   LIKE  against other column:            "L<column-number>"
**   GLOB  against other column:            "G<column-number>"
**   Equality constraint against the rowid: "="
**   A < or <= against the rowid:           "<"
**   A > or >= against the rowid:           ">"
**
** This function ensures that there is at most one "r" or "=". And that if
** there exists an "=" then there is no "<" or ">".
**
** If an unusable MATCH operator is present in the WHERE clause, then
** SQLITE_CONSTRAINT is returned.
**
** Costs are assigned as follows:
**
**  a) If a MATCH operator is present, the cost depends on the other
**     constraints also present. As follows:
**
**       * No other constraints:         cost=1000.0
**       * One rowid range constraint:   cost=750.0
**       * Both rowid range constraints: cost=500.0
**       * An == rowid constraint:       cost=100.0
**
**  b) Otherwise, if there is no MATCH:
**
**       * No other constraints:         cost=1000000.0
**       * One rowid range constraint:   cost=750000.0
**       * Both rowid range constraints: cost=250000.0
**       * An == rowid constraint:       cost=10.0
**
** Costs are not modified by the ORDER BY clause.
*/
static int fts5BestIndexMethod(sqlite3_vtab *pVTab, sqlite3_index_info *pInfo){
  Fts5Table *pTab = (Fts5Table*)pVTab;
  Fts5Config *pConfig = pTab->pConfig;
  const int nCol = pConfig->nCol;
  int idxFlags = 0;               /* Parameter passed through to xFilter() */
  int i;

  char *idxStr;
  int iIdxStr = 0;
  int iCons = 0;

  int bSeenEq = 0;
  int bSeenGt = 0;
  int bSeenLt = 0;
  int nSeenMatch = 0;
  int bSeenRank = 0;


  assert( SQLITE_INDEX_CONSTRAINT_EQ<SQLITE_INDEX_CONSTRAINT_MATCH );
  assert( SQLITE_INDEX_CONSTRAINT_GT<SQLITE_INDEX_CONSTRAINT_MATCH );
  assert( SQLITE_INDEX_CONSTRAINT_LE<SQLITE_INDEX_CONSTRAINT_MATCH );
  assert( SQLITE_INDEX_CONSTRAINT_GE<SQLITE_INDEX_CONSTRAINT_MATCH );
  assert( SQLITE_INDEX_CONSTRAINT_LE<SQLITE_INDEX_CONSTRAINT_MATCH );

  if( pConfig->bLock ){
    pTab->base.zErrMsg = sqlite3_mprintf(
        "recursively defined fts5 content table"
    );
    return SQLITE_ERROR;
  }

  idxStr = (char*)sqlite3_malloc(pInfo->nConstraint * 8 + 1);
  if( idxStr==0 ) return SQLITE_NOMEM;
  pInfo->idxStr = idxStr;
  pInfo->needToFreeIdxStr = 1;

  for(i=0; i<pInfo->nConstraint; i++){
    struct sqlite3_index_constraint *p = &pInfo->aConstraint[i];
    int iCol = p->iColumn;
    if( p->op==SQLITE_INDEX_CONSTRAINT_MATCH
     || (p->op==SQLITE_INDEX_CONSTRAINT_EQ && iCol>=nCol)
    ){
      /* A MATCH operator or equivalent */
      if( p->usable==0 || iCol<0 ){
        /* As there exists an unusable MATCH constraint this is an 
        ** unusable plan. Return SQLITE_CONSTRAINT. */
        idxStr[iIdxStr] = 0;
        return SQLITE_CONSTRAINT;
      }else{
        if( iCol==nCol+1 ){
          if( bSeenRank ) continue;
          idxStr[iIdxStr++] = 'r';
          bSeenRank = 1;
        }else{
          nSeenMatch++;
          idxStr[iIdxStr++] = 'M';
          sqlite3_snprintf(6, &idxStr[iIdxStr], "%d", iCol);
          idxStr += strlen(&idxStr[iIdxStr]);
          assert( idxStr[iIdxStr]=='\0' );
        }
        pInfo->aConstraintUsage[i].argvIndex = ++iCons;
        pInfo->aConstraintUsage[i].omit = 1;
      }
    }else if( p->usable ){
      if( iCol>=0 && iCol<nCol && fts5UsePatternMatch(pConfig, p) ){
        assert( p->op==FTS5_PATTERN_LIKE || p->op==FTS5_PATTERN_GLOB );
        idxStr[iIdxStr++] = p->op==FTS5_PATTERN_LIKE ? 'L' : 'G';
        sqlite3_snprintf(6, &idxStr[iIdxStr], "%d", iCol);
        idxStr += strlen(&idxStr[iIdxStr]);
        pInfo->aConstraintUsage[i].argvIndex = ++iCons;
        assert( idxStr[iIdxStr]=='\0' );
        nSeenMatch++;
      }else if( bSeenEq==0 && p->op==SQLITE_INDEX_CONSTRAINT_EQ && iCol<0 ){
        idxStr[iIdxStr++] = '=';
        bSeenEq = 1;
        pInfo->aConstraintUsage[i].argvIndex = ++iCons;
      }
    }
  }

  if( bSeenEq==0 ){
    for(i=0; i<pInfo->nConstraint; i++){
      struct sqlite3_index_constraint *p = &pInfo->aConstraint[i];
      if( p->iColumn<0 && p->usable ){
        int op = p->op;
        if( op==SQLITE_INDEX_CONSTRAINT_LT || op==SQLITE_INDEX_CONSTRAINT_LE ){
          if( bSeenLt ) continue;
          idxStr[iIdxStr++] = '<';
          pInfo->aConstraintUsage[i].argvIndex = ++iCons;
          bSeenLt = 1;
        }else
        if( op==SQLITE_INDEX_CONSTRAINT_GT || op==SQLITE_INDEX_CONSTRAINT_GE ){
          if( bSeenGt ) continue;
          idxStr[iIdxStr++] = '>';
          pInfo->aConstraintUsage[i].argvIndex = ++iCons;
          bSeenGt = 1;
        }
      }
    }
  }
  idxStr[iIdxStr] = '\0';

  /* Set idxFlags flags for the ORDER BY clause
  **
  ** Note that tokendata=1 tables cannot currently handle "ORDER BY rowid DESC".
  */
  if( pInfo->nOrderBy==1 ){
    int iSort = pInfo->aOrderBy[0].iColumn;
    if( iSort==(pConfig->nCol+1) && nSeenMatch>0 ){
      idxFlags |= FTS5_BI_ORDER_RANK;
    }else if( iSort==-1 && (!pInfo->aOrderBy[0].desc || !pConfig->bTokendata) ){
      idxFlags |= FTS5_BI_ORDER_ROWID;
    }
    if( BitFlagTest(idxFlags, FTS5_BI_ORDER_RANK|FTS5_BI_ORDER_ROWID) ){
      pInfo->orderByConsumed = 1;
      if( pInfo->aOrderBy[0].desc ){
        idxFlags |= FTS5_BI_ORDER_DESC;
      }
    }
  }

  /* Calculate the estimated cost based on the flags set in idxFlags. */
  if( bSeenEq ){
    pInfo->estimatedCost = nSeenMatch ? 1000.0 : 10.0;
    if( nSeenMatch==0 ) fts5SetUniqueFlag(pInfo);
  }else if( bSeenLt && bSeenGt ){
    pInfo->estimatedCost = nSeenMatch ? 5000.0 : 250000.0;
  }else if( bSeenLt || bSeenGt ){
    pInfo->estimatedCost = nSeenMatch ? 7500.0 : 750000.0;
  }else{
    pInfo->estimatedCost = nSeenMatch ? 10000.0 : 1000000.0;
  }
  for(i=1; i<nSeenMatch; i++){
    pInfo->estimatedCost *= 0.4;
  }

  pInfo->idxNum = idxFlags;
  return SQLITE_OK;
}

static int fts5NewTransaction(Fts5FullTable *pTab){
  Fts5Cursor *pCsr;
  for(pCsr=pTab->pGlobal->pCsr; pCsr; pCsr=pCsr->pNext){
    if( pCsr->base.pVtab==(sqlite3_vtab*)pTab ) return SQLITE_OK;
  }
  return sqlite3Fts5StorageReset(pTab->pStorage);
}

/*
** Implementation of xOpen method.
*/
static int fts5OpenMethod(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCsr){
  Fts5FullTable *pTab = (Fts5FullTable*)pVTab;
  Fts5Config *pConfig = pTab->p.pConfig;
  Fts5Cursor *pCsr = 0;           /* New cursor object */
  sqlite3_int64 nByte;            /* Bytes of space to allocate */
  int rc;                         /* Return code */

  rc = fts5NewTransaction(pTab);
  if( rc==SQLITE_OK ){
    nByte = sizeof(Fts5Cursor) + pConfig->nCol * sizeof(int);
    pCsr = (Fts5Cursor*)sqlite3_malloc64(nByte);
    if( pCsr ){
      Fts5Global *pGlobal = pTab->pGlobal;
      memset(pCsr, 0, (size_t)nByte);
      pCsr->aColumnSize = (int*)&pCsr[1];
      pCsr->pNext = pGlobal->pCsr;
      pGlobal->pCsr = pCsr;
      pCsr->iCsrId = ++pGlobal->iNextId;
    }else{
      rc = SQLITE_NOMEM;
    }
  }
  *ppCsr = (sqlite3_vtab_cursor*)pCsr;
  return rc;
}

static int fts5StmtType(Fts5Cursor *pCsr){
  if( pCsr->ePlan==FTS5_PLAN_SCAN ){
    return (pCsr->bDesc) ? FTS5_STMT_SCAN_DESC : FTS5_STMT_SCAN_ASC;
  }
  return FTS5_STMT_LOOKUP;
}

/*
** This function is called after the cursor passed as the only argument
** is moved to point at a different row. It clears all cached data 
** specific to the previous row stored by the cursor object.
*/
static void fts5CsrNewrow(Fts5Cursor *pCsr){
  CsrFlagSet(pCsr, 
      FTS5CSR_REQUIRE_CONTENT 
    | FTS5CSR_REQUIRE_DOCSIZE 
    | FTS5CSR_REQUIRE_INST 
    | FTS5CSR_REQUIRE_POSLIST 
  );
}

static void fts5FreeCursorComponents(Fts5Cursor *pCsr){
  Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
  Fts5Auxdata *pData;
  Fts5Auxdata *pNext;

  sqlite3_free(pCsr->aInstIter);
  sqlite3_free(pCsr->aInst);
  if( pCsr->pStmt ){
    int eStmt = fts5StmtType(pCsr);
    sqlite3Fts5StorageStmtRelease(pTab->pStorage, eStmt, pCsr->pStmt);
  }
  if( pCsr->pSorter ){
    Fts5Sorter *pSorter = pCsr->pSorter;
    sqlite3_finalize(pSorter->pStmt);
    sqlite3_free(pSorter);
  }

  if( pCsr->ePlan!=FTS5_PLAN_SOURCE ){
    sqlite3Fts5ExprFree(pCsr->pExpr);
  }

  for(pData=pCsr->pAuxdata; pData; pData=pNext){
    pNext = pData->pNext;
    if( pData->xDelete ) pData->xDelete(pData->pPtr);
    sqlite3_free(pData);
  }

  sqlite3_finalize(pCsr->pRankArgStmt);
  sqlite3_free(pCsr->apRankArg);

  if( CsrFlagTest(pCsr, FTS5CSR_FREE_ZRANK) ){
    sqlite3_free(pCsr->zRank);
    sqlite3_free(pCsr->zRankArgs);
  }

  sqlite3Fts5IndexCloseReader(pTab->p.pIndex);
  memset(&pCsr->ePlan, 0, sizeof(Fts5Cursor) - ((u8*)&pCsr->ePlan - (u8*)pCsr));
}


/*
** Close the cursor.  For additional information see the documentation
** on the xClose method of the virtual table interface.
*/
static int fts5CloseMethod(sqlite3_vtab_cursor *pCursor){
  if( pCursor ){
    Fts5FullTable *pTab = (Fts5FullTable*)(pCursor->pVtab);
    Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
    Fts5Cursor **pp;

    fts5FreeCursorComponents(pCsr);
    /* Remove the cursor from the Fts5Global.pCsr list */
    for(pp=&pTab->pGlobal->pCsr; (*pp)!=pCsr; pp=&(*pp)->pNext);
    *pp = pCsr->pNext;

    sqlite3_free(pCsr);
  }
  return SQLITE_OK;
}

static int fts5SorterNext(Fts5Cursor *pCsr){
  Fts5Sorter *pSorter = pCsr->pSorter;
  int rc;

  rc = sqlite3_step(pSorter->pStmt);
  if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
    CsrFlagSet(pCsr, FTS5CSR_EOF|FTS5CSR_REQUIRE_CONTENT);
  }else if( rc==SQLITE_ROW ){
    const u8 *a;
    const u8 *aBlob;
    int nBlob;
    int i;
    int iOff = 0;
    rc = SQLITE_OK;

    pSorter->iRowid = sqlite3_column_int64(pSorter->pStmt, 0);
    nBlob = sqlite3_column_bytes(pSorter->pStmt, 1);
    aBlob = a = sqlite3_column_blob(pSorter->pStmt, 1);

    /* nBlob==0 in detail=none mode. */
    if( nBlob>0 ){
      for(i=0; i<(pSorter->nIdx-1); i++){
        int iVal;
        a += fts5GetVarint32(a, iVal);
        iOff += iVal;
        pSorter->aIdx[i] = iOff;
      }
      pSorter->aIdx[i] = &aBlob[nBlob] - a;
      pSorter->aPoslist = a;
    }

    fts5CsrNewrow(pCsr);
  }

  return rc;
}


/*
** Set the FTS5CSR_REQUIRE_RESEEK flag on all FTS5_PLAN_MATCH cursors 
** open on table pTab.
*/
static void fts5TripCursors(Fts5FullTable *pTab){
  Fts5Cursor *pCsr;
  for(pCsr=pTab->pGlobal->pCsr; pCsr; pCsr=pCsr->pNext){
    if( pCsr->ePlan==FTS5_PLAN_MATCH
     && pCsr->base.pVtab==(sqlite3_vtab*)pTab 
    ){
      CsrFlagSet(pCsr, FTS5CSR_REQUIRE_RESEEK);
    }
  }
}

/*
** If the REQUIRE_RESEEK flag is set on the cursor passed as the first
** argument, close and reopen all Fts5IndexIter iterators that the cursor 
** is using. Then attempt to move the cursor to a rowid equal to or laster
** (in the cursors sort order - ASC or DESC) than the current rowid. 
**
** If the new rowid is not equal to the old, set output parameter *pbSkip
** to 1 before returning. Otherwise, leave it unchanged.
**
** Return SQLITE_OK if successful or if no reseek was required, or an 
** error code if an error occurred.
*/
static int fts5CursorReseek(Fts5Cursor *pCsr, int *pbSkip){
  int rc = SQLITE_OK;
  assert( *pbSkip==0 );
  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_RESEEK) ){
    Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
    int bDesc = pCsr->bDesc;
    i64 iRowid = sqlite3Fts5ExprRowid(pCsr->pExpr);

    rc = sqlite3Fts5ExprFirst(pCsr->pExpr, pTab->p.pIndex, iRowid, bDesc);
    if( rc==SQLITE_OK &&  iRowid!=sqlite3Fts5ExprRowid(pCsr->pExpr) ){
      *pbSkip = 1;
    }

    CsrFlagClear(pCsr, FTS5CSR_REQUIRE_RESEEK);
    fts5CsrNewrow(pCsr);
    if( sqlite3Fts5ExprEof(pCsr->pExpr) ){
      CsrFlagSet(pCsr, FTS5CSR_EOF);
      *pbSkip = 1;
    }
  }
  return rc;
}


/*
** Advance the cursor to the next row in the table that matches the 
** search criteria.
**
** Return SQLITE_OK if nothing goes wrong.  SQLITE_OK is returned
** even if we reach end-of-file.  The fts5EofMethod() will be called
** subsequently to determine whether or not an EOF was hit.
*/
static int fts5NextMethod(sqlite3_vtab_cursor *pCursor){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int rc;

  assert( (pCsr->ePlan<3)==
          (pCsr->ePlan==FTS5_PLAN_MATCH || pCsr->ePlan==FTS5_PLAN_SOURCE) 
  );
  assert( !CsrFlagTest(pCsr, FTS5CSR_EOF) );

  /* If this cursor uses FTS5_PLAN_MATCH and this is a tokendata=1 table,
  ** clear any token mappings accumulated at the fts5_index.c level. In
  ** other cases, specifically FTS5_PLAN_SOURCE and FTS5_PLAN_SORTED_MATCH,
  ** we need to retain the mappings for the entire query.  */
  if( pCsr->ePlan==FTS5_PLAN_MATCH 
   && ((Fts5Table*)pCursor->pVtab)->pConfig->bTokendata 
  ){
    sqlite3Fts5ExprClearTokens(pCsr->pExpr);
  }

  if( pCsr->ePlan<3 ){
    int bSkip = 0;
    if( (rc = fts5CursorReseek(pCsr, &bSkip)) || bSkip ) return rc;
    rc = sqlite3Fts5ExprNext(pCsr->pExpr, pCsr->iLastRowid);
    CsrFlagSet(pCsr, sqlite3Fts5ExprEof(pCsr->pExpr));
    fts5CsrNewrow(pCsr);
  }else{
    switch( pCsr->ePlan ){
      case FTS5_PLAN_SPECIAL: {
        CsrFlagSet(pCsr, FTS5CSR_EOF);
        rc = SQLITE_OK;
        break;
      }
  
      case FTS5_PLAN_SORTED_MATCH: {
        rc = fts5SorterNext(pCsr);
        break;
      }
  
      default: {
        Fts5Config *pConfig = ((Fts5Table*)pCursor->pVtab)->pConfig;
        pConfig->bLock++;
        rc = sqlite3_step(pCsr->pStmt);
        pConfig->bLock--;
        if( rc!=SQLITE_ROW ){
          CsrFlagSet(pCsr, FTS5CSR_EOF);
          rc = sqlite3_reset(pCsr->pStmt);
          if( rc!=SQLITE_OK ){
            pCursor->pVtab->zErrMsg = sqlite3_mprintf(
                "%s", sqlite3_errmsg(pConfig->db)
            );
          }
        }else{
          rc = SQLITE_OK;
          CsrFlagSet(pCsr, FTS5CSR_REQUIRE_DOCSIZE);
        }
        break;
      }
    }
  }
  
  return rc;
}


static int fts5PrepareStatement(
  sqlite3_stmt **ppStmt,
  Fts5Config *pConfig, 
  const char *zFmt,
  ...
){
  sqlite3_stmt *pRet = 0;
  int rc;
  char *zSql;
  va_list ap;

  va_start(ap, zFmt);
  zSql = sqlite3_vmprintf(zFmt, ap);
  if( zSql==0 ){
    rc = SQLITE_NOMEM; 
  }else{
    rc = sqlite3_prepare_v3(pConfig->db, zSql, -1, 
                            SQLITE_PREPARE_PERSISTENT, &pRet, 0);
    if( rc!=SQLITE_OK ){
      sqlite3Fts5ConfigErrmsg(pConfig, "%s", sqlite3_errmsg(pConfig->db));
    }
    sqlite3_free(zSql);
  }

  va_end(ap);
  *ppStmt = pRet;
  return rc;
} 

static int fts5CursorFirstSorted(
  Fts5FullTable *pTab, 
  Fts5Cursor *pCsr, 
  int bDesc
){
  Fts5Config *pConfig = pTab->p.pConfig;
  Fts5Sorter *pSorter;
  int nPhrase;
  sqlite3_int64 nByte;
  int rc;
  const char *zRank = pCsr->zRank;
  const char *zRankArgs = pCsr->zRankArgs;
  
  nPhrase = sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
  nByte = SZ_FTS5SORTER(nPhrase);
  pSorter = (Fts5Sorter*)sqlite3_malloc64(nByte);
  if( pSorter==0 ) return SQLITE_NOMEM;
  memset(pSorter, 0, (size_t)nByte);
  pSorter->nIdx = nPhrase;

  /* TODO: It would be better to have some system for reusing statement
  ** handles here, rather than preparing a new one for each query. But that
  ** is not possible as SQLite reference counts the virtual table objects.
  ** And since the statement required here reads from this very virtual 
  ** table, saving it creates a circular reference.
  **
  ** If SQLite a built-in statement cache, this wouldn't be a problem. */
  rc = fts5PrepareStatement(&pSorter->pStmt, pConfig,
      "SELECT rowid, rank FROM %Q.%Q ORDER BY %s(\"%w\"%s%s) %s",
      pConfig->zDb, pConfig->zName, zRank, pConfig->zName,
      (zRankArgs ? ", " : ""),
      (zRankArgs ? zRankArgs : ""),
      bDesc ? "DESC" : "ASC"
  );

  pCsr->pSorter = pSorter;
  if( rc==SQLITE_OK ){
    assert( pTab->pSortCsr==0 );
    pTab->pSortCsr = pCsr;
    rc = fts5SorterNext(pCsr);
    pTab->pSortCsr = 0;
  }

  if( rc!=SQLITE_OK ){
    sqlite3_finalize(pSorter->pStmt);
    sqlite3_free(pSorter);
    pCsr->pSorter = 0;
  }

  return rc;
}

static int fts5CursorFirst(Fts5FullTable *pTab, Fts5Cursor *pCsr, int bDesc){
  int rc;
  Fts5Expr *pExpr = pCsr->pExpr;
  rc = sqlite3Fts5ExprFirst(pExpr, pTab->p.pIndex, pCsr->iFirstRowid, bDesc);
  if( sqlite3Fts5ExprEof(pExpr) ){
    CsrFlagSet(pCsr, FTS5CSR_EOF);
  }
  fts5CsrNewrow(pCsr);
  return rc;
}

/*
** Process a "special" query. A special query is identified as one with a
** MATCH expression that begins with a '*' character. The remainder of
** the text passed to the MATCH operator are used as  the special query
** parameters.
*/
static int fts5SpecialMatch(
  Fts5FullTable *pTab, 
  Fts5Cursor *pCsr, 
  const char *zQuery
){
  int rc = SQLITE_OK;             /* Return code */
  const char *z = zQuery;         /* Special query text */
  int n;                          /* Number of bytes in text at z */

  while( z[0]==' ' ) z++;
  for(n=0; z[n] && z[n]!=' '; n++);

  assert( pTab->p.base.zErrMsg==0 );
  pCsr->ePlan = FTS5_PLAN_SPECIAL;

  if( n==5 && 0==sqlite3_strnicmp("reads", z, n) ){
    pCsr->iSpecial = sqlite3Fts5IndexReads(pTab->p.pIndex);
  }
  else if( n==2 && 0==sqlite3_strnicmp("id", z, n) ){
    pCsr->iSpecial = pCsr->iCsrId;
  }
  else{
    /* An unrecognized directive. Return an error message. */
    pTab->p.base.zErrMsg = sqlite3_mprintf("unknown special query: %.*s", n, z);
    rc = SQLITE_ERROR;
  }

  return rc;
}

/*
** Search for an auxiliary function named zName that can be used with table
** pTab. If one is found, return a pointer to the corresponding Fts5Auxiliary
** structure. Otherwise, if no such function exists, return NULL.
*/
static Fts5Auxiliary *fts5FindAuxiliary(Fts5FullTable *pTab, const char *zName){
  Fts5Auxiliary *pAux;

  for(pAux=pTab->pGlobal->pAux; pAux; pAux=pAux->pNext){
    if( sqlite3_stricmp(zName, pAux->zFunc)==0 ) return pAux;
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}


static int fts5FindRankFunction(Fts5Cursor *pCsr){
  Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
  Fts5Config *pConfig = pTab->p.pConfig;
  int rc = SQLITE_OK;
  Fts5Auxiliary *pAux = 0;
  const char *zRank = pCsr->zRank;
  const char *zRankArgs = pCsr->zRankArgs;

  if( zRankArgs ){
    char *zSql = sqlite3Fts5Mprintf(&rc, "SELECT %s", zRankArgs);
    if( zSql ){
      sqlite3_stmt *pStmt = 0;
      rc = sqlite3_prepare_v3(pConfig->db, zSql, -1,
                              SQLITE_PREPARE_PERSISTENT, &pStmt, 0);
      sqlite3_free(zSql);
      assert( rc==SQLITE_OK || pCsr->pRankArgStmt==0 );
      if( rc==SQLITE_OK ){
        if( SQLITE_ROW==sqlite3_step(pStmt) ){
          sqlite3_int64 nByte;
          pCsr->nRankArg = sqlite3_column_count(pStmt);
          nByte = sizeof(sqlite3_value*)*pCsr->nRankArg;
          pCsr->apRankArg = (sqlite3_value**)sqlite3Fts5MallocZero(&rc, nByte);
          if( rc==SQLITE_OK ){
            int i;
            for(i=0; i<pCsr->nRankArg; i++){
              pCsr->apRankArg[i] = sqlite3_column_value(pStmt, i);
            }
          }
          pCsr->pRankArgStmt = pStmt;
        }else{
          rc = sqlite3_finalize(pStmt);
          assert( rc!=SQLITE_OK );
        }
      }
    }
  }

  if( rc==SQLITE_OK ){
    pAux = fts5FindAuxiliary(pTab, zRank);
    if( pAux==0 ){
      assert( pTab->p.base.zErrMsg==0 );
      pTab->p.base.zErrMsg = sqlite3_mprintf("no such function: %s", zRank);
      rc = SQLITE_ERROR;
    }
  }

  pCsr->pRank = pAux;
  return rc;
}


static int fts5CursorParseRank(
  Fts5Config *pConfig,
  Fts5Cursor *pCsr, 
  sqlite3_value *pRank
){
  int rc = SQLITE_OK;
  if( pRank ){
    const char *z = (const char*)sqlite3_value_text(pRank);
    char *zRank = 0;
    char *zRankArgs = 0;

    if( z==0 ){
      if( sqlite3_value_type(pRank)==SQLITE_NULL ) rc = SQLITE_ERROR;
    }else{
      rc = sqlite3Fts5ConfigParseRank(z, &zRank, &zRankArgs);
    }
    if( rc==SQLITE_OK ){
      pCsr->zRank = zRank;
      pCsr->zRankArgs = zRankArgs;
      CsrFlagSet(pCsr, FTS5CSR_FREE_ZRANK);
    }else if( rc==SQLITE_ERROR ){
      pCsr->base.pVtab->zErrMsg = sqlite3_mprintf(
          "parse error in rank function: %s", z
      );
    }
  }else{
    if( pConfig->zRank ){
      pCsr->zRank = (char*)pConfig->zRank;
      pCsr->zRankArgs = (char*)pConfig->zRankArgs;
    }else{
      pCsr->zRank = (char*)FTS5_DEFAULT_RANK;
      pCsr->zRankArgs = 0;
    }
  }
  return rc;
}

static i64 fts5GetRowidLimit(sqlite3_value *pVal, i64 iDefault){
  if( pVal ){
    int eType = sqlite3_value_numeric_type(pVal);
    if( eType==SQLITE_INTEGER ){
      return sqlite3_value_int64(pVal);
    }
  }
  return iDefault;
}

/*
** Set the error message on the virtual table passed as the first argument.
*/
static void fts5SetVtabError(Fts5FullTable *p, const char *zFormat, ...){
  va_list ap;                     /* ... printf arguments */
  va_start(ap, zFormat);
  sqlite3_free(p->p.base.zErrMsg);
  p->p.base.zErrMsg = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
}

/*
** Arrange for subsequent calls to sqlite3Fts5Tokenize() to use the locale
** specified by pLocale/nLocale. The buffer indicated by pLocale must remain
** valid until after the final call to sqlite3Fts5Tokenize() that will use
** the locale.
*/
static void sqlite3Fts5SetLocale(
  Fts5Config *pConfig, 
  const char *zLocale, 
  int nLocale
){
  Fts5TokenizerConfig *pT = &pConfig->t;
  pT->pLocale = zLocale;
  pT->nLocale = nLocale;
}

/*
** Clear any locale configured by an earlier call to sqlite3Fts5SetLocale().
*/
void sqlite3Fts5ClearLocale(Fts5Config *pConfig){
  sqlite3Fts5SetLocale(pConfig, 0, 0);
}

/*
** Return true if the value passed as the only argument is an 
** fts5_locale() value.  
*/
int sqlite3Fts5IsLocaleValue(Fts5Config *pConfig, sqlite3_value *pVal){
  int ret = 0;
  if( sqlite3_value_type(pVal)==SQLITE_BLOB ){
    /* Call sqlite3_value_bytes() after sqlite3_value_blob() in this case.
    ** If the blob was created using zeroblob(), then sqlite3_value_blob()
    ** may call malloc(). If this malloc() fails, then the values returned
    ** by both value_blob() and value_bytes() will be 0. If value_bytes() were 
    ** called first, then the NULL pointer returned by value_blob() might
    ** be dereferenced.  */
    const u8 *pBlob = sqlite3_value_blob(pVal);
    int nBlob = sqlite3_value_bytes(pVal); 
    if( nBlob>FTS5_LOCALE_HDR_SIZE
     && 0==memcmp(pBlob, FTS5_LOCALE_HDR(pConfig), FTS5_LOCALE_HDR_SIZE)
    ){
      ret = 1;
    }
  }
  return ret;
}

/*
** Value pVal is guaranteed to be an fts5_locale() value, according to
** sqlite3Fts5IsLocaleValue(). This function extracts the text and locale
** from the value and returns them separately.
**
** If successful, SQLITE_OK is returned and (*ppText) and (*ppLoc) set
** to point to buffers containing the text and locale, as utf-8,
** respectively. In this case output parameters (*pnText) and (*pnLoc) are
** set to the sizes in bytes of these two buffers.
**
** Or, if an error occurs, then an SQLite error code is returned. The final
** value of the four output parameters is undefined in this case.
*/
int sqlite3Fts5DecodeLocaleValue(
  sqlite3_value *pVal, 
  const char **ppText,
  int *pnText, 
  const char **ppLoc, 
  int *pnLoc
){
  const char *p = sqlite3_value_blob(pVal);
  int n = sqlite3_value_bytes(pVal);
  int nLoc = 0;

  assert( sqlite3_value_type(pVal)==SQLITE_BLOB );
  assert( n>FTS5_LOCALE_HDR_SIZE );

  for(nLoc=FTS5_LOCALE_HDR_SIZE; p[nLoc]; nLoc++){
    if( nLoc==(n-1) ){
      return SQLITE_MISMATCH;
    }
  }
  *ppLoc = &p[FTS5_LOCALE_HDR_SIZE];
  *pnLoc = nLoc - FTS5_LOCALE_HDR_SIZE;

  *ppText = &p[nLoc+1];
  *pnText = n - nLoc - 1;
  return SQLITE_OK;
}

/*
** Argument pVal is the text of a full-text search expression. It may or
** may not have been wrapped by fts5_locale(). This function extracts
** the text of the expression, and sets output variable (*pzText) to 
** point to a nul-terminated buffer containing the expression.
**
** If pVal was an fts5_locale() value, then sqlite3Fts5SetLocale() is called 
** to set the tokenizer to use the specified locale.
**
** If output variable (*pbFreeAndReset) is set to true, then the caller
** is required to (a) call sqlite3Fts5ClearLocale() to reset the tokenizer 
** locale, and (b) call sqlite3_free() to free (*pzText).
*/
static int fts5ExtractExprText(
  Fts5Config *pConfig,            /* Fts5 configuration */
  sqlite3_value *pVal,            /* Value to extract expression text from */
  char **pzText,                  /* OUT: nul-terminated buffer of text */
  int *pbFreeAndReset             /* OUT: Free (*pzText) and clear locale */
){
  int rc = SQLITE_OK;

  if( sqlite3Fts5IsLocaleValue(pConfig, pVal) ){
    const char *pText = 0;
    int nText = 0;
    const char *pLoc = 0;
    int nLoc = 0;
    rc = sqlite3Fts5DecodeLocaleValue(pVal, &pText, &nText, &pLoc, &nLoc);
    *pzText = sqlite3Fts5Mprintf(&rc, "%.*s", nText, pText);
    if( rc==SQLITE_OK ){
      sqlite3Fts5SetLocale(pConfig, pLoc, nLoc);
    }
    *pbFreeAndReset = 1;
  }else{
    *pzText = (char*)sqlite3_value_text(pVal);
    *pbFreeAndReset = 0;
  }

  return rc;
}


/*
** This is the xFilter interface for the virtual table.  See
** the virtual table xFilter method documentation for additional
** information.
** 
** There are three possible query strategies:
**
**   1. Full-text search using a MATCH operator.
**   2. A by-rowid lookup.
**   3. A full-table scan.
*/
static int fts5FilterMethod(
  sqlite3_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  sqlite3_value **apVal           /* Arguments for the indexing scheme */
){
  Fts5FullTable *pTab = (Fts5FullTable*)(pCursor->pVtab);
  Fts5Config *pConfig = pTab->p.pConfig;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int rc = SQLITE_OK;             /* Error code */
  int bDesc;                      /* True if ORDER BY [rank|rowid] DESC */
  int bOrderByRank;               /* True if ORDER BY rank */
  sqlite3_value *pRank = 0;       /* rank MATCH ? expression (or NULL) */
  sqlite3_value *pRowidEq = 0;    /* rowid = ? expression (or NULL) */
  sqlite3_value *pRowidLe = 0;    /* rowid <= ? expression (or NULL) */
  sqlite3_value *pRowidGe = 0;    /* rowid >= ? expression (or NULL) */
  int iCol;                       /* Column on LHS of MATCH operator */
  char **pzErrmsg = pConfig->pzErrmsg;
  int bPrefixInsttoken = pConfig->bPrefixInsttoken;
  int i;
  int iIdxStr = 0;
  Fts5Expr *pExpr = 0;

  assert( pConfig->bLock==0 );
  if( pCsr->ePlan ){
    fts5FreeCursorComponents(pCsr);
    memset(&pCsr->ePlan, 0, sizeof(Fts5Cursor) - ((u8*)&pCsr->ePlan-(u8*)pCsr));
  }

  assert( pCsr->pStmt==0 );
  assert( pCsr->pExpr==0 );
  assert( pCsr->csrflags==0 );
  assert( pCsr->pRank==0 );
  assert( pCsr->zRank==0 );
  assert( pCsr->zRankArgs==0 );
  assert( pTab->pSortCsr==0 || nVal==0 );

  assert( pzErrmsg==0 || pzErrmsg==&pTab->p.base.zErrMsg );
  pConfig->pzErrmsg = &pTab->p.base.zErrMsg;

  /* Decode the arguments passed through to this function. */
  for(i=0; i<nVal; i++){
    switch( idxStr[iIdxStr++] ){
      case 'r':
        pRank = apVal[i];
        break;
      case 'M': {
        char *zText = 0;
        int bFreeAndReset = 0;
        int bInternal = 0;

        rc = fts5ExtractExprText(pConfig, apVal[i], &zText, &bFreeAndReset);
        if( rc!=SQLITE_OK ) goto filter_out;
        if( zText==0 ) zText = "";
        if( sqlite3_value_subtype(apVal[i])==FTS5_INSTTOKEN_SUBTYPE ){
          pConfig->bPrefixInsttoken = 1;
        }

        iCol = 0;
        do{
          iCol = iCol*10 + (idxStr[iIdxStr]-'0');
          iIdxStr++;
        }while( idxStr[iIdxStr]>='0' && idxStr[iIdxStr]<='9' );

        if( zText[0]=='*' ){
          /* The user has issued a query of the form "MATCH '*...'". This
          ** indicates that the MATCH expression is not a full text query,
          ** but a request for an internal parameter.  */
          rc = fts5SpecialMatch(pTab, pCsr, &zText[1]);
          bInternal = 1;
        }else{
          char **pzErr = &pTab->p.base.zErrMsg;
          rc = sqlite3Fts5ExprNew(pConfig, 0, iCol, zText, &pExpr, pzErr);
          if( rc==SQLITE_OK ){
            rc = sqlite3Fts5ExprAnd(&pCsr->pExpr, pExpr);
            pExpr = 0;
          }
        }

        if( bFreeAndReset ){
          sqlite3_free(zText);
          sqlite3Fts5ClearLocale(pConfig);
        }

        if( bInternal || rc!=SQLITE_OK ) goto filter_out;

        break;
      }
      case 'L':
      case 'G': {
        int bGlob = (idxStr[iIdxStr-1]=='G');
        const char *zText = (const char*)sqlite3_value_text(apVal[i]);
        iCol = 0;
        do{
          iCol = iCol*10 + (idxStr[iIdxStr]-'0');
          iIdxStr++;
        }while( idxStr[iIdxStr]>='0' && idxStr[iIdxStr]<='9' );
        if( zText ){
          rc = sqlite3Fts5ExprPattern(pConfig, bGlob, iCol, zText, &pExpr);
        }
        if( rc==SQLITE_OK ){
          rc = sqlite3Fts5ExprAnd(&pCsr->pExpr, pExpr);
          pExpr = 0;
        }
        if( rc!=SQLITE_OK ) goto filter_out;
        break;
      }
      case '=':
        pRowidEq = apVal[i];
        break;
      case '<':
        pRowidLe = apVal[i];
        break;
      default: assert( idxStr[iIdxStr-1]=='>' );
        pRowidGe = apVal[i];
        break;
    }
  }
  bOrderByRank = ((idxNum & FTS5_BI_ORDER_RANK) ? 1 : 0);
  pCsr->bDesc = bDesc = ((idxNum & FTS5_BI_ORDER_DESC) ? 1 : 0);

  /* Set the cursor upper and lower rowid limits. Only some strategies 
  ** actually use them. This is ok, as the xBestIndex() method leaves the
  ** sqlite3_index_constraint.omit flag clear for range constraints
  ** on the rowid field.  */
  if( pRowidEq ){
    pRowidLe = pRowidGe = pRowidEq;
  }
  if( bDesc ){
    pCsr->iFirstRowid = fts5GetRowidLimit(pRowidLe, LARGEST_INT64);
    pCsr->iLastRowid = fts5GetRowidLimit(pRowidGe, SMALLEST_INT64);
  }else{
    pCsr->iLastRowid = fts5GetRowidLimit(pRowidLe, LARGEST_INT64);
    pCsr->iFirstRowid = fts5GetRowidLimit(pRowidGe, SMALLEST_INT64);
  }

  rc = sqlite3Fts5IndexLoadConfig(pTab->p.pIndex);
  if( rc!=SQLITE_OK ) goto filter_out;

  if( pTab->pSortCsr ){
    /* If pSortCsr is non-NULL, then this call is being made as part of 
    ** processing for a "... MATCH <expr> ORDER BY rank" query (ePlan is
    ** set to FTS5_PLAN_SORTED_MATCH). pSortCsr is the cursor that will
    ** return results to the user for this query. The current cursor 
    ** (pCursor) is used to execute the query issued by function 
    ** fts5CursorFirstSorted() above.  */
    assert( pRowidEq==0 && pRowidLe==0 && pRowidGe==0 && pRank==0 );
    assert( nVal==0 && bOrderByRank==0 && bDesc==0 );
    assert( pCsr->iLastRowid==LARGEST_INT64 );
    assert( pCsr->iFirstRowid==SMALLEST_INT64 );
    if( pTab->pSortCsr->bDesc ){
      pCsr->iLastRowid = pTab->pSortCsr->iFirstRowid;
      pCsr->iFirstRowid = pTab->pSortCsr->iLastRowid;
    }else{
      pCsr->iLastRowid = pTab->pSortCsr->iLastRowid;
      pCsr->iFirstRowid = pTab->pSortCsr->iFirstRowid;
    }
    pCsr->ePlan = FTS5_PLAN_SOURCE;
    pCsr->pExpr = pTab->pSortCsr->pExpr;
    rc = fts5CursorFirst(pTab, pCsr, bDesc);
  }else if( pCsr->pExpr ){
    assert( rc==SQLITE_OK );
    rc = fts5CursorParseRank(pConfig, pCsr, pRank);
    if( rc==SQLITE_OK ){
      if( bOrderByRank ){
        pCsr->ePlan = FTS5_PLAN_SORTED_MATCH;
        rc = fts5CursorFirstSorted(pTab, pCsr, bDesc);
      }else{
        pCsr->ePlan = FTS5_PLAN_MATCH;
        rc = fts5CursorFirst(pTab, pCsr, bDesc);
      }
    }
  }else if( pConfig->zContent==0 ){
    fts5SetVtabError(pTab,"%s: table does not support scanning",pConfig->zName);
    rc = SQLITE_ERROR;
  }else{
    /* This is either a full-table scan (ePlan==FTS5_PLAN_SCAN) or a lookup
    ** by rowid (ePlan==FTS5_PLAN_ROWID).  */
    pCsr->ePlan = (pRowidEq ? FTS5_PLAN_ROWID : FTS5_PLAN_SCAN);
    rc = sqlite3Fts5StorageStmt(
        pTab->pStorage, fts5StmtType(pCsr), &pCsr->pStmt, &pTab->p.base.zErrMsg
    );
    if( rc==SQLITE_OK ){
      if( pRowidEq!=0 ){
        assert( pCsr->ePlan==FTS5_PLAN_ROWID );
        sqlite3_bind_value(pCsr->pStmt, 1, pRowidEq);
      }else{
        sqlite3_bind_int64(pCsr->pStmt, 1, pCsr->iFirstRowid);
        sqlite3_bind_int64(pCsr->pStmt, 2, pCsr->iLastRowid);
      }
      rc = fts5NextMethod(pCursor);
    }
  }

 filter_out:
  sqlite3Fts5ExprFree(pExpr);
  pConfig->pzErrmsg = pzErrmsg;
  pConfig->bPrefixInsttoken = bPrefixInsttoken;
  return rc;
}

/* 
** This is the xEof method of the virtual table. SQLite calls this 
** routine to find out if it has reached the end of a result set.
*/
static int fts5EofMethod(sqlite3_vtab_cursor *pCursor){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  return (CsrFlagTest(pCsr, FTS5CSR_EOF) ? 1 : 0);
}

/*
** Return the rowid that the cursor currently points to.
*/
static i64 fts5CursorRowid(Fts5Cursor *pCsr){
  assert( pCsr->ePlan==FTS5_PLAN_MATCH 
       || pCsr->ePlan==FTS5_PLAN_SORTED_MATCH 
       || pCsr->ePlan==FTS5_PLAN_SOURCE 
       || pCsr->ePlan==FTS5_PLAN_SCAN 
       || pCsr->ePlan==FTS5_PLAN_ROWID 
  );
  if( pCsr->pSorter ){
    return pCsr->pSorter->iRowid;
  }else if( pCsr->ePlan>=FTS5_PLAN_SCAN ){
    return sqlite3_column_int64(pCsr->pStmt, 0);
  }else{
    return sqlite3Fts5ExprRowid(pCsr->pExpr);
  }
}

/* 
** This is the xRowid method. The SQLite core calls this routine to
** retrieve the rowid for the current row of the result set. fts5
** exposes %_content.rowid as the rowid for the virtual table. The
** rowid should be written to *pRowid.
*/
static int fts5RowidMethod(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int ePlan = pCsr->ePlan;
  
  assert( CsrFlagTest(pCsr, FTS5CSR_EOF)==0 );
  if( ePlan==FTS5_PLAN_SPECIAL ){
    *pRowid = 0;
  }else{
    *pRowid = fts5CursorRowid(pCsr);
  }

  return SQLITE_OK;
}


/*
** If the cursor requires seeking (bSeekRequired flag is set), seek it.
** Return SQLITE_OK if no error occurs, or an SQLite error code otherwise.
**
** If argument bErrormsg is true and an error occurs, an error message may
** be left in sqlite3_vtab.zErrMsg.
*/
static int fts5SeekCursor(Fts5Cursor *pCsr, int bErrormsg){
  int rc = SQLITE_OK;

  /* If the cursor does not yet have a statement handle, obtain one now. */ 
  if( pCsr->pStmt==0 ){
    Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
    int eStmt = fts5StmtType(pCsr);
    rc = sqlite3Fts5StorageStmt(
        pTab->pStorage, eStmt, &pCsr->pStmt, (bErrormsg?&pTab->p.base.zErrMsg:0)
    );
    assert( rc!=SQLITE_OK || pTab->p.base.zErrMsg==0 );
    assert( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_CONTENT) );
  }

  if( rc==SQLITE_OK && CsrFlagTest(pCsr, FTS5CSR_REQUIRE_CONTENT) ){
    Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
    assert( pCsr->pExpr );
    sqlite3_reset(pCsr->pStmt);
    sqlite3_bind_int64(pCsr->pStmt, 1, fts5CursorRowid(pCsr));
    pTab->pConfig->bLock++;
    rc = sqlite3_step(pCsr->pStmt);
    pTab->pConfig->bLock--;
    if( rc==SQLITE_ROW ){
      rc = SQLITE_OK;
      CsrFlagClear(pCsr, FTS5CSR_REQUIRE_CONTENT);
    }else{
      rc = sqlite3_reset(pCsr->pStmt);
      if( rc==SQLITE_OK ){
        rc = FTS5_CORRUPT;
        fts5SetVtabError((Fts5FullTable*)pTab, 
            "fts5: missing row %lld from content table %s",
            fts5CursorRowid(pCsr), 
            pTab->pConfig->zContent
        );
      }else if( pTab->pConfig->pzErrmsg ){
        fts5SetVtabError((Fts5FullTable*)pTab, 
            "%s", sqlite3_errmsg(pTab->pConfig->db)
        );
      }
    }
  }
  return rc;
}

/*
** This function is called to handle an FTS INSERT command. In other words,
** an INSERT statement of the form:
**
**     INSERT INTO fts(fts) VALUES($pCmd)
**     INSERT INTO fts(fts, rank) VALUES($pCmd, $pVal)
**
** Argument pVal is the value assigned to column "fts" by the INSERT 
** statement. This function returns SQLITE_OK if successful, or an SQLite
** error code if an error occurs.
**
** The commands implemented by this function are documented in the "Special
** INSERT Directives" section of the documentation. It should be updated if
** more commands are added to this function.
*/
static int fts5SpecialInsert(
  Fts5FullTable *pTab,            /* Fts5 table object */
  const char *zCmd,               /* Text inserted into table-name column */
  sqlite3_value *pVal             /* Value inserted into rank column */
){
  Fts5Config *pConfig = pTab->p.pConfig;
  int rc = SQLITE_OK;
  int bError = 0;
  int bLoadConfig = 0;

  if( 0==sqlite3_stricmp("delete-all", zCmd) ){
    if( pConfig->eContent==FTS5_CONTENT_NORMAL ){
      fts5SetVtabError(pTab, 
          "'delete-all' may only be used with a "
          "contentless or external content fts5 table"
      );
      rc = SQLITE_ERROR;
    }else{
      rc = sqlite3Fts5StorageDeleteAll(pTab->pStorage);
    }
    bLoadConfig = 1;
  }else if( 0==sqlite3_stricmp("rebuild", zCmd) ){
    if( fts5IsContentless(pTab, 1) ){
      fts5SetVtabError(pTab, 
          "'rebuild' may not be used with a contentless fts5 table"
      );
      rc = SQLITE_ERROR;
    }else{
      rc = sqlite3Fts5StorageRebuild(pTab->pStorage);
    }
    bLoadConfig = 1;
  }else if( 0==sqlite3_stricmp("optimize", zCmd) ){
    rc = sqlite3Fts5StorageOptimize(pTab->pStorage);
  }else if( 0==sqlite3_stricmp("merge", zCmd) ){
    int nMerge = sqlite3_value_int(pVal);
    rc = sqlite3Fts5StorageMerge(pTab->pStorage, nMerge);
  }else if( 0==sqlite3_stricmp("integrity-check", zCmd) ){
    int iArg = sqlite3_value_int(pVal);
    rc = sqlite3Fts5StorageIntegrity(pTab->pStorage, iArg);
#ifdef SQLITE_DEBUG
  }else if( 0==sqlite3_stricmp("prefix-index", zCmd) ){
    pConfig->bPrefixIndex = sqlite3_value_int(pVal);
#endif
  }else if( 0==sqlite3_stricmp("flush", zCmd) ){
    rc = sqlite3Fts5FlushToDisk(&pTab->p);
  }else{
    rc = sqlite3Fts5FlushToDisk(&pTab->p);
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5IndexLoadConfig(pTab->p.pIndex);
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5ConfigSetValue(pTab->p.pConfig, zCmd, pVal, &bError);
    }
    if( rc==SQLITE_OK ){
      if( bError ){
        rc = SQLITE_ERROR;
      }else{
        rc = sqlite3Fts5StorageConfigValue(pTab->pStorage, zCmd, pVal, 0);
      }
    }
  }

  if( rc==SQLITE_OK && bLoadConfig ){
    pTab->p.pConfig->iCookie--;
    rc = sqlite3Fts5IndexLoadConfig(pTab->p.pIndex);
  }

  return rc;
}

static int fts5SpecialDelete(
  Fts5FullTable *pTab, 
  sqlite3_value **apVal
){
  int rc = SQLITE_OK;
  int eType1 = sqlite3_value_type(apVal[1]);
  if( eType1==SQLITE_INTEGER ){
    sqlite3_int64 iDel = sqlite3_value_int64(apVal[1]);
    rc = sqlite3Fts5StorageDelete(pTab->pStorage, iDel, &apVal[2], 0);
  }
  return rc;
}

static void fts5StorageInsert(
  int *pRc, 
  Fts5FullTable *pTab, 
  sqlite3_value **apVal, 
  i64 *piRowid
){
  int rc = *pRc;
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5StorageContentInsert(pTab->pStorage, 0, apVal, piRowid);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5StorageIndexInsert(pTab->pStorage, apVal, *piRowid);
  }
  *pRc = rc;
}

/*
**
** This function is called when the user attempts an UPDATE on a contentless
** table. Parameter bRowidModified is true if the UPDATE statement modifies
** the rowid value. Parameter apVal[] contains the new values for each user
** defined column of the fts5 table. pConfig is the configuration object of the
** table being updated (guaranteed to be contentless). The contentless_delete=1
** and contentless_unindexed=1 options may or may not be set. 
**
** This function returns SQLITE_OK if the UPDATE can go ahead, or an SQLite
** error code if it cannot. In this case an error message is also loaded into
** pConfig. Output parameter (*pbContent) is set to true if the caller should
** update the %_content table only - not the FTS index or any other shadow
** table. This occurs when an UPDATE modifies only UNINDEXED columns of the
** table.
**
** An UPDATE may proceed if:
**
**   * The only columns modified are UNINDEXED columns, or
**
**   * The contentless_delete=1 option was specified and all of the indexed
**     columns (not a subset) have been modified.
*/
static int fts5ContentlessUpdate(
  Fts5Config *pConfig,
  sqlite3_value **apVal,
  int bRowidModified,
  int *pbContent
){
  int ii;
  int bSeenIndex = 0;             /* Have seen modified indexed column */
  int bSeenIndexNC = 0;           /* Have seen unmodified indexed column */
  int rc = SQLITE_OK;

  for(ii=0; ii<pConfig->nCol; ii++){
    if( pConfig->abUnindexed[ii]==0 ){
      if( sqlite3_value_nochange(apVal[ii]) ){
        bSeenIndexNC++;
      }else{
        bSeenIndex++;
      }
    }
  }

  if( bSeenIndex==0 && bRowidModified==0 ){
    *pbContent = 1;
  }else{
    if( bSeenIndexNC || pConfig->bContentlessDelete==0 ){
      rc = SQLITE_ERROR;
      sqlite3Fts5ConfigErrmsg(pConfig, 
          (pConfig->bContentlessDelete ?
          "%s a subset of columns on fts5 contentless-delete table: %s" :
          "%s contentless fts5 table: %s")
          , "cannot UPDATE", pConfig->zName
      );
    }
  }

  return rc;
}

/* 
** This function is the implementation of the xUpdate callback used by 
** FTS3 virtual tables. It is invoked by SQLite each time a row is to be
** inserted, updated or deleted.
**
** A delete specifies a single argument - the rowid of the row to remove.
** 
** Update and insert operations pass:
**
**   1. The "old" rowid, or NULL.
**   2. The "new" rowid.
**   3. Values for each of the nCol matchable columns.
**   4. Values for the two hidden columns (<tablename> and "rank").
*/
static int fts5UpdateMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  int nArg,                       /* Size of argument array */
  sqlite3_value **apVal,          /* Array of arguments */
  sqlite_int64 *pRowid            /* OUT: The affected (or effected) rowid */
){
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  Fts5Config *pConfig = pTab->p.pConfig;
  int eType0;                     /* value_type() of apVal[0] */
  int rc = SQLITE_OK;             /* Return code */

  /* A transaction must be open when this is called. */
  assert( pTab->ts.eState==1 || pTab->ts.eState==2 );

  assert( pVtab->zErrMsg==0 );
  assert( nArg==1 || nArg==(2+pConfig->nCol+2) );
  assert( sqlite3_value_type(apVal[0])==SQLITE_INTEGER 
       || sqlite3_value_type(apVal[0])==SQLITE_NULL 
  );
  assert( pTab->p.pConfig->pzErrmsg==0 );
  if( pConfig->pgsz==0 ){
    rc = sqlite3Fts5ConfigLoad(pTab->p.pConfig, pTab->p.pConfig->iCookie);
    if( rc!=SQLITE_OK ) return rc;
  }

  pTab->p.pConfig->pzErrmsg = &pTab->p.base.zErrMsg;

  /* Put any active cursors into REQUIRE_SEEK state. */
  fts5TripCursors(pTab);

  eType0 = sqlite3_value_type(apVal[0]);
  if( eType0==SQLITE_NULL 
   && sqlite3_value_type(apVal[2+pConfig->nCol])!=SQLITE_NULL 
  ){
    /* A "special" INSERT op. These are handled separately. */
    const char *z = (const char*)sqlite3_value_text(apVal[2+pConfig->nCol]);
    if( pConfig->eContent!=FTS5_CONTENT_NORMAL 
      && 0==sqlite3_stricmp("delete", z) 
    ){
      if( pConfig->bContentlessDelete ){
        fts5SetVtabError(pTab, 
            "'delete' may not be used with a contentless_delete=1 table"
        );
        rc = SQLITE_ERROR;
      }else{
        rc = fts5SpecialDelete(pTab, apVal);
      }
    }else{
      rc = fts5SpecialInsert(pTab, z, apVal[2 + pConfig->nCol + 1]);
    }
  }else{
    /* A regular INSERT, UPDATE or DELETE statement. The trick here is that
    ** any conflict on the rowid value must be detected before any 
    ** modifications are made to the database file. There are 4 cases:
    **
    **   1) DELETE
    **   2) UPDATE (rowid not modified)
    **   3) UPDATE (rowid modified)
    **   4) INSERT
    **
    ** Cases 3 and 4 may violate the rowid constraint.
    */
    int eConflict = SQLITE_ABORT;
    if( pConfig->eContent==FTS5_CONTENT_NORMAL || pConfig->bContentlessDelete ){
      eConflict = sqlite3_vtab_on_conflict(pConfig->db);
    }

    assert( eType0==SQLITE_INTEGER || eType0==SQLITE_NULL );
    assert( nArg!=1 || eType0==SQLITE_INTEGER );

    /* DELETE */
    if( nArg==1 ){
      /* It is only possible to DELETE from a contentless table if the
      ** contentless_delete=1 flag is set. */
      if( fts5IsContentless(pTab, 1) && pConfig->bContentlessDelete==0 ){
        fts5SetVtabError(pTab, 
            "cannot DELETE from contentless fts5 table: %s", pConfig->zName
        );
        rc = SQLITE_ERROR;
      }else{
        i64 iDel = sqlite3_value_int64(apVal[0]);  /* Rowid to delete */
        rc = sqlite3Fts5StorageDelete(pTab->pStorage, iDel, 0, 0);
      }
    }

    /* INSERT or UPDATE */
    else{
      int eType1 = sqlite3_value_numeric_type(apVal[1]);

      /* It is an error to write an fts5_locale() value to a table without
      ** the locale=1 option. */
      if( pConfig->bLocale==0 ){
        int ii;
        for(ii=0; ii<pConfig->nCol; ii++){
          sqlite3_value *pVal = apVal[ii+2];
          if( sqlite3Fts5IsLocaleValue(pConfig, pVal) ){
            fts5SetVtabError(pTab, "fts5_locale() requires locale=1");
            rc = SQLITE_MISMATCH;
            goto update_out;
          }
        }
      }

      if( eType0!=SQLITE_INTEGER ){
        /* An INSERT statement. If the conflict-mode is REPLACE, first remove
        ** the current entry (if any). */
        if( eConflict==SQLITE_REPLACE && eType1==SQLITE_INTEGER ){
          i64 iNew = sqlite3_value_int64(apVal[1]);  /* Rowid to delete */
          rc = sqlite3Fts5StorageDelete(pTab->pStorage, iNew, 0, 0);
        }
        fts5StorageInsert(&rc, pTab, apVal, pRowid);
      }

      /* UPDATE */
      else{
        Fts5Storage *pStorage = pTab->pStorage;
        i64 iOld = sqlite3_value_int64(apVal[0]);  /* Old rowid */
        i64 iNew = sqlite3_value_int64(apVal[1]);  /* New rowid */
        int bContent = 0;         /* Content only update */

        /* If this is a contentless table (including contentless_unindexed=1
        ** tables), check if the UPDATE may proceed.  */
        if( fts5IsContentless(pTab, 1) ){
          rc = fts5ContentlessUpdate(pConfig, &apVal[2], iOld!=iNew, &bContent);
          if( rc!=SQLITE_OK ) goto update_out;
        }

        if( eType1!=SQLITE_INTEGER ){
          rc = SQLITE_MISMATCH;
        }else if( iOld!=iNew ){
          assert( bContent==0 );
          if( eConflict==SQLITE_REPLACE ){
            rc = sqlite3Fts5StorageDelete(pStorage, iOld, 0, 1);
            if( rc==SQLITE_OK ){
              rc = sqlite3Fts5StorageDelete(pStorage, iNew, 0, 0);
            }
            fts5StorageInsert(&rc, pTab, apVal, pRowid);
          }else{
            rc = sqlite3Fts5StorageFindDeleteRow(pStorage, iOld);
            if( rc==SQLITE_OK ){
              rc = sqlite3Fts5StorageContentInsert(pStorage, 0, apVal, pRowid);
            }
            if( rc==SQLITE_OK ){
              rc = sqlite3Fts5StorageDelete(pStorage, iOld, 0, 0);
            }
            if( rc==SQLITE_OK ){
              rc = sqlite3Fts5StorageIndexInsert(pStorage, apVal, *pRowid);
            }
          }
        }else if( bContent ){
          /* This occurs when an UPDATE on a contentless table affects *only*
          ** UNINDEXED columns. This is a no-op for contentless_unindexed=0
          ** tables, or a write to the %_content table only for =1 tables.  */
          assert( fts5IsContentless(pTab, 1) );
          rc = sqlite3Fts5StorageFindDeleteRow(pStorage, iOld);
          if( rc==SQLITE_OK ){
            rc = sqlite3Fts5StorageContentInsert(pStorage, 1, apVal, pRowid);
          }
        }else{
          rc = sqlite3Fts5StorageDelete(pStorage, iOld, 0, 1);
          fts5StorageInsert(&rc, pTab, apVal, pRowid);
        }
        sqlite3Fts5StorageReleaseDeleteRow(pStorage);
      }
    }
  }

 update_out:
  pTab->p.pConfig->pzErrmsg = 0;
  return rc;
}

/*
** Implementation of xSync() method. 
*/
static int fts5SyncMethod(sqlite3_vtab *pVtab){
  int rc;
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_SYNC, 0);
  pTab->p.pConfig->pzErrmsg = &pTab->p.base.zErrMsg;
  rc = sqlite3Fts5FlushToDisk(&pTab->p);
  pTab->p.pConfig->pzErrmsg = 0;
  return rc;
}

/*
** Implementation of xBegin() method. 
*/
static int fts5BeginMethod(sqlite3_vtab *pVtab){
  int rc = fts5NewTransaction((Fts5FullTable*)pVtab);
  if( rc==SQLITE_OK ){
    fts5CheckTransactionState((Fts5FullTable*)pVtab, FTS5_BEGIN, 0);
  }
  return rc;
}

/*
** Implementation of xCommit() method. This is a no-op. The contents of
** the pending-terms hash-table have already been flushed into the database
** by fts5SyncMethod().
*/
static int fts5CommitMethod(sqlite3_vtab *pVtab){
  UNUSED_PARAM(pVtab);  /* Call below is a no-op for NDEBUG builds */
  fts5CheckTransactionState((Fts5FullTable*)pVtab, FTS5_COMMIT, 0);
  return SQLITE_OK;
}

/*
** Implementation of xRollback(). Discard the contents of the pending-terms
** hash-table. Any changes made to the database are reverted by SQLite.
*/
static int fts5RollbackMethod(sqlite3_vtab *pVtab){
  int rc;
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_ROLLBACK, 0);
  rc = sqlite3Fts5StorageRollback(pTab->pStorage);
  pTab->p.pConfig->pgsz = 0;
  return rc;
}

static int fts5CsrPoslist(Fts5Cursor*, int, const u8**, int*);

static void *fts5ApiUserData(Fts5Context *pCtx){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return pCsr->pAux->pUserData;
}

static int fts5ApiColumnCount(Fts5Context *pCtx){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return ((Fts5Table*)(pCsr->base.pVtab))->pConfig->nCol;
}

static int fts5ApiColumnTotalSize(
  Fts5Context *pCtx, 
  int iCol, 
  sqlite3_int64 *pnToken
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
  return sqlite3Fts5StorageSize(pTab->pStorage, iCol, pnToken);
}

static int fts5ApiRowCount(Fts5Context *pCtx, i64 *pnRow){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
  return sqlite3Fts5StorageRowCount(pTab->pStorage, pnRow);
}

/*
** Implementation of xTokenize_v2() API.
*/
static int fts5ApiTokenize_v2(
  Fts5Context *pCtx, 
  const char *pText, int nText, 
  const char *pLoc, int nLoc, 
  void *pUserData,
  int (*xToken)(void*, int, const char*, int, int, int)
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  int rc = SQLITE_OK;

  sqlite3Fts5SetLocale(pTab->pConfig, pLoc, nLoc);
  rc = sqlite3Fts5Tokenize(pTab->pConfig, 
      FTS5_TOKENIZE_AUX, pText, nText, pUserData, xToken
  );
  sqlite3Fts5SetLocale(pTab->pConfig, 0, 0);

  return rc;
}

/*
** Implementation of xTokenize() API. This is just xTokenize_v2() with NULL/0
** passed as the locale.
*/
static int fts5ApiTokenize(
  Fts5Context *pCtx, 
  const char *pText, int nText, 
  void *pUserData,
  int (*xToken)(void*, int, const char*, int, int, int)
){
  return fts5ApiTokenize_v2(pCtx, pText, nText, 0, 0, pUserData, xToken);
}

static int fts5ApiPhraseCount(Fts5Context *pCtx){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
}

static int fts5ApiPhraseSize(Fts5Context *pCtx, int iPhrase){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return sqlite3Fts5ExprPhraseSize(pCsr->pExpr, iPhrase);
}

/*
** Argument pStmt is an SQL statement of the type used by Fts5Cursor. This
** function extracts the text value of column iCol of the current row.
** Additionally, if there is an associated locale, it invokes
** sqlite3Fts5SetLocale() to configure the tokenizer. In all cases the caller
** should invoke sqlite3Fts5ClearLocale() to clear the locale at some point
** after this function returns.
**
** If successful, (*ppText) is set to point to a buffer containing the text
** value as utf-8 and SQLITE_OK returned. (*pnText) is set to the size of that
** buffer in bytes. It is not guaranteed to be nul-terminated. If an error
** occurs, an SQLite error code is returned. The final values of the two
** output parameters are undefined in this case.
*/
static int fts5TextFromStmt(
  Fts5Config *pConfig,
  sqlite3_stmt *pStmt,
  int iCol,
  const char **ppText,
  int *pnText
){
  sqlite3_value *pVal = sqlite3_column_value(pStmt, iCol+1);
  const char *pLoc = 0;
  int nLoc = 0;
  int rc = SQLITE_OK;

  if( pConfig->bLocale 
   && pConfig->eContent==FTS5_CONTENT_EXTERNAL
   && sqlite3Fts5IsLocaleValue(pConfig, pVal) 
  ){
    rc = sqlite3Fts5DecodeLocaleValue(pVal, ppText, pnText, &pLoc, &nLoc);
  }else{
    *ppText = (const char*)sqlite3_value_text(pVal);
    *pnText = sqlite3_value_bytes(pVal);
    if( pConfig->bLocale && pConfig->eContent==FTS5_CONTENT_NORMAL ){
      pLoc = (const char*)sqlite3_column_text(pStmt, iCol+1+pConfig->nCol);
      nLoc = sqlite3_column_bytes(pStmt, iCol+1+pConfig->nCol);
    }
  }
  sqlite3Fts5SetLocale(pConfig, pLoc, nLoc);
  return rc;
}

static int fts5ApiColumnText(
  Fts5Context *pCtx, 
  int iCol, 
  const char **pz, 
  int *pn
){
  int rc = SQLITE_OK;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);

  assert( pCsr->ePlan!=FTS5_PLAN_SPECIAL );
  if( iCol<0 || iCol>=pTab->pConfig->nCol ){
    rc = SQLITE_RANGE;
  }else if( fts5IsContentless((Fts5FullTable*)(pCsr->base.pVtab), 0) ){
    *pz = 0;
    *pn = 0;
  }else{
    rc = fts5SeekCursor(pCsr, 0);
    if( rc==SQLITE_OK ){
      rc = fts5TextFromStmt(pTab->pConfig, pCsr->pStmt, iCol, pz, pn);
      sqlite3Fts5ClearLocale(pTab->pConfig);
    }
  }
  return rc;
}

/*
** This is called by various API functions - xInst, xPhraseFirst, 
** xPhraseFirstColumn etc. - to obtain the position list for phrase iPhrase
** of the current row. This function works for both detail=full tables (in
** which case the position-list was read from the fts index) or for other
** detail= modes if the row content is available.
*/
static int fts5CsrPoslist(
  Fts5Cursor *pCsr,               /* Fts5 cursor object */
  int iPhrase,                    /* Phrase to find position list for */
  const u8 **pa,                  /* OUT: Pointer to position list buffer */
  int *pn                         /* OUT: Size of (*pa) in bytes */
){
  Fts5Config *pConfig = ((Fts5Table*)(pCsr->base.pVtab))->pConfig;
  int rc = SQLITE_OK;
  int bLive = (pCsr->pSorter==0);

  if( iPhrase<0 || iPhrase>=sqlite3Fts5ExprPhraseCount(pCsr->pExpr) ){
    rc = SQLITE_RANGE;
  }else if( pConfig->eDetail!=FTS5_DETAIL_FULL 
         && fts5IsContentless((Fts5FullTable*)pCsr->base.pVtab, 1)
  ){
    *pa = 0;
    *pn = 0;
    return SQLITE_OK;
  }else if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_POSLIST) ){
    if( pConfig->eDetail!=FTS5_DETAIL_FULL ){
      Fts5PoslistPopulator *aPopulator;
      int i;

      aPopulator = sqlite3Fts5ExprClearPoslists(pCsr->pExpr, bLive);
      if( aPopulator==0 ) rc = SQLITE_NOMEM;
      if( rc==SQLITE_OK ){
        rc = fts5SeekCursor(pCsr, 0);
      }
      for(i=0; i<pConfig->nCol && rc==SQLITE_OK; i++){
        const char *z = 0;
        int n = 0; 
        rc = fts5TextFromStmt(pConfig, pCsr->pStmt, i, &z, &n);
        if( rc==SQLITE_OK ){
          rc = sqlite3Fts5ExprPopulatePoslists(
              pConfig, pCsr->pExpr, aPopulator, i, z, n
          );
        }
        sqlite3Fts5ClearLocale(pConfig);
      }
      sqlite3_free(aPopulator);

      if( pCsr->pSorter ){
        sqlite3Fts5ExprCheckPoslists(pCsr->pExpr, pCsr->pSorter->iRowid);
      }
    }
    CsrFlagClear(pCsr, FTS5CSR_REQUIRE_POSLIST);
  }

  if( rc==SQLITE_OK ){
    if( pCsr->pSorter && pConfig->eDetail==FTS5_DETAIL_FULL ){
      Fts5Sorter *pSorter = pCsr->pSorter;
      int i1 = (iPhrase==0 ? 0 : pSorter->aIdx[iPhrase-1]);
      *pn = pSorter->aIdx[iPhrase] - i1;
      *pa = &pSorter->aPoslist[i1];
    }else{
      *pn = sqlite3Fts5ExprPoslist(pCsr->pExpr, iPhrase, pa);
    }
  }else{
    *pa = 0;
    *pn = 0;
  }

  return rc;
}

/*
** Ensure that the Fts5Cursor.nInstCount and aInst[] variables are populated
** correctly for the current view. Return SQLITE_OK if successful, or an
** SQLite error code otherwise.
*/
static int fts5CacheInstArray(Fts5Cursor *pCsr){
  int rc = SQLITE_OK;
  Fts5PoslistReader *aIter;       /* One iterator for each phrase */
  int nIter;                      /* Number of iterators/phrases */
  int nCol = ((Fts5Table*)pCsr->base.pVtab)->pConfig->nCol;
  
  nIter = sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
  if( pCsr->aInstIter==0 ){
    sqlite3_int64 nByte = sizeof(Fts5PoslistReader) * nIter;
    pCsr->aInstIter = (Fts5PoslistReader*)sqlite3Fts5MallocZero(&rc, nByte);
  }
  aIter = pCsr->aInstIter;

  if( aIter ){
    int nInst = 0;                /* Number instances seen so far */
    int i;

    /* Initialize all iterators */
    for(i=0; i<nIter && rc==SQLITE_OK; i++){
      const u8 *a;
      int n; 
      rc = fts5CsrPoslist(pCsr, i, &a, &n);
      if( rc==SQLITE_OK ){
        sqlite3Fts5PoslistReaderInit(a, n, &aIter[i]);
      }
    }

    if( rc==SQLITE_OK ){
      while( 1 ){
        int *aInst;
        int iBest = -1;
        for(i=0; i<nIter; i++){
          if( (aIter[i].bEof==0) 
              && (iBest<0 || aIter[i].iPos<aIter[iBest].iPos) 
            ){
            iBest = i;
          }
        }
        if( iBest<0 ) break;

        nInst++;
        if( nInst>=pCsr->nInstAlloc ){
          int nNewSize = pCsr->nInstAlloc ? pCsr->nInstAlloc*2 : 32;
          aInst = (int*)sqlite3_realloc64(
              pCsr->aInst, nNewSize*sizeof(int)*3
              );
          if( aInst ){
            pCsr->aInst = aInst;
            pCsr->nInstAlloc = nNewSize;
          }else{
            nInst--;
            rc = SQLITE_NOMEM;
            break;
          }
        }

        aInst = &pCsr->aInst[3 * (nInst-1)];
        aInst[0] = iBest;
        aInst[1] = FTS5_POS2COLUMN(aIter[iBest].iPos);
        aInst[2] = FTS5_POS2OFFSET(aIter[iBest].iPos);
        assert( aInst[1]>=0 );
        if( aInst[1]>=nCol ){
          rc = FTS5_CORRUPT;
          break;
        }
        sqlite3Fts5PoslistReaderNext(&aIter[iBest]);
      }
    }

    pCsr->nInstCount = nInst;
    CsrFlagClear(pCsr, FTS5CSR_REQUIRE_INST);
  }
  return rc;
}

static int fts5ApiInstCount(Fts5Context *pCtx, int *pnInst){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  int rc = SQLITE_OK;
  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_INST)==0 
   || SQLITE_OK==(rc = fts5CacheInstArray(pCsr)) ){
    *pnInst = pCsr->nInstCount;
  }
  return rc;
}

static int fts5ApiInst(
  Fts5Context *pCtx, 
  int iIdx, 
  int *piPhrase, 
  int *piCol, 
  int *piOff
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  int rc = SQLITE_OK;
  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_INST)==0 
   || SQLITE_OK==(rc = fts5CacheInstArray(pCsr)) 
  ){
    if( iIdx<0 || iIdx>=pCsr->nInstCount ){
      rc = SQLITE_RANGE;
    }else{
      *piPhrase = pCsr->aInst[iIdx*3];
      *piCol = pCsr->aInst[iIdx*3 + 1];
      *piOff = pCsr->aInst[iIdx*3 + 2];
    }
  }
  return rc;
}

static sqlite3_int64 fts5ApiRowid(Fts5Context *pCtx){
  return fts5CursorRowid((Fts5Cursor*)pCtx);
}

static int fts5ColumnSizeCb(
  void *pContext,                 /* Pointer to int */
  int tflags,
  const char *pUnused,            /* Buffer containing token */
  int nUnused,                    /* Size of token in bytes */
  int iUnused1,                   /* Start offset of token */
  int iUnused2                    /* End offset of token */
){
  int *pCnt = (int*)pContext;
  UNUSED_PARAM2(pUnused, nUnused);
  UNUSED_PARAM2(iUnused1, iUnused2);
  if( (tflags & FTS5_TOKEN_COLOCATED)==0 ){
    (*pCnt)++;
  }
  return SQLITE_OK;
}

static int fts5ApiColumnSize(Fts5Context *pCtx, int iCol, int *pnToken){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
  Fts5Config *pConfig = pTab->p.pConfig;
  int rc = SQLITE_OK;

  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_DOCSIZE) ){
    if( pConfig->bColumnsize ){
      i64 iRowid = fts5CursorRowid(pCsr);
      rc = sqlite3Fts5StorageDocsize(pTab->pStorage, iRowid, pCsr->aColumnSize);
    }else if( !pConfig->zContent || pConfig->eContent==FTS5_CONTENT_UNINDEXED ){
      int i;
      for(i=0; i<pConfig->nCol; i++){
        if( pConfig->abUnindexed[i]==0 ){
          pCsr->aColumnSize[i] = -1;
        }
      }
    }else{
      int i;
      rc = fts5SeekCursor(pCsr, 0);
      for(i=0; rc==SQLITE_OK && i<pConfig->nCol; i++){
        if( pConfig->abUnindexed[i]==0 ){
          const char *z = 0; 
          int n = 0;
          pCsr->aColumnSize[i] = 0;
          rc = fts5TextFromStmt(pConfig, pCsr->pStmt, i, &z, &n);
          if( rc==SQLITE_OK ){
            rc = sqlite3Fts5Tokenize(pConfig, FTS5_TOKENIZE_AUX, 
                z, n, (void*)&pCsr->aColumnSize[i], fts5ColumnSizeCb
            );
          }
          sqlite3Fts5ClearLocale(pConfig);
        }
      }
    }
    CsrFlagClear(pCsr, FTS5CSR_REQUIRE_DOCSIZE);
  }
  if( iCol<0 ){
    int i;
    *pnToken = 0;
    for(i=0; i<pConfig->nCol; i++){
      *pnToken += pCsr->aColumnSize[i];
    }
  }else if( iCol<pConfig->nCol ){
    *pnToken = pCsr->aColumnSize[iCol];
  }else{
    *pnToken = 0;
    rc = SQLITE_RANGE;
  }
  return rc;
}

/*
** Implementation of the xSetAuxdata() method.
*/
static int fts5ApiSetAuxdata(
  Fts5Context *pCtx,              /* Fts5 context */
  void *pPtr,                     /* Pointer to save as auxdata */
  void(*xDelete)(void*)           /* Destructor for pPtr (or NULL) */
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Auxdata *pData;

  /* Search through the cursors list of Fts5Auxdata objects for one that
  ** corresponds to the currently executing auxiliary function.  */
  for(pData=pCsr->pAuxdata; pData; pData=pData->pNext){
    if( pData->pAux==pCsr->pAux ) break;
  }

  if( pData ){
    if( pData->xDelete ){
      pData->xDelete(pData->pPtr);
    }
  }else{
    int rc = SQLITE_OK;
    pData = (Fts5Auxdata*)sqlite3Fts5MallocZero(&rc, sizeof(Fts5Auxdata));
    if( pData==0 ){
      if( xDelete ) xDelete(pPtr);
      return rc;
    }
    pData->pAux = pCsr->pAux;
    pData->pNext = pCsr->pAuxdata;
    pCsr->pAuxdata = pData;
  }

  pData->xDelete = xDelete;
  pData->pPtr = pPtr;
  return SQLITE_OK;
}

static void *fts5ApiGetAuxdata(Fts5Context *pCtx, int bClear){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Auxdata *pData;
  void *pRet = 0;

  for(pData=pCsr->pAuxdata; pData; pData=pData->pNext){
    if( pData->pAux==pCsr->pAux ) break;
  }

  if( pData ){
    pRet = pData->pPtr;
    if( bClear ){
      pData->pPtr = 0;
      pData->xDelete = 0;
    }
  }

  return pRet;
}

static void fts5ApiPhraseNext(
  Fts5Context *pCtx, 
  Fts5PhraseIter *pIter, 
  int *piCol, int *piOff
){
  if( pIter->a>=pIter->b ){
    *piCol = -1;
    *piOff = -1;
  }else{
    int iVal;
    pIter->a += fts5GetVarint32(pIter->a, iVal);
    if( iVal==1 ){
      /* Avoid returning a (*piCol) value that is too large for the table,
      ** even if the position-list is corrupt. The caller might not be
      ** expecting it.  */
      int nCol = ((Fts5Table*)(((Fts5Cursor*)pCtx)->base.pVtab))->pConfig->nCol;
      pIter->a += fts5GetVarint32(pIter->a, iVal);
      *piCol = (iVal>=nCol ? nCol-1 : iVal);
      *piOff = 0;
      pIter->a += fts5GetVarint32(pIter->a, iVal);
    }
    *piOff += (iVal-2);
  }
}

static int fts5ApiPhraseFirst(
  Fts5Context *pCtx, 
  int iPhrase, 
  Fts5PhraseIter *pIter, 
  int *piCol, int *piOff
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  int n;
  int rc = fts5CsrPoslist(pCsr, iPhrase, &pIter->a, &n);
  if( rc==SQLITE_OK ){
    assert( pIter->a || n==0 );
    pIter->b = (pIter->a ? &pIter->a[n] : 0);
    *piCol = 0;
    *piOff = 0;
    fts5ApiPhraseNext(pCtx, pIter, piCol, piOff);
  }
  return rc;
}

static void fts5ApiPhraseNextColumn(
  Fts5Context *pCtx, 
  Fts5PhraseIter *pIter, 
  int *piCol
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Config *pConfig = ((Fts5Table*)(pCsr->base.pVtab))->pConfig;

  if( pConfig->eDetail==FTS5_DETAIL_COLUMNS ){
    if( pIter->a>=pIter->b ){
      *piCol = -1;
    }else{
      int iIncr;
      pIter->a += fts5GetVarint32(&pIter->a[0], iIncr);
      *piCol += (iIncr-2);
    }
  }else{
    while( 1 ){
      int dummy;
      if( pIter->a>=pIter->b ){
        *piCol = -1;
        return;
      }
      if( pIter->a[0]==0x01 ) break;
      pIter->a += fts5GetVarint32(pIter->a, dummy);
    }
    pIter->a += 1 + fts5GetVarint32(&pIter->a[1], *piCol);
  }
}

static int fts5ApiPhraseFirstColumn(
  Fts5Context *pCtx, 
  int iPhrase, 
  Fts5PhraseIter *pIter, 
  int *piCol
){
  int rc = SQLITE_OK;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Config *pConfig = ((Fts5Table*)(pCsr->base.pVtab))->pConfig;

  if( pConfig->eDetail==FTS5_DETAIL_COLUMNS ){
    Fts5Sorter *pSorter = pCsr->pSorter;
    int n;
    if( pSorter ){
      int i1 = (iPhrase==0 ? 0 : pSorter->aIdx[iPhrase-1]);
      n = pSorter->aIdx[iPhrase] - i1;
      pIter->a = &pSorter->aPoslist[i1];
    }else{
      rc = sqlite3Fts5ExprPhraseCollist(pCsr->pExpr, iPhrase, &pIter->a, &n);
    }
    if( rc==SQLITE_OK ){
      assert( pIter->a || n==0 );
      pIter->b = (pIter->a ? &pIter->a[n] : 0);
      *piCol = 0;
      fts5ApiPhraseNextColumn(pCtx, pIter, piCol);
    }
  }else{
    int n;
    rc = fts5CsrPoslist(pCsr, iPhrase, &pIter->a, &n);
    if( rc==SQLITE_OK ){
      assert( pIter->a || n==0 );
      pIter->b = (pIter->a ? &pIter->a[n] : 0);
      if( n<=0 ){
        *piCol = -1;
      }else if( pIter->a[0]==0x01 ){
        pIter->a += 1 + fts5GetVarint32(&pIter->a[1], *piCol);
      }else{
        *piCol = 0;
      }
    }
  }

  return rc;
}

/*
** xQueryToken() API implemenetation.
*/
static int fts5ApiQueryToken(
  Fts5Context* pCtx, 
  int iPhrase, 
  int iToken, 
  const char **ppOut, 
  int *pnOut
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return sqlite3Fts5ExprQueryToken(pCsr->pExpr, iPhrase, iToken, ppOut, pnOut);
}

/*
** xInstToken() API implemenetation.
*/
static int fts5ApiInstToken(
  Fts5Context *pCtx,
  int iIdx, 
  int iToken,
  const char **ppOut, int *pnOut
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  int rc = SQLITE_OK;
  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_INST)==0 
   || SQLITE_OK==(rc = fts5CacheInstArray(pCsr)) 
  ){
    if( iIdx<0 || iIdx>=pCsr->nInstCount ){
      rc = SQLITE_RANGE;
    }else{
      int iPhrase = pCsr->aInst[iIdx*3];
      int iCol = pCsr->aInst[iIdx*3 + 1];
      int iOff = pCsr->aInst[iIdx*3 + 2];
      i64 iRowid = fts5CursorRowid(pCsr);
      rc = sqlite3Fts5ExprInstToken(
          pCsr->pExpr, iRowid, iPhrase, iCol, iOff, iToken, ppOut, pnOut
      );
    }
  }
  return rc;
}


static int fts5ApiQueryPhrase(Fts5Context*, int, void*, 
    int(*)(const Fts5ExtensionApi*, Fts5Context*, void*)
);

/*
** The xColumnLocale() API.
*/
static int fts5ApiColumnLocale(
  Fts5Context *pCtx, 
  int iCol, 
  const char **pzLocale, 
  int *pnLocale
){
  int rc = SQLITE_OK;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Config *pConfig = ((Fts5Table*)(pCsr->base.pVtab))->pConfig;

  *pzLocale = 0;
  *pnLocale = 0;

  assert( pCsr->ePlan!=FTS5_PLAN_SPECIAL );
  if( iCol<0 || iCol>=pConfig->nCol ){
    rc = SQLITE_RANGE;
  }else if(
      pConfig->abUnindexed[iCol]==0
   && 0==fts5IsContentless((Fts5FullTable*)pCsr->base.pVtab, 1)
   && pConfig->bLocale
  ){
    rc = fts5SeekCursor(pCsr, 0);
    if( rc==SQLITE_OK ){
      const char *zDummy = 0;
      int nDummy = 0;
      rc = fts5TextFromStmt(pConfig, pCsr->pStmt, iCol, &zDummy, &nDummy);
      if( rc==SQLITE_OK ){
        *pzLocale = pConfig->t.pLocale;
        *pnLocale = pConfig->t.nLocale;
      }
      sqlite3Fts5ClearLocale(pConfig);
    }
  }

  return rc;
}

static const Fts5ExtensionApi sFts5Api = {
  4,                            /* iVersion */
  fts5ApiUserData,
  fts5ApiColumnCount,
  fts5ApiRowCount,
  fts5ApiColumnTotalSize,
  fts5ApiTokenize,
  fts5ApiPhraseCount,
  fts5ApiPhraseSize,
  fts5ApiInstCount,
  fts5ApiInst,
  fts5ApiRowid,
  fts5ApiColumnText,
  fts5ApiColumnSize,
  fts5ApiQueryPhrase,
  fts5ApiSetAuxdata,
  fts5ApiGetAuxdata,
  fts5ApiPhraseFirst,
  fts5ApiPhraseNext,
  fts5ApiPhraseFirstColumn,
  fts5ApiPhraseNextColumn,
  fts5ApiQueryToken,
  fts5ApiInstToken,
  fts5ApiColumnLocale,
  fts5ApiTokenize_v2
};

/*
** Implementation of API function xQueryPhrase().
*/
static int fts5ApiQueryPhrase(
  Fts5Context *pCtx, 
  int iPhrase, 
  void *pUserData,
  int(*xCallback)(const Fts5ExtensionApi*, Fts5Context*, void*)
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5FullTable *pTab = (Fts5FullTable*)(pCsr->base.pVtab);
  int rc;
  Fts5Cursor *pNew = 0;

  rc = fts5OpenMethod(pCsr->base.pVtab, (sqlite3_vtab_cursor**)&pNew);
  if( rc==SQLITE_OK ){
    pNew->ePlan = FTS5_PLAN_MATCH;
    pNew->iFirstRowid = SMALLEST_INT64;
    pNew->iLastRowid = LARGEST_INT64;
    pNew->base.pVtab = (sqlite3_vtab*)pTab;
    rc = sqlite3Fts5ExprClonePhrase(pCsr->pExpr, iPhrase, &pNew->pExpr);
  }

  if( rc==SQLITE_OK ){
    for(rc = fts5CursorFirst(pTab, pNew, 0);
        rc==SQLITE_OK && CsrFlagTest(pNew, FTS5CSR_EOF)==0;
        rc = fts5NextMethod((sqlite3_vtab_cursor*)pNew)
    ){
      rc = xCallback(&sFts5Api, (Fts5Context*)pNew, pUserData);
      if( rc!=SQLITE_OK ){
        if( rc==SQLITE_DONE ) rc = SQLITE_OK;
        break;
      }
    }
  }

  fts5CloseMethod((sqlite3_vtab_cursor*)pNew);
  return rc;
}

static void fts5ApiInvoke(
  Fts5Auxiliary *pAux,
  Fts5Cursor *pCsr,
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  assert( pCsr->pAux==0 );
  assert( pCsr->ePlan!=FTS5_PLAN_SPECIAL );
  pCsr->pAux = pAux;
  pAux->xFunc(&sFts5Api, (Fts5Context*)pCsr, context, argc, argv);
  pCsr->pAux = 0;
}

static Fts5Cursor *fts5CursorFromCsrid(Fts5Global *pGlobal, i64 iCsrId){
  Fts5Cursor *pCsr;
  for(pCsr=pGlobal->pCsr; pCsr; pCsr=pCsr->pNext){
    if( pCsr->iCsrId==iCsrId ) break;
  }
  return pCsr;
}

/*
** Parameter zFmt is a printf() style formatting string. This function
** formats it using the trailing arguments and returns the result as
** an error message to the context passed as the first argument.
*/
static void fts5ResultError(sqlite3_context *pCtx, const char *zFmt, ...){
  char *zErr = 0;
  va_list ap;
  va_start(ap, zFmt);
  zErr = sqlite3_vmprintf(zFmt, ap);
  sqlite3_result_error(pCtx, zErr, -1);
  sqlite3_free(zErr);
  va_end(ap);
}

static void fts5ApiCallback(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){

  Fts5Auxiliary *pAux;
  Fts5Cursor *pCsr;
  i64 iCsrId;

  assert( argc>=1 );
  pAux = (Fts5Auxiliary*)sqlite3_user_data(context);
  iCsrId = sqlite3_value_int64(argv[0]);

  pCsr = fts5CursorFromCsrid(pAux->pGlobal, iCsrId);
  if( pCsr==0 || (pCsr->ePlan==0 || pCsr->ePlan==FTS5_PLAN_SPECIAL) ){
    fts5ResultError(context, "no such cursor: %lld", iCsrId);
  }else{
    sqlite3_vtab *pTab = pCsr->base.pVtab;
    fts5ApiInvoke(pAux, pCsr, context, argc-1, &argv[1]);
    sqlite3_free(pTab->zErrMsg);
    pTab->zErrMsg = 0;
  }
}


/*
** Given cursor id iId, return a pointer to the corresponding Fts5Table 
** object. Or NULL If the cursor id does not exist.
*/
Fts5Table *sqlite3Fts5TableFromCsrid(
  Fts5Global *pGlobal,            /* FTS5 global context for db handle */
  i64 iCsrId                      /* Id of cursor to find */
){
  Fts5Cursor *pCsr;
  pCsr = fts5CursorFromCsrid(pGlobal, iCsrId);
  if( pCsr ){
    return (Fts5Table*)pCsr->base.pVtab;
  }
  return 0;
}

/*
** Return a "position-list blob" corresponding to the current position of
** cursor pCsr via sqlite3_result_blob(). A position-list blob contains
** the current position-list for each phrase in the query associated with
** cursor pCsr.
**
** A position-list blob begins with (nPhrase-1) varints, where nPhrase is
** the number of phrases in the query. Following the varints are the
** concatenated position lists for each phrase, in order.
**
** The first varint (if it exists) contains the size of the position list
** for phrase 0. The second (same disclaimer) contains the size of position
** list 1. And so on. There is no size field for the final position list,
** as it can be derived from the total size of the blob.
*/
static int fts5PoslistBlob(sqlite3_context *pCtx, Fts5Cursor *pCsr){
  int i;
  int rc = SQLITE_OK;
  int nPhrase = sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
  Fts5Buffer val;

  memset(&val, 0, sizeof(Fts5Buffer));
  switch( ((Fts5Table*)(pCsr->base.pVtab))->pConfig->eDetail ){
    case FTS5_DETAIL_FULL:

      /* Append the varints */
      for(i=0; i<(nPhrase-1); i++){
        const u8 *dummy;
        int nByte = sqlite3Fts5ExprPoslist(pCsr->pExpr, i, &dummy);
        sqlite3Fts5BufferAppendVarint(&rc, &val, nByte);
      }

      /* Append the position lists */
      for(i=0; i<nPhrase; i++){
        const u8 *pPoslist;
        int nPoslist;
        nPoslist = sqlite3Fts5ExprPoslist(pCsr->pExpr, i, &pPoslist);
        sqlite3Fts5BufferAppendBlob(&rc, &val, nPoslist, pPoslist);
      }
      break;

    case FTS5_DETAIL_COLUMNS:

      /* Append the varints */
      for(i=0; rc==SQLITE_OK && i<(nPhrase-1); i++){
        const u8 *dummy;
        int nByte;
        rc = sqlite3Fts5ExprPhraseCollist(pCsr->pExpr, i, &dummy, &nByte);
        sqlite3Fts5BufferAppendVarint(&rc, &val, nByte);
      }

      /* Append the position lists */
      for(i=0; rc==SQLITE_OK && i<nPhrase; i++){
        const u8 *pPoslist;
        int nPoslist;
        rc = sqlite3Fts5ExprPhraseCollist(pCsr->pExpr, i, &pPoslist, &nPoslist);
        sqlite3Fts5BufferAppendBlob(&rc, &val, nPoslist, pPoslist);
      }
      break;

    default:
      break;
  }

  sqlite3_result_blob(pCtx, val.p, val.n, sqlite3_free);
  return rc;
}

/* 
** This is the xColumn method, called by SQLite to request a value from
** the row that the supplied cursor currently points to.
*/
static int fts5ColumnMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite3_context *pCtx,          /* Context for sqlite3_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts5FullTable *pTab = (Fts5FullTable*)(pCursor->pVtab);
  Fts5Config *pConfig = pTab->p.pConfig;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int rc = SQLITE_OK;
  
  assert( CsrFlagTest(pCsr, FTS5CSR_EOF)==0 );

  if( pCsr->ePlan==FTS5_PLAN_SPECIAL ){
    if( iCol==pConfig->nCol ){
      sqlite3_result_int64(pCtx, pCsr->iSpecial);
    }
  }else

  if( iCol==pConfig->nCol ){
    /* User is requesting the value of the special column with the same name
    ** as the table. Return the cursor integer id number. This value is only
    ** useful in that it may be passed as the first argument to an FTS5
    ** auxiliary function.  */
    sqlite3_result_int64(pCtx, pCsr->iCsrId);
  }else if( iCol==pConfig->nCol+1 ){
    /* The value of the "rank" column. */

    if( pCsr->ePlan==FTS5_PLAN_SOURCE ){
      fts5PoslistBlob(pCtx, pCsr);
    }else if( 
        pCsr->ePlan==FTS5_PLAN_MATCH
     || pCsr->ePlan==FTS5_PLAN_SORTED_MATCH
    ){
      if( pCsr->pRank || SQLITE_OK==(rc = fts5FindRankFunction(pCsr)) ){
        fts5ApiInvoke(pCsr->pRank, pCsr, pCtx, pCsr->nRankArg, pCsr->apRankArg);
      }
    }
  }else{
    if( !sqlite3_vtab_nochange(pCtx) && pConfig->eContent!=FTS5_CONTENT_NONE ){
      pConfig->pzErrmsg = &pTab->p.base.zErrMsg;
      rc = fts5SeekCursor(pCsr, 1);
      if( rc==SQLITE_OK ){
        sqlite3_value *pVal = sqlite3_column_value(pCsr->pStmt, iCol+1);
        if( pConfig->bLocale 
         && pConfig->eContent==FTS5_CONTENT_EXTERNAL 
         && sqlite3Fts5IsLocaleValue(pConfig, pVal)
        ){
          const char *z = 0;
          int n = 0;
          rc = fts5TextFromStmt(pConfig, pCsr->pStmt, iCol, &z, &n);
          if( rc==SQLITE_OK ){
            sqlite3_result_text(pCtx, z, n, SQLITE_TRANSIENT);
          }
          sqlite3Fts5ClearLocale(pConfig);
        }else{
          sqlite3_result_value(pCtx, pVal);
        }
      }

      pConfig->pzErrmsg = 0;
    }
  }

  return rc;
}


/*
** This routine implements the xFindFunction method for the FTS3
** virtual table.
*/
static int fts5FindFunctionMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  int nUnused,                    /* Number of SQL function arguments */
  const char *zName,              /* Name of SQL function */
  void (**pxFunc)(sqlite3_context*,int,sqlite3_value**), /* OUT: Result */
  void **ppArg                    /* OUT: User data for *pxFunc */
){
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  Fts5Auxiliary *pAux;

  UNUSED_PARAM(nUnused);
  pAux = fts5FindAuxiliary(pTab, zName);
  if( pAux ){
    *pxFunc = fts5ApiCallback;
    *ppArg = (void*)pAux;
    return 1;
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}

/*
** Implementation of FTS5 xRename method. Rename an fts5 table.
*/
static int fts5RenameMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  const char *zName               /* New name of table */
){
  int rc;
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  rc = sqlite3Fts5StorageRename(pTab->pStorage, zName);
  return rc;
}

int sqlite3Fts5FlushToDisk(Fts5Table *pTab){
  fts5TripCursors((Fts5FullTable*)pTab);
  return sqlite3Fts5StorageSync(((Fts5FullTable*)pTab)->pStorage);
}

/*
** The xSavepoint() method.
**
** Flush the contents of the pending-terms table to disk.
*/
static int fts5SavepointMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  int rc = SQLITE_OK;

  fts5CheckTransactionState(pTab, FTS5_SAVEPOINT, iSavepoint);
  rc = sqlite3Fts5FlushToDisk((Fts5Table*)pVtab);
  if( rc==SQLITE_OK ){
    pTab->iSavepoint = iSavepoint+1;
  }
  return rc;
}

/*
** The xRelease() method.
**
** This is a no-op.
*/
static int fts5ReleaseMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  int rc = SQLITE_OK;
  fts5CheckTransactionState(pTab, FTS5_RELEASE, iSavepoint);
  if( (iSavepoint+1)<pTab->iSavepoint ){
    rc = sqlite3Fts5FlushToDisk(&pTab->p);
    if( rc==SQLITE_OK ){
      pTab->iSavepoint = iSavepoint;
    }
  }
  return rc;
}

/*
** The xRollbackTo() method.
**
** Discard the contents of the pending terms table.
*/
static int fts5RollbackToMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  int rc = SQLITE_OK;
  fts5CheckTransactionState(pTab, FTS5_ROLLBACKTO, iSavepoint);
  fts5TripCursors(pTab);
  if( (iSavepoint+1)<=pTab->iSavepoint ){
    pTab->p.pConfig->pgsz = 0;
    rc = sqlite3Fts5StorageRollback(pTab->pStorage);
  }
  return rc;
}

/*
** Register a new auxiliary function with global context pGlobal.
*/
static int fts5CreateAux(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void *pUserData,                /* User data for aux. function */
  fts5_extension_function xFunc,  /* Aux. function implementation */
  void(*xDestroy)(void*)          /* Destructor for pUserData */
){
  Fts5Global *pGlobal = (Fts5Global*)pApi;
  int rc = sqlite3_overload_function(pGlobal->db, zName, -1);
  if( rc==SQLITE_OK ){
    Fts5Auxiliary *pAux;
    sqlite3_int64 nName;            /* Size of zName in bytes, including \0 */
    sqlite3_int64 nByte;            /* Bytes of space to allocate */

    nName = strlen(zName) + 1;
    nByte = sizeof(Fts5Auxiliary) + nName;
    pAux = (Fts5Auxiliary*)sqlite3_malloc64(nByte);
    if( pAux ){
      memset(pAux, 0, (size_t)nByte);
      pAux->zFunc = (char*)&pAux[1];
      memcpy(pAux->zFunc, zName, nName);
      pAux->pGlobal = pGlobal;
      pAux->pUserData = pUserData;
      pAux->xFunc = xFunc;
      pAux->xDestroy = xDestroy;
      pAux->pNext = pGlobal->pAux;
      pGlobal->pAux = pAux;
    }else{
      rc = SQLITE_NOMEM;
    }
  }

  return rc;
}

/*
** This function is used by xCreateTokenizer_v2() and xCreateTokenizer().
** It allocates and partially populates a new Fts5TokenizerModule object.
** The new object is already linked into the Fts5Global context before
** returning.
**
** If successful, SQLITE_OK is returned and a pointer to the new
** Fts5TokenizerModule object returned via output parameter (*ppNew). All
** that is required is for the caller to fill in the methods in
** Fts5TokenizerModule.x1 and x2, and to set Fts5TokenizerModule.bV2Native
** as appropriate.
**
** If an error occurs, an SQLite error code is returned and the final value
** of (*ppNew) undefined.
*/
static int fts5NewTokenizerModule(
  Fts5Global *pGlobal,            /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void *pUserData,                /* User data for aux. function */
  void(*xDestroy)(void*),         /* Destructor for pUserData */
  Fts5TokenizerModule **ppNew
){
  int rc = SQLITE_OK;
  Fts5TokenizerModule *pNew;
  sqlite3_int64 nName;          /* Size of zName and its \0 terminator */
  sqlite3_int64 nByte;          /* Bytes of space to allocate */

  nName = strlen(zName) + 1;
  nByte = sizeof(Fts5TokenizerModule) + nName;
  *ppNew = pNew = (Fts5TokenizerModule*)sqlite3Fts5MallocZero(&rc, nByte);
  if( pNew ){
    pNew->zName = (char*)&pNew[1];
    memcpy(pNew->zName, zName, nName);
    pNew->pUserData = pUserData;
    pNew->xDestroy = xDestroy;
    pNew->pNext = pGlobal->pTok;
    pGlobal->pTok = pNew;
    if( pNew->pNext==0 ){
      pGlobal->pDfltTok = pNew;
    }
  }

  return rc;
}

/*
** An instance of this type is used as the Fts5Tokenizer object for
** wrapper tokenizers - those that provide access to a v1 tokenizer via
** the fts5_tokenizer_v2 API, and those that provide access to a v2 tokenizer
** via the fts5_tokenizer API.
*/
typedef struct Fts5VtoVTokenizer Fts5VtoVTokenizer;
struct Fts5VtoVTokenizer {
  int bV2Native;                  /* True if v2 native tokenizer */
  fts5_tokenizer x1;              /* Tokenizer functions */
  fts5_tokenizer_v2 x2;           /* V2 tokenizer functions */
  Fts5Tokenizer *pReal;
};

/*
** Create a wrapper tokenizer. The context argument pCtx points to the
** Fts5TokenizerModule object.
*/
static int fts5VtoVCreate(
  void *pCtx, 
  const char **azArg, 
  int nArg, 
  Fts5Tokenizer **ppOut
){
  Fts5TokenizerModule *pMod = (Fts5TokenizerModule*)pCtx;
  Fts5VtoVTokenizer *pNew = 0;
  int rc = SQLITE_OK;

  pNew = (Fts5VtoVTokenizer*)sqlite3Fts5MallocZero(&rc, sizeof(*pNew));
  if( rc==SQLITE_OK ){
    pNew->x1 = pMod->x1;
    pNew->x2 = pMod->x2;
    pNew->bV2Native = pMod->bV2Native;
    if( pMod->bV2Native ){
      rc = pMod->x2.xCreate(pMod->pUserData, azArg, nArg, &pNew->pReal);
    }else{
      rc = pMod->x1.xCreate(pMod->pUserData, azArg, nArg, &pNew->pReal);
    }
    if( rc!=SQLITE_OK ){
      sqlite3_free(pNew);
      pNew = 0;
    }
  }

  *ppOut = (Fts5Tokenizer*)pNew;
  return rc;
}

/*
** Delete an Fts5VtoVTokenizer wrapper tokenizer. 
*/
static void fts5VtoVDelete(Fts5Tokenizer *pTok){
  Fts5VtoVTokenizer *p = (Fts5VtoVTokenizer*)pTok;
  if( p ){
    if( p->bV2Native ){
      p->x2.xDelete(p->pReal);
    }else{
      p->x1.xDelete(p->pReal);
    }
    sqlite3_free(p);
  }
}


/*
** xTokenizer method for a wrapper tokenizer that offers the v1 interface
** (no support for locales).
*/
static int fts5V1toV2Tokenize(
  Fts5Tokenizer *pTok, 
  void *pCtx, int flags,
  const char *pText, int nText, 
  int (*xToken)(void*, int, const char*, int, int, int)
){
  Fts5VtoVTokenizer *p = (Fts5VtoVTokenizer*)pTok;
  assert( p->bV2Native );
  return p->x2.xTokenize(p->pReal, pCtx, flags, pText, nText, 0, 0, xToken);
}

/*
** xTokenizer method for a wrapper tokenizer that offers the v2 interface
** (with locale support).
*/
static int fts5V2toV1Tokenize(
  Fts5Tokenizer *pTok, 
  void *pCtx, int flags,
  const char *pText, int nText, 
  const char *pLocale, int nLocale, 
  int (*xToken)(void*, int, const char*, int, int, int)
){
  Fts5VtoVTokenizer *p = (Fts5VtoVTokenizer*)pTok;
  assert( p->bV2Native==0 );
  UNUSED_PARAM2(pLocale,nLocale);
  return p->x1.xTokenize(p->pReal, pCtx, flags, pText, nText, xToken);
}

/*
** Register a new tokenizer. This is the implementation of the 
** fts5_api.xCreateTokenizer_v2() method.
*/
static int fts5CreateTokenizer_v2(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void *pUserData,                /* User data for aux. function */
  fts5_tokenizer_v2 *pTokenizer,  /* Tokenizer implementation */
  void(*xDestroy)(void*)          /* Destructor for pUserData */
){
  Fts5Global *pGlobal = (Fts5Global*)pApi;
  int rc = SQLITE_OK;

  if( pTokenizer->iVersion>2 ){
    rc = SQLITE_ERROR;
  }else{
    Fts5TokenizerModule *pNew = 0;
    rc = fts5NewTokenizerModule(pGlobal, zName, pUserData, xDestroy, &pNew);
    if( pNew ){
      pNew->x2 = *pTokenizer;
      pNew->bV2Native = 1;
      pNew->x1.xCreate = fts5VtoVCreate;
      pNew->x1.xTokenize = fts5V1toV2Tokenize;
      pNew->x1.xDelete = fts5VtoVDelete;
    }
  }

  return rc;
}

/*
** The fts5_api.xCreateTokenizer() method.
*/
static int fts5CreateTokenizer(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void *pUserData,                /* User data for aux. function */
  fts5_tokenizer *pTokenizer,     /* Tokenizer implementation */
  void(*xDestroy)(void*)          /* Destructor for pUserData */
){
  Fts5TokenizerModule *pNew = 0;
  int rc = SQLITE_OK;

  rc = fts5NewTokenizerModule(
      (Fts5Global*)pApi, zName, pUserData, xDestroy, &pNew
  );
  if( pNew ){
    pNew->x1 = *pTokenizer;
    pNew->x2.xCreate = fts5VtoVCreate;
    pNew->x2.xTokenize = fts5V2toV1Tokenize;
    pNew->x2.xDelete = fts5VtoVDelete;
  }
  return rc;
}

/*
** Search the global context passed as the first argument for a tokenizer
** module named zName. If found, return a pointer to the Fts5TokenizerModule
** object. Otherwise, return NULL.
*/
static Fts5TokenizerModule *fts5LocateTokenizer(
  Fts5Global *pGlobal,            /* Global (one per db handle) object */
  const char *zName               /* Name of tokenizer module to find */
){
  Fts5TokenizerModule *pMod = 0;

  if( zName==0 ){
    pMod = pGlobal->pDfltTok;
  }else{
    for(pMod=pGlobal->pTok; pMod; pMod=pMod->pNext){
      if( sqlite3_stricmp(zName, pMod->zName)==0 ) break;
    }
  }

  return pMod;
}

/*
** Find a tokenizer. This is the implementation of the 
** fts5_api.xFindTokenizer_v2() method.
*/
static int fts5FindTokenizer_v2(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of tokenizer */
  void **ppUserData,
  fts5_tokenizer_v2 **ppTokenizer /* Populate this object */
){
  int rc = SQLITE_OK;
  Fts5TokenizerModule *pMod;

  pMod = fts5LocateTokenizer((Fts5Global*)pApi, zName);
  if( pMod ){
    if( pMod->bV2Native ){
      *ppUserData = pMod->pUserData;
    }else{
      *ppUserData = (void*)pMod;
    }
    *ppTokenizer = &pMod->x2;
  }else{
    *ppTokenizer = 0;
    *ppUserData = 0;
    rc = SQLITE_ERROR;
  }

  return rc;
}

/*
** Find a tokenizer. This is the implementation of the 
** fts5_api.xFindTokenizer() method.
*/
static int fts5FindTokenizer(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void **ppUserData,
  fts5_tokenizer *pTokenizer      /* Populate this object */
){
  int rc = SQLITE_OK;
  Fts5TokenizerModule *pMod;

  pMod = fts5LocateTokenizer((Fts5Global*)pApi, zName);
  if( pMod ){
    if( pMod->bV2Native==0 ){
      *ppUserData = pMod->pUserData;
    }else{
      *ppUserData = (void*)pMod;
    }
    *pTokenizer = pMod->x1;
  }else{
    memset(pTokenizer, 0, sizeof(*pTokenizer));
    *ppUserData = 0;
    rc = SQLITE_ERROR;
  }

  return rc;
}

/*
** Attempt to instantiate the tokenizer.
*/
int sqlite3Fts5LoadTokenizer(Fts5Config *pConfig){
  const char **azArg = pConfig->t.azArg;
  const int nArg = pConfig->t.nArg;
  Fts5TokenizerModule *pMod = 0;
  int rc = SQLITE_OK;

  pMod = fts5LocateTokenizer(pConfig->pGlobal, nArg==0 ? 0 : azArg[0]);
  if( pMod==0 ){
    assert( nArg>0 );
    rc = SQLITE_ERROR;
    sqlite3Fts5ConfigErrmsg(pConfig, "no such tokenizer: %s", azArg[0]);
  }else{
    int (*xCreate)(void*, const char**, int, Fts5Tokenizer**) = 0;
    if( pMod->bV2Native ){
      xCreate = pMod->x2.xCreate;
      pConfig->t.pApi2 = &pMod->x2;
    }else{
      pConfig->t.pApi1 = &pMod->x1;
      xCreate = pMod->x1.xCreate;
    }
    
    rc = xCreate(pMod->pUserData, 
        (azArg?&azArg[1]:0), (nArg?nArg-1:0), &pConfig->t.pTok
    );

    if( rc!=SQLITE_OK ){
      if( rc!=SQLITE_NOMEM ){
        sqlite3Fts5ConfigErrmsg(pConfig, "error in tokenizer constructor");
      }
    }else if( pMod->bV2Native==0 ){
      pConfig->t.ePattern = sqlite3Fts5TokenizerPattern(
          pMod->x1.xCreate, pConfig->t.pTok
      );
    }
  }

  if( rc!=SQLITE_OK ){
    pConfig->t.pApi1 = 0;
    pConfig->t.pApi2 = 0;
    pConfig->t.pTok = 0;
  }

  return rc;
}


/*
** xDestroy callback passed to sqlite3_create_module(). This is invoked
** when the db handle is being closed. Free memory associated with 
** tokenizers and aux functions registered with this db handle.
*/
static void fts5ModuleDestroy(void *pCtx){
  Fts5TokenizerModule *pTok, *pNextTok;
  Fts5Auxiliary *pAux, *pNextAux;
  Fts5Global *pGlobal = (Fts5Global*)pCtx;

  for(pAux=pGlobal->pAux; pAux; pAux=pNextAux){
    pNextAux = pAux->pNext;
    if( pAux->xDestroy ) pAux->xDestroy(pAux->pUserData);
    sqlite3_free(pAux);
  }

  for(pTok=pGlobal->pTok; pTok; pTok=pNextTok){
    pNextTok = pTok->pNext;
    if( pTok->xDestroy ) pTok->xDestroy(pTok->pUserData);
    sqlite3_free(pTok);
  }

  sqlite3_free(pGlobal);
}

/*
** Implementation of the fts5() function used by clients to obtain the
** API pointer.
*/
static void fts5Fts5Func(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args */
  sqlite3_value **apArg           /* Function arguments */
){
  Fts5Global *pGlobal = (Fts5Global*)sqlite3_user_data(pCtx);
  fts5_api **ppApi;
  UNUSED_PARAM(nArg);
  assert( nArg==1 );
  ppApi = (fts5_api**)sqlite3_value_pointer(apArg[0], "fts5_api_ptr");
  if( ppApi ) *ppApi = &pGlobal->api;
}

/*
** Implementation of fts5_source_id() function.
*/
static void fts5SourceIdFunc(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args */
  sqlite3_value **apUnused        /* Function arguments */
){
  assert( nArg==0 );
  UNUSED_PARAM2(nArg, apUnused);
  sqlite3_result_text(pCtx, "--FTS5-SOURCE-ID--", -1, SQLITE_TRANSIENT);
}

/*
** Implementation of fts5_locale(LOCALE, TEXT) function.
**
** If parameter LOCALE is NULL, or a zero-length string, then a copy of
** TEXT is returned. Otherwise, both LOCALE and TEXT are interpreted as
** text, and the value returned is a blob consisting of:
**
**     * The 4 bytes 0x00, 0xE0, 0xB2, 0xEb (FTS5_LOCALE_HEADER).
**     * The LOCALE, as utf-8 text, followed by
**     * 0x00, followed by
**     * The TEXT, as utf-8 text.
**
** There is no final nul-terminator following the TEXT value.
*/
static void fts5LocaleFunc(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args */
  sqlite3_value **apArg           /* Function arguments */
){
  const char *zLocale = 0;
  int nLocale = 0;
  const char *zText = 0;
  int nText = 0;

  assert( nArg==2 );
  UNUSED_PARAM(nArg);

  zLocale = (const char*)sqlite3_value_text(apArg[0]);
  nLocale = sqlite3_value_bytes(apArg[0]);

  zText = (const char*)sqlite3_value_text(apArg[1]);
  nText = sqlite3_value_bytes(apArg[1]);

  if( zLocale==0 || zLocale[0]=='\0' ){
    sqlite3_result_text(pCtx, zText, nText, SQLITE_TRANSIENT);
  }else{
    Fts5Global *p = (Fts5Global*)sqlite3_user_data(pCtx);
    u8 *pBlob = 0;
    u8 *pCsr = 0;
    int nBlob = 0;

    nBlob = FTS5_LOCALE_HDR_SIZE + nLocale + 1 + nText;
    pBlob = (u8*)sqlite3_malloc(nBlob);
    if( pBlob==0 ){
      sqlite3_result_error_nomem(pCtx);
      return;
    }

    pCsr = pBlob;
    memcpy(pCsr, (const u8*)p->aLocaleHdr, FTS5_LOCALE_HDR_SIZE);
    pCsr += FTS5_LOCALE_HDR_SIZE;
    memcpy(pCsr, zLocale, nLocale);
    pCsr += nLocale;
    (*pCsr++) = 0x00;
    if( zText ) memcpy(pCsr, zText, nText);
    assert( &pCsr[nText]==&pBlob[nBlob] );

    sqlite3_result_blob(pCtx, pBlob, nBlob, sqlite3_free);
  }
}

/*
** Implementation of fts5_insttoken() function.
*/
static void fts5InsttokenFunc(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args */
  sqlite3_value **apArg           /* Function arguments */
){
  assert( nArg==1 );
  (void)nArg;
  sqlite3_result_value(pCtx, apArg[0]);
  sqlite3_result_subtype(pCtx, FTS5_INSTTOKEN_SUBTYPE);
}

/*
** Return true if zName is the extension on one of the shadow tables used
** by this module.
*/
static int fts5ShadowName(const char *zName){
  static const char *azName[] = {
    "config", "content", "data", "docsize", "idx"
  };
  unsigned int i;
  for(i=0; i<sizeof(azName)/sizeof(azName[0]); i++){
    if( sqlite3_stricmp(zName, azName[i])==0 ) return 1;
  }
  return 0;
}

/*
** Run an integrity check on the FTS5 data structures.  Return a string
** if anything is found amiss.  Return a NULL pointer if everything is
** OK.
*/
static int fts5IntegrityMethod(
  sqlite3_vtab *pVtab,    /* the FTS5 virtual table to check */
  const char *zSchema,    /* Name of schema in which this table lives */
  const char *zTabname,   /* Name of the table itself */
  int isQuick,            /* True if this is a quick-check */
  char **pzErr            /* Write error message here */
){
  Fts5FullTable *pTab = (Fts5FullTable*)pVtab;
  int rc;

  assert( pzErr!=0 && *pzErr==0 );
  UNUSED_PARAM(isQuick);
  assert( pTab->p.pConfig->pzErrmsg==0 );
  pTab->p.pConfig->pzErrmsg = pzErr;
  rc = sqlite3Fts5StorageIntegrity(pTab->pStorage, 0);
  if( *pzErr==0 && rc!=SQLITE_OK ){
    if( (rc&0xff)==SQLITE_CORRUPT ){
      *pzErr = sqlite3_mprintf("malformed inverted index for FTS5 table %s.%s",
          zSchema, zTabname);
      rc = (*pzErr) ? SQLITE_OK : SQLITE_NOMEM;
    }else{
      *pzErr = sqlite3_mprintf("unable to validate the inverted index for"
          " FTS5 table %s.%s: %s",
          zSchema, zTabname, sqlite3_errstr(rc));
    }
  }else if( (rc&0xff)==SQLITE_CORRUPT ){
    rc = SQLITE_OK;
  }
  sqlite3Fts5IndexCloseReader(pTab->p.pIndex);
  pTab->p.pConfig->pzErrmsg = 0;

  return rc;
}

static int fts5Init(sqlite3 *db){
  static const sqlite3_module fts5Mod = {
    /* iVersion      */ 4,
    /* xCreate       */ fts5CreateMethod,
    /* xConnect      */ fts5ConnectMethod,
    /* xBestIndex    */ fts5BestIndexMethod,
    /* xDisconnect   */ fts5DisconnectMethod,
    /* xDestroy      */ fts5DestroyMethod,
    /* xOpen         */ fts5OpenMethod,
    /* xClose        */ fts5CloseMethod,
    /* xFilter       */ fts5FilterMethod,
    /* xNext         */ fts5NextMethod,
    /* xEof          */ fts5EofMethod,
    /* xColumn       */ fts5ColumnMethod,
    /* xRowid        */ fts5RowidMethod,
    /* xUpdate       */ fts5UpdateMethod,
    /* xBegin        */ fts5BeginMethod,
    /* xSync         */ fts5SyncMethod,
    /* xCommit       */ fts5CommitMethod,
    /* xRollback     */ fts5RollbackMethod,
    /* xFindFunction */ fts5FindFunctionMethod,
    /* xRename       */ fts5RenameMethod,
    /* xSavepoint    */ fts5SavepointMethod,
    /* xRelease      */ fts5ReleaseMethod,
    /* xRollbackTo   */ fts5RollbackToMethod,
    /* xShadowName   */ fts5ShadowName,
    /* xIntegrity    */ fts5IntegrityMethod
  };

  int rc;
  Fts5Global *pGlobal = 0;

  pGlobal = (Fts5Global*)sqlite3_malloc(sizeof(Fts5Global));
  if( pGlobal==0 ){
    rc = SQLITE_NOMEM;
  }else{
    void *p = (void*)pGlobal;
    memset(pGlobal, 0, sizeof(Fts5Global));
    pGlobal->db = db;
    pGlobal->api.iVersion = 3;
    pGlobal->api.xCreateFunction = fts5CreateAux;
    pGlobal->api.xCreateTokenizer = fts5CreateTokenizer;
    pGlobal->api.xFindTokenizer = fts5FindTokenizer;
    pGlobal->api.xCreateTokenizer_v2 = fts5CreateTokenizer_v2;
    pGlobal->api.xFindTokenizer_v2 = fts5FindTokenizer_v2;

    /* Initialize pGlobal->aLocaleHdr[] to a 128-bit pseudo-random vector.
    ** The constants below were generated randomly.  */
    sqlite3_randomness(sizeof(pGlobal->aLocaleHdr), pGlobal->aLocaleHdr);
    pGlobal->aLocaleHdr[0] ^= 0xF924976D;
    pGlobal->aLocaleHdr[1] ^= 0x16596E13;
    pGlobal->aLocaleHdr[2] ^= 0x7C80BEAA;
    pGlobal->aLocaleHdr[3] ^= 0x9B03A67F;
    assert( sizeof(pGlobal->aLocaleHdr)==16 );

    rc = sqlite3_create_module_v2(db, "fts5", &fts5Mod, p, fts5ModuleDestroy);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5IndexInit(db);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5ExprInit(pGlobal, db);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5AuxInit(&pGlobal->api);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5TokenizerInit(&pGlobal->api);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5VocabInit(pGlobal, db);
    if( rc==SQLITE_OK ){
      rc = sqlite3_create_function(
          db, "fts5", 1, SQLITE_UTF8, p, fts5Fts5Func, 0, 0
      );
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3_create_function(
          db, "fts5_source_id", 0, 
          SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS,
          p, fts5SourceIdFunc, 0, 0
      );
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3_create_function(
          db, "fts5_locale", 2, 
          SQLITE_UTF8|SQLITE_INNOCUOUS|SQLITE_RESULT_SUBTYPE|SQLITE_SUBTYPE,
          p, fts5LocaleFunc, 0, 0
      );
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3_create_function(
          db, "fts5_insttoken", 1, 
          SQLITE_UTF8|SQLITE_INNOCUOUS|SQLITE_RESULT_SUBTYPE,
          p, fts5InsttokenFunc, 0, 0
      );
    }
  }

  /* If SQLITE_FTS5_ENABLE_TEST_MI is defined, assume that the file
  ** fts5_test_mi.c is compiled and linked into the executable. And call
  ** its entry point to enable the matchinfo() demo.  */
#ifdef SQLITE_FTS5_ENABLE_TEST_MI
  if( rc==SQLITE_OK ){
    extern int sqlite3Fts5TestRegisterMatchinfoAPI(fts5_api*);
    rc = sqlite3Fts5TestRegisterMatchinfoAPI(&pGlobal->api);
  }
#endif

  return rc;
}

/*
** The following functions are used to register the module with SQLite. If
** this module is being built as part of the SQLite core (SQLITE_CORE is
** defined), then sqlite3_open() will call sqlite3Fts5Init() directly.
**
** Or, if this module is being built as a loadable extension, 
** sqlite3Fts5Init() is omitted and the two standard entry points
** sqlite3_fts_init() and sqlite3_fts5_init() defined instead.
*/
#ifndef SQLITE_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_fts_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  return fts5Init(db);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_fts5_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  return fts5Init(db);
}
#else
int sqlite3Fts5Init(sqlite3 *db){
  return fts5Init(db);
}
#endif
