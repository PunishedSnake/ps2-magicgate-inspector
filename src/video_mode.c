#include "video_mode.h"

#include <stddef.h>

/*
 * Geometry follows fhdb-bootstrap-manager 0.4.3, which fixed progressive and
 * interlaced presentation on physical PS2 hardware. The logical Inspector UI
 * remains 640x224; viewport scaling is handled separately by ui_layout.c.
 *
 * User-facing labels intentionally omit "progressive"/"interlaced": the p/i
 * suffix already communicates scan type and the shorter form stays readable
 * in the Settings value column without unnecessary marquee animation.
 */
typedef struct MciVideoIdentity {
    const char *id;
    const char *name;
    MciVideoGeometry geometry;
} MciVideoIdentity;

static const MciVideoIdentity Modes[MCI_VIDEO_MODE_COUNT] = {
    {"native", "Native (640x224)",
     {640, 448, 640, 224, 640, 224, 0, 0, 640, 224, 32, 2, 1, 1}},
    {"480p", "480p (720x448)",
     {720, 480, 720, 448, 768, 448, 0, 0, 720, 448, 32, 2, 0, 1}},
    {"576p", "576p (720x576)",
     {720, 576, 720, 576, 768, 576, 40, 0, 640, 480, 32, 1, 0, 0}},
    {"720p", "720p (1280x720)",
     {1280, 720, 640, 720, 640, 720, 0, 0, 640, 720, 32, 1, 0, 0}},
    {"1080i", "1080i (1920x1080)",
     {1920, 1080, 640, 540, 640, 540, 0, 13, 640, 527, 32, 2, 1, 0}}
};

const char *MciVideoModeName(MciVideoMode mode)
{
    if ((unsigned int)mode >= MCI_VIDEO_MODE_COUNT)
        return "Unknown";
    return Modes[(unsigned int)mode].name;
}

const char *MciVideoModeId(MciVideoMode mode)
{
    if ((unsigned int)mode >= MCI_VIDEO_MODE_COUNT)
        return "unknown";
    return Modes[(unsigned int)mode].id;
}

const MciVideoGeometry *MciVideoModeGeometry(MciVideoMode mode)
{
    if ((unsigned int)mode >= MCI_VIDEO_MODE_COUNT)
        return NULL;
    return &Modes[(unsigned int)mode].geometry;
}

MciVideoMode MciVideoModeClamp(int value)
{
    if (value < 0)
        return MCI_VIDEO_NATIVE;
    if ((unsigned int)value >= MCI_VIDEO_MODE_COUNT)
        return (MciVideoMode)(MCI_VIDEO_MODE_COUNT - 1u);
    return (MciVideoMode)value;
}
