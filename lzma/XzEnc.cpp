/* XzEnc.c -- Xz Encode
2021-04-01 : Igor Pavlov : Public domain */

#include "XzEnc.h"

#include "Compiler.h"

#include <cstring>

#include "7zCrc.h"
#include "CpuArch.h"
#include "Bra.h"

// #define _7ZIP_ST

#ifndef _7ZIP_ST
#include "MtCoder.h"
#else
#define MTCODER__THREADS_MAX 1
#define MTCODER__BLOCKS_MAX 1
#endif

#define XZ_GET_PAD_SIZE(dataSize) ((4 - ((unsigned)(dataSize) & 3)) & 3)

/* max pack size for LZMA2 block + check-64bytrs: */
#define XZ_GET_MAX_BLOCK_PACK_SIZE(unpackSize) ((unpackSize) + ((unpackSize) >> 10) + 16 + 64)

#define XZ_GET_ESTIMATED_BLOCK_TOTAL_PACK_SIZE(unpackSize) (XZ_BLOCK_HEADER_SIZE_MAX + XZ_GET_MAX_BLOCK_PACK_SIZE(unpackSize))


#define XzBlock_ClearFlags(p)       (p)->flags = 0;
#define XzBlock_SetNumFilters(p, n) (p)->flags = (Byte)((p)->flags | ((n) - 1));
#define XzBlock_SetHasPackSize(p)   (p)->flags |= XZ_BF_PACK_SIZE;
#define XzBlock_SetHasUnpackSize(p) (p)->flags |= XZ_BF_UNPACK_SIZE;


static SRes WriteBytes(ISeqOutStream *s, const void *buf, size_t size)
{
  return (ISeqOutStream_Write(s, buf, size) == size) ? SZ_OK : SZ_ERROR_WRITE;
}

static SRes WriteBytesUpdateCrc(ISeqOutStream *s, const void *buf, size_t size, UInt32 *crc)
{
  *crc = CrcUpdate(*crc, buf, size);
  return WriteBytes(s, buf, size);
}


static SRes Xz_WriteHeader(CXzStreamFlags f, ISeqOutStream *s)
{
  UInt32 crc;
  Byte header[XZ_STREAM_HEADER_SIZE];
  memcpy(header, XZ_SIG, XZ_SIG_SIZE);
  header[XZ_SIG_SIZE] = (Byte)(f >> 8);
  header[XZ_SIG_SIZE + 1] = (Byte)(f & 0xFF);
  crc = CrcCalc(header + XZ_SIG_SIZE, XZ_STREAM_FLAGS_SIZE);
  SetUi32(header + XZ_SIG_SIZE + XZ_STREAM_FLAGS_SIZE, crc);
  return WriteBytes(s, header, XZ_STREAM_HEADER_SIZE);
}


static SRes XzBlock_WriteHeader(const CXzBlock *p, ISeqOutStream *s)
{
  Byte header[XZ_BLOCK_HEADER_SIZE_MAX];

  unsigned pos = 1;
  unsigned numFilters, i;
  header[pos++] = p->flags;

  if (XzBlock_HasPackSize(p)) pos += Xz_WriteVarInt(header + pos, p->packSize);
  if (XzBlock_HasUnpackSize(p)) pos += Xz_WriteVarInt(header + pos, p->unpackSize);
  numFilters = XzBlock_GetNumFilters(p);
  
  for (i = 0; i < numFilters; i++)
  {
    const CXzFilter *f = &p->filters[i];
    pos += Xz_WriteVarInt(header + pos, f->id);
    pos += Xz_WriteVarInt(header + pos, f->propsSize);
    memcpy(header + pos, f->props, f->propsSize);
    pos += f->propsSize;
  }

  while ((pos & 3) != 0)
    header[pos++] = 0;

  header[0] = (Byte)(pos >> 2);
  SetUi32(header + pos, CrcCalc(header, pos));
  return WriteBytes(s, header, pos + 4);
}




typedef struct
{
  size_t numBlocks;
  size_t size;
  size_t allocated;
  Byte *blocks;
} CXzEncIndex;


static void XzEncIndex_Construct(CXzEncIndex *p)
{
  p->numBlocks = 0;
  p->size = 0;
  p->allocated = 0;
  p->blocks = NULL;
}

static void XzEncIndex_Init(CXzEncIndex *p)
{
  p->numBlocks = 0;
  p->size = 0;
}

static void XzEncIndex_Free(CXzEncIndex *p, ISzAllocPtr alloc)
{
  if (p->blocks)
  {
    ISzAlloc_Free(alloc, p->blocks);
    p->blocks = NULL;
  }
  p->numBlocks = 0;
  p->size = 0;
  p->allocated = 0;
}


static SRes XzEncIndex_ReAlloc(CXzEncIndex *p, size_t newSize, ISzAllocPtr alloc)
{
  Byte *blocks = (Byte *)ISzAlloc_Alloc(alloc, newSize);
  if (!blocks)
    return SZ_ERROR_MEM;
  if (p->size != 0)
    memcpy(blocks, p->blocks, p->size);
  if (p->blocks)
    ISzAlloc_Free(alloc, p->blocks);
  p->blocks = blocks;
  p->allocated = newSize;
  return SZ_OK;
}


static SRes XzEncIndex_PreAlloc(CXzEncIndex *p, UInt64 numBlocks, UInt64 unpackSize, UInt64 totalSize, ISzAllocPtr alloc)
{
  UInt64 pos;
  {
    Byte buf[32];
    unsigned pos2 = Xz_WriteVarInt(buf, totalSize);
    pos2 += Xz_WriteVarInt(buf + pos2, unpackSize);
    pos = numBlocks * pos2;
  }
  
  if (pos <= p->allocated - p->size)
    return SZ_OK;
  {
    UInt64 newSize64 = p->size + pos;
    size_t newSize = (size_t)newSize64;
    if (newSize != newSize64)
      return SZ_ERROR_MEM;
    return XzEncIndex_ReAlloc(p, newSize, alloc);
  }
}


static SRes XzEncIndex_AddIndexRecord(CXzEncIndex *p, UInt64 unpackSize, UInt64 totalSize, ISzAllocPtr alloc)
{
  Byte buf[32];
  unsigned pos = Xz_WriteVarInt(buf, totalSize);
  pos += Xz_WriteVarInt(buf + pos, unpackSize);

  if (pos > p->allocated - p->size)
  {
    size_t newSize = p->allocated * 2 + 16 * 2;
    if (newSize < p->size + pos)
      return SZ_ERROR_MEM;
    RINOK(XzEncIndex_ReAlloc(p, newSize, alloc));
  }
  memcpy(p->blocks + p->size, buf, pos);
  p->size += pos;
  p->numBlocks++;
  return SZ_OK;
}


static SRes XzEncIndex_WriteFooter(const CXzEncIndex *p, CXzStreamFlags flags, ISeqOutStream *s)
{
  Byte buf[32];
  UInt64 globalPos;
  UInt32 crc = CRC_INIT_VAL;
  unsigned pos = 1 + Xz_WriteVarInt(buf + 1, p->numBlocks);
  
  globalPos = pos;
  buf[0] = 0;
  RINOK(WriteBytesUpdateCrc(s, buf, pos, &crc));
  RINOK(WriteBytesUpdateCrc(s, p->blocks, p->size, &crc));
  globalPos += p->size;
  
  pos = XZ_GET_PAD_SIZE(globalPos);
  buf[1] = 0;
  buf[2] = 0;
  buf[3] = 0;
  globalPos += pos;
  
  crc = CrcUpdate(crc, buf + 4 - pos, pos);
  SetUi32(buf + 4, CRC_GET_DIGEST(crc));
  
  SetUi32(buf + 8 + 4, (UInt32)(globalPos >> 2));
  buf[8 + 8] = (Byte)(flags >> 8);
  buf[8 + 9] = (Byte)(flags & 0xFF);
  SetUi32(buf + 8, CrcCalc(buf + 8 + 4, 6));
  buf[8 + 10] = XZ_FOOTER_SIG_0;
  buf[8 + 11] = XZ_FOOTER_SIG_1;
  
  return WriteBytes(s, buf + 4 - pos, pos + 4 + 12);
}



/* ---------- CSeqCheckInStream ---------- */

typedef struct
{
  ISeqInStream vt;
  ISeqInStream *realStream;
  const Byte *data;
  UInt64 limit;
  UInt64 processed;
  int realStreamFinished;
  CXzCheck check;
} CSeqCheckInStream;

static void SeqCheckInStream_Init(CSeqCheckInStream *p, unsigned checkMode)
{
  p->limit = (UInt64)(Int64)-1;
  p->processed = 0;
  p->realStreamFinished = 0;
  XzCheck_Init(&p->check, checkMode);
}

static void SeqCheckInStream_GetDigest(CSeqCheckInStream *p, Byte *digest)
{
  XzCheck_Final(&p->check, digest);
}

static SRes SeqCheckInStream_Read(const ISeqInStream *pp, void *data, size_t *size)
{
  CSeqCheckInStream *p = CONTAINER_FROM_VTBL(pp, CSeqCheckInStream, vt);
  size_t size2 = *size;
  SRes res = SZ_OK;
  
  if (p->limit != (UInt64)(Int64)-1)
  {
    UInt64 rem = p->limit - p->processed;
    if (size2 > rem)
      size2 = (size_t)rem;
  }
  if (size2 != 0)
  {
    if (p->realStream)
    {
      res = ISeqInStream_Read(p->realStream, data, &size2);
      p->realStreamFinished = (size2 == 0) ? 1 : 0;
    }
    else
      memcpy(data, p->data + (size_t)p->processed, size2);
    XzCheck_Update(&p->check, data, size2);
    p->processed += size2;
  }
  *size = size2;
  return res;
}


/* ---------- CSeqSizeOutStream ---------- */

typedef struct
{
  ISeqOutStream vt;
  ISeqOutStream *realStream;
  Byte *outBuf;
  size_t outBufLimit;
  UInt64 processed;
} CSeqSizeOutStream;

static size_t SeqSizeOutStream_Write(const ISeqOutStream *pp, const void *data, size_t size)
{
  CSeqSizeOutStream *p = CONTAINER_FROM_VTBL(pp, CSeqSizeOutStream, vt);
  if (p->realStream)
    size = ISeqOutStream_Write(p->realStream, data, size);
  else
  {
    if (size > p->outBufLimit - (size_t)p->processed)
      return 0;
    memcpy(p->outBuf + (size_t)p->processed, data, size);
  }
  p->processed += size;
  return size;
}


/* ---------- CSeqInFilter ---------- */

#define FILTER_BUF_SIZE (1 << 20)

typedef struct
{
  ISeqInStream p;
  ISeqInStream *realStream;
  IStateCoder StateCoder;
  Byte *buf;
  size_t curPos;
  size_t endPos;
  int srcWasFinished;
} CSeqInFilter;


SRes BraState_SetFromMethod(IStateCoder *p, UInt64 id, int encodeMode, ISzAllocPtr alloc);

static SRes SeqInFilter_Init(CSeqInFilter *p, const CXzFilter *props, ISzAllocPtr alloc)
{
  if (!p->buf)
  {
    p->buf = (Byte *)ISzAlloc_Alloc(alloc, FILTER_BUF_SIZE);
    if (!p->buf)
      return SZ_ERROR_MEM;
  }
  p->curPos = p->endPos = 0;
  p->srcWasFinished = 0;
  RINOK(BraState_SetFromMethod(&p->StateCoder, props->id, 1, alloc));
  RINOK(p->StateCoder.SetProps(p->StateCoder.p, props->props, props->propsSize, alloc));
  p->StateCoder.Init(p->StateCoder.p);
  return SZ_OK;
}


static SRes SeqInFilter_Read(const ISeqInStream *pp, void *data, size_t *size)
{
  CSeqInFilter *p = CONTAINER_FROM_VTBL(pp, CSeqInFilter, p);
  size_t sizeOriginal = *size;
  if (sizeOriginal == 0)
    return SZ_OK;
  *size = 0;
  
  for (;;)
  {
    if (!p->srcWasFinished && p->curPos == p->endPos)
    {
      p->curPos = 0;
      p->endPos = FILTER_BUF_SIZE;
      RINOK(ISeqInStream_Read(p->realStream, p->buf, &p->endPos));
      if (p->endPos == 0)
        p->srcWasFinished = 1;
    }
    {
      SizeT srcLen = p->endPos - p->curPos;
      ECoderStatus status;
      SRes res;
      *size = sizeOriginal;
      res = p->StateCoder.Code2(p->StateCoder.p,
          (Byte *)data, size,
          p->buf + p->curPos, &srcLen,
          p->srcWasFinished, CODER_FINISH_ANY,
          &status);
      p->curPos += srcLen;
      if (*size != 0 || srcLen == 0 || res != SZ_OK)
        return res;
    }
  }
}

static void SeqInFilter_Construct(CSeqInFilter *p)
{
  p->buf = NULL;
  p->StateCoder.p = NULL;
  p->p.Read = SeqInFilter_Read;
}

static void SeqInFilter_Free(CSeqInFilter *p, ISzAllocPtr alloc)
{
  if (p->StateCoder.p)
  {
    p->StateCoder.Free(p->StateCoder.p, alloc);
    p->StateCoder.p = NULL;
  }
  if (p->buf)
  {
    ISzAlloc_Free(alloc, p->buf);
    p->buf = NULL;
  }
}



/* ---------- CXzProps ---------- */


void XzFilterProps_Init(CXzFilterProps *p)
{
  p->id = 0;
  p->delta = 0;
  p->ip = 0;
  p->ipDefined = False;
}

void XzProps_Init(CXzProps *p)
{
  p->checkId = XZ_CHECK_CRC32;
  p->blockSize = XZ_PROPS__BLOCK_SIZE__AUTO;
  p->numBlockThreads_Reduced = -1;
  p->numBlockThreads_Max = -1;
  p->numTotalThreads = -1;
  p->reduceSize = (UInt64)(Int64)-1;
  p->forceWriteSizesInHeader = 0;
  // p->forceWriteSizesInHeader = 1;

  XzFilterProps_Init(&p->filterProps);
  Lzma2EncProps_Init(&p->lzma2Props);
}


static void XzEncProps_Normalize_Fixed(CXzProps *p)
{
  UInt64 fileSize;
  int t1, t1n, t2, t2r, t3;
  {
    CLzma2EncProps tp = p->lzma2Props;
    if (tp.numTotalThreads <= 0)
      tp.numTotalThreads = p->numTotalThreads;
    Lzma2EncProps_Normalize(&tp);
    t1n = tp.numTotalThreads;
  }

  t1 = p->lzma2Props.numTotalThreads;
  t2 = p->numBlockThreads_Max;
  t3 = p->numTotalThreads;

  if (t2 > MTCODER__THREADS_MAX)
    t2 = MTCODER__THREADS_MAX;

  if (t3 <= 0)
  {
    if (t2 <= 0)
      t2 = 1;
    t3 = t1n * t2;
  }
  else if (t2 <= 0)
  {
    t2 = t3 / t1n;
    if (t2 == 0)
    {
      t1 = 1;
      t2 = t3;
    }
    if (t2 > MTCODER__THREADS_MAX)
      t2 = MTCODER__THREADS_MAX;
  }
  else if (t1 <= 0)
  {
    t1 = t3 / t2;
    if (t1 == 0)
      t1 = 1;
  }
  else
    t3 = t1n * t2;

  p->lzma2Props.numTotalThreads = t1;

  t2r = t2;

  fileSize = p->reduceSize;

  if ((p->blockSize < fileSize || fileSize == (UInt64)(Int64)-1))
    p->lzma2Props.lzmaProps.reduceSize = p->blockSize;

  Lzma2EncProps_Normalize(&p->lzma2Props);

  t1 = p->lzma2Props.numTotalThreads;

  {
    if (t2 > 1 && fileSize != (UInt64)(Int64)-1)
    {
      UInt64 numBlocks = fileSize / p->blockSize;
      if (numBlocks * p->blockSize != fileSize)
        numBlocks++;
      if (numBlocks < (unsigned)t2)
      {
        t2r = (int)numBlocks;
        if (t2r == 0)
          t2r = 1;
        t3 = t1 * t2r;
      }
    }
  }
  
  p->numBlockThreads_Max = t2;
  p->numBlockThreads_Reduced = t2r;
  p->numTotalThreads = t3;
}


