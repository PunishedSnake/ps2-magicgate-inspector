#ifndef MCI_VIDEO_MODE_H
#define MCI_VIDEO_MODE_H

/*
 * Runtime display-mode model adapted from the hardware-tested
 * fhdb-bootstrap-manager 0.4.3 display backend. Inspector keeps the same
 * conservative geometry rather than inventing a second set of GS timings.
 */

#define MCI_VIDEO_MODE_COUNT 5u

typedef enum MciVideoMode {
    MCI_VIDEO_NATIVE = 0,
    MCI_VIDEO_480P,
    MCI_VIDEO_576P,
    MCI_VIDEO_720P,
    MCI_VIDEO_1080I
} MciVideoMode;

typedef struct MciVideoGeometry {
    unsigned int signal_width;
    unsigned int signal_height;
    unsigned int surface_width;
    unsigned int surface_height;
    unsigned int frame_width;
    unsigned int frame_height;
    unsigned int viewport_x;
    unsigned int viewport_y;
    unsigned int viewport_width;
    unsigned int viewport_height;
    unsigned int bits_per_pixel;
    unsigned int frame_count;
    int interlaced;
    int hardware_validated;
} MciVideoGeometry;

const char *MciVideoModeName(MciVideoMode mode);
const char *MciVideoModeId(MciVideoMode mode);
const MciVideoGeometry *MciVideoModeGeometry(MciVideoMode mode);
MciVideoMode MciVideoModeClamp(int value);

#endif /* MCI_VIDEO_MODE_H */
