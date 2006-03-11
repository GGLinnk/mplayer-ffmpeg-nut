#define N 0


#include "nutmerge.h"
#include <string.h>

#define mmioFOURCC(ch0, ch1, ch2, ch3) ((ch0) | ((ch1) << 8) | ((ch2) << 16) | ((ch3) << 24))
#define strFOURCC(str) mmioFOURCC((str)[0], (str)[1], (str)[2], (str)[3])

#ifdef WORDS_BIGENDIAN
#define FIXENDIAN32(a) do { \
	(a) = (((a) & 0xFF00FF00) >> 8)  | (((a) & 0x00FF00FF) << 8); \
	(a) = (((a) & 0xFFFF0000) >> 16) | (((a) & 0x0000FFFF) << 16); \
	} while(0)
#define FIXENDIAN16(a) \
	(a) = (((a) & 0xFF00) >> 8)  | (((a) & 0x00FF) << 8)
#else
#define FIXENDIAN32(a) do{}while(0)
#define FIXENDIAN16(a) do{}while(0)
#endif

#define FREAD(file, len, var) do { if (fread((var), 1, (len), (file)) != (len)) return 1; }while(0)

typedef struct riff_tree_s {
	uint32_t len;
	char name[4];
	char listname[4];
	int type; // 0 - list/tree, 1 - node
	int amount; // if a list, amount of nodes
	struct riff_tree_s * tree; // this is an array (size is 'amount')
	char * data;
	int offset;
} riff_tree_t;

typedef struct {
	int amount;
	riff_tree_t * tree;
} full_riff_tree_t;

