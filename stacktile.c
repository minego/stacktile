#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include<wayland-client.h>
#include<wayland-client-protocol.h>

#include"river-layout-v2.h"

/* A few macros to indulge the inner glibc user. */
#define MIN(a, b) ( a < b ? a : b )
#define MAX(a, b) ( a > b ? a : b )
#define CLAMP(a, b, c) ( MIN(MAX(b, c), MAX(MIN(b, c), a)) )

const char usage[] =
	"Usage: stacktile [options...]\n"
	"   --inner-padding  <int>\n"
	"   --outer-padding  <int>\n"
	"   --main-count     <int>\n"
	"   --main-factor    <float>\n"
	"   --sublayout      rows|columns|stack\n"
	"\n";

enum Sublayout
{
	COLUMNS,
	ROWS,
	STACK,
};

enum Layout_value_status
{
	UNCHANGED = 0,
	NEW,
	MOD,
};

struct Layout_config
{
	struct wl_list link;
	uint32_t tags;

	uint32_t main_count;
	double main_factor;
	uint32_t inner_padding;
	uint32_t outer_padding;
	enum Sublayout sublayout;
};

struct Output
{
	struct wl_list link;

	struct wl_output       *output;
	struct river_layout_v2 *layout;

	struct wl_list layout_configs;

	struct
	{
		enum Layout_value_status main_count_status;
		int32_t main_count;

		enum Layout_value_status main_factor_status;
		double main_factor;

		enum Layout_value_status inner_padding_status;
		int32_t inner_padding;

		enum Layout_value_status outer_padding_status;
		int32_t outer_padding;

		enum Layout_value_status sublayout_status;
		enum Sublayout sublayout;
	} pending_layout_config;

	bool configured;
};

struct wl_display  *wl_display;
struct wl_registry *wl_registry;
struct wl_callback *sync_callback;
struct river_layout_manager_v2 *layout_manager;
struct wl_list outputs;
bool loop = true;
int ret = EXIT_FAILURE;

struct Layout_config default_layout_config = {
	.main_count = 1,
	.main_factor = 0.6,
	.inner_padding = 10,
	.outer_padding = 10,
	.sublayout = ROWS
};

static void sublayout_stack (struct river_layout_v2 *river_layout_v2, uint32_t serial,
		uint32_t x, uint32_t y, uint32_t _width, uint32_t _height, uint32_t amount)
{
	if ( amount == 0 )
		return;
	if ( amount == 1 )
	{
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)x, (int32_t)y, _width, _height);
		return;
	}

	const uint32_t width = (uint32_t)(0.95 * (double)_width);
	const uint32_t height = (uint32_t)(0.95 * (double)_height);
	const uint32_t x_offset = (uint32_t)(0.05 * (double)_width) / (amount - 1);
	const uint32_t y_offset = (uint32_t)(0.05 * (double)_height) / (amount - 1);

	for (uint32_t i = 0; i < amount; i++)
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)(x + (i * x_offset)),
				(int32_t)(y + (i * y_offset)),
				width, height);
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
				(int32_t)x, (int32_t)y, _width, _height);
		return;
	}

	const uint32_t width = (_width - ((amount - 1) * inner_padding)) / amount;
	const uint32_t height = _height;

	for (uint32_t i = 0; i < amount; i++)
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)(x + (i * width + (i * inner_padding))),
				(int32_t)y,
				width, height);
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
				(int32_t)x, (int32_t)y, _width, _height);
		return;
	}

	const uint32_t width = _width;
	const uint32_t height = (_height - ((amount - 1) * inner_padding)) / amount;

	for (uint32_t i = 0; i < amount; i++)
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)y,
				(int32_t)(y + (i * height + (i * inner_padding))),
				width, height);
}

/**
 * Returns a layout config pointer for the given tag set, taking into account
 * the pending layout configuration.
 * 
 * The returned config should not be modified.
 */
