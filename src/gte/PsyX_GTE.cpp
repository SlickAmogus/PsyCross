#include "PsyX_GTE.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"

#include "psx/libgte.h"
#include "psx/gtereg.h"

#include <math.h>
#include <array>
#include <stdint.h>
#include <string.h>
#include <unordered_map>



GTERegisters gteRegs;

#define GTE_SF(op)			((op >> 19) & 1)
#define GTE_MX(op)			((op >> 17) & 3)
#define GTE_V(op)			((op >> 15) & 3)
#define GTE_CV(op)			((op >> 13) & 3)
#define GTE_LM(op)			((op >> 10) & 1)
#define GTE_FUNCT(op)		(op & 63)

#define gteop(code)			(code & 0x1ffffff)

#define VX(n)				(n < 3 ? gteRegs.CP2D.p[ n << 1 ].sw.l : C2_IR1)
#define VY(n)				(n < 3 ? gteRegs.CP2D.p[ n << 1 ].sw.h : C2_IR2)
#define VZ(n)				(n < 3 ? gteRegs.CP2D.p[ (n << 1) + 1 ].sw.l : C2_IR3)
#define MX11(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) ].sw.l : -C2_R << 4)
#define MX12(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) ].sw.h : C2_R << 4)
#define MX13(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 1 ].sw.l : C2_IR0)
#define MX21(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 1 ].sw.h : C2_R13)
#define MX22(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 2 ].sw.l : C2_R13)
#define MX23(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 2 ].sw.h : C2_R13)
#define MX31(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 3 ].sw.l : C2_R22)
#define MX32(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 3 ].sw.h : C2_R22)
#define MX33(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 4 ].sw.l : C2_R22)
#define CV1(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 5 ].sd : 0)
#define CV2(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 6 ].sd : 0)
#define CV3(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 7 ].sd : 0)

#ifndef max
#   define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#   define min(a, b) ((a) < (b) ? (a) : (b))
#endif


static int m_sf;
static long long m_mac0;
static long long m_mac3;

/* Unquantized rotation-matrix shadow used only by PGXP's side channel.  The
 * GTE registers and every integer result remain untouched.  Pointer entries
 * are validated against all nine Q12 coefficients; value recovery is allowed
 * only while a coefficient tuple has one unambiguous exact meaning in the
 * current frame. */
