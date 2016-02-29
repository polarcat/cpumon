/* cpumon.c: primitive cpu load monitor
 *
 * By Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Copyright (C) 2015
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <xcb/xcb.h>

#ifndef False
#define False 0
#endif

#define msg(fmt, args...) printf("(==) "fmt, ##args);
#define err(fmt, args...) { \
	int __errnotmp__ = errno; \
	fprintf(stderr, "(EE) %s:%d: "fmt", %s (%d)\n", \
		__func__, __LINE__, ##args, strerror(errno), errno); \
	errno = __errnotmp__; \
}
#define eopt(opt) printf("(EE) malformed "opt" parameter\n");

#ifndef LINESIZE
#define LINESIZE 128
#endif

#ifndef STATFILE
#define STATFILE "/proc/stat"
#endif

#ifndef LOADFACTOR
#define LOADFACTOR 1024
#endif

struct cpustat {
	unsigned long load;
	unsigned long long prev_used;
	unsigned long long prev_total;
};

struct ctx {
	xcb_connection_t *dpy;
	xcb_drawable_t win;
	xcb_gcontext_t gc;
	xcb_point_t *dat;
	xcb_point_t *cur;
	unsigned int w, h, us, fg, bg;
	const char *class;
	FILE *file;
	char *line;
	struct cpustat stat;
	int cpu;
	uint8_t bw;
};

static void usage(struct ctx *ctx, const char *name)
{
	printf("Usage: %s <options>\n"
		"Options:\n"
		"  -h, --help                  print this message\n"
		"  -s, --size          <WxH>   set window size\n"
		"  -c, --cpu           <n>     CPU to monitor\n"
		"  -n, --name          <name>  set class name\n"
		"  -i, --interval      <n>     refresh interval (ms)\n"
		"  -fg, --fgcolor      <n>     foreground color\n"
		"  -bg, --bgcolor      <n>     background color\n"
		"  -bw, --border-width <n>     border width (px)\n"
		"Defaults:\n"
		"  aggregated load from all CPUs\n"
		"  class %s, size %dx%d, interval %d ms, fg %d, bg %d\n",
		name, ctx->class, ctx->w, ctx->h, ctx->us / 1000, ctx->fg,
		ctx->bg);
}

static int opt(const char *s, const char *l, const char *o)
{
	return !strcmp(s, o) || !strcmp(l, o);
}

static int param(const char *opt, const char *param)
{
	if (!param) {
		printf("missing %s parameter\n", opt);
		return 0;
	}

	return 1;
}

static int opts(struct ctx *ctx, int argc, char *argv[])
{
	int i = 0;
	char *tmp;

	while (++i < argc && argv[i]) {
		if (opt("-h", "--help", argv[i])) {
			usage(ctx, argv[0]);
			return -1;
		} else if (opt("-s", "--size", argv[i])) {
			if (!param("size", argv[i + 1]))
				return -1;

			tmp = strchr(argv[i + 1], 'x');
			if (!tmp) {
				eopt("size");
				return -1;
			}
			ctx->w = atoi(argv[i + 1]);
			ctx->h = atoi(tmp + 1);
			if (ctx->w == 0 || ctx->h == 0) {
				eopt("size");
				return -1;
			}
		} else if (opt("-i", "--interval", argv[i])) {
			if (!param("interval", argv[i + 1]))
				return -1;
			ctx->us = atoi(argv[i + 1]) * 1000;
			if (ctx->us == 0) {
				eopt("interval");
				return -1;
			}
		} else if (opt("-c", "--cpu", argv[i])) {
			if (!param("cpu", argv[i + 1]))
				return -1;
			ctx->cpu = atoi(argv[i + 1]);
		} else if (opt("-n", "--name", argv[i])) {
			if (!param("name", argv[i + 1]))
				return -1;
			ctx->class = argv[i + 1];
		} else if (opt("-fg", "--fgcolor", argv[i])) {
			if (!param("fgcolor", argv[i + 1]))
				return -1;
			ctx->fg = atoi(argv[i + 1]);
		} else if (opt("-bg", "--bgcolor", argv[i])) {
			if (!param("bgcolor", argv[i + 1]))
				return -1;
			ctx->bg = atoi(argv[i + 1]);
		} else if (opt("-bw", "--border-width", argv[i])) {
			if (!param("border-width", argv[i + 1]))
				return -1;
			ctx->bw = atoi(argv[i + 1]);
		}
	}

	return 0;
}

static void wait(struct ctx *ctx)
{
	int state;
	xcb_generic_event_t *e;

	while (1) {
		e = xcb_wait_for_event(ctx->dpy);
		if (!e)
			continue;

		state = ((xcb_visibility_notify_event_t *) e)->state;
		free(e);
		if (state == XCB_VISIBILITY_UNOBSCURED)
			break;
		else if (state == XCB_VISIBILITY_PARTIALLY_OBSCURED)
			break;
	}
}

static void events(struct ctx *ctx)
{
	xcb_generic_event_t *e = xcb_poll_for_event(ctx->dpy);

	if (!e)
		return;

	switch (e->response_type & ~0x80) {
	case XCB_VISIBILITY_NOTIFY:
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_FULLY_OBSCURED:
			wait(ctx);
			break;
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
		case XCB_VISIBILITY_UNOBSCURED:
			break;
		}
		break;
	}
	free(e);
}

static inline void stat(struct ctx *ctx)
{
	int tmp;
	unsigned long user, nice, system, idle, iowait, irq, softirq;
	unsigned long used, total;

	tmp = sscanf(ctx->line, "%*s %lu %lu %lu %lu %lu %lu %lu",
		     &user, &nice, &system, &idle, &iowait, &irq, &softirq);
	if (tmp < 7)
		iowait = irq = softirq = 0;

	used = user + nice + system + irq + softirq;
	total = used + idle + iowait;

	tmp = total - ctx->stat.prev_total;
	if (tmp != 0)
		ctx->stat.load = LOADFACTOR * (used - ctx->stat.prev_used) / tmp;
	else
		ctx->stat.load = 0;

	ctx->stat.prev_used = used;
	ctx->stat.prev_total = total;
}

#define cpustr(str) (str[0] == 'c' && str[1] == 'p' && str[2] == 'u')

static inline void plot(struct ctx *ctx)
{
	int pcnt;
	xcb_point_t *tmp;

	fseek(ctx->file, 0, SEEK_SET);
	while (fgets(ctx->line, LINESIZE, ctx->file)) {
		if (!cpustr(ctx->line))
			continue;
		if (ctx->cpu < 0 && ctx->line[3] != ' ')
			continue;
		if (ctx->cpu >= 0 && (atoi(&ctx->line[3]) != ctx->cpu))
			continue;

		stat(ctx);
		pcnt = ctx->stat.load * 100 / LOADFACTOR;

		(*ctx->cur++).y = ctx->h - (int) floorf((pcnt / 100.0) * ctx->h);

		xcb_fill_poly(ctx->dpy, ctx->win, ctx->gc,
			      XCB_POLY_SHAPE_COMPLEX, XCB_COORD_MODE_ORIGIN,
			      ctx->w + 2, ctx->dat);
		xcb_flush(ctx->dpy);

		tmp = ctx->dat;
		while (++tmp < &ctx->dat[ctx->w + 1])
			(*tmp).y = (*(tmp + 1)).y;

		if (ctx->cur >= &ctx->dat[ctx->w + 1])
			ctx->cur = &ctx->dat[ctx->w];
	}
	xcb_clear_area(ctx->dpy, False, ctx->win, 0, 0, 0, 0);
}

static int init(struct ctx *ctx)
{
	int cpucnt;

	ctx->dpy = NULL;
	ctx->w = 32;
	ctx->h = 16;
	ctx->us = 100000;
	ctx->fg = 0x8fb2d8;
	ctx->bg = 0;
	ctx->class = "cpumon";
	ctx->line = NULL;
	ctx->dat = NULL;
	ctx->cpu = -1;

	ctx->file = fopen(STATFILE, "r");
	if (!ctx->file) {
		err("open("STATFILE") failed\n");
		return -1;
	}

	ctx->line = calloc(1, LINESIZE + 1);
	if (!ctx->line) {
		err("calloc(%d) failed\n", LINESIZE + 1);
		return -1;
	}

	cpucnt = 0;
	while (fgets(ctx->line, LINESIZE, ctx->file)) {
		if (cpustr(ctx->line))
			cpucnt++;
		else if (cpucnt > 0)
			break; /* assume that CPU stats are grouped */
	}

	msg("%d CPUs detected\n", cpucnt - 1);
	return 0;
}

