/* Sha256.c -- SHA-256 Hash
2021-04-01 : Igor Pavlov : Public domain
This code is based on public domain code from Wei Dai's Crypto++ library. */

#include "Sha256.h"

#include <cstring>

#include "CpuArch.h"
#include "RotateDefs.h"


extern bool __common_memset(void*, int, size_t);
extern bool __common_memcpy(void* _restrict, const void* _restrict, size_t);


#if defined(_MSC_VER) && (_MSC_VER < 1900)
// #define USE_MY_MM
#endif

#ifdef MY_CPU_X86_OR_AMD64
  #ifdef _MSC_VER
    #if _MSC_VER >= 1200
      #define _SHA_SUPPORTED
    #endif
  #elif defined(__clang__)
    #if (__clang_major__ >= 8) // fix that check
      #define _SHA_SUPPORTED
    #endif
  #elif defined(__GNUC__)
    #if (__GNUC__ >= 8) // fix that check
      #define _SHA_SUPPORTED
    #endif
  #elif defined(__INTEL_COMPILER)
    #if (__INTEL_COMPILER >= 1800) // fix that check
      #define _SHA_SUPPORTED
    #endif
  #endif
#elif defined(MY_CPU_ARM_OR_ARM64)
  #ifdef _MSC_VER
    #if _MSC_VER >= 1910
      #define _SHA_SUPPORTED
    #endif
  #elif defined(__clang__)
    #if (__clang_major__ >= 8) // fix that check
      #define _SHA_SUPPORTED
    #endif
  #elif defined(__GNUC__)
    #if (__GNUC__ >= 6) // fix that check
      #define _SHA_SUPPORTED
    #endif
  #endif
#endif

void MY_FAST_CALL Sha256_UpdateBlocks(UInt32 state[8], const Byte *data, size_t numBlocks);

#ifdef _SHA_SUPPORTED
  void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks);

  static SHA256_FUNC_UPDATE_BLOCKS g_FUNC_UPDATE_BLOCKS = Sha256_UpdateBlocks;
  static SHA256_FUNC_UPDATE_BLOCKS g_FUNC_UPDATE_BLOCKS_HW;

  #define UPDATE_BLOCKS(p) p->func_UpdateBlocks
#else
  #define UPDATE_BLOCKS(p) Sha256_UpdateBlocks
#endif


BoolInt Sha256_SetFunction(CSha256 *p, unsigned algo)
{
  SHA256_FUNC_UPDATE_BLOCKS func = Sha256_UpdateBlocks;
  
  #ifdef _SHA_SUPPORTED
    if (algo != SHA256_ALGO_SW)
    {
      if (algo == SHA256_ALGO_DEFAULT)
        func = g_FUNC_UPDATE_BLOCKS;
      else
      {
        if (algo != SHA256_ALGO_HW)
          return False;
        func = g_FUNC_UPDATE_BLOCKS_HW;
        if (!func)
          return False;
      }
    }
  #else
    if (algo > 1)
      return False;
  #endif

  p->func_UpdateBlocks = func;
  return True;
}


/* define it for speed optimization */

#ifdef _SFX
  #define STEP_PRE 1
  #define STEP_MAIN 1
#else
  #define STEP_PRE 2
  #define STEP_MAIN 4
  // #define _SHA256_UNROLL
#endif

#if STEP_MAIN != 16
  #define _SHA256_BIG_W
#endif




void Sha256_InitState(CSha256 *p)
{
  p->count = 0;
  p->state[0] = 0x6a09e667;
  p->state[1] = 0xbb67ae85;
  p->state[2] = 0x3c6ef372;
  p->state[3] = 0xa54ff53a;
  p->state[4] = 0x510e527f;
  p->state[5] = 0x9b05688c;
  p->state[6] = 0x1f83d9ab;
  p->state[7] = 0x5be0cd19;
}

void Sha256_Init(CSha256 *p)
{
  p->func_UpdateBlocks =
  #ifdef _SHA_SUPPORTED
      g_FUNC_UPDATE_BLOCKS;
  #else
      NULL;
  #endif
  Sha256_InitState(p);
}

#define S0(x) (rotrFixed(x, 2) ^ rotrFixed(x,13) ^ rotrFixed(x, 22))
#define S1(x) (rotrFixed(x, 6) ^ rotrFixed(x,11) ^ rotrFixed(x, 25))
#define s0(x) (rotrFixed(x, 7) ^ rotrFixed(x,18) ^ (x >> 3))
#define s1(x) (rotrFixed(x,17) ^ rotrFixed(x,19) ^ (x >> 10))

