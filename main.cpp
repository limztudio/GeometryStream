#include <vector>
#include <string>
#include <iostream>
#include <iomanip>

#include "GeometryIO.h"


struct Geometry
{
	std::wstring Name;
	
	double Scale[3];
	double Rotation[4];
	double Position[3];

	std::vector<double> Verts;
	std::vector<unsigned long> Inds;
};


static unsigned long long _rand()
{
	// xorshift*

	thread_local unsigned long long X = 1; // initial seed must be nonzero
	X ^= X >> 12;
	X ^= X << 25;
	X ^= X >> 27;
	return X * 0x2545F4914F6CDD1DULL;
}
static double _randD()
{
	return static_cast<double>(_rand() & 0x00ffffffffffffff) / 72057594037927936.;
}


static bool CustomFileTell(void* Handle, unsigned long long* Pos)
{
	FILE* f = reinterpret_cast<FILE*>(Handle);
	long long v = _ftelli64(f);
	if (v == -1)
	{
		return false;
	}
	(*Pos) = static_cast<unsigned long long>(v);
	return true;
}
static bool CustomFileJump(void* Handle, unsigned long long Pos)
{
	FILE* f = reinterpret_cast<FILE*>(Handle);
	if (_fseeki64(f, static_cast<long long>(Pos), SEEK_SET) != 0)
	{
		return false;
	}
	return true;
}
static bool CustomFileWrite(void* Handle, unsigned long long Size, const void* Data)
{
	FILE* f = reinterpret_cast<FILE*>(Handle);
	if (fwrite(Data, 1u, Size, f) != Size)
	{
		return false;
	}
	return true;
}
static bool CustomFileRead(void* Handle, unsigned long long Size, void* Data)
{
	FILE* f = reinterpret_cast<FILE*>(Handle);
	if (fread_s(Data, Size, 1u, Size, f) != Size)
	{
		return false;
	}
	return true;
}


static Geometry MakeRandomGeom()
{
	Geometry Output;

	{
		Output.Name.resize(_rand() & 7);
		for (wchar_t& i : Output.Name)
		{
			i = (_rand() & 0xff) + 1;
		}
		Output.Name += L"_";
	}

	{
		Output.Scale[0] = _randD();
		Output.Scale[1] = _randD();
		Output.Scale[2] = _randD();
	}
	{
		Output.Rotation[0] = _randD();
		Output.Rotation[1] = _randD();
		Output.Rotation[2] = _randD();
		Output.Rotation[3] = _randD();

		double L = Output.Rotation[0] * Output.Rotation[0];
		L += Output.Rotation[1] * Output.Rotation[1];
		L += Output.Rotation[2] * Output.Rotation[2];
		L += Output.Rotation[3] * Output.Rotation[3];
		L = std::sqrt(L);

		if (std::abs(L) > 0.)
		{
			Output.Rotation[0] /= L;
			Output.Rotation[1] /= L;
			Output.Rotation[2] /= L;
			Output.Rotation[3] /= L;
		}
		else
		{
			Output.Rotation[0] = 0.;
			Output.Rotation[1] = 0.;
			Output.Rotation[2] = 0.;
			Output.Rotation[3] = 1.;
		}
	}
	{
		Output.Position[0] = _randD();
		Output.Position[1] = _randD();
		Output.Position[2] = _randD();
	}

	{
		Output.Verts.resize((_rand() & 0xfffff) * 3u);
		for (double& i : Output.Verts)
		{
			i = _randD();
			if (i > 0.)
				i = 1. / i;
			i -= i * 0.5;
		}
	}
	{
		Output.Inds.resize((_rand() & 0xffffff) * 3u);
		for (unsigned long& i : Output.Inds)
		{
			i = static_cast<unsigned long>(_rand() % (Output.Verts.size() / 3));
		}
	}

	return Output;
}

static unsigned long long EstimatedSizeOf(const Geometry& Geom)
{
	unsigned long long Size = ((Geom.Name.length() + 1u) << 1u);

	Size += 6u << 3u;
	
	Size += sizeof(Geom.Scale);
	Size += sizeof(Geom.Rotation);
	Size += sizeof(Geom.Position);

	Size += 4u;
	Size += Geom.Verts.size() << 3u;

	Size += 4u;
	Size += Geom.Inds.size() << 2u;

	return Size;
}


