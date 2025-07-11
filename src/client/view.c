/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// cl_view.c -- player rendering positioning

#include "client.h"
#include "shared/debug.h"

//=============
//
// development tools for weapons
//
int         gun_frame;
qhandle_t   gun_model;

//=============

static cvar_t   *cl_add_particles;
static cvar_t   *cl_add_lights;
static cvar_t   *cl_show_lights;
static cvar_t   *cl_flashlight;
static cvar_t   *cl_flashlight_intensity;
static cvar_t   *cl_add_entities;
static cvar_t   *cl_add_blend;
static cvar_t   *cl_flashlight_offset;
static vec3_t   flashlight_offset;

#if USE_DEBUG
static cvar_t   *cl_testparticles;
static cvar_t   *cl_testentities;
static cvar_t   *cl_testlights;
static cvar_t   *cl_testblend;
static cvar_t   *cl_testdebuglines;

static cvar_t   *cl_stats;
#endif

cvar_t   *cl_adjustfov;

int         r_numdlights;
dlight_t    r_dlights[MAX_DLIGHTS];
static qhandle_t flashlight_profile_tex;

int         r_numentities;
entity_t    r_entities[MAX_ENTITIES];

int         r_numparticles;
particle_t  r_particles[MAX_PARTICLES];

lightstyle_t    r_lightstyles[MAX_LIGHTSTYLES];

/*
====================
V_ClearScene

Specifies the model that will be used as the world
====================
*/
static void V_ClearScene(void)
{
    r_numdlights = 0;
    r_numentities = 0;
    r_numparticles = 0;
}


/*
=====================
V_AddEntity

=====================
*/
void V_AddEntity(entity_t *ent)
{
    if (r_numentities >= MAX_ENTITIES)
        return;

    r_entities[r_numentities++] = *ent;
}


/*
=====================
V_AddParticle

=====================
*/
void V_AddParticle(particle_t *p)
{
    if (r_numparticles >= MAX_PARTICLES)
        return;
    r_particles[r_numparticles++] = *p;
}

/*
=====================
V_AddLight

=====================
*/
void V_AddSphereLight(const vec3_t org, float intensity, float r, float g, float b, float radius)
{
    dlight_t    *dl;

    if (r_numdlights >= MAX_DLIGHTS)
        return;
    dl = &r_dlights[r_numdlights++];
    memset(dl, 0, sizeof(dlight_t));
    VectorCopy(org, dl->origin);
    dl->intensity = intensity;
    dl->color[0] = r;
    dl->color[1] = g;
    dl->color[2] = b;
	dl->radius = radius;

	if (cl_show_lights->integer && r_numparticles < MAX_PARTICLES)
	{
		particle_t* part = &r_particles[r_numparticles++];

		VectorCopy(dl->origin, part->origin);
		part->radius = radius;
		part->brightness = max(r, max(g, b));
		part->color = -1;
		part->rgba.u8[0] = (uint8_t)max(0.f, min(255.f, r / part->brightness * 255.f));
		part->rgba.u8[1] = (uint8_t)max(0.f, min(255.f, g / part->brightness * 255.f));
		part->rgba.u8[2] = (uint8_t)max(0.f, min(255.f, b / part->brightness * 255.f));
		part->rgba.u8[3] = 255;
		part->alpha = 1.f;
	}
}

static dlight_t* add_spot_light_common(const vec3_t org, const vec3_t dir, float intensity, float r, float g, float b)
{
    dlight_t    *dl;

    if (r_numdlights >= MAX_DLIGHTS)
        return NULL;
    dl = &r_dlights[r_numdlights++];
    memset(dl, 0, sizeof(dlight_t));
    VectorCopy(org, dl->origin);
    dl->intensity = intensity;
    dl->color[0] = r;
    dl->color[1] = g;
    dl->color[2] = b;
    dl->radius = 1.0f;
    dl->light_type = DLIGHT_SPOT;
    VectorCopy(dir, dl->spot.direction);

    // what would make sense for cl_show_lights here?

    return dl;
}

void V_AddSpotLight(const vec3_t org, const vec3_t dir, float intensity, float r, float g, float b, float width_angle, float falloff_angle)
{
    dlight_t *dl = add_spot_light_common(org, dir, intensity, r, g, b);
    if(!dl)
        return;

    dl->spot.emission_profile = DLIGHT_SPOT_EMISSION_PROFILE_FALLOFF;
    dl->spot.cos_total_width = cosf(DEG2RAD(width_angle));
    dl->spot.cos_falloff_start = cosf(DEG2RAD(falloff_angle));
}