#define Ch(x,y,z) (z^(x&(y^z)))
#define Maj(x,y,z) ((x&y)|(z&(x|y)))


#define W_PRE(i) (W[(i) + (size_t)(j)] = GetBe32(data + ((size_t)(j) + i) * 4))

#define blk2_main(j, i)  s1(w(j, (i)-2)) + w(j, (i)-7) + s0(w(j, (i)-15))

#ifdef _SHA256_BIG_W
    // we use +i instead of +(i) to change the order to solve CLANG compiler warning for signed/unsigned.
    #define w(j, i)     W[(size_t)(j) + i]
    #define blk2(j, i)  (w(j, i) = w(j, (i)-16) + blk2_main(j, i))
#else
    #if STEP_MAIN == 16
        #define w(j, i)  W[(i) & 15]
    #else
        #define w(j, i)  W[((size_t)(j) + (i)) & 15]
    #endif
    #define blk2(j, i)  (w(j, i) += blk2_main(j, i))
#endif

#define W_MAIN(i)  blk2(j, i)


#define T1(wx, i) \
    tmp = h + S1(e) + Ch(e,f,g) + K[(i)+(size_t)(j)] + wx(i); \
    h = g; \
    g = f; \
    f = e; \
    e = d + tmp; \
    tmp += S0(a) + Maj(a, b, c); \
    d = c; \
    c = b; \
    b = a; \
    a = tmp; \

#define R1_PRE(i)  T1( W_PRE, i)
#define R1_MAIN(i) T1( W_MAIN, i)

#if (!defined(_SHA256_UNROLL) || STEP_MAIN < 8) && (STEP_MAIN >= 4)
#define R2_MAIN(i) \
    R1_MAIN(i) \
    R1_MAIN(i + 1) \

#endif



#if defined(_SHA256_UNROLL) && STEP_MAIN >= 8

#define T4( a,b,c,d,e,f,g,h, wx, i) \
    h += S1(e) + Ch(e,f,g) + K[(i)+(size_t)(j)] + wx(i); \
    tmp = h; \
    h += d; \
    d = tmp + S0(a) + Maj(a, b, c); \

#define R4( wx, i) \
    T4 ( a,b,c,d,e,f,g,h, wx, (i  )); \
    T4 ( d,a,b,c,h,e,f,g, wx, (i+1)); \
    T4 ( c,d,a,b,g,h,e,f, wx, (i+2)); \
    T4 ( b,c,d,a,f,g,h,e, wx, (i+3)); \

#define R4_PRE(i)  R4( W_PRE, i)
#define R4_MAIN(i) R4( W_MAIN, i)


#define T8( a,b,c,d,e,f,g,h, wx, i) \
    h += S1(e) + Ch(e,f,g) + K[(i)+(size_t)(j)] + wx(i); \
    d += h; \
    h += S0(a) + Maj(a, b, c); \

#define R8( wx, i) \
    T8 ( a,b,c,d,e,f,g,h, wx, i  ); \
    T8 ( h,a,b,c,d,e,f,g, wx, i+1); \
    T8 ( g,h,a,b,c,d,e,f, wx, i+2); \
    T8 ( f,g,h,a,b,c,d,e, wx, i+3); \
    T8 ( e,f,g,h,a,b,c,d, wx, i+4); \
    T8 ( d,e,f,g,h,a,b,c, wx, i+5); \
    T8 ( c,d,e,f,g,h,a,b, wx, i+6); \
    T8 ( b,c,d,e,f,g,h,a, wx, i+7); \

#define R8_PRE(i)  R8( W_PRE, i)
#define R8_MAIN(i) R8( W_MAIN, i)

#endif

void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks);

// static
extern MY_ALIGN(64)
const UInt32 SHA256_K_ARRAY[64];

