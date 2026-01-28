#include "system.h"
#include "render.h"
#include "mem.h"
#include "utils.h"
#include "platform.h"

#define NEAR_PLANE 16.0
#define FAR_PLANE (RENDER_FADEOUT_FAR)
#define TEXTURES_MAX 1024


static void line(vec2i_t p0, vec2i_t p1, rgba_t color);

static rgba_t *screen_buffer;
static int32_t screen_pitch;
static int32_t screen_ppr;
static vec2i_t screen_size;

static mat4_t view_mat = mat4_identity();
static mat4_t mvp_mat = mat4_identity();
static mat4_t projection_mat = mat4_identity();
static mat4_t sprite_mat = mat4_identity();

uint16_t RENDER_NO_TEXTURE;

void render_cleanup(void) {}


// ----- PS2 -----

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

static void render_flush(void);


static inline float fclamp(const float v, const float min, const float max) {
    return (v < min) ? min : (v > max) ? max : v;
}


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

	render_set_screen_size(screen_size);
	textures_len = 0;

	rgba_t white_pixels[4] = {
		rgba(128,128,128,255), rgba(128,128,128,255),
		rgba(128,128,128,255), rgba(128,128,128,255)
	};
	RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);
    gfx_ps2_select_texture(0, RENDER_NO_TEXTURE);
}

