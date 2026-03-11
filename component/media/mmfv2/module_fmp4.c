/******************************************************************************
*
* Copyright(c) 2007 - 2018 Realtek Corporation. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the License); you may
* not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an AS IS BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
******************************************************************************/
#include <stdint.h>
#include <string.h>
#include "avcodec.h"
#include "mmf2_module.h"
#include "module_fmp4.h"
#include "mov-buffer.h"
#include "mov-format.h"
//------------------------------------------------------------------------------
#define BUFFER_SIZE_BY_BITRATE(n)	(n)*1024*1024/8

static void h264_fmp4_write(void *param, const void *data, int bytes);
static void aac_fmp4_write(void *param, const uint8_t *ptr, int bytes);

static void fmp4_maybe_save_segment(fmp4_ctx_t *ctx, uint32_t pts_ms)
{
	if (!ctx || !ctx->fmp4 || !ctx->wfp || ctx->segment_ms == 0) {
		return;
	}

	// If we're in A/V mode, avoid segmenting until audio track exists,
	// otherwise early segments may be video-only unexpectedly.
	if (ctx->require_audio_track && !ctx->add_audio_track_done) {
		return;
	}

	if (!ctx->have_segment_pts) {
		ctx->last_segment_pts_ms = pts_ms;
		ctx->have_segment_pts = 1;
		return;
	}

	uint32_t dt = pts_ms - ctx->last_segment_pts_ms; // uint32 wrap-safe
	if (dt < ctx->segment_ms) {
		return;
	}

	(void)fmp4_writer_save_segment(ctx->fmp4);
	(void)fflush(ctx->wfp);
	ctx->last_segment_pts_ms = pts_ms;
}

static void fmp4_maybe_write_init_segment(fmp4_ctx_t *ctx)
{
	if (!ctx || !ctx->fmp4 || !ctx->wfp || ctx->segment_ms == 0 || ctx->init_segment_written) {
		return;
	}

	// Only write init segment once we know the track list.
	// - Video-only: after video track exists.
	// - A/V: after both audio and video tracks exist.
	if (!ctx->add_video_track_done) {
		return;
	}
	if (ctx->require_audio_track && !ctx->add_audio_track_done) {
		return;
	}

	(void)fmp4_writer_init_segment(ctx->fmp4);
	(void)fflush(ctx->wfp);
	ctx->init_segment_written = 1;
}

static uint32_t fmp4_normalize_pts_ms(fmp4_ctx_t *ctx, uint32_t raw_pts_ms)
{
	// Timelapse mode already synthesizes timestamps starting at 0.
	if (!ctx || ctx->timelapse_ts_enable) {
		return raw_pts_ms;
	}

	if (!ctx->base_ts_valid) {
		ctx->base_ts_ms = raw_pts_ms;
		ctx->base_ts_valid = 1;
		return 0;
	}

	// Unsigned subtraction keeps behavior wrap-safe for uint32 tick counters.
	return (uint32_t)(raw_pts_ms - ctx->base_ts_ms);
}

static uint32_t fmp4_timelapse_next_pts_ms(fmp4_ctx_t *ctx, uint32_t passthrough_pts_ms)
{
	if (!ctx || !ctx->timelapse_ts_enable || ctx->timelapse_record_fps == 0) {
		return passthrough_pts_ms;
	}

	uint32_t pts = ctx->tl_pts_ms;

	// Keep average cadence at (1000 / fps) ms while spreading rounding error.
	// tl_remainder accumulates "1000" in units of 1ms*fps.
	ctx->tl_remainder += 1000;
	ctx->tl_pts_ms += (ctx->tl_remainder / ctx->timelapse_record_fps);
	ctx->tl_remainder = (ctx->tl_remainder % ctx->timelapse_record_fps);

	return pts;
}

