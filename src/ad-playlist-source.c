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
#include <util/util_uint64.h>

#include <math.h>
#include <stdlib.h>

#include "ad-playlist-source.h"
#include "playlist-common.h"

#define do_log(level, format, ...) \
	blog(level, "[ad playlist: '%s'] " format, obs_source_get_name(ap->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

/* clang-format off */

#define S_FILES                        SI_S_FILES
#define S_IMAGE_TIME                   "image_duration"
#define S_LOOP                         "loop"
#define S_SHUFFLE                      "shuffle"
#define S_HIDE_DONE                    "hide_when_done"
#define S_RESTART_ON_SHOW              "restart_on_show"
#define S_PRELOAD                      "preload"
#define S_HW_DECODE                    "hw_decode"
#define S_VOLUME                       "volume"
#define S_NORMALIZE                    "normalize_loudness"
#define S_NORM_TARGET                  "normalize_target"
#define S_NORM_MAX_BOOST               "normalize_max_boost"

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

#define S_GROUP_PLAYBACK               "playback_group"
#define S_GROUP_AUDIO                  "audio_group"
#define S_GROUP_TRANSITION             "transition_group"

#define TIMING_OVERLAP                 "overlap"
#define TIMING_AFTER                   "after"

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
#define T_VOLUME_DESC                  T_("Volume.Desc")
#define T_NORMALIZE                    T_("Normalize")
#define T_NORMALIZE_DESC               T_("Normalize.Desc")
#define T_NORM_TARGET                  T_("Normalize.Target")
#define T_NORM_TARGET_DESC             T_("Normalize.Target.Desc")
#define T_NORM_MAX_BOOST               T_("Normalize.MaxBoost")
#define T_NORM_MAX_BOOST_DESC          T_("Normalize.MaxBoost.Desc")

#define T_GROUP_PLAYBACK               T_("Group.Playback")
#define T_GROUP_AUDIO                  T_("Group.Audio")
#define T_GROUP_TRANSITION             T_("Group.Transition")

#define T_TRANSITION                   T_("Transition")
#define T_TR_DURATION                  T_("Transition.Duration")
#define T_TR_TIMING                    T_("Transition.Timing")
#define T_TR_TIMING_OVERLAP            T_("Transition.Timing.Overlap")
#define T_TR_TIMING_AFTER              T_("Transition.Timing.After")

#define T_HK_PLAY_PAUSE                T_("Hotkey.PlayPause")
#define T_HK_RESTART                   T_("Hotkey.Restart")
#define T_HK_STOP                      T_("Hotkey.Stop")
#define T_HK_NEXT                      T_("Hotkey.Next")
#define T_HK_PREVIOUS                  T_("Hotkey.Previous")

/* clang-format on */

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

/* ---- audio mixer ---- */

/* How many sources the mixer takes audio from at once: the item being
 * transitioned away from, the one coming in, and whatever the transition plays
 * of its own - a stinger has a video of its own and, with a track matte on, a
 * second one alongside it. */
#define AP_NUM_DECKS 4

/* How far a source's audio may drift from where it was anchored before it is
 * anchored again, in nanoseconds. A drift this large is a clock that has been
 * put back (a restarted file) rather than one that is merely running late. */
#define AP_DECK_RESYNC_NS UINT64_C(500000000)

/* How long the mixer holds a sample waiting on a source that still has audio
 * to come, in nanoseconds. Both halves of a transition have to be in before
 * either can be mixed; the bound keeps one that stops dead from taking the
 * whole mix down with it. */
#define AP_MIX_HOLD_NS UINT64_C(60000000)

/* How long a source that has stopped handing audio over is waited on before it
 * is written off, in nanoseconds. A file with no audio track never hands
 * anything over at all, and must not hold up the other half of a transition. */
#define AP_DECK_IDLE_NS UINT64_C(200000000)

/* the most audio the mixer will hold at once, in nanoseconds */
#define AP_MIX_MAX_NS UINT64_C(1000000000)

/* ---- loudness normalization (ITU-R BS.1770 / EBU R128 style) ---- */

/* Loudness is measured over 400 ms blocks stepped every 100 ms. */
#define LN_SUB_BLOCKS 4

/* Blocks quieter than this are silence and do not count, in LUFS. */
#define LN_ABS_GATE (-70.0)

/* Blocks this far below the running measurement are pauses between lines or
 * a quiet intro, and do not count either, in LU. */
#define LN_REL_GATE (-10.0)

/* How many blocks have to be measured before the measurement is trusted to
 * move the level - a little over a second of actual sound. */
#define LN_MIN_BLOCKS 10

/* How quickly the level follows the measurement, in seconds. Slow enough that
 * the start of an unmeasured file does not pump, quick enough that it settles
 * within the first few seconds. A file that has played before starts at its
 * remembered level straight away. */
#define LN_SMOOTH_SECONDS 1.0

/* How far a loud file may be turned down, in dB. */
#define LN_MAX_CUT_DB 30.0

/* Boosting never takes a packet's peak above this. */
#define LN_PEAK_CEILING 0.95f

/* How many files' loudness is remembered. */
#define LN_MAX_RECORDS 2048

#define LN_SILENCE (-200.0)
#define LN_PI 3.14159265358979323846

/* where the transition's options are kept */
static const struct si_tr_keys tr_keys = {
	.direction = S_TR_DIRECTION,
	.swipe_in = S_TR_SWIPE_IN,
	.color = S_TR_COLOR,
	.switch_point = S_TR_SWITCH_POINT,
	.stinger_path = S_TR_STINGER_PATH,
	.stinger_point = S_TR_STINGER_POINT,
	.stinger_hw = S_TR_STINGER_HW,
};

/* ------------------------------------------------------------------------- */

struct ad_item {
	char *path;
	bool is_video;
	obs_source_t *source;
};

/* K-weighting: a high shelf modelling the head, then a high pass. */
struct ap_kfilter {
	double b[2][3];
	double a[2][3];
};

/* A running loudness measurement of one file. */
struct ap_loudness {
	/* filter state, two biquads per channel */
	double z[MAX_AUDIO_CHANNELS][4];
	double sub_energy;
	size_t sub_frames;
	double ring[LN_SUB_BLOCKS];
	size_t ring_num;
	size_t ring_pos;
	/* the gated blocks so far: their summed mean square and how many */
	double sum;
	uint64_t blocks;
};

/* What is remembered of a file between plays. */
struct ap_loudness_record {
	char *path;
	double sum;
	uint64_t blocks;
};

/* One source the mixer is taking audio from. */
struct ap_deck {
	/* the source being tapped; the deck holds a reference while it is */
	obs_source_t *source;
	/* whether this is something the transition brought along (a stinger's
	 * own video) rather than a file out of the playlist */
	bool from_transition;
	/* the playlist file, for remembering its loudness; NULL otherwise */
	char *path;
	/* what the transition scales it by, written by the tick */
	float gain;
	/* the gain the last packet ended on, so a change rides across the next
	 * one rather than stepping and clicking */
	float applied_gain;
	/* What has to be added to this source's timestamps to put them on the
	 * system clock. Every source counts from a base of its own, so the mixer
	 * works in the system clock and anchors each source to it as its first
	 * packet arrives. */
	int64_t offset;
	bool anchored;
	/* just past the last sample handed over, on the system clock */
	uint64_t end_ts;
	/* when that arrived */
	uint64_t arrived;
	/* whether this source is handing audio over at all */
	bool producing;

	struct ap_loudness meter;
	/* the normalization being applied right now, in dB */
	double norm_db;
};

typedef DARRAY(struct ap_loudness_record) record_array_t;

/* Where the audio of the items being played is added up before it is handed to
 * OBS. See the audio section further down for what it is for. */
struct ap_audio {
	/* Guards everything below. It is a leaf: nothing else is taken under
	 * it, and it is never taken under ap->mutex. */
	pthread_mutex_t mutex;

	struct ap_deck deck[AP_NUM_DECKS];

	/* Samples that have been handed over but not let out yet, planar float
	 * in the mix format; 'ts' is where the first of them sits. */
	float *buf[MAX_AUDIO_CHANNELS];
	size_t capacity;
	size_t frames;
	uint64_t ts;
	bool have;

	size_t channels;
	size_t sample_rate;
	enum speaker_layout speakers;

	/* just past the last sample let out, so audio that turns up behind it
	 * is dropped rather than mixed in under what has already gone */
	uint64_t next_ts;
	bool flushed;

	struct ap_kfilter kfilter;
	size_t kfilter_rate;

	/* settings */
	float volume;
	bool normalize;
	double target_lufs;
	double max_boost_db;

	/* how loud every file played so far measured */
	record_array_t records;
};

typedef DARRAY(struct ad_item) ad_item_array_t;

struct ad_playlist {
	obs_source_t *source;

	/* Recursive: signals and media callbacks emitted while the lock is held
	 * can re-enter this source through its procs. */
	pthread_mutex_t mutex;

	/* ---- settings ---- */
	float image_time;
	uint32_t tr_duration_ms;
	uint32_t stinger_point_ms;
	bool overlap;
	bool loop;
	bool shuffle;
	bool hide_when_done;
	bool restart_on_show;
	bool preload;
	bool hw_decode;
	bool use_cut;
	bool use_stinger;
	bool use_to_color;
	float switch_point; /* fade to color, 0..1 */
	char *tr_type;      /* transition setting value */
	const char *tr_id;  /* libobs transition source id */

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
	si_path_array_t configured_paths;
	char *tr_settings_sig;

	/* ---- children ---- */
	obs_source_t *transition;
	ad_item_array_t items;
	si_index_array_t order;

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
	struct si_layout layout;
	float size_timer;

	/* ---- audio ---- */
	/* Whether the transition is between two items, so their audio has to be
	 * crossed over. Set when one starts and cleared by its transition_stop
	 * signal, which arrives on the graphics thread. */
	volatile bool tr_running;
	/* where the items' audio is mixed on its way out to OBS; guards itself */
	struct ap_audio audio;

	/* paths handed to the missing files dialog */
	si_missing_array_t missing_paths;
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

static inline bool supported_extension(const char *ext)
{
	return si_is_image_extension(ext) || si_is_media_extension(ext);
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
	bool is_video = !si_is_image_extension(ext);
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

	/* The item is played through the playlist's own mixer and nowhere else.
	 * Leaving it no mixers of its own is what makes that true: libobs mixes
	 * a source it meets twice in the audio tree straight into the program
	 * mix, past every parent that would have scaled it, and an item is met
	 * twice as soon as anything looks at the scene a second time (studio
	 * mode, extra canvases). With no mixers there is nothing for that path
	 * to carry. Its audio is tapped as it is decoded instead. */
	obs_source_set_audio_mixers(source, 0);

	item.path = bstrdup(path);
	item.is_video = is_video;
	item.source = source;
	da_push_back(*new_items, &item);
}

/* ------------------------------------------------------------------------- */
/* playback order                                                             */

static void build_order(struct ad_playlist *ap)
{
	si_order_build(&ap->order, ap->items.num, ap->shuffle);
}

/* Reshuffle for the next pass without repeating an item across the loop point. */
static void reshuffle_order(struct ad_playlist *ap)
{
	si_order_reshuffle(&ap->order, ap->items.num, ap->shuffle);
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

		if (started)
			os_atomic_set_bool(&ap->tr_running, true);

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

	si_layout_size(&ap->layout, cx, cy, &cx, &cy);

	if (cx == ap->cx && cy == ap->cy)
		return;

	ap->cx = cx;
	ap->cy = cy;

	if (ap->transition)
		obs_transition_set_size(ap->transition, cx, cy);
}

/* ------------------------------------------------------------------------- */
/* settings                                                                   */

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

static void transition_stopped(void *data, calldata_t *cd)
{
	struct ad_playlist *ap = data;

	UNUSED_PARAMETER(cd);
	os_atomic_set_bool(&ap->tr_running, false);
}

static void connect_transition(struct ad_playlist *ap, obs_source_t *tr, bool connect)
{
	signal_handler_t *sh = obs_source_get_signal_handler(tr);

	if (connect)
		signal_handler_connect(sh, "transition_stop", transition_stopped, ap);
	else
		signal_handler_disconnect(sh, "transition_stop", transition_stopped, ap);
}

/*
 * OBS calls this for every change to any property, and a slider hands over a
 * new value for every step of the drag. Rebuilding the playlist reopens every
 * media file and restarts playback from the top, so it only runs when the
 * settings behind the children actually changed - a volume drag just stores a
 * float and leaves the rotation alone.
 *
 * libobs never runs this on the caller's thread for a video source:
 * obs_source_update() only merges the settings and counts the update, and the
 * update itself runs on the graphics thread's next tick. Updates are therefore
 * serialized with each other and with video_tick(); only the mutex shared with
 * the audio, render and proc callbacks is needed.
 */
static void ap_update(void *data, obs_data_t *settings)
{
	struct ad_playlist *ap = data;
	ad_item_array_t new_items;
	ad_item_array_t old_items;
	obs_source_t *new_tr = NULL;
	obs_source_t *old_tr = NULL;
	obs_data_t *tr_settings;
	si_path_array_t paths;
	si_path_array_t files;
	const char *tr_type;
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
	da_init(files);

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
	ap->switch_point = (float)obs_data_get_int(settings, S_TR_SWITCH_POINT) / 100.0f;

	pthread_mutex_lock(&ap->audio.mutex);
	ap->audio.volume = (float)obs_data_get_int(settings, S_VOLUME) / 100.0f;
	ap->audio.normalize = obs_data_get_bool(settings, S_NORMALIZE);
	ap->audio.target_lufs = (double)obs_data_get_int(settings, S_NORM_TARGET);
	ap->audio.max_boost_db = (double)obs_data_get_int(settings, S_NORM_MAX_BOOST);
	pthread_mutex_unlock(&ap->audio.mutex);

	tr_type = obs_data_get_string(settings, S_TRANSITION);
	ap->tr_id = si_transition_id(tr_type, &is_cut, &is_stinger);
	ap->use_cut = is_cut;
	ap->use_stinger = is_stinger;
	ap->use_to_color = strcmp(ap->tr_id, "fade_to_color_transition") == 0;

	if (!ap->tr_type || strcmp(ap->tr_type, tr_type) != 0) {
		new_tr = obs_source_create_private(ap->tr_id, NULL, NULL);
		bfree(ap->tr_type);
		ap->tr_type = bstrdup(tr_type);

		if (new_tr) {
			/* the playlist mixes what the transition holds itself */
			obs_source_set_audio_mixers(new_tr, 0);
			connect_transition(ap, new_tr, true);
		} else
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
	si_paths_read(settings, S_FILES, &paths);

	reload_items = first_update || preload != ap->preload || hw_decode != ap->hw_decode ||
		       !si_paths_equal(&paths, &ap->configured_paths);

	ap->preload = preload;
	ap->hw_decode = hw_decode;

	if (reload_items) {
		si_paths_expand(&paths, supported_extension, obs_source_get_name(ap->source), &files);

		for (size_t i = 0; i < files.num; i++)
			add_item(ap, &new_items, files.array[i]);

		si_paths_free(&files);
		si_paths_free(&ap->configured_paths);
		ap->configured_paths = paths;
	} else {
		si_paths_free(&paths);
	}

	/* ------------------------------------- */
	/* appearance                            */

	si_layout_read(&ap->layout, settings);

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
		connect_transition(ap, old_tr, false);
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

	tr_settings = si_transition_settings(ap->tr_id, &tr_keys, settings);
	tr_settings_moved = signature_changed(&ap->tr_settings_sig, transition_signature(tr_settings));

	/* a stinger reopens its video every time it is updated, so only hand the
	 * settings over when they moved - or when the transition is brand new */
	if (tr_settings_moved || new_tr)
		obs_source_update(ap->transition, tr_settings);

	obs_data_release(tr_settings);

	si_layout_apply(&ap->layout, ap->transition);

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
/* audio
 *
 * The playlist hands OBS its own audio rather than leaving the items to be
 * mixed up the source tree through the transition. That is what puts it in the
 * audio mixer next to every other source, with a fader, a mute, a meter,
 * monitoring, audio filters and its own tracks: a composite source may not
 * carry OBS_SOURCE_AUDIO at all, so while the items were mixed for it the
 * playlist had none of that and the volume property was the only level it had.
 *
 * So the items are taken out of libobs' audio altogether: each is opened with
 * no audio mixers of its own, and the playlist taps its samples as they are
 * decoded instead. What is tapped is already in the mix format, because libobs
 * resamples on the way into obs_source_output_audio(), so the mixer below only
 * has to line the two items of a transition up in time, scale them the way the
 * transition would have, and pass the result on through
 * obs_source_output_audio() on the playlist itself.
 *
 * On the way through, every file is measured the way broadcast loudness is
 * (ITU-R BS.1770: K-weighted, gated, integrated) and brought toward a common
 * target, so a quiet spot is lifted and a loud one is turned down and the
 * rotation plays at one even level. The measurement is remembered per file,
 * so a file that has been through the rotation once starts at its level.    */

static inline uint64_t ap_frames_to_ns(size_t sample_rate, uint64_t frames)
{
	return util_mul_div64(frames, UINT64_C(1000000000), sample_rate);
}

static inline uint64_t ap_ns_to_frames(size_t sample_rate, uint64_t ns)
{
	return util_mul_div64(ns, sample_rate, UINT64_C(1000000000));
}

/* ---- loudness ---- */

/* K-weighting filter coefficients for any sample rate, as derived by
 * libebur128 from the 48 kHz ones in BS.1770. */
static void ap_kfilter_init(struct ap_kfilter *kf, size_t sample_rate)
{
	double fs = (double)sample_rate;
	double f0 = 1681.974450955533;
	double g = 3.999843853973347;
	double q = 0.7071752369554196;
	double k = tan(LN_PI * f0 / fs);
	double vh = pow(10.0, g / 20.0);
	double vb = pow(vh, 0.4996667741545416);
	double a0 = 1.0 + k / q + k * k;

	kf->b[0][0] = (vh + vb * k / q + k * k) / a0;
	kf->b[0][1] = 2.0 * (k * k - vh) / a0;
	kf->b[0][2] = (vh - vb * k / q + k * k) / a0;
	kf->a[0][0] = 1.0;
	kf->a[0][1] = 2.0 * (k * k - 1.0) / a0;
	kf->a[0][2] = (1.0 - k / q + k * k) / a0;

	f0 = 38.13547087602444;
	q = 0.5003270373238773;
	k = tan(LN_PI * f0 / fs);
	a0 = 1.0 + k / q + k * k;

	kf->b[1][0] = 1.0;
	kf->b[1][1] = -2.0;
	kf->b[1][2] = 1.0;
	kf->a[1][0] = 1.0;
	kf->a[1][1] = 2.0 * (k * k - 1.0) / a0;
	kf->a[1][2] = (1.0 - k / q + k * k) / a0;
}

static inline double ap_loudness_of(double power)
{
	return power > 0.0 ? -0.691 + 10.0 * log10(power) : LN_SILENCE;
}

static inline double ap_meter_integrated(const struct ap_loudness *m)
{
	return m->blocks ? ap_loudness_of(m->sum / (double)m->blocks) : LN_SILENCE;
}

/* A 400 ms block is complete: count it unless it is silence, or a pause well
 * below what has been heard so far. */
static void ap_meter_block(struct ap_loudness *m)
{
	double power = 0.0;
	double lufs;

	for (size_t i = 0; i < LN_SUB_BLOCKS; i++)
		power += m->ring[i];

	power /= (double)LN_SUB_BLOCKS;
	lufs = ap_loudness_of(power);

	if (lufs < LN_ABS_GATE)
		return;
	if (m->blocks >= LN_MIN_BLOCKS && lufs < ap_meter_integrated(m) + LN_REL_GATE)
		return;

	m->sum += power;
	m->blocks++;
}

/* Runs a packet of a file's own audio, before anything is done to its level,
 * through the meter. Expects the mixer's lock to be held. */
static void ap_meter_feed(struct ap_audio *a, struct ap_loudness *m, const struct audio_data *audio, size_t channels)
{
	const struct ap_kfilter *kf = &a->kfilter;
	size_t sub_len = a->sample_rate / 10;
	/* the LFE channel of 5.1 and 7.1 is left out of the measurement */
	size_t lfe = channels >= 6 ? 3 : MAX_AUDIO_CHANNELS;

	if (!sub_len)
		return;

	for (size_t i = 0; i < audio->frames; i++) {
		for (size_t ch = 0; ch < channels; ch++) {
			const float *in = (const float *)audio->data[ch];
			double *z = m->z[ch];
			double x, y, w;

			if (!in || ch == lfe)
				continue;

			x = (double)in[i];

			y = kf->b[0][0] * x + z[0];
			z[0] = kf->b[0][1] * x - kf->a[0][1] * y + z[1];
			z[1] = kf->b[0][2] * x - kf->a[0][2] * y;

			w = kf->b[1][0] * y + z[2];
			z[2] = kf->b[1][1] * y - kf->a[1][1] * w + z[3];
			z[3] = kf->b[1][2] * y - kf->a[1][2] * w;

			m->sub_energy += w * w;
		}

		if (++m->sub_frames < sub_len)
			continue;

		m->ring[m->ring_pos] = m->sub_energy / (double)sub_len;
		m->ring_pos = (m->ring_pos + 1) % LN_SUB_BLOCKS;
		if (m->ring_num < LN_SUB_BLOCKS)
			m->ring_num++;
		if (m->ring_num == LN_SUB_BLOCKS)
			ap_meter_block(m);

		m->sub_energy = 0.0;
		m->sub_frames = 0;
	}
}

/* What a file wants to be played at to land on the target, in dB, or 0 while
 * there is not enough of it measured to tell. Expects the mixer's lock to be
 * held. */
static double ap_norm_target_db(const struct ap_audio *a, const struct ap_loudness *m)
{
	double db;

	if (!a->normalize || m->blocks < LN_MIN_BLOCKS)
		return 0.0;

	db = a->target_lufs - ap_meter_integrated(m);

	if (db > a->max_boost_db)
		db = a->max_boost_db;
	else if (db < -LN_MAX_CUT_DB)
		db = -LN_MAX_CUT_DB;

	return db;
}

static struct ap_loudness_record *ap_record_find(struct ap_audio *a, const char *path)
{
	for (size_t i = 0; i < a->records.num; i++) {
		if (strcmp(a->records.array[i].path, path) == 0)
			return &a->records.array[i];
	}

	return NULL;
}

/* Remembers how loud a file measured. Expects the mixer's lock to be held. */
static void ap_record_save(struct ap_audio *a, const char *path, const struct ap_loudness *m)
{
	struct ap_loudness_record *rec;

	if (!path || !m->blocks)
		return;

	rec = ap_record_find(a, path);
	if (!rec) {
		if (a->records.num >= LN_MAX_RECORDS)
			return;

		rec = da_push_back_new(a->records);
		rec->path = bstrdup(path);
	}

	rec->sum = m->sum;
	rec->blocks = m->blocks;
}

/* Sets a deck up for a file that is just being picked up, starting from what
 * is remembered of it. Expects the mixer's lock to be held. */
static void ap_deck_seed(struct ap_audio *a, struct ap_deck *deck)
{
	struct ap_loudness_record *rec = deck->path ? ap_record_find(a, deck->path) : NULL;

	if (rec) {
		deck->meter.sum = rec->sum;
		deck->meter.blocks = rec->blocks;
	}

	/* a file that has been measured before starts at its level rather than
	 * gliding into it */
	deck->norm_db = ap_norm_target_db(a, &deck->meter);
}

/* ---- mixing ---- */

/* Throws away what the mixer is holding and sets the format it works in.
 * Expects the mixer's lock to be held. */
static void ap_mix_reset(struct ap_audio *a, size_t channels, size_t sample_rate)
{
	for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
		bfree(a->buf[ch]);
		a->buf[ch] = NULL;
	}

	a->capacity = 0;
	a->frames = 0;
	a->ts = 0;
	a->have = false;
	a->next_ts = 0;
	a->flushed = false;
	a->channels = channels;
	a->sample_rate = sample_rate;

	if (sample_rate && sample_rate != a->kfilter_rate) {
		ap_kfilter_init(&a->kfilter, sample_rate);
		a->kfilter_rate = sample_rate;
	}
}

/* Makes room for 'frames' samples and clears whatever of them is new, so that
 * a gap between two items is silence rather than the last thing that stood
 * there. Returns false when that is more than the mixer will hold. Expects the
 * mixer's lock to be held. */
static bool ap_mix_reserve(struct ap_audio *a, size_t frames)
{
	size_t limit = (size_t)ap_ns_to_frames(a->sample_rate, AP_MIX_MAX_NS);

	if (!a->channels || frames > limit)
		return false;

	if (frames > a->capacity) {
		size_t capacity = a->capacity ? a->capacity : 1024;

		while (capacity < frames)
			capacity *= 2;
		if (capacity > limit)
			capacity = limit;

		for (size_t ch = 0; ch < a->channels; ch++) {
			a->buf[ch] = brealloc(a->buf[ch], capacity * sizeof(float));
			memset(a->buf[ch] + a->frames, 0, (capacity - a->frames) * sizeof(float));
		}

		a->capacity = capacity;
	} else if (frames > a->frames) {
		for (size_t ch = 0; ch < a->channels; ch++)
			memset(a->buf[ch] + a->frames, 0, (frames - a->frames) * sizeof(float));
	}

	if (frames > a->frames)
		a->frames = frames;

	return true;
}

/* Hands OBS whatever of the mix nothing is going to add to any more: everything
 * up to the point the source that is furthest behind has reached, and never
 * more than the hold behind the clock. Expects the mixer's lock to be held. */
static void ap_mix_flush(struct ad_playlist *ap)
{
	struct ap_audio *a = &ap->audio;
	struct obs_source_audio out = {0};
	uint64_t now;
	uint64_t horizon;
	size_t frames;

	if (!a->have || !a->frames || !a->sample_rate || !a->channels)
		return;

	now = os_gettime_ns();
	horizon = a->ts + ap_frames_to_ns(a->sample_rate, a->frames);

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		struct ap_deck *deck = &a->deck[i];

		if (!deck->source || !deck->producing)
			continue;

		if (now - deck->arrived > AP_DECK_IDLE_NS) {
			deck->producing = false;
			continue;
		}

		if (deck->end_ts < horizon)
			horizon = deck->end_ts;
	}

	if (now > AP_MIX_HOLD_NS && horizon < now - AP_MIX_HOLD_NS)
		horizon = now - AP_MIX_HOLD_NS;

	if (horizon <= a->ts)
		return;

	frames = (size_t)ap_ns_to_frames(a->sample_rate, horizon - a->ts);
	if (frames > a->frames)
		frames = a->frames;
	if (!frames)
		return;

	for (size_t ch = 0; ch < a->channels; ch++)
		out.data[ch] = (const uint8_t *)a->buf[ch];

	out.frames = (uint32_t)frames;
	out.speakers = a->speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)a->sample_rate;
	out.timestamp = a->ts;

	obs_source_output_audio(ap->source, &out);

	a->ts += ap_frames_to_ns(a->sample_rate, frames);
	a->next_ts = a->ts;
	a->flushed = true;
	a->frames -= frames;

	if (a->frames) {
		for (size_t ch = 0; ch < a->channels; ch++)
			memmove(a->buf[ch], a->buf[ch] + frames, a->frames * sizeof(float));
	} else {
		a->have = false;
	}
}

/* The level a playlist file is played at on top of the playlist volume and the
 * transition: its normalization, eased toward where the measurement says it
 * should be, and held back where boosting this packet would take it near
 * clipping. Expects the mixer's lock to be held. */
static float ap_deck_norm_gain(struct ap_audio *a, struct ap_deck *deck, const struct audio_data *audio, size_t skipped,
			       size_t frames, size_t channels, float level)
{
	double target = ap_norm_target_db(a, &deck->meter);
	double dt = (double)audio->frames / (double)a->sample_rate;
	float norm;
	float peak = 0.0f;
	float scale = deck->gain * level;

	deck->norm_db += (target - deck->norm_db) * (1.0 - exp(-dt / LN_SMOOTH_SECONDS));
	norm = (float)pow(10.0, deck->norm_db / 20.0);

	if (norm <= 1.0f || scale <= 0.0f)
		return norm;

	for (size_t ch = 0; ch < channels; ch++) {
		const float *in = (const float *)audio->data[ch];

		if (!in)
			continue;

		for (size_t i = skipped; i < skipped + frames; i++) {
			float v = fabsf(in[i]);
			if (v > peak)
				peak = v;
		}
	}

	if (peak * scale * norm > LN_PEAK_CEILING) {
		float limited = LN_PEAK_CEILING / (peak * scale);
		norm = limited < 1.0f ? 1.0f : limited;
	}

	return norm;
}

/* Takes a packet of one source's audio into the mix. Runs on that source's own
 * media thread, which is why the mixer has a lock of its own: ap->mutex is held
 * across work that hands sources to the transition, and audio cannot wait on
 * it. */
static void ap_mix_submit(struct ad_playlist *ap, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	audio_t *obs_audio = obs_get_audio();
	const struct audio_output_info *info;
	struct ap_audio *a = &ap->audio;
	struct ap_deck *deck = NULL;
	size_t channels;
	size_t sample_rate;

	if (!obs_audio || !audio || !audio->frames)
		return;

	info = audio_output_get_info(obs_audio);
	channels = audio_output_get_channels(obs_audio);
	sample_rate = audio_output_get_sample_rate(obs_audio);

	if (!info || !channels || channels > MAX_AUDIO_CHANNELS || !sample_rate)
		return;

	pthread_mutex_lock(&a->mutex);

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		if (a->deck[i].source == source) {
			deck = &a->deck[i];
			break;
		}
	}

	/* the source was taken off the mixer between the packet being handed
	 * over and this getting to look at it */
	if (!deck)
		goto unlock;

	if (a->channels != channels || a->sample_rate != sample_rate)
		ap_mix_reset(a, channels, sample_rate);

	a->speakers = info->speakers;

	if (!deck->from_transition)
		ap_meter_feed(a, &deck->meter, audio, channels);

	{
		/* put the packet on the clock the mixer works in */
		uint64_t arrived = os_gettime_ns();
		int64_t raw = (int64_t)audio->timestamp;
		uint64_t ts;
		size_t frames = audio->frames;
		size_t skipped = 0;
		uint64_t floor_ts = a->flushed ? a->next_ts : 0;
		bool have_floor = a->flushed;

		if (!deck->anchored) {
			deck->offset = (int64_t)arrived - raw;
			deck->anchored = true;
		} else {
			uint64_t placed = (uint64_t)(raw + deck->offset);
			uint64_t drift = placed > arrived ? placed - arrived : arrived - placed;

			if (drift > AP_DECK_RESYNC_NS)
				deck->offset = (int64_t)arrived - raw;
		}

		ts = (uint64_t)(raw + deck->offset);

		/* Never mix behind what has already gone out, or behind what is
		 * being held: a packet that turns up late is trimmed rather
		 * than mixed in under audio that has moved on. */
		if (a->have && (!have_floor || a->ts > floor_ts)) {
			floor_ts = a->ts;
			have_floor = true;
		}

		if (have_floor && ts < floor_ts) {
			skipped = (size_t)ap_ns_to_frames(sample_rate, floor_ts - ts);

			if (skipped >= frames) {
				skipped = frames;
				frames = 0;
			} else {
				frames -= skipped;
			}

			ts = floor_ts;
		}

		if (frames) {
			size_t offset;
			float level = a->volume;
			float norm = 1.0f;
			float target;
			float step;

			if (!a->have) {
				a->ts = ts;
				a->frames = 0;
				a->have = true;
			}

			offset = (size_t)ap_ns_to_frames(sample_rate, ts - a->ts);

			if (!ap_mix_reserve(a, offset + frames)) {
				/* Further ahead than the mixer will hold, which
				 * takes a source whose timestamps have run away.
				 * Start again from this packet. */
				ap_mix_reset(a, channels, sample_rate);
				a->ts = ts;
				a->have = true;
				offset = 0;

				if (!ap_mix_reserve(a, frames))
					frames = 0;
			}

			if (!deck->from_transition)
				norm = ap_deck_norm_gain(a, deck, audio, skipped, frames, channels, level);

			target = muted ? 0.0f : deck->gain * level * norm;
			step = frames ? (target - deck->applied_gain) / (float)frames : 0.0f;

			for (size_t ch = 0; ch < channels && frames; ch++) {
				const float *in = (const float *)audio->data[ch];
				float *out = a->buf[ch] + offset;
				float gain = deck->applied_gain;

				if (!in)
					continue;

				in += skipped;

				for (size_t i = 0; i < frames; i++) {
					out[i] += in[i] * gain;
					gain += step;
				}
			}

			if (frames)
				deck->applied_gain = target;
		}

		deck->end_ts = ts + ap_frames_to_ns(sample_rate, frames);
		deck->arrived = arrived;
		deck->producing = true;
	}

	ap_mix_flush(ap);

unlock:
	pthread_mutex_unlock(&a->mutex);
}

static void ap_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	ap_mix_submit(param, source, audio, muted);
}

