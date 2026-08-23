#include "ui_layout.h"

#include <stddef.h>

static unsigned int round_positive(float value)
{
    if (value <= 0.0f)
        return 0u;
    return (unsigned int)(value + 0.5f);
}

int MciUiLayoutConfigure(MciUiLayout *layout,
                         unsigned int active_width,
                         unsigned int active_height,
                         unsigned int frame_width,
                         unsigned int frame_height,
                         unsigned int viewport_x,
                         unsigned int viewport_y,
                         unsigned int viewport_width,
                         unsigned int viewport_height)
{
    if (layout == NULL || active_width == 0u || active_height == 0u ||
        frame_width == 0u || frame_height == 0u ||
        viewport_width == 0u || viewport_height == 0u)
        return -1;
    if (active_width > frame_width || active_height > frame_height)
        return -2;
    if (viewport_x > active_width || viewport_y > active_height ||
        viewport_width > active_width - viewport_x ||
        viewport_height > active_height - viewport_y)
        return -3;

    layout->active_width = active_width;
    layout->active_height = active_height;
    layout->frame_width = frame_width;
    layout->frame_height = frame_height;
    layout->viewport_x = viewport_x;
    layout->viewport_y = viewport_y;
    layout->viewport_width = viewport_width;
    layout->viewport_height = viewport_height;
    layout->scale_x = (float)viewport_width / (float)MCI_UI_LOGICAL_WIDTH;
    layout->scale_y = (float)viewport_height / (float)MCI_UI_LOGICAL_HEIGHT;
    return 0;
}

float MciUiMapX(const MciUiLayout *layout, float logical_x)
{
    return (float)layout->viewport_x + logical_x * layout->scale_x;
}

float MciUiMapY(const MciUiLayout *layout, float logical_y)
{
    return (float)layout->viewport_y + logical_y * layout->scale_y;
}

float MciUiSnapX(const MciUiLayout *layout, float logical_x)
{
    return (float)round_positive(MciUiMapX(layout, logical_x));
}

float MciUiSnapY(const MciUiLayout *layout, float logical_y)
{
    return (float)round_positive(MciUiMapY(layout, logical_y));
}

void MciUiTextCell(const MciUiLayout *layout,
                   float logical_x, float logical_y,
                   unsigned int *x, unsigned int *y,
                   unsigned int *width, unsigned int *height)
{
    unsigned int x0 = round_positive(MciUiMapX(layout, logical_x));
    unsigned int y0 = round_positive(MciUiMapY(layout, logical_y));
    unsigned int x1 = round_positive(MciUiMapX(
        layout, logical_x + (float)MCI_UI_CELL_WIDTH));
    unsigned int y1 = round_positive(MciUiMapY(
        layout, logical_y + (float)MCI_UI_CELL_HEIGHT));

    if (x != NULL) *x = x0;
    if (y != NULL) *y = y0;
    if (width != NULL) *width = x1 > x0 ? x1 - x0 : 1u;
    if (height != NULL) *height = y1 > y0 ? y1 - y0 : 1u;
}
