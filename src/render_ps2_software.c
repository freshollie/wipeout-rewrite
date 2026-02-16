#include "system.h"
#include "render.h"
#include "mem.h"
#include "utils.h"
#include "platform.h"

// TODO: Clean up
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <stdio.h>
#include <malloc.h>

#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>

#include <math3d.h>

#define NEAR_PLANE 16.0
#define FAR_PLANE (RENDER_FADEOUT_FAR)
#define TEXTURES_MAX 1024


typedef struct {
	vec2i_t size;
	rgba_t *pixels;
} render_texture_t;

static void line(vec2i_t p0, vec2i_t p1, rgba_t color);

static rgba_t *screen_buffer;
static int32_t screen_pitch;
static int32_t screen_ppr;
static vec2i_t screen_size;

static mat4_t view_mat = mat4_identity();
static mat4_t mvp_mat = mat4_identity();
static mat4_t projection_mat = mat4_identity();
static mat4_t sprite_mat = mat4_identity();

// static render_texture_t textures[TEXTURES_MAX];
// static uint32_t textures_len;

uint16_t RENDER_NO_TEXTURE;

#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))

#define MAX_TEXTURES 512
#define TEXCACHE_SIZE (10 * 1024 * 1024)

// GS_SETREG_ALPHA(A, B, C, D, FIX)
// A = 0 = Cs
// B = 1 = Cd
// C = 0 = As
// D = 1 = Cd
// FIX =  128 (const alpha, unused)
// RGB = (A - B) * C + D = (Cs - Cd) * As + Cd -> normal blending, alpha 0-128
#define BMODE_BLEND GS_SETREG_ALPHA(0, 1, 0, 1, 128)

// A = 0 = Cs
// B = 2 = 0
// C = 1 = Ad
// D = 1 = Cd
// FIX =  128 (unused)
// RGB = (A - B) * C + D = Cs * Ad + Cd -> additive-ish blending
#define BMODE_ADD   GS_SETREG_ALPHA(0, 2, 1, 1, 128)

#define GS_SETREG_FOGCOL(R,G,B) \
    (u64)((R) & 0x000000FF) <<  0 | (u64)((G) & 0x000000FF) <<  8 | \
    (u64)((B) & 0x000000FF) << 16

extern GSGLOBAL *gs_global;

enum TexMode {
    TEXMODE_MODULATE,
    TEXMODE_DECAL,
    TEXMODE_REPLACE,
};

enum DrawFunc {
    DRAW_COL0,
    DRAW_COL0_FOG,
    DRAW_TEX0,
    DRAW_TEX0_FOG,
    DRAW_TEX0_COL0,
    DRAW_TEX0_COL0_FOG,
    DRAW_TEX0_COL0_DECAL,
    DRAW_TEX0_COL0_TEXA,
    DRAW_TEX0_COL0_COL1,
    DRAW_TEX0_TEX1_COL0,
};

typedef struct {
    float x, y, z;
    float u, v;
    uint32_t rgba;
} ps2_vertex_t;

#define PS2_VERT_CAP 4096

static ps2_vertex_t ps2_verts[PS2_VERT_CAP];
static int ps2_vert_len = 0;

typedef union TexCoord { 
    struct {
        float s, t;
    };
    u64 word;
} __attribute__((packed, aligned(8))) TexCoord;

typedef union ColorQ {
    struct {
        u8 r, g, b, a;
        float q;
    };
    u32 rgba;
    u64 word;
} __attribute__((packed, aligned(8))) ColorQ;

struct ShaderProgram {
    uint32_t shader_id;
    uint8_t num_inputs;
    bool used_textures[2];
    bool use_alpha;
    bool use_fog;
    bool alpha_test;
    enum TexMode tex_mode;
    enum DrawFunc draw_fn;
};

struct Texture {
    GSTEXTURE tex;
    uint32_t clamp_s;
    uint32_t clamp_t;
};

struct Viewport {
    float x, cy;
    float y, cx;
    float w, hw;
    float h, hh;
};

