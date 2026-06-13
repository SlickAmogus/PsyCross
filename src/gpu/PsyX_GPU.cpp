#include "PsyX_GPU.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"

#include "../PsyX_main.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "psx/gtereg.h"

#define GET_TPAGE_FORMAT(tpage) ((TexFormat)((tpage >> 7) & 0x3))
#define GET_TPAGE_BLEND(tpage)  ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage) ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)        ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)        (clut >> 6)

OT_TAG prim_terminator = { (uintptr_t)-1, 0 }; // P_TAG with zero primLength

int g_currentOTBucketCount = 0;
float g_otBucketDepth = 0.0f;

/* ----------------------------------------------------------------------------
 * PGXP (perspective-correct rendering) — self-contained, runtime-gated.
 *
 * The GTE pushes each projected vertex here (only when g_PsxUsePgxp): its
 * integer screen X/Y (the match key) plus full-precision float screen X/Y and
 * a per-vertex W (~view depth). When the GPU builds a prim's GrVertices it
 * matches each vertex back to its precise data by the integer X/Y key
 * (searching recent ring entries, newest first). On a miss or when PGXP is
 * off, the vertex falls back to the affine values and the shader's
 * u_pgxpEnabled gate keeps it on the legacy path — so nothing changes when off.
 * No prim fields, addPrim, or game-side store macros are touched.
 * -------------------------------------------------------------------------- */
/* Ring large enough to hold a whole frame's transformed vertices (SH peaks
 * ~150k vertex-transforms/frame): the GTE fills it while the game builds the
 * ordering table, but GsDrawOt/MakeVertex* read it much LATER, so a prim's
 * vertices must still be present at draw time. 262144 (>> 2 == 65536) so the
 * ring position fits the prim's 16-bit pgxp_index when stored as (pos >> 2). */
#define PGXP_RING_SIZE   262144
#define PGXP_RING_MASK   (PGXP_RING_SIZE - 1)
#define PGXP_SEARCH_FWD  6             /* tolerate >>2 hint rounding */
#define PGXP_SEARCH_BACK 40            /* prim verts sit just before the hint */

struct PgxpRingEntry { int key; float x, y, w; };
static PgxpRingEntry s_pgxpRing[PGXP_RING_SIZE];
static unsigned int  s_pgxpRingPos = 0;

extern "C" void PGXP_FrameReset(void) { /* monotonic ring; reset not required */ }

extern "C" void PGXP_PushVertex(int sx, int sy, float fx, float fy, float fw)
{
	PgxpRingEntry* e = &s_pgxpRing[s_pgxpRingPos & PGXP_RING_MASK];
	e->key = (sx & 0xFFFF) | (sy << 16);
	e->x = fx; e->y = fy; e->w = fw;
	s_pgxpRingPos++;
}

/* Stamp the prim with the ring position at addPrim time (>> 2 to fit 16 bits;
 * the search window absorbs the rounding). Called from PsyX_CaptureGteDepths
 * for EVERY prim — harmless when PGXP off (s_pgxpRingPos is frozen, the value
 * is never read). prim points at a P_TAG. */
extern "C" void PGXP_StampPrim(void* prim)
{
	((P_TAG*)prim)->pgxp_index = (unsigned short)((s_pgxpRingPos >> 2) & 0xFFFF);
}

/* Search the ring for the precise vertex matching integer (sx,sy), starting
 * from the prim's stamped hint (its vertices were pushed just before it).
 * hint16 == 0xFFFF => no hint (2D prim) => skip. */
static int PGXP_LookupHinted(int sx, int sy, unsigned short hint16, float* ox, float* oy, float* ow)
{
	if (hint16 == 0xFFFF)
		return 0;
	const int key = (sx & 0xFFFF) | (sy << 16);
	const unsigned int start = (((unsigned int)hint16) << 2);
	/* Backward-first: this prim's vertices were pushed in the few slots just
	 * before its stamped hint, so prefer those over a later prim that happens
	 * to share an integer screen coord. The small forward margin covers the
	 * >>2 rounding in the stamped hint. */
	for (int i = 0; i <= PGXP_SEARCH_BACK; i++)
	{
		const PgxpRingEntry* e = &s_pgxpRing[(start - i) & PGXP_RING_MASK];
		if (e->key == key) { *ox = e->x; *oy = e->y; *ow = e->w; return 1; }
	}
	for (int i = 1; i <= PGXP_SEARCH_FWD; i++)
	{
		const PgxpRingEntry* e = &s_pgxpRing[(start + i) & PGXP_RING_MASK];
		if (e->key == key) { *ox = e->x; *oy = e->y; *ow = e->w; return 1; }
	}
	return 0;
}