static void gfx_ps2_set_sampler_parameters(int tile, bool linear_filter) {
    cur_tex[tile]->tex.Filter = linear_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    cur_tex[tile]->clamp_s = GS_CMODE_CLAMP;
    cur_tex[tile]->clamp_t = GS_CMODE_CLAMP;
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

static void gfx_ps2_set_viewport(int x, int y, int width, int height) {
    r_view.x = x;
    r_view.y = y;
    r_view.w = width;
    // 1080i requires the view point is half height
    if (gs_global->Mode == GS_MODE_DTV_1080I) {
        height /= 2;
    }
    r_view.h = height;
    r_view.hw = r_view.w * 0.5f;
    r_view.hh = r_view.h * 0.5f;
    r_view.cx = r_view.x + r_view.hw;
    r_view.cy = r_view.y + r_view.hh;
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

    // memcpy(last_tex->tex.Mem, buf, in_size);
}

static bool tex_changed(uint16_t texture_id) {
	if (last_tex == NULL) {
		return false;
	}
	return &textures[texture_id] != last_tex ? true : false;
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


void render_push_2d(vec2i_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
	render_push_2d_tile(pos, vec2i(0, 0), render_texture_size(texture_index), size, color, texture_index);
}

void render_push_2d_tile(vec2i_t pos, vec2i_t uv_offset, vec2i_t uv_size, vec2i_t size, rgba_t color, uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
    printf("push 2d tile %d %d?\n", pos.x, pos.y);
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
			{.pos = p2, .uv = {0, 0 + t->tex.Height }, .color = color},
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

vec2i_t render_texture_size(uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	vec2i_t ret = { textures[texture_index].tex.Width, textures[texture_index].tex.Height };
	return ret;
}

void render_texture_replace_pixels(int16_t texture_index, rgba_t *pixels) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	struct Texture *t = &textures[texture_index];
	memcpy(t->tex.Mem, pixels, t->tex.Width * t->tex.Height * sizeof(rgba_t));
	gsKit_TexManager_invalidate(gs_global, &t->tex);
}

uint16_t render_textures_len(void) {
	return textures_len;
}

static inline void draw_set_blendmode(const u64 blend) {
    gs_global->PrimAlphaEnable = !!blend;
    gs_global->PrimAlpha = blend;
    gs_global->PABE = 0;

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = gs_global->PrimAlpha;
    *p_data++ = GS_ALPHA_1 + gs_global->PrimContext;
}

static void gfx_ps2_set_use_alpha(bool use_alpha) {
    do_blend = use_alpha;
    draw_set_blendmode(do_blend ? BMODE_BLEND : 0);
}

static void gfx_ps2_set_fog_color(const uint8_t r, const uint8_t g, const uint8_t b) {
    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_FOGCOL(r, g, b);
    *p_data++ = GS_FOGCOL;
}

static inline void viewport_transform(float *v) {
    v[0] = v[0] *  r_view.hw + r_view.cx;
    v[1] = v[1] * -r_view.hh + r_view.cy;
    v[2] = fclamp((1.0f - v[2]) * 65535.f + z_offset, 0.f, 65535.f);
}

static inline void viewport_transform_vertex(vertex_t *v) {
    v->pos.x = v->pos.x *  r_view.hw + r_view.cx;
    v->pos.y = v->pos.y * -r_view.hh + r_view.cy;
    v->pos.z = fclamp((1.0f - v->pos.z) * 65535.f + z_offset, 0.f, 65535.f);
    // v->pos.z = 0x800000;
}


// these are exactly the same as their regular varieties, but the mapping type is set to ST (UV / w)

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


#define GIF_TAG_TRIANGLE_GORAUD_TEXTURED_ST_FOG_REGS(ctx) \
    ((u64)(GS_TEX0_1 + ctx) << 0 ) | \
    ((u64)(GS_PRIM)         << 4 ) | \
    ((u64)(GS_RGBAQ)        << 8 ) | \
    ((u64)(GS_ST)           << 12) | \
    ((u64)(GS_XYZF2)        << 16) | \
    ((u64)(GS_RGBAQ)        << 20) | \
    ((u64)(GS_ST)           << 24) | \
    ((u64)(GS_XYZF2)        << 28) | \
    ((u64)(GS_RGBAQ)        << 32) | \
    ((u64)(GS_ST)           << 36) | \
    ((u64)(GS_XYZF2)        << 40) | \
    ((u64)(GIF_NOP)         << 44)

// this is the same as the TRIANGLE_GORAUD primitive, but with XYZF

#define GIF_TAG_TRIANGLE_GOURAUD_FOG_REGS \
    ((u64)(GS_PRIM)  << 0)  | \
    ((u64)(GS_RGBAQ) << 4)  | \
    ((u64)(GS_XYZF2) << 8)  | \
    ((u64)(GS_RGBAQ) << 12) | \
    ((u64)(GS_XYZF2) << 16) | \
    ((u64)(GS_RGBAQ) << 20) | \
    ((u64)(GS_XYZF2) << 24) | \
    ((u64)(GIF_NOP)  << 28)

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

static void gsKit_prim_triangle_gouraud_3d_fog(
    GSGLOBAL *gsGlobal, float x1, float y1, int iz1,
    float x2, float y2, int iz2,
    float x3, float y3, int iz3,
    u64 color1, u64 color2, u64 color3,
    u8 fog1, u8 fog2, u8 fog3
) {
    u64* p_store;
    u64* p_data;
    const int qsize = 4;
    const int bsize = 64;

    int ix1 = gsKit_float_to_int_x(gsGlobal, x1);
    int iy1 = gsKit_float_to_int_y(gsGlobal, y1);

    int ix2 = gsKit_float_to_int_x(gsGlobal, x2);
    int iy2 = gsKit_float_to_int_y(gsGlobal, y2);

    int ix3 = gsKit_float_to_int_x(gsGlobal, x3);
    int iy3 = gsKit_float_to_int_y(gsGlobal, y3);

    p_store = p_data = gsKit_heap_alloc(gsGlobal, qsize, bsize, GSKIT_GIF_PRIM_TRIANGLE_GOURAUD);

    if (p_store == gsGlobal->CurQueue->last_tag) {
        *p_data++ = GIF_TAG_TRIANGLE_GOURAUD(0);
        *p_data++ = GIF_TAG_TRIANGLE_GOURAUD_FOG_REGS;
    }

    *p_data++ = GS_SETREG_PRIM( GS_PRIM_PRIM_TRIANGLE, 1, 0, gsGlobal->PrimFogEnable,
                gsGlobal->PrimAlphaEnable, gsGlobal->PrimAAEnable,
                0, gsGlobal->PrimContext, 0) ;

    *p_data++ = color1;
    *p_data++ = GS_SETREG_XYZF2(ix1, iy1, iz1, fog1);

    *p_data++ = color2;
    *p_data++ = GS_SETREG_XYZF2(ix2, iy2, iz2, fog2);

    *p_data++ = color3;
    *p_data++ = GS_SETREG_XYZF2(ix3, iy3, iz3, fog3);
}

static void gsKit_prim_triangle_goraud_texture_3d_st_fog(
    GSGLOBAL *gsGlobal, GSTEXTURE *Texture,
    float x1, float y1, int iz1, float u1, float v1,
    float x2, float y2, int iz2, float u2, float v2,
    float x3, float y3, int iz3, float u3, float v3,
    u64 color1, u64 color2, u64 color3,
    u8 fog1, u8 fog2, u8 fog3
) {
    gsKit_set_texfilter(gsGlobal, Texture->Filter);
    u64* p_store;
    u64* p_data;
    int qsize = 6;
    int bsize = 96;

    int tw, th;
    gsKit_set_tw_th(Texture, &tw, &th);

    int ix1 = gsKit_float_to_int_x(gsGlobal, x1);
    int ix2 = gsKit_float_to_int_x(gsGlobal, x2);
    int ix3 = gsKit_float_to_int_x(gsGlobal, x3);
    int iy1 = gsKit_float_to_int_y(gsGlobal, y1);
    int iy2 = gsKit_float_to_int_y(gsGlobal, y2);
    int iy3 = gsKit_float_to_int_y(gsGlobal, y3);

    TexCoord st1 = (TexCoord) { { u1, v1 } };
    TexCoord st2 = (TexCoord) { { u2, v2 } };
    TexCoord st3 = (TexCoord) { { u3, v3 } };

    p_store = p_data = gsKit_heap_alloc(gsGlobal, qsize, bsize, GSKIT_GIF_PRIM_TRIANGLE_TEXTURED);

    *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED(0);
    *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED_ST_FOG_REGS(gsGlobal->PrimContext);

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
    *p_data++ = GS_SETREG_XYZF2( ix1, iy1, iz1, fog1 );

    *p_data++ = color2;
    *p_data++ = st2.word;
    *p_data++ = GS_SETREG_XYZF2( ix2, iy2, iz2, fog2 );

    *p_data++ = color3;
    *p_data++ = st3.word;
    *p_data++ = GS_SETREG_XYZF2( ix3, iy3, iz3, fog3 );
}


static inline void draw_update_env(const bool atest, const int ztest, const bool fog) {
    if (atest) {
        gs_global->Test->ATE = 1;
        gs_global->Test->ATST = 6; // ATEST_METHOD_GREATER
        gs_global->Test->AREF = 0x40;
    } else {
        gs_global->Test->ATE = 0;
        gs_global->Test->ATST = 1; // ATEST_METHOD_ALLPASS
    }

    gs_global->Test->ZTST = ztest; // 1 is ALLPASS, 2 is GREATER
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

#define MAX_BUFFERED 256
static float buf_vbo[MAX_BUFFERED * (26 * 3)] __attribute__((__aligned__(16))); // 3 vertices in a triangle and 26 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static inline void draw_triangles_tex_col(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = 6;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }
}

static inline void draw_triangles_tex_col_fog(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = 7;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st_fog(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word,
            v0[6], v1[6], v2[6]
        );
    }
}

static inline void draw_triangles_tex_col_texalpha(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x00, 0x00, 0x00, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x00, 0x00, 0x00, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x00, 0x00, 0x00, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = 6;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.a = 0x80; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.a = 0x80; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.a = 0x80; c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }
}

