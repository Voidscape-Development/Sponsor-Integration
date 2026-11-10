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

/*
 * Picture Gallery
 *
 * A rotation of still (or animated) images with a separate transition for
 * each "direction" the gallery can move in:
 *
 *   Normal     image -> image
 *   Show       nothing -> image   (the gallery comes in)
 *   Hide       image -> nothing   (the gallery goes out)
 *   Loop wrap  last image -> first image
 *
 * and an optional gap between images: with a gap set, every change goes out
 * to nothing with the Hide transition, holds empty for the gap, and comes back
 * in with the Show transition (the Loop wrap transition, when it has one of
 * its own, brings the first image back in after the gap at the loop point).
 *
 * Each direction owns a private transition source of its own. Only one of them
 * is on screen at a time - `cur` - and when the gallery moves to a different
 * direction, whatever the current one shows is handed over to the next one
 * before it starts, so the new transition starts from the right picture.
 *
 * Every direction can use one of the built-in transitions or one of the scene
 * transitions configured in OBS (when built with the frontend API). A scene
 * transition is copied into a private transition of the same kind with the
 * same settings, and the copy follows that scene transition's settings when
 * they are edited.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>

#include <stdlib.h>

#ifdef HAVE_OBS_FRONTEND_API
#include <obs-frontend-api.h>
#endif

#include "picture-gallery-source.h"
#include "source-common.h"

#define do_log(level, format, ...) \
	blog(level, "[picture gallery: '%s'] " format, obs_source_get_name(pg->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

/* clang-format off */

#define S_FILES                        "files"
#define S_HOLD_TIME                    "hold_time"
#define S_GAP_TIME                     "gap_time"
#define S_LOOP                         "loop"
#define S_SHUFFLE                      "shuffle"
#define S_HIDE_DONE                    "hide_when_done"
#define S_RESTART_ON_SHOW              "restart_on_show"

#define S_BOUNDING                     "bounding_size"
#define S_SCALE_MODE                   "scale_mode"
#define S_ALIGNMENT                    "alignment"

#define S_GROUP_PLAYBACK               "playback_group"
#define S_GROUP_APPEARANCE             "appearance_group"

/* per-direction settings are "<direction>_<suffix>", e.g. "show_duration" */
#define SS_TYPE                        "transition"
#define SS_SCENE                       "scene_transition"
#define SS_DURATION                    "duration"
#define SS_DIRECTION                   "direction"
#define SS_SWIPE_IN                    "swipe_in"
#define SS_COLOR                       "color"
#define SS_SWITCH_POINT                "switch_point"
#define SS_STINGER_PATH                "stinger_path"
#define SS_STINGER_POINT               "stinger_transition_point"
#define SS_STINGER_HW                  "stinger_hw_decode"
#define SS_GROUP                       "group"

#define TR_SAME                        "same"
#define TR_CUT                         "cut"
#define TR_FADE                        "fade"
#define TR_SWIPE                       "swipe"
#define TR_SLIDE                       "slide"
#define TR_FADE_TO_COLOR               "fade_to_color"
#define TR_STINGER                     "stinger"
#define TR_SCENE                       "scene"

#define SCALE_FIT                      "fit"
#define SCALE_STRETCH                  "stretch"
#define SCALE_DOWN_ONLY                "down_only"

#define T_(text) obs_module_text("PictureGallery." text)
#define T_PICTURE_GALLERY              T_("Name")
#define T_FILES                        T_("Files")
#define T_HOLD_TIME                    T_("HoldTime")
#define T_HOLD_TIME_DESC               T_("HoldTime.Desc")
#define T_GAP_TIME                     T_("GapTime")
#define T_GAP_TIME_DESC                T_("GapTime.Desc")
#define T_LOOP                         T_("Loop")
#define T_SHUFFLE                      T_("Shuffle")
#define T_HIDE_DONE                    T_("HideWhenDone")
#define T_RESTART_ON_SHOW              T_("RestartOnShow")

#define T_GROUP_PLAYBACK               T_("Group.Playback")
#define T_GROUP_APPEARANCE             T_("Group.Appearance")

#define T_TR_TYPE                      T_("Transition")
#define T_TR_SAME                      T_("Transition.Same")
#define T_TR_CUT                       T_("Transition.Cut")
#define T_TR_FADE                      T_("Transition.Fade")
#define T_TR_SWIPE                     T_("Transition.Swipe")
#define T_TR_SLIDE                     T_("Transition.Slide")
#define T_TR_FADE_TO_COLOR             T_("Transition.FadeToColor")
#define T_TR_STINGER                   T_("Transition.Stinger")
#define T_TR_SCENE                     T_("Transition.Scene")
#define T_TR_SCENE_NAME                T_("Transition.SceneName")
#define T_TR_SCENE_NAME_DESC           T_("Transition.SceneName.Desc")
#define T_TR_SCENE_MISSING             T_("Transition.SceneName.Missing")
#define T_TR_DURATION                  T_("Transition.Duration")
#define T_TR_DIRECTION                 T_("Transition.Direction")
#define T_TR_SWIPE_IN                  T_("Transition.SwipeIn")
#define T_TR_COLOR                     T_("Transition.Color")
#define T_TR_SWITCH_POINT              T_("Transition.SwitchPoint")
#define T_TR_STINGER_PATH              T_("Transition.Stinger.Path")
#define T_TR_STINGER_POINT             T_("Transition.Stinger.TransitionPoint")
#define T_TR_STINGER_HW                T_("Transition.Stinger.HardwareDecode")

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
#define T_HK_SHOW                      T_("Hotkey.Show")
#define T_HK_HIDE                      T_("Hotkey.Hide")

/* clang-format on */

/* Maximum number of files pulled in from a single directory entry. */
#define MAX_DIR_FILES 500

/* Images only report their real dimensions once they have loaded, so automatic
 * sizing is refreshed on this interval (in seconds). */
#define SIZE_UPDATE_INTERVAL 0.25f

/* How long past its configured duration a transition is waited on before it
 * is taken to have finished, in seconds. A transition reports its own end, but
 * one that is never rendered (or a stinger whose video will not open) must not
 * hold the rotation up for good. */
#define TRANSITION_GRACE 10.0f

/* ------------------------------------------------------------------------- */

enum pg_slot_id {
	PG_SLOT_NORMAL,
	PG_SLOT_SHOW,
	PG_SLOT_HIDE,
	PG_SLOT_WRAP,
	PG_NUM_SLOTS,
};

struct pg_slot_desc {
	const char *prefix;
	const char *group_text; /* locale key, without the PictureGallery. prefix */
	const char *group_desc;
	const char *default_type;
	uint32_t default_duration;
	/* whether "Same as the normal transition" is offered */
	bool allow_same;
};

static const struct pg_slot_desc slot_desc[PG_NUM_SLOTS] = {
	{"normal", "Group.Normal", "Group.Normal.Desc", TR_FADE, 500, false},
	{"show", "Group.Show", "Group.Show.Desc", TR_FADE, 500, false},
	{"hide", "Group.Hide", "Group.Hide.Desc", TR_FADE, 500, false},
	{"wrap", "Group.Wrap", "Group.Wrap.Desc", TR_SAME, 500, true},
};

struct picture_gallery;

/* One direction's transition. */
struct pg_slot {
	struct picture_gallery *pg;
	enum pg_slot_id id;

	/* The private transition this direction plays. NULL while it is set to
	 * "same as normal". Owned; written by update() under the mutex. */
	obs_source_t *tr;
	/* the libobs id `tr` was created as */
	char *tr_id;
	/* what `tr`'s settings were last built from, to skip no-op updates */
	char *sig;

	bool use_normal;
	bool cut;
	uint32_t duration_ms;

	/* The scene transition this direction copies, when it uses one, and
	 * whose edits it follows. */
	obs_weak_source_t *scene;

	/* Set by the transition's transition_stop signal. */
	volatile bool done;
};

struct pg_item {
	char *path;
	obs_source_t *source;
};

/*
 * What the gallery is doing:
 *
 *   IDLE  nothing in progress - either empty or holding a picture that is
 *         not going anywhere (stopped, hidden, a finished playlist)
 *   IN    a transition is bringing an image on screen
 *   HOLD  the image is fully on screen and its hold time is running
 *   OUT   the Hide transition is taking the gallery to nothing
 *   GAP   the gallery is empty and the gap is running
 */
enum pg_phase {
	PG_IDLE,
	PG_IN,
	PG_HOLD,
	PG_OUT,
	PG_GAP,
};

/* where the gallery goes once the Hide transition has finished */
enum pg_after_out {
	PG_AFTER_IDLE,
	PG_AFTER_GAP,
};

typedef DARRAY(struct pg_item) pg_item_array_t;
typedef DARRAY(size_t) pg_index_array_t;
typedef DARRAY(char *) pg_path_array_t;

struct picture_gallery {
	obs_source_t *source;

	/* Recursive: signals emitted while the lock is held can re-enter this
	 * source through its procs. */
	pthread_mutex_t mutex;