MY_ALIGN(64)
const UInt32 SHA256_K_ARRAY[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define K SHA256_K_ARRAY


MY_NO_INLINE
void MY_FAST_CALL Sha256_UpdateBlocks(UInt32 state[8], const Byte *data, size_t numBlocks)
{
  UInt32 W
  #ifdef _SHA256_BIG_W
      [64];
  #else
      [16];
  #endif

  unsigned j;

  UInt32 a,b,c,d,e,f,g,h;

  #if !defined(_SHA256_UNROLL) || (STEP_MAIN <= 4) || (STEP_PRE <= 4)
  UInt32 tmp;
  #endif
  
  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];
  f = state[5];
  g = state[6];
  h = state[7];

  while (numBlocks)
  {

  for (j = 0; j < 16; j += STEP_PRE)
  {
    #if STEP_PRE > 4

      #if STEP_PRE < 8
      R4_PRE(0);
      #else
      R8_PRE(0);
      #if STEP_PRE == 16
      R8_PRE(8);
      #endif
      #endif

    #else

      R1_PRE(0);
      #if STEP_PRE >= 2
      R1_PRE(1);
      #if STEP_PRE >= 4
      R1_PRE(2);
      R1_PRE(3);
      #endif
      #endif
    
    #endif
  }

  for (j = 16; j < 64; j += STEP_MAIN)
  {
    #if defined(_SHA256_UNROLL) && STEP_MAIN >= 8

      #if STEP_MAIN < 8
      R4_MAIN(0);
      #else
      R8_MAIN(0);
      #if STEP_MAIN == 16
      R8_MAIN(8);
      #endif
      #endif

    #else
      
      R1_MAIN(0);
      #if STEP_MAIN >= 2
      R1_MAIN(1);
      #if STEP_MAIN >= 4
      R2_MAIN(2);
      #if STEP_MAIN >= 8
      R2_MAIN(4);
      R2_MAIN(6);
      #if STEP_MAIN >= 16
      R2_MAIN(8);
      R2_MAIN(10);
      R2_MAIN(12);
      R2_MAIN(14);
      #endif
      #endif
      #endif
      #endif
    #endif
  }

  a += state[0]; state[0] = a;
  b += state[1]; state[1] = b;
  c += state[2]; state[2] = c;
  d += state[3]; state[3] = d;
  e += state[4]; state[4] = e;
  f += state[5]; state[5] = f;
  g += state[6]; state[6] = g;
  h += state[7]; state[7] = h;

  data += 64;
  numBlocks--;
  }

  /* Wipe variables */
  /* __common_memset(W, 0, sizeof(W)); */
}

#undef S0
#undef S1
#undef s0
#undef s1
#undef K

#define Sha256_UpdateBlock(p) UPDATE_BLOCKS(p)(p->state, p->buffer, 1)

void Sha256_Update(CSha256 *p, const Byte *data, size_t size)
{
  if (size == 0)
    return;

  {
    unsigned pos = (unsigned)p->count & 0x3F;
    unsigned num;
    
    p->count += size;
    
    num = 64 - pos;
    if (num > size)
    {
      __common_memcpy(p->buffer + pos, data, size);
      return;
    }
    
    if (pos != 0)
    {
      size -= num;
      __common_memcpy(p->buffer + pos, data, num);
      data += num;
      Sha256_UpdateBlock(p);
    }
  }
  {
    size_t numBlocks = size >> 6;
    UPDATE_BLOCKS(p)(p->state, data, numBlocks);
    size &= 0x3F;
    if (size == 0)
      return;
    data += (numBlocks << 6);
    __common_memcpy(p->buffer, data, size);
  }
}


void Sha256_Final(CSha256 *p, Byte *digest)
{
  unsigned pos = (unsigned)p->count & 0x3F;
  unsigned i;
  
  p->buffer[pos++] = 0x80;
  
  if (pos > (64 - 8))
  {
    while (pos != 64) { p->buffer[pos++] = 0; }
    // __common_memset(&p->buf.buffer[pos], 0, 64 - pos);
    Sha256_UpdateBlock(p);
    pos = 0;
  }

  /*
  if (pos & 3)
  {
    p->buffer[pos] = 0;
    p->buffer[pos + 1] = 0;
    p->buffer[pos + 2] = 0;
    pos += 3;
    pos &= ~3;
  }
  {
    for (; pos < 64 - 8; pos += 4)
      *(UInt32 *)(&p->buffer[pos]) = 0;
  }
  */

  __common_memset(&p->buffer[pos], 0, (64 - 8) - pos);

  {
    UInt64 numBits = (p->count << 3);
    SetBe32(p->buffer + 64 - 8, (UInt32)(numBits >> 32));
    SetBe32(p->buffer + 64 - 4, (UInt32)(numBits));
  }
  
  Sha256_UpdateBlock(p);

  for (i = 0; i < 8; i += 2)
  {
    UInt32 v0 = p->state[i];
    UInt32 v1 = p->state[(size_t)i + 1];
    SetBe32(digest    , v0);
    SetBe32(digest + 4, v1);
    digest += 8;
  }
  
  Sha256_InitState(p);
}


