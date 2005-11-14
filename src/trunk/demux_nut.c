#include <stdio.h>

#include "config.h"
#include "mp_msg.h"

#include "stream.h"
#include "demuxer.h"
#include "stheader.h"

#define USE_LIBNUT
#ifdef USE_LIBNUT

#include "nut.h"

typedef struct {
	int max_pts; // FIXME
	int last_pts; // FIXME
	nut_context_t * nut;
} nut_priv_t;

static size_t mp_read(void * h, size_t len, uint8_t * buf) {
	stream_t * stream = (stream_t*)h;

	if(stream_eof(stream)) return 0;

	return stream_read(stream, buf, len);
}

static off_t mp_seek(void * h, long pos, int whence) {
	stream_t * stream = (stream_t*)h;

	if (whence == SEEK_CUR) pos += stream_tell(stream);
	else if (whence == SEEK_END) pos += stream->end_pos;
	else if (whence != SEEK_SET) return -1;

	if (pos < stream->end_pos && stream->eof) stream_reset(stream);
	if (stream_seek(stream, pos) == 0) return -1;

	return pos;
}

static demuxer_t * demux_open_nut(demuxer_t * demuxer) {
	nut_demuxer_opts_t dopts = {
		.input = {
			.priv = demuxer->stream,
			.read = mp_read,
			.seek = mp_seek,
			.eof = NULL,
		},
		.read_index = 1
	};
	nut_priv_t * priv = demuxer->priv = calloc(1, sizeof(nut_priv_t));
	nut_context_t * nut = priv->nut = nut_demuxer_init(&dopts);
	nut_packet_t pd;
	nut_stream_header_t * s;
	int ret;
	int i;

	while ((ret = nut_read_next_packet(nut, &pd))) {
		if (ret < 0) mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret));
		if (ret == 1) break;
	}
	if (ret || pd.type != e_headers)  {
		nut_demuxer_uninit(nut);
		free(priv);
		return NULL;
	}

	if ((ret = nut_read_headers(nut, &pd,  &s))) {
		if (ret < 0) mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret));
		nut_demuxer_uninit(nut);
		free(priv);
		return NULL;
	}

	for (i = 0; s[i].type != -1; i++) switch(s[i].type) {
		case NUT_AUDIO_CLASS: {
			WAVEFORMATEX *wf= calloc(sizeof(WAVEFORMATEX) + s[i].codec_specific_len, 1);
			sh_audio_t* sh_audio=new_sh_audio(demuxer, i);

			sh_audio->wf= wf; sh_audio->ds = demuxer->audio;
			sh_audio->audio.dwSampleSize = 0; // FIXME
			sh_audio->audio.dwScale = s[i].time_base_nom;
			sh_audio->audio.dwRate = s[i].time_base_denom;
			sh_audio->format = *(uint16_t*)s[i].fourcc; // FIXME
			sh_audio->channels = s[i].channel_count;
			sh_audio->samplerate = s[i].samplerate_nom / s[i].samplerate_denom;
			sh_audio->i_bps = 0; // FIXME

			wf->wFormatTag = *(uint16_t*)s[i].fourcc; // FIXME
			wf->nChannels = s[i].channel_count;
			wf->nSamplesPerSec = s[i].samplerate_nom / s[i].samplerate_denom;
			wf->nAvgBytesPerSec = 0; // FIXME
			wf->nBlockAlign = 0; // FIXME
			wf->wBitsPerSample = 0; // FIXME
			wf->cbSize = s[i].codec_specific_len;
			if (s[i].codec_specific_len)
				memcpy(wf + 1, s[i].codec_specific, s[i].codec_specific_len);

			demuxer->audio->id = i;
			demuxer->audio->sh= demuxer->a_streams[i];
			break;
		}
		case NUT_VIDEO_CLASS: {
			BITMAPINFOHEADER * bih = calloc(sizeof(BITMAPINFOHEADER) + s[i].codec_specific_len, 1);
			sh_video_t * sh_video = new_sh_video(demuxer, i);

			priv->max_pts = s[i].max_pts;

			sh_video->bih = bih;
			sh_video->ds = demuxer->video;
			sh_video->disp_w = s[i].width;
			sh_video->disp_h = s[i].height;
			sh_video->video.dwScale = s[i].time_base_nom;
			sh_video->video.dwRate  = s[i].time_base_denom;

			sh_video->fps=(float)sh_video->video.dwRate/(float)sh_video->video.dwScale;
			sh_video->frametime=(float)sh_video->video.dwScale/(float)sh_video->video.dwRate;
			sh_video->format = *(uint32_t*)s[i].fourcc; // FIXME
			if (!s[i].sample_height) sh_video->aspect = 0;
			else sh_video->aspect = (float)s[i].sample_width / s[i].sample_height;
			sh_video->i_bps = 0; // FIXME

			bih->biSize = sizeof(BITMAPINFOHEADER) + s[i].codec_specific_len;
			bih->biWidth = s[i].width;
			bih->biHeight = s[i].height;
			bih->biBitCount = 0; // FIXME
			bih->biSizeImage = 0; // FIXME
			bih->biCompression = *(uint32_t*)s[i].fourcc; // FIXME

			if (s[i].codec_specific_len)
				memcpy(bih + 1, s[i].codec_specific, s[i].codec_specific_len);

			demuxer->video->id = i;
			demuxer->video->sh = demuxer->v_streams[i];
			break;
		}
	}
	free(s);

	return demuxer;
}

