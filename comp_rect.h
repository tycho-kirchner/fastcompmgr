#pragma once

#include <stdbool.h>

typedef struct {
    short x1;
    short y1;
    short x2;
    short y2;
    short w;
    short h;
} CompRect;



bool rect_paint_needed(CompRect* ignore_reg, CompRect* reg);