	/* ---- settings ---- */
	float hold_time;
	float gap_time;
	bool loop;
	bool shuffle;
	bool hide_when_done;
	bool restart_on_show;

	pg_path_array_t configured_paths;

	/* ---- children ---- */
	struct pg_slot slots[PG_NUM_SLOTS];
	/* the slot on screen, NULL before the first image is shown */
	struct pg_slot *cur;
	pg_item_array_t items;
	pg_index_array_t order;

	/* ---- playback state ---- */
	size_t order_pos;
	enum pg_phase phase;
	enum pg_after_out after_out;
	/* the direction that brings the next image in once the gap is over */
	enum pg_slot_id gap_in;
	float elapsed;
	/* whether the last transition went to an image rather than to nothing */
	bool on_screen;

	bool paused;
	bool stopped;
	bool finished;     /* reached the end of a non-looping playlist */
	bool user_hidden;  /* taken out with the Hide control */
	bool showing;      /* the source is visible somewhere */
	bool show_pending; /* became visible; handled on the next tick */
	enum obs_media_state state;

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

	bool first_update_done;

	obs_hotkey_id play_pause_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id prev_hotkey;
	obs_hotkey_id show_hotkey;
	obs_hotkey_id hide_hotkey;
};

/* ------------------------------------------------------------------------- */
/* scene transitions (frontend)                                               */

/*
 * The scene transitions configured in OBS, by name. The frontend's list may
 * only be read on the UI thread, while a source's update() can arrive from
 * anywhere (obs-websocket, scripts), so the list is copied here whenever it
 * changes - on the UI thread - and update() only ever reads the copy.
 */
struct st_entry {
	char *name;
	obs_weak_source_t *weak;
};

static pthread_mutex_t st_mutex;
static bool st_mutex_ready = false;
static DARRAY(struct st_entry) st_cache;

/* Every gallery that exists, so their scene transitions can be looked up
 * again when the frontend's list changes. Guarded by st_mutex. */
static DARRAY(struct picture_gallery *) st_galleries;

static void st_cache_free(void)
{
	for (size_t i = 0; i < st_cache.num; i++) {
		bfree(st_cache.array[i].name);
		obs_weak_source_release(st_cache.array[i].weak);
	}

	da_free(st_cache);
}

#ifdef HAVE_OBS_FRONTEND_API
/* UI thread only. */
static void st_refresh(void)
{
	struct obs_frontend_source_list list = {0};

	if (!st_mutex_ready)
		return;

	obs_frontend_get_transitions(&list);

	pthread_mutex_lock(&st_mutex);
	st_cache_free();

	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *tr = list.sources.array[i];
		const char *name = obs_source_get_name(tr);
		struct st_entry entry;

		if (!name || !*name)
			continue;

		entry.name = bstrdup(name);
		entry.weak = obs_source_get_weak_source(tr);
		da_push_back(st_cache, &entry);
	}

	pthread_mutex_unlock(&st_mutex);

	obs_frontend_source_list_free(&list);
}
#endif

/* A strong reference to the scene transition called `name`, or NULL. */
static obs_source_t *st_find(const char *name)
{
	obs_source_t *found = NULL;

	if (!st_mutex_ready || !name || !*name)
		return NULL;

	pthread_mutex_lock(&st_mutex);
	for (size_t i = 0; i < st_cache.num; i++) {
		if (strcmp(st_cache.array[i].name, name) == 0) {
			found = obs_weak_source_get_source(st_cache.array[i].weak);
			break;
		}
	}
	pthread_mutex_unlock(&st_mutex);

	return found;
}

/* Whether the frontend's list has been read at all yet. */
static bool st_have_list(void)
{
	bool have;

	if (!st_mutex_ready)
		return false;

	pthread_mutex_lock(&st_mutex);
	have = st_cache.num > 0;
	pthread_mutex_unlock(&st_mutex);

	return have;
}

static void st_register(struct picture_gallery *pg, bool add)
{
	if (!st_mutex_ready)
		return;

	pthread_mutex_lock(&st_mutex);
	if (add)
		da_push_back(st_galleries, &pg);
	else
		da_erase_item(st_galleries, &pg);
	pthread_mutex_unlock(&st_mutex);
}

#ifdef HAVE_OBS_FRONTEND_API
/* Runs every gallery's update() again so it picks its scene transitions up
 * afresh - they may have been created, renamed or removed. */
static void st_update_galleries(void)
{
	DARRAY(obs_source_t *) sources;

	da_init(sources);

	pthread_mutex_lock(&st_mutex);
	for (size_t i = 0; i < st_galleries.num; i++) {
		obs_source_t *source = obs_source_get_ref(st_galleries.array[i]->source);

		if (source)
			da_push_back(sources, &source);
	}
	pthread_mutex_unlock(&st_mutex);

	for (size_t i = 0; i < sources.num; i++) {
		obs_source_update(sources.array[i], NULL);
		obs_source_release(sources.array[i]);
	}

	da_free(sources);
}

static void st_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		st_refresh();
		st_update_galleries();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (st_mutex_ready) {
			pthread_mutex_lock(&st_mutex);
			st_cache_free();
			pthread_mutex_unlock(&st_mutex);
		}
		break;
	default:
		break;
	}
}
#endif

void picture_gallery_module_load(void)
{
	pthread_mutex_init_value(&st_mutex);
	if (pthread_mutex_init(&st_mutex, NULL) != 0)
		return;

	da_init(st_cache);
	da_init(st_galleries);
	st_mutex_ready = true;

#ifdef HAVE_OBS_FRONTEND_API
	obs_frontend_add_event_callback(st_frontend_event, NULL);
#endif
}

void picture_gallery_module_unload(void)
{
	if (!st_mutex_ready)
		return;

#ifdef HAVE_OBS_FRONTEND_API
	obs_frontend_remove_event_callback(st_frontend_event, NULL);
#endif

	pthread_mutex_lock(&st_mutex);
	st_cache_free();
	da_free(st_galleries);
	pthread_mutex_unlock(&st_mutex);

	st_mutex_ready = false;
	pthread_mutex_destroy(&st_mutex);
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */

static const char *slot_key(char *buf, size_t size, enum pg_slot_id id, const char *suffix)
{
	snprintf(buf, size, "%s_%s", slot_desc[id].prefix, suffix);
	return buf;
}

static inline void set_media_state(struct picture_gallery *pg, enum obs_media_state state)
{
	pg->state = state;
}

static obs_source_t *get_current_transition(struct picture_gallery *pg)
{
	obs_source_t *tr = NULL;

	pthread_mutex_lock(&pg->mutex);
	if (pg->cur)
		tr = obs_source_get_ref(pg->cur->tr);
	pthread_mutex_unlock(&pg->mutex);

	return tr;
}

static void free_items(pg_item_array_t *items)
{
	for (size_t i = 0; i < items->num; i++) {
		bfree(items->array[i].path);
		obs_source_release(items->array[i].source);
	}

	da_free(*items);
}

static void free_paths(pg_path_array_t *paths)
{
	for (size_t i = 0; i < paths->num; i++)
		bfree(paths->array[i]);

	da_free(*paths);
}

static obs_source_t *find_existing_source(pg_item_array_t *items, const char *path)
{
	for (size_t i = 0; i < items->num; i++) {
		if (strcmp(items->array[i].path, path) == 0)
			return obs_source_get_ref(items->array[i].source);
	}

	return NULL;
}

static void add_item(struct picture_gallery *pg, pg_item_array_t *new_items, const char *path)
{
	struct pg_item item;
	obs_source_t *source;

	if (!si_is_image_extension(os_get_path_extension(path))) {
		warn("'%s' is not an image, skipping it", path);
		return;
	}

	pthread_mutex_lock(&pg->mutex);
	source = find_existing_source(&pg->items, path);
	pthread_mutex_unlock(&pg->mutex);

	if (!source)
		source = find_existing_source(new_items, path);

	if (!source) {
		obs_data_t *settings = obs_data_create();

		obs_data_set_string(settings, "file", path);
		/* keep every image decoded so it is ready the moment it is
		 * transitioned to */
		obs_data_set_bool(settings, "unload", false);

		source = obs_source_create_private("image_source", NULL, settings);
		obs_data_release(settings);
	}

	if (!source) {
		warn("failed to create an image source for '%s'", path);
		return;
	}

	item.path = bstrdup(path);
	item.source = source;
	da_push_back(*new_items, &item);
}

static int compare_paths(const void *a, const void *b)
{
	return astrcmpi(*(const char *const *)a, *(const char *const *)b);
}

/* Adds every image in `path`. Returns false if it is not a directory. */
static bool add_directory(struct picture_gallery *pg, pg_item_array_t *new_items, const char *path)
{
	DARRAY(char *) entries;
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
		if (!si_is_image_extension(os_get_path_extension(ent->d_name)))
			continue;

		entry = bstrdup(ent->d_name);
		da_push_back(entries, &entry);

		if (entries.num >= MAX_DIR_FILES) {
			warn("directory '%s' holds more than %d images, ignoring the rest", path, MAX_DIR_FILES);
			break;
		}
	}

	os_closedir(dir);

	/* directory order is filesystem dependent - sort so the gallery plays
	 * in the same order on every machine and every launch */
	if (entries.num > 1)
		qsort(entries.array, entries.num, sizeof(char *), compare_paths);

	for (size_t i = 0; i < entries.num; i++) {
		dstr_copy(&file_path, path);
		dstr_cat_ch(&file_path, '/');
		dstr_cat(&file_path, entries.array[i]);
		add_item(pg, new_items, file_path.array);
		bfree(entries.array[i]);
	}

	dstr_free(&file_path);
	da_free(entries);

	return true;
}

