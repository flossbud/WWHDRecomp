// The game's camera at the TV picture's aspect ratio (b-wide, os/aspect.h): hooks (generate.py's hook:NAME,
// config/US_v0/tick_rules/aspect.txt) at the places the camera's aspect is set and used: the six the rival recomp found
// (docs/research/rival-study.md 4.7) and camera_draw's call giving the HD engine its camera (rt_aspect_engine):
//   camera_execute and the camera's creation store the 16/9 constant (1004AAF0) as view.mAspect: the latched aspect
//   instead (rt_aspect_store, on f0);
//   camera_draw's and the file select scene's C_MTXPerspective, and view_setup's two mDoLib_clipper::setup (the culling
//   frustum, so nothing pops in at the new edges), take f1 the vertical field of view in degrees and f2 the aspect:
//   the latched aspect, and for a picture narrower than 16:9 the vertical field of view widened so that the horizontal
//   one stays (Vert+) (rt_aspect_projection).
// Only an aspect the game itself puts there is changed (16:9, or one of ours a frame old): a cutscene's own isn't. At
// 16:9 nothing is touched, so every check's run is the game's own.
#include "override.h"
#include "../os/aspect.h"
#include <cmath>

namespace
{
	namespace aspect = wwhd::os::aspect;

	void SetF(PPCInterpreter_t* ctx, int n, float v)
	{
		ctx->fpr[n].fp0 = v;
		ctx->fpr[n].fp1 = v;
	}
}

// stfs f0 -> view.mAspect: f0 holds the 16/9 constant
void rt_aspect_store(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current();
	if (aspect::Same(a, aspect::kBase) || !aspect::Same((float)ctx->fpr[0].fp0, aspect::kBase))
		return;
	SetF(ctx, 0, a);
}

namespace
{
	// Vert+: the vertical field of view (degrees) widened so that the horizontal one stays
	double Widened(double fovyDeg, float ky)
	{
		return 2.0 * std::atan(std::tan(fovyDeg * (M_PI / 360.0)) * ky) * (180.0 / M_PI);
	}
}

// bl 0274E04C in camera_draw: the HD engine's camera, f3 = f9 (fovy in degrees) x a constant, f4 the aspect
void rt_aspect_engine(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current();
	if (aspect::Same(a, aspect::kBase) || !aspect::Recent((float)ctx->fpr[4].fp0))
		return;
	SetF(ctx, 4, a);
	float kx, ky;
	aspect::Factors(a, kx, ky);
	const double deg = ctx->fpr[9].fp0, f3 = ctx->fpr[3].fp0;
	if (ky != 1.0f && deg > 0.0 && f3 > 0.0)
		SetF(ctx, 3, (float)(Widened(deg, ky) * (f3 / deg)));
}

// bl C_MTXPerspective / mDoLib_clipper::setup (f1 fovy in degrees, f2 aspect)
void rt_aspect_projection(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current(), have = (float)ctx->fpr[2].fp0;
	if (aspect::Same(have, a) && aspect::Same(a, aspect::kBase))
		return;                                                  // 16:9 (every check)
	if (!aspect::Recent(have))
		return;                                                  // an aspect of the game's own (not its 16:9 camera)
	float kx, ky;
	aspect::Factors(a, kx, ky);
	SetF(ctx, 2, a);
	if (ky != 1.0f)                                              // Vert+: the vertical field of view widened
		SetF(ctx, 1, (float)Widened(ctx->fpr[1].fp0, ky));
}
