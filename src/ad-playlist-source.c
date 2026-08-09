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
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>

#include <stdlib.h>

#include "ad-playlist-source.h"

#define do_log(level, format, ...) \
	blog(level, "[ad playlist: '%s'] " format, obs_source_get_name(ap->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

/* clang-format off */

#define S_FILES                        "files"
#define S_IMAGE_TIME                   "image_duration"
#define S_LOOP                         "loop"
#define S_SHUFFLE                      "shuffle"
#define S_HIDE_DONE                    "hide_when_done"
#define S_RESTART_ON_SHOW              "restart_on_show"
#define S_PRELOAD                      "preload"
#define S_HW_DECODE                    "hw_decode"
#define S_VOLUME                       "volume"

#define S_TRANSITION                   "transition"
#define S_TR_DURATION                  "transition_duration"
#define S_TR_TIMING                    "transition_timing"
#define S_TR_DIRECTION                 "transition_direction"
#define S_TR_SWIPE_IN                  "transition_swipe_in"
#define S_TR_COLOR                     "transition_color"
#define S_TR_SWITCH_POINT              "transition_switch_point"
#define S_TR_STINGER_PATH              "stinger_path"
#define S_TR_STINGER_POINT             "stinger_transition_point"
#define S_TR_STINGER_HW                "stinger_hw_decode"

#define S_BOUNDING                     "bounding_size"
#define S_SCALE_MODE                   "scale_mode"
#define S_ALIGNMENT                    "alignment"

#define S_GROUP_PLAYBACK               "playback_group"
#define S_GROUP_TRANSITION             "transition_group"
#define S_GROUP_APPEARANCE             "appearance_group"

#define TR_CUT                         "cut"
#define TR_FADE                        "fade"
#define TR_SWIPE                       "swipe"
#define TR_SLIDE                       "slide"
#define TR_FADE_TO_COLOR               "fade_to_color"
#define TR_STINGER                     "stinger"

#define TIMING_OVERLAP                 "overlap"
#define TIMING_AFTER                   "after"

#define SCALE_FIT                      "fit"
#define SCALE_STRETCH                  "stretch"
#define SCALE_DOWN_ONLY                "down_only"

#define T_(text) obs_module_text("AdPlaylist." text)
#define T_AD_PLAYLIST                  T_("Name")
#define T_FILES                        T_("Files")
#define T_IMAGE_TIME                   T_("ImageDuration")
#define T_LOOP                         T_("Loop")
#define T_SHUFFLE                      T_("Shuffle")
#define T_HIDE_DONE                    T_("HideWhenDone")
#define T_RESTART_ON_SHOW              T_("RestartOnShow")
#define T_PRELOAD                      T_("Preload")
#define T_HW_DECODE                    T_("HardwareDecode")
#define T_VOLUME                       T_("Volume")

#define T_GROUP_PLAYBACK               T_("Group.Playback")
#define T_GROUP_TRANSITION             T_("Group.Transition")
#define T_GROUP_APPEARANCE             T_("Group.Appearance")

#define T_TRANSITION                   T_("Transition")
#define T_TR_DURATION                  T_("Transition.Duration")
#define T_TR_TIMING                    T_("Transition.Timing")
#define T_TR_TIMING_OVERLAP            T_("Transition.Timing.Overlap")
#define T_TR_TIMING_AFTER              T_("Transition.Timing.After")
#define T_TR_DIRECTION                 T_("Transition.Direction")
#define T_TR_SWIPE_IN                  T_("Transition.SwipeIn")
#define T_TR_COLOR                     T_("Transition.Color")
#define T_TR_SWITCH_POINT              T_("Transition.SwitchPoint")
#define T_TR_STINGER_PATH              T_("Transition.Stinger.Path")
#define T_TR_STINGER_POINT             T_("Transition.Stinger.TransitionPoint")
#define T_TR_STINGER_HW                T_("Transition.Stinger.HardwareDecode")

#define T_TR_CUT                       T_("Transition.Cut")
#define T_TR_FADE                      T_("Transition.Fade")
#define T_TR_SWIPE                     T_("Transition.Swipe")
#define T_TR_SLIDE                     T_("Transition.Slide")
#define T_TR_FADE_TO_COLOR             T_("Transition.FadeToColor")
#define T_TR_STINGER                   T_("Transition.Stinger")

#define T_DIR_LEFT                     T_("Direction.Left")
#define T_DIR_RIGHT                    T_("Direction.Right")
#define T_DIR_UP                       T_("Direction.Up")
#define T_DIR_DOWN                     T_("Direction.Down")

#define T_BOUNDING                     T_("BoundingSize")
#define T_BOUNDING_AUTO                T_("BoundingSize.Auto")
#define T_SCALE_MODE                   T_("ScaleMode")
#define T_SCALE_FIT                    T_("ScaleMode.Fit")
#define T_SCALE_STRETCH                T_("ScaleMode.Stretch")
#define T_SCALE_DOWN_ONLY              T_("ScaleMode.DownOnly")
#define T_ALIGNMENT                    T_("Alignment")

#define T_ALIGN_CENTER                 T_("Align.Center")
#define T_ALIGN_TOP_LEFT               T_("Align.TopLeft")
#define T_ALIGN_TOP                    T_("Align.Top")
#define T_ALIGN_TOP_RIGHT              T_("Align.TopRight")
#define T_ALIGN_LEFT                   T_("Align.Left")
#define T_ALIGN_RIGHT                  T_("Align.Right")
#define T_ALIGN_BOTTOM_LEFT            T_("Align.BottomLeft")
#define T_ALIGN_BOTTOM                 T_("Align.Bottom")
#define T_ALIGN_BOTTOM_RIGHT           T_("Align.BottomRight")

#define T_HK_PLAY_PAUSE                T_("Hotkey.PlayPause")
#define T_HK_RESTART                   T_("Hotkey.Restart")
#define T_HK_STOP                      T_("Hotkey.Stop")
#define T_HK_NEXT                      T_("Hotkey.Next")
#define T_HK_PREVIOUS                  T_("Hotkey.Previous")

/* clang-format on */

/* Maximum number of files pulled in from a single directory entry. */
#define MAX_DIR_FILES 500

/* Seconds to wait for a media file to start playing before giving up on it and
 * moving on. Keeps a broken or unsupported file from stalling the playlist. */
#define MEDIA_START_TIMEOUT 5.0f

/* Media items only report their real dimensions once they have been opened, so
 * automatic sizing is refreshed on this interval (in seconds). */
#define SIZE_UPDATE_INTERVAL 0.25f

/* How far before the real end of a media file playback is considered finished
 * when the transition is set to start after the item, in milliseconds. Roughly
 * one frame, so the transition still has a picture to work with. */
#define END_GUARD_MS 50

/* How often the state of the audio chain is reported, in seconds. */
#define AUDIO_DIAG_INTERVAL 1.0f

/* ------------------------------------------------------------------------- */

/*
 * How far the audio handed over by the transition got on the last mix. Audio is
 * pulled on the audio thread, where a silent playlist otherwise leaves no trace
 * anywhere it can be seen, so the stage is recorded there and reported from the
 * video tick whenever it changes.
 */
enum ap_audio_stage {
	AP_AUDIO_OK,
	AP_AUDIO_NO_TRANSITION,
	AP_AUDIO_TR_PENDING,
	AP_AUDIO_TR_NO_TS,
};

struct ad_item {
	char *path;
	bool is_video;
	obs_source_t *source;
};

typedef DARRAY(struct ad_item) ad_item_array_t;
typedef DARRAY(size_t) index_array_t;
typedef DARRAY(char *) path_array_t;

struct ad_playlist {
	obs_source_t *source;

	/* Recursive: signals and media callbacks emitted while the lock is held
	 * can re-enter this source through its procs. */
	pthread_mutex_t mutex;

	/* ---- settings ---- */
	float image_time;
	uint32_t tr_duration_ms;
	uint32_t stinger_point_ms;
	float volume;
	bool overlap;
	bool loop;
	bool shuffle;
	bool hide_when_done;
	bool restart_on_show;
	bool preload;
	bool hw_decode;
	bool use_cut;
	bool use_stinger;
	char *tr_type;     /* transition setting value */
	const char *tr_id; /* libobs transition source id */

	/*
	 * What the children were last built from. OBS calls update() for every
	 * change to any property - including every step of a slider drag - and
	 * rebuilding the playlist or reopening the transition for a setting that
	 * neither depends on is both ruinously expensive and disruptive to
	 * whatever is on screen. These say whether the inputs actually moved.
	 *
	 * `configured_paths` is the file list as it is configured, before
	 * directories are expanded.
	 */
	path_array_t configured_paths;
	char *tr_settings_sig;

	/* ---- children ---- */
	obs_source_t *transition;
	ad_item_array_t items;
	index_array_t order;

	/* ---- playback state ---- */
	size_t order_pos;
	float elapsed;
	bool started; /* current media item was observed playing */
	bool paused;
	bool stopped;
	bool finished; /* reached the end of a non-looping playlist */
	bool showing;
	bool restart_pending;
	enum obs_media_state state;

	/* item whose media is stopped once the transition away from it ends */
	obs_source_t *stop_pending;

	/* ---- sizing ---- */
	uint32_t cx;
	uint32_t cy;
	uint32_t content_cx;
	uint32_t content_cy;
	bool auto_size;
	bool aspect_only;
	uint32_t bound_cx;
	uint32_t bound_cy;
	enum obs_transition_scale_type scale_type;
	uint32_t alignment;
	float size_timer;

	/* ---- audio ---- */
	volatile long audio_stage; /* enum ap_audio_stage, written on the audio thread */
	long audio_stage_logged;
	float audio_diag_timer;

	obs_hotkey_id play_pause_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id prev_hotkey;
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */

static inline void set_media_state(struct ad_playlist *ap, enum obs_media_state state)
{
	ap->state = state;
}

static obs_source_t *get_transition(struct ad_playlist *ap)
{
	obs_source_t *tr;

	pthread_mutex_lock(&ap->mutex);
	tr = obs_source_get_ref(ap->transition);
	pthread_mutex_unlock(&ap->mutex);

	return tr;
}

static bool is_image_extension(const char *ext)
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

static bool is_media_extension(const char *ext)
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

static inline bool supported_extension(const char *ext)
{
	return is_image_extension(ext) || is_media_extension(ext);
}

static obs_data_t *image_settings(const char *path)
{
	obs_data_t *settings = obs_data_create();

	obs_data_set_string(settings, "file", path);
	obs_data_set_bool(settings, "unload", false);

	return settings;
}

/*
 * Media items must never play on their own:
 *
 *   restart_on_activate = true   keeps the source from starting when it is
 *                                created or updated - playback only ever
 *                                starts when this source hands it its turn
 *   close_when_inactive = !preload
 *                                with preloading the file stays open and its
 *                                first frame is decoded ahead of time, so an
 *                                item starts instantly when it comes up
 */
static obs_data_t *media_settings(const char *path, bool preload, bool hw_decode)
{
	obs_data_t *settings = obs_data_create();

	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "clear_on_media_end", true);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "close_when_inactive", !preload);
	obs_data_set_bool(settings, "hw_decode", hw_decode);

	return settings;
}

static void free_items(ad_item_array_t *items)
{
	for (size_t i = 0; i < items->num; i++) {
		bfree(items->array[i].path);
		obs_source_release(items->array[i].source);
	}

	da_free(*items);
}

static obs_source_t *find_existing_source(ad_item_array_t *items, const char *path, bool is_video)
{
	for (size_t i = 0; i < items->num; i++) {
		if (items->array[i].is_video == is_video && strcmp(items->array[i].path, path) == 0)
			return obs_source_get_ref(items->array[i].source);
	}

	return NULL;
}

static void add_item(struct ad_playlist *ap, ad_item_array_t *new_items, const char *path)
{
	const char *ext = os_get_path_extension(path);
	bool is_video = !is_image_extension(ext);
	struct ad_item item;
	obs_data_t *settings;
	obs_source_t *source;

	pthread_mutex_lock(&ap->mutex);
	source = find_existing_source(&ap->items, path, is_video);
	pthread_mutex_unlock(&ap->mutex);

	if (!source)
		source = find_existing_source(new_items, path, is_video);

	settings = is_video ? media_settings(path, ap->preload, ap->hw_decode) : image_settings(path);

	if (source)
		/* reused from the previous playlist - refresh it in case the
		 * preload or decoding options changed */
		obs_source_update(source, settings);
	else
		source = obs_source_create_private(is_video ? "ffmpeg_source" : "image_source", NULL, settings);

	obs_data_release(settings);

	if (!source) {
		warn("failed to create source for '%s'", path);
		return;
	}

	item.path = bstrdup(path);
	item.is_video = is_video;
	item.source = source;
	da_push_back(*new_items, &item);
}

static int compare_paths(const void *a, const void *b)
{
	return astrcmpi(*(const char *const *)a, *(const char *const *)b);
}

/* Adds every supported file in `path`. Returns false if it is not a directory. */
static bool add_directory(struct ad_playlist *ap, ad_item_array_t *new_items, const char *path)
{
	DARRAY(char *) entries;
	struct dstr file_path = {0};
	os_dir_t *dir = os_opendir(path);

	if (!dir)
		return false;

	da_init(entries);

	for (;;) {
		struct os_dirent *ent = os_readdir(dir);
		const char *ext;
		char *entry;

		if (!ent)
			break;
		if (ent->directory)
			continue;

		ext = os_get_path_extension(ent->d_name);
		if (!supported_extension(ext))
			continue;

		entry = bstrdup(ent->d_name);
		da_push_back(entries, &entry);

		if (entries.num >= MAX_DIR_FILES) {
			warn("directory '%s' holds more than %d usable files, ignoring the rest", path, MAX_DIR_FILES);
			break;
		}
	}

	os_closedir(dir);

	/* directory order is filesystem dependent - sort so a playlist plays in
	 * the same order on every machine and every launch */
	if (entries.num > 1)
		qsort(entries.array, entries.num, sizeof(char *), compare_paths);

	for (size_t i = 0; i < entries.num; i++) {
		dstr_copy(&file_path, path);
		dstr_cat_ch(&file_path, '/');
		dstr_cat(&file_path, entries.array[i]);
		add_item(ap, new_items, file_path.array);
		bfree(entries.array[i]);
	}

	dstr_free(&file_path);
	da_free(entries);

	return true;
}

/* ------------------------------------------------------------------------- */
/* playback order                                                             */

static void build_order(struct ad_playlist *ap)
{
	size_t count = ap->items.num;

	da_resize(ap->order, count);

	for (size_t i = 0; i < count; i++)
		ap->order.array[i] = i;

	if (!ap->shuffle || count < 2)
		return;

	/* Fisher-Yates. Shuffling the order (rather than picking at random each
	 * time) guarantees every sponsor spot runs exactly once per pass. */
	for (size_t i = count - 1; i > 0; i--) {
		size_t j = (size_t)(rand() % (int)(i + 1));
		size_t tmp = ap->order.array[i];

		ap->order.array[i] = ap->order.array[j];
		ap->order.array[j] = tmp;
	}
}

/* Reshuffle for the next pass without repeating an item across the loop point. */
static void reshuffle_order(struct ad_playlist *ap)
{
	size_t last = ap->order.num ? ap->order.array[ap->order.num - 1] : 0;

	build_order(ap);

	if (ap->order.num > 1 && ap->order.array[0] == last) {
		size_t tmp = ap->order.array[0];

		ap->order.array[0] = ap->order.array[ap->order.num - 1];
		ap->order.array[ap->order.num - 1] = tmp;
	}
}

static inline bool item_valid(struct ad_playlist *ap)
{
	return ap->items.num && ap->order.num && ap->order_pos < ap->order.num &&
	       ap->order.array[ap->order_pos] < ap->items.num;
}

static inline struct ad_item *current_item(struct ad_playlist *ap)
{
	if (!item_valid(ap))
		return NULL;

	return &ap->items.array[ap->order.array[ap->order_pos]];
}

/* ------------------------------------------------------------------------- */
/* transitions                                                                */

/*
 * How far ahead of the end of an item the transition has to start for it to
 * land exactly on the item's last frame. For a stinger that is the transition
 * point (where the incoming source is swapped in behind the stinger video),
 * for everything else it is the whole transition.
 */
static inline uint32_t transition_lead_ms(struct ad_playlist *ap)
{
	if (ap->use_cut)
		return 0;
	if (ap->use_stinger)
		return ap->stinger_point_ms;

	return ap->tr_duration_ms;
}

static void stop_pending_media(struct ad_playlist *ap, bool force)
{
	struct ad_item *item;

	if (!ap->stop_pending)
		return;

	if (!force && ap->transition && obs_transition_get_time(ap->transition) < 1.0f)
		return;

	item = current_item(ap);
	if (!item || item->source != ap->stop_pending)
		obs_source_media_stop(ap->stop_pending);

	obs_source_release(ap->stop_pending);
	ap->stop_pending = NULL;
}

/*
 * Queue the item that is on screen right now to be stopped once the transition
 * away from it has run its course: its tail keeps playing (and stays audible)
 * for the length of the transition, and it stops decoding afterwards.
 *
 * Must be called before the playlist position moves.
 */
static void queue_stop_of_current(struct ad_playlist *ap)
{
	struct ad_item *item = current_item(ap);

	stop_pending_media(ap, true);

	if (item && item->is_video)
		ap->stop_pending = obs_source_get_ref(item->source);
}

/*
 * The calldata is a local, never a member: this signal is emitted with the
 * mutex held, and a handler that turns around and moves the playlist - a script
 * calling next() on the item it was just told about, say - re-enters and emits
 * again from underneath us. A shared buffer would be rewritten (and its string
 * freed) while the handlers of the outer signal were still reading it.
 *
 * Everything the signal carries is read out of the playlist before it is
 * emitted, for the same reason: `item` belongs to the item array, which a
 * re-entrant handler is free to rebuild.
 */
static void signal_item_changed(struct ad_playlist *ap, struct ad_item *item)
{
	signal_handler_t *sh = obs_source_get_signal_handler(ap->source);
	calldata_t cd;

	calldata_init(&cd);

	calldata_set_int(&cd, "index", (long long)ap->order.array[ap->order_pos]);
	calldata_set_int(&cd, "position", (long long)ap->order_pos);
	calldata_set_string(&cd, "path", item->path);
	calldata_set_bool(&cd, "is_video", item->is_video);

	signal_handler_signal(sh, "ad_item_changed", &cd);

	calldata_free(&cd);
}

/* Hand the transition over to the current item, or to nothing at all when
 * `to_null` is set. Mutex must be held. */
static void do_transition(struct ad_playlist *ap, bool to_null)
{
	struct ad_item *item = to_null ? NULL : current_item(ap);

	if (!ap->transition)
		return;

	if (item && ap->use_cut) {
		obs_transition_set(ap->transition, item->source);
	} else {
		bool started = obs_transition_start(ap->transition, OBS_TRANSITION_MODE_AUTO, ap->tr_duration_ms,
						    item ? item->source : NULL);

		/* libobs rejects a transition to whatever is already on screen,
		 * which happens with a single item playlist or the same file
		 * listed twice in a row. Swap it in directly instead. */
		if (!started && item)
			obs_transition_set(ap->transition, item->source);
	}

	ap->elapsed = 0.0f;
	ap->started = false;

	if (!item)
		return;

	if (item->is_video)
		obs_source_media_restart(item->source);

	signal_item_changed(ap, item);
}

static void playlist_finished(struct ad_playlist *ap)
{
	struct ad_item *item = current_item(ap);
	signal_handler_t *sh = obs_source_get_signal_handler(ap->source);
	calldata_t cd;

	ap->finished = true;
	set_media_state(ap, OBS_MEDIA_STATE_ENDED);

	if (ap->hide_when_done) {
		queue_stop_of_current(ap);
		do_transition(ap, true);
	} else if (item && item->is_video) {
		/* hold the last frame instead of clearing to nothing */
		obs_source_media_play_pause(item->source, true);
	}

	calldata_init(&cd);
	signal_handler_signal(sh, "ad_playlist_ended", &cd);
	calldata_free(&cd);

	obs_source_media_ended(ap->source);
}

/* Move to the next (or previous) item, or finish the playlist. Mutex must be
 * held. */
static void advance_item(struct ad_playlist *ap, bool forward)
{
	if (!ap->items.num || !ap->order.num)
		return;

	if (forward && ap->order_pos + 1 >= ap->order.num && !ap->loop) {
		playlist_finished(ap);
		return;
	}

	queue_stop_of_current(ap);

	if (forward) {
		if (ap->order_pos + 1 >= ap->order.num) {
			if (ap->shuffle)
				reshuffle_order(ap);

			ap->order_pos = 0;
		} else {
			ap->order_pos++;
		}
	} else {
		if (ap->order_pos == 0)
			ap->order_pos = ap->order.num - 1;
		else
			ap->order_pos--;
	}

	ap->finished = false;

	if (!ap->paused)
		set_media_state(ap, OBS_MEDIA_STATE_PLAYING);

	do_transition(ap, false);
}

/* Start over from the top of the playlist. Mutex must be held. */
static void restart_playlist(struct ad_playlist *ap)
{
	queue_stop_of_current(ap);

	if (ap->shuffle)
		build_order(ap);

	ap->order_pos = 0;
	ap->finished = false;
	ap->stopped = false;
	ap->paused = false;
	ap->restart_pending = false;

	if (ap->items.num) {
		set_media_state(ap, OBS_MEDIA_STATE_PLAYING);
		do_transition(ap, false);
		obs_source_media_started(ap->source);
	} else {
		set_media_state(ap, OBS_MEDIA_STATE_NONE);
		do_transition(ap, true);
	}
}

/* ------------------------------------------------------------------------- */
/* sizing                                                                     */

/*
 * Recalculates the source dimensions. `content_cx`/`content_cy` track the
 * largest item in the playlist; the source itself is either that (automatic) or
 * the configured bounding box. Mutex must be held.
 */
static void update_size(struct ad_playlist *ap)
{
	uint32_t cx = ap->content_cx;
	uint32_t cy = ap->content_cy;

	for (size_t i = 0; i < ap->items.num; i++) {
		uint32_t item_cx = obs_source_get_width(ap->items.array[i].source);
		uint32_t item_cy = obs_source_get_height(ap->items.array[i].source);

		if (item_cx > cx)
			cx = item_cx;
		if (item_cy > cy)
			cy = item_cy;
	}

	/* Content size never shrinks within a session: an item that has not
	 * been opened yet reports nothing, and a scene layout that resizes
	 * itself mid-playlist is worse than one that is slightly too large. */
	ap->content_cx = cx;
	ap->content_cy = cy;

	if (!ap->auto_size) {
		if (ap->aspect_only) {
			if (cx && cy) {
				double cx_f = (double)cx;
				double cy_f = (double)cy;
				double old_aspect = cx_f / cy_f;
				double new_aspect = (double)ap->bound_cx / (double)ap->bound_cy;

				if (new_aspect > old_aspect)
					cx = (uint32_t)(cy_f * new_aspect);
				else
					cy = (uint32_t)(cx_f / new_aspect);
			}
		} else {
			cx = ap->bound_cx;
			cy = ap->bound_cy;
		}
	}

	if (cx == ap->cx && cy == ap->cy)
		return;

	ap->cx = cx;
	ap->cy = cy;

	if (ap->transition)
		obs_transition_set_size(ap->transition, cx, cy);
}

/* ------------------------------------------------------------------------- */
/* settings                                                                   */

static const char *transition_id_for(const char *type, bool *is_cut, bool *is_stinger)
{
	*is_cut = false;
	*is_stinger = false;

	if (astrcmpi(type, TR_CUT) == 0) {
		*is_cut = true;
		return "cut_transition";
	}
	if (astrcmpi(type, TR_SWIPE) == 0)
		return "swipe_transition";
	if (astrcmpi(type, TR_SLIDE) == 0)
		return "slide_transition";
	if (astrcmpi(type, TR_FADE_TO_COLOR) == 0)
		return "fade_to_color_transition";
	if (astrcmpi(type, TR_STINGER) == 0) {
		*is_stinger = true;
		return "obs_stinger_transition";
	}

	return "fade_transition";
}

static obs_data_t *build_transition_settings(struct ad_playlist *ap, obs_data_t *settings)
{
	obs_data_t *tr_settings = obs_data_create();

	if (strcmp(ap->tr_id, "swipe_transition") == 0 || strcmp(ap->tr_id, "slide_transition") == 0) {
		obs_data_set_string(tr_settings, "direction", obs_data_get_string(settings, S_TR_DIRECTION));
		obs_data_set_bool(tr_settings, "swipe_in", obs_data_get_bool(settings, S_TR_SWIPE_IN));
	} else if (strcmp(ap->tr_id, "fade_to_color_transition") == 0) {
		obs_data_set_int(tr_settings, "color", obs_data_get_int(settings, S_TR_COLOR));
		obs_data_set_int(tr_settings, "switch_point", obs_data_get_int(settings, S_TR_SWITCH_POINT));
	} else if (ap->use_stinger) {
		obs_data_set_string(tr_settings, "path", obs_data_get_string(settings, S_TR_STINGER_PATH));
		obs_data_set_int(tr_settings, "tp_type", 0 /* transition point in milliseconds */);
		obs_data_set_int(tr_settings, "transition_point", (long long)ap->stinger_point_ms);
		obs_data_set_bool(tr_settings, "hw_decode", obs_data_get_bool(settings, S_TR_STINGER_HW));
	}

	return tr_settings;
}

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

/* "1920x1080" is a fixed size, "16:9" is a fixed aspect ratio, and anything
 * else - including the localized "Automatic" entry - means automatic. */
static void parse_bounding_size(struct ad_playlist *ap, const char *val)
{
	int cx = 0;
	int cy = 0;

	ap->auto_size = true;
	ap->aspect_only = false;

	if (!val || !*val)
		return;

	if (sscanf(val, "%dx%d", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		ap->auto_size = false;
		ap->aspect_only = false;
	} else if (sscanf(val, "%d:%d", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		ap->auto_size = false;
		ap->aspect_only = true;
	} else {
		return;
	}

	ap->bound_cx = (uint32_t)cx;
	ap->bound_cy = (uint32_t)cy;
}

static void free_paths(path_array_t *paths)
{
	for (size_t i = 0; i < paths->num; i++)
		bfree(paths->array[i]);

	da_free(*paths);
}

/* The playlist as it is configured, with the blank entries dropped and the
 * directories still unexpanded. */
static void read_configured_paths(obs_data_t *settings, path_array_t *paths)
{
	obs_data_array_t *array = obs_data_get_array(settings, S_FILES);
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

static bool paths_equal(const path_array_t *a, const path_array_t *b)
{
	if (a->num != b->num)
		return false;

	for (size_t i = 0; i < a->num; i++) {
		if (strcmp(a->array[i], b->array[i]) != 0)
			return false;
	}

	return true;
}

static char *transition_signature(obs_data_t *tr_settings)
{
	const char *json = obs_data_get_json(tr_settings);
	return bstrdup(json ? json : "");
}

/* Takes ownership of `fresh`. */
static bool signature_changed(char **stored, char *fresh)
{
	if (*stored && strcmp(*stored, fresh) == 0) {
		bfree(fresh);
		return false;
	}

	bfree(*stored);
	*stored = fresh;

	return true;
}

/*
 * OBS calls this for every change to any property, and a slider hands over a
 * new value for every step of the drag. Rebuilding the playlist reopens every
 * media file and restarts playback from the top, so it only runs when the
 * settings behind the children actually changed - a volume drag just stores a
 * float and leaves the rotation alone.
 */
static void ap_update(void *data, obs_data_t *settings)
{
	struct ad_playlist *ap = data;
	ad_item_array_t new_items;
	ad_item_array_t old_items;
	obs_source_t *new_tr = NULL;
	obs_source_t *old_tr = NULL;
	obs_data_t *tr_settings;
	path_array_t paths;
	const char *tr_type;
	const char *scale_mode;
	bool is_cut;
	bool is_stinger;
	bool shuffle;
	bool preload;
	bool hw_decode;
	bool reload_items;
	bool reload_order;
	bool tr_settings_moved;
	bool first_update = !ap->tr_type;

	da_init(new_items);
	da_init(old_items);
	da_init(paths);

	/* ------------------------------------- */
	/* plain settings                        */

	shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	preload = obs_data_get_bool(settings, S_PRELOAD);
	hw_decode = obs_data_get_bool(settings, S_HW_DECODE);

	reload_order = shuffle != ap->shuffle;

	ap->overlap = astrcmpi(obs_data_get_string(settings, S_TR_TIMING), TIMING_AFTER) != 0;
	ap->loop = obs_data_get_bool(settings, S_LOOP);
	ap->shuffle = shuffle;
	ap->hide_when_done = obs_data_get_bool(settings, S_HIDE_DONE);
	ap->restart_on_show = obs_data_get_bool(settings, S_RESTART_ON_SHOW);
	ap->image_time = (float)obs_data_get_int(settings, S_IMAGE_TIME) / 1000.0f;
	ap->tr_duration_ms = (uint32_t)obs_data_get_int(settings, S_TR_DURATION);
	ap->stinger_point_ms = (uint32_t)obs_data_get_int(settings, S_TR_STINGER_POINT);
	ap->volume = (float)obs_data_get_int(settings, S_VOLUME) / 100.0f;

	tr_type = obs_data_get_string(settings, S_TRANSITION);
	ap->tr_id = transition_id_for(tr_type, &is_cut, &is_stinger);
	ap->use_cut = is_cut;
	ap->use_stinger = is_stinger;

	if (!ap->tr_type || strcmp(ap->tr_type, tr_type) != 0) {
		new_tr = obs_source_create_private(ap->tr_id, NULL, NULL);
		bfree(ap->tr_type);
		ap->tr_type = bstrdup(tr_type);

		if (!new_tr)
			warn("failed to create transition '%s'", ap->tr_id);
	}

	/* ------------------------------------- */
	/* playlist contents                     */

	/*
	 * The child sources are built from the file list and from the two
	 * options baked into each media source. Anything else - the volume in
	 * particular - leaves them alone: expanding the directories walks the
	 * filesystem, and handing a media source new settings makes it reopen
	 * its file, neither of which survives being done once per slider step.
	 */
	read_configured_paths(settings, &paths);

	reload_items = first_update || preload != ap->preload || hw_decode != ap->hw_decode ||
		       !paths_equal(&paths, &ap->configured_paths);

	ap->preload = preload;
	ap->hw_decode = hw_decode;

	if (reload_items) {
		for (size_t i = 0; i < paths.num; i++) {
			if (!add_directory(ap, &new_items, paths.array[i]))
				add_item(ap, &new_items, paths.array[i]);
		}

		free_paths(&ap->configured_paths);
		ap->configured_paths = paths;
	} else {
		free_paths(&paths);
	}

	/* ------------------------------------- */
	/* appearance                            */

	parse_bounding_size(ap, obs_data_get_string(settings, S_BOUNDING));

	scale_mode = obs_data_get_string(settings, S_SCALE_MODE);
	if (astrcmpi(scale_mode, SCALE_STRETCH) == 0)
		ap->scale_type = OBS_TRANSITION_SCALE_STRETCH;
	else if (astrcmpi(scale_mode, SCALE_DOWN_ONLY) == 0)
		ap->scale_type = OBS_TRANSITION_SCALE_MAX_ONLY;
	else
		ap->scale_type = OBS_TRANSITION_SCALE_ASPECT;

	ap->alignment = alignment_from_string(obs_data_get_string(settings, S_ALIGNMENT));

	/* ------------------------------------- */
	/* swap in the new state                 */

	pthread_mutex_lock(&ap->mutex);

	if (reload_items) {
		queue_stop_of_current(ap);
		stop_pending_media(ap, true);

		old_items = ap->items;
		ap->items = new_items;
	}

	if (new_tr) {
		old_tr = ap->transition;
		ap->transition = new_tr;
	}

	if (reload_items || reload_order) {
		build_order(ap);
		ap->order_pos = 0;
		ap->elapsed = 0.0f;
		ap->started = false;
		ap->finished = false;
	}

	pthread_mutex_unlock(&ap->mutex);

	if (old_tr) {
		obs_source_remove_active_child(ap->source, old_tr);
		obs_source_release(old_tr);
	}

	free_items(&old_items);

	/* ------------------------------------- */
	/* apply to the transition               */

	if (!ap->transition) {
		set_media_state(ap, OBS_MEDIA_STATE_ERROR);
		return;
	}

	tr_settings = build_transition_settings(ap, settings);
	tr_settings_moved = signature_changed(&ap->tr_settings_sig, transition_signature(tr_settings));

	/* a stinger reopens its video every time it is updated, so only hand the
	 * settings over when they moved - or when the transition is brand new */
	if (tr_settings_moved || new_tr)
		obs_source_update(ap->transition, tr_settings);

	obs_data_release(tr_settings);

	obs_transition_set_alignment(ap->transition, ap->alignment);
	obs_transition_set_scale_type(ap->transition, ap->scale_type);

	if (new_tr)
		obs_source_add_active_child(ap->source, new_tr);

	pthread_mutex_lock(&ap->mutex);

	if (reload_items) {
		ap->cx = 0;
		ap->cy = 0;
		ap->content_cx = 0;
		ap->content_cy = 0;
	}

	update_size(ap);

	/* A transition that was just created is showing nothing, so the current
	 * item has to be handed to it even when the playlist itself stood still. */
	if (reload_items || reload_order || new_tr) {
		if (ap->items.num) {
			ap->stopped = false;
			ap->paused = false;
			set_media_state(ap, OBS_MEDIA_STATE_PLAYING);
			do_transition(ap, false);
			obs_source_media_started(ap->source);
		} else {
			set_media_state(ap, OBS_MEDIA_STATE_NONE);
			do_transition(ap, true);
		}
	}

	pthread_mutex_unlock(&ap->mutex);
}

/* ------------------------------------------------------------------------- */
/* media controls                                                             */

static enum obs_media_state ap_get_state(void *data)
{
	struct ad_playlist *ap = data;
	return ap->state;
}

static void ap_play_pause(void *data, bool pause)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;

	pthread_mutex_lock(&ap->mutex);

	/* pressing play after a stop, or after a non-looping playlist ran out,
	 * starts the playlist over */
	if (!pause && (ap->stopped || ap->finished)) {
		restart_playlist(ap);
		pthread_mutex_unlock(&ap->mutex);
		return;
	}

	ap->paused = pause;
	item = current_item(ap);

	if (item && item->is_video)
		obs_source_media_play_pause(item->source, pause);

	set_media_state(ap, pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING);

	pthread_mutex_unlock(&ap->mutex);
}

static void ap_restart(void *data)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);
	restart_playlist(ap);
	pthread_mutex_unlock(&ap->mutex);
}

static void ap_stop(void *data)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);

	queue_stop_of_current(ap);
	stop_pending_media(ap, true);

	ap->order_pos = 0;
	ap->elapsed = 0.0f;
	ap->started = false;
	ap->stopped = true;
	ap->paused = false;
	ap->finished = false;

	do_transition(ap, true);
	set_media_state(ap, OBS_MEDIA_STATE_STOPPED);

	pthread_mutex_unlock(&ap->mutex);
}