/* ------------------------------------------------------------------------- */
/* running order                                                              */

static void build_order(struct picture_gallery *pg)
{
	size_t count = pg->items.num;

	da_resize(pg->order, count);

	for (size_t i = 0; i < count; i++)
		pg->order.array[i] = i;

	if (!pg->shuffle || count < 2)
		return;

	/* Fisher-Yates, so every image comes up exactly once per pass */
	for (size_t i = count - 1; i > 0; i--) {
		size_t j = (size_t)(rand() % (int)(i + 1));
		size_t tmp = pg->order.array[i];

		pg->order.array[i] = pg->order.array[j];
		pg->order.array[j] = tmp;
	}
}

/* Reshuffle for the next pass without repeating an image across the loop. */
static void reshuffle_order(struct picture_gallery *pg)
{
	size_t last = pg->order.num ? pg->order.array[pg->order.num - 1] : 0;

	build_order(pg);

	if (pg->order.num > 1 && pg->order.array[0] == last) {
		size_t tmp = pg->order.array[0];

		pg->order.array[0] = pg->order.array[pg->order.num - 1];
		pg->order.array[pg->order.num - 1] = tmp;
	}
}

static inline bool item_valid(struct picture_gallery *pg)
{
	return pg->items.num && pg->order.num && pg->order_pos < pg->order.num &&
	       pg->order.array[pg->order_pos] < pg->items.num;
}

static inline struct pg_item *current_item(struct picture_gallery *pg)
{
	if (!item_valid(pg))
		return NULL;

	return &pg->items.array[pg->order.array[pg->order_pos]];
}

/*
 * Moves the position one image along. Returns true when it wrapped around the
 * end of the list (or the start, going backwards). Mutex must be held.
 */
static bool move_position(struct picture_gallery *pg, bool forward)
{
	if (!pg->order.num)
		return false;

	if (forward) {
		if (pg->order_pos + 1 >= pg->order.num) {
			if (pg->shuffle)
				reshuffle_order(pg);

			pg->order_pos = 0;
			return true;
		}

		pg->order_pos++;
		return false;
	}

	if (pg->order_pos == 0) {
		pg->order_pos = pg->order.num - 1;
		return true;
	}

	pg->order_pos--;
	return false;
}

/* ------------------------------------------------------------------------- */
/* transitions                                                                */

/* The slot that actually plays for direction `id`: the Loop wrap direction
 * may borrow the normal one. Mutex must be held. */
static struct pg_slot *resolve_slot(struct picture_gallery *pg, enum pg_slot_id id)
{
	struct pg_slot *slot = &pg->slots[id];

	if (!slot->tr || slot->use_normal)
		slot = &pg->slots[PG_SLOT_NORMAL];

	return slot->tr ? slot : NULL;
}

/*
 * Make `slot` the one on screen. Whatever the current slot shows is handed to
 * the new one first, so its transition starts from the right picture, and the
 * old one lets go of it. Mutex must be held.
 */
static void switch_to_slot(struct picture_gallery *pg, struct pg_slot *slot)
{
	obs_source_t *showing = NULL;

	if (pg->cur == slot)
		return;

	if (pg->cur && pg->cur->tr)
		showing = obs_transition_get_active_source(pg->cur->tr);

	obs_transition_set(slot->tr, showing);

	if (pg->cur && pg->cur->tr)
		obs_transition_clear(pg->cur->tr);

	obs_source_release(showing);
	pg->cur = slot;
}

/*
 * Run the transition for direction `id` to `dest` (NULL for nothing).
 * Mutex must be held.
 */
static void run_transition(struct picture_gallery *pg, enum pg_slot_id id, obs_source_t *dest)
{
	struct pg_slot *slot = resolve_slot(pg, id);

	if (!slot)
		return;

	switch_to_slot(pg, slot);

	os_atomic_set_bool(&slot->done, false);

	if (slot->cut) {
		obs_transition_set(slot->tr, dest);
		os_atomic_set_bool(&slot->done, true);
	} else if (!obs_transition_start(slot->tr, OBS_TRANSITION_MODE_AUTO, slot->duration_ms, dest)) {
		/* libobs refuses a transition to whatever is already on screen
		 * (a single image gallery, the same file listed twice in a
		 * row, nothing to nothing) - swap it in directly */
		obs_transition_set(slot->tr, dest);
		os_atomic_set_bool(&slot->done, true);
	}

	pg->on_screen = dest != NULL;
}

/* Whether the transition that is running has finished. Mutex must be held. */
static bool transition_done(struct picture_gallery *pg)
{
	struct pg_slot *slot = pg->cur;
	float limit;

	if (!slot || !slot->tr || os_atomic_load_bool(&slot->done))
		return true;

	limit = (float)slot->duration_ms / 1000.0f + TRANSITION_GRACE;
	if (pg->elapsed >= limit) {
		warn("the %s transition did not report its end after %.1f seconds, moving on",
		     slot_desc[slot->id].prefix, limit);
		os_atomic_set_bool(&slot->done, true);
		return true;
	}

	return false;
}

static void transition_stopped(void *data, calldata_t *cd)
{
	struct pg_slot *slot = data;

	UNUSED_PARAMETER(cd);
	os_atomic_set_bool(&slot->done, true);
}

/* Applies the gallery's sizing to one of its transitions. */
static void apply_transition_layout(struct picture_gallery *pg, obs_source_t *tr)
{
	obs_transition_set_alignment(tr, pg->alignment);
	obs_transition_set_scale_type(tr, pg->scale_type);

	if (pg->cx && pg->cy)
		obs_transition_set_size(tr, pg->cx, pg->cy);
}

/* ------------------------------------------------------------------------- */
/* playback                                                                   */

/*
 * The calldata is a local, never a member: the signal is emitted with the mutex
 * held, and a handler that moves the gallery (a script calling next() on the
 * image it was just told about) re-enters and emits again from underneath.
 */
static void signal_image_changed(struct picture_gallery *pg, struct pg_item *item)
{
	signal_handler_t *sh = obs_source_get_signal_handler(pg->source);
	calldata_t cd;

	calldata_init(&cd);
	calldata_set_int(&cd, "index", (long long)pg->order.array[pg->order_pos]);
	calldata_set_int(&cd, "position", (long long)pg->order_pos);
	calldata_set_string(&cd, "path", item->path);

	signal_handler_signal(sh, "gallery_image_changed", &cd);

	calldata_free(&cd);
}

static inline void enter_phase(struct picture_gallery *pg, enum pg_phase phase)
{
	pg->phase = phase;
	pg->elapsed = 0.0f;
}

/* Bring the current image on screen with direction `id`. Mutex must be held. */
static void bring_in(struct picture_gallery *pg, enum pg_slot_id id)
{
	struct pg_item *item = current_item(pg);

	if (!item)
		return;

	run_transition(pg, id, item->source);
	enter_phase(pg, PG_IN);
	signal_image_changed(pg, item);
}

/* Take the gallery out to nothing with the Hide transition. Mutex must be
 * held. */
static void take_out(struct picture_gallery *pg, enum pg_after_out after)
{
	if (pg->phase == PG_OUT) {
		/* already on its way out - just change where it goes next */
		pg->after_out = after;
		return;
	}

	if (!pg->on_screen) {
		enter_phase(pg, after == PG_AFTER_GAP ? PG_GAP : PG_IDLE);
		return;
	}

	run_transition(pg, PG_SLOT_HIDE, NULL);
	pg->after_out = after;
	enter_phase(pg, PG_OUT);
}

/* Drop whatever is on screen at once, without a transition. Mutex must be
 * held. */
static void clear_now(struct picture_gallery *pg)
{
	if (pg->cur && pg->cur->tr) {
		obs_transition_set(pg->cur->tr, NULL);
		os_atomic_set_bool(&pg->cur->done, true);
	}

	pg->on_screen = false;
	enter_phase(pg, PG_IDLE);
}

/* Bring the current image in, from wherever the gallery is: with the normal
 * transition if a picture is up, with the Show transition if it is empty.
 * Mutex must be held. */
static void bring_in_from_anywhere(struct picture_gallery *pg)
{
	bool picture_up = pg->on_screen && pg->phase != PG_OUT;

	bring_in(pg, picture_up ? PG_SLOT_NORMAL : PG_SLOT_SHOW);
}

