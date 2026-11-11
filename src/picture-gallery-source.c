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
 *   Show       brings a picture in
 *   Hide       takes a picture out
 *   Normal     image -> image, when Show and Hide are not used
 *   Loop wrap  brings the first image in again at the loop point
 *
 * By default every picture comes in with the Show transition and goes out with
 * the Hide transition. With no gap the two run at once: the outgoing picture
 * plays Hide on top (the `out` layer) while the next one plays Show
 * underneath. With a gap the picture goes out with Hide, the gallery holds
 * empty for the gap, and the next comes in with Show.
 *
 * Any direction can be set to None. None for Show and Hide means the picture
 * just appears or disappears around the other one; with both None the Normal
 * transition goes straight from one image to the next. Whenever the gallery
 * has to come in from or go out to nothing and the direction is None, the
 * Normal transition stands in for it.
 *
 * "Show/Hide only at the edges" keeps the older behaviour, which is also how
 * the Source Switcher plugin behaves: Normal between images, and Show/Hide only
 * when the gallery comes in from or goes out to nothing (including the gap).
 *
 * Each direction owns a private transition source of its own (a cut for a
 * direction set to None). One of them is the base on screen - `cur` - and when
 * the gallery moves to a different direction, whatever the current one shows
 * is handed over to the next one before it starts, so the new transition
 * starts from the right picture.
 *
 * Every direction can use one of the built-in transitions or one of the scene
 * transitions configured in OBS (when running inside the OBS UI). A scene
 * transition is copied into a private transition of the same kind with the
 * same settings, and the copy follows that scene transition's settings when
 * they are edited.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>

#include "picture-gallery-source.h"
#include "playlist-common.h"
#include "frontend-shim.h"

#define do_log(level, format, ...) \
	blog(level, "[picture gallery: '%s'] " format, obs_source_get_name(pg->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

/* clang-format off */

#define S_FILES                        SI_S_FILES
#define S_HOLD_TIME                    "hold_time"
#define S_GAP_TIME                     "gap_time"
#define S_LOOP                         "loop"
#define S_SHUFFLE                      "shuffle"
#define S_HIDE_DONE                    "hide_when_done"
#define S_RESTART_ON_SHOW              "restart_on_show"
#define S_EDGES_ONLY                   "show_hide_edges_only"

#define S_GROUP_PLAYBACK               "playback_group"

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

#define TR_NONE                        "none"
/* what "None" for Loop wrap used to be stored as */
#define TR_SAME                        "same"
#define TR_SCENE                       "scene"

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
#define T_EDGES_ONLY                   T_("EdgesOnly")
#define T_EDGES_ONLY_DESC              T_("EdgesOnly.Desc")

#define T_GROUP_PLAYBACK               T_("Group.Playback")

#define T_TR_TYPE                      T_("Transition")
#define T_TR_NONE                      T_("Transition.None")
#define T_TR_SCENE                     T_("Transition.Scene")
#define T_TR_SCENE_NAME                T_("Transition.SceneName")
#define T_TR_SCENE_NAME_DESC           T_("Transition.SceneName.Desc")
#define T_TR_SCENE_MISSING             T_("Transition.SceneName.Missing")
#define T_TR_DURATION                  T_("Transition.Duration")

#define T_HK_PLAY_PAUSE                T_("Hotkey.PlayPause")
#define T_HK_RESTART                   T_("Hotkey.Restart")
#define T_HK_STOP                      T_("Hotkey.Stop")
#define T_HK_NEXT                      T_("Hotkey.Next")
#define T_HK_PREVIOUS                  T_("Hotkey.Previous")
#define T_HK_SHOW                      T_("Hotkey.Show")
#define T_HK_HIDE                      T_("Hotkey.Hide")

/* clang-format on */

/* Images only report their real dimensions once they have loaded, so automatic
 * sizing is refreshed on this interval (in seconds). */
#define SIZE_UPDATE_INTERVAL 0.25f

/* How long past its configured duration a transition is waited on before it
 * is taken to have finished, in seconds. A transition reports its own end, but
 * one that is never rendered must not hold the rotation up for good. */
#define TRANSITION_GRACE 10.0f

/* The same for a transition that sets its own length - a stinger, or a scene
 * transition with a fixed duration - whose length the gallery cannot know
 * ahead of time. Long enough for any real stinger, short enough that a broken
 * one does not stop the gallery for good. */
#define FIXED_TRANSITION_LIMIT 120.0f

/* How many ticks the previous transition stays on screen after another one
 * takes over without starting. See trail_previous(). */
#define TRAIL_TICKS 2

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
};

/* clang-format off */
#define SLOT_TR_KEYS(prefix) {                              \
		.direction = prefix "_" SS_DIRECTION,          \
		.swipe_in = prefix "_" SS_SWIPE_IN,            \
		.color = prefix "_" SS_COLOR,                  \
		.switch_point = prefix "_" SS_SWITCH_POINT,    \
		.stinger_path = prefix "_" SS_STINGER_PATH,    \
		.stinger_point = prefix "_" SS_STINGER_POINT,  \
		.stinger_hw = prefix "_" SS_STINGER_HW,        \
	}

