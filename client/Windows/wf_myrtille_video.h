#ifndef WF_MYRTILLE_VIDEO_H
#define WF_MYRTILLE_VIDEO_H
#include <freerdp/settings.h>
#include <string.h>

static inline BOOL wf_myrtille_configure_video(rdpSettings* settings, const char* mode)
{
    if (!freerdp_settings_get_string(settings, FreeRDP_MyrtilleSessionId) ||
        !mode || strcmp(mode, "1") != 0 ||
        !(freerdp_settings_get_bool(settings, FreeRDP_GfxH264) ||
          freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444)))
        return TRUE;
    return freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE) &&
           freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE) &&
           freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE);
}
#endif
