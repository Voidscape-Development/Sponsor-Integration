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
 * Runtime tests: real libobs, headless under Xvfb, with the stock image source,
 * transitions and media plugins and this plugin loaded. Each scenario drives a
 * source and checks what it signals and what it actually renders - the center
 * pixel, sampled every 50 ms - against what it should do.
 *
 * The test program also stands in for the OBS frontend: it defines the three
 * frontend functions the plugin looks up at runtime, so the scene transition
 * path can be tested without the OBS UI.
 *
 *   runtime-test <scenario> <image dir> <plugin .so> <plugin data dir>
 *                [stinger video]
 *
 * Environment: OBS_PLUGIN_DIR, OBS_PLUGIN_DATA_DIR - where the stock OBS
 * plugins live (default: Ubuntu's locations).
 *
 * Exits 0 when every check passes. See run-runtime-tests.sh.
 */

#include <obs.h>
#include <obs-nix-platform.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>
#include <util/dstr.h>

#include <X11/Xlib.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT_SYM __attribute__((visibility("default")))

/* ------------------------------------------------------------------------- */
/* recording                                                                  */

enum color {
	C_EMPTY,
	C_RED,
	C_GREEN,
	C_BLUE,
	C_WHITE,
	C_BLACK,
	C_MIX,
	C_NONE,
};

static const char *color_names[] = {"empty", "red", "green", "blue", "white", "black", "mix", "none"};

struct sample {
	double t;
	uint8_t r, g, b, a;
	enum color c;
};

enum event_kind {
	EV_CHANGED,
	EV_ENDED,
	EV_MARK,
};

struct event {
	double t;
	enum event_kind kind;
	long long index;
	char what[64];
};

static uint64_t t0;
static DARRAY(struct sample) samples;
static DARRAY(struct event) events;
static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static int failures = 0;
static bool verbose = false;

static double now(void)
{
	return (double)(os_gettime_ns() - t0) / 1e9;
}

static void record(enum event_kind kind, long long index, const char *what)
{
	struct event ev = {now(), kind, index, ""};

	snprintf(ev.what, sizeof(ev.what), "%s", what ? what : "");

	pthread_mutex_lock(&rec_mutex);
	da_push_back(events, &ev);
	pthread_mutex_unlock(&rec_mutex);

	if (verbose)
		printf("%6.2f  event %s %lld %s\n", ev.t,
		       kind == EV_CHANGED ? "changed"
		       : kind == EV_ENDED ? "ended"
					  : "mark",
		       index, ev.what);
}

static void mark(const char *what)
{
	record(EV_MARK, -1, what);
}

static void on_changed(void *data, calldata_t *cd)
{
	const char *path = calldata_string(cd, "path");
	const char *slash = path ? strrchr(path, '/') : NULL;

	(void)data;
	record(EV_CHANGED, calldata_int(cd, "index"), slash ? slash + 1 : path);
}

static void on_ended(void *data, calldata_t *cd)
{
	(void)data;
	(void)cd;
	record(EV_ENDED, -1, NULL);
}

static void check(bool ok, const char *fmt, ...)
{
	va_list args;

	printf("  %s  ", ok ? "ok  " : "FAIL");
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\n");

	if (!ok)
		failures++;
}

/* ------------------------------------------------------------------------- */
/* sampling                                                                   */

static gs_texrender_t *texrender;
static gs_stagesurf_t *stagesurf;
static uint32_t stage_cx, stage_cy;

static enum color classify(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (a < 10)
		return C_EMPTY;
	if (a < 245)
		return C_MIX;
	if (r > 225 && g < 30 && b < 30)
		return C_RED;
	if (g > 225 && r < 30 && b < 30)
		return C_GREEN;
	if (b > 225 && r < 30 && g < 30)
		return C_BLUE;
	if (r > 200 && g > 200 && b > 200)
		return C_WHITE;
	if (r < 40 && g < 40 && b < 40)
		return C_BLACK;
	return C_MIX;
}

/* The center pixel of the source as it renders right now. */
static void sample(obs_source_t *source)
{
	uint32_t cx = obs_source_get_width(source);
	uint32_t cy = obs_source_get_height(source);
	struct sample s = {now(), 0, 0, 0, 0, C_NONE};
	uint8_t *data;
	uint32_t linesize;

	if (cx && cy) {
		obs_enter_graphics();

		if (!texrender)
			texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!stagesurf || stage_cx != cx || stage_cy != cy) {
			gs_stagesurface_destroy(stagesurf);
			stagesurf = gs_stagesurface_create(cx, cy, GS_RGBA);
			stage_cx = cx;
			stage_cy = cy;
		}

		gs_texrender_reset(texrender);
		if (gs_texrender_begin(texrender, cx, cy)) {
			struct vec4 clear;

			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
			obs_source_video_render(source);
			gs_texrender_end(texrender);
		}

		gs_stage_texture(stagesurf, gs_texrender_get_texture(texrender));
		if (gs_stagesurface_map(stagesurf, &data, &linesize)) {
			uint8_t *px = data + (cy / 2) * linesize + (cx / 2) * 4;

			s.r = px[0];
			s.g = px[1];
			s.b = px[2];
			s.a = px[3];
			s.c = classify(s.r, s.g, s.b, s.a);
			gs_stagesurface_unmap(stagesurf);
		}

		obs_leave_graphics();
	}

	pthread_mutex_lock(&rec_mutex);
	if (verbose && (!samples.num || samples.array[samples.num - 1].c != s.c))
		printf("%6.2f  pixel %-5s r%3d g%3d b%3d a%3d\n", s.t, color_names[s.c], s.r, s.g, s.b, s.a);
	da_push_back(samples, &s);
	pthread_mutex_unlock(&rec_mutex);
}

