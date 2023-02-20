#include "GeometryIO.h"

#include "fpzip/fpzip.h"


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#define USE_LZMA2


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#ifdef USE_LZMA2
#include "lzma/Lzma2Enc.h"
#include "lzma/Lzma2Dec.h"
#else
#include "lzma/LzmaEnc.h"
#include "lzma/LzmaDec.h"
#endif


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#define MAKE_TEXT(V) #V


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


extern fpzipError fpzip_errno;
extern const char* const fpzip_errstr[];


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


namespace __hidden_GeometryIOProcessor
{
	static const char ERRPrefixFPZIP[] = "fpzip: ";
	static const char ERRPrefixLZMA[] = "lzma: ";


	void* CurGeometryIOProcessorAlloc(CustomIO* _this, unsigned long long size)
	{
		return _this->CustomAlloc(size);
	}
	void CurGeometryIOProcessorFree(CustomIO* _this, void* p)
	{
		return _this->CustomFree(p);
	}

	
	thread_local CustomIO* FPZIPGeometryIOProcessor = nullptr;
	void* FPZIPGeometryIOProcessorAlloc(unsigned long long size)
	{
		return CurGeometryIOProcessorAlloc(FPZIPGeometryIOProcessor, size);
	}
	void FPZIPGeometryIOProcessorFree(void* p)
	{
		return CurGeometryIOProcessorFree(FPZIPGeometryIOProcessor, p);
	}
	

	struct ISzAllocForGeometry : public ISzAlloc
	{
		CustomIO* _this;
	};
	static void* LZMAAlloc(ISzAllocPtr raw, size_t size)
	{
		return CurGeometryIOProcessorAlloc(static_cast<const ISzAllocForGeometry*>(raw)->_this, size);
	}
	static void LZMAFree(ISzAllocPtr raw, void* address)
	{
		return CurGeometryIOProcessorFree(static_cast<const ISzAllocForGeometry*>(raw)->_this, address);
	}


#ifdef USE_LZMA2
	static SRes LZMA2Encode(Byte* dest, SizeT* destLen, Byte* prop, const Byte* src, SizeT srcLen, const CLzma2EncProps* props, ICompressProgress* progress, ISzAllocPtr alloc, ISzAllocPtr allocBig)
	{
		CLzma2EncHandle p = Lzma2Enc_Create(alloc, allocBig);
		if (!p)
		{
			return SZ_ERROR_MEM;
		}

		SRes res = SZ_OK;
		do
		{
			res = Lzma2Enc_SetProps(p, props);
			if (res != SZ_OK)
			{
				break;
			}

			(*prop) = Lzma2Enc_WriteProperties(p);
			
			Lzma2Enc_SetDataSize(p, srcLen);
			
			res = Lzma2Enc_Encode2(p, nullptr, dest, destLen, nullptr, src, srcLen, progress);
			if (res != SZ_OK)
			{
				break;
			}
		}
		while (false);

		Lzma2Enc_Destroy(p);
		return res;
	}
	static SRes LZMA2Decode(Byte* dest, SizeT* destLen, const Byte* src, SizeT* srcLen, Byte prop, ELzmaFinishMode finishMode, ELzmaStatus* status, ISzAllocPtr alloc)
	{
		CLzma2Dec p;
		Lzma2Dec_Construct(&p);

		SizeT outSize = *destLen;
		SizeT inSize = *srcLen;

		*destLen = 0u;
		*srcLen = 0u;

		*status = LZMA_STATUS_NOT_SPECIFIED;
		
		SRes res = Lzma2Dec_AllocateProbs(&p, prop, alloc);
		if (res != SZ_OK)
		{
			return res;
		}
		
		p.decoder.dic = dest;
		p.decoder.dicBufSize = outSize;
		Lzma2Dec_Init(&p);

		*srcLen = inSize;

		res = Lzma2Dec_DecodeToDic(&p, outSize, src, srcLen, finishMode, status);
		*destLen = p.decoder.dicPos;
		if (res == SZ_OK && *status == LZMA_STATUS_NEEDS_MORE_INPUT)
		{
			res = SZ_ERROR_INPUT_EOF;
		}
		
		Lzma2Dec_FreeProbs(&p, alloc);
		
		return res;
	}
#endif


	static void FPZIPGetErrorMsg(fpzipError Err, TempBuffer<char>* Buffer)
	{
		const char* Msg = fpzip_errstr[Err];

		const unsigned long long PrefixLen = strlen(ERRPrefixFPZIP);
		const unsigned long long MsgLen = strlen(Msg);
		const unsigned long long TotalLen = PrefixLen + MsgLen + 1;
		Buffer->Resize(TotalLen);
		Memcpy(Buffer->Get(), ERRPrefixFPZIP, PrefixLen);
		Memcpy(Buffer->Get() + TotalLen, Msg, MsgLen);
		*(Buffer->Get() + (TotalLen - 1)) = 0u;
	}
	
	static void LZMAGetErrorMsg(SRes Err, TempBuffer<char>* Buffer)
	{
		const char* Msg;
		switch(Err)
		{
		case SZ_ERROR_DATA:
			Msg = MAKE_TEXT(SZ_ERROR_DATA);
			break;
		case SZ_ERROR_MEM:
			Msg = MAKE_TEXT(SZ_ERROR_MEM);
			break;
		case SZ_ERROR_CRC:
			Msg = MAKE_TEXT(SZ_ERROR_CRC);
			break;
		case SZ_ERROR_UNSUPPORTED:
			Msg = MAKE_TEXT(SZ_ERROR_UNSUPPORTED);
			break;
		case SZ_ERROR_PARAM:
			Msg = MAKE_TEXT(SZ_ERROR_PARAM);
			break;
		case SZ_ERROR_INPUT_EOF:
			Msg = MAKE_TEXT(SZ_ERROR_INPUT_EOF);
			break;
		case SZ_ERROR_OUTPUT_EOF:
			Msg = MAKE_TEXT(SZ_ERROR_OUTPUT_EOF);
			break;
		case SZ_ERROR_READ:
			Msg = MAKE_TEXT(SZ_ERROR_READ);
			break;
		case SZ_ERROR_WRITE:
			Msg = MAKE_TEXT(SZ_ERROR_WRITE);
			break;
		case SZ_ERROR_PROGRESS:
			Msg = MAKE_TEXT(SZ_ERROR_PROGRESS);
			break;
		case SZ_ERROR_FAIL:
			Msg = MAKE_TEXT(SZ_ERROR_FAIL);
			break;
		case SZ_ERROR_THREAD:
			Msg = MAKE_TEXT(SZ_ERROR_THREAD);
			break;

		case SZ_ERROR_ARCHIVE:
			Msg = MAKE_TEXT(SZ_ERROR_ARCHIVE);
			break;
		case SZ_ERROR_NO_ARCHIVE:
			Msg = MAKE_TEXT(SZ_ERROR_NO_ARCHIVE);
			break;

		default:
			Msg = "";
			break;
		}

		const unsigned long long PrefixLen = strlen(ERRPrefixLZMA);
		const unsigned long long MsgLen = strlen(Msg);
		const unsigned long long TotalLen = PrefixLen + MsgLen + 1;
		Buffer->Resize(TotalLen);
		Memcpy(Buffer->Get(), ERRPrefixLZMA, PrefixLen);
		Memcpy(Buffer->Get() + TotalLen, Msg, MsgLen);
		*(Buffer->Get() + (TotalLen - 1)) = 0u;
	}