void Sha256Prepare()
{
  #ifdef _SHA_SUPPORTED
  SHA256_FUNC_UPDATE_BLOCKS f, f_hw;
  f = Sha256_UpdateBlocks;
  f_hw = NULL;
  #ifdef MY_CPU_X86_OR_AMD64
  #ifndef USE_MY_MM
  if (CPU_IsSupported_SHA()
      && CPU_IsSupported_SSSE3()
      // && CPU_IsSupported_SSE41()
      )
  #endif
  #else
  if (CPU_IsSupported_SHA2())
  #endif
  {
    // printf("\n========== HW SHA256 ======== \n");
    f = f_hw = Sha256_UpdateBlocks_HW;
  }
  g_FUNC_UPDATE_BLOCKS    = f;
  g_FUNC_UPDATE_BLOCKS_HW = f_hw;
  #endif
}




/* Sha256Opt.cpp */




#ifdef MY_CPU_X86_OR_AMD64
  #if defined(__clang__)
    #if (__clang_major__ >= 8) // fix that check
      #define USE_HW_SHA
      #ifndef __SHA__
        #define ATTRIB_SHA __attribute__((__target__("sha,ssse3")))
        #if defined(_MSC_VER)
          // SSSE3: for clang-cl:
          #include <tmmintrin.h>
          #define __SHA__
        #endif
      #endif

    #endif
  #elif defined(__GNUC__)
    #if (__GNUC__ >= 8) // fix that check
      #define USE_HW_SHA
      #ifndef __SHA__
        #define ATTRIB_SHA __attribute__((__target__("sha,ssse3")))
        // #pragma GCC target("sha,ssse3")
      #endif
    #endif
  #elif defined(__INTEL_COMPILER)
    #if (__INTEL_COMPILER >= 1800) // fix that check
      #define USE_HW_SHA
    #endif
  #elif defined(_MSC_VER)
    #ifdef USE_MY_MM
      #define USE_VER_MIN 1300
    #else
      #define USE_VER_MIN 1910
    #endif
    #if _MSC_VER >= USE_VER_MIN
      #define USE_HW_SHA
    #endif
  #endif
// #endif // MY_CPU_X86_OR_AMD64

#ifdef USE_HW_SHA

// #pragma message("Sha256 HW")
// #include <wmmintrin.h>

#if !defined(_MSC_VER) || (_MSC_VER >= 1900)
#include <immintrin.h>
#else
#include <emmintrin.h>

#if defined(_MSC_VER) && (_MSC_VER >= 1600)
// #include <intrin.h>
#endif

#ifdef USE_MY_MM
#include "My_mm.h"
#endif

#endif

/*
SHA256 uses:
SSE2:
  _mm_loadu_si128
  _mm_storeu_si128
  _mm_set_epi32
  _mm_add_epi32
  _mm_shuffle_epi32 / pshufd


  
SSSE3:
  _mm_shuffle_epi8 / pshufb
  _mm_alignr_epi8
SHA:
  _mm_sha256*
*/

// K array must be aligned for 16-bytes at least.
// The compiler can look align attribute and selects
//   movdqu - for code without align attribute
//   movdqa - for code with    align attribute
extern
MY_ALIGN(64)
const UInt32 SHA256_K_ARRAY[64];

#define K SHA256_K_ARRAY


#define ADD_EPI32(dest, src) dest = _mm_add_epi32(dest, src);
#define SHA256_MSG1(dest, src) dest = _mm_sha256msg1_epu32(dest, src);
#define SHA25G_MSG2(dest, src) dest = _mm_sha256msg2_epu32(dest, src);


#define LOAD_SHUFFLE(m, k) \
    m = _mm_loadu_si128((const __m128i *)(const void *)(data + (k) * 16)); \
    m = _mm_shuffle_epi8(m, mask); \

#define SM1(g0, g1, g2, g3) \
    SHA256_MSG1(g3, g0); \

#define SM2(g0, g1, g2, g3) \
    tmp = _mm_alignr_epi8(g1, g0, 4); \
    ADD_EPI32(g2, tmp); \
    SHA25G_MSG2(g2, g1); \

// #define LS0(k, g0, g1, g2, g3) LOAD_SHUFFLE(g0, k)
// #define LS1(k, g0, g1, g2, g3) LOAD_SHUFFLE(g1, k+1)