/* ---- following the transition ---- */

/* What the transition on screen does to the audio of the two items it holds,
 * read alongside the transition itself. */
struct ap_tr_audio {
	bool running;
	bool is_stinger;
	bool to_color;
	uint32_t stinger_point_ms;
	float switch_point;
};

static obs_source_t *get_transition_audio(struct ad_playlist *ap, struct ap_tr_audio *info)
{
	obs_source_t *tr;

	pthread_mutex_lock(&ap->mutex);
	tr = obs_source_get_ref(ap->transition);
	info->running = os_atomic_load_bool(&ap->tr_running);
	info->is_stinger = ap->use_stinger;
	info->to_color = ap->use_to_color;
	info->stinger_point_ms = ap->stinger_point_ms;
	info->switch_point = ap->switch_point;
	pthread_mutex_unlock(&ap->mutex);

	return tr;
}

static inline float ap_clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline float ap_smoothstep(float edge0, float edge1, float x)
{
	float t = edge1 > edge0 ? ap_clamp01((x - edge0) / (edge1 - edge0)) : (x >= edge1 ? 1.0f : 0.0f);
	return t * t * (3.0f - 2.0f * t);
}

static inline float ap_lerp(float a, float b, float x)
{
	return a + (b - a) * x;
}

/* What the two halves of a running transition are played at, worked out the
 * way the transition itself works them out: a stinger (fading out and back in,
 * its default) takes the outgoing item down to nothing by its transition point
 * and brings the incoming one up from there, fade to color dips through
 * silence at its switch point, and everything else crosses the two over.
 * 'stinger_ms' is the length of the stinger video, 0 when it is not known. */
