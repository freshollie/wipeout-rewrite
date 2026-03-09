// #include "SDL.h"
#include <stdlib.h>
#include <ctype.h>
#include <tamtypes.h>
#include <kernel.h>
#include <iopcontrol.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <ps2_filesystem_driver.h>

#define PS2_MEMCARD_IMPLEMENTATION
#include <ps2_memcard.h>
#include <stdio.h>
#include <kernel.h>
#include <ps2_audio_driver.h>
#include <gsKit.h>

#include "audsrv.h"


#include "platform.h"
#include "input.h"
#include "system.h"
#include "utils.h"
#include "mem.h"


#define ARRAY_COUNT(arr) (s32)(sizeof(arr) / sizeof(arr[0]))

static uint64_t perf_freq = 0;
static bool wants_to_exit = false;
// static SDL_Window *window;
// static SDL_AudioDeviceID audio_device;
// static SDL_GameController *gamepad;
static void (*audio_callback)(int16_t *buffer, uint32_t len) = NULL;
static char *path_assets = "";
static char *path_userdata = "";
static char *temp_path = NULL;

#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528

static bool audio_ps2_init(void) {
    if (init_audio_driver() != 0) return false;
    audsrv_set_volume(MAX_VOLUME);

    audsrv_fmt_t fmt;

    fmt.freq = 44100;
    fmt.bits = 16;
    fmt.channels = 2;

    if (audsrv_set_format(&fmt)) {
        printf("audio_ps2: unsupported sound format\n");
        audsrv_quit();
        return false;
    }

    return true;
}

static int audio_ps2_buffered(void) {
    return audsrv_queued() / 4;
}

static int audio_ps2_get_desired_buffered(void) {
    return 1100;
}

static void audio_ps2_play(const uint8_t *buf, size_t len) {
    if (audio_ps2_buffered() < 6000) {
        audsrv_play_audio(buf, len);
    }
}

static void audio_ps2_pause(const uint8_t *buf, size_t len) {
    audsrv_stop_audio();
}

struct VidMode {
    const char *name;
    s16 mode;
    s16 interlace;
    s16 field;
    int max_width;
    int max_height;
    int width;
    int height;
    int vck;
    int iPassCount;
    int x_off;
    int y_off;
};

static const struct VidMode vid_modes[] = {
    { "240p", GS_MODE_NTSC,      GS_NONINTERLACED, GS_FRAME,  652,  224,  320,  224, 2, 1, 0, 0 },
#if !defined(VERSION_EU)
    // NTSC
    { "480i", GS_MODE_NTSC,      GS_INTERLACED,    GS_FIELD,  704,  480,  704,  452, 4, 1, 0, 0 },
    { "480p", GS_MODE_DTV_480P,  GS_NONINTERLACED, GS_FRAME,  704,  480,  704,  452, 2, 1, 0, 0 },
#else
    // PAL
    { "576i", GS_MODE_PAL,       GS_INTERLACED,    GS_FIELD,  704,  576,  704,  536, 4, 1, 0, 0 },
    { "576p", GS_MODE_DTV_576P,  GS_NONINTERLACED, GS_FRAME,  704,  576,  704,  536, 2, 1, 0, 0 },
#endif
    // HDTV
    { "720p", GS_MODE_DTV_720P,  GS_NONINTERLACED, GS_FRAME, 1280,  720, 1280,  720, 1, 2, 0, 0 },
    {"1080i", GS_MODE_DTV_1080I, GS_INTERLACED,    GS_FRAME, 1920, 1080, 1920, 1080, 1, 2, 0, 0 },
};

GSGLOBAL *gs_global;

static int vsync_sema_1st_id;
static int vsync_sema_2nd_id;
static int vsync_sema_id = -1;
static int vsync_id = -1;

static const struct VidMode *vid_mode;
static bool use_hires = false;

/* Copy of gsKit_sync_flip, but without the 'flip' */
static void gsKit_sync(GSGLOBAL *gsGlobal)
{
    WaitSema(vsync_sema_1st_id);
    WaitSema(vsync_sema_2nd_id);
}