static void ap_next(void *data)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);

	if (ap->items.num) {
		ap->stopped = false;
		ap->finished = false;
		advance_item(ap, true);
	}

	pthread_mutex_unlock(&ap->mutex);
}

static void ap_previous(void *data)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);

	if (ap->items.num) {
		ap->stopped = false;
		ap->finished = false;
		advance_item(ap, false);
	}

	pthread_mutex_unlock(&ap->mutex);
}

static int64_t ap_get_duration(void *data)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;
	int64_t duration = 0;

	pthread_mutex_lock(&ap->mutex);

	item = current_item(ap);
	if (item)
		duration = item->is_video ? obs_source_media_get_duration(item->source)
					  : (int64_t)(ap->image_time * 1000.0f);

	pthread_mutex_unlock(&ap->mutex);

	return duration;
}

static int64_t ap_get_time(void *data)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;
	int64_t time = 0;

	pthread_mutex_lock(&ap->mutex);

	item = current_item(ap);
	if (item)
		time = item->is_video ? obs_source_media_get_time(item->source) : (int64_t)(ap->elapsed * 1000.0f);

	pthread_mutex_unlock(&ap->mutex);

	return time;
}

static void ap_set_time(void *data, int64_t ms)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;

	pthread_mutex_lock(&ap->mutex);

	item = current_item(ap);
	if (item) {
		if (item->is_video)
			obs_source_media_set_time(item->source, ms);
		else
			ap->elapsed = (float)ms / 1000.0f;
	}

	pthread_mutex_unlock(&ap->mutex);
}