struct Clip {
    int x0;
    int y0;
    int x1;
    int y1;
};

#define RENDER_TRIS_BUFFER_CAPACITY 2048

static tris_t tris_buffer[RENDER_TRIS_BUFFER_CAPACITY];
static uint32_t tris_len = 0;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader;

static uint8_t *tex_cache;
static uint8_t *tex_cache_ptr;
static uint8_t *tex_cache_end;

static struct Texture textures[MAX_TEXTURES];
static uint16_t textures_len;

static struct Texture *cur_tex[2];
static struct Texture *last_tex;

static struct Clip r_clip;
static struct Viewport r_view;

static bool z_test = true;
static bool z_mask = false;
static bool z_decal = false;
static float z_offset = 0.f;

static bool a_test = false;
static bool do_blend = false;

static bool cull_backface = false;

static const uint64_t c_white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00);
static const uint64_t c_black = GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00);

static render_blend_mode_t blend_mode = RENDER_BLEND_NORMAL;

VECTOR   *verts;
GSPRIMPOINT *gs_vertices;


static void gfx_ps2_select_texture(int tile, uint16_t texture_id) {
    cur_tex[tile] = last_tex = textures + texture_id;
}

void render_init(vec2i_t screen_size) {
	gsKit_mode_switch(gs_global, GS_ONESHOT);

    gs_global->Test->ZTST = 2;

    // set alpha register for proper RGBA5551 alpha:
    // TA0 = 0x00: alpha bit is 0 -> alpha is 0x00
    // TA1 = 0x80: alpha bit is 1 -> alpha is 0x80

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_TEXA(0x00, 0, 0x80);
    *p_data++ = GS_TEXA;

    gsKit_queue_exec(gs_global);
    gsKit_queue_reset(gs_global->Os_Queue);

    // allocate texture cache
    tex_cache = memalign(128, TEXCACHE_SIZE);
    if (!tex_cache) {
        printf("gfx_ps2_init(): could not alloc %u byte texture cache\n", TEXCACHE_SIZE);
        abort();
    }
    tex_cache_end = tex_cache + TEXCACHE_SIZE;
    tex_cache_ptr = tex_cache;

	// allocate vertices buffer
	gs_vertices = (GSPRIMPOINT *)memalign(128, sizeof(GSPRIMPOINT) * 3);

	render_set_screen_size(screen_size);
	textures_len = 0;

	rgba_t white_pixels[4] = {
		rgba(128,128,128,255), rgba(128,128,128,255),
		rgba(128,128,128,255), rgba(128,128,128,255)
	};
	RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);
    gfx_ps2_select_texture(0, RENDER_NO_TEXTURE);
}

void render_cleanup(void) {}

void render_set_screen_size(vec2i_t size) {
	screen_size = size;

	float aspect = (float)size.x / (float)size.y;
	float fov = (73.75 / 180.0) * 3.14159265358;
	float f = 1.0 / tan(fov / 2);
	float nf = 1.0 / (NEAR_PLANE - FAR_PLANE);
	projection_mat = mat4(
		f / aspect, 0, 0, 0,
		0, f, 0, 0, 
		0, 0, (FAR_PLANE + NEAR_PLANE) * nf, -1, 
		0, 0, 2 * FAR_PLANE * NEAR_PLANE * nf, 0
	);
}

void render_set_resolution(render_resolution_t res) {}
void render_set_post_effect(render_post_effect_t post) {}

vec2i_t render_size(void) {
	return screen_size;
}

static void gfx_ps2_set_depth_test(bool depth_test) {
    z_test = depth_test;
}

static void gfx_ps2_set_depth_mask(bool z_upd) {
    z_mask = !z_upd;

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_ZBUF_1(gs_global->ZBuffer / 8192, gs_global->PSMZ, z_mask);
    *p_data++ = GS_ZBUF_1 + gs_global->PrimContext;
}

static void gfx_ps2_set_zmode_offset(float offset) {
	if (offset == 0) {
		z_decal = false;
	} else {
		z_decal = true;
	}

    z_offset = z_decal ? offset : 0.f;
}