static inline void draw_triangles_col(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride, const size_t rgba_add) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = 4 + rgba_add;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
        gsKit_prim_triangle_gouraud_3d(
            gs_global,
            v0[0], v0[1], v0[2],
            v1[0], v1[1], v1[2],
            v2[0], v2[1], v2[2],
            c0.word, c1.word, c2.word
        );
    }
}

static inline void draw_triangles_col_fog(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride, const size_t rgba_add) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = 5 + rgba_add;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
        gsKit_prim_triangle_gouraud_3d_fog(
            gs_global,
            v0[0], v0[1], v0[2],
            v1[0], v1[1], v1[2],
            v2[0], v2[1], v2[2],
            c0.word, c1.word, c2.word,
            v0[4], v1[4], v2[4]
        );
    }
}

static void draw_triangles_tex(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.q = v0[3];
        c1.q = v1[3];
        c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }
}

static void draw_triangles_tex_fog(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.q = v0[3];
        c1.q = v1[3];
        c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st_fog(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word,
            v0[6], v1[6], v2[6]
        );
    }
}

static void draw_triangles_tex_col_decal(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    // draw color base, color offset is 2 because we skip UVs
    draw_triangles_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride, 2);

    // alpha test on, blending on, ztest to GEQUAL
    const bool old_blend = do_blend;
    if (!old_blend) gfx_ps2_set_use_alpha(true);
    draw_update_env(a_test, 2, cur_shader->use_fog);

    // draw texture with blending on top, don't need to transform this time

    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;
        v1 = v0 + vtx_stride;
        v2 = v1 + vtx_stride;
        c0.q = v0[3];
        c1.q = v1[3];
        c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }

    // restore old state
    if (!old_blend) gfx_ps2_set_use_alpha(false);
    draw_update_env(a_test, z_test + (z_test && z_decal) + 1, cur_shader->use_fog);
}