/* ------------------------------------------------------------------------- */
/* hotkeys                                                                    */

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct ad_playlist *ap = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(ap->source))
		obs_source_media_play_pause(ap->source, !ap->paused);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct ad_playlist *ap = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(ap->source))
		obs_source_media_restart(ap->source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct ad_playlist *ap = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(ap->source))
		obs_source_media_stop(ap->source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct ad_playlist *ap = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(ap->source))
		obs_source_media_next(ap->source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct ad_playlist *ap = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(ap->source))
		obs_source_media_previous(ap->source);
}

/* ------------------------------------------------------------------------- */
/* procs                                                                      */

static void current_index_proc(void *data, calldata_t *cd)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);
	calldata_set_int(cd, "current_index", item_valid(ap) ? (long long)ap->order.array[ap->order_pos] : -1);
	pthread_mutex_unlock(&ap->mutex);
}

static void current_path_proc(void *data, calldata_t *cd)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;

	pthread_mutex_lock(&ap->mutex);
	item = current_item(ap);
	calldata_set_string(cd, "current_path", item ? item->path : "");
	pthread_mutex_unlock(&ap->mutex);
}

static void total_items_proc(void *data, calldata_t *cd)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);
	calldata_set_int(cd, "total_items", (long long)ap->items.num);
	pthread_mutex_unlock(&ap->mutex);
}