namespace
{
using MatrixKey = std::array<short, 9>;
using ExactMatrix = std::array<double, 9>;
using TranslationKey = std::array<int, 3>;
using ExactTranslation = std::array<double, 3>;
using VectorKey = std::array<short, 3>;
using ExactVector = std::array<double, 3>;

struct MatrixKeyHash
{
	size_t operator()(const MatrixKey& key) const
	{
		size_t hash = (size_t)1469598103934665603ULL;
		for (short value : key)
		{
			hash ^= (unsigned short)value;
			hash *= (size_t)1099511628211ULL;
		}
		return hash;
	}
};

struct VectorKeyHash
{
	size_t operator()(const VectorKey& key) const
	{
		size_t hash = (size_t)1469598103934665603ULL;
		for (short value : key)
		{
			hash ^= (unsigned short)value;
			hash *= (size_t)1099511628211ULL;
		}
		return hash;
	}
};

struct AddressEntry
{
	MatrixKey key{};
	ExactMatrix exact{};
	uint64_t generation = 0;
	bool valid = false;
};

struct ValueEntry
{
	ExactMatrix exact{};
	bool ambiguous = false;
};

struct TranslationEntry
{
	TranslationKey key{};
	ExactTranslation exact{};
	uint64_t generation = 0;
	bool valid = false;
};

struct VectorAddressEntry
{
	VectorKey key{};
	ExactVector exact{};
	uint64_t generation = 0;
	bool valid = false;
};

struct VectorValueEntry
{
	ExactVector exact{};
	bool ambiguous = false;
};

static std::unordered_map<const void*, AddressEntry> s_matrixByAddress;
static std::unordered_map<MatrixKey, ValueEntry, MatrixKeyHash> s_matrixByValue;
static std::unordered_map<const void*, TranslationEntry> s_translationByAddress;
static std::unordered_map<const void*, VectorAddressEntry> s_vectorByAddress;
static std::unordered_map<VectorKey, VectorValueEntry, VectorKeyHash> s_vectorByValue;
static uint64_t s_matrixGeneration = 1;

static MatrixKey MatrixKeyFromMemory(const void* matrix)
{
	MatrixKey key;
	const short* values = (const short*)matrix;
	for (int i = 0; i < 9; ++i)
		key[i] = values[i];
	return key;
}

static MatrixKey MatrixKeyFromGte(void)
{
	return MatrixKey{{ C2_R11, C2_R12, C2_R13,
		C2_R21, C2_R22, C2_R23,
		C2_R31, C2_R32, C2_R33 }};
}

static TranslationKey TranslationKeyFromMemory(const void* matrix)
{
	const MATRIX* m = (const MATRIX*)matrix;
	return TranslationKey{{ m->t[0], m->t[1], m->t[2] }};
}

static TranslationKey TranslationKeyFromGte(void)
{
	return TranslationKey{{ C2_TRX, C2_TRY, C2_TRZ }};
}

static VectorKey VectorKeyFromMemory(const void* vector)
{
	const short* v = (const short*)vector;
	return VectorKey{{ v[0], v[1], v[2] }};
}

static VectorKey VectorKeyFromGte(int slot)
{
	return VectorKey{{ (short)VX(slot), (short)VY(slot), (short)VZ(slot) }};
}

static bool ExactEqual(const ExactMatrix& a, const ExactMatrix& b)
{
	for (int i = 0; i < 9; ++i)
		if (a[i] != b[i])
			return false;
	return true;
}

static bool IdentityKey(const MatrixKey& key)
{
	static const MatrixKey identity = {{ 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }};
	return key == identity;
}

static bool AspectIdentityKey(const MatrixKey& key)
{
	/* Psy-Q GsIDMATRIX2: identity plus the exact 3/4 NTSC Y scale. */
	static const MatrixKey identity = {{ 4096, 0, 0, 0, 3072, 0, 0, 0, 4096 }};
	return key == identity;
}

static ExactMatrix ExactIdentity(void)
{
	return ExactMatrix{{ 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 }};
}

static ExactMatrix ExactAspectIdentity(void)
{
	return ExactMatrix{{ 1.0, 0.0, 0.0, 0.0, 0.75, 0.0, 0.0, 0.0, 1.0 }};
}

static ExactMatrix ExactQuantizedMatrix(const MatrixKey& key)
{
	ExactMatrix exact;
	for (int i = 0; i < 9; ++i)
		exact[i] = (double)key[i] / 4096.0;
	return exact;
}

static void AddValueEntry(const MatrixKey& key, const ExactMatrix& exact)
{
	auto inserted = s_matrixByValue.emplace(key, ValueEntry{ exact, false });
	if (!inserted.second && !inserted.first->second.ambiguous &&
		!ExactEqual(inserted.first->second.exact, exact))
	{
		/* Once ambiguous, stay ambiguous for this generation.  In particular,
		 * never replace the canonical value with the most recent writer. */
		inserted.first->second.ambiguous = true;
	}
}

static void RegisterMatrix(const void* matrix, const MatrixKey& key,
	const ExactMatrix& exact)
{
	AddressEntry entry;
	entry.key = key;
	entry.exact = exact;
	entry.generation = s_matrixGeneration;
	entry.valid = true;
	s_matrixByAddress[matrix] = entry;
	AddValueEntry(key, exact);
}

static bool LookupMatrix(const void* matrix, ExactMatrix& exact)
{
	const MatrixKey key = MatrixKeyFromMemory(matrix);
	auto address = s_matrixByAddress.find(matrix);
	if (address != s_matrixByAddress.end())
	{
		const AddressEntry& entry = address->second;
		/* Address provenance is generation-scoped.  Keeping it across frames
		 * would make a reused stack slot with the same Q12 tuple indistinguishable
		 * from the old matrix.  Expiry trades only coverage for safety. */
		const bool live = entry.generation == s_matrixGeneration;
		if (live && entry.key == key)
		{
			if (!entry.valid)
			{
				/* Invalid means "do not trust the old unquantized twin", not
				 * "discard all parent precision". The current stored Q12 matrix
				 * is always a safe exact fallback for the PGXP side channel. */
				exact = ExactQuantizedMatrix(key);
				RegisterMatrix(matrix, key, exact);
				return true;
			}
			exact = entry.exact;
			return true;
		}
	}

	/* The canonical Q12 identity has no lost information and is therefore safe
	 * even when it originated as a static initializer rather than a producer. */
	if (IdentityKey(key))
	{
		exact = ExactIdentity();
		RegisterMatrix(matrix, key, exact);
		return true;
	}
	if (AspectIdentityKey(key))
	{
		exact = ExactAspectIdentity();
		RegisterMatrix(matrix, key, exact);
		return true;
	}

	auto value = s_matrixByValue.find(key);
	if (value != s_matrixByValue.end() && !value->second.ambiguous)
		exact = value->second.exact;
	else
		exact = ExactQuantizedMatrix(key);
	/* A value-recovered copy is deliberately generation-scoped: on the next
	 * frame the same integer tuple may represent a slightly different angle. */
	RegisterMatrix(matrix, key, exact);
	return true;
}

static void RegisterTranslation(const void* matrix, const TranslationKey& key,
	const ExactTranslation& exact)
{
	TranslationEntry entry;
	entry.key = key;
	entry.exact = exact;
	entry.generation = s_matrixGeneration;
	entry.valid = true;
	s_translationByAddress[matrix] = entry;
}

static bool LookupTranslation(const void* matrix, ExactTranslation& exact)
{
	const TranslationKey key = TranslationKeyFromMemory(matrix);
	auto address = s_translationByAddress.find(matrix);
	if (address != s_translationByAddress.end())
	{
		const TranslationEntry& entry = address->second;
		if (entry.generation == s_matrixGeneration && entry.key == key)
		{
			if (!entry.valid)
			{
				exact = ExactTranslation{{ (double)key[0], (double)key[1], (double)key[2] }};
				RegisterTranslation(matrix, key, exact);
				return true;
			}
			exact = entry.exact;
			return true;
		}
	}

	/* An integer GTE translation is already exact in its own units.  Unknown
	 * provenance therefore degrades to the legacy value without losing matrix
	 * rotation coverage. */
	exact = ExactTranslation{{ (double)key[0], (double)key[1], (double)key[2] }};
	RegisterTranslation(matrix, key, exact);
	return true;
}

static bool ExactVectorEqual(const ExactVector& a, const ExactVector& b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static void RegisterVector(const void* vector, const VectorKey& key, const ExactVector& exact)
{
	VectorAddressEntry address;
	address.key = key;
	address.exact = exact;
	address.generation = s_matrixGeneration;
	address.valid = true;
	s_vectorByAddress[vector] = address;

	auto value = s_vectorByValue.emplace(key, VectorValueEntry{ exact, false });
	if (!value.second && !value.first->second.ambiguous &&
		!ExactVectorEqual(value.first->second.exact, exact))
	{
		/* A tuple remains unusable for value recovery after the first conflict;
		 * never oscillate to whichever exact vector registered last. */
		value.first->second.ambiguous = true;
	}
}

static bool LookupVector(const void* vector, ExactVector& exact)
{
	const VectorKey key = VectorKeyFromMemory(vector);
	auto address = s_vectorByAddress.find(vector);
	if (address != s_vectorByAddress.end() &&
		address->second.generation == s_matrixGeneration &&
		address->second.key == key)
	{
		if (!address->second.valid)
			return false;
		exact = address->second.exact;
		return true;
	}

	auto value = s_vectorByValue.find(key);
	if (value == s_vectorByValue.end() || value->second.ambiguous)
		return false;
	exact = value->second.exact;
	RegisterVector(vector, key, exact);
	return true;
}

struct CurrentRotation
{
	MatrixKey key{};
	ExactMatrix exact{};
	bool valid = false;
};

static CurrentRotation s_currentRotation;

struct CurrentTranslation
{
	TranslationKey key{};
	ExactTranslation exact{};
	bool valid = false;
};

static CurrentTranslation s_currentTranslation;

struct CurrentVector
{
	VectorKey key{};
	ExactVector exact{};
	bool valid = false;
};

static CurrentVector s_currentVector[3];

static bool CurrentExact(ExactMatrix& exact)
{
	const MatrixKey key = MatrixKeyFromGte();
	if (s_currentRotation.valid && s_currentRotation.key == key)
	{
		exact = s_currentRotation.exact;
		return true;
	}
	if (IdentityKey(key))
	{
		exact = ExactIdentity();
		return true;
	}
	if (AspectIdentityKey(key))
	{
		exact = ExactAspectIdentity();
		return true;
	}
	return false;
}

static bool CurrentExactTranslation(ExactTranslation& exact)
{
	const TranslationKey key = TranslationKeyFromGte();
	if (s_currentTranslation.valid && s_currentTranslation.key == key)
	{
		exact = s_currentTranslation.exact;
		return true;
	}
	return false;
}

static bool CurrentExactVector(int slot, ExactVector& exact)
{
	if ((unsigned)slot > 2u)
		return false;
	const VectorKey key = VectorKeyFromGte(slot);
	if (s_currentVector[slot].valid && s_currentVector[slot].key == key)
	{
		exact = s_currentVector[slot].exact;
		return true;
	}
	return false;
}

struct ColumnMultiply
{
	const char* inputBase = nullptr;
	char* outputBase = nullptr;
	MatrixKey inputKey{};
	ExactMatrix result{};
	int column = 0;
	bool valid = false;
};

static ColumnMultiply s_columnMultiply;
}

extern "C" void PGXP_MatrixRegister(const MATRIX* matrix, const double exactValues[9])
{
	if (!matrix || !exactValues)
		return;
	ExactMatrix exact;
	for (int i = 0; i < 9; ++i)
		exact[i] = exactValues[i];
	RegisterMatrix(matrix, MatrixKeyFromMemory(matrix), exact);
}

extern "C" int PGXP_MatrixLookup(const MATRIX* matrix, double exactValues[9])
{
	if (!matrix || !exactValues)
		return 0;
	ExactMatrix exact;
	if (!LookupMatrix(matrix, exact))
		return 0;
	for (int i = 0; i < 9; ++i)
		exactValues[i] = exact[i];
	return 1;
}

extern "C" int PGXP_MatrixLookupCurrent(double exactValues[9])
{
	if (!exactValues)
		return 0;
	ExactMatrix exact;
	if (!CurrentExact(exact))
		return 0;
	for (int i = 0; i < 9; ++i)
		exactValues[i] = exact[i];
	return 1;
}

extern "C" void PGXP_MatrixCopy(MATRIX* dst, const MATRIX* src)
{
	if (!dst || !src)
		return;
	ExactMatrix exact;
	if (LookupMatrix(src, exact))
		RegisterMatrix(dst, MatrixKeyFromMemory(dst), exact);
	else
	{
		AddressEntry entry;
		entry.key = MatrixKeyFromMemory(dst);
		entry.generation = s_matrixGeneration;
		s_matrixByAddress[dst] = entry;
	}
}

extern "C" void PGXP_MatrixRegisterTranslation(MATRIX* matrix, const double exactValues[3])
{
	if (!matrix || !exactValues)
		return;
	const ExactTranslation exact = {{ exactValues[0], exactValues[1], exactValues[2] }};
	RegisterTranslation(matrix, TranslationKeyFromMemory(matrix), exact);
}

extern "C" void PGXP_MatrixRegisterTranslationQ12(MATRIX* matrix, int x, int y, int z)
{
	if (!matrix)
		return;
	/* Validate the producer's Q12->Q8 integer result before associating its
	 * discarded four fractional bits with this MATRIX. */
	if (matrix->t[0] != (x >> 4) || matrix->t[1] != (y >> 4) || matrix->t[2] != (z >> 4))
	{
		PGXP_MatrixInvalidateTranslation(matrix);
		return;
	}
	const ExactTranslation exact = {{ (double)x / 16.0, (double)y / 16.0, (double)z / 16.0 }};
	RegisterTranslation(matrix, TranslationKeyFromMemory(matrix), exact);
}

extern "C" int PGXP_MatrixLookupTranslation(const MATRIX* matrix, double exactValues[3])
{
	if (!matrix || !exactValues)
		return 0;
	ExactTranslation exact;
	if (!LookupTranslation(matrix, exact))
		return 0;
	exactValues[0] = exact[0];
	exactValues[1] = exact[1];
	exactValues[2] = exact[2];
	return 1;
}

extern "C" void PGXP_MatrixInvalidateTranslation(MATRIX* matrix)
{
	if (!matrix)
		return;
	TranslationEntry entry;
	entry.key = TranslationKeyFromMemory(matrix);
	entry.generation = s_matrixGeneration;
	s_translationByAddress[matrix] = entry;
}

extern "C" void PGXP_MatrixCopyFull(MATRIX* dst, const MATRIX* src)
{
	if (!dst || !src)
		return;
	PGXP_MatrixCopy(dst, src);
	ExactTranslation exact;
	if (LookupTranslation(src, exact))
		RegisterTranslation(dst, TranslationKeyFromMemory(dst), exact);
	else
		PGXP_MatrixInvalidateTranslation(dst);
}

extern "C" void PGXP_VectorRegisterQ12(const void* vector, int x, int y, int z)
{
	PGXP_VectorRegisterFixed(vector, x, y, z, 4);
}

extern "C" void PGXP_VectorRegisterFixed(const void* vector, int x, int y, int z, int shift)
{
	if (!g_PsxUsePgxp || !vector || shift < 0 || shift > 30)
		return;
	const VectorKey key = VectorKeyFromMemory(vector);
	if ((int)key[0] != (x >> shift) || (int)key[1] != (y >> shift) || (int)key[2] != (z >> shift))
		return;
	const double scale = (double)(1u << shift);
	const ExactVector exact = {{ (double)x / scale, (double)y / scale, (double)z / scale }};
	RegisterVector(vector, key, exact);
}

extern "C" void PGXP_MatrixInvalidate(MATRIX* matrix)
{
	if (!matrix)
		return;
	AddressEntry entry;
	entry.key = MatrixKeyFromMemory(matrix);
	entry.generation = s_matrixGeneration;
	s_matrixByAddress[matrix] = entry;
}

extern "C" void PGXP_MatrixNextGeneration(void)
{
	++s_matrixGeneration;
	if (s_matrixGeneration == 0)
		s_matrixGeneration = 1;
	s_matrixByValue.clear();
	/* Entries are generation-validated already; clearing also bounds storage and
	 * eliminates any chance of a reused stack address inheriting old metadata. */
	s_matrixByAddress.clear();
	s_translationByAddress.clear();
	s_vectorByAddress.clear();
	s_vectorByValue.clear();
}

extern "C" void PGXP_MatrixInvalidateCurrent(void)
{
	s_currentRotation.valid = false;
	s_columnMultiply = ColumnMultiply{};
}

extern "C" void PGXP_MatrixInvalidateCurrentTranslation(void)
{
	s_currentTranslation.valid = false;
}

extern "C" void PGXP_VectorInvalidateCurrent(int slot)
{
	if ((unsigned)slot <= 2u)
		s_currentVector[slot].valid = false;
}

extern "C" void PGXP_MatrixSetRot(const void* matrix)
{
	s_currentRotation.valid = false;
	s_columnMultiply = ColumnMultiply{};
	if (!matrix)
		return;
	ExactMatrix exact;
	const MatrixKey memoryKey = MatrixKeyFromMemory(matrix);
	const MatrixKey gteKey = MatrixKeyFromGte();
	if (memoryKey == gteKey && LookupMatrix(matrix, exact))
	{
		s_currentRotation.key = gteKey;
		s_currentRotation.exact = exact;
		s_currentRotation.valid = true;
	}
}

extern "C" void PGXP_MatrixSetTrans(const void* matrix)
{
	s_currentTranslation.valid = false;
	if (!matrix)
		return;
	ExactTranslation exact;
	const TranslationKey memoryKey = TranslationKeyFromMemory(matrix);
	const TranslationKey gteKey = TranslationKeyFromGte();
	if (memoryKey == gteKey && LookupTranslation(matrix, exact))
	{
		s_currentTranslation.key = gteKey;
		s_currentTranslation.exact = exact;
		s_currentTranslation.valid = true;
	}
}

extern "C" void PGXP_VectorLoad(const void* vector, int slot)
{
	if ((unsigned)slot > 2u)
		return;
	s_currentVector[slot].valid = false;
	if (!g_PsxUsePgxp || !vector)
		return;
	ExactVector exact;
	const VectorKey memoryKey = VectorKeyFromMemory(vector);
	const VectorKey gteKey = VectorKeyFromGte(slot);
	if (memoryKey == gteKey && LookupVector(vector, exact))
	{
		s_currentVector[slot].key = gteKey;
		s_currentVector[slot].exact = exact;
		s_currentVector[slot].valid = true;
	}
}

extern "C" void PGXP_MatrixCaptureCurrent(void* matrix)
{
	if (!matrix)
		return;
	ExactMatrix exact;
	const MatrixKey memoryKey = MatrixKeyFromMemory(matrix);
	if (memoryKey == MatrixKeyFromGte() && CurrentExact(exact))
		RegisterMatrix(matrix, memoryKey, exact);
	else
		PGXP_MatrixInvalidate((MATRIX*)matrix);

	ExactTranslation translation;
	const TranslationKey memoryTranslation = TranslationKeyFromMemory(matrix);
	if (memoryTranslation == TranslationKeyFromGte() && CurrentExactTranslation(translation))
		RegisterTranslation(matrix, memoryTranslation, translation);
	else
		PGXP_MatrixInvalidateTranslation((MATRIX*)matrix);
}

extern "C" void PGXP_MatrixLoadColumn(const void* columnPtr)
{
	if (!columnPtr)
	{
		s_columnMultiply.valid = false;
		return;
	}

	if (s_columnMultiply.column == 0)
	{
		s_columnMultiply = ColumnMultiply{};
		s_columnMultiply.inputBase = (const char*)columnPtr;
		ExactMatrix lhs, rhs;
		if (CurrentExact(lhs) && LookupMatrix(columnPtr, rhs))
		{
			s_columnMultiply.inputKey = MatrixKeyFromMemory(columnPtr);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col)
					s_columnMultiply.result[row * 3 + col] =
						lhs[row * 3 + 0] * rhs[0 * 3 + col] +
						lhs[row * 3 + 1] * rhs[1 * 3 + col] +
						lhs[row * 3 + 2] * rhs[2 * 3 + col];
			s_columnMultiply.valid = true;
		}
	}
	else
	{
		const int col = s_columnMultiply.column;
		if ((const char*)columnPtr != s_columnMultiply.inputBase + col * (int)sizeof(short))
			s_columnMultiply.valid = false;
		if (s_columnMultiply.valid)
		{
			const short* p = (const short*)columnPtr;
			for (int row = 0; row < 3; ++row)
				if (p[row * 3] != s_columnMultiply.inputKey[row * 3 + col])
					s_columnMultiply.valid = false;
		}
	}
}