static void draw_triangles_tex_col_col(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    // draw color base, color offset is 2 because we skip UVs
    draw_triangles_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride, 3);

    // alpha test off, special blending on, ztest to GEQUAL
    draw_set_blendmode(BMODE_ADD);
    draw_update_env(0, 2, cur_shader->use_fog);

    // draw texture with blending on top, don't need to transform this time

    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;
        v1 = v0 + vtx_stride;
        v2 = v1 + vtx_stride;
        c0.rgba = ((u32 *)v0)[7]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[7]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[7]; c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }

    // restore old state
    gfx_ps2_set_use_alpha(do_blend);
    draw_update_env(a_test, z_test + (z_test && z_decal) + 1, cur_shader->use_fog);
}

static void draw_triangles_tex_tex_col(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    // draw base textire with plain white color
    draw_triangles_tex(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride);

    // alpha test off, blending on, ztest to GEQUAL
    if (!do_blend) draw_set_blendmode(BMODE_BLEND);
    draw_update_env(0, 2, cur_shader->use_fog);

    // draw second texture with blending on top, don't need to transform this time
    // however use color as alpha, since alpha is fixed at 1 in that shader

    draw_set_clamp(cur_tex[1]->clamp_s, cur_tex[1]->clamp_t);
    gsKit_TexManager_bind(gs_global, &cur_tex[1]->tex);

    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;
        v1 = v0 + vtx_stride;
        v2 = v1 + vtx_stride;
        c0.a = ((u32 *)v0)[6] & 0xFF; c0.q = v0[3];
        c1.a = ((u32 *)v1)[6] & 0xFF; c1.q = v1[3];
        c2.a = ((u32 *)v2)[6] & 0xFF; c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[1]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }

    // restore old state
    if (!do_blend) gfx_ps2_set_use_alpha(false);
    draw_update_env(a_test, z_test + (z_test && z_decal) + 1, cur_shader->use_fog);
}

