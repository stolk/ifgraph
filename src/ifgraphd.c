//
// ifgraphd.c
//
// ifgraph daemon process.
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
#include <signal.h>

#include "ifgraph.h"


// The candidates for network interfaces which we can analyze.
static int		numcand=0;
static char		candidates[MAXIF][32];

// The actual interfaces that we analyze.
static int		numif=0;
static char		ifnames[MAXIF][32];
static FILE*		f_rx[MAXIF];
static FILE*		f_tx[MAXIF];
static int		fd_shm[MAXIF];
static statistics_t*	statistics[MAXIF];

// Last read values, from which we compute deltas.
static uint64_t		previous_rx[MAXIF][RESCNT];
static uint64_t		previous_tx[MAXIF][RESCNT];

// The seconds-graph needs to record on every tick.
// The days-graph only needs to record on every 86400 ticks.
static int		ticks[RESCNT];
static const int	periods[RESCNT] = { 1, 60, 60*60, 60*60*24 };


// Sample network interface statistics.
static void sample(int ifnr, uint64_t* rx, uint64_t* tx)
{
	char info_rx[32] = {0,};
	char info_tx[32] = {0,};
	const int numr_rx = fread(info_rx, 1, sizeof(info_rx)-1, f_rx[ifnr]);
	const int numr_tx = fread(info_tx, 1, sizeof(info_tx)-1, f_tx[ifnr]);
	assert(numr_rx > 0);
	assert(numr_tx > 0);
	rewind(f_rx[ifnr]);
	rewind(f_tx[ifnr]);
	*rx = atol(info_rx);
	*tx = atol(info_tx);
}


// Record all samples for all interfaces for this tick.
static int record(void)
{
	int numrecorded = 0;
	for (int ifnr=0; ifnr<numif; ++ifnr)
	{
		uint64_t rx;
		uint64_t tx;
		sample(ifnr, &rx, &tx);
		//fprintf(stderr, "rx = %" PRIu64 "\n", rx);
		for (int res=0; res<RESCNT; ++res)
		{
			if (ticks[res] == 0)
			{
				uint16_t* wr = statistics[ifnr]->wr + res;
				*wr = (*wr+1) & HISTMSK;
				const uint64_t d_rx = rx - previous_rx[ifnr][res];
				const uint64_t d_tx = tx - previous_tx[ifnr][res];
				//fprintf(stderr, "res: %d  d_rx: %" PRIu64 "  d_tx: %" PRIu64 "\n", res, d_rx, d_tx);
				statistics[ifnr]->rx[res][*wr] = d_rx;
				statistics[ifnr]->tx[res][*wr] = d_tx;
				previous_rx[ifnr][res] = rx;
				previous_tx[ifnr][res] = tx;
				numrecorded += 1;
			}
		}
	}
	for (int res=0; res<RESCNT; ++res)
	{
		ticks[res] += 1;
		if (ticks[res] >= periods[res])
			ticks[res] = 0;
	}
	return numrecorded;
}


static int unlink_shared_memory_blocks(void)
{
	int numunlinked=0;
	for (int i=0; i<numif; ++i)
	{
		char nm[128] = {0,};
		snprintf(nm, sizeof(nm)-1, SHM_NAME_FMT, ifnames[i]);
		if (fd_shm[i])
		{
			if (statistics[i])
			{
				if (munmap(statistics[i], sizeof(statistics_t)))
					fprintf(stderr, "munmap failed: %s\n", strerror(errno));
				statistics[i] = 0;
			}
			if (shm_unlink(nm))
				fprintf(stderr, "shm_unlink failed for %s: %s\n", nm, strerror(errno));
			else
				numunlinked += 1;
		}
	}
	return numunlinked;
}