static void ap_transition_gains(const struct ap_tr_audio *info, int64_t stinger_ms, float t, float *out, float *in)
{
	t = ap_clamp01(t);

	if (info->is_stinger) {
		float point = stinger_ms > 0 ? (float)info->stinger_point_ms / (float)stinger_ms : 0.5f;

		if (point < 0.001f)
			point = 0.001f;
		else if (point > 0.999f)
			point = 0.999f;

		*out = 1.0f - ap_clamp01(t / point);
		*in = 1.0f - ap_clamp01((1.0f - t) / (1.0f - point));
		return;
	}

	if (info->to_color) {
		float sp = info->switch_point;

		*out = ap_lerp(1.0f - t, 0.0f, ap_smoothstep(0.0f, sp, t));
		*in = ap_lerp(0.0f, t, ap_smoothstep(sp, 1.0f, t));
		return;
	}

	*out = 1.0f - t;
	*in = t;
}

/* What the mixer should be listening to. */
struct ap_audio_scan {
	obs_source_t *want[AP_NUM_DECKS];
	float gain[AP_NUM_DECKS];
	bool from_transition[AP_NUM_DECKS];
	char *path[AP_NUM_DECKS];
	size_t num;
	/* the longest media the transition plays of its own: the stinger */
	int64_t own_ms;
};