typedef struct  __attribute__((packed)) {
	uint8_t wFormatTag[2];
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WAVEFORMATEX;

typedef struct  __attribute__((packed)) {
	uint32_t biSize;
	uint32_t biWidth;
	uint32_t biHeight;
	uint16_t biPlanes;
	uint16_t biBitCount;
	uint8_t biCompression[4];
	uint32_t biSizeImage;
	uint32_t biXPelsPerMeter;
	uint32_t biYPelsPerMeter;
	uint32_t biClrUsed;
	uint32_t biClrImportant;
} BITMAPINFOHEADER;

typedef struct  __attribute__((packed)) {
	uint32_t dwMicroSecPerFrame;
	uint32_t dwMaxBytesPerSec;
	uint32_t dwReserved1;
	uint32_t dwFlags;
	uint32_t dwTotalFrames;
	uint32_t dwInitialFrames;
	uint32_t dwStreams;
	uint32_t dwSuggestedBufferSize;
	uint32_t dwWidth;
	uint32_t dwHeight;
	uint32_t dwScale;
	uint32_t dwRate;
	uint32_t dwStart;
	uint32_t dwLength;
} MainAVIHeader;

typedef struct  __attribute__((packed)) {
	uint8_t fccType[4];
	uint8_t fccHandler[4];
	uint32_t dwFlags;
	uint32_t dwReserved1;
	uint32_t dwInitialFrames;
	uint32_t dwScale;
	uint32_t dwRate;
	uint32_t dwStart;
	uint32_t dwLength;
	uint32_t dwSuggestedBufferSize;
	uint32_t dwQuality;
	uint32_t dwSampleSize;
	uint16_t rcframe[4];
} AVIStreamHeader;

typedef struct __attribute__((packed)) {
	uint8_t ckid[4];
	uint32_t dwFlags;
	uint32_t dwChunkOffset;
	uint32_t dwChunkLength;
} AVIINDEXENTRY;

typedef struct {
	int type; // 0 video, 1 audio
	AVIStreamHeader * strh; // these are all pointers to data
	BITMAPINFOHEADER * video;
	WAVEFORMATEX * audio;
	int extra_len;
	int last_pts;
	uint8_t * extra;
} AVIStreamContext;

typedef struct {
	full_riff_tree_t * riff;
	MainAVIHeader * avih;
	AVIStreamContext * stream; // this is an array, free this
	AVIINDEXENTRY * index; // this is an array and data
	int packets;
	FILE * in;
	int cur;
	uint8_t * buf;
} AVIContext;

static int mk_riff_tree(FILE * in, riff_tree_t * tree) {
	int left;
	tree->tree = NULL;
	tree->data = NULL;
	tree->amount = 0;
	tree->offset = ftell(in);
	FREAD(in, 4, tree->name);
	FREAD(in, 4, &tree->len);
	FIXENDIAN32(tree->len);
	left = tree->len;

	switch(strFOURCC(tree->name)) {
		case mmioFOURCC('L','I','S','T'):
		case mmioFOURCC('R','I','F','F'):
			tree->type = 0;
			FREAD(in, 4, tree->listname); left -= 4; // read real name
			if (!strncmp(tree->listname, "movi", 4)) {
				fseek(in, left, SEEK_CUR);
				break;
			}
			while (left > 0) {
				int err;
				tree->tree =
					realloc(tree->tree, sizeof(riff_tree_t) * (tree->amount+1));
				if ((err = mk_riff_tree(in, &tree->tree[tree->amount])))
					return err;
				left -= (tree->tree[tree->amount].len + 8);
				if (tree->tree[tree->amount].len & 1) left--;
				tree->amount++;
			}
			break;
		default:
			tree->type = 1;
			tree->data = malloc(left);
			FREAD(in, left, tree->data);
	}
	if (tree->len & 1) fgetc(in);
	return 0;
}

static void free_riff_tree(riff_tree_t * tree) {
	int i;
	if (!tree) return;

	for (i = 0; i < tree->amount; i++) free_riff_tree(&tree->tree[i]);
	tree->amount = 0;

	free(tree->tree); tree->tree = NULL;
	free(tree->data); tree->data = NULL;
}

static full_riff_tree_t * init_riff() {
	full_riff_tree_t * full = malloc(sizeof(full_riff_tree_t));
	full->amount = 0;
	full->tree = NULL;
	return full;
}

static int get_full_riff_tree(FILE * in, full_riff_tree_t * full) {
	int err = 0;

	while (1) {
		int c;
		if ((c = fgetc(in)) == EOF) break; ungetc(c, in);
		full->tree = realloc(full->tree, sizeof(riff_tree_t) * ++full->amount);
		if ((err = mk_riff_tree(in, &full->tree[full->amount - 1]))) goto err_out;
	}
err_out:
	return err;
}

static void uninit_riff(full_riff_tree_t * full) {
	int i;
	if (!full) return;
	for (i = 0; i < full->amount; i++) free_riff_tree(&full->tree[i]);
	free(full->tree);
	free(full);
}

static int avi_read_stream_header(AVIStreamContext * stream, riff_tree_t * tree) {
	int i, j;
	assert(tree->type == 0);
	assert(strFOURCC(tree->listname) == mmioFOURCC('s','t','r','l'));

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "strh", 4)) {
			if (tree->tree[i].len != 56) return 2;
			stream->strh = (AVIStreamHeader*)tree->tree[i].data;
			break;
		}
	}
	if (i == tree->amount) return 3;

	for(i = 2; i < 12; i++) FIXENDIAN32(((uint32_t*)stream->strh)[i]);

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "strf", 4)) {
			int len = tree->tree[i].len;
			switch(strFOURCC(stream->strh->fccType)) {
				case mmioFOURCC('v','i','d','s'):
					if (len < 40) return 4;
					stream->type = 0;
					stream->video = (BITMAPINFOHEADER*)tree->tree[i].data;
					for(j = 0; j < 3; j++)  FIXENDIAN32(((uint32_t*)stream->video)[j]);
					for(j = 6; j < 8; j++)  FIXENDIAN16(((uint16_t*)stream->video)[j]);
					for(j = 5; j < 10; j++) FIXENDIAN32(((uint32_t*)stream->video)[j]);
					stream->extra_len = len - 40;
					if (len > 40) stream->extra = (uint8_t*)tree->tree[i].data + 40;
					break;
				case mmioFOURCC('a','u','d','s'):
					if (len < 18) return 5;
					stream->type = 1;
					stream->audio = (WAVEFORMATEX *)tree->tree[i].data;
					for(j = 1; j < 2; j++) FIXENDIAN16(((uint16_t*)stream->audio)[j]);
					for(j = 1; j < 3; j++) FIXENDIAN32(((uint32_t*)stream->audio)[j]);
					for(j = 6; j < 9; j++) FIXENDIAN16(((uint16_t*)stream->audio)[j]);
					stream->extra_len = len - 18;
					if (len > 18) stream->extra = (uint8_t*)tree->tree[i].data + 18;
					break;
				default:
					return 6;
			}
			break;
		}
	}
	if (i == tree->amount) return 7;

	return 0;
}

