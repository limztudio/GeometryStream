/* Ppmd7.c -- PPMdH codec
2021-04-13 : Igor Pavlov : Public domain
This code is based on PPMd var.H (2001): Dmitry Shkarin : Public domain */

#include "Ppmd7.h"

#include <cstring>


extern bool __common_memcpy(void*, const void*, size_t);


/* define PPMD7_ORDER_0_SUPPPORT to suport order-0 mode, unsupported by orignal PPMd var.H. code */
// #define PPMD7_ORDER_0_SUPPPORT
 
MY_ALIGN(16)
static const Byte PPMD7_kExpEscape[16] = { 25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2 };
MY_ALIGN(16)
static const UInt16 kInitBinEsc[] = { 0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};

#define MAX_FREQ 124
#define UNIT_SIZE 12

#define U2B(nu) ((UInt32)(nu) * UNIT_SIZE)
#define U2I(nu) (p->Units2Indx[(size_t)(nu) - 1])
#define I2U(indx) ((unsigned)p->Indx2Units[indx])
#define I2U_UInt16(indx) ((UInt16)p->Indx2Units[indx])

#define REF(ptr) Ppmd_GetRef(p, ptr)

#define STATS_REF(ptr) ((CPpmd_State_Ref)REF(ptr))

#define CTX(ref) ((CPpmd7_Context *)Ppmd7_GetContext(p, ref))
#define PPMD7_STATS(ctx) Ppmd7_GetStats(p, ctx)
#define ONE_STATE(ctx) Ppmd7Context_OneState(ctx)
#define SUFFIX(ctx) CTX((ctx)->Suffix)

typedef CPpmd7_Context * CTX_PTR;

struct CPpmd7_Node_;

typedef Ppmd_Ref_Type(struct CPpmd7_Node_) CPpmd7_Node_Ref;

typedef struct CPpmd7_Node_
{
  UInt16 Stamp; /* must be at offset 0 as CPpmd7_Context::NumStats. Stamp=0 means free */
  UInt16 NU;
  CPpmd7_Node_Ref Next; /* must be at offset >= 4 */
  CPpmd7_Node_Ref Prev;
} CPpmd7_Node;

#define NODE(r)  Ppmd_GetPtr_Type(p, r, CPpmd7_Node)

void Ppmd7_Construct(CPpmd7 *p)
{
  unsigned i, k, m;

  p->Base = NULL;

  for (i = 0, k = 0; i < PPMD_NUM_INDEXES; i++)
  {
    unsigned step = (i >= 12 ? 4 : (i >> 2) + 1);
    do { p->Units2Indx[k++] = (Byte)i; } while (--step);
    p->Indx2Units[i] = (Byte)k;
  }

  p->NS2BSIndx[0] = (0 << 1);
  p->NS2BSIndx[1] = (1 << 1);
  memset(p->NS2BSIndx + 2, (2 << 1), 9);
  memset(p->NS2BSIndx + 11, (3 << 1), 256 - 11);

  for (i = 0; i < 3; i++)
    p->NS2Indx[i] = (Byte)i;

  for (m = i, k = 1; i < 256; i++)
  {
    p->NS2Indx[i] = (Byte)m;
    if (--k == 0)
      k = (++m) - 2;
  }

  __common_memcpy(p->ExpEscape, PPMD7_kExpEscape, 16);
}


void Ppmd7_Free(CPpmd7 *p, ISzAllocPtr alloc)
{
  ISzAlloc_Free(alloc, p->Base);
  p->Size = 0;
  p->Base = NULL;
}


BoolInt Ppmd7_Alloc(CPpmd7 *p, UInt32 size, ISzAllocPtr alloc)
{
  if (!p->Base || p->Size != size)
  {
    Ppmd7_Free(p, alloc);
    p->AlignOffset = (4 - size) & 3;
    if ((p->Base = (Byte *)ISzAlloc_Alloc(alloc, p->AlignOffset + size)) == NULL)
      return False;
    p->Size = size;
  }
  return True;
}



// ---------- Internal Memory Allocator ----------

/* We can use CPpmd7_Node in list of free units (as in Ppmd8)
   But we still need one additional list walk pass in GlueFreeBlocks().
   So we use simple CPpmd_Void_Ref instead of CPpmd7_Node in InsertNode() / RemoveNode()
*/

#define EMPTY_NODE 0


static void InsertNode(CPpmd7 *p, void *node, unsigned indx)
{
  *((CPpmd_Void_Ref *)node) = p->FreeList[indx];
  // ((CPpmd7_Node *)node)->Next = (CPpmd7_Node_Ref)p->FreeList[indx];

  p->FreeList[indx] = REF(node);

}


static void *RemoveNode(CPpmd7 *p, unsigned indx)
{
  CPpmd_Void_Ref *node = (CPpmd_Void_Ref *)Ppmd7_GetPtr(p, p->FreeList[indx]);
  p->FreeList[indx] = *node;
  // CPpmd7_Node *node = NODE((CPpmd7_Node_Ref)p->FreeList[indx]);
  // p->FreeList[indx] = node->Next;
  return node;
}


static void SplitBlock(CPpmd7 *p, void *ptr, unsigned oldIndx, unsigned newIndx)
{
  unsigned i, nu = I2U(oldIndx) - I2U(newIndx);
  ptr = (Byte *)ptr + U2B(I2U(newIndx));
  if (I2U(i = U2I(nu)) != nu)
  {
    unsigned k = I2U(--i);
    InsertNode(p, ((Byte *)ptr) + U2B(k), nu - k - 1);
  }
  InsertNode(p, ptr, i);
}


/* we use CPpmd7_Node_Union union to solve XLC -O2 strict pointer aliasing problem */

typedef union _CPpmd7_Node_Union
{
  CPpmd7_Node     Node;
  CPpmd7_Node_Ref NextRef;
} CPpmd7_Node_Union;

/* Original PPmdH (Ppmd7) code uses doubly linked list in GlueFreeBlocks()
   we use single linked list similar to Ppmd8 code */


