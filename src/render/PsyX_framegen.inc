/* Frame interpolation. Included by PsyX_render.cpp, which owns every static
 * this touches.
 *
 * The game keeps ticking at its own rate; the frames shown between two ticks
 * are synthesised from the two finished pictures. Each pixel's motion comes
 * from the depth buffer and the camera's move between the ticks, so anything
 * that only moves because the camera does lands exactly. A frame can only be
 * placed between two real ones once the later one exists, so the picture runs
 * one tick behind the game.
 *
 * Depth is only metric with PGXP on (view Z over a fixed 262144), which is why
 * the feature stands down without it. The world layer is captured before the
 * 2D ordering table draws: pixels the HUD, the fade and the overlays changed
 * afterwards are taken from the finished frame untouched. */
extern char begin_scene_flag;

#if PSYX_HAS_POSTPROCESS

extern "C" {
	/* 0 off, 1 on, 2 on + reprojection-error view (black = motion is right). */
	int g_PsyX_FrameInterp = 0;
	/* Presented rate to aim for; 0 = the display's refresh rate. */
	int g_PsyX_FrameInterpTargetHz = 0;
	/* Intermediate frames shown for the last tick, for the overlay/log. */
	int g_PsyX_FrameInterpLastCount = 0;
}

#define FG_MAX_ACTORS 8

typedef struct
{
	float rot[9];      /* world -> view, row-major */
	float trans[3];
	float proj[3];     /* OFX, OFY, H */
	float ortho[4];    /* L, R, T, B in prim coordinates */
	float vp[4];       /* x, y, w, h as fractions of the target */
	int   valid;
	/* Characters, by a slot id that is stable from tick to tick: world
	 * position of the root (feet), same units as trans. */
	int   actCount;
	int   actId[FG_MAX_ACTORS];
	float actPos[FG_MAX_ACTORS][3];
} FgRecord;

static GLuint   s_fgWorldFBO[2] = { 0, 0 };
static GLuint   s_fgWorldTex[2] = { 0, 0 };
static GLuint   s_fgDepthTex[2] = { 0, 0 };
static GLuint   s_fgCompFBO = 0, s_fgCompTex = 0;
static int      s_fgW = 0, s_fgH = 0;
static FgRecord s_fgRec[2];
static int      s_fgCur = 0;
static int      s_fgCaptured = 0;
static int      s_fgPrevValid = 0;
static float    s_fgTickHz = 0.0f;
static int      s_fgIntermediateSwap = 0;
static int      s_fgAppliedWant = 0;
static int      s_fgQueueTotal = 0;
static int      s_fgQueueNext  = 0;
extern FILE*    g_logStream;
static ShaderID s_fgShader = (ShaderID)-1;
static GLint    s_fgLoc_t, s_fgLoc_relR, s_fgLoc_relT, s_fgLoc_debug;
static GLint    s_fgLoc_actCount, s_fgLoc_actRoot, s_fgLoc_actOff, s_fgLoc_actAxis;
static int      s_fgPendActCount = 0;
static int      s_fgPendActId[FG_MAX_ACTORS];
static float    s_fgPendActPos[FG_MAX_ACTORS][3];
static int      s_fgActCount = 0;
static float    s_fgActRoot[FG_MAX_ACTORS * 3], s_fgActOff[FG_MAX_ACTORS * 3], s_fgActAxis[3];
static GLint    s_fgLoc_orthoA, s_fgLoc_orthoB, s_fgLoc_vpA, s_fgLoc_vpB, s_fgLoc_projA, s_fgLoc_projB;
static float    s_fgRelR[9];   /* column-major, B view -> A view */
static float    s_fgRelT[3];
static Uint64   s_fgT0 = 0;

