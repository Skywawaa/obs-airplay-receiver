#pragma once

#include <obs-module.h>

#define ap_log(level, fmt, ...) \
	blog(level, "[AirPlay] " fmt, ##__VA_ARGS__)

#define ap_info(fmt, ...)  ap_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define ap_warn(fmt, ...)  ap_log(LOG_WARNING, fmt, ##__VA_ARGS__)
#define ap_error(fmt, ...) ap_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define ap_debug(fmt, ...) ap_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