static int demux_nut_fill_buffer(demuxer_t * demuxer, demux_stream_t * dsds) {
	nut_context_t * nut = ((nut_priv_t*)demuxer->priv)->nut;
	demux_packet_t *dp;
	demux_stream_t *ds;
	nut_packet_t pd;
	int ret;
	double time_base;

	demuxer->filepos = stream_tell(demuxer->stream);
	if (stream_eof(demuxer->stream)) return 0;

	while (1) {
		ret = nut_read_next_packet(nut, &pd);
		if (ret < 0) { mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret)); continue; }
		if (ret == 1) return 0; // EOF
		if (pd.type == e_frame) break;
		// else, skip this packet
		while ((ret = nut_skip_packet(nut, &pd.len))) {
			if (ret < 0) {
				mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret));
				break;
			}
			if (ret == 1) return 0; // EOF
		}
	}

	if (pd.stream == demuxer->audio->id) {
		sh_audio_t * sh_audio = demuxer->audio->sh;
		time_base = (double)sh_audio->audio.dwScale / sh_audio->audio.dwRate;
		ds = demuxer->audio;
	}
	else if(pd.stream == demuxer->video->id) {
		sh_video_t * sh_video = demuxer->video->sh;
		time_base = (double)sh_video->video.dwScale / sh_video->video.dwRate;
		ds = demuxer->video;
		((nut_priv_t*)demuxer->priv)->last_pts = pd.pts;
	}
	else {
		while ((ret = nut_skip_packet(nut, &pd.len))) {
			if (ret < 0) {
				mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret));
				break;
			}
			if (ret == 1) return 0; // EOF
		}
		return 1;
	}

	dp = new_demux_packet(pd.len);

	dp->pts = pd.pts * time_base;

	dp->pos = demuxer->filepos;
	dp->flags= pd.is_key;

	while ((ret = nut_read_frame(nut, &pd.len, dp->buffer))) {
		if (ret < 0) {
			mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret));
			break;
		}
		if (ret == 1) return 0; // EOF
	}
	ds_add_packet(ds, dp); // append packet to DS stream
	return 1;
}

static void demux_seek_nut(demuxer_t * demuxer, float time_pos, int flags) {
	nut_context_t * nut = ((nut_priv_t*)demuxer->priv)->nut;
	sh_video_t * sh_video = demuxer->video->sh;
	int nutflags = 0;
	int ret;

	if (!(flags & 1)) nutflags |= 1; // relative
	if (time_pos < 0) nutflags |= 2; // backwards

	if (flags & 2) // percent
		time_pos *= ((nut_priv_t*)demuxer->priv)->max_pts *
				(double)sh_video->video.dwScale / sh_video->video.dwRate;

	ret = nut_seek(nut, time_pos, nutflags, NULL);
	if (ret < 0) mp_msg(MSGT_HEADER, MSGL_ERR, "NUT error: %s\n", nut_error(-ret));
}

static int demux_control_nut(demuxer_t * demuxer, int cmd, void * arg) {
	nut_priv_t * priv = demuxer->priv;
	sh_video_t * sh_video = demuxer->video->sh;
	switch (cmd) {
		case DEMUXER_CTRL_GET_TIME_LENGTH:
			if (priv->max_pts == 0) return DEMUXER_CTRL_DONTKNOW;

			*((double *)arg) = priv->max_pts *
				(double)sh_video->video.dwScale / sh_video->video.dwRate;

			return DEMUXER_CTRL_OK;
		case DEMUXER_CTRL_GET_PERCENT_POS:
			if (priv->max_pts == 0) return DEMUXER_CTRL_DONTKNOW;
			*((int *)arg) = priv->last_pts * 100 / (double)priv->max_pts;
			return DEMUXER_CTRL_OK;
		default:
			return DEMUXER_CTRL_NOTIMPL;
	}
}

static void demux_close_nut(demuxer_t *demuxer) {
	nut_context_t * nut = ((nut_priv_t*)demuxer->priv)->nut;
	nut_demuxer_uninit(nut);
	free(demuxer->priv);
	demuxer->priv = NULL;
}


demuxer_desc_t demuxer_desc_nut = {
	"NUT demuxer",
	"nut",
	"libnut",
	"Oded Shimon (ods15)",
	"NUT demuxer, requires libnut",
	DEMUXER_TYPE_NUT,
	0, // Check after other demuxer
	NULL, // check
	demux_nut_fill_buffer,
	demux_open_nut,
	demux_close_nut,
	demux_seek_nut,
	demux_control_nut
};

#endif // USE_LIBNUT

