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
#include <util/dstr.h>

#include "source-common.h"

bool si_is_image_extension(const char *ext)
{
	if (!ext)
		return false;

	return astrcmpi(ext, ".bmp") == 0 || astrcmpi(ext, ".tga") == 0 || astrcmpi(ext, ".png") == 0 ||
	       astrcmpi(ext, ".jpeg") == 0 || astrcmpi(ext, ".jpg") == 0 || astrcmpi(ext, ".jpe") == 0 ||
	       astrcmpi(ext, ".jfif") == 0 || astrcmpi(ext, ".gif") == 0 || astrcmpi(ext, ".webp") == 0 ||
	       astrcmpi(ext, ".psd") == 0 || astrcmpi(ext, ".tif") == 0 || astrcmpi(ext, ".tiff") == 0
#ifdef _WIN32
	       || astrcmpi(ext, ".jxr") == 0
#endif
		;
}

uint32_t si_alignment_from_string(const char *val)
{
	if (astrcmpi(val, "topleft") == 0)
		return OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	if (astrcmpi(val, "top") == 0)
		return OBS_ALIGN_TOP;
	if (astrcmpi(val, "topright") == 0)
		return OBS_ALIGN_TOP | OBS_ALIGN_RIGHT;
	if (astrcmpi(val, "left") == 0)
		return OBS_ALIGN_LEFT;
	if (astrcmpi(val, "right") == 0)
		return OBS_ALIGN_RIGHT;
	if (astrcmpi(val, "bottomleft") == 0)
		return OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT;
	if (astrcmpi(val, "bottom") == 0)
		return OBS_ALIGN_BOTTOM;
	if (astrcmpi(val, "bottomright") == 0)
		return OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT;

	return OBS_ALIGN_CENTER;
}