void V_AddSpotLightTexEmission(const vec3_t org, const vec3_t dir, float intensity, float r, float g, float b, float width_angle, qhandle_t emission_tex)
{
    dlight_t *dl = add_spot_light_common(org, dir, intensity, r, g, b);
    if(!dl)
        return;

    dl->spot.emission_profile = DLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE;
    dl->spot.total_width = DEG2RAD(width_angle);
    dl->spot.texture = emission_tex;
}

void V_AddLight(const vec3_t org, float intensity, float r, float g, float b)
{
	V_AddSphereLight(org, intensity, r, g, b, 10.f);
}

void V_Flashlight(const entity_t *ent, const centity_state_t *cent_state)
{
    // Flashlight origin
    vec3_t light_pos;
    // Flashlight direction vectors
    vec3_t view_dir, right_dir, up_dir;

    if (!ent || cent_state->number == cl.frame.clientNum + 1) {
        player_state_t* ps = &cl.frame.ps;
        player_state_t* ops = &cl.oldframe.ps;

        // Flashlight direction (as angles)
        vec3_t flashlight_angles;

        if (cls.demo.playback) {
            /* If a demo is played we don't have predicted_angles,
             * and we can't use cl.refdef.viewangles for the same reason
             * below. However, lerping the angles from the old & current frame
             * work nicely. */
            LerpAngles(cl.oldframe.ps.viewangles, cl.frame.ps.viewangles, cl.lerpfrac, flashlight_angles);
        } else {
            /* Use cl.playerEntityOrigin+viewoffset, playerEntityAngles instead of
             * cl.refdef.vieworg, cl.refdef.viewangles as as the cl.refdef values
             * are the camera values, but not the player "eye" values in 3rd person mode. */
            VectorCopy(cl.predicted_angles, flashlight_angles);
        }
        // Add a bit of gun bob to the flashlight as well
        vec3_t gunangles;
        LerpVector(ops->gunangles, ps->gunangles, cl.lerpfrac, gunangles);
        VectorAdd(flashlight_angles, gunangles, flashlight_angles);

        AngleVectors(flashlight_angles, view_dir, right_dir, up_dir);

        /* Start off with the player eye position. */
        vec3_t viewoffset;
        LerpVector(ops->viewoffset, ps->viewoffset, cl.lerpfrac, viewoffset);
        VectorAdd(cl.playerEntityOrigin, viewoffset, light_pos);

        /* Slightly move position downward, right, and forward to get a position
         * that looks somewhat as if it was attached to the gun.
         * Generally, the spot light origin should be placed away from the player model
         * to avoid interactions with it (mainly unexpected shadowing).
         * When adjusting the offsets care must be taken that
         * the flashlight doesn't also light the view weapon. */
        VectorMA(light_pos, flashlight_offset[2] * cl_gunscale->value, view_dir, light_pos);
        float leftright = flashlight_offset[0] * cl_gunscale->value;
        if(info_hand->integer == 1)
            leftright = -leftright; // left handed
        else if(info_hand->integer == 2)
            leftright = 0.f; // "center" handed
        VectorMA(light_pos, leftright, right_dir, light_pos);
        VectorMA(light_pos, flashlight_offset[1] * cl_gunscale->value, up_dir, light_pos);
    } else {
        AngleVectors(ent->angles, view_dir, right_dir, up_dir);
        VectorCopy(ent->origin, light_pos);
    }

    if(cls.ref_type == REF_TYPE_VKPT) {
        V_AddSpotLightTexEmission(light_pos, view_dir, cl_flashlight_intensity->value, 1.f, 1.f, 1.f, 90.0f, flashlight_profile_tex);
    } else {
        const int cent_num = cent_state ? cent_state->number : cl.frame.clientNum + 1;
        centity_t *cent = &cl_entities[cent_num];
        vec3_t start, end;
        trace_t trace;

        VectorMA(light_pos, 256, view_dir, end);
        VectorCopy(light_pos, start);

        CL_Trace(&trace, start, vec3_origin, vec3_origin, end, CONTENTS_SOLID | CONTENTS_MONSTER);
        LerpVector(start, end, cent->flashlightfrac, end);
        V_AddLight(end, 256, 1, 1, 1);

        // smooth out distance "jumps"
        CL_AdvanceValue(&cent->flashlightfrac, trace.fraction, 1);
    }
}