static size_t ap_audio_scan_find(const struct ap_audio_scan *scan, obs_source_t *source)
{
	for (size_t i = 0; i < scan->num; i++) {
		if (scan->want[i] == source)
			return i;
	}

	return DARRAY_INVALID;
}

/* A source that stands in more than one place is played once, at everything it
 * is asked for put together: the two halves of a transition are the same item
 * whenever a one item playlist loops. */
static size_t ap_audio_scan_add(struct ap_audio_scan *scan, obs_source_t *source, float gain, bool from_transition)
{
	size_t found;

	if (!source)
		return DARRAY_INVALID;

	found = ap_audio_scan_find(scan, source);
	if (found != DARRAY_INVALID) {
		scan->gain[found] += gain;
		return found;
	}

	if (scan->num == AP_NUM_DECKS)
		return DARRAY_INVALID;

	scan->want[scan->num] = obs_source_get_ref(source);
	scan->gain[scan->num] = gain;
	scan->from_transition[scan->num] = from_transition;
	scan->path[scan->num] = NULL;

	return scan->num++;
}

/* Picks up whatever the transition plays on top of the two halves it holds,
 * which for a stinger is its own video and the matte alongside it. */
static void ap_audio_scan_transition(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct ap_audio_scan *scan = param;
	int64_t ms;

	UNUSED_PARAMETER(parent);

	if (ap_audio_scan_find(scan, child) != DARRAY_INVALID)
		return;

	ms = obs_source_media_get_duration(child);
	if (ms > scan->own_ms)
		scan->own_ms = ms;

	ap_audio_scan_add(scan, child, 1.0f, true);
}