static int avi_read_main_header(AVIContext * avi, const riff_tree_t * tree) {
	int i, tmp = 0, err;
	assert(tree->type == 0);
	assert(strFOURCC(tree->listname) == mmioFOURCC('h','d','r','l'));

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "avih", 4)) {
			if (tree->tree[i].len != 56) return 8;
			avi->avih = (MainAVIHeader*)tree->tree[i].data;
			break;
		}
	}
	if (i == tree->amount) return 9;

	for(i = 0; i < 14; i++) FIXENDIAN32(((uint32_t*)avi->avih)[i]);

	if (avi->avih->dwStreams > 200) return 10;
	avi->stream = malloc(avi->avih->dwStreams * sizeof(AVIStreamContext));
	for (i = 0; i < avi->avih->dwStreams; i++) {
		avi->stream[i].video = NULL;
		avi->stream[i].audio = NULL;
		avi->stream[i].extra = NULL;
		avi->stream[i].extra_len = 0;
		avi->stream[i].last_pts = 0;
	}
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "strl", 4)) {
			if ((err = avi_read_stream_header(&avi->stream[tmp++], &tree->tree[i]))) return err;
		}
	}
	if (tmp != avi->avih->dwStreams) return 11;
	return 0;
}

static int avi_read_headers(AVIContext * avi) {
	const riff_tree_t * tree;
	int i, err;
	if ((err = get_full_riff_tree(avi->in, avi->riff))) return err;
	tree = &avi->riff->tree[0];
	if (tree->type != 0) return 12;
	if (strncmp(tree->name, "RIFF", 4)) return 13;
	if (strncmp(tree->listname, "AVI ", 4)) return 14;
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "hdrl", 4)) {
			if ((err = avi_read_main_header(avi, &tree->tree[i]))) return err;
			break;
		}
	}
	if (i == tree->amount) return 15;
	for (i = 0; i < avi->riff->amount; i++) {
		int j;
		tree = &avi->riff->tree[i];
		for (j = 0; j < tree->amount; j++) {
			if (tree->tree[j].type == 1 && !strncmp(tree->tree[j].name, "idx1", 4)) {
				avi->index = (AVIINDEXENTRY *)tree->tree[j].data;
				avi->packets = tree->tree[j].len / 16;
				for (i = 0; i < avi->packets; i++) {
					FIXENDIAN32(avi->index[i].dwFlags);
					FIXENDIAN32(avi->index[i].dwChunkOffset);
					FIXENDIAN32(avi->index[i].dwChunkLength);
				}
				break;
			}
		}
		if (j != tree->amount) break;
	}
	if (i == avi->riff->amount) return 16;
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "movi", 4)) {
			fseek(avi->in, tree->tree[i].offset + 12, SEEK_SET);
			break;
		}
	}
	if (i == tree->amount) return 17;
	return 0;
}

static void * init(FILE * in) {
	AVIContext * avi = malloc(sizeof(AVIContext));
	avi->avih = NULL;
	avi->stream = NULL;
	avi->index = NULL;
	avi->in = in;
	avi->riff = init_riff();
	avi->cur = 0;
	avi->buf = NULL;
	return avi;
}