static void GlueFreeBlocks(CPpmd7 *p)
{
  /*
  we use first UInt16 field of 12-bytes UNITs as record type stamp
    CPpmd_State    { Byte Symbol; Byte Freq; : Freq != 0
    CPpmd7_Context { UInt16 NumStats;        : NumStats != 0
    CPpmd7_Node    { UInt16 Stamp            : Stamp == 0 for free record
                                             : Stamp == 1 for head record and guard
    Last 12-bytes UNIT in array is always contains 12-bytes order-0 CPpmd7_Context record.
  */
  CPpmd7_Node_Ref head, n = 0;
 
  p->GlueCount = 255;

  
  /* we set guard NODE at LoUnit */
  if (p->LoUnit != p->HiUnit)
    ((CPpmd7_Node *)(void *)p->LoUnit)->Stamp = 1;

  {
    /* Create list of free blocks.
       We still need one additional list walk pass before Glue. */
    unsigned i;
    for (i = 0; i < PPMD_NUM_INDEXES; i++)
    {
      const UInt16 nu = I2U_UInt16(i);
      CPpmd7_Node_Ref next = (CPpmd7_Node_Ref)p->FreeList[i];
      p->FreeList[i] = 0;
      while (next != 0)
      {
        /* Don't change the order of the following commands: */
        CPpmd7_Node_Union *un = (CPpmd7_Node_Union *)NODE(next);
        const CPpmd7_Node_Ref tmp = next;
        next = un->NextRef;
        un->Node.Stamp = EMPTY_NODE;
        un->Node.NU = nu;
        un->Node.Next = n;
        n = tmp;
      }
    }
  }

  head = n;
  /* Glue and Fill must walk the list in same direction */
  {
    /* Glue free blocks */
    CPpmd7_Node_Ref *prev = &head;
    while (n)
    {
      CPpmd7_Node *node = NODE(n);
      UInt32 nu = node->NU;
      n = node->Next;
      if (nu == 0)
      {
        *prev = n;
        continue;
      }
      prev = &node->Next;
      for (;;)
      {
        CPpmd7_Node *node2 = node + nu;
        nu += node2->NU;
        if (node2->Stamp != EMPTY_NODE || nu >= 0x10000)
          break;
        node->NU = (UInt16)nu;
        node2->NU = 0;
      }
    }
  }

  /* Fill lists of free blocks */
  for (n = head; n != 0;)
  {
    CPpmd7_Node *node = NODE(n);
    UInt32 nu = node->NU;
    unsigned i;
    n = node->Next;
    if (nu == 0)
      continue;
    for (; nu > 128; nu -= 128, node += 128)
      InsertNode(p, node, PPMD_NUM_INDEXES - 1);
    if (I2U(i = U2I(nu)) != nu)
    {
      unsigned k = I2U(--i);
      InsertNode(p, node + k, (unsigned)nu - k - 1);
    }
    InsertNode(p, node, i);
  }
}


MY_NO_INLINE
static void *AllocUnitsRare(CPpmd7 *p, unsigned indx)
{
  unsigned i;
  
  if (p->GlueCount == 0)
  {
    GlueFreeBlocks(p);
    if (p->FreeList[indx] != 0)
      return RemoveNode(p, indx);
  }
  
  i = indx;
  
  do
  {
    if (++i == PPMD_NUM_INDEXES)
    {
      UInt32 numBytes = U2B(I2U(indx));
      Byte *us = p->UnitsStart;
      p->GlueCount--;
      return ((UInt32)(us - p->Text) > numBytes) ? (p->UnitsStart = us - numBytes) : NULL;
    }
  }
  while (p->FreeList[i] == 0);

  {
    void *block = RemoveNode(p, i);
    SplitBlock(p, block, i, indx);
    return block;
  }
}


static void *AllocUnits(CPpmd7 *p, unsigned indx)
{
  if (p->FreeList[indx] != 0)
    return RemoveNode(p, indx);
  {
    UInt32 numBytes = U2B(I2U(indx));
    Byte *lo = p->LoUnit;
    if ((UInt32)(p->HiUnit - lo) >= numBytes)
    {
      p->LoUnit = lo + numBytes;
      return lo;
    }
  }
  return AllocUnitsRare(p, indx);
}


#define MyMem12Cpy(dest, src, num) \
  { UInt32 *d = (UInt32 *)dest; const UInt32 *z = (const UInt32 *)src; UInt32 n = num; \
    do { d[0] = z[0]; d[1] = z[1]; d[2] = z[2]; z += 3; d += 3; } while (--n); }


/*
static void *ShrinkUnits(CPpmd7 *p, void *oldPtr, unsigned oldNU, unsigned newNU)
{
  unsigned i0 = U2I(oldNU);
  unsigned i1 = U2I(newNU);
  if (i0 == i1)
    return oldPtr;
  if (p->FreeList[i1] != 0)
  {
    void *ptr = RemoveNode(p, i1);
    MyMem12Cpy(ptr, oldPtr, newNU);
    InsertNode(p, oldPtr, i0);
    return ptr;
  }
  SplitBlock(p, oldPtr, i0, i1);
  return oldPtr;
}
*/


#define SUCCESSOR(p) Ppmd_GET_SUCCESSOR(p)
static void SetSuccessor(CPpmd_State *p, CPpmd_Void_Ref v)
{
  Ppmd_SET_SUCCESSOR(p, v);
}



MY_NO_INLINE
static
void RestartModel(CPpmd7 *p)
{
  unsigned i, k;

  memset(p->FreeList, 0, sizeof(p->FreeList));
  
  p->Text = p->Base + p->AlignOffset;
  p->HiUnit = p->Text + p->Size;
  p->LoUnit = p->UnitsStart = p->HiUnit - p->Size / 8 / UNIT_SIZE * 7 * UNIT_SIZE;
  p->GlueCount = 0;

  p->OrderFall = p->MaxOrder;
  p->RunLength = p->InitRL = -(Int32)((p->MaxOrder < 12) ? p->MaxOrder : 12) - 1;
  p->PrevSuccess = 0;

  {
    CPpmd7_Context *mc = (CTX_PTR)(void *)(p->HiUnit -= UNIT_SIZE); /* AllocContext(p); */
    CPpmd_State *s = (CPpmd_State *)p->LoUnit; /* AllocUnits(p, PPMD_NUM_INDEXES - 1); */
    
    p->LoUnit += U2B(256 / 2);
    p->MaxContext = p->MinContext = mc;
    p->FoundState = s;

    mc->NumStats = 256;
    mc->Union2.SummFreq = 256 + 1;
    mc->Union4.Stats = REF(s);
    mc->Suffix = 0;

    for (i = 0; i < 256; i++, s++)
    {
      s->Symbol = (Byte)i;
      s->Freq = 1;
      SetSuccessor(s, 0);
    }

    #ifdef PPMD7_ORDER_0_SUPPPORT
    if (p->MaxOrder == 0)
    {
      CPpmd_Void_Ref r = REF(mc);
      s = p->FoundState;
      for (i = 0; i < 256; i++, s++)
        SetSuccessor(s, r);
      return;
    }
    #endif
  }

  for (i = 0; i < 128; i++)
    
    
    
    for (k = 0; k < 8; k++)
    {
      unsigned m;
      UInt16 *dest = p->BinSumm[i] + k;
      UInt16 val = (UInt16)(PPMD_BIN_SCALE - kInitBinEsc[k] / (i + 2));
      for (m = 0; m < 64; m += 8)
        dest[m] = val;
    }

    
  for (i = 0; i < 25; i++)
  {

    CPpmd_See *s = p->See[i];
    
    
    
    unsigned summ = ((5 * i + 10) << (PPMD_PERIOD_BITS - 4));
    for (k = 0; k < 16; k++, s++)
    {
      s->Summ = (UInt16)summ;
      s->Shift = (PPMD_PERIOD_BITS - 4);
      s->Count = 4;
    }
  }
  
  p->DummySee.Summ = 0; /* unused */
  p->DummySee.Shift = PPMD_PERIOD_BITS;
  p->DummySee.Count = 64; /* unused */
}