/* Finds the playlist file a source plays. Expects ap->mutex to be held. */
static const char *ap_item_path(struct ad_playlist *ap, obs_source_t *source)
{
	for (size_t i = 0; i < ap->items.num; i++) {
		if (ap->items.array[i].source == source)
			return ap->items.array[i].path;
	}

	return NULL;
}

/* A file that has left the mixer, for the log. */
struct ap_audio_report {
	char *path;
	double lufs;
	double db;
	bool measured;
};

/* Takes a deck off its source. Expects the mixer's lock to be held. */
static void ap_deck_retire(struct ap_audio *a, struct ap_deck *deck, struct ap_audio_report *report)
{
	ap_record_save(a, deck->path, &deck->meter);

	report->path = deck->path;
	report->measured = deck->path && deck->meter.blocks >= LN_MIN_BLOCKS;
	report->lufs = ap_meter_integrated(&deck->meter);
	report->db = deck->norm_db;

	memset(deck, 0, sizeof(*deck));
}

static void ap_audio_report_log(struct ad_playlist *ap, struct ap_audio_report *report, bool normalize)
{
	if (!report->path)
		return;

	if (report->measured && normalize)
		info("audio: '%s' measured %.1f LUFS, played at %+.1f dB", report->path, report->lufs, report->db);
	else if (report->measured)
		info("audio: '%s' measured %.1f LUFS", report->path, report->lufs);

	bfree(report->path);
	report->path = NULL;
}