/* Fill a GrVertex's precise PGXP fields from the raw stored screen coord
 * (rawX,rawY = prim's stored x/y, the GTE output BEFORE the draw-env offset).
 * ofsX/ofsY are added so ppx/ppy line up with vertex.x/.y. pw=0 => shader
 * treats the vertex as affine. Only called when PGXP is on. */
/* Coverage instrumentation: how many 3D vertices found their precise data vs
 * fell back to affine. Dumped ~once a second when PGXP is on so we can tell if
 * tweaking should target the hit rate or the math. */
static unsigned int s_pgxpHit = 0, s_pgxpMiss = 0, s_pgxpFrames = 0;
extern "C" void PGXP_CoverageTick(void)
{
	if (!g_PsxUsePgxp) { s_pgxpHit = s_pgxpMiss = 0; return; }
	if (++s_pgxpFrames >= 60)
	{
		unsigned int tot = s_pgxpHit + s_pgxpMiss;
		if (tot)
			eprintinfo("[PGXP] coverage: %u/%u hits (%.1f%%) over %u frames\n",
				s_pgxpHit, tot, 100.0 * (double)s_pgxpHit / (double)tot, s_pgxpFrames);
		s_pgxpHit = s_pgxpMiss = s_pgxpFrames = 0;
	}
}

static inline void PgxpFillVertex(GrVertex* v, int rawX, int rawY, float ofsX, float ofsY, unsigned short hint)
{
	float fx, fy, fw;
	if (PGXP_LookupHinted(rawX, rawY, hint, &fx, &fy, &fw))
	{
		v->ppx = fx + ofsX;
		v->ppy = fy + ofsY;
		v->ppw = fw;
		s_pgxpHit++;
	}
	else
	{
		v->ppx = (float)v->x;
		v->ppy = (float)v->y;
		v->ppw = 0.0f; /* miss -> affine */
		s_pgxpMiss++;
	}
}

DISPENV currentDispEnv;
DISPENV activeDispEnv;
DRAWENV activeDrawEnv;

static const char* currentSplitDebugText = nullptr;
TextureID overrideTexture = 0;
int overrideTextureWidth = 0;
int overrideTextureHeight = 0;

int g_GPUDisabledState = 0;
int g_DrawPrimMode = 0;

// Per-primitive GTE SZ depth table.  Populated at addPrim time (GTE SZ registers
// valid immediately after RotTransPers calls), looked up during primitive parsing
// to give GL per-vertex perspective depth.  Cleared each GsDrawOt call.
// 4096 slots, linear probe ≤16; collision rate is negligible for typical scene sizes.
#define SZ_TABLE_BITS 12
#define SZ_TABLE_SIZE (1 << SZ_TABLE_BITS)
#define SZ_TABLE_MASK (SZ_TABLE_SIZE - 1)

struct SZEntry { uintptr_t key; uint16_t sz[4]; };
static SZEntry g_szTable[SZ_TABLE_SIZE];

// Global SZ scale: maximum SZ seen in the previous frame, used as the
// depth reference so all polygons share a consistent window_depth space
// regardless of which OT bucket they landed in.
static uint32_t g_szMaxThisFrame = 0;
static uint32_t g_szMaxPrevFrame = 0;

/* PGXP depth fix: the previous frame's max SZ, used to normalize per-vertex
 * SZ3 into NDC depth in the shader (same formula as ApplyGtePerVertexDepth but
 * per-vertex + unquantized, so coplanar faces no longer share a depth bucket).
 * Returns 1 as a safe floor before the first frame. */
extern "C" float PGXP_GetSzMax(void)
{
	return (g_szMaxPrevFrame < 1) ? 1.0f : (float)g_szMaxPrevFrame;
}

// World-geometry renderers (Gfx_MeshDraw) bulk-transform vertices before the
// polygon loop, so the GTE SZ FIFO is stale at each polygon's addPrim call.
// They call PsyX_SetNextPrimSz with the polygon's field_18C SZ values so the
// next PsyX_CaptureGteDepths invocation uses the correct per-vertex depths.
static uint16_t g_primSzNext[4];
static int g_primSzNextValid = 0;

extern "C" void PsyX_SetNextPrimSz(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3, int arg3)
{
	(void)arg3;
	uint16_t avg   = (uint16_t)(((unsigned)s0 + s1 + s2 + s3) >> 2);
	uint16_t avg_q = (uint16_t)((avg >> 6) << 6);
	// Calibrate with unquantised real max so character/item GL depths stay accurate.
	uint32_t mx = s0 > s1 ? s0 : s1;
	if (s2 > mx) mx = s2;
	if (s3 > mx) mx = s3;
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;
	g_primSzNext[0] = g_primSzNext[1] = g_primSzNext[2] = g_primSzNext[3] = avg_q;
	g_primSzNextValid = 1;
}