	static double Abs64(double V)
	{
		unsigned long long* CurBitsPtr = reinterpret_cast<unsigned long long*>(&V);
		unsigned long long CurBits = *CurBitsPtr;
		CurBits <<= 1u;
		CurBits >>= 1u;
		(*CurBitsPtr) = CurBits;
		return V;
	}
	static float Abs32(float V)
	{
		unsigned long* CurBitsPtr = reinterpret_cast<unsigned long*>(&V);
		unsigned long CurBits = *CurBitsPtr;
		CurBits <<= 1u;
		CurBits >>= 1u;
		(*CurBitsPtr) = CurBits;
		return V;
	}
	
	
	static double Rsqrt64_base(double V)
	{
		double X2 = V * 0.5;
		double Y = V;
		unsigned long long I = *reinterpret_cast<unsigned long long*>(&Y);
		I = 0x5fe6eb50c7b537a9 - (I >> 1);
		Y = *reinterpret_cast<double*>(&I);
		Y = Y * (1.5 - (X2 * Y * Y));
		return Y;
	}
	static float Rsqrt32_base(float V)
	{
		float X2 = V * 0.5f;
		float Y = V;
		unsigned long I = *reinterpret_cast<unsigned long*>(&Y);
		I = 0x5F3759DF - (I >> 1);
		Y = *reinterpret_cast<float*>(&I);
		Y = Y * (1.5f - (X2 * Y * Y));
		return Y;
	}
	
	static double Rsqrt64(double V)
	{
		double X2 = V * 0.5;
		double Y = Rsqrt64_base(V);

		// more newton iteration for accuracy
		Y = Y * (1.5 - (X2 * Y * Y));
		Y = Y * (1.5 - (X2 * Y * Y));

		return Y;
	}
	static float Rsqrt32(float V)
	{
		float X2 = V * 0.5f;
		float Y = Rsqrt32_base(V);

		// more newton iteration for accuracy
		Y = Y * (1.5f - (X2 * Y * Y));
		Y = Y * (1.5f - (X2 * Y * Y));

		return Y;
	}
	
	static double Sqrt64(double V)
	{
		double Y = 1. / Rsqrt64(V);

		return Y;
	}
	static float Sqrt32(float V)
	{
		float Y = 1.f / Rsqrt32(V);

		return Y;
	}
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool __common_memset(void* Dest, int V, size_t Len)
{
	return __hidden_GeometryIOProcessor::Memset(reinterpret_cast<unsigned char*>(Dest), static_cast<unsigned char>(V), Len);
}
bool __common_memmove(void* Dest, const void* Src, size_t Len)
{
	return __hidden_GeometryIOProcessor::Memmove(Dest, Src, Len);
}
bool __common_memcpy(void* _restrict Dest, const void* _restrict Src, size_t Len)
{
	return __hidden_GeometryIOProcessor::Memcpy(Dest, Src, Len);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void* __fpzip_alloc(size_t size)
{
	return __hidden_GeometryIOProcessor::FPZIPGeometryIOProcessorAlloc(size);
}
void __fpzip_free(void* p)
{
	return __hidden_GeometryIOProcessor::FPZIPGeometryIOProcessorFree(p);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool GeometryWriter::Encode(
	const double* Scale,
	const double* Rotation,
	const double* Position,
	unsigned long VertCount,
	unsigned long IndCount,
	const double* Verts,
	const unsigned long* Inds,
	unsigned long long* EncodedSize,
	unsigned char** EncodedData,

	unsigned long OptionalEncodeOffset,
	bool OptionalUseFloat32Vertex
	)
{
	{
		const SizeT VertLen = (VertCount << 3u);
		const SizeT IndLen = (IndCount << 2u);
		
		SizeT InitLen = ((3u + 4u + 3u) << 3u);
		InitLen += 4u + 4u + 8u + 8u;
		InitLen += VertLen;
		InitLen += IndLen;
		
		TempSrcForEncoding.Resize(InitLen);
		unsigned char* Ptr = TempSrcForEncoding.Get();

		__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, Scale, 3u << 3u);
		__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, Rotation, 4u << 3u);
		__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, Position, 3u << 3u);
		
		__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, &VertCount, 4u);
		__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, &IndCount, 4u);
		
		__hidden_GeometryIOProcessor::MemsetAndMove(Ptr, static_cast<unsigned char>(0), 8u);
		__hidden_GeometryIOProcessor::MemsetAndMove(Ptr, static_cast<unsigned char>(0), 8u);

		__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, Verts, VertLen);
		__hidden_GeometryIOProcessor::Memcpy(Ptr, Inds, IndLen);
	}
	
	if (!Pack(OptionalUseFloat32Vertex))
	{
		return false;
	}

	{
		__hidden_GeometryIOProcessor::ISzAllocForGeometry allocator;
		{
			allocator.Alloc = __hidden_GeometryIOProcessor::LZMAAlloc;
			allocator.Free = __hidden_GeometryIOProcessor::LZMAFree;
			allocator._this = this;
		}

#ifdef USE_LZMA2
		static const unsigned long PropSize = 1u;
#else
		static const unsigned long PropSize = LZMA_PROPS_SIZE;
#endif

		const SizeT srcLen = TempSrcForEncoding.Size();
		SizeT destLen = srcLen;
		{
			destLen += destLen / 3 + 128u;
			
			TempDestForEncoding.Resize(8u + PropSize + destLen);
		}

#ifdef USE_LZMA2
		CLzma2EncProps props;
		Lzma2EncProps_Init(&props);
		props.lzmaProps.level = 5;
		props.lzmaProps.lc = 3;
		props.lzmaProps.lp = 0;
		props.lzmaProps.pb = 2;
		props.lzmaProps.fb = 32;
		props.lzmaProps.numThreads = 8;
		
		SRes res = __hidden_GeometryIOProcessor::LZMA2Encode(TempDestForEncoding.Get() + 8u + PropSize, &destLen, TempDestForEncoding.Get() + 8u, TempSrcForEncoding.Get(), srcLen, &props, nullptr, &allocator, &allocator);
#else
		CLzmaEncProps props;
		LzmaEncProps_Init(&props);
		props.level = 5;
		props.lc = 3;
		props.lp = 0;
		props.pb = 2;
		props.fb = 32;
		props.numThreads = 2;

		SizeT propsSize = PropSize;
		
		SRes res = LzmaEncode(TempDestForEncoding.Get() + 8u + PropSize, &destLen, TempSrcForEncoding.Get(), srcLen, &props, TempDestForEncoding.Get() + 8u, &propsSize, 0, nullptr, &allocator, &allocator);
#endif
		if (res != SZ_OK)
		{
			__hidden_GeometryIOProcessor::LZMAGetErrorMsg(res, &ErrorMsg);
			return false;
		}

		{
			if ((srcLen + OptionalEncodeOffset) <= destLen)
			{
				const unsigned long long BufferSize = srcLen | 0x8000000000000000;
			
				__hidden_GeometryIOProcessor::Memcpy(TempDestForEncoding.Get(), &BufferSize, 8u);
				__hidden_GeometryIOProcessor::Memcpy(TempDestForEncoding.Get() + 8u, TempSrcForEncoding.Get(), TempSrcForEncoding.Size());
				TempDestForEncoding.Resize(8u + TempSrcForEncoding.Size());
			}
			else
			{
				const unsigned long long BufferSize = srcLen & 0x7FFFFFFFFFFFFFFF;
			
				__hidden_GeometryIOProcessor::Memcpy(TempDestForEncoding.Get(), &BufferSize, 8u);
				TempDestForEncoding.Resize(8u + PropSize + destLen);
			}
		}
	}

	{
		(*EncodedSize) = static_cast<unsigned long long>(TempDestForEncoding.Size());
		(*EncodedData) = TempDestForEncoding.Get();
	}

	return true;
}