static void XzProps_Normalize(CXzProps *p)
{
  /* we normalize xzProps properties, but we normalize only some of CXzProps::lzma2Props properties.
     Lzma2Enc_SetProps() will normalize lzma2Props later. */
  
  if (p->blockSize == XZ_PROPS__BLOCK_SIZE__SOLID)
  {
    p->lzma2Props.lzmaProps.reduceSize = p->reduceSize;
    p->numBlockThreads_Reduced = 1;
    p->numBlockThreads_Max = 1;
    if (p->lzma2Props.numTotalThreads <= 0)
      p->lzma2Props.numTotalThreads = p->numTotalThreads;
    return;
  }
  else
  {
    CLzma2EncProps *lzma2 = &p->lzma2Props;
    if (p->blockSize == LZMA2_ENC_PROPS__BLOCK_SIZE__AUTO)
    {
      // xz-auto
      p->lzma2Props.lzmaProps.reduceSize = p->reduceSize;

      if (lzma2->blockSize == LZMA2_ENC_PROPS__BLOCK_SIZE__SOLID)
      {
        // if (xz-auto && lzma2-solid) - we use solid for both
        p->blockSize = XZ_PROPS__BLOCK_SIZE__SOLID;
        p->numBlockThreads_Reduced = 1;
        p->numBlockThreads_Max = 1;
        if (p->lzma2Props.numTotalThreads <= 0)
          p->lzma2Props.numTotalThreads = p->numTotalThreads;
      }
      else
      {
        // if (xz-auto && (lzma2-auto || lzma2-fixed_)
        //   we calculate block size for lzma2 and use that block size for xz, lzma2 uses single-chunk per block
        CLzma2EncProps tp = p->lzma2Props;
        if (tp.numTotalThreads <= 0)
          tp.numTotalThreads = p->numTotalThreads;
        
        Lzma2EncProps_Normalize(&tp);
        
        p->blockSize = tp.blockSize; // fixed or solid
        p->numBlockThreads_Reduced = tp.numBlockThreads_Reduced;
        p->numBlockThreads_Max = tp.numBlockThreads_Max;
        if (lzma2->blockSize == LZMA2_ENC_PROPS__BLOCK_SIZE__AUTO)
          lzma2->blockSize = tp.blockSize; // fixed or solid, LZMA2_ENC_PROPS__BLOCK_SIZE__SOLID
        if (lzma2->lzmaProps.reduceSize > tp.blockSize && tp.blockSize != LZMA2_ENC_PROPS__BLOCK_SIZE__SOLID)
          lzma2->lzmaProps.reduceSize = tp.blockSize;
        lzma2->numBlockThreads_Reduced = 1;
        lzma2->numBlockThreads_Max = 1;
        return;
      }
    }
    else
    {
      // xz-fixed
      // we can use xz::reduceSize or xz::blockSize as base for lzmaProps::reduceSize
      
      p->lzma2Props.lzmaProps.reduceSize = p->reduceSize;
      {
        UInt64 r = p->reduceSize;
        if (r > p->blockSize || r == (UInt64)(Int64)-1)
          r = p->blockSize;
        lzma2->lzmaProps.reduceSize = r;
      }
      if (lzma2->blockSize == LZMA2_ENC_PROPS__BLOCK_SIZE__AUTO)
        lzma2->blockSize = LZMA2_ENC_PROPS__BLOCK_SIZE__SOLID;
      else if (lzma2->blockSize > p->blockSize && lzma2->blockSize != LZMA2_ENC_PROPS__BLOCK_SIZE__SOLID)
        lzma2->blockSize = p->blockSize;
      
      XzEncProps_Normalize_Fixed(p);
    }
  }
}


/* ---------- CLzma2WithFilters ---------- */

typedef struct
{
  CLzma2EncHandle lzma2;
  CSeqInFilter filter;
  
} CLzma2WithFilters;


static void Lzma2WithFilters_Construct(CLzma2WithFilters *p)
{
  p->lzma2 = NULL;
  SeqInFilter_Construct(&p->filter);
}


static SRes Lzma2WithFilters_Create(CLzma2WithFilters *p, ISzAllocPtr alloc, ISzAllocPtr bigAlloc)
{
  if (!p->lzma2)
  {
    p->lzma2 = Lzma2Enc_Create(alloc, bigAlloc);
    if (!p->lzma2)
      return SZ_ERROR_MEM;
  }
  return SZ_OK;
}


static void Lzma2WithFilters_Free(CLzma2WithFilters *p, ISzAllocPtr alloc)
{
  SeqInFilter_Free(&p->filter, alloc);
  if (p->lzma2)
  {
    Lzma2Enc_Destroy(p->lzma2);
    p->lzma2 = NULL;
  }
}


typedef struct
{
  UInt64 unpackSize;
  UInt64 totalSize;
  size_t headerSize;
} CXzEncBlockInfo;


static SRes Xz_CompressBlock(
    CLzma2WithFilters *lzmaf,
    
    ISeqOutStream *outStream,
    Byte *outBufHeader,
    Byte *outBufData, size_t outBufDataLimit,

    ISeqInStream *inStream,
    // UInt64 expectedSize,
    const Byte *inBuf, // used if (!inStream)
    size_t inBufSize,  // used if (!inStream), it's block size, props->blockSize is ignored

    const CXzProps *props,
    ICompressProgress *progress,
    int *inStreamFinished,  /* only for inStream version */
    CXzEncBlockInfo *blockSizes,
    ISzAllocPtr alloc,
    ISzAllocPtr allocBig)
{
  CSeqCheckInStream checkInStream;
  CSeqSizeOutStream seqSizeOutStream;
  CXzBlock block;
  unsigned filterIndex = 0;
  CXzFilter *filter = NULL;
  const CXzFilterProps *fp = &props->filterProps;
  if (fp->id == 0)
    fp = NULL;
  
  *inStreamFinished = False;
  
  RINOK(Lzma2WithFilters_Create(lzmaf, alloc, allocBig));
  
  RINOK(Lzma2Enc_SetProps(lzmaf->lzma2, &props->lzma2Props));
  
  XzBlock_ClearFlags(&block);
  XzBlock_SetNumFilters(&block, 1 + (fp ? 1 : 0));
  
  if (fp)
  {
    filter = &block.filters[filterIndex++];
    filter->id = fp->id;
    filter->propsSize = 0;
    
    if (fp->id == XZ_ID_Delta)
    {
      filter->props[0] = (Byte)(fp->delta - 1);
      filter->propsSize = 1;
    }
    else if (fp->ipDefined)
    {
      Byte *ptr = filter->props;
      SetUi32(ptr, fp->ip);
      filter->propsSize = 4;
    }
  }
  
  {
    CXzFilter *f = &block.filters[filterIndex++];
    f->id = XZ_ID_LZMA2;
    f->propsSize = 1;
    f->props[0] = Lzma2Enc_WriteProperties(lzmaf->lzma2);
  }
  
  seqSizeOutStream.vt.Write = SeqSizeOutStream_Write;
  seqSizeOutStream.realStream = outStream;
  seqSizeOutStream.outBuf = outBufData;
  seqSizeOutStream.outBufLimit = outBufDataLimit;
  seqSizeOutStream.processed = 0;
    
  /*
  if (expectedSize != (UInt64)(Int64)-1)
  {
    block.unpackSize = expectedSize;
    if (props->blockSize != (UInt64)(Int64)-1)
      if (expectedSize > props->blockSize)
        block.unpackSize = props->blockSize;
    XzBlock_SetHasUnpackSize(&block);
  }
  */

  if (outStream)
  {
    RINOK(XzBlock_WriteHeader(&block, &seqSizeOutStream.vt));
  }
  
  checkInStream.vt.Read = SeqCheckInStream_Read;
  SeqCheckInStream_Init(&checkInStream, props->checkId);
  
  checkInStream.realStream = inStream;
  checkInStream.data = inBuf;
  checkInStream.limit = props->blockSize;
  if (!inStream)
    checkInStream.limit = inBufSize;

  if (fp)
  {
    lzmaf->filter.realStream = &checkInStream.vt;
    RINOK(SeqInFilter_Init(&lzmaf->filter, filter, alloc));
  }

  {
    SRes res;
    Byte *outBuf = NULL;
    size_t outSize = 0;
    BoolInt useStream = (fp || inStream);
    // useStream = True;
    
    if (!useStream)
    {
      XzCheck_Update(&checkInStream.check, inBuf, inBufSize);
      checkInStream.processed = inBufSize;
    }
    
    if (!outStream)
    {
      outBuf = seqSizeOutStream.outBuf; //  + (size_t)seqSizeOutStream.processed;
      outSize = seqSizeOutStream.outBufLimit; // - (size_t)seqSizeOutStream.processed;
    }
    
    res = Lzma2Enc_Encode2(lzmaf->lzma2,
        outBuf ? NULL : &seqSizeOutStream.vt,
        outBuf,
        outBuf ? &outSize : NULL,
      
        useStream ?
          (fp ?
            (
            &lzmaf->filter.p) :
            &checkInStream.vt) : NULL,
      
        useStream ? NULL : inBuf,
        useStream ? 0 : inBufSize,
        
        progress);
    
    if (outBuf)
      seqSizeOutStream.processed += outSize;
    
    RINOK(res);
    blockSizes->unpackSize = checkInStream.processed;
  }
  {
    Byte buf[4 + 64];
    unsigned padSize = XZ_GET_PAD_SIZE(seqSizeOutStream.processed);
    UInt64 packSize = seqSizeOutStream.processed;
    
    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;
    
    SeqCheckInStream_GetDigest(&checkInStream, buf + 4);
    RINOK(WriteBytes(&seqSizeOutStream.vt, buf + (4 - padSize), padSize + XzFlags_GetCheckSize((CXzStreamFlags)props->checkId)));
    
    blockSizes->totalSize = seqSizeOutStream.processed - padSize;
    
    if (!outStream)
    {
      seqSizeOutStream.outBuf = outBufHeader;
      seqSizeOutStream.outBufLimit = XZ_BLOCK_HEADER_SIZE_MAX;
      seqSizeOutStream.processed = 0;
      
      block.unpackSize = blockSizes->unpackSize;
      XzBlock_SetHasUnpackSize(&block);
      
      block.packSize = packSize;
      XzBlock_SetHasPackSize(&block);
      
      RINOK(XzBlock_WriteHeader(&block, &seqSizeOutStream.vt));
      
      blockSizes->headerSize = (size_t)seqSizeOutStream.processed;
      blockSizes->totalSize += seqSizeOutStream.processed;
    }
  }
  
  if (inStream)
    *inStreamFinished = checkInStream.realStreamFinished;
  else
  {
    *inStreamFinished = False;
    if (checkInStream.processed != inBufSize)
      return SZ_ERROR_FAIL;
  }

  return SZ_OK;
}



typedef struct
{
  ICompressProgress vt;
  ICompressProgress *progress;
  UInt64 inOffset;
  UInt64 outOffset;
} CCompressProgress_XzEncOffset;


static SRes CompressProgress_XzEncOffset_Progress(const ICompressProgress *pp, UInt64 inSize, UInt64 outSize)
{
  const CCompressProgress_XzEncOffset *p = CONTAINER_FROM_VTBL(pp, CCompressProgress_XzEncOffset, vt);
  inSize += p->inOffset;
  outSize += p->outOffset;
  return ICompressProgress_Progress(p->progress, inSize, outSize);
}




typedef struct
{
  ISzAllocPtr alloc;
  ISzAllocPtr allocBig;

  CXzProps xzProps;
  UInt64 expectedDataSize;

  CXzEncIndex xzIndex;

  CLzma2WithFilters lzmaf_Items[MTCODER__THREADS_MAX];
  
  size_t outBufSize;       /* size of allocated outBufs[i] */
  Byte *outBufs[MTCODER__BLOCKS_MAX];

  #ifndef _7ZIP_ST
  unsigned checkType;
  ISeqOutStream *outStream;
  BoolInt mtCoder_WasConstructed;
  CMtCoder mtCoder;
  CXzEncBlockInfo EncBlocks[MTCODER__BLOCKS_MAX];
  #endif

} CXzEnc;


static void XzEnc_Construct(CXzEnc *p)
{
  unsigned i;

  XzEncIndex_Construct(&p->xzIndex);

  for (i = 0; i < MTCODER__THREADS_MAX; i++)
    Lzma2WithFilters_Construct(&p->lzmaf_Items[i]);

  #ifndef _7ZIP_ST
  p->mtCoder_WasConstructed = False;
  {
    for (i = 0; i < MTCODER__BLOCKS_MAX; i++)
      p->outBufs[i] = NULL;
    p->outBufSize = 0;
  }
  #endif
}


static void XzEnc_FreeOutBufs(CXzEnc *p)
{
  unsigned i;
  for (i = 0; i < MTCODER__BLOCKS_MAX; i++)
    if (p->outBufs[i])
    {
      ISzAlloc_Free(p->alloc, p->outBufs[i]);
      p->outBufs[i] = NULL;
    }
  p->outBufSize = 0;
}


static void XzEnc_Free(CXzEnc *p, ISzAllocPtr alloc)
{
  unsigned i;

  XzEncIndex_Free(&p->xzIndex, alloc);

  for (i = 0; i < MTCODER__THREADS_MAX; i++)
    Lzma2WithFilters_Free(&p->lzmaf_Items[i], alloc);
  
  #ifndef _7ZIP_ST
  if (p->mtCoder_WasConstructed)
  {
    MtCoder_Destruct(&p->mtCoder);
    p->mtCoder_WasConstructed = False;
  }
  XzEnc_FreeOutBufs(p);
  #endif
}


CXzEncHandle XzEnc_Create(ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CXzEnc *p = (CXzEnc *)ISzAlloc_Alloc(alloc, sizeof(CXzEnc));
  if (!p)
    return NULL;
  XzEnc_Construct(p);
  XzProps_Init(&p->xzProps);
  XzProps_Normalize(&p->xzProps);
  p->expectedDataSize = (UInt64)(Int64)-1;
  p->alloc = alloc;
  p->allocBig = allocBig;
  return p;
}


void XzEnc_Destroy(CXzEncHandle pp)
{
  CXzEnc *p = (CXzEnc *)pp;
  XzEnc_Free(p, p->alloc);
  ISzAlloc_Free(p->alloc, p);
}