/*
=====================
V_AddLightStyle

=====================
*/
void V_AddLightStyle(int style, float value)
{
    lightstyle_t    *ls;

    Q_assert(style >= 0 && style < MAX_LIGHTSTYLES);
    ls = &r_lightstyles[style];
    ls->white = value;
}

#if USE_DEBUG

/*
================
V_TestParticles

If cl_testparticles is set, create 4096 particles in the view
================
*/
static void V_TestParticles(void)
{
    particle_t  *p;
    int         i, j;
    float       d, r, u;

    r_numparticles = MAX_PARTICLES;
    for (i = 0; i < r_numparticles; i++) {
        d = i * 0.25f;
        r = 4 * ((i & 7) - 3.5f);
        u = 4 * (((i >> 3) & 7) - 3.5f);
        p = &r_particles[i];

        for (j = 0; j < 3; j++)
            p->origin[j] = cl.refdef.vieworg[j] + cl.v_forward[j] * d +
                           cl.v_right[j] * r + cl.v_up[j] * u;

        p->color = 8;
        p->alpha = 1;
    }
}

/*
================
V_TestEntities

If cl_testentities is set, create 32 player models
================
*/
static void V_TestEntities(void)
{
    int         i, j;
    float       f, r;
    entity_t    *ent;

    r_numentities = 32;
    memset(r_entities, 0, sizeof(r_entities));

    for (i = 0; i < r_numentities; i++) {
        ent = &r_entities[i];

        r = 64 * ((i % 4) - 1.5f);
        f = 64 * (i / 4) + 128;

        for (j = 0; j < 3; j++)
            ent->origin[j] = cl.refdef.vieworg[j] + cl.v_forward[j] * f +
                             cl.v_right[j] * r;

        ent->model = cl.baseclientinfo.model;
        ent->skin = cl.baseclientinfo.skin;
    }
}

/*
================
V_TestLights

If cl_testlights is set, create 32 lights models
================
*/
static void V_TestLights(void)
{
    int         i, j;
    float       f, r;
    dlight_t    *dl;

    if (cl_testlights->integer != 1) {
        dl = &r_dlights[0];
        memset(dl, 0, sizeof(dlight_t));
        r_numdlights = 1;

        VectorMA(cl.refdef.vieworg, 256, cl.v_forward, dl->origin);
        if (cl_testlights->integer == -1)
            VectorSet(dl->color, -1, -1, -1);
        else
            VectorSet(dl->color, 1, 1, 1);
        dl->intensity = 256;
        dl->radius = 16;
        return;
    }

    r_numdlights = 32;
    memset(r_dlights, 0, sizeof(r_dlights));

    for (i = 0; i < r_numdlights; i++) {
        dl = &r_dlights[i];

        r = 64 * ((i % 4) - 1.5f);
        f = 64 * (i / 4) + 128;

        for (j = 0; j < 3; j++)
            dl->origin[j] = cl.refdef.vieworg[j] + cl.v_forward[j] * f +
                            cl.v_right[j] * r;
        dl->color[0] = ((i % 6) + 1) & 1;
        dl->color[1] = (((i % 6) + 1) & 2) >> 1;
        dl->color[2] = (((i % 6) + 1) & 4) >> 2;
        dl->intensity = 200;
        dl->radius = 16;
    }
}

static void V_TestDebugLines(void)
{
    static const uint32_t colors[] = {U32_RED, U32_GREEN, U32_BLUE, U32_YELLOW, U32_MAGENTA, U32_CYAN};
    const float point_size = 16.f;
    const float text_size = 0.5f;

    bool depth_test = cl_testdebuglines->integer == 1 || cl_testdebuglines->integer == 3;
    bool print_id = cl_testdebuglines->integer >= 3;
    for (int i = 0; i < r_numentities; i++) {
        const entity_t *ent = r_entities + i;
        if (ent->id == 1 /* gun */)
            continue;
        uint32_t color = colors[ent->id % q_countof(colors)];
        R_AddDebugPoint(ent->origin, point_size, color, 0, depth_test);

        if (print_id) {
            vec3_t text_org;
            VectorCopy(ent->origin, text_org);
            text_org[2] += point_size;
            R_AddDebugText(text_org, NULL, va("%d", ent->id), text_size, color, 0, depth_test);
        }
    }
}

#endif

//===================================================================

void CL_UpdateBlendSetting(void)
{
    if (cls.netchan.protocol < PROTOCOL_VERSION_R1Q2) {
        return;
    }

    MSG_WriteByte(clc_setting);
    MSG_WriteShort(CLS_NOBLEND);
    MSG_WriteShort(!cl_add_blend->integer);
    MSG_FlushTo(&cls.netchan.message);
}