static void uninit(void * priv) {
	AVIContext * avi = priv;
	if (!avi) return;

	uninit_riff(avi->riff);
	free(avi->stream);
	free(avi->buf);
	free(avi);
}

static int read_headers(void * priv, nut_stream_header_t ** nut_streams) {
	AVIContext * avi = priv;
	nut_stream_header_t * s;
	int i;
	if ((i = avi_read_headers(avi))) return i;
	*nut_streams = s = malloc(sizeof(nut_stream_header_t) * (avi->avih->dwStreams + 1 + N));
	for (i = 0; i < avi->avih->dwStreams; i++) {
		s[i].type = avi->stream[i].type;
		s[i].timebase.den = avi->stream[i].strh->dwRate;
		s[i].timebase.nom = avi->stream[i].strh->dwScale;
		s[i].fixed_fps = 1;
		s[i].decode_delay = !i; // FIXME
		s[i].codec_specific_len = avi->stream[i].extra_len;
		s[i].codec_specific = avi->stream[i].extra;
		if (avi->stream[i].type == 0) { // video
			s[i].fourcc_len = 4;
			s[i].fourcc = avi->stream[i].video->biCompression;

			s[i].width = avi->stream[i].video->biWidth;
			s[i].height = avi->stream[i].video->biHeight;
			s[i].sample_width = 0;
			s[i].sample_height = 0;
			s[i].colorspace_type = 0;
		} else { // audio
			s[i].fourcc_len = 2;
			s[i].fourcc = avi->stream[i].audio->wFormatTag;

			s[i].samplerate_nom = 1;
			s[i].samplerate_denom = avi->stream[i].audio->nSamplesPerSec;
			s[i].channel_count = avi->stream[i].audio->nChannels;
		}
	}
	while (i < N + 2) s[i++] = s[0];
	s[i].type = -1;
	return 0;
}

static int find_frame_type(FILE * in, int len, int * type) {
	uint8_t buf[len];
	int i;
	if (!len) { *type = 1; return 0; }
	FREAD(in, len, buf);
	fseek(in, -len, SEEK_CUR);
	for (i = 0; i < len; i++) {
		if (buf[i] != 0xB6) continue;

		if (i == len - 1) return 11;
		*type = buf[i+1] >> 6;
		return 0;
	}
	return 13;
}

static int get_packet(void * priv, nut_packet_t * p, uint8_t ** buf) {
	AVIContext * avi = priv;
	char fourcc[4];
	int err = 0;
	int s; // stream
	uint32_t len;
	if (ftell(avi->in) & 1) fgetc(avi->in);

	if (avi->cur >= avi->packets) return -1;

	if ((avi->stream[0].last_pts % 1000) < N && avi->buf) {
		p->next_pts = 0;
		p->len = 5;
		p->flags = NUT_FLAG_KEY;
		p->stream = 2;//2 + (avi->stream[0].last_pts % 100);
		p->pts = avi->stream[0].last_pts;
		if (avi->stream[0].last_pts % 1000) p->flags |= NUT_FLAG_EOR;
		*buf = (void*)avi;
		free(avi->buf);
		avi->buf = NULL;
		return 0;
	}

	FREAD(avi->in, 4, fourcc);
	FREAD(avi->in, 4, &len);
	FIXENDIAN32(len);
	p->next_pts = 0;
	p->len = len;
	p->flags = (avi->index[avi->cur++].dwFlags & 0x10) ? NUT_FLAG_KEY : 0;
	p->stream = s = (fourcc[0] - '0') * 10 + (fourcc[1] - '0');
	if (s == 0) { // 1 frame of video
		int type;
		p->pts = avi->stream[0].last_pts++; // FIXME
		if ((err = find_frame_type(avi->in, len, &type))) return err;
		if (stats) fprintf(stats, "%c", type==0?'I':type==1?'P':type==2?'B':'S');
		switch (type) {
			case 0: // I
				if (!(p->flags & NUT_FLAG_KEY)) printf("Error detected stream %d frame %d\n", s, (int)p->pts);
				p->flags |= NUT_FLAG_KEY;
				break;
			case 3: // S
				printf("S-Frame %d\n", (int)ftell(avi->in));
				//err = 12;
				//goto err_out;
				// FALL THROUGH!
			case 1: { // P
				off_t where = ftell(avi->in);
				while (fourcc[0] != 'i') {
					len += len & 1; // round up
					fseek(avi->in, len, SEEK_CUR);
					FREAD(avi->in, 4, fourcc);
					FREAD(avi->in, 4, &len);
					FIXENDIAN32(len);
					if ((fourcc[0] - '0') * 10 + (fourcc[1] - '0') != 0) continue;
					if ((err = find_frame_type(avi->in, len, &type))) goto err_out;
					if (type != 2) break;
					p->pts++;
				}
				fseek(avi->in, where, SEEK_SET);
				break;
			}
			case 2: // B
				p->pts--;
				break;
		}
	} else if (s < avi->avih->dwStreams) { // 0.5 secs of audio or a single packet
		int samplesize = avi->stream[s].strh->dwSampleSize;

		p->pts = avi->stream[s].last_pts;
		if (samplesize) avi->stream[s].last_pts += p->len / samplesize;
		else avi->stream[s].last_pts++;

		if (!(p->flags & NUT_FLAG_KEY)) printf("Error detected stream %d frame %d\n", s, (int)p->pts);
		p->flags |= NUT_FLAG_KEY;
	} else {
		printf("%d %4.4s\n", avi->cur, fourcc);
		err = 10;
		goto err_out;
	}
	*buf = avi->buf = realloc(avi->buf, p->len);
	FREAD(avi->in, p->len, *buf);
err_out:
	return err;
}

