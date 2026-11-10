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

/* Helpers shared by the sources in this plugin. */

/* Whether `ext` (".png", including the dot) is a still or animated image the
 * OBS image source can load. */
bool si_is_image_extension(const char *ext);

/* Turns an alignment setting value ("center", "topleft", "bottom", ...) into
 * OBS_ALIGN_* flags. Anything unrecognised is centered. */
uint32_t si_alignment_from_string(const char *val);

#ifdef __cplusplus
}
#endif
