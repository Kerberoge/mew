#include <ctype.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/timerfd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define MAX(A, B)             ((A) > (B) ? (A) : (B))
#define MIN(A, B)             ((A) < (B) ? (A) : (B))
#define BETWEEN(X, A, B)      ((A) <= (X) && (X) <= (B))
#define LENGTH(X)             (sizeof (X) / sizeof (X)[0])

#include "drwl.h"
#include "poolbuf.h"
#include "xdg-activation-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define TEXTW(X)              (drwl_font_getwidth(drw, (X)) + lrpad)

enum { SchemeNorm, SchemeSel }; /* color schemes */

struct item {
	char *text;
	struct item *left, *right;
};

static struct {
	struct wl_keyboard *wl_keyboard;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	int repeat_delay;
	int repeat_period;
	int repeat_timer;
	enum wl_keyboard_key_state repeat_key_state;
	xkb_keysym_t repeat_sym;
} kbd;

static char text[BUFSIZ] = "";
static int bh, mw, mh;
static int inputw = 0, promptw;
static int32_t scale = 1;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int running = 1;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_seat *seat;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct xdg_activation_v1 *activation;
static struct wl_surface *surface;
static struct wl_registry *registry;
static Drwl *drw;

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;
static int (*submit)(const char*) = puts;

static void
noop()
{
	/* Space intentionally left blank */
}

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(EXIT_FAILURE);
}

static void
activation_token_handle_done(void *data,
	struct xdg_activation_token_v1 *activation_token, const char *token)
{
	char *argv[] = {"/bin/sh", "-c", (char*)data, NULL};

	xdg_activation_token_v1_destroy(activation_token);

	switch (fork()) {
	case -1:
		die("fork:");
	case 0:
		setenv("XDG_ACTIVATION_TOKEN", token, 1);
		execv(argv[0], argv);
		die("execvp:");
	}

	running = 0;
}

static const struct xdg_activation_token_v1_listener activation_token_listener = {
	.done = activation_token_handle_done,
};

static int
exec_cmd(const char *text)
{
	struct xdg_activation_token_v1 *activation_token;

	activation_token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_set_surface(activation_token, surface);
	xdg_activation_token_v1_add_listener(activation_token,
		&activation_token_listener, (void *)text);
	xdg_activation_token_v1_commit(activation_token);

	return 0;
}

static void
parse_color(uint32_t *dest, const char *src)
{
	int len;

	if (src[0] == '#')
		src++;
	len = strlen(src);
	if (len != 6 && len != 8)
		die("bad color: %s", src);

	*dest = strtoul(src, NULL, 16);
	if (len == 6)
		*dest = (*dest << 8) | 0xFF;
}

static void
loadfonts(void)
{
	char fontattrs[12];

	drwl_destroy_font(drw->font);
	snprintf(fontattrs, sizeof(fontattrs), "dpi=%d", 96 * scale);
	if (!(drwl_load_font(drw, LENGTH(fonts), fonts, fontattrs)))
		die("no fonts could be loaded");

	lrpad = drw->font->height;
	bh = drw->font->height + 2;
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
}

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drwl_font_getwidth_clamp(drw, str, n) + 2 * item_padding;
	return MIN(w, n);
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : textw_clamp(next->text, n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : textw_clamp(prev->left->text, n)) > n)
			break;
}

static void
cleanup(void)
{
	size_t i;

	for (i = 0; items && items[i].text; ++i)
		free(items[i].text);
	free(items);

	drwl_destroy(drw);
	drwl_fini();

	zwlr_layer_shell_v1_destroy(layer_shell);
	xdg_activation_v1_destroy(activation);
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
}