struct demuxer_t avi_demuxer = {
	"avi",
	init,
	read_headers,
	get_packet,
	uninit
};

#ifdef RIFF_PROG
void print_riff_tree(riff_tree_t * tree, int indent) {
	char ind[indent + 1];
	int i;
	memset(ind, ' ', indent);
	ind[indent] = 0;

	if (tree->type == 0) {
		printf("%s%4.4s: offset: %d name: `%4.4s', len: %u (amount: %d)\n",
			ind, tree->name, tree->offset, tree->listname, tree->len, tree->amount);
		for (i = 0; i < tree->amount; i++) {
			print_riff_tree(&tree->tree[i], indent + 4);
		}
	} else {
		printf("%sDATA: offset: %d name: `%4.4s', len: %u\n",
			ind, tree->offset, tree->name, tree->len);
	}
}

FILE * stats = NULL;
int main(int argc, char * argv []) {
	FILE * in;
	full_riff_tree_t * full = init_riff();
	int err = 0;
	int i;
	if (argc < 2) { printf("bleh, more params you fool...\n"); return 1; }
	in = fopen(argv[1], "r");

	if ((err = get_full_riff_tree(in, full))) goto err_out;
	for (i = 0; i < full->amount; i++) {
		print_riff_tree(&full->tree[i], 0);
	}

err_out:
	uninit_riff(full);
	fclose(in);
	return err;
}
#endif

