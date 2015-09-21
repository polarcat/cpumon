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
#define eopt(opt) printf("(EE) malformed "opt" parameter");

#define SLEEP_TIME 100000 /* microseconds */
#define LINE_SIZE 256
#define STAT_FILE "/proc/stat"
#define LOAD_FACTOR 1024

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

struct cpustat {
	unsigned long load;
	unsigned long long prev_used;
	unsigned long long prev_total;
};

static inline void line2cpustat(char *line, struct cpustat *stat)
{
	int tmp;
	unsigned long user, nice, system, idle, iowait, irq, softirq;
	unsigned long used, total;

	tmp = sscanf(line, "%*s %lu %lu %lu %lu %lu %lu %lu",
		     &user, &nice, &system, &idle, &iowait, &irq, &softirq);
	if (tmp < 7)
		iowait = irq = softirq = 0;

	used = user + nice + system + irq + softirq;
	total = used + idle + iowait;

	tmp = total - stat->prev_total;
	if (tmp != 0)
		stat->load = LOAD_FACTOR * (used - stat->prev_used) / tmp;
	else
		stat->load = 0;

	stat->prev_used = used;
	stat->prev_total = total;
}

struct gcfg {
	xcb_connection_t *dpy;
	xcb_drawable_t win;
	xcb_gcontext_t gc;
	xcb_point_t *dat;
	unsigned int w, h, us, fg, bg;
	const char *class;
};

static void *plot(void *arg)
{
	xcb_point_t *tmp, *cur, *end, *ptr;
	struct gcfg *cfg = (struct gcfg *) arg;
	FILE *f;
	char *user;
	char *line;
	size_t size;
	ssize_t len;
	int i, cpucnt, pcnt;
	struct cpustat **stat = NULL;
	pthread_t t;

	f = fopen(STAT_FILE, "r");
	if (!f) {
		err("open("STAT_FILE") failed\n");
		return NULL;
	}

	size = LINE_SIZE;
	line = calloc(1, size + 1);
	if (!line) {
		err("calloc(%d) failed\n", (int) size + 1);
		goto out;
	}

	cpucnt = 0;
	while ((len = getline(&line, &size, f)) > 0) {
		if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u')
			cpucnt++;
		else if (cpucnt > 0)
			break;
	}

	msg("%d CPUs detected\n", cpucnt - 1);
	stat = calloc(1, cpucnt * sizeof(*stat));
	if (!stat) {
		err("calloc(%d) failed\n", (int) (cpucnt * sizeof(*stat)));
		goto out;
	}
	for (i = 0; i < cpucnt; i++) {
		stat[i] = calloc(1, sizeof(stat));
		if (!stat[i]) {
			err("calloc(%d) failed\n", (int) sizeof(stat[0]));
			goto out;
		}
	}

	ptr = cfg->dat;
	(*ptr).x = (*(ptr + 1)).x = 0;
	(*ptr).y = (*(ptr + 1)).y = (*(ptr + (cfg->w + 2))).y = cfg->h;

	cfg->dat[0].x = cfg->dat[1].x = 0;
	cfg->dat[0].y = cfg->dat[cfg->w + 1].y = cfg->dat[cfg->w + 2].y = cfg->h;
	/* initialize data */
	tmp = &cfg->dat[1];
	i = 0;
	while (tmp++ < &cfg->dat[cfg->w + 1]) {
		(*tmp).y = cfg->h;
		(*tmp).x = i++;
	}

	cur = &cfg->dat[1];
	end = &cfg->dat[cfg->w + 1];
loop:
	fseek(f, 0, SEEK_SET);
	while ((len = getline(&line, &size, f)) > 0) {
		if (line[0] != 'c' || line[1] != 'p' || line[2] != 'u')
			continue;
		if (line[3] != ' ' && line[3] != '\t')
			continue;

		line2cpustat(line, stat[0]);
		pcnt = stat[0]->load * 100 / LOAD_FACTOR;

		(*cur++).y = cfg->h - (int) floorf((pcnt / 100.0) * cfg->h);

		xcb_clear_area(cfg->dpy, False, cfg->win, 0, 0, 0, 0);
		xcb_fill_poly(cfg->dpy, cfg->win, cfg->gc,
			      XCB_POLY_SHAPE_COMPLEX, XCB_COORD_MODE_ORIGIN,
			      cfg->w + 2, cfg->dat);
		xcb_flush(cfg->dpy);

		tmp = cfg->dat;
		while (++tmp < end)
			(*tmp).y = (*(tmp + 1)).y;

#if 0
	for (i = 0; i < cfg->w + 2; i++) {
		printf("%02d ", cfg->dat[i].y);
	}
	printf("\n");
#endif

		if (cur >= end)
			cur = end - 1;
	}
	usleep(cfg->us);
	goto loop;

out:
	printf("done\n");
	free(line);
	if (stat) {
		for (i = 0; i < cpucnt; i++)
			free(stat[i]);
		free(stat);
	}
	fclose(f);
	return NULL;
}

