/* 
 * Copyright (C) 2012 Simon Wunderlich <siwu@hrz.tu-chemnitz.de>
 * Copyright (C) 2012 Fraunhofer-Gesellschaft zur Foerderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 * 
 */

/*
 * This program has been created to aid open source spectrum
 * analyzer development for Qualcomm/Atheros AR92xx and AR93xx
 * based chipsets.
 */

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <pcap.h>
#include <pthread.h>
#include <unistd.h>

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>

#include "fft_eval.h"

#include "net80211/ieee80211.h"
#include "net80211/ieee80211_radiotap.h"

#include "libradarpkt/pkt.h"
#include "libradarpkt/ar5212_radar.h"
#include "libradarpkt/ar5416_radar.h"
#include "libradarpkt/ar9280_radar.h"

#include "fft_eval.h"
#include "fft_freebsd.h"
#include "fft_histogram.h"

#define WIDTH	1600
#define HEIGHT	650
#define BPP	32

#define X_SCALE	5
#define Y_SCALE	4

#define	RMASK 	0x000000ff
#define RBITS	0
#define	GMASK	0x0000ff00
#define GBITS	8
#define	BMASK	0x00ff0000
#define	BBITS	16
#define	AMASK	0xff000000

/* XXX ew globals */
pthread_mutex_t mtx_histogram;
int g_do_update = 0;

SDL_Surface *screen = NULL;
TTF_Font *font = NULL;

int graphics_init_sdl(void)
{
	SDL_VideoInfo *VideoInfo;
	int SDLFlags;

	SDLFlags = SDL_HWPALETTE | SDL_RESIZABLE;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "Initializing SDL failed\n");
		return -1;
	}
		
	if ((VideoInfo = (SDL_VideoInfo *) SDL_GetVideoInfo()) == NULL) {
		fprintf(stderr, "Getting SDL Video Info failed\n");
		return -1;
	}

	else {
		if (VideoInfo->hw_available) {
			SDLFlags |= SDL_HWSURFACE;
		} else {
			SDLFlags |= SDL_SWSURFACE;
		}
		if (VideoInfo->blit_hw)
			SDLFlags |= SDL_HWACCEL;
	}

	SDL_WM_SetCaption("FFT eval", "FFT eval");
	screen = SDL_SetVideoMode(WIDTH, HEIGHT, BPP, SDLFlags);

	if (TTF_Init() < 0) {
		fprintf(stderr, "Initializing SDL TTF failed\n");
		return -1;
	}

	font = TTF_OpenFont("font/LiberationSans-Regular.ttf", 14);
	if (!font) {
		fprintf(stderr, "Opening font failed\n");
		return -1;
	}

	return 0;
}

void graphics_quit_sdl(void)
{
	SDL_Quit();
}

int pixel(Uint32 *pixels, int x, int y, Uint32 color)
{
	if (x < 0 || x >= WIDTH)
		return -1;
	if (y < 0 || y >= HEIGHT)
		return -1;

	pixels[x + y * WIDTH] |= color;
	return 0;
}

#define SIZE 2

/* Is this pixel in the viewport? */
static int
is_in_viewport(int x, int y)
{
	if (x - SIZE < 0 || x + SIZE >= WIDTH)
		return 0;
	if (y - SIZE < 0 || y + SIZE >= HEIGHT)
		return 0;
	return (1);
}

/* this function blends a 2*SIZE x 2*SIZE blob at the given position with
 * the defined opacity. */
int bigpixel(Uint32 *pixels, int x, int y, Uint32 color, uint8_t opacity)
{
	int x1, y1;

	if (! is_in_viewport(x, y))
		return -1;

	for (x1 = x - SIZE; x1 < x + SIZE; x1++)
	for (y1 = y - SIZE; y1 < y + SIZE; y1++) {
		Uint32 r, g, b;

		r = ((pixels[x1 + y1 * WIDTH] & RMASK) >> RBITS) + ((((color & RMASK) >> RBITS) * opacity) / 255);
		if (r > 255) r = 255;
		g = ((pixels[x1 + y1 * WIDTH] & GMASK) >> GBITS) + ((((color & GMASK) >> GBITS) * opacity) / 255);
		if (g > 255) g = 255;
		b = ((pixels[x1 + y1 * WIDTH] & BMASK) >> BBITS) + ((((color & BMASK) >> BBITS) * opacity) / 255);
		if (b > 255) b = 255;

		pixels[x1 + y1 * WIDTH] = r << RBITS | g << GBITS | b << BBITS | (color & AMASK);
	}
	return 0;
}

