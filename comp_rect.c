
#include "comp_rect.h"



/// Returns true, if r1 fully contains r2
static bool rect_contains(CompRect* r1, CompRect* r2){
    return r1->x1 <= r2->x1 && r1->y1 <= r2->y1 &&
            r1->x2 >= r2->x2 && r1->y2 >=r2->y2;
}


static bool rects_are_intersecting(CompRect* r1, CompRect* r2)
{
    // if the left point of one rect is greater
    // than the right one of the other, nothing intersects.
    if(r1->x1 > r2->x2 || r2->x1 > r1->x2){
        return false;
    }
    if(r1->y1 > r2->y2 || r2->y1 > r1->y2){
        return false;
    }
    return true;
}

/// Check if we can omit painting a window (rect). E.g., a window
/// completely occluded by another one, does not need to be
/// painted. Further, we try to select the largest possible ignore region
/// Based on window and intersection areas.
bool rect_paint_needed(CompRect* ignore_reg, CompRect* reg){
    if(rect_contains(ignore_reg, reg)){
        // the ignore-region completely occludes the window.
        return false;
    }
    if(! rects_are_intersecting(ignore_reg, reg)){
        // KISS and just use the greater rect as new ignore region.
        if(reg->w*reg->h > ignore_reg->w*ignore_reg->h){
            *ignore_reg = *reg;
        }
        return true;
    }

    // calculate the intersection rect.
    short x1 = (ignore_reg->x1 > reg->x1) ? ignore_reg->x1 : reg->x1;
    short x2 = (ignore_reg->x2 < reg->x2) ? ignore_reg->x1 : reg->x1;
    short y1 = (ignore_reg->y1 > reg->y1) ? ignore_reg->y1 : reg->y1;
    short y2 = (ignore_reg->y2 < reg->y2) ? ignore_reg->y1 : reg->y1;
    short w = x2 - x1;
    short h = y2 - y1;

    // KISS and just use the biggest rect as new ignore rect
    if(reg->w*reg->h > ignore_reg->w*ignore_reg->h){
        *ignore_reg = *reg;
    }
    if(w*h > ignore_reg->w*ignore_reg->h){
        CompRect r_intersect = {.x1 = x1, .y1 = y1,
                  .x2 = x2, .y2 = y2,
                  .w = w, .h = h };
        *ignore_reg = r_intersect;
    }
    return true;
}
