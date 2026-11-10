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
#include <util/platform.h>
#include <util/dstr.h>

#include <stdlib.h>

#include "playlist-common.h"

#define T_(text) obs_module_text("Common." text)

/* ------------------------------------------------------------------------- */
/* files                                                                      */

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

bool si_is_media_extension(const char *ext)
{
	if (!ext)
		return false;

	return astrcmpi(ext, ".mp4") == 0 || astrcmpi(ext, ".m4v") == 0 || astrcmpi(ext, ".mov") == 0 ||
	       astrcmpi(ext, ".mkv") == 0 || astrcmpi(ext, ".webm") == 0 || astrcmpi(ext, ".ts") == 0 ||
	       astrcmpi(ext, ".m2ts") == 0 || astrcmpi(ext, ".mts") == 0 || astrcmpi(ext, ".mpg") == 0 ||
	       astrcmpi(ext, ".mpeg") == 0 || astrcmpi(ext, ".avi") == 0 || astrcmpi(ext, ".flv") == 0 ||
	       astrcmpi(ext, ".wmv") == 0 || astrcmpi(ext, ".mxf") == 0 || astrcmpi(ext, ".3gp") == 0 ||
	       astrcmpi(ext, ".mp3") == 0 || astrcmpi(ext, ".aac") == 0 || astrcmpi(ext, ".m4a") == 0 ||
	       astrcmpi(ext, ".wav") == 0 || astrcmpi(ext, ".ogg") == 0 || astrcmpi(ext, ".opus") == 0 ||
	       astrcmpi(ext, ".flac") == 0;
}

void si_paths_free(si_path_array_t *paths)
{
	for (size_t i = 0; i < paths->num; i++)
		bfree(paths->array[i]);

	da_free(*paths);
}

void si_paths_read(obs_data_t *settings, const char *key, si_path_array_t *paths)
{
	obs_data_array_t *array = obs_data_get_array(settings, key);
	size_t count = obs_data_array_count(array);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");

		if (path && *path) {
			char *copy = bstrdup(path);
			da_push_back(*paths, &copy);
		}

		obs_data_release(item);
	}

	obs_data_array_release(array);
}

bool si_paths_equal(const si_path_array_t *a, const si_path_array_t *b)
{
	if (a->num != b->num)
		return false;

	for (size_t i = 0; i < a->num; i++) {
		if (strcmp(a->array[i], b->array[i]) != 0)
			return false;
	}

	return true;
}

static int compare_paths(const void *a, const void *b)
{
	return astrcmpi(*(const char *const *)a, *(const char *const *)b);
}

/* Adds the files in `path` that pass `filter`. Returns false if it is not a
 * directory. */
static bool expand_directory(const char *path, si_extension_filter_t filter, const char *log_name,
			     si_path_array_t *files)
{
	si_path_array_t entries;
	struct dstr file_path = {0};
	os_dir_t *dir = os_opendir(path);

	if (!dir)
		return false;

	da_init(entries);

	for (;;) {
		struct os_dirent *ent = os_readdir(dir);
		char *entry;

		if (!ent)
			break;
		if (ent->directory)
			continue;
		if (!filter(os_get_path_extension(ent->d_name)))
			continue;

		entry = bstrdup(ent->d_name);
		da_push_back(entries, &entry);

		if (entries.num >= SI_MAX_DIR_FILES) {
			blog(LOG_WARNING, "[%s] directory '%s' holds more than %d usable files, ignoring the rest",
			     log_name, path, SI_MAX_DIR_FILES);
			break;
		}
	}

	os_closedir(dir);

	/* directory order is filesystem dependent */
	if (entries.num > 1)
		qsort(entries.array, entries.num, sizeof(char *), compare_paths);

	for (size_t i = 0; i < entries.num; i++) {
		dstr_copy(&file_path, path);
		dstr_cat_ch(&file_path, '/');
		dstr_cat(&file_path, entries.array[i]);

		da_push_back(*files, &file_path.array);
		/* the array now owns the string */
		dstr_init(&file_path);
	}

	si_paths_free(&entries);

	return true;
}

