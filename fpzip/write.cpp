#include <cstdio>
#include <cstdlib>
#include "pcencoder.h"
#include "rcqsmodel.h"
#include "front.h"
#include "fpzip.h"
#include "codec.h"
#include "write.h"

// array meta data and encoder
struct FPZoutput : public FPZ {
  RCencoder* re;
};

// allocate output stream
static FPZoutput*
allocate_output()
{
  FPZoutput* stream = _fpzip_new<FPZoutput>();
  stream->type = FPZIP_TYPE_FLOAT;
  stream->prec = 0;
  stream->nx = stream->ny = stream->nz = stream->nf = 1;
  stream->re = 0;
  return stream;
}

// compress 3D array at specified precision using floating-point arithmetic
template <typename T, uint bits>
static void
compress3d(
  RCencoder* re,   // entropy encoder
  const T*   data, // flattened 3D array to compress
  uint       nx,   // number of x samples
  uint       ny,   // number of y samples
  uint       nz    // number of z samples
)
{
  // initialize compressor
  typedef PCmap<T, bits> Map;
  RCmodel* rm = _fpzip_new<RCqsmodel>(true, PCencoder<T, Map>::symbols);
  PCencoder<T, Map>* fe = _fpzip_new<PCencoder<T, Map>>(re, &rm);
  Front<T> f(nx, ny);

  // encode difference between predicted (p) and actual (a) value
  uint x, y, z;
  for (z = 0, f.advance(0, 0, 1); z < nz; z++)
    for (y = 0, f.advance(0, 1, 0); y < ny; y++)
      for (x = 0, f.advance(1, 0, 0); x < nx; x++) {
      	volatile T p = f(1, 1, 1);
      	p += f(1, 0, 0);
      	p -= f(0, 1, 1);
      	p += f(0, 1, 0);
      	p -= f(1, 0, 1);
      	p += f(0, 0, 1);
      	p -= f(1, 1, 0);
        T a = *data++;
        a = fe->encode(a, p);
        f.push(a);
      }

  _fpzip_delete(fe);
  _fpzip_delete(rm);
}

// compress p-bit float, 2p-bit double
#define compress_case(p)\
  case subsize(T, p):\
    compress3d<T, subsize(T, p)>(stream->re, data, stream->nx, stream->ny, stream->nz);\
    break

// compress 4D array
template <typename T>
static bool
compress4d(
  FPZoutput* stream, // output stream
  const T*   data    // flattened 4D array to compress
)
{
  // compress one field at a time
  for (int i = 0; i < stream->nf; i++) {
    int bits = stream->prec ? stream->prec : (int)(CHAR_BIT * sizeof(T));
    switch (bits) {
      compress_case( 2);
      compress_case( 3);
      compress_case( 4);
      compress_case( 5);
      compress_case( 6);
      compress_case( 7);
      compress_case( 8);
      compress_case( 9);
      compress_case(10);
      compress_case(11);
      compress_case(12);
      compress_case(13);
      compress_case(14);
      compress_case(15);
      compress_case(16);
      compress_case(17);
      compress_case(18);
      compress_case(19);
      compress_case(20);
      compress_case(21);
      compress_case(22);
      compress_case(23);
      compress_case(24);
      compress_case(25);
      compress_case(26);
      compress_case(27);
      compress_case(28);
      compress_case(29);
      compress_case(30);
      compress_case(31);
      compress_case(32);
      default:
        fpzip_errno = fpzipErrorBadPrecision;
        return false;
    }
    data += stream->nx * stream->ny * stream->nz;
  }
  return true;
}

// write compressed stream to file
FPZ*
fpzip_write_to_file(
  FILE* file // binary output stream
)
{
  fpzip_errno = fpzipSuccess;
  FPZoutput* stream = allocate_output();
  stream->re = _fpzip_new<RCfileencoder>(file);
  return static_cast<FPZ*>(stream);
}

// write compressed stream to memory buffer
FPZ*
fpzip_write_to_buffer(
  void*  buffer, // pointer to compressed data
  size_t size    // size of buffer
)
{
  fpzip_errno = fpzipSuccess;
  FPZoutput* stream = allocate_output();
  stream->re = _fpzip_new<RCmemencoder>(buffer, size);
  return static_cast<FPZ*>(stream);
}

// close stream for writing and clean up
void
fpzip_write_close(
  FPZ* fpz // stream handle
)
{
  FPZoutput* stream = static_cast<FPZoutput*>(fpz);
  _fpzip_delete(stream->re);
  _fpzip_delete(stream);
}

// write meta data
int
fpzip_write_header(
  FPZ* fpz // stream handle
)
{
  fpzip_errno = fpzipSuccess;

  FPZoutput* stream = static_cast<FPZoutput*>(fpz);
  RCencoder* re = stream->re;

  // magic
  re->encode<uint>('f', 8);
  re->encode<uint>('p', 8);
  re->encode<uint>('z', 8);
  re->encode<uint>('\0', 8);

  // format version
  re->encode<uint>(FPZ_MAJ_VERSION, 16);
  re->encode<uint>(FPZ_MIN_VERSION, 8);

  // type and precision
  re->encode<uint>(stream->type, 1);
  re->encode<uint>(stream->prec, 7);

  // array dimensions
  re->encode<uint>(stream->nx, 32);
  re->encode<uint>(stream->ny, 32);
  re->encode<uint>(stream->nz, 32);
  re->encode<uint>(stream->nf, 32);

  if (re->error) {
    fpzip_errno = fpzipErrorWriteStream;
    return 0;
  }

  return 1;
}

// compress a single- or double-precision 4D array
size_t
fpzip_write(
  FPZ*        fpz, // stream handle
  const void* data // array to write
)
{
  fpzip_errno = fpzipSuccess;
  size_t bytes = 0;
  {
    FPZoutput* stream = static_cast<FPZoutput*>(fpz);
    bool success = (stream->type == FPZIP_TYPE_FLOAT
      ? compress4d(stream, static_cast<const float*>(data))
      : compress4d(stream, static_cast<const double*>(data)));
    if (success) {
      RCencoder* re = stream->re;
      re->finish();
      if (re->error) {
        if (fpzip_errno == fpzipSuccess)
          fpzip_errno = fpzipErrorWriteStream;
      }
      else
        bytes = re->bytes();
    }
  }
  
  return bytes;
}