int fmp4_handle(void *p, void *input, void *output)
{
	int ret = 0;
	fmp4_ctx_t *ctx = (fmp4_ctx_t *)p;
	mm_queue_item_t *input_item = (mm_queue_item_t *)input;

	if (input_item->type == AV_CODEC_ID_H264) {
		ctx->mov_h264_ctx.ptr = (uint8_t *)input_item->data_addr;
		uint32_t in_ts = (uint32_t)input_item->timestamp;
		uint32_t pts = fmp4_timelapse_next_pts_ms(ctx, in_ts);
		pts = fmp4_normalize_pts_ms(ctx, pts);
		ctx->mov_h264_ctx.pts = pts;
		ctx->mov_h264_ctx.dts = pts;
		//printf("\r\nVideo timestamp = %d", ctx->mov_h264_ctx.pts);
		h264_fmp4_write(ctx, (uint8_t *)input_item->data_addr, input_item->size);
	} else if (input_item->type == AV_CODEC_ID_MP4A_LATM) {
		uint32_t pts = (uint32_t)input_item->timestamp;
		pts = fmp4_normalize_pts_ms(ctx, pts);
		ctx->mov_aac_ctx.pts = pts;
		//printf("\r\nAudio timestamp = %d", ctx->mov_aac_ctx.pts);
		aac_fmp4_write(ctx, (uint8_t *)input_item->data_addr, input_item->size);
	}
	return ret;
}

static void h264_fmp4_write(void *param, const void *data, int bytes)
{
	fmp4_ctx_t *ctx = (fmp4_ctx_t *)param;

	int vcl = 0;
	int update = 0;
	int n = h264_annexbtomp4(&ctx->mov_h264_ctx.avc, data, bytes, ctx->s_buffer, ctx->s_buffer_len, &vcl, &update);

	if (ctx->mov_h264_ctx.track < 0) {
		if (ctx->mov_h264_ctx.avc.nb_sps < 1 || ctx->mov_h264_ctx.avc.nb_pps < 1) {
			//ctx->ptr = end;
			printf("waiting for sps/pps\r\n");
			return;
		}

		int extra_data_size = mpeg4_avc_decoder_configuration_record_save(&ctx->mov_h264_ctx.avc, ctx->s_extra_data, ctx->s_extra_data_len);
		if (extra_data_size <= 0) {
			// invalid AVCC
			printf("error: invalid AVCC\r\n");
			return;
		}

		// TODO: waiting for key frame ???
		ctx->mov_h264_ctx.track = fmp4_writer_add_video(ctx->fmp4, MOV_OBJECT_H264, ctx->mov_h264_ctx.width, ctx->mov_h264_ctx.height, ctx->s_extra_data,
								  extra_data_size);
		if (ctx->mov_h264_ctx.track < 0) {
			printf("error: fmp4 writer add video fail\r\n");
			return;
		}

		ctx->add_video_track_done = 1;
	}

	if (ctx->add_video_track_done && (ctx->add_audio_track_done || !ctx->require_audio_track)) {
		fmp4_maybe_write_init_segment(ctx);
		fmp4_writer_write(ctx->fmp4, ctx->mov_h264_ctx.track, ctx->s_buffer, n, ctx->mov_h264_ctx.pts, ctx->mov_h264_ctx.dts, 1 == vcl ? MOV_AV_FLAG_KEYFREAME : 0);
		fmp4_maybe_save_segment(ctx, (uint32_t)ctx->mov_h264_ctx.pts);
	}

}

static void aac_fmp4_write(void *param, const uint8_t *ptr, int bytes)
{
	fmp4_ctx_t *ctx = (fmp4_ctx_t *)param;

	int rate = 1;
	struct mpeg4_aac_t aac;

	uint8_t *end = (uint8_t *)(ptr + bytes);

	while (ptr + 7 < end) {
		mpeg4_aac_adts_load(ptr, end - ptr, &aac);
		if (-1 == ctx->mov_aac_ctx.track) {
			int extra_data_size = mpeg4_aac_audio_specific_config_save(&aac, ctx->s_extra_data, ctx->s_extra_data_len);
			if (extra_data_size <= 0) {
				printf("error: invalid AAC\r\n");
				return;
			}
			rate = mpeg4_aac_audio_frequency_to((enum mpeg4_aac_frequency)aac.sampling_frequency_index);
			if (rate == 0) {
				printf("error: aac rate is 0\r\n");
				return;
			}
			ctx->mov_aac_ctx.track = fmp4_writer_add_audio(ctx->fmp4, MOV_OBJECT_AAC, aac.channel_configuration, 16, rate, ctx->s_extra_data, extra_data_size);
			if (ctx->mov_aac_ctx.track < 0) {
				printf("error: fmp4 writer add audio fail\r\n");
				return;
			}

			ctx->add_audio_track_done = 1;
		}

		int framelen = ((ptr[3] & 0x03) << 11) | (ptr[4] << 3) | (ptr[5] >> 5);
		if (ctx->add_video_track_done && ctx->add_audio_track_done) {
			fmp4_maybe_write_init_segment(ctx);
			fmp4_writer_write(ctx->fmp4, ctx->mov_aac_ctx.track, ptr + 7, framelen - 7, ctx->mov_aac_ctx.pts, ctx->mov_aac_ctx.pts, 0);
			fmp4_maybe_save_segment(ctx, (uint32_t)ctx->mov_aac_ctx.pts);
		}
		ptr += framelen;
	}

}