/* Copy of gsKit_sync_flip, but without the 'sync' */
static void gsKit_flip(GSGLOBAL *gsGlobal)
{
   if (!gsGlobal->FirstFrame)
   {
      if (gsGlobal->DoubleBuffering == GS_SETTING_ON)
      {
         GS_SET_DISPFB2( gsGlobal->ScreenBuffer[
               gsGlobal->ActiveBuffer & 1] / 8192,
               gsGlobal->Width / 64, gsGlobal->PSM, 0, 0 );

         gsGlobal->ActiveBuffer ^= 1;
      }

   }

   gsKit_setactive(gsGlobal);
}

/* PRIVATE METHODS */
static int vsync_handler()
{
   iSignalSema(vsync_sema_id ? vsync_sema_2nd_id : vsync_sema_1st_id);
   vsync_sema_id ^= 1;

   ExitHandler();
   return 0;
}

static void prepare_sema() {
    ee_sema_t sema_1st;
    sema_1st.init_count = 0;
    sema_1st.max_count = 1;
    sema_1st.option = 0;
    vsync_sema_1st_id = CreateSema(&sema_1st);

    ee_sema_t sema_2nd;
    sema_2nd.init_count = 0;
    sema_2nd.max_count = 1;
    sema_2nd.option = 0;
    vsync_sema_2nd_id = CreateSema(&sema_2nd);
}

static void gfx_ps2_init(const char *game_name, bool start_in_fullscreen) {
    
}

static bool gfx_ps2_set_vid_mode(uint8_t vid_mode_idx) {
    if (vid_mode_idx >= ARRAY_COUNT(vid_modes)) {
        return false;
    }

    if (vid_mode != &vid_modes[vid_mode_idx]) {
        vid_mode = &vid_modes[vid_mode_idx];
        gfx_ps2_init(NULL, false);
        return true;
    }
    return false;
}

static void gfx_ps2_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {

}

static void gfx_ps2_set_fullscreen(bool enable) {

}

static void gfx_ps2_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode), void (*on_all_keys_up)(void)) {

}

static void gfx_ps2_main_loop(void (*run_one_game_iter)(void)) {
    run_one_game_iter();
}

static void gfx_ps2_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = gs_global->Width;
    *height = gs_global->Height;
    // the game doesn't need to know that we are 
    // rendering at half height for 1080i
    if (gs_global->Mode == GS_MODE_DTV_1080I) {
        *height *= 2;
    }
}

static void gfx_ps2_handle_events(void) {

}

static bool gfx_ps2_start_frame(void) {
    return 1;
}

static void gfx_ps2_swap_buffers_begin(void) {
    if (use_hires) {
        return;
    }
    if (vsync_sema_id != -1) return;

    prepare_sema();
    vsync_sema_id = 0;
    vsync_id = gsKit_add_vsync_handler(vsync_handler);
}

static double gfx_ps2_get_time(void) {
    return 0.0;
}


// uint8_t platform_sdl_gamepad_map[] = {
// 	[SDL_CONTROLLER_BUTTON_A] = INPUT_GAMEPAD_A,
// 	[SDL_CONTROLLER_BUTTON_B] = INPUT_GAMEPAD_B,
// 	[SDL_CONTROLLER_BUTTON_X] = INPUT_GAMEPAD_X,
// 	[SDL_CONTROLLER_BUTTON_Y] = INPUT_GAMEPAD_Y,
// 	[SDL_CONTROLLER_BUTTON_BACK] = INPUT_GAMEPAD_SELECT,
// 	[SDL_CONTROLLER_BUTTON_GUIDE] = INPUT_GAMEPAD_HOME,
// 	[SDL_CONTROLLER_BUTTON_START] = INPUT_GAMEPAD_START,
// 	[SDL_CONTROLLER_BUTTON_LEFTSTICK] = INPUT_GAMEPAD_L_STICK_PRESS,
// 	[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = INPUT_GAMEPAD_R_STICK_PRESS,
// 	[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = INPUT_GAMEPAD_L_SHOULDER,
// 	[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = INPUT_GAMEPAD_R_SHOULDER,
// 	[SDL_CONTROLLER_BUTTON_DPAD_UP] = INPUT_GAMEPAD_DPAD_UP,
// 	[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = INPUT_GAMEPAD_DPAD_DOWN,
// 	[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = INPUT_GAMEPAD_DPAD_LEFT,
// 	[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = INPUT_GAMEPAD_DPAD_RIGHT,
// 	[SDL_CONTROLLER_BUTTON_MAX] = INPUT_INVALID
// };