static bool operator==(const Geometry& Lhs, const Geometry& Rhs)
{
	bool bRet = true;
	
	if (Lhs.Name != Rhs.Name)
	{
		std::wcout << L"different name found: " << L"\"" << Lhs.Name << L"\" \"" << Rhs.Name << L"\"" << std::endl;
		bRet = false;
	}

	if (Lhs.Scale[0] != Rhs.Scale[0])
	{
		std::cout << "different scale(x) found: " << "\"" << Lhs.Scale[0] << "\" \"" << Rhs.Scale[0] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Scale[1] != Rhs.Scale[1])
	{
		std::cout << "different scale(y) found: " << "\"" << Lhs.Scale[1] << "\" \"" << Rhs.Scale[1] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Scale[2] != Rhs.Scale[2])
	{
		std::cout << "different scale(z) found: " << "\"" << Lhs.Scale[2] << "\" \"" << Rhs.Scale[2] << "\"" << std::endl;
		bRet = false;
	}

	if (Lhs.Rotation[0] != Rhs.Rotation[0])
	{
		std::cout << "different rotation(x) found: " << "\"" << Lhs.Rotation[0] << "\" \"" << Rhs.Rotation[0] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Rotation[1] != Rhs.Rotation[1])
	{
		std::cout << "different rotation(y) found: " << "\"" << Lhs.Rotation[1] << "\" \"" << Rhs.Rotation[1] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Rotation[2] != Rhs.Rotation[2])
	{
		std::cout << "different rotation(z) found: " << "\"" << Lhs.Rotation[2] << "\" \"" << Rhs.Rotation[2] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Rotation[3] != Rhs.Rotation[3])
	{
		std::cout << "different rotation(w) found: " << "\"" << Lhs.Rotation[3] << "\" \"" << Rhs.Rotation[3] << "\"" << std::endl;
		bRet = false;
	}

	if (Lhs.Position[0] != Rhs.Position[0])
	{
		std::cout << "different position(x) found: " << "\"" << Lhs.Position[0] << "\" \"" << Rhs.Position[0] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Position[1] != Rhs.Position[1])
	{
		std::cout << "different position(y) found: " << "\"" << Lhs.Position[1] << "\" \"" << Rhs.Position[1] << "\"" << std::endl;
		bRet = false;
	}
	if (Lhs.Position[2] != Rhs.Position[2])
	{
		std::cout << "different position(z) found: " << "\"" << Lhs.Position[2] << "\" \"" << Rhs.Position[2] << "\"" << std::endl;
		bRet = false;
	}

	if (Lhs.Verts.size() != Rhs.Verts.size())
	{
		std::cout << "different vertex count found: " << "\"" << Lhs.Verts.size() << "\" \"" << Rhs.Verts.size() << "\"" << std::endl;
		bRet = false;
	}
	else
	{
		unsigned long DiffCount = 0u;
		double MaxDiff = 0.;
		double TotalDiff = 0.;
		
		for (size_t i = 0u, e = Lhs.Verts.size(); i < e; ++i)
		{
			const double LhsVert = Lhs.Verts[i];
			const double RhsVert = Rhs.Verts[i];

			const double Diff = std::abs(LhsVert - RhsVert);
			
			if (Diff >= 0.)
			{
				++DiffCount;
				MaxDiff = (Diff > MaxDiff) ? Diff : MaxDiff;
				TotalDiff += Diff;
			}
		}

		if (TotalDiff > 0.)
		{
			const double AvgDiff = TotalDiff / static_cast<double>(DiffCount);

			std::cout << DiffCount << " of different vertices found. avg. diff: " << std::setprecision(10) << AvgDiff << " max diff: " << std::setprecision(10) << MaxDiff << std::endl;
			bRet = false;
		}
	}
	if (Lhs.Inds.size() != Rhs.Inds.size())
	{
		std::cout << "different index count found: " << "\"" << Lhs.Inds.size() << "\" \"" << Rhs.Inds.size() << "\"" << std::endl;
		bRet = false;
	}
	else
	{
		for (size_t i = 0u, e = Lhs.Inds.size(); i < e; ++i)
		{
			const unsigned long LhsInd = Lhs.Inds[i];
			const unsigned long RhsInd = Rhs.Inds[i];

			if (LhsInd != RhsInd)
			{
				std::cout << "different index found: " << "\"" << LhsInd << "\" \"" << RhsInd << "\" at " << i << std::endl;
				bRet = false;
			}
		}
	}

	return bRet;
}
static bool operator!=(const Geometry& Lhs, const Geometry& Rhs)
{
	return !(Lhs == Rhs);
}


