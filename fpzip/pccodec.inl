#ifndef _PCCODEC_INL_
#define _PCCODEC_INL_


#if (defined(__i386__) && defined(USEASM))
#if (__i386__ && USEASM)
#define __bsr_asm 1
#endif
#endif

#ifndef __bsr_asm
#define __bsr_asm 0
#endif


template <typename U>
uint bsr(U x)
{
  uint k;
#if (__bsr_asm == 1)
  __asm__("bsr %1, %0" : "=r"(k) : "r"(x));
#else
  k = 0;
  do k++; while (x >>= 1);
  k--;
#endif
  return k;
}


#undef __bsr_asm


#endif