// uint8_t platform_sdl_axis_map[] = {
// 	[SDL_CONTROLLER_AXIS_LEFTX] = INPUT_GAMEPAD_L_STICK_LEFT,
// 	[SDL_CONTROLLER_AXIS_LEFTY] = INPUT_GAMEPAD_L_STICK_UP,
// 	[SDL_CONTROLLER_AXIS_RIGHTX] = INPUT_GAMEPAD_R_STICK_LEFT,
// 	[SDL_CONTROLLER_AXIS_RIGHTY] = INPUT_GAMEPAD_R_STICK_UP,
// 	[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = INPUT_GAMEPAD_L_TRIGGER,
// 	[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = INPUT_GAMEPAD_R_TRIGGER,
// 	[SDL_CONTROLLER_AXIS_MAX] = INPUT_INVALID
// };


void platform_exit(void) {
	wants_to_exit = true;
}

#include <ps2_joystick_driver.h>
#include <libpad.h>
#include <libmtap.h>


#define DEADZONE    24
#define DEADZONE_SQ (DEADZONE * DEADZONE)

static u8 padbuf[256] __attribute__((aligned(64)));
static int init_done = 0;

static int joy_port = -1;
static int joy_slot = -1;
static int joy_id = -1;
static struct padButtonStatus joy_buttons __attribute__((aligned(64)));

static inline int wait_pad(int tries) {
    int state = padGetState(joy_port, joy_slot);
    if (state == PAD_STATE_DISCONN) {
        joy_id = -1;
        return -1;
    }

    while ((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1)) {
        state = padGetState(joy_port, joy_slot);
        if (--tries == 0) break;
    }

    return 0;
}

static int detect_pad(void) {
    int id = padInfoMode(joy_port, joy_slot, PAD_MODECURID, 0);
    if (id <= 0) return -1;

    const int ext = padInfoMode(joy_port, joy_slot, PAD_MODECUREXID, 0);
    if (ext) id = ext;

    printf("controller_ps2: detected pad type %d\n", id);

    if (id == PAD_TYPE_DIGITAL || id == PAD_TYPE_DUALSHOCK)
        padSetMainMode(joy_port, joy_slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);

    return id;
}

static void controller_ps2_init(void) {
    int ret = -1;
    
    // MEMORY CARD already initied SIO2MAN
    ret = init_joystick_driver(false);

    if (ret != 0) {
        printf("controller_ps2: failed to init joystick driver: %d\n", ret);
        return;
    }

    const int numports = padGetPortMax();
    // Find the first device connected
    for (int port = 0; port < numports && joy_port < 0; ++port) {
        if (joy_port == -1 && joy_slot == -1 && mtapPortOpen(port)) {
            const int maxslots = padGetSlotMax(port);
            for (int slot = 0; slot < maxslots; ++slot) {
                if (joy_port == -1 && joy_slot == -1 && padPortOpen(port, slot, padbuf) >= 0) {
                    joy_port = port;
                    joy_slot = slot;
                    printf("controller_ps2: using pad (%d, %d)\n", port, slot);
                    break;
                }
            }
        }
    }

    if (joy_slot < 0 || joy_port < 0) {
        printf("controller_ps2: could not open a single port\n");
        return;
    }

    init_done = 1;
}

// static void controller_ps2_read(OSContPad *pad) {
//     if (!init_done) return;

//     if (wait_pad(10) < 0)
//         return; // nothing received

//     if (joy_id < 0) {
//         // pad not detected yet, do it
//         joy_id = detect_pad();
//         if (joy_id < 0) return; // still nothing
//         if (wait_pad(10) < 0) return;
//     }

//     if (padRead(joy_port, joy_slot, &joy_buttons)) {
//         const u32 btns = 0xffff ^ joy_buttons.btns;

//         for (int i = 0; i < num_joy_binds; ++i)
//             if (btns & joy_binds[i].sce_btn)
//                 pad->button |= joy_binds[i].n64_btn;

//         const int lstick_x = (int)joy_buttons.ljoy_h - 128;
//         const int lstick_y = (int)joy_buttons.ljoy_v - 128;
//         const int rstick_x = (int)joy_buttons.rjoy_h - 128;
//         const int rstick_y = (int)joy_buttons.rjoy_v - 128;