/* Points the mixer at the sources the transition on screen is playing and sets
 * what each of them is played at. Runs on the tick with no lock held: putting a
 * tap on a source takes that source's own lock, so it is never done under the
 * mixer's. */
static void ap_audio_sync(struct ad_playlist *ap)
{
	struct ap_audio *a = &ap->audio;
	struct ap_tr_audio info;
	struct ap_audio_scan scan = {0};
	struct ap_audio_report reports[AP_NUM_DECKS] = {0};
	bool placed[AP_NUM_DECKS] = {false};
	obs_source_t *taken[AP_NUM_DECKS] = {NULL};
	obs_source_t *dropped[AP_NUM_DECKS] = {NULL};
	obs_source_t *transition = get_transition_audio(ap, &info);
	bool normalize;

	if (transition) {
		obs_source_t *from = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);
		obs_source_t *into = NULL;
		size_t from_idx;
		size_t into_idx = DARRAY_INVALID;
		bool active;

		/* An incoming source only stands in the transition while it is
		 * running; one to nothing (hiding at the end of the playlist)
		 * has none, and is known by the flag alone. */
		into = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
		active = into || info.running;

		from_idx = ap_audio_scan_add(&scan, from, 0.0f, false);
		into_idx = ap_audio_scan_add(&scan, into, 0.0f, false);

		/* the halves are in already, so this only adds what the
		 * transition brought along itself */
		obs_source_enum_active_sources(transition, ap_audio_scan_transition, &scan);

		if (active) {
			float from_gain;
			float into_gain;

			ap_transition_gains(&info, scan.own_ms, obs_transition_get_time(transition), &from_gain,
					    &into_gain);

			if (from_idx != DARRAY_INVALID)
				scan.gain[from_idx] += from_gain;
			if (into_idx != DARRAY_INVALID)
				scan.gain[into_idx] += into_gain;
		} else if (from_idx != DARRAY_INVALID) {
			scan.gain[from_idx] = 1.0f;
		}

		obs_source_release(from);
		obs_source_release(into);
		obs_source_release(transition);
	}

	/* the playlist files, so what they measure can be remembered */
	pthread_mutex_lock(&ap->mutex);
	for (size_t j = 0; j < scan.num; j++) {
		const char *path = scan.from_transition[j] ? NULL : ap_item_path(ap, scan.want[j]);
		scan.path[j] = path ? bstrdup(path) : NULL;
	}
	pthread_mutex_unlock(&ap->mutex);

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		obs_source_t *held = a->deck[i].source;

		if (held && ap_audio_scan_find(&scan, held) == DARRAY_INVALID) {
			obs_source_remove_audio_capture_callback(held, ap_audio_capture, ap);
			dropped[i] = held;
		}
	}

	pthread_mutex_lock(&a->mutex);

	normalize = a->normalize;

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		if (dropped[i])
			ap_deck_retire(a, &a->deck[i], &reports[i]);
	}

	/* a source that is already tapped keeps its deck, so a crossfade that
	 * is running rides on rather than starting over from silence */
	for (size_t j = 0; j < scan.num; j++) {
		for (size_t i = 0; i < AP_NUM_DECKS; i++) {
			if (a->deck[i].source != scan.want[j])
				continue;

			a->deck[i].gain = scan.gain[j];
			a->deck[i].from_transition = scan.from_transition[j];
			placed[j] = true;
			break;
		}
	}

	for (size_t j = 0; j < scan.num; j++) {
		if (placed[j])
			continue;

		for (size_t i = 0; i < AP_NUM_DECKS; i++) {
			struct ap_deck *deck = &a->deck[i];

			if (deck->source)
				continue;

			memset(deck, 0, sizeof(*deck));
			deck->source = obs_source_get_ref(scan.want[j]);
			deck->gain = scan.gain[j];
			deck->from_transition = scan.from_transition[j];
			deck->path = scan.path[j];
			scan.path[j] = NULL;
			ap_deck_seed(a, deck);

			taken[i] = deck->source;
			placed[j] = true;
			break;
		}
	}

	pthread_mutex_unlock(&a->mutex);

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		if (!taken[i])
			continue;

		/* Everything the playlist listens to is played by the playlist
		 * and by nothing else. The items are opened without mixers of
		 * their own; a stinger's own video is not the playlist's to
		 * open, so it is quietened here as it is picked up. */
		obs_source_set_audio_mixers(taken[i], 0);
		obs_source_add_audio_capture_callback(taken[i], ap_audio_capture, ap);
	}

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		if (dropped[i])
			obs_source_release(dropped[i]);
		ap_audio_report_log(ap, &reports[i], normalize);
	}

	for (size_t j = 0; j < scan.num; j++) {
		obs_source_release(scan.want[j]);
		bfree(scan.path[j]);
	}

	/* a source that has gone quiet, or gone entirely, leaves the last of its
	 * audio behind; nothing is going to add to it, so it goes out now */
	pthread_mutex_lock(&a->mutex);
	ap_mix_flush(ap);
	pthread_mutex_unlock(&a->mutex);
}