typedef void (*step_fn)(obs_source_t *source, double t, int *step);

static void run(obs_source_t *source, double seconds, step_fn step)
{
	int state = 0;
	double end = now() + seconds;

	while (now() < end) {
		os_sleep_ms(50);
		sample(source);
		if (step)
			step(source, now(), &state);
	}
}

/* ------------------------------------------------------------------------- */
/* queries over what was recorded                                             */

static size_t count_changes(void)
{
	size_t n = 0;

	for (size_t i = 0; i < events.num; i++)
		n += events.array[i].kind == EV_CHANGED;

	return n;
}

/* the n-th change (0-based), or NULL */
static struct event *change(size_t n)
{
	for (size_t i = 0; i < events.num; i++) {
		if (events.array[i].kind == EV_CHANGED && n-- == 0)
			return &events.array[i];
	}

	return NULL;
}

/* the first change at or after `t`, or NULL */
static struct event *change_after(double t)
{
	for (size_t i = 0; i < events.num; i++) {
		if (events.array[i].kind == EV_CHANGED && events.array[i].t >= t)
			return &events.array[i];
	}

	return NULL;
}

static size_t count_color(double from, double to, enum color c)
{
	size_t n = 0;

	for (size_t i = 0; i < samples.num; i++) {
		struct sample *s = &samples.array[i];

		if (s->t >= from && s->t < to && s->c == c)
			n++;
	}

	return n;
}

static size_t count_samples(double from, double to)
{
	size_t n = 0;

	for (size_t i = 0; i < samples.num; i++)
		n += samples.array[i].t >= from && samples.array[i].t < to;

	return n;
}

/* samples blending red and blue at once - what a fade between them shows */
static size_t count_red_blue_blend(double from, double to)
{
	size_t n = 0;

	for (size_t i = 0; i < samples.num; i++) {
		struct sample *s = &samples.array[i];

		if (s->t >= from && s->t < to && s->r > 40 && s->b > 40 && s->a > 200)
			n++;
	}

	return n;
}

/* samples with two primaries at once - any blend between two images */
static size_t count_blends(double from, double to)
{
	size_t n = 0;

	for (size_t i = 0; i < samples.num; i++) {
		struct sample *s = &samples.array[i];
		int lit = (s->r > 40) + (s->g > 40) + (s->b > 40);

		if (s->t >= from && s->t < to && s->a > 200 && lit == 2)
			n++;
	}

	return n;
}

/* samples that are not fully opaque yet - something fading in or out */
static size_t count_translucent(double from, double to)
{
	size_t n = 0;

	for (size_t i = 0; i < samples.num; i++) {
		struct sample *s = &samples.array[i];

		if (s->t >= from && s->t < to && s->c != C_NONE && s->a < 200)
			n++;
	}

	return n;
}

/* samples drawn but not fully opaque - two images each fading on their own,
 * rather than one crossfade between them, which stays opaque */
static size_t count_see_through(double from, double to)
{
	size_t n = 0;

	for (size_t i = 0; i < samples.num; i++) {
		struct sample *s = &samples.array[i];

		if (s->t >= from && s->t < to && s->c != C_NONE && s->a >= 10 && s->a < 245)
			n++;
	}

	return n;
}

/* Prints what was recorded between `from` and `to`, for a check that failed
 * somewhere it cannot be reproduced. */
static void dump(double from, double to)
{
	for (size_t i = 0; i < events.num; i++) {
		struct event *e = &events.array[i];

		if (e->t >= from && e->t < to)
			printf("%6.2f  event %s %lld %s\n", e->t,
			       e->kind == EV_CHANGED ? "changed"
			       : e->kind == EV_ENDED ? "ended"
						     : "mark",
			       e->index, e->what);
	}
	for (size_t i = 0; i < samples.num; i++) {
		struct sample *s = &samples.array[i];

		if (s->t >= from && s->t < to)
			printf("%6.2f  pixel %-5s r%3d g%3d b%3d a%3d\n", s->t, color_names[s->c], s->r, s->g, s->b,
			       s->a);
	}
}

static enum color last_color(void)
{
	return samples.num ? samples.array[samples.num - 1].c : C_NONE;
}

/* ------------------------------------------------------------------------- */
/* the fake frontend                                                          */

struct fake_source_list {
	DARRAY(obs_source_t *) sources;
};

typedef void (*fake_event_cb)(int event, void *data);

#define FAKE_MAX_TRANSITIONS 8
#define FRONTEND_EVENT_TRANSITION_LIST_CHANGED 12
#define FRONTEND_EVENT_FINISHED_LOADING 26

static obs_source_t *fake_transitions[FAKE_MAX_TRANSITIONS];
static size_t fake_num_transitions;
static fake_event_cb fake_cb;
static void *fake_cb_data;

EXPORT_SYM void obs_frontend_get_transitions(struct fake_source_list *list)
{
	for (size_t i = 0; i < fake_num_transitions; i++) {
		obs_source_t *tr = obs_source_get_ref(fake_transitions[i]);

		da_push_back(list->sources, &tr);
	}
}

