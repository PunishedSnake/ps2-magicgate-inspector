#ifndef MCI_UI_LAYOUT_H
#define MCI_UI_LAYOUT_H

#define MCI_UI_LOGICAL_WIDTH 640u
#define MCI_UI_LOGICAL_HEIGHT 224u
#define MCI_UI_CELL_WIDTH 8u
#define MCI_UI_CELL_HEIGHT 8u

typedef struct MciUiLayout {
    unsigned int active_width;
    unsigned int active_height;
    unsigned int frame_width;
    unsigned int frame_height;
    unsigned int viewport_x;
    unsigned int viewport_y;
    unsigned int viewport_width;
    unsigned int viewport_height;
    float scale_x;
    float scale_y;
} MciUiLayout;

int MciUiLayoutConfigure(MciUiLayout *layout,
                         unsigned int active_width,
                         unsigned int active_height,
                         unsigned int frame_width,
                         unsigned int frame_height,
                         unsigned int viewport_x,
                         unsigned int viewport_y,
                         unsigned int viewport_width,
                         unsigned int viewport_height);
float MciUiMapX(const MciUiLayout *layout, float logical_x);
float MciUiMapY(const MciUiLayout *layout, float logical_y);
float MciUiSnapX(const MciUiLayout *layout, float logical_x);
float MciUiSnapY(const MciUiLayout *layout, float logical_y);
void MciUiTextCell(const MciUiLayout *layout,
                   float logical_x, float logical_y,
                   unsigned int *x, unsigned int *y,
                   unsigned int *width, unsigned int *height);

#endif /* MCI_UI_LAYOUT_H */
