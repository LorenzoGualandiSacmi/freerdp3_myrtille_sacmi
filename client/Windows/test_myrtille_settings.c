#include <freerdp/config.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/settings.h>
#include <winpr/crypto.h>
#include <winpr/ssl.h>
#include <stdio.h>
#include <string.h>
#include "wf_myrtille_video.h"

/* Exercise the actual gateway command line and settings clone/free ownership. */
int main(void)
{
    char* args[] = { "Myrtille.RDP.exe", "/myrtille-sid:regression", "/myrtille-log",
        "/w:1280", "/h:720", "-wallpaper", "-aero", "-window-drag", "-menu-anims",
        "+themes", "+fonts", "+compression", "/cert-ignore", "+mouse-motion",
        "-async-input", "-async-update", "-async-channels", "+clipboard", "/sound",
        "/audio-mode:0", "/gfx-h264:AVC444" };
    int result = 1;
    BYTE digest[16];
    const BYTE expected[16] = { 0xdb,0x34,0x6d,0x69,0x1d,0x7a,0xcc,0x4d,
                               0xc2,0x62,0x5d,0xb1,0x9f,0x9e,0x3f,0x52 };
    rdpSettings* settings = freerdp_settings_new(0);
    rdpSettings* copy = NULL;
    if (!settings) goto done;
    /* NTLM requires MD4, supplied by OpenSSL 3's legacy provider. */
    if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT) ||
        !winpr_Digest(WINPR_MD_MD4, "test", 4, digest, sizeof(digest)) ||
        memcmp(digest, expected, sizeof(digest))) goto done;
    if (freerdp_client_settings_parse_command_line(settings,
            sizeof(args) / sizeof(args[0]), args, FALSE) != 0) goto done;
    if (strcmp(freerdp_settings_get_string(settings, FreeRDP_MyrtilleSessionId), "regression")) goto done;
    if (!freerdp_settings_get_bool(settings, FreeRDP_MyrtilleDebugLog)) goto done;
    if (freerdp_settings_get_bool(settings, FreeRDP_MyrtilleShowWindow)) goto done;
    if (!freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444)) goto done;
    if (!freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline)) goto done;
    if (!wf_myrtille_configure_video(settings, NULL) ||
        !freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444)) goto done;
    if (!wf_myrtille_configure_video(settings, "1") ||
        !freerdp_settings_get_bool(settings, FreeRDP_GfxH264) ||
        freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444) ||
        freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2)) goto done;
    copy = freerdp_settings_clone(settings);
    if (!copy) goto done;
    if (!freerdp_settings_set_string(settings, FreeRDP_MyrtilleSessionId, "changed")) goto done;
    freerdp_settings_free(settings);
    settings = NULL;
    if (strcmp(freerdp_settings_get_string(copy, FreeRDP_MyrtilleSessionId), "regression")) goto done;
    if (!freerdp_settings_get_bool(copy, FreeRDP_MyrtilleDebugLog)) goto done;
    puts("PASS: OpenSSL MD4, gateway arguments, AVC444, custom settings clone/free");
    result = 0;
done:
    freerdp_settings_free(copy);
    freerdp_settings_free(settings);
    if (result) fputs("FAIL: Myrtille settings compatibility\n", stderr);
    return result;
}
