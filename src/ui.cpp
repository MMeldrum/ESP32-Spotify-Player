#include "ui.h"
#include "app_log.h"
#include "board.h"
#include "config.h"
#include "clock_time.h"
#include "display.h"
#include "web_portal.h"
#include "led.h"
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <lvgl.h>

LV_FONT_DECLARE(FONT_UI_SMALL)
LV_FONT_DECLARE(FONT_UI_LARGE)
LV_FONT_DECLARE(FONT_CLOCK)

namespace {

// Layout values are designed for 320x240 and scaled to the board's screen.
constexpr int S(int v) { return v * UI_SCALE_NUM / UI_SCALE_DEN; }
// Height left over when the scaled layout is shorter than the screen (square panels).
constexpr int EXTRA_H = SCREEN_H - S(240);
constexpr int BG_MAX_W = ART_BUFFER_W;
constexpr int BG_MAX_H = (BG_MAX_W * SCREEN_H + SCREEN_W - 1) / SCREEN_W + 1;
constexpr uint32_t OPTIMISTIC_MS = 2500;
constexpr int LIST_TOP = S(42);
constexpr int LIST_ROW_PITCH = S(34);
constexpr int LIST_FOOTER_Y = SCREEN_H - S(27);
constexpr int LIST_ROWS = (LIST_FOOTER_Y - LIST_TOP) / LIST_ROW_PITCH;

const lv_color_t DEFAULT_ACCENT = lv_color_hex(0x1DB954);
const lv_color_t DEFAULT_TINT = lv_color_hex(0x1B1B3A);
const lv_color_t BG_BOTTOM = lv_color_hex(0x07070D);
const lv_color_t TEXT_MAIN = lv_color_hex(0xFFFFFF);
const lv_color_t TEXT_DIM = lv_color_hex(0xE0E0EC);
const lv_color_t TEXT_WARN = lv_color_hex(0xFFB347);

lv_obj_t *screenSetup = nullptr;
lv_obj_t *lblSetupInstructions;
lv_obj_t *lblSetupIp;

lv_obj_t *screenPlayer = nullptr;
lv_obj_t *bgImg;
lv_obj_t *dimLayer;
lv_obj_t *lblTitle;
lv_obj_t *lblArtist;
lv_obj_t *lblMessage;
lv_obj_t *lblVolume;
lv_obj_t *barProgress;
lv_obj_t *lblElapsed;
lv_obj_t *lblTotal;
lv_obj_t *btnPlay;
lv_obj_t *lblPlayPause;

lv_obj_t *screenList = nullptr;
lv_obj_t *screenSettings = nullptr;
lv_obj_t *sliderBrightness;
lv_obj_t *lblBrightness;
lv_obj_t *switchNight;
lv_obj_t *ddNightFrom;
lv_obj_t *ddNightTo;
lv_obj_t *sliderNight;
lv_obj_t *lblNightBrightness;
lv_obj_t *ddTimeZone;
lv_obj_t *ddIdle;
lv_obj_t *lblClockNow;
bool nightActive = false;
int appliedBrightness = -1;
lv_obj_t *lblListTitle;
lv_obj_t *lblListStatus;
lv_obj_t *lblListPage;
lv_obj_t *listRowBtn[LIST_ROWS];
lv_obj_t *listRowLbl[LIST_ROWS];

lv_obj_t *popup = nullptr;
String (*infoProvider)() = nullptr;
void (*resetHandler)() = nullptr;
uint32_t resetArmedUntil = 0;

lv_color_t *bgPixels = nullptr;
lv_img_dsc_t bgDsc;
int bgW = BG_MAX_W;
int bgH = BG_MAX_H;
int decodedW = 1;
int cropY = 0;
uint32_t sumR, sumG, sumB, pixelCount;
lv_color_t curAccent = DEFAULT_ACCENT;
bool ledArmed = false;

PlayerState lastState;
String shownTrackId;
String shownTitle;
String shownArtist;
String shownMessage;
String tempMessage;
String hintText;
uint32_t tempMessageUntil = 0;
bool shownPlaying = false;
int shownVolume = -2;
int shownElapsedSec = -1;
int shownTotalSec = -1;
uint32_t optimisticUntil = 0;
uint32_t seekBaseMs = 0;
uint32_t seekStamp = 0;
uint32_t seekUntil = 0;

ListKind listKind = LIST_DEVICES;
std::vector<ListItem> listItems;
bool listLoading = false;
bool playlistsFetched = false;
String listErrorText;
uint32_t playlistNext = 0;
uint32_t playlistTotal = 0;
int listPage = 0;
uint32_t listShownVersion = 0;
constexpr size_t MAX_LOADED_PLAYLISTS = 100;

// While music plays, the bottom controls fade out after a while without touches; the first tap brings
// them back. The black layer over the album art is lighter while they are hidden.
constexpr uint32_t CONTROLS_HIDE_MS = 5000;
constexpr uint32_t FADE_MS = 400;
// The backlight switches off after this long without touches; a touch or a new track switches it back on.
constexpr uint32_t SCREEN_OFF_MS = 10 * 60 * 1000;
constexpr lv_opa_t DIM_WITH_CONTROLS = LV_OPA_40;
constexpr lv_opa_t DIM_WITHOUT_CONTROLS = LV_OPA_20;
lv_obj_t *controlsPanel;
lv_obj_t *textBox;
// How far the track text slides down when the controls are hidden: to just above the screen's bottom edge.
// Square screens have room for a second row of controls (shuffle, like, repeat) above the progress bar.
constexpr bool EXTRA_ROW = EXTRA_H > 0;
constexpr int ROW_OFF = EXTRA_ROW ? S(28) : 0;
constexpr int PANEL_H = S(64) + ROW_OFF;
constexpr int PANEL_Y = SCREEN_H - S(6) - PANEL_H;
constexpr int TEXT_DROP = PANEL_H + S(2);
lv_obj_t *btnShuffle = nullptr;
lv_obj_t *btnLike = nullptr;
lv_obj_t *btnRepeat = nullptr;
bool shownShuffle = false;
RepeatMode shownRepeat = REPEAT_OFF;
int8_t shownLiked = -1;
lv_obj_t *wakeCatcher;
bool controlsShown = true;
bool bgVisible = false;

// Idle clock (screen built further down).
lv_obj_t *screenClock = nullptr;
void showClock();
void updateClock();
void syncClockBackground();

bool optimisticActive() { return (int32_t)(millis() - optimisticUntil) < 0; }

uint32_t nextCodepoint(const char *&p) {
    uint8_t c = *p++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 3 ? (c & 0x07) : extra == 2 ? (c & 0x0F) : extra == 1 ? (c & 0x1F) : 0xFFFD;
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    return cp;
}

// Drops characters the UI font lacks (emoji, CJK) so they do not render as boxes.
String cleanText(const String &in, const char *fallback = "") {
    String out;
    out.reserve(in.length());
    const char *p = in.c_str();
    bool pendingSpace = false;
    while (*p) {
        const char *start = p;
        uint32_t cp = nextCodepoint(p);
        if (cp == ' ' || cp == '\t') {
            pendingSpace = out.length() > 0;
            continue;
        }
        bool keep = cp == '\n';
        if (!keep && cp >= 0x20) {
            lv_font_glyph_dsc_t dsc;
            keep = lv_font_get_glyph_dsc(&FONT_UI_SMALL, &dsc, cp, 0);
        }
        if (!keep) continue;
        if (pendingSpace) out += ' ';
        pendingSpace = false;
        out.concat(start, p - start);
    }
    return out.length() ? out : String(fallback);
}

void applyGlass(lv_obj_t *o, int radius, lv_opa_t fill) {
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, fill, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makeGlass(lv_obj_t *parent, int x, int y, int w, int h, int radius, lv_opa_t fill = LV_OPA_10) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    applyGlass(o, radius, fill);
    return o;
}

void styleScreen(lv_obj_t *scr, lv_color_t top) {
    lv_obj_set_style_bg_color(scr, top, 0);
    lv_obj_set_style_bg_grad_color(scr, BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makeLabel(lv_obj_t *parent, int x, int y, int w, lv_color_t color, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    if (w > 0) lv_obj_set_width(lbl, w);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

void makeSingleLine(lv_obj_t *lbl, const lv_font_t *font) {
    lv_obj_set_height(lbl, lv_font_get_line_height(font));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
}

// A line of the text column: as tall as its text, up to maxH, then cut off with dots.
lv_obj_t *makeTextBlock(lv_obj_t *parent, int maxH, lv_color_t color, const lv_font_t *font) {
    lv_obj_t *lbl = makeLabel(parent, 0, 0, 0, color, font);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(lbl, maxH, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

lv_obj_t *makeGlassButton(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb, void *userData) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    applyGlass(btn, LV_RADIUS_CIRCLE, LV_OPA_20);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, userData);
    return btn;
}

lv_obj_t *makeButtonLabel(lv_obj_t *btn, const char *text) {
    lv_obj_t *lbl = makeLabel(btn, 0, 0, 0, TEXT_MAIN, &FONT_UI_SMALL);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return lbl;
}

void setToggleLabel(lv_obj_t *btn, const char *text, lv_color_t color, lv_opa_t opa) {
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_opa(lbl, opa, 0);
}

// Shuffle, like and repeat light up in the album colour when on.
void refreshExtraRow() {
    if (!btnShuffle) return;
    setToggleLabel(btnShuffle, LV_SYMBOL_SHUFFLE, shownShuffle ? curAccent : TEXT_MAIN, shownShuffle ? LV_OPA_COVER : LV_OPA_60);
    setToggleLabel(btnRepeat, shownRepeat == REPEAT_TRACK ? LV_SYMBOL_LOOP " 1" : LV_SYMBOL_LOOP,
                   shownRepeat != REPEAT_OFF ? curAccent : TEXT_MAIN, shownRepeat != REPEAT_OFF ? LV_OPA_COVER : LV_OPA_60);
    // Filled heart when liked, outline when not, faint while unknown.
    setToggleLabel(btnLike, shownLiked == 1 ? "\xE2\x99\xA5" : "\xE2\x99\xA1", shownLiked == 1 ? curAccent : TEXT_MAIN,
                   shownLiked < 0 ? LV_OPA_30 : shownLiked ? LV_OPA_COVER : LV_OPA_60);
}

void applyTheme(lv_color_t accent, lv_color_t tint) {
    curAccent = accent;
    if (ledArmed) led_set_color(lv_color_to32(accent) & 0xFFFFFF);
    styleScreen(screenPlayer, tint);
    lv_obj_set_style_bg_color(barProgress, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(btnPlay, accent, 0);
    lv_obj_set_style_text_color(lblVolume, accent, 0);
    refreshExtraRow();
}

void execOpa(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, v, 0); }
void execBgOpa(void *obj, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)obj, v, 0); }
void execTranslateY(void *obj, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0); }

// Animates from the current value, so a fade that is interrupted reverses smoothly.
void animateTo(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t time) {
    lv_anim_del(obj, exec);
    if (!time || from == to) {
        exec(obj, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void updateDim(uint32_t time) {
    lv_opa_t target = !bgVisible ? LV_OPA_TRANSP : controlsShown ? DIM_WITH_CONTROLS : DIM_WITHOUT_CONTROLS;
    animateTo(dimLayer, execBgOpa, lv_obj_get_style_bg_opa(dimLayer, 0), target, time);
}

void setControlsShown(bool shown, uint32_t time) {
    controlsShown = shown;
    animateTo(controlsPanel, execOpa, lv_obj_get_style_opa(controlsPanel, 0), shown ? LV_OPA_COVER : LV_OPA_TRANSP, time);
    // The text jumps instead of sliding: up at once when the controls return, down once they have faded out.
    lv_anim_del(textBox, execTranslateY);
    if (shown || !time) {
        execTranslateY(textBox, shown ? 0 : TEXT_DROP);
    } else {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, textBox);
        lv_anim_set_exec_cb(&a, execTranslateY);
        lv_anim_set_values(&a, 0, TEXT_DROP);
        lv_anim_set_time(&a, time);
        lv_anim_set_path_cb(&a, lv_anim_path_step);
        lv_anim_start(&a);
    }
    // While hidden, a full-screen layer takes the first tap so it does not also press a control.
    if (shown) {
        lv_obj_add_flag(wakeCatcher, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(wakeCatcher, LV_OBJ_FLAG_HIDDEN);
    }
    updateDim(time);
}

void onWakeTap(lv_event_t *) { setControlsShown(true, FADE_MS); }

// A touch that switched the screen on also brings the controls back, so the next tap can use them.
void onTouchWake() {
    if (lv_scr_act() == screenPlayer) setControlsShown(true, 0);
}

void updateNightMode(bool force);
void updateSettingsClock();

void controlsTimerCb(lv_timer_t *) {
    // During a firmware update the flash writes keep stalling the PSRAM the panel scans from, which makes
    // the picture jump, so the backlight stays off; the web page shows the progress.
    int updatePercent = 0;
    if (web_portal_update_progress(updatePercent)) {
        display_set_screen_on(false);
        return;
    }
    // After a while without touches the screen switches off, or shows the clock if that is chosen.
    // The setup screen stays lit: it shows what to connect to.
    if (display_screen_on() && lv_scr_act() != screenSetup && lv_disp_get_inactive_time(nullptr) >= SCREEN_OFF_MS) {
        if (!g_config.idleClock) {
            display_set_screen_on(false);
        } else if (lv_scr_act() != screenClock) {
            showClock();
        }
    }
    updateNightMode(false);
    if (lv_scr_act() == screenSettings) updateSettingsClock();
    if (lv_scr_act() == screenClock) updateClock();
    if (lv_scr_act() != screenPlayer) return;
    bool playing = lastState.active && shownPlaying;
    if (!controlsShown) {
        if (!playing) setControlsShown(true, FADE_MS);
        return;
    }
    if (playing && !popup && lv_disp_get_inactive_time(nullptr) >= CONTROLS_HIDE_MS) setControlsShown(false, FADE_MS);
}

void setBackgroundVisible(bool visible) {
    bgVisible = visible;
    if (visible) {
        lv_obj_clear_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    }
    updateDim(0);
    syncClockBackground();
}

void closePopup(lv_event_t *) {
    if (!popup) return;
    lv_obj_del_async(popup);
    popup = nullptr;
}

void onResetTap(lv_event_t *e) {
    lv_obj_t *label = lv_obj_get_child(lv_event_get_target(e), 0);
    if ((int32_t)(millis() - resetArmedUntil) < 0) {
        lv_label_set_text(label, "Working...");
        if (resetHandler) resetHandler();
        return;
    }
    resetArmedUntil = millis() + 4000;
    lv_label_set_text(label, "Tap to confirm");
}

void showPopup(const char *title, const String &body, const char *footer, bool withReset = false) {
    if (popup) lv_obj_del(popup);

    popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(popup, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(popup, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_80, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_radius(popup, 0, 0);
    lv_obj_set_style_pad_all(popup, 0, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(popup, closePopup, LV_EVENT_CLICKED, nullptr);

    const int cardW = SCREEN_W - S(24);
    const int cardH = SCREEN_H - S(24);
    const int textW = cardW - S(32);
    lv_obj_t *card = makeGlass(popup, S(12), S(12), cardW, cardH, S(22), LV_OPA_COVER);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x181822), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lblT = makeLabel(card, S(16), S(8), textW, curAccent, &FONT_UI_LARGE);
    makeSingleLine(lblT, &FONT_UI_LARGE);
    lv_label_set_text(lblT, title);

    lv_obj_t *lblB = makeLabel(card, S(16), S(36), textW, TEXT_MAIN, &FONT_UI_SMALL);
    lv_obj_set_height(lblB, cardH - S(64));
    lv_label_set_long_mode(lblB, LV_LABEL_LONG_DOT);
    lv_label_set_text(lblB, body.c_str());

    lv_obj_t *lblF = makeLabel(card, S(16), cardH - S(26), textW, TEXT_DIM, &FONT_UI_SMALL);
    makeSingleLine(lblF, &FONT_UI_SMALL);
    lv_label_set_text(lblF, footer);

    if (withReset) {
        lv_obj_set_height(lblB, cardH - S(82));
        lv_obj_set_width(lblF, S(110));
        lv_obj_t *btn = makeGlassButton(card, cardW - S(156), cardH - S(40), S(140), S(30), onResetTap, nullptr);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xB3261E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        makeButtonLabel(btn, "Forget Wi-Fi");
    }
}

void onLongPress(lv_event_t *) {
    if (popup || !infoProvider) return;
    showPopup("Device info", infoProvider(), "Tap to close", true);
}

String formatDuration(uint32_t ms) {
    uint32_t sec = ms / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    return buf;
}

void onTrackTap(lv_event_t *) {
    if (popup || !lastState.active) return;
    const PlayerState &s = lastState;

    String t;
    if (s.artist.length()) t += s.artist + "\n";
    if (s.album.length()) {
        t += "Album: " + s.album;
        if (s.albumType.length()) t += " (" + s.albumType + ")";
        t += "\n";
    }
    if (s.releaseDate.length()) t += "Released: " + s.releaseDate + "\n";
    if (s.trackNumber) {
        t += "Track " + String(s.trackNumber);
        if (s.totalTracks) t += " of " + String(s.totalTracks);
        if (s.discNumber > 1) t += ", disc " + String(s.discNumber);
        t += "\n";
    }
    t += "Length: " + formatDuration(s.durationMs) + (s.isExplicit ? "  Explicit" : "") + "\n";
    if (s.isrc.length()) t += "ISRC: " + s.isrc + "\n";

    showPopup(cleanText(s.title, "Unknown").c_str(), cleanText(t), "Tap to close  -  Data from Spotify");
}

void buildSetupUi() {
    screenSetup = lv_obj_create(nullptr);
    styleScreen(screenSetup, DEFAULT_TINT);
    lv_obj_add_event_cb(screenSetup, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);

    lv_obj_t *title = makeLabel(screenSetup, 0, 0, 0, DEFAULT_ACCENT, &FONT_UI_LARGE);
    lv_label_set_text(title, "Device Setup");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, S(16));

    lv_obj_t *card = makeGlass(screenSetup, S(16), S(56), SCREEN_W - S(32), SCREEN_H - S(72), S(22));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lblSetupInstructions = makeLabel(card, 0, 0, SCREEN_W - S(64), TEXT_MAIN, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblSetupInstructions, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lblSetupInstructions, LV_ALIGN_TOP_MID, 0, S(18) + EXTRA_H / 4);

    lblSetupIp = makeLabel(card, 0, 0, SCREEN_W - S(64), DEFAULT_ACCENT, &FONT_UI_LARGE);
    lv_obj_set_style_text_align(lblSetupIp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lblSetupIp, LV_ALIGN_BOTTOM_MID, 0, -S(18) - EXTRA_H / 4);
}

void setPlayIcon() {
    lv_label_set_text(lblPlayPause, shownPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void setVolumeLabel() {
    char buf[24];
    if (shownVolume < 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MID " --");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MID " %d%%", shownVolume);
    }
    lv_label_set_text(lblVolume, buf);
}

void onSeekTap(lv_event_t *) {
    if (!lastState.active || lastState.durationMs == 0) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t area;
    lv_obj_get_coords(barProgress, &area);
    int width = lv_area_get_width(&area);
    int rel = constrain(point.x - area.x1, 0, width);
    uint32_t target = (uint64_t)lastState.durationMs * rel / width;

    seekBaseMs = target;
    seekStamp = millis();
    seekUntil = millis() + OPTIMISTIC_MS;
    spotify_seek(target);
}

void onButton(lv_event_t *e) {
    SpotifyCmd cmd = (SpotifyCmd)(uintptr_t)lv_event_get_user_data(e);
    bool volumeCmd = cmd == CMD_VOL_UP || cmd == CMD_VOL_DOWN;
    if (volumeCmd && lastState.active && !lastState.volumeAllowed) {
        tempMessage = VOLUME_NOT_ALLOWED;
        tempMessageUntil = millis() + 3000;
        ui_update_player(lastState);
        return;
    }
    if (cmd == CMD_LIKE && lastState.active && shownLiked < 0) {
        if (lastState.likeNeedsLogin || lastState.isEpisode) {
            tempMessage = lastState.isEpisode ? "Podcast episodes can't be liked here" : LIKE_NEEDS_LOGIN;
            tempMessageUntil = millis() + 4000;
            ui_update_player(lastState);
        }
        return;
    }
    if (cmd == CMD_SHUFFLE) {
        shownShuffle = !shownShuffle;
        optimisticUntil = millis() + OPTIMISTIC_MS;
        refreshExtraRow();
    } else if (cmd == CMD_REPEAT) {
        shownRepeat = (RepeatMode)((shownRepeat + 1) % 3);
        optimisticUntil = millis() + OPTIMISTIC_MS;
        refreshExtraRow();
    } else if (cmd == CMD_LIKE) {
        shownLiked = shownLiked ? 0 : 1;
        optimisticUntil = millis() + OPTIMISTIC_MS;
        refreshExtraRow();
    } else if (cmd == CMD_PLAY_PAUSE) {
        shownPlaying = !shownPlaying;
        setPlayIcon();
        optimisticUntil = millis() + OPTIMISTIC_MS;
    } else if ((cmd == CMD_VOL_UP || cmd == CMD_VOL_DOWN) && shownVolume >= 0) {
        shownVolume = constrain(shownVolume + (cmd == CMD_VOL_UP ? 10 : -10), 0, 100);
        setVolumeLabel();
        optimisticUntil = millis() + OPTIMISTIC_MS;
    }
    spotify_send(cmd);
}

// Swiping left skips to the next track, swiping right goes back. Works with the controls hidden too.
void onSwipe(lv_event_t *) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || popup || !lastState.active) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
    // Do not also click whatever the swipe started on.
    lv_indev_wait_release(indev);
    spotify_send(dir == LV_DIR_LEFT ? CMD_NEXT : CMD_PREV);
    tempMessage = dir == LV_DIR_LEFT ? LV_SYMBOL_NEXT " Next track" : LV_SYMBOL_PREV " Previous track";
    tempMessageUntil = millis() + 1500;
    ui_update_player(lastState);
}

lv_obj_t *makeControl(lv_obj_t *parent, int x, int w, SpotifyCmd cmd, const char *text) {
    lv_obj_t *btn = makeGlassButton(parent, S(x), S(26) + ROW_OFF, S(w), S(34), onButton, (void *)(uintptr_t)cmd);
    makeButtonLabel(btn, text);
    return btn;
}

void showPlayerScreen() {
    setControlsShown(true, 0);
    lv_scr_load(screenPlayer);
}

bool hasMoreRow() {
    return listKind == LIST_PLAYLISTS && (!playlistsFetched || playlistNext < playlistTotal) && listItems.size() < MAX_LOADED_PLAYLISTS;
}

void renderList() {
    bool moreRow = hasMoreRow();
    int total = listItems.size() + (moreRow ? 1 : 0);
    int pages = max(1, (total + LIST_ROWS - 1) / LIST_ROWS);
    listPage = constrain(listPage, 0, pages - 1);

    String status;
    if (total == 0) {
        if (listLoading) {
            status = "Loading...";
        } else if (listErrorText.length()) {
            status = listErrorText;
        } else {
            status = listKind == LIST_DEVICES ? "No devices found. Open Spotify on a device." : "No playlists found.";
        }
    }
    lv_label_set_text(lblListStatus, status.c_str());

    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = listPage * LIST_ROWS + i;
        String text;
        if (idx < (int)listItems.size()) {
            const ListItem &item = listItems[idx];
            String prefix = item.active ? LV_SYMBOL_OK " " : item.pinned ? "\xE2\x98\x85 " : "";
            text = prefix + cleanText(item.name, "(unsupported name)");
        } else if (idx == (int)listItems.size() && moreRow) {
            if (listLoading) {
                text = "Loading...";
            } else if (listErrorText.length()) {
                text = "Failed, tap to retry: " + listErrorText;
            } else if (playlistsFetched) {
                text = LV_SYMBOL_DOWNLOAD " Load more (" + String(playlistTotal - playlistNext) + " left)";
            } else {
                text = LV_SYMBOL_DOWNLOAD " All my playlists...";
            }
        } else {
            lv_obj_add_flag(listRowBtn[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_label_set_text(listRowLbl[i], text.c_str());
        lv_obj_clear_flag(listRowBtn[i], LV_OBJ_FLAG_HIDDEN);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", listPage + 1, pages);
    lv_label_set_text(lblListPage, buf);
}

void onRowTap(lv_event_t *e) {
    int idx = listPage * LIST_ROWS + (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx == (int)listItems.size() && hasMoreRow()) {
        if (listLoading) return;
        listLoading = true;
        listErrorText = "";
        spotify_request_list(LIST_PLAYLISTS, playlistsFetched ? playlistNext : 0);
        listShownVersion = spotify_list_version(LIST_PLAYLISTS);
        renderList();
        return;
    }
    if (idx >= (int)listItems.size()) return;
    spotify_select(listKind, listItems[idx].id);
    tempMessage = listKind == LIST_DEVICES ? "Switching device..." : "Starting playlist...";
    tempMessageUntil = millis() + 4000;
    showPlayerScreen();
}

void onPageTap(lv_event_t *e) {
    listPage += (int)(intptr_t)lv_event_get_user_data(e);
    renderList();
}

void openList(ListKind kind) {
    listKind = kind;
    listPage = 0;
    listItems.clear();
    listErrorText = "";
    playlistsFetched = false;
    playlistNext = 0;
    playlistTotal = 0;
    listLoading = false;
    lv_label_set_text(lblListTitle, kind == LIST_DEVICES ? "Devices" : "Playlists");

    if (kind == LIST_PLAYLISTS) {
        for (const Pin &pin : config_get_pins()) {
            ListItem item;
            item.id = pin.id;
            item.name = pin.name;
            item.pinned = true;
            listItems.push_back(std::move(item));
        }
    }
    if (kind == LIST_DEVICES || listItems.empty()) {
        listLoading = true;
        spotify_request_list(kind, 0);
    }
    listShownVersion = spotify_list_version(kind);
    renderList();
    lv_scr_load(screenList);
}

void applyListResult(const ListData &data) {
    listLoading = false;
    if (data.status == LIST_ERROR) {
        listErrorText = data.error;
    } else if (listKind == LIST_DEVICES) {
        listItems = data.items;
        listErrorText = "";
    } else {
        listErrorText = "";
        size_t firstNew = listItems.size();
        for (const ListItem &item : data.items) {
            bool known = false;
            for (const ListItem &existing : listItems) {
                if (existing.id == item.id) {
                    known = true;
                    break;
                }
            }
            if (!known) listItems.push_back(item);
        }
        playlistsFetched = true;
        playlistNext = data.next;
        playlistTotal = data.items.empty() ? data.next : data.total;
        listPage = firstNew / LIST_ROWS;
    }
    renderList();
}

void onOpenDevices(lv_event_t *) { openList(LIST_DEVICES); }
void onOpenPlaylists(lv_event_t *) { openList(LIST_PLAYLISTS); }
void onBack(lv_event_t *) { showPlayerScreen(); }

void setPercentLabel(lv_obj_t *lbl, int value) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    lv_label_set_text(lbl, buf);
}

bool isNightHour(int hour) {
    int from = g_config.nightFrom, to = g_config.nightTo;
    if (from == to) return false;
    return from < to ? (hour >= from && hour < to) : (hour >= from || hour < to);
}

// Applies the day or night brightness. Checked every timer tick; only touches the backlight on a change.
void updateNightMode(bool force) {
    struct tm now;
    bool night = g_config.nightMode && time_now(now) && isNightHour(now.tm_hour);
    // While a slider is being dragged it previews its own value.
    if (!force && lv_scr_act() == screenSettings &&
        (lv_obj_has_state(sliderBrightness, LV_STATE_PRESSED) || lv_obj_has_state(sliderNight, LV_STATE_PRESSED))) {
        return;
    }
    int level = night ? g_config.nightBrightness : g_config.screenBrightness;
    if (force || night != nightActive || level != appliedBrightness) {
        nightActive = night;
        appliedBrightness = level;
        display_set_brightness(level);
    }
}

void updateSettingsClock() {
    static int shownMinute = -2;
    struct tm now;
    int minute = time_now(now) ? now.tm_hour * 60 + now.tm_min : -1;
    if (minute == shownMinute) return;
    shownMinute = minute;
    char buf[24];
    if (minute < 0) {
        snprintf(buf, sizeof(buf), "Time not set yet");
    } else {
        snprintf(buf, sizeof(buf), "Now %02d:%02d", now.tm_hour, now.tm_min);
    }
    lv_label_set_text(lblClockNow, buf);
}

// Sliders change the backlight while dragging (as a preview) and save when the finger lifts.
void onBrightness(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    bool isNight = slider == sliderNight;
    int value = lv_slider_get_value(slider);
    display_set_brightness(value);
    setPercentLabel(isNight ? lblNightBrightness : lblBrightness, value);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    int &stored = isNight ? g_config.nightBrightness : g_config.screenBrightness;
    if (value != stored) {
        stored = value;
        config_save();
    }
    updateNightMode(true);
}

void onNightSetting(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target == switchNight) {
        g_config.nightMode = lv_obj_has_state(switchNight, LV_STATE_CHECKED);
    } else if (target == ddNightFrom) {
        g_config.nightFrom = lv_dropdown_get_selected(ddNightFrom);
    } else if (target == ddNightTo) {
        g_config.nightTo = lv_dropdown_get_selected(ddNightTo);
    } else if (target == ddIdle) {
        g_config.idleClock = lv_dropdown_get_selected(ddIdle) == 1;
    } else if (target == ddTimeZone) {
        g_config.timeZone = lv_dropdown_get_selected(ddTimeZone);
        time_set_zone(g_config.timeZone);
        updateSettingsClock();
    }
    config_save();
    updateNightMode(true);
}

void onOpenSettings(lv_event_t *) {
    lv_slider_set_value(sliderBrightness, g_config.screenBrightness, LV_ANIM_OFF);
    setPercentLabel(lblBrightness, g_config.screenBrightness);
    lv_slider_set_value(sliderNight, g_config.nightBrightness, LV_ANIM_OFF);
    setPercentLabel(lblNightBrightness, g_config.nightBrightness);
    if (g_config.nightMode) {
        lv_obj_add_state(switchNight, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(switchNight, LV_STATE_CHECKED);
    }
    lv_dropdown_set_selected(ddNightFrom, g_config.nightFrom);
    lv_dropdown_set_selected(ddNightTo, g_config.nightTo);
    lv_dropdown_set_selected(ddTimeZone, constrain(g_config.timeZone, 0, TIME_ZONE_COUNT - 1));
    lv_dropdown_set_selected(ddIdle, g_config.idleClock ? 1 : 0);
    updateSettingsClock();
    lv_obj_scroll_to_y(screenSettings, 0, LV_ANIM_OFF);
    lv_scr_load(screenSettings);
}

lv_obj_t *makeSettingsSlider(lv_obj_t *card, int y, int minValue) {
    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, S(20), y);
    lv_obj_set_size(slider, SCREEN_W - S(24) - S(40), S(8));
    lv_slider_set_range(slider, minValue, 100);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, DEFAULT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, S(6), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, S(14));
    lv_obj_add_event_cb(slider, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, onBrightness, LV_EVENT_RELEASED, nullptr);
    return slider;
}

lv_obj_t *makeDropdown(lv_obj_t *parent, int x, int y, int w, const char *options) {
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, S(28));
    lv_dropdown_set_options(dd, options);
    lv_obj_set_style_text_font(dd, &FONT_UI_SMALL, 0);
    lv_obj_set_style_text_color(dd, TEXT_MAIN, 0);
    lv_obj_set_style_bg_color(dd, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_20, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_radius(dd, S(10), 0);
    lv_obj_set_style_pad_ver(dd, S(4), 0);
    lv_obj_set_style_pad_hor(dd, S(8), 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &FONT_UI_SMALL, 0);
    lv_obj_set_style_text_color(list, TEXT_MAIN, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x282830), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_max_height(list, S(140), 0);
    lv_obj_set_style_bg_color(list, DEFAULT_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, DEFAULT_ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_event_cb(dd, onNightSetting, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

void buildSettingsUi() {
    screenSettings = lv_obj_create(nullptr);
    styleScreen(screenSettings, DEFAULT_TINT);
    // Scrolls on screens too short for all the cards (the CYD).
    lv_obj_add_flag(screenSettings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(screenSettings, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(screenSettings, S(10), 0);

    lv_obj_t *back = makeGlassButton(screenSettings, S(10), S(6), S(44), S(28), onBack, nullptr);
    makeButtonLabel(back, LV_SYMBOL_LEFT);
    lv_obj_t *title = makeLabel(screenSettings, S(62), S(9), SCREEN_W - S(140), TEXT_MAIN, &FONT_UI_LARGE);
    makeSingleLine(title, &FONT_UI_LARGE);
    lv_label_set_text(title, "Settings");

    const int cardW = SCREEN_W - S(24);
    const int valueX = cardW - S(74);

    // Brightness
    lv_obj_t *card = makeGlass(screenSettings, S(12), S(44), cardW, S(64), S(18));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *lbl = makeLabel(card, S(14), S(8), S(180), TEXT_MAIN, &FONT_UI_SMALL);
    lv_label_set_text(lbl, LV_SYMBOL_IMAGE "  Brightness");
    lblBrightness = makeLabel(card, valueX, S(8), S(60), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblBrightness, LV_TEXT_ALIGN_RIGHT, 0);
    sliderBrightness = makeSettingsSlider(card, S(40), DISPLAY_MIN_BRIGHTNESS);

    // Night mode
    card = makeGlass(screenSettings, S(12), S(116), cardW, S(126), S(18));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lbl = makeLabel(card, S(14), S(9), S(180), TEXT_MAIN, &FONT_UI_SMALL);
    lv_label_set_text(lbl, "\xE2\x98\xBE  Night mode");
    switchNight = lv_switch_create(card);
    lv_obj_set_pos(switchNight, cardW - S(60), S(6));
    lv_obj_set_size(switchNight, S(46), S(24));
    lv_obj_set_style_bg_color(switchNight, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(switchNight, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(switchNight, DEFAULT_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(switchNight, onNightSetting, LV_EVENT_VALUE_CHANGED, nullptr);

    static char hours[24 * 6 + 1];
    char *p = hours;
    for (int h = 0; h < 24; h++) p += sprintf(p, h ? "\n%02d:00" : "%02d:00", h);
    lbl = makeLabel(card, S(14), S(44), S(50), TEXT_DIM, &FONT_UI_SMALL);
    lv_label_set_text(lbl, "From");
    ddNightFrom = makeDropdown(card, S(60), S(40), S(84), hours);
    lbl = makeLabel(card, S(152), S(44), S(30), TEXT_DIM, &FONT_UI_SMALL);
    lv_label_set_text(lbl, "to");
    ddNightTo = makeDropdown(card, S(178), S(40), S(84), hours);

    lbl = makeLabel(card, S(14), S(76), S(200), TEXT_MAIN, &FONT_UI_SMALL);
    lv_label_set_text(lbl, "Brightness at night");
    lblNightBrightness = makeLabel(card, valueX, S(76), S(60), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblNightBrightness, LV_TEXT_ALIGN_RIGHT, 0);
    sliderNight = makeSettingsSlider(card, S(106), DISPLAY_MIN_BRIGHTNESS);

    // Time zone (for night mode and the clock)
    card = makeGlass(screenSettings, S(12), S(250), cardW, S(72), S(18));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lbl = makeLabel(card, S(14), S(8), S(150), TEXT_MAIN, &FONT_UI_SMALL);
    lv_label_set_text(lbl, "Time zone");
    lblClockNow = makeLabel(card, cardW - S(144), S(8), S(130), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblClockNow, LV_TEXT_ALIGN_RIGHT, 0);
    String zones;
    for (int i = 0; i < TIME_ZONE_COUNT; i++) {
        if (i) zones += "\n";
        zones += TIME_ZONES[i].name;
    }
    ddTimeZone = makeDropdown(card, S(14), S(36), cardW - S(28), zones.c_str());

    // What happens after 10 minutes without touches
    card = makeGlass(screenSettings, S(12), S(330), cardW, S(72), S(18));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lbl = makeLabel(card, S(14), S(8), cardW - S(28), TEXT_MAIN, &FONT_UI_SMALL);
    lv_label_set_text(lbl, "After 10 minutes idle");
    ddIdle = makeDropdown(card, S(14), S(36), cardW - S(28), "Turn the screen off\nShow the clock");
}

// --- Idle clock ---

lv_obj_t *clockBg;
lv_obj_t *clockTime;
lv_obj_t *clockDate;
lv_obj_t *clockTrack;
int clockShownMinute = -2;
String clockShownTrack;

// The clock shows the player's album art behind it (same pixels, darkened).
void syncClockBackground() {
    if (!screenClock) return;
    if (lv_obj_has_flag(bgImg, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(clockBg, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_img_set_src(clockBg, &bgDsc);
    lv_img_set_antialias(clockBg, true);
    lv_img_set_zoom(clockBg, lv_img_get_zoom(bgImg));
    lv_obj_set_pos(clockBg, lv_obj_get_x(bgImg), lv_obj_get_y(bgImg));
    lv_obj_clear_flag(clockBg, LV_OBJ_FLAG_HIDDEN);
}

void updateClock() {
    struct tm now;
    bool known = time_now(now);
    int minute = known ? now.tm_yday * 1440 + now.tm_hour * 60 + now.tm_min : -1;
    if (minute != clockShownMinute) {
        clockShownMinute = minute;
        if (known) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
            lv_label_set_text(clockTime, buf);
            char weekday[16], month[16];
            strftime(weekday, sizeof(weekday), "%A", &now);
            strftime(month, sizeof(month), "%B", &now);
            snprintf(buf, sizeof(buf), "%s %d %s", weekday, now.tm_mday, month);
            lv_label_set_text(clockDate, buf);
        } else {
            lv_label_set_text(clockTime, "--:--");
            lv_label_set_text(clockDate, "Waiting for the time");
        }
    }
    String track;
    if (lastState.active) {
        track = "\xE2\x99\xAA " + cleanText(lastState.title, "Unknown");
        String artist = cleanText(lastState.artist);
        if (artist.length()) track += "\n" + artist;
    }
    if (track != clockShownTrack) {
        clockShownTrack = track;
        lv_label_set_text(clockTrack, track.c_str());
    }
}

void showClock() {
    closePopup(nullptr);
    clockShownMinute = -2;
    syncClockBackground();
    updateClock();
    lv_scr_load(screenClock);
}

// A tap on the clock goes back to the player without pressing anything there.
void onClockTap(lv_event_t *) {
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_wait_release(indev);
    showPlayerScreen();
}

void buildClockUi() {
    screenClock = lv_obj_create(nullptr);
    styleScreen(screenClock, DEFAULT_TINT);
    lv_obj_add_event_cb(screenClock, onClockTap, LV_EVENT_PRESSED, nullptr);

    clockBg = lv_img_create(screenClock);
    lv_obj_add_flag(clockBg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *shade = lv_obj_create(screenClock);
    lv_obj_remove_style_all(shade);
    lv_obj_set_size(shade, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
    lv_obj_clear_flag(shade, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    clockTime = makeLabel(screenClock, 0, 0, 0, TEXT_MAIN, &FONT_CLOCK);
    lv_label_set_text(clockTime, "--:--");
    lv_obj_align(clockTime, LV_ALIGN_CENTER, 0, -S(30) - EXTRA_H / 6);
    clockDate = makeLabel(screenClock, 0, 0, SCREEN_W - S(32), TEXT_DIM, &FONT_UI_LARGE);
    lv_obj_set_style_text_align(clockDate, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(clockDate, "");
    lv_obj_align_to(clockDate, clockTime, LV_ALIGN_OUT_BOTTOM_MID, 0, S(4));
    clockTrack = makeLabel(screenClock, 0, 0, SCREEN_W - S(32), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(clockTrack, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(clockTrack, 2 * lv_font_get_line_height(&FONT_UI_SMALL));
    lv_label_set_long_mode(clockTrack, LV_LABEL_LONG_DOT);
    lv_label_set_text(clockTrack, "");
    lv_obj_align(clockTrack, LV_ALIGN_BOTTOM_MID, 0, -S(14));
}

void buildListUi() {
    screenList = lv_obj_create(nullptr);
    styleScreen(screenList, DEFAULT_TINT);

    lv_obj_t *back = makeGlassButton(screenList, S(10), S(6), S(44), S(28), onBack, nullptr);
    makeButtonLabel(back, LV_SYMBOL_LEFT);

    lblListTitle = makeLabel(screenList, S(62), S(9), SCREEN_W - S(140), TEXT_MAIN, &FONT_UI_LARGE);
    makeSingleLine(lblListTitle, &FONT_UI_LARGE);
    lv_obj_t *brand = makeLabel(screenList, SCREEN_W - S(70), S(12), S(60), DEFAULT_ACCENT, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(brand, "Spotify");

    for (int i = 0; i < LIST_ROWS; i++) {
        lv_obj_t *btn = lv_btn_create(screenList);
        lv_obj_set_pos(btn, S(12), LIST_TOP + i * LIST_ROW_PITCH);
        lv_obj_set_size(btn, SCREEN_W - S(24), S(32));
        applyGlass(btn, S(14), LV_OPA_10);
        lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, onRowTap, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *lbl = makeLabel(btn, S(12), 0, SCREEN_W - S(50), TEXT_MAIN, &FONT_UI_SMALL);
        makeSingleLine(lbl, &FONT_UI_SMALL);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, S(12), 0);
        listRowBtn[i] = btn;
        listRowLbl[i] = lbl;
    }

    lblListStatus = makeLabel(screenList, S(16), S(90), SCREEN_W - S(32), TEXT_WARN, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblListStatus, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lblListStatus, "");

    lv_obj_t *prev = makeGlassButton(screenList, S(12), LIST_FOOTER_Y, S(60), S(24), onPageTap, (void *)(intptr_t)-1);
    makeButtonLabel(prev, LV_SYMBOL_LEFT);
    lv_obj_t *next = makeGlassButton(screenList, SCREEN_W - S(72), LIST_FOOTER_Y, S(60), S(24), onPageTap, (void *)(intptr_t)1);
    makeButtonLabel(next, LV_SYMBOL_RIGHT);
    lblListPage = makeLabel(screenList, (SCREEN_W - S(120)) / 2, LIST_FOOTER_Y + S(3), S(120), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblListPage, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lblListPage, "1/1");
}

void buildPlayerUi() {
    screenPlayer = lv_obj_create(nullptr);
    styleScreen(screenPlayer, DEFAULT_TINT);
    lv_obj_add_event_cb(screenPlayer, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(screenPlayer, onSwipe, LV_EVENT_GESTURE, nullptr);

    bgDsc.header.always_zero = 0;
    bgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    bgDsc.header.w = bgW;
    bgDsc.header.h = bgH;
    bgDsc.data_size = bgW * bgH * sizeof(lv_color_t);
    bgDsc.data = (const uint8_t *)bgPixels;

    bgImg = lv_img_create(screenPlayer);
    lv_img_set_src(bgImg, &bgDsc);
    lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);

    dimLayer = lv_obj_create(screenPlayer);
    lv_obj_set_size(dimLayer, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(dimLayer, 0, 0);
    lv_obj_set_style_bg_color(dimLayer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dimLayer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dimLayer, 0, 0);
    lv_obj_set_style_radius(dimLayer, 0, 0);
    lv_obj_clear_flag(dimLayer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lblVolume = makeLabel(screenPlayer, S(14), S(10), S(62), DEFAULT_ACCENT, &FONT_UI_SMALL);
    makeSingleLine(lblVolume, &FONT_UI_SMALL);

    lv_obj_t *btnDevice = makeGlassButton(screenPlayer, SCREEN_W - S(122), S(5), S(54), S(28), onOpenDevices, nullptr);
    makeButtonLabel(btnDevice, LV_SYMBOL_AUDIO);

    lv_obj_t *btnLists = makeGlassButton(screenPlayer, SCREEN_W - S(64), S(5), S(54), S(28), onOpenPlaylists, nullptr);
    makeButtonLabel(btnLists, LV_SYMBOL_LIST);

    lv_obj_t *btnSettings = makeGlassButton(screenPlayer, SCREEN_W - S(180), S(5), S(54), S(28), onOpenSettings, nullptr);
    makeButtonLabel(btnSettings, LV_SYMBOL_SETTINGS);

    // The text sits in a column anchored just above the controls: any message, the title (up to two lines),
    // then the artist. Empty lines are hidden, so the text always ends right above the controls.
    const int controlsY = PANEL_Y;
    textBox = lv_obj_create(screenPlayer);
    lv_obj_remove_style_all(textBox);
    lv_obj_set_pos(textBox, S(16), S(38));
    lv_obj_set_size(textBox, SCREEN_W - S(32), controlsY - S(6) - S(38));
    lv_obj_clear_flag(textBox, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_flex_flow(textBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(textBox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(textBox, S(2), 0);

    int titleH = max(S(50), 2 * (int)lv_font_get_line_height(&FONT_UI_LARGE));
    lblMessage = makeTextBlock(textBox, S(36), TEXT_WARN, &FONT_UI_SMALL);
    lblTitle = makeTextBlock(textBox, titleH, TEXT_MAIN, &FONT_UI_LARGE);
    lblArtist = makeTextBlock(textBox, S(20), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_add_flag(lblMessage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblArtist, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *lbl : {lblTitle, lblArtist}) {
        lv_obj_add_flag(lbl, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
        lv_obj_add_event_cb(lbl, onTrackTap, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *panel = makeGlass(screenPlayer, S(10), PANEL_Y, S(300), PANEL_H, S(24), LV_OPA_20);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    controlsPanel = panel;

    if (EXTRA_ROW) {
        auto makeToggle = [&](int x, SpotifyCmd cmd) {
            lv_obj_t *btn = makeGlassButton(panel, S(x), S(4), S(50), S(24), onButton, (void *)(uintptr_t)cmd);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            makeButtonLabel(btn, "");
            return btn;
        };
        btnShuffle = makeToggle(65, CMD_SHUFFLE);
        btnLike = makeToggle(125, CMD_LIKE);
        btnRepeat = makeToggle(185, CMD_REPEAT);
    }

    lblElapsed = makeLabel(panel, S(12), S(3) + ROW_OFF, S(36), TEXT_DIM, &FONT_UI_SMALL);
    lv_label_set_text(lblElapsed, "0:00");

    barProgress = lv_bar_create(panel);
    lv_obj_set_pos(barProgress, S(52), S(10) + ROW_OFF);
    lv_obj_set_size(barProgress, S(196), S(8));
    lv_bar_set_range(barProgress, 0, 1000);
    lv_obj_set_style_bg_color(barProgress, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(barProgress, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(barProgress, DEFAULT_ACCENT, LV_PART_INDICATOR);

    lv_obj_t *seekArea = lv_obj_create(panel);
    lv_obj_set_pos(seekArea, S(44), ROW_OFF);
    lv_obj_set_size(seekArea, S(212), S(26));
    lv_obj_set_style_bg_opa(seekArea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(seekArea, 0, 0);
    lv_obj_set_style_pad_all(seekArea, 0, 0);
    lv_obj_clear_flag(seekArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seekArea, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(seekArea, onSeekTap, LV_EVENT_CLICKED, nullptr);

    lblTotal = makeLabel(panel, S(252), S(3) + ROW_OFF, S(36), TEXT_DIM, &FONT_UI_SMALL);
    lv_obj_set_style_text_align(lblTotal, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lblTotal, "0:00");

    makeControl(panel, 9, 46, CMD_VOL_DOWN, LV_SYMBOL_VOLUME_MID);
    makeControl(panel, 64, 46, CMD_PREV, LV_SYMBOL_PREV);
    btnPlay = makeControl(panel, 119, 62, CMD_PLAY_PAUSE, LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_opa(btnPlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btnPlay, 0, 0);
    lblPlayPause = lv_obj_get_child(btnPlay, 0);
    lv_obj_set_style_text_color(lblPlayPause, BG_BOTTOM, 0);
    makeControl(panel, 190, 46, CMD_NEXT, LV_SYMBOL_NEXT);
    makeControl(panel, 245, 46, CMD_VOL_UP, LV_SYMBOL_VOLUME_MAX);

    wakeCatcher = lv_obj_create(screenPlayer);
    lv_obj_set_pos(wakeCatcher, 0, 0);
    lv_obj_set_size(wakeCatcher, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_opa(wakeCatcher, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wakeCatcher, 0, 0);
    lv_obj_set_style_radius(wakeCatcher, 0, 0);
    lv_obj_clear_flag(wakeCatcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wakeCatcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wakeCatcher, onWakeTap, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(wakeCatcher, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);
    lv_timer_create(controlsTimerCb, 250, nullptr);
    display_on_touch_wake(onTouchWake);

    setVolumeLabel();
    applyTheme(DEFAULT_ACCENT, DEFAULT_TINT);
}

void formatTime(char *out, size_t size, int seconds) {
    snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// Scales the decoded image to bgW wide (nearest pixel) and keeps the middle bgH rows.
bool artCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    for (int row = 0; row < h; row++) {
        int by = (y + row) * bgW / decodedW - cropY;
        if (by < 0 || by >= bgH) continue;
        for (int col = 0; col < w; col++) {
            int bx = (x + col) * bgW / decodedW;
            if (bx >= bgW) continue;
            uint16_t p = bitmap[row * w + col];
            bgPixels[by * bgW + bx].full = p;
            sumR += (p >> 11) & 0x1F;
            sumG += (p >> 5) & 0x3F;
            sumB += p & 0x1F;
            pixelCount++;
        }
    }
    return true;
}

void themeFromArt() {
    if (!pixelCount) return;
    lv_color_t avg = lv_color_make(((sumR / pixelCount) * 255) / 31, ((sumG / pixelCount) * 255) / 63, ((sumB / pixelCount) * 255) / 31);
    lv_color_hsv_t hsv = lv_color_to_hsv(avg);
    uint8_t accentSat = hsv.s < 20 ? hsv.s : min<int>(100, hsv.s + 25);
    applyTheme(lv_color_hsv_to_rgb(hsv.h, accentSat, 95), lv_color_hsv_to_rgb(hsv.h, min<int>(100, hsv.s + 10), 30));
}

} // namespace

void ui_init() {
#ifdef BOARD_HAS_PSRAM
    bgPixels = (lv_color_t *)ps_malloc(BG_MAX_W * BG_MAX_H * sizeof(lv_color_t));
#else
    bgPixels = (lv_color_t *)malloc(BG_MAX_W * BG_MAX_H * sizeof(lv_color_t));
#endif
    TJpgDec.setCallback(artCallback);
    buildSetupUi();
    buildPlayerUi();
    buildListUi();
    buildSettingsUi();
    buildClockUi();
}

void ui_set_info_provider(String (*provider)()) {
    infoProvider = provider;
}

void ui_set_hint(const char *text) {
    hintText = text;
}

void ui_set_reset_handler(void (*handler)()) {
    resetHandler = handler;
}

void show_setup_screen() {
    display_set_screen_on(true);
    ledArmed = false;
    led_set_color(0);
    lv_scr_load(screenSetup);
}

void update_setup_screen(const char *ip_address, bool is_ap_mode) {
    if (is_ap_mode) {
        lv_label_set_text(lblSetupInstructions, "1. Connect to Wi-Fi:\n'Spotify-Player-Setup'\n\n2. Open browser to:");
    } else {
        lv_label_set_text(lblSetupInstructions, "Wi-Fi Connected!\n\nTo configure Spotify,\nopen browser to:");
    }
    lv_label_set_text(lblSetupIp, ip_address);
}

void ui_set_setup_status(const char *text) {
    lv_label_set_text(lblSetupInstructions, text);
    lv_label_set_text(lblSetupIp, "");
}

void show_player_screen() {
    ledArmed = true;
    led_set_color(lv_color_to32(curAccent) & 0xFFFFFF);
    showPlayerScreen();
}

// Sets a line of the text column, hiding it when empty so the lines below stay above the controls.
static void setLine(lv_obj_t *lbl, const String &text) {
    lv_label_set_text(lbl, text.c_str());
    if (text.length()) {
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_player(const PlayerState &s) {
    lastState = s;

    String title = s.active ? cleanText(s.title, "Unknown") : String("Nothing playing");
    String artist = s.active ? cleanText(s.artist) : String();
    if (title != shownTitle) {
        shownTitle = title;
        lv_label_set_text(lblTitle, title.c_str());
    }
    if (artist != shownArtist) {
        shownArtist = artist;
        setLine(lblArtist, artist);
    }

    bool tempActive = (int32_t)(millis() - tempMessageUntil) < 0;
    const String &rawMessage = tempActive ? tempMessage : (hintText.length() ? hintText : s.message);
    String message = cleanText(rawMessage);
    if (message != shownMessage) {
        shownMessage = message;
        setLine(lblMessage, message);
    }

    if (!optimisticActive()) {
        if (s.playing != shownPlaying) {
            shownPlaying = s.playing;
            setPlayIcon();
        }
        if (s.volume != shownVolume) {
            shownVolume = s.volume;
            setVolumeLabel();
        }
        int8_t liked = s.active ? s.liked : -1;
        if (s.shuffle != shownShuffle || s.repeat != shownRepeat || liked != shownLiked) {
            shownShuffle = s.shuffle;
            shownRepeat = s.repeat;
            shownLiked = liked;
            refreshExtraRow();
        }
    }
    if (s.trackId != shownTrackId) {
        shownTrackId = s.trackId;
        // A new track counts as activity: the screen comes on and stays on for another minute.
        if (s.trackId.length()) {
            lv_disp_trig_activity(nullptr);
            display_set_screen_on(true);
        }
        if (s.artUrl.isEmpty()) {
            setBackgroundVisible(false);
            applyTheme(DEFAULT_ACCENT, DEFAULT_TINT);
        }
    }

    uint32_t progress = s.progressMs;
    if (s.playing) progress += millis() - s.stamp;
    if ((int32_t)(millis() - seekUntil) < 0) {
        progress = seekBaseMs + (s.playing ? millis() - seekStamp : 0);
    }
    if (s.durationMs && progress > s.durationMs) progress = s.durationMs;

    lv_bar_set_value(barProgress, s.durationMs ? (int32_t)((uint64_t)progress * 1000 / s.durationMs) : 0, LV_ANIM_OFF);

    int elapsed = progress / 1000;
    if (elapsed != shownElapsedSec) {
        shownElapsedSec = elapsed;
        char buf[12];
        formatTime(buf, sizeof(buf), elapsed);
        lv_label_set_text(lblElapsed, buf);
    }
    int total = s.durationMs / 1000;
    if (total != shownTotalSec) {
        shownTotalSec = total;
        char buf[12];
        formatTime(buf, sizeof(buf), total);
        lv_label_set_text(lblTotal, buf);
    }

    if (lv_scr_act() == screenList) {
        uint32_t version = spotify_list_version(listKind);
        if (version != listShownVersion) {
            listShownVersion = version;
            ListData data;
            spotify_get_list(listKind, data);
            if (data.status == LIST_READY || data.status == LIST_ERROR) applyListResult(data);
        }
    }
}

void ui_show_art() {
    if (!bgPixels) {
        app_log(LL_ERROR, "Art: no background buffer");
        return;
    }
    size_t artSize = 0;
    const uint8_t *art = spotify_art_data(artSize);
    uint16_t w = 0, h = 0;
    JRESULT res = art ? TJpgDec.getJpgSize(&w, &h, art, artSize) : TJpgDec.getFsJpgSize(&w, &h, SPOTIFY_ART_PATH, LittleFS);
    if (res != JDR_OK) {
        app_log(LL_ERROR, "Art: JPEG header error %d", res);
        return;
    }

    // Decode at the smallest JPEG scale that is still at least the buffer width, then scale the whole
    // image to the buffer width (480 px on the 4848S040 gives a 1:1 picture). Smaller art is stretched by LVGL.
    uint8_t scale = 1;
    while (scale < 8 && w / (scale * 2) >= BG_MAX_W) scale *= 2;
    TJpgDec.setJpgScale(scale);

    decodedW = max(1, w / scale);
    int decodedH = h / scale;
    bgW = min(decodedW, BG_MAX_W);
    int scaledH = decodedH * bgW / decodedW;
    bgH = min(scaledH, (bgW * SCREEN_H + SCREEN_W - 1) / SCREEN_W + 1);
    cropY = (scaledH - bgH) / 2;
    sumR = sumG = sumB = pixelCount = 0;

    res = art ? TJpgDec.drawJpg(0, 0, art, artSize) : TJpgDec.drawFsJpg(0, 0, SPOTIFY_ART_PATH, LittleFS);
    if (res != JDR_OK) {
        app_log(LL_ERROR, "Art: decode error %d", res);
        return;
    }

    bgDsc.header.w = bgW;
    bgDsc.header.h = bgH;
    bgDsc.data_size = bgW * bgH * sizeof(lv_color_t);
    lv_img_set_src(bgImg, &bgDsc);
    lv_img_set_antialias(bgImg, true);
    lv_img_set_zoom(bgImg, (SCREEN_W * 256) / bgW);
    lv_obj_set_pos(bgImg, (SCREEN_W - bgW) / 2, (SCREEN_H - bgH) / 2);
    setBackgroundVisible(true);
    lv_obj_invalidate(screenPlayer);
    themeFromArt();
}