static inline void draw_update_env(const bool atest, const int ztest, const bool fog) {
	// return;
    if (atest) {
        gs_global->Test->ATE = 1;
        gs_global->Test->ATST = 6; // ATEST_METHOD_GREATER
        gs_global->Test->AREF = 0x40;
    } else {
        gs_global->Test->ATE = 0;
        gs_global->Test->ATST = 1; // ATEST_METHOD_ALLPASS
    }

    gs_global->Test->ZTST = 1; // 1 is ALLPASS, 2 is GREATER
    gs_global->PrimFogEnable = fog;

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_TEST(
        gs_global->Test->ATE,  gs_global->Test->ATST,
        gs_global->Test->AREF, gs_global->Test->AFAIL,
        gs_global->Test->DATE, gs_global->Test->DATM,
        gs_global->Test->ZTE,  gs_global->Test->ZTST
    );
    *p_data++ = GS_TEST_1 + gs_global->PrimContext;
}


static inline void draw_set_scissor(const int x0, const int y0, const int x1, const int y1) {
    // scissor doesn't work with gskit hires
    if (gs_global->Mode == GS_MODE_DTV_720P || gs_global->Mode == GS_MODE_DTV_1080I) {
        return;
    }

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_SCISSOR_1(x0, x1, y0, y1);
    *p_data++ = GS_SCISSOR_1 + gs_global->PrimContext;
}

static void gfx_ps2_set_scissor(int x, int y, int width, int height) {
    r_clip.x0 = x;
    r_clip.y0 = gs_global->Height - y - height;
    r_clip.x1 = r_clip.x0 + width - 1;
    r_clip.y1 = r_clip.y0 + height - 1;
    draw_set_scissor(r_clip.x0, r_clip.y0, r_clip.x1, r_clip.y1);
}

static void draw_clear(const u64 color) {
    const bool old_zmask = z_mask;
    const bool old_tests = z_test || a_test;

    if (old_zmask) gfx_ps2_set_depth_mask(true); // write Z on clear
    if (old_tests) draw_update_env(0, 1, false); // no alpha test, zpass=ALWAYS, no fog
    if (r_clip.x0 || r_clip.y0) draw_set_scissor(0, 0, gs_global->Width - 1, gs_global->Height - 1); // clear whole screen

    u8 strips = gs_global->Width >> 6;
    const u8 remain = gs_global->Width & 63;

    u32 pos = 0;

    while (strips--) {
        gsKit_prim_sprite(gs_global, pos, 0, pos + 64, gs_global->Height, 0, color);
        pos += 64;
    }

    if (remain)
        gsKit_prim_sprite(gs_global, pos, 0, remain + pos, gs_global->Height, 0, color);

    if (old_zmask) gfx_ps2_set_depth_mask(false); // mask Z again
    if (old_tests) draw_update_env(a_test, z_test + (z_test && z_decal) + 1, false); // restore old tests
    if (r_clip.x0 || r_clip.y0) draw_set_scissor(r_clip.x0, r_clip.y0, r_clip.x1, r_clip.y1); // restore clip
}

static void draw_set_clamp(const u32 clamp_s, const u32 clamp_t) {
    gs_global->Clamp->WMS = clamp_s;
    gs_global->Clamp->WMT = clamp_t;

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_CLAMP(
        gs_global->Clamp->WMS, gs_global->Clamp->WMT,
        gs_global->Clamp->MINU, gs_global->Clamp->MAXU,
        gs_global->Clamp->MINV, gs_global->Clamp->MAXV
    );

    *p_data++ = GS_CLAMP_1 + gs_global->PrimContext;
}


void render_set_view(vec3_t pos, vec3_t angles) {
	view_mat = mat4_identity();
	mat4_set_translation(&view_mat, vec3(0, 0, 0));
	mat4_set_roll_pitch_yaw(&view_mat, vec3(angles.x, -angles.y + M_PI, angles.z + M_PI));
	mat4_translate(&view_mat, vec3_inv(pos));
	mat4_set_yaw_pitch_roll(&sprite_mat, vec3(-angles.x, angles.y - M_PI, 0));

	render_set_model_mat(&mat4_identity());
}

