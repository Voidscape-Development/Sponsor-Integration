/*
Sponsor Integration
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "ad-playlist-source.h"
#include "picture-gallery-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("SponsorIntegration.Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("SponsorIntegration.Name");
}

bool obs_module_load(void)
{
	obs_register_source(&ad_playlist_source_info);
	obs_register_source(&picture_gallery_source_info);
	picture_gallery_module_load();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	picture_gallery_module_unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