void Ppmd7_Init(CPpmd7 *p, unsigned maxOrder)
{
  p->MaxOrder = maxOrder;

  RestartModel(p);
}



/*
  CreateSuccessors()
  It's called when (FoundState->Successor) is RAW-Successor,
  that is the link to position in Raw text.
  So we create Context records and write the links to
  FoundState->Successor and to identical RAW-Successors in suffix
  contexts of MinContex.
  
  The function returns:
  if (OrderFall == 0) then MinContext is already at MAX order,
    { return pointer to new or existing context of same MAX order }
  else
    { return pointer to new real context that will be (Order+1) in comparison with MinContext

  also it can return pointer to real context of same order,
*/

MY_NO_INLINE
static CTX_PTR CreateSuccessors(CPpmd7 *p)
{
  CTX_PTR c = p->MinContext;
  CPpmd_Byte_Ref upBranch = (CPpmd_Byte_Ref)SUCCESSOR(p->FoundState);
  Byte newSym, newFreq;
  unsigned numPs = 0;
  CPpmd_State *ps[PPMD7_MAX_ORDER];

  if (p->OrderFall != 0)
    ps[numPs++] = p->FoundState;
  
  while (c->Suffix)
  {
    CPpmd_Void_Ref successor;
    CPpmd_State *s;
    c = SUFFIX(c);
    

    if (c->NumStats != 1)
    {
      Byte sym = p->FoundState->Symbol;
      for (s = PPMD7_STATS(c); s->Symbol != sym; s++);

    }
    else
    {
      s = ONE_STATE(c);

    }
    successor = SUCCESSOR(s);
    if (successor != upBranch)
    {
      // (c) is real record Context here,
      c = CTX(successor);
      if (numPs == 0)
      {
        // (c) is real record MAX Order Context here,
        // So we don't need to create any new contexts.
        return c;
      }
      break;
    }
    ps[numPs++] = s;
  }
  
  // All created contexts will have single-symbol with new RAW-Successor
  // All new RAW-Successors will point to next position in RAW text
  // after FoundState->Successor

  newSym = *(const Byte *)Ppmd7_GetPtr(p, upBranch);
  upBranch++;
  
  
  if (c->NumStats == 1)
    newFreq = ONE_STATE(c)->Freq;
  else
  {
    UInt32 cf, s0;
    CPpmd_State *s;
    for (s = PPMD7_STATS(c); s->Symbol != newSym; s++);
    cf = (UInt32)s->Freq - 1;
    s0 = (UInt32)c->Union2.SummFreq - c->NumStats - cf;
    /*
      cf - is frequency of symbol that will be Successor in new context records.
      s0 - is commulative frequency sum of another symbols from parent context.
      max(newFreq)= (s->Freq + 1), when (s0 == 1)
      we have requirement (Ppmd7Context_OneState()->Freq <= 128) in BinSumm[]
      so (s->Freq < 128) - is requirement for multi-symbol contexts
    */
    newFreq = (Byte)(1 + ((2 * cf <= s0) ? (5 * cf > s0) : (2 * cf + s0 - 1) / (2 * s0) + 1));
  }

  // Create new single-symbol contexts from low order to high order in loop

  do
  {
    CTX_PTR c1;
    /* = AllocContext(p); */
    if (p->HiUnit != p->LoUnit)
      c1 = (CTX_PTR)(void *)(p->HiUnit -= UNIT_SIZE);
    else if (p->FreeList[0] != 0)
      c1 = (CTX_PTR)RemoveNode(p, 0);
    else
    {
      c1 = (CTX_PTR)AllocUnitsRare(p, 0);
      if (!c1)
        return NULL;
    }
    
    c1->NumStats = 1;
    ONE_STATE(c1)->Symbol = newSym;
    ONE_STATE(c1)->Freq = newFreq;
    SetSuccessor(ONE_STATE(c1), upBranch);
    c1->Suffix = REF(c);
    SetSuccessor(ps[--numPs], REF(c1));
    c = c1;
  }
  while (numPs != 0);
  
  return c;
}



#define SwapStates(s) \
  { CPpmd_State tmp = s[0]; s[0] = s[-1]; s[-1] = tmp; }