//         if (rstick_x < -64)     pad->button |= L_CBUTTONS;
//         else if (rstick_x > 63) pad->button |= R_CBUTTONS;
//         if (rstick_y < -64)     pad->button |= U_CBUTTONS;
//         else if (rstick_y > 63) pad->button |= D_CBUTTONS;

//         const uint32_t lstick_mag = (u32)(lstick_x * lstick_x) + (u32)(lstick_y * lstick_y);
//         if (lstick_mag > (u32)DEADZONE_SQ) {
//             pad->stick_x = roundf(((float) lstick_x) / 128.f * 80.f);
//             pad->stick_y = roundf(((float)-lstick_y) / 128.f * 80.f);
//         }
//     }
// }

int frames = 0;

void platform_pump_events(void) {
    frames++;
    if (frames < 10) {
        input_set_button_state(INPUT_KEY_RETURN, 1);
        input_set_button_state(INPUT_KEY_RETURN, 0);
    }

    if (frames > 10) {
         input_set_button_state(INPUT_GAMEPAD_A, 1);
    }
    // handle controller input
	// SDL_Event ev;
	// while (SDL_PollEvent(&ev)) {
	// 	// Detect ALT+Enter press to toggle fullscreen
	// 	if (
	// 		ev.type == SDL_KEYDOWN && 
	// 		ev.key.keysym.scancode == SDL_SCANCODE_RETURN &&
	// 		(ev.key.keysym.mod & (KMOD_LALT | KMOD_RALT))
	// 	) {
	// 		platform_set_fullscreen(!platform_get_fullscreen());
	// 	}

	// 	// Input Keyboard
	// 	else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
	// 		int code = ev.key.keysym.scancode;
	// 		float state = ev.type == SDL_KEYDOWN ? 1.0 : 0.0;
	// 		if (code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RALT) {
	// 			int code_internal = code - SDL_SCANCODE_LCTRL + INPUT_KEY_LCTRL;
	// 			input_set_button_state(code_internal, state);
	// 		}
	// 		else if (code > 0 && code < INPUT_KEY_MAX) {
	// 			input_set_button_state(code, state);
	// 		}
	// 	}

	// 	else if (ev.type == SDL_TEXTINPUT) {
	// 		input_textinput(ev.text.text[0]);
	// 	}

	// 	// Gamepads connect/disconnect
	// 	else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
	// 		gamepad = SDL_GameControllerOpen(ev.cdevice.which);
	// 	}
	// 	else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
	// 		if (gamepad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad))) {
	// 			SDL_GameControllerClose(gamepad);
	// 			gamepad = platform_find_gamepad();
	// 		}
	// 	}

	// 	// Input Gamepad Buttons
	// 	else if (
	// 		ev.type == SDL_CONTROLLERBUTTONDOWN || 
	// 		ev.type == SDL_CONTROLLERBUTTONUP
	// 	) {
	// 		if (ev.cbutton.button < SDL_CONTROLLER_BUTTON_MAX) {
	// 			button_t button = platform_sdl_gamepad_map[ev.cbutton.button];
	// 			if (button != INPUT_INVALID) {
	// 				float state = ev.type == SDL_CONTROLLERBUTTONDOWN ? 1.0 : 0.0;
	// 				input_set_button_state(button, state);
	// 			}
	// 		}
	// 	}

	// 	// Input Gamepad Axis
	// 	else if (ev.type == SDL_CONTROLLERAXISMOTION) {
	// 		float state = (float)ev.caxis.value / 32767.0;

	// 		if (ev.caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
	// 			int code = platform_sdl_axis_map[ev.caxis.axis];
	// 			if (
	// 				code == INPUT_GAMEPAD_L_TRIGGER || 
	// 				code == INPUT_GAMEPAD_R_TRIGGER
	// 			) {
	// 				input_set_button_state(code, state);
	// 			}
	// 			else if (state > 0) {
	// 				input_set_button_state(code, 0.0);
	// 				input_set_button_state(code+1, state);
	// 			}
	// 			else {
	// 				input_set_button_state(code, -state);
	// 				input_set_button_state(code+1, 0.0);
	// 			}
	// 		}
	// 	}

	// 	// Mouse buttons
	// 	else if (
	// 		ev.type == SDL_MOUSEBUTTONDOWN ||
	// 		ev.type == SDL_MOUSEBUTTONUP
	// 	) {
	// 		button_t button = INPUT_BUTTON_NONE;
	// 		switch (ev.button.button) {
	// 			case SDL_BUTTON_LEFT: button = INPUT_MOUSE_LEFT; break;
	// 			case SDL_BUTTON_MIDDLE: button = INPUT_MOUSE_MIDDLE; break;
	// 			case SDL_BUTTON_RIGHT: button = INPUT_MOUSE_RIGHT; break;
	// 			default: break;
	// 		}
	// 		if (button != INPUT_BUTTON_NONE) {
	// 			float state = ev.type == SDL_MOUSEBUTTONDOWN ? 1.0 : 0.0;
	// 			input_set_button_state(button, state);
	// 		}
	// 	}

	// 	// Mouse wheel
	// 	else if (ev.type == SDL_MOUSEWHEEL) {
	// 		button_t button = ev.wheel.y > 0 
	// 			? INPUT_MOUSE_WHEEL_UP
	// 			: INPUT_MOUSE_WHEEL_DOWN;
	// 		input_set_button_state(button, 1.0);
	// 		input_set_button_state(button, 0.0);
	// 	}

	// 	// Mouse move
	// 	else if (ev.type == SDL_MOUSEMOTION) {
	// 		input_set_mouse_pos(ev.motion.x, ev.motion.y);
	// 	}

	// 	// Window Events
	// 	if (ev.type == SDL_QUIT) {
	// 		wants_to_exit = true;
	// 	}
	// 	else if (
	// 		ev.type == SDL_WINDOWEVENT &&
	// 		(
	// 			ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
	// 			ev.window.event == SDL_WINDOWEVENT_RESIZED
	// 		)
	// 	) {
	// 		system_resize(platform_screen_size());
	// 	}
	// }
}