/* where each direction's transition options are kept, e.g. "show_direction" */
static const struct si_tr_keys slot_tr_keys[PG_NUM_SLOTS] = {
	SLOT_TR_KEYS("normal"),
	SLOT_TR_KEYS("show"),
	SLOT_TR_KEYS("hide"),
	SLOT_TR_KEYS("wrap"),
};
/* clang-format on */

static const struct pg_slot_desc slot_desc[PG_NUM_SLOTS] = {
	{"normal", "Group.Normal", "Group.Normal.Desc", SI_TR_FADE, 500},
	{"show", "Group.Show", "Group.Show.Desc", SI_TR_FADE, 500},
	{"hide", "Group.Hide", "Group.Hide.Desc", SI_TR_FADE, 500},
	{"wrap", "Group.Wrap", "Group.Wrap.Desc", TR_NONE, 500},
};

struct picture_gallery;

/* One direction's transition. */
struct pg_slot {
	struct picture_gallery *pg;
	enum pg_slot_id id;

	/* The private transition this direction plays - a cut while it is set
	 * to None. Owned; written by update() under the mutex. */
	obs_source_t *tr;
	/* the libobs id `tr` was created as */
	char *tr_id;
	/* what `tr`'s settings were last built from, to skip no-op updates */
	char *sig;

	/* set to None: not used where another direction can stand in */
	bool none;
	bool cut;
	uint32_t duration_ms;

	/* The scene transition this direction copies, when it uses one, and
	 * whose edits it follows. */
	obs_weak_source_t *scene;

	/* Set by the transition's transition_stop signal. */
	volatile bool done;
};

/* a transition that has been replaced, and the slot it was replaced in */
struct pg_retired {
	obs_source_t *tr;
	struct pg_slot *slot;
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
	/* Show/Hide only when the gallery comes in from or goes out to nothing */
	bool edges_only;

	si_path_array_t configured_paths;

	/* ---- children ---- */
	struct pg_slot slots[PG_NUM_SLOTS];
	/* the slot on screen, NULL before the first image is shown */
	struct pg_slot *cur;
	/* The Hide slot while it takes the previous picture out on top of `cur`
	 * bringing the next one in, NULL otherwise. See swap_picture(). */
	struct pg_slot *out;

	/* The transition that was on screen before `cur` took over, still drawn
	 * in its place for a couple of ticks - see trail_previous(). A
	 * reference. `trail_retired` is the slot it was replaced in, when it no
	 * longer belongs to one and has to be retired once the trail ends. */
	obs_source_t *trail_tr;
	struct pg_slot *trail_retired;
	int trail_ticks;

	/* replaced transitions waiting to be let go of outside the mutex */
	DARRAY(struct pg_retired) retired;
	pg_item_array_t items;
	si_index_array_t order;

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
	struct si_layout layout;
	float size_timer;

	bool first_update_done;

