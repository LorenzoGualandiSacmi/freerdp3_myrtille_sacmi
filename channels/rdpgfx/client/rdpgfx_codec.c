/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Graphics Pipeline Extension
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <winpr/crt.h>
#include <winpr/stream.h>
#include <freerdp/log.h>
#include <freerdp/utils/profiler.h>

#include "rdpgfx_common.h"

#include "rdpgfx_codec.h"
#include <freerdp/client.h>
#include <freerdp/gdi/gfx.h>

/* MAV1: magic, surface, origin x/y, width/height, rectangle count, payload size;
 * all fields uint32 little endian, followed by RECT16s and one Annex B frame.
 * Non-output/scaled surfaces keep the native compositor path.
 */
static UINT myrtille_video_packet(RDPGFX_PLUGIN* gfx, RDPGFX_SURFACE_COMMAND* cmd,
                                 const RDPGFX_AVC420_BITMAP_STREAM* h264,
                                 UINT32 kind, BOOL present)
{
	RdpgfxClientContext* ctx = gfx->context;
	gdiGfxSurface* surface = ctx && ctx->GetSurfaceData ? ctx->GetSurfaceData(ctx, (UINT16)cmd->surfaceId) : NULL;
	if (!surface || !surface->outputMapped || surface->windowMapped ||
	    surface->width != surface->outputTargetWidth ||
	    surface->height != surface->outputTargetHeight)
		return CHANNEL_RC_OK; /* Already rendered by the native compositor. */
	const BOOL avc444 = kind != 3;
	const size_t size = (avc444 ? 40ULL : 32ULL) + 8ULL * h264->meta.numRegionRects + h264->length;
	if (size > 32ULL * 1024 * 1024)
		return ERROR_INVALID_DATA;
	wStream* packet = Stream_New(NULL, size);
	if (!packet)
		return CHANNEL_RC_NO_MEMORY;
	Stream_Write_UINT32(packet, avc444 ? 0x3256414D : 0x3156414D); /* MAV2 / MAV1 */
	Stream_Write_UINT32(packet, cmd->surfaceId);
	Stream_Write_UINT32(packet, surface->outputOriginX);
	Stream_Write_UINT32(packet, surface->outputOriginY);
	Stream_Write_UINT32(packet, surface->width);
	Stream_Write_UINT32(packet, surface->height);
	Stream_Write_UINT32(packet, h264->meta.numRegionRects);
	Stream_Write_UINT32(packet, h264->length);
	if (avc444)
	{
		Stream_Write_UINT32(packet, kind); /* 0=luma, 1=chroma v1, 2=chroma v2 */
		Stream_Write_UINT32(packet, present ? 1 : 0);
	}
	for (UINT32 i = 0; i < h264->meta.numRegionRects; i++)
	{
		const RECTANGLE_16* r = &h264->meta.regionRects[i];
		if (r->left >= r->right || r->top >= r->bottom || r->right > surface->width || r->bottom > surface->height)
		{
			Stream_Free(packet, TRUE);
			return ERROR_INVALID_DATA;
		}
		Stream_Write_UINT16(packet, r->left); Stream_Write_UINT16(packet, r->top);
		Stream_Write_UINT16(packet, r->right); Stream_Write_UINT16(packet, r->bottom);
	}
	Stream_Write(packet, h264->data, h264->length);
	/* Explicit local diagnostic capture, bounded per RDP process. The graphics
	 * channel serializes these callbacks. Same framing as avc444-fixture. */
	const char* capturePrefix = getenv("MYRTILLE_VIDEO_CAPTURE");
	if (capturePrefix)
	{
		static size_t capturedBytes = 0;
		static UINT32 capturedPackets = 0;
		const UINT32 length = (UINT32)Stream_GetPosition(packet);
		if (capturedPackets < 256 && capturedBytes + length + 4 <= 64ULL * 1024 * 1024)
		{
			char path[4096];
			const int n = snprintf(path, sizeof(path), "%s-%lu.packets", capturePrefix, GetCurrentProcessId());
			if (n > 0 && (size_t)n < sizeof(path))
			{
				FILE* file = fopen(path, capturedPackets ? "ab" : "wb");
				if (file)
				{
					BYTE header[4] = { length & 255, (length >> 8) & 255,
					                   (length >> 16) & 255, (length >> 24) & 255 };
					const BOOL ok = fwrite(header, 1, 4, file) == 4 &&
					                fwrite(Stream_Buffer(packet), 1, length, file) == length;
					fclose(file);
					if (!ok) capturedPackets = 256;
					else { capturedPackets++; capturedBytes += length + 4; }
				}
			}
		}
	}
	RDP_CLIENT_ENTRY_POINTS* entry = gfx->rdpcontext->instance->pClientEntryPoints;
	IFCALL(entry->ClientVideo, gfx->rdpcontext, Stream_Buffer(packet), Stream_GetPosition(packet));
	Stream_Free(packet, TRUE);
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpgfx_read_h264_metablock(WINPR_ATTR_UNUSED RDPGFX_PLUGIN* gfx, wStream* s,
                                       RDPGFX_H264_METABLOCK* meta)
{
	RECTANGLE_16* regionRect = nullptr;
	RDPGFX_H264_QUANT_QUALITY* quantQualityVal = nullptr;
	UINT error = ERROR_INVALID_DATA;
	meta->regionRects = nullptr;
	meta->quantQualityVals = nullptr;

	if (!Stream_CheckAndLogRequiredLengthWLog(gfx->base.log, s, 4))
		goto error_out;

	Stream_Read_UINT32(s, meta->numRegionRects); /* numRegionRects (4 bytes) */

	if (!Stream_CheckAndLogRequiredLengthOfSizeWLog(gfx->base.log, s, meta->numRegionRects, 8ull))
		goto error_out;

	meta->regionRects = (RECTANGLE_16*)calloc(meta->numRegionRects, sizeof(RECTANGLE_16));

	if (!meta->regionRects)
	{
		WLog_Print(gfx->base.log, WLOG_ERROR, "malloc failed!");
		error = CHANNEL_RC_NO_MEMORY;
		goto error_out;
	}

	meta->quantQualityVals =
	    (RDPGFX_H264_QUANT_QUALITY*)calloc(meta->numRegionRects, sizeof(RDPGFX_H264_QUANT_QUALITY));

	if (!meta->quantQualityVals)
	{
		WLog_Print(gfx->base.log, WLOG_ERROR, "malloc failed!");
		error = CHANNEL_RC_NO_MEMORY;
		goto error_out;
	}

	WLog_Print(gfx->base.log, WLOG_TRACE, "H264_METABLOCK: numRegionRects: %" PRIu32 "",
	           meta->numRegionRects);

	for (UINT32 index = 0; index < meta->numRegionRects; index++)
	{
		regionRect = &(meta->regionRects[index]);

		if ((error = rdpgfx_read_rect16(gfx->base.log, s, regionRect)))
		{
			WLog_Print(gfx->base.log, WLOG_ERROR,
			           "rdpgfx_read_rect16 failed with error %" PRIu32 "!", error);
			goto error_out;
		}

		WLog_Print(gfx->base.log, WLOG_TRACE,
		           "regionRects[%" PRIu32 "]: left: %" PRIu16 " top: %" PRIu16 " right: %" PRIu16
		           " bottom: %" PRIu16 "",
		           index, regionRect->left, regionRect->top, regionRect->right, regionRect->bottom);
	}

	if (!Stream_CheckAndLogRequiredLengthOfSizeWLog(gfx->base.log, s, meta->numRegionRects, 2ull))
	{
		error = ERROR_INVALID_DATA;
		goto error_out;
	}

	for (UINT32 index = 0; index < meta->numRegionRects; index++)
	{
		quantQualityVal = &(meta->quantQualityVals[index]);
		Stream_Read_UINT8(s, quantQualityVal->qpVal);      /* qpVal (1 byte) */
		Stream_Read_UINT8(s, quantQualityVal->qualityVal); /* qualityVal (1 byte) */
		quantQualityVal->qp = quantQualityVal->qpVal & 0x3F;
		quantQualityVal->r = (quantQualityVal->qpVal >> 6) & 1;
		quantQualityVal->p = (quantQualityVal->qpVal >> 7) & 1;
		WLog_Print(gfx->base.log, WLOG_TRACE,
		           "quantQualityVals[%" PRIu32 "]: qp: %" PRIu8 " r: %" PRIu8 " p: %" PRIu8
		           " qualityVal: %" PRIu8 "",
		           index, quantQualityVal->qp, quantQualityVal->r, quantQualityVal->p,
		           quantQualityVal->qualityVal);
	}

	return CHANNEL_RC_OK;
error_out:
	free_h264_metablock(meta);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpgfx_decode_AVC420(RDPGFX_PLUGIN* gfx, RDPGFX_SURFACE_COMMAND* cmd)
{
	RDPGFX_AVC420_BITMAP_STREAM h264 = WINPR_C_ARRAY_INIT;
	wStream* s = Stream_New(cmd->data, cmd->length);

	if (!s)
	{
		WLog_Print(gfx->base.log, WLOG_ERROR, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	UINT error = rdpgfx_read_h264_metablock(gfx, s, &(h264.meta));
	if (error != CHANNEL_RC_OK)
	{
		Stream_Free(s, FALSE);
		WLog_Print(gfx->base.log, WLOG_ERROR,
		           "rdpgfx_read_h264_metablock failed with error %" PRIu32 "!", error);
		return error;
	}

	h264.data = Stream_Pointer(s);
	h264.length = (UINT32)Stream_GetRemainingLength(s);
	Stream_Free(s, FALSE);
	cmd->extra = (void*)&h264;

	error = logSurfaceCommand(gfx, cmd);
	if (!error && freerdp_settings_get_string(gfx->rdpcontext->settings, FreeRDP_MyrtilleSessionId))
		error = myrtille_video_packet(gfx, cmd, &h264, 3, TRUE);

	free_h264_metablock(&h264.meta);
	cmd->extra = nullptr;
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpgfx_decode_AVC444(RDPGFX_PLUGIN* gfx, RDPGFX_SURFACE_COMMAND* cmd)
{
	UINT error = CHANNEL_RC_OK;
	UINT32 tmp = 0;
	size_t pos1 = 0;
	size_t pos2 = 0;

	RDPGFX_AVC444_BITMAP_STREAM h264 = WINPR_C_ARRAY_INIT;
	wStream* s = Stream_New(cmd->data, cmd->length);

	if (!s)
	{
		WLog_Print(gfx->base.log, WLOG_ERROR, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	if (!Stream_CheckAndLogRequiredLengthWLog(gfx->base.log, s, 4))
	{
		error = ERROR_INVALID_DATA;
		goto fail;
	}

	Stream_Read_UINT32(s, tmp);
	h264.cbAvc420EncodedBitstream1 = tmp & 0x3FFFFFFFUL;
	h264.LC = (tmp >> 30UL) & 0x03UL;

	if (h264.LC == 0x03)
	{
		error = ERROR_INVALID_DATA;
		goto fail;
	}

	pos1 = Stream_GetPosition(s);

	if ((error = rdpgfx_read_h264_metablock(gfx, s, &(h264.bitstream[0].meta))))
	{
		WLog_Print(gfx->base.log, WLOG_ERROR,
		           "rdpgfx_read_h264_metablock failed with error %" PRIu32 "!", error);
		goto fail;
	}

	pos2 = Stream_GetPosition(s);
	h264.bitstream[0].data = Stream_Pointer(s);

	if (h264.LC == 0)
	{
		const size_t bitstreamLen = 1ULL * h264.cbAvc420EncodedBitstream1 - pos2 + pos1;

		if ((bitstreamLen > UINT32_MAX) || (h264.cbAvc420EncodedBitstream1 < pos2 - pos1) ||
		    !Stream_CheckAndLogRequiredLengthWLog(gfx->base.log, s, bitstreamLen))
		{
			error = ERROR_INVALID_DATA;
			goto fail;
		}

		h264.bitstream[0].length = (UINT32)bitstreamLen;
		Stream_Seek(s, bitstreamLen);

		if ((error = rdpgfx_read_h264_metablock(gfx, s, &(h264.bitstream[1].meta))))
		{
			WLog_Print(gfx->base.log, WLOG_ERROR,
			           "rdpgfx_read_h264_metablock failed with error %" PRIu32 "!", error);
			goto fail;
		}

		h264.bitstream[1].data = Stream_Pointer(s);

		const size_t len = Stream_GetRemainingLength(s);
		if (len > UINT32_MAX)
			goto fail;
		h264.bitstream[1].length = (UINT32)len;
	}
	else
	{
		const size_t len = Stream_GetRemainingLength(s);
		if (len > UINT32_MAX)
			goto fail;
		h264.bitstream[0].length = (UINT32)len;
	}

	cmd->extra = (void*)&h264;

	if (freerdp_settings_get_string(gfx->rdpcontext->settings, FreeRDP_MyrtilleSessionId))
	{
		/* Decode both components natively to preserve bitmap/cache state, then
		 * forward both in decoder order. LC=2 is chroma, never a normal picture. */
		error = logSurfaceCommand(gfx, cmd);
		if (error)
			goto fail;
		const UINT32 chroma = cmd->codecId == RDPGFX_CODECID_AVC444v2 ? 2 : 1;
		error = myrtille_video_packet(gfx, cmd, &h264.bitstream[0],
		                             h264.LC == 2 ? chroma : 0, h264.LC != 0);
		if (!error && h264.LC == 0)
			error = myrtille_video_packet(gfx, cmd, &h264.bitstream[1], chroma, TRUE);
		goto fail;
	}
	error = logSurfaceCommand(gfx, cmd);

fail:
	Stream_Free(s, FALSE);
	free_h264_metablock(&h264.bitstream[0].meta);
	free_h264_metablock(&h264.bitstream[1].meta);
	cmd->extra = nullptr;
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
UINT rdpgfx_decode(RDPGFX_PLUGIN* gfx, RDPGFX_SURFACE_COMMAND* cmd)
{
	UINT error = CHANNEL_RC_OK;
	PROFILER_ENTER(context->SurfaceProfiler)

	switch (cmd->codecId)
	{
#if defined(WITH_GFX_AV1)
		case RDPGFX_CODECID_AV1:
#endif
		case RDPGFX_CODECID_AVC420:
			if ((error = rdpgfx_decode_AVC420(gfx, cmd)))
				WLog_Print(gfx->base.log, WLOG_ERROR,
				           "rdpgfx_decode_AVC420 failed with error %" PRIu32 "", error);

			break;

		case RDPGFX_CODECID_AVC444:
		case RDPGFX_CODECID_AVC444v2:
			if ((error = rdpgfx_decode_AVC444(gfx, cmd)))
				WLog_Print(gfx->base.log, WLOG_ERROR,
				           "rdpgfx_decode_AVC444 failed with error %" PRIu32 "", error);

			break;

		default:
			error = logSurfaceCommand(gfx, cmd);
			break;
	}

	PROFILER_EXIT(context->SurfaceProfiler)
	return error;
}