bool GeometryReader::Decode(
	unsigned long long EncodedSize,
	const unsigned char* EncodedData,
	double* Scale,
	double* Rotation,
	double* Position,
	unsigned long* VertCount,
	unsigned long* IndCount,
	double** Verts,
	unsigned long** Inds
	)
{
	unsigned long long BufferSize = *reinterpret_cast<const unsigned long long*>(EncodedData);
	const bool bNeedDecode = ((BufferSize & 0x8000000000000000) == 0u);
	BufferSize &= 0x7FFFFFFFFFFFFFFF;
	EncodedData += 8u;

	const unsigned char* RawPtr; 
	if (bNeedDecode)
	{
		__hidden_GeometryIOProcessor::ISzAllocForGeometry allocator;
		{
			allocator.Alloc = __hidden_GeometryIOProcessor::LZMAAlloc;
			allocator.Free = __hidden_GeometryIOProcessor::LZMAFree;
			allocator._this = this;
		}

#ifdef USE_LZMA2
		static const unsigned long PropSize = 1u;
#else
		static const unsigned long PropSize = LZMA_PROPS_SIZE;
#endif
		
		TempSrcForDecoding.Resize(BufferSize);

		SizeT srcLen = EncodedSize - 8u - PropSize;
		SizeT destLen = TempSrcForDecoding.Size();

#ifdef USE_LZMA2
		ELzmaStatus status;
		SRes res = __hidden_GeometryIOProcessor::LZMA2Decode(TempSrcForDecoding.Get(), &destLen, EncodedData + PropSize, &srcLen, *EncodedData, LZMA_FINISH_ANY, &status, &allocator);
#else
		ELzmaStatus status;
		SRes res = LzmaDecode(TempSrcForDecoding.Get(), &destLen, EncodedData + PropSize, &srcLen, EncodedData, PropSize, LZMA_FINISH_ANY, &status, &allocator);
#endif
		if (res != SZ_OK)
		{
			__hidden_GeometryIOProcessor::LZMAGetErrorMsg(res, &ErrorMsg);
			return false;
		}

		RawPtr = TempSrcForDecoding.Get();
	}
	else
	{
		RawPtr = EncodedData;
	}

	if (!Unpack(RawPtr, Scale, Rotation, Position, VertCount, IndCount))
	{
		return false;
	}

	{
		unsigned char* Ptr = TempDestForDecoding.Get();

		(*Verts) = reinterpret_cast<double*>(Ptr);
		Ptr += ((*VertCount) << 3u);
		(*Inds) =  reinterpret_cast<unsigned long*>(Ptr);
	}

	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool GeometryWriter::Pack(bool bUseFloat32)
{
	unsigned char* Ptr = TempSrcForEncoding.Get();

	const double* Scale = reinterpret_cast<const double*>(Ptr);
	Ptr += (3u << 3u);
	const double* Rotation = reinterpret_cast<const double*>(Ptr);
	Ptr += (4u << 3u);
	const double* Position = reinterpret_cast<const double*>(Ptr);
	Ptr += (3u << 3u);

	const unsigned long VertCount = *reinterpret_cast<unsigned long*>(Ptr);
	Ptr += 4u;
	const unsigned long IndCount = *reinterpret_cast<unsigned long*>(Ptr);
	Ptr += 4u;

	unsigned long long* PackVertCount = reinterpret_cast<unsigned long long*>(Ptr);
	Ptr += 8u;
	unsigned long long* PackIndCount = reinterpret_cast<unsigned long long*>(Ptr);
	Ptr += 8u;

	unsigned char* Verts = Ptr;
	Ptr += VertCount << 3u;
	unsigned char* Inds = Ptr;

	if (!bUseFloat32)
	{
		bUseFloat32 = ShouldConvertToFloat(VertCount, IndCount, reinterpret_cast<const double*>(Verts), reinterpret_cast<const unsigned long*>(Inds));
	}
	
	if (!PackVerts(bUseFloat32, VertCount, PackVertCount, Verts))
	{
		return false;
	}
	PackInds(VertCount, IndCount, PackIndCount, Inds);

	{
		const unsigned long long PackVertCountPure =(*PackVertCount) & 0x7FFFFFFFFFFFFFFF; 
		
		unsigned char* OldInds = Inds;
		Inds = Verts + PackVertCountPure;

		for (unsigned char* EndInds = Inds + (*PackIndCount); Inds < EndInds; ++OldInds, ++Inds)
		{
			(*Inds) = (*OldInds);
		}

		TempSrcForEncoding.Resize(((3u + 4u + 3u) << 3u) + 4u + 4u + 8u + 8u + PackVertCountPure + (*PackIndCount));
	}

	return true;
}

bool GeometryReader::Unpack(const unsigned char* Ptr, double* Scale, double* Rotation, double* Position, unsigned long* VertCount, unsigned long* IndCount)
{
	__hidden_GeometryIOProcessor::Memcpy(Scale, Ptr, 3u << 3u);
	Ptr += (3u << 3u);
	__hidden_GeometryIOProcessor::Memcpy(Rotation, Ptr, 4u << 3u);
	Ptr += (4u << 3u);
	__hidden_GeometryIOProcessor::Memcpy(Position, Ptr, 3u << 3u);
	Ptr += (3u << 3u);
	
	(*VertCount) = *reinterpret_cast<const unsigned long*>(Ptr);
	Ptr += 4u;
	(*IndCount) = *reinterpret_cast<const unsigned long*>(Ptr);
	Ptr += 4u;

	unsigned long long PackVertCount = *reinterpret_cast<const unsigned long long*>(Ptr);
	Ptr += 8u;
	unsigned long long PackIndCount = *reinterpret_cast<const unsigned long long*>(Ptr);
	Ptr += 8u;

	const bool bFloatInRange = (PackVertCount & 0x8000000000000000) != 0u;
	PackVertCount &= 0x7FFFFFFFFFFFFFFF;
	
	const unsigned char* InVerts = Ptr;
	Ptr += PackVertCount;
	const unsigned char* InInds = Ptr;

	TempDestForDecoding.Resize(((*VertCount) << 3u) + ((*IndCount) << 2u));

	unsigned char* OutVerts = TempDestForDecoding.Get();
	unsigned char* OutInds = TempDestForDecoding.Get() + ((*VertCount) << 3u);
	
	if (!UnpackVerts(*VertCount, InVerts, bFloatInRange, OutVerts))
	{
		return false;
	}
	UnpackInds(*VertCount, *IndCount, InInds, OutInds);

	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool GeometryWriter::ShouldConvertToFloat(unsigned long VertCount, unsigned long IndCount, const double* Verts, const unsigned long* Inds)
{
	bool bFloatInRange = true;
	do
	{
		for (const double *i = Verts, *e = Verts + VertCount; i != e; ++i)
		{
			const double CurValue64 = __hidden_GeometryIOProcessor::Abs64(*i);
			if (CurValue64 <= FLT_MIN)
			{
				bFloatInRange = false;
				break;
			}
			else if (CurValue64 >= 8796093022208.0)
			{
				bFloatInRange = false;
				break;	
			}

			const float CurValue32 = static_cast<float>(CurValue64);

			const double Diff = __hidden_GeometryIOProcessor::Abs64(CurValue64 - CurValue32);
			if (Diff <= FLT_EPSILON)
			{
				bFloatInRange = false;
				break;
			}
		}
		if (!bFloatInRange)
		{
			break;
		}

		for (const unsigned long *i = Inds, *e = Inds + IndCount; i != e; i += 3u)
		{
			double Magnitude64;
			{
				double _0X, _0Y, _0Z;
				{
					const double* Ptr = Verts + ((*i) * 3u);

					_0X = *Ptr;
					_0Y = *(Ptr + 1u);
					_0Z = *(Ptr + 2u);
				}

				double _1X, _1Y, _1Z;
				{
					const double* Ptr = Verts + ((*(i + 1u)) * 3u);

					_1X = *Ptr;
					_1Y = *(Ptr + 1u);
					_1Z = *(Ptr + 2u);
				}

				double _2X, _2Y, _2Z;
				{
					const double* Ptr = Verts + ((*(i + 2u)) * 3u);

					_2X = *Ptr;
					_2Y = *(Ptr + 1u);
					_2Z = *(Ptr + 2u);
				}

				const double d0X = _1X - _0X;
				const double d0Y = _1Y - _0Y;
				const double d0Z = _1Z - _0Z;

				const double d1X = _2X - _0X;
				const double d1Y = _2Y - _0Y;
				const double d1Z = _2Z - _0Z;

				const double cX = d0Y * d1Z - d0Z * d1Y;
				const double cY = d0Z * d1X - d0X * d1Z;
				const double cZ = d0X * d1Y - d0Y * d1X;

				double Magnitude = cX * cX + cY * cY + cZ * cZ;
				Magnitude = __hidden_GeometryIOProcessor::Sqrt64(Magnitude);
				Magnitude *= 0.5;

				if (Magnitude <= FLT_MIN)
				{
					bFloatInRange = false;
					break;
				}
				else if (Magnitude >= 8796093022208.0)
				{
					bFloatInRange = false;
					break;	
				}

				Magnitude64 = Magnitude;
			}

			float Magnitude32;
			{
				float _0X, _0Y, _0Z;
				{
					const double* Ptr = Verts + ((*i) * 3u);

					_0X = static_cast<float>(*Ptr);
					_0Y = static_cast<float>(*(Ptr + 1u));
					_0Z = static_cast<float>(*(Ptr + 2u));
				}

				float _1X, _1Y, _1Z;
				{
					const double* Ptr = Verts + ((*(i + 1u)) * 3u);

					_1X = static_cast<float>(*Ptr);
					_1Y = static_cast<float>(*(Ptr + 1u));
					_1Z = static_cast<float>(*(Ptr + 2u));
				}

				float _2X, _2Y, _2Z;
				{
					const double* Ptr = Verts + ((*(i + 2u)) * 3u);

					_2X = static_cast<float>(*Ptr);
					_2Y = static_cast<float>(*(Ptr + 1u));
					_2Z = static_cast<float>(*(Ptr + 2u));
				}

				const float d0X = _1X - _0X;
				const float d0Y = _1Y - _0Y;
				const float d0Z = _1Z - _0Z;

				const float d1X = _2X - _0X;
				const float d1Y = _2Y - _0Y;
				const float d1Z = _2Z - _0Z;

				const float cX = d0Y * d1Z - d0Z * d1Y;
				const float cY = d0Z * d1X - d0X * d1Z;
				const float cZ = d0X * d1Y - d0Y * d1X;

				float Magnitude = cX * cX + cY * cY + cZ * cZ;
				Magnitude = __hidden_GeometryIOProcessor::Sqrt32(Magnitude);
				Magnitude *= 0.5f;

				Magnitude32 = Magnitude;
			}

			const double Diff = __hidden_GeometryIOProcessor::Abs64(Magnitude64 - Magnitude32);
			if (Diff <= FLT_EPSILON)
			{
				bFloatInRange = false;
				break;
			}
		}
	}
	while (false);

	return bFloatInRange;
}
bool GeometryWriter::PackVerts(bool bFloatInRange, unsigned long SrcCount, unsigned long long* DestCount, void* Data)
{
	__hidden_GeometryIOProcessor::FPZIPGeometryIOProcessor = this;

	const double* SrcData = static_cast<const double*>(Data);

	FPZ* fpz;
	if (bFloatInRange)
	{
		TempDestForEncoding.Resize(1024u + (SrcCount << 2u));

		const double* Src = reinterpret_cast<const double*>(Data);
		float* Dest = reinterpret_cast<float*>(Data);
		for (const double* SrcEnd = reinterpret_cast<const double*>(Data) + SrcCount; Src != SrcEnd; ++Src, ++Dest)
		{
			*Dest = static_cast<float>(*Src);
		}

		fpz = fpzip_write_to_buffer(TempDestForEncoding.Get(), TempDestForEncoding.Size());
		fpz->type = FPZIP_TYPE_FLOAT;
	}
	else
	{
		TempDestForEncoding.Resize(1024u + (SrcCount << 3u));

		fpz = fpzip_write_to_buffer(TempDestForEncoding.Get(), TempDestForEncoding.Size());
		fpz->type = FPZIP_TYPE_DOUBLE;
	}
	
	fpz->nx = static_cast<decltype(fpz->nx)>(SrcCount);
	fpz->ny = 1;
	fpz->nz = 1;
	fpz->nf = 1;

	bool bRet = true;
	const size_t outBytes = fpzip_write(fpz, Data);
	if (fpzip_errno != fpzipSuccess)
	{
		bRet = false;
		__hidden_GeometryIOProcessor::FPZIPGetErrorMsg(fpzip_errno, &ErrorMsg);
	}
	
	fpzip_write_close(fpz);
	TempDestForEncoding.Resize(outBytes);

	if (!bRet)
	{
		return false;
	}
	
	if (bFloatInRange)
	{
		__hidden_GeometryIOProcessor::Memcpy(Data, TempDestForEncoding.Get(), TempDestForEncoding.Size());
		(*DestCount) = static_cast<unsigned long long>(TempDestForEncoding.Size()) | 0x8000000000000000;
	}
	else
	{
		__hidden_GeometryIOProcessor::Memcpy(Data, TempDestForEncoding.Get(), TempDestForEncoding.Size());
		(*DestCount) = static_cast<unsigned long long>(TempDestForEncoding.Size()) & 0x7FFFFFFFFFFFFFFF;
	}

	return true;
}

bool GeometryReader::UnpackVerts(unsigned long SrcCount, const void* InData, bool bFloatInRange, void* OutData)
{
	__hidden_GeometryIOProcessor::FPZIPGeometryIOProcessor = this;

	FPZ* fpz = fpzip_read_from_buffer(InData);

	if (bFloatInRange)
	{
		fpz->type = FPZIP_TYPE_FLOAT;
	}
	else
	{
		fpz->type = FPZIP_TYPE_DOUBLE;
	}
	
	fpz->nx = static_cast<decltype(fpz->nx)>(SrcCount);
	fpz->ny = 1;
	fpz->nz = 1;
	fpz->nf = 1;

	bool bRet = true;
	fpzip_read(fpz, OutData);
	if (fpzip_errno != fpzipSuccess)
	{
		bRet = false;
		__hidden_GeometryIOProcessor::FPZIPGetErrorMsg(fpzip_errno, &ErrorMsg);
	}
	
	fpzip_read_close(fpz);

	if (!bRet)
	{
		return false;
	}

	if (bFloatInRange)
	{
		const float* Src = reinterpret_cast<const float*>(OutData) + SrcCount - 1u;
		double* Dest = reinterpret_cast<double*>(OutData) + SrcCount - 1u;
		for (unsigned long i = 0u; i < SrcCount; ++i, --Src, --Dest)
		{
			(*Dest) = static_cast<double>(*Src);
		}
	}

	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void GeometryWriter::PackInds(unsigned long VertCount, unsigned long SrcCount, unsigned long long* DestCount, void* Data)
{
	unsigned long RequireBitsPerSingle = 0u;
	{
		long long i = VertCount;
		while (i > 0u)
		{
			i >>= 1u;
			++RequireBitsPerSingle;
		}
	}

	unsigned long long TotalRequireBits = RequireBitsPerSingle;
	TotalRequireBits *= SrcCount;

	const unsigned long long TotalRequireBytes = (TotalRequireBits + 7u) >> 3u;

	TempDestForEncoding.Resize(TotalRequireBytes, 0u);

	unsigned long long BitOffset = 0u;
	const unsigned long* SrcIndPtr = static_cast<const unsigned long*>(Data);
	const unsigned long* SrcIndEndPtr = static_cast<const unsigned long*>(Data) + SrcCount;
	for (unsigned long j = 0u; SrcIndPtr != SrcIndEndPtr; ++SrcIndPtr, ++j)
	{
		const unsigned long SrcInd = (*SrcIndPtr);
		for (unsigned long i = 0u; i < RequireBitsPerSingle; ++i)
		{
			const unsigned long long CurBit = (SrcInd >> i) & 1u;
			TempDestForEncoding[BitOffset >> 3] |= (CurBit << (BitOffset & 7u));
			++BitOffset;
		}
	}
	
	(*DestCount) = static_cast<unsigned long long>(TempDestForEncoding.Size());
	__hidden_GeometryIOProcessor::Memcpy(Data, TempDestForEncoding.Get(), TempDestForEncoding.Size());
}

void GeometryReader::UnpackInds(unsigned long VertCount, unsigned long SrcCount, const void* InData, void* OutData)
{
	unsigned long RequireBitsPerSingle = 0u;
	{
		long long i = VertCount;
		while (i > 0u)
		{
			i >>= 1u;
			++RequireBitsPerSingle;
		}
	}

	unsigned long long BitOffset = 0u;
	unsigned long* DestIndPtr = reinterpret_cast<unsigned long*>(OutData);
	const unsigned long* DestIndEndPtr = reinterpret_cast<const unsigned long*>(OutData) + SrcCount;
	for (unsigned long j = 0u; DestIndPtr != DestIndEndPtr; ++DestIndPtr, ++j)
	{
		unsigned long long CurInd = 0u;
		for (unsigned long i = 0u; i < RequireBitsPerSingle; ++i)
		{
			const unsigned long long CurBit = (reinterpret_cast<const unsigned char*>(InData)[BitOffset >> 3u] >> (BitOffset & 7u)) & 1u;
			CurInd |= CurBit << i;
			++BitOffset;
		}
		(*DestIndPtr) = static_cast<unsigned long>(CurInd);
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool GeometryStreamWriter::BeginWrite(void* _Handle)
{
	if (Handle)
	{
		return false;
	}
	Handle = _Handle;
	
	{
		const unsigned long long Dummy = static_cast<unsigned long long>(-1);

		if (!CustomTell(Handle, &FileBegin))
		{
			return false;
		}
		
		if (!CustomWrite(Handle, sizeof(Dummy), &Dummy))
		{
			return false;
		}

		GeometryCount = 0u;
	}

	return true;
}

bool GeometryStreamReader::BeginRead(void* _Handle)
{
	if (Handle)
	{
		return false;
	}
	Handle = _Handle;

	unsigned long long HeaderPos = static_cast<unsigned long long>(-1);
	if (!CustomRead(Handle, sizeof(HeaderPos), &HeaderPos))
	{
		return false;
	}
	if (!CustomTell(Handle, &FileBegin))
	{
		return false;
	}

	const bool bEncodedHeader = ((HeaderPos & 0x8000000000000000) != 0u);
	HeaderPos &= 0x7FFFFFFFFFFFFFFF;
	if (!CustomJump(Handle, HeaderPos))
	{
		return false;
	}

	{
		if (bEncodedHeader)
		{
			__hidden_GeometryIOProcessor::ISzAllocForGeometry allocator;
			{
				allocator.Alloc = __hidden_GeometryIOProcessor::LZMAAlloc;
				allocator.Free = __hidden_GeometryIOProcessor::LZMAFree;
				allocator._this = this;
			}

#ifdef USE_LZMA2
			static const unsigned long PropSize = 1u;
#else
			static const unsigned long PropSize = LZMA_PROPS_SIZE;
#endif
			
			struct
			{
				unsigned long DestSize;
				unsigned long SrcSize;
				unsigned char Prop[PropSize];
			}
			PreHeader;
			if (!CustomRead(Handle, 8u + PropSize, &PreHeader))
			{
				return false;
			}

			Temporal.Resize(PreHeader.SrcSize + PreHeader.DestSize);
			if (!CustomRead(Handle, PreHeader.SrcSize, Temporal.Get()))
			{
				return false;
			}

			SizeT SrcLen = PreHeader.SrcSize;
			SizeT DestLen = PreHeader.DestSize;
			
			unsigned char* PtrSrc = Temporal.Get();
			unsigned char* PtrDest = Temporal.Get() + PreHeader.SrcSize;

#ifdef USE_LZMA2
			ELzmaStatus status;
			SRes res = __hidden_GeometryIOProcessor::LZMA2Decode(PtrDest, &DestLen, PtrSrc, &SrcLen, PreHeader.Prop[0], LZMA_FINISH_ANY, &status, &allocator);
#else
			ELzmaStatus status;
			SRes res = LzmaDecode(PtrDest, &DestLen, PtrSrc, &SrcLen, PreHeader.Prop, PropSize, LZMA_FINISH_ANY, &status, &allocator);
#endif
			if (res != SZ_OK)
			{
				__hidden_GeometryIOProcessor::LZMAGetErrorMsg(res, &ErrorMsg);
				return false;
			}

			unsigned long long GeometryCount;
			__hidden_GeometryIOProcessor::Memcpy(&GeometryCount, PtrDest, sizeof(GeometryCount));
			PtrDest += sizeof(GeometryCount);

			if (GeometryCount == static_cast<unsigned long long>(-1))
			{
				return false;
			}

			HeaderNames.Resize(GeometryCount);
			HeaderMinMaxes.Resize(GeometryCount);

			HeaderRawNames.Resize(GeometryCount << 1u);
			HeaderRawNames.Resize(0u);
			for (unsigned long long i = 0u; i < GeometryCount; ++i)
			{
				wchar_t Chr = 0;
				do
				{
					__hidden_GeometryIOProcessor::Memcpy(&Chr, PtrDest, sizeof(Chr));
					PtrDest += sizeof(Chr);

					HeaderRawNames.Resize(HeaderRawNames.Size() + 1u);
					HeaderRawNames[HeaderRawNames.Size() - 1u] = Chr;
				}
				while (Chr != 0);
			}

			__hidden_GeometryIOProcessor::Memcpy(HeaderMinMaxes.Get(), PtrDest, GeometryCount * sizeof(__hidden_GeometryIOProcessor::MinMax));
		}
		else
		{
			unsigned long long GeometryCount = static_cast<unsigned long long>(-1);
			if (!CustomRead(Handle, sizeof(GeometryCount), &GeometryCount))
			{
				return false;
			}

			if (GeometryCount == static_cast<unsigned long long>(-1))
			{
				return false;
			}

			HeaderNames.Resize(GeometryCount);
			HeaderMinMaxes.Resize(GeometryCount);

			HeaderRawNames.Resize(GeometryCount << 1u);
			HeaderRawNames.Resize(0u);
			for (unsigned long long i = 0u; i < GeometryCount; ++i)
			{
				wchar_t Chr = 0;
				do
				{
					if (!CustomRead(Handle, sizeof(Chr), &Chr))
					{
						return false;
					}

					HeaderRawNames.Resize(HeaderRawNames.Size() + 1u);
					HeaderRawNames[HeaderRawNames.Size() - 1u] = Chr;
				}
				while (Chr != 0);
			}

			if (!CustomRead(Handle, GeometryCount * sizeof(__hidden_GeometryIOProcessor::MinMax), HeaderMinMaxes.Get()))
			{
				return false;
			}			
		}

		{
			static const wchar_t Dummy = 0;
			
			const wchar_t** Names = HeaderNames.Get();
			for (const wchar_t *PtrPrev = &Dummy, *Ptr = HeaderRawNames.Get(), *PtrEnd = HeaderRawNames.Get() + HeaderRawNames.Size(); Ptr != PtrEnd; PtrPrev = Ptr, ++Ptr)
			{
				if ((*PtrPrev) == 0)
				{
					(*Names) = Ptr;
					++Names;
				}
			}
		}
	}

	if (!CustomJump(Handle, FileBegin))
	{
		return false;
	}

	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool GeometryStreamWriter::EndWrite()
{
	if (!Handle)
	{
		return false;
	}

	unsigned long long HeaderPos = static_cast<unsigned long long>(-1);
	if (!CustomTell(Handle, &HeaderPos))
	{
		return false;
	}

	{
		__hidden_GeometryIOProcessor::ISzAllocForGeometry allocator;
		{
			allocator.Alloc = __hidden_GeometryIOProcessor::LZMAAlloc;
			allocator.Free = __hidden_GeometryIOProcessor::LZMAFree;
			allocator._this = this;
		}

#ifdef USE_LZMA2
		static const unsigned long PropSize = 1u;
#else
		static const unsigned long PropSize = LZMA_PROPS_SIZE;
#endif

		SizeT SrcSize = sizeof(GeometryCount) + (HeaderNames.Size() * sizeof(wchar_t)) + (HeaderMinMaxes.Size() * sizeof(__hidden_GeometryIOProcessor::MinMax));
		SizeT DestSize = SrcSize + (SrcSize / 3 + 128u);
		Temporal.Resize(SrcSize + 8u + PropSize + DestSize);
		{
			unsigned char* Ptr = Temporal.Get();
			
			__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, &GeometryCount, sizeof(GeometryCount));
			__hidden_GeometryIOProcessor::MemcpyAndMove(Ptr, HeaderNames.Get(), HeaderNames.Size() * sizeof(wchar_t));
			__hidden_GeometryIOProcessor::Memcpy(Ptr, HeaderMinMaxes.Get(), HeaderMinMaxes.Size() * sizeof(__hidden_GeometryIOProcessor::MinMax));
		}

#ifdef USE_LZMA2
		CLzma2EncProps props;
		Lzma2EncProps_Init(&props);
		props.lzmaProps.level = 5;
		props.lzmaProps.lc = 3;
		props.lzmaProps.lp = 0;
		props.lzmaProps.pb = 2;
		props.lzmaProps.fb = 32;
		props.lzmaProps.numThreads = 8;
		
		SRes res = __hidden_GeometryIOProcessor::LZMA2Encode(Temporal.Get() + SrcSize + 8u + PropSize, &DestSize, Temporal.Get() + SrcSize + 8u, Temporal.Get(), SrcSize, &props, nullptr, &allocator, &allocator);
#else
		CLzmaEncProps props;
		LzmaEncProps_Init(&props);
		props.level = 5;
		props.lc = 3;
		props.lp = 0;
		props.pb = 2;
		props.fb = 32;
		props.numThreads = 2;

		SizeT propsSize = PropSize;
		
		SRes res = LzmaEncode(Temporal.Get() + SrcSize + 8u + PropSize, &DestSize, Temporal.Get(), SrcSize, &props, Temporal.Get() + SrcSize + 8u, &propsSize, 0, nullptr, &allocator, &allocator);
#endif
		if (res != SZ_OK)
		{
			__hidden_GeometryIOProcessor::LZMAGetErrorMsg(res, &ErrorMsg);
			return false;
		}

		if (SrcSize > (8u + PropSize + DestSize))
		{
			HeaderPos |= 0x8000000000000000;

			unsigned char* Ptr = Temporal.Get() + SrcSize;
			{
				unsigned long WriteSize = static_cast<unsigned long>(SrcSize);
				__hidden_GeometryIOProcessor::Memcpy(Ptr, &WriteSize, 4u);
				
				WriteSize = static_cast<unsigned long>(DestSize);
				__hidden_GeometryIOProcessor::Memcpy(Ptr + 4u, &WriteSize, 4u);
			}

			if (!CustomWrite(Handle, 8u + PropSize + DestSize, Ptr))
			{
				return false;
			}
		}
		else
		{
			HeaderPos &= 0x7FFFFFFFFFFFFFFF;
			
			if (!CustomWrite(Handle, SrcSize, Temporal.Get()))
			{
				return false;
			}
		}
	}

	unsigned long long LastPos = static_cast<unsigned long long>(-1);
	if (!CustomTell(Handle, &LastPos))
	{
		return false;
	}

	if (!CustomJump(Handle, FileBegin))
	{
		return false;
	}
	if (!CustomWrite(Handle, sizeof(HeaderPos), &HeaderPos))
	{
		return false;
	}

	if (!CustomJump(Handle, LastPos))
	{
		return false;
	}
	
	return true;
}

bool GeometryStreamReader::EndRead()
{
	if (!Handle)
	{
		return false;
	}
	
	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


unsigned long long GeometryStreamWriter::EmplaceGeometry(
	const wchar_t* ID,
	const double* Scale,
	const double* Rotation,
	const double* Position,
	unsigned long VertCount,
	unsigned long IndCount,
	const double* Verts,
	const unsigned long* Inds,

	unsigned long OptionalEncodeOffset,
	bool OptionalUseFloat32Vertex
	)
{
	unsigned long long EncodedSize;
	unsigned char* EncodedData;
	if (!Encode(Scale, Rotation, Position, VertCount, IndCount, Verts, Inds, &EncodedSize, &EncodedData, OptionalEncodeOffset, OptionalUseFloat32Vertex))
	{
		return static_cast<unsigned long long>(-1);
	}

	{
		if (!CustomWrite(Handle, sizeof(EncodedSize), &EncodedSize))
		{
			return static_cast<unsigned long long>(-1);
		}

		if (!CustomWrite(Handle, EncodedSize, EncodedData))
		{
			return static_cast<unsigned long long>(-1);
		}
	}
	
	__hidden_GeometryIOProcessor::MinMax GeometryMinMax;
	{
		Temporal.Resize(VertCount << 3u);

		double GeometryMin[] = { DBL_MAX, DBL_MAX, DBL_MAX };
		double GeometryMax[] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
		{
			const double* Src = Verts;
			double* Dest = reinterpret_cast<double*>(Temporal.Get());
			for (const double* SrcEnd = Src + VertCount; Src != SrcEnd; Src += 3u, Dest += 3u)
			{
				{
					Dest[0] = Src[0] * Scale[0];
					Dest[1] = Src[1] * Scale[1];
					Dest[2] = Src[2] * Scale[2];
				}

				{ // http://people.csail.mit.edu/bkph/articles/Quaternions.pdf
					double TTX = 2. * (Rotation[1] * Dest[2] - Rotation[2] * Dest[1]);
					double TTY = 2. * (Rotation[2] * Dest[0] - Rotation[0] * Dest[2]);
					double TTZ = 2. * (Rotation[0] * Dest[1] - Rotation[1] * Dest[0]);

					const double TT2X = 2. * (Rotation[1] * TTZ - Rotation[2] * TTY);
					const double TT2Y = 2. * (Rotation[2] * TTX - Rotation[0] * TTZ);
					const double TT2Z = 2. * (Rotation[0] * TTY - Rotation[1] * TTX);

					TTX *= Rotation[3];
					TTY *= Rotation[3];
					TTZ *= Rotation[3];

					Dest[0] += TTX + TT2X;
					Dest[1] += TTY + TT2Y;
					Dest[2] += TTZ + TT2Z;
				}

				{
					Dest[0] += Position[0];
					Dest[1] += Position[1];
					Dest[2] += Position[2];
				}
			}
		}

		{
			const unsigned long* Src = Inds;
			for (const unsigned long* SrcEnd = Src + IndCount; Src != SrcEnd; ++Src)
			{
				double X, Y, Z;
				{
					const double* Ptr = &(reinterpret_cast<const double*>(Temporal.Get())[(*Src) * 3u]);

					X = *Ptr;
					Y = *(Ptr + 1u);
					Z = *(Ptr + 2u);
				}

				GeometryMin[0] = (GeometryMin[0] > X) ? X : GeometryMin[0];
				GeometryMin[1] = (GeometryMin[1] > Y) ? Y : GeometryMin[1];
				GeometryMin[2] = (GeometryMin[2] > Z) ? Z : GeometryMin[2];

				GeometryMax[0] = (GeometryMax[0] < X) ? X : GeometryMax[0];
				GeometryMax[1] = (GeometryMax[1] < Y) ? Y : GeometryMax[1];
				GeometryMax[2] = (GeometryMax[2] < Z) ? Z : GeometryMax[2];
			}
		}

		__hidden_GeometryIOProcessor::Memcpy(GeometryMinMax.Min, GeometryMin, sizeof(GeometryMin));
		__hidden_GeometryIOProcessor::Memcpy(GeometryMinMax.Max, GeometryMax, sizeof(GeometryMax));
	}
	
	{
		const unsigned long long IDLen = wcslen(ID);
		const unsigned long long OldSize = HeaderNames.Size();
		
		HeaderNames.Resize(OldSize + IDLen + 1u);

		__hidden_GeometryIOProcessor::Memcpy(HeaderNames.Get() + OldSize, ID, IDLen * sizeof(wchar_t));
		*(HeaderNames.Get() + HeaderNames.Size() - 1u) = 0u;
	}
	{
		const unsigned long long OldSize = HeaderMinMaxes.Size();
		
		HeaderMinMaxes.Resize(OldSize + 1u);
		__hidden_GeometryIOProcessor::Memcpy(HeaderMinMaxes.Get() + OldSize, &GeometryMinMax, sizeof(GeometryMinMax));
	}
	
	return ++GeometryCount;
}

bool GeometryStreamReader::GetGeometry(
	unsigned long Index,
	double* Scale,
	double* Rotation,
	double* Position,
	unsigned long* VertCount,
	unsigned long* IndCount,
	double** Verts,
	unsigned long** Inds
	)
{
	if (!CustomJump(Handle, FileBegin))
	{
		return false;
	}
	
	for (unsigned long i = 0u; i < Index; ++i)
	{
		unsigned long long EncodedSize = 0u;
		if (!CustomRead(Handle, sizeof(EncodedSize), &EncodedSize))
		{
			return false;
		}
		if (!CustomJump(Handle, EncodedSize))
		{
			return false;
		}
	}
	{
		unsigned long long EncodedSize = 0u;
		if (!CustomRead(Handle, sizeof(EncodedSize), &EncodedSize))
		{
			return false;
		}

		Temporal.Resize(EncodedSize);
		if (!CustomRead(Handle, EncodedSize, Temporal.Get()))
		{
			return false;
		}
	}

	if (!Decode(Temporal.Size(), Temporal.Get(), Scale, Rotation, Position, VertCount, IndCount, Verts, Inds))
	{
		return false;
	}

	return true;
}
bool GeometryStreamReader::GetGeometry(
	unsigned long Index,
	double* Rotation,
	double* Position,
	unsigned long* VertCount,
	unsigned long* IndCount,
	double** Verts,
	unsigned long** Inds
	)
{
	double Scale[3] = { 1., 1., 1. };
	if (!GetGeometry(
		Index,
		Scale,
		Rotation,
		Position,
		VertCount,
		IndCount,
		Verts,
		Inds
		))
	{
		return false;
	}

	for (double *Ptr = (*Verts), *PtrEnd = (*Verts) + (*VertCount); Ptr != PtrEnd; Ptr += 3)
	{
		(*Ptr) *= Scale[0];
		(*(Ptr + 1)) *= Scale[1];
		(*(Ptr + 2)) *= Scale[2];
	}

	return true;
}
bool GeometryStreamReader::GetGeometry(
	unsigned long Index,
	double* Scale,
	double* Rotation,
	double* Position,
	unsigned long* VertCount,
	unsigned long* IndCount,
	float** Verts,
	unsigned long** Inds
)
{
	double** RawVerts = nullptr;
	if (!GetGeometry(
	Index,
	Scale,
	Rotation,
	Position,
	VertCount,
	IndCount,
	RawVerts,
	Inds
	))
	{
		return false;
	}

	const double* Verts64 = (*RawVerts);
	float* Verts32 = reinterpret_cast<float*>(*RawVerts);
	for (const double* Verts64End = Verts64 + (*VertCount); Verts64 != Verts64End; ++Verts64, ++Verts32)
	{
		(*Verts32) = static_cast<float>(*Verts64);
	}

	(*Verts) = Verts32;
	return true;
}
bool GeometryStreamReader::GetGeometry(
	unsigned long Index,
	double* Rotation,
	double* Position,
	unsigned long* VertCount,
	unsigned long* IndCount,
	float** Verts,
	unsigned long** Inds
)
{
	double Scale[3] = { 1., 1., 1. };
	double** RawVerts = nullptr;
	if (!GetGeometry(
	Index,
	Scale,
	Rotation,
	Position,
	VertCount,
	IndCount,
	RawVerts,
	Inds
	))
	{
		return false;
	}

	const double* Verts64 = (*RawVerts);
	float* Verts32 = reinterpret_cast<float*>(*RawVerts);
	for (const double* Verts64End = Verts64 + (*VertCount); Verts64 != Verts64End; Verts64 += 3u, Verts32 += 3u)
	{
		(*Verts32) = static_cast<float>((*Verts64) * Scale[0]);
		(*(Verts32 + 1u)) = static_cast<float>((*(Verts64 + 1u)) * Scale[1]);
		(*(Verts32 + 2u)) = static_cast<float>((*(Verts64 + 2u)) * Scale[2]);
	}

	(*Verts) = Verts32;
	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#undef MAKE_TEXT


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#ifdef USE_LZMA2
#undef USE_LZMA2
#endif


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

