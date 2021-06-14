#include<assert.h>
#include<stdbool.h>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

#include<wayland-client.h>
#include<wayland-client-protocol.h>

#include"river-layout-v2.h"

/* A few macros to indulge the inner glibc user. */
#define MIN(a, b) ( a < b ? a : b )
#define MAX(a, b) ( a > b ? a : b )
#define CLAMP(a, b, c) ( MIN(MAX(b, c), MAX(MIN(b, c), a)) )

struct Output
{
	struct wl_list link;

	struct wl_output       *output;
	struct river_layout_v2 *layout;

	uint32_t main_count;
	double main_factor;
	uint32_t inner_padding;
	uint32_t outer_padding;

	bool configured;
};

struct wl_display  *wl_display;
struct wl_registry *wl_registry;
struct wl_callback *sync_callback;
struct river_layout_manager_v2 *layout_manager;
struct wl_list outputs;
bool loop = true;
int ret = EXIT_FAILURE;

static void layout_handle_layout_demand (void *data, struct river_layout_v2 *river_layout_v2,
		uint32_t view_count, uint32_t width, uint32_t height, uint32_t tags, uint32_t serial)
{
	struct Output *output = (struct Output *)data;

	width -= 2 * output->outer_padding, height -= 2 * output->outer_padding;
	unsigned int singular_main_size, main_size, stack_size, view_x, view_y, view_width, view_height;
	int left_over = view_count - output->main_count - 1;
	const float secondary_area_size = 0.6;
	const float stack_area_size     = 0.4;

	if ( output->main_count == 0 )
	{
		main_size  = 0;
		stack_size = width;
	}
	else
	{
		if ( view_count <= output->main_count )
		{
			main_size  = width;
			stack_size = 0;
		}
		else
		{
			main_size  = (width * output->main_factor) - (output->inner_padding / 2);
			stack_size = width - (main_size + output->inner_padding);
		}

		if ( output->main_count == 1 )
			singular_main_size = main_size;
		else
		{
			const int real_main_count = MIN(output->main_count, view_count);
			singular_main_size = (main_size - ((real_main_count - 1) * output->inner_padding)) / real_main_count;
		}
	}
	for (unsigned int i = 0; i < view_count; i++)
	{
		if ( i < output->main_count  ) /* Main area. */
		{
			view_width  = singular_main_size;
			view_height = height;
			view_x      = i * view_width + (i * output->inner_padding);
			view_y      = 0;
		}
		else if ( i == output->main_count ) /* Secondary area. */
		{
			view_x      = main_size + output->inner_padding;
			view_width  = stack_size;
			view_y      = 0;
			view_height = left_over == 0 ? height : (secondary_area_size * height) - (output->inner_padding / 2);
		}
		else /* Stack area. */
		{
			if ( left_over == 1 )
			{
				view_x      = main_size + output->inner_padding;
				view_width  = stack_size;
				view_height = (stack_area_size * height) - (output->inner_padding / 2);
				view_y      = (secondary_area_size * height) + (output->inner_padding / 2);
			}
			else
			{
				view_x      = main_size + output->inner_padding + (0.1 * stack_size / (left_over - 1)) * (i - output->main_count - 1);
				view_width  = stack_size * 0.9;
				view_height = ((stack_area_size * height) - (output->inner_padding / 2)) * 0.9;
				view_y      = secondary_area_size * height + (output->inner_padding / 2) + (0.1 * (stack_area_size * height) / (left_over - 1)) * (i - output->main_count - 1);
			}
		}

		river_layout_v2_push_view_dimensions(output->layout, serial,
				view_x + output->outer_padding, view_y + output->outer_padding,
				view_width, view_height);
	}

	river_layout_v2_commit(output->layout, serial);
}

static void layout_handle_namespace_in_use (void *data, struct river_layout_v2 *river_layout_v2)
{
	fputs("Namespace already in use.\n", stderr);
	loop = false;
}

static void layout_handle_set_int_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, int32_t value)
{
	struct Output *output = (struct Output *)data;

	/* All integer parameters of this layout only accept positive values. */
	if ( value < 0 )
		return;

	if ( strcmp(name, "main_count") == 0 )
		output->main_count = (uint32_t)value;
	else if ( strcmp(name, "inner_padding") == 0 )
		output->inner_padding = (uint32_t)value;
	else if ( strcmp(name, "outer_padding") == 0 )
		output->outer_padding = (uint32_t)value;
	else if ( strcmp(name, "all_padding") == 0 )
	{
		output->inner_padding = (uint32_t)value;
		output->outer_padding = (uint32_t)value;
	}
}