/* takes every tap off and drops what the mixer is holding */
static void ap_audio_free(struct ad_playlist *ap)
{
	struct ap_audio *a = &ap->audio;
	obs_source_t *held[AP_NUM_DECKS];

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		held[i] = a->deck[i].source;

		if (held[i])
			obs_source_remove_audio_capture_callback(held[i], ap_audio_capture, ap);
	}

	pthread_mutex_lock(&a->mutex);
	for (size_t i = 0; i < AP_NUM_DECKS; i++)
		bfree(a->deck[i].path);
	memset(a->deck, 0, sizeof(a->deck));
	ap_mix_reset(a, 0, 0);
	pthread_mutex_unlock(&a->mutex);

	for (size_t i = 0; i < AP_NUM_DECKS; i++) {
		if (held[i])
			obs_source_release(held[i]);
	}
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

	/* the items stop being tapped before anything lets go of them */
	ap_audio_free(ap);

	if (ap->stop_pending)
		obs_source_release(ap->stop_pending);

	if (ap->transition)
		connect_transition(ap, ap->transition, false);
	obs_source_release(ap->transition);
	free_items(&ap->items);
	da_free(ap->order);
	bfree(ap->tr_type);
	bfree(ap->tr_settings_sig);
	si_paths_free(&ap->configured_paths);
	si_missing_free(&ap->missing_paths);

	for (size_t i = 0; i < ap->audio.records.num; i++)
		bfree(ap->audio.records.array[i].path);
	da_free(ap->audio.records);

	pthread_mutex_destroy(&ap->audio.mutex);
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
	si_layout_init(&ap->layout);
	ap->audio.volume = 1.0f;

	da_init(ap->items);
	da_init(ap->order);
	da_init(ap->audio.records);
	da_init(ap->configured_paths);
	da_init(ap->missing_paths);

	pthread_mutex_init_value(&ap->mutex);
	if (pthread_mutex_init_recursive(&ap->mutex) != 0) {
		bfree(ap);
		return NULL;
	}

	pthread_mutex_init_value(&ap->audio.mutex);
	if (pthread_mutex_init(&ap->audio.mutex, NULL) != 0) {
		pthread_mutex_destroy(&ap->mutex);
		bfree(ap);
		return NULL;
	}

	si_media_hotkeys_register(
		source,
		&(struct si_hotkey_names){"AdPlaylist.PlayPause", "AdPlaylist.Restart", "AdPlaylist.Stop",
					  "AdPlaylist.NextItem", "AdPlaylist.PreviousItem"},
		&(struct si_hotkey_names){T_HK_PLAY_PAUSE, T_HK_RESTART, T_HK_STOP, T_HK_NEXT, T_HK_PREVIOUS});

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

	last_item = !ap->loop && ap->order_pos + 1 >= ap->order.num;

	if (item_reached_end(ap, item, last_item && !ap->hide_when_done))
		advance_item(ap, true);