extern "C" void PGXP_MatrixStoreColumn(void* columnPtr)
{
	if (!columnPtr)
	{
		s_columnMultiply = ColumnMultiply{};
		return;
	}

	const int col = s_columnMultiply.column;
	if (col == 0)
	{
		s_columnMultiply.outputBase = (char*)columnPtr;
		/* Do not let an old address twin escape while this matrix is only partly
		 * overwritten.  Avoid reading its not-yet-initialized remaining columns. */
		AddressEntry entry;
		entry.generation = s_matrixGeneration;
		s_matrixByAddress[columnPtr] = entry;
	}
	else if ((char*)columnPtr != s_columnMultiply.outputBase + col * (int)sizeof(short))
	{
		s_columnMultiply.valid = false;
	}

	if (++s_columnMultiply.column == 3)
	{
		MATRIX* output = (MATRIX*)s_columnMultiply.outputBase;
		if (output && s_columnMultiply.valid)
			RegisterMatrix(output, MatrixKeyFromMemory(output), s_columnMultiply.result);
		else if (output)
			PGXP_MatrixInvalidate(output);
		s_columnMultiply = ColumnMultiply{};
	}
}

unsigned int gte_leadingzerocount(unsigned int lzcs) 
{
#if 0 // OLD AND SLOW WAY
	unsigned int lzcr = 0;

	if ((lzcs & 0x80000000) == 0)
		lzcs = ~lzcs;

	while ((lzcs & 0x80000000) != 0) {
		lzcr++;
		lzcs <<= 1;
	}

	return lzcr;
#endif

	if (!lzcs)
		return 32;

	// perform fast bit scan

	unsigned int lzcr = lzcs;
	static char debruijn32[32] = {
        0, 31, 9, 30, 3, 8, 13, 29, 2, 5, 7, 21, 12, 24, 28, 19,
        1, 10, 4, 14, 6, 22, 25, 20, 11, 15, 23, 26, 16, 27, 17, 18
    };

	lzcr |= lzcr >> 1;
	lzcr |= lzcr >> 2;
	lzcr |= lzcr >> 4;
	lzcr |= lzcr >> 8;
	lzcr |= lzcr >> 16;
	lzcr++;

    return debruijn32[lzcr * 0x076be629 >> 27];
}

