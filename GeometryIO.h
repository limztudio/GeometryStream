#ifndef _GEOMETRYIO_H_
#define _GEOMETRYIO_H_


#include <memory>


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#define ENCODE_OFFSET (1u << 20u)


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


namespace __hidden_GeometryIOProcessor
{
	class CustomIO;

	
	static bool Memcpy(void* Dest, const void* Src, unsigned long long Len)
	{
		return (memcpy_s(Dest, Len, Src, Len) == 0);
	}
	template<typename T>
	static bool MemcpyAndMove(T*& Dest, const void* Src, unsigned long long Len)
	{
		const bool bRes = Memcpy(Dest, Src, Len);
		
		unsigned char* Ptr = reinterpret_cast<unsigned char*>(Dest);
		Ptr += Len;
		Dest = reinterpret_cast<T*>(Ptr);
		
		return bRes;
	}

	template<typename T, unsigned long long N = sizeof(T)>
	static bool Memset(T* Dest, const T& V, unsigned long long Len)
	{
		bool bRes = true;
		
		unsigned long long i = 0u;
		for (unsigned char* Ptr = reinterpret_cast<unsigned char*>(Dest); i < Len; i += N)
		{
			if (!MemcpyAndMove(Ptr, &V, N))
			{
				bRes = false;
			}
		}
		if (i > Len)
		{
			i -= N;
			Len -= i;
			if (!Memcpy(reinterpret_cast<unsigned char*>(Dest) + i, &V, Len))
			{
				bRes = false;
			}
		}

		return bRes;
	}
	template<typename T, unsigned long long N = sizeof(T)>
	static bool MemsetAndMove(T*& Dest, const T& V, unsigned long long Len)
	{
		const bool bRes = Memset<T, N>(Dest, V, Len);

		unsigned char* Ptr = reinterpret_cast<unsigned char*>(Dest);
		Ptr += Len;
		Dest = reinterpret_cast<T*>(Ptr);
		
		return bRes;
	}

	
	extern void* CurGeometryIOProcessorAlloc(CustomIO*, unsigned long long);
	extern void CurGeometryIOProcessorFree(CustomIO*, void*);

	
	class CustomIO
	{
	public:
		typedef void* (*MemAlloc)(unsigned long long);
		typedef void (*MemFree)(void*);

		
	public:
		CustomIO(MemAlloc Alloc, MemFree Free)
			: CustomAlloc(Alloc)
			, CustomFree(Free)
		{}


	protected:
		MemAlloc CustomAlloc;
		MemFree CustomFree;

		
	public:
		friend void* CurGeometryIOProcessorAlloc(CustomIO*, unsigned long long);
		friend void CurGeometryIOProcessorFree(CustomIO*, void*);

		template<typename BufferType>
		friend class TempBuffer;
	};

	class CustomFileWriter
	{
	public:
		typedef bool (*FileTell)(void*, unsigned long long*);
		typedef bool (*FileJump)(void*, unsigned long long);
		typedef bool (*FileWrite)(void*, unsigned long long, const void*);


	public:
		CustomFileWriter(FileTell Tell, FileJump Jump, FileWrite Write)
			: CustomTell(Tell)
			, CustomJump(Jump)
			, CustomWrite(Write)
		{}


	protected:
		FileTell CustomTell;
		FileJump CustomJump;
		FileWrite CustomWrite;
	};
	
	class CustomFileReader
	{
	public:
		typedef bool (*FileTell)(void*, unsigned long long*);
		typedef bool (*FileJump)(void*, unsigned long long);
		typedef bool (*FileRead)(void*, unsigned long long, void*);


	public:
		CustomFileReader(FileTell Tell, FileJump Jump, FileRead Read)
			: CustomTell(Tell)
			, CustomJump(Jump)
			, CustomRead(Read)
		{}

		
	protected:
		FileTell CustomTell;
		FileJump CustomJump;
		FileRead CustomRead;
	};


	template<typename BufferType>
	class TempBuffer
	{
	public:
		typedef unsigned long long SizeType;


