// #include "SDL.h"

#include "platform.h"
#include "input.h"
#include "system.h"
#include "utils.h"
#include "mem.h"

static uint64_t perf_freq = 0;
static bool wants_to_exit = false;
// static SDL_Window *window;
// static SDL_AudioDeviceID audio_device;
// static SDL_GameController *gamepad;
static void (*audio_callback)(float *buffer, uint32_t len) = NULL;
static char *path_assets = "";
static char *path_userdata = "";
static char *temp_path = NULL;


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

void platform_pump_events(void) {
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

double platform_now(void) {
	// uint64_t perf_counter = SDL_GetPerformanceCounter();
	// return (double)perf_counter / (double)perf_freq;
}

bool platform_get_fullscreen(void) {
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

void platform_audio_callback(void* userdata, uint8_t* stream, int len) {
	if (audio_callback) {
		audio_callback((float *)stream, len/sizeof(float));
	}
	else {
		memset(stream, 0, len);
	}
}

void platform_set_audio_mix_cb(void (*cb)(float *buffer, uint32_t len)) {
	audio_callback = cb;
	// SDL_PauseAudioDevice(audio_device, 0);
}


FILE *platform_open_asset(const char *name, const char *mode) {
	char *path = strcat(strcpy(temp_path, path_assets), name);
	return fopen(path, mode);
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
	char *path = strcat(strcpy(temp_path, path_assets), name);
	return file_load(path, bytes_read);
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
	char *path = strcat(strcpy(temp_path, path_userdata), name);
	if (!file_exists(path)) {
		*bytes_read = 0;
		return NULL;
	}
	return file_load(path, bytes_read);
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
	char *path = strcat(strcpy(temp_path, path_userdata), name);
	return file_store(path, bytes, len);
}

	
void platform_video_init(void) {
	// #if defined(USE_GLES2)
	// 	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	// 	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	// 	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	// #endif

	// platform_gl = SDL_GL_CreateContext(window);
	// SDL_GL_SetSwapInterval(1);
}

void platform_prepare_frame(void) {
	// nothing
}

void platform_video_cleanup(void) {
	// SDL_GL_DeleteContext(platform_gl);
}

void platform_end_frame(void) {
	// SDL_GL_SwapWindow(window);
}

vec2i_t platform_screen_size(void) {
	int width, height;
	// SDL_GL_GetDrawableSize(window, &width, &height);
	return vec2i(width, height);
}

int main(int argc, char *argv[]) {
	return 0;
}