SRes XzEnc_SetProps(CXzEncHandle pp, const CXzProps *props)
{
  CXzEnc *p = (CXzEnc *)pp;
  p->xzProps = *props;
  XzProps_Normalize(&p->xzProps);
  return SZ_OK;
}


void XzEnc_SetDataSize(CXzEncHandle pp, UInt64 expectedDataSiize)
{
  CXzEnc *p = (CXzEnc *)pp;
  p->expectedDataSize = expectedDataSiize;
}




#ifndef _7ZIP_ST

static SRes XzEnc_MtCallback_Code(void *pp, unsigned coderIndex, unsigned outBufIndex,
    const Byte *src, size_t srcSize, int finished)
{
  CXzEnc *me = (CXzEnc *)pp;
  SRes res;
  CMtProgressThunk progressThunk;

  Byte *dest = me->outBufs[outBufIndex];

  UNUSED_VAR(finished)

  {
    CXzEncBlockInfo *bInfo = &me->EncBlocks[outBufIndex];
    bInfo->totalSize = 0;
    bInfo->unpackSize = 0;
    bInfo->headerSize = 0;
  }

  if (!dest)
  {
    dest = (Byte *)ISzAlloc_Alloc(me->alloc, me->outBufSize);
    if (!dest)
      return SZ_ERROR_MEM;
    me->outBufs[outBufIndex] = dest;
  }
  
  MtProgressThunk_CreateVTable(&progressThunk);
  progressThunk.mtProgress = &me->mtCoder.mtProgress;
  MtProgressThunk_Init(&progressThunk);

  {
    CXzEncBlockInfo blockSizes;
    int inStreamFinished;

    res = Xz_CompressBlock(
        &me->lzmaf_Items[coderIndex],
        
        NULL,
        dest,
        dest + XZ_BLOCK_HEADER_SIZE_MAX, me->outBufSize - XZ_BLOCK_HEADER_SIZE_MAX,

        NULL,
        // srcSize, // expectedSize
        src, srcSize,

        &me->xzProps,
        &progressThunk.vt,
        &inStreamFinished,
        &blockSizes,
        me->alloc,
        me->allocBig);
    
    if (res == SZ_OK)
      me->EncBlocks[outBufIndex] = blockSizes;

    return res;
  }
}


static SRes XzEnc_MtCallback_Write(void *pp, unsigned outBufIndex)
{
  CXzEnc *me = (CXzEnc *)pp;

  const CXzEncBlockInfo *bInfo = &me->EncBlocks[outBufIndex];
  const Byte *data = me->outBufs[outBufIndex];

  RINOK(WriteBytes(me->outStream, data, bInfo->headerSize));

  {
    UInt64 totalPackFull = bInfo->totalSize + XZ_GET_PAD_SIZE(bInfo->totalSize);
    RINOK(WriteBytes(me->outStream, data + XZ_BLOCK_HEADER_SIZE_MAX, (size_t)totalPackFull - bInfo->headerSize));
  }

  return XzEncIndex_AddIndexRecord(&me->xzIndex, bInfo->unpackSize, bInfo->totalSize, me->alloc);
}

#endif



SRes XzEnc_Encode(CXzEncHandle pp, ISeqOutStream *outStream, ISeqInStream *inStream, ICompressProgress *progress)
{
  CXzEnc *p = (CXzEnc *)pp;

  const CXzProps *props = &p->xzProps;

  XzEncIndex_Init(&p->xzIndex);
  {
    UInt64 numBlocks = 1;
    UInt64 blockSize = props->blockSize;
    
    if (blockSize != XZ_PROPS__BLOCK_SIZE__SOLID
        && props->reduceSize != (UInt64)(Int64)-1)
    {
      numBlocks = props->reduceSize / blockSize;
      if (numBlocks * blockSize != props->reduceSize)
        numBlocks++;
    }
    else
      blockSize = (UInt64)1 << 62;
    
    RINOK(XzEncIndex_PreAlloc(&p->xzIndex, numBlocks, blockSize, XZ_GET_ESTIMATED_BLOCK_TOTAL_PACK_SIZE(blockSize), p->alloc));
  }

  RINOK(Xz_WriteHeader((CXzStreamFlags)props->checkId, outStream));


  #ifndef _7ZIP_ST
  if (props->numBlockThreads_Reduced > 1)
  {
    IMtCoderCallback2 vt;

    if (!p->mtCoder_WasConstructed)
    {
      p->mtCoder_WasConstructed = True;
      MtCoder_Construct(&p->mtCoder);
    }

    vt.Code = XzEnc_MtCallback_Code;
    vt.Write = XzEnc_MtCallback_Write;

    p->checkType = props->checkId;
    p->xzProps = *props;
    
    p->outStream = outStream;

    p->mtCoder.allocBig = p->allocBig;
    p->mtCoder.progress = progress;
    p->mtCoder.inStream = inStream;
    p->mtCoder.inData = NULL;
    p->mtCoder.inDataSize = 0;
    p->mtCoder.mtCallback = &vt;
    p->mtCoder.mtCallbackObject = p;

    if (   props->blockSize == XZ_PROPS__BLOCK_SIZE__SOLID
        || props->blockSize == XZ_PROPS__BLOCK_SIZE__AUTO)
      return SZ_ERROR_FAIL;

    p->mtCoder.blockSize = (size_t)props->blockSize;
    if (p->mtCoder.blockSize != props->blockSize)
      return SZ_ERROR_PARAM; /* SZ_ERROR_MEM */

    {
      size_t destBlockSize = XZ_BLOCK_HEADER_SIZE_MAX + XZ_GET_MAX_BLOCK_PACK_SIZE(p->mtCoder.blockSize);
      if (destBlockSize < p->mtCoder.blockSize)
        return SZ_ERROR_PARAM;
      if (p->outBufSize != destBlockSize)
        XzEnc_FreeOutBufs(p);
      p->outBufSize = destBlockSize;
    }

    p->mtCoder.numThreadsMax = (unsigned)props->numBlockThreads_Max;
    p->mtCoder.expectedDataSize = p->expectedDataSize;
    
    RINOK(MtCoder_Code(&p->mtCoder));
  }
  else
  #endif
  {
    int writeStartSizes;
    CCompressProgress_XzEncOffset progress2;
    Byte *bufData = NULL;
    size_t bufSize = 0;

    progress2.vt.Progress = CompressProgress_XzEncOffset_Progress;
    progress2.inOffset = 0;
    progress2.outOffset = 0;
    progress2.progress = progress;
    
    writeStartSizes = 0;
    
    if (props->blockSize != XZ_PROPS__BLOCK_SIZE__SOLID)
    {
      writeStartSizes = (props->forceWriteSizesInHeader > 0);
      
      if (writeStartSizes)
      {
        size_t t2;
        size_t t = (size_t)props->blockSize;
        if (t != props->blockSize)
          return SZ_ERROR_PARAM;
        t = XZ_GET_MAX_BLOCK_PACK_SIZE(t);
        if (t < props->blockSize)
          return SZ_ERROR_PARAM;
        t2 = XZ_BLOCK_HEADER_SIZE_MAX + t;
        if (!p->outBufs[0] || t2 != p->outBufSize)
        {
          XzEnc_FreeOutBufs(p);
          p->outBufs[0] = (Byte *)ISzAlloc_Alloc(p->alloc, t2);
          if (!p->outBufs[0])
            return SZ_ERROR_MEM;
          p->outBufSize = t2;
        }
        bufData = p->outBufs[0] + XZ_BLOCK_HEADER_SIZE_MAX;
        bufSize = t;
      }
    }
    
    for (;;)
    {
      CXzEncBlockInfo blockSizes;
      int inStreamFinished;
      
      /*
      UInt64 rem = (UInt64)(Int64)-1;
      if (props->reduceSize != (UInt64)(Int64)-1
          && props->reduceSize >= progress2.inOffset)
        rem = props->reduceSize - progress2.inOffset;
      */

      blockSizes.headerSize = 0; // for GCC
      
      RINOK(Xz_CompressBlock(
          &p->lzmaf_Items[0],
          
          writeStartSizes ? NULL : outStream,
          writeStartSizes ? p->outBufs[0] : NULL,
          bufData, bufSize,
          
          inStream,
          // rem,
          NULL, 0,
          
          props,
          progress ? &progress2.vt : NULL,
          &inStreamFinished,
          &blockSizes,
          p->alloc,
          p->allocBig));

      {
        UInt64 totalPackFull = blockSizes.totalSize + XZ_GET_PAD_SIZE(blockSizes.totalSize);
      
        if (writeStartSizes)
        {
          RINOK(WriteBytes(outStream, p->outBufs[0], blockSizes.headerSize));
          RINOK(WriteBytes(outStream, bufData, (size_t)totalPackFull - blockSizes.headerSize));
        }
        
        RINOK(XzEncIndex_AddIndexRecord(&p->xzIndex, blockSizes.unpackSize, blockSizes.totalSize, p->alloc));
        
        progress2.inOffset += blockSizes.unpackSize;
        progress2.outOffset += totalPackFull;
      }
        
      if (inStreamFinished)
        break;
    }
  }

  return XzEncIndex_WriteFooter(&p->xzIndex, (CXzStreamFlags)props->checkId, outStream);
}


#include "Alloc.h"

SRes Xz_Encode(ISeqOutStream *outStream, ISeqInStream *inStream,
    const CXzProps *props, ICompressProgress *progress)
{
  SRes res;
  CXzEncHandle xz = XzEnc_Create(&g_Alloc, &g_BigAlloc);
  if (!xz)
    return SZ_ERROR_MEM;
  res = XzEnc_SetProps(xz, props);
  if (res == SZ_OK)
    res = XzEnc_Encode(xz, outStream, inStream, progress);
  XzEnc_Destroy(xz);
  return res;
}


SRes Xz_EncodeEmpty(ISeqOutStream *outStream)
{
  SRes res;
  CXzEncIndex xzIndex;
  XzEncIndex_Construct(&xzIndex);
  res = Xz_WriteHeader((CXzStreamFlags)0, outStream);
  if (res == SZ_OK)
    res = XzEncIndex_WriteFooter(&xzIndex, (CXzStreamFlags)0, outStream);
  XzEncIndex_Free(&xzIndex, NULL); // g_Alloc
  return res;
}




/* XzDec.cpp */




#ifdef XZ_DUMP
#include <cstdio>
#endif

// #define SHOW_DEBUG_INFO

#ifdef SHOW_DEBUG_INFO
#include <cstdio>
#endif

#ifdef SHOW_DEBUG_INFO
#define PRF(x) x
#else
#define PRF(x)
#endif

#define PRF_STR(s) PRF(printf("\n" s "\n"))
#define PRF_STR_INT(s, d) PRF(printf("\n" s " %d\n", (unsigned)d))


#include "Delta.h"
#include "Lzma2Dec.h"


#define XZ_CHECK_SIZE_MAX 64

#define CODER_BUF_SIZE ((size_t)1 << 17)

unsigned Xz_ReadVarInt(const Byte *p, size_t maxSize, UInt64 *value)
{
  unsigned i, limit;
  *value = 0;
  limit = (maxSize > 9) ? 9 : (unsigned)maxSize;

  for (i = 0; i < limit;)
  {
    Byte b = p[i];
    *value |= (UInt64)(b & 0x7F) << (7 * i++);
    if ((b & 0x80) == 0)
      return (b == 0 && i != 1) ? 0 : i;
  }
  return 0;
}

/* ---------- BraState ---------- */

#define BRA_BUF_SIZE (1 << 14)

typedef struct
{
  size_t bufPos;
  size_t bufConv;
  size_t bufTotal;

  int encodeMode;

  UInt32 methodId;
  UInt32 delta;
  UInt32 ip;
  UInt32 x86State;
  Byte deltaState[DELTA_STATE_SIZE];

  Byte buf[BRA_BUF_SIZE];
} CBraState;

static void BraState_Free(void *pp, ISzAllocPtr alloc)
{
  ISzAlloc_Free(alloc, pp);
}

static SRes BraState_SetProps(void *pp, const Byte *props, size_t propSize, ISzAllocPtr alloc)
{
  CBraState *p = ((CBraState *)pp);
  UNUSED_VAR(alloc);
  p->ip = 0;
  if (p->methodId == XZ_ID_Delta)
  {
    if (propSize != 1)
      return SZ_ERROR_UNSUPPORTED;
    p->delta = (unsigned)props[0] + 1;
  }
  else
  {
    if (propSize == 4)
    {
      UInt32 v = GetUi32(props);
      switch (p->methodId)
      {
        case XZ_ID_PPC:
        case XZ_ID_ARM:
        case XZ_ID_SPARC:
          if ((v & 3) != 0)
            return SZ_ERROR_UNSUPPORTED;
          break;
        case XZ_ID_ARMT:
          if ((v & 1) != 0)
            return SZ_ERROR_UNSUPPORTED;
          break;
        case XZ_ID_IA64:
          if ((v & 0xF) != 0)
            return SZ_ERROR_UNSUPPORTED;
          break;
      }
      p->ip = v;
    }
    else if (propSize != 0)
      return SZ_ERROR_UNSUPPORTED;
  }
  return SZ_OK;
}

static void BraState_Init(void *pp)
{
  CBraState *p = ((CBraState *)pp);
  p->bufPos = p->bufConv = p->bufTotal = 0;
  x86_Convert_Init(p->x86State);
  if (p->methodId == XZ_ID_Delta)
    Delta_Init(p->deltaState);
}


#define CASE_BRA_CONV(isa) case XZ_ID_ ## isa: size = isa ## _Convert(data, size, p->ip, p->encodeMode); break;

static SizeT BraState_Filter(void *pp, Byte *data, SizeT size)
{
  CBraState *p = ((CBraState *)pp);
  switch (p->methodId)
  {
    case XZ_ID_Delta:
      if (p->encodeMode)
        Delta_Encode(p->deltaState, p->delta, data, size);
      else
        Delta_Decode(p->deltaState, p->delta, data, size);
      break;
    case XZ_ID_X86:
      size = x86_Convert(data, size, p->ip, &p->x86State, p->encodeMode);
      break;
    CASE_BRA_CONV(PPC)
    CASE_BRA_CONV(IA64)
    CASE_BRA_CONV(ARM)
    CASE_BRA_CONV(ARMT)
    CASE_BRA_CONV(SPARC)
  }
  p->ip += (UInt32)size;
  return size;
}


static SRes BraState_Code2(void *pp,
    Byte *dest, SizeT *destLen,
    const Byte *src, SizeT *srcLen, int srcWasFinished,
    ECoderFinishMode finishMode,
    // int *wasFinished
    ECoderStatus *status)
{
  CBraState *p = ((CBraState *)pp);
  SizeT destRem = *destLen;
  SizeT srcRem = *srcLen;
  UNUSED_VAR(finishMode);

  *destLen = 0;
  *srcLen = 0;
  // *wasFinished = False;
  *status = CODER_STATUS_NOT_FINISHED;
  
  while (destRem > 0)
  {
    if (p->bufPos != p->bufConv)
    {
      size_t size = p->bufConv - p->bufPos;
      if (size > destRem)
        size = destRem;
      memcpy(dest, p->buf + p->bufPos, size);
      p->bufPos += size;
      *destLen += size;
      dest += size;
      destRem -= size;
      continue;
    }
    
    p->bufTotal -= p->bufPos;
    memmove(p->buf, p->buf + p->bufPos, p->bufTotal);
    p->bufPos = 0;
    p->bufConv = 0;
    {
      size_t size = BRA_BUF_SIZE - p->bufTotal;
      if (size > srcRem)
        size = srcRem;
      memcpy(p->buf + p->bufTotal, src, size);
      *srcLen += size;
      src += size;
      srcRem -= size;
      p->bufTotal += size;
    }
    if (p->bufTotal == 0)
      break;
    
    p->bufConv = BraState_Filter(pp, p->buf, p->bufTotal);

    if (p->bufConv == 0)
    {
      if (!srcWasFinished)
        break;
      p->bufConv = p->bufTotal;
    }
  }

  if (p->bufTotal == p->bufPos && srcRem == 0 && srcWasFinished)
  {
    *status = CODER_STATUS_FINISHED_WITH_MARK;
    // *wasFinished = 1;
  }

  return SZ_OK;
}


