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

typedef struct
{
	float rot[9];      /* world -> view, row-major */
	float trans[3];
	float proj[3];     /* OFX, OFY, H */
	float ortho[4];    /* L, R, T, B in prim coordinates */
	float vp[4];       /* x, y, w, h as fractions of the target */
	int   valid;
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
extern FILE*    g_logStream;
static ShaderID s_fgShader = (ShaderID)-1;
static GLint    s_fgLoc_t, s_fgLoc_relR, s_fgLoc_relT, s_fgLoc_debug;
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
	"const float SZMAX = 262144.0;\n"
	/* Opaque world depth is written 64 units behind the surface. */
	"const float FARBIAS = 64.0;\n"
	"vec3 viewFromB(vec2 uv, float d) {\n"
	"	vec2 f = (uv - u_vpB.xy) / u_vpB.zw;\n"
	"	float sx = mix(u_orthoB.x, u_orthoB.y, f.x);\n"
	"	float sy = mix(u_orthoB.z, u_orthoB.w, 1.0 - f.y);\n"
	"	float z = max(d * SZMAX - FARBIAS, 1.0);\n"
	"	return vec3((sx - u_projB.x) * z / u_projB.z, (sy - u_projB.y) * z / u_projB.z, z);\n"
	"}\n"
	"vec2 uvInA(vec3 p) {\n"
	"	float sx = u_projA.x + p.x * u_projA.z / p.z;\n"
	"	float sy = u_projA.y + p.y * u_projA.z / p.z;\n"
	"	vec2 f = vec2((sx - u_orthoA.x) / (u_orthoA.y - u_orthoA.x),\n"
	"	              1.0 - (sy - u_orthoA.z) / (u_orthoA.w - u_orthoA.z));\n"
	"	return u_vpA.xy + f * u_vpA.zw;\n"
	"}\n"
	/* xy: how far the surface seen at uv in B moved since A. z: its view Z in A. */
	"vec3 motionAt(vec2 uv) {\n"
	"	vec3 pa = u_relR * viewFromB(uv, texture2D(s_depB, uv).r) + u_relT;\n"
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

	/* Straight from the scene target, not the colour-only resolve mirror: a
	 * blit from a multisample source resolves depth as well. */
	GR_FgBlitFromScene(s_fgWorldFBO[s_fgCur], GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, g_internalFBO);
	s_fgCaptured = 1;
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

	count = (int)((float)GR_FgDisplayHz() / s_fgTickHz + 0.5f) - 1;
	if (count > 7)
		count = 7;
	if (count < 1)
		return 0;

	b = &s_fgRec[s_fgCur];
	a = &s_fgRec[s_fgCur ^ 1];

	/* Pa = Ra * Rb^T * Pb + (Ta - Ra * Rb^T * Tb) */
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
		{
			float s = 0.0f;
			for (k = 0; k < 3; k++)
				s += a->rot[i * 3 + k] * b->rot[j * 3 + k];
			rel[i * 3 + j] = s;
		}
	for (i = 0; i < 3; i++)
	{
		d[i] = a->trans[i] - (rel[i * 3 + 0] * b->trans[0] + rel[i * 3 + 1] * b->trans[1] + rel[i * 3 + 2] * b->trans[2]);
		s_fgRelT[i] = d[i];
		for (j = 0; j < 3; j++)
			s_fgRelR[j * 3 + i] = rel[i * 3 + j];
	}

	/* A cut, a teleport or a fixed-camera switch: the two frames are different
	 * shots and there is nothing between them. 384 units is 1.5 m in one tick;
	 * the trace bound is a 30 degree turn. */
	tr    = rel[0] + rel[4] + rel[8];
	dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if (dist2 > 384.0f * 384.0f || tr < 2.732f)
		return 0;

	GR_FgBlitFromScene(s_fgCompFBO, GL_COLOR_BUFFER_BIT, GR_ScreenReadFBO());

	s_fgT0 = SDL_GetPerformanceCounter();
	g_PsyX_FrameInterpLastCount = count;
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

/* Holds until the next synthesised frame is due. With vsync the swap has
 * already spent the time and this returns at once. */
static void GR_FgWait(int index, int total)
{
	const Uint64 freq   = SDL_GetPerformanceFrequency();
	const Uint64 target = s_fgT0 + (Uint64)((double)freq * (double)index / ((double)s_fgTickHz * (double)total));
	Uint64       now    = SDL_GetPerformanceCounter();

	if (now >= target)
		return;
	{
		const Uint64 remainMs = ((target - now) * 1000) / freq;

		if (remainMs > 2)
			SDL_Delay((Uint32)(remainMs - 1));
	}
	while (SDL_GetPerformanceCounter() < target) {}
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

/* Shows the synthesised frames for this tick, then leaves the finished frame
 * in the scene target for the normal post-process + present that follows. */
extern "C" void GR_FrameInterp_Present(void)
{
	extern void GR_PostProcess(void);
	int count, i;

	if (!g_PsyX_FrameInterp)
	{
		if (s_fgWorldFBO[0])
			GR_FgDestroy();
		return;
	}

	count = GR_FgPrepare();
	if (count <= 0)
		return;

	for (i = 1; i <= count; i++)
	{
		GR_FgDrawIntermediate(i, count + 1);
		GR_PostProcess();
		s_fgIntermediateSwap = 1;
		GR_SwapWindow();
		s_fgIntermediateSwap = 0;
		GR_FgWait(i, count + 1);
	}

	GR_FgRestoreComposite();
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
extern "C" void GR_FrameInterp_Present(void) {}

#endif