EXPORT_SYM void obs_frontend_add_event_callback(fake_event_cb cb, void *data)
{
	fake_cb = cb;
	fake_cb_data = data;
}

EXPORT_SYM void obs_frontend_remove_event_callback(fake_event_cb cb, void *data)
{
	(void)data;
	if (fake_cb == cb)
		fake_cb = NULL;
}

static obs_source_t *fake_add_transition(const char *id, const char *name, obs_data_t *settings)
{
	obs_source_t *tr = obs_source_create_private(id, name, settings);

	fake_transitions[fake_num_transitions++] = tr;
	return tr;
}

static void fake_fire(int event)
{
	if (fake_cb)
		fake_cb(event, fake_cb_data);
}

static void fake_release_all(void)
{
	for (size_t i = 0; i < fake_num_transitions; i++)
		obs_source_release(fake_transitions[i]);

	fake_num_transitions = 0;
}

/* ------------------------------------------------------------------------- */
/* setup                                                                      */

static const char *image_dir;
static const char *stinger_path;
static obs_scene_t *scene;

static void load_module(const char *bin, const char *data)
{
	obs_module_t *module;

	if (obs_open_module(&module, bin, data) != MODULE_SUCCESS || !obs_init_module(module)) {
		fprintf(stderr, "failed to load %s\n", bin);
		exit(2);
	}
}

static void load_stock_module(const char *name)
{
	const char *bin_dir = getenv("OBS_PLUGIN_DIR");
	const char *data_dir = getenv("OBS_PLUGIN_DATA_DIR");
	struct dstr bin = {0};
	struct dstr data = {0};

	dstr_printf(&bin, "%s/%s.so", bin_dir ? bin_dir : "/usr/lib/x86_64-linux-gnu/obs-plugins", name);
	dstr_printf(&data, "%s/%s", data_dir ? data_dir : "/usr/share/obs/obs-plugins", name);
	load_module(bin.array, data.array);
	dstr_free(&bin);
	dstr_free(&data);
}

static bool start_obs(const char *plugin, const char *plugin_data)
{
	struct obs_audio_info ai = {48000, SPEAKERS_STEREO};
	struct obs_video_info ovi = {0};

	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(XOpenDisplay(NULL));

	if (!obs_startup("en-US", NULL, NULL))
		return false;

	obs_reset_audio(&ai);

	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 640;
	ovi.base_height = ovi.output_height = 360;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return false;

	load_stock_module("image-source");
	load_stock_module("obs-transitions");
	load_stock_module("obs-ffmpeg");
	load_module(plugin, plugin_data);
	obs_post_load_modules();

	return true;
}

static obs_data_t *gallery_settings(void)
{
	obs_data_t *s = obs_data_create();
	obs_data_array_t *files = obs_data_array_create();
	obs_data_t *f = obs_data_create();

	obs_data_set_string(f, "value", image_dir);
	obs_data_array_push_back(files, f);
	obs_data_release(f);
	obs_data_set_array(s, "files", files);
	obs_data_array_release(files);

	obs_data_set_int(s, "hold_time", 1000);
	obs_data_set_int(s, "normal_duration", 300);
	obs_data_set_int(s, "show_duration", 300);
	obs_data_set_int(s, "hide_duration", 300);

	return s;
}

/* Creates the source and puts it on the program output, so it is showing,
 * ticking and rendering as it would on stream. */
static obs_source_t *start_source(const char *id, obs_data_t *settings, const char *changed_signal,
				  const char *ended_signal)
{
	obs_source_t *source = obs_source_create(id, "under test", settings, NULL);
	signal_handler_t *sh;

	obs_data_release(settings);

	if (!source) {
		fprintf(stderr, "failed to create %s\n", id);
		exit(2);
	}

	sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, changed_signal, on_changed, NULL);
	signal_handler_connect(sh, ended_signal, on_ended, NULL);

	scene = obs_scene_create("test scene");
	obs_scene_add(scene, source);
	obs_set_output_source(0, obs_scene_get_source(scene));

	return source;
}

static obs_source_t *start_gallery(obs_data_t *settings)
{
	return start_source("sponsor_picture_gallery", settings, "gallery_image_changed", "gallery_ended");
}

static void set_item_visible(bool visible)
{
	obs_sceneitem_t *item = obs_scene_find_source(scene, "under test");

	obs_sceneitem_set_visible(item, visible);
}

static void call_proc(obs_source_t *source, const char *name, calldata_t *cd)
{
	proc_handler_call(obs_source_get_proc_handler(source), name, cd);
}

static void call_simple(obs_source_t *source, const char *name)
{
	calldata_t cd;

	calldata_init(&cd);
	call_proc(source, name, &cd);
	calldata_free(&cd);
}

static bool gallery_is_hidden(obs_source_t *source)
{
	calldata_t cd;
	bool hidden;

	calldata_init(&cd);
	call_proc(source, "is_hidden", &cd);
	hidden = calldata_bool(&cd, "hidden");
	calldata_free(&cd);

	return hidden;
}

static void update_string(obs_source_t *source, const char *key, const char *val)
{
	obs_data_t *s = obs_data_create();

	obs_data_set_string(s, key, val);
	obs_source_update(source, s);
	obs_data_release(s);
}

/* ------------------------------------------------------------------------- */
/* scenarios                                                                  */

/* No gap: the first image comes in from nothing, then each image is up for
 * the hold time plus the transition, never leaving an empty frame - each one
 * goes out with Hide over the next coming in with Show. */