static int mov_file_read(void *fp, void *data, uint64_t bytes)
{
	if (bytes == fread(data, 1, bytes, (FILE *)fp)) {
		return 0;
	}
	return 0 != ferror((FILE *)fp) ? ferror((FILE *)fp) : -1 /*EOF*/;
}

static int mov_file_write(void *fp, const void *data, uint64_t bytes)
{
	return bytes == fwrite(data, 1, bytes, (FILE *)fp) ? 0 : ferror((FILE *)fp);
}

static int mov_file_seek(void *fp, uint64_t offset)
{
	return fseek((FILE *)fp, offset, SEEK_SET);
}

static uint64_t mov_file_tell(void *fp)
{
	return ftell((FILE *)fp);
}

const struct mov_buffer_t *mov_file_buffer(void)
{
	static struct mov_buffer_t s_io = {
		mov_file_read,
		mov_file_write,
		mov_file_seek,
		mov_file_tell,
	};
	return &s_io;
}

int fmp4_control(void *p, int cmd, int arg)
{
	fmp4_ctx_t *ctx = (fmp4_ctx_t *)p;

	switch (cmd) {
	case CMD_FMP4_SET_WIDTH:
		ctx->mov_h264_ctx.width = (int)arg;
		break;
	case CMD_FMP4_SET_HEIGHT:
		ctx->mov_h264_ctx.height = (int)arg;
		break;
	case CMD_FMP4_SET_FILENAME:
		memset(ctx->fmp4_ram_filename, 0x00, sizeof(ctx->fmp4_ram_filename));
		if ((char *)arg) {
			strncpy((char *)ctx->fmp4_ram_filename, (char *)arg, sizeof(ctx->fmp4_ram_filename) - 1);
		}
		break;
	case CMD_FMP4_FILE_OPEN:
		ctx->wfp = fopen(ctx->fmp4_ram_filename, "wb+");
		if (ctx->wfp == NULL) {
			printf("Fail to open file\r\n");
			return -1;
		}
		ctx->fmp4 = fmp4_writer_create(mov_file_buffer(), ctx->wfp, MOV_FLAG_FASTSTART | (ctx->segment_ms > 0 ? MOV_FLAG_SEGMENT : 0));

		ctx->mov_h264_ctx.track = -1;
		ctx->mov_aac_ctx.track = -1;

		ctx->add_audio_track_done = 0;
		ctx->add_video_track_done = 0;
		ctx->tl_pts_ms = 0;
		ctx->tl_remainder = 0;
		ctx->base_ts_valid = 0;
		ctx->base_ts_ms = 0;
		ctx->last_segment_pts_ms = 0;
		ctx->have_segment_pts = 0;
		ctx->init_segment_written = 0;
		break;
	case CMD_FMP4_FILE_CLOSE:
		// Finalize current segment (best-effort) before closing (segmented mode only).
		if (ctx->fmp4 && ctx->segment_ms > 0 && ctx->init_segment_written) {
			(void)fmp4_writer_save_segment(ctx->fmp4);
		}
		if (ctx->wfp) {
			(void)fflush(ctx->wfp);
		}
		if (ctx->fmp4) {
			fmp4_writer_destroy(ctx->fmp4);
			ctx->fmp4 = NULL;
		}
		if (ctx->wfp) {
			fclose(ctx->wfp);
			ctx->wfp = NULL;
		}
		ctx->add_audio_track_done = 0;
		ctx->add_video_track_done = 0;
		ctx->have_segment_pts = 0;
		ctx->init_segment_written = 0;
		break;
	case CMD_FMP4_APPLY:

		break;
	case CMD_FMP4_SET_VIDEO_ONLY:
		ctx->require_audio_track = (arg ? 0 : 1);
		break;
	case CMD_FMP4_SET_TIMELAPSE_FPS:
		if (arg <= 0) {
			ctx->timelapse_ts_enable = 0;
			ctx->timelapse_record_fps = 0;
			ctx->tl_pts_ms = 0;
			ctx->tl_remainder = 0;
			ctx->base_ts_valid = 0;
			ctx->base_ts_ms = 0;
		} else {
			ctx->timelapse_ts_enable = 1;
			ctx->timelapse_record_fps = (uint32_t)arg;
			ctx->tl_pts_ms = 0;
			ctx->tl_remainder = 0;
			ctx->base_ts_valid = 0;
			ctx->base_ts_ms = 0;
		}
		break;
	case CMD_FMP4_SET_SEGMENT_MS: {
		// 0 disables segmentation; otherwise clamp to a reasonable range.
		if (arg <= 0) {
			ctx->segment_ms = 0;
		} else {
			uint32_t seg = (uint32_t)arg;
			if (seg < 200) {
				seg = 200;
			} else if (seg > 60000) {
				seg = 60000;
			}
			ctx->segment_ms = seg;
		}
		break;
	}
	default:
		break;
	}

	return 0;
}