/* ------------------------------------------------------------------------- */
/* source callbacks                                                           */

static const char *ap_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_AD_PLAYLIST;
}

static void ap_destroy(void *data)
{
	struct ad_playlist *ap = data;

	if (ap->stop_pending)
		obs_source_release(ap->stop_pending);

	obs_source_release(ap->transition);
	free_items(&ap->items);
	da_free(ap->order);
	bfree(ap->tr_type);
	bfree(ap->tr_settings_sig);
	free_paths(&ap->configured_paths);
	pthread_mutex_destroy(&ap->mutex);
	bfree(ap);
}

static void *ap_create(obs_data_t *settings, obs_source_t *source)
{
	struct ad_playlist *ap = bzalloc(sizeof(struct ad_playlist));
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	signal_handler_t *sh = obs_source_get_signal_handler(source);

	ap->source = source;
	ap->state = OBS_MEDIA_STATE_NONE;
	ap->scale_type = OBS_TRANSITION_SCALE_ASPECT;
	ap->alignment = OBS_ALIGN_CENTER;
	ap->auto_size = true;
	ap->volume = 1.0f;
	ap->audio_stage_logged = -1; /* nothing reported yet - the first report always lands */

	da_init(ap->items);
	da_init(ap->order);

	pthread_mutex_init_value(&ap->mutex);
	if (pthread_mutex_init_recursive(&ap->mutex) != 0) {
		bfree(ap);
		return NULL;
	}

	ap->play_pause_hotkey =
		obs_hotkey_register_source(source, "AdPlaylist.PlayPause", T_HK_PLAY_PAUSE, play_pause_hotkey, ap);
	ap->restart_hotkey = obs_hotkey_register_source(source, "AdPlaylist.Restart", T_HK_RESTART, restart_hotkey, ap);
	ap->stop_hotkey = obs_hotkey_register_source(source, "AdPlaylist.Stop", T_HK_STOP, stop_hotkey, ap);
	ap->next_hotkey = obs_hotkey_register_source(source, "AdPlaylist.NextItem", T_HK_NEXT, next_hotkey, ap);
	ap->prev_hotkey =
		obs_hotkey_register_source(source, "AdPlaylist.PreviousItem", T_HK_PREVIOUS, previous_hotkey, ap);

	proc_handler_add(ph, "void current_index(out int current_index)", current_index_proc, ap);
	proc_handler_add(ph, "void current_path(out string current_path)", current_path_proc, ap);
	proc_handler_add(ph, "void total_items(out int total_items)", total_items_proc, ap);

	signal_handler_add(sh, "void ad_item_changed(int index, int position, string path, bool is_video)");
	signal_handler_add(sh, "void ad_playlist_ended()");

	obs_source_update(source, NULL);

	UNUSED_PARAMETER(settings);
	return ap;
}

