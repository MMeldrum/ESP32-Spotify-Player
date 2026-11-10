#pragma once
// Stand-in for Bodmer's TJpg_Decoder, using the copy of the same TJpgDec decoder that ships with LVGL.
#include <cstdint>
#include <lvgl.h>
#include "src/extra/libs/sjpg/tjpgd.h"

typedef bool (*SketchCallback)(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data);

class TJpg_Decoder {
public:
    void setCallback(SketchCallback cb) { callback_ = cb; }
    void setJpgScale(uint8_t scale) { scale_ = scale >= 8 ? 3 : scale >= 4 ? 2 : scale >= 2 ? 1 : 0; }
    JRESULT getJpgSize(uint16_t *w, uint16_t *h, const uint8_t *data, uint32_t size);
    JRESULT drawJpg(int32_t x, int32_t y, const uint8_t *data, uint32_t size);
    template <class FS> JRESULT getFsJpgSize(uint16_t *, uint16_t *, const char *, FS &) { return JDR_INP; }
    template <class FS> JRESULT drawFsJpg(int32_t, int32_t, const char *, FS &) { return JDR_INP; }

    SketchCallback callback_ = nullptr;
    uint8_t scale_ = 0;
};

extern TJpg_Decoder TJpgDec;