int render_text(SDL_Surface *surface, char *text, int x, int y)
{
	SDL_Surface *text_surface;
	SDL_Color fontcolor = {255, 255, 255, 255};
	SDL_Rect fontdest = {0, 0, 0, 0};

	fontdest.x = x;
	fontdest.y = y;

	text_surface = TTF_RenderText_Solid(font, text, fontcolor);
	if (!text_surface)
		return -1;

	SDL_BlitSurface(text_surface, NULL, surface, &fontdest);
	SDL_FreeSurface(text_surface);

	return 0;
}

/*
 * draw_picture - draws the current screen.
 *
 * @highlight: the index of the dataset to be highlighted
 *
 * returns the center frequency of the currently highlighted dataset
 */
int draw_picture(int highlight, int startfreq)
{
	Uint32 *pixels, color, opacity;
	int x, y, i, rnum, j;
	int highlight_freq = startfreq + 20;
	char text[1024];
	struct scanresult *result;
	SDL_Surface *surface;

	surface = SDL_CreateRGBSurface(SDL_SWSURFACE, WIDTH, HEIGHT, BPP, RMASK, GMASK, BMASK, AMASK);
	pixels = (Uint32 *) surface->pixels;
	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			pixels[x + y * WIDTH] = AMASK;

	/* vertical lines (frequency) */
	for (i = 2300; i < 6000; i += 20) {
		x = (X_SCALE * (i - startfreq));

		if (x < 0 || x > WIDTH)
			continue;

		for (y = 0; y < HEIGHT - 20; y++)
			pixels[x + y * WIDTH] = 0x40404040 | AMASK;

		snprintf(text, sizeof(text), "%d MHz", i);
		render_text(surface, text, x - 30, HEIGHT - 20);
	}

	/* horizontal lines (dBm) */
	for (i = 0; i < 150; i += 10) {
		y = 600 - Y_SCALE * i;
		
		for (x = 0; x < WIDTH; x++)
			pixels[x + y * WIDTH] = 0x40404040 | AMASK;

		snprintf(text, sizeof(text), "-%d dBm", (150 - i));
		render_text(surface, text, 5, y - 15);
	}

	/* Render 2300 -> 6000 in 1MHz increments, but using KHz math */
	/* .. as right now the canvas is .. quite large. */
	/* XXX should just do it based on the current viewport! */
	for (i = 2300*1000; i < 6000*1000; i+= 250) {
		float signal;
		int freqKhz = i;
		int16_t *s;

		x = X_SCALE * (freqKhz - (startfreq * 1000)) / 1000;

		if (x < 0 || x > WIDTH)
			continue;

		/* Fetch dBm value at the given frequency in KHz */
		s = fft_fetch_freq_avg(freqKhz);
		if (s == NULL)
			continue;

		for (j = 0; j < FFT_HISTOGRAM_HISTORY_DEPTH; j++) {
			if (s[j] == 0)
				continue;
			signal = (float) s[j];
			color = BMASK | AMASK;
			opacity = 64;
			y = 400 - (400.0 + Y_SCALE * signal);
			if (bigpixel(pixels, x, y, color, opacity) < 0)
				continue;
		}


		/* .. and the max */
		signal = (float) fft_fetch_freq_max(freqKhz);
		color = RMASK | AMASK;
		opacity = 128;
		y = 400 - (400.0 + Y_SCALE * signal);
		if (bigpixel(pixels, x, y, color, opacity) < 0)
			continue;
	}

	SDL_BlitSurface(surface, NULL, screen, NULL);
	SDL_FreeSurface(surface);
	SDL_Flip(screen); 

	return highlight_freq;
}

/*
 * graphics_main - sets up the data and holds the mainloop.
 *
 */