void render_set_view_2d(void) {
	float near = -1;
	float far = 1;
	float left = 0;
	float right = screen_size.x;
	float bottom = screen_size.y;
	float top = 0;
	float lr = 1 / (left - right);
	float bt = 1 / (bottom - top);
	float nf = 1 / (near - far);
	mvp_mat = mat4(
		-2 * lr,  0,  0,  0,
		0,  -2 * bt,  0,  0,
		0,        0,  2 * nf,    0, 
		(left + right) * lr, (top + bottom) * bt, (far + near) * nf, 1
	);
}

void render_set_model_mat(mat4_t *m) {
	// printf("set model mat\n");
	mat4_t vm_mat;
	mat4_mul(&vm_mat, &view_mat, m);
	mat4_mul(&mvp_mat, &projection_mat, &vm_mat);
}

void render_set_depth_write(bool enabled) {}
void render_set_depth_test(bool enabled) {}
void render_set_depth_offset(float offset) {}
void render_set_screen_position(vec2_t pos) {}
void render_set_blend_mode(render_blend_mode_t mode) {}
void render_set_cull_backface(bool enabled) {}

vec3_t render_transform(vec3_t pos) {
	return vec3_transform(vec3_transform(pos, &view_mat), &projection_mat);
}

static inline u32 lzw(u32 val) {
    u32 res;
    __asm__ __volatile__ ("   plzcw   %0, %1    " : "=r" (res) : "r" (val));
    return(res);
}

static inline void gsKit_set_tw_th(const GSTEXTURE *Texture, int *tw, int *th) {
    *tw = 31 - (lzw(Texture->Width) + 1);
    if(Texture->Width > (1<<*tw))
        (*tw)++;

    *th = 31 - (lzw(Texture->Height) + 1);
    if(Texture->Height > (1<<*th))
        (*th)++;
}


#define GIF_TAG_TRIANGLE_GORAUD_TEXTURED_ST_REGS(ctx) \
    ((u64)(GS_TEX0_1 + ctx) << 0 ) | \
    ((u64)(GS_PRIM)         << 4 ) | \
    ((u64)(GS_RGBAQ)        << 8 ) | \
    ((u64)(GS_ST)           << 12) | \
    ((u64)(GS_XYZ2)         << 16) | \
    ((u64)(GS_RGBAQ)        << 20) | \
    ((u64)(GS_ST)           << 24) | \
    ((u64)(GS_XYZ2)         << 28) | \
    ((u64)(GS_RGBAQ)        << 32) | \
    ((u64)(GS_ST)           << 36) | \
    ((u64)(GS_XYZ2)         << 40) | \
    ((u64)(GIF_NOP)         << 44)

