#ifndef FPZIP_TYPES_H
#define FPZIP_TYPES_H

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;

#if __cplusplus >= 201103L
  // C++11 and later: use standard integer types
  #include <cstdint>
  #include <cinttypes>
  #define INT64C(x) INT64_C(x)
  #define UINT64C(x) UINT64_C(x)
  #define INT64PRId PRId64
  #define INT64PRIi PRIi64
  #define UINT64PRIo PRIo64
  #define UINT64PRIu PRIu64
  #define UINT64PRIx PRIx64
  #define INT64SCNd SCNd64
  #define INT64SCNi SCNi64
  #define UINT64SCNo SCNo64
  #define UINT64SCNu SCNu64
  #define UINT64SCNx SCNx64
  typedef int8_t int8;
  typedef uint8_t uint8;
  typedef int16_t int16;
  typedef uint16_t uint16;
  typedef int32_t int32;
  typedef uint32_t uint32;
  typedef int64_t int64;
  typedef uint64_t uint64;
#else
  // C++98: assume common integer types
  typedef signed char int8;
  typedef unsigned char uint8;
  typedef signed short int16;
  typedef unsigned short uint16;

  // assume 32-bit integers (LP64, LLP64)
  typedef signed int int32;
  typedef unsigned int uint32;

  // determine 64-bit data model
  #if defined(_WIN32) || defined(_WIN64)
    // assume ILP32 or LLP64 (MSVC, MinGW)
    #define FPZIP_LLP64 1
  #else
    // assume LP64 (Linux, macOS, ...)
    #define FPZIP_LP64 1
  #endif

  // concatenation for literal suffixes
  #define _fpzip_cat_(x, y) x ## y
  #define _fpzip_cat(x, y) _fpzip_cat_(x, y)

  // signed 64-bit integers
  #if defined(FPZIP_INT64) && defined(FPZIP_INT64_SUFFIX)
    #define INT64C(x) _fpzip_cat(x, FPZIP_INT64_SUFFIX)
    #define INT64PRId #FPZIP_INT64_SUFFIX "d"
    #define INT64PRIi #FPZIP_INT64_SUFFIX "i"
    typedef FPZIP_INT64 int64;
  #elif FPZIP_LP64
    #define INT64C(x) x ## l
    #define INT64PRId "ld"
    #define INT64PRIi "li"
    typedef signed long int64;
  #elif FPZIP_LLP64
    #define INT64C(x) x ## ll
    #define INT64PRId "lld"
    #define INT64PRIi "lli"
    typedef signed long long int64;
  #else
    #error "unknown 64-bit signed integer type"
  #endif
  #define INT64SCNd INT64PRId
  #define INT64SCNi INT64PRIi

  // unsigned 64-bit integers
  #if defined(FPZIP_UINT64) && defined(FPZIP_UINT64_SUFFIX)
    #define UINT64C(x) _fpzip_cat(x, FPZIP_UINT64_SUFFIX)
    #ifdef FPZIP_INT64_SUFFIX
      #define UINT64PRIo #FPZIP_INT64_SUFFIX "o"
      #define UINT64PRIu #FPZIP_INT64_SUFFIX "u"
      #define UINT64PRIx #FPZIP_INT64_SUFFIX "x"
    #endif
    typedef FPZIP_UINT64 uint64;
  #elif FPZIP_LP64
    #define UINT64C(x) x ## ul
    #define UINT64PRIo "lo"
    #define UINT64PRIu "lu"
    #define UINT64PRIx "lx"
    typedef unsigned long uint64;
  #elif FPZIP_LLP64
    #define UINT64C(x) x ## ull
    #define UINT64PRIo "llo"
    #define UINT64PRIu "llu"
    #define UINT64PRIx "llx"
    typedef unsigned long long uint64;
  #else
    #error "unknown 64-bit unsigned integer type"
  #endif
  #define UINT64SCNo UINT64PRIo
  #define UINT64SCNu UINT64PRIu
  #define UINT64SCNx UINT64PRIx
#endif

extern void* _fpzip_alloc(size_t);
extern void _fpzip_free(void*);

#include <utility>
#include <new>

template<typename T, typename... ARGS>
T* _fpzip_new(ARGS&&... args)
{
  T* p = static_cast<T*>(_fpzip_alloc(sizeof(T)));
  new (p) T(std::forward<ARGS>(args)...);
  return p;
}
template<typename T>
void _fpzip_delete(T* p)
{
  p->~T();
  _fpzip_free(p);
}

#endif