int LIM(int value, int max, int min, unsigned int flag) {
	if (value > max) {
		C2_FLAG |= flag;
		return max;
	}
	else if (value < min) {
		C2_FLAG |= flag;
		return min;
	}

	return value;
}

#define _oB_ (gteRegs.GPR.r[_Rs_] + _Imm_)

inline long long gte_shift(long long a, int sf) {
	if (sf > 0)
		return a >> 12;
	else if (sf < 0)
		return a << 12;

	return a;
}

int BOUNDS(/*int44*/long long value, int max_flag, int min_flag) {
	if (value/*.positive_overflow()*/ > (long long)0x7ffffffffff)
		C2_FLAG |= max_flag;

	if (value/*.negative_overflow()*/ < (long long)-0x8000000000)
		C2_FLAG |= min_flag;

	return int(gte_shift(value/*.value()*/, m_sf));
}

unsigned int gte_divide(unsigned short numerator, unsigned short denominator)
{
	if (numerator < (denominator * 2))
	{
		static unsigned char table[] =
		{
			0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
			0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5, 0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
			0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
			0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
			0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
			0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
			0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
			0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
			0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
			0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
			0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
			0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
			0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
			0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
			0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
			0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
			0x00
		};

		int shift = gte_leadingzerocount(denominator) - 16;

		int r1 = (denominator << shift) & 0x7fff;
		int r2 = table[((r1 + 0x40) >> 7)] + 0x101;
		int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1ffff;
		unsigned int reciprocal = ((r2 * r3) + 0x80) >> 8;

		return (unsigned int)((((unsigned long long)reciprocal * (numerator << shift)) + 0x8000) >> 16);
	}

	return 0xffffffff;
}

