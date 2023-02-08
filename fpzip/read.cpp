#include "read.h"
#include <cstdio>
#include <cstdlib>
#include "pcdecoder.h"
#include "rcqsmodel.h"
#include "front.h"
#include "codec.h"

// array meta data and decoder
struct FPZinput : public FPZ {
  RCdecoder* rd;
};

// allocate input stream
static FPZinput*
allocate_input()
{
  FPZinput* stream = _fpzip_new<FPZinput>();
  stream->type = FPZIP_TYPE_FLOAT;
  stream->prec = 0;
  stream->nx = stream->ny = stream->nz = stream->nf = 1;
  stream->rd = 0;
  return stream;
}

// decompress 3D array at specified precision using floating-point arithmetic
template <typename T, uint bits>
static void
decompress3d(
  RCdecoder* rd,   // entropy decoder
  T*         data, // flattened 3D array to decompress to
  uint       nx,   // number of x samples
  uint       ny,   // number of y samples
  uint       nz    // number of z samples
)
{
  // initialize decompressor
  typedef PCmap<T, bits> Map;
  RCmodel* rm = _fpzip_new<RCqsmodel>(false, PCdecoder<T, Map>::symbols);
  PCdecoder<T, Map>* fd = _fpzip_new<PCdecoder<T, Map>>(rd, &rm);
  Front<T> f(nx, ny);

  // decode difference between predicted (p) and actual (a) value
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
        T a = fd->decode(p);
        *data++ = a;
        f.push(a);
      }

  _fpzip_delete(fd);
  _fpzip_delete(rm);
}

// decompress p-bit float, 2p-bit double
#define decompress_case(p)\
  case subsize(T, p):\
    decompress3d<T, subsize(T, p)>(stream->rd, data, stream->nx, stream->ny, stream->nz);\
    break

// decompress 4D array
template <typename T>
static bool
decompress4d(
  FPZinput* stream, // input stream
  T*        data    // flattened 4D array to decompress to
)
{
  // decompress one field at a time
  for (int i = 0; i < stream->nf; i++) {
    int bits = stream->prec ? stream->prec : (int)(CHAR_BIT * sizeof(T));
    switch (bits) {
      decompress_case( 2);
      decompress_case( 3);
      decompress_case( 4);
      decompress_case( 5);
      decompress_case( 6);
      decompress_case( 7);
      decompress_case( 8);
      decompress_case( 9);
      decompress_case(10);
      decompress_case(11);
      decompress_case(12);
      decompress_case(13);
      decompress_case(14);
      decompress_case(15);
      decompress_case(16);
      decompress_case(17);
      decompress_case(18);
      decompress_case(19);
      decompress_case(20);
      decompress_case(21);
      decompress_case(22);
      decompress_case(23);
      decompress_case(24);
      decompress_case(25);
      decompress_case(26);
      decompress_case(27);
      decompress_case(28);
      decompress_case(29);
      decompress_case(30);
      decompress_case(31);
      decompress_case(32);
      default:
        fpzip_errno = fpzipErrorBadPrecision;
        return false;
    }
    data += stream->nx * stream->ny * stream->nz;
  }
  return true;
}

// read compressed stream from file
FPZ*
fpzip_read_from_file(
  FILE* file // binary input stream
)
{
  fpzip_errno = fpzipSuccess;
  FPZinput* stream = allocate_input();
  stream->rd = _fpzip_new<RCfiledecoder>(file);
  stream->rd->init();
  return static_cast<FPZ*>(stream);
}

// read compressed stream from memory buffer
FPZ*
fpzip_read_from_buffer(
  const void* buffer // pointer to compressed data
)
{
  fpzip_errno = fpzipSuccess;
  FPZinput* stream = allocate_input();
  stream->rd = _fpzip_new<RCmemdecoder>(buffer);
  stream->rd->init();
  return static_cast<FPZ*>(stream);
}

// close stream for reading and clean up
void
fpzip_read_close(
  FPZ* fpz // stream handle
)
{
  FPZinput* stream = static_cast<FPZinput*>(fpz);
  _fpzip_delete(stream->rd);
  _fpzip_delete(stream);
}

// read meta data
int
fpzip_read_header(
  FPZ* fpz // stream handle
)
{
  fpzip_errno = fpzipSuccess;

  FPZinput* stream = static_cast<FPZinput*>(fpz);
  RCdecoder* rd = stream->rd;

  // magic
  if (rd->decode<uint>(8) != 'f' ||
      rd->decode<uint>(8) != 'p' ||
      rd->decode<uint>(8) != 'z' ||
      rd->decode<uint>(8) != '\0') {
    fpzip_errno = fpzipErrorBadFormat;
    return 0;
  }

  // format version
  if (rd->decode<uint>(16) != FPZ_MAJ_VERSION ||
      rd->decode<uint>(8) != FPZ_MIN_VERSION) {
    fpzip_errno = fpzipErrorBadVersion;
    return 0;
  }

  // type and precision
  stream->type = rd->decode<uint>(1);
  stream->prec = rd->decode<uint>(7);

  // array dimensions
  stream->nx = rd->decode<uint>(32);
  stream->ny = rd->decode<uint>(32);
  stream->nz = rd->decode<uint>(32);
  stream->nf = rd->decode<uint>(32);

  return 1;
}

// decompress a single- or double-precision 4D array
size_t
fpzip_read(
  FPZ*  fpz, // stream handle
  void* data // array to read
)
{
  fpzip_errno = fpzipSuccess;
  size_t bytes = 0;
  {
    FPZinput* stream = static_cast<FPZinput*>(fpz);
    bool success = (stream->type == FPZIP_TYPE_FLOAT
      ? decompress4d(stream, static_cast<float*>(data))
      : decompress4d(stream, static_cast<double*>(data)));
    if (success) {
      RCdecoder* rd = stream->rd;
      if (rd->error) {
        if (fpzip_errno == fpzipSuccess)
          fpzip_errno = fpzipErrorReadStream;
      }
      else
        bytes = rd->bytes();
    }
  }

  return bytes;
}