static const char* s_fgShaderSrc =
	"varying vec2 v_uv;\n"
	"#ifdef VERTEX\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	v_uv = (p + 1.0) * 0.5;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n"
	"#else\n"
	"uniform sampler2D s_colA;\n"
	"uniform sampler2D s_colB;\n"
	"uniform sampler2D s_depA;\n"
	"uniform sampler2D s_depB;\n"
	"uniform sampler2D s_comp;\n"
	"uniform float u_t;\n"
	"uniform mat3  u_relR;\n"
	"uniform vec3  u_relT;\n"
	"uniform vec4  u_orthoA;\n"
	"uniform vec4  u_orthoB;\n"
	"uniform vec4  u_vpA;\n"
	"uniform vec4  u_vpB;\n"
	"uniform vec3  u_projA;\n"
	"uniform vec3  u_projB;\n"
	"uniform int   u_debug;\n"
	"uniform int   u_actCount;\n"
	"uniform vec3  u_actRoot[8];\n"
	"uniform vec3  u_actOff[8];\n"
	"uniform vec3  u_actAxis;\n"
	"const float SZMAX = 262144.0;\n"
	/* Opaque world depth is written 64 units behind the surface. */
	"const float FARBIAS = 64.0;\n"
	"vec3 viewFromB(vec2 uv, float z) {\n"
	"	vec2 f = (uv - u_vpB.xy) / u_vpB.zw;\n"
	"	float sx = mix(u_orthoB.x, u_orthoB.y, f.x);\n"
	"	float sy = mix(u_orthoB.z, u_orthoB.w, 1.0 - f.y);\n"
	"	return vec3((sx - u_projB.x) * z / u_projB.z, (sy - u_projB.y) * z / u_projB.z, z);\n"
	"}\n"
	"vec2 uvInA(vec3 p) {\n"
	"	float sx = u_projA.x + p.x * u_projA.z / p.z;\n"
	"	float sy = u_projA.y + p.y * u_projA.z / p.z;\n"
	"	vec2 f = vec2((sx - u_orthoA.x) / (u_orthoA.y - u_orthoA.x),\n"
	"	              1.0 - (sy - u_orthoA.z) / (u_orthoA.w - u_orthoA.z));\n"
	"	return u_vpA.xy + f * u_vpA.zw;\n"
	"}\n"
	/* xy: how far the surface seen at uv in B moved since A. z: its view Z in A.
	 * A pixel inside a character's bounding cylinder moves with that character
	 * on top of the camera: there is no per-pixel id to ask, but characters are
	 * drawn at their true depth, so the position test finds them. Limbs still
	 * animate at the game's rate; the body as a whole no longer rides the
	 * world. Units are 256 to the metre. */
	"vec3 motionAt(vec2 uv) {\n"
	"	float zr = texture2D(s_depB, uv).r * SZMAX;\n"
	"	vec3 pa = u_relR * viewFromB(uv, max(zr - FARBIAS, 1.0)) + u_relT;\n"
	"	if (u_actCount > 0) {\n"
	"		vec3 pr = viewFromB(uv, max(zr, 1.0));\n"
	"		for (int i = 0; i < 8; i++) {\n"
	"			if (i >= u_actCount) break;\n"
	"			vec3 r = pr - u_actRoot[i];\n"
	"			float h = dot(r, u_actAxis);\n"
	"			if (h > 10.0 && h < 540.0 && length(r - h * u_actAxis) < 190.0) {\n"
	"				pa = u_relR * pr + u_relT + u_actOff[i];\n"
	"				break;\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	if (pa.z < 1.0) return vec3(0.0, 0.0, -1.0);\n"
	"	return vec3(uv - uvInA(pa), pa.z);\n"
	"}\n"
	"bool inside(vec2 uv, vec4 vp) {\n"
	"	return uv.x >= vp.x && uv.y >= vp.y && uv.x <= vp.x + vp.z && uv.y <= vp.y + vp.w;\n"
	"}\n"
	"void main() {\n"
	"	vec3 comp = texture2D(s_comp, v_uv).rgb;\n"
	"	vec3 wb   = texture2D(s_colB, v_uv).rgb;\n"
	"	vec3 dl   = abs(comp - wb);\n"
	"	if (u_debug == 0 && max(dl.r, max(dl.g, dl.b)) > 0.006) {\n"
	"		fragColor = vec4(comp, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	if (u_debug != 0) {\n"
	"		vec3 m0 = motionAt(v_uv);\n"
	"		vec3 pa = texture2D(s_colA, v_uv - m0.xy).rgb;\n"
	"		fragColor = vec4(abs(wb - pa) * 4.0, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	vec3 m = motionAt(v_uv);\n"
	"	m = motionAt(clamp(v_uv + (1.0 - u_t) * m.xy, u_vpB.xy, u_vpB.xy + u_vpB.zw));\n"
	"	vec2 ua = v_uv - u_t * m.xy;\n"
	"	vec2 ub = v_uv + (1.0 - u_t) * m.xy;\n"
	"	bool okA = m.z > 0.0 && inside(ua, u_vpA);\n"
	"	bool okB = inside(ub, u_vpB);\n"
	"	vec3 ca = texture2D(s_colA, ua).rgb;\n"
	"	vec3 cb = texture2D(s_colB, ub).rgb;\n"
	/* Same surface in both frames only if A's depth there is where the
	 * reprojection says it should be; otherwise something moved on its own or
	 * was uncovered, and blending would ghost it. */
	"	float za = texture2D(s_depA, ua).r * SZMAX - FARBIAS;\n"
	"	bool same = okA && abs(za - m.z) < 0.08 * m.z + 48.0;\n"
	"	vec3 col;\n"
	"	if (okA && okB && same)  col = mix(ca, cb, u_t);\n"
	"	else if (okB && (u_t >= 0.5 || !okA)) col = cb;\n"
	"	else if (okA) col = ca;\n"
	"	else col = wb;\n"
	"	fragColor = vec4(col, 1.0);\n"
	"}\n"
	"#endif\n";