void graphics_main(void)
{
	SDL_Event event;
	int quit = 0;
	int highlight = 0;
	int change = 1, scroll = 0;
	int startfreq = 2350, accel = 0;
	int highlight_freq = startfreq;

	if (graphics_init_sdl() < 0) {
		fprintf(stderr, "Failed to initialize graphics.\n");
		return;
	}
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
	while (!quit) {
		pthread_mutex_lock(&mtx_histogram);
		if (g_do_update == 1) {
			change = 1;	/* XXX always render */
			g_do_update = 0;
		}
		pthread_mutex_unlock(&mtx_histogram);

		if (change) {
			highlight_freq = draw_picture(highlight, startfreq);
			change = 0;
		}

		if (!scroll) {
			/* move to highlighted object */
			if (highlight_freq - 20 < startfreq)
				accel = -10;
			if (highlight_freq > (startfreq + WIDTH/X_SCALE))
				accel = 10;
			
			/* if we are "far off", move a little bit faster */
			if (highlight_freq + 300 < startfreq)
				accel = -100;
	
			if (highlight_freq - 300 > (startfreq + WIDTH/X_SCALE))
				accel = 100;
		}

//		if (accel)
			SDL_PollEvent(&event);
//		else
//			SDL_WaitEvent(&event);

		switch (event.type) {
		case SDL_QUIT:
			quit = 1;
			break;
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym) {
#if 0
			case SDLK_LEFT:
				if (highlight > 0) {
					highlight--;
					scroll = 0;
					change = 1;
				}
				break;
			case SDLK_RIGHT:
				if (highlight < scanresults_n - 1){
					highlight++;
					scroll = 0;
					change = 1;
				}
				break;
#endif
			case SDLK_PAGEUP:
				accel-= 2;
				scroll = 1;
				break;
			case SDLK_PAGEDOWN:
				accel+= 2;
				scroll = 1;
				break;
			case SDLK_HOME:
				startfreq = 2300;
				accel = 0;
				break;
			case SDLK_END:
				startfreq = 5100;
				accel = 0;
				break;
			default:
				break;
			}
			break;
		}
		if (accel) {
			startfreq += accel;
			if (accel > 0)			accel--;
			if (accel < 0)			accel++;
			change = 1;
		}
		if (startfreq < 2300)		startfreq = 2300;
		if (startfreq > 6000)		startfreq = 6000;
		if (accel < -20)		accel = -20;
		if (accel >  20)		accel = 20;
	}

	graphics_quit_sdl();
}

void usage(int argc, char *argv[])
{
	fprintf(stderr, "Usage: %s [scanfile]\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "scanfile is generated by the spectral analyzer feature\n");
	fprintf(stderr, "of your wifi card. If you have a AR92xx or AR93xx based\n");
	fprintf(stderr, "card, try:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "ifconfig wlan0 up\n");
	fprintf(stderr, "iw dev wlan0 scan spec-scan\n");
	fprintf(stderr, "cat /sys/kernel/debug/ieee80211/phy0/ath9k/spectral_scan > /tmp/fft_results\n");
	fprintf(stderr, "%s /tmp/fft_results\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "(NOTE: maybe debugfs must be mounted first: mount -t debugfs none /sys/kernel/debug/ )\n");
	fprintf(stderr, "\n");

}

static void
fft_eval_cb(struct radar_entry *re, void *cbdata)
{
	struct radar_fft_entry *fe;
	int i;

	pthread_mutex_lock(&mtx_histogram);
	for (i = 0; i < re->re_num_spectral_entries; i++) {
		fft_add_sample(re, &re->re_spectral_entries[i]);
	}
	g_do_update = 1;
	pthread_mutex_unlock(&mtx_histogram);

}

int main(int argc, char *argv[])
{
	int ret;

	if (argc < 2) {
		usage(argc, argv);
		return -1;
	}

	fprintf(stderr, "WARNING: Experimental Software! Don't trust anything you see. :)\n");
	fprintf(stderr, "\n");

	/* Setup radar entry callback */
	pthread_mutex_init(&mtx_histogram, NULL);
	set_scandata_callback(fft_eval_cb, NULL);

	/* Fetch data */
	ret = read_scandata_freebsd(argv[1], NULL);

	if (ret < 0) {
		fprintf(stderr, "Couldn't read scanfile ...\n");
		usage(argc, argv);
		return -1;
	}
	graphics_main();

	return 0;
}