//============================================================================

// gun frame debugging functions
static void V_Gun_Next_f(void)
{
    gun_frame++;
    Com_Printf("frame %i\n", gun_frame);
}

static void V_Gun_Prev_f(void)
{
    gun_frame--;
    if (gun_frame < 0)
        gun_frame = 0;
    Com_Printf("frame %i\n", gun_frame);
}

static void V_Gun_Model_f(void)
{
    char    name[MAX_QPATH];

    if (Cmd_Argc() != 2) {
        gun_model = 0;
        return;
    }
    Q_concat(name, sizeof(name), "models/", Cmd_Argv(1), "/tris.md2");
    gun_model = R_RegisterModel(name);
}

//============================================================================

static int entitycmpfnc(const void *_a, const void *_b)
{
    const entity_t *a = (const entity_t *)_a;
    const entity_t *b = (const entity_t *)_b;

    // all other models are sorted by model then skin
    if (a->model == b->model)
        return a->skin - b->skin;
    else
        return a->model - b->model;
}

static void V_SetLightLevel(void)
{
    vec3_t shadelight;

    // save off light value for server to look at (BIG HACK!)
    R_LightPoint(cl.refdef.vieworg, shadelight);

    // pick the greatest component, which should be the same
    // as the mono value returned by software
    if (shadelight[0] > shadelight[1]) {
        if (shadelight[0] > shadelight[2]) {
            cl.lightlevel = 150.0f * shadelight[0];
        } else {
            cl.lightlevel = 150.0f * shadelight[2];
        }
    } else {
        if (shadelight[1] > shadelight[2]) {
            cl.lightlevel = 150.0f * shadelight[1];
        } else {
            cl.lightlevel = 150.0f * shadelight[2];
        }
    }
}

/*
====================
V_CalcFov
====================
*/
float V_CalcFov(float fov_x, float width, float height)
{
    float    a;
    float    x;

    if (fov_x <= 0 || fov_x > 179)
        Com_Error(ERR_DROP, "%s: bad fov: %f", __func__, fov_x);

    x = width / tan(fov_x * (M_PI / 360));

    a = atan(height / x);
    a = a * (360 / M_PI);

    return a;
}


/*
==================
V_RenderView

==================
*/
void V_RenderView(void)
{
    // an invalid frame will just use the exact previous refdef
    // we can't use the old frame if the video mode has changed, though...
    if (cl.frame.valid) {
        V_ClearScene();

        // build a refresh entity list and calc cl.sim*
        // this also calls CL_CalcViewValues which loads
        // v_forward, etc.
        CL_AddEntities();

#if USE_DEBUG
        if (cl_testparticles->integer)
            V_TestParticles();
        if (cl_testentities->integer)
            V_TestEntities();
        if (cl_testlights->integer)
            V_TestLights();
        if (cl_testblend->integer) {
            cl.refdef.blend[0] = 1;
            cl.refdef.blend[1] = 0.5f;
            cl.refdef.blend[2] = 0.25f;
            cl.refdef.blend[3] = 0.5f;
        }
        if (cl_testdebuglines->integer)
            V_TestDebugLines();
#endif

        // Render client-side flash light, but skip if "flashlight" effect is already used on client ent
        bool client_flashlight = (cl_entities[cl.frame.clientNum + 1].current.morefx & EFX_FLASHLIGHT) != 0;
        if(cl_flashlight->integer && !client_flashlight)
            V_Flashlight(NULL, NULL);

        // never let it sit exactly on a node line, because a water plane can
        // dissapear when viewed with the eye exactly on it.
        // the server protocol only specifies to 1/8 pixel, so add 1/16 in each axis
        cl.refdef.vieworg[0] += 1.0f / 16;
        cl.refdef.vieworg[1] += 1.0f / 16;
        cl.refdef.vieworg[2] += 1.0f / 16;

        cl.refdef.x = scr_vrect.x;
        cl.refdef.y = scr_vrect.y;
        cl.refdef.width = scr_vrect.width;
        cl.refdef.height = scr_vrect.height;

        // adjust for non-4/3 screens
        if (cl_adjustfov->integer) {
            cl.refdef.fov_y = cl.fov_y;
            cl.refdef.fov_x = V_CalcFov(cl.refdef.fov_y, cl.refdef.height, cl.refdef.width);
        } else {
            cl.refdef.fov_x = cl.fov_x;
            cl.refdef.fov_y = V_CalcFov(cl.refdef.fov_x, cl.refdef.width, cl.refdef.height);
        }

        cl.refdef.time = cl.time * 0.001f;

        if (cl.frame.areabytes) {
            cl.refdef.areabits = cl.frame.areabits;
        } else {
            cl.refdef.areabits = NULL;
        }

        if (!cl_add_entities->integer)
            r_numentities = 0;
        if (!cl_add_particles->integer)
            r_numparticles = 0;
        if (!cl_add_lights->integer)
            r_numdlights = 0;
        if (!cl_add_blend->integer)
            Vector4Clear(cl.refdef.blend);

        cl.refdef.num_entities = r_numentities;
        cl.refdef.entities = r_entities;
        cl.refdef.num_particles = r_numparticles;
        cl.refdef.particles = r_particles;
        cl.refdef.num_dlights = r_numdlights;
        cl.refdef.dlights = r_dlights;
        cl.refdef.lightstyles = r_lightstyles;

        cl.refdef.rdflags = cl.frame.ps.rdflags;

        // sort entities for better cache locality
        qsort(cl.refdef.entities, cl.refdef.num_entities, sizeof(cl.refdef.entities[0]), entitycmpfnc);
    }

    R_RenderFrame(&cl.refdef);
#if USE_DEBUG
    if (cl_stats->integer)
        Com_Printf("ent:%i  lt:%i  part:%i\n", r_numentities, r_numdlights, r_numparticles);
#endif

    V_SetLightLevel();
}