static void gallery_finished(struct picture_gallery *pg)
{
	signal_handler_t *sh = obs_source_get_signal_handler(pg->source);
	calldata_t cd;

	pg->finished = true;
	set_media_state(pg, OBS_MEDIA_STATE_ENDED);

	if (pg->hide_when_done)
		take_out(pg, PG_AFTER_IDLE);
	else
		enter_phase(pg, PG_IDLE);

	calldata_init(&cd);
	signal_handler_signal(sh, "gallery_ended", &cd);
	calldata_free(&cd);

	obs_source_media_ended(pg->source);
}

/* The current image's hold time ran out. Mutex must be held. */
static void advance_auto(struct picture_gallery *pg)
{
	bool wrapped;

	if (!pg->loop && pg->order_pos + 1 >= pg->order.num) {
		gallery_finished(pg);
		return;
	}

	wrapped = move_position(pg, true);

	if (pg->gap_time > 0.0f) {
		/* after the gap a wrap comes in with the Loop wrap transition
		 * when it has one of its own, everything else with Show */
		bool own_wrap = pg->slots[PG_SLOT_WRAP].tr && !pg->slots[PG_SLOT_WRAP].use_normal;

		pg->gap_in = wrapped && own_wrap ? PG_SLOT_WRAP : PG_SLOT_SHOW;
		take_out(pg, PG_AFTER_GAP);
	} else {
		bring_in(pg, wrapped ? PG_SLOT_WRAP : PG_SLOT_NORMAL);
	}
}

/* Next/Previous. Manual skips never wait out the gap. Mutex must be held. */
static void skip(struct picture_gallery *pg, bool forward)
{
	if (!pg->items.num || !pg->order.num)
		return;

	move_position(pg, forward);

	/* hidden with the Hide control: the position moves, nothing shows */
	if (pg->user_hidden)
		return;

	pg->stopped = false;
	pg->finished = false;

	if (!pg->paused)
		set_media_state(pg, OBS_MEDIA_STATE_PLAYING);

	/* not visible: it is brought in when the source is shown */
	if (!pg->showing) {
		enter_phase(pg, PG_IDLE);
		return;
	}

	bring_in_from_anywhere(pg);
}

/* Start over from the first image. Mutex must be held. */
static void restart_gallery(struct picture_gallery *pg)
{
	if (pg->shuffle)
		build_order(pg);

	pg->order_pos = 0;
	pg->finished = false;
	pg->stopped = false;
	pg->paused = false;
	pg->user_hidden = false;

	if (!pg->items.num) {
		set_media_state(pg, OBS_MEDIA_STATE_NONE);
		clear_now(pg);
		return;
	}

	set_media_state(pg, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(pg->source);

	if (pg->showing)
		bring_in_from_anywhere(pg);
	else
		enter_phase(pg, PG_IDLE);
}

/* Whether anything should be on screen at all. */
static inline bool wants_picture(struct picture_gallery *pg)
{
	return pg->items.num && !pg->stopped && !pg->user_hidden && !(pg->finished && pg->hide_when_done);
}

/* The source just became visible. Mutex must be held. */
static void handle_shown(struct picture_gallery *pg)
{
	if (pg->restart_on_show) {
		restart_gallery(pg);
		return;
	}

	if (!wants_picture(pg))
		return;

	bring_in(pg, PG_SLOT_SHOW);
}

/* ------------------------------------------------------------------------- */
/* sizing                                                                     */

/* Recalculates the source dimensions. Mutex must be held. */
static void update_size(struct picture_gallery *pg)
{
	uint32_t cx = pg->content_cx;
	uint32_t cy = pg->content_cy;

	for (size_t i = 0; i < pg->items.num; i++) {
		uint32_t item_cx = obs_source_get_width(pg->items.array[i].source);
		uint32_t item_cy = obs_source_get_height(pg->items.array[i].source);

		if (item_cx > cx)
			cx = item_cx;
		if (item_cy > cy)
			cy = item_cy;
	}

	/* never shrinks within a session, so a scene layout does not jump
	 * around while images load */
	pg->content_cx = cx;
	pg->content_cy = cy;

	if (!pg->auto_size) {
		if (pg->aspect_only) {
			if (cx && cy) {
				double cx_f = (double)cx;
				double cy_f = (double)cy;
				double old_aspect = cx_f / cy_f;
				double new_aspect = (double)pg->bound_cx / (double)pg->bound_cy;

				if (new_aspect > old_aspect)
					cx = (uint32_t)(cy_f * new_aspect);
				else
					cy = (uint32_t)(cx_f / new_aspect);
			}
		} else {
			cx = pg->bound_cx;
			cy = pg->bound_cy;
		}
	}

	if (cx == pg->cx && cy == pg->cy)
		return;

	pg->cx = cx;
	pg->cy = cy;

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		if (pg->slots[i].tr)
			obs_transition_set_size(pg->slots[i].tr, cx, cy);
	}
}

/* "1920x1080" is a fixed size, "16:9" is a fixed aspect ratio, and anything
 * else - including the localized "Automatic" entry - means automatic. */