void Ppmd7_UpdateModel(CPpmd7 *p);
MY_NO_INLINE
void Ppmd7_UpdateModel(CPpmd7 *p)
{
  CPpmd_Void_Ref maxSuccessor, minSuccessor;
  CTX_PTR c, mc;
  unsigned s0, ns;



  if (p->FoundState->Freq < MAX_FREQ / 4 && p->MinContext->Suffix != 0)
  {
    /* Update Freqs in Suffix Context */

    c = SUFFIX(p->MinContext);
    
    if (c->NumStats == 1)
    {
      CPpmd_State *s = ONE_STATE(c);
      if (s->Freq < 32)
        s->Freq++;
    }
    else
    {
      CPpmd_State *s = PPMD7_STATS(c);
      Byte sym = p->FoundState->Symbol;
      
      if (s->Symbol != sym)
      {
        do
        {
          // s++; if (s->Symbol == sym) break;
          s++;
        }
        while (s->Symbol != sym);
        
        if (s[0].Freq >= s[-1].Freq)
        {
          SwapStates(s);
          s--;
        }
      }

      if (s->Freq < MAX_FREQ - 9)
      {
        s->Freq = (Byte)(s->Freq + 2);
        c->Union2.SummFreq = (UInt16)(c->Union2.SummFreq + 2);
      }
    }
  }

  
  if (p->OrderFall == 0)
  {
    /* MAX ORDER context */
    /* (FoundState->Successor) is RAW-Successor. */
    p->MaxContext = p->MinContext = CreateSuccessors(p);
    if (!p->MinContext)
    {
      RestartModel(p);
      return;
    }
    SetSuccessor(p->FoundState, REF(p->MinContext));
    return;
  }

  
  /* NON-MAX ORDER context */
  
  {
    Byte *text = p->Text;
    *text++ = p->FoundState->Symbol;
    p->Text = text;
    if (text >= p->UnitsStart)
    {
      RestartModel(p);
      return;
    }
    maxSuccessor = REF(text);
  }
  
  minSuccessor = SUCCESSOR(p->FoundState);

  if (minSuccessor)
  {
    // there is Successor for FoundState in MinContext.
    // So the next context will be one order higher than MinContext.
    
    if (minSuccessor <= maxSuccessor)
    {
      // minSuccessor is RAW-Successor. So we will create real contexts records:
      CTX_PTR cs = CreateSuccessors(p);
      if (!cs)
      {
        RestartModel(p);
        return;
      }
      minSuccessor = REF(cs);
    }

    // minSuccessor now is real Context pointer that points to existing (Order+1) context
    
    if (--p->OrderFall == 0)
    {
      /*
      if we move to MaxOrder context, then minSuccessor will be common Succesor for both:
        MinContext that is (MaxOrder - 1)
        MaxContext that is (MaxOrder)
      so we don't need new RAW-Successor, and we can use real minSuccessor
      as succssors for both MinContext and MaxContext.
      */
      maxSuccessor = minSuccessor;
      
      /*
      if (MaxContext != MinContext)
      {
        there was order fall from MaxOrder and we don't need current symbol
        to transfer some RAW-Succesors to real contexts.
        So we roll back pointer in raw data for one position.
      }
      */
      p->Text -= (p->MaxContext != p->MinContext);
    }
  }
  else
  {
    /*
    FoundState has NULL-Successor here.
    And only root 0-order context can contain NULL-Successors.
    We change Successor in FoundState to RAW-Successor,
    And next context will be same 0-order root Context.
    */
    SetSuccessor(p->FoundState, maxSuccessor);
    minSuccessor = REF(p->MinContext);
  }

  mc = p->MinContext;
  c = p->MaxContext;

  p->MaxContext = p->MinContext = CTX(minSuccessor);

  if (c == mc)
    return;

  // s0 : is pure Escape Freq
  s0 = mc->Union2.SummFreq - (ns = mc->NumStats) - ((unsigned)p->FoundState->Freq - 1);

  do
  {
    unsigned ns1;
    UInt32 sum;
    
    if ((ns1 = c->NumStats) != 1)
    {
      if ((ns1 & 1) == 0)
      {
        /* Expand for one UNIT */
        unsigned oldNU = ns1 >> 1;
        unsigned i = U2I(oldNU);
        if (i != U2I((size_t)oldNU + 1))
        {
          void *ptr = AllocUnits(p, i + 1);
          void *oldPtr;
          if (!ptr)
          {
            RestartModel(p);
            return;
          }
          oldPtr = PPMD7_STATS(c);
          MyMem12Cpy(ptr, oldPtr, oldNU);
          InsertNode(p, oldPtr, i);
          c->Union4.Stats = STATS_REF(ptr);
        }
      }
      sum = c->Union2.SummFreq;
      /* max increase of Escape_Freq is 3 here.
         total increase of Union2.SummFreq for all symbols is less than 256 here */
      sum += (UInt32)(2 * ns1 < ns) + 2 * ((unsigned)(4 * ns1 <= ns) & (sum <= 8 * ns1));
      /* original PPMdH uses 16-bit variable for (sum) here.
         But (sum < 0x9000). So we don't truncate (sum) to 16-bit */
      // sum = (UInt16)sum;
    }
    else
    {
      // instead of One-symbol context we create 2-symbol context
      CPpmd_State *s = (CPpmd_State*)AllocUnits(p, 0);
      if (!s)
      {
        RestartModel(p);
        return;
      }
      {
        unsigned freq = c->Union2.State2.Freq;
        // s = *ONE_STATE(c);
        s->Symbol = c->Union2.State2.Symbol;
        s->Successor_0 = c->Union4.State4.Successor_0;
        s->Successor_1 = c->Union4.State4.Successor_1;
        // SetSuccessor(s, c->Union4.Stats);  // call it only for debug purposes to check the order of
                                              // (Successor_0 and Successor_1) in LE/BE.
        c->Union4.Stats = REF(s);
        if (freq < MAX_FREQ / 4 - 1)
          freq <<= 1;
        else
          freq = MAX_FREQ - 4;
        // (max(s->freq) == 120), when we convert from 1-symbol into 2-symbol context
        s->Freq = (Byte)freq;
        // max(InitEsc = PPMD7_kExpEscape[*]) is 25. So the max(escapeFreq) is 26 here
        sum = freq + p->InitEsc + (ns > 3);
      }
    }
    
    {
      CPpmd_State *s = PPMD7_STATS(c) + ns1;
      UInt32 cf = 2 * (sum + 6) * (UInt32)p->FoundState->Freq;
      UInt32 sf = (UInt32)s0 + sum;
      s->Symbol = p->FoundState->Symbol;
      c->NumStats = (UInt16)(ns1 + 1);
      SetSuccessor(s, maxSuccessor);
      
      if (cf < 6 * sf)
      {
        cf = (UInt32)1 + (cf > sf) + (cf >= 4 * sf);
        sum += 3;
        /* It can add (0, 1, 2) to Escape_Freq */
      }
      else
      {
        cf = (UInt32)4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
        sum += cf;
      }
     
      c->Union2.SummFreq = (UInt16)sum;
      s->Freq = (Byte)cf;
    }
    c = SUFFIX(c);
  }
  while (c != mc);
}
  


MY_NO_INLINE
static void Rescale(CPpmd7 *p)
{
  unsigned i, adder, sumFreq, escFreq;
  CPpmd_State *stats = PPMD7_STATS(p->MinContext);
  CPpmd_State *s = p->FoundState;

  /* Sort the list by Freq */
  if (s != stats)
  {
    CPpmd_State tmp = *s;
    do
      s[0] = s[-1];
    while (--s != stats);
    *s = tmp;
  }

  sumFreq = s->Freq;
  escFreq = p->MinContext->Union2.SummFreq - sumFreq;
  
  /*
  if (p->OrderFall == 0), adder = 0 : it's     allowed to remove symbol from     MAX Order context
  if (p->OrderFall != 0), adder = 1 : it's NOT allowed to remove symbol from NON-MAX Order context
  */

  adder = (p->OrderFall != 0);

  #ifdef PPMD7_ORDER_0_SUPPPORT
  adder |= (p->MaxOrder == 0); // we don't remove symbols from order-0 context
  #endif

  sumFreq = (sumFreq + 4 + adder) >> 1;
  i = (unsigned)p->MinContext->NumStats - 1;
  s->Freq = (Byte)sumFreq;
  
  do
  {
    unsigned freq = (++s)->Freq;
    escFreq -= freq;
    freq = (freq + adder) >> 1;
    sumFreq += freq;
    s->Freq = (Byte)freq;
    if (freq > s[-1].Freq)
    {
      CPpmd_State tmp = *s;
      CPpmd_State *s1 = s;
      do
      {
        s1[0] = s1[-1];
      }
      while (--s1 != stats && freq > s1[-1].Freq);
      *s1 = tmp;
    }
  }
  while (--i);
  
  if (s->Freq == 0)
  {
    /* Remove all items with Freq == 0 */
    CPpmd7_Context *mc;
    unsigned numStats, numStatsNew, n0, n1;
    
    i = 0; do { i++; } while ((--s)->Freq == 0);
    
    /* We increase (escFreq) for the number of removed symbols.
       So we will have (0.5) increase for Escape_Freq in avarage per
       removed symbol after Escape_Freq halving */
    escFreq += i;
    mc = p->MinContext;
    numStats = mc->NumStats;
    numStatsNew = numStats - i;
    mc->NumStats = (UInt16)(numStatsNew);
    n0 = (numStats + 1) >> 1;
    
    if (numStatsNew == 1)
    {
      /* Create Single-Symbol context */
      unsigned freq = stats->Freq;
      
      do
      {
        escFreq >>= 1;
        freq = (freq + 1) >> 1;
      }
      while (escFreq > 1);

      s = ONE_STATE(mc);
      *s = *stats;
      s->Freq = (Byte)freq; // (freq <= 260 / 4)
      p->FoundState = s;
      InsertNode(p, stats, U2I(n0));
      return;
    }
    
    n1 = (numStatsNew + 1) >> 1;
    if (n0 != n1)
    {
      // p->MinContext->Union4.Stats = STATS_REF(ShrinkUnits(p, stats, n0, n1));
      unsigned i0 = U2I(n0);
      unsigned i1 = U2I(n1);
      if (i0 != i1)
      {
        if (p->FreeList[i1] != 0)
        {
          void *ptr = RemoveNode(p, i1);
          p->MinContext->Union4.Stats = STATS_REF(ptr);
          MyMem12Cpy(ptr, (const void *)stats, n1);
          InsertNode(p, stats, i0);
        }
        else
          SplitBlock(p, stats, i0, i1);
      }
    }
  }
  {
    CPpmd7_Context *mc = p->MinContext;
    mc->Union2.SummFreq = (UInt16)(sumFreq + escFreq - (escFreq >> 1));
    // Escape_Freq halving here
    p->FoundState = PPMD7_STATS(mc);
  }
}