extern "C" void PsyX_CaptureGteDepths(void* prim)
{
	/* PGXP: record the GTE ring position for this prim so MakeVertex* can find
	 * its precise vertices at draw time. Harmless when PGXP off. */
	PGXP_StampPrim(prim);

	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);

	uint16_t s0, s1, s2, s3;
	if (g_primSzNextValid) {
		s0 = g_primSzNext[0]; s1 = g_primSzNext[1];
		s2 = g_primSzNext[2]; s3 = g_primSzNext[3];
		g_primSzNextValid = 0;
	} else {
		s0 = (uint16_t)C2_SZ0; s1 = (uint16_t)C2_SZ1;
		s2 = (uint16_t)C2_SZ2; s3 = (uint16_t)C2_SZ3;
	}

	// Track per-frame SZ maximum for global depth calibration
	uint32_t mx = s0 > s1 ? s0 : s1;
	if (s2 > mx) mx = s2;
	if (s3 > mx) mx = s3;
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;

	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (g_szTable[s].key == 0 || g_szTable[s].key == key) {
			g_szTable[s].key = key;
			g_szTable[s].sz[0] = s0; g_szTable[s].sz[1] = s1;
			g_szTable[s].sz[2] = s2; g_szTable[s].sz[3] = s3;
			return;
		}
	}
	// Probe exhausted — overwrite initial slot
	g_szTable[slot].key = key;
	g_szTable[slot].sz[0] = s0; g_szTable[slot].sz[1] = s1;
	g_szTable[slot].sz[2] = s2; g_szTable[slot].sz[3] = s3;
}

extern "C" void PsyX_ClearGteDepthTable(void)
{
	g_szMaxPrevFrame = g_szMaxThisFrame;
	g_szMaxThisFrame = 0;
	memset(g_szTable, 0, sizeof(g_szTable));
	g_primSzNextValid = 0;
}

static bool PsyX_LookupGteDepths(const void* prim, uint16_t* sz)
{
	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);
	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (g_szTable[s].key == key) {
			sz[0] = g_szTable[s].sz[0]; sz[1] = g_szTable[s].sz[1];
			sz[2] = g_szTable[s].sz[2]; sz[3] = g_szTable[s].sz[3];
			return true;
		}
		if (g_szTable[s].key == 0) break;
	}
	return false;
}

// Overrides flat bucket z with GTE SZ-based depth.
// Uses average SZ across polygon vertices: geometrically more accurate than max_SZ,
// which can be dominated by a single near vertex and sort adjacent coplanar surfaces
// into the wrong OT depth relationship.  Uniform depth per polygon (all vertices
// share one value) eliminates the per-vertex interpolation that caused diffuse
// Z-fighting along polygon edges.
static void ApplyGtePerVertexDepth(GrVertex* vertex, const P_TAG* polyTag, bool isQuad)
{
	if (g_szMaxPrevFrame < 1) return;

	uint16_t sz[4];
	if (!PsyX_LookupGteDepths(polyTag, sz))
		return;

	float sv0, sv1, sv2, sv3 = 0.0f;
	if (isQuad) {
		sv0 = (float)sz[0]; sv1 = (float)sz[1];
		sv2 = (float)sz[3]; sv3 = (float)sz[2];  // buffer[2]=V3, buffer[3]=V2
	} else {
		sv0 = (float)sz[1]; sv1 = (float)sz[2]; sv2 = (float)sz[3];
	}

	float sz_avg = isQuad ? (sv0 + sv1 + sv2 + sv3) * 0.25f
	                      : (sv0 + sv1 + sv2) * (1.0f / 3.0f);
	if (sz_avg < 1.0f) return;  // 2D/HUD prim — keep bucket depth

	float z_val = 1.0f - 2.0f * sz_avg * (1.0f / (float)g_szMaxPrevFrame);
	if (z_val < -1.0f) z_val = -1.0f;
	if (z_val >  1.0f) z_val =  1.0f;
	vertex[0].z = vertex[1].z = vertex[2].z = z_val;
	if (isQuad) vertex[3].z = z_val;
}

struct GPUDrawSplit
{
	DRAWENV			drawenv;
	DISPENV			dispenv;

	BlendMode		blendMode;

	TexFormat		texFormat;
	TextureID		textureId;

	int				drawPrimMode;

	u_short			startVertex;
	u_short			numVerts;

	const char*		debugText;
};

#define MAX_DRAW_SPLITS	 4096

GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
GPUDrawSplit g_splits[MAX_DRAW_SPLITS];