static void gfx_ps2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    const size_t vtx_stride = buf_vbo_len / (buf_vbo_num_tris * 3);
    const size_t tri_stride = vtx_stride * 3;

    const bool zge = z_test && z_decal;
    draw_update_env(a_test, z_test + zge + 1, false);

    // if (cur_shader->used_textures[0]) {
    //     draw_set_clamp(cur_tex[0]->clamp_s, cur_tex[0]->clamp_t);
    //     gsKit_TexManager_bind(gs_global, &cur_tex[0]->tex);
    // }

    switch (DRAW_TEX0_COL0) {
        // case DRAW_TEX0_TEX1_COL0:  draw_triangles_tex_tex_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_TEX0_COL0_COL1:  draw_triangles_tex_col_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_TEX0_COL0_TEXA:  draw_triangles_tex_col_texalpha(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_TEX0_COL0_DECAL: draw_triangles_tex_col_decal(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_TEX0_COL0_FOG:   draw_triangles_tex_col_fog(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        case DRAW_TEX0_COL0:       draw_triangles_tex_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_TEX0_FOG:        draw_triangles_tex_fog(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_TEX0:            draw_triangles_tex(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride); break;
        // case DRAW_COL0_FOG:        draw_triangles_col_fog(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride, (cur_shader->num_inputs > 1)); break;
        // default:                   draw_triangles_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride, (cur_shader->num_inputs > 1)); break;
    }
}


