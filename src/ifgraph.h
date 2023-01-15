//
// ifgraph.h
// 
// (c)2023 by Bram Stolk (b.stolk@gmail.com)
//

#define HISTSZ	256		// size of the history we record.
#define HISTMSK (HISTSZ-1)

#define MAXIF	32		// no more than 32 network devices.

enum {
	RES_SC=0,	// sample per second.
	RES_MN,		// sample per minute.
	RES_HR,		// sample per hour.
	RES_DY,		// sample per day.
	RESCNT
};

typedef struct
{
	uint64_t tx[RESCNT][HISTSZ];	// transmitted
	uint64_t rx[RESCNT][HISTSZ];	// received
	uint16_t wr[RESCNT];		// wr index into circular buffers.
} statistics_t;