static void ap_video_render(void *data, gs_effect_t *effect)
{
	struct ad_playlist *ap = data;
	obs_source_t *transition = get_transition(ap);

	if (transition) {
		obs_source_video_render(transition);
		obs_source_release(transition);
	}

	UNUSED_PARAMETER(effect);
}

/*
 * Has the current item run its course? `ignore_overlap` forces an item to play
 * all the way out - used for the last item of a playlist that is not looping
 * and not hiding itself, where there is nothing to transition to anyway.
 *
 * Mutex must be held.
 */
static bool item_reached_end(struct ad_playlist *ap, struct ad_item *item, bool ignore_overlap)
{
	bool overlap = ap->overlap && !ignore_overlap;
	uint32_t lead_ms;
	int64_t duration;
	int64_t time;
	enum obs_media_state state;

	if (!item->is_video) {
		float hold = ap->image_time;

		/* in overlap mode the transition eats into the hold time, so an
		 * image is on screen for exactly the configured duration */
		if (overlap)
			hold -= (float)transition_lead_ms(ap) / 1000.0f;

		if (hold < 0.05f)
			hold = 0.05f;

		return ap->elapsed >= hold;
	}

	state = obs_source_media_get_state(item->source);

	if (state == OBS_MEDIA_STATE_PLAYING)
		ap->started = true;

	if (!ap->started) {
		/* a file that never starts (missing, unsupported, no decoder)
		 * must not stall the playlist */
		if (ap->elapsed > MEDIA_START_TIMEOUT) {
			warn("'%s' did not start playing, skipping it", item->path);
			return true;
		}

		return false;
	}

	if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED)
		return true;

	duration = obs_source_media_get_duration(item->source);
	time = obs_source_media_get_time(item->source);

	/* nothing to time against - wait for the media to report that it ended */
	if (duration <= 0)
		return false;

	/*
	 * Overlapping starts the transition early enough to land on the last
	 * frame. Without overlap the item still hands over a frame before it
	 * ends: a media source blanks itself the moment playback stops, so
	 * waiting for the end outright would transition away from nothing.
	 */
	lead_ms = overlap ? transition_lead_ms(ap) : END_GUARD_MS;

	/* never start the transition before the item has had its moment */
	if ((int64_t)lead_ms > duration / 2)
		lead_ms = (uint32_t)(duration / 2);

	return time >= duration - (int64_t)lead_ms;
}