static void parse_bounding_size(struct picture_gallery *pg, const char *val)
{
	int cx = 0;
	int cy = 0;

	pg->auto_size = true;
	pg->aspect_only = false;

	if (!val || !*val)
		return;

	if (sscanf(val, "%dx%d", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		pg->auto_size = false;
	} else if (sscanf(val, "%d:%d", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		pg->auto_size = false;
		pg->aspect_only = true;
	} else {
		return;
	}

	pg->bound_cx = (uint32_t)cx;
	pg->bound_cy = (uint32_t)cy;
}

/* ------------------------------------------------------------------------- */
/* settings                                                                   */

static const char *builtin_transition_id(const char *type, bool *is_cut)
{
	*is_cut = false;

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
	if (astrcmpi(type, TR_STINGER) == 0)
		return "obs_stinger_transition";

	return "fade_transition";
}

static obs_data_t *builtin_transition_settings(const char *tr_id, enum pg_slot_id id, obs_data_t *settings)
{
	obs_data_t *tr_settings = obs_data_create();
	char key[64];

	if (strcmp(tr_id, "swipe_transition") == 0 || strcmp(tr_id, "slide_transition") == 0) {
		obs_data_set_string(tr_settings, "direction",
				    obs_data_get_string(settings, slot_key(key, sizeof(key), id, SS_DIRECTION)));
		obs_data_set_bool(tr_settings, "swipe_in",
				  obs_data_get_bool(settings, slot_key(key, sizeof(key), id, SS_SWIPE_IN)));
	} else if (strcmp(tr_id, "fade_to_color_transition") == 0) {
		obs_data_set_int(tr_settings, "color",
				 obs_data_get_int(settings, slot_key(key, sizeof(key), id, SS_COLOR)));
		obs_data_set_int(tr_settings, "switch_point",
				 obs_data_get_int(settings, slot_key(key, sizeof(key), id, SS_SWITCH_POINT)));
	} else if (strcmp(tr_id, "obs_stinger_transition") == 0) {
		obs_data_set_string(tr_settings, "path",
				    obs_data_get_string(settings, slot_key(key, sizeof(key), id, SS_STINGER_PATH)));
		obs_data_set_int(tr_settings, "tp_type", 0 /* transition point in milliseconds */);
		obs_data_set_int(tr_settings, "transition_point",
				 obs_data_get_int(settings, slot_key(key, sizeof(key), id, SS_STINGER_POINT)));
		obs_data_set_bool(tr_settings, "hw_decode",
				  obs_data_get_bool(settings, slot_key(key, sizeof(key), id, SS_STINGER_HW)));
	}

	return tr_settings;
}

/* Follows edits made to a scene transition into the private copy of it. */
static void scene_transition_updated(void *data, calldata_t *cd)
{
	struct pg_slot *slot = data;
	struct picture_gallery *pg = slot->pg;
	obs_source_t *scene = calldata_ptr(cd, "source");
	obs_source_t *tr;
	obs_data_t *settings;

	if (!scene)
		return;

	pthread_mutex_lock(&pg->mutex);
	tr = obs_source_get_ref(slot->tr);
	pthread_mutex_unlock(&pg->mutex);

	if (!tr)
		return;

	settings = obs_source_get_settings(scene);
	obs_source_update(tr, settings);
	obs_data_release(settings);
	obs_source_release(tr);
}

static void watch_scene_transition(struct pg_slot *slot, obs_source_t *scene)
{
	obs_source_t *old = obs_weak_source_get_source(slot->scene);

	if (old == scene) {
		obs_source_release(old);
		return;
	}

	if (old) {
		signal_handler_disconnect(obs_source_get_signal_handler(old), "update", scene_transition_updated, slot);
		obs_source_release(old);
	}

	obs_weak_source_release(slot->scene);
	slot->scene = NULL;

	if (scene) {
		slot->scene = obs_source_get_weak_source(scene);
		signal_handler_connect(obs_source_get_signal_handler(scene), "update", scene_transition_updated, slot);
	}
}

/* Lets go of a transition that is no longer used. No lock may be held.
 * `destroying` skips detaching it from a gallery that is going away anyway. */
static void retire_transition(struct picture_gallery *pg, struct pg_slot *slot, obs_source_t *tr, bool destroying)
{
	if (!tr)
		return;

	signal_handler_disconnect(obs_source_get_signal_handler(tr), "transition_stop", transition_stopped, slot);

	if (!destroying) {
		obs_source_remove_active_child(pg->source, tr);
		obs_transition_clear(tr);
	}

	obs_source_release(tr);
}

/*
 * Brings one direction's transition in line with the settings: picks the kind
 * of transition, creates a fresh one when the kind changed, and hands it its
 * settings when they moved.
 */
static void update_slot(struct picture_gallery *pg, struct pg_slot *slot, obs_data_t *settings)
{
	enum pg_slot_id id = slot->id;
	char key[64];
	const char *type = obs_data_get_string(settings, slot_key(key, sizeof(key), id, SS_TYPE));
	obs_source_t *scene = NULL;
	obs_source_t *new_tr = NULL;
	obs_source_t *old_tr = NULL;
	obs_data_t *tr_settings;
	const char *tr_id;
	struct dstr sig = {0};
	bool is_cut = false;
	bool settings_moved;

	/* ---- "same as normal" ---- */

	if (slot_desc[id].allow_same && (!*type || astrcmpi(type, TR_SAME) == 0)) {
		watch_scene_transition(slot, NULL);

		pthread_mutex_lock(&pg->mutex);
		slot->use_normal = true;
		if (slot->tr) {
			if (pg->cur == slot && pg->slots[PG_SLOT_NORMAL].tr)
				switch_to_slot(pg, &pg->slots[PG_SLOT_NORMAL]);
			else if (pg->cur == slot)
				pg->cur = NULL;

			old_tr = slot->tr;
			slot->tr = NULL;
		}
		pthread_mutex_unlock(&pg->mutex);

		retire_transition(pg, slot, old_tr, false);
		bfree(slot->tr_id);
		slot->tr_id = NULL;
		bfree(slot->sig);
		slot->sig = NULL;
		return;
	}

	/* ---- which transition, with which settings ---- */

	if (astrcmpi(type, TR_SCENE) == 0) {
		const char *name = obs_data_get_string(settings, slot_key(key, sizeof(key), id, SS_SCENE));

		scene = st_find(name);
		dstr_printf(&sig, "scene|%s|%p|", name ? name : "", (void *)scene);

		if (scene) {
			tr_id = obs_source_get_id(scene);
			tr_settings = obs_source_get_settings(scene);
			is_cut = strcmp(tr_id, "cut_transition") == 0;
		} else {
			/* Not loaded yet (OBS loads sources before its scene
			 * transitions), renamed or removed: fade until it can be
			 * found again. Only worth a warning once the list is in
			 * and the situation is new. */
			if (name && *name && st_have_list() &&
			    (!slot->sig || strncmp(slot->sig, sig.array, sig.len) != 0))
				warn("scene transition '%s' for the %s transition was not found, using a fade until it is",
				     name, slot_desc[id].prefix);

			tr_id = "fade_transition";
			tr_settings = obs_data_create();
		}
	} else {
		tr_id = builtin_transition_id(type, &is_cut);
		tr_settings = builtin_transition_settings(tr_id, id, settings);
		dstr_printf(&sig, "builtin|");
	}

	dstr_cat(&sig, tr_id);
	dstr_cat_ch(&sig, '|');
	dstr_cat(&sig, obs_data_get_json(tr_settings));

	watch_scene_transition(slot, scene);

	/* ---- a new transition when the kind changed ---- */

	if (!slot->tr || !slot->tr_id || strcmp(slot->tr_id, tr_id) != 0) {
		new_tr = obs_source_create_private(tr_id, NULL, tr_settings);

		if (new_tr) {
			signal_handler_connect(obs_source_get_signal_handler(new_tr), "transition_stop",
					       transition_stopped, slot);
			apply_transition_layout(pg, new_tr);
			obs_source_add_active_child(pg->source, new_tr);
		} else {
			warn("failed to create the %s transition '%s'", slot_desc[id].prefix, tr_id);
		}
	}

	settings_moved = !slot->sig || strcmp(slot->sig, sig.array) != 0;

	pthread_mutex_lock(&pg->mutex);

	slot->use_normal = false;
	slot->cut = is_cut;
	slot->duration_ms = (uint32_t)obs_data_get_int(settings, slot_key(key, sizeof(key), id, SS_DURATION));

	if (new_tr) {
		old_tr = slot->tr;

		/* the slot on screen keeps its picture across the swap */
		if (pg->cur == slot && old_tr) {
			obs_source_t *showing = obs_transition_get_active_source(old_tr);

			obs_transition_set(new_tr, showing);
			obs_source_release(showing);
			os_atomic_set_bool(&slot->done, true);
		}

		slot->tr = new_tr;
		bfree(slot->tr_id);
		slot->tr_id = bstrdup(tr_id);
	}

	pthread_mutex_unlock(&pg->mutex);

	retire_transition(pg, slot, old_tr, false);

	/* A stinger reopens its video every time it is updated, so settings
	 * are only handed over when they moved. A brand new transition was
	 * created with them already. */
	if (!new_tr && settings_moved && slot->tr)
		obs_source_update(slot->tr, tr_settings);

	bfree(slot->sig);
	slot->sig = bstrdup(sig.array);

	dstr_free(&sig);
	obs_data_release(tr_settings);
	obs_source_release(scene);
}

static void read_configured_paths(obs_data_t *settings, pg_path_array_t *paths)
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

static bool paths_equal(const pg_path_array_t *a, const pg_path_array_t *b)
{
	if (a->num != b->num)
		return false;

	for (size_t i = 0; i < a->num; i++) {
		if (strcmp(a->array[i], b->array[i]) != 0)
			return false;
	}

	return true;
}

static void pg_update(void *data, obs_data_t *settings)
{
	struct picture_gallery *pg = data;
	pg_item_array_t new_items;
	pg_item_array_t old_items;
	pg_path_array_t paths;
	const char *scale_mode;
	bool first_update = !pg->first_update_done;
	bool shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	bool reload_items;
	bool reload_order;

	da_init(new_items);
	da_init(old_items);
	da_init(paths);

	/* ---- plain settings ---- */

	pthread_mutex_lock(&pg->mutex);

	reload_order = shuffle != pg->shuffle;

	pg->hold_time = (float)obs_data_get_int(settings, S_HOLD_TIME) / 1000.0f;
	pg->gap_time = (float)obs_data_get_int(settings, S_GAP_TIME) / 1000.0f;
	pg->loop = obs_data_get_bool(settings, S_LOOP);
	pg->shuffle = shuffle;
	pg->hide_when_done = obs_data_get_bool(settings, S_HIDE_DONE);
	pg->restart_on_show = obs_data_get_bool(settings, S_RESTART_ON_SHOW);

	parse_bounding_size(pg, obs_data_get_string(settings, S_BOUNDING));

	scale_mode = obs_data_get_string(settings, S_SCALE_MODE);
	if (astrcmpi(scale_mode, SCALE_STRETCH) == 0)
		pg->scale_type = OBS_TRANSITION_SCALE_STRETCH;
	else if (astrcmpi(scale_mode, SCALE_DOWN_ONLY) == 0)
		pg->scale_type = OBS_TRANSITION_SCALE_MAX_ONLY;
	else
		pg->scale_type = OBS_TRANSITION_SCALE_ASPECT;

	pg->alignment = si_alignment_from_string(obs_data_get_string(settings, S_ALIGNMENT));

	pthread_mutex_unlock(&pg->mutex);

	/* ---- transitions ---- */

	for (size_t i = 0; i < PG_NUM_SLOTS; i++)
		update_slot(pg, &pg->slots[i], settings);

	/* ---- images ---- */

	/* Expanding directories walks the filesystem, so the images are only
	 * rebuilt when the list itself changed - not for every step of a
	 * slider drag elsewhere in the properties. */
	read_configured_paths(settings, &paths);
	reload_items = first_update || !paths_equal(&paths, &pg->configured_paths);

	if (reload_items) {
		for (size_t i = 0; i < paths.num; i++) {
			if (!add_directory(pg, &new_items, paths.array[i]))
				add_item(pg, &new_items, paths.array[i]);
		}

		free_paths(&pg->configured_paths);
		pg->configured_paths = paths;
	} else {
		free_paths(&paths);
	}

	/* ---- swap in ---- */

	pthread_mutex_lock(&pg->mutex);

	if (reload_items) {
		/* nothing from the old list may stay on screen */
		clear_now(pg);

		old_items = pg->items;
		pg->items = new_items;

		pg->cx = 0;
		pg->cy = 0;
		pg->content_cx = 0;
		pg->content_cy = 0;
	}

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		if (pg->slots[i].tr)
			apply_transition_layout(pg, pg->slots[i].tr);
	}

	update_size(pg);

	if (reload_items || reload_order) {
		build_order(pg);
		pg->order_pos = 0;
		/* this brings the first image in itself */
		pg->show_pending = false;

		if (pg->items.num) {
			pg->stopped = false;
			pg->paused = false;
			pg->finished = false;
			set_media_state(pg, OBS_MEDIA_STATE_PLAYING);
			obs_source_media_started(pg->source);

			if (pg->showing && wants_picture(pg))
				bring_in_from_anywhere(pg);
		} else {
			set_media_state(pg, OBS_MEDIA_STATE_NONE);
			clear_now(pg);
		}
	}

	pg->first_update_done = true;

	pthread_mutex_unlock(&pg->mutex);

	free_items(&old_items);
}

/* ------------------------------------------------------------------------- */
/* media controls                                                             */

static enum obs_media_state pg_get_state(void *data)
{
	struct picture_gallery *pg = data;
	return pg->state;
}

static void pg_play_pause(void *data, bool pause)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);

	/* pressing play after a stop, or after a non-looping gallery ran out,
	 * starts it over */
	if (!pause && (pg->stopped || pg->finished)) {
		restart_gallery(pg);
		pthread_mutex_unlock(&pg->mutex);
		return;
	}

	pg->paused = pause;
	set_media_state(pg, pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING);

	pthread_mutex_unlock(&pg->mutex);
}