/* Setting bits 12 & 19-22 in FLAG does not set bit 31 */

int A1(/*int44*/long long a) { return BOUNDS(a, (1 << 31) | (1 << 30), (1 << 31) | (1 << 27)); }
int A2(/*int44*/long long a) { return BOUNDS(a, (1 << 31) | (1 << 29), (1 << 31) | (1 << 26)); }
int A3(/*int44*/long long a) { m_mac3 = a; return BOUNDS(a, (1 << 31) | (1 << 28), (1 << 31) | (1 << 25)); }
int Lm_B1(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 24)); }
int Lm_B2(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 23)); }
int Lm_B3(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 22)); }

int Lm_B3_sf(long long value, int sf, int lm) {
	int value_sf = int(gte_shift(value, sf));
	int value_12 = int(gte_shift(value, 1));
	int max = 0x7fff;
	int min = 0;
	if (lm == 0)
		min = -0x8000;

	if (value_12 < -0x8000 || value_12 > 0x7fff)
		C2_FLAG |= (1 << 22);

	if (value_sf > max)
		return max;
	else if (value_sf < min)
		return min;

	return value_sf;
}

int Lm_C1(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 21)); }
int Lm_C2(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 20)); }
int Lm_C3(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 19)); }
int Lm_D(long long a, int sf) { return LIM(int(gte_shift(a, sf)), 0xffff, 0x0000, (1 << 31) | (1 << 18)); }

unsigned int Lm_E(unsigned int result) {
	if (result == 0xffffffff) {
		C2_FLAG |= (1 << 31) | (1 << 17);
		return 0x1ffff;
	}

	if (result > 0x1ffff)
		return 0x1ffff;

	return result;
}

long long F(long long a) {
	m_mac0 = a;

	if (a > 0x7fffffffLL)
		C2_FLAG |= (1 << 31) | (1 << 16);

	if (a < -0x80000000LL)
		C2_FLAG |= (1 << 31) | (1 << 15);

	return a;
}

int Lm_G1(long long a) {
	if (a > 0x3ff) {
		C2_FLAG |= (1 << 31) | (1 << 14);
		return 0x3ff;
	}
	if (a < -0x400) {
		C2_FLAG |= (1 << 31) | (1 << 14);
		return -0x400;
	}

	return int(a);
}