static const char *audio_stage_name(long stage)
{
	switch ((enum ap_audio_stage)stage) {
	case AP_AUDIO_OK:
		return "mixing";
	case AP_AUDIO_NO_TRANSITION:
		return "no transition to pull from";
	case AP_AUDIO_TR_PENDING:
		return "transition handed over no audio";
	case AP_AUDIO_TR_NO_TS:
		return "transition audio has no timestamp";
	}

	return "unknown";
}

/*
 * Report where the audio chain got, and what the item itself is handing to the
 * transition, whenever either changes. Between them the two tell apart a media
 * file that is producing no audio at all, a transition that is not passing it
 * on, and a playlist that is mixing audio that goes silent further downstream.
 *
 * Mutex must be held.
 */
static void report_audio_state(struct ad_playlist *ap, struct ad_item *item, float seconds)
{
	long stage;

	ap->audio_diag_timer += seconds;
	if (ap->audio_diag_timer < AUDIO_DIAG_INTERVAL)
		return;

	ap->audio_diag_timer = 0.0f;

	/* only meaningful once the item is actually rolling */
	if (!item->is_video || !ap->started)
		return;

	stage = os_atomic_load_long(&ap->audio_stage);
	if (stage == ap->audio_stage_logged)
		return;

	ap->audio_stage_logged = stage;

	if (stage == AP_AUDIO_OK)
		info("audio: mixing '%s' at %d%%", item->path, (int)(ap->volume * 100.0f + 0.5f));
	else
		warn("audio: %s for '%s' (item audio %s, timestamp %llu)", audio_stage_name(stage), item->path,
		     obs_source_audio_pending(item->source) ? "pending" : "ready",
		     (unsigned long long)obs_source_get_audio_timestamp(item->source));
}

static void ap_video_tick(void *data, float seconds)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;
	bool last_item;

	pthread_mutex_lock(&ap->mutex);

	if (!ap->transition)
		goto finish;

	stop_pending_media(ap, false);

	ap->size_timer += seconds;
	if (ap->size_timer >= SIZE_UPDATE_INTERVAL) {
		ap->size_timer = 0.0f;
		update_size(ap);
	}

	if (!ap->items.num) {
		obs_source_t *active = obs_transition_get_active_source(ap->transition);

		/* the playlist was emptied out from under us */
		if (active) {
			obs_source_release(active);
			do_transition(ap, true);
		}

		goto finish;
	}

	if (ap->restart_pending && ap->showing) {
		restart_playlist(ap);
		goto finish;
	}

	/* Media items only decode while the source is visible, so the playlist
	 * holds its position instead of racing ahead while hidden. */
	if (!ap->showing || ap->paused || ap->stopped || ap->finished)
		goto finish;

	item = current_item(ap);
	if (!item)
		goto finish;

	ap->elapsed += seconds;

	report_audio_state(ap, item, seconds);

	last_item = !ap->loop && ap->order_pos + 1 >= ap->order.num;

	if (item_reached_end(ap, item, last_item && !ap->hide_when_done))
		advance_item(ap, true);