static void scenario_nogap(void)
{
	obs_source_t *src = start_gallery(gallery_settings());

	run(src, 6.0, NULL);

	check(count_changes() >= 4, "at least 4 images in 6 s (%zu)", count_changes());
	for (size_t i = 0; i < 4 && change(i); i++)
		check(change(i)->index == (long long)(i % 3), "change %zu shows image %zu (%lld)", i, i % 3,
		      change(i)->index);
	for (size_t i = 1; i < 4 && change(i); i++) {
		double gap = change(i)->t - change(i - 1)->t;

		check(gap > 1.15 && gap < 1.65, "change %zu comes 0.3 s fade + 1.0 s hold after the last (%.2f s)", i,
		      gap);
	}
	check(count_translucent(0.0, 0.35) > 0, "fades in from nothing (Show transition)");
	if (!count_translucent(0.0, 0.35))
		dump(0.0, 0.7);
	/* libobs ends a transition with the picture it went to not yet laid
	 * out - from nothing, not drawn at all for a frame */
	check(count_color(0.1, 0.7, C_EMPTY) == 0, "no empty frame as the first fade in ends (%zu)",
	      count_color(0.1, 0.7, C_EMPTY));
	check(count_color(0.7, 6.0, C_EMPTY) == 0, "never empty once running");
	check(count_blends(1.2, 6.0) > 0, "images fade into each other");
	check(count_see_through(1.2, 6.0) > 0, "each image fades out with Hide over the next fading in with Show");

	obs_source_release(src);
}

/* A gap: out with Hide, empty for the gap, in with Show. */
static void scenario_gap(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;

	obs_data_set_int(s, "gap_time", 600);
	src = start_gallery(s);

	run(src, 8.0, NULL);

	check(count_changes() >= 3, "at least 3 images in 8 s (%zu)", count_changes());
	for (size_t i = 1; i < 4 && change(i); i++) {
		double from = change(i - 1)->t;
		double to = change(i)->t;
		double gap = to - from;

		/* in 0.3 + hold 1.0 + out 0.3 + gap 0.6 */
		check(gap > 2.05 && gap < 2.7, "change %zu comes in + hold + out + gap after the last (%.2f s)", i,
		      gap);
		check(count_color(to - 0.75, to, C_EMPTY) >= 8, "empty for the gap before change %zu (%zu samples)", i,
		      count_color(to - 0.75, to, C_EMPTY));
		check(count_blends(from, to) == 0, "no image ever blends into the next (%zu)", count_blends(from, to));
		check(count_color(to + 0.1, to + 1.0, C_EMPTY) == 0,
		      "no empty frame as change %zu finishes coming in from the gap (%zu)", i,
		      count_color(to + 0.1, to + 1.0, C_EMPTY));
	}

	obs_source_release(src);
}

/* Show set to None: the next image is there at once, underneath the last one
 * going out with Hide - never translucent, never empty. */
static void scenario_show_none(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;

	obs_data_set_string(s, "show_transition", "none");
	src = start_gallery(s);

	run(src, 5.0, NULL);

	check(count_changes() >= 3, "at least 3 images in 5 s (%zu)", count_changes());
	check(count_color(0.7, 5.0, C_EMPTY) == 0, "never empty once running");
	check(count_translucent(0.7, 5.0) == 0, "the next image is fully there underneath (%zu translucent)",
	      count_translucent(0.7, 5.0));
	check(count_blends(1.0, 5.0) > 0, "the last image fades out over the next (Hide)");

	obs_source_release(src);
}

/* Show and Hide both None: the Normal transition goes from image to image. */
static void scenario_show_hide_none(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;

	obs_data_set_string(s, "show_transition", "none");
	obs_data_set_string(s, "hide_transition", "none");
	obs_data_set_string(s, "normal_transition", "fade_to_color");
	src = start_gallery(s);

	run(src, 5.0, NULL);

	check(count_changes() >= 3, "at least 3 images in 5 s (%zu)", count_changes());
	check(count_color(1.2, 5.0, C_BLACK) > 0, "the Normal fade to color plays between images");
	check(count_blends(1.2, 5.0) == 0, "no Show/Hide blend (%zu)", count_blends(1.2, 5.0));

	obs_source_release(src);
}

/* Everything None: plain cuts. */
static void scenario_all_none(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;

	obs_data_set_string(s, "show_transition", "none");
	obs_data_set_string(s, "hide_transition", "none");
	obs_data_set_string(s, "normal_transition", "none");
	src = start_gallery(s);

	run(src, 4.0, NULL);

	check(count_changes() >= 3, "at least 3 images in 4 s (%zu)", count_changes());
	check(count_color(0.3, 4.0, C_EMPTY) == 0, "never empty");
	check(count_color(0.3, 4.0, C_MIX) == 0, "cuts from image to image (%zu mixed)", count_color(0.3, 4.0, C_MIX));

	obs_source_release(src);
}

/* Show/Hide only at the edges: images change with the Normal transition. */
static void scenario_edges_only(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;

	obs_data_set_bool(s, "show_hide_edges_only", true);
	obs_data_set_string(s, "normal_transition", "fade_to_color");
	src = start_gallery(s);

	run(src, 5.0, NULL);

	check(count_changes() >= 3, "at least 3 images in 5 s (%zu)", count_changes());
	check(count_translucent(0.0, 0.35) > 0, "fades in from nothing (Show transition)");
	check(count_color(1.2, 5.0, C_BLACK) > 0, "the Normal fade to color plays between images");
	check(count_blends(1.2, 5.0) == 0, "no Show/Hide between images (%zu)", count_blends(1.2, 5.0));
	check(count_see_through(1.2, 5.0) == 0, "no image fades out on its own between images");

	obs_source_release(src);
}