	public:
		TempBuffer(CustomIO* IO)
			: AssignedSize(0u)
			, VisibleSize(0u)
			, Buffer(nullptr)
			, CurIO(IO)
		{}
		~TempBuffer()
		{
			if (Buffer)
			{
				CurIO->CustomFree(Buffer);
			}
		}


	public:
		inline BufferType& operator[] (SizeType Index)
		{
			return Buffer[Index];
		}
		inline const BufferType& operator[] (SizeType Index) const
		{
			return Buffer[Index];
		}
		
	public:
		void Resize(SizeType NewSize)
		{
			BufferType* OldBuffer = Buffer;
			if (!OldBuffer)
			{
				Buffer = reinterpret_cast<BufferType*>(CurIO->CustomAlloc(NewSize * sizeof(BufferType)));
				AssignedSize = NewSize;
			}
			else if (AssignedSize < NewSize)
			{
				Buffer = reinterpret_cast<BufferType*>(CurIO->CustomAlloc(NewSize * sizeof(BufferType)));
				Memcpy(Buffer, OldBuffer, AssignedSize * sizeof(BufferType));
				CurIO->CustomFree(OldBuffer);
				AssignedSize = NewSize;
			}
			VisibleSize = NewSize;
		}
		void Resize(SizeType NewSize, BufferType Init)
		{
			Resize(NewSize);
			for (BufferType *Ptr = Buffer, *PtrEnd = Buffer + VisibleSize; Ptr != PtrEnd; ++Ptr)
			{
				(*Ptr) = Init;
			}
		}

		inline BufferType* Get()
		{
			return Buffer;
		}
		inline const BufferType* Get() const
		{
			return Buffer;
		}