static void usage(struct gcfg *cfg, const char *name)
{
	printf("Usage: %s <options>\n"
		"Options:\n"
		"  -h, --help           print this message\n"
		"  -s, --size     <WxH> set window size\n"
		"  -i, --interval <n>   refresh interval (ms)\n"
		"  -fg, --fgcolor <n>   foreground color\n"
		"  -bg, --bgcolor <n>   background color\n"
		"Defaults:\n"
		"  size: %dx%d, interval %d ms, fg %d, bg %d\n",
		name, cfg->w, cfg->h, cfg->us / 1000, cfg->fg, cfg->bg);
}

static int getopt(const char *s, const char *l, const char *o)
{
	return !strcmp(s, o) || !strcmp(l, o);
}

static int getopt2(const char *s, const char *l, const char *o, const char *p)
{
	return (!strcmp(s, o) || !strcmp(l, o)) && p;
}

static void checkparam(const char *opt, const char *param)
{
	if (!param) {
		printf("missing %s parameter\n", opt);
		exit(1);
	}
}

static void getopts(struct gcfg *cfg, int argc, char *argv[])
{
	int i = 0;

	while (++i < argc && argv[i]) {
		if (getopt("-h", "--help", argv[i])) {
			usage(cfg, argv[0]);
			exit(0);
		} else if (getopt("-s", "--size", argv[i])) {
			checkparam("size", argv[i + 1]);
			char *tmp = strchr(argv[i + 1], 'x');

			if (!tmp) {
				eopt("size");
				exit(1);
			}
			cfg->w = atoi(argv[i + 1]);
			cfg->h = atoi(tmp + 1);
			if (cfg->w == 0 || cfg->h == 0) {
				eopt("size");
				exit(1);
			}
		} else if (getopt("-i", "--interval", argv[i])) {
			checkparam("interval", argv[i + 1]);
			cfg->us = atoi(argv[i + 1]) * 1000;
			if (cfg->us == 0) {
				eopt("interval");
				exit(1);
			}
		} else if (getopt("-c", "--class", argv[i])) {
			checkparam("class", argv[i + 1]);
			cfg->class = argv[i + 1];
		} else if (getopt("-fg", "--fgcolor", argv[i])) {
			checkparam("fgcolor", argv[i + 1]);
			cfg->fg = atoi(argv[i + 1]);
		} else if (getopt("-bg", "--bgcolor", argv[i])) {
			checkparam("bgcolor", argv[i + 1]);
			cfg->bg = atoi(argv[i + 1]);
		}
	}
}

int main(int argc, char *argv[])
{
	pthread_t t;
	struct gcfg cfg;
	int started;
	uint32_t val[2], mask;
	xcb_screen_t *scr;
	xcb_generic_event_t *evt;

	/* set defaults */
	cfg.w = 32;
	cfg.h = 16;
	cfg.us = 100000;
	cfg.fg = 0x8fb2d8;
	cfg.bg = 0;
	cfg.class = "cpumon";

	getopts(&cfg, argc, argv);

	if (cfg.w > 0) {
		int size = (cfg.w + 2) * sizeof(*cfg.dat);
		cfg.dat = malloc(size);
		if (!cfg.dat) {
			err("malloc(%d) failed\n", size);
			return 1;
		}
	}

	cfg.dpy = xcb_connect(NULL, NULL);
	if (!cfg.dpy) {
		err("xcb_connect() failed\n");
		free(cfg.dat);
		return 1;
	}

	scr = xcb_setup_roots_iterator(xcb_get_setup(cfg.dpy)).data;

	cfg.gc = xcb_generate_id(cfg.dpy);

	mask = XCB_GC_FOREGROUND;
	val[0] = cfg.fg;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(cfg.dpy, cfg.gc, scr->root, mask, val);

	cfg.win = xcb_generate_id(cfg.dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = cfg.bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	xcb_create_window(cfg.dpy, XCB_COPY_FROM_PARENT, cfg.win, scr->root,
			  0, 0, cfg.w, cfg.h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, mask, val);

        xcb_change_property(cfg.dpy, XCB_PROP_MODE_REPLACE, cfg.win,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            sizeof("cpumon") - 1, "cpumon");

        xcb_change_property(cfg.dpy, XCB_PROP_MODE_REPLACE, cfg.win,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            strlen(cfg.class), cfg.class);

	xcb_map_window(cfg.dpy, cfg.win);
	xcb_flush(cfg.dpy);
	started = 0;
	while ((evt = xcb_wait_for_event(cfg.dpy))) {
		switch (evt->response_type & ~0x80) {
		case XCB_EXPOSE:
			if (!started) {
				pthread_create(&t, NULL, plot, &cfg);
				started = 1;
			}
			xcb_flush(cfg.dpy);
			break;
		default:
			break;
		}
		free(evt);
	}

	free(cfg.dat);
	xcb_destroy_window(cfg.dpy, cfg.win);
	xcb_disconnect(cfg.dpy);
	return 0;
}