/* A Loop wrap transition of its own: a cut at the loop point, fades
 * elsewhere (with Show/Hide only at the edges, so the wrap replaces the Normal
 * transition). */
static void scenario_wrap(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;
	struct event *wrap;

	obs_data_set_bool(s, "show_hide_edges_only", true);
	obs_data_set_string(s, "wrap_transition", "cut");
	src = start_gallery(s);

	run(src, 5.0, NULL);

	wrap = change(3);
	check(wrap && wrap->index == 0, "the 4th change wraps to the first image");
	if (wrap) {
		check(count_red_blue_blend(wrap->t - 0.1, wrap->t + 0.5) == 0, "the wrap cuts from blue to red");
		check(count_blends(change(1)->t, change(2)->t + 0.4) > 0, "other changes fade");
		/* the wrap slot had never held a picture before: it must not
		 * draw a frame before it has been laid out */
		check(count_color(0.7, 5.0, C_EMPTY) == 0, "no empty frame, including at the first cut");
	}

	obs_source_release(src);
}

/* No loop, hide when done: ends once and goes out to nothing. */
static void scenario_end(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;
	size_t ended = 0;
	double ended_at = 0.0;

	obs_data_set_bool(s, "loop", false);
	obs_data_set_bool(s, "hide_when_done", true);
	src = start_gallery(s);

	run(src, 5.5, NULL);

	for (size_t i = 0; i < events.num; i++) {
		if (events.array[i].kind == EV_ENDED) {
			ended++;
			ended_at = events.array[i].t;
		}
	}

	check(count_changes() == 3, "each image once (%zu)", count_changes());
	check(ended == 1, "ended exactly once (%zu)", ended);
	check(!change_after(ended_at + 0.01), "nothing after the end");
	check(last_color() == C_EMPTY, "hidden after the end (%s)", color_names[last_color()]);
	check(count_blends(ended_at, ended_at + 0.5) == 0 && count_color(ended_at, ended_at + 0.5, C_MIX) > 0,
	      "fades out rather than cutting");

	obs_source_release(src);
}

static double t_hide, t_show, t_next, t_stop, t_play;
static long long index_before_hide = -1;

static void controls_steps(obs_source_t *src, double t, int *step)
{
	if (*step == 0 && t > 1.6) {
		struct event *last = change(count_changes() - 1);

		index_before_hide = last ? last->index : -1;
		t_hide = t;
		mark("hide_gallery");
		call_simple(src, "hide_gallery");
		(*step)++;
	} else if (*step == 1 && t > 3.0) {
		t_show = t;
		mark("show_gallery");
		call_simple(src, "show_gallery");
		(*step)++;
	} else if (*step == 2 && t > 4.0) {
		t_next = t;
		mark("next");
		obs_source_media_next(src);
		(*step)++;
	} else if (*step == 3 && t > 5.0) {
		t_stop = t;
		mark("stop");
		obs_source_media_stop(src);
		(*step)++;
	} else if (*step == 4 && t > 6.5) {
		t_play = t;
		mark("play");
		obs_source_media_play_pause(src, false);
		(*step)++;
	}
}

static void scenario_controls(void)
{
	obs_source_t *src = start_gallery(gallery_settings());
	struct event *ev;

	run(src, 8.0, controls_steps);

	check(count_color(t_hide + 0.6, t_show, C_EMPTY) == count_samples(t_hide + 0.6, t_show),
	      "hide_gallery takes it out and keeps it out");
	check(count_color(t_hide, t_hide + 0.6, C_MIX) > 0, "hide_gallery fades out");

	ev = change_after(t_show);
	check(ev && ev->t < t_show + 0.2 && ev->index == index_before_hide,
	      "show_gallery brings back the same image (%lld, was %lld)", ev ? ev->index : -1, index_before_hide);
	check(count_color(t_show + 0.6, t_next, C_EMPTY) == 0, "shown after show_gallery");

	ev = change_after(t_next);
	check(ev && ev->t < t_next + 0.2 && ev->index == (index_before_hide + 1) % 3, "next moves on one image");

	check(count_color(t_stop + 0.6, t_play, C_EMPTY) == count_samples(t_stop + 0.6, t_play), "stop takes it out");

	ev = change_after(t_play);
	check(ev && ev->t < t_play + 0.2 && ev->index == 0, "play after stop starts from the first image");
	check(count_color(t_play + 0.4, t_play + 1.2, C_RED) > 0, "the first image is up after play");

	obs_source_release(src);
}

/* The Hide control holds even when the source is hidden and shown again with
 * "restart when visible" on; only show_gallery brings it back. */
static double t_reveal;

static void hidden_restart_steps(obs_source_t *src, double t, int *step)
{
	if (*step == 0 && t > 1.0) {
		mark("hide_gallery");
		call_simple(src, "hide_gallery");
		(*step)++;
	} else if (*step == 1 && t > 2.0) {
		mark("scene item hidden");
		set_item_visible(false);
		(*step)++;
	} else if (*step == 2 && t > 2.5) {
		mark("scene item visible");
		set_item_visible(true);
		(*step)++;
	} else if (*step == 3 && t > 4.0) {
		check(gallery_is_hidden(src), "is_hidden is still true after the source was shown again");
		t_reveal = t;
		mark("show_gallery");
		call_simple(src, "show_gallery");
		(*step)++;
	}
}