CPpmd_See *Ppmd7_MakeEscFreq(CPpmd7 *p, unsigned numMasked, UInt32 *escFreq)
{
  CPpmd_See *see;
  const CPpmd7_Context *mc = p->MinContext;
  unsigned numStats = mc->NumStats;
  if (numStats != 256)
  {
    unsigned nonMasked = numStats - numMasked;
    see = p->See[(unsigned)p->NS2Indx[(size_t)nonMasked - 1]]
        + (nonMasked < (unsigned)SUFFIX(mc)->NumStats - numStats)
        + 2 * (unsigned)(mc->Union2.SummFreq < 11 * numStats)
        + 4 * (unsigned)(numMasked > nonMasked) +
        p->HiBitsFlag;
    {
      // if (see->Summ) field is larger than 16-bit, we need only low 16 bits of Summ
      unsigned summ = (UInt16)see->Summ; // & 0xFFFF
      unsigned r = (summ >> see->Shift);
      see->Summ = (UInt16)(summ - r);
      *escFreq = r + (r == 0);
    }
  }
  else
  {
    see = &p->DummySee;
    *escFreq = 1;
  }
  return see;
}


static void NextContext(CPpmd7 *p)
{
  CTX_PTR c = CTX(SUCCESSOR(p->FoundState));
  if (p->OrderFall == 0 && (const Byte *)c > p->Text)
    p->MaxContext = p->MinContext = c;
  else
    Ppmd7_UpdateModel(p);
}


void Ppmd7_Update1(CPpmd7 *p)
{
  CPpmd_State *s = p->FoundState;
  unsigned freq = s->Freq;
  freq += 4;
  p->MinContext->Union2.SummFreq = (UInt16)(p->MinContext->Union2.SummFreq + 4);
  s->Freq = (Byte)freq;
  if (freq > s[-1].Freq)
  {
    SwapStates(s);
    p->FoundState = --s;
    if (freq > MAX_FREQ)
      Rescale(p);
  }
  NextContext(p);
}


void Ppmd7_Update1_0(CPpmd7 *p)
{
  CPpmd_State *s = p->FoundState;
  CPpmd7_Context *mc = p->MinContext;
  unsigned freq = s->Freq;
  unsigned summFreq = mc->Union2.SummFreq;
  p->PrevSuccess = (2 * freq > summFreq);
  p->RunLength += (int)p->PrevSuccess;
  mc->Union2.SummFreq = (UInt16)(summFreq + 4);
  freq += 4;
  s->Freq = (Byte)freq;
  if (freq > MAX_FREQ)
    Rescale(p);
  NextContext(p);
}


/*
void Ppmd7_UpdateBin(CPpmd7 *p)
{
  unsigned freq = p->FoundState->Freq;
  p->FoundState->Freq = (Byte)(freq + (freq < 128));
  p->PrevSuccess = 1;
  p->RunLength++;
  NextContext(p);
}
*/

void Ppmd7_Update2(CPpmd7 *p)
{
  CPpmd_State *s = p->FoundState;
  unsigned freq = s->Freq;
  freq += 4;
  p->RunLength = p->InitRL;
  p->MinContext->Union2.SummFreq = (UInt16)(p->MinContext->Union2.SummFreq + 4);
  s->Freq = (Byte)freq;
  if (freq > MAX_FREQ)
    Rescale(p);
  Ppmd7_UpdateModel(p);
}



/*
PPMd Memory Map:
{
  [ 0 ]           contains subset of original raw text, that is required to create context
                  records, Some symbols are not written, when max order context was reached
  [ Text ]        free area
  [ UnitsStart ]  CPpmd_State vectors and CPpmd7_Context records
  [ LoUnit ]      free  area for CPpmd_State and CPpmd7_Context items
[ HiUnit ]      CPpmd7_Context records
  [ Size ]        end of array
}

These addresses don't cross at any time.
And the following condtions is true for addresses:
  (0  <= Text < UnitsStart <= LoUnit <= HiUnit <= Size)

Raw text is BYTE--aligned.
the data in block [ UnitsStart ... Size ] contains 12-bytes aligned UNITs.

Last UNIT of array at offset (Size - 12) is root order-0 CPpmd7_Context record.
The code can free UNITs memory blocks that were allocated to store CPpmd_State vectors.
The code doesn't free UNITs allocated for CPpmd7_Context records.

The code calls RestartModel(), when there is no free memory for allocation.
And RestartModel() changes the state to orignal start state, with full free block.


The code allocates UNITs with the following order:

Allocation of 1 UNIT for Context record
  - from free space (HiUnit) down to (LoUnit)
  - from FreeList[0]
  - AllocUnitsRare()

AllocUnits() for CPpmd_State vectors:
  - from FreeList[i]
  - from free space (LoUnit) up to (HiUnit)
  - AllocUnitsRare()

AllocUnitsRare()
  - if (GlueCount == 0)
       {  Glue lists, GlueCount = 255, allocate from FreeList[i]] }
  - loop for all higher sized FreeList[...] lists
  - from (UnitsStart - Text), GlueCount--
  - ERROR


Each Record with Context contains the CPpmd_State vector, where each
CPpmd_State contains the link to Successor.
There are 3 types of Successor:
  1) NULL-Successor   - NULL pointer. NULL-Successor links can be stored
                        only in 0-order Root Context Record.
                        We use 0 value as NULL-Successor
  2) RAW-Successor    - the link to position in raw text,
                        that "RAW-Successor" is being created after first
                        occurrence of new symbol for some existing context record.
                        (RAW-Successor > 0).
  3) RECORD-Successor - the link to CPpmd7_Context record of (Order+1),
                        that record is being created when we go via RAW-Successor again.

For any successors at any time: the following condtions are true for Successor links:
(NULL-Successor < RAW-Successor < UnitsStart <= RECORD-Successor)


---------- Symbol Frequency, SummFreq and Range in Range_Coder ----------

CPpmd7_Context::SummFreq = Sum(Stats[].Freq) + Escape_Freq

The PPMd code tries to fulfill the condition:
  (SummFreq <= (256 * 128 = RC::kBot))

We have (Sum(Stats[].Freq) <= 256 * 124), because of (MAX_FREQ = 124)
So (4 = 128 - 124) is average reserve for Escape_Freq for each symbol.
If (CPpmd_State::Freq) is not aligned for 4, the reserve can be 5, 6 or 7.
SummFreq and Escape_Freq can be changed in Rescale() and *Update*() functions.
Rescale() can remove symbols only from max-order contexts. So Escape_Freq can increase after multiple calls of Rescale() for
max-order context.

When the PPMd code still break (Total <= RC::Range) condition in range coder,
we have two ways to resolve that problem:
  1) we can report error, if we want to keep compatibility with original PPMd code that has no fix for such cases.
  2) we can reduce (Total) value to (RC::Range) by reducing (Escape_Freq) part of (Total) value.
*/