static void GR_FgDestroy(void)
{
	if (s_fgWorldFBO[0])
	{
		glDeleteFramebuffers(2, s_fgWorldFBO);
		glDeleteTextures(2, s_fgWorldTex);
		glDeleteTextures(2, s_fgDepthTex);
		glDeleteFramebuffers(1, &s_fgCompFBO);
		glDeleteTextures(1, &s_fgCompTex);
	}
	s_fgWorldFBO[0] = s_fgWorldFBO[1] = 0;
	s_fgCompFBO = 0;
	s_fgW = s_fgH = 0;
	s_fgPrevValid = 0;
	s_fgCaptured = 0;
}

static void GR_FgColorTex(GLuint tex, int w, int h)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static int GR_FgEnsure(void)
{
	int i, ok = 1;

	if (g_internalFBO == 0 || s_internalW <= 0 || s_internalH <= 0)
		return 0;
	if (s_fgWorldFBO[0] != 0 && s_fgW == s_internalW && s_fgH == s_internalH)
		return 1;

	GR_FgDestroy();

	if (s_fgShader == (ShaderID)-1)
	{
		GR_InitPostProcess();
		s_fgShader = GR_Shader_Compile(s_fgShaderSrc);
		glUseProgram(s_fgShader);
		glUniform1i(glGetUniformLocation(s_fgShader, "s_colA"), 3);
		glUniform1i(glGetUniformLocation(s_fgShader, "s_colB"), 4);
		glUniform1i(glGetUniformLocation(s_fgShader, "s_depA"), 5);
		glUniform1i(glGetUniformLocation(s_fgShader, "s_depB"), 6);
		glUniform1i(glGetUniformLocation(s_fgShader, "s_comp"), 7);
		s_fgLoc_t      = glGetUniformLocation(s_fgShader, "u_t");
		s_fgLoc_relR   = glGetUniformLocation(s_fgShader, "u_relR");
		s_fgLoc_relT   = glGetUniformLocation(s_fgShader, "u_relT");
		s_fgLoc_debug  = glGetUniformLocation(s_fgShader, "u_debug");
		s_fgLoc_orthoA = glGetUniformLocation(s_fgShader, "u_orthoA");
		s_fgLoc_orthoB = glGetUniformLocation(s_fgShader, "u_orthoB");
		s_fgLoc_vpA    = glGetUniformLocation(s_fgShader, "u_vpA");
		s_fgLoc_vpB    = glGetUniformLocation(s_fgShader, "u_vpB");
		s_fgLoc_projA  = glGetUniformLocation(s_fgShader, "u_projA");
		s_fgLoc_projB  = glGetUniformLocation(s_fgShader, "u_projB");
		s_fgLoc_actCount = glGetUniformLocation(s_fgShader, "u_actCount");
		s_fgLoc_actRoot  = glGetUniformLocation(s_fgShader, "u_actRoot");
		s_fgLoc_actOff   = glGetUniformLocation(s_fgShader, "u_actOff");
		s_fgLoc_actAxis  = glGetUniformLocation(s_fgShader, "u_actAxis");
		g_PreviousShader = (ShaderID)-1;
	}

	s_fgW = s_internalW;
	s_fgH = s_internalH;

	glGenFramebuffers(2, s_fgWorldFBO);
	glGenTextures(2, s_fgWorldTex);
	glGenTextures(2, s_fgDepthTex);
	glGenFramebuffers(1, &s_fgCompFBO);
	glGenTextures(1, &s_fgCompTex);

	for (i = 0; i < 2; i++)
	{
		GR_FgColorTex(s_fgWorldTex[i], s_fgW, s_fgH);

		/* Same format as the scene's depth so a depth blit is legal on ES. */
		glBindTexture(GL_TEXTURE_2D, s_fgDepthTex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, s_fgW, s_fgH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glBindFramebuffer(GL_FRAMEBUFFER, s_fgWorldFBO[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fgWorldTex[i], 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_fgDepthTex[i], 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			ok = 0;
	}

	GR_FgColorTex(s_fgCompTex, s_fgW, s_fgH);
	glBindFramebuffer(GL_FRAMEBUFFER, s_fgCompFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fgCompTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		ok = 0;

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
	g_lastBoundTexture = (TextureID)-1;

	if (g_logStream)
	{
		fprintf(g_logStream, "[FG] targets %dx%d %s\n", s_fgW, s_fgH, ok ? "ready" : "INCOMPLETE - interpolation off");
		fflush(g_logStream);
	}
	if (!ok)
	{
		GR_FgDestroy();
		g_PsyX_FrameInterp = 0;
	}
	return ok;
}

/* The scene target is needed even where the scene could draw straight to the
 * window (ANGLE): the passes read it back. */
static int GR_FgWantsTarget(void)
{
	s_fgAppliedWant = (g_PsyX_FrameInterp != 0);
	return s_fgAppliedWant;
}

static int GR_FgTargetChanged(void)
{
	return s_fgAppliedWant != (g_PsyX_FrameInterp != 0);
}

static void GR_FgBlitFromScene(GLuint dstFBO, GLbitfield mask, GLuint srcFBO)
{
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

	if (scissor)
		glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
	glBlitFramebuffer(0, 0, s_fgW, s_fgH, 0, 0, s_fgW, s_fgH, mask, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
	if (scissor)
		glEnable(GL_SCISSOR_TEST);
}

/* The characters on screen this tick, given before the world capture.
 * xyz: root (feet) world positions, three ints each, in the units of the
 * camera translation. ids: any value stable for one character across ticks. */
extern "C" void PsyX_FrameInterp_SetActors(const int* xyz, const int* ids, int count)
{
	int i;

	if (count > FG_MAX_ACTORS)
		count = FG_MAX_ACTORS;
	if (count < 0 || !xyz || !ids)
		count = 0;
	for (i = 0; i < count; i++)
	{
		s_fgPendActId[i]     = ids[i];
		s_fgPendActPos[i][0] = (float)xyz[i * 3 + 0];
		s_fgPendActPos[i][1] = (float)xyz[i * 3 + 1];
		s_fgPendActPos[i][2] = (float)xyz[i * 3 + 2];
	}
	s_fgPendActCount = count;
}

/* Called by the game once the 3D ordering table has been drawn and before the
 * 2D one: the target holds the world alone, with its depth.
 * rot/trans: the world->view matrix those prims were projected with (Q12
 * rotation, translation in the same units as view Z). worldValid: this frame
 * is a normal 3D view the camera matrix describes. tickHz: how often the game
 * is producing frames right now (0 = unknown/uncapped). */
extern "C" void PsyX_FrameInterp_CaptureWorld(const short* rot, const int* trans, int worldValid, int tickHz)
{
	FgRecord*   r;
	float       ox = 0.0f, oy = 0.0f;
	int         i;

	s_fgTickHz = (float)tickHz;

	if (!g_PsyX_FrameInterp || !begin_scene_flag || g_PcNativeRender || !rot || !trans)
		return;
	if (!worldValid || !g_PsxUsePgxp || !g_cfg_pgxpZBuffer || !g_PsxWorldOrthoValid || s_overlayToWindow)
		return;
	if (g_presentVp[2] <= 0 || g_presentVp[3] <= 0 || C2_H <= 0)
		return;
	if (!GR_FgEnsure())
		return;

	/* Any frame of the previous pair not shown yet is too late: its source
	 * layer is about to be overwritten. */
	s_fgQueueNext = 0;
	s_fgCur ^= 1;
	r = &s_fgRec[s_fgCur];

	for (i = 0; i < 9; i++)
		r->rot[i] = (float)rot[i] / 4096.0f;
	for (i = 0; i < 3; i++)
		r->trans[i] = (float)trans[i];

	r->proj[0] = (float)C2_OFX / 65536.0f;
	r->proj[1] = (float)C2_OFY / 65536.0f;
	r->proj[2] = (float)C2_H;

	PsyX_GetDrawEnvOffset(&ox, &oy);
	r->ortho[0] = g_PsxWorldOrtho[0] - ox;
	r->ortho[1] = g_PsxWorldOrtho[1] - ox;
	r->ortho[2] = g_PsxWorldOrtho[2] - oy;
	r->ortho[3] = g_PsxWorldOrtho[3] - oy;

	r->vp[0] = (float)g_presentVp[0] / (float)s_fgW;
	r->vp[1] = (float)g_presentVp[1] / (float)s_fgH;
	r->vp[2] = (float)g_presentVp[2] / (float)s_fgW;
	r->vp[3] = (float)g_presentVp[3] / (float)s_fgH;
	r->valid = 1;
	r->actCount = s_fgPendActCount;
	memcpy(r->actId, s_fgPendActId, sizeof(r->actId));
	memcpy(r->actPos, s_fgPendActPos, sizeof(r->actPos));
	s_fgPendActCount = 0;

	/* Straight from the scene target, not the colour-only resolve mirror: a
	 * blit from a multisample source resolves depth as well. */
	GR_FgBlitFromScene(s_fgWorldFBO[s_fgCur], GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, g_internalFBO);
	s_fgCaptured = 1;
}

/* One-shot self-check (console: frameinterp check). For a grid of points it
 * logs the motion the camera + depth maths predicts next to the motion
 * actually found by matching a patch of this frame's world layer in the
 * previous one. Desktop GL only: ES cannot read depth back. */
extern "C" { int g_PsyX_FrameInterpCheck = 0; }

static void GR_FgSelfCheck(const FgRecord* a, const FgRecord* b, const float* rel, const float* relT)
{
	enum { PB = 33, PA = 129, RNG = 48 };
	static unsigned char pb[PB * PB * 4], pa[PA * PA * 4];
	int gx, gy;

	if (g_grIsGLES || !g_logStream)
		return;

	fprintf(g_logStream, "[FGCHK] relR %.5f %.5f %.5f | %.5f %.5f %.5f | %.5f %.5f %.5f  relT %.2f %.2f %.2f" "\n",
			rel[0], rel[1], rel[2], rel[3], rel[4], rel[5], rel[6], rel[7], rel[8], relT[0], relT[1], relT[2]);
	fprintf(g_logStream, "[FGCHK] B: T %.1f %.1f %.1f proj %.2f %.2f %.0f ortho %.2f %.2f %.2f %.2f vp %.4f %.4f %.4f %.4f size %dx%d" "\n",
			b->trans[0], b->trans[1], b->trans[2], b->proj[0], b->proj[1], b->proj[2],
			b->ortho[0], b->ortho[1], b->ortho[2], b->ortho[3], b->vp[0], b->vp[1], b->vp[2], b->vp[3], s_fgW, s_fgH);

	for (gy = 1; gy <= 3; gy++)
		for (gx = 1; gx <= 5; gx++)
		{
			const int   px = s_fgW * gx / 6, py = s_fgH * gy / 4;
			const float u = ((float)px + 0.5f) / (float)s_fgW, v = ((float)py + 0.5f) / (float)s_fgH;
			float	   d = 1.0f, fx, fy, sx, sy, z, P[3], Q[3], sxa, sya, ua, va;
			int		 x, y, sxs, sys, bestX = 0, bestY = 0;
			long		best = -1, flat = 0;

			if (px - PA / 2 < 0 || py - PA / 2 < 0 || px + PA / 2 >= s_fgW || py + PA / 2 >= s_fgH)
				continue;

			glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fgWorldFBO[s_fgCur]);
			glReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
			glReadPixels(px - PB / 2, py - PB / 2, PB, PB, GL_RGBA, GL_UNSIGNED_BYTE, pb);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fgWorldFBO[s_fgCur ^ 1]);
			glReadPixels(px - PA / 2, py - PA / 2, PA, PA, GL_RGBA, GL_UNSIGNED_BYTE, pa);

			fx = (u - b->vp[0]) / b->vp[2];
			fy = (v - b->vp[1]) / b->vp[3];
			sx = b->ortho[0] + (b->ortho[1] - b->ortho[0]) * fx;
			sy = b->ortho[2] + (b->ortho[3] - b->ortho[2]) * (1.0f - fy);
			z  = d * 262144.0f - 64.0f;
			if (z < 1.0f) z = 1.0f;
			P[0] = (sx - b->proj[0]) * z / b->proj[2];
			P[1] = (sy - b->proj[1]) * z / b->proj[2];
			P[2] = z;
			for (x = 0; x < 3; x++)
				Q[x] = rel[x * 3 + 0] * P[0] + rel[x * 3 + 1] * P[1] + rel[x * 3 + 2] * P[2] + relT[x];
			sxa = a->proj[0] + Q[0] * a->proj[2] / (Q[2] > 1.0f ? Q[2] : 1.0f);
			sya = a->proj[1] + Q[1] * a->proj[2] / (Q[2] > 1.0f ? Q[2] : 1.0f);
			ua  = a->vp[0] + (sxa - a->ortho[0]) / (a->ortho[1] - a->ortho[0]) * a->vp[2];
			va  = a->vp[1] + (1.0f - (sya - a->ortho[2]) / (a->ortho[3] - a->ortho[2])) * a->vp[3];

			/* Where in A does B's patch sit? (bestX, bestY) = A position - B position. */
			for (sys = -RNG; sys <= RNG; sys++)
				for (sxs = -RNG; sxs <= RNG; sxs++)
				{
					long e = 0;
					for (y = 0; y < PB; y += 2)
						for (x = 0; x < PB; x += 2)
						{
							const unsigned char* q = &pb[(y * PB + x) * 4];
							const unsigned char* r = &pa[((y + RNG + sys) * PA + (x + RNG + sxs)) * 4];
							e += labs((long)q[0] - r[0]) + labs((long)q[1] - r[1]) + labs((long)q[2] - r[2]);
						}
					if (best < 0 || e < best) { best = e; bestX = sxs; bestY = sys; }
				}
			for (y = 0; y < PB - 2; y += 2)
				for (x = 0; x < PB - 2; x += 2)
					flat += labs((long)pb[(y * PB + x) * 4 + 1] - pb[(y * PB + x + 2) * 4 + 1]) +
							labs((long)pb[(y * PB + x) * 4 + 1] - pb[((y + 2) * PB + x) * 4 + 1]);

			/* All in pixels, GL orientation (y up), as "position in B minus position in A". */
			fprintf(g_logStream, "[FGCHK] px %4d,%4d depth %.6f z %.0f | predicted %+.1f,%+.1f | measured %+d,%+d (sad %ld, detail %ld)" "\n",
					px, py, d, z, (u - ua) * (float)s_fgW, (v - va) * (float)s_fgH, -bestX, -bestY, best, flat);
		}

	fflush(g_logStream);
	glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
}

static int GR_FgDisplayHz(void)
{
	SDL_DisplayMode mode;

	if (g_PsyX_FrameInterpTargetHz > 0)
		return g_PsyX_FrameInterpTargetHz;
	if (g_window && SDL_GetWindowDisplayMode(g_window, &mode) == 0 && mode.refresh_rate > 0)
		return mode.refresh_rate;
	return 60;
}

/* After the whole frame is composed. Returns how many synthesised frames to
 * show before the real one (0 = present normally). */
static int GR_FgPrepare(void)
{
	const FgRecord* a;
	const FgRecord* b;
	const int       curValid = s_fgCaptured;
	const int       hadPrev  = s_fgPrevValid;
	float           rel[9], d[3], tr, dist2;
	int             i, j, k, count;

	s_fgCaptured  = 0;
	s_fgPrevValid = curValid;
	g_PsyX_FrameInterpLastCount = 0;

	if (!curValid || !hadPrev || g_internalFBO == 0 || s_fgW != s_internalW || s_fgH != s_internalH)
		return 0;
	if (s_fgTickHz < 10.0f)
		return 0;

	/* Under vsync every present costs a whole refresh, so the frames shown per
	 * tick must fit inside it: rounding 144/30 up to five stretched each tick
	 * to 34.7 ms and slowed the game. Without vsync the waits are ours. */
	{
		extern int g_cfg_swapInterval;
		const float ratio = (float)GR_FgDisplayHz() / s_fgTickHz;

		count = (int)(g_cfg_swapInterval ? ratio + 0.02f : ratio + 0.5f) - 1;
	}
	if (count > 7)
		count = 7;
	if (count < 1)
		return 0;

	b = &s_fgRec[s_fgCur];
	a = &s_fgRec[s_fgCur ^ 1];

	/* Pa = Ra * Rb^-1 * (Pb - Tb) + Ta.
	 *
	 * The true inverse, in doubles, not the transpose: the game's rotation is
	 * Q12 and its rows come out a hair short of unit length, so Ra * Rb^T is
	 * 0.9995 * identity for a camera that has not moved. That 0.05% multiplies
	 * a translation tens of thousands of units long and turned a still camera
	 * into a 10-unit move -- every synthesised frame drifted and snapped back. */
	{
		double B[9], inv[9], relD[9], det;

		for (i = 0; i < 9; i++)
			B[i] = (double)b->rot[i];
		inv[0] = B[4] * B[8] - B[5] * B[7];
		inv[1] = B[2] * B[7] - B[1] * B[8];
		inv[2] = B[1] * B[5] - B[2] * B[4];
		inv[3] = B[5] * B[6] - B[3] * B[8];
		inv[4] = B[0] * B[8] - B[2] * B[6];
		inv[5] = B[2] * B[3] - B[0] * B[5];
		inv[6] = B[3] * B[7] - B[4] * B[6];
		inv[7] = B[1] * B[6] - B[0] * B[7];
		inv[8] = B[0] * B[4] - B[1] * B[3];
		det = B[0] * inv[0] + B[1] * inv[3] + B[2] * inv[6];
		if (det > -1e-6 && det < 1e-6)
			return 0;
		for (i = 0; i < 9; i++)
			inv[i] /= det;

		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++)
			{
				double acc = 0.0;
				for (k = 0; k < 3; k++)
					acc += (double)a->rot[i * 3 + k] * inv[k * 3 + j];
				relD[i * 3 + j] = acc;
				rel[i * 3 + j]  = (float)acc;
			}
		for (i = 0; i < 3; i++)
		{
			d[i] = (float)((double)a->trans[i] - (relD[i * 3 + 0] * (double)b->trans[0] +
												  relD[i * 3 + 1] * (double)b->trans[1] +
												  relD[i * 3 + 2] * (double)b->trans[2]));
			s_fgRelT[i] = d[i];
			for (j = 0; j < 3; j++)
				s_fgRelR[j * 3 + i] = rel[i * 3 + j];
		}
	}

	/* Each character present in both ticks: where its root sits in B's view
	 * space, and the extra move in A's view space that undoes its own travel
	 * (-Ra * v). One that jumped more than a metre was repositioned, not moved. */
	s_fgActCount = 0;
	for (i = 0; i < b->actCount; i++)
	{
		for (j = 0; j < a->actCount; j++)
			if (a->actId[j] == b->actId[i])
				break;
		if (j < a->actCount)
		{
			const float vx = b->actPos[i][0] - a->actPos[j][0];
			const float vy = b->actPos[i][1] - a->actPos[j][1];
			const float vz = b->actPos[i][2] - a->actPos[j][2];
			float*      root = &s_fgActRoot[s_fgActCount * 3];
			float*      off  = &s_fgActOff[s_fgActCount * 3];

			if (vx * vx + vy * vy + vz * vz > 256.0f * 256.0f)
				continue;
			for (k = 0; k < 3; k++)
			{
				root[k] = b->rot[k * 3 + 0] * b->actPos[i][0] + b->rot[k * 3 + 1] * b->actPos[i][1] +
				          b->rot[k * 3 + 2] * b->actPos[i][2] + b->trans[k];
				off[k]  = -(a->rot[k * 3 + 0] * vx + a->rot[k * 3 + 1] * vy + a->rot[k * 3 + 2] * vz);
			}
			s_fgActCount++;
		}
	}
	/* World up is -Y. */
	for (k = 0; k < 3; k++)
		s_fgActAxis[k] = -b->rot[k * 3 + 1];

	/* A cut, a teleport or a fixed-camera switch: the two frames are different
	 * shots and there is nothing between them. 384 units is 1.5 m in one tick;
	 * the trace bound is a 30 degree turn. */
	tr    = rel[0] + rel[4] + rel[8];
	dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if (dist2 > 384.0f * 384.0f || tr < 2.732f)
		return 0;

	/* Only ticks where the camera moved say anything; the console that arms
	 * this pauses the game. */
	if (g_PsyX_FrameInterpCheck > 0 && (dist2 > 0.25f || tr < 2.99999f))
	{
		g_PsyX_FrameInterpCheck--;
		GR_FgSelfCheck(a, b, rel, d);
	}

	GR_FgBlitFromScene(s_fgCompFBO, GL_COLOR_BUFFER_BIT, GR_ScreenReadFBO());

	s_fgT0 = SDL_GetPerformanceCounter();
	g_PsyX_FrameInterpLastCount = count;
	{
		/* On a change of rate only: one line per transition, not per frame. */
		static int s_logged = -1;

		if (count != s_logged && g_logStream)
		{
			s_logged = count;
			fprintf(g_logStream, "[FG] +%d frame(s) per tick: tick %.0f Hz, display %d Hz, H=%.0f ortho=%.1f..%.1f x %.1f..%.1f\n",
			        count, s_fgTickHz, GR_FgDisplayHz(), b->proj[2], b->ortho[0], b->ortho[1], b->ortho[2], b->ortho[3]);
			fflush(g_logStream);
		}
	}
	return count;
}

static void GR_FgDrawIntermediate(int index, int total)
{
	const FgRecord* b = &s_fgRec[s_fgCur];
	const FgRecord* a = &s_fgRec[s_fgCur ^ 1];
	const int       ia = s_fgCur ^ 1, ib = s_fgCur;

	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
	GR_Viewport(0, 0, g_windowWidth, g_windowHeight);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(s_fgShader);
	glUniform1f(s_fgLoc_t, (float)index / (float)total);
	glUniformMatrix3fv(s_fgLoc_relR, 1, GL_FALSE, s_fgRelR);
	glUniform3fv(s_fgLoc_relT, 1, s_fgRelT);
	glUniform1i(s_fgLoc_debug, g_PsyX_FrameInterp == 2 ? 1 : 0);
	glUniform4fv(s_fgLoc_orthoA, 1, a->ortho);
	glUniform4fv(s_fgLoc_orthoB, 1, b->ortho);
	glUniform4fv(s_fgLoc_vpA, 1, a->vp);
	glUniform4fv(s_fgLoc_vpB, 1, b->vp);
	glUniform3fv(s_fgLoc_projA, 1, a->proj);
	glUniform3fv(s_fgLoc_projB, 1, b->proj);
	glUniform1i(s_fgLoc_actCount, s_fgActCount);
	if (s_fgActCount > 0)
	{
		glUniform3fv(s_fgLoc_actRoot, s_fgActCount, s_fgActRoot);
		glUniform3fv(s_fgLoc_actOff, s_fgActCount, s_fgActOff);
		glUniform3fv(s_fgLoc_actAxis, 1, s_fgActAxis);
	}

	/* Units 3-7: 0-2 belong to the PSX shaders (VRAM, LUT, shadow map) and
	 * their bindings are cached. */
	glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, s_fgWorldTex[ia]);
	glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_fgWorldTex[ib]);
	glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, s_fgDepthTex[ia]);
	glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, s_fgDepthTex[ib]);
	glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, s_fgCompTex);
	glActiveTexture(GL_TEXTURE0);

	glBindVertexArray(g_postVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glEnable(GL_STENCIL_TEST);

	g_PreviousShader       = (ShaderID)-1;
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousScissorState = -999;
}

