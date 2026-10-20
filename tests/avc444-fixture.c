/* Produces real AVC444 packets through the production parser and native RGBA
 * reference output for browser differential testing. Generates synthetic
 * images by default; --replay reads a private local MAV2 capture. */
#include <freerdp/config.h>
#include <freerdp/client.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/codec/color.h>
#include <freerdp/primitives.h>
#include "../libfreerdp/codec/h264.h"
#include "rdpgfx_codec.h"
#include <stdio.h>
#include <string.h>
static gdiGfxSurface surface;
static H264_CONTEXT* decoder;
static BYTE* rgba;
static UINT32 width=32,height=32,format=PIXEL_FORMAT_RGBA32;
static FILE* packets;
static int writeFailed;
static void* getSurface(RdpgfxClientContext* c, UINT16 id) { (void)c; (void)id; return &surface; }
static void video(rdpContext* c, const BYTE* bytes, size_t size) {
    (void)c;
    const UINT32 len=(UINT32)size;
    if (fwrite(&len,4,1,packets)!=1 || fwrite(bytes,1,size,packets)!=size) writeFailed=1;
}
UINT logSurfaceCommand(RDPGFX_PLUGIN* gfx, const RDPGFX_SURFACE_COMMAND* cmd) {
    (void)gfx;
    RDPGFX_AVC444_BITMAP_STREAM* b=cmd->extra;
    return avc444_decompress(decoder,b->LC,b->bitstream[0].meta.regionRects,
        b->bitstream[0].meta.numRegionRects,b->bitstream[0].data,b->bitstream[0].length,
        b->bitstream[1].meta.regionRects,b->bitstream[1].meta.numRegionRects,
        b->bitstream[1].data,b->bitstream[1].length,rgba,format,width*4,width,height,cmd->codecId)<0 ? ERROR_INVALID_DATA : 0;
}
static void subframe(wStream* s, const BYTE* bytes, UINT32 length) {
    Stream_Write_UINT32(s,1);
    Stream_Write_UINT16(s,0); Stream_Write_UINT16(s,0);
    Stream_Write_UINT16(s,width); Stream_Write_UINT16(s,height);
    Stream_Write_UINT8(s,20); Stream_Write_UINT8(s,100);
    Stream_Write(s,bytes,length);
}
static int replay(const char* input, const char* output) {
    if(getenv("MYRTILLE_REPLAY_SCALAR")) {
        primitives_get()->YUV420CombineToYUV444=primitives_get_generic()->YUV420CombineToYUV444;
        primitives_get()->YUV444ToRGB_8u_P3AC4R=primitives_get_generic()->YUV444ToRGB_8u_P3AC4R;
    }
    FILE* in=fopen(input,"rb"); if(!in)return 20;
    UINT32 n=0,index=0; H264_CONTEXT* dec=NULL; BYTE* pixels=NULL;
    UINT32 w=0,h=0;
    while(fread(&n,4,1,in)==1) {
        if(n<40 || n>32*1024*1024)return 21;
        /* H.264 readers may read ahead into zero padding beyond the payload. */
        BYTE* p=calloc((size_t)n+64,1); if(!p || fread(p,1,n,in)!=n)return 22;
        UINT32 hdr[10];memcpy(hdr,p,40);
        if(hdr[0]!=0x3256414D || hdr[8]>2 || 40ULL+8ULL*hdr[6]+hdr[7]!=n)return 23;
        if(!dec) {
            w=hdr[4];h=hdr[5]; if(!w || !h || w>4096 || h>4096)return 24;
            dec=h264_context_new(FALSE);pixels=calloc((size_t)w*h,4);
            if(!dec || !pixels || !h264_context_reset(dec,w,h))return 25;
        }
        if(w!=hdr[4] || h!=hdr[5])return 26;
        RECTANGLE_16* rects=(RECTANGLE_16*)(p+40);
        int rc=avc444_decompress(dec,hdr[8]?2:1,rects,hdr[6],p+40+8*hdr[6],hdr[7],
            NULL,0,NULL,0,pixels,PIXEL_FORMAT_RGBA32,w*4,w,h,
            hdr[8]==1?RDPGFX_CODECID_AVC444:RDPGFX_CODECID_AVC444v2);
        if(rc<0){printf("FAIL packet %u component %u rc=%d\n",index,hdr[8],rc);return 27;}
        if(index<5)printf("raw %u kind=%u decoder=%s decoded=%ux%u stride=%u,%u,%u combined=%u\n",index,hdr[8],dec->subsystem->name,dec->YUVWidth,dec->YUVHeight,dec->iStride[0],dec->iStride[1],dec->iStride[2],dec->iYUV444Stride[0]);
        if(index==30) {
            char path[4096];snprintf(path,sizeof(path),"%s-raw.yuv",output);
            FILE* f=fopen(path,"wb");if(!f)return 28;
            for(unsigned plane=0;plane<3;plane++)for(unsigned y=0;y<(plane?h/2:h);y++)
                fwrite(dec->pYUVData[plane]+y*dec->iStride[plane],1,plane?w/2:w,f);
            fclose(f);snprintf(path,sizeof(path),"%s-combined.yuv",output);f=fopen(path,"wb");if(!f)return 28;
            for(unsigned plane=0;plane<3;plane++)for(unsigned y=0;y<h;y++)
                fwrite(dec->pYUV444Data[plane]+y*dec->iYUV444Stride[plane],1,w,f);
            fclose(f);
        }
        if(index<5 || index==30) {
            char path[4096];snprintf(path,sizeof(path),"%s-%u.rgba",output,index);
            FILE* f=fopen(path,"wb");if(!f)return 28;
            if(fwrite(pixels,4,(size_t)w*h,f)!=(size_t)w*h)return 29;fclose(f);
        }
        free(p);index++;
    }
    char path[4096];snprintf(path,sizeof(path),"%s-last.rgba",output);
    FILE* f=fopen(path,"wb");if(!f)return 28;
    if(fwrite(pixels,4,(size_t)w*h,f)!=(size_t)w*h)return 29;fclose(f);
    printf("PASS replay %u components %ux%u\n",index,w,h);
    h264_context_free(dec);free(pixels);fclose(in);return 0;
}
int main(int argc, char** argv) {
    if(argc==4 && strcmp(argv[1],"--replay")==0)return replay(argv[2],argv[3]);
    if (argc!=4 && argc!=7) return 1;
    if(argc==7) {width=atoi(argv[4]);height=atoi(argv[5]);if(atoi(argv[6]))format=PIXEL_FORMAT_BGRA32;}
    if(!width || !height || width>4096 || height>4096) return 1;
    rgba=calloc((size_t)width*height,4); if(!rgba)return 2;
    const BYTE version=(BYTE)atoi(argv[1]);
    H264_CONTEXT* encoder=h264_context_new(TRUE);
    decoder=h264_context_new(FALSE);
    if (!encoder || !decoder || !h264_context_reset(encoder,width,height) || !h264_context_reset(decoder,width,height)) return 2;
    if (!h264_context_set_option(encoder,H264_CONTEXT_OPTION_RATECONTROL,H264_RATECONTROL_CQP)) return 2;
    packets=fopen(argv[2],"wb"); FILE* reference=fopen(argv[3],"wb");
    if (!packets || !reference) return 3;
    RDPGFX_PLUGIN gfx={0}; rdpContext context={0}; freerdp instance={0};
    RDP_CLIENT_ENTRY_POINTS entry={0}; RdpgfxClientContext client={0};
    context.settings=freerdp_settings_new(0);
    if (!context.settings || !freerdp_settings_set_string(context.settings,FreeRDP_MyrtilleSessionId,"fixture")) return 4;
    context.instance=&instance; instance.pClientEntryPoints=&entry; entry.ClientVideo=video;
    gfx.rdpcontext=&context; gfx.context=&client; gfx.base.log=WLog_Get("avc444.fixture");
    client.GetSurfaceData=getSurface;
    surface.outputMapped=TRUE; surface.width=surface.outputTargetWidth=width; surface.height=surface.outputTargetHeight=height;
    for (unsigned frame=0;frame<3;frame++) {
        BYTE* rgb=calloc((size_t)width*height,4), *base=NULL, *aux=NULL, lc=0;
        if(!rgb)return 2;
        UINT32 baseLen=0,auxLen=0; RDPGFX_H264_METABLOCK meta={0},auxMeta={0};
        RECTANGLE_16 rect={0,0,width,height};
        for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
            const unsigned o=(y*width+x)*4;
            rgb[o]=(BYTE)((x&1)?220:30); rgb[o+1]=(BYTE)(40+frame*30+(y&1)*60);
            rgb[o+2]=(BYTE)((x&1)?30:220); rgb[o+3]=255;
        }
        if (avc444_compress(encoder,rgb,PIXEL_FORMAT_RGBA32,width*4,width,height,version,&rect,&lc,
            &base,&baseLen,&aux,&auxLen,&meta,&auxMeta)<=0) return 5;
        printf("encoded %ux%u LC=%u base=%u aux=%u\n",width,height,lc,baseLen,auxLen);
        wStream* s=Stream_New(NULL,32ULL+baseLen+auxLen);
        if (!s) return 6;
        Stream_Write_UINT32(s,((UINT32)lc<<30)|(lc==0?14+baseLen:0));
        subframe(s,lc==2?aux:base,lc==2?auxLen:baseLen);
        if (lc==0) subframe(s,aux,auxLen);
        RDPGFX_SURFACE_COMMAND cmd={0}; cmd.surfaceId=1;
        cmd.codecId=version==2?RDPGFX_CODECID_AVC444v2:RDPGFX_CODECID_AVC444;
        cmd.data=Stream_Buffer(s);cmd.length=(UINT32)Stream_GetPosition(s);
        if (rdpgfx_decode(&gfx,&cmd)) return 7;
        for (unsigned i=3;i<((size_t)width*height*4);i+=4) rgba[i]=255;
        if (fwrite(rgba,1,((size_t)width*height*4),reference)!=((size_t)width*height*4)) return 8;
        printf("AVC444v%u frame %u LC=%u base=%u auxiliary=%u\n",version,frame,lc,baseLen,auxLen);
        free(rgb); Stream_Free(s,TRUE); free_h264_metablock(&meta);free_h264_metablock(&auxMeta);
    }
    fclose(packets);fclose(reference);freerdp_settings_free(context.settings);
    h264_context_free(encoder);h264_context_free(decoder);
    free(rgba); return writeFailed?9:0;
}