void *fmp4_destroy(void *p)
{
	fmp4_ctx_t *ctx = (fmp4_ctx_t *)p;

	if (ctx) {
		if (ctx->s_buffer) {
			free(ctx->s_buffer);
			ctx->s_buffer = NULL;
		}
		if (ctx->s_extra_data) {
			free(ctx->s_extra_data);
			ctx->s_extra_data = NULL;
		}
		if (ctx->wfp) {
			fclose(ctx->wfp);
			ctx->wfp = NULL;
		}
		if (ctx->fmp4) {
			fmp4_writer_destroy(ctx->fmp4);
		}
		free(ctx);
	}
	return NULL;
}

void *fmp4_create(void *parent)
{
	fmp4_ctx_t *ctx = malloc(sizeof(fmp4_ctx_t));
	if (!ctx) {
		goto error;
	}
	memset(ctx, 0, sizeof(fmp4_ctx_t));
	ctx->parent = parent;
	ctx->require_audio_track = 1;
	ctx->timelapse_ts_enable = 0;
	ctx->timelapse_record_fps = 0;
	ctx->tl_pts_ms = 0;
	ctx->tl_remainder = 0;
	ctx->base_ts_valid = 0;
	ctx->base_ts_ms = 0;
	ctx->segment_ms = 0;
	ctx->last_segment_pts_ms = 0;
	ctx->have_segment_pts = 0;
	ctx->init_segment_written = 0;

	ctx->s_buffer_len = BUFFER_SIZE_BY_BITRATE(4);
	ctx->s_buffer = (uint8_t *)malloc(sizeof(uint8_t) * BUFFER_SIZE_BY_BITRATE(4));
	if (ctx->s_buffer == NULL) {
		printf("Fail to allicate memory for s_buffer\r\n");
		goto error;
	}
	ctx->s_extra_data_len = 1024;
	ctx->s_extra_data = (uint8_t *)malloc(ctx->s_extra_data_len);
	if (ctx->s_extra_data == NULL) {
		printf("Fail to allicate memory for s_extra_data\r\n");
		goto error;
	}

	return ctx;

error:
	if (ctx) {
		if (ctx->s_buffer) {
			free(ctx->s_buffer);
			ctx->s_buffer = NULL;
		}
		if (ctx->s_extra_data) {
			free(ctx->s_extra_data);
			ctx->s_extra_data = NULL;
		}
		free(ctx);
	}
	return NULL;
}

mm_module_t fmp4_module = {
	.create = fmp4_create,
	.destroy = fmp4_destroy,
	.control = fmp4_control,
	.handle = fmp4_handle,

	.new_item = NULL,
	.del_item = NULL,

	.output_type = MM_TYPE_NONE,
	.module_type = MM_TYPE_AVSINK,
	.name = "FMP4"
};