static char *
cistrstr(const char *h, const char *n)
{
	size_t i;

	if (!n[0])
		return (char *)h;

	for (; *h; ++h) {
		for (i = 0; n[i] && tolower((unsigned char)n[i]) ==
		            tolower((unsigned char)h[i]); ++i)
			;
		if (n[i] == '\0')
			return (char *)h;
	}
	return NULL;
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	if (item == sel)
		drwl_setscheme(drw, colors[SchemeSel]);
	else
		drwl_setscheme(drw, colors[SchemeNorm]);

	return drwl_text(drw, x, y, w, bh, item_padding, item->text, 0);
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, w;
	PoolBuf *buf;

	if (!(buf = poolbuf_create(shm, mw, mh)))
		die("poolbuf_create:");

	drwl_prepare_drawing(drw, mw, mh, buf->data, buf->stride);

	drwl_setscheme(drw, colors[SchemeNorm]);
	drwl_rect(drw, 0, 0, mw, mh, 1, 1);

	if (prompt && *prompt) {
		drwl_setscheme(drw, colors[SchemeSel]);
		x = drwl_text(drw, x, 0, promptw, bh, lrpad / 2, prompt, 0);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;
	drwl_setscheme(drw, colors[SchemeNorm]);
	drwl_text(drw, x, 0, w, bh, lrpad / 2, text, 0);

	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	if ((curpos += lrpad / 2 - 1) < w) {
		drwl_setscheme(drw, colors[SchemeNorm]);
		drwl_rect(drw, x + curpos, 2, 2, bh - 4, 1, 0);
	}

	if (lines > 0) {
		/* draw vertical list */
		for (item = curr; item != next; item = item->right)
			drawitem(item, x, y += bh, mw - x);
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if (curr->left) {
			drwl_setscheme(drw, colors[SchemeNorm]);
			drwl_text(drw, x, 0, w, bh, lrpad / 2, "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0, textw_clamp(item->text, mw - x - TEXTW(">")));
		if (next) {
			w = TEXTW(">");
			drwl_setscheme(drw, colors[SchemeNorm]);
			drwl_text(drw, mw - w, 0, w, bh, lrpad / 2, ">", 0);
		}
	}

	drwl_finish_drawing(drw);
	wl_surface_set_buffer_scale(surface, scale);
	wl_surface_attach(surface, buf->wl_buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, mw, mh);
	poolbuf_destroy(buf);
	wl_surface_commit(surface);
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %zu bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
keyboard_keypress(enum wl_keyboard_key_state state, xkb_keysym_t sym)
{
	char buf[8];

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (sym) {
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XKB_KEY_BackSpace:
		if (cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XKB_KEY_Escape:
		cleanup();
		exit(EXIT_FAILURE);
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		if (sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XKB_KEY_Next:
	case XKB_KEY_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XKB_KEY_Prior:
	case XKB_KEY_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		submit(sel->text);
		if (submit != exec_cmd)
			running = 0;
		return;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		if (sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	default:
		if (xkb_keysym_to_utf8(sym, buf, 8))
			insert(buf, strnlen(buf, 8));
	}
	drawmenu();
}

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size)
{
	char *map_shm;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		die("unknown keymap");

	map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED)
		die("mmap:");
	
	kbd.xkb_keymap = xkb_keymap_new_from_string(kbd.xkb_context, map_shm,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	kbd.xkb_state = xkb_state_new(kbd.xkb_keymap);
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t _key_state)
{
	struct itimerspec spec = { 0 };
	enum wl_keyboard_key_state key_state = _key_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(kbd.xkb_state, key + 8);

	keyboard_keypress(key_state, sym);

	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED && kbd.repeat_period >= 0) {
		kbd.repeat_key_state = key_state;
		kbd.repeat_sym = sym;
		spec.it_value.tv_sec = kbd.repeat_delay / 1000;
		spec.it_value.tv_nsec = (kbd.repeat_delay % 1000) * 1000000l;
	} 
	timerfd_settime(kbd.repeat_timer, 0, &spec, NULL);
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed,
		uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
	xkb_state_update_mask(kbd.xkb_state,
		mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void
keyboard_repeat(void)
{
	struct itimerspec spec = { 0 };

	keyboard_keypress(kbd.repeat_key_state, kbd.repeat_sym);

	spec.it_value.tv_sec = kbd.repeat_period / 1000;
	spec.it_value.tv_nsec = (kbd.repeat_period % 1000) * 1000000l;
	timerfd_settime(kbd.repeat_timer, 0, &spec, NULL);
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay)
{
	kbd.repeat_delay = delay;
	kbd.repeat_period = rate >= 0 ? 1000 / rate : -1;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_handle_keymap,
	.enter = noop,
	.leave = noop,
	.key = keyboard_handle_key,
	.modifiers = keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat_info,
};

static void
layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	mw = width * scale;
	mh = height * scale;
	inputw = mw / 4; /* input width: ~25% of monitor width */
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	drawmenu();
}

static void
layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
};

static void 
surface_handle_preferred_scale(void *data,
	struct wl_surface *wl_surface, int32_t factor)
{
	scale = factor;
	loadfonts();
	zwlr_layer_surface_v1_set_size(layer_surface, 0, mh / scale);
}

static const struct wl_surface_listener surface_listener = {
	.enter = noop,
	.leave = noop,
	.preferred_buffer_scale = surface_handle_preferred_scale,
	.preferred_buffer_transform = noop,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat, enum wl_seat_capability caps)
{
	if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD))
		return;

	kbd.wl_keyboard = wl_seat_get_keyboard(seat);
	if (!(kbd.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
		die("xkb_context_new failed");
	if ((kbd.repeat_timer = timerfd_create(CLOCK_MONOTONIC, 0)) < 0)
		die("timerfd_create:");
	wl_keyboard_add_listener(kbd.wl_keyboard, &keyboard_listener, NULL);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = noop,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 6);
 	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(interface, xdg_activation_v1_interface.name))
		activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
	else if (!strcmp(interface, wl_seat_interface.name)) {
		seat = wl_registry_bind (registry, name, &wl_seat_interface, 4);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = noop,
};

static void
readstdin(void)
{
	char *line = NULL;
	size_t i, itemsiz = 0, linesiz = 0;
	ssize_t len;

	/* read each line from stdin and add it to the item list */
	for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1; i++) {
		if (i + 1 >= itemsiz) {
			itemsiz += 256;
			if (!(items = realloc(items, itemsiz * sizeof(*items))))
				die("cannot realloc %zu bytes:", itemsiz * sizeof(*items));
		}
		if (line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (!(items[i].text = strdup(line)))
			die("strdup:");
	}
	free(line);
	if (items)
		items[i].text = NULL;
	lines = MIN(lines, i);
}

static void
run(void)
{
	struct pollfd pfds[] = { 
		{ wl_display_get_fd(display), POLLIN },
		{ kbd.repeat_timer, POLLIN },
	}; 

	match();
	drawmenu();

	while (running) { 
		if (wl_display_prepare_read(display) < 0)
			if (wl_display_dispatch_pending(display) < 0)
				die("wl_display_dispatch_pending:");

		if (wl_display_flush(display) < 0)
			die("wl_display_flush:");

		if (poll(pfds, LENGTH(pfds), -1) < 0) {
			wl_display_cancel_read(display);
			die("poll:");
		}

		if (pfds[1].revents & POLLIN)
			keyboard_repeat();

		if (!(pfds[0].revents & POLLIN)) {
			wl_display_cancel_read(display);
			continue;
		}

		if (wl_display_read_events(display) < 0)
			die("wl_display_read_events:");
		if (wl_display_dispatch_pending(display) < 0)
			die("wl_display_dispatch_pending:");
	}
}

static void
setup(void)
{
	if (!(display = wl_display_connect(NULL)))
		die("failed to connect to wayland");

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	
	if (!compositor)
		die("wl_compositor not available");
	if (!shm)
		die("wl_shm not available");
	if (!layer_shell)
		die("layer_shell not available");

	drwl_init();
	if (!(drw = drwl_create()))
		die("cannot create drwl drawing context");
	loadfonts();

	match();

	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
				surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mew");
	zwlr_layer_surface_v1_set_size(layer_surface, 0, mh);
	zwlr_layer_surface_v1_set_anchor(layer_surface, 
		(top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM ) |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, true);
	zwlr_layer_surface_v1_add_listener(layer_surface,
			&layer_surface_listener, NULL);

	wl_surface_commit(surface);
	wl_display_roundtrip(display);
}

static void
usage(void)
{
	die("usage: mew [-beiv] [-l lines] [-p prompt] [-f font]\n"
	    "           [-nb color] [-nf color] [-sb color] [-sf color]");
}

int
main(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++) {
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {
			puts("mew-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b"))
			top = 0;
		else if (!strcmp(argv[i], "-e"))
			submit = exec_cmd;
		else if (!strcmp(argv[i], "-i")) { 
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		} else if (i + 1 == argc)
			usage();
		else if (!strcmp(argv[i], "-l"))
			lines = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-f"))
			fonts[0] = argv[++i];
		else if (!strcmp(argv[i], "-nb"))
			parse_color(&colors[SchemeNorm][ColBg], argv[++i]);
		else if (!strcmp(argv[i], "-nf"))
			parse_color(&colors[SchemeNorm][ColFg], argv[++i]);
		else if (!strcmp(argv[i], "-sb"))
			parse_color(&colors[SchemeSel][ColBg], argv[++i]);
		else if (!strcmp(argv[i], "-sf"))
			parse_color(&colors[SchemeSel][ColFg], argv[++i]);
		else
			usage();
	}

	readstdin();
#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif
	setup();
	run();
	cleanup();

	return EXIT_SUCCESS;
}