SRes BraState_SetFromMethod(IStateCoder *p, UInt64 id, int encodeMode, ISzAllocPtr alloc);
SRes BraState_SetFromMethod(IStateCoder *p, UInt64 id, int encodeMode, ISzAllocPtr alloc)
{
  CBraState *decoder;
  if (id < XZ_ID_Delta || id > XZ_ID_SPARC)
    return SZ_ERROR_UNSUPPORTED;
  decoder = (CBraState *)p->p;
  if (!decoder)
  {
    decoder = (CBraState *)ISzAlloc_Alloc(alloc, sizeof(CBraState));
    if (!decoder)
      return SZ_ERROR_MEM;
    p->p = decoder;
    p->Free = BraState_Free;
    p->SetProps = BraState_SetProps;
    p->Init = BraState_Init;
    p->Code2 = BraState_Code2;
    p->Filter = BraState_Filter;
  }
  decoder->methodId = (UInt32)id;
  decoder->encodeMode = encodeMode;
  return SZ_OK;
}



/* ---------- Lzma2 ---------- */

typedef struct
{
  CLzma2Dec decoder;
  BoolInt outBufMode;
} CLzma2Dec_Spec;


static void Lzma2State_Free(void *pp, ISzAllocPtr alloc)
{
  CLzma2Dec_Spec *p = (CLzma2Dec_Spec *)pp;
  if (p->outBufMode)
    Lzma2Dec_FreeProbs(&p->decoder, alloc);
  else
    Lzma2Dec_Free(&p->decoder, alloc);
  ISzAlloc_Free(alloc, pp);
}

static SRes Lzma2State_SetProps(void *pp, const Byte *props, size_t propSize, ISzAllocPtr alloc)
{
  if (propSize != 1)
    return SZ_ERROR_UNSUPPORTED;
  {
    CLzma2Dec_Spec *p = (CLzma2Dec_Spec *)pp;
    if (p->outBufMode)
      return Lzma2Dec_AllocateProbs(&p->decoder, props[0], alloc);
    else
      return Lzma2Dec_Allocate(&p->decoder, props[0], alloc);
  }
}

static void Lzma2State_Init(void *pp)
{
  Lzma2Dec_Init(&((CLzma2Dec_Spec *)pp)->decoder);
}


/*
  if (outBufMode), then (dest) is not used. Use NULL.
         Data is unpacked to (spec->decoder.decoder.dic) output buffer.
*/

static SRes Lzma2State_Code2(void *pp, Byte *dest, SizeT *destLen, const Byte *src, SizeT *srcLen,
    int srcWasFinished, ECoderFinishMode finishMode,
    // int *wasFinished,
    ECoderStatus *status)
{
  CLzma2Dec_Spec *spec = (CLzma2Dec_Spec *)pp;
  ELzmaStatus status2;
  /* ELzmaFinishMode fm = (finishMode == LZMA_FINISH_ANY) ? LZMA_FINISH_ANY : LZMA_FINISH_END; */
  SRes res;
  UNUSED_VAR(srcWasFinished);
  if (spec->outBufMode)
  {
    SizeT dicPos = spec->decoder.decoder.dicPos;
    SizeT dicLimit = dicPos + *destLen;
    res = Lzma2Dec_DecodeToDic(&spec->decoder, dicLimit, src, srcLen, (ELzmaFinishMode)finishMode, &status2);
    *destLen = spec->decoder.decoder.dicPos - dicPos;
  }
  else
    res = Lzma2Dec_DecodeToBuf(&spec->decoder, dest, destLen, src, srcLen, (ELzmaFinishMode)finishMode, &status2);
  // *wasFinished = (status2 == LZMA_STATUS_FINISHED_WITH_MARK);
  // ECoderStatus values are identical to ELzmaStatus values of LZMA2 decoder
  *status = (ECoderStatus)status2;
  return res;
}


static SRes Lzma2State_SetFromMethod(IStateCoder *p, Byte *outBuf, size_t outBufSize, ISzAllocPtr alloc)
{
  CLzma2Dec_Spec *spec = (CLzma2Dec_Spec *)p->p;
  if (!spec)
  {
    spec = (CLzma2Dec_Spec *)ISzAlloc_Alloc(alloc, sizeof(CLzma2Dec_Spec));
    if (!spec)
      return SZ_ERROR_MEM;
    p->p = spec;
    p->Free = Lzma2State_Free;
    p->SetProps = Lzma2State_SetProps;
    p->Init = Lzma2State_Init;
    p->Code2 = Lzma2State_Code2;
    p->Filter = NULL;
    Lzma2Dec_Construct(&spec->decoder);
  }
  spec->outBufMode = False;
  if (outBuf)
  {
    spec->outBufMode = True;
    spec->decoder.decoder.dic = outBuf;
    spec->decoder.decoder.dicBufSize = outBufSize;
  }
  return SZ_OK;
}


static SRes Lzma2State_ResetOutBuf(IStateCoder *p, Byte *outBuf, size_t outBufSize)
{
  CLzma2Dec_Spec *spec = (CLzma2Dec_Spec *)p->p;
  if ((spec->outBufMode && !outBuf) || (!spec->outBufMode && outBuf))
    return SZ_ERROR_FAIL;
  if (outBuf)
  {
    spec->decoder.decoder.dic = outBuf;
    spec->decoder.decoder.dicBufSize = outBufSize;
  }
  return SZ_OK;
}



static void MixCoder_Construct(CMixCoder *p, ISzAllocPtr alloc)
{
  unsigned i;
  p->alloc = alloc;
  p->buf = NULL;
  p->numCoders = 0;
  
  p->outBufSize = 0;
  p->outBuf = NULL;
  // p->SingleBufMode = False;

  for (i = 0; i < MIXCODER_NUM_FILTERS_MAX; i++)
    p->coders[i].p = NULL;
}


static void MixCoder_Free(CMixCoder *p)
{
  unsigned i;
  p->numCoders = 0;
  for (i = 0; i < MIXCODER_NUM_FILTERS_MAX; i++)
  {
    IStateCoder *sc = &p->coders[i];
    if (sc->p)
    {
      sc->Free(sc->p, p->alloc);
      sc->p = NULL;
    }
  }
  if (p->buf)
  {
    ISzAlloc_Free(p->alloc, p->buf);
    p->buf = NULL; /* 9.31: the BUG was fixed */
  }
}

static void MixCoder_Init(CMixCoder *p)
{
  unsigned i;
  for (i = 0; i < MIXCODER_NUM_FILTERS_MAX - 1; i++)
  {
    p->size[i] = 0;
    p->pos[i] = 0;
    p->finished[i] = 0;
  }
  for (i = 0; i < p->numCoders; i++)
  {
    IStateCoder *coder = &p->coders[i];
    coder->Init(coder->p);
    p->results[i] = SZ_OK;
  }
  p->outWritten = 0;
  p->wasFinished = False;
  p->res = SZ_OK;
  p->status = CODER_STATUS_NOT_SPECIFIED;
}


static SRes MixCoder_SetFromMethod(CMixCoder *p, unsigned coderIndex, UInt64 methodId, Byte *outBuf, size_t outBufSize)
{
  IStateCoder *sc = &p->coders[coderIndex];
  p->ids[coderIndex] = methodId;
  switch (methodId)
  {
    case XZ_ID_LZMA2: return Lzma2State_SetFromMethod(sc, outBuf, outBufSize, p->alloc);
  }
  if (coderIndex == 0)
    return SZ_ERROR_UNSUPPORTED;
  return BraState_SetFromMethod(sc, methodId, 0, p->alloc);
}


static SRes MixCoder_ResetFromMethod(CMixCoder *p, unsigned coderIndex, UInt64 methodId, Byte *outBuf, size_t outBufSize)
{
  IStateCoder *sc = &p->coders[coderIndex];
  switch (methodId)
  {
    case XZ_ID_LZMA2: return Lzma2State_ResetOutBuf(sc, outBuf, outBufSize);
  }
  return SZ_ERROR_UNSUPPORTED;
}



/*
 if (destFinish) - then unpack data block is finished at (*destLen) position,
                   and we can return data that were not processed by filter

output (status) can be :
  CODER_STATUS_NOT_FINISHED
  CODER_STATUS_FINISHED_WITH_MARK
  CODER_STATUS_NEEDS_MORE_INPUT - not implemented still
*/

static SRes MixCoder_Code(CMixCoder *p,
    Byte *dest, SizeT *destLen, int destFinish,
    const Byte *src, SizeT *srcLen, int srcWasFinished,
    ECoderFinishMode finishMode)
{
  SizeT destLenOrig = *destLen;
  SizeT srcLenOrig = *srcLen;

  *destLen = 0;
  *srcLen = 0;

  if (p->wasFinished)
    return p->res;
  
  p->status = CODER_STATUS_NOT_FINISHED;

  // if (p->SingleBufMode)
  if (p->outBuf)
  {
    SRes res;
    SizeT destLen2, srcLen2;
    int wasFinished;
    
    PRF_STR("------- MixCoder Single ----------");
      
    srcLen2 = srcLenOrig;
    destLen2 = destLenOrig;
    
    {
      IStateCoder *coder = &p->coders[0];
      res = coder->Code2(coder->p, NULL, &destLen2, src, &srcLen2, srcWasFinished, finishMode,
          // &wasFinished,
          &p->status);
      wasFinished = (p->status == CODER_STATUS_FINISHED_WITH_MARK);
    }
    
    p->res = res;
    
    /*
    if (wasFinished)
      p->status = CODER_STATUS_FINISHED_WITH_MARK;
    else
    {
      if (res == SZ_OK)
        if (destLen2 != destLenOrig)
          p->status = CODER_STATUS_NEEDS_MORE_INPUT;
    }
    */

    
    *srcLen = srcLen2;
    src += srcLen2;
    p->outWritten += destLen2;
    
    if (res != SZ_OK || srcWasFinished || wasFinished)
      p->wasFinished = True;
    
    if (p->numCoders == 1)
      *destLen = destLen2;
    else if (p->wasFinished)
    {
      unsigned i;
      size_t processed = p->outWritten;
      
      for (i = 1; i < p->numCoders; i++)
      {
        IStateCoder *coder = &p->coders[i];
        processed = coder->Filter(coder->p, p->outBuf, processed);
        if (wasFinished || (destFinish && p->outWritten == destLenOrig))
          processed = p->outWritten;
        PRF_STR_INT("filter", i);
      }
      *destLen = processed;
    }
    return res;
  }

  PRF_STR("standard mix");

  if (p->numCoders != 1)
  {
    if (!p->buf)
    {
      p->buf = (Byte *)ISzAlloc_Alloc(p->alloc, CODER_BUF_SIZE * (MIXCODER_NUM_FILTERS_MAX - 1));
      if (!p->buf)
        return SZ_ERROR_MEM;
    }
    
    finishMode = CODER_FINISH_ANY;
  }

  for (;;)
  {
    BoolInt processed = False;
    BoolInt allFinished = True;
    SRes resMain = SZ_OK;
    unsigned i;

    p->status = CODER_STATUS_NOT_FINISHED;
    /*
    if (p->numCoders == 1 && *destLen == destLenOrig && finishMode == LZMA_FINISH_ANY)
      break;
    */

    for (i = 0; i < p->numCoders; i++)
    {
      SRes res;
      IStateCoder *coder = &p->coders[i];
      Byte *dest2;
      SizeT destLen2, srcLen2; // destLen2_Orig;
      const Byte *src2;
      int srcFinished2;
      int encodingWasFinished;
      ECoderStatus status2;
      
      if (i == 0)
      {
        src2 = src;
        srcLen2 = srcLenOrig - *srcLen;
        srcFinished2 = srcWasFinished;
      }
      else
      {
        size_t k = i - 1;
        src2 = p->buf + (CODER_BUF_SIZE * k) + p->pos[k];
        srcLen2 = p->size[k] - p->pos[k];
        srcFinished2 = p->finished[k];
      }
      
      if (i == p->numCoders - 1)
      {
        dest2 = dest;
        destLen2 = destLenOrig - *destLen;
      }
      else
      {
        if (p->pos[i] != p->size[i])
          continue;
        dest2 = p->buf + (CODER_BUF_SIZE * i);
        destLen2 = CODER_BUF_SIZE;
      }
      
      // destLen2_Orig = destLen2;
      
      if (p->results[i] != SZ_OK)
      {
        if (resMain == SZ_OK)
          resMain = p->results[i];
        continue;
      }

      res = coder->Code2(coder->p,
          dest2, &destLen2,
          src2, &srcLen2, srcFinished2,
          finishMode,
          // &encodingWasFinished,
          &status2);

      if (res != SZ_OK)
      {
        p->results[i] = res;
        if (resMain == SZ_OK)
          resMain = res;
      }

      encodingWasFinished = (status2 == CODER_STATUS_FINISHED_WITH_MARK);
      
      if (!encodingWasFinished)
      {
        allFinished = False;
        if (p->numCoders == 1 && res == SZ_OK)
          p->status = status2;
      }

      if (i == 0)
      {
        *srcLen += srcLen2;
        src += srcLen2;
      }
      else
        p->pos[(size_t)i - 1] += srcLen2;

      if (i == p->numCoders - 1)
      {
        *destLen += destLen2;
        dest += destLen2;
      }
      else
      {
        p->size[i] = destLen2;
        p->pos[i] = 0;
        p->finished[i] = encodingWasFinished;
      }
      
      if (destLen2 != 0 || srcLen2 != 0)
        processed = True;
    }
    
    if (!processed)
    {
      if (allFinished)
        p->status = CODER_STATUS_FINISHED_WITH_MARK;
      return resMain;
    }
  }
}


SRes Xz_ParseHeader(CXzStreamFlags *p, const Byte *buf)
{
  *p = (CXzStreamFlags)GetBe16(buf + XZ_SIG_SIZE);
  if (CrcCalc(buf + XZ_SIG_SIZE, XZ_STREAM_FLAGS_SIZE) !=
      GetUi32(buf + XZ_SIG_SIZE + XZ_STREAM_FLAGS_SIZE))
    return SZ_ERROR_NO_ARCHIVE;
  return XzFlags_IsSupported(*p) ? SZ_OK : SZ_ERROR_UNSUPPORTED;
}

static BoolInt Xz_CheckFooter(CXzStreamFlags flags, UInt64 indexSize, const Byte *buf)
{
  return indexSize == (((UInt64)GetUi32(buf + 4) + 1) << 2)
      && GetUi32(buf) == CrcCalc(buf + 4, 6)
      && flags == GetBe16(buf + 8)
      && buf[10] == XZ_FOOTER_SIG_0
      && buf[11] == XZ_FOOTER_SIG_1;
}

