// TJpg_Decoder stand-in: decodes from memory with LVGL's TJpgDec (RGB888 output) and hands RGB565 blocks to
// the UI's callback, like the library does on the board.
#include <TJpg_Decoder.h>
#include <vector>

TJpg_Decoder TJpgDec;

namespace {

struct Source {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int32_t x, y;
};

size_t readInput(JDEC *jd, uint8_t *buf, size_t n) {
    auto *src = (Source *)jd->device;
    n = std::min(n, src->size - src->pos);
    if (buf) memcpy(buf, src->data + src->pos, n);
    src->pos += n;
    return n;
}

int writeOutput(JDEC *jd, void *bitmap, JRECT *rect) {
    auto *src = (Source *)jd->device;
    uint16_t w = rect->right - rect->left + 1;
    uint16_t h = rect->bottom - rect->top + 1;
    std::vector<uint16_t> block(w * h);
    const uint8_t *rgb = (const uint8_t *)bitmap;
    for (size_t i = 0; i < block.size(); i++, rgb += 3) {
        block[i] = (uint16_t)(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
    }
    return TJpgDec.callback_ ? TJpgDec.callback_(src->x + rect->left, src->y + rect->top, w, h, block.data()) : 1;
}

uint8_t pool[8192];

} // namespace

JRESULT TJpg_Decoder::getJpgSize(uint16_t *w, uint16_t *h, const uint8_t *data, uint32_t size) {
    Source src{data, size, 0, 0, 0};
    JDEC jd;
    JRESULT res = jd_prepare(&jd, readInput, pool, sizeof(pool), &src);
    if (res == JDR_OK) {
        *w = jd.width;
        *h = jd.height;
    }
    return res;
}

JRESULT TJpg_Decoder::drawJpg(int32_t x, int32_t y, const uint8_t *data, uint32_t size) {
    Source src{data, size, 0, x, y};
    JDEC jd;
    JRESULT res = jd_prepare(&jd, readInput, pool, sizeof(pool), &src);
    if (res != JDR_OK) return res;
    return jd_decomp(&jd, writeOutput, scale_);
}