static void pg_restart(void *data)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	restart_gallery(pg);
	pthread_mutex_unlock(&pg->mutex);
}

static void pg_stop(void *data)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);

	pg->order_pos = 0;
	pg->stopped = true;
	pg->paused = false;
	pg->finished = false;
	set_media_state(pg, OBS_MEDIA_STATE_STOPPED);

	if (pg->showing)
		take_out(pg, PG_AFTER_IDLE);
	else
		clear_now(pg);

	pthread_mutex_unlock(&pg->mutex);
}

static void pg_next(void *data)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	skip(pg, true);
	pthread_mutex_unlock(&pg->mutex);
}

static void pg_previous(void *data)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	skip(pg, false);
	pthread_mutex_unlock(&pg->mutex);
}

static int64_t pg_get_duration(void *data)
{
	struct picture_gallery *pg = data;
	int64_t duration;

	pthread_mutex_lock(&pg->mutex);
	duration = item_valid(pg) ? (int64_t)(pg->hold_time * 1000.0f) : 0;
	pthread_mutex_unlock(&pg->mutex);

	return duration;
}

static int64_t pg_get_time(void *data)
{
	struct picture_gallery *pg = data;
	int64_t time = 0;

	pthread_mutex_lock(&pg->mutex);
	if (pg->phase == PG_HOLD)
		time = (int64_t)(pg->elapsed * 1000.0f);
	else if (pg->phase == PG_OUT || pg->phase == PG_GAP || (pg->phase == PG_IDLE && pg->finished))
		time = (int64_t)(pg->hold_time * 1000.0f);
	pthread_mutex_unlock(&pg->mutex);

	return time;
}

static void pg_set_time(void *data, int64_t ms)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	if (pg->phase == PG_HOLD)
		pg->elapsed = (float)ms / 1000.0f;
	pthread_mutex_unlock(&pg->mutex);
}

/* ------------------------------------------------------------------------- */
/* show / hide controls                                                       */

/* Bring the gallery back after the Hide control. */
static void show_gallery(struct picture_gallery *pg)
{
	pthread_mutex_lock(&pg->mutex);

	if (pg->user_hidden) {
		pg->user_hidden = false;

		if (pg->stopped || pg->finished) {
			restart_gallery(pg);
		} else if (pg->showing && pg->items.num) {
			if (!pg->paused)
				set_media_state(pg, OBS_MEDIA_STATE_PLAYING);
			bring_in(pg, PG_SLOT_SHOW);
		}
	}

	pthread_mutex_unlock(&pg->mutex);
}

/* Take the gallery out with its Hide transition and keep it out until it is
 * shown again. The position is kept. */
static void hide_gallery(struct picture_gallery *pg)
{
	pthread_mutex_lock(&pg->mutex);

	if (!pg->user_hidden) {
		pg->user_hidden = true;

		if (pg->showing)
			take_out(pg, PG_AFTER_IDLE);
		else
			clear_now(pg);
	}

	pthread_mutex_unlock(&pg->mutex);
}

/* ------------------------------------------------------------------------- */
/* hotkeys                                                                    */

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct picture_gallery *pg = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(pg->source))
		obs_source_media_play_pause(pg->source, !pg->paused);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct picture_gallery *pg = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(pg->source))
		obs_source_media_restart(pg->source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct picture_gallery *pg = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(pg->source))
		obs_source_media_stop(pg->source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct picture_gallery *pg = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(pg->source))
		obs_source_media_next(pg->source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct picture_gallery *pg = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed && obs_source_showing(pg->source))
		obs_source_media_previous(pg->source);
}

static void show_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		show_gallery(data);
}

static void hide_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		hide_gallery(data);
}

/* ------------------------------------------------------------------------- */
/* procs                                                                      */

static void current_index_proc(void *data, calldata_t *cd)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	calldata_set_int(cd, "current_index", item_valid(pg) ? (long long)pg->order.array[pg->order_pos] : -1);
	pthread_mutex_unlock(&pg->mutex);
}

static void current_path_proc(void *data, calldata_t *cd)
{
	struct picture_gallery *pg = data;
	struct pg_item *item;

	pthread_mutex_lock(&pg->mutex);
	item = current_item(pg);
	calldata_set_string(cd, "current_path", item ? item->path : "");
	pthread_mutex_unlock(&pg->mutex);
}

static void total_items_proc(void *data, calldata_t *cd)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	calldata_set_int(cd, "total_items", (long long)pg->items.num);
	pthread_mutex_unlock(&pg->mutex);
}

static void show_gallery_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	show_gallery(data);
}

static void hide_gallery_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	hide_gallery(data);
}

static void is_hidden_proc(void *data, calldata_t *cd)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	calldata_set_bool(cd, "hidden", pg->user_hidden);
	pthread_mutex_unlock(&pg->mutex);
}

/* ------------------------------------------------------------------------- */
/* source callbacks                                                           */

static const char *pg_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_PICTURE_GALLERY;
}

static void pg_destroy(void *data)
{
	struct picture_gallery *pg = data;

	st_register(pg, false);

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		struct pg_slot *slot = &pg->slots[i];

		watch_scene_transition(slot, NULL);
		retire_transition(pg, slot, slot->tr, true);
		slot->tr = NULL;
		bfree(slot->tr_id);
		bfree(slot->sig);
	}

	free_items(&pg->items);
	da_free(pg->order);
	free_paths(&pg->configured_paths);

	pthread_mutex_destroy(&pg->mutex);
	bfree(pg);
}

static void *pg_create(obs_data_t *settings, obs_source_t *source)
{
	struct picture_gallery *pg = bzalloc(sizeof(struct picture_gallery));
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	signal_handler_t *sh = obs_source_get_signal_handler(source);

	pg->source = source;
	pg->state = OBS_MEDIA_STATE_NONE;
	pg->phase = PG_IDLE;
	pg->scale_type = OBS_TRANSITION_SCALE_ASPECT;
	pg->alignment = OBS_ALIGN_CENTER;
	pg->auto_size = true;

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		pg->slots[i].pg = pg;
		pg->slots[i].id = (enum pg_slot_id)i;
		pg->slots[i].done = true;
	}

	da_init(pg->items);
	da_init(pg->order);
	da_init(pg->configured_paths);

	pthread_mutex_init_value(&pg->mutex);
	if (pthread_mutex_init_recursive(&pg->mutex) != 0) {
		bfree(pg);
		return NULL;
	}

	pg->play_pause_hotkey =
		obs_hotkey_register_source(source, "PictureGallery.PlayPause", T_HK_PLAY_PAUSE, play_pause_hotkey, pg);
	pg->restart_hotkey =
		obs_hotkey_register_source(source, "PictureGallery.Restart", T_HK_RESTART, restart_hotkey, pg);
	pg->stop_hotkey = obs_hotkey_register_source(source, "PictureGallery.Stop", T_HK_STOP, stop_hotkey, pg);
	pg->next_hotkey = obs_hotkey_register_source(source, "PictureGallery.Next", T_HK_NEXT, next_hotkey, pg);
	pg->prev_hotkey =
		obs_hotkey_register_source(source, "PictureGallery.Previous", T_HK_PREVIOUS, previous_hotkey, pg);
	pg->show_hotkey = obs_hotkey_register_source(source, "PictureGallery.Show", T_HK_SHOW, show_hotkey, pg);
	pg->hide_hotkey = obs_hotkey_register_source(source, "PictureGallery.Hide", T_HK_HIDE, hide_hotkey, pg);

	proc_handler_add(ph, "void current_index(out int current_index)", current_index_proc, pg);
	proc_handler_add(ph, "void current_path(out string current_path)", current_path_proc, pg);
	proc_handler_add(ph, "void total_items(out int total_items)", total_items_proc, pg);
	proc_handler_add(ph, "void show_gallery()", show_gallery_proc, pg);
	proc_handler_add(ph, "void hide_gallery()", hide_gallery_proc, pg);
	proc_handler_add(ph, "void is_hidden(out bool hidden)", is_hidden_proc, pg);

	signal_handler_add(sh, "void gallery_image_changed(int index, int position, string path)");
	signal_handler_add(sh, "void gallery_ended()");

	st_register(pg, true);

	obs_source_update(source, NULL);

	UNUSED_PARAMETER(settings);
	return pg;
}

