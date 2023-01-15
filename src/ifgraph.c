//
// ifgraph.c
//
// ifgraph front-end.
//
// (c)2023 by Bram Stolk (b.stolk@gmail.com)
//

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <inttypes.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#include <time.h>
#include <termios.h>
#include <ctype.h>

#include "ifgraph.h"	// definitions shared between front-end / back-end.
#include "grapher.h"	// plots an image to the terminal.
#include "hsv.h"	// convert between hue-saturation-value and red-grn-blu colours.


// The candidates for network interfaces which we can analyze.
static int		numcand=0;
static char		candidates[MAXIF][32];

// The actual interfaces that we analyze.
static int		numif=0;
static char		ifnames[MAXIF][32];
static int		fd_shm[MAXIF];
static statistics_t*	statistics[MAXIF];

static const int	periods[RESCNT] = { 1, 60, 60*60, 60*60*24 };
static const char*	periodnames[RESCNT] = { "secs", "mins", "hrs", "days" };
static int		y_scale_indices[RESCNT];
static uint32_t		colours_rx[MAXIF];
static uint32_t		colours_tx[MAXIF];

static const uint64_t	scalebasis[] =
{
	15UL,
	20UL,
	30UL,
	40UL,
	60UL,
	80UL,
	100UL,
};
#define ORDERS_OF_MAGNITUDE_IN_SCALING	10	// Supports 100Gbps scale.
#define BASISLEN (sizeof(scalebasis) / sizeof(uint64_t))
#define NUMAXISSCALES (ORDERS_OF_MAGNITUDE_IN_SCALING * BASISLEN)
static uint64_t		axisscales[NUMAXISSCALES];


// At the bottom of the screen we will place a line containing the legend for the graph.
static void set_postscript(void)
{
	postscript[0] = 0;
	for (int i=0; i<numif; ++i)
	{
		strncat(postscript, SETFG "255;255;255m", sizeof(postscript) - 1 - strlen(postscript));
		strncat(postscript, ifnames[i], sizeof(postscript) - 1 - strlen(postscript));
		strncat(postscript, ": ", sizeof(postscript) - 1 - strlen(postscript));
		char legend_rx[60] = {0,};
		char legend_tx[60] = {0,};
		snprintf
		(
			legend_rx,
			sizeof(legend_rx) - 1,
			SETFG "%hhu;%hhu;%hhumRX ",
			(colours_rx[i]>> 0)&0xff,
			(colours_rx[i]>> 8)&0xff,
			(colours_rx[i]>>16)&0xff
		);
		snprintf
		(
			legend_tx,
			sizeof(legend_tx) - 1,
			SETFG "%hhu;%hhu;%hhumTX ",
			(colours_tx[i]>> 0)&0xff,
			(colours_tx[i]>> 8)&0xff,
			(colours_tx[i]>>16)&0xff
		);
		strncat(postscript, legend_rx, sizeof(postscript) - 1 - strlen(postscript));
		strncat(postscript, legend_tx, sizeof(postscript) - 1 - strlen(postscript));
	};
}