static void gsKit_prim_triangle_goraud_texture_3d_st(
    GSGLOBAL *gsGlobal, GSTEXTURE *Texture,
    float x1, float y1, int iz1, float u1, float v1,
    float x2, float y2, int iz2, float u2, float v2,
    float x3, float y3, int iz3, float u3, float v3,
    u64 color1, u64 color2, u64 color3
) {
    gsKit_set_texfilter(gsGlobal, Texture->Filter);
    u64* p_store;
    u64* p_data;
    const int qsize = 6;
    const int bsize = 96;

    int tw, th;
    gsKit_set_tw_th(Texture, &tw, &th);

    int ix1 = gsKit_float_to_int_x(gsGlobal, x1);
    int ix2 = gsKit_float_to_int_x(gsGlobal, x2);
    int ix3 = gsKit_float_to_int_x(gsGlobal, x3);
    int iy1 = gsKit_float_to_int_y(gsGlobal, y1);
    int iy2 = gsKit_float_to_int_y(gsGlobal, y2);
    int iy3 = gsKit_float_to_int_y(gsGlobal, y3);

	// printf("tex: %f, %f\n", u1, v1);

    TexCoord st1 = (TexCoord) { { u1, v1 } };
    TexCoord st2 = (TexCoord) { { u2, v2 } };
    TexCoord st3 = (TexCoord) { { u3, v3 } };

    p_store = p_data = gsKit_heap_alloc(gsGlobal, qsize, bsize, GSKIT_GIF_PRIM_TRIANGLE_TEXTURED);

    *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED(0);
    *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED_ST_REGS(gsGlobal->PrimContext);

    const int replace = 0; // cur_shader->tex_mode == TEXMODE_REPLACE;
    const int alpha = gsGlobal->PrimAlphaEnable;

    if (Texture->VramClut == 0) {
        *p_data++ = GS_SETREG_TEX0(Texture->Vram/256, Texture->TBW, Texture->PSM,
            tw, th, alpha, replace,
            0, 0, 0, 0, GS_CLUT_STOREMODE_NOLOAD);
    } else {
        *p_data++ = GS_SETREG_TEX0(Texture->Vram/256, Texture->TBW, Texture->PSM,
            tw, th, alpha, replace,
            Texture->VramClut/256, Texture->ClutPSM, 0, 0, GS_CLUT_STOREMODE_LOAD);
    }

    *p_data++ = GS_SETREG_PRIM( GS_PRIM_PRIM_TRIANGLE, 1, 1, gsGlobal->PrimFogEnable,
                gsGlobal->PrimAlphaEnable, gsGlobal->PrimAAEnable,
                0, gsGlobal->PrimContext, 0);


    *p_data++ = color1;
    *p_data++ = st1.word;
    *p_data++ = GS_SETREG_XYZ2( ix1, iy1, iz1 );

    *p_data++ = color2;
    *p_data++ = st2.word;
    *p_data++ = GS_SETREG_XYZ2( ix2, iy2, iz2 );

    *p_data++ = color3;
    *p_data++ = st3.word;
    *p_data++ = GS_SETREG_XYZ2( ix3, iy3, iz3 );
}

VECTOR b;

void dummy(VECTOR* vert) {
	// g = vert;
	printf("%f\n", vert[0][0]);
}

void render_push_tris(tris_t tris, uint16_t texture_index) {
	// return;
	int center_x = screen_size.x >> 1;
	int center_y = screen_size.y >> 1;
	unsigned int max_z = 1 << (16 - 1);

	vec3_t temp_verts[3] = {
		// tris.vertices[0].pos,
		// tris.vertices[1].pos,
		// tris.vertices[2].pos,
		vec3_transform(tris.vertices[0].pos, &mvp_mat),
		vec3_transform(tris.vertices[1].pos, &mvp_mat),
		vec3_transform(tris.vertices[2].pos, &mvp_mat),
	};
	VECTOR verts[3];
	// printf("in: x: %f, y: %f, z: %f\n", tris.vertices[0].pos.x, tris.vertices[0].pos.y, tris.vertices[0].pos.z);
	// printf("out: x: %f, y: %f, z: %f\n", temp_verts[0].x, temp_verts[0].y, temp_verts[0].z);

	for (int i = 0; i < 3; i++) {
		if (temp_verts[i].z >= 1.0) {
			return;
		}
	}

	gfx_ps2_select_texture(0, texture_index);
    
	struct Texture* t = cur_tex[0];

    const bool zge = z_test && z_decal;
    draw_update_env(a_test, z_test + zge + 1, false);

    // if (cur_shader->used_textures[0]) {
    draw_set_clamp(t->clamp_s, t->clamp_t);
	// printf("BIND %d?\n", texture_index);
    gsKit_TexManager_bind(gs_global, &t->tex);
	// printf("Bound\n");

	for (int i = 0; i < 3; i++) {
		rgba_t* color = &tris.vertices[i].color;
		color->r = min(color->r * 2, 255);
		color->g = min(color->g * 2, 255);
		color->b = min(color->b * 2, 255);
		// color->a = clamp(color->a * (1.0-temp_verts[0].z) * FAR_PLANE * (2.0/255.0), 0, 255);
	}
	// printf("z: %f\n", temp_verts[0].z);

	for (int i = 0; i < 3; i++)
	{
		verts[i][0] = ((temp_verts[i].x + 1.0f) * center_x);
		verts[i][1] = ((1.0f - temp_verts[i].y) * center_y);
		verts[i][2] = ((temp_verts[i].z + 1.0f) * max_z);
	}
	// *b = *verts[0];
	// dummy(verts);


	for (int i = 0; i < 3; i++)
	{
		gs_vertices[i].rgbaq = color_to_RGBAQ(tris.vertices[i].color.r, tris.vertices[i].color.g, tris.vertices[i].color.b, tris.vertices[i].color.a, 0.0f);
		gs_vertices[i].xyz2 = vertex_to_XYZ2(gs_global, verts[i][0], verts[i][1], verts[i][2]);
	}

	gsKit_prim_triangle_goraud_texture_3d(gs_global, &t->tex, 
		verts[0][0], verts[0][1], verts[0][2], tris.vertices[0].uv.x, tris.vertices[0].uv.y, 
		verts[1][0], verts[1][1], verts[1][2], tris.vertices[1].uv.x, tris.vertices[1].uv.y,
		verts[2][0], verts[2][1], verts[2][2], tris.vertices[2].uv.x, tris.vertices[2].uv.y,  
		gs_vertices[0].rgbaq.color.rgbaq, 
		gs_vertices[1].rgbaq.color.rgbaq, 
		gs_vertices[2].rgbaq.color.rgbaq
	);
}

