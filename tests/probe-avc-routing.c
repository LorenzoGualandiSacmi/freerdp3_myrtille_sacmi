/* Diagnostic: feeds synthetic RDP envelopes to the production parser.
 * Payload markers are NOT playable H.264. This tests routing, not decoding.
 * Verifies component routing and metadata, not actual H.264 decoding.
 */
#include <freerdp/config.h>
#include <freerdp/client.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/codec/color.h>
#include <freerdp/primitives.h>
#include <freerdp/codec/yuv.h>
#include "rdpgfx_codec.h"
#include <stdio.h>
#include <string.h>

static unsigned calls, rendered;
static BYTE marker;
static BYTE captured[128];
static size_t capturedSize;
static gdiGfxSurface testSurface;
static void* getSurface(RdpgfxClientContext* ctx, UINT16 id)
{
    (void)ctx; (void)id;
    return &testSurface;
}
static void video(rdpContext* context, const BYTE* data, size_t size)
{
    (void)context;
    calls++;
    marker = size ? data[size - 1] : 0;
    capturedSize = size;
    if (size <= sizeof(captured)) memcpy(captured, data, size);
}
/* Only the native rendering sink is replaced; parsing/dispatch is production code. */
UINT logSurfaceCommand(RDPGFX_PLUGIN* gfx, const RDPGFX_SURFACE_COMMAND* cmd)
{
    (void)gfx; (void)cmd;
    rendered++;
    return CHANNEL_RC_OK;
}
static void payload(wStream* s, BYTE value)
{
    Stream_Write_UINT32(s, 1); /* one changed rectangle */
    Stream_Write_UINT16(s, 8); Stream_Write_UINT16(s, 8);
    Stream_Write_UINT16(s, 16); Stream_Write_UINT16(s, 16);
    Stream_Write_UINT8(s, 20); Stream_Write_UINT8(s, 100);
    Stream_Write_UINT8(s, value);
}
int main(int argc, char** argv)
{
    RDPGFX_PLUGIN gfx = { 0 };
    rdpContext context = { 0 };
    freerdp instance = { 0 };
    RDP_CLIENT_ENTRY_POINTS entry = { 0 };
    RdpgfxClientContext mapped = { 0 };
    int failed = 0;
    {
        BYTE y[32*32], u[16*16], v[16*16], generic[3][32*32], optimized[3][32*32];
        const BYTE* input[3] = { y, u, v };
        BYTE* a[3] = { generic[0], generic[1], generic[2] };
        BYTE* b[3] = { optimized[0], optimized[1], optimized[2] };
        const UINT32 srcStride[3] = { 32, 16, 16 }, dstStride[3] = { 32, 32, 32 };
        const RECTANGLE_16 rect = { 0, 0, 32, 32 };
        for (unsigned i=0;i<sizeof(y);i++) y[i]=(BYTE)(i%251);
        for (unsigned i=0;i<sizeof(u);i++) { u[i]=(BYTE)(i%197); v[i]=(BYTE)(220-i%173); }
        memset(generic,127,sizeof(generic));memset(optimized,127,sizeof(optimized));
        if (primitives_get_generic()->YUV420CombineToYUV444(AVC444_CHROMAv1,input,srcStride,32,32,a,dstStride,&rect) ||
            primitives_get()->YUV420CombineToYUV444(AVC444_CHROMAv1,input,srcStride,32,32,b,dstStride,&rect) ||
            memcmp(generic,optimized,sizeof(generic))) {
            puts("FAIL: native optimized AVC444 v1 differs from scalar reconstruction"); failed=1;
        } else puts("PASS: native optimized/scalar AVC444 v1 parity");
    }
    {
        BYTE planes[3][64*8], scalar[64*8*4], simd[64*8*4];
        const BYTE* input[3] = { planes[0], planes[1], planes[2] };
        const UINT32 strides[3] = {64,64,64};
        for (unsigned p=0;p<3;p++) for (unsigned i=0;i<64*8;i++)
            planes[p][i]=(BYTE)((i*13+p*71)%256);
        for (unsigned width=16;width<=64;width+=2) {
            prim_size_t roi={width,8};
            memset(scalar,255,sizeof(scalar));memset(simd,255,sizeof(simd));
            primitives_get_generic()->YUV444ToRGB_8u_P3AC4R(input,strides,scalar,256,PIXEL_FORMAT_BGRX32,&roi);
            primitives_get()->YUV444ToRGB_8u_P3AC4R(input,strides,simd,256,PIXEL_FORMAT_BGRX32,&roi);
            if (memcmp(scalar,simd,sizeof(scalar))) {
                printf("FAIL: BGRX scalar/SIMD conversion width=%u\n",width);failed=1;
            }
        }
    }
    {
        BYTE raw[3][32*32], fullPlanes[3][32*32], partPlanes[3][32*32];
        BYTE fullRGB[32*32*4], partRGB[32*32*4];
        const BYTE* src[3]={raw[0],raw[1],raw[2]};
        BYTE* full[3]={fullPlanes[0],fullPlanes[1],fullPlanes[2]};
        BYTE* part[3]={partPlanes[0],partPlanes[1],partPlanes[2]};
        const UINT32 strides[3]={32,32,32};
        const RECTANGLE_16 all={0,0,32,32}, mask={18,18,19,19};
        YUV_CONTEXT* yuv=yuv_context_new(FALSE,0);
        if (!yuv || !yuv_context_reset(yuv,32,32)) return 1;
        for (unsigned i=0;i<sizeof(raw);i++) ((BYTE*)raw)[i]=(BYTE)(i*17+51);
        for (BYTE type=AVC444_CHROMAv1;type<=AVC444_CHROMAv2;type++) {
            memset(fullPlanes,127,sizeof(fullPlanes));memset(partPlanes,127,sizeof(partPlanes));
            memset(fullRGB,255,sizeof(fullRGB));memset(partRGB,255,sizeof(partRGB));
            if (!yuv444_context_decode(yuv,type,src,strides,32,full,strides,PIXEL_FORMAT_RGBA32,fullRGB,128,&all,1) ||
                !yuv444_context_decode(yuv,type,src,strides,32,part,strides,PIXEL_FORMAT_RGBA32,partRGB,128,&mask,1)) return 1;
            for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) for(unsigned c=0;c<3;c++) {
                const unsigned i=(y*32+x)*4+c;
                if (partRGB[i] != ((x==18&&y==18)?fullRGB[i]:255)) {
                    printf("FAIL: native masked AVC444 type=%u at %u,%u\n",type,x,y);failed=1;
                    y=32;x=32;break;
                }
            }
        }
        yuv_context_free(yuv);
    }
    context.settings = freerdp_settings_new(0);
    if (!context.settings) return 1;
    if (!freerdp_settings_set_string(context.settings, FreeRDP_MyrtilleSessionId, "probe"))
        return 1;
    entry.ClientVideo = video;
    instance.pClientEntryPoints = &entry;
    context.instance = &instance;
    gfx.rdpcontext = &context;
    gfx.base.log = WLog_Get("myrtille.avc.probe");
    mapped.GetSurfaceData = getSurface;
    gfx.context = &mapped;
    testSurface.outputMapped = TRUE;
    testSurface.width = testSurface.outputTargetWidth = 32;
    testSurface.height = testSurface.outputTargetHeight = 32;
    for (unsigned variant = 0; variant < 2; variant++)
    for (unsigned lc = 0; lc < 3; lc++)
    {
        RDPGFX_SURFACE_COMMAND cmd = { 0 };
        wStream* s = Stream_New(NULL, 64);
        if (!s) return 1;
        Stream_Write_UINT32(s, (lc << 30) | (lc == 0 ? 15 : 0));
        payload(s, lc == 2 ? 0xCC : 0x11);
        if (lc == 0) payload(s, 0xCC);
        cmd.codecId = variant ? RDPGFX_CODECID_AVC444v2 : RDPGFX_CODECID_AVC444;
        cmd.data = Stream_Buffer(s);
        cmd.length = (UINT32)Stream_GetPosition(s);
        calls = rendered = 0;
        UINT status = rdpgfx_decode(&gfx, &cmd);
        printf("AVC444%s LC=%u: status=%u videoCalls=%u lastPayload=%02X nativeCalls=%u\n",
               variant ? "v2" : "", lc, status, calls, marker, rendered);
        if (status || calls != (lc == 0 ? 2 : 1) || rendered != 1 ||
            marker != (lc == 1 ? 0x11 : 0xCC) || cmd.extra ||
            capturedSize != 49 || memcmp(captured, "MAV2", 4) ||
            captured[32] != (lc == 1 ? 0 : variant + 1) || captured[36] != 1)
            failed = 1;
        Stream_Free(s, TRUE);
    }
    gfx.context = NULL;
    {
        RDPGFX_SURFACE_COMMAND cmd = { 0 };
        wStream* s = Stream_New(NULL, 64);
        if (!s) return 1;
        payload(s, 0x11);
        cmd.codecId = RDPGFX_CODECID_AVC420;
        cmd.data = Stream_Buffer(s);
        cmd.length = (UINT32)Stream_GetPosition(s);
        calls = rendered = 0;
        UINT status = rdpgfx_decode(&gfx, &cmd);
        printf("AVC420: status=%u videoCalls=%u nativeCalls=%u\n", status, calls, rendered);
        if (status || calls || rendered != 1 || cmd.extra) failed = 1;
        Stream_Free(s, TRUE);
    }
    {
        RdpgfxClientContext client = { 0 };
        RDPGFX_SURFACE_COMMAND cmd = { 0 };
        wStream* s = Stream_New(NULL, 64);
        if (!s) return 1;
        client.GetSurfaceData = getSurface;
        gfx.context = &client;
        testSurface.outputMapped = TRUE;
        testSurface.width = testSurface.outputTargetWidth = 32;
        testSurface.height = testSurface.outputTargetHeight = 32;
        testSurface.outputOriginX = 100;
        testSurface.outputOriginY = 50;
        payload(s, 0x11);
        cmd.codecId = RDPGFX_CODECID_AVC420;
        cmd.surfaceId = 7;
        cmd.data = Stream_Buffer(s);
        cmd.length = (UINT32)Stream_GetPosition(s);
        calls = rendered = 0;
        UINT status = rdpgfx_decode(&gfx, &cmd);
        if (status || calls != 1 || rendered != 1 || capturedSize != 41 ||
            memcmp(captured, "MAV1", 4) || captured[4] != 7 || captured[8] != 100 ||
            captured[12] != 50 || captured[16] != 32 || captured[20] != 32 ||
            captured[24] != 1 || captured[28] != 1 || captured[32] != 8 ||
            captured[34] != 8 || captured[36] != 16 || captured[38] != 16 || captured[40] != 0x11)
            failed = 1;
        puts("AVC420 mapped surface: checked MAV1 header, rectangle, payload and native-state preservation");
        testSurface.outputTargetWidth = 64; // Scaled surface must use native compositor.
        calls = rendered = 0;
        status = rdpgfx_decode(&gfx, &cmd);
        if (status || calls || rendered != 1) failed = 1;
        Stream_Free(s, TRUE);
    }
    freerdp_settings_free(context.settings);
    puts(failed ? "Unexpected routing: investigate probe failure" :
         "PASS: AVC420/AVC444/v2 packets, LC=0/1/2 and native fallback");
    if (!failed && argc == 2)
    {
        BYTE rgb[32 * 32 * 4];
        BYTE* encoded = NULL;
        UINT32 length = 0;
        RECTANGLE_16 rect = { 0, 0, 32, 32 };
        RDPGFX_H264_METABLOCK meta = { 0 };
        H264_CONTEXT* encoder = h264_context_new(TRUE);
        for (unsigned i = 0; i < sizeof(rgb); i += 4)
        { rgb[i] = 0; rgb[i + 1] = 0; rgb[i + 2] = 255; rgb[i + 3] = 255; }
        if (!encoder || !h264_context_reset(encoder, 32, 32) ||
            avc420_compress(encoder, rgb, PIXEL_FORMAT_BGRA32, 128, 32, 32,
                            &rect, &encoded, &length, &meta) < 0 || !length)
            failed = 1;
        if (!failed) {
            FILE* out = fopen(argv[1], "wb");
            if (!out) failed = 1;
            else { if (fwrite(encoded, 1, length, out) != length) failed = 1; fclose(out); }
        }
        free_h264_metablock(&meta);
        h264_context_free(encoder);
    }
    return failed;
}
