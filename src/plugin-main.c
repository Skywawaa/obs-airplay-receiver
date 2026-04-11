/*
 * OBS AirPlay Receiver Plugin
 * Receives AirPlay screen mirroring from Apple devices as an OBS source.
 */

#include <obs-module.h>
#include "airplay-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-airplay-receiver", "en-US")

bool obs_module_load(void)
{
	airplay_source_register();
	blog(LOG_INFO, "[AirPlay] Plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[AirPlay] Plugin unloaded");
}

const char *obs_module_name(void)
{
	return "OBS AirPlay Receiver";
}

const char *obs_module_description(void)
{
	return "Receive AirPlay screen mirroring from Apple devices";
}