void render_push_sprite(vec3_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);

	vec3_t p0 = vec3_add(pos, vec3_transform(vec3(-size.x * 0.5, -size.y * 0.5, 0), &sprite_mat));
	vec3_t p1 = vec3_add(pos, vec3_transform(vec3( size.x * 0.5, -size.y * 0.5, 0), &sprite_mat));
	vec3_t p2 = vec3_add(pos, vec3_transform(vec3(-size.x * 0.5,  size.y * 0.5, 0), &sprite_mat));
	vec3_t p3 = vec3_add(pos, vec3_transform(vec3( size.x * 0.5,  size.y * 0.5, 0), &sprite_mat));

	struct Texture *t = &textures[texture_index];
	render_push_tris((tris_t){
		.vertices = {
			{.pos = p0, .uv = {0, 0}, .color = color},
			{.pos = p1, .uv = {0 + t->tex.Width ,0}, .color = color},
			{.pos = p2, .uv = {0, 0 + t->tex.Height}, .color = color},
		}
	}, texture_index);
	render_push_tris((tris_t){
		.vertices = {
			{.pos = p2, .uv = {0, 0 + t->tex.Height}, .color = color},
			{.pos = p1, .uv = {0 + t->tex.Width, 0}, .color = color},
			{.pos = p3, .uv = {0 + t->tex.Width, 0 + t->tex.Height}, .color = color},
		}
	}, texture_index);
}

void render_push_2d(vec2i_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
	render_push_2d_tile(pos, vec2i(0, 0), render_texture_size(texture_index), size, color, texture_index);
}

void render_push_2d_tile(vec2i_t pos, vec2i_t uv_offset, vec2i_t uv_size, vec2i_t size, rgba_t color, uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	render_push_tris((tris_t){
		.vertices = {
			{.pos = {pos.x, pos.y + size.y, 0}, .uv = {uv_offset.x , uv_offset.y + uv_size.y}, .color = color},
			{.pos = {pos.x + size.x, pos.y, 0}, .uv = {uv_offset.x +  uv_size.x, uv_offset.y}, .color = color},
			{.pos = {pos.x, pos.y, 0}, .uv = {uv_offset.x , uv_offset.y}, .color = color},
		}
	}, texture_index);

	render_push_tris((tris_t){
		.vertices = {
			{.pos = {pos.x + size.x, pos.y + size.y, 0}, .uv = {uv_offset.x + uv_size.x, uv_offset.y + uv_size.y}, .color = color},
			{.pos = {pos.x + size.x, pos.y, 0}, .uv = {uv_offset.x + uv_size.x, uv_offset.y}, .color = color},
			{.pos = {pos.x, pos.y + size.y, 0}, .uv = {uv_offset.x , uv_offset.y + uv_size.y}, .color = color},
		}
	}, texture_index);
}