void si_paths_expand(const si_path_array_t *configured, si_extension_filter_t filter, const char *log_name,
		     si_path_array_t *files)
{
	for (size_t i = 0; i < configured->num; i++) {
		const char *path = configured->array[i];

		if (!expand_directory(path, filter, log_name, files)) {
			char *copy = bstrdup(path);
			da_push_back(*files, &copy);
		}
	}
}

void si_path_directory(const char *path, struct dstr *dir)
{
	const char *slash;

	dstr_copy(dir, path);
	dstr_replace(dir, "\\", "/");
	slash = strrchr(dir->array, '/');
	if (slash)
		dstr_resize(dir, slash - dir->array + 1);
}

/* ------------------------------------------------------------------------- */
/* running order                                                              */

void si_order_build(si_index_array_t *order, size_t count, bool shuffle)
{
	da_resize(*order, count);

	for (size_t i = 0; i < count; i++)
		order->array[i] = i;

	if (!shuffle || count < 2)
		return;

	for (size_t i = count - 1; i > 0; i--) {
		size_t j = (size_t)(rand() % (int)(i + 1));
		size_t tmp = order->array[i];

		order->array[i] = order->array[j];
		order->array[j] = tmp;
	}
}

void si_order_reshuffle(si_index_array_t *order, size_t count, bool shuffle)
{
	size_t last = order->num ? order->array[order->num - 1] : 0;

	si_order_build(order, count, shuffle);

	if (order->num > 1 && order->array[0] == last) {
		size_t tmp = order->array[0];

		order->array[0] = order->array[order->num - 1];
		order->array[order->num - 1] = tmp;
	}
}

/* ------------------------------------------------------------------------- */
/* layout                                                                     */

#define SCALE_FIT "fit"
#define SCALE_STRETCH "stretch"
#define SCALE_DOWN_ONLY "down_only"

static uint32_t alignment_from_string(const char *val)
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

void si_layout_init(struct si_layout *layout)
{
	memset(layout, 0, sizeof(*layout));
	layout->auto_size = true;
	layout->scale_type = OBS_TRANSITION_SCALE_ASPECT;
	layout->alignment = OBS_ALIGN_CENTER;
}

/* "1920x1080" is a fixed size, "16:9" is a fixed aspect ratio, and anything
 * else - including the localized "Automatic" entry - means automatic. */
static void parse_bounding_size(struct si_layout *layout, const char *val)
{
	int cx = 0;
	int cy = 0;

	layout->auto_size = true;
	layout->aspect_only = false;

	if (!val || !*val)
		return;

	if (sscanf(val, "%dx%d", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		layout->auto_size = false;
	} else if (sscanf(val, "%d:%d", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		layout->auto_size = false;
		layout->aspect_only = true;
	} else {
		return;
	}

	layout->bound_cx = (uint32_t)cx;
	layout->bound_cy = (uint32_t)cy;
}

void si_layout_read(struct si_layout *layout, obs_data_t *settings)
{
	const char *scale_mode = obs_data_get_string(settings, SI_S_SCALE_MODE);

	parse_bounding_size(layout, obs_data_get_string(settings, SI_S_BOUNDING));

	if (astrcmpi(scale_mode, SCALE_STRETCH) == 0)
		layout->scale_type = OBS_TRANSITION_SCALE_STRETCH;
	else if (astrcmpi(scale_mode, SCALE_DOWN_ONLY) == 0)
		layout->scale_type = OBS_TRANSITION_SCALE_MAX_ONLY;
	else
		layout->scale_type = OBS_TRANSITION_SCALE_ASPECT;

	layout->alignment = alignment_from_string(obs_data_get_string(settings, SI_S_ALIGNMENT));
}

void si_layout_size(const struct si_layout *layout, uint32_t content_cx, uint32_t content_cy, uint32_t *cx,
		    uint32_t *cy)
{
	*cx = content_cx;
	*cy = content_cy;

	if (layout->auto_size)
		return;

	if (!layout->aspect_only) {
		*cx = layout->bound_cx;
		*cy = layout->bound_cy;
		return;
	}

	if (content_cx && content_cy) {
		double cx_f = (double)content_cx;
		double cy_f = (double)content_cy;
		double old_aspect = cx_f / cy_f;
		double new_aspect = (double)layout->bound_cx / (double)layout->bound_cy;

		if (new_aspect > old_aspect)
			*cx = (uint32_t)(cy_f * new_aspect);
		else
			*cy = (uint32_t)(cx_f / new_aspect);
	}
}

void si_layout_apply(const struct si_layout *layout, obs_source_t *transition)
{
	obs_transition_set_alignment(transition, layout->alignment);
	obs_transition_set_scale_type(transition, layout->scale_type);
}

void si_layout_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SI_S_BOUNDING, T_("BoundingSize.Auto"));
	obs_data_set_default_string(settings, SI_S_SCALE_MODE, SCALE_FIT);
	obs_data_set_default_string(settings, SI_S_ALIGNMENT, "center");
}

