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

/*
 * The parts of a playlist source that do not care what is being played: the
 * file list and its directories, the running order, the bounding box, the
 * built-in transitions and their options, the media control hotkeys and the
 * missing files dialog. Shared by the Ad Playlist and the Picture Gallery.
 */

#include <obs-module.h>
#include <util/darray.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* files                                                                      */

/* the setting holding the list of files and directories */
#define SI_S_FILES "files"

typedef DARRAY(char *) si_path_array_t;

/* Maximum number of files pulled in from a single directory entry. */
#define SI_MAX_DIR_FILES 500

typedef bool (*si_extension_filter_t)(const char *ext);

/* Whether `ext` (".png", including the dot) is an image the OBS image source
 * can load. */
bool si_is_image_extension(const char *ext);

/* Whether `ext` is a video or audio file for the OBS media source. */
bool si_is_media_extension(const char *ext);

void si_paths_free(si_path_array_t *paths);

/* The list in `settings[key]` as configured, blank entries dropped and
 * directories not expanded. */
void si_paths_read(obs_data_t *settings, const char *key, si_path_array_t *paths);

bool si_paths_equal(const si_path_array_t *a, const si_path_array_t *b);

/*
 * Expands a configured list into the files to play: a directory becomes every
 * file in it that passes `filter`, sorted by name so the order is the same on
 * every machine; anything else is kept as it is. `log_name` names the source
 * in warnings.
 */
void si_paths_expand(const si_path_array_t *configured, si_extension_filter_t filter, const char *log_name,
		     si_path_array_t *files);

/* The directory part of `path`, with a trailing slash, for a file dialog to
 * start in. */
void si_path_directory(const char *path, struct dstr *dir);

/* ------------------------------------------------------------------------- */
/* running order                                                              */

typedef DARRAY(size_t) si_index_array_t;

/* 0..count-1, shuffled with Fisher-Yates when `shuffle` is set, so every item
 * comes up exactly once per pass. */
void si_order_build(si_index_array_t *order, size_t count, bool shuffle);

/* A fresh shuffle for the next pass that does not repeat the last item of the
 * previous pass first. */
void si_order_reshuffle(si_index_array_t *order, size_t count, bool shuffle);

/* ------------------------------------------------------------------------- */
/* layout                                                                     */

#define SI_S_BOUNDING "bounding_size"
#define SI_S_SCALE_MODE "scale_mode"
#define SI_S_ALIGNMENT "alignment"

struct si_layout {
	bool auto_size;
	bool aspect_only;
	uint32_t bound_cx;
	uint32_t bound_cy;
	enum obs_transition_scale_type scale_type;
	uint32_t alignment;
};

void si_layout_init(struct si_layout *layout);
void si_layout_read(struct si_layout *layout, obs_data_t *settings);

/* The source size for content that is `content_cx` x `content_cy`. */
void si_layout_size(const struct si_layout *layout, uint32_t content_cx, uint32_t content_cy, uint32_t *cx,
		    uint32_t *cy);

void si_layout_apply(const struct si_layout *layout, obs_source_t *transition);

void si_layout_defaults(obs_data_t *settings);

/* Adds the Appearance group. */
void si_layout_properties(obs_properties_t *props);

/* ------------------------------------------------------------------------- */
/* built-in transitions                                                       */

#define SI_TR_CUT "cut"
#define SI_TR_FADE "fade"
#define SI_TR_SWIPE "swipe"
#define SI_TR_SLIDE "slide"
#define SI_TR_FADE_TO_COLOR "fade_to_color"
#define SI_TR_STINGER "stinger"

/* Where a source keeps the options of one of its transitions. */
struct si_tr_keys {
	const char *direction;
	const char *swipe_in;
	const char *color;
	const char *switch_point;
	const char *stinger_path;
	const char *stinger_point;
	const char *stinger_hw;
};

/* The libobs transition id for a transition setting value. */
const char *si_transition_id(const char *type, bool *is_cut, bool *is_stinger);

/* The settings for a built-in transition `tr_id`, read from `settings`. */
obs_data_t *si_transition_settings(const char *tr_id, const struct si_tr_keys *keys, obs_data_t *settings);

void si_transition_defaults(obs_data_t *settings, const struct si_tr_keys *keys);

/* Adds Cut, Fade, Swipe, Slide, Fade to Color and Stinger to a list. */
void si_transition_list_add_builtins(obs_property_t *list);

/* Adds the options of the built-in transitions (direction, color, stinger
 * file, ...), to be shown and hidden with si_transition_options_visible(). */
void si_transition_option_properties(obs_properties_t *props, const struct si_tr_keys *keys);

void si_transition_options_visible(obs_properties_t *props, const struct si_tr_keys *keys, const char *type);

/* ------------------------------------------------------------------------- */
/* media controls                                                             */

struct si_hotkey_names {
	const char *play_pause;
	const char *restart;
	const char *stop;
	const char *next;
	const char *previous;
};

/* Registers play/pause, restart, stop, next and previous hotkeys that drive
 * the source's media controls while it is showing. `names` are the saved
 * hotkey ids, `texts` what the settings show for them. */
void si_media_hotkeys_register(obs_source_t *source, const struct si_hotkey_names *names,
			       const struct si_hotkey_names *texts);

/* ------------------------------------------------------------------------- */
/* missing files                                                              */

struct si_missing_file;
typedef DARRAY(struct si_missing_file *) si_missing_array_t;

/*
 * The missing files dialog for the source's file list.
 *
 * libobs hands the callback the source's private data, not the source, and
 * keeps no copy of the data pointer it is given - so the source and a copy of
 * each missing path are kept in `owned`, which lives as long as the source and
 * is freed with si_missing_free() when it is destroyed. Only call this from
 * one thread at a time for a given source (libobs calls it from the UI
 * thread).
 */
obs_missing_files_t *si_missing_files(obs_source_t *source, si_missing_array_t *owned);

void si_missing_free(si_missing_array_t *owned);

#ifdef __cplusplus
}
#endif
