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
 * The few OBS frontend API calls this plugin uses, looked up at runtime in the
 * OBS process instead of linked against.
 *
 * Linking obs-frontend-api would make the whole plugin - both sources - fail
 * to load anywhere that library is absent (headless or custom builds of libobs)
 * just so one option of one source can list the scene transitions. Looked up
 * at runtime, the plugin loads everywhere and the option is simply not offered
 * when there is no frontend.
 *
 * The declarations mirror obs-frontend-api.h, whose ABI is stable: the event
 * values below are fixed and new ones are only ever appended.
 */

#include <obs-module.h>
#include <util/darray.h>

#ifdef __cplusplus
extern "C" {
#endif

enum si_frontend_event {
	SI_FRONTEND_EVENT_TRANSITION_LIST_CHANGED = 12,
	SI_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED = 13,
	SI_FRONTEND_EVENT_EXIT = 17,
	SI_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP = 25,
	SI_FRONTEND_EVENT_FINISHED_LOADING = 26,
};

/* struct obs_frontend_source_list */
struct si_frontend_source_list {
	DARRAY(obs_source_t *) sources;
};

/* the frontend passes its own enum; it is an int underneath */
typedef void (*si_frontend_event_cb)(int event, void *private_data);

/* Looks the frontend up. Call from obs_module_post_load, by which point the
 * OBS UI has loaded it and set it up. Returns whether it is there. */
bool si_frontend_load(void);

bool si_frontend_available(void);

/* UI thread only. Fills `list` with references to the scene transitions;
 * release with si_frontend_source_list_free(). Does nothing without a
 * frontend. */
void si_frontend_get_transitions(struct si_frontend_source_list *list);

void si_frontend_source_list_free(struct si_frontend_source_list *list);

bool si_frontend_add_event_callback(si_frontend_event_cb callback, void *private_data);
void si_frontend_remove_event_callback(si_frontend_event_cb callback, void *private_data);

#ifdef __cplusplus
}
#endif