static struct Layout_config *get_layout_config (struct Output *output, uint32_t tags)
{
	struct Layout_config *config = NULL, *tmp;
	wl_list_for_each(tmp, &output->layout_configs, link)
		if ( tmp->tags == tags )
		{
			config = tmp;
			break;
		}

	if ( config == NULL )
	{
		/* No config has been found. If there are pending changes, we
		 * need to create a new one based on the default config.
		 */
		if ( output->pending_layout_config.main_count_status != UNCHANGED
				|| output->pending_layout_config.main_factor_status != UNCHANGED
				|| output->pending_layout_config.inner_padding_status != UNCHANGED
				|| output->pending_layout_config.outer_padding_status != UNCHANGED
				|| output->pending_layout_config.sublayout_status != UNCHANGED )
		{
			config = calloc(1, sizeof(struct Layout_config));
			if ( config == NULL )
			{
				fprintf(stderr, "ERROR: calloc: %s\n", strerror(errno));
				return &default_layout_config;
			}
			memcpy(config, &default_layout_config, sizeof(struct Layout_config));
			config->tags = tags;
			wl_list_insert(&output->layout_configs, &config->link);
		}
		else
		{
			/* No pending changes, so we can just use the default config. */
			return &default_layout_config;
		}
	}

	if ( output->pending_layout_config.sublayout_status != UNCHANGED )
	{
		config->sublayout = output->pending_layout_config.sublayout;
		output->pending_layout_config.sublayout_status = UNCHANGED;
	}

	if ( output->pending_layout_config.main_count_status == NEW )
	{
		config->main_count = (uint32_t)output->pending_layout_config.main_count;
		output->pending_layout_config.main_count_status = UNCHANGED;
	}
	else if ( output->pending_layout_config.main_count_status == MOD )
	{
		if ( (int32_t)config->main_count + output->pending_layout_config.main_count >= 0 )
			config->main_count += (uint32_t)output->pending_layout_config.main_count;
		output->pending_layout_config.main_count_status = UNCHANGED;
	}

	if ( output->pending_layout_config.inner_padding_status == NEW )
	{
		config->inner_padding = (uint32_t)output->pending_layout_config.inner_padding;
		output->pending_layout_config.inner_padding_status = UNCHANGED;
	}
	else if ( output->pending_layout_config.inner_padding_status == MOD )
	{
		if ( (int32_t)config->inner_padding + output->pending_layout_config.inner_padding >= 0 )
			config->inner_padding += (uint32_t)output->pending_layout_config.inner_padding;
		output->pending_layout_config.inner_padding_status = UNCHANGED;
	}

	if ( output->pending_layout_config.outer_padding_status == NEW )
	{
		config->outer_padding = (uint32_t)output->pending_layout_config.outer_padding;
		output->pending_layout_config.outer_padding_status = UNCHANGED;
	}
	else if ( output->pending_layout_config.outer_padding_status == MOD )
	{
		if ( (int32_t)config->outer_padding + output->pending_layout_config.outer_padding >= 0 )
			config->outer_padding += (uint32_t)output->pending_layout_config.outer_padding;
		output->pending_layout_config.outer_padding_status = UNCHANGED;
	}

	if ( output->pending_layout_config.main_factor_status == NEW )
	{
		config->main_factor = output->pending_layout_config.main_factor;
		output->pending_layout_config.main_factor_status = UNCHANGED;
	}
	else if ( output->pending_layout_config.main_factor_status == MOD )
	{
		config->main_factor = CLAMP(config->main_factor + output->pending_layout_config.main_factor, 0.1, 0.9);
		output->pending_layout_config.main_factor_status = UNCHANGED;
	}

	return config;
}