#define NNN(g0, g1, g2, g3)


#define RND2(t0, t1) \
    t0 = _mm_sha256rnds2_epu32(t0, t1, msg);

#define RND2_0(m, k) \
    msg = _mm_add_epi32(m, *(const __m128i *) (const void *) &K[(k) * 4]); \
    RND2(state0, state1); \
    msg = _mm_shuffle_epi32(msg, 0x0E); \


#define RND2_1 \
    RND2(state1, state0); \


// We use scheme with 3 rounds ahead for SHA256_MSG1 / 2 rounds ahead for SHA256_MSG2

#define R4(k, g0, g1, g2, g3, OP0, OP1) \
    RND2_0(g0, k); \
    OP0(g0, g1, g2, g3); \
    RND2_1; \
    OP1(g0, g1, g2, g3); \

#define R16(k, OP0, OP1, OP2, OP3, OP4, OP5, OP6, OP7) \
    R4 ( (k)*4+0, m0, m1, m2, m3, OP0, OP1 ) \
    R4 ( (k)*4+1, m1, m2, m3, m0, OP2, OP3 ) \
    R4 ( (k)*4+2, m2, m3, m0, m1, OP4, OP5 ) \
    R4 ( (k)*4+3, m3, m0, m1, m2, OP6, OP7 ) \

#define PREPARE_STATE \
    tmp    = _mm_shuffle_epi32(state0, 0x1B); /* abcd */ \
    state0 = _mm_shuffle_epi32(state1, 0x1B); /* efgh */ \
    state1 = state0; \
    state0 = _mm_unpacklo_epi64(state0, tmp); /* cdgh */ \
    state1 = _mm_unpackhi_epi64(state1, tmp); /* abef */ \


void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks);
#ifdef ATTRIB_SHA
ATTRIB_SHA
#endif
void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks)
{
  const __m128i mask = _mm_set_epi32(0x0c0d0e0f, 0x08090a0b, 0x04050607, 0x00010203);
  __m128i tmp;
  __m128i state0, state1;

  if (numBlocks == 0)
    return;

  state0 = _mm_loadu_si128((const __m128i *) (const void *) &state[0]);
  state1 = _mm_loadu_si128((const __m128i *) (const void *) &state[4]);
  
  PREPARE_STATE

  do
  {
    __m128i state0_save, state1_save;
    __m128i m0, m1, m2, m3;
    __m128i msg;
    // #define msg tmp

    state0_save = state0;
    state1_save = state1;
    
    LOAD_SHUFFLE (m0, 0)
    LOAD_SHUFFLE (m1, 1)
    LOAD_SHUFFLE (m2, 2)
    LOAD_SHUFFLE (m3, 3)



    R16 ( 0, NNN, NNN, SM1, NNN, SM1, SM2, SM1, SM2 );
    R16 ( 1, SM1, SM2, SM1, SM2, SM1, SM2, SM1, SM2 );
    R16 ( 2, SM1, SM2, SM1, SM2, SM1, SM2, SM1, SM2 );
    R16 ( 3, SM1, SM2, NNN, SM2, NNN, NNN, NNN, NNN );
    
    ADD_EPI32(state0, state0_save);
    ADD_EPI32(state1, state1_save);
    
    data += 64;
  }
  while (--numBlocks);

  PREPARE_STATE

  _mm_storeu_si128((__m128i *) (void *) &state[0], state0);
  _mm_storeu_si128((__m128i *) (void *) &state[4], state1);
}

#endif // USE_HW_SHA

#elif defined(MY_CPU_ARM_OR_ARM64)

  #if defined(__clang__)
    #if (__clang_major__ >= 8) // fix that check
      #define USE_HW_SHA
    #endif
  #elif defined(__GNUC__)
    #if (__GNUC__ >= 6) // fix that check
      #define USE_HW_SHA
    #endif
  #elif defined(_MSC_VER)
    #if _MSC_VER >= 1910
      #define USE_HW_SHA
    #endif
  #endif

#ifdef USE_HW_SHA

// #pragma message("=== Sha256 HW === ")

#if defined(__clang__) || defined(__GNUC__)
  #ifdef MY_CPU_ARM64
    #define ATTRIB_SHA __attribute__((__target__("+crypto")))
  #else
    #define ATTRIB_SHA __attribute__((__target__("fpu=crypto-neon-fp-armv8")))
  #endif