		inline SizeType Size() const
		{
			return VisibleSize;
		}

		
	private:
		SizeType AssignedSize, VisibleSize;
		BufferType* Buffer;
		CustomIO* CurIO;
	};


#pragma pack(push, 1)
	union MinMax
	{
		struct
		{
			double Min[3];
			double Max[3];
		};
		double Raw[6];
	};
#pragma pack(pop)
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


class GeometryWriter : public __hidden_GeometryIOProcessor::CustomIO
{
public:
	GeometryWriter(MemAlloc Alloc, MemFree Free)
		: __hidden_GeometryIOProcessor::CustomIO(Alloc, Free)
		, TempSrcForEncoding(this)
		, TempDestForEncoding(this)
		, ErrorMsg(this)
	{}


public:
	const char* GetLastError() const
	{
		static const char* NullStr = "";
		return ErrorMsg.Get() ? ErrorMsg.Get() : NullStr;
	}

	
public:
	bool Encode(
		const double* Scale,
		const double* Rotation,
		const double* Position,
		unsigned long VertCount,
		unsigned long IndCount,
		const double* Verts,
		const unsigned long* Inds,
		unsigned long long* EncodedSize,
		unsigned char** EncodedData,

		unsigned long OptionalEncodeOffset = ENCODE_OFFSET,
		bool OptionalUseFloat32Vertex = false
		);

	
private:
	bool Pack(bool bUseFloat32);
	
private:
	bool ShouldConvertToFloat(unsigned long VertCount, unsigned long IndCount, const double* Verts, const unsigned long* Inds);
	bool PackVerts(bool bFloatInRange, unsigned long SrcCount, unsigned long long* DestCount, void* Data);
	
private:
	void PackInds(unsigned long VertCount, unsigned long SrcCount, unsigned long long* DestCount, void* Data);

	
private:
	__hidden_GeometryIOProcessor::TempBuffer<unsigned char> TempSrcForEncoding;
	__hidden_GeometryIOProcessor::TempBuffer<unsigned char> TempDestForEncoding;

protected:
	__hidden_GeometryIOProcessor::TempBuffer<char> ErrorMsg;
};

class GeometryReader : public __hidden_GeometryIOProcessor::CustomIO
{
public:
	GeometryReader(MemAlloc Alloc, MemFree Free)
		: __hidden_GeometryIOProcessor::CustomIO(Alloc, Free)
		, TempSrcForDecoding(this)
		, TempDestForDecoding(this)
		, ErrorMsg(this)
	{}


public:
	const char* GetLastError() const
	{
		static const char* NullStr = "";
		return ErrorMsg.Get() ? ErrorMsg.Get() : NullStr;
	}

	
public:
	bool Decode(
		unsigned long long EncodedSize,
		const unsigned char* EncodedData,
		double* Scale,
		double* Rotation,
		double* Position,
		unsigned long* VertCount,
		unsigned long* IndCount,
		double** Verts,
		unsigned long** Inds
		);

	
private:
	bool Unpack(const unsigned char* Ptr, double* Scale, double* Rotation, double* Position, unsigned long* VertCount, unsigned long* IndCount);
	
private:
	bool UnpackVerts(unsigned long SrcCount, const void* InData, bool bFloatInRange, void* OutData);
	
private:
	void UnpackInds(unsigned long VertCount, unsigned long SrcCount, const void* InData, void* OutData);

	
private:
	__hidden_GeometryIOProcessor::TempBuffer<unsigned char> TempSrcForDecoding;
	__hidden_GeometryIOProcessor::TempBuffer<unsigned char> TempDestForDecoding;

protected:
	__hidden_GeometryIOProcessor::TempBuffer<char> ErrorMsg;
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


class GeometryStreamWriter : private GeometryWriter, public __hidden_GeometryIOProcessor::CustomFileWriter
{
public:
	GeometryStreamWriter(MemAlloc Alloc, MemFree Free, FileTell Tell, FileJump Jump, FileWrite Write)
		: GeometryWriter(Alloc, Free)
		, __hidden_GeometryIOProcessor::CustomFileWriter(Tell, Jump, Write)
		, HeaderNames(this)
		, HeaderMinMaxes(this)
		, Temporal(this)
		, TemporalErrorMsg(this)
		, Handle(nullptr)
		, FileBegin(0u)
		, GeometryCount(0u)
	{}


public:
	inline const char* GetLastError() const
	{
		return GeometryWriter::GetLastError();
	}

	
public:
	template<typename FUNC>
	bool ScopedWrite(void* _Handle, FUNC&& Func)
	{
		if (!BeginWrite(_Handle))
		{
			return false;
		}

		const bool bSucceeded = Func();
		if (!bSucceeded)
		{
			const unsigned long long LenMsg = ErrorMsg.Size();
			if (LenMsg > 0u)
			{
				TemporalErrorMsg.Resize(LenMsg);
				__hidden_GeometryIOProcessor::Memcpy(TemporalErrorMsg.Get(), ErrorMsg.Get(), LenMsg);
			}
		}

		if (!EndWrite())
		{
			if (!bSucceeded)
			{
				const unsigned long long LenMsg = TemporalErrorMsg.Size();
				if (LenMsg > 0u)
				{
					ErrorMsg.Resize(LenMsg);
					__hidden_GeometryIOProcessor::Memcpy(ErrorMsg.Get(), TemporalErrorMsg.Get(), LenMsg);
				}
			}
			return false;
		}

		return true;
	}
	
	
public:
	bool BeginWrite(void* _Handle);

public:
	bool EndWrite();

public:
	unsigned long long EmplaceGeometry(
		const wchar_t* ID,
		const double* Scale,
		const double* Rotation,
		const double* Position,
		unsigned long VertCount,
		unsigned long IndCount,
		const double* Verts,
		const unsigned long* Inds,

		unsigned long OptionalEncodeOffset = ENCODE_OFFSET,
		bool OptionalUseFloat32Vertex = false
		);


private:
	__hidden_GeometryIOProcessor::TempBuffer<wchar_t> HeaderNames;
	__hidden_GeometryIOProcessor::TempBuffer<__hidden_GeometryIOProcessor::MinMax> HeaderMinMaxes;
	