int Lm_G2(long long a) {
	if (a > 0x3ff) {
		C2_FLAG |= (1 << 31) | (1 << 13);
		return 0x3ff;
	}

	if (a < -0x400) {
		C2_FLAG |= (1 << 31) | (1 << 13);
		return -0x400;
	}

	return int(a);
}

int Lm_G1_ia(long long a) {
	if (a > 0x3ffffff)
		return 0x3ffffff;

	if (a < -0x4000000)
		return -0x4000000;

	return int(a);
}

int Lm_G2_ia(long long a) {
	if (a > 0x3ffffff)
		return 0x3ffffff;

	if (a < -0x4000000)
		return -0x4000000;

	return int(a);
}

int Lm_H(long long value, int sf) {
	long long value_sf = gte_shift(value, sf);
	int value_12 = int(gte_shift(value, 1));
	int max = 0x1000;
	int min = 0x0000;

	if (value_sf < min || value_sf > max)
		C2_FLAG |= (1 << 12);

	if (value_12 > max)
		return max;

	if (value_12 < min)
		return min;

	return value_12;
}



/* PGXP precise screen-coord FIFO, mirrors the GTE SXY0/SXY1/SXY2 FIFO so the
 * store macros can resolve a destination address to the precise float coord
 * the GTE just produced. Updated only when g_PsxUsePgxp. */
static float s_pgxpFifoX[3], s_pgxpFifoY[3], s_pgxpFifoW[3];

/* Parallel FIFO for the per-pixel flashlight: the GTE RTPS view-space (camera-
 * space) position MAC1/MAC2/MAC3 of each projected vertex. Mirrors the PGXP
 * SXY FIFO so the store hook can resolve a destination address to the view
 * position the GTE just produced. Updated only when g_PsyX_UsePerPixelFlashlight. */
static float s_vsFifoX[3], s_vsFifoY[3], s_vsFifoZ[3];

extern "C" int g_PgxpUseUnquantizedDepth; /* defined in PsyX_GPU.cpp */
extern "C" float g_PgxpGteOfx, g_PgxpGteOfy, g_PgxpGteH; /* PsyX_GPU.cpp */
extern "C" void VShadow_Store(void* addr, float x, float y, float z); /* PsyX_GPU.cpp */

/* Called from the gte_stsxy* store macros (only when g_PsxUsePgxp): the macro
 * just wrote the integer screen coord for FIFO slot `slot` (SXY0=0, SXY1=1,
 * SXY2=2) to `addr`, so record the shadow keyed by that address, validated by
 * the integer value the macro left there (DuckStation's SWC2 hook). */
extern "C" void Shadow_Store(void* addr, float x, float y, float w, unsigned value);
extern "C" void PGXP_StoreAddr(void* addr, int slot)
{
	if ((unsigned)slot > 2u) return;
	/* The gte_stsxy* macros now call this when (g_PsxUsePgxp ||
	 * g_PsyX_UsePerPixelFlashlight), so each store is independently gated:
	 * PGXP unchanged when its flag is off, flashlight a no-op when its flag is
	 * off, both off => not called at all (byte-identical legacy path). */
	if (g_PsxUsePgxp)
		Shadow_Store(addr, s_pgxpFifoX[slot], s_pgxpFifoY[slot], s_pgxpFifoW[slot], *(unsigned*)addr);
	/* View-space shadow also feeds the PGXP near-plane clipper, so it must be
	 * recorded whenever PGXP is on, not just for the per-pixel flashlight. Gate
	 * matches the vs FIFO fill in GTE_RotTransPers below. */
	if (g_PsyX_UsePerPixelFlashlight || g_PsxUsePgxp)
		VShadow_Store(addr, s_vsFifoX[slot], s_vsFifoY[slot], s_vsFifoZ[slot]);
}