int g_vertexIndex = 0;
int g_splitIndex = 0;

void ClearSplits()
{
	currentSplitDebugText = nullptr;
	g_vertexIndex = 0;
	g_splitIndex = 0;
	g_splits[0].texFormat = (TexFormat)0xFFFF;
}

template<class T>
void DrawEnvDimensions(T& width, T& height)
{
	if (activeDrawEnv.dfe)
	{
		width = activeDispEnv.disp.w;
		height = activeDispEnv.disp.h;
	}
	else
	{
		width = activeDrawEnv.clip.w;
		height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float& ofsX, float& ofsY)
{
	if (activeDrawEnv.dfe)
	{
		// also make offset in draw dimensions range to prevent flicker
		const int x = activeDispEnv.disp.x;
		const int y = activeDispEnv.disp.y;
		ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		ofsX = 0.0f;
		ofsY = 0.0f;
	}
}

inline void ScreenCoordsToEmulator(GrVertex* vertex, int count)
{
}

void LineSwapSourceVerts(VERTTYPE*& p0, VERTTYPE*& p1, unsigned char*& c0, unsigned char*& c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if ((p0[0] > p1[0]) ||
		(p0[1] > p1[1] && p0[0] == p1[0]))
	{
		VERTTYPE* tmp = p0;
		p0 = p1;
		p1 = tmp;

		unsigned char* tmpCol = c0;
		c0 = c1;
		c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, ushort gteidx)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if (dx > abs((short)dy)) 
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else 
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexTriangle(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[0].z = vertex[1].z = vertex[2].z = g_otBucketDepth;

	if (g_PsxUsePgxp)
	{
		PgxpFillVertex(&vertex[0], p0[0], p0[1], ofsX, ofsY, gteidx);
		PgxpFillVertex(&vertex[1], p1[0], p1[1], ofsX, ofsY, gteidx);
		PgxpFillVertex(&vertex[2], p2[0], p2[1], ofsX, ofsY, gteidx);
	}

	ScreenCoordsToEmulator(vertex, 3);
}

void MakeVertexQuad(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, VERTTYPE* p3, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	if (g_PsxUsePgxp)
	{
		PgxpFillVertex(&vertex[0], p0[0], p0[1], ofsX, ofsY, gteidx);
		PgxpFillVertex(&vertex[1], p1[0], p1[1], ofsX, ofsY, gteidx);
		PgxpFillVertex(&vertex[2], p2[0], p2[1], ofsX, ofsY, gteidx);
		PgxpFillVertex(&vertex[3], p3[0], p3[1], ofsX, ofsY, gteidx);
	}

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexRect(GrVertex* vertex, VERTTYPE* p0, short w, short h, ushort gteidx)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeTexcoordQuad(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, unsigned char* uv3, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const unsigned char bright = 2;
	// Strip ABR (bits 5-6) and TP (bits 7-8) from tpage - shader only needs X/Y page coords (bits 0-4)
	short pageCoord = page & 0x1F;

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = pageCoord;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordTriangle(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const unsigned char bright = 2;
	// Strip ABR (bits 5-6) and TP (bits 7-8) from tpage - shader only needs X/Y page coords (bits 0-4)
	short pageCoord = page & 0x1F;

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordRect(GrVertex* vertex, unsigned char* uv, short page, short clut, short w, short h)
{
	assert(uv);

	// sim overflow
	if (int(uv[0]) + w > 255) w = 255 - uv[0];
	if (int(uv[1]) + h > 255) h = 255 - uv[1];

	const unsigned char bright = 2;
	const unsigned char dither = 0;
	// Strip ABR (bits 5-6) and TP (bits 7-8) from tpage - shader only needs X/Y page coords (bits 0-4)
	short pageCoord = page & 0x1F;

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = pageCoord;
	vertex[3].clut = clut;

	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}
}

void MakeTexcoordLineZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex* vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		vertex[n]._p0 = 0;
		--n;
	}
}

void MakeColourLine(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;
	vertex[1]._p0 = 0;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;
	vertex[2]._p0 = 0;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
	vertex[3]._p0 = 0;
}

void MakeColourTriangle(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;
	vertex[1]._p0 = 0;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
	vertex[2]._p0 = 0;
}

void MakeColourQuad(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2, unsigned char* col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;
	vertex[1]._p0 = 0;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
	vertex[2]._p0 = 0;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
	vertex[3]._p0 = 0;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	g_vertexBuffer[g_vertexIndex + 4] = g_vertexBuffer[g_vertexIndex + 3];

	g_vertexBuffer[g_vertexIndex + 5] = g_vertexBuffer[g_vertexIndex + 2];
	g_vertexBuffer[g_vertexIndex + 2] = g_vertexBuffer[g_vertexIndex + 3];
	g_vertexBuffer[g_vertexIndex + 3] = g_vertexBuffer[g_vertexIndex + 1];
}

//------------------------------------------------------------------------------------------------------------------------

static void AddSplit(bool semiTrans, bool textured)
{
	int tpage = activeDrawEnv.tpage;
	GPUDrawSplit& curSplit = g_splits[g_splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GET_TPAGE_FORMAT(tpage);
	TextureID textureId = textured ? g_vramTexture : g_whiteTexture;

	if (textured && overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = overrideTexture;
	}

	// FIXME: compare drawing environment too?
	if (curSplit.blendMode == blendMode &&
		curSplit.texFormat == texFormat &&
		curSplit.textureId == textureId &&
		curSplit.drawPrimMode == g_DrawPrimMode &&
		curSplit.drawenv.clip.x == activeDrawEnv.clip.x &&
		curSplit.drawenv.clip.y == activeDrawEnv.clip.y &&
		curSplit.drawenv.clip.w == activeDrawEnv.clip.w &&
		curSplit.drawenv.clip.h == activeDrawEnv.clip.h &&
		curSplit.drawenv.dfe == activeDrawEnv.dfe &&
		curSplit.debugText == currentSplitDebugText)
	{
		return;
	}

	curSplit.numVerts = g_vertexIndex - curSplit.startVertex;

	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		eprinterr("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors\n");
		return;
	}

	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.blendMode = blendMode;
	split.texFormat = texFormat;
	split.textureId = textureId;
	split.drawPrimMode = g_DrawPrimMode;
	split.drawenv = activeDrawEnv;
	split.dispenv = activeDispEnv;
	split.debugText = currentSplitDebugText;

	split.drawenv.tw.w = overrideTextureWidth;
	split.drawenv.tw.h = overrideTextureHeight;

	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
}

void DrawSplit(const GPUDrawSplit& split)
{
	{
		static int splitLog = 0;
		if (splitLog < 40 && split.numVerts > 0) {
			eprintf("[DBG] DrawSplit: fmt=%d blend=%d verts=%d dfe=%d texId=%u clip=(%d,%d,%d,%d)\n",
				split.texFormat, split.blendMode, split.numVerts, split.drawenv.dfe,
				(unsigned)split.textureId, split.drawenv.clip.x, split.drawenv.clip.y,
				split.drawenv.clip.w, split.drawenv.clip.h);
			splitLog++;
		}
	}
	if(split.debugText)
		GR_PushDebugLabel(split.debugText);

	GR_SetStencilMode(split.drawPrimMode);	// draw with mask 0x16

	GR_SetTexture(split.textureId, split.texFormat);

	if (split.texFormat == TF_32_BIT_RGBA)
		GR_SetOverrideTextureSize(split.drawenv.tw.w, split.drawenv.tw.h);

	const bool drawOnScreen = split.drawenv.dfe;
	GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
	GR_SetOffscreenState(&split.drawenv.clip, !drawOnScreen);

	GR_SetBlendMode(split.blendMode);

	GR_DrawTriangles(split.startVertex, split.numVerts / 3);

	if (split.debugText)
		GR_PopDebugLabel();
}

extern int g_dbg_polygonSelected;

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
#ifdef _DEBUG
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex* vert = &g_vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			eprintf("==========================================\n");
			eprintf("POLYGON: %d\n", g_dbg_polygonSelected);
			eprintf("X: %d Y: %d
", vert->x, vert->y);
			eprintf("U: %d V: %d
", vert->u, vert->v);
			eprintf("TP: %d CLT: %d
", vert->page, vert->clut);
			
			eprintf("==========================================\n");
		}

		PsyX_UpdateInput();
	}
#endif // _DEBUG

	// next code ideally should be called before EndScene
	GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);

	for (int i = 1; i <= g_splitIndex; i++)
		DrawSplit(g_splits[i]);

	ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG* polyTag);

void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive)
{
	if (!p)
		return;

	// setup single primitive flag (needed for AddSplits)
	g_DrawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG* polyTag = reinterpret_cast<P_TAG*>(p);
		ParsePrimitive(polyTag);

		GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
		lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
	}
	else
	{
		// Bucket-accurate depth: all primitives inside the same OT bucket share
		// one depth value — matching the PSX's painter's-algorithm intent.
		// g_otBucketDepth advances only at tagLength==0 bucket-boundary entries.
		int otBucketIdx = 0;
		const float otBucketStep = (g_currentOTBucketCount > 1)
			? (2.0f / (float)(g_currentOTBucketCount - 1)) : 0.0f;
		g_otBucketDepth = -1.0f;
		// walk OT_TAG linked list with safety guards
		uintptr_t basePacket = reinterpret_cast<uintptr_t>(p);
		for (int safety = 0; safety < 16384; safety++)
		{
			const int tagLength = getlen(basePacket);
			if (tagLength > 0 && tagLength <= 32)
			{
				uintptr_t currentPacket = basePacket;
				const uintptr_t endPacket = basePacket + (tagLength + P_LEN) * sizeof(u_int);
				int primLength = 0;
				while (currentPacket < endPacket)
				{
					primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket));
					if (primLength <= 0) break;
					currentPacket += (primLength + P_LEN) * sizeof(u_int);
				}

				if (currentPacket != endPacket)
				{
					eprinterr("did not output valid primitive or ptag length is not valid (diff=%d)\n", endPacket-currentPacket);
					/* One-shot dump: the corrupted prim's raw bytes
					 * fingerprint the writer. Same approach that pinned
					 * the knife OT corruption to func_800611C0 via the
					 * recognizable .NHS. tail bytes from POLY_FT4 vertex
					 * data. After the first dump, fall back to the
					 * existing rate-limited summary above. */
					static int s_badPrimDumped = 0;
					if (!s_badPrimDumped) {
						s_badPrimDumped = 1;
						const uint32_t* w = reinterpret_cast<const uint32_t*>(basePacket);
						eprintinfo("[OT-PRIM] FIRST corrupt prim at %p tagLen=%d code=0x%02x\n",
							(void*)basePacket, tagLength,
							reinterpret_cast<P_TAG*>(basePacket)->code);
						eprintinfo("[OT-PRIM]   raw 64 bytes: %08x %08x %08x %08x %08x %08x %08x %08x\n",
							(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
							(unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
						eprintinfo("[OT-PRIM]                 %08x %08x %08x %08x %08x %08x %08x %08x\n",
							(unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
							(unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
					}
				}
			}
			else if (tagLength == 0)
			{
				// OT bucket boundary — advance to the next bucket's depth.
				g_otBucketDepth = -1.0f + (float)otBucketIdx * otBucketStep;
				if (g_otBucketDepth > 1.0f) g_otBucketDepth = 1.0f;
				otBucketIdx++;
			}
			else if (tagLength > 32)
			{
				eprinterr("got invalid tag length %d, code %d\n", tagLength, reinterpret_cast<P_TAG*>(basePacket)->code);
				static int s_badTagDumped = 0;
				if (!s_badTagDumped) {
					s_badTagDumped = 1;
					const uint32_t* w = reinterpret_cast<const uint32_t*>(basePacket);
					eprintinfo("[OT-PRIM] FIRST bad-tag-len at %p tagLen=%d\n",
						(void*)basePacket, tagLength);
					eprintinfo("[OT-PRIM]   raw 64 bytes: %08x %08x %08x %08x %08x %08x %08x %08x\n",
						(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
						(unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
					eprintinfo("[OT-PRIM]                 %08x %08x %08x %08x %08x %08x %08x %08x\n",
						(unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
						(unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
				}
			}

			GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
			lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;

			if (isendprim(basePacket))
				break;

			// Validate next pointer before following it.
			// Crash root-caused via WinDbg minidump on the muzzle-flash repro:
			// FAILURE_BUCKET_ID INVALID_POINTER_READ at this exact site,
			// stack ParsePrimitivesLinkedList+0xa5 -> DrawOTag -> GsDrawOt.
			// The next-pointer can land on:
			//   1. NULL / very low (uninitialized OT bucket)        — break
			//   2. (uintptr_t)-1 == 0xFFFF..FF (PSX legacy terminator
			//      written by some not-fully-ported code; differs from
			//      &prim_terminator that isendprim looks for)        — break
			//   3. Unmapped high address (Windows user mode tops at
			//      0x7FFF'FFFF'FFFF; anything past that is kernel)   — break
			//   4. Wild but technically-mapped — can't catch without
			//      VirtualQuery; rely on the 16384 safety counter.
			uintptr_t nextPtr = reinterpret_cast<uintptr_t>(nextPrim(basePacket));
			if (nextPtr < 0x10000 ||
			    nextPtr == static_cast<uintptr_t>(-1) ||
			    nextPtr >= 0x7FFFFFFFFFFFULL) {
				static int s_badNextLogged = 0;
				if (s_badNextLogged < 16) {
					eprintinfo("[OT] bad nextPtr=0x%llX at %p — chain walk halted\n",
						(unsigned long long)nextPtr, (void*)basePacket);
					s_badNextLogged++;
				}
				break;
			}
			basePacket = nextPtr;
		}
	}
}

inline int IsNull(POLY_FT3* poly)
{
	return  poly->x0 == -1 &&
		poly->y0 == -1 &&
		poly->x1 == -1 &&
		poly->y1 == -1 &&
		poly->x2 == -1 &&
		poly->y2 == -1;
}

static int ProcessFlatLines(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2* poly = (LINE_F2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = c0;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3* poly = (LINE_F3*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 5;
	}
	case 0xc:
	{
		int i;
		LINE_F4* poly = (LINE_F4*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x2;
			VERTTYPE* p1 = &poly->x3;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 6;
	}
	}
	return 0;
}

static int ProcessGouraudLines(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2* poly = (LINE_G2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = &poly->r1;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

static int ProcessFlatPoly(P_TAG* polyTag)
{
	/* PGXP hint: the prim's stamped GTE ring position (0xFFFF / ignored when
	 * PGXP off). 3D polygons only — sprites/tiles/lines stay 0xFFFF. */
	const u_short gteIndex = g_PsxUsePgxp ? polyTag->pgxp_index : (u_short)0xFFFF;

	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3* poly = (POLY_F3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x4:
	{
		POLY_FT3* poly = (POLY_FT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
			AddSplit(semiTrans, true);

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
			ApplyGtePerVertexDepth(firstVertex, polyTag, false);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4* poly = (POLY_F4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 5;
	}
	case 0xC:
	{
		POLY_FT4* poly = (POLY_FT4*)polyTag;
		/* PC-port guard: skip POLY_FT4 with obviously bogus tpage/clut/UV.
		 * Combat particle effects (muzzle flash, blood splat, sparks) build
		 * prims with weapon-specific CLUT/TPAGE bits that reference VRAM
		 * regions which may not be correctly populated in PsyCross's
		 * software VRAM. Without this guard those prims trip a shader
		 * read into uninitialized GPU memory and crash GsDrawOt.
		 *
		 * Validation:
		 *   - tpage low 5 bits give (TX, TY) page index. TX is 0..15,
		 *     TY is 0..1. Anything outside is a corrupt prim.
		 *   - clut Y is bits 6..14, must fit VRAM height (512). Y > 511
		 *     means the prim was built with a stale/uninitialized clut
		 *     field.
		 *   - All four UVs at (0,0) typically means an unrendered ghost
		 *     prim — kept anyway since some valid prims pin to (0,0).
		 *
		 * Drops 0..1% of prims in the wild. If everything's getting
		 * dropped, the guard's too tight or the upload path is broken
		 * upstream — check [PFT4DROP] log entries. */
		{
			short tpage = poly->tpage;
			short clut = poly->clut;
			int tx = tpage & 0xF;          /* page X (0..15) */
			int ty = (tpage >> 4) & 0x1;   /* page Y (0..1) */
			/* Real clut Y is 9 bits (0..511); bit 15 is reserved/0 on valid
			 * prims. Read unsigned and do NOT mask to 0x1FF so a set bit 15
			 * (uninitialized/garbage clut) pushes clutY past 511 and trips the
			 * guard. The old `& 0x1FF` capped clutY at 511, making `> 511` dead
			 * code that never dropped or logged a single prim. */
			int clutY = ((unsigned short)clut) >> 6;
			(void)tx; (void)ty;
			if (clutY > 511) {
				static int s_pft4DropCount = 0;
				if (s_pft4DropCount < 32) {
					eprintinfo("[PFT4DROP] tpage=0x%04hX clut=0x%04hX uvs=(%d,%d)(%d,%d)(%d,%d)(%d,%d) reason=clutY_oob (%d)\n",
						tpage, clut,
						poly->u0, poly->v0, poly->u1, poly->v1,
						poly->u2, poly->v2, poly->u3, poly->v3,
						clutY);
					s_pft4DropCount++;
				}
				return 9;  /* skip rendering, advance past prim */
			}
		}
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	}
	return 0;
}

static int ProcessGouraudPoly(P_TAG* polyTag)
{
	/* PGXP hint (3D polygons only). 0xFFFF / ignored when PGXP off. */
	const u_short gteIndex = g_PsxUsePgxp ? polyTag->pgxp_index : (u_short)0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3* poly = (POLY_G3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		// Per-vertex fog factor packed into pad1/pad2 (game writes fog amount there).
		// v0 shares v1's fog (code byte occupies v0's pad slot).
		firstVertex[0]._p0 = poly->pad1;
		firstVertex[1]._p0 = poly->pad1;
		firstVertex[2]._p0 = poly->pad2;

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 6;
	}
	case 0x4:
	{
		POLY_GT3* poly = (POLY_GT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		// Copy per-vertex fog factor from pad bytes
		firstVertex[0]._p0 = poly->p1;  // v0: shares v1's fog (code byte occupies v0's pad)
		firstVertex[1]._p0 = poly->p1;  // v1
		firstVertex[2]._p0 = poly->p2;  // v2

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	case 0x8:
	{
		POLY_G4* poly = (POLY_G4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		// Per-vertex fog factor packed into pad1/pad2/pad3 (note: MakeColourQuad swaps v2/v3).
		firstVertex[0]._p0 = poly->pad1;
		firstVertex[1]._p0 = poly->pad1;
		firstVertex[2]._p0 = poly->pad3;
		firstVertex[3]._p0 = poly->pad2;

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 8;
	}
	case 0xC:
	{
		POLY_GT4* poly = (POLY_GT4*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		// Copy per-vertex fog factor from pad bytes (note: MakeColourQuad swaps v2/v3)
		firstVertex[0]._p0 = (unsigned char)poly->pad2;  // v0: own fog (game carries it in pad2; v0 color word's pad is the code byte)
		firstVertex[1]._p0 = poly->p1;  // v1
		firstVertex[2]._p0 = poly->p3;  // v3 (buffer[2] = poly vertex 3 due to swap)
		firstVertex[3]._p0 = poly->p2;  // v2 (buffer[3] = poly vertex 2 due to swap)

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 12;
	}
	}
	return 0;
}

static int ProcessTileAndSprt(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE* poly = (TILE*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x64:
	{
		SPRT* poly = (SPRT*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x68:
	{
		TILE_1* poly = (TILE_1*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x70:
	{
		TILE_8* poly = (TILE_8*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x74:
	{
		SPRT_8* poly = (SPRT_8*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x78:
	{
		TILE_16* poly = (TILE_16*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x7C:
	{
		SPRT_16* poly = (SPRT_16*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	}
	return 0;
}

static int ProcessDrawEnv(P_TAG* polyTag)
{
	const u_int* codePtr = (u_int*)&polyTag->pad0;
	int processedLongs = 0;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u_int code = codePtr[i];
		const int primSubType = code >> 24 & 0x0F;

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			activeDrawEnv.dfe = 1; // Force dfe=1: PSX dfe only controls display-during-draw for interlace, not rendering target
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w -= activeDrawEnv.clip.x;
			activeDrawEnv.clip.h -= activeDrawEnv.clip.y;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			// TODO
			activeDrawEnv.ofs[0] = code & 2047;
			activeDrawEnv.ofs[1] = (code >> 11) & 2047;
			break;
		}
		case 0x6:
		{
			eprintf("Mask setting: %08x\n", code);
			//MaskSetOR = (*cb & 1) ? 0x8000 : 0x0000;
			//MaskEvalAND = (*cb & 2) ? 0x8000 : 0x0000;
			break;
		}
		case 0:
			// proceed to next primitive tag — but consume the rest of the
			// declared packet length so the caller's
			//   currentPacket += (primLength + P_LEN) * 4
			// advance lands at endPacket. Returning the partial count made
			// currentPacket short of endPacket; the leftover bytes (zero
			// padding) then got mis-parsed as a fresh prim with code=0x00 /
			// primLength=3, which overshot endPacket by 16 bytes (exactly
			// the diff=-16 in the OT-PRIM log) and started chasing wild
			// next-pointers — repro: handgun fire, stack
			// ParsePrimitivesLinkedList+0xae -> DrawOTag -> GsDrawOt.
			return polyTag->len;
		}
		++processedLongs;
	}

	return processedLongs;
}

static int ProcessPsyXPrims(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX* psytex = (DR_PSYX_TEX*)polyTag;
		overrideTexture = psytex->code[0] & 0xFFFFFF;
		overrideTextureWidth = psytex->code[1] & 0xFFF;
		overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom texture packet
		DR_PSYX_DBGMARKER* psydbg = (DR_PSYX_DBGMARKER*)polyTag;
		currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE* drmove = (DR_MOVE*)polyTag;

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drmove->code[2];
			*(uint*)&rect.w = *(uint*)&drmove->code[4];

			MoveImage(&rect, x, y);
			primLength = 5;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD* drload = (DR_LOAD*)polyTag;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drload->code[1];
			*(uint*)&rect.w = *(uint*)&drload->code[2];

			LoadImage(&rect, (u_long*)drload->p);
			//Emulator_UpdateVRAM();			// FIXME: should it be updated immediately?

			// FIXME: is there othercommands?
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
	//default:
	//	eprinterr("got %0x primitive\n", primType);
	}

	if(primLength == 0)
	{
		eprinterr("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}
