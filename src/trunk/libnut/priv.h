#ifndef _NUT_PRIV_H
#define _NUT_PRIV_H

//#define NDEBUG
//#define TRACE
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ID_STRING "nut/multimedia container"

#if 0
#define     MAIN_STARTCODE (0x7A561F5F04ADULL + (((uint64_t)('N'<<8) + 'M')<<48))
#define   STREAM_STARTCODE (0x11405BF2F9DBULL + (((uint64_t)('N'<<8) + 'S')<<48))
#define KEYFRAME_STARTCODE (0xE4ADEECA4569ULL + (((uint64_t)('N'<<8) + 'K')<<48))
#define    INDEX_STARTCODE (0xDD672F23E64EULL + (((uint64_t)('N'<<8) + 'X')<<48))
#define     INFO_STARTCODE (0xAB68B596BA78ULL + (((uint64_t)('N'<<8) + 'I')<<48))
#else
#define     MAIN_STARTCODE 0x4E4D7A561F5F04ADULL
#define   STREAM_STARTCODE 0x4E5311405BF2F9DBULL
#define KEYFRAME_STARTCODE 0x4E4BE4ADEECA4569ULL
#define    INDEX_STARTCODE 0x4E58DD672F23E64EULL
#define     INFO_STARTCODE 0x4E49AB68B596BA78ULL
#endif

// FIXME subtitles

#define PREALLOC_SIZE 4096

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

enum errors {
	ERR_GENERAL_ERROR = 1,
	ERR_BAD_VERSION = 2,
	ERR_NOT_FRAME_NOT_N = 3,
	ERR_BAD_CHECKSUM = 4,
	ERR_NO_HEADERS = 6,
	ERR_NOT_SEEKABLE = 7,
	ERR_BAD_STREAM_ORDER = 11,
	ERR_NOSTREAM_STARTCODE = 12,
	ERR_BAD_EOF = 13,
};

typedef struct {
	nut_input_stream_t isc;
	int is_mem;
	uint8_t *buf;
	uint8_t *buf_ptr;
	int write_len; // memory allocated
	int read_len;  // data in memory
	off_t file_pos;
	off_t filesize;
} input_buffer_t;

typedef struct {
	nut_output_stream_t osc;
        int is_mem;
	uint8_t *buf;
	uint8_t *buf_ptr;
	int write_len; // memory allocated
	off_t file_pos;
} output_buffer_t;

typedef struct {
	int flags;
	int stream_plus1;
	int pts_delta;
	int lsb;
	int mul;
	int reserved;
} frame_table_t;

typedef struct {
	uint64_t pts;
	off_t pos;
} index_item_t;

typedef struct {
	int len;
	int alloc_len;
	index_item_t * ii;
} index_context_t;

typedef struct {
	uint64_t pts;
	off_t pos;
	int back_ptr;
} syncpoint_t;

typedef struct {
	int len;
	int alloc_len;
	syncpoint_t * s;
} syncpoint_list_t;

typedef struct {
	nut_packet_t p;
	uint8_t * buf;
	int64_t dts;
} reorder_packet_t;

typedef struct {
	int last_key; // muxer.c, re-set to 0 on every keyframe
	int last_pts;
	int last_dts;
	int msb_pts_shift;
	int decode_delay; // FIXME
	index_context_t index;
	nut_stream_header_t sh;
	int64_t * pts_cache;
	off_t back_ptr;
	// reorder.c
	int next_pts;
	reorder_packet_t * packets;
	int num_packets;
	int64_t * reorder_pts_cache;
	// debug stuff
	int overhead;
	int tot_size;
	int total_frames;
} stream_context_t;

typedef struct {
	int tmp_flag;      // 1 => use msb, 2 => is keyframe, 4 => invalid
	int tmp_fields;
	int tmp_pts;       // tmp_fields = 1
	int tmp_mul;       // tmp_fields = 2
	int tmp_stream;    // tmp_fields = 3
	int tmp_size;      // tmp_fields = 4
	int count;         // tmp_fields = 6 (5 is reserved)
} frame_table_input_t;

struct nut_context_s {
	nut_muxer_opts_t mopts;
	nut_demuxer_opts_t dopts;
	input_buffer_t * i;
	output_buffer_t * o;

	int stream_count;
	stream_context_t * sc;

	int max_distance;
	int max_index_distance;
	nut_timebase_t global_timebase;
	frame_table_input_t * fti;
	frame_table_t ft[256];

	off_t last_syncpoint; // for checking corruption and putting syncpoints, also for back_ptr
	off_t last_headers; // for header repetition and state for demuxer

	off_t before_seek; // position before any seek mess
	off_t seek_status;

	syncpoint_list_t syncpoints;

	// debug
	int sync_overhead;
};

static const struct { char * name, * type; } info_table [] = {
        {NULL                   ,  NULL }, // end
        {NULL                   ,  NULL },
        {NULL                   , "UTF8"},
        {NULL                   , "v"},
        {NULL                   , "s"},
        {"StreamId"             , "v"},
        {"Author"               , "UTF8"},
        {"Title"                , "UTF8"},
        {"Language"             , "UTF8"},
        {"Description"          , "UTF8"},
        {"Copyright"            , "UTF8"},
        {"Encoder"              , "UTF8"},
        {"Keyword"              , "UTF8"},
        {"Cover"                , "JPEG"},
        {"Cover"                , "PNG"},
        {"Disposition"          , "UTF8"},
};

static inline uint32_t adler32(uint8_t * buf, int len) {
	unsigned long a = 1, b = 0;
	int k;
	while (len > 0) {
		k = MIN(len, 5552);
		len -= k;
		while (k--) b += (a += *buf++);
		a %= 65521;
		b %= 65521;
	}
	return (b << 16) | a;
}

static inline uint64_t convert_ts(uint64_t sn, nut_timebase_t from, nut_timebase_t to) {
	uint64_t ln, d1, d2;
	ln = (uint64_t)from.nom * to.den;
	d1 = from.den;
	d2 = to.nom;
	return (ln / d1 * sn + (ln%d1) * sn / d1) / d2;
}

static inline int get_dts(int d, uint64_t * pts_cache, int pts) {
	while (d--) {
		int64_t t = pts_cache[d];
		if (t < pts) {
			pts_cache[d] = pts;
			pts = t;
		}
	}
	return pts;
}

#define bctello(bc) ((bc)->file_pos + ((bc)->buf_ptr - (bc)->buf))

#endif // _NUT_PRIV_H