	__hidden_GeometryIOProcessor::TempBuffer<unsigned char> Temporal;
	decltype(ErrorMsg) TemporalErrorMsg;
	
private:
	void* Handle;
	unsigned long long FileBegin;
	unsigned long long GeometryCount;
};


class GeometryStreamReader : private GeometryReader, public __hidden_GeometryIOProcessor::CustomFileReader
{
public:
	GeometryStreamReader(MemAlloc Alloc, MemFree Free, FileTell Tell, FileJump Jump, FileRead Read)
		: GeometryReader(Alloc, Free)
		, __hidden_GeometryIOProcessor::CustomFileReader(Tell, Jump, Read)
		, HeaderRawNames(this)
		, HeaderNames(this)
		, HeaderMinMaxes(this)
		, Temporal(this)
		, TemporalErrorMsg(this)
		, Handle(nullptr)
		, FileBegin(0u)
	{}


public:
	inline const char* GetLastError() const
	{
		return GeometryReader::GetLastError();
	}


public:
	template<typename FUNC>
	bool ScopedRead(void* _Handle, FUNC&& Func)
	{
		if (!BeginRead(_Handle))
		{
			return false;
		}

		const bool bSucceeded = Func();
		if (!bSucceeded)
		{
			const unsigned long long LenMsg = ErrorMsg.Size();
			if (LenMsg > 0u)
			{
				TemporalErrorMsg.Resize(LenMsg);
				__hidden_GeometryIOProcessor::Memcpy(TemporalErrorMsg.Get(), ErrorMsg.Get(), LenMsg);
			}
		}

		if (!EndRead())
		{
			if (!bSucceeded)
			{
				const unsigned long long LenMsg = TemporalErrorMsg.Size();
				if (LenMsg > 0u)
				{
					ErrorMsg.Resize(LenMsg);
					__hidden_GeometryIOProcessor::Memcpy(ErrorMsg.Get(), TemporalErrorMsg.Get(), LenMsg);
				}
			}
			return false;
		}

		return true;
	}
	
	
public:
	bool BeginRead(void* _Handle);

public:
	bool EndRead();

public:
	const unsigned long GetGeometryCount() const
	{
		return static_cast<unsigned long>(HeaderMinMaxes.Size());
	}
	
	const wchar_t* GetGeometryName(unsigned long Index) const
	{
		return HeaderNames[Index];
	}
	const __hidden_GeometryIOProcessor::MinMax& GetGeometryAABB(unsigned long Index) const
	{
		return HeaderMinMaxes[Index];
	}
	bool GetGeometry(
		unsigned long Index,
		double* Scale,
		double* Rotation,
		double* Position,
		unsigned long* VertCount,
		unsigned long* IndCount,
		double** Verts,
		unsigned long** Inds
		);
	bool GetGeometry(
		unsigned long Index,
		double* Rotation,
		double* Position,
		unsigned long* VertCount,
		unsigned long* IndCount,
		double** Verts,
		unsigned long** Inds
		);
	bool GetGeometry(
		unsigned long Index,
		double* Scale,
		double* Rotation,
		double* Position,
		unsigned long* VertCount,
		unsigned long* IndCount,
		float** Verts,
		unsigned long** Inds
	);
	bool GetGeometry(
		unsigned long Index,
		double* Rotation,
		double* Position,
		unsigned long* VertCount,
		unsigned long* IndCount,
		float** Verts,
		unsigned long** Inds
	);
	

private:
	__hidden_GeometryIOProcessor::TempBuffer<wchar_t> HeaderRawNames;
	__hidden_GeometryIOProcessor::TempBuffer<const wchar_t*> HeaderNames;
	__hidden_GeometryIOProcessor::TempBuffer<__hidden_GeometryIOProcessor::MinMax> HeaderMinMaxes;
	
	__hidden_GeometryIOProcessor::TempBuffer<unsigned char> Temporal;
	decltype(ErrorMsg) TemporalErrorMsg;
	
private:
	void* Handle;
	unsigned long long FileBegin;
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#undef ENCODE_OFFSET


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#endif