static void pg_video_render(void *data, gs_effect_t *effect)
{
	struct picture_gallery *pg = data;
	obs_source_t *transition = get_current_transition(pg);

	if (transition) {
		obs_source_video_render(transition);
		obs_source_release(transition);
	}

	UNUSED_PARAMETER(effect);
}

static void pg_video_tick(void *data, float seconds)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);

	pg->size_timer += seconds;
	if (pg->size_timer >= SIZE_UPDATE_INTERVAL) {
		pg->size_timer = 0.0f;
		update_size(pg);
	}

	if (!pg->items.num) {
		pg->show_pending = false;
		if (pg->on_screen)
			clear_now(pg);
		goto finish;
	}

	if (pg->show_pending && pg->showing) {
		pg->show_pending = false;
		handle_shown(pg);
		goto finish;
	}

	/* Nothing moves while nobody can see it, so the gallery holds its
	 * place instead of running through the rotation off screen. */
	if (!pg->showing || pg->paused)
		goto finish;

	pg->elapsed += seconds;

	switch (pg->phase) {
	case PG_IDLE:
		break;

	case PG_IN:
		if (transition_done(pg))
			enter_phase(pg, PG_HOLD);
		break;

	case PG_HOLD:
		if (!pg->finished && !pg->stopped && !pg->user_hidden && pg->elapsed >= pg->hold_time)
			advance_auto(pg);
		break;

	case PG_OUT:
		if (transition_done(pg))
			enter_phase(pg, pg->after_out == PG_AFTER_GAP ? PG_GAP : PG_IDLE);
		break;

	case PG_GAP:
		if (pg->elapsed >= pg->gap_time)
			bring_in(pg, pg->gap_in);
		break;
	}

finish:
	pthread_mutex_unlock(&pg->mutex);
}

static void pg_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct picture_gallery *pg = data;

	/* every direction's transition is a child: an idle one holds nothing
	 * and costs nothing, and keeping them all active means a stinger has
	 * its video open before it is needed */
	pthread_mutex_lock(&pg->mutex);
	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		if (pg->slots[i].tr)
			cb(pg->source, pg->slots[i].tr, param);
	}
	pthread_mutex_unlock(&pg->mutex);
}

static uint32_t pg_get_width(void *data)
{
	struct picture_gallery *pg = data;
	return pg->cx;
}

static uint32_t pg_get_height(void *data)
{
	struct picture_gallery *pg = data;
	return pg->cy;
}

static void pg_show(void *data)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);
	pg->showing = true;
	/* deferred to the next tick, by which point the children are showing
	 * too and the transition can actually run */
	pg->show_pending = true;
	pthread_mutex_unlock(&pg->mutex);
}

static void pg_hide(void *data)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);

	pg->showing = false;
	pg->show_pending = false;

	/* Nobody is watching, so there is nothing to animate: drop the picture
	 * now, and the Show transition brings it back when the source is
	 * visible again rather than it popping in. */
	clear_now(pg);

	pthread_mutex_unlock(&pg->mutex);
}

static bool pg_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			    size_t channels, size_t sample_rate)
{
	struct picture_gallery *pg = data;
	obs_source_t *transition = get_current_transition(pg);
	struct obs_source_audio_mix child_audio;
	uint64_t source_ts;
	bool success = false;

	UNUSED_PARAMETER(sample_rate);

	/* the images are silent; this is a stinger's own sound */
	if (!transition)
		return false;

	if (obs_source_audio_pending(transition))
		goto finish;

	source_ts = obs_source_get_audio_timestamp(transition);
	if (!source_ts)
		goto finish;

	obs_source_get_audio_mix(transition, &child_audio);

	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++)
			memcpy(audio_output->output[mix].data[ch], child_audio.output[mix].data[ch],
			       AUDIO_OUTPUT_FRAMES * sizeof(float));
	}

	*ts_out = source_ts;
	success = true;

finish:
	obs_source_release(transition);
	return success;
}

static void pg_defaults(obs_data_t *settings)
{
	char key[64];

	obs_data_set_default_int(settings, S_HOLD_TIME, 5000);
	obs_data_set_default_int(settings, S_GAP_TIME, 0);
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_SHUFFLE, false);
	obs_data_set_default_bool(settings, S_HIDE_DONE, false);
	obs_data_set_default_bool(settings, S_RESTART_ON_SHOW, true);

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		enum pg_slot_id id = (enum pg_slot_id)i;

		obs_data_set_default_string(settings, slot_key(key, sizeof(key), id, SS_TYPE),
					    slot_desc[i].default_type);
		obs_data_set_default_int(settings, slot_key(key, sizeof(key), id, SS_DURATION),
					 slot_desc[i].default_duration);
		obs_data_set_default_string(settings, slot_key(key, sizeof(key), id, SS_DIRECTION), "left");
		obs_data_set_default_bool(settings, slot_key(key, sizeof(key), id, SS_SWIPE_IN), false);
		obs_data_set_default_int(settings, slot_key(key, sizeof(key), id, SS_COLOR), 0xFF000000);
		obs_data_set_default_int(settings, slot_key(key, sizeof(key), id, SS_SWITCH_POINT), 50);
		obs_data_set_default_int(settings, slot_key(key, sizeof(key), id, SS_STINGER_POINT), 1000);
		obs_data_set_default_bool(settings, slot_key(key, sizeof(key), id, SS_STINGER_HW), true);
	}

	obs_data_set_default_string(settings, S_BOUNDING, T_BOUNDING_AUTO);
	obs_data_set_default_string(settings, S_SCALE_MODE, SCALE_FIT);
	obs_data_set_default_string(settings, S_ALIGNMENT, "center");
}

/* ------------------------------------------------------------------------- */
/* properties                                                                 */

static void set_visible(obs_properties_t *props, enum pg_slot_id id, const char *suffix, bool visible)
{
	char key[64];
	obs_property_t *p = obs_properties_get(props, slot_key(key, sizeof(key), id, suffix));

	if (p)
		obs_property_set_visible(p, visible);
}

static bool slot_type_changed(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const struct pg_slot_desc *desc = priv;
	enum pg_slot_id id = (enum pg_slot_id)(desc - slot_desc);
	char key[64];
	const char *type = obs_data_get_string(settings, slot_key(key, sizeof(key), id, SS_TYPE));
	bool same = desc->allow_same && (!*type || astrcmpi(type, TR_SAME) == 0);
	bool scene = astrcmpi(type, TR_SCENE) == 0;
	bool is_cut = !same && astrcmpi(type, TR_CUT) == 0;
	bool is_swipe = !same && astrcmpi(type, TR_SWIPE) == 0;
	bool is_slide = !same && astrcmpi(type, TR_SLIDE) == 0;
	bool is_stinger = !same && astrcmpi(type, TR_STINGER) == 0;
	bool to_color = !same && astrcmpi(type, TR_FADE_TO_COLOR) == 0;

	UNUSED_PARAMETER(p);

	set_visible(props, id, SS_SCENE, scene);
	set_visible(props, id, SS_DURATION, !same && !is_cut && !is_stinger);
	set_visible(props, id, SS_DIRECTION, is_swipe || is_slide);
	set_visible(props, id, SS_SWIPE_IN, is_swipe);
	set_visible(props, id, SS_COLOR, to_color);
	set_visible(props, id, SS_SWITCH_POINT, to_color);
	set_visible(props, id, SS_STINGER_PATH, is_stinger);
	set_visible(props, id, SS_STINGER_POINT, is_stinger);
	set_visible(props, id, SS_STINGER_HW, is_stinger);

	return true;
}

/* clang-format off */
static const char *image_filter =
	"Image files (*.bmp *.tga *.png *.jpg *.jpeg *.jpe *.jfif *.gif *.webp *.psd *.tif *.tiff"
#ifdef _WIN32
	" *.jxr"
#endif
	");;All files (*.*)";

static const char *stinger_filter = "Video files (*.mp4 *.m4v *.mov *.mkv *.webm *.ts *.avi *.flv);;All files (*.*)";
/* clang-format on */

static const char *aspects[] = {"16:9", "16:10", "4:3", "1:1", "9:16"};

#define NUM_ASPECTS (sizeof(aspects) / sizeof(const char *))

/* The names of the scene transitions configured in OBS, plus the one this
 * direction is set to if it has gone missing, so the setting is not silently
 * replaced by whatever happens to be first in the list. */