#define READ_VARINT_AND_CHECK(buf, pos, size, res) \
  { unsigned s = Xz_ReadVarInt(buf + pos, size - pos, res); \
  if (s == 0) return SZ_ERROR_ARCHIVE; \
  pos += s; }


static BoolInt XzBlock_AreSupportedFilters(const CXzBlock *p)
{
  unsigned numFilters = XzBlock_GetNumFilters(p) - 1;
  unsigned i;
  {
    const CXzFilter *f = &p->filters[numFilters];
    if (f->id != XZ_ID_LZMA2 || f->propsSize != 1 || f->props[0] > 40)
      return False;
  }

  for (i = 0; i < numFilters; i++)
  {
    const CXzFilter *f = &p->filters[i];
    if (f->id == XZ_ID_Delta)
    {
      if (f->propsSize != 1)
        return False;
    }
    else if (f->id < XZ_ID_Delta
        || f->id > XZ_ID_SPARC
        || (f->propsSize != 0 && f->propsSize != 4))
      return False;
  }
  return True;
}


SRes XzBlock_Parse(CXzBlock *p, const Byte *header)
{
  unsigned pos;
  unsigned numFilters, i;
  unsigned headerSize = (unsigned)header[0] << 2;

  /* (headerSize != 0) : another code checks */

  if (CrcCalc(header, headerSize) != GetUi32(header + headerSize))
    return SZ_ERROR_ARCHIVE;

  pos = 1;
  p->flags = header[pos++];

  p->packSize = (UInt64)(Int64)-1;
  if (XzBlock_HasPackSize(p))
  {
    READ_VARINT_AND_CHECK(header, pos, headerSize, &p->packSize);
    if (p->packSize == 0 || p->packSize + headerSize >= (UInt64)1 << 63)
      return SZ_ERROR_ARCHIVE;
  }

  p->unpackSize = (UInt64)(Int64)-1;
  if (XzBlock_HasUnpackSize(p))
    READ_VARINT_AND_CHECK(header, pos, headerSize, &p->unpackSize);

  numFilters = XzBlock_GetNumFilters(p);
  for (i = 0; i < numFilters; i++)
  {
    CXzFilter *filter = p->filters + i;
    UInt64 size;
    READ_VARINT_AND_CHECK(header, pos, headerSize, &filter->id);
    READ_VARINT_AND_CHECK(header, pos, headerSize, &size);
    if (size > headerSize - pos || size > XZ_FILTER_PROPS_SIZE_MAX)
      return SZ_ERROR_ARCHIVE;
    filter->propsSize = (UInt32)size;
    memcpy(filter->props, header + pos, (size_t)size);
    pos += (unsigned)size;

    #ifdef XZ_DUMP
    printf("\nf[%u] = %2X: ", i, (unsigned)filter->id);
    {
      unsigned i;
      for (i = 0; i < size; i++)
        printf(" %2X", filter->props[i]);
    }
    #endif
  }

  if (XzBlock_HasUnsupportedFlags(p))
    return SZ_ERROR_UNSUPPORTED;

  while (pos < headerSize)
    if (header[pos++] != 0)
      return SZ_ERROR_ARCHIVE;
  return SZ_OK;
}




static SRes XzDecMix_Init(CMixCoder *p, const CXzBlock *block, Byte *outBuf, size_t outBufSize)
{
  unsigned i;
  BoolInt needReInit = True;
  unsigned numFilters = XzBlock_GetNumFilters(block);

  if (numFilters == p->numCoders && ((p->outBuf && outBuf) || (!p->outBuf && !outBuf)))
  {
    needReInit = False;
    for (i = 0; i < numFilters; i++)
      if (p->ids[i] != block->filters[numFilters - 1 - i].id)
      {
        needReInit = True;
        break;
      }
  }

  // p->SingleBufMode = (outBuf != NULL);
  p->outBuf = outBuf;
  p->outBufSize = outBufSize;

  // p->SingleBufMode = False;
  // outBuf = NULL;
  
  if (needReInit)
  {
    MixCoder_Free(p);
    for (i = 0; i < numFilters; i++)
    {
      RINOK(MixCoder_SetFromMethod(p, i, block->filters[numFilters - 1 - i].id, outBuf, outBufSize));
    }
    p->numCoders = numFilters;
  }
  else
  {
    RINOK(MixCoder_ResetFromMethod(p, 0, block->filters[numFilters - 1].id, outBuf, outBufSize));
  }

  for (i = 0; i < numFilters; i++)
  {
    const CXzFilter *f = &block->filters[numFilters - 1 - i];
    IStateCoder *sc = &p->coders[i];
    RINOK(sc->SetProps(sc->p, f->props, f->propsSize, p->alloc));
  }
  
  MixCoder_Init(p);
  return SZ_OK;
}



void XzUnpacker_Init(CXzUnpacker *p)
{
  p->state = XZ_STATE_STREAM_HEADER;
  p->pos = 0;
  p->numStartedStreams = 0;
  p->numFinishedStreams = 0;
  p->numTotalBlocks = 0;
  p->padSize = 0;
  p->decodeOnlyOneBlock = 0;

  p->parseMode = False;
  p->decodeToStreamSignature = False;

  // p->outBuf = NULL;
  // p->outBufSize = 0;
  p->outDataWritten = 0;
}


void XzUnpacker_SetOutBuf(CXzUnpacker *p, Byte *outBuf, size_t outBufSize)
{
  p->outBuf = outBuf;
  p->outBufSize = outBufSize;
}


void XzUnpacker_Construct(CXzUnpacker *p, ISzAllocPtr alloc)
{
  MixCoder_Construct(&p->decoder, alloc);
  p->outBuf = NULL;
  p->outBufSize = 0;
  XzUnpacker_Init(p);
}


void XzUnpacker_Free(CXzUnpacker *p)
{
  MixCoder_Free(&p->decoder);
}


void XzUnpacker_PrepareToRandomBlockDecoding(CXzUnpacker *p)
{
  p->indexSize = 0;
  p->numBlocks = 0;
  Sha256_Init(&p->sha);
  p->state = XZ_STATE_BLOCK_HEADER;
  p->pos = 0;
  p->decodeOnlyOneBlock = 1;
}


static void XzUnpacker_UpdateIndex(CXzUnpacker *p, UInt64 packSize, UInt64 unpackSize)
{
  Byte temp[32];
  unsigned num = Xz_WriteVarInt(temp, packSize);
  num += Xz_WriteVarInt(temp + num, unpackSize);
  Sha256_Update(&p->sha, temp, num);
  p->indexSize += num;
  p->numBlocks++;
}



SRes XzUnpacker_Code(CXzUnpacker *p, Byte *dest, SizeT *destLen,
    const Byte *src, SizeT *srcLen, int srcFinished,
    ECoderFinishMode finishMode, ECoderStatus *status)
{
  SizeT destLenOrig = *destLen;
  SizeT srcLenOrig = *srcLen;
  *destLen = 0;
  *srcLen = 0;
  *status = CODER_STATUS_NOT_SPECIFIED;

  for (;;)
  {
    SizeT srcRem;

    if (p->state == XZ_STATE_BLOCK)
    {
      SizeT destLen2 = destLenOrig - *destLen;
      SizeT srcLen2 = srcLenOrig - *srcLen;
      SRes res;

      ECoderFinishMode finishMode2 = finishMode;
      BoolInt srcFinished2 = srcFinished;
      BoolInt destFinish = False;

      if (p->block.packSize != (UInt64)(Int64)-1)
      {
        UInt64 rem = p->block.packSize - p->packSize;
        if (srcLen2 >= rem)
        {
          srcFinished2 = True;
          srcLen2 = (SizeT)rem;
        }
        if (rem == 0 && p->block.unpackSize == p->unpackSize)
          return SZ_ERROR_DATA;
      }

      if (p->block.unpackSize != (UInt64)(Int64)-1)
      {
        UInt64 rem = p->block.unpackSize - p->unpackSize;
        if (destLen2 >= rem)
        {
          destFinish = True;
          finishMode2 = CODER_FINISH_END;
          destLen2 = (SizeT)rem;
        }
      }

      /*
      if (srcLen2 == 0 && destLen2 == 0)
      {
        *status = CODER_STATUS_NOT_FINISHED;
        return SZ_OK;
      }
      */
      
      {
        res = MixCoder_Code(&p->decoder,
            (p->outBuf ? NULL : dest), &destLen2, destFinish,
            src, &srcLen2, srcFinished2,
            finishMode2);

        *status = p->decoder.status;
        XzCheck_Update(&p->check, (p->outBuf ? p->outBuf + p->outDataWritten : dest), destLen2);
        if (!p->outBuf)
          dest += destLen2;
        p->outDataWritten += destLen2;
      }
      
      (*srcLen) += srcLen2;
      src += srcLen2;
      p->packSize += srcLen2;
      (*destLen) += destLen2;
      p->unpackSize += destLen2;

      RINOK(res);

      if (*status != CODER_STATUS_FINISHED_WITH_MARK)
      {
        if (p->block.packSize == p->packSize
            && *status == CODER_STATUS_NEEDS_MORE_INPUT)
        {
          PRF_STR("CODER_STATUS_NEEDS_MORE_INPUT");
          *status = CODER_STATUS_NOT_SPECIFIED;
          return SZ_ERROR_DATA;
        }
        
        return SZ_OK;
      }
      {
        XzUnpacker_UpdateIndex(p, XzUnpacker_GetPackSizeForIndex(p), p->unpackSize);
        p->state = XZ_STATE_BLOCK_FOOTER;
        p->pos = 0;
        p->alignPos = 0;
        *status = CODER_STATUS_NOT_SPECIFIED;

        if ((p->block.packSize != (UInt64)(Int64)-1 && p->block.packSize != p->packSize)
           || (p->block.unpackSize != (UInt64)(Int64)-1 && p->block.unpackSize != p->unpackSize))
        {
          PRF_STR("ERROR: block.size mismatch");
          return SZ_ERROR_DATA;
        }
      }
      // continue;
    }

    srcRem = srcLenOrig - *srcLen;

    // XZ_STATE_BLOCK_FOOTER can transit to XZ_STATE_BLOCK_HEADER without input bytes
    if (srcRem == 0 && p->state != XZ_STATE_BLOCK_FOOTER)
    {
      *status = CODER_STATUS_NEEDS_MORE_INPUT;
      return SZ_OK;
    }

    switch (p->state)
    {
      case XZ_STATE_STREAM_HEADER:
      {
        if (p->pos < XZ_STREAM_HEADER_SIZE)
        {
          if (p->pos < XZ_SIG_SIZE && *src != XZ_SIG[p->pos])
            return SZ_ERROR_NO_ARCHIVE;
          if (p->decodeToStreamSignature)
            return SZ_OK;
          p->buf[p->pos++] = *src++;
          (*srcLen)++;
        }
        else
        {
          RINOK(Xz_ParseHeader(&p->streamFlags, p->buf));
          p->numStartedStreams++;
          p->indexSize = 0;
          p->numBlocks = 0;
          Sha256_Init(&p->sha);
          p->state = XZ_STATE_BLOCK_HEADER;
          p->pos = 0;
        }
        break;
      }

      case XZ_STATE_BLOCK_HEADER:
      {
        if (p->pos == 0)
        {
          p->buf[p->pos++] = *src++;
          (*srcLen)++;
          if (p->buf[0] == 0)
          {
            if (p->decodeOnlyOneBlock)
              return SZ_ERROR_DATA;
            p->indexPreSize = 1 + Xz_WriteVarInt(p->buf + 1, p->numBlocks);
            p->indexPos = p->indexPreSize;
            p->indexSize += p->indexPreSize;
            Sha256_Final(&p->sha, p->shaDigest);
            Sha256_Init(&p->sha);
            p->crc = CrcUpdate(CRC_INIT_VAL, p->buf, p->indexPreSize);
            p->state = XZ_STATE_STREAM_INDEX;
            break;
          }
          p->blockHeaderSize = ((UInt32)p->buf[0] << 2) + 4;
          break;
        }
        
        if (p->pos != p->blockHeaderSize)
        {
          UInt32 cur = p->blockHeaderSize - p->pos;
          if (cur > srcRem)
            cur = (UInt32)srcRem;
          memcpy(p->buf + p->pos, src, cur);
          p->pos += cur;
          (*srcLen) += cur;
          src += cur;
        }
        else
        {
          RINOK(XzBlock_Parse(&p->block, p->buf));
          if (!XzBlock_AreSupportedFilters(&p->block))
            return SZ_ERROR_UNSUPPORTED;
          p->numTotalBlocks++;
          p->state = XZ_STATE_BLOCK;
          p->packSize = 0;
          p->unpackSize = 0;
          XzCheck_Init(&p->check, XzFlags_GetCheckType(p->streamFlags));
          if (p->parseMode)
          {
            p->headerParsedOk = True;
            return SZ_OK;
          }
          RINOK(XzDecMix_Init(&p->decoder, &p->block, p->outBuf, p->outBufSize));
        }
        break;
      }

      case XZ_STATE_BLOCK_FOOTER:
      {
        if ((((unsigned)p->packSize + p->alignPos) & 3) != 0)
        {
          if (srcRem == 0)
          {
            *status = CODER_STATUS_NEEDS_MORE_INPUT;
            return SZ_OK;
          }
          (*srcLen)++;
          p->alignPos++;
          if (*src++ != 0)
            return SZ_ERROR_CRC;
        }
        else
        {
          UInt32 checkSize = XzFlags_GetCheckSize(p->streamFlags);
          UInt32 cur = checkSize - p->pos;
          if (cur != 0)
          {
            if (srcRem == 0)
            {
              *status = CODER_STATUS_NEEDS_MORE_INPUT;
              return SZ_OK;
            }
            if (cur > srcRem)
              cur = (UInt32)srcRem;
            memcpy(p->buf + p->pos, src, cur);
            p->pos += cur;
            (*srcLen) += cur;
            src += cur;
            if (checkSize != p->pos)
              break;
          }
          {
            Byte digest[XZ_CHECK_SIZE_MAX];
            p->state = XZ_STATE_BLOCK_HEADER;
            p->pos = 0;
            if (XzCheck_Final(&p->check, digest) && memcmp(digest, p->buf, checkSize) != 0)
              return SZ_ERROR_CRC;
            if (p->decodeOnlyOneBlock)
            {
              *status = CODER_STATUS_FINISHED_WITH_MARK;
              return SZ_OK;
            }
          }
        }
        break;
      }

      case XZ_STATE_STREAM_INDEX:
      {
        if (p->pos < p->indexPreSize)
        {
          (*srcLen)++;
          if (*src++ != p->buf[p->pos++])
            return SZ_ERROR_CRC;
        }
        else
        {
          if (p->indexPos < p->indexSize)
          {
            UInt64 cur = p->indexSize - p->indexPos;
            if (srcRem > cur)
              srcRem = (SizeT)cur;
            p->crc = CrcUpdate(p->crc, src, srcRem);
            Sha256_Update(&p->sha, src, srcRem);
            (*srcLen) += srcRem;
            src += srcRem;
            p->indexPos += srcRem;
          }
          else if ((p->indexPos & 3) != 0)
          {
            Byte b = *src++;
            p->crc = CRC_UPDATE_BYTE(p->crc, b);
            (*srcLen)++;
            p->indexPos++;
            p->indexSize++;
            if (b != 0)
              return SZ_ERROR_CRC;
          }
          else
          {
            Byte digest[SHA256_DIGEST_SIZE];
            p->state = XZ_STATE_STREAM_INDEX_CRC;
            p->indexSize += 4;
            p->pos = 0;
            Sha256_Final(&p->sha, digest);
            if (memcmp(digest, p->shaDigest, SHA256_DIGEST_SIZE) != 0)
              return SZ_ERROR_CRC;
          }
        }
        break;
      }

      case XZ_STATE_STREAM_INDEX_CRC:
      {
        if (p->pos < 4)
        {
          (*srcLen)++;
          p->buf[p->pos++] = *src++;
        }
        else
        {
          const Byte *ptr = p->buf;
          p->state = XZ_STATE_STREAM_FOOTER;
          p->pos = 0;
          if (CRC_GET_DIGEST(p->crc) != GetUi32(ptr))
            return SZ_ERROR_CRC;
        }
        break;
      }

      case XZ_STATE_STREAM_FOOTER:
      {
        UInt32 cur = XZ_STREAM_FOOTER_SIZE - p->pos;
        if (cur > srcRem)
          cur = (UInt32)srcRem;
        memcpy(p->buf + p->pos, src, cur);
        p->pos += cur;
        (*srcLen) += cur;
        src += cur;
        if (p->pos == XZ_STREAM_FOOTER_SIZE)
        {
          p->state = XZ_STATE_STREAM_PADDING;
          p->numFinishedStreams++;
          p->padSize = 0;
          if (!Xz_CheckFooter(p->streamFlags, p->indexSize, p->buf))
            return SZ_ERROR_CRC;
        }
        break;
      }

      case XZ_STATE_STREAM_PADDING:
      {
        if (*src != 0)
        {
          if (((UInt32)p->padSize & 3) != 0)
            return SZ_ERROR_NO_ARCHIVE;
          p->pos = 0;
          p->state = XZ_STATE_STREAM_HEADER;
        }
        else
        {
          (*srcLen)++;
          src++;
          p->padSize++;
        }
        break;
      }
      
      case XZ_STATE_BLOCK: break; /* to disable GCC warning */
    }
  }
  /*
  if (p->state == XZ_STATE_FINISHED)
    *status = CODER_STATUS_FINISHED_WITH_MARK;
  return SZ_OK;
  */
}