static void GR_FgRestoreComposite(void)
{
	/* A draw, not a blit: single-sample -> multisample blits are illegal. */
	if (s_internalSamples > 0)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
		GR_Viewport(0, 0, g_windowWidth, g_windowHeight);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_STENCIL_TEST);
		glUseProgram(s_fgShader);
		/* u_t = 1 with A == B and no motion reduces the shader to "the
		 * finished frame": every pixel either differs from the world layer
		 * (taken from the composite) or equals it. */
		glUniform1f(s_fgLoc_t, 1.0f);
		glUniform1i(s_fgLoc_debug, 0);
		{
			static const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
			static const float zero[3]  = { 0, 0, 0 };
			glUniformMatrix3fv(s_fgLoc_relR, 1, GL_FALSE, ident);
			glUniform3fv(s_fgLoc_relT, 1, zero);
		}
		glUniform4fv(s_fgLoc_orthoA, 1, s_fgRec[s_fgCur].ortho);
		glUniform4fv(s_fgLoc_orthoB, 1, s_fgRec[s_fgCur].ortho);
		glUniform4fv(s_fgLoc_vpA, 1, s_fgRec[s_fgCur].vp);
		glUniform4fv(s_fgLoc_vpB, 1, s_fgRec[s_fgCur].vp);
		glUniform3fv(s_fgLoc_projA, 1, s_fgRec[s_fgCur].proj);
		glUniform3fv(s_fgLoc_projB, 1, s_fgRec[s_fgCur].proj);
		glUniform1i(s_fgLoc_actCount, 0);
		glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, s_fgCompTex);
		glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_fgCompTex);
		glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, s_fgDepthTex[s_fgCur]);
		glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, s_fgDepthTex[s_fgCur]);
		glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, s_fgCompTex);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(g_postVAO);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
		glEnable(GL_STENCIL_TEST);
		g_PreviousShader       = (ShaderID)-1;
		g_PreviousBlendMode    = -999;
		g_PreviousDepthMode    = -999;
		g_PreviousScissorState = -999;
		return;
	}

	{
		const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

		if (scissor)
			glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fgCompFBO);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_internalFBO);
		glBlitFramebuffer(0, 0, s_fgW, s_fgH, 0, 0, s_fgW, s_fgH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
		if (scissor)
			glEnable(GL_SCISSOR_TEST);
	}
}