static void scenario_hidden_restart(void)
{
	obs_source_t *src = start_gallery(gallery_settings());
	struct event *ev;

	run(src, 5.0, hidden_restart_steps);

	check(count_color(1.6, t_reveal, C_EMPTY) == count_samples(1.6, t_reveal),
	      "stays out through the source being hidden and shown again");
	check(!change_after(1.6) || change_after(1.6)->t >= t_reveal, "nothing is brought in until show_gallery");

	ev = change_after(t_reveal);
	check(ev && ev->index == 0, "show_gallery brings in the first image (the restart moved it to the start)");
	check(last_color() == C_RED, "showing at the end (%s)", color_names[last_color()]);

	obs_source_release(src);
}

/* Changing transition types while it runs keeps the picture - no empty frame
 * while the new transition, created mid-tick, waits for its first tick. */
static void retype_steps(obs_source_t *src, double t, int *step)
{
	if (*step == 0 && t > 2.0) {
		obs_data_t *s = obs_data_create();

		mark("retype");
		obs_data_set_string(s, "normal_transition", "slide");
		obs_data_set_string(s, "wrap_transition", "fade_to_color");
		obs_source_update(src, s);
		obs_data_release(s);
		(*step)++;
	}
}

static void scenario_retype(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;
	size_t before;

	/* the Normal transition is the one on screen */
	obs_data_set_bool(s, "show_hide_edges_only", true);
	src = start_gallery(s);

	run(src, 2.1, retype_steps);
	before = count_changes();
	run(src, 3.0, NULL);

	check(count_changes() >= before + 2, "keeps going after the change (%zu more)", count_changes() - before);
	check(count_color(2.0, 5.1, C_EMPTY) == 0, "never drops the picture");
	check(count_color(2.0, 5.1, C_BLACK) > 0, "the new fade to color wrap is used");

	obs_source_release(src);
}

/*
 * Settings churn: four threads changing transitions, durations, the gap,
 * shuffle and the file list as fast as they can while the gallery runs, which
 * must neither crash nor leak.
 *
 * libobs runs a video source's update() on the graphics thread, so the source
 * itself never sees two at once - but obs_source_update() merges the new
 * settings into the source's settings object first, and libobs' obs_data is
 * not safe with two writers at once. The writers take turns here, as they do
 * in OBS, where settings are changed from the UI thread.
 */
static volatile bool hammering;
static pthread_mutex_t hammer_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *hammer(void *data)
{
	obs_source_t *src = data;
	static const char *types[] = {"fade", "cut", "swipe", "slide", "fade_to_color", "none", "same"};
	unsigned seed = (unsigned)(uintptr_t)pthread_self();

	while (os_atomic_load_bool(&hammering)) {
		obs_data_t *s = obs_data_create();
		int which = rand_r(&seed);

		obs_data_set_string(s, "normal_transition", types[which % 6]);
		obs_data_set_string(s, "wrap_transition", types[(which >> 4) % 7]);
		obs_data_set_string(s, "hide_transition", types[(which >> 8) % 6]);
		obs_data_set_string(s, "show_transition", types[(which >> 24) % 6]);
		obs_data_set_bool(s, "show_hide_edges_only", (which >> 27) % 2);
		obs_data_set_int(s, "normal_duration", 100 + (which >> 12) % 300);
		obs_data_set_int(s, "gap_time", (which >> 16) % 2 ? 0 : 200);
		obs_data_set_bool(s, "shuffle", (which >> 18) % 2);

		if ((which >> 20) % 8 == 0) {
			/* now and then swap the file list too */
			obs_data_array_t *files = obs_data_array_create();
			obs_data_t *f = obs_data_create();
			struct dstr path = {0};

			if ((which >> 23) % 2)
				dstr_copy(&path, image_dir);
			else
				dstr_printf(&path, "%s/2_green.png", image_dir);

			obs_data_set_string(f, "value", path.array);
			obs_data_array_push_back(files, f);
			obs_data_set_array(s, "files", files);
			obs_data_release(f);
			obs_data_array_release(files);
			dstr_free(&path);
		}

		pthread_mutex_lock(&hammer_mutex);
		obs_source_update(src, s);
		pthread_mutex_unlock(&hammer_mutex);
		obs_data_release(s);
		os_sleep_ms(2);
	}

	return NULL;
}

static void scenario_hammer(void)
{
	obs_source_t *src = start_gallery(gallery_settings());
	pthread_t threads[4];
	obs_data_t *s;
	size_t before;

	run(src, 1.0, NULL);

	os_atomic_set_bool(&hammering, true);
	for (size_t i = 0; i < 4; i++)
		pthread_create(&threads[i], NULL, hammer, src);
	run(src, 3.0, NULL);
	os_atomic_set_bool(&hammering, false);
	for (size_t i = 0; i < 4; i++)
		pthread_join(threads[i], NULL);

	mark("settled");
	s = gallery_settings();
	obs_data_set_string(s, "normal_transition", "fade");
	obs_data_set_string(s, "wrap_transition", "same");
	obs_data_set_string(s, "show_transition", "fade");
	obs_data_set_string(s, "hide_transition", "fade");
	obs_data_set_bool(s, "show_hide_edges_only", false);
	obs_data_set_int(s, "gap_time", 0);
	obs_data_set_bool(s, "shuffle", false);
	obs_source_update(src, s);
	obs_data_release(s);

	before = count_changes();
	run(src, 3.5, NULL);

	check(true, "survived 3 s of settings churn from 4 threads");
	check(count_changes() >= before + 2, "rotates normally afterwards (%zu changes)", count_changes() - before);
	check(last_color() != C_NONE && last_color() != C_EMPTY, "showing a picture afterwards (%s)",
	      color_names[last_color()]);

	obs_source_release(src);
}