finish:
	pthread_mutex_unlock(&ap->mutex);
}

static inline enum ap_audio_stage render_child_audio(obs_source_t *transition, uint64_t *ts_out,
						     struct obs_source_audio_mix *audio_output, uint32_t mixers,
						     size_t channels, float volume)
{
	struct obs_source_audio_mix child_audio;
	uint64_t source_ts;

	if (obs_source_audio_pending(transition))
		return AP_AUDIO_TR_PENDING;

	source_ts = obs_source_get_audio_timestamp(transition);
	if (!source_ts)
		return AP_AUDIO_TR_NO_TS;

	obs_source_get_audio_mix(transition, &child_audio);

	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			float *in = child_audio.output[mix].data[ch];

			if (volume == 1.0f) {
				memcpy(out, in, AUDIO_OUTPUT_FRAMES * sizeof(float));
			} else {
				for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
					out[i] = in[i] * volume;
			}
		}
	}

	*ts_out = source_ts;
	return AP_AUDIO_OK;
}

static bool ap_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			    size_t channels, size_t sample_rate)
{
	struct ad_playlist *ap = data;
	obs_source_t *transition = get_transition(ap);
	enum ap_audio_stage stage;

	if (!transition) {
		os_atomic_set_long(&ap->audio_stage, AP_AUDIO_NO_TRANSITION);
		return false;
	}

	stage = render_child_audio(transition, ts_out, audio_output, mixers, channels, ap->volume);
	obs_source_release(transition);

	os_atomic_set_long(&ap->audio_stage, (long)stage);

	UNUSED_PARAMETER(sample_rate);
	return stage == AP_AUDIO_OK;
}

static void ap_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);
	if (ap->transition)
		cb(ap->source, ap->transition, param);
	pthread_mutex_unlock(&ap->mutex);
}

static uint32_t ap_get_width(void *data)
{
	struct ad_playlist *ap = data;
	return ap->transition ? ap->cx : 0;
}

static uint32_t ap_get_height(void *data)
{
	struct ad_playlist *ap = data;
	return ap->transition ? ap->cy : 0;
}

static void ap_show(void *data)
{
	struct ad_playlist *ap = data;

	pthread_mutex_lock(&ap->mutex);

	ap->showing = true;

	if (ap->restart_on_show) {
		/* deferred to the next tick, by which point the child sources
		 * are showing and can actually start */
		ap->restart_pending = true;
	} else if (!ap->paused && !ap->stopped && !ap->finished) {
		struct ad_item *item = current_item(ap);

		if (item && item->is_video) {
			obs_source_media_restart(item->source);
			ap->elapsed = 0.0f;
			ap->started = false;
		}
	}

	pthread_mutex_unlock(&ap->mutex);
}

static void ap_hide(void *data)
{
	struct ad_playlist *ap = data;
	struct ad_item *item;

	pthread_mutex_lock(&ap->mutex);

	ap->showing = false;

	/* stop decoding while nobody is watching */
	item = current_item(ap);
	if (item && item->is_video)
		obs_source_media_play_pause(item->source, true);

	pthread_mutex_unlock(&ap->mutex);
}

static void ap_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_IMAGE_TIME, 10000);
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_SHUFFLE, false);
	obs_data_set_default_bool(settings, S_HIDE_DONE, false);
	obs_data_set_default_bool(settings, S_RESTART_ON_SHOW, true);
	obs_data_set_default_bool(settings, S_PRELOAD, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, false);
	obs_data_set_default_int(settings, S_VOLUME, 100);

	obs_data_set_default_string(settings, S_TRANSITION, TR_FADE);
	obs_data_set_default_int(settings, S_TR_DURATION, 500);
	obs_data_set_default_string(settings, S_TR_TIMING, TIMING_OVERLAP);
	obs_data_set_default_string(settings, S_TR_DIRECTION, "left");
	obs_data_set_default_bool(settings, S_TR_SWIPE_IN, false);
	obs_data_set_default_int(settings, S_TR_COLOR, 0xFF000000);
	obs_data_set_default_int(settings, S_TR_SWITCH_POINT, 50);
	obs_data_set_default_int(settings, S_TR_STINGER_POINT, 1000);
	obs_data_set_default_bool(settings, S_TR_STINGER_HW, true);

	obs_data_set_default_string(settings, S_BOUNDING, T_BOUNDING_AUTO);
	obs_data_set_default_string(settings, S_SCALE_MODE, SCALE_FIT);
	obs_data_set_default_string(settings, S_ALIGNMENT, "center");
}

static bool transition_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const char *type = obs_data_get_string(settings, S_TRANSITION);
	bool is_cut = astrcmpi(type, TR_CUT) == 0;
	bool is_swipe = astrcmpi(type, TR_SWIPE) == 0;
	bool is_stinger = astrcmpi(type, TR_STINGER) == 0;
	bool directional = is_swipe || astrcmpi(type, TR_SLIDE) == 0;
	bool to_color = astrcmpi(type, TR_FADE_TO_COLOR) == 0;

	UNUSED_PARAMETER(p);

	obs_property_set_visible(obs_properties_get(props, S_TR_DURATION), !is_cut && !is_stinger);
	obs_property_set_visible(obs_properties_get(props, S_TR_TIMING), !is_cut);
	obs_property_set_visible(obs_properties_get(props, S_TR_DIRECTION), directional);
	obs_property_set_visible(obs_properties_get(props, S_TR_SWIPE_IN), is_swipe);
	obs_property_set_visible(obs_properties_get(props, S_TR_COLOR), to_color);
	obs_property_set_visible(obs_properties_get(props, S_TR_SWITCH_POINT), to_color);
	obs_property_set_visible(obs_properties_get(props, S_TR_STINGER_PATH), is_stinger);
	obs_property_set_visible(obs_properties_get(props, S_TR_STINGER_POINT), is_stinger);
	obs_property_set_visible(obs_properties_get(props, S_TR_STINGER_HW), is_stinger);

	return true;
}

/* clang-format off */
static const char *media_filter =
	"Media files (*.mp4 *.m4v *.mov *.mkv *.webm *.ts *.m2ts *.mts *.mpg *.mpeg *.avi *.flv *.wmv *.mxf *.3gp"
	" *.mp3 *.aac *.m4a *.wav *.ogg *.opus *.flac"
	" *.bmp *.tga *.png *.jpg *.jpeg *.jpe *.jfif *.gif *.webp *.psd *.tif *.tiff"
#ifdef _WIN32
	" *.jxr"
#endif
	");;All files (*.*)";

static const char *stinger_filter = "Video files (*.mp4 *.m4v *.mov *.mkv *.webm *.ts *.avi *.flv);;All files (*.*)";
/* clang-format on */

static const char *aspects[] = {"16:9", "16:10", "4:3", "1:1", "9:16"};

#define NUM_ASPECTS (sizeof(aspects) / sizeof(const char *))