static const char *aspects[] = {"16:9", "16:10", "4:3", "1:1", "9:16"};

#define NUM_ASPECTS (sizeof(aspects) / sizeof(const char *))

void si_layout_properties(obs_properties_t *props)
{
	obs_properties_t *appearance = obs_properties_create();
	struct obs_video_info ovi;
	obs_property_t *p;
	char res[32];

	p = obs_properties_add_list(appearance, SI_S_BOUNDING, T_("BoundingSize"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("BoundingSize.Auto"), T_("BoundingSize.Auto"));

	obs_get_video_info(&ovi);
	snprintf(res, sizeof(res), "%dx%d", (int)ovi.base_width, (int)ovi.base_height);
	obs_property_list_add_string(p, res, res);

	for (size_t i = 0; i < NUM_ASPECTS; i++)
		obs_property_list_add_string(p, aspects[i], aspects[i]);

	p = obs_properties_add_list(appearance, SI_S_SCALE_MODE, T_("ScaleMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("ScaleMode.Fit"), SCALE_FIT);
	obs_property_list_add_string(p, T_("ScaleMode.Stretch"), SCALE_STRETCH);
	obs_property_list_add_string(p, T_("ScaleMode.DownOnly"), SCALE_DOWN_ONLY);

	p = obs_properties_add_list(appearance, SI_S_ALIGNMENT, T_("Alignment"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Align.Center"), "center");
	obs_property_list_add_string(p, T_("Align.TopLeft"), "topleft");
	obs_property_list_add_string(p, T_("Align.Top"), "top");
	obs_property_list_add_string(p, T_("Align.TopRight"), "topright");
	obs_property_list_add_string(p, T_("Align.Left"), "left");
	obs_property_list_add_string(p, T_("Align.Right"), "right");
	obs_property_list_add_string(p, T_("Align.BottomLeft"), "bottomleft");
	obs_property_list_add_string(p, T_("Align.Bottom"), "bottom");
	obs_property_list_add_string(p, T_("Align.BottomRight"), "bottomright");

	obs_properties_add_group(props, "appearance_group", T_("Group.Appearance"), OBS_GROUP_NORMAL, appearance);
}

/* ------------------------------------------------------------------------- */
/* built-in transitions                                                       */

const char *si_transition_id(const char *type, bool *is_cut, bool *is_stinger)
{
	*is_cut = false;
	*is_stinger = false;

	if (astrcmpi(type, SI_TR_CUT) == 0) {
		*is_cut = true;
		return "cut_transition";
	}
	if (astrcmpi(type, SI_TR_SWIPE) == 0)
		return "swipe_transition";
	if (astrcmpi(type, SI_TR_SLIDE) == 0)
		return "slide_transition";
	if (astrcmpi(type, SI_TR_FADE_TO_COLOR) == 0)
		return "fade_to_color_transition";
	if (astrcmpi(type, SI_TR_STINGER) == 0) {
		*is_stinger = true;
		return "obs_stinger_transition";
	}

	return "fade_transition";
}

obs_data_t *si_transition_settings(const char *tr_id, const struct si_tr_keys *keys, obs_data_t *settings)
{
	obs_data_t *tr_settings = obs_data_create();

	if (strcmp(tr_id, "swipe_transition") == 0 || strcmp(tr_id, "slide_transition") == 0) {
		obs_data_set_string(tr_settings, "direction", obs_data_get_string(settings, keys->direction));
		obs_data_set_bool(tr_settings, "swipe_in", obs_data_get_bool(settings, keys->swipe_in));
	} else if (strcmp(tr_id, "fade_to_color_transition") == 0) {
		obs_data_set_int(tr_settings, "color", obs_data_get_int(settings, keys->color));
		obs_data_set_int(tr_settings, "switch_point", obs_data_get_int(settings, keys->switch_point));
	} else if (strcmp(tr_id, "obs_stinger_transition") == 0) {
		obs_data_set_string(tr_settings, "path", obs_data_get_string(settings, keys->stinger_path));
		obs_data_set_int(tr_settings, "tp_type", 0 /* transition point in milliseconds */);
		obs_data_set_int(tr_settings, "transition_point", obs_data_get_int(settings, keys->stinger_point));
		obs_data_set_bool(tr_settings, "hw_decode", obs_data_get_bool(settings, keys->stinger_hw));
	}

	return tr_settings;
}

void si_transition_defaults(obs_data_t *settings, const struct si_tr_keys *keys)
{
	obs_data_set_default_string(settings, keys->direction, "left");
	obs_data_set_default_bool(settings, keys->swipe_in, false);
	obs_data_set_default_int(settings, keys->color, 0xFF000000);
	obs_data_set_default_int(settings, keys->switch_point, 50);
	obs_data_set_default_int(settings, keys->stinger_point, 1000);
	obs_data_set_default_bool(settings, keys->stinger_hw, true);
}

void si_transition_list_add_builtins(obs_property_t *list)
{
	obs_property_list_add_string(list, T_("Transition.Cut"), SI_TR_CUT);
	obs_property_list_add_string(list, T_("Transition.Fade"), SI_TR_FADE);
	obs_property_list_add_string(list, T_("Transition.Swipe"), SI_TR_SWIPE);
	obs_property_list_add_string(list, T_("Transition.Slide"), SI_TR_SLIDE);
	obs_property_list_add_string(list, T_("Transition.FadeToColor"), SI_TR_FADE_TO_COLOR);
	obs_property_list_add_string(list, T_("Transition.Stinger"), SI_TR_STINGER);
}

static const char *stinger_filter = "Video files (*.mp4 *.m4v *.mov *.mkv *.webm *.ts *.avi *.flv);;All files (*.*)";

void si_transition_option_properties(obs_properties_t *props, const struct si_tr_keys *keys)
{
	obs_property_t *p;

	p = obs_properties_add_list(props, keys->direction, T_("Transition.Direction"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Direction.Left"), "left");
	obs_property_list_add_string(p, T_("Direction.Right"), "right");
	obs_property_list_add_string(p, T_("Direction.Up"), "up");
	obs_property_list_add_string(p, T_("Direction.Down"), "down");

	obs_properties_add_bool(props, keys->swipe_in, T_("Transition.SwipeIn"));

	obs_properties_add_color(props, keys->color, T_("Transition.Color"));
	p = obs_properties_add_int_slider(props, keys->switch_point, T_("Transition.SwitchPoint"), 0, 100, 1);
	obs_property_int_set_suffix(p, " %");

	obs_properties_add_path(props, keys->stinger_path, T_("Transition.Stinger.Path"), OBS_PATH_FILE, stinger_filter,
				NULL);
	p = obs_properties_add_int(props, keys->stinger_point, T_("Transition.Stinger.TransitionPoint"), 0, 120000, 50);
	obs_property_int_set_suffix(p, " ms");
	obs_properties_add_bool(props, keys->stinger_hw, T_("Transition.Stinger.HardwareDecode"));
}

static void set_visible(obs_properties_t *props, const char *key, bool visible)
{
	obs_property_t *p = obs_properties_get(props, key);

	if (p)
		obs_property_set_visible(p, visible);
}

void si_transition_options_visible(obs_properties_t *props, const struct si_tr_keys *keys, const char *type)
{
	bool is_swipe = astrcmpi(type, SI_TR_SWIPE) == 0;
	bool is_slide = astrcmpi(type, SI_TR_SLIDE) == 0;
	bool to_color = astrcmpi(type, SI_TR_FADE_TO_COLOR) == 0;
	bool is_stinger = astrcmpi(type, SI_TR_STINGER) == 0;

	set_visible(props, keys->direction, is_swipe || is_slide);
	set_visible(props, keys->swipe_in, is_swipe);
	set_visible(props, keys->color, to_color);
	set_visible(props, keys->switch_point, to_color);
	set_visible(props, keys->stinger_path, is_stinger);
	set_visible(props, keys->stinger_point, is_stinger);
	set_visible(props, keys->stinger_hw, is_stinger);
}

/* ------------------------------------------------------------------------- */
/* media controls                                                             */

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	obs_source_t *source = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(source))
		obs_source_media_play_pause(source, obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	obs_source_t *source = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(source))
		obs_source_media_restart(source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	obs_source_t *source = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(source))
		obs_source_media_stop(source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	obs_source_t *source = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(source))
		obs_source_media_next(source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	obs_source_t *source = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(source))
		obs_source_media_previous(source);
}

void si_media_hotkeys_register(obs_source_t *source, const struct si_hotkey_names *names,
			       const struct si_hotkey_names *texts)
{
	/* The source is the hotkey data: hotkeys registered on a source are
	 * unregistered by libobs when the source is destroyed. */
	obs_hotkey_register_source(source, names->play_pause, texts->play_pause, play_pause_hotkey, source);
	obs_hotkey_register_source(source, names->restart, texts->restart, restart_hotkey, source);
	obs_hotkey_register_source(source, names->stop, texts->stop, stop_hotkey, source);
	obs_hotkey_register_source(source, names->next, texts->next, next_hotkey, source);
	obs_hotkey_register_source(source, names->previous, texts->previous, previous_hotkey, source);
}

/* ------------------------------------------------------------------------- */
/* missing files                                                              */

struct si_missing_file {
	/* not a reference: these live exactly as long as the source does */
	obs_source_t *source;
	char *path;
};

/* `src` is the source's private data, which is no use here: everything needed
 * comes in `data`. */
static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct si_missing_file *missing = data;
	obs_source_t *source = missing->source;
	const char *orig_path = missing->path;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, SI_S_FILES);
	size_t count = obs_data_array_count(files);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *file = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(file, "value");

		if (strcmp(path, orig_path) == 0) {
			if (new_path && *new_path)
				obs_data_set_string(file, "value", new_path);
			else
				obs_data_array_erase(files, i);

			obs_data_release(file);
			break;
		}

		obs_data_release(file);
	}

	obs_source_update(source, settings);

	obs_data_array_release(files);
	obs_data_release(settings);

	UNUSED_PARAMETER(src);
}

obs_missing_files_t *si_missing_files(obs_source_t *source, si_missing_array_t *owned)
{
	obs_missing_files_t *missing_files = obs_missing_files_create();
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, SI_S_FILES);
	size_t count = obs_data_array_count(files);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (path && *path && !os_file_exists(path)) {
			/* The callback may run long after this, when the
			 * settings - and the string `path` points into - have
			 * long been replaced. It gets a copy that lives as long
			 * as the source does. */
			struct si_missing_file *missing = bzalloc(sizeof(*missing));
			obs_missing_file_t *file;

			missing->source = source;
			missing->path = bstrdup(path);
			da_push_back(*owned, &missing);

			file = obs_missing_file_create(path, missing_file_callback, OBS_MISSING_FILE_SOURCE, source,
						       missing);
			obs_missing_files_add_file(missing_files, file);
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}

void si_missing_free(si_missing_array_t *owned)
{
	for (size_t i = 0; i < owned->num; i++) {
		bfree(owned->array[i]->path);
		bfree(owned->array[i]);
	}

	da_free(*owned);
}
