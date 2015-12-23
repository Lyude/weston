/*
 * Copyright © 2015 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <unistd.h>

#include "compositor.h"
#include "shared/helpers.h"
#include "protocol/primary-selection-unstable-v1-server-protocol.h"

static void
destroy_primary_selection_data_source(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = container_of(listener, struct weston_seat,
						primary_data_source_listener);

	seat->primary_selection_data_source = NULL;
	wl_signal_emit(&seat->primary_selection_signal, seat);
}

WL_EXPORT void
weston_seat_set_primary_selection(struct weston_seat *seat,
				  struct weston_data_source *source)
{
	struct weston_data_source *current_source =
		seat->primary_selection_data_source;

	if (current_source) {
		seat->primary_selection_data_source->cancel(current_source);

		wl_list_remove(&seat->primary_data_source_listener.link);
	}

	seat->primary_selection_data_source = source;

	if (source) {
		seat->primary_data_source_listener.notify =
			destroy_primary_selection_data_source;
		wl_signal_add(&source->destroy_signal,
			      &seat->primary_data_source_listener);
	}
}

static void
weston_primary_selection_device_set_selection(struct wl_client *client,
					      struct wl_resource *resource,
					      struct wl_resource *source_resource)
{
	struct weston_seat *seat = wl_resource_get_user_data(resource);

	if (seat->pointer_state->focus_client->client != client)
		return;

	weston_seat_set_primary_selection(wl_resource_get_user_data(resource),
					  wl_resource_get_user_data(source_resource));
}

static void
weston_primary_selection_device_destroy(struct wl_client *client,
					struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
	wl_resource_destroy(resource);
}

struct zwp_primary_selection_device_v1_interface primary_selection_device_interface = {
	weston_primary_selection_device_set_selection,
	weston_primary_selection_device_destroy,
};

static void
unbind_primary_selection_device(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
client_source_send(struct weston_data_source *source,
		   const char *mime_type, int32_t fd)
{
	zwp_primary_selection_source_v1_send_send(source->resource, mime_type,
						  fd);
	close(fd);
}

static void
client_source_cancel(struct weston_data_source *source)
{
	zwp_primary_selection_source_v1_send_cancelled(source->resource);
}

static void
primary_selection_source_destroy(struct wl_client *client,
				 struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static struct zwp_primary_selection_source_v1_interface primary_selection_source_interface = {
	weston_data_source_offer,
	primary_selection_source_destroy
};

static void
create_primary_selection_source(struct wl_client *client,
				struct wl_resource *manager_resource, uint32_t id)
{
	struct weston_data_source *source;

	source = malloc(sizeof(*source));
	if (source == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	source->resource = wl_resource_create(
	    client, &zwp_primary_selection_source_v1_interface,
	    wl_resource_get_version(manager_resource), id);
	if (source->resource == NULL) {
		free(source);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	wl_signal_init(&source->destroy_signal);
	wl_array_init(&source->mime_types);

	source->accept = NULL;
	source->send = client_source_send;
	source->cancel = client_source_cancel;

	wl_resource_set_implementation(source->resource,
				       &primary_selection_source_interface,
				       source, weston_data_source_destroy);
}

static void
get_primary_selection_device(struct wl_client *client,
			     struct wl_resource *manager_resource, uint32_t id,
			     struct wl_resource *seat_resource)
{
	struct weston_seat *seat = wl_resource_get_user_data(seat_resource);
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &zwp_primary_selection_device_v1_interface,
				      wl_resource_get_version(manager_resource),
				      id);
	if (resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	wl_list_insert(&seat->primary_selection_device_resource_list,
		       wl_resource_get_link(resource));
	wl_resource_set_implementation(resource,
				       &primary_selection_device_interface,
				       seat, unbind_primary_selection_device);
}

static void
destroy_primary_selection_device(struct wl_client *client,
				 struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
	wl_resource_destroy(resource); /* XXX: We might not need this */
}

struct zwp_primary_selection_device_manager_v1_interface primary_selection_device_manager_interface = {
	create_primary_selection_source,
	get_primary_selection_device,
	destroy_primary_selection_device
};

static void
bind_primary_selection_manager(struct wl_client *client, void *data,
			       uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &zwp_primary_selection_device_manager_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource,
				       &primary_selection_device_manager_interface,
				       NULL, NULL);
}

WL_EXPORT int
wl_primary_selection_device_manager_init(struct wl_display *display)
{
	if (wl_global_create(display,
			     &zwp_primary_selection_device_manager_v1_interface, 1,
			     NULL, bind_primary_selection_manager) == NULL)
		return -1;

	return 0;
}

static const struct zwp_primary_selection_offer_v1_interface primary_selection_offer_interface = {
	data_offer_receive,
	data_offer_destroy
};

static void
weston_primary_selection_source_send_offer(struct weston_data_source *source,
					   struct wl_resource *target)
{
	struct weston_data_offer *offer =
		weston_data_offer_create(source, target,
					 &zwp_primary_selection_offer_v1_interface);
	char **p;

	if (!offer)
		return;

	zwp_primary_selection_device_v1_send_selection_offer(target,
							     offer->resource);

	wl_array_for_each(p, &source->mime_types)
		zwp_primary_selection_offer_v1_send_offer(offer->resource, *p);

	return;
}

WL_EXPORT void
middle_click_paste(struct weston_pointer *pointer, uint32_t time,
		   uint32_t value, void *data)
{
	struct weston_seat *seat = pointer->seat;
	struct weston_data_source *source = seat->primary_selection_data_source;
	struct wl_resource *resource;
	struct wl_client *client;

	if (!pointer->focus || !pointer->focus->surface || !source)
		return;

	client = wl_resource_get_client(pointer->focus->surface->resource);
	resource = wl_resource_find_for_client(
	    &seat->primary_selection_device_resource_list, client);

	if (!resource)
		return;

	weston_primary_selection_source_send_offer(source, resource);
}