void render_flush(void) {
    if (buf_vbo_len > 0) {
        int num = buf_vbo_num_tris;
        gfx_ps2_draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
    // if (tris_len == 0) {
	// 	return;
	// }

    // ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    // ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    // ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    // register float *v0, *v1, *v2;
    // register float *p = buf_vbo;
    // register size_t i;

    // const int cofs = 7;
    // for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
    //     v0 = p + 0;           viewport_transform(v0);
    //     v1 = v0 + vtx_stride; viewport_transform(v1);
    //     v2 = v1 + vtx_stride; viewport_transform(v2);
    //     c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
    //     c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
    //     c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
    //     gsKit_prim_triangle_goraud_texture_3d_st_fog(
    //         gs_global, &cur_tex[0]->tex,
    //         v0[0], v0[1], v0[2], v0[4], v0[5],
    //         v1[0], v1[1], v1[2], v1[4], v1[5],
    //         v2[0], v2[1], v2[2], v2[4], v2[5],
    //         c0.word, c1.word, c2.word,
    //         v0[6], v1[6], v2[6]
    //     );
    // }

	// if (texture_mipmap_is_dirty) {
	// 	glGenerateMipmap(GL_TEXTURE_2D);
	// 	texture_mipmap_is_dirty = false;
	// }

	// glBindBuffer(GL_ARRAY_BUFFER, vbo);
	// glBufferData(GL_ARRAY_BUFFER, sizeof(tris_t) * tris_len, tris_buffer, GL_DYNAMIC_DRAW);
	// glDrawArrays(GL_TRIANGLES, 0, tris_len * 3);
	tris_len = 0;
}

void render_set_depth_write(bool enabled) {

}
void render_set_depth_test(bool enabled) {
	render_flush();
	gfx_ps2_set_depth_test(enabled);
}
void render_set_depth_offset(float offset) {
	render_flush();
	gfx_ps2_set_zmode_offset(offset);
}
void render_set_screen_position(vec2_t pos) {
	render_flush();
	// // TODO: this couldbr wrong
	// gs_global->OffsetX = pos.x;
	// gs_global->OffsetY = pos.y;
}

#define GFX_OUT_COORD(x) (x * inv_w)

void render_push_tris(tris_t tris, uint16_t texture_index) {
    // const struct LoadedVertex *v_arr[3] = {{tris[0].x, tris[0].y, tris[0].z, 0}, v2, v3};
    error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);

    gfx_ps2_select_texture(0, RENDER_NO_TEXTURE);
    
	struct Texture* t = cur_tex[0];

    const bool zge = z_test && z_decal;
    draw_update_env(a_test, z_test + zge + 1, false);

    // if (cur_shader->used_textures[0]) {
    draw_set_clamp(t->clamp_s, t->clamp_t);
    gsKit_TexManager_bind(gs_global, &t->tex);


    vertex_t *v0 = &tris.vertices[0];
    vertex_t *v1 = &tris.vertices[1];
    vertex_t *v2 = &tris.vertices[2];

    gsKit_prim_triangle_goraud_texture_3d(
        gs_global,
        &t->tex,

        // vertex 0
        v0->pos.x, v0->pos.y, v0->pos.z,
        v0->uv.x,  v0->uv.y,
        GS_SETREG_RGBAQ(
            v0->color.r,
            v0->color.g,
            v0->color.b,
            v0->color.a,
            0x00
        ),

        // vertex 1
        v1->pos.x, v1->pos.y, v1->pos.z,
        v1->uv.x,  v1->uv.y,
        GS_SETREG_RGBAQ(
            v1->color.r,
            v1->color.g,
            v1->color.b,
            v1->color.a,
            0x00
        ),

        // vertex 2
        v2->pos.x, v2->pos.y, v2->pos.z,
        v2->uv.x,  v2->uv.y,
        GS_SETREG_RGBAQ(
            v2->color.r,
            v2->color.g,
            v2->color.b,
            v2->color.a,
            0x00
        )
    );

    // ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    // ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    // ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    // vertex_t *v0, *v1, *v2;

    // v0 = &tris.vertices[0]; viewport_transform_vertex(v0);
    // v1 = &tris.vertices[1]; viewport_transform_vertex(v1);
    // v2 = &tris.vertices[2]; viewport_transform_vertex(v2);
    // c0.rgba = *(u32*)&v0->color; c0.q = 1.0;
    // c1.rgba = *(u32*)&v1->color; c1.q = 1.0;
    // c2.rgba = *(u32*)&v2->color; c2.q = 1.0;
    // printf("coord %f %f %f %f %f\n", v0->pos.x, v0->pos.y, v0->pos.z, v0->uv.x, v0->uv.y);
    // gsKit_prim_triangle_goraud_texture_3d_st(
    //     gs_global, &cur_tex[0]->tex,
    //     v0->pos.x, v0->pos.y, v0->pos.z, v0->uv.x, v0->uv.y,
    //     v1->pos.x, v1->pos.y, v1->pos.z, v1->uv.x, v1->uv.y,
    //     v2->pos.x, v2->pos.y, v2->pos.z, v2->uv.x, v2->uv.y,
    //     c0.word, c1.word, c2.word
    // );

//     bool used_textures[2], use_fog = false, use_alpha = false;
// 	const bool z_is_from_0_to_1 = true;

//     const bool solid_texture = true;

//     for (int i = 0; i < 3; i++) {
//         const float w = v_arr[i]->w;
//         const float inv_w = 1.f / w;

//         float z = v_arr[i]->z;
//         if (z_is_from_0_to_1) z = (z + w) * 0.5f;

//         buf_vbo[buf_vbo_len++] = GFX_OUT_COORD(v_arr[i]->x);
//         buf_vbo[buf_vbo_len++] = GFX_OUT_COORD(v_arr[i]->y);
//         buf_vbo[buf_vbo_len++] = GFX_OUT_COORD(z);
//         buf_vbo[buf_vbo_len++] = inv_w;

//         if (use_texture) {
//             float u = (v_arr[i]->u - rdp.texture_tile.uls * 8) / 32.0f;
//             float v = (v_arr[i]->v - rdp.texture_tile.ult * 8) / 32.0f;
//             if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
//                 // Linear filter adds 0.5f to the coordinates
//                 u += 0.5f;
//                 v += 0.5f;
//             }
//             u *= inv_tex_width;
//             v *= inv_tex_height;
//             // quads with mirror textures on them usually go (-1, +1)
//             if (mirror_u) u *= 0.5f;
//             if (mirror_v) v *= 0.5f;
//             buf_vbo[buf_vbo_len++] = GFX_OUT_COORD(u);
//             buf_vbo[buf_vbo_len++] = GFX_OUT_COORD(v);
//         }

//         if (use_fog) {
//             buf_vbo[buf_vbo_len++] = 255.f - (float)v_arr[i]->color.a; // fog factor (not alpha)
//         }

//         for (int j = 0; j < num_inputs; j++) {
//             const union RGBA *color;
//             union RGBA tmp;
//             union RGBA out = (union RGBA) { { 0x80, 0x80, 0x80, GFX_ALPHA_ONE } };
//             for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
//                 switch (comb->shader_input_mapping[k][j]) {
//                     case CC_PRIM:
//                         color = &rdp.prim_color;
//                         break;
//                     case CC_SHADE:
//                         color = &v_arr[i]->color;
//                         break;
//                     case CC_ENV:
//                         color = &rdp.env_color;
//                         break;
//                     case CC_LOD:
//                     {
//                         float distance_frac = (v1->w - 3000.0f) / 3000.0f;
//                         if (distance_frac < 0.0f) distance_frac = 0.0f;
//                         if (distance_frac > 1.0f) distance_frac = 1.0f;
//                         tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
//                         color = &tmp;
//                         break;
//                     }
//                     default:
//                         memset(&tmp, 0, sizeof(tmp));
//                         color = &tmp;
//                         break;
//                 }
// #ifndef GFX_PACK_COLORS
//                 if (k == 0) {
//                     buf_vbo[buf_vbo_len++] = GFX_COLOR_CONV(color->r);
//                     buf_vbo[buf_vbo_len++] = GFX_COLOR_CONV(color->g);
//                     buf_vbo[buf_vbo_len++] = GFX_COLOR_CONV(color->b);
//                 } else {
//                     if (use_fog && color == &v_arr[i]->color) {
//                         // Shade alpha is 100% for fog
//                         buf_vbo[buf_vbo_len++] = 1.f;
//                     } else {
//                         buf_vbo[buf_vbo_len++] = GFX_ALPHA_CONV(color->a);
//                     }
//                 }
//             }
// #else
//                 if (k == 0) {
//                     // only halve colors if we're going full modulate
//                     if (solid_texture) {
//                         out.r = GFX_COLOR_CONV(color->r);
//                         out.g = GFX_COLOR_CONV(color->g);
//                         out.b = GFX_COLOR_CONV(color->b);
//                     } else {
//                         out.r = color->r;
//                         out.g = color->g;
//                         out.b = color->b;
//                     }
//                 } else {
//                     if (use_fog && color == &v_arr[i]->color)
//                         // Shade alpha is 100% for fog
//                         out.a = GFX_ALPHA_ONE;
//                     else
//                         out.a = GFX_ALPHA_CONV(color->a);
//                 }
//             }
//             ((uint32_t *)buf_vbo)[buf_vbo_len++] = out.rgba;
// #endif
//         }
//         /*union RGBA *color = &v_arr[i]->color;
//         buf_vbo[buf_vbo_len++] = color->r / 255.0f;
//         buf_vbo[buf_vbo_len++] = color->g / 255.0f;
//         buf_vbo[buf_vbo_len++] = color->b / 255.0f;
//         buf_vbo[buf_vbo_len++] = color->a / 255.0f;*/
//     }
//     if (++buf_vbo_num_tris == MAX_BUFFERED) {
//         render_flush();
//     }
}