int main()
{
	const char Filepath[] = "./Test.bin";
	
	std::vector<Geometry> Geoms(_rand() & 3u);
	for (Geometry& i : Geoms)
	{
		i = MakeRandomGeom();
	}

	unsigned long long RawSize = 0u;
	{
		RawSize += 4u;
		for (const Geometry& i : Geoms)
		{
			RawSize += EstimatedSizeOf(i);
		}
	}

	unsigned long long EncodedSize;
	{
		FILE* f;
		if (fopen_s(&f, Filepath, "wb") != 0)
		{
			return -1;
		}
		if (!f)
		{
			return -1;
		}
		
		do
		{
			GeometryStreamWriter Processor(malloc, free, CustomFileTell, CustomFileJump, CustomFileWrite);

			{
				GeometryStreamScopedIO<GeometryStreamWriter> IO(Processor, f);

				for (const Geometry& i : Geoms)
				{
					if (Processor.EmplaceGeometry(
						i.Name.c_str(),
						i.Scale,
						i.Rotation,
						i.Position,
						static_cast<unsigned long>(i.Verts.size()),
						static_cast<unsigned long>(i.Inds.size()),
						i.Verts.data(),
						i.Inds.data()
						) == static_cast<unsigned long long>(-1))
					{
						std:: cout << Processor.GetLastError() << std::endl;
						break;
					}
				}
			}
		}
		while (false);

		EncodedSize = static_cast<unsigned long long>(_ftelli64(f));

		if (fclose(f) == -1)
		{
			return -1;
		}
	}

	std::vector<Geometry> DecGeoms;
	{
		FILE* f;
		if (fopen_s(&f, Filepath, "rb") != 0)
		{
			return -1;
		}
		if (!f)
		{
			return -1;
		}

		do
		{
			GeometryStreamReader Processor(malloc, free, CustomFileTell, CustomFileJump, CustomFileRead);

			{
				GeometryStreamScopedIO<GeometryStreamReader> IO(Processor, f);

				unsigned long e = Processor.GetGeometryCount();
				DecGeoms.resize(e);
				
				for(unsigned long i = 0u; i < e; ++i)
				{
					Geometry& p = DecGeoms[i];
					
					p.Name = Processor.GetGeometryName(i);

					unsigned long VertCount;
					unsigned long IndCount;
					double* Verts;
					unsigned long* Inds;
					if (!Processor.GetGeometry(
						i,
						p.Scale,
						p.Rotation,
						p.Position,
						&VertCount,
						&IndCount,
						&Verts,
						&Inds
						))
					{
						std:: cout << Processor.GetLastError() << std::endl;
						break;
					}

					p.Verts.resize(VertCount);
					memcpy_s(p.Verts.data(), VertCount << 3u, Verts, VertCount << 3u);

					p.Inds.resize(IndCount);
					memcpy_s(p.Inds.data(), IndCount << 2u, Inds, IndCount << 2u);
				}
			}
		}
		while (false);

		if (fclose(f) == -1)
		{
			return -1;
		}
	}

	{
		if (Geoms.size() != DecGeoms.size())
		{
			std::cout << "different geometry count found: " << "\"" << Geoms.size() << "\" \"" << DecGeoms.size() << "\"" << std::endl;
			return -1;
		}

		for (size_t i = 0u, e = Geoms.size(); i < e; ++i)
		{
			const Geometry& Org = Geoms[i];
			const Geometry& Dec = DecGeoms[i];

			if (Org != Dec)
			{
				continue;
			}
		}
	}
 
	std::cout << "compressed: " << (((double)EncodedSize / (double)RawSize) * 100) << "%" << std::endl;

	return 0;
}