SRes XzUnpacker_CodeFull(CXzUnpacker *p, Byte *dest, SizeT *destLen,
    const Byte *src, SizeT *srcLen,
    ECoderFinishMode finishMode, ECoderStatus *status)
{
  XzUnpacker_Init(p);
  XzUnpacker_SetOutBuf(p, dest, *destLen);

  return XzUnpacker_Code(p,
      NULL, destLen,
      src, srcLen, True,
      finishMode, status);
}


BoolInt XzUnpacker_IsBlockFinished(const CXzUnpacker *p)
{
  return (p->state == XZ_STATE_BLOCK_HEADER) && (p->pos == 0);
}

BoolInt XzUnpacker_IsStreamWasFinished(const CXzUnpacker *p)
{
  return (p->state == XZ_STATE_STREAM_PADDING) && (((UInt32)p->padSize & 3) == 0);
}

UInt64 XzUnpacker_GetExtraSize(const CXzUnpacker *p)
{
  UInt64 num = 0;
  if (p->state == XZ_STATE_STREAM_PADDING)
    num = p->padSize;
  else if (p->state == XZ_STATE_STREAM_HEADER)
    num = p->padSize + p->pos;
  return num;
}





















#ifndef _7ZIP_ST
#include "MtDec.h"
#endif


void XzDecMtProps_Init(CXzDecMtProps *p)
{
  p->inBufSize_ST = 1 << 18;
  p->outStep_ST = 1 << 20;
  p->ignoreErrors = False;

  #ifndef _7ZIP_ST
  p->numThreads = 1;
  p->inBufSize_MT = 1 << 18;
  p->memUseMax = sizeof(size_t) << 28;
  #endif
}



#ifndef _7ZIP_ST

/* ---------- CXzDecMtThread ---------- */

typedef struct
{
  Byte *outBuf;
  size_t outBufSize;
  size_t outPreSize;
  size_t inPreSize;
  size_t inPreHeaderSize;
  size_t blockPackSize_for_Index;  // including block header and checksum.
  size_t blockPackTotal;  // including stream header, block header and checksum.
  size_t inCodeSize;
  size_t outCodeSize;
  ECoderStatus status;
  SRes codeRes;
  BoolInt skipMode;
  // BoolInt finishedWithMark;
  EMtDecParseState parseState;
  BoolInt parsing_Truncated;
  BoolInt atBlockHeader;
  CXzStreamFlags streamFlags;
  // UInt64 numFinishedStreams
  UInt64 numStreams;
  UInt64 numTotalBlocks;
  UInt64 numBlocks;

  BoolInt dec_created;
  CXzUnpacker dec;

  Byte mtPad[1 << 7];
} CXzDecMtThread;

#endif


/* ---------- CXzDecMt ---------- */

typedef struct
{
  CAlignOffsetAlloc alignOffsetAlloc;
  ISzAllocPtr allocMid;

  CXzDecMtProps props;
  size_t unpackBlockMaxSize;
  
  ISeqInStream *inStream;
  ISeqOutStream *outStream;
  ICompressProgress *progress;

  BoolInt finishMode;
  BoolInt outSize_Defined;
  UInt64 outSize;

  UInt64 outProcessed;
  UInt64 inProcessed;
  UInt64 readProcessed;
  BoolInt readWasFinished;
  SRes readRes;
  SRes writeRes;

  Byte *outBuf;
  size_t outBufSize;
  Byte *inBuf;
  size_t inBufSize;

  CXzUnpacker dec;

  ECoderStatus status;
  SRes codeRes;

  #ifndef _7ZIP_ST
  BoolInt mainDecoderWasCalled;
  // int statErrorDefined;
  int finishedDecoderIndex;

  // global values that are used in Parse stage
  CXzStreamFlags streamFlags;
  // UInt64 numFinishedStreams
  UInt64 numStreams;
  UInt64 numTotalBlocks;
  UInt64 numBlocks;

  // UInt64 numBadBlocks;
  SRes mainErrorCode;  // it's set to error code, if the size Code() output doesn't patch the size from Parsing stage
                       // it can be = SZ_ERROR_INPUT_EOF
                       // it can be = SZ_ERROR_DATA, in some another cases
  BoolInt isBlockHeaderState_Parse;
  BoolInt isBlockHeaderState_Write;
  UInt64 outProcessed_Parse;
  BoolInt parsing_Truncated;

  BoolInt mtc_WasConstructed;
  CMtDec mtc;
  CXzDecMtThread coders[MTDEC__THREADS_MAX];
  #endif

} CXzDecMt;



CXzDecMtHandle XzDecMt_Create(ISzAllocPtr alloc, ISzAllocPtr allocMid)
{
  CXzDecMt *p = (CXzDecMt *)ISzAlloc_Alloc(alloc, sizeof(CXzDecMt));
  if (!p)
    return NULL;
  
  AlignOffsetAlloc_CreateVTable(&p->alignOffsetAlloc);
  p->alignOffsetAlloc.baseAlloc = alloc;
  p->alignOffsetAlloc.numAlignBits = 7;
  p->alignOffsetAlloc.offset = 0;

  p->allocMid = allocMid;

  p->outBuf = NULL;
  p->outBufSize = 0;
  p->inBuf = NULL;
  p->inBufSize = 0;

  XzUnpacker_Construct(&p->dec, &p->alignOffsetAlloc.vt);

  p->unpackBlockMaxSize = 0;

  XzDecMtProps_Init(&p->props);

  #ifndef _7ZIP_ST
  p->mtc_WasConstructed = False;
  {
    unsigned i;
    for (i = 0; i < MTDEC__THREADS_MAX; i++)
    {
      CXzDecMtThread *coder = &p->coders[i];
      coder->dec_created = False;
      coder->outBuf = NULL;
      coder->outBufSize = 0;
    }
  }
  #endif

  return p;
}


#ifndef _7ZIP_ST

static void XzDecMt_FreeOutBufs(CXzDecMt *p)
{
  unsigned i;
  for (i = 0; i < MTDEC__THREADS_MAX; i++)
  {
    CXzDecMtThread *coder = &p->coders[i];
    if (coder->outBuf)
    {
      ISzAlloc_Free(p->allocMid, coder->outBuf);
      coder->outBuf = NULL;
      coder->outBufSize = 0;
    }
  }
  p->unpackBlockMaxSize = 0;
}

#endif



static void XzDecMt_FreeSt(CXzDecMt *p)
{
  XzUnpacker_Free(&p->dec);
  
  if (p->outBuf)
  {
    ISzAlloc_Free(p->allocMid, p->outBuf);
    p->outBuf = NULL;
  }
  p->outBufSize = 0;
  
  if (p->inBuf)
  {
    ISzAlloc_Free(p->allocMid, p->inBuf);
    p->inBuf = NULL;
  }
  p->inBufSize = 0;
}


void XzDecMt_Destroy(CXzDecMtHandle pp)
{
  CXzDecMt *p = (CXzDecMt *)pp;

  XzDecMt_FreeSt(p);

  #ifndef _7ZIP_ST

  if (p->mtc_WasConstructed)
  {
    MtDec_Destruct(&p->mtc);
    p->mtc_WasConstructed = False;
  }
  {
    unsigned i;
    for (i = 0; i < MTDEC__THREADS_MAX; i++)
    {
      CXzDecMtThread *t = &p->coders[i];
      if (t->dec_created)
      {
        // we don't need to free dict here
        XzUnpacker_Free(&t->dec);
        t->dec_created = False;
      }
    }
  }
  XzDecMt_FreeOutBufs(p);

  #endif

  ISzAlloc_Free(p->alignOffsetAlloc.baseAlloc, pp);
}



#ifndef _7ZIP_ST

static void XzDecMt_Callback_Parse(void *obj, unsigned coderIndex, CMtDecCallbackInfo *cc)
{
  CXzDecMt *me = (CXzDecMt *)obj;
  CXzDecMtThread *coder = &me->coders[coderIndex];
  size_t srcSize = cc->srcSize;

  cc->srcSize = 0;
  cc->outPos = 0;
  cc->state = MTDEC_PARSE_CONTINUE;

  cc->canCreateNewThread = True;

  if (cc->startCall)
  {
    coder->outPreSize = 0;
    coder->inPreSize = 0;
    coder->inPreHeaderSize = 0;
    coder->parseState = MTDEC_PARSE_CONTINUE;
    coder->parsing_Truncated = False;
    coder->skipMode = False;
    coder->codeRes = SZ_OK;
    coder->status = CODER_STATUS_NOT_SPECIFIED;
    coder->inCodeSize = 0;
    coder->outCodeSize = 0;

    coder->numStreams = me->numStreams;
    coder->numTotalBlocks = me->numTotalBlocks;
    coder->numBlocks = me->numBlocks;

    if (!coder->dec_created)
    {
      XzUnpacker_Construct(&coder->dec, &me->alignOffsetAlloc.vt);
      coder->dec_created = True;
    }
    
    XzUnpacker_Init(&coder->dec);

    if (me->isBlockHeaderState_Parse)
    {
      coder->dec.streamFlags = me->streamFlags;
      coder->atBlockHeader = True;
      XzUnpacker_PrepareToRandomBlockDecoding(&coder->dec);
    }
    else
    {
      coder->atBlockHeader = False;
      me->isBlockHeaderState_Parse = True;
    }

    coder->dec.numStartedStreams = me->numStreams;
    coder->dec.numTotalBlocks = me->numTotalBlocks;
    coder->dec.numBlocks = me->numBlocks;
  }

  while (!coder->skipMode)
  {
    ECoderStatus status;
    SRes res;
    size_t srcSize2 = srcSize;
    size_t destSize = (size_t)0 - 1;

    coder->dec.parseMode = True;
    coder->dec.headerParsedOk = False;
    
    PRF_STR_INT("Parse", srcSize2);
    
    res = XzUnpacker_Code(&coder->dec,
        NULL, &destSize,
        cc->src, &srcSize2, cc->srcFinished,
        CODER_FINISH_END, &status);
    
    // PRF(printf(" res = %d, srcSize2 = %d", res, (unsigned)srcSize2));
    
    coder->codeRes = res;
    coder->status = status;
    cc->srcSize += srcSize2;
    srcSize -= srcSize2;
    coder->inPreHeaderSize += srcSize2;
    coder->inPreSize = coder->inPreHeaderSize;
    
    if (res != SZ_OK)
    {
      cc->state =
      coder->parseState = MTDEC_PARSE_END;
      /*
      if (res == SZ_ERROR_MEM)
        return res;
      return SZ_OK;
      */
      return; // res;
    }
    
    if (coder->dec.headerParsedOk)
    {
      const CXzBlock *block = &coder->dec.block;
      if (XzBlock_HasUnpackSize(block)
          // && block->unpackSize <= me->props.outBlockMax
          && XzBlock_HasPackSize(block))
      {
        {
          if (block->unpackSize * 2 * me->mtc.numStartedThreads > me->props.memUseMax)
          {
            cc->state = MTDEC_PARSE_OVERFLOW;
            return; // SZ_OK;
          }
        }
        {
        UInt64 packSize = block->packSize;
        UInt64 packSizeAligned = packSize + ((0 - (unsigned)packSize) & 3);
        UInt32 checkSize = XzFlags_GetCheckSize(coder->dec.streamFlags);
        UInt64 blockPackSum = coder->inPreSize + packSizeAligned + checkSize;
        // if (blockPackSum <= me->props.inBlockMax)
        // unpackBlockMaxSize
        {
          coder->blockPackSize_for_Index = (size_t)(coder->dec.blockHeaderSize + packSize + checkSize);
          coder->blockPackTotal = (size_t)blockPackSum;
          coder->outPreSize = (size_t)block->unpackSize;
          coder->streamFlags = coder->dec.streamFlags;
          me->streamFlags = coder->dec.streamFlags;
          coder->skipMode = True;
          break;
        }
        }
      }
    }
    else
    // if (coder->inPreSize <= me->props.inBlockMax)
    {
      if (!cc->srcFinished)
        return; // SZ_OK;
      cc->state =
      coder->parseState = MTDEC_PARSE_END;
      return; // SZ_OK;
    }
    cc->state = MTDEC_PARSE_OVERFLOW;
    return; // SZ_OK;
  }

  // ---------- skipMode ----------
  {
    UInt64 rem = coder->blockPackTotal - coder->inPreSize;
    size_t cur = srcSize;
    if (cur > rem)
      cur = (size_t)rem;
    cc->srcSize += cur;
    coder->inPreSize += cur;
    srcSize -= cur;

    if (coder->inPreSize == coder->blockPackTotal)
    {
      if (srcSize == 0)
      {
        if (!cc->srcFinished)
          return; // SZ_OK;
        cc->state = MTDEC_PARSE_END;
      }
      else if ((cc->src)[cc->srcSize] == 0) // we check control byte of next block
        cc->state = MTDEC_PARSE_END;
      else
      {
        cc->state = MTDEC_PARSE_NEW;

        {
          size_t blockMax = me->unpackBlockMaxSize;
          if (blockMax < coder->outPreSize)
            blockMax = coder->outPreSize;
          {
            UInt64 required = (UInt64)blockMax * (me->mtc.numStartedThreads + 1) * 2;
            if (me->props.memUseMax < required)
              cc->canCreateNewThread = False;
          }
        }

        if (me->outSize_Defined)
        {
          // next block can be zero size
          const UInt64 rem2 = me->outSize - me->outProcessed_Parse;
          if (rem2 < coder->outPreSize)
          {
            coder->parsing_Truncated = True;
            cc->state = MTDEC_PARSE_END;
          }
          me->outProcessed_Parse += coder->outPreSize;
        }
      }
    }
    else if (cc->srcFinished)
      cc->state = MTDEC_PARSE_END;
    else
      return; // SZ_OK;

    coder->parseState = cc->state;
    cc->outPos = coder->outPreSize;
    
    me->numStreams = coder->dec.numStartedStreams;
    me->numTotalBlocks = coder->dec.numTotalBlocks;
    me->numBlocks = coder->dec.numBlocks + 1;
    return; // SZ_OK;
  }
}