static obs_properties_t *ap_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *playback = obs_properties_create();
	obs_properties_t *transition = obs_properties_create();
	obs_properties_t *appearance = obs_properties_create();
	struct ad_playlist *ap = data;
	struct obs_video_info ovi;
	struct dstr last_dir = {0};
	obs_property_t *p;
	char res[32];

	/* ----------------- playlist ----------------- */

	if (ap) {
		pthread_mutex_lock(&ap->mutex);
		if (ap->items.num) {
			struct ad_item *last = da_end(ap->items);
			const char *slash;

			dstr_copy(&last_dir, last->path);
			dstr_replace(&last_dir, "\\", "/");
			slash = strrchr(last_dir.array, '/');
			if (slash)
				dstr_resize(&last_dir, slash - last_dir.array + 1);
		}
		pthread_mutex_unlock(&ap->mutex);
	}

	obs_properties_add_editable_list(props, S_FILES, T_FILES, OBS_EDITABLE_LIST_TYPE_FILES, media_filter,
					 last_dir.array);
	dstr_free(&last_dir);

	/* ----------------- playback ----------------- */

	p = obs_properties_add_int(playback, S_IMAGE_TIME, T_IMAGE_TIME, 100, 3600000, 100);
	obs_property_int_set_suffix(p, " ms");

	obs_properties_add_bool(playback, S_LOOP, T_LOOP);
	obs_properties_add_bool(playback, S_SHUFFLE, T_SHUFFLE);
	obs_properties_add_bool(playback, S_HIDE_DONE, T_HIDE_DONE);
	obs_properties_add_bool(playback, S_RESTART_ON_SHOW, T_RESTART_ON_SHOW);

	p = obs_properties_add_int_slider(playback, S_VOLUME, T_VOLUME, 0, 100, 1);
	obs_property_int_set_suffix(p, " %");

	obs_properties_add_bool(playback, S_PRELOAD, T_PRELOAD);
	obs_properties_add_bool(playback, S_HW_DECODE, T_HW_DECODE);

	obs_properties_add_group(props, S_GROUP_PLAYBACK, T_GROUP_PLAYBACK, OBS_GROUP_NORMAL, playback);

	/* ----------------- transition ----------------- */

	p = obs_properties_add_list(transition, S_TRANSITION, T_TRANSITION, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_TR_CUT, TR_CUT);
	obs_property_list_add_string(p, T_TR_FADE, TR_FADE);
	obs_property_list_add_string(p, T_TR_SWIPE, TR_SWIPE);
	obs_property_list_add_string(p, T_TR_SLIDE, TR_SLIDE);
	obs_property_list_add_string(p, T_TR_FADE_TO_COLOR, TR_FADE_TO_COLOR);
	obs_property_list_add_string(p, T_TR_STINGER, TR_STINGER);
	obs_property_set_modified_callback(p, transition_changed);

	p = obs_properties_add_int(transition, S_TR_DURATION, T_TR_DURATION, 0, 60000, 50);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(transition, S_TR_TIMING, T_TR_TIMING, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_TR_TIMING_OVERLAP, TIMING_OVERLAP);
	obs_property_list_add_string(p, T_TR_TIMING_AFTER, TIMING_AFTER);

	p = obs_properties_add_list(transition, S_TR_DIRECTION, T_TR_DIRECTION, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_DIR_LEFT, "left");
	obs_property_list_add_string(p, T_DIR_RIGHT, "right");
	obs_property_list_add_string(p, T_DIR_UP, "up");
	obs_property_list_add_string(p, T_DIR_DOWN, "down");

	obs_properties_add_bool(transition, S_TR_SWIPE_IN, T_TR_SWIPE_IN);

	obs_properties_add_color(transition, S_TR_COLOR, T_TR_COLOR);
	obs_properties_add_int_slider(transition, S_TR_SWITCH_POINT, T_TR_SWITCH_POINT, 0, 100, 1);

	obs_properties_add_path(transition, S_TR_STINGER_PATH, T_TR_STINGER_PATH, OBS_PATH_FILE, stinger_filter, NULL);
	p = obs_properties_add_int(transition, S_TR_STINGER_POINT, T_TR_STINGER_POINT, 0, 120000, 50);
	obs_property_int_set_suffix(p, " ms");
	obs_properties_add_bool(transition, S_TR_STINGER_HW, T_TR_STINGER_HW);

	obs_properties_add_group(props, S_GROUP_TRANSITION, T_GROUP_TRANSITION, OBS_GROUP_NORMAL, transition);

	/* ----------------- appearance ----------------- */

	p = obs_properties_add_list(appearance, S_BOUNDING, T_BOUNDING, OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_BOUNDING_AUTO, T_BOUNDING_AUTO);

	obs_get_video_info(&ovi);
	snprintf(res, sizeof(res), "%dx%d", (int)ovi.base_width, (int)ovi.base_height);
	obs_property_list_add_string(p, res, res);

	for (size_t i = 0; i < NUM_ASPECTS; i++)
		obs_property_list_add_string(p, aspects[i], aspects[i]);

	p = obs_properties_add_list(appearance, S_SCALE_MODE, T_SCALE_MODE, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_SCALE_FIT, SCALE_FIT);
	obs_property_list_add_string(p, T_SCALE_STRETCH, SCALE_STRETCH);
	obs_property_list_add_string(p, T_SCALE_DOWN_ONLY, SCALE_DOWN_ONLY);

	p = obs_properties_add_list(appearance, S_ALIGNMENT, T_ALIGNMENT, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_ALIGN_CENTER, "center");
	obs_property_list_add_string(p, T_ALIGN_TOP_LEFT, "topleft");
	obs_property_list_add_string(p, T_ALIGN_TOP, "top");
	obs_property_list_add_string(p, T_ALIGN_TOP_RIGHT, "topright");
	obs_property_list_add_string(p, T_ALIGN_LEFT, "left");
	obs_property_list_add_string(p, T_ALIGN_RIGHT, "right");
	obs_property_list_add_string(p, T_ALIGN_BOTTOM_LEFT, "bottomleft");
	obs_property_list_add_string(p, T_ALIGN_BOTTOM, "bottom");
	obs_property_list_add_string(p, T_ALIGN_BOTTOM_RIGHT, "bottomright");

	obs_properties_add_group(props, S_GROUP_APPEARANCE, T_GROUP_APPEARANCE, OBS_GROUP_NORMAL, appearance);

	return props;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct ad_playlist *ap = src;
	const char *orig_path = data;
	obs_source_t *source = ap->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_FILES);
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
}

static obs_missing_files_t *ap_missing_files(void *data)
{
	struct ad_playlist *ap = data;
	obs_missing_files_t *missing_files = obs_missing_files_create();
	obs_data_t *settings = obs_source_get_settings(ap->source);
	obs_data_array_t *files = obs_data_get_array(settings, S_FILES);
	size_t count = obs_data_array_count(files);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (path && *path && !os_file_exists(path)) {
			obs_missing_file_t *file = obs_missing_file_create(
				path, missing_file_callback, OBS_MISSING_FILE_SOURCE, ap->source, (void *)path);

			obs_missing_files_add_file(missing_files, file);
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}

static enum gs_color_space ap_video_get_color_space(void *data, size_t count,
						    const enum gs_color_space *preferred_spaces)
{
	struct ad_playlist *ap = data;
	obs_source_t *transition = get_transition(ap);
	enum gs_color_space space = GS_CS_SRGB;

	if (transition) {
		space = obs_source_get_color_space(transition, count, preferred_spaces);
		obs_source_release(transition);
	}

	return space;
}

struct obs_source_info ad_playlist_source_info = {
	.id = "sponsor_ad_playlist",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = ap_get_name,
	.create = ap_create,
	.destroy = ap_destroy,
	.update = ap_update,
	.show = ap_show,
	.hide = ap_hide,
	.video_render = ap_video_render,
	.video_tick = ap_video_tick,
	.audio_render = ap_audio_render,
	.enum_active_sources = ap_enum_sources,
	.get_width = ap_get_width,
	.get_height = ap_get_height,
	.get_defaults = ap_defaults,
	.get_properties = ap_properties,
	.missing_files = ap_missing_files,
	.icon_type = OBS_ICON_TYPE_SLIDESHOW,
	.media_play_pause = ap_play_pause,
	.media_restart = ap_restart,
	.media_stop = ap_stop,
	.media_next = ap_next,
	.media_previous = ap_previous,
	.media_get_duration = ap_get_duration,
	.media_get_time = ap_get_time,
	.media_set_time = ap_set_time,
	.media_get_state = ap_get_state,
	.video_get_color_space = ap_video_get_color_space,
};