finish:
	pthread_mutex_unlock(&ap->mutex);

	/* Follow the transition with the audio, after any switch above rather
	 * than before it, so an item going up is listened to from the tick it
	 * goes up. Taps are put on sources with no lock held. */
	ap_audio_sync(ap);
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
	obs_data_set_default_bool(settings, S_NORMALIZE, true);
	obs_data_set_default_int(settings, S_NORM_TARGET, -18);
	obs_data_set_default_int(settings, S_NORM_MAX_BOOST, 12);

	obs_data_set_default_string(settings, S_TRANSITION, SI_TR_FADE);
	obs_data_set_default_int(settings, S_TR_DURATION, 500);
	obs_data_set_default_string(settings, S_TR_TIMING, TIMING_OVERLAP);
	si_transition_defaults(settings, &tr_keys);
	si_layout_defaults(settings);
}

static bool transition_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const char *type = obs_data_get_string(settings, S_TRANSITION);
	bool is_cut = astrcmpi(type, SI_TR_CUT) == 0;
	bool is_stinger = astrcmpi(type, SI_TR_STINGER) == 0;

	UNUSED_PARAMETER(p);

	obs_property_set_visible(obs_properties_get(props, S_TR_DURATION), !is_cut && !is_stinger);
	obs_property_set_visible(obs_properties_get(props, S_TR_TIMING), !is_cut);
	si_transition_options_visible(props, &tr_keys, type);

	return true;
}

static bool normalize_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	bool on = obs_data_get_bool(settings, S_NORMALIZE);

	UNUSED_PARAMETER(p);

	obs_property_set_visible(obs_properties_get(props, S_NORM_TARGET), on);
	obs_property_set_visible(obs_properties_get(props, S_NORM_MAX_BOOST), on);

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

/* clang-format on */

static obs_properties_t *ap_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *playback = obs_properties_create();
	obs_properties_t *audio = obs_properties_create();
	obs_properties_t *transition = obs_properties_create();
	struct ad_playlist *ap = data;
	struct dstr last_dir = {0};
	obs_property_t *p;

	/* ----------------- playlist ----------------- */

	if (ap) {
		pthread_mutex_lock(&ap->mutex);
		if (ap->items.num)
			si_path_directory(ap->items.array[ap->items.num - 1].path, &last_dir);
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

	obs_properties_add_bool(playback, S_PRELOAD, T_PRELOAD);
	obs_properties_add_bool(playback, S_HW_DECODE, T_HW_DECODE);

	obs_properties_add_group(props, S_GROUP_PLAYBACK, T_GROUP_PLAYBACK, OBS_GROUP_NORMAL, playback);

	/* ----------------- audio ----------------- */

	p = obs_properties_add_int_slider(audio, S_VOLUME, T_VOLUME, 0, 100, 1);
	obs_property_int_set_suffix(p, " %");
	obs_property_set_long_description(p, T_VOLUME_DESC);

	p = obs_properties_add_bool(audio, S_NORMALIZE, T_NORMALIZE);
	obs_property_set_long_description(p, T_NORMALIZE_DESC);
	obs_property_set_modified_callback(p, normalize_changed);

	p = obs_properties_add_int_slider(audio, S_NORM_TARGET, T_NORM_TARGET, -36, -6, 1);
	obs_property_int_set_suffix(p, " LUFS");
	obs_property_set_long_description(p, T_NORM_TARGET_DESC);

	p = obs_properties_add_int_slider(audio, S_NORM_MAX_BOOST, T_NORM_MAX_BOOST, 0, 24, 1);
	obs_property_int_set_suffix(p, " dB");
	obs_property_set_long_description(p, T_NORM_MAX_BOOST_DESC);

	obs_properties_add_group(props, S_GROUP_AUDIO, T_GROUP_AUDIO, OBS_GROUP_NORMAL, audio);

	/* ----------------- transition ----------------- */

	p = obs_properties_add_list(transition, S_TRANSITION, T_TRANSITION, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	si_transition_list_add_builtins(p);
	obs_property_set_modified_callback(p, transition_changed);

	p = obs_properties_add_int(transition, S_TR_DURATION, T_TR_DURATION, 0, 60000, 50);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(transition, S_TR_TIMING, T_TR_TIMING, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_TR_TIMING_OVERLAP, TIMING_OVERLAP);
	obs_property_list_add_string(p, T_TR_TIMING_AFTER, TIMING_AFTER);

	si_transition_option_properties(transition, &tr_keys);

	obs_properties_add_group(props, S_GROUP_TRANSITION, T_GROUP_TRANSITION, OBS_GROUP_NORMAL, transition);

	/* ----------------- appearance ----------------- */

	si_layout_properties(props);

	return props;
}

static obs_missing_files_t *ap_missing_files(void *data)
{
	struct ad_playlist *ap = data;
	return si_missing_files(ap->source, &ap->missing_paths);
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
	/* The playlist renders its picture itself and hands its audio over
	 * itself, the way the browser source does. It is deliberately not
	 * composite: obs_register_source() refuses OBS_SOURCE_AUDIO alongside
	 * OBS_SOURCE_COMPOSITE, and without OBS_SOURCE_AUDIO there is no
	 * playlist in the OBS audio mixer to fade, mute, monitor, filter or
	 * route. The items are mixed by the audio section above instead. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = ap_get_name,
	.create = ap_create,
	.destroy = ap_destroy,
	.update = ap_update,
	.show = ap_show,
	.hide = ap_hide,
	.video_render = ap_video_render,
	.video_tick = ap_video_tick,
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