static inline u32 get_cycle_count(void)
{
    u32 count;
    asm volatile("mfc0 %0, $9" : "=r"(count));
    return count;
}

double count = 0;
double platform_now(void)
{
	// TODO: this might be AI nonsense
    // return (double)get_cycle_count() / 147456000.0;
    return count += 0.016666666666666666;
}

bool platform_get_fullscreen(void) {
    return true;
	// return SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
}

void platform_set_fullscreen(bool fullscreen) {
	// if (fullscreen) {
	// 	int32_t display = SDL_GetWindowDisplayIndex(window);
		
	// 	SDL_DisplayMode mode;
	// 	SDL_GetDesktopDisplayMode(display, &mode);
	// 	SDL_SetWindowDisplayMode(window, &mode);
	// 	SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
	// 	SDL_ShowCursor(SDL_DISABLE);
	// }
	// else {
	// 	SDL_SetWindowFullscreen(window, 0);
	// 	SDL_ShowCursor(SDL_ENABLE);
	// }
}

void platform_audio_callback(int16_t* buffer, int num_frames, int num_channels) {
	if (audio_callback) {
		audio_callback(buffer, num_frames * num_channels);
	}
	else {
		memset(buffer, 0, num_frames * sizeof(float));
	}
}

void platform_set_audio_mix_cb(void (*cb)(int16_t *buffer, uint32_t len)) {
	audio_callback = cb;
	audsrv_stop_audio();
}

void upper(char *s)
{
    while(*s) {
        *s++ = toupper((unsigned char)*s);
    }
}