/* The frames of one tick, shown over the tick rather than all at once.
 *
 * Showing them back to back inside PsyX_EndScene held the game there for most
 * of a tick, and with its own update and draw on top the tick overran: 30 fps
 * became 27. Instead the first one goes out at once and the rest are handed
 * out by GR_FrameInterp_Pump from the wait the game already sits in between
 * building a frame and drawing it, so the only time the picture cannot change
 * is while the game is genuinely busy. Item `total` is the real frame. */
static void GR_FgShowItem(int index)
{
	extern void GR_PostProcess(void);

	if (index >= s_fgQueueTotal)
		GR_FgRestoreComposite();
	else
		GR_FgDrawIntermediate(index, s_fgQueueTotal);
	GR_PostProcess();
	/* The per-frame counters close once per tick, on the first swap. */
	s_fgIntermediateSwap = (index != 1);
	GR_SwapWindow();
	s_fgIntermediateSwap = 0;
}

static Uint64 GR_FgDue(int index)
{
	return s_fgT0 + (Uint64)((double)SDL_GetPerformanceFrequency() * (double)(index - 1) /
							 ((double)s_fgTickHz * (double)s_fgQueueTotal));
}

/* Called from the game's frame-pacing wait. Cheap when nothing is due. */
extern "C" void GR_FrameInterp_Pump(void)
{
	int	due;
	Uint64 now;

	if (s_fgQueueNext <= 0 || s_fgQueueNext > s_fgQueueTotal || begin_scene_flag)
		return;

	now = SDL_GetPerformanceCounter();
	if (now < GR_FgDue(s_fgQueueNext))
		return;

	/* Late (the game was busy): skip to the newest one that is due. */
	due = s_fgQueueNext;
	while (due < s_fgQueueTotal && now >= GR_FgDue(due + 1))
		due++;

	GR_FgShowItem(due);
	s_fgQueueNext = due + 1;
}

