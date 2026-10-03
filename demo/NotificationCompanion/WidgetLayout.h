#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>

namespace companion {
inline bool overlaps(RECT a, RECT b, int gap = 0) {
    InflateRect(&b, gap, gap);
    RECT intersection{};
    return IntersectRect(&intersection, &a, &b) != FALSE;
}
inline bool allowed_slot(RECT slot, RECT work, RECT reserved, int gap) {
    return slot.left >= work.left + gap && slot.top >= work.top + gap &&
        slot.right <= work.right - gap && slot.bottom <= work.bottom - gap &&
        (IsRectEmpty(&reserved) || !overlaps(slot, reserved, gap));
}
// First fill the column immediately beside the Shell, bottom to top, then
// extend left. The remaining workspace is available if that strip is full.
inline std::vector<RECT> arrange_widgets(RECT work, RECT reserved, int width,
                                        std::vector<int> const& heights, int gap) {
    std::vector<RECT> slots;
    int bottom = IsRectEmpty(&reserved) ? work.bottom-gap : std::min(int(work.bottom)-gap, int(reserved.bottom));
    int first = IsRectEmpty(&reserved) ? work.right-gap-width : reserved.left-gap-width;
    std::vector<int> columns;
    for (int x=first; x>=work.left+gap; x-=width+gap) columns.push_back(x);
    for (int x=work.right-gap-width; x>=work.left+gap; x-=width+gap)
        if (std::find(columns.begin(),columns.end(),x)==columns.end()) columns.push_back(x);
    for (int height : heights) {
        RECT found{};
        for (int x : columns) {
            std::vector<int> rows{bottom-height, int(reserved.top)-gap-height};
            for (auto slot : slots) rows.push_back(slot.top-gap-height);
            std::sort(rows.begin(),rows.end(),std::greater<int>());
            for (int y : rows) {
                RECT candidate{x,y,x+width,y+height};
                if (!allowed_slot(candidate,work,reserved,gap)) continue;
                if (std::any_of(slots.begin(),slots.end(),[&](RECT slot){return overlaps(candidate,slot,gap);})) continue;
                found=candidate;break;
            }
            if (!IsRectEmpty(&found)) break;
        }
        slots.push_back(found); // Unplaceable cards stay hidden; never cover Shell.
    }
    return slots;
}
inline RECT constrain_widget(RECT requested, RECT fallback, RECT work, RECT reserved, int gap) {
    int width=requested.right-requested.left, height=requested.bottom-requested.top;
    if (width>work.right-work.left-gap*2 || height>work.bottom-work.top-gap*2) return fallback;
    int x=std::clamp(int(requested.left),int(work.left)+gap,int(work.right)-gap-width);
    int y=std::clamp(int(requested.top),int(work.top)+gap,int(work.bottom)-gap-height);
    RECT result{x,y,x+width,y+height};
    if (allowed_slot(result,work,reserved,gap)) return result;
    std::vector<RECT> alternatives{
        {reserved.left-gap-width,y,reserved.left-gap,y+height},
        {reserved.right+gap,y,reserved.right+gap+width,y+height},
        {x,reserved.top-gap-height,x+width,reserved.top-gap},
        {x,reserved.bottom+gap,x+width,reserved.bottom+gap+height}};
    auto distance=[&](RECT a){return std::abs(double(a.left)-requested.left)+std::abs(double(a.top)-requested.top);};
    std::sort(alternatives.begin(),alternatives.end(),[&](RECT a,RECT b){return distance(a)<distance(b);});
    for (auto alternative:alternatives) if (allowed_slot(alternative,work,reserved,gap)) return alternative;
    return fallback;
}
}