static void fill_scene_transitions(obs_property_t *p, const char *current)
{
	bool found = false;

	if (st_mutex_ready) {
		pthread_mutex_lock(&st_mutex);
		for (size_t i = 0; i < st_cache.num; i++) {
			const char *name = st_cache.array[i].name;

			obs_property_list_add_string(p, name, name);
			if (current && strcmp(current, name) == 0)
				found = true;
		}
		pthread_mutex_unlock(&st_mutex);
	}

	if (current && *current && !found) {
		struct dstr label = {0};

		dstr_printf(&label, "%s %s", current, T_TR_SCENE_MISSING);
		obs_property_list_add_string(p, label.array, current);
		dstr_free(&label);
	}
}

static void add_slot_properties(obs_properties_t *props, enum pg_slot_id id, obs_data_t *settings)
{
	const struct pg_slot_desc *desc = &slot_desc[id];
	obs_properties_t *group = obs_properties_create();
	struct dstr text = {0};
	obs_property_t *p;
	char key[64];

	p = obs_properties_add_list(group, slot_key(key, sizeof(key), id, SS_TYPE), T_TR_TYPE, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	if (desc->allow_same)
		obs_property_list_add_string(p, T_TR_SAME, TR_SAME);
	obs_property_list_add_string(p, T_TR_CUT, TR_CUT);
	obs_property_list_add_string(p, T_TR_FADE, TR_FADE);
	obs_property_list_add_string(p, T_TR_SWIPE, TR_SWIPE);
	obs_property_list_add_string(p, T_TR_SLIDE, TR_SLIDE);
	obs_property_list_add_string(p, T_TR_FADE_TO_COLOR, TR_FADE_TO_COLOR);
	obs_property_list_add_string(p, T_TR_STINGER, TR_STINGER);
#ifdef HAVE_OBS_FRONTEND_API
	obs_property_list_add_string(p, T_TR_SCENE, TR_SCENE);
#endif
	obs_property_set_modified_callback2(p, slot_type_changed, (void *)desc);

	p = obs_properties_add_list(group, slot_key(key, sizeof(key), id, SS_SCENE), T_TR_SCENE_NAME,
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(p, T_TR_SCENE_NAME_DESC);
	fill_scene_transitions(p, settings ? obs_data_get_string(settings, key) : NULL);

	p = obs_properties_add_int(group, slot_key(key, sizeof(key), id, SS_DURATION), T_TR_DURATION, 0, 60000, 50);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(group, slot_key(key, sizeof(key), id, SS_DIRECTION), T_TR_DIRECTION,
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_DIR_LEFT, "left");
	obs_property_list_add_string(p, T_DIR_RIGHT, "right");
	obs_property_list_add_string(p, T_DIR_UP, "up");
	obs_property_list_add_string(p, T_DIR_DOWN, "down");

	obs_properties_add_bool(group, slot_key(key, sizeof(key), id, SS_SWIPE_IN), T_TR_SWIPE_IN);

	obs_properties_add_color(group, slot_key(key, sizeof(key), id, SS_COLOR), T_TR_COLOR);
	p = obs_properties_add_int_slider(group, slot_key(key, sizeof(key), id, SS_SWITCH_POINT), T_TR_SWITCH_POINT, 0,
					  100, 1);
	obs_property_int_set_suffix(p, " %");

	obs_properties_add_path(group, slot_key(key, sizeof(key), id, SS_STINGER_PATH), T_TR_STINGER_PATH,
				OBS_PATH_FILE, stinger_filter, NULL);
	p = obs_properties_add_int(group, slot_key(key, sizeof(key), id, SS_STINGER_POINT), T_TR_STINGER_POINT, 0,
				   120000, 50);
	obs_property_int_set_suffix(p, " ms");
	obs_properties_add_bool(group, slot_key(key, sizeof(key), id, SS_STINGER_HW), T_TR_STINGER_HW);

	dstr_copy(&text, "PictureGallery.");
	dstr_cat(&text, desc->group_text);
	p = obs_properties_add_group(props, slot_key(key, sizeof(key), id, SS_GROUP), obs_module_text(text.array),
				     OBS_GROUP_NORMAL, group);

	dstr_copy(&text, "PictureGallery.");
	dstr_cat(&text, desc->group_desc);
	obs_property_set_long_description(p, obs_module_text(text.array));

	dstr_free(&text);
}

static obs_properties_t *pg_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *playback = obs_properties_create();
	obs_properties_t *appearance = obs_properties_create();
	struct picture_gallery *pg = data;
	obs_data_t *settings = pg ? obs_source_get_settings(pg->source) : NULL;
	struct obs_video_info ovi;
	struct dstr last_dir = {0};
	obs_property_t *p;
	char res[32];

	/* ----------------- images ----------------- */

	if (pg) {
		pthread_mutex_lock(&pg->mutex);
		if (pg->items.num) {
			struct pg_item *last = da_end(pg->items);
			const char *slash;

			dstr_copy(&last_dir, last->path);
			dstr_replace(&last_dir, "\\", "/");
			slash = strrchr(last_dir.array, '/');
			if (slash)
				dstr_resize(&last_dir, slash - last_dir.array + 1);
		}
		pthread_mutex_unlock(&pg->mutex);
	}

	obs_properties_add_editable_list(props, S_FILES, T_FILES, OBS_EDITABLE_LIST_TYPE_FILES, image_filter,
					 last_dir.array);
	dstr_free(&last_dir);

	/* ----------------- playback ----------------- */

	p = obs_properties_add_int(playback, S_HOLD_TIME, T_HOLD_TIME, 100, 3600000, 100);
	obs_property_int_set_suffix(p, " ms");
	obs_property_set_long_description(p, T_HOLD_TIME_DESC);

	p = obs_properties_add_int(playback, S_GAP_TIME, T_GAP_TIME, 0, 3600000, 100);
	obs_property_int_set_suffix(p, " ms");
	obs_property_set_long_description(p, T_GAP_TIME_DESC);

	obs_properties_add_bool(playback, S_LOOP, T_LOOP);
	obs_properties_add_bool(playback, S_SHUFFLE, T_SHUFFLE);
	obs_properties_add_bool(playback, S_HIDE_DONE, T_HIDE_DONE);
	obs_properties_add_bool(playback, S_RESTART_ON_SHOW, T_RESTART_ON_SHOW);

	obs_properties_add_group(props, S_GROUP_PLAYBACK, T_GROUP_PLAYBACK, OBS_GROUP_NORMAL, playback);

	/* ----------------- transitions ----------------- */

	for (size_t i = 0; i < PG_NUM_SLOTS; i++)
		add_slot_properties(props, (enum pg_slot_id)i, settings);

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

	obs_data_release(settings);
	return props;
}

/* ------------------------------------------------------------------------- */
/* missing files                                                              */

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct picture_gallery *pg = src;
	const char *orig_path = data;
	obs_source_t *source = pg->source;
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

static obs_missing_files_t *pg_missing_files(void *data)
{
	struct picture_gallery *pg = data;
	obs_missing_files_t *missing_files = obs_missing_files_create();
	obs_data_t *settings = obs_source_get_settings(pg->source);
	obs_data_array_t *files = obs_data_get_array(settings, S_FILES);
	size_t count = obs_data_array_count(files);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (path && *path && !os_file_exists(path)) {
			obs_missing_file_t *file = obs_missing_file_create(
				path, missing_file_callback, OBS_MISSING_FILE_SOURCE, pg->source, (void *)path);

			obs_missing_files_add_file(missing_files, file);
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}

static enum gs_color_space pg_video_get_color_space(void *data, size_t count,
						    const enum gs_color_space *preferred_spaces)
{
	struct picture_gallery *pg = data;
	obs_source_t *transition = get_current_transition(pg);
	enum gs_color_space space = GS_CS_SRGB;

	if (transition) {
		space = obs_source_get_color_space(transition, count, preferred_spaces);
		obs_source_release(transition);
	}

	return space;
}

struct obs_source_info picture_gallery_source_info = {
	.id = "sponsor_picture_gallery",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* Composite like the Image Slide Show: the images are silent, and the
	 * only sound is a stinger's own, which the parent scene mixes in
	 * through audio_render below. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = pg_get_name,
	.create = pg_create,
	.destroy = pg_destroy,
	.update = pg_update,
	.show = pg_show,
	.hide = pg_hide,
	.video_render = pg_video_render,
	.video_tick = pg_video_tick,
	.audio_render = pg_audio_render,
	.enum_active_sources = pg_enum_sources,
	.get_width = pg_get_width,
	.get_height = pg_get_height,
	.get_defaults = pg_defaults,
	.get_properties = pg_properties,
	.missing_files = pg_missing_files,
	.icon_type = OBS_ICON_TYPE_SLIDESHOW,
	.media_play_pause = pg_play_pause,
	.media_restart = pg_restart,
	.media_stop = pg_stop,
	.media_next = pg_next,
	.media_previous = pg_previous,
	.media_get_duration = pg_get_duration,
	.media_get_time = pg_get_time,
	.media_set_time = pg_set_time,
	.media_get_state = pg_get_state,
	.video_get_color_space = pg_video_get_color_space,
};