static void layout_handle_layout_demand (void *data, struct river_layout_v2 *river_layout_v2,
		uint32_t view_count, uint32_t width, uint32_t height, uint32_t tags, uint32_t serial)
{
	struct Output *output = (struct Output *)data;
	struct Layout_config *config = get_layout_config(output, tags);

	width -= 2 * config->outer_padding, height -= 2 * config->outer_padding;
	uint32_t main_size, stack_size;

	const uint32_t main_count = MIN(config->main_count, view_count);
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
		main_size  = (uint32_t)((double)width * config->main_factor) - (config->inner_padding / 2);
		stack_size = width - (main_size + config->inner_padding);
	}

	switch (config->sublayout)
	{
		case COLUMNS:
			sublayout_columns(river_layout_v2, serial,
				config->outer_padding, config->outer_padding,
				main_size, height, main_count,
				config->inner_padding);
			break;

		case ROWS:
			sublayout_rows(river_layout_v2, serial,
				config->outer_padding, config->outer_padding,
				main_size, height, main_count,
				config->inner_padding);
			break;

		case STACK:
			sublayout_stack(river_layout_v2, serial,
				config->outer_padding, config->outer_padding,
				main_size, height, main_count);
			break;
	}

	if ( remainder_count == 1 )
		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)(config->outer_padding + (main_size == 0 ? 0 : main_size + config->inner_padding)),
				(int32_t)(config->outer_padding), stack_size, height);
	else if ( remainder_count > 1 )
	{
		const uint32_t remainder_x = config->inner_padding + (main_size == 0 ? 0 : main_size + config->inner_padding);
		const uint32_t top_size = (uint32_t)(0.6 * (double)(height - config->inner_padding));
		const uint32_t bottom_size = (uint32_t)(0.4 * (double)(height - config->inner_padding));

		river_layout_v2_push_view_dimensions(river_layout_v2, serial,
				(int32_t)remainder_x, (int32_t)(config->outer_padding),
				stack_size, top_size);

		sublayout_stack(river_layout_v2, serial,
				remainder_x, config->outer_padding + top_size + config->inner_padding,
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
	{
		output->pending_layout_config.main_count = value;
		output->pending_layout_config.main_count_status = NEW;
	}
	else if ( strcmp(name, "inner_padding") == 0 )
	{
		output->pending_layout_config.inner_padding = value;
		output->pending_layout_config.inner_padding_status = NEW;
	}
	else if ( strcmp(name, "outer_padding") == 0 )
	{
		output->pending_layout_config.outer_padding = value;
		output->pending_layout_config.outer_padding_status = NEW;
	}
	else if ( strcmp(name, "all_padding") == 0 )
	{
		output->pending_layout_config.inner_padding = value;
		output->pending_layout_config.inner_padding_status = NEW;

		output->pending_layout_config.outer_padding = value;
		output->pending_layout_config.outer_padding_status = NEW;
	}
}

static void layout_handle_mod_int_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, int32_t delta)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "main_count") == 0 )
	{
		output->pending_layout_config.main_count = delta;
		output->pending_layout_config.main_count_status = MOD;
	}
	else if ( strcmp(name, "inner_padding") == 0 )
	{
		output->pending_layout_config.inner_padding = delta;
		output->pending_layout_config.inner_padding_status = MOD;
	}
	else if ( strcmp(name, "outer_padding") == 0 )
	{
		output->pending_layout_config.outer_padding = delta;
		output->pending_layout_config.outer_padding_status = MOD;
	}
	else if ( strcmp(name, "all_padding") == 0 )
	{
		output->pending_layout_config.inner_padding = delta;
		output->pending_layout_config.inner_padding_status = MOD;

		output->pending_layout_config.outer_padding = delta;
		output->pending_layout_config.outer_padding_status = MOD;
	}
}

static void layout_handle_set_fixed_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, wl_fixed_t value)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "main_factor") == 0 )
	{
		output->pending_layout_config.main_factor = CLAMP(wl_fixed_to_double(value), 0.1, 0.9);
		output->pending_layout_config.main_factor_status = NEW;
	}
}