static int create_shared_memory_blocks(void)
{
	int numcreated=0;
	for (int i=0; i<numif; ++i)
	{
		char nm[128] = {0,};
		snprintf(nm, sizeof(nm)-1, SHM_NAME_FMT, ifnames[i]);
		fd_shm[i] = shm_open(nm, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR);
		if (!fd_shm[i])
			fprintf(stderr, "shm_open failed for %s: %s\n", nm, strerror(errno));
		else
		{
			if (ftruncate(fd_shm[i], sizeof(statistics_t)))
				fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
			const size_t sz = sizeof(statistics_t);
			statistics[i] = (statistics_t*) mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm[i], 0);
			if (statistics[i] == MAP_FAILED)
			{
				fprintf(stderr, "mmap failed: %s\n", strerror(errno));
			}
			else
			{
				memset(statistics[i], 0, sizeof(statistics_t));
				numcreated++;
			}
		}
	}
	return numcreated;
}


static void service(void)
{
	while(1)
	{
		record();
		usleep(1000000);
	}
}


static void prepare_service(void)
{
	// Read the initial counts from /sysfs
	for (int i=0; i<numif; ++i)
	{
		assert(f_rx[i]);
		assert(f_tx[i]);
		sample(i, previous_rx[i], previous_tx[i]);
		for (int r=1; r<RESCNT; ++r)
		{
			previous_rx[i][r] = previous_rx[i][0];
			previous_tx[i][r] = previous_tx[i][0];
		}
	}
}


static void cleanup(void)
{
	const int numunlinked = unlink_shared_memory_blocks();
	if (numunlinked != numif)
		fprintf(stderr, "Not all shared memory blocks were unlinked?\n");
	else
		fprintf(stderr, "Unlinked %d shared memory blocks.\n", numunlinked);
}


static void sig_handler(int signum)
{
	if (signum == SIGHUP)
	{
	}
	if (signum == SIGTERM || signum == SIGINT)
	{
		cleanup();
		exit(0);
	}
}


int main(int argc, char* argv[])
{
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
		else if (entry && (entry->d_type & DT_LNK) && strcmp(entry->d_name, "lo"))
		{
			strncpy(candidates[numcand], entry->d_name, sizeof(candidates[numcand]) - 1);
			numcand++;
		}
	} while(entry && numcand < MAXIF);

	// If specified on command line, use those interface names, else use all candidates.
	if (argc==1)
	{
		memcpy(ifnames, candidates, sizeof(ifnames));
		numif = numcand;
	}
	else
	{
		for (int i=1; i<argc; ++i)
		{
			const char* name = argv[i];
			for (int j=0; j<numcand; ++j)
			{
				if (!strcmp(candidates[j], name))
				{
					strncpy(ifnames[numif], candidates[j], sizeof(ifnames[numif]) - 1);
					numif += 1;
				}
			}
		}
	}
	if (!numif)
	{
		if (argc>1)
		{
			fprintf(stderr, "No interface selected. Candidates are:\n");
			for (int i=0; i<numcand; ++i)
				fprintf(stderr, "%s\n", candidates[i]);
		}
		else
			fprintf(stderr, "No interfaces found.\n");
		exit(3);
	}

	for (int i=0; i<numif; ++i)
	{
		const char* nm = ifnames[i];
		char path_rx[128] = {0,};
		char path_tx[128] = {0,};
		snprintf(path_rx, sizeof(path_rx)-1, "%s/%s/statistics/rx_bytes", dname, nm);
		snprintf(path_tx, sizeof(path_tx)-1, "%s/%s/statistics/tx_bytes", dname, nm);
		f_rx[i] = fopen(path_rx, "rb");
		if (!f_rx[i])
		{
			fprintf(stderr, "Failed to open %s: %s\n", path_rx, strerror(errno));
			exit(4);
		}
		f_tx[i] = fopen(path_tx, "rb");
		if (!f_tx[i])
		{
			fprintf(stderr, "Failed to open %s: %s\n", path_tx, strerror(errno));
			exit(4);
		}
	}

	fprintf(stderr, "Recording statistics for ");
	for (int i=0; i<numif; ++i)
		fprintf(stderr,"%s%c", ifnames[i], i==numif-1 ? '\n' : ' ');

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGHUP,  sig_handler);

	const int numcreated = create_shared_memory_blocks();
	if (numcreated == numif)
	{
		prepare_service();
		service();
	}

	cleanup();

	return 0;
}

