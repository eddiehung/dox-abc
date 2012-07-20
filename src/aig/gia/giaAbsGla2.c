/**CFile****************************************************************

  FileName    [gia.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Scalable AIG package.]

  Synopsis    [Scalable gate-level abstraction.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: gia.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "gia.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Ga2_Man_t_ Ga2_Man_t; // manager
struct Ga2_Man_t_
{
    // user data
    Gia_Man_t *    pGia;            // working AIG manager
    Gia_ParVta_t * pPars;           // parameters
    // internal data
    int            nObjs;           // the number of objects (abstracted and PPIs)
    int            nObjsAlloc;      // the number of allocated objects
    Vec_Int_t *    vAbs;            // array of abstracted objects
    int            nAbs;            // starting abstraction
//    Vec_Int_t *    vExtra;          // additional objects 
    // object structure
    Vec_Int_t *    pvLeaves;        // leaves for each object
    Vec_Int_t *    pvCnf0;          // positive CNF 
    Vec_Int_t *    pvCnf1;          // negative CNF
    Vec_Int_t *    pvMap;           // mapping into SAT vars for each frame  
    // temporaries
    Vec_Int_t *    vCnf;
    Vec_Int_t *    vLits;
    Vec_Int_t *    vIsopMem;
    // other data
    sat_solver2 *  pSat;            // incremental SAT solver
    int            nSatVars;        // the number of SAT variables
    // statistics  
    clock_t        timeStart;
    clock_t        timeInit;
    clock_t        timeSat;
    clock_t        timeUnsat;
    clock_t        timeCex;
    clock_t        timeOther;
};

// returns literal of this object, or -1 if the literal is not assigned
static inline int Ga2_ObjFindLit( Ga2_Man_t * p, Gia_Obj_t * pObj, int f )  
{ 
    Vec_Int_t * vMap;
    assert( pObj->fPhase );
    if ( pObj->Value == 0 )
        return -1;
    vMap = &p->pvMap[pObj->Value];
    if ( f < Vec_IntSize(vMap) )
        return -1;
    return Vec_IntEntry( vMap, f );
}
// inserts the literal for this object
static inline void Ga2_ObjAddLit( Ga2_Man_t * p, Gia_Obj_t * pObj, int f, int Lit )  
{ 
    Vec_Int_t * vMap;
    assert( Lit > 1 );
    assert( pObj->fPhase ); 
    assert( Ga2_ObjFindLit(p, pObj, f) == -1 );
    if ( pObj->Value == 0 )
        pObj->Value = p->nObjs++;
    vMap = &p->pvMap[pObj->Value];
    Vec_IntSetEntry( vMap, f, Lit );
}
// returns 
static inline int Ga2_ObjFindOrAddLit( Ga2_Man_t * p, Gia_Obj_t * pObj, int f )  
{ 
    int Lit = Ga2_ObjFindLit( p, pObj, f );
    if ( Lit == -1 )
    {
        Lit = toLitCond( p->nSatVars++, 0 );
        Ga2_ObjAddLit( p, pObj, f, Lit );
    }
    assert( Lit > 1 );
    return Lit;
}

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    [Returns AIG marked for CNF generation.]

  Description [The marking satisfies the following requirements:
  Each marked node has the number of marked fanins no more than N.]
               
  SideEffects [Uses pObj->fPhase to store the markings.]

  SeeAlso     []

***********************************************************************/
int Ga2_ManBreakTree_rec( Gia_Man_t * p, Gia_Obj_t * pObj, int fFirst, int N )
{   // breaks a tree rooted at the node into N-feasible subtrees
    int Val0, Val1;
    if ( pObj->fPhase && !fFirst )
        return 1;
    Val0 = Ga2_ManBreakTree_rec( p, Gia_ObjFanin0(pObj), 0, N );
    Val1 = Ga2_ManBreakTree_rec( p, Gia_ObjFanin1(pObj), 0, N );
    if ( Val0 + Val1 < N )
        return Val0 + Val1;
    if ( Val0 + Val1 == N )
    {
        pObj->fPhase = 1;
        return 1;
    }
    assert( Val0 + Val1 > N );
    assert( Val0 < N && Val1 < N );
    if ( Val0 >= Val1 )
    {
        Gia_ObjFanin0(pObj)->fPhase = 1;
        Val0 = 1;
    }
    else 
    {
        Gia_ObjFanin1(pObj)->fPhase = 1;
        Val1 = 1;
    }
    if ( Val0 + Val1 < N )
        return Val0 + Val1;
    if ( Val0 + Val1 == N )
    {
        pObj->fPhase = 1;
        return 1;
    }
    assert( 0 );
    return -1;
}
int Ga2_ManCheckNodesAnd( Gia_Man_t * p, Vec_Int_t * vNodes )
{
    Gia_Obj_t * pObj;
    int i;
    Gia_ManForEachObjVec( vNodes, p, pObj, i )
        if ( (!Gia_ObjFanin0(pObj)->fPhase && Gia_ObjFaninC0(pObj)) || 
             (!Gia_ObjFanin1(pObj)->fPhase && Gia_ObjFaninC1(pObj)) )
            return 0;
    return 1;
}
void Ga2_ManCollectNodes_rec( Gia_Man_t * p, Gia_Obj_t * pObj, Vec_Int_t * vNodes, int fFirst )
{
    if ( pObj->fPhase && !fFirst )
        return;
    assert( Gia_ObjIsAnd(pObj) );
    Ga2_ManCollectNodes_rec( p, Gia_ObjFanin0(pObj), vNodes, 0 );
    Ga2_ManCollectNodes_rec( p, Gia_ObjFanin1(pObj), vNodes, 0 );
    Vec_IntPush( vNodes, Gia_ObjId(p, pObj) );

}
void Ga2_ManCollectLeaves_rec( Gia_Man_t * p, Gia_Obj_t * pObj, Vec_Int_t * vLeaves, int fFirst )
{
    if ( pObj->fPhase && !fFirst )
    {
        Vec_IntPushUnique( vLeaves, Gia_ObjId(p, pObj) );
        return;
    }
    assert( Gia_ObjIsAnd(pObj) );
    Ga2_ManCollectLeaves_rec( p, Gia_ObjFanin0(pObj), vLeaves, 0 );
    Ga2_ManCollectLeaves_rec( p, Gia_ObjFanin1(pObj), vLeaves, 0 );
}
int Ga2_ManMarkup( Gia_Man_t * p, int N )
{
    clock_t clk = clock();
    Vec_Int_t * vLeaves;
    Gia_Obj_t * pObj;
    int i, CountMarks;
    // label nodes with multiple fanouts and inputs MUXes
    Gia_ManForEachObj( p, pObj, i )
    {
        pObj->Value = 0;
        if ( !Gia_ObjIsAnd(pObj) )
            continue;
        Gia_ObjFanin0(pObj)->Value++;
        Gia_ObjFanin1(pObj)->Value++;
        if ( !Gia_ObjIsMuxType(pObj) )
            continue;
        Gia_ObjFanin0(Gia_ObjFanin0(pObj))->Value++;
        Gia_ObjFanin1(Gia_ObjFanin0(pObj))->Value++;
        Gia_ObjFanin0(Gia_ObjFanin1(pObj))->Value++;
        Gia_ObjFanin1(Gia_ObjFanin1(pObj))->Value++;
    }
    Gia_ManForEachObj( p, pObj, i )
    {
        pObj->fPhase = 0;
        if ( Gia_ObjIsAnd(pObj) )
            pObj->fPhase = (pObj->Value > 1);
        else if ( Gia_ObjIsCo(pObj) )
            Gia_ObjFanin0(pObj)->fPhase = 1;
        else 
            pObj->fPhase = 1;
        pObj->Value = 0;
    } 
    // add marks when needed
    vLeaves = Vec_IntAlloc( 100 );
    Gia_ManForEachAnd( p, pObj, i )
    {
        if ( !pObj->fPhase )
            continue;
        Vec_IntClear( vLeaves );
        Ga2_ManCollectLeaves_rec( p, pObj, vLeaves, 1 );
        if ( Vec_IntSize(vLeaves) > N )
            Ga2_ManBreakTree_rec( p, pObj, 1, N );
    }
    // verify that the tree is split correctly
    CountMarks = 0;
    Gia_ManForEachAnd( p, pObj, i )
    {
        if ( !pObj->fPhase )
            continue;
        Vec_IntClear( vLeaves );
        Ga2_ManCollectLeaves_rec( p, pObj, vLeaves, 1 );
        assert( Vec_IntSize(vLeaves) <= N );
//        printf( "%d ", Vec_IntSize(vLeaves) );
        CountMarks++;
    }
//    printf( "Internal nodes = %d.   ", CountMarks );
    Abc_PrintTime( 1, "Time", clock() - clk );
    Vec_IntFree( vLeaves );
    return CountMarks;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Gla_Man_t * Ga2_ManStart( Gia_Man_t * pGia, Gia_ParVta_t * pPars )
{
    Gla_Man_t * p;
    int Lit;
    p = ABC_CALLOC( Gla_Man_t, 1 );
    p->pGia  = pGia;
    p->pPars = pPars;
    // internal data
    p->vAbs   = Vec_IntAlloc( 100 );
//    p->vExtra = Vec_IntAlloc( 100 );
    // object structure
    p->nObjsAlloc = 256;
    p->pvLeaves = ABC_CALLOC( Vec_Int_t, p->nObjsAlloc );
    p->pvCnf0   = ABC_CALLOC( Vec_Int_t, p->nObjsAlloc );
    p->pvCnf1   = ABC_CALLOC( Vec_Int_t, p->nObjsAlloc );
    p->pvMap    = ABC_CALLOC( Vec_Int_t, p->nObjsAlloc );
    // temporaries
    p->vCnf     = Vec_IntAlloc( 100 );
    p->vLits    = Vec_IntAlloc( 100 );
    p->vIsopMem = Vec_IntAlloc( 100 );
    // prepare AIG
    p->timeStart = clock();
    Ga2_ManMarkup( pGia, 5 );
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManStop( Gla_Man_t * p )
{
    int i;
//    if ( p->pPars->fVerbose )
        Abc_Print( 1, "SAT solver:  Var = %d  Cla = %d  Conf = %d  Reduce = %d  Cex = %d  ObjsAdded = %d\n", 
            sat_solver2_nvars(p->pSat), sat_solver2_nclauses(p->pSat), sat_solver2_nconflicts(p->pSat), p->pSat->nDBreduces, p->nCexes, p->nObjAdded );
    for ( i = 0; i < p->nObjsAlloc; i++ )
    {
        ABC_FREE( p->pvLeaves->pArray );
        ABC_FREE( p->pvCnf0->pArray );
        ABC_FREE( p->pvCnf1->pArray );
        ABC_FREE( p->pvMap->pArray );
    }
    ABC_FREE( p->pvLeaves );
    ABC_FREE( p->pvCnf0 );
    ABC_FREE( p->pvCnf1 );
    ABC_FREE( p->pvMap );
    Vec_IntFree( p->vCnf );
    Vec_IntFree( p->vLits );
    Vec_IntFree( p->vIsopMem );
    Vec_IntFree( p->vAbs );
//    Vec_IntFree( p->vExtra );
    sat_solver2_delete( p->pSat );
    ABC_FREE( p );
}


/**Function*************************************************************

  Synopsis    [Computes truth table for the marked node.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
unsigned Ga2_ObjComputeTruth_rec( Gia_Man_t * p, Gia_Obj_t * pObj, int fFirst )
{
    unsigned Val0, Val1;
    if ( pObj->fPhase && !fFirst )
        return pObj->Value;
    assert( Gia_ObjIsAnd(pObj) );
    Val0 = Ga2_ObjComputeTruth_rec( p, Gia_ObjFanin0(pObj), 0 );
    Val1 = Ga2_ObjComputeTruth_rec( p, Gia_ObjFanin1(pObj), 0 );
    return (Gia_ObjFaninC0(pObj) ? ~Val0 : Val0) & (Gia_ObjFaninC1(pObj) ? ~Val1 : Val1);
}
unsigned Ga2_ManComputeTruth( Gia_Man_t * p, Gia_Obj_t * pRoot, Vec_Int_t * vLeaves )
{
    static unsigned uTruth5[5] = { 0xAAAAAAAA, 0xCCCCCCCC, 0xF0F0F0F0, 0xFF00FF00, 0xFFFF0000 };
    unsigned Res, Values[5];
    Gia_Obj_t * pObj;
    int i;
    // compute leaves
    Vec_IntClear( vLeaves );
    Ga2_ManCollectLeaves_rec( p, pRoot, vLeaves, 1 );
    // assign elementary truth tables
    Gia_ManForEachObjVec( vLeaves, p, pObj, i )
    {
        assert( pObj->fPhase );
        Values[i] = pObj->Value;
        pObj->Value = uTruth5[i];
    }
    Res = Ga2_ObjComputeTruth_rec( p, pRoot, 1 );
    // return values
    Gia_ManForEachObjVec( vLeaves, p, pObj, i )
        pObj->Value = Values[i];
    return Res;
}
void Ga2_ManComputeTest( Gia_Man_t * p )
{
    clock_t clk;
    Vec_Int_t * vLeaves;
    Gia_Obj_t * pObj;
    int i;
    Ga2_ManMarkup( p, 5 );
    clk = clock();
    vLeaves = Vec_IntAlloc( 100 );
    Gia_ManForEachAnd( p, pObj, i )
    {
        if ( !pObj->fPhase )
            continue;
        Ga2_ManComputeTruth( p, pObj, vLeaves );
    }
    Vec_IntFree( vLeaves );
    Abc_PrintTime( 1, "Time", clock() - clk );
}

/**Function*************************************************************

  Synopsis    [Computes and minimizes the truth table.]

  Description [Array of input literals may contain 0 (const0), 1 (const1)
  or 2 (literal).  Upon exit, it contains the variables in the support.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline unsigned Ga2_ObjTruthDepends( unsigned t, int v )
{
    assert( v >= 0 && v <= 4 );
    if ( v == 0 )
        return ((t ^ (t >> 1)) & 0x55555555);
    if ( v == 1 )
        return ((t ^ (t >> 2)) & 0x33333333);
    if ( v == 2 )
        return ((t ^ (t >> 4)) & 0x0F0F0F0F);
    if ( v == 3 )
        return ((t ^ (t >> 8)) & 0x00FF00FF);
    if ( v == 4 )
        return ((t ^ (t >>16)) & 0x0000FFFF);
    return -1;
}
unsigned Ga2_ObjComputeTruthSpecial( Gia_Man_t * p, Gia_Obj_t * pRoot, Vec_Int_t * vLeaves, Vec_Int_t * vLits )
{
    static unsigned uTruth5[5] = { 0xAAAAAAAA, 0xCCCCCCCC, 0xF0F0F0F0, 0xFF00FF00, 0xFFFF0000 };
    unsigned Res, Values[5];
    Gia_Obj_t * pObj;
    int i, k, Entry;
    // assign elementary truth tables
    Gia_ManForEachObjVec( vLeaves, p, pObj, i )
    {
        assert( pObj->fPhase );
        Values[i] = pObj->Value;
        Entry = Vec_IntEntry( vLits, i );
        assert( Entry >= 0 && Entry <= 2 );
        if ( Entry == 0 )
            pObj->Value = 0;
        else if ( Entry == 1 )
            pObj->Value = ~0;
        else // if ( Entry == 2 )
            pObj->Value = uTruth5[i];
    }
    Res = Ga2_ObjComputeTruth_rec( p, pRoot, 1 );
    Vec_IntClear( vLits );
    if ( Res != 0 && Res != ~0 )
    {
        // check if truth table depends on them
        k = 0;
        Gia_ManForEachObjVec( vLeaves, p, pObj, i )
        {
            if ( Ga2_ObjTruthDepends( Res, i ) )
            {
                pObj->Value = uTruth5[k++];
                Vec_IntPush( vLits, i );
            }
        }
        // recompute the truth table
        Res = Ga2_ObjComputeTruth_rec( p, pRoot, 1 );
        // verify that the true table depends on them
        for ( i = 0; i < Vec_IntSize(vLeaves); i++ )
            assert( (i < Vec_IntSize(vLits)) == (Ga2_ObjTruthDepends(Res, i) > 0) );
    }
    // return values
    Gia_ManForEachObjVec( vLeaves, p, pObj, i )
        pObj->Value = Values[i];
    return Res;
}

/**Function*************************************************************

  Synopsis    [Returns CNF of the function.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManCnfCompute( unsigned uTruth, int nVars, Vec_Int_t * vCnf, Vec_Int_t * vCover )
{
    extern int Kit_TruthIsop( unsigned * puTruth, int nVars, Vec_Int_t * vMemory, int fTryBoth );
    int i, k, Cube, Literal, NewCube, RetValue;
    assert( n == 5 );
    // transform truth table into the SOP
    RetValue = Kit_TruthIsop( &uTruth, 5, vCover, 0 );
    assert( RetValue == 0 );
    // check the case of constant cover
    Vec_IntClear( vCnf );
    Vec_IntForEachEntry( vCover, Cube, i )
    {
        for ( k = 0; k < nVars; k++ )
        {
            Literal = 3 & (Cube >> (k << 1));
            if ( Literal == 1 )
//                pCube[k] = '0';
                NewCube = NewCube * 3 + 0;
            else if ( Literal == 2 )
//                pCube[k] = '1';
                NewCube = NewCube * 3 + 1;
            else if ( Literal == 0 )
                NewCube = NewCube * 3 + 2;
            else
                assert( 0 );
        }
        Vec_IntPush( vCnf, NewCube );
    }
}


/**Function*************************************************************

  Synopsis    [Derives CNF for one node.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManCnfAddClause( Vec_Int_t * vCnf, int Lits[], int iLitOut, int ProofId )
{
    int k, b, Clause, nClaLits, ClaLits[6];
//    for ( k = 0; k < p->pSopSizes[uTruth]; k++ )
    Vec_IntForEacEntry( vCnf, Clause, k )
    {
        nClaLits = 0;
        ClaLits[nClaLits++] = i ? lit_neg(iLitOut) : iLitOut;
//        Clause = p->pSops[uTruth][k];
        for ( b = 3; b >= 0; b-- )
        {
            if ( Clause % 3 == 0 ) // value 0 --> write positive literal
            {
                assert( Lits[b] > 1 );
                ClaLits[nClaLits++] = Lits[b];
            }
            else if ( Clause % 3 == 1 ) // value 1 --> write negative literal
            {
                assert( Lits[b] > 1 );
                ClaLits[nClaLits++] = lit_neg(Lits[b]);
            }
            Clause = Clause / 3;
        }
        sat_solver2_addclause( p->pSat, ClaLits, ClaLits+nClaLits, ProofId ) );
    }
}
void Ga2_ManCnfAddPrecomputed( Ga2_Man_t * p, int uTruth, int Lits[], int iLitOut )
{
    int i, k;
    assert( uTruth > 0 && uTruth < 0xffff );
    // write positive/negative polarity
    for ( i = 0; i < 2; i++ )
    {
        if ( i )
            uTruth = 0xffff & ~uTruth;
//        Extra_PrintBinary( stdout, &uTruth, 16 ); printf( "\n" );
        Vec_IntClear( p->vCnf );
        for ( k = 0; k < p->pSopSizes[uTruth]; k++ )
            Vec_IntPush( p->vCnf, p->pSops[uTruth][k] );
        Ga2_ManCnfAddClause( p->vCnf, Lits, (i ? lit_neg(iLitOut) : iLitOut), ProofId );
    }
}
void Ga2_ManCnfAddDerived( Ga2_Man_t * p, Vec_Int_t * vCnf0, Vec_Int_t * vCnf1, int Lits[], int iLitOut )
{
    Ga2_ManCnfAddClause( vCnf0, Lits, iLitOut,          ProofId );
    Ga2_ManCnfAddClause( vCnf1, Lits, lit_neg(iLitOut), ProofId );
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManSetupNode( Ga2_Man_t * p, Gia_Obj_t * pObj )
{
    int Id = p->nObjs++;
    Vec_Int_t * vLeaves  = &p->pvLeaves[Id];
    Vec_Int_t * vCnf0    = &p->pvCnfs0[Id];
    Vec_Int_t * vCnf1    = &p->pvCnfs1[Id];
    Vec_Int_t * vMap     = &p->pvMaps[Id];
    unsigned uTruth;
    assert( pObj->Value == 0 );
    assert( p->nObjs > 1 );
    // prepare leaves
    if ( Vec_IntSize(vLeaves) == 0 )
    {
        Vec_IntGrow( vLeaves, 5 );
        Ga2_ManCollectLeaves_rec( p->pGia, pObj, vLeaves, 1 );
        assert( Vec_IntSize(vLeaves) < 6 );
        // compute truth table
        uTruth = Ga2_ManComputeTruth( p->pGia, pObj, vLeaves );
        // prepare CNF
        Ga2_ManCnfCompute( uTruth, Vec_IntSize(vLeaves), vCnf0, p->vCover );
        uTruth = (~uTruth) & ~((~0) << (1 << Vec_IntSize(vLeaves)));
        Ga2_ManCnfCompute( uTruth, Vec_IntSize(vLeaves), vCnf1, p->vCover );
        // prepare mapping
        Vec_IntGrow( vLeaves, 100 );
    }
    else
        Vec_IntClear( vMap );
    // remember the number
    pObj->Value = Id;
    // realloc
    if ( p->nObjs == p->nObjsAlloc )
    {
        p->pvLeaves = ABC_REALLOC( Vec_Int_t, p->pvLeaves, 2 * p->nObjsAlloc );
        p->pvCnfs0  = ABC_REALLOC( Vec_Int_t, p->pvCnfs0,  2 * p->nObjsAlloc );
        p->pvCnfs1  = ABC_REALLOC( Vec_Int_t, p->pvCnfs1,  2 * p->nObjsAlloc );
        p->pvMaps   = ABC_REALLOC( Vec_Int_t, p->pvMaps,   2 * p->nObjsAlloc );
        memset( p->pvLeaves + p->nObjsAlloc, 0, sizeof(Vec_Int_t) * p->nObjsAlloc );
        memset( p->pvCnfs0  + p->nObjsAlloc, 0, sizeof(Vec_Int_t) * p->nObjsAlloc );
        memset( p->pvCnfs1  + p->nObjsAlloc, 0, sizeof(Vec_Int_t) * p->nObjsAlloc );
        memset( p->pvMaps   + p->nObjsAlloc, 0, sizeof(Vec_Int_t) * p->nObjsAlloc );
        p->nObjsAlloc *= 2;
    }
}
void Ga2_ManSetdownNode( Ga2_Man_t * p, Gia_Obj_t * pObj )
{
    assert( pObj->Value > 0 );
    pObj->Value = 0;
}
void Ga2_ManAddNode( Ga2_Man_t * p, Gia_Obj_t * pObj, int f )
{
    assert( pObj->Value > 0 );

}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManRestart( Ga2_Man_t * p )
{
    Gia_Obj_t * pObj;
    int i;
    assert( p->pGia != NULL );
    assert( p->pGia->vGateClasses != NULL );
    assert( Gia_ManPi(p->pGia, 0)->fPhase ); // marks are set
    // clear mappings from objects
    for ( i = 1; i < p->nObjs; i++ )
    {
        Vec_IntShrink( &p->pvLeaves[i], 0 );
        Vec_IntShrink( &p->pvCnfs0[i], 0 );
        Vec_IntShrink( &p->pvCnfs1[i], 0 );
        Vec_IntShrink( &p->pvMaps[i], 0 );
    }
    // clear SAT variable numbers (begin with 1)
    if ( p->pSat ) sat_solver2_delete( p->pSat );
    p->pSat      = sat_solver2_new();
    p->nSatVars  = 1;
    // create constant literal
    Lit = toLitCond( 1, 1 );
    sat_solver2_addclause( p->pSat, &Lit, &Lit + 1, 0 );
    // collect abstraction
    p->nObjs = 1;
    Gia_ManCleanValue( p->pGia );
    Vec_IntClear( p->vAbs );
    Gia_ManForEachObj( p, pObj, i )
    {
        if ( pObj->fPhase && Vec_IntEntry(p->pGia->vGateClasses, i) )
        {
            Vec_IntPush( p->vAbs, i );
            Ga2_ManSetupNode( p, pObj );
        }
    }
    p->nAbs = Vec_IntSize( p->vAbs );
    // set runtime limit
    if ( p->pPars->nTimeOut )
        sat_solver2_set_runtime_limit( p->pSat, p->pPars->nTimeOut * CLOCKS_PER_SEC + p->timeStart );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManTranslate_rec( Gia_Man_t * p, Gia_Obj_t * pObj, Vec_Int_t * vClasses, int fFirst )
{
    if ( pObj->fPhase && !fFirst )
        return;
    assert( Gia_ObjIsAnd(pObj) );
    Ga2_ManTranslate_rec( p, Gia_ObjFanin0(pObj), vClasses, 0 );
    Ga2_ManTranslate_rec( p, Gia_ObjFanin1(pObj), vClasses, 0 );
    Vec_IntWriteEntry( vClasses, Gia_ObjId(p, pObj), 1 );
}
Vec_Int_t * Ga2_ManTranslate( Ga2_Man_t * p )
{
    Vec_Int_t * vGateClasses;
    Gia_Obj_t * pObj;
    int i;
    vGateClasses = Vec_IntStart( Gia_ManObjNum(p->pGia) );
    Gia_ManForEachObjVec( p->vAbs, p, pObj, i )
        Ga2_ManTranslate_rec( p, pObj, vGateClasses, 1 );
    return vGateClasses;
}

/**Function*************************************************************

  Synopsis    [Unrolls one timeframe.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ga2_ManUnroll( Ga2_Man_t * p, int f )
{
    Gia_Obj_t * pObj, * pObjRi, * pLeaf;
    unsigned uTruth;
    int i, k, Lit, fFullTable;
    // construct CNF for internal nodes
    Gia_ManForEachObjVec( p->vAbs, p, pObj, i )
    {
        // assign RO literal values (no need to add clauses)
        assert( pObj->fPhase && pObj->Value );
        if ( Gia_ObjIsConst0(pObj) )
        {
            Ga2_ObjAddLit( p, pObj, f, 3 ); // const 0 
            continue;
        }
        if ( Gia_ObjIsRo(p->pGia, pObj) )
        {
            pObjRi = Gia_ObjRoToRi(p->pGia, pObj);
            Lit = f ? Ga2_ObjFindOrAddLit( p, pObjRi, f-1 ) : 3;  // const 0
            Ga2_ObjAddLit( p, pObj, f, Lit ); 
            continue;
        }
        assert( Gia_ObjIsAnd(pObj) );
        vLeaves = Ga2_ManReadLeaves( p, pObj );
        // for nodes recently added to abstration, add CNF without const propagation
        fFullTable = 1;
        if ( i < p->nAbs )
        {
            Gia_ManForEachObjVec( vLeaves, p->pGia, pLeaf, k )
            {
                Lit = Ga2_ObjReadLit( p, pLeaf, f );
                if ( Lit == 2 || Lit == 3 )
                {
                    fFullTable = 0;
                    break;
                }
            }
        }
        if ( fFullTable )
        {
            Vec_IntClear( p->vLits );
            Gia_ManForEachObjVec( vLeaves, p->pGia, pLeaf, k )
                Vec_IntWriteEntry( p->vLits, Ga2_ObjFindOrAddLit(p, pLeaf, f) );        
            iLitOut = Ga2_ObjFindOrAddLit(p, pObj, f);
            Ga2_ManCnfAddClause( vCnf0, Vec_IntArray(p->vLits), iLitOut,          i < p->nAbs ? 0 : Gia_ObjId(p->pGia, pObj) );
            Ga2_ManCnfAddClause( vCnf1, Vec_IntArray(p->vLits), lit_neg(iLitOut), i < p->nAbs ? 0 : Gia_ObjId(p->pGia, pObj) );
            continue;
        }
        assert( i < p->nAbs );
        // collect literal types
        Vec_IntClear( p->vLits );
        Gia_ManForEachObjVec( vLeaves, p->pGia, pLeaf, k )
        {
            Lit = Ga2_ObjReadLit( p, pLeaf, f );
            if ( Lit == 3 ) // const 0
                Vec_IntPush( p->vLits, 0 );
            else if ( Lit == 2 ) // const 1
                Vec_IntPush( p->vLits, 1 );
            else 
                Vec_IntPush( p->vLits, 2 );
        }
        uTruth = Ga2_ObjComputeTruthSpecial( p, pObj, vLeaves, p->vLits );
        if ( uTruth == 0 || uTruth == ~0 )
            Ga2_ObjAddLit( p, pObj, f, uTruth == 0 ? 3 : 2 );     // const 0 / 1
        else if ( uTruth == 0xAAAAAAAA || uTruth == 0x55555555 )  // buffer / inverter
        {
            Lit = Vec_IntEntry( p->vLits, 0 );
            pLeaf = Gia_ManObj( p->pGia, Vec_IntEntry(vLeaves, Lit) );
            Lit = Ga2_ObjFindOrAddLit( p, pLeaf, f );
            Ga2_ObjAddLit( p, pObj, f, Abc_LitNotCond(Lit, uTruth == 0x55555555) );
        }
        else
        {
            // replace numbers of literals by actual literals
            Vec_IntForEachEntry( p->vLits, Lit, i )
            {
                pLeaf = Gia_ManObj( p->pGia, Vec_IntEntry(vLeaves, Lit) );
                Lit = Ga2_ObjFindOrAddLit(p, pLeaf, f);
                Vec_IntWriteEntry( p->vLits, Lit );        
            }
            // add CNF
            iLitOut = Ga2_ObjFindOrAddLit(p, pObj, f);
            Ga2_ManCnfAddPrecomputed( p, uTruth, Vec_IntArray(p->vLits), iLitOut );
        }

    }
    // propagate literals to the PO and flop outputs
    pObjRi = Gia_ManPo( p->pGia, 0 );
    Lit = Ga2_ObjFindLit( p, Gia_ObjFanin0(pObjRi), f );
    assert( Lit > 1 );
    Lit = Abc_LitNotCond( Lit, Gia_ObjFaninC0(pObjRi) );
    Ga2_ObjAddLit( p, pObj, f, Lit ); 
    Gia_ManForEachObjVec( p->vAbs, p, pObj, i )
    {
        if ( !Gia_ObjIsRo(p->pGia, pObj) )
            continue;
        pObjRi = Gia_ObjRoToRi(p->pGia, pObj);
        Lit = Ga2_ObjFindLit( p, Gia_ObjFanin0(pObjRi), f );
        assert( Lit > 1 );
        Lit = Abc_LitNotCond( Lit, Gia_ObjFaninC0(pObjRi) );
        Ga2_ObjAddLit( p, pObjRi, f, Lit ); 
    }
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Vec_IntCheckUnique( Vec_Int_t * p )
{
    int RetValue;
    Vec_Int_t * pDup = Vec_IntDup( p );
    Vec_IntUniqify( pDup );
    RetValue = Vec_IntSize(p) - Vec_IntSize(pDup);
    Vec_IntFree( pDup );
    return RetValue;
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Vec_Int_t * Ga2_ManRefine( Gla_Man_t * p )
{
    return NULL;
}

/**Function*************************************************************

  Synopsis    [Performs gate-level abstraction.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ga2_ManPerform( Gia_Man_t * pAig, Gia_ParVta_t * pPars )
{
    Gla_Man_t * p;
    Vec_Int_t * vCore, * vPPis;
    clock_t clk = clock();
    int i, c, f, Lit, Status, RetValue = -1;;
    // start the manager
    p = Ga2_ManStart( pAig, pPars );
    // check trivial case 
    assert( Gia_ManPoNum(pAig) == 1 );
    ABC_FREE( pAig->pCexSeq );
    if ( Gia_ObjIsConst0(Gia_ObjFanin0(Gia_ManPo(pAig,0))) )
    {
        if ( !Gia_ObjFaninC0(Gia_ManPo(pAig,0)) )
        {
            printf( "Sequential miter is trivially UNSAT.\n" );
            return 1;
        }
        pAig->pCexSeq = Abc_CexMakeTriv( Gia_ManRegNum(pAig), Gia_ManPiNum(pAig), 1, 0 );
        printf( "Sequential miter is trivially SAT.\n" );
        return 0;
    }
    // create gate classes if not given
    if ( pAig->vGateClasses == NULL )
    {
        pAig->vGateClasses = Vec_IntStart( Gia_ManObjNum(pAig) );
        Vec_IntWriteEntry( pAig->vGateClasses, 0, 1 );
        Vec_IntWriteEntry( pAig->vGateClasses, Gia_ObjFaninId0p(pAig, Gia_ManPo(pAig, 0)), 1 );
    }
    // start the manager
    p = Gla_ManStart( pAig, pPars );
    p->timeInit = clock() - clk;
    // perform initial abstraction
    if ( p->pPars->fVerbose )
    {
        Abc_Print( 1, "Running gate-level abstraction (GLA) with the following parameters:\n" );
        Abc_Print( 1, "FrameMax = %d  ConfMax = %d  Timeout = %d  RatioMin = %d %%.\n", 
            pPars->nFramesMax, pPars->nConfLimit, pPars->nTimeOut, pPars->nRatioMin );
        Abc_Print( 1, "LearnStart = %d  LearnDelta = %d  LearnRatio = %d %%.\n", 
            pPars->nLearnedStart, pPars->nLearnedDelta, pPars->nLearnedPerce );
        Abc_Print( 1, "Frame   %%   Abs  PPI   FF   LUT   Confl  Cex   Vars   Clas   Lrns     Time      Mem\n" );
    }
    // iterate unrolling
    for ( i = f = 0; !pPars->nFramesMax || f < pPars->nFramesMax; i++ )
    {
        // create new SAT solver
        Ga2_ManRestart( p );
        // unroll the circuit
        for ( f = 0; !pPars->nFramesMax || f < pPars->nFramesMax; f++ )
        {
            // add one more time-frame
            Ga2_ManUnroll( p, f );
            // check for counter-examples
            for ( c = 0; ; c++ )
            {
                // perform SAT solving
                Lit = Ga2_ObjFindOrAddLit( p, Gia_ManPo(p->pGia, 0), f );
                Status = sat_solver2_solve( pSat, &iLit, &iLit+1, (ABC_INT64_T)pPars->nConfLimit, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0 );
                if ( Status == l_True ) // perform refinement
                {
                    vPPis = Ga2_ManRefine( p );
                    if ( vPPis == NULL )
                        goto finish;
                    Vec_IntAppend( p->vAbs, vPPis );
                    Vec_IntFree( vPPis );
                    if ( Vec_IntCheckUnique(p->vAbs) )
                        printf( "Vector has %d duplicated entries.\n", Vec_IntCheckUnique(p->vAbs) );
                    continue;
                }
                if ( p->pSat->nRuntimeLimit && clock() > p->pSat->nRuntimeLimit ) // timeout
                    goto finish;
                if ( Status == l_Undef ) // ran out of resources
                    goto finish;
                assert( RetValue == l_False );
                // derive UNSAT core
                vCore = (Vec_Int_t *)Sat_ProofCore( pSat );
                Vec_IntAppend( p->vAbs, vCore );
                Vec_IntSort( p->vAbs, 0 ); // check unique!!!
                Vec_IntFree( vCore );
                if ( Vec_IntCheckUnique(p->vAbs) )
                    printf( "Vector has %d duplicated entries.\n", Vec_IntCheckUnique(p->vAbs) );
                break;
            }
            if ( c > 0 )
            {
                Vec_IntFreeP( &pAig->vGateClasses );
                pAig->vGateClasses = Ga2_ManTranslate( p );
                break;
            }
        }
        // check if the number of objects is below limit
        if ( Gia_GlaAbsCount(p,0,0) >= (p->nObjs - 1) * (100 - pPars->nRatioMin) / 100 )
        {
            Status = l_Undef;
            break;
        }
    }
finish:
    // analize the results
    if ( pAig->pCexSeq == NULL )
    {
        if ( pAig->vGateClasses != NULL )
            Abc_Print( 1, "Replacing the old abstraction by a new one.\n" );
        Vec_IntFreeP( &pAig->vGateClasses );
        pAig->vGateClasses = Ga2_ManTranslate( p );
        if ( Status == l_Undef )
        {
            if ( p->pPars->nTimeOut && clock() >= p->pSat->nRuntimeLimit ) 
                Abc_Print( 1, "SAT solver ran out of time at %d sec in frame %d.  ", p->pPars->nTimeOut, f );
            else if ( pPars->nConfLimit && sat_solver2_nconflicts(p->pSat) >= pPars->nConfLimit )
                Abc_Print( 1, "SAT solver ran out of resources at %d conflicts in frame %d.  ", pPars->nConfLimit, f );
            else if ( Gia_GlaAbsCount(p,0,0) >= (p->nObjs - 1) * (100 - pPars->nRatioMin) / 100 )
                Abc_Print( 1, "The ratio of abstracted objects is less than %d %% in frame %d.  ", pPars->nRatioMin, f );
            else
                Abc_Print( 1, "Abstraction stopped for unknown reason in frame %d.  ", f );
        }
        else
        {
            p->pPars->iFrame++;
            Abc_Print( 1, "SAT solver completed %d frames and produced an abstraction.  ", f );
        }
    }
    else
    {
        if ( !Gia_ManVerifyCex( pAig, pAig->pCexSeq, 0 ) )
            Abc_Print( 1, "    Gia_GlaPerform(): CEX verification has failed!\n" );
        Abc_Print( 1, "Counter-example detected in frame %d.  ", f );
        p->pPars->iFrame = pCex->iFrame - 1;
        Vec_IntFreeP( &pAig->vGateClasses );
    }
    Abc_PrintTime( 1, "Time", clock() - clk );
    if ( p->pPars->fVerbose )
    {
        p->timeOther = (clock() - clk) - p->timeUnsat - p->timeSat - p->timeCex - p->timeInit;
        ABC_PRTP( "Runtime: Initializing", p->timeInit,   clock() - clk );
        ABC_PRTP( "Runtime: Solver UNSAT", p->timeUnsat,  clock() - clk );
        ABC_PRTP( "Runtime: Solver SAT  ", p->timeSat,    clock() - clk );
        ABC_PRTP( "Runtime: Refinement  ", p->timeCex,    clock() - clk );
        ABC_PRTP( "Runtime: Other       ", p->timeOther,  clock() - clk );
        ABC_PRTP( "Runtime: TOTAL       ", clock() - clk, clock() - clk );
        Gla_ManReportMemory( p );
    }
    Gla_ManStop( p );
    fflush( stdout );
    return RetValue;


////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END