// Terminal handling
static struct termios orig_termios;
static void disableRawMode()
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
static void enableRawMode()
{
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(disableRawMode);
	struct termios raw = orig_termios;
	raw.c_lflag &= ~(ECHO);			// Don't echo key presses.
	raw.c_lflag &= ~(ICANON);		// Read by char, not by line.
	raw.c_cc[VMIN] = 0;			// No minimum nr of chars.
	raw.c_cc[VTIME] = 0;			// No waiting time.
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


static void draw_overlay(int res, int y0, int y1)
{
	// Draw the Y-scale.
	const int height = y1-y0;
	const int y_scl_idx = y_scale_indices[res];
	const uint64_t yscl = axisscales[y_scl_idx];
	const uint64_t maxbw = yscl;
	const uint64_t qmaxbw = maxbw / 4;
	for (int i=0; i<4; ++i)
	{
		uint64_t bw = (4-i) * qmaxbw;
		const char* units = "Bps";
		if (bw >= 10000000000UL)
		{
			units = "GBps";
			bw = bw / 1000000000UL;
		}
		else if (maxbw >= 10000000UL)
		{
			units = "MBps";
			bw = bw / 1000000UL;
		}
		else if (maxbw >= 10000UL)
		{
			units = "KBps";
			bw = bw / 1000UL;
		}
		memset  (overlay + imw * (y0 + height/4 * i) + 1, 0, imw < 9 ? imw : 9);
		snprintf(overlay + imw * (y0 + height/4 * i) + 1, 20, "%u %s", (uint32_t)bw, units);
	}
	// Draw the title.
	char title[80] = {0,};
	snprintf(title, sizeof(title), "last %d%s", imw-2, periodnames[res]);
	int len = (int) strlen(title);
	strncpy(overlay + imw * (y0) + (imw-len)/2, title, 20);
}


static void choose_colours(void)
{
	const float delta_hue = 1.0f / (numif+1);
	for (int i=0; i<numif; ++i)
	{
		const float hue = (i + 0.5f) * delta_hue;
		colours_tx[i] = hsv_to_rgb24(hue, 0.75f, 0.75f);
		colours_rx[i] = hsv_to_rgb24(hue, 0.60f, 0.90f);
	}
}


static int update_image(void)
{
	if ( grapher_resized )
	{
		grapher_adapt_to_new_size();
	}

	grapher_update();
	return 0;
}


static int draw_range(int histidx, int64_t maxbw, uint32_t colour, int64_t fr, int64_t to, int y0, int y1)
{
	assert(fr>=0 && to>=0);
	assert(to>=fr);
	const int x  = imw-2-histidx;
	const int64_t height = y1-y0;
	const int64_t l0 = (int64_t) (fr * 1.0f * height / maxbw + 0.5f);
	const int64_t l1 = (int64_t) (to * 1.0f * height / maxbw + 0.5f);
	const int64_t y_hi = height-1-l0;
	const int64_t y_lo = height-1-l1;
	for (int64_t y=y_lo; y<y_hi; ++y)
		if (y>=0 && y<height)
			im[ (y+y0) * imw + x ] = colour;
	return l1 >= height-1;
}


static void draw_samples(int res, int y0, int y1)
{
	assert(y0 <= imh);
	assert(y1 <= imh);

	int height = y1-y0;

	// Clear the background with dark grey bands.
	const uint8_t blck = 0x12;
	const uint8_t grey = 0x1f;
	for (int y=0; y<height; ++y)
	{
		const uint8_t v = ( ((y*4) / height) & 1 ) ? grey : blck;
		memset(im + (y0+y)*imw, v, sizeof(uint32_t)*imw);
	}

	const int y_scale_idx = y_scale_indices[res];
	const uint64_t maxbw  = axisscales[y_scale_idx];
	const uint64_t maxval = maxbw * periods[res];

	int overflow = 0;
	int underflow = 1;
	for (int x=0; x<imw-2; ++x)
	{
		uint64_t cumul = 0;
		for (int i=0; i<numif; ++i)
		{
			const uint16_t idx = (statistics[i]->wr[res] - x) & HISTMSK;
			uint64_t rx = statistics[i]->rx[res][idx];
			uint64_t tx = statistics[i]->tx[res][idx];
			overflow += draw_range(x, maxval, colours_tx[i], cumul+0,  cumul+tx,    y0, y1);
			overflow += draw_range(x, maxval, colours_rx[i], cumul+tx, cumul+tx+rx, y0, y1);
			cumul += (rx+tx);
			if (cumul > maxval/2)
				underflow = 0;
		}
	}
	if (overflow)
	{
		if (y_scale_indices[res] < (int)NUMAXISSCALES-1)
			y_scale_indices[res] += 1;
	}
	else if (underflow)
	{
		if (y_scale_indices[res] > 0)
			y_scale_indices[res] -= 1;
	}
}


static void prepare_drawing(void)
{
	// Set up scaling for Y-axis, that needs to range between 15bps and 100Gbps.
	// We want "neat" numbers at the axis ticks.
	uint64_t mult = 1UL;
	int writer = 0;
	for (int o=0; o<ORDERS_OF_MAGNITUDE_IN_SCALING; ++o)
	{
		for (uint32_t i=0; i<BASISLEN; ++i)
			axisscales[writer++] = mult * scalebasis[i];
		mult *= 10UL;
	}
	assert(writer == NUMAXISSCALES);
	y_scale_indices[0] = 15;
	y_scale_indices[1] = 15;
	y_scale_indices[2] = 15;
	y_scale_indices[3] = 15;

	choose_colours();
	set_postscript();
	int result = grapher_init();
	if ( result < 0 )
	{
		fprintf( stderr, "Failed to intialize grapher(), maybe we are not running in a terminal?\n" );
		exit(2);
	}
	enableRawMode();
	printf(SETBG "0;0;0m");
	printf(CLEARSCREEN);
	update_image();
}


static void drawloop(void)
{
	int done=0;
	do
	{
		usleep(1000000);

		draw_overlay(0, 0,       imh/4);

		draw_overlay(1, imh/4+1, imh/2);

		draw_samples(0, 0,       imh/2);

		draw_samples(1, imh/2+1, imh-0);

		update_image();

		// See if user pressed ESC.
		char c=0;
		const int numr = read( STDIN_FILENO, &c, 1 );
		if ( numr == 1 && ( c == 27 || c == 'q' || c == 'Q' ) )
			done=1;
	} while(!done);

	grapher_exit();
}


static void cleanup(void)
{
	for (int i=0; i<numif; ++i)
	{
		if (munmap(statistics[i], sizeof(statistics)))
			fprintf(stderr, "munmap failed: %s\n", strerror(errno));
		statistics[i] = 0;
		if (close(fd_shm[i]))
			fprintf(stderr, "close failed: %s\n", strerror(errno));
		fd_shm[i] = 0;
	}
}


int main(int argc, char* argv[])
{
	if (argc != 1)
	{
		fprintf(stderr, "%s takes no arguments.\n", argv[0]);
		exit(1);
	}

	const char* dname = "/sys/class/net";

	// Determine all candidate network interfaces.
	DIR* dir = opendir(dname);
	if (!dir)
	{
		fprintf(stderr, "Failed to open %s: %s\n", dname, strerror(errno));
		exit(1);
	}
	struct dirent* entry=0;
	numcand=0;
	do {
		errno = 0;
		entry = readdir(dir);
		if (!entry && errno)
		{
			fprintf(stderr, "readdir() failed for %s: %s\n", dname, strerror(errno));
			exit(2);
		}
		else if (entry && (entry->d_type & DT_LNK))
		{
			strncpy(candidates[numcand], entry->d_name, sizeof(candidates[numcand]) - 1);
			numcand++;
		}
	} while(entry && numcand < MAXIF);

	// See which of the candidate interfaces have readable shared memory blocks for it.
	for (int i=0; i<numcand; ++i)
	{
		const char* name = candidates[i];
		char shm_nm[80] = {0,};
		snprintf(shm_nm, sizeof(shm_nm), SHM_NAME_FMT, name);
		const int fd = shm_open(shm_nm, O_RDONLY, 0);
		if (fd > 0)
		{
			fd_shm[numif] = fd;
			strncpy(ifnames[numif], name, sizeof(ifnames[numif]) - 1);
			const size_t sz = sizeof(statistics_t);
			statistics[numif] = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
			if (statistics[numif] == MAP_FAILED)
				fprintf(stderr, "mmap failed: %s\n", strerror(errno));
			else
				numif += 1;
		}
	}

	if (!numif)
	{
		fprintf(stderr, "None of the %d candidate network interfaces had shared memory blocks defined for it.\n", numcand);
		fprintf(stderr, "Is the ifgraphd daemon running?\n");
		exit(3);
	}

	fprintf(stderr, "Reading statistics for ");
	for (int i=0; i<numif; ++i)
		fprintf(stderr,"%s%c", ifnames[i], i==numif-1 ? '\n' : ' ');

	prepare_drawing();

	drawloop();

	cleanup();

	return 0;
}