/* Scene transitions: copies follow edits to the original, a missing one falls
 * back to a fade, and turning up later is noticed. */
static obs_source_t *tint;
static double t_recolor, t_missing, t_appear;

static void scene_steps(obs_source_t *src, double t, int *step)
{
	struct event *c1 = change(1);
	struct event *c2 = change(2);
	struct event *c3 = change(3);

	if (*step == 0 && c1 && t > c1->t + 0.8) {
		obs_data_t *s = obs_data_create();

		t_recolor = t;
		mark("scene transition recolored to black");
		obs_data_set_int(s, "color", 0xFF000000);
		obs_source_update(tint, s);
		obs_data_release(s);
		(*step)++;
	} else if (*step == 1 && c2 && t > c2->t + 0.8) {
		t_missing = t;
		mark("switched to a scene transition that does not exist");
		update_string(src, "normal_scene_transition", "Later");
		(*step)++;
	} else if (*step == 2 && c3 && t > c3->t + 0.8) {
		t_appear = t;
		mark("scene transition 'Later' added");
		fake_add_transition("cut_transition", "Later", NULL);
		fake_fire(FRONTEND_EVENT_TRANSITION_LIST_CHANGED);
		(*step)++;
	}
}

static void scenario_scene(void)
{
	obs_data_t *tint_settings = obs_data_create();
	obs_data_t *s = gallery_settings();
	obs_source_t *src;
	struct event *ev;

	/* OBS loads its scene transitions after the sources */
	obs_data_set_int(tint_settings, "color", 0xFFFFFFFF);
	obs_data_set_int(tint_settings, "switch_point", 50);

	obs_data_set_bool(s, "show_hide_edges_only", true);
	obs_data_set_string(s, "normal_transition", "scene");
	obs_data_set_string(s, "normal_scene_transition", "Tint");
	obs_data_set_int(s, "normal_duration", 600);
	src = start_gallery(s);

	tint = fake_add_transition("fade_to_color_transition", "Tint", tint_settings);
	obs_data_release(tint_settings);
	fake_fire(FRONTEND_EVENT_FINISHED_LOADING);

	run(src, 7.5, scene_steps);

	ev = change(1);
	check(ev && count_color(ev->t, ev->t + 0.7, C_WHITE) > 0, "uses the scene transition once OBS has loaded it");
	ev = change_after(t_recolor);
	check(ev && count_color(ev->t, ev->t + 0.7, C_BLACK) > 0 && count_color(ev->t, ev->t + 0.7, C_WHITE) == 0,
	      "follows an edit to the scene transition");
	ev = change_after(t_missing);
	check(ev && count_blends(ev->t, ev->t + 0.7) > 0 && count_color(ev->t, ev->t + 0.7, C_BLACK) == 0,
	      "falls back to a fade while the scene transition is missing");
	ev = change_after(t_appear);
	check(ev && count_blends(ev->t - 0.05, ev->t + 0.7) == 0 && count_color(ev->t, ev->t + 0.7, C_MIX) == 0,
	      "picks the scene transition up once it is added");

	obs_source_release(src);
	fake_release_all();
}

/* A stinger longer than duration + grace: the hold starts after it, not on
 * the safety timeout. */
static void scenario_stinger(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;
	double gap;

	if (!stinger_path) {
		check(false, "no stinger video was given");
		return;
	}

	obs_data_set_bool(s, "show_hide_edges_only", true);
	obs_data_set_string(s, "normal_transition", "stinger");
	obs_data_set_string(s, "normal_stinger_path", stinger_path);
	obs_data_set_int(s, "normal_stinger_transition_point", 500);
	obs_data_set_int(s, "normal_duration", 300);
	src = start_gallery(s);

	run(src, 17.5, NULL);

	check(count_changes() >= 3, "three images (%zu)", count_changes());
	if (count_changes() >= 3) {
		gap = change(2)->t - change(1)->t;
		/* 13 s stinger + 1 s hold */
		check(gap > 13.6 && gap < 15.5, "the hold starts after the 13 s stinger (%.2f s between images)", gap);
	}

	obs_source_release(src);
}