static void layout_handle_mod_int_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, int32_t delta)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "main_count") == 0 )
	{
		if ( (int32_t)output->main_count + delta >= 0 )
			output->main_count = output->main_count + delta;
	}
	else if ( strcmp(name, "inner_padding") == 0 )
	{
		if ( (int32_t)output->inner_padding + delta >= 0 )
			output->inner_padding = output->inner_padding + delta;
	}
	else if ( strcmp(name, "outer_padding") == 0 )
	{
		if ( (int32_t)output->outer_padding + delta >= 0 )
			output->outer_padding = output->outer_padding + delta;
	}
	else if ( strcmp(name, "all_padding") == 0 )
	{
		if ( (int32_t)output->inner_padding + delta >= 0 )
			output->inner_padding = output->inner_padding + delta;
		if ( (int32_t)output->outer_padding + delta >= 0 )
			output->outer_padding = output->outer_padding + delta;
	}
}

static void layout_handle_set_fixed_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, wl_fixed_t value)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "main_factor") == 0 )
		output->main_factor = CLAMP(wl_fixed_to_double(value), 0.1, 0.9);
}

static void layout_handle_mod_fixed_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, wl_fixed_t delta)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "main_factor") == 0 )
		output->main_factor = CLAMP(output->main_factor + wl_fixed_to_double(delta), 0.1, 0.9);
}

static void noop () {}

static const struct river_layout_v2_listener layout_listener = {
	.namespace_in_use = layout_handle_namespace_in_use,
	.layout_demand    = layout_handle_layout_demand,
	.set_int_value    = layout_handle_set_int_value,
	.mod_int_value    = layout_handle_mod_int_value,
	.set_fixed_value  = layout_handle_set_fixed_value,
	.mod_fixed_value  = layout_handle_mod_fixed_value,
	.set_string_value = noop,
	.advertise_view   = noop,
	.advertise_done   = noop,
};

static void configure_output (struct Output *output)
{
	output->configured = true;
	output->layout = river_layout_manager_v2_get_layout(layout_manager,
			output->output, "stacktile");
	river_layout_v2_add_listener(output->layout, &layout_listener, output);
}

static bool create_output (struct wl_output *wl_output)
{
	struct Output *output = calloc(1, sizeof(struct Output));
	if ( output == NULL )
	{
		fputs("Failed to allocate.\n", stderr);
		return false;
	}

	output->output     = wl_output;
	output->layout     = NULL;
	output->configured = false;

	output->main_count    = 1;
	output->main_factor   = 0.6;
	output->inner_padding = 10;
	output->outer_padding = 10;

	if ( layout_manager != NULL )
		configure_output(output);

	wl_list_insert(&outputs, &output->link);
	return true;
}

static void destroy_output (struct Output *output)
{
	if ( output->layout != NULL )
		river_layout_v2_destroy(output->layout);
	wl_output_destroy(output->output);
	wl_list_remove(&output->link);
	free(output);
}

static void destroy_all_outputs ()
{
	struct Output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &outputs, link)
		destroy_output(output);
}

static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if (! strcmp(interface, river_layout_manager_v2_interface.name))
		layout_manager = wl_registry_bind(registry, name,
				&river_layout_manager_v2_interface, 1);
	else if (! strcmp(interface, wl_output_interface.name))
	{
		struct wl_output *wl_output = wl_registry_bind(registry, name,
				&wl_output_interface, version);
		if (! create_output(wl_output))
		{
			loop = false;
			ret = EXIT_FAILURE;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_handle_global,
	.global_remove = noop
};

static void sync_handle_done (void *data, struct wl_callback *wl_callback,
		uint32_t irrelevant)
{
	wl_callback_destroy(wl_callback);
	sync_callback = NULL;

	if ( layout_manager == NULL )
	{
		fputs("Wayland compositor does not support river-layout-v2.\n", stderr);
		ret = EXIT_FAILURE;
		loop = false;
		return;
	}

	struct Output *output;
	wl_list_for_each(output, &outputs, link)
		if (! output->configured)
			configure_output(output);
}

static const struct wl_callback_listener sync_callback_listener = {
	.done = sync_handle_done,
};

static bool init_wayland (void)
{
	/* We query the display name here instead of letting wl_display_connect()
	 * figure it out itself, because libwayland (for legacy reasons) falls
	 * back to using "wayland-0" when $WAYLAND_DISPLAY is not set, which is
	 * generally not desirable.
	 */
	const char *display_name = getenv("WAYLAND_DISPLAY");
	if ( display_name == NULL )
	{
		fputs("WAYLAND_DISPLAY is not set.\n", stderr);
		return false;
	}

	wl_display = wl_display_connect(display_name);
	if ( wl_display == NULL )
	{
		fputs("Can not connect to Wayland server.\n", stderr);
		return false;
	}

	wl_list_init(&outputs);

	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	return true;
}

static void finish_wayland (void)
{
	if ( wl_display == NULL )
		return;

	destroy_all_outputs();

	if ( sync_callback != NULL )
		wl_callback_destroy(sync_callback);
	if ( layout_manager != NULL )
		river_layout_manager_v2_destroy(layout_manager);

	wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);
}

int main (int argc, char *argv[])
{
	if (init_wayland())
	{
		ret = EXIT_SUCCESS;
		while ( loop && wl_display_dispatch(wl_display) != -1 );
	}
	finish_wayland();
	return ret;
}