static void layout_handle_mod_fixed_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, wl_fixed_t delta)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "main_factor") == 0 )
	{
		output->pending_layout_config.main_factor = wl_fixed_to_double(delta);
		output->pending_layout_config.main_factor_status = MOD;
	}
}

static bool sublayout_from_string (const char *str, enum Sublayout *sublayout)
{
	if ( strcmp(str, "columns") == 0 )
		*sublayout = COLUMNS;
	else if ( strcmp(str, "rows") == 0 )
		*sublayout = ROWS;
	else if ( strcmp(str, "stack") == 0 )
		*sublayout = STACK;
	else
		return false;
	return true;
}

static void layout_handle_set_string_value (void *data, struct river_layout_v2 *river_layout_v2,
		const char *name, const char *str)
{
	struct Output *output = (struct Output *)data;
	if ( strcmp(name, "sublayout") == 0 )
	{
		if (! sublayout_from_string(str, &output->pending_layout_config.sublayout))
		{
			fprintf(stderr, "ERROR: Unknown sublayout: %s\n", str);
			return;
		}
		output->pending_layout_config.sublayout_status = NEW;
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

	wl_list_init(&output->layout_configs);

	if ( layout_manager != NULL )
		configure_output(output);

	wl_list_insert(&outputs, &output->link);
	return true;
}

static void destroy_output (struct Output *output)
{
	struct Layout_config *config, *tmp;
	wl_list_for_each_safe(config, tmp, &output->layout_configs, link)
	{
		wl_list_remove(&config->link);
		free(config);
	}

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
	enum
	{
		INNER_PADDING,
		OUTER_PADDING,
		MAIN_FACTOR,
		MAIN_COUNT,
		SUBLAYOUT,
	};

	const struct option opts[] = {
		{ "help",          no_argument,       NULL, 'h'           },
		{ "inner-padding", no_argument,       NULL, INNER_PADDING },
		{ "outer-padding", required_argument, NULL, OUTER_PADDING },
		{ "main-factor",   required_argument, NULL, MAIN_FACTOR   },
		{ "main-count",    required_argument, NULL, MAIN_COUNT    },
		{ "sublayout",     required_argument, NULL, SUBLAYOUT     },
	};

	int opt;
	int32_t tmp;
	while ( (opt = getopt_long(argc, argv, "h", opts, NULL)) != -1 ) switch (opt)
	{
		case 'h':
			fputs(usage, stderr);
			return EXIT_SUCCESS;

		case INNER_PADDING:
			tmp = atoi(optarg);
			if ( tmp < 0 )
			{
				fputs("ERROR: Inner padding may not be negative.\n", stderr);
				return EXIT_FAILURE;
			}
			default_layout_config.inner_padding = (uint32_t)tmp;
			break;

		case OUTER_PADDING:
			tmp = atoi(optarg);
			if ( tmp < 0 )
			{
				fputs("ERROR: Outer padding may not be negative.\n", stderr);
				return EXIT_FAILURE;
			}
			default_layout_config.outer_padding = (uint32_t)tmp;
			break;

		case MAIN_COUNT:
			tmp = atoi(optarg);
			if ( tmp < 0 )
			{
				fputs("ERROR: Main count may not be negative.\n", stderr);
				return EXIT_FAILURE;
			}
			default_layout_config.main_count = (uint32_t)tmp;
			break;

		case MAIN_FACTOR:
			default_layout_config.main_factor = CLAMP(atof(optarg), 0.1, 0.9);
			break;

		case SUBLAYOUT:
			if (!sublayout_from_string(optarg, &default_layout_config.sublayout))
			{
				fputs("ERROR: Invalid sublayout.\n", stderr);
				return EXIT_FAILURE;
			}
			break;

		default:
			return EXIT_FAILURE;

	}


	if (init_wayland())
	{
		ret = EXIT_SUCCESS;
		while ( loop && wl_display_dispatch(wl_display) != -1 );
	}
	finish_wayland();
	return ret;
}

