#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "utils.h"
#include "mem.h"

#include "trig_tables.inc.c"

char temp_path[64];
char *get_path(const char *dir, const char *file) {
	strcpy(temp_path, dir);
	strcpy(temp_path + strlen(dir), file);
	return temp_path;
}


bool file_exists(const char *path) {
	struct stat s;
	return (stat(path, &s) == 0);
}

uint8_t *file_load(const char *path, uint32_t *bytes_read) {
	FILE *f = fopen(path, "rb");
	error_if(!f, "Could not open file for reading: %s", path);

	fseek(f, 0, SEEK_END);
	int32_t size = ftell(f);
	if (size <= 0) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);

	uint8_t *bytes = mem_temp_alloc(size);
	if (!bytes) {
		fclose(f);
		return NULL;
	}

	*bytes_read = fread(bytes, 1, size, f);
	fclose(f);
	
	error_if(*bytes_read != size, "Could not read file: %s", path);
	return bytes;
}

uint32_t file_store(const char *path, void *bytes, int32_t len) {
	FILE *f = fopen(path, "wb");
	error_if(!f, "Could not open file for writing: %s", path);

	if (fwrite(bytes, 1, len, f) != len) {
		die("Could not write file file %s", path);
	}
	
	fclose(f);
	return len;
}

bool str_starts_with(const char *haystack, const char *needle) {
	return (strncmp(haystack, needle, strlen(needle)) == 0);
}

float rand_float(float min, float max) {
	return min + ((float)rand() / (float)RAND_MAX) * (max - min);
}

int32_t rand_int(int32_t min, int32_t max) {
	return min + rand() % (max - min);
}


/**
 * Helper function for atan2s. Does a look up of the arctangent of y/x assuming
 * the resulting angle is in range [0, 0x2000] (1/8 of a circle).
 */
static uint16_t atan2_lookup(float y, float x) {
    uint16_t ret;

    if (x == 0) {
        ret = gArctanTable[0];
    } else {
        ret = gArctanTable[(int32_t)(y / x * 1024 + 0.5f)];
    }
    return ret;
}

/**
 * Compute the angle from (0, 0) to (x, y) as a s16. Given that terrain is in
 * the xz-plane, this is commonly called with (z, x) to get a yaw angle.
 */
int16_t atan2s(float y, float x) {
    uint16_t ret;

    if (x >= 0) {
        if (y >= 0) {
            if (y >= x) {
                ret = atan2f(x, y);
            } else {
                ret = 0x4000 - atan2f(y, x);
            }
        } else {
            y = -y;
            if (y < x) {
                ret = 0x4000 + atan2f(y, x);
            } else {
                ret = 0x8000 - atan2f(x, y);
            }
        }
    } else {
        x = -x;
        if (y < 0) {
            y = -y;
            if (y >= x) {
                ret = 0x8000 + atan2f(x, y);
            } else {
                ret = 0xC000 - atan2f(y, x);
            }
        } else {
            if (y < x) {
                ret = 0xC000 + atan2f(y, x);
            } else {
                ret = -atan2f(x, y);
            }
        }
    }
    return ret;
}

float atan2f(float y, float x) {
    return (float) atan2s(y, x) * M_PI / 0x8000;
}