int GTE_RotTransPers(int idx, int lm)
{
	int h_over_sz3;

	const long long rtpsMac1 = /*int44*/(long long)((long long)C2_TRX << 12) + (C2_R11 * VX(idx)) + (C2_R12 * VY(idx)) + (C2_R13 * VZ(idx));
	const long long rtpsMac2 = /*int44*/(long long)((long long)C2_TRY << 12) + (C2_R21 * VX(idx)) + (C2_R22 * VY(idx)) + (C2_R23 * VZ(idx));
	const long long rtpsMac3 = /*int44*/(long long)((long long)C2_TRZ << 12) + (C2_R31 * VX(idx)) + (C2_R32 * VY(idx)) + (C2_R33 * VZ(idx));
	C2_MAC1 = A1(rtpsMac1);
	C2_MAC2 = A2(rtpsMac2);
	C2_MAC3 = A3(rtpsMac3);
	C2_IR1 = Lm_B1(C2_MAC1, lm);
	C2_IR2 = Lm_B2(C2_MAC2, lm);
	C2_IR3 = Lm_B3_sf(m_mac3, m_sf, lm);
	C2_SZ0 = C2_SZ1;
	C2_SZ1 = C2_SZ2;
	C2_SZ2 = C2_SZ3;
	C2_SZ3 = Lm_D(m_mac3, 1);
	h_over_sz3 = Lm_E(gte_divide(C2_H, C2_SZ3));
	C2_SXY0 = C2_SXY1;
	C2_SXY1 = C2_SXY2;
	C2_SX2 = Lm_G1(F((long long)C2_OFX + ((long long)C2_IR1 * h_over_sz3)) >> 16);
	C2_SY2 = Lm_G2(F((long long)C2_OFY + ((long long)C2_IR2 * h_over_sz3)) >> 16);

	const bool capturePrecise = g_PsxUsePgxp || g_PsyX_UsePerPixelFlashlight;
	double viewX = 0.0, viewY = 0.0, viewZ = 0.0;
	if (capturePrecise)
	{
		/* Saturated IR/SZ coordinates paired with an unsaturated W do not form a
		 * coherent homogeneous position. Preserve the raw Q12 transform instead. */
		const double q12Scale = 1.0 / 4096.0;
		viewX = (double)rtpsMac1 * q12Scale;
		viewY = (double)rtpsMac2 * q12Scale;
		viewZ = (double)rtpsMac3 * q12Scale;

		/* When all nine rotation and all three translation registers still
		 * validate, PGXP may project through their unquantized twins.  Every
		 * legacy MAC/IR/SZ/SXY result above remains bit-identical. */
		ExactMatrix exactRotation;
		ExactTranslation exactTranslation;
		if (g_PsxUsePgxp && CurrentExact(exactRotation) && CurrentExactTranslation(exactTranslation))
		{
			double vx = (double)VX(idx);
			double vy = (double)VY(idx);
			double vz = (double)VZ(idx);
			ExactVector exactInput;
			if (CurrentExactVector(idx, exactInput))
			{
				vx = exactInput[0];
				vy = exactInput[1];
				vz = exactInput[2];
			}
			viewX = exactTranslation[0] + exactRotation[0] * vx + exactRotation[1] * vy + exactRotation[2] * vz;
			viewY = exactTranslation[1] + exactRotation[3] * vx + exactRotation[4] * vy + exactRotation[5] * vz;
			viewZ = exactTranslation[2] + exactRotation[6] * vx + exactRotation[7] * vy + exactRotation[8] * vz;
		}
	}

	/* PGXP: stash the full-precision projection keyed by the clamped integer screen
	 * coord the prim will store. Gated — zero cost / zero effect when PGXP is off. */
	if (g_PsxUsePgxp)
	{
		/* The hardware reciprocal and screen registers saturate near the eye and at
		 * the guard band. The precise twin uses the unsaturated view tuple so XY and W
		 * remain one coherent projection; the legacy GTE registers above stay intact. */
		double fx, fy;
		float  pgxpW;
		if (viewZ > 0.0) {
			double ratio = (double)C2_H / viewZ;
			fx = (double)C2_OFX / 65536.0 + viewX * ratio;
			fy = (double)C2_OFY / 65536.0 + viewY * ratio;
			/* The optional legacy value remains available for console A/B testing. */
			pgxpW = g_PgxpUseUnquantizedDepth ? (float)viewZ : (float)C2_SZ3;
		} else {
			/* At / behind the eye there is no valid projection; the clipper handles crossings. */
			fx = (double)C2_SX2; fy = (double)C2_SY2;
			pgxpW = 0.0f;
		}

		/* Mirror the GTE SXY FIFO with a precise FIFO so the gte_stsxy* store macros
		 * (which know the destination address but not the precise value) can record
		 * address->precise deterministically. Shift exactly as the C2_SXY shift above. */
		s_pgxpFifoX[0] = s_pgxpFifoX[1]; s_pgxpFifoX[1] = s_pgxpFifoX[2]; s_pgxpFifoX[2] = (float)fx;
		s_pgxpFifoY[0] = s_pgxpFifoY[1]; s_pgxpFifoY[1] = s_pgxpFifoY[2]; s_pgxpFifoY[2] = (float)fy;
		s_pgxpFifoW[0] = s_pgxpFifoW[1]; s_pgxpFifoW[1] = s_pgxpFifoW[2]; s_pgxpFifoW[2] = pgxpW;

		/* Near-clip reprojection constants: the projection registers active when
		 * this vertex was transformed. The GL near-plane clipper re-projects the
		 * clip vertices it creates with the exact same formula (sx = OFX + x*H/z).
		 * Per-frame constants in SH1, so plain globals suffice. */
		g_PgxpGteOfx = (float)((double)C2_OFX / 65536.0);
		g_PgxpGteOfy = (float)((double)C2_OFY / 65536.0);
		g_PgxpGteH   = (float)C2_H;
	}

	/* View-space FIFO: stash this vertex's camera-space position (RTPS
	 * MAC1/MAC2/MAC3). Source data for the per-pixel flashlight AND the PGXP
	 * near-plane clipper, so it runs when either is on. Mirrors the SXY FIFO
	 * shift above so gte_stsxy* can resolve address->view-pos. Off = no cost. */
	if (capturePrecise)
	{
		s_vsFifoX[0] = s_vsFifoX[1]; s_vsFifoX[1] = s_vsFifoX[2]; s_vsFifoX[2] = (float)viewX;
		s_vsFifoY[0] = s_vsFifoY[1]; s_vsFifoY[1] = s_vsFifoY[2]; s_vsFifoY[2] = (float)viewY;
		s_vsFifoZ[0] = s_vsFifoZ[1]; s_vsFifoZ[1] = s_vsFifoZ[2]; s_vsFifoZ[2] = (float)viewZ;
	}

	return h_over_sz3;
}

int GTE_operator(int op)
{
	int v;
	int lm;
	int cv;
	int mx;
	int h_over_sz3 = 0;

	lm = GTE_LM(gteop(op));
	m_sf = GTE_SF(gteop(op));

	C2_FLAG = 0;

	switch (GTE_FUNCT(gteop(op)))
	{
	case 0x00:
	case 0x01:
#ifdef GTE_LOG
		GTELOG("%08x RTPS", op);
#endif
		h_over_sz3 = GTE_RotTransPers(0, lm);

		C2_MAC0 = int(F((long long)C2_DQB + ((long long)C2_DQA * h_over_sz3)));
		C2_IR0 = Lm_H(m_mac0, 1);

		return 1;

	case 0x06:
#ifdef GTE_LOG
		GTELOG("%08x NCLIP", op);
#endif
		C2_MAC0 = int(F((long long)(C2_SX0 * C2_SY1) + (C2_SX1 * C2_SY2) + (C2_SX2 * C2_SY0) - (C2_SX0 * C2_SY2) - (C2_SX1 * C2_SY0) - (C2_SX2 * C2_SY1)));
		C2_FLAG = 0;
		return 1;

	case 0x0c:
#ifdef GTE_LOG
		GTELOG("%08x OP", op);
#endif

		C2_MAC1 = A1((long long)(C2_R22 * C2_IR3) - (C2_R33 * C2_IR2));
		C2_MAC2 = A2((long long)(C2_R33 * C2_IR1) - (C2_R11 * C2_IR3));
		C2_MAC3 = A3((long long)(C2_R11 * C2_IR2) - (C2_R22 * C2_IR1));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x10:
#ifdef GTE_LOG
		GTELOG("%08x DPCS", op);
#endif

		C2_MAC1 = A1((C2_R << 16) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_R << 16)), 0)));
		C2_MAC2 = A2((C2_G << 16) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_G << 16)), 0)));
		C2_MAC3 = A3((C2_B << 16) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_B << 16)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x11:
#ifdef GTE_LOG
		GTELOG("%08x INTPL", op);
#endif

		C2_MAC1 = A1((C2_IR1 << 12) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_IR1 << 12)), 0)));
		C2_MAC2 = A2((C2_IR2 << 12) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_IR2 << 12)), 0)));
		C2_MAC3 = A3((C2_IR3 << 12) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_IR3 << 12)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x12:
#ifdef GTE_LOG
		GTELOG("%08x MVMVA", op);
#endif

		mx = GTE_MX(gteop(op));
		v = GTE_V(gteop(op));
		cv = GTE_CV(gteop(op));

		switch (cv) {
		case 2:
			C2_MAC1 = A1((long long)(MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)));
			C2_MAC2 = A2((long long)(MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)));
			C2_MAC3 = A3((long long)(MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)));
			Lm_B1(A1(((long long)CV1(cv) << 12) + (MX11(mx) * VX(v))), 0);
			Lm_B2(A2(((long long)CV2(cv) << 12) + (MX21(mx) * VX(v))), 0);
			Lm_B3(A3(((long long)CV3(cv) << 12) + (MX31(mx) * VX(v))), 0);
			break;

		default:
			C2_MAC1 = A1(/*int44*/(long long)((long long)CV1(cv) << 12) + (MX11(mx) * VX(v)) + (MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)));
			C2_MAC2 = A2(/*int44*/(long long)((long long)CV2(cv) << 12) + (MX21(mx) * VX(v)) + (MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)));
			C2_MAC3 = A3(/*int44*/(long long)((long long)CV3(cv) << 12) + (MX31(mx) * VX(v)) + (MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)));
			break;
		}

		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x13:
#ifdef GTE_LOG
		GTELOG("%08x NCDS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x14:
#ifdef GTE_LOG
		GTELOG("%08x CDP", op);
#endif

		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x16:
#ifdef GTE_LOG
		GTELOG("%08x NCDT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
			C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
			C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x1b:
#ifdef GTE_LOG
		GTELOG("%08x NCCS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1c:
#ifdef GTE_LOG
		GTELOG("%08x CC", op);
#endif

		C2_MAC1 = A1(/*int44*/(long long)(((long long)C2_RBK) << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)(((long long)C2_GBK) << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)(((long long)C2_BBK) << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1e:
#ifdef GTE_LOG
		GTELOG("%08x NCS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x20:
#ifdef GTE_LOG
		GTELOG("%08x NCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x28:
#ifdef GTE_LOG
		GTELOG("%08x SQR", op);
#endif

		C2_MAC1 = A1(C2_IR1 * C2_IR1);
		C2_MAC2 = A2(C2_IR2 * C2_IR2);
		C2_MAC3 = A3(C2_IR3 * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x29:
#ifdef GTE_LOG
		GTELOG("%08x DPCL", op);
#endif

		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x2a:
#ifdef GTE_LOG
		GTELOG("%08x DPCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((C2_R0 << 16) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_R0 << 16)), 0)));
			C2_MAC2 = A2((C2_G0 << 16) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_G0 << 16)), 0)));
			C2_MAC3 = A3((C2_B0 << 16) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_B0 << 16)), 0)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x2d:
#ifdef GTE_LOG
		GTELOG("%08x AVSZ3", op);
#endif

		C2_MAC0 = int(F((long long)(C2_ZSF3 * C2_SZ1) + (C2_ZSF3 * C2_SZ2) + (C2_ZSF3 * C2_SZ3)));
		C2_OTZ = Lm_D(m_mac0, 1);
		return 1;

	case 0x2e:
#ifdef GTE_LOG
		GTELOG("%08x AVSZ4", op);
#endif

		C2_MAC0 = int(F((long long)(C2_ZSF4 * C2_SZ0) + (C2_ZSF4 * C2_SZ1) + (C2_ZSF4 * C2_SZ2) + (C2_ZSF4 * C2_SZ3)));
		C2_OTZ = Lm_D(m_mac0, 1);
		return 1;

	case 0x30:
#ifdef GTE_LOG
		GTELOG("%08x RTPT", op);
#endif

		for (v = 0; v < 3; v++)
			h_over_sz3 = GTE_RotTransPers(v, lm);

		C2_MAC0 = int(F((long long)C2_DQB + ((long long)C2_DQA * h_over_sz3)));
		C2_IR0 = Lm_H(m_mac0, 1);
		return 1;

	case 0x3d:
#ifdef GTE_LOG
		GTELOG("%08x GPF", op);
#endif

		C2_MAC1 = A1(C2_IR0 * C2_IR1);
		C2_MAC2 = A2(C2_IR0 * C2_IR2);
		C2_MAC3 = A3(C2_IR0 * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3e:
#ifdef GTE_LOG
		GTELOG("%08x GPL", op);
#endif

		C2_MAC1 = A1(gte_shift(C2_MAC1, -m_sf) + (C2_IR0 * C2_IR1));
		C2_MAC2 = A2(gte_shift(C2_MAC2, -m_sf) + (C2_IR0 * C2_IR2));
		C2_MAC3 = A3(gte_shift(C2_MAC3, -m_sf) + (C2_IR0 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3f:
#ifdef GTE_LOG
		GTELOG("%08x NCCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1((C2_R << 4) * C2_IR1);
			C2_MAC2 = A2((C2_G << 4) * C2_IR2);
			C2_MAC3 = A3((C2_B << 4) * C2_IR3);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;
	}

	return 0;
}