	/* paths handed to the missing files dialog */
	si_missing_array_t missing_paths;
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

/* UI thread only. */
static void st_refresh(void)
{
	struct si_frontend_source_list list = {0};

	if (!st_mutex_ready || !si_frontend_available())
		return;

	si_frontend_get_transitions(&list);

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

	si_frontend_source_list_free(&list);
}

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

static void st_frontend_event(int event, void *unused)
{
	UNUSED_PARAMETER(unused);

	switch (event) {
	case SI_FRONTEND_EVENT_FINISHED_LOADING:
	case SI_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case SI_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		st_refresh();
		st_update_galleries();
		break;
	case SI_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case SI_FRONTEND_EVENT_EXIT:
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

static bool st_listening = false;

void picture_gallery_module_load(void)
{
	pthread_mutex_init_value(&st_mutex);
	if (pthread_mutex_init(&st_mutex, NULL) != 0)
		return;

	da_init(st_cache);
	da_init(st_galleries);
	st_mutex_ready = true;
}

void picture_gallery_module_post_load(void)
{
	/* Post-load rather than load: by now the OBS UI has set the frontend
	 * up, and anywhere without one this quietly finds nothing. */
	if (st_mutex_ready && si_frontend_load())
		st_listening = si_frontend_add_event_callback(st_frontend_event, NULL);
}

void picture_gallery_module_unload(void)
{
	if (!st_mutex_ready)
		return;

	if (st_listening) {
		si_frontend_remove_event_callback(st_frontend_event, NULL);
		st_listening = false;
	}

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

/* Whether a direction's transition type means None. Loop wrap's None used to
 * be "same as the normal transition", which is what None does there. */
static bool is_none_type(enum pg_slot_id id, const char *type)
{
	if (astrcmpi(type, TR_NONE) == 0 || astrcmpi(type, TR_SAME) == 0)
		return true;

	return id == PG_SLOT_WRAP && !*type;
}

static inline void set_media_state(struct picture_gallery *pg, enum obs_media_state state)
{
	pg->state = state;
}

/* The transition being drawn: the current one, or the one trailing it. */
static obs_source_t *get_current_transition(struct picture_gallery *pg)
{
	obs_source_t *tr = NULL;

	pthread_mutex_lock(&pg->mutex);
	if (pg->trail_tr)
		tr = obs_source_get_ref(pg->trail_tr);
	else if (pg->cur)
		tr = obs_source_get_ref(pg->cur->tr);
	pthread_mutex_unlock(&pg->mutex);

	return tr;
}

/* The Hide transition drawn on top while it takes a picture out, if any. */
static obs_source_t *get_out_transition(struct picture_gallery *pg)
{
	obs_source_t *tr = NULL;

	pthread_mutex_lock(&pg->mutex);
	if (pg->out)
		tr = obs_source_get_ref(pg->out->tr);
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

/* ------------------------------------------------------------------------- */
/* running order                                                              */

static void build_order(struct picture_gallery *pg)
{
	si_order_build(&pg->order, pg->items.num, pg->shuffle);
}

static void reshuffle_order(struct picture_gallery *pg)
{
	si_order_reshuffle(&pg->order, pg->items.num, pg->shuffle);
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

/* The slot that actually plays for direction `id`: a direction set to None
 * borrows the normal one (and a Normal set to None is a cut). Mutex must be
 * held. */
static struct pg_slot *resolve_slot(struct picture_gallery *pg, enum pg_slot_id id)
{
	struct pg_slot *slot = &pg->slots[id];

	if (!slot->tr || slot->none)
		slot = &pg->slots[PG_SLOT_NORMAL];

	return slot->tr ? slot : NULL;
}

/* Whether direction `id` is set to a transition of its own. Mutex must be
 * held. */
static inline bool slot_used(struct picture_gallery *pg, enum pg_slot_id id)
{
	return pg->slots[id].tr && !pg->slots[id].none;
}

/* Stops drawing the Hide transition that was taking a picture out on top.
 * Mutex must be held. */
static void drop_out(struct picture_gallery *pg)
{
	struct pg_slot *out = pg->out;

	if (!out)
		return;

	pg->out = NULL;
	os_atomic_set_bool(&out->done, true);

	if (out != pg->cur && out->tr)
		obs_transition_clear(out->tr);
}

/*
 * Keeping the picture steady while the transition on screen changes.
 *
 * A transition works out where to draw what it holds when it ticks or when a
 * transition starts - never when it is simply handed a picture with
 * obs_transition_set(). A transition that is brand new (a settings change made
 * while it is on screen), or that has not held a picture before, has nothing
 * worked out yet, and one created during this source's tick is not ticked by
 * libobs until the frame after. Drawn straight away, it would draw nothing: a
 * one-frame flicker on stream.
 *
 * So when the transition on screen changes, the previous one - which still
 * holds the same picture, laid out - goes on being drawn for TRAIL_TICKS more
 * ticks, by which point libobs has ticked the new one. A transition that is
 * started rather than handed its picture is laid out by the start, and ends
 * the trail at once.
 */

/* Ends the trail. Mutex must be held. A transition that was replaced is
 * queued to be retired by flush_retired() once the mutex is let go of. */
static void end_trail(struct picture_gallery *pg)
{
	obs_source_t *tr = pg->trail_tr;
	struct pg_slot *retired = pg->trail_retired;

	if (!tr)
		return;

	pg->trail_tr = NULL;
	pg->trail_retired = NULL;
	pg->trail_ticks = 0;

	if (retired) {
		struct pg_retired entry = {tr, retired};

		da_push_back(pg->retired, &entry);
		return;
	}

	/* still a slot's own transition: it lets go of the picture, unless it
	 * has meanwhile gone back on screen */
	if (!pg->cur || pg->cur->tr != tr)
		obs_transition_clear(tr);

	obs_source_release(tr);
}

/* Keeps `tr` (a reference, which this takes over) on screen for a couple of
 * ticks while the transition replacing it gets laid out. Mutex must be held. */
static void trail_previous(struct picture_gallery *pg, obs_source_t *tr, struct pg_slot *retired_from)
{
	end_trail(pg);

	pg->trail_tr = tr;
	pg->trail_retired = retired_from;
	pg->trail_ticks = TRAIL_TICKS;
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

	if (pg->cur && pg->cur->tr) {
		showing = obs_transition_get_active_source(pg->cur->tr);
		trail_previous(pg, obs_source_get_ref(pg->cur->tr), NULL);
	}

	obs_transition_set(slot->tr, showing);

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

	/* whatever was still going out on top is gone the moment the gallery
	 * moves on again */
	drop_out(pg);
	switch_to_slot(pg, slot);

	os_atomic_set_bool(&slot->done, false);

	if (slot->cut) {
		obs_transition_set(slot->tr, dest);
		os_atomic_set_bool(&slot->done, true);
	} else if (obs_transition_start(slot->tr, OBS_TRANSITION_MODE_AUTO, slot->duration_ms, dest)) {
		/* starting lays the transition out: nothing to wait for */
		end_trail(pg);
	} else {
		/* libobs refuses a transition to whatever is already on screen
		 * (a single image gallery, the same file listed twice in a
		 * row, nothing to nothing) - it is already there */
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

	/* A stinger runs for as long as its video does, whatever the duration
	 * setting says; going by the setting would start the hold time while
	 * the stinger was still playing. */
	if (obs_transition_fixed(slot->tr))
		limit = FIXED_TRANSITION_LIMIT;
	else
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
	si_layout_apply(&pg->layout, tr);

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

/*
 * Swap the picture on screen for the current image, each with its own
 * transition at the same time: the picture on screen goes out with Hide, drawn
 * on top, while the current image comes in from nothing with `in_id` (Show or
 * Loop wrap) underneath.
 *
 * With Hide set to None there is nothing to take the old picture out, so the
 * incoming transition plays straight from it instead (with Show set to None as
 * well, that is the Normal transition). With Show set to None and Hide used,
 * the new image is simply there underneath as the old one goes out on top.
 *
 * Mutex must be held.
 */
static void swap_picture(struct picture_gallery *pg, enum pg_slot_id in_id)
{
	struct pg_item *item = current_item(pg);
	struct pg_slot *hide = &pg->slots[PG_SLOT_HIDE];
	struct pg_slot *in = &pg->slots[in_id];
	obs_source_t *showing = NULL;

	if (!item)
		return;

	if (pg->cur && pg->cur->tr)
		showing = obs_transition_get_active_source(pg->cur->tr);

	/* Nothing to take out, the same picture again, or Hide not used:
	 * one transition from the picture on screen. */
	if (!showing || showing == item->source || !slot_used(pg, PG_SLOT_HIDE) || !in->tr) {
		obs_source_release(showing);
		bring_in(pg, in_id);
		return;
	}

	drop_out(pg);
	end_trail(pg);

	/* the slot holding the picture lets go of it, unless it is one of the
	 * two about to play */
	if (pg->cur && pg->cur != hide && pg->cur != in)
		obs_transition_clear(pg->cur->tr);

	/* out with Hide, on top */
	obs_transition_set(hide->tr, showing);
	if (!hide->cut && obs_transition_start(hide->tr, OBS_TRANSITION_MODE_AUTO, hide->duration_ms, NULL)) {
		os_atomic_set_bool(&hide->done, false);
		pg->out = hide;
	} else {
		obs_transition_set(hide->tr, NULL);
		os_atomic_set_bool(&hide->done, true);
	}
	obs_source_release(showing);

	/* in from nothing, underneath */
	pg->cur = in;
	os_atomic_set_bool(&in->done, false);
	obs_transition_set(in->tr, NULL);
	if (in->cut || !obs_transition_start(in->tr, OBS_TRANSITION_MODE_AUTO, in->duration_ms, item->source)) {
		obs_transition_set(in->tr, item->source);
		os_atomic_set_bool(&in->done, true);
	}

	pg->on_screen = true;
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
	drop_out(pg);
	end_trail(pg);

	if (pg->cur && pg->cur->tr) {
		obs_transition_set(pg->cur->tr, NULL);
		os_atomic_set_bool(&pg->cur->done, true);
	}

	pg->on_screen = false;
	enter_phase(pg, PG_IDLE);
}

/* Bring the current image in, from wherever the gallery is: with the Show
 * transition if it is empty, and if a picture is up, by swapping it out (or
 * with the Normal transition when Show/Hide are only used at the edges).
 * Mutex must be held. */
static void bring_in_from_anywhere(struct picture_gallery *pg)
{
	bool picture_up = pg->on_screen && pg->phase != PG_OUT;

	if (!picture_up)
		bring_in(pg, PG_SLOT_SHOW);
	else if (pg->edges_only)
		bring_in(pg, PG_SLOT_NORMAL);
	else
		swap_picture(pg, PG_SLOT_SHOW);
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
	enum pg_slot_id in_id;
	bool wrapped;

	if (!pg->loop && pg->order_pos + 1 >= pg->order.num) {
		gallery_finished(pg);
		return;
	}

	wrapped = move_position(pg, true);

	/* a wrap comes in with the Loop wrap transition when it has one of its
	 * own, everything else with Show */
	in_id = wrapped && slot_used(pg, PG_SLOT_WRAP) ? PG_SLOT_WRAP : PG_SLOT_SHOW;

	if (pg->gap_time > 0.0f) {
		pg->gap_in = in_id;
		take_out(pg, PG_AFTER_GAP);
	} else if (pg->edges_only) {
		bring_in(pg, wrapped ? PG_SLOT_WRAP : PG_SLOT_NORMAL);
	} else {
		swap_picture(pg, in_id);
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

/*
 * Start over from the first image. A gallery taken out with the Hide control
 * stays out: only the Show control brings it back, so a restart - pressing
 * play, the restart button, or the source becoming visible with "restart when
 * visible" on - moves it back to the start without revealing it.
 *
 * Mutex must be held.
 */
static void restart_gallery(struct picture_gallery *pg)
{
	if (pg->shuffle)
		build_order(pg);

	pg->order_pos = 0;
	pg->finished = false;
	pg->stopped = false;
	pg->paused = false;

	if (!pg->items.num) {
		set_media_state(pg, OBS_MEDIA_STATE_NONE);
		clear_now(pg);
		return;
	}

	set_media_state(pg, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(pg->source);

	if (pg->showing && !pg->user_hidden)
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

	si_layout_size(&pg->layout, cx, cy, &cx, &cy);

	if (cx == pg->cx && cy == pg->cy)
		return;

	pg->cx = cx;
	pg->cy = cy;

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		if (pg->slots[i].tr)
			obs_transition_set_size(pg->slots[i].tr, cx, cy);
	}
}

/* ------------------------------------------------------------------------- */
/* settings                                                                   */

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
	obs_source_t *old;

	/* still watching the same one */
	if (slot->scene && scene && obs_weak_source_references_source(slot->scene, scene))
		return;

	/* Whatever was watched before is let go of - including when it has
	 * been deleted since and can no longer be reached. */
	old = obs_weak_source_get_source(slot->scene);
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

/* Retires the transitions end_trail() queued. No lock may be held. */
static void flush_retired(struct picture_gallery *pg)
{
	DARRAY(struct pg_retired) retired;

	pthread_mutex_lock(&pg->mutex);
	retired.da = pg->retired.da;
	da_init(pg->retired);
	pthread_mutex_unlock(&pg->mutex);

	for (size_t i = 0; i < retired.num; i++)
		retire_transition(pg, retired.array[i].slot, retired.array[i].tr, false);

	da_free(retired);
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
	bool none = is_none_type(id, type);
	bool settings_moved;

	/* ---- which transition, with which settings ---- */

	if (none) {
		/* Not used: a cut that only ever plays where nothing else can
		 * stand in for it, and that keeps a picture on screen. */
		tr_id = "cut_transition";
		tr_settings = obs_data_create();
		is_cut = true;
		dstr_printf(&sig, "none|");
	} else if (astrcmpi(type, TR_SCENE) == 0) {
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
		bool is_stinger;

		tr_id = si_transition_id(type, &is_cut, &is_stinger);
		tr_settings = si_transition_settings(tr_id, &slot_tr_keys[id], settings);
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

	slot->none = none;
	slot->cut = is_cut;
	slot->duration_ms = (uint32_t)obs_data_get_int(settings, slot_key(key, sizeof(key), id, SS_DURATION));

	if (new_tr) {
		old_tr = slot->tr;

		/* a Hide on its way out on top is not carried over */
		if (pg->out == slot) {
			pg->out = NULL;
			os_atomic_set_bool(&slot->done, true);
		}

		/* the slot on screen keeps its picture across the swap, and the
		 * old transition stays up until the new one is laid out */
		if (pg->cur == slot && old_tr) {
			obs_source_t *showing = obs_transition_get_active_source(old_tr);

			obs_transition_set(new_tr, showing);
			obs_source_release(showing);
			os_atomic_set_bool(&slot->done, true);

			trail_previous(pg, old_tr, slot);
			old_tr = NULL;
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

/*
 * libobs never runs this on the caller's thread for a video source:
 * obs_source_update() only merges the settings and counts the update, and the
 * update itself runs on the graphics thread's next tick. Updates are therefore
 * serialized with each other and with video_tick(); only the mutex shared with
 * the audio, render and proc callbacks is needed.
 */
static void pg_update(void *data, obs_data_t *settings)
{
	struct picture_gallery *pg = data;
	pg_item_array_t new_items;
	pg_item_array_t old_items;
	si_path_array_t paths;
	si_path_array_t files;
	bool first_update = !pg->first_update_done;
	bool shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	bool reload_items;
	bool reload_order;

	da_init(new_items);
	da_init(old_items);
	da_init(paths);
	da_init(files);

	/* Loop wrap's "same as the normal transition" is None now - shown as
	 * such in the properties. */
	if (astrcmpi(obs_data_get_string(settings, "wrap_" SS_TYPE), TR_SAME) == 0)
		obs_data_set_string(settings, "wrap_" SS_TYPE, TR_NONE);

	/* ---- plain settings ---- */

	pthread_mutex_lock(&pg->mutex);

	reload_order = shuffle != pg->shuffle;

	pg->hold_time = (float)obs_data_get_int(settings, S_HOLD_TIME) / 1000.0f;
	pg->gap_time = (float)obs_data_get_int(settings, S_GAP_TIME) / 1000.0f;
	pg->loop = obs_data_get_bool(settings, S_LOOP);
	pg->shuffle = shuffle;
	pg->hide_when_done = obs_data_get_bool(settings, S_HIDE_DONE);
	pg->restart_on_show = obs_data_get_bool(settings, S_RESTART_ON_SHOW);
	pg->edges_only = obs_data_get_bool(settings, S_EDGES_ONLY);

	si_layout_read(&pg->layout, settings);

	pthread_mutex_unlock(&pg->mutex);

	/* ---- transitions ---- */

	for (size_t i = 0; i < PG_NUM_SLOTS; i++)
		update_slot(pg, &pg->slots[i], settings);

	/* ---- images ---- */

	/* Expanding directories walks the filesystem, so the images are only
	 * rebuilt when the list itself changed - not for every step of a
	 * slider drag elsewhere in the properties. */
	si_paths_read(settings, S_FILES, &paths);
	reload_items = first_update || !si_paths_equal(&paths, &pg->configured_paths);

	if (reload_items) {
		si_paths_expand(&paths, si_is_image_extension, obs_source_get_name(pg->source), &files);

		for (size_t i = 0; i < files.num; i++)
			add_item(pg, &new_items, files.array[i]);

		si_paths_free(&files);
		si_paths_free(&pg->configured_paths);
		pg->configured_paths = paths;
	} else {
		si_paths_free(&paths);
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
	flush_retired(pg);
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

	/* the trail and anything waiting to be retired go first: they may
	 * belong to the slots below */
	if (pg->trail_tr) {
		if (pg->trail_retired)
			retire_transition(pg, pg->trail_retired, pg->trail_tr, true);
		else
			obs_source_release(pg->trail_tr);
		pg->trail_tr = NULL;
	}
	for (size_t i = 0; i < pg->retired.num; i++)
		retire_transition(pg, pg->retired.array[i].slot, pg->retired.array[i].tr, true);
	da_free(pg->retired);

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
	si_paths_free(&pg->configured_paths);
	si_missing_free(&pg->missing_paths);

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
	si_layout_init(&pg->layout);

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		pg->slots[i].pg = pg;
		pg->slots[i].id = (enum pg_slot_id)i;
		pg->slots[i].done = true;
	}

	da_init(pg->items);
	da_init(pg->order);
	da_init(pg->configured_paths);
	da_init(pg->missing_paths);
	da_init(pg->retired);

	pthread_mutex_init_value(&pg->mutex);
	if (pthread_mutex_init_recursive(&pg->mutex) != 0) {
		bfree(pg);
		return NULL;
	}

	si_media_hotkeys_register(
		source,
		&(struct si_hotkey_names){"PictureGallery.PlayPause", "PictureGallery.Restart", "PictureGallery.Stop",
					  "PictureGallery.Next", "PictureGallery.Previous"},
		&(struct si_hotkey_names){T_HK_PLAY_PAUSE, T_HK_RESTART, T_HK_STOP, T_HK_NEXT, T_HK_PREVIOUS});
	obs_hotkey_register_source(source, "PictureGallery.Show", T_HK_SHOW, show_hotkey, pg);
	obs_hotkey_register_source(source, "PictureGallery.Hide", T_HK_HIDE, hide_hotkey, pg);

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
	obs_source_t *out = get_out_transition(pg);

	if (transition) {
		obs_source_video_render(transition);
		obs_source_release(transition);
	}

	/* a picture going out with Hide is drawn over the one coming in */
	if (out) {
		obs_source_video_render(out);
		obs_source_release(out);
	}

	UNUSED_PARAMETER(effect);
}

static void pg_video_tick(void *data, float seconds)
{
	struct picture_gallery *pg = data;

	pthread_mutex_lock(&pg->mutex);

	if (pg->trail_tr && --pg->trail_ticks <= 0)
		end_trail(pg);

	if (pg->out && os_atomic_load_bool(&pg->out->done))
		drop_out(pg);

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

	flush_retired(pg);
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

	/* a replaced transition still trailing is a child until it is retired */
	if (pg->trail_tr && pg->trail_retired)
		cb(pg->source, pg->trail_tr, param);
	for (size_t i = 0; i < pg->retired.num; i++)
		cb(pg->source, pg->retired.array[i].tr, param);
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
	obs_source_t *transition = get_out_transition(pg);
	struct obs_source_audio_mix child_audio;
	uint64_t source_ts = 0;
	bool success = false;

	UNUSED_PARAMETER(sample_rate);

	/* The images are silent; this is a stinger's own sound. A Hide stinger
	 * taking a picture out on top is heard over whatever brings the next
	 * one in, which only has sound of its own while nothing is going out. */
	if (transition && (obs_source_audio_pending(transition) || !obs_source_get_audio_timestamp(transition))) {
		obs_source_release(transition);
		transition = NULL;
	}
	if (!transition)
		transition = get_current_transition(pg);
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
	obs_data_set_default_bool(settings, S_EDGES_ONLY, false);

	for (size_t i = 0; i < PG_NUM_SLOTS; i++) {
		enum pg_slot_id id = (enum pg_slot_id)i;

		obs_data_set_default_string(settings, slot_key(key, sizeof(key), id, SS_TYPE),
					    slot_desc[i].default_type);
		obs_data_set_default_int(settings, slot_key(key, sizeof(key), id, SS_DURATION),
					 slot_desc[i].default_duration);
		si_transition_defaults(settings, &slot_tr_keys[i]);
	}

	si_layout_defaults(settings);
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
	bool none = is_none_type(id, type);
	bool scene = astrcmpi(type, TR_SCENE) == 0;
	bool is_cut = !none && astrcmpi(type, SI_TR_CUT) == 0;
	bool is_stinger = !none && astrcmpi(type, SI_TR_STINGER) == 0;

	UNUSED_PARAMETER(p);

	set_visible(props, id, SS_SCENE, scene);
	set_visible(props, id, SS_DURATION, !none && !is_cut && !is_stinger);
	si_transition_options_visible(props, &slot_tr_keys[id], none ? "" : type);

	return true;
}

/* clang-format off */
static const char *image_filter =
	"Image files (*.bmp *.tga *.png *.jpg *.jpeg *.jpe *.jfif *.gif *.webp *.psd *.tif *.tiff"
#ifdef _WIN32
	" *.jxr"
#endif
	");;All files (*.*)";

/* clang-format on */

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
	obs_property_list_add_string(p, T_TR_NONE, TR_NONE);
	si_transition_list_add_builtins(p);
	/* only inside the OBS UI, where there are scene transitions to use */
	if (si_frontend_available())
		obs_property_list_add_string(p, T_TR_SCENE, TR_SCENE);
	obs_property_set_modified_callback2(p, slot_type_changed, (void *)desc);

	p = obs_properties_add_list(group, slot_key(key, sizeof(key), id, SS_SCENE), T_TR_SCENE_NAME,
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(p, T_TR_SCENE_NAME_DESC);
	fill_scene_transitions(p, settings ? obs_data_get_string(settings, key) : NULL);

	p = obs_properties_add_int(group, slot_key(key, sizeof(key), id, SS_DURATION), T_TR_DURATION, 0, 60000, 50);
	obs_property_int_set_suffix(p, " ms");

	si_transition_option_properties(group, &slot_tr_keys[id]);

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
	struct picture_gallery *pg = data;
	obs_data_t *settings = pg ? obs_source_get_settings(pg->source) : NULL;
	struct dstr last_dir = {0};
	obs_property_t *p;

	/* ----------------- images ----------------- */

	if (pg) {
		pthread_mutex_lock(&pg->mutex);
		if (pg->items.num)
			si_path_directory(pg->items.array[pg->items.num - 1].path, &last_dir);
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

	p = obs_properties_add_bool(playback, S_EDGES_ONLY, T_EDGES_ONLY);
	obs_property_set_long_description(p, T_EDGES_ONLY_DESC);

	obs_properties_add_group(props, S_GROUP_PLAYBACK, T_GROUP_PLAYBACK, OBS_GROUP_NORMAL, playback);

	/* ----------------- transitions ----------------- */

	for (size_t i = 0; i < PG_NUM_SLOTS; i++)
		add_slot_properties(props, (enum pg_slot_id)i, settings);

	/* ----------------- appearance ----------------- */

	si_layout_properties(props);

	obs_data_release(settings);
	return props;
}

static obs_missing_files_t *pg_missing_files(void *data)
{
	struct picture_gallery *pg = data;
	return si_missing_files(pg->source, &pg->missing_paths);
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