#ifdef AVI_PROG
FILE * stats = NULL;
int main(int argc, char * argv []) {
	FILE * in;
	AVIContext * avi = NULL;
	int err = 0;
	int i;
	if (argc < 2) { printf("bleh, more params you fool...\n"); return 1; }

	in = fopen(argv[1], "r");
	avi = init(in);

	if ((err = avi_read_headers(avi))) goto err_out;

	printf("Main AVI Header:\n");
	printf("dwMicroSecPerFrame: %u\n", avi->avih->dwMicroSecPerFrame);
	printf("dwMaxBytesPerSec: %u\n", avi->avih->dwMaxBytesPerSec);
	printf("dwReserved1: %u\n", avi->avih->dwReserved1);
	printf("dwFlags: %u\n", avi->avih->dwFlags);
	printf("dwTotalFrames: %u\n", avi->avih->dwTotalFrames);
	printf("dwInitialFrames: %u\n", avi->avih->dwInitialFrames);
	printf("dwStreams: %u\n", avi->avih->dwStreams);
	printf("dwSuggestedBufferSize: %u\n", avi->avih->dwSuggestedBufferSize);
	printf("dwWidth: %u\n", avi->avih->dwWidth);
	printf("dwHeight: %u\n", avi->avih->dwHeight);
	printf("dwScale: %u\n", avi->avih->dwScale);
	printf("dwRate: %u\n", avi->avih->dwRate);
	printf("dwStart: %u\n", avi->avih->dwStart);
	printf("dwLength: %u\n", avi->avih->dwLength);

	for (i = 0; i < avi->avih->dwStreams; i++) {
		printf("\n");
		printf("Stream header number %d\n", i);

		printf(" fccType: %.4s\n", avi->stream[i].strh->fccType);
		printf(" fccHandler: %.4s\n", avi->stream[i].strh->fccHandler);

		printf(" dwFlags: %u\n", avi->stream[i].strh->dwFlags);
		printf(" dwReserved1: %u\n", avi->stream[i].strh->dwReserved1);
		printf(" dwInitialFrames: %u\n", avi->stream[i].strh->dwInitialFrames);
		printf(" dwScale: %u\n", avi->stream[i].strh->dwScale);
		printf(" dwRate: %u\n", avi->stream[i].strh->dwRate);
		printf(" dwStart: %u\n", avi->stream[i].strh->dwStart);
		printf(" dwLength: %u\n", avi->stream[i].strh->dwLength);
		printf(" dwSuggestedBufferSize: %u\n", avi->stream[i].strh->dwSuggestedBufferSize);
		printf(" dwQuality: %u\n", avi->stream[i].strh->dwQuality);
		printf(" dwSampleSize: %u\n", avi->stream[i].strh->dwSampleSize);

		printf(" rcframe: %u %u %u %u\n",
			avi->stream[i].strh->rcframe[0], avi->stream[i].strh->rcframe[1],
			avi->stream[i].strh->rcframe[2], avi->stream[i].strh->rcframe[3]);

		if (avi->stream[i].type == 0) { // video
			printf(" video:\n");
			printf("  biSize: %u\n", avi->stream[i].video->biSize);
			printf("  biWidth: %u\n", avi->stream[i].video->biWidth);
			printf("  biHeight: %u\n", avi->stream[i].video->biHeight);
			printf("  biPlanes: %u\n", avi->stream[i].video->biPlanes);
			printf("  biBitCount: %u\n", avi->stream[i].video->biBitCount);

			printf("  biCompression: %.4s\n", avi->stream[i].video->biCompression);

			printf("  biSizeImage: %u\n", avi->stream[i].video->biSizeImage);
			printf("  biXPelsPerMeter: %u\n", avi->stream[i].video->biXPelsPerMeter);
			printf("  biYPelsPerMeter: %u\n", avi->stream[i].video->biYPelsPerMeter);
			printf("  biClrUsed: %u\n", avi->stream[i].video->biClrUsed);
			printf("  biClrImportant: %u\n", avi->stream[i].video->biClrImportant);
		} else {
			printf(" audio:\n");
			printf("  wFormatTag: 0x%04X\n", *(uint16_t*)avi->stream[i].audio->wFormatTag);
			printf("  nChannels: %u\n", avi->stream[i].audio->nChannels);
			printf("  nSamplesPerSec: %u\n", avi->stream[i].audio->nSamplesPerSec);
			printf("  nAvgBytesPerSec: %u\n", avi->stream[i].audio->nAvgBytesPerSec);
			printf("  nBlockAlign: %u\n", avi->stream[i].audio->nBlockAlign);
			printf("  wBitsPerSample: %u\n", avi->stream[i].audio->wBitsPerSample);
			printf("  cbSize: %u\n", avi->stream[i].audio->cbSize);
		}
	}
	printf("\nNum packets: %d\n", avi->packets);

err_out:
	uninit(avi);
	fclose(in);
	return err;
}
#endif