static SRes XzDecMt_Callback_PreCode(void *pp, unsigned coderIndex)
{
  CXzDecMt *me = (CXzDecMt *)pp;
  CXzDecMtThread *coder = &me->coders[coderIndex];
  Byte *dest;

  if (!coder->dec.headerParsedOk)
    return SZ_OK;

  dest = coder->outBuf;

  if (!dest || coder->outBufSize < coder->outPreSize)
  {
    if (dest)
    {
      ISzAlloc_Free(me->allocMid, dest);
      coder->outBuf = NULL;
      coder->outBufSize = 0;
    }
    {
      size_t outPreSize = coder->outPreSize;
      if (outPreSize == 0)
        outPreSize = 1;
      dest = (Byte *)ISzAlloc_Alloc(me->allocMid, outPreSize);
    }
    if (!dest)
      return SZ_ERROR_MEM;
    coder->outBuf = dest;
    coder->outBufSize = coder->outPreSize;

    if (coder->outBufSize > me->unpackBlockMaxSize)
      me->unpackBlockMaxSize = coder->outBufSize;
  }

  // return SZ_ERROR_MEM;

  XzUnpacker_SetOutBuf(&coder->dec, coder->outBuf, coder->outBufSize);

  {
    SRes res = XzDecMix_Init(&coder->dec.decoder, &coder->dec.block, coder->outBuf, coder->outBufSize);
    // res = SZ_ERROR_UNSUPPORTED; // to test
    coder->codeRes = res;
    if (res != SZ_OK)
    {
      // if (res == SZ_ERROR_MEM) return res;
      if (me->props.ignoreErrors && res != SZ_ERROR_MEM)
        return SZ_OK;
      return res;
    }
  }

  return SZ_OK;
}


static SRes XzDecMt_Callback_Code(void *pp, unsigned coderIndex,
    const Byte *src, size_t srcSize, int srcFinished,
    // int finished, int blockFinished,
    UInt64 *inCodePos, UInt64 *outCodePos, int *stop)
{
  CXzDecMt *me = (CXzDecMt *)pp;
  CXzDecMtThread *coder = &me->coders[coderIndex];

  *inCodePos = coder->inCodeSize;
  *outCodePos = coder->outCodeSize;
  *stop = True;

  if (srcSize > coder->inPreSize - coder->inCodeSize)
    return SZ_ERROR_FAIL;
  
  if (coder->inCodeSize < coder->inPreHeaderSize)
  {
    size_t step = coder->inPreHeaderSize - coder->inCodeSize;
    if (step > srcSize)
      step = srcSize;
    src += step;
    srcSize -= step;
    coder->inCodeSize += step;
    *inCodePos = coder->inCodeSize;
    if (coder->inCodeSize < coder->inPreHeaderSize)
    {
      *stop = False;
      return SZ_OK;
    }
  }

  if (!coder->dec.headerParsedOk)
    return SZ_OK;
  if (!coder->outBuf)
    return SZ_OK;

  if (coder->codeRes == SZ_OK)
  {
    ECoderStatus status;
    SRes res;
    size_t srcProcessed = srcSize;
    size_t outSizeCur = coder->outPreSize - coder->dec.outDataWritten;

    // PRF(printf("\nCallback_Code: Code %d %d\n", (unsigned)srcSize, (unsigned)outSizeCur));

    res = XzUnpacker_Code(&coder->dec,
        NULL, &outSizeCur,
        src, &srcProcessed, srcFinished,
        // coder->finishedWithMark ? CODER_FINISH_END : CODER_FINISH_ANY,
        CODER_FINISH_END,
        &status);

    // PRF(printf(" res = %d, srcSize2 = %d, outSizeCur = %d", res, (unsigned)srcProcessed, (unsigned)outSizeCur));

    coder->codeRes = res;
    coder->status = status;
    coder->inCodeSize += srcProcessed;
    coder->outCodeSize = coder->dec.outDataWritten;
    *inCodePos = coder->inCodeSize;
    *outCodePos = coder->outCodeSize;

    if (res == SZ_OK)
    {
      if (srcProcessed == srcSize)
        *stop = False;
      return SZ_OK;
    }
  }

  if (me->props.ignoreErrors && coder->codeRes != SZ_ERROR_MEM)
  {
    *inCodePos = coder->inPreSize;
    *outCodePos = coder->outPreSize;
    return SZ_OK;
  }
  return coder->codeRes;
}


#define XZDECMT_STREAM_WRITE_STEP (1 << 24)

static SRes XzDecMt_Callback_Write(void *pp, unsigned coderIndex,
    BoolInt needWriteToStream,
    const Byte *src, size_t srcSize, BoolInt isCross,
    // int srcFinished,
    BoolInt *needContinue,
    BoolInt *canRecode)
{
  CXzDecMt *me = (CXzDecMt *)pp;
  const CXzDecMtThread *coder = &me->coders[coderIndex];

  // PRF(printf("\nWrite processed = %d srcSize = %d\n", (unsigned)me->mtc.inProcessed, (unsigned)srcSize));
  
  *needContinue = False;
  *canRecode = True;
  
  if (!needWriteToStream)
    return SZ_OK;

  if (!coder->dec.headerParsedOk || !coder->outBuf)
  {
    if (me->finishedDecoderIndex < 0)
      me->finishedDecoderIndex = (int)coderIndex;
    return SZ_OK;
  }

  if (me->finishedDecoderIndex >= 0)
    return SZ_OK;

  me->mtc.inProcessed += coder->inCodeSize;

  *canRecode = False;

  {
    SRes res;
    size_t size = coder->outCodeSize;
    Byte *data = coder->outBuf;
    
    // we use in me->dec: sha, numBlocks, indexSize

    if (!me->isBlockHeaderState_Write)
    {
      XzUnpacker_PrepareToRandomBlockDecoding(&me->dec);
      me->dec.decodeOnlyOneBlock = False;
      me->dec.numStartedStreams = coder->dec.numStartedStreams;
      me->dec.streamFlags = coder->streamFlags;

      me->isBlockHeaderState_Write = True;
    }
    
    me->dec.numTotalBlocks = coder->dec.numTotalBlocks;
    XzUnpacker_UpdateIndex(&me->dec, coder->blockPackSize_for_Index, coder->outPreSize);
    
    if (coder->outPreSize != size)
    {
      if (me->props.ignoreErrors)
      {
        memset(data + size, 0, coder->outPreSize - size);
        size = coder->outPreSize;
      }
      // me->numBadBlocks++;
      if (me->mainErrorCode == SZ_OK)
      {
        if ((int)coder->status == LZMA_STATUS_NEEDS_MORE_INPUT)
          me->mainErrorCode = SZ_ERROR_INPUT_EOF;
        else
          me->mainErrorCode = SZ_ERROR_DATA;
      }
    }
    
    if (me->writeRes != SZ_OK)
      return me->writeRes;

    res = SZ_OK;
    {
      if (me->outSize_Defined)
      {
        const UInt64 rem = me->outSize - me->outProcessed;
        if (size > rem)
          size = (SizeT)rem;
      }

      for (;;)
      {
        size_t cur = size;
        size_t written;
        if (cur > XZDECMT_STREAM_WRITE_STEP)
          cur = XZDECMT_STREAM_WRITE_STEP;

        written = ISeqOutStream_Write(me->outStream, data, cur);

        // PRF(printf("\nWritten ask = %d written = %d\n", (unsigned)cur, (unsigned)written));
        
        me->outProcessed += written;
        if (written != cur)
        {
          me->writeRes = SZ_ERROR_WRITE;
          res = me->writeRes;
          break;
        }
        data += cur;
        size -= cur;
        // PRF_STR_INT("Written size =", size);
        if (size == 0)
          break;
        res = MtProgress_ProgressAdd(&me->mtc.mtProgress, 0, 0);
        if (res != SZ_OK)
          break;
      }
    }

    if (coder->codeRes != SZ_OK)
      if (!me->props.ignoreErrors)
      {
        me->finishedDecoderIndex = (int)coderIndex;
        return res;
      }

    RINOK(res);

    if (coder->inPreSize != coder->inCodeSize
        || coder->blockPackTotal != coder->inCodeSize)
    {
      me->finishedDecoderIndex = (int)coderIndex;
      return SZ_OK;
    }

    if (coder->parseState != MTDEC_PARSE_END)
    {
      *needContinue = True;
      return SZ_OK;
    }
  }

  // (coder->state == MTDEC_PARSE_END) means that there are no other working threads
  // so we can use mtc variables without lock

  PRF_STR_INT("Write MTDEC_PARSE_END", me->mtc.inProcessed);

  me->mtc.mtProgress.totalInSize = me->mtc.inProcessed;
  {
    CXzUnpacker *dec = &me->dec;
    
    PRF_STR_INT("PostSingle", srcSize);
    
    {
      size_t srcProcessed = srcSize;
      ECoderStatus status;
      size_t outSizeCur = 0;
      SRes res;
      
      // dec->decodeOnlyOneBlock = False;
      dec->decodeToStreamSignature = True;

      me->mainDecoderWasCalled = True;

      if (coder->parsing_Truncated)
      {
        me->parsing_Truncated = True;
        return SZ_OK;
      }
      
      /*
      We have processed all xz-blocks of stream,
      And xz unpacker is at XZ_STATE_BLOCK_HEADER state, where
      (src) is a pointer to xz-Index structure.
      We finish reading of current xz-Stream, including Zero padding after xz-Stream.
      We exit, if we reach extra byte (first byte of new-Stream or another data).
      But we don't update input stream pointer for that new extra byte.
      If extra byte is not correct first byte of xz-signature,
      we have SZ_ERROR_NO_ARCHIVE error here.
      */

      res = XzUnpacker_Code(dec,
          NULL, &outSizeCur,
          src, &srcProcessed,
          me->mtc.readWasFinished, // srcFinished
          CODER_FINISH_END, // CODER_FINISH_ANY,
          &status);

      // res = SZ_ERROR_ARCHIVE; // for failure test
      
      me->status = status;
      me->codeRes = res;

      if (isCross)
        me->mtc.crossStart += srcProcessed;

      me->mtc.inProcessed += srcProcessed;
      me->mtc.mtProgress.totalInSize = me->mtc.inProcessed;

      srcSize -= srcProcessed;
      src += srcProcessed;

      if (res != SZ_OK)
      {
        return SZ_OK;
        // return res;
      }
      
      if (dec->state == XZ_STATE_STREAM_HEADER)
      {
        *needContinue = True;
        me->isBlockHeaderState_Parse = False;
        me->isBlockHeaderState_Write = False;

        if (!isCross)
        {
          Byte *crossBuf = MtDec_GetCrossBuff(&me->mtc);
          if (!crossBuf)
            return SZ_ERROR_MEM;
          if (srcSize != 0)
            memcpy(crossBuf, src, srcSize);
          me->mtc.crossStart = 0;
          me->mtc.crossEnd = srcSize;
        }

        PRF_STR_INT("XZ_STATE_STREAM_HEADER crossEnd = ", (unsigned)me->mtc.crossEnd);

        return SZ_OK;
      }
      
      if (status != CODER_STATUS_NEEDS_MORE_INPUT || srcSize != 0)
      {
        return SZ_ERROR_FAIL;
      }
      
      if (me->mtc.readWasFinished)
      {
        return SZ_OK;
      }
    }
    
    {
      size_t inPos;
      size_t inLim;
      // const Byte *inData;
      UInt64 inProgressPrev = me->mtc.inProcessed;
      
      // XzDecMt_Prepare_InBuf_ST(p);
      Byte *crossBuf = MtDec_GetCrossBuff(&me->mtc);
      if (!crossBuf)
        return SZ_ERROR_MEM;
      
      inPos = 0;
      inLim = 0;
      
      // inData = crossBuf;
      
      for (;;)
      {
        SizeT inProcessed;
        SizeT outProcessed;
        ECoderStatus status;
        SRes res;
        
        if (inPos == inLim)
        {
          if (!me->mtc.readWasFinished)
          {
            inPos = 0;
            inLim = me->mtc.inBufSize;
            me->mtc.readRes = ISeqInStream_Read(me->inStream, (void *)crossBuf, &inLim);
            me->mtc.readProcessed += inLim;
            if (inLim == 0 || me->mtc.readRes != SZ_OK)
              me->mtc.readWasFinished = True;
          }
        }
        
        inProcessed = inLim - inPos;
        outProcessed = 0;

        res = XzUnpacker_Code(dec,
            NULL, &outProcessed,
            crossBuf + inPos, &inProcessed,
            (inProcessed == 0), // srcFinished
            CODER_FINISH_END, &status);
        
        me->codeRes = res;
        me->status = status;
        inPos += inProcessed;
        me->mtc.inProcessed += inProcessed;
        me->mtc.mtProgress.totalInSize = me->mtc.inProcessed;

        if (res != SZ_OK)
        {
          return SZ_OK;
          // return res;
        }

        if (dec->state == XZ_STATE_STREAM_HEADER)
        {
          *needContinue = True;
          me->mtc.crossStart = inPos;
          me->mtc.crossEnd = inLim;
          me->isBlockHeaderState_Parse = False;
          me->isBlockHeaderState_Write = False;
          return SZ_OK;
        }
        
        if (status != CODER_STATUS_NEEDS_MORE_INPUT)
          return SZ_ERROR_FAIL;
        
        if (me->mtc.progress)
        {
          UInt64 inDelta = me->mtc.inProcessed - inProgressPrev;
          if (inDelta >= (1 << 22))
          {
            RINOK(MtProgress_Progress_ST(&me->mtc.mtProgress));
            inProgressPrev = me->mtc.inProcessed;
          }
        }
        if (me->mtc.readWasFinished)
          return SZ_OK;
      }
    }
  }
}


#endif



void XzStatInfo_Clear(CXzStatInfo *p)
{
  p->InSize = 0;
  p->OutSize = 0;
  
  p->NumStreams = 0;
  p->NumBlocks = 0;
  
  p->UnpackSize_Defined = False;
  
  p->NumStreams_Defined = False;
  p->NumBlocks_Defined = False;
  
  p->DataAfterEnd = False;
  p->DecodingTruncated = False;
  
  p->DecodeRes = SZ_OK;
  p->ReadRes = SZ_OK;
  p->ProgressRes = SZ_OK;

  p->CombinedRes = SZ_OK;
  p->CombinedRes_Type = SZ_OK;
}



