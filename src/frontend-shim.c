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

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "frontend-shim.h"

typedef void (*get_transitions_t)(struct si_frontend_source_list *sources);
typedef void (*event_callback_t)(si_frontend_event_cb callback, void *private_data);

static get_transitions_t get_transitions = NULL;
static event_callback_t add_event_callback = NULL;
static event_callback_t remove_event_callback = NULL;

/*
 * Finds a function of the frontend API among what OBS has already loaded,
 * without loading anything: OBS links the frontend API into its executable,
 * so inside the OBS UI it is there, and anywhere else it is not.
 *
 * The address is copied into `function` - a pointer of the function's real
 * type - rather than cast to it: a data-to-function or function-to-function
 * pointer cast draws a warning on some compiler or other, a copy on none.
 */
static void find_function(const char *name, void *function, size_t size)
{
#ifdef _WIN32
	HMODULE module = GetModuleHandleW(L"obs-frontend-api.dll");
	FARPROC address = module ? GetProcAddress(module, name) : NULL;
#else
	void *address = dlsym(RTLD_DEFAULT, name);
#endif

	if (size == sizeof(address))
		memcpy(function, &address, size);
}

bool si_frontend_load(void)
{
	find_function("obs_frontend_get_transitions", &get_transitions, sizeof(get_transitions));
	find_function("obs_frontend_add_event_callback", &add_event_callback, sizeof(add_event_callback));
	find_function("obs_frontend_remove_event_callback", &remove_event_callback, sizeof(remove_event_callback));

	if (!si_frontend_available()) {
		get_transitions = NULL;
		add_event_callback = NULL;
		remove_event_callback = NULL;
		return false;
	}

	return true;
}

bool si_frontend_available(void)
{
	return get_transitions && add_event_callback && remove_event_callback;
}

void si_frontend_get_transitions(struct si_frontend_source_list *list)
{
	if (get_transitions)
		get_transitions(list);
}

void si_frontend_source_list_free(struct si_frontend_source_list *list)
{
	for (size_t i = 0; i < list->sources.num; i++)
		obs_source_release(list->sources.array[i]);

	da_free(list->sources);
}

bool si_frontend_add_event_callback(si_frontend_event_cb callback, void *private_data)
{
	if (!add_event_callback)
		return false;

	add_event_callback(callback, private_data);
	return true;
}

void si_frontend_remove_event_callback(si_frontend_event_cb callback, void *private_data)
{
	if (remove_event_callback)
		remove_event_callback(callback, private_data);
}
