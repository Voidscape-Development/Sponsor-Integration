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

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct obs_source_info picture_gallery_source_info;

/* Hooks the gallery into the OBS frontend so it can use the scene transitions
 * configured there. Call once from obs_module_load / obs_module_unload. */
void picture_gallery_module_load(void);
void picture_gallery_module_unload(void);

#ifdef __cplusplus
}
#endif