/* Ppmd7Enc.cpp */




#define kTopValue (1 << 24)

#define R (&p->rc.enc)

void Ppmd7z_Init_RangeEnc(CPpmd7 *p)
{
  R->Low = 0;
  R->Range = 0xFFFFFFFF;
  R->Cache = 0;
  R->CacheSize = 1;
}

MY_NO_INLINE
static void RangeEnc_ShiftLow(CPpmd7 *p)
{
  if ((UInt32)R->Low < (UInt32)0xFF000000 || (unsigned)(R->Low >> 32) != 0)
  {
    Byte temp = R->Cache;
    do
    {
      IByteOut_Write(R->Stream, (Byte)(temp + (Byte)(R->Low >> 32)));
      temp = 0xFF;
    }
    while (--R->CacheSize != 0);
    R->Cache = (Byte)((UInt32)R->Low >> 24);
  }
  R->CacheSize++;
  R->Low = (UInt32)((UInt32)R->Low << 8);
}

#define RC_NORM_BASE(p) if (R->Range < kTopValue) { R->Range <<= 8; RangeEnc_ShiftLow(p);
#define RC_NORM_1(p) RC_NORM_BASE(p) }
#define RC_NORM(p) RC_NORM_BASE(p) RC_NORM_BASE(p) }}

// we must use only one type of Normalization from two: LOCAL or REMOTE
#define RC_NORM_LOCAL(p)    // RC_NORM(p)
#define RC_NORM_REMOTE(p)   RC_NORM(p)

/*
#define RangeEnc_Encode(p, start, _size_) \
  { UInt32 size = _size_; \
    R->Low += start * R->Range; \
    R->Range *= size; \
    RC_NORM_LOCAL(p); }
*/

MY_FORCE_INLINE
// MY_NO_INLINE
static void RangeEnc_Encode(CPpmd7 *p, UInt32 start, UInt32 size)
{
  R->Low += start * R->Range;
  R->Range *= size;
  RC_NORM_LOCAL(p);
}

void Ppmd7z_Flush_RangeEnc(CPpmd7 *p)
{
  unsigned i;
  for (i = 0; i < 5; i++)
    RangeEnc_ShiftLow(p);
}



#define RC_Encode(start, size) RangeEnc_Encode(p, start, size);
#define RC_EncodeFinal(start, size) RC_Encode(start, size); RC_NORM_REMOTE(p);

#define CTX(ref) ((CPpmd7_Context *)Ppmd7_GetContext(p, ref))
#define SUFFIX(ctx) CTX((ctx)->Suffix)
typedef CPpmd7_Context * CTX_PTR;
#define SUCCESSOR(p) Ppmd_GET_SUCCESSOR(p)

void Ppmd7_UpdateModel(CPpmd7 *p);

#define MASK(sym) ((unsigned char *)charMask)[sym]