/* End of a tick. Returns 1 when it presented the frame itself, 0 when the
 * caller should post-process and swap as usual. */
extern "C" int GR_FrameInterp_Present(void)
{
	int count;

	s_fgQueueNext = 0;

	if (!g_PsyX_FrameInterp)
	{
		if (s_fgWorldFBO[0])
			GR_FgDestroy();
		return 0;
	}

	count = GR_FgPrepare();
	if (count <= 0)
		return 0;

	s_fgQueueTotal = count + 1;
	GR_FgShowItem(1);
	s_fgQueueNext = 2;
	return 1;
}

#else

extern "C" {
	int g_PsyX_FrameInterp = 0;
	int g_PsyX_FrameInterpTargetHz = 0;
	int g_PsyX_FrameInterpLastCount = 0;
}
static int s_fgIntermediateSwap = 0;
static int GR_FgWantsTarget(void) { return 0; }
static int GR_FgTargetChanged(void) { return 0; }
extern "C" void PsyX_FrameInterp_CaptureWorld(const short* rot, const int* trans, int worldValid, int tickHz) {}
extern "C" void PsyX_FrameInterp_SetActors(const int* xyz, const int* ids, int count) {}
extern "C" int  GR_FrameInterp_Present(void) { return 0; }
extern "C" void GR_FrameInterp_Pump(void) {}

#endif