static void gfx_ps2_upload_texture_ext(rgba_t *buf, int width, int height) {
    last_tex->tex.Width = width;
    last_tex->tex.Height = height;
    last_tex->tex.Filter = GS_FILTER_NEAREST;

	// TODO: these are what I think are the game defaults
    last_tex->clamp_s = GS_CMODE_CLAMP;
    last_tex->clamp_t = GS_CMODE_CLAMP;

    last_tex->tex.PSM = GS_PSM_CT32; // RGBA8888

    const uint32_t in_size = gsKit_texture_size_ee(width, height, last_tex->tex.PSM);
    // DMA has to copy from a 128-aligned address; cache base is 128-aligned, so we just align size to 128
    const uint32_t aligned_size = ALIGN(in_size, 128);

    if (tex_cache_ptr + aligned_size > tex_cache_end) {
        printf("gfx_ps2_upload_texture_ext(%p, %d, %d): out of cache space!\n", buf, width, height);
        tex_cache_ptr = tex_cache; // whatever, just continue from start
    }

    last_tex->tex.Mem = (void *)tex_cache_ptr;
    tex_cache_ptr += aligned_size;

    memcpy(last_tex->tex.Mem, buf, in_size);
}

static uint16_t gfx_ps2_new_texture(void) {
    const uint16_t tid = textures_len++;

    struct Texture *tex = textures + tid;

    if (cur_tex[0] == tex) cur_tex[0] = NULL;
    if (cur_tex[1] == tex) cur_tex[1] = NULL;
    if (last_tex == tex) last_tex = NULL;

    if (tex->tex.Vram) {
        // this was probably already freed by gsKit_TexManager_init
        gsKit_TexManager_invalidate(gs_global, &tex->tex);
    }

    tex->tex.Mem = NULL;

    return tid;
}

uint16_t render_texture_create(uint32_t width, uint32_t height, rgba_t *pixels) {
	uint16_t tid = gfx_ps2_new_texture();
	// TODO: do we need this "curr_tex" stuff?
	gfx_ps2_select_texture(0, tid);
	gfx_ps2_upload_texture_ext(pixels, width, height);
	return tid;
}

vec2i_t render_texture_size(uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	vec2i_t ret = { textures[texture_index].tex.Width, textures[texture_index].tex.Height };
	return ret;
}

void render_texture_replace_pixels(int16_t texture_index, rgba_t *pixels) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	struct Texture *t = &textures[texture_index];
	const uint32_t in_size = gsKit_texture_size_ee(t->tex.Width, t->tex.Height, t->tex.PSM);
	memcpy(t->tex.Mem, pixels, ALIGN(in_size, 128));
	gsKit_TexManager_invalidate(gs_global, &t->tex);
}

uint16_t render_textures_len(void) {
	return textures_len;
}

void render_textures_reset(uint16_t len) {
	error_if(len > textures_len, "Invalid texture reset len %d >= %d", len, textures_len);
	textures_len = len;
}

void render_textures_dump(const char *path) {}

void render_frame_prepare(void) {
	// printf("new frame\n");
	draw_clear(c_black);
    // gfx_ps2_set_viewport(0, 0, screen_size.x, screen_size.y);
	gfx_ps2_set_depth_test(true);
	gfx_ps2_set_depth_mask(true);
}

void render_frame_end(void) {
    // gfx_ps2_set_viewport(0, 0, screen_size.x, screen_size.y);
}

// -----------------------------------------------------------------------------

static inline rgba_t color_mix(rgba_t in, rgba_t out) {
	return rgba(
		lerp(in.r, out.r, out.a/255.0),
		lerp(in.g, out.g, out.a/255.0),
		lerp(in.b, out.b, out.a/255.0),
		1
	);
}