void render_textures_reset(uint16_t len) {
	error_if(len > textures_len, "Invalid texture reset len %d >= %d", len, textures_len);
	render_flush();
	textures_len = len;

	if (len == 0) {
		rgba_t white_pixels[4] = {
			rgba(128,128,128,255), rgba(128,128,128,255),
			rgba(128,128,128,255), rgba(128,128,128,255)
		};
		RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);
        gfx_ps2_select_texture(0, RENDER_NO_TEXTURE);
		return;
	}
}

void render_textures_dump(const char *path) {}

void render_set_screen_size(vec2i_t size) {
	screen_size = size;

// 	// float aspect = (float)size.x / (float)size.y;
// 	// float fov = (73.75 / 180.0) * 3.14159265358;
// 	// float f = 1.0 / tan(fov / 2);
// 	// float nf = 1.0 / (NEAR_PLANE - FAR_PLANE);
// 	// projection_mat = mat4(
// 	// 	f / aspect, 0, 0, 0,
// 	// 	0, f, 0, 0, 
// 	// 	0, 0, (FAR_PLANE + NEAR_PLANE) * nf, -1, 
// 	// 	0, 0, 2 * FAR_PLANE * NEAR_PLANE * nf, 0
// 	// );
}

void render_set_resolution(render_resolution_t res) {}
void render_set_post_effect(render_post_effect_t post) {}

