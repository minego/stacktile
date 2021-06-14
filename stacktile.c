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

enum Sublayout
{
	COLUMNS,
	ROWS,
	STACK,
};

struct Output
{
	struct wl_list link;

	struct wl_output       *output;
	struct river_layout_v2 *layout;

	uint32_t main_count;
	double main_factor;
	uint32_t inner_padding;
	uint32_t outer_padding;
	enum Sublayout sublayout;

	bool configured;
};

struct wl_display  *wl_display;
struct wl_registry *wl_registry;
struct wl_callback *sync_callback;
struct river_layout_manager_v2 *layout_manager;
struct wl_list outputs;
bool loop = true;
int ret = EXIT_FAILURE;

static void sublayout_stack (struct river_layout_v2 *river_layout_v2, uint32_t serial,
		uint32_t x, uint32_t y, uint32_t _width, uint32_t _height, uint32_t amount)
{
	if ( amount == 0 )
		return;
	if ( amount == 1 )
	{
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)x, (int32_t)y, (int32_t)_width, (int32_t)_height);
		return;
	}

	const uint32_t width = 0.95 * _width;
	const uint32_t height = 0.95 * _height;
	const uint32_t x_offset = (0.05 *_width) / (amount - 1);
	const uint32_t y_offset = (0.05 *_height) / (amount - 1);

	for (uint32_t i = 0; i < amount; i++)
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)(x + (i * x_offset)),
				(int32_t)(y + (i * y_offset)),
				(int32_t)width, (int32_t)height);
}

static void sublayout_columns (struct river_layout_v2 *river_layout_v2, uint32_t serial,
		uint32_t x, uint32_t y, uint32_t _width, uint32_t _height, uint32_t amount,
		uint32_t inner_padding)
{
	if ( amount == 0 )
		return;
	if ( amount == 1 )
	{
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)x, (int32_t)y, (int32_t)_width, (int32_t)_height);
		return;
	}

	const uint32_t width = (_width - ((amount - 1) * inner_padding)) / amount;
	const uint32_t height = _height;

	for (uint32_t i = 0; i < amount; i++)
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)(x + (i * width + (i * inner_padding))),
				(int32_t)y,
				(int32_t)width, (int32_t)height);
}

static void sublayout_rows (struct river_layout_v2 *river_layout_v2, uint32_t serial,
		uint32_t x, uint32_t y, uint32_t _width, uint32_t _height, uint32_t amount,
		uint32_t inner_padding)
{
	if ( amount == 0 )
		return;
	if ( amount == 1 )
	{
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)x, (int32_t)y, (int32_t)_width, (int32_t)_height);
		return;
	}

	const uint32_t width = _width;
	const uint32_t height = (_height - ((amount - 1) * inner_padding)) / amount;

	for (uint32_t i = 0; i < amount; i++)
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)y,
				(int32_t)(y + (i * height + (i * inner_padding))),
				(int32_t)width, (int32_t)height);
}

static void layout_handle_layout_demand (void *data, struct river_layout_v2 *river_layout_v2,
		uint32_t view_count, uint32_t width, uint32_t height, uint32_t tags, uint32_t serial)
{
	struct Output *output = (struct Output *)data;
	width -= 2 * output->outer_padding, height -= 2 * output->outer_padding;
	uint32_t main_size, stack_size;

	const uint32_t main_count = MIN(output->main_count, view_count);
	const uint32_t remainder_count = view_count - main_count;

	if ( main_count == 0 ) /* No main, only stack. */
	{
		main_size  = 0;
		stack_size = width;
	}
	else if ( view_count <= main_count ) /* No stack, only main. */
	{
		main_size  = width;
		stack_size = 0;
	}
	else /* Both main and stack. */
	{
		main_size  = (width * output->main_factor) - (output->inner_padding / 2);
		stack_size = width - (main_size + output->inner_padding);
	}

	switch (output->sublayout)
	{
		case COLUMNS:
			sublayout_columns(river_layout_v2, serial,
				output->outer_padding, output->outer_padding,
				main_size, height, main_count,
				output->inner_padding);
			break;

		case ROWS:
			sublayout_rows(river_layout_v2, serial,
				output->outer_padding, output->outer_padding,
				main_size, height, main_count,
				output->inner_padding);
			break;

		case STACK:
			sublayout_stack(river_layout_v2, serial,
				output->outer_padding, output->outer_padding,
				main_size, height, main_count);
			break;
	}

	if ( remainder_count == 1 )
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)(output->outer_padding + main_size + output->inner_padding),
				(int32_t)(output->outer_padding), (int32_t)stack_size, (int32_t)height);
	else if ( remainder_count > 1 )
	{
		const uint32_t remainder_x = output->inner_padding + main_size + output->inner_padding;
		const uint32_t top_size = 0.6 * (height - output->inner_padding);
		const uint32_t bottom_size = 0.4 * (height - output->inner_padding);

		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)remainder_x, (int32_t)(output->outer_padding),
				(int32_t)stack_size, (int32_t)top_size);

		sublayout_stack(river_layout_v2, serial,
				remainder_x, output->outer_padding + top_size + output->inner_padding,
				stack_size, bottom_size, remainder_count - 1);
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

static void layout_handle_set_string_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, const char *str)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "sublayout") == 0 )
	{
		if ( strcmp(str, "columns") == 0 )
			output->sublayout = COLUMNS;
		else if ( strcmp(str, "rows") == 0 )
			output->sublayout = ROWS;
		else if ( strcmp(str, "stack") == 0 )
			output->sublayout = STACK;
	}
}

static void noop () {}

static const struct river_layout_v2_listener layout_listener = {
	.namespace_in_use = layout_handle_namespace_in_use,
	.layout_demand    = layout_handle_layout_demand,
	.set_int_value    = layout_handle_set_int_value,
	.mod_int_value    = layout_handle_mod_int_value,
	.set_fixed_value  = layout_handle_set_fixed_value,
	.mod_fixed_value  = layout_handle_mod_fixed_value,
	.set_string_value = layout_handle_set_string_value,
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
	output->sublayout     = COLUMNS;

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