/* The Ad Playlist after its move onto the shared code. */
static void scenario_ad_playlist(void)
{
	obs_data_t *s = gallery_settings();
	obs_source_t *src;
	size_t first;

	obs_data_set_int(s, "image_duration", 1000);
	obs_data_set_int(s, "transition_duration", 300);
	src = start_source("sponsor_ad_playlist", s, "ad_item_changed", "ad_playlist_ended");

	run(src, 4.5, NULL);

	/* The playlist signals its first item twice: once when it is loaded
	 * and again when it restarts on becoming visible. That is how it has
	 * always behaved; skip the repeat. */
	first = change(1) && change(1)->index == 0 && change(1)->t - change(0)->t < 0.2 ? 1 : 0;

	check(count_changes() >= first + 4, "at least 4 items in 4.5 s (%zu)", count_changes() - first);
	for (size_t i = 0; i < 4 && change(first + i); i++)
		check(change(first + i)->index == (long long)(i % 3), "change %zu plays item %zu (%lld)", i, i % 3,
		      change(first + i)->index);
	for (size_t i = 1; i < 4 && change(first + i); i++) {
		double gap = change(first + i)->t - change(first + i - 1)->t;

		/* overlapping: each image is on screen for exactly its hold
		 * time, transitions included, so a new one starts every
		 * hold - transition = 0.7 s */
		check(gap > 0.6 && gap < 0.9, "a new item every 1.0 s hold - 0.3 s overlap (%.2f s)", gap);
	}
	check(count_color(0.5, 4.5, C_RED) && count_color(0.5, 4.5, C_GREEN) && count_color(0.5, 4.5, C_BLUE),
	      "renders all three images");

	obs_source_release(src);
}

/* The missing files dialog relinks a file in both sources (this used to read
 * the source as the playlist's own data and crash). */
static void check_missing_files(const char *id)
{
	obs_data_t *s = gallery_settings();
	obs_data_array_t *files = obs_data_get_array(s, "files");
	obs_data_t *missing = obs_data_create();
	obs_missing_files_t *list;
	obs_source_t *src;
	struct dstr replacement = {0};

	obs_data_set_string(missing, "value", "/nonexistent/missing.png");
	obs_data_array_push_back(files, missing);
	obs_data_release(missing);
	obs_data_array_release(files);

	src = obs_source_create(id, "missing files", s, NULL);
	obs_data_release(s);

	list = obs_source_get_missing_files(src);
	check(obs_missing_files_count(list) == 1, "%s reports the one missing file", id);

	/* settings change in between, as they would while the dialog is open */
	update_string(src, "alignment", "top");

	if (obs_missing_files_count(list) == 1) {
		dstr_printf(&replacement, "%s/1_red.png", image_dir);
		obs_missing_file_issue_callback(obs_missing_files_get_file(list, 0), replacement.array);

		s = obs_source_get_settings(src);
		files = obs_data_get_array(s, "files");
		missing = obs_data_array_item(files, 1);
		check(strcmp(obs_data_get_string(missing, "value"), replacement.array) == 0,
		      "%s relinks the missing file", id);
		obs_data_release(missing);
		obs_data_array_release(files);
		obs_data_release(s);
	}

	dstr_free(&replacement);
	obs_missing_files_destroy(list);
	obs_source_release(src);
}

static void scenario_missing_files(void)
{
	check_missing_files("sponsor_picture_gallery");
	check_missing_files("sponsor_ad_playlist");
}

/* ------------------------------------------------------------------------- */

struct scenario {
	const char *name;
	void (*fn)(void);
};

static const struct scenario scenarios[] = {
	{"nogap", scenario_nogap},
	{"gap", scenario_gap},
	{"show_none", scenario_show_none},
	{"show_hide_none", scenario_show_hide_none},
	{"all_none", scenario_all_none},
	{"edges_only", scenario_edges_only},
	{"wrap", scenario_wrap},
	{"end", scenario_end},
	{"controls", scenario_controls},
	{"hidden_restart", scenario_hidden_restart},
	{"retype", scenario_retype},
	{"hammer", scenario_hammer},
	{"scene", scenario_scene},
	{"stinger", scenario_stinger},
	{"ad_playlist", scenario_ad_playlist},
	{"missing_files", scenario_missing_files},
};

#define NUM_SCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

int main(int argc, char **argv)
{
	const struct scenario *scenario = NULL;
	long leaks;

	if (argc < 5) {
		fprintf(stderr, "usage: %s <scenario> <image dir> <plugin> <plugin data> [stinger]\n", argv[0]);
		for (size_t i = 0; i < NUM_SCENARIOS; i++)
			fprintf(stderr, "  %s\n", scenarios[i].name);
		return 2;
	}

	for (size_t i = 0; i < NUM_SCENARIOS; i++) {
		if (strcmp(scenarios[i].name, argv[1]) == 0)
			scenario = &scenarios[i];
	}
	if (!scenario) {
		fprintf(stderr, "unknown scenario '%s'\n", argv[1]);
		return 2;
	}

	image_dir = argv[2];
	stinger_path = argc > 5 ? argv[5] : NULL;
	verbose = getenv("RUNTIME_TEST_VERBOSE") != NULL;

	if (!start_obs(argv[3], argv[4])) {
		fprintf(stderr, "could not start libobs\n");
		return 2;
	}

	printf("== %s\n", scenario->name);
	t0 = os_gettime_ns();
	scenario->fn();

	obs_set_output_source(0, NULL);
	/* A public scene stays registered until it is removed, as OBS removes
	 * one when it is deleted; releasing it alone leaks it from OBS 32 on. */
	if (scene)
		obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);

	obs_enter_graphics();
	gs_texrender_destroy(texrender);
	gs_stagesurface_destroy(stagesurf);
	obs_leave_graphics();

	da_free(samples);
	da_free(events);

	obs_shutdown();

	leaks = bnum_allocs();
	check(leaks == 0, "no memory leaks (%ld)", leaks);

	printf("== %s: %s\n", scenario->name, failures ? "FAILED" : "passed");
	return failures ? 1 : 0;
}