int main(int argc, char *argv[])
{
	struct ctx ctx;
	int started;
	uint32_t val[2], mask;
	xcb_screen_t *scr;
	int ret = 1, size, i;

	if (init(&ctx) < 0)
		goto out;

	if (opts(&ctx, argc, argv) < 0)
		goto out;

	if (ctx.cpu < 0)
		msg("monitor aggregated load\n")
	else
		msg("monitor cpu%d\n", ctx.cpu);

	size = (ctx.w + 2) * sizeof(*ctx.dat);
	ctx.dat = malloc(size);
	if (!ctx.dat) {
		err("malloc(%d) failed\n", size);
		goto out;
	}
	ctx.cur = &ctx.dat[1];
	ctx.dat[0].y = ctx.h;
	for (i = 1; i < ctx.w + 2; i++) {
		ctx.dat[i].y = ctx.h;
		ctx.dat[i].x = i - 1;
	}

	ctx.dpy = xcb_connect(NULL, NULL);
	if (!ctx.dpy) {
		err("xcb_connect() failed\n");
		goto out;
	}

	scr = xcb_setup_roots_iterator(xcb_get_setup(ctx.dpy)).data;

	ctx.gc = xcb_generate_id(ctx.dpy);

	mask = XCB_GC_FOREGROUND;
	val[0] = ctx.fg;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(ctx.dpy, ctx.gc, scr->root, mask, val);

	ctx.win = xcb_generate_id(ctx.dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = ctx.bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE;
	xcb_create_window(ctx.dpy, XCB_COPY_FROM_PARENT, ctx.win, scr->root,
			  0, 0, ctx.w, ctx.h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, mask, val);

        xcb_change_property(ctx.dpy, XCB_PROP_MODE_REPLACE, ctx.win,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            sizeof("cpumon") - 1, "cpumon");

        xcb_change_property(ctx.dpy, XCB_PROP_MODE_REPLACE, ctx.win,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            strlen(ctx.class), ctx.class);

	if (ctx.bw) {
		val[0] = ctx.bw;
		mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;
		xcb_configure_window_checked(ctx.dpy, ctx.win, mask, val);
	}

	xcb_map_window(ctx.dpy, ctx.win);
	xcb_flush(ctx.dpy);

	while (1) {
		events(&ctx);
		plot(&ctx);
		usleep(ctx.us);
	}

	xcb_destroy_window(ctx.dpy, ctx.win);
	ret = 0;
out:
	fclose(ctx.file);
	free(ctx.line);
	free(ctx.dat);

	if (ctx.dpy)
		xcb_disconnect(ctx.dpy);

	return ret;
}