void render_set_blend_mode(render_blend_mode_t mode) {
	if (mode == blend_mode)
        return;

    render_flush();

    blend_mode = mode;

    switch (blend_mode) {
        case RENDER_BLEND_NORMAL:
            draw_set_blendmode(BMODE_BLEND);
            gfx_ps2_set_use_alpha(true);
            break;

        case RENDER_BLEND_LIGHTER:
            draw_set_blendmode(BMODE_ADD);
            gfx_ps2_set_use_alpha(true);
            break;

        default:
            gfx_ps2_set_use_alpha(false);
            break;
    }
}


void render_set_view_2d(void) {
	render_flush();
	render_set_depth_test(false);
	render_set_depth_write(false);
	render_set_model_mat(&mat4_identity());
	// glUniform3f(prg_game->uniform.camera_pos, 0, 0, 0);
	// glUniformMatrix4fv(prg_game->uniform.view, 1, false, mat4_identity().m);
	// glUniformMatrix4fv(prg_game->uniform.projection, 1, false, projection_mat_2d.m);
	// float near = -1;
	// float far = 1;
	// float left = 0;
	// float right = screen_size.x;
	// float bottom = screen_size.y;
	// float top = 0;
	// float lr = 1 / (left - right);
	// float bt = 1 / (bottom - top);
	// float nf = 1 / (near - far);
	// mvp_mat = mat4(
	// 	-2 * lr,  0,  0,  0,
	// 	0,  -2 * bt,  0,  0,
	// 	0,        0,  2 * nf,    0, 
	// 	(left + right) * lr, (top + bottom) * bt, (far + near) * nf, 1
	// );
}


void render_set_view(vec3_t pos, vec3_t angles) {
    render_flush();
	view_mat = mat4_identity();
	mat4_set_translation(&view_mat, vec3(0, 0, 0));
	mat4_set_roll_pitch_yaw(&view_mat, vec3(angles.x, -angles.y + M_PI, angles.z + M_PI));
	mat4_translate(&view_mat, vec3_inv(pos));
	mat4_set_yaw_pitch_roll(&sprite_mat, vec3(-angles.x, angles.y - M_PI, 0));

	render_set_model_mat(&mat4_identity());
}

void render_set_model_mat(mat4_t *m) {
    render_flush();
	mat4_t vm_mat;
	mat4_mul(&vm_mat, &view_mat, m);
	mat4_mul(&mvp_mat, &projection_mat, &vm_mat);
}


vec3_t render_transform(vec3_t pos) {
	return vec3_transform(vec3_transform(pos, &view_mat), &projection_mat);
}

void render_set_cull_backface(bool enabled) {
	render_flush();
	cull_backface = enabled;
}

vec2i_t render_size(void) {
	return screen_size;
}

void render_frame_prepare(void) {
	draw_clear(c_white);
    gfx_ps2_set_viewport(0, 0, screen_size.x, screen_size.y);
	gfx_ps2_set_depth_test(true);
	gfx_ps2_set_depth_mask(true);
}

void render_frame_end(void) {
	render_flush();
    gfx_ps2_set_viewport(0, 0, screen_size.x, screen_size.y);
}