MY_FORCE_INLINE
static
void Ppmd7z_EncodeSymbol(CPpmd7 *p, int symbol)
{
  size_t charMask[256 / sizeof(size_t)];
  
  if (p->MinContext->NumStats != 1)
  {
    CPpmd_State *s = Ppmd7_GetStats(p, p->MinContext);
    UInt32 sum;
    unsigned i;
   

    
    
    R->Range /= p->MinContext->Union2.SummFreq;
    
    if (s->Symbol == symbol)
    {
      // R->Range /= p->MinContext->Union2.SummFreq;
      RC_EncodeFinal(0, s->Freq);
      p->FoundState = s;
      Ppmd7_Update1_0(p);
      return;
    }
    p->PrevSuccess = 0;
    sum = s->Freq;
    i = (unsigned)p->MinContext->NumStats - 1;
    do
    {
      if ((++s)->Symbol == symbol)
      {
        // R->Range /= p->MinContext->Union2.SummFreq;
        RC_EncodeFinal(sum, s->Freq);
        p->FoundState = s;
        Ppmd7_Update1(p);
        return;
      }
      sum += s->Freq;
    }
    while (--i);

    // R->Range /= p->MinContext->Union2.SummFreq;
    RC_Encode(sum, p->MinContext->Union2.SummFreq - sum);
    
    p->HiBitsFlag = PPMD7_HiBitsFlag_3(p->FoundState->Symbol);
    PPMD_SetAllBitsIn256Bytes(charMask);
    // MASK(s->Symbol) = 0;
    // i = p->MinContext->NumStats - 1;
    // do { MASK((--s)->Symbol) = 0; } while (--i);
    {
      CPpmd_State *s2 = Ppmd7_GetStats(p, p->MinContext);
      MASK(s->Symbol) = 0;
      do
      {
        unsigned sym0 = s2[0].Symbol;
        unsigned sym1 = s2[1].Symbol;
        s2 += 2;
        MASK(sym0) = 0;
        MASK(sym1) = 0;
      }
      while (s2 < s);
    }
  }
  else
  {
    UInt16 *prob = Ppmd7_GetBinSumm(p);
    CPpmd_State *s = Ppmd7Context_OneState(p->MinContext);
    UInt32 pr = *prob;
    UInt32 bound = (R->Range >> 14) * pr;
    pr = PPMD_UPDATE_PROB_1(pr);
    if (s->Symbol == symbol)
    {
      *prob = (UInt16)(pr + (1 << PPMD_INT_BITS));
      // RangeEnc_EncodeBit_0(p, bound);
      R->Range = bound;
      RC_NORM_1(p);
      
      // p->FoundState = s;
      // Ppmd7_UpdateBin(p);
      {
        unsigned freq = s->Freq;
        CTX_PTR c = CTX(SUCCESSOR(s));
        p->FoundState = s;
        p->PrevSuccess = 1;
        p->RunLength++;
        s->Freq = (Byte)(freq + (freq < 128));
        // NextContext(p);
        if (p->OrderFall == 0 && (const Byte *)c > p->Text)
          p->MaxContext = p->MinContext = c;
        else
          Ppmd7_UpdateModel(p);
      }
      return;
    }

    *prob = (UInt16)pr;
    p->InitEsc = p->ExpEscape[pr >> 10];
    // RangeEnc_EncodeBit_1(p, bound);
    R->Low += bound;
    R->Range -= bound;
    RC_NORM_LOCAL(p)
    
    PPMD_SetAllBitsIn256Bytes(charMask);
    MASK(s->Symbol) = 0;
    p->PrevSuccess = 0;
  }

  for (;;)
  {
    CPpmd_See *see;
    CPpmd_State *s;
    UInt32 sum, escFreq;
    CPpmd7_Context *mc;
    unsigned i, numMasked;
    
    RC_NORM_REMOTE(p)

    mc = p->MinContext;
    numMasked = mc->NumStats;

    do
    {
      p->OrderFall++;
      if (!mc->Suffix)
        return; /* EndMarker (symbol = -1) */
      mc = Ppmd7_GetContext(p, mc->Suffix);
      i = mc->NumStats;
    }
    while (i == numMasked);

    p->MinContext = mc;
    
    // see = Ppmd7_MakeEscFreq(p, numMasked, &escFreq);
    {
      if (i != 256)
      {
        unsigned nonMasked = i - numMasked;
        see = p->See[(unsigned)p->NS2Indx[(size_t)nonMasked - 1]]
            + p->HiBitsFlag
            + (nonMasked < (unsigned)SUFFIX(mc)->NumStats - i)
            + 2 * (unsigned)(mc->Union2.SummFreq < 11 * i)
            + 4 * (unsigned)(numMasked > nonMasked);
        {
          // if (see->Summ) field is larger than 16-bit, we need only low 16 bits of Summ
          unsigned summ = (UInt16)see->Summ; // & 0xFFFF
          unsigned r = (summ >> see->Shift);
          see->Summ = (UInt16)(summ - r);
          escFreq = r + (r == 0);
        }
      }
      else
      {
        see = &p->DummySee;
        escFreq = 1;
      }
    }

    s = Ppmd7_GetStats(p, mc);
    sum = 0;
    // i = mc->NumStats;

    do
    {
      unsigned cur = s->Symbol;
      if ((int)cur == symbol)
      {
        UInt32 low = sum;
        UInt32 freq = s->Freq;
        unsigned num2;

        Ppmd_See_Update(see);
        p->FoundState = s;
        sum += escFreq;

        num2 = i / 2;
        i &= 1;
        sum += freq & (0 - (UInt32)i);
        if (num2 != 0)
        {
          s += i;
          for (;;)
          {
            unsigned sym0 = s[0].Symbol;
            unsigned sym1 = s[1].Symbol;
            s += 2;
            sum += (s[-2].Freq & (unsigned)(MASK(sym0)));
            sum += (s[-1].Freq & (unsigned)(MASK(sym1)));
            if (--num2 == 0)
              break;
          }
        }

        
        R->Range /= sum;
        RC_EncodeFinal(low, freq);
        Ppmd7_Update2(p);
        return;
      }
      sum += (s->Freq & (unsigned)(MASK(cur)));
      s++;
    }
    while (--i);
    
    {
      UInt32 total = sum + escFreq;
      see->Summ = (UInt16)(see->Summ + total);

      R->Range /= total;
      RC_Encode(sum, escFreq);
    }

    {
      CPpmd_State *s2 = Ppmd7_GetStats(p, p->MinContext);
      s--;
      MASK(s->Symbol) = 0;
      do
      {
        unsigned sym0 = s2[0].Symbol;
        unsigned sym1 = s2[1].Symbol;
        s2 += 2;
        MASK(sym0) = 0;
        MASK(sym1) = 0;
      }
      while (s2 < s);
    }
  }
}


void Ppmd7z_EncodeSymbols(CPpmd7 *p, const Byte *buf, const Byte *lim)
{
  for (; buf < lim; buf++)
  {
    Ppmd7z_EncodeSymbol(p, *buf);
  }
}


#undef kTopValue

#undef R


#undef RC_NORM_BASE
#undef RC_NORM_1
#undef RC_NORM

#undef RC_NORM_LOCAL
#undef RC_NORM_REMOTE


#undef RC_Encode
#undef RC_EncodeFinal

#undef CTX
#undef SUFFIX
#undef SUCCESSOR

#undef MASK




/* Ppmd7Dec.cpp */




#define kTopValue (1 << 24)


#define READ_BYTE(p) IByteIn_Read((p)->Stream)

BoolInt Ppmd7z_RangeDec_Init(CPpmd7_RangeDec *p)
{
  unsigned i;
  p->Code = 0;
  p->Range = 0xFFFFFFFF;
  if (READ_BYTE(p) != 0)
    return False;
  for (i = 0; i < 4; i++)
    p->Code = (p->Code << 8) | READ_BYTE(p);
  return (p->Code < 0xFFFFFFFF);
}