#else
  // _MSC_VER
  // for arm32
  #define _ARM_USE_NEW_NEON_INTRINSICS
#endif

#if defined(_MSC_VER) && defined(MY_CPU_ARM64)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

typedef uint32x4_t v128;
// typedef __n128 v128; // MSVC

#ifdef MY_CPU_BE
  #define MY_rev32_for_LE(x)
#else
  #define MY_rev32_for_LE(x) x = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(x)))
#endif

#define LOAD_128(_p)      (*(const v128 *)(const void *)(_p))
#define STORE_128(_p, _v) *(v128 *)(void *)(_p) = (_v)

#define LOAD_SHUFFLE(m, k) \
    m = LOAD_128((data + (k) * 16)); \
    MY_rev32_for_LE(m); \

// K array must be aligned for 16-bytes at least.
extern
MY_ALIGN(64)
const UInt32 SHA256_K_ARRAY[64];

#define K SHA256_K_ARRAY


#define SHA256_SU0(dest, src)        dest = vsha256su0q_u32(dest, src);
#define SHA25G_SU1(dest, src2, src3) dest = vsha256su1q_u32(dest, src2, src3);

#define SM1(g0, g1, g2, g3)  SHA256_SU0(g3, g0)
#define SM2(g0, g1, g2, g3)  SHA25G_SU1(g2, g0, g1)
#define NNN(g0, g1, g2, g3)


#define R4(k, g0, g1, g2, g3, OP0, OP1) \
    msg = vaddq_u32(g0, *(const v128 *) (const void *) &K[(k) * 4]); \
    tmp = state0; \
    state0 = vsha256hq_u32( state0, state1, msg ); \
    state1 = vsha256h2q_u32( state1, tmp, msg ); \
    OP0(g0, g1, g2, g3); \
    OP1(g0, g1, g2, g3); \


#define R16(k, OP0, OP1, OP2, OP3, OP4, OP5, OP6, OP7) \
    R4 ( (k)*4+0, m0, m1, m2, m3, OP0, OP1 ) \
    R4 ( (k)*4+1, m1, m2, m3, m0, OP2, OP3 ) \
    R4 ( (k)*4+2, m2, m3, m0, m1, OP4, OP5 ) \
    R4 ( (k)*4+3, m3, m0, m1, m2, OP6, OP7 ) \


void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks);
#ifdef ATTRIB_SHA
ATTRIB_SHA
#endif
void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks)
{
  v128 state0, state1;

  if (numBlocks == 0)
    return;

  state0 = LOAD_128(&state[0]);
  state1 = LOAD_128(&state[4]);
  
  do
  {
    v128 state0_save, state1_save;
    v128 m0, m1, m2, m3;
    v128 msg, tmp;

    state0_save = state0;
    state1_save = state1;
    
    LOAD_SHUFFLE (m0, 0)
    LOAD_SHUFFLE (m1, 1)
    LOAD_SHUFFLE (m2, 2)
    LOAD_SHUFFLE (m3, 3)

    R16 ( 0, NNN, NNN, SM1, NNN, SM1, SM2, SM1, SM2 );
    R16 ( 1, SM1, SM2, SM1, SM2, SM1, SM2, SM1, SM2 );
    R16 ( 2, SM1, SM2, SM1, SM2, SM1, SM2, SM1, SM2 );
    R16 ( 3, SM1, SM2, NNN, SM2, NNN, NNN, NNN, NNN );
    
    state0 = vaddq_u32(state0, state0_save);
    state1 = vaddq_u32(state1, state1_save);
    
    data += 64;
  }
  while (--numBlocks);

  STORE_128(&state[0], state0);
  STORE_128(&state[4], state1);
}

#endif // USE_HW_SHA

#endif // MY_CPU_ARM_OR_ARM64


#ifndef USE_HW_SHA

// #error Stop_Compiling_UNSUPPORTED_SHA
// #include <cstdlib>

// #include "Sha256.h"
void MY_FAST_CALL Sha256_UpdateBlocks(UInt32 state[8], const Byte *data, size_t numBlocks);

#pragma message("Sha256 HW-SW stub was used")

void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks);
void MY_FAST_CALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks)
{
  Sha256_UpdateBlocks(state, data, numBlocks);
  /*
  UNUSED_VAR(state);
  UNUSED_VAR(data);
  UNUSED_VAR(numBlocks);
  exit(1);
  return;
  */
}

#endif