FILE *platform_open_asset(const char *name, const char *mode) {
    upper(name);
	char *path = strcat(strcpy(temp_path, path_assets), name);
    // printf("Before: %s\n", path);
    // fix_slashes(path);
    // printf("After: %s\n", path);
	return fopen(path, mode);
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
    upper(name);
	char *path = strcat(strcpy(temp_path, path_assets), name);
    // printf("Before: %s\n", path);
    // fix_slashes(path);
    // printf("Open: %s\n", path);
	return file_load(path, bytes_read);
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
	// char *path = strcat(strcpy(temp_path, path_userdata), name);
	// if (!file_exists(path)) {
	// 	*bytes_read = 0;
	// 	return NULL;
	// }
	// return file_load(path, bytes_read);
    return NULL;
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
	// char *path = strcat(strcpy(temp_path, path_userdata), name);
	// return file_store(path, bytes, len);
}

	
void platform_video_init(void) {
	if (vid_mode == NULL) {
        vid_mode = &vid_modes[1]; // Standard def 480i
    } else {
        if (use_hires) {
            gsKit_hires_deinit_global(gs_global);
        } else {
            gsKit_deinit_global(gs_global);
            if (vsync_id != -1) {
                gsKit_remove_vsync_handler(vsync_id);
            }
            vsync_sema_id = -1;
        }
    }
    use_hires = (vid_mode->mode == GS_MODE_DTV_720P || vid_mode->mode == GS_MODE_DTV_1080I);

    if (use_hires) {
        gs_global = gsKit_hires_init_global();
    } else {
        gs_global = gsKit_init_global();
    }

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);

    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gs_global->Mode = vid_mode->mode;
    gs_global->Width = vid_mode->width;
    gs_global->Height = vid_mode->height;
    if (gs_global->Mode == GS_MODE_DTV_1080I) {
        gs_global->Height /= 2;
    }

    gs_global->Interlace = vid_mode->interlace;
    gs_global->Field = vid_mode->field;
    gs_global->ZBuffering = GS_SETTING_ON;
    gs_global->DoubleBuffering = GS_SETTING_ON;
    gs_global->PrimAAEnable = GS_SETTING_OFF;
    // this could be enabled for hires, but I don't like it
    gs_global->Dithering = use_hires ? GS_SETTING_ON : GS_SETTING_OFF;
    // hires runs out of VRAM if using more than 16bit color
    gs_global->PSM = use_hires ? GS_PSM_CT16 : GS_PSM_CT32;
    gs_global->PSMZ = GS_PSMZ_16; // 16-bit unsigned zbuffer

    if (use_hires) {
        gsKit_hires_init_screen(gs_global, vid_mode->iPassCount);
    } else {
        gsKit_init_screen(gs_global);
    }
    // hires sets the texture pointer to the wrong location. Ensure it's correct.
    gs_global->TexturePointer = gs_global->CurrentPointer;
    gsKit_TexManager_init(gs_global);
}

void platform_prepare_frame(void) {
	// nothing
}

void platform_video_cleanup(void) {
	// SDL_GL_DeleteContext(platform_gl);
}

void platform_end_frame(void) {
	if (use_hires) {
        gsKit_hires_flip_ext(gs_global, GSFLIP_RATE_LIMIT_1);
    } else {
        // gsKit_flip(gs_global);
        gsKit_sync_flip(gs_global);
        gsKit_queue_exec(gs_global);
    }
    gsKit_TexManager_nextFrame(gs_global);
}

vec2i_t platform_screen_size(void) {
	uint32_t width, height;
	gfx_ps2_get_dimensions(&width, &height);
	return vec2i(width, height);
}

void reset_IOP() {
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {} // Comment this line if you want to "debug" through ps2link
    while (!SifIopSync()) {} 
}

static void prepare_IOP() {
    reset_IOP();
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
}

static void init_drivers() {
    init_only_boot_ps2_filesystem_driver();
    init_memcard_driver(true);
    ps2_memcard_init();
}

static void deinit_drivers() {
    deinit_memcard_driver(true);
    deinit_only_boot_ps2_filesystem_driver();
}

static inline void audio_frame(void) {
    static float float_buf[735 * 2];
    static int16_t pcm_buf[735 * 2];
    int num_samples = 735;

    platform_audio_callback(pcm_buf, num_samples, 2);

    // Send PCM16 to SPU2
    audio_ps2_play(
        (const uint8_t*)pcm_buf,
        num_samples * 2 * sizeof(int16_t)
    );
}


int main(int argc, char *argv[]) {
	// static u64 pool[0x165000/8 / 4 * sizeof(void *)];
    char path_data[200];
    temp_path = path_data;

#ifdef TARGET_PS2
    prepare_IOP();
    init_drivers();
#endif


    // printf("WTf?\n");
	audio_ps2_init();
	platform_video_init();
    controller_ps2_init();
	path_assets = "host:";
    // printf("WTf?\n");
    // while(true) {};

    system_init();
	while (!wants_to_exit) {
		platform_pump_events();
		platform_prepare_frame();
		system_update();
        // audio breaks it right now, oops
        // audio_frame();
        gfx_ps2_swap_buffers_begin();
		platform_end_frame();
	}

    deinit_drivers();
}