#define RC_NORM_BASE(p) if ((p)->Range < kTopValue) \
  { (p)->Code = ((p)->Code << 8) | READ_BYTE(p); (p)->Range <<= 8;

#define RC_NORM_1(p)  RC_NORM_BASE(p) }
#define RC_NORM(p)    RC_NORM_BASE(p) RC_NORM_BASE(p) }}

// we must use only one type of Normalization from two: LOCAL or REMOTE
#define RC_NORM_LOCAL(p)    // RC_NORM(p)
#define RC_NORM_REMOTE(p)   RC_NORM(p)

#define R (&p->rc.dec)

MY_FORCE_INLINE
// MY_NO_INLINE
static void RangeDec_Decode(CPpmd7 *p, UInt32 start, UInt32 size)
{

  
  R->Code -= start * R->Range;
  R->Range *= size;
  RC_NORM_LOCAL(R)
}

#define RC_Decode(start, size) RangeDec_Decode(p, start, size);
#define RC_DecodeFinal(start, size) RC_Decode(start, size) RC_NORM_REMOTE(R)
#define RC_GetThreshold(total) (R->Code / (R->Range /= (total)))


#define CTX(ref) ((CPpmd7_Context *)Ppmd7_GetContext(p, ref))
typedef CPpmd7_Context * CTX_PTR;
#define SUCCESSOR(p) Ppmd_GET_SUCCESSOR(p)
void Ppmd7_UpdateModel(CPpmd7 *p);

#define MASK(sym) ((unsigned char *)charMask)[sym]
// MY_FORCE_INLINE
// static
int Ppmd7z_DecodeSymbol(CPpmd7 *p)
{
  size_t charMask[256 / sizeof(size_t)];

  if (p->MinContext->NumStats != 1)
  {
    CPpmd_State *s = Ppmd7_GetStats(p, p->MinContext);
    unsigned i;
    UInt32 count, hiCnt;
    UInt32 summFreq = p->MinContext->Union2.SummFreq;

    
    
    
    count = RC_GetThreshold(summFreq);
    hiCnt = count;
    
    if ((Int32)(count -= s->Freq) < 0)
    {
      Byte sym;
      RC_DecodeFinal(0, s->Freq);
      p->FoundState = s;
      sym = s->Symbol;
      Ppmd7_Update1_0(p);
      return sym;
    }
  
    p->PrevSuccess = 0;
    i = (unsigned)p->MinContext->NumStats - 1;
    
    do
    {
      if ((Int32)(count -= (++s)->Freq) < 0)
      {
        Byte sym;
        RC_DecodeFinal((hiCnt - count) - s->Freq, s->Freq);
        p->FoundState = s;
        sym = s->Symbol;
        Ppmd7_Update1(p);
        return sym;
      }
    }
    while (--i);
    
    if (hiCnt >= summFreq)
      return PPMD7_SYM_ERROR;
    
    hiCnt -= count;
    RC_Decode(hiCnt, summFreq - hiCnt);

    p->HiBitsFlag = PPMD7_HiBitsFlag_3(p->FoundState->Symbol);
    PPMD_SetAllBitsIn256Bytes(charMask);
    // i = p->MinContext->NumStats - 1;
    // do { MASK((--s)->Symbol) = 0; } while (--i);
    {
      CPpmd_State *s2 = Ppmd7_GetStats(p, p->MinContext);
      MASK(s->Symbol) = 0;
      do
      {
        unsigned sym0 = s2[0].Symbol;
        unsigned sym1 = s2[1].Symbol;
        s2 += 2;
        MASK(sym0) = 0;
        MASK(sym1) = 0;
      }
      while (s2 < s);
    }
  }
  else
  {
    CPpmd_State *s = Ppmd7Context_OneState(p->MinContext);
    UInt16 *prob = Ppmd7_GetBinSumm(p);
    UInt32 pr = *prob;
    UInt32 size0 = (R->Range >> 14) * pr;
    pr = PPMD_UPDATE_PROB_1(pr);

    if (R->Code < size0)
    {
      Byte sym;
      *prob = (UInt16)(pr + (1 << PPMD_INT_BITS));
      
      // RangeDec_DecodeBit0(size0);
      R->Range = size0;
      RC_NORM_1(R)
      /* we can use single byte normalization here because of
         (min(BinSumm[][]) = 95) > (1 << (14 - 8)) */

      // sym = (p->FoundState = Ppmd7Context_OneState(p->MinContext))->Symbol;
      // Ppmd7_UpdateBin(p);
      {
        unsigned freq = s->Freq;
        CTX_PTR c = CTX(SUCCESSOR(s));
        sym = s->Symbol;
        p->FoundState = s;
        p->PrevSuccess = 1;
        p->RunLength++;
        s->Freq = (Byte)(freq + (freq < 128));
        // NextContext(p);
        if (p->OrderFall == 0 && (const Byte *)c > p->Text)
          p->MaxContext = p->MinContext = c;
        else
          Ppmd7_UpdateModel(p);
      }
      return sym;
    }

    *prob = (UInt16)pr;
    p->InitEsc = p->ExpEscape[pr >> 10];

    // RangeDec_DecodeBit1(size0);
    
    R->Code -= size0;
    R->Range -= size0;
    RC_NORM_LOCAL(R)
    
    PPMD_SetAllBitsIn256Bytes(charMask);
    MASK(Ppmd7Context_OneState(p->MinContext)->Symbol) = 0;
    p->PrevSuccess = 0;
  }

  for (;;)
  {
    CPpmd_State *s, *s2;
    UInt32 freqSum, count, hiCnt;

    CPpmd_See *see;
    CPpmd7_Context *mc;
    unsigned numMasked;
    RC_NORM_REMOTE(R)
    mc = p->MinContext;
    numMasked = mc->NumStats;

    do
    {
      p->OrderFall++;
      if (!mc->Suffix)
        return PPMD7_SYM_END;
      mc = Ppmd7_GetContext(p, mc->Suffix);
    }
    while (mc->NumStats == numMasked);
    
    s = Ppmd7_GetStats(p, mc);

    {
      unsigned num = mc->NumStats;
      unsigned num2 = num / 2;
      
      num &= 1;
      hiCnt = (s->Freq & (unsigned)(MASK(s->Symbol))) & (0 - (UInt32)num);
      s += num;
      p->MinContext = mc;

      do
      {
        unsigned sym0 = s[0].Symbol;
        unsigned sym1 = s[1].Symbol;
        s += 2;
        hiCnt += (s[-2].Freq & (unsigned)(MASK(sym0)));
        hiCnt += (s[-1].Freq & (unsigned)(MASK(sym1)));
      }
      while (--num2);
    }

    see = Ppmd7_MakeEscFreq(p, numMasked, &freqSum);
    freqSum += hiCnt;




    count = RC_GetThreshold(freqSum);
    
    if (count < hiCnt)
    {
      Byte sym;

      s = Ppmd7_GetStats(p, p->MinContext);
      hiCnt = count;
      // count -= s->Freq & (unsigned)(MASK(s->Symbol));
      // if ((Int32)count >= 0)
      {
        for (;;)
        {
          count -= s->Freq & (unsigned)(MASK((s)->Symbol)); s++; if ((Int32)count < 0) break;
          // count -= s->Freq & (unsigned)(MASK((s)->Symbol)); s++; if ((Int32)count < 0) break;
        };
      }
      s--;
      RC_DecodeFinal((hiCnt - count) - s->Freq, s->Freq);

      // new (see->Summ) value can overflow over 16-bits in some rare cases
      Ppmd_See_Update(see);
      p->FoundState = s;
      sym = s->Symbol;
      Ppmd7_Update2(p);
      return sym;
    }

    if (count >= freqSum)
      return PPMD7_SYM_ERROR;
    
    RC_Decode(hiCnt, freqSum - hiCnt);

    // We increase (see->Summ) for sum of Freqs of all non_Masked symbols.
    // new (see->Summ) value can overflow over 16-bits in some rare cases
    see->Summ = (UInt16)(see->Summ + freqSum);

    s = Ppmd7_GetStats(p, p->MinContext);
    s2 = s + p->MinContext->NumStats;
    do
    {
      MASK(s->Symbol) = 0;
      s++;
    }
    while (s != s2);
  }
}

/*
Byte *Ppmd7z_DecodeSymbols(CPpmd7 *p, Byte *buf, const Byte *lim)
{
  int sym = 0;
  if (buf != lim)
  do
  {
    sym = Ppmd7z_DecodeSymbol(p);
    if (sym < 0)
      break;
    *buf = (Byte)sym;
  }
  while (++buf < lim);
  p->LastSymbol = sym;
  return buf;
}
*/


#undef kTopValue


#undef READ_BYTE


#undef RC_NORM_BASE

#undef RC_NORM_1
#undef RC_NORM

#undef RC_NORM_LOCAL
#undef RC_NORM_REMOTE

#undef R


#undef RC_Decode
#undef RC_DecodeFinal
#undef RC_GetThreshold


#undef CTX
#undef SUCCESSOR

#undef MASK