/*
  XzDecMt_Decode_ST() can return SZ_OK or the following errors
     - SZ_ERROR_MEM for memory allocation error
     - error from XzUnpacker_Code() function
     - SZ_ERROR_WRITE for ISeqOutStream::Write(). stat->CombinedRes_Type = SZ_ERROR_WRITE in that case
     - ICompressProgress::Progress() error,  stat->CombinedRes_Type = SZ_ERROR_PROGRESS.
  But XzDecMt_Decode_ST() doesn't return ISeqInStream::Read() errors.
  ISeqInStream::Read() result is set to p->readRes.
  also it can set stat->CombinedRes_Type to SZ_ERROR_WRITE or SZ_ERROR_PROGRESS.
*/

static SRes XzDecMt_Decode_ST(CXzDecMt *p
    #ifndef _7ZIP_ST
    , BoolInt tMode
    #endif
    , CXzStatInfo *stat)
{
  size_t outPos;
  size_t inPos, inLim;
  const Byte *inData;
  UInt64 inPrev, outPrev;

  CXzUnpacker *dec;

  #ifndef _7ZIP_ST
  if (tMode)
  {
    XzDecMt_FreeOutBufs(p);
    tMode = MtDec_PrepareRead(&p->mtc);
  }
  #endif

  if (!p->outBuf || p->outBufSize != p->props.outStep_ST)
  {
    ISzAlloc_Free(p->allocMid, p->outBuf);
    p->outBufSize = 0;
    p->outBuf = (Byte *)ISzAlloc_Alloc(p->allocMid, p->props.outStep_ST);
    if (!p->outBuf)
      return SZ_ERROR_MEM;
    p->outBufSize = p->props.outStep_ST;
  }

  if (!p->inBuf || p->inBufSize != p->props.inBufSize_ST)
  {
    ISzAlloc_Free(p->allocMid, p->inBuf);
    p->inBufSize = 0;
    p->inBuf = (Byte *)ISzAlloc_Alloc(p->allocMid, p->props.inBufSize_ST);
    if (!p->inBuf)
      return SZ_ERROR_MEM;
    p->inBufSize = p->props.inBufSize_ST;
  }

  dec = &p->dec;
  dec->decodeToStreamSignature = False;
  // dec->decodeOnlyOneBlock = False;

  XzUnpacker_SetOutBuf(dec, NULL, 0);

  inPrev = p->inProcessed;
  outPrev = p->outProcessed;

  inPos = 0;
  inLim = 0;
  inData = NULL;
  outPos = 0;

  for (;;)
  {
    SizeT outSize;
    BoolInt finished;
    ECoderFinishMode finishMode;
    SizeT inProcessed;
    ECoderStatus status;
    SRes res;

    SizeT outProcessed;



    if (inPos == inLim)
    {
      #ifndef _7ZIP_ST
      if (tMode)
      {
        inData = MtDec_Read(&p->mtc, &inLim);
        inPos = 0;
        if (inData)
          continue;
        tMode = False;
        inLim = 0;
      }
      #endif
      
      if (!p->readWasFinished)
      {
        inPos = 0;
        inLim = p->inBufSize;
        inData = p->inBuf;
        p->readRes = ISeqInStream_Read(p->inStream, (void *)p->inBuf, &inLim);
        p->readProcessed += inLim;
        if (inLim == 0 || p->readRes != SZ_OK)
          p->readWasFinished = True;
      }
    }

    outSize = p->props.outStep_ST - outPos;

    finishMode = CODER_FINISH_ANY;
    if (p->outSize_Defined)
    {
      const UInt64 rem = p->outSize - p->outProcessed;
      if (outSize >= rem)
      {
        outSize = (SizeT)rem;
        if (p->finishMode)
          finishMode = CODER_FINISH_END;
      }
    }

    inProcessed = inLim - inPos;
    outProcessed = outSize;

    res = XzUnpacker_Code(dec, p->outBuf + outPos, &outProcessed,
        inData + inPos, &inProcessed,
        (inPos == inLim), // srcFinished
        finishMode, &status);

    p->codeRes = res;
    p->status = status;

    inPos += inProcessed;
    outPos += outProcessed;
    p->inProcessed += inProcessed;
    p->outProcessed += outProcessed;

    finished = ((inProcessed == 0 && outProcessed == 0) || res != SZ_OK);

    if (finished || outProcessed >= outSize)
      if (outPos != 0)
      {
        const size_t written = ISeqOutStream_Write(p->outStream, p->outBuf, outPos);
        // p->outProcessed += written; // 21.01: BUG fixed
        if (written != outPos)
        {
          stat->CombinedRes_Type = SZ_ERROR_WRITE;
          return SZ_ERROR_WRITE;
        }
        outPos = 0;
      }

    if (p->progress && res == SZ_OK)
    {
      if (p->inProcessed - inPrev >= (1 << 22) ||
          p->outProcessed - outPrev >= (1 << 22))
      {
        res = ICompressProgress_Progress(p->progress, p->inProcessed, p->outProcessed);
        if (res != SZ_OK)
        {
          stat->CombinedRes_Type = SZ_ERROR_PROGRESS;
          stat->ProgressRes = res;
          return res;
        }
        inPrev = p->inProcessed;
        outPrev = p->outProcessed;
      }
    }

    if (finished)
    {
      // p->codeRes is preliminary error from XzUnpacker_Code.
      // and it can be corrected later as final result
      // so we return SZ_OK here instead of (res);
      return SZ_OK;
      // return res;
    }
  }
}



/*
XzStatInfo_SetStat() transforms
    CXzUnpacker return code and status to combined CXzStatInfo results.
    it can convert SZ_OK to SZ_ERROR_INPUT_EOF
    it can convert SZ_ERROR_NO_ARCHIVE to SZ_OK and (DataAfterEnd = 1)
*/

static void XzStatInfo_SetStat(const CXzUnpacker *dec,
    int finishMode,
    // UInt64 readProcessed,
    UInt64 inProcessed,
    SRes res,                     // it's result from CXzUnpacker unpacker
    ECoderStatus status,
    BoolInt decodingTruncated,
    CXzStatInfo *stat)
{
  UInt64 extraSize;
  
  stat->DecodingTruncated = (Byte)(decodingTruncated ? 1 : 0);
  stat->InSize = inProcessed;
  stat->NumStreams = dec->numStartedStreams;
  stat->NumBlocks = dec->numTotalBlocks;
  
  stat->UnpackSize_Defined = True;
  stat->NumStreams_Defined = True;
  stat->NumBlocks_Defined = True;
  
  extraSize = XzUnpacker_GetExtraSize(dec);
  
  if (res == SZ_OK)
  {
    if (status == CODER_STATUS_NEEDS_MORE_INPUT)
    {
      // CODER_STATUS_NEEDS_MORE_INPUT is expected status for correct xz streams
      // any extra data is part of correct data
      extraSize = 0;
      // if xz stream was not finished, then we need more data
      if (!XzUnpacker_IsStreamWasFinished(dec))
        res = SZ_ERROR_INPUT_EOF;
    }
    else
    {
      // CODER_STATUS_FINISHED_WITH_MARK is not possible for multi stream xz decoding
      // so he we have (status == CODER_STATUS_NOT_FINISHED)
      // if (status != CODER_STATUS_FINISHED_WITH_MARK)
      if (!decodingTruncated || finishMode)
        res = SZ_ERROR_DATA;
    }
  }
  else if (res == SZ_ERROR_NO_ARCHIVE)
  {
    /*
    SZ_ERROR_NO_ARCHIVE is possible for 2 states:
      XZ_STATE_STREAM_HEADER  - if bad signature or bad CRC
      XZ_STATE_STREAM_PADDING - if non-zero padding data
    extraSize and inProcessed don't include "bad" byte
    */
    // if (inProcessed == extraSize), there was no any good xz stream header, and we keep error
    if (inProcessed != extraSize) // if there were good xz streams before error
    {
      // if (extraSize != 0 || readProcessed != inProcessed)
      {
        // he we suppose that all xz streams were finsihed OK, and we have
        // some extra data after all streams
        stat->DataAfterEnd = True;
        res = SZ_OK;
      }
    }
  }
  
  if (stat->DecodeRes == SZ_OK)
    stat->DecodeRes = res;

  stat->InSize -= extraSize;
}



SRes XzDecMt_Decode(CXzDecMtHandle pp,
    const CXzDecMtProps *props,
    const UInt64 *outDataSize, int finishMode,
    ISeqOutStream *outStream,
    // Byte *outBuf, size_t *outBufSize,
    ISeqInStream *inStream,
    // const Byte *inData, size_t inDataSize,
    CXzStatInfo *stat,
    int *isMT,
    ICompressProgress *progress)
{
  CXzDecMt *p = (CXzDecMt *)pp;
  #ifndef _7ZIP_ST
  BoolInt tMode;
  #endif

  XzStatInfo_Clear(stat);

  p->props = *props;

  p->inStream = inStream;
  p->outStream = outStream;
  p->progress = progress;
  // p->stat = stat;

  p->outSize = 0;
  p->outSize_Defined = False;
  if (outDataSize)
  {
    p->outSize_Defined = True;
    p->outSize = *outDataSize;
  }

  p->finishMode = finishMode;

  // p->outSize = 457; p->outSize_Defined = True; p->finishMode = False; // for test

  p->writeRes = SZ_OK;
  p->outProcessed = 0;
  p->inProcessed = 0;
  p->readProcessed = 0;
  p->readWasFinished = False;
  p->readRes = SZ_OK;

  p->codeRes = SZ_OK;
  p->status = CODER_STATUS_NOT_SPECIFIED;

  XzUnpacker_Init(&p->dec);

  *isMT = False;

    /*
    p->outBuf = NULL;
    p->outBufSize = 0;
    if (!outStream)
    {
      p->outBuf = outBuf;
      p->outBufSize = *outBufSize;
      *outBufSize = 0;
    }
    */

  
  #ifndef _7ZIP_ST

  p->isBlockHeaderState_Parse = False;
  p->isBlockHeaderState_Write = False;
  // p->numBadBlocks = 0;
  p->mainErrorCode = SZ_OK;
  p->mainDecoderWasCalled = False;

  tMode = False;

  if (p->props.numThreads > 1)
  {
    IMtDecCallback2 vt;
    BoolInt needContinue;
    SRes res;
    // we just free ST buffers here
    // but we still keep state variables, that was set in XzUnpacker_Init()
    XzDecMt_FreeSt(p);

    p->outProcessed_Parse = 0;
    p->parsing_Truncated = False;

    p->numStreams = 0;
    p->numTotalBlocks = 0;
    p->numBlocks = 0;
    p->finishedDecoderIndex = -1;

    if (!p->mtc_WasConstructed)
    {
      p->mtc_WasConstructed = True;
      MtDec_Construct(&p->mtc);
    }
    
    p->mtc.mtCallback = &vt;
    p->mtc.mtCallbackObject = p;

    p->mtc.progress = progress;
    p->mtc.inStream = inStream;
    p->mtc.alloc = &p->alignOffsetAlloc.vt;
    // p->mtc.inData = inData;
    // p->mtc.inDataSize = inDataSize;
    p->mtc.inBufSize = p->props.inBufSize_MT;
    // p->mtc.inBlockMax = p->props.inBlockMax;
    p->mtc.numThreadsMax = p->props.numThreads;

    *isMT = True;

    vt.Parse = XzDecMt_Callback_Parse;
    vt.PreCode = XzDecMt_Callback_PreCode;
    vt.Code = XzDecMt_Callback_Code;
    vt.Write = XzDecMt_Callback_Write;


    res = MtDec_Code(&p->mtc);


    stat->InSize = p->mtc.inProcessed;
    
    p->inProcessed = p->mtc.inProcessed;
    p->readRes = p->mtc.readRes;
    p->readWasFinished = p->mtc.readWasFinished;
    p->readProcessed = p->mtc.readProcessed;
    
    tMode = True;
    needContinue = False;
    
    if (res == SZ_OK)
    {
      if (p->mtc.mtProgress.res != SZ_OK)
      {
        res = p->mtc.mtProgress.res;
        stat->ProgressRes = res;
        stat->CombinedRes_Type = SZ_ERROR_PROGRESS;
      }
      else
        needContinue = p->mtc.needContinue;
    }
    
    if (!needContinue)
    {
      {
        SRes codeRes;
        BoolInt truncated = False;
        ECoderStatus status;
        const CXzUnpacker *dec;

        stat->OutSize = p->outProcessed;
       
        if (p->finishedDecoderIndex >= 0)
        {
          const CXzDecMtThread *coder = &p->coders[(unsigned)p->finishedDecoderIndex];
          codeRes = coder->codeRes;
          dec = &coder->dec;
          status = coder->status;
        }
        else if (p->mainDecoderWasCalled)
        {
          codeRes = p->codeRes;
          dec = &p->dec;
          status = p->status;
          truncated = p->parsing_Truncated;
        }
        else
          return SZ_ERROR_FAIL;

        if (p->mainErrorCode != SZ_OK)
          stat->DecodeRes = p->mainErrorCode;

        XzStatInfo_SetStat(dec, p->finishMode,
            // p->mtc.readProcessed,
            p->mtc.inProcessed,
            codeRes, status,
            truncated,
            stat);
      }

      if (res == SZ_OK)
      {
        stat->ReadRes = p->mtc.readRes;

        if (p->writeRes != SZ_OK)
        {
          res = p->writeRes;
          stat->CombinedRes_Type = SZ_ERROR_WRITE;
        }
        else if (p->mtc.readRes != SZ_OK
            // && p->mtc.inProcessed == p->mtc.readProcessed
            && stat->DecodeRes == SZ_ERROR_INPUT_EOF)
        {
          res = p->mtc.readRes;
          stat->CombinedRes_Type = SZ_ERROR_READ;
        }
        else if (stat->DecodeRes != SZ_OK)
          res = stat->DecodeRes;
      }
      
      stat->CombinedRes = res;
      if (stat->CombinedRes_Type == SZ_OK)
        stat->CombinedRes_Type = res;
      return res;
    }

    PRF_STR("----- decoding ST -----");
  }

  #endif


  *isMT = False;

  {
    SRes res = XzDecMt_Decode_ST(p
        #ifndef _7ZIP_ST
        , tMode
        #endif
        , stat
        );

    #ifndef _7ZIP_ST
    // we must set error code from MT decoding at first
    if (p->mainErrorCode != SZ_OK)
      stat->DecodeRes = p->mainErrorCode;
    #endif

    XzStatInfo_SetStat(&p->dec,
        p->finishMode,
        // p->readProcessed,
        p->inProcessed,
        p->codeRes, p->status,
        False, // truncated
        stat);

    stat->ReadRes = p->readRes;

    if (res == SZ_OK)
    {
      if (p->readRes != SZ_OK
          // && p->inProcessed == p->readProcessed
          && stat->DecodeRes == SZ_ERROR_INPUT_EOF)
      {
        // we set read error as combined error, only if that error was the reason
        // of decoding problem
        res = p->readRes;
        stat->CombinedRes_Type = SZ_ERROR_READ;
      }
      else if (stat->DecodeRes != SZ_OK)
        res = stat->DecodeRes;
    }

    stat->CombinedRes = res;
    if (stat->CombinedRes_Type == SZ_OK)
      stat->CombinedRes_Type = res;
    return res;
  }
}