typedef enum {
	CLIP_INSIDE = 0,
	CLIP_LEFT   = (1<<0),
	CLIP_RIGHT  = (1<<1),
	CLIP_BOTTOM = (1<<2),
	CLIP_TOP    = (1<<3),
} clip_code_t;

static inline clip_code_t clip_code(vec2i_t p) {
	clip_code_t cc = CLIP_INSIDE;
	if (p.x < 0) {
		flags_add(cc, CLIP_LEFT);
	}
	else if (p.x >= screen_size.x) {
		flags_add(cc, CLIP_RIGHT);	
	}
	if (p.y < 0) {
		flags_add(cc, CLIP_BOTTOM);
	}
	else if (p.y >= screen_size.y) {
		flags_add(cc, CLIP_TOP);	
	}
	return cc;
}

static void line(vec2i_t p0, vec2i_t p1, rgba_t color) {
	// Cohen Sutherland Line Clipping
	clip_code_t cc0 = clip_code(p0);
	clip_code_t cc1 = clip_code(p1);
	bool accept = false;

	vec2i_t ss = vec2i(screen_size.x-1, screen_size.y-1);
	while (true) {
		if (!(cc0 | cc1)) {
			accept = true;
			break;
		}
		else if (cc0 & cc1) {
			break;
		}
		else {
			vec2i_t r = p0;
			clip_code_t cc_out = cc0 ? cc0 : cc1;

			if (flags_is(cc_out, CLIP_TOP)) {
				r.x = p0.x + (p1.x - p0.x) * (ss.y - p0.y) / (p1.y - p0.y);
				r.y = ss.y;
			}
			else if (flags_is(cc_out, CLIP_BOTTOM)) {
				r.x = p0.x + (p1.x - p0.x) * (-p0.y) / (p1.y - p0.y);
				r.y = 0;
			}
			else if (flags_is(cc_out, CLIP_RIGHT)) {
				r.y = p0.y + (p1.y - p0.y) * (ss.x - p0.x) / (p1.x - p0.x);
				r.x = ss.x;
			}
			else if (flags_is(cc_out, CLIP_LEFT)) {
				r.y = p0.y + (p1.y - p0.y) * (-p0.x) / (p1.x - p0.x);
				r.x = 0;
			}

			if (cc_out == cc0) {
				p0.x = r.x;
				p0.y = r.y;
				cc0 = clip_code(p0);
			}
			else {
				p1.x = r.x;
				p1.y = r.y;
				cc1 = clip_code(p1);
			}
		}
	}
	if (!accept) {
		return;
	}

	// Bresenham's line algorithm
	bool steep = false; 
	if (abs(p0.x - p1.x) < abs(p0.y - p1.y)) {
		swap(p0.x, p0.y); 
		swap(p1.x, p1.y); 
		steep = true;
	} 
	if (p0.x > p1.x) { 
		swap(p0.x, p1.x); 
		swap(p0.y, p1.y); 
	} 
	int32_t dx = p1.x - p0.x; 
	int32_t dy = p1.y - p0.y; 
	int32_t derror2 = abs(dy) * 2; 
	int32_t error2 = 0; 
	int32_t y = p0.y;
	int32_t ydir = (p1.y > p0.y ? 1 : -1);

	if (steep) {
		for (int32_t x = p0.x; x <= p1.x; x++) {
			screen_buffer[x * screen_ppr + y] = color_mix(screen_buffer[x * screen_ppr + y], color);
			error2 += derror2; 
			if (error2 > dx) { 
				y += ydir;
				error2 -= dx * 2; 
			} 
		}
	}
	else {
		for (int32_t x = p0.x; x <= p1.x; x++) {
			screen_buffer[y * screen_ppr + x] = color_mix(screen_buffer[y * screen_ppr + x], color);
			error2 += derror2; 
			if (error2 > dx) { 
				y += ydir;
				error2 -= dx * 2; 
			} 
		}
	}
}