/*
=============
V_Viewpos_f
=============
*/
static void V_Viewpos_f(void)
{
    Com_Printf("%s : %.f\n", vtos(cl.refdef.vieworg), cl.refdef.viewangles[YAW]);
}

static const cmdreg_t v_cmds[] = {
    { "gun_next", V_Gun_Next_f },
    { "gun_prev", V_Gun_Prev_f },
    { "gun_model", V_Gun_Model_f },
    { "viewpos", V_Viewpos_f },
    { NULL }
};

static void cl_add_blend_changed(cvar_t *self)
{
    CL_UpdateBlendSetting();
}

static void cl_flashlight_offset_changed(cvar_t *self)
{
    sscanf(cl_flashlight_offset->string, "%f %f %f", &flashlight_offset[0], &flashlight_offset[1], &flashlight_offset[2]);
}

/*
=============
V_Init
=============
*/
void V_Init(void)
{
    Cmd_Register(v_cmds);

#if USE_DEBUG
    cl_testblend = Cvar_Get("cl_testblend", "0", 0);
    cl_testparticles = Cvar_Get("cl_testparticles", "0", 0);
    cl_testentities = Cvar_Get("cl_testentities", "0", 0);
    cl_testlights = Cvar_Get("cl_testlights", "0", CVAR_CHEAT);
    cl_testdebuglines = Cvar_Get("cl_testdebuglines", "0", CVAR_CHEAT);

    cl_stats = Cvar_Get("cl_stats", "0", 0);
#endif

    cl_add_lights = Cvar_Get("cl_lights", "1", 0);
	cl_show_lights = Cvar_Get("cl_show_lights", "0", 0);
    cl_flashlight = Cvar_Get("cl_flashlight", "0", 0);
    cl_flashlight_intensity = Cvar_Get("cl_flashlight_intensity", "20000", CVAR_ARCHIVE);
    if(cls.ref_type == REF_TYPE_VKPT)
        flashlight_profile_tex = R_RegisterImage("flashlight_profile", IT_PIC, IF_PERMANENT | IF_BILERP);
    else
        flashlight_profile_tex = -1;
    cl_add_particles = Cvar_Get("cl_particles", "1", 0);
    cl_add_entities = Cvar_Get("cl_entities", "1", 0);
    cl_add_blend = Cvar_Get("cl_blend", "1", 0);
    cl_add_blend->changed = cl_add_blend_changed;

    cl_flashlight_offset = Cvar_Get("cl_flashlight_offset", "10 -10 32", 0);
    cl_flashlight_offset->changed = cl_flashlight_offset_changed;
    cl_flashlight_offset_changed(cl_flashlight_offset);

    cl_adjustfov = Cvar_Get("cl_adjustfov", "1", 0);
}

void V_Shutdown(void)
{
    if(flashlight_profile_tex != -1)
        R_UnregisterImage(flashlight_profile_tex);
    Cmd_Deregister(v_cmds);
}



