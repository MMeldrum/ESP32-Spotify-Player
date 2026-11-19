// Groove Breaker: a breakout-style easter egg. Drag the paddle along the bottom to keep the ball in play
// and clear a 12x8 wall of bricks sampled from the current album cover.
//
// Everything is drawn by one draw callback on the game screen (no LVGL object per brick), and each frame
// only invalidates what moved: the ball, the paddle and any brick that broke. The panel copy then only
// touches those rows, so the game keeps up with the 15 ms refresh.
#include "game.h"

#if BOARD_HAS_GAME
#include <Arduino.h>
#include <math.h>

LV_FONT_DECLARE(FONT_UI_SMALL)
LV_FONT_DECLARE(FONT_UI_LARGE)

namespace {

constexpr int COLS = 12;
constexpr int ROWS = 8;
constexpr int FIELD_X = 12;
constexpr int FIELD_TOP = 48;  // below the score row
constexpr int CELL_W = (SCREEN_W - 2 * FIELD_X) / COLS;
constexpr int CELL_H = 22;
constexpr int BRICKS_Y = FIELD_TOP + 22;
constexpr int BRICK_GAP = 3;
constexpr int PADDLE_W = 92;
constexpr int PADDLE_H = 14;
constexpr int PADDLE_Y = SCREEN_H - 40;
constexpr int BALL_R = 7;
constexpr float START_SPEED = 300.0f;  // px/s
constexpr float LEVEL_SPEEDUP = 1.08f;
constexpr float MAX_SPEED = 620.0f;
constexpr float MAX_BOUNCE = 1.05f;  // radians from vertical at the paddle's ends (~60 degrees)
constexpr int START_LIVES = 3;
constexpr uint32_t TICK_MS = 15;

const lv_color_t BG_TOP = lv_color_hex(0x1B1B3A);
const lv_color_t BG_BOTTOM = lv_color_hex(0x07070D);

enum class Phase { Serve, Play, LevelClear, GameOver };

struct Game {
    lv_obj_t *screen = nullptr;
    lv_obj_t *lblScore = nullptr;
    lv_obj_t *lblLives = nullptr;
    lv_obj_t *lblMessage = nullptr;
    lv_timer_t *timer = nullptr;
    void (*onExit)() = nullptr;

    lv_color_t brickColor[ROWS][COLS];
    bool brickAlive[ROWS][COLS];
    int bricksLeft = 0;

    lv_color_t accent;
    float paddleX = SCREEN_W / 2;  // centre
    float ballX = 0, ballY = 0, velX = 0, velY = 0, speed = START_SPEED;
    Phase phase = Phase::Serve;
    int level = 1;
    int score = 0;
    int lives = START_LIVES;
    uint32_t lastTick = 0;
};

Game *game = nullptr;
int bestScore = 0;  // for this power-on

lv_area_t brickArea(int r, int c) {
    lv_area_t a;
    a.x1 = FIELD_X + c * CELL_W + BRICK_GAP / 2;
    a.y1 = BRICKS_Y + r * CELL_H + BRICK_GAP / 2;
    a.x2 = a.x1 + CELL_W - BRICK_GAP - 1;
    a.y2 = a.y1 + CELL_H - BRICK_GAP - 1;
    return a;
}

lv_area_t paddleArea(float x) {
    lv_area_t a;
    a.x1 = (lv_coord_t)lroundf(x) - PADDLE_W / 2;
    a.x2 = a.x1 + PADDLE_W - 1;
    a.y1 = PADDLE_Y;
    a.y2 = PADDLE_Y + PADDLE_H - 1;
    return a;
}

lv_area_t ballArea(float x, float y) {
    lv_area_t a;
    a.x1 = (lv_coord_t)lroundf(x) - BALL_R;
    a.y1 = (lv_coord_t)lroundf(y) - BALL_R;
    a.x2 = a.x1 + 2 * BALL_R;
    a.y2 = a.y1 + 2 * BALL_R;
    return a;
}

void invalidate(lv_area_t a, int grow = 2) {
    a.x1 -= grow;
    a.y1 -= grow;
    a.x2 += grow;
    a.y2 += grow;
    lv_obj_invalidate_area(game->screen, &a);
}

// Bricks take the colours of the cover, squeezed into the 12x8 grid, and are lifted to a minimum
// brightness so they stand out from the dark background. Without art, each row gets its own hue.
void colourBricks(const lv_color_t *art, int artW, int artH) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            lv_color_t colour;
            if (art && artW > 0 && artH > 0) {
                int x0 = c * artW / COLS, x1 = (c + 1) * artW / COLS;
                int y0 = r * artH / ROWS, y1 = (r + 1) * artH / ROWS;
                uint32_t sr = 0, sg = 0, sb = 0, n = 0;
                for (int y = y0; y < y1; y += 3) {
                    for (int x = x0; x < x1; x += 3) {
                        lv_color_t p = art[y * artW + x];
                        sr += LV_COLOR_GET_R(p) * 255 / 31;
                        sg += LV_COLOR_GET_G(p) * 255 / 63;
                        sb += LV_COLOR_GET_B(p) * 255 / 31;
                        n++;
                    }
                }
                n = n ? n : 1;
                int rr = sr / n, gg = sg / n, bb = sb / n;
                int luma = (rr * 3 + gg * 6 + bb) / 10;
                if (luma < 90) {
                    // Mix towards white until it is bright enough to read as a brick.
                    int mix = (90 - luma) * 255 / 165;
                    rr += (255 - rr) * mix / 255;
                    gg += (255 - gg) * mix / 255;
                    bb += (255 - bb) * mix / 255;
                }
                colour = lv_color_make(rr, gg, bb);
            } else {
                colour = lv_color_hsv_to_rgb(r * 360 / ROWS, 70, 95);
            }
            game->brickColor[r][c] = colour;
        }
    }
}

void setMessage(const char *text) {
    if (text && *text) {
        lv_label_set_text(game->lblMessage, text);
        lv_obj_clear_flag(game->lblMessage, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(game->lblMessage, LV_OBJ_FLAG_HIDDEN);
    }
}

void updateHud() {
    char buf[48];
    snprintf(buf, sizeof(buf), "Level %d   %d", game->level, game->score);
    lv_label_set_text(game->lblScore, buf);
    String hearts;
    for (int i = 0; i < game->lives; i++) hearts += "\xE2\x99\xA5";  // U+2665
    lv_label_set_text(game->lblLives, hearts.c_str());
}

void restoreWall() {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) game->brickAlive[r][c] = true;
    game->bricksLeft = ROWS * COLS;
#ifdef SIMULATOR
    // SIM_GAME_BRICKS=n keeps only the bottom n bricks, for checking level changes quickly.
    if (const char *keep = getenv("SIM_GAME_BRICKS")) {
        int n = constrain(atoi(keep), 1, ROWS * COLS);
        for (int i = 0; i < ROWS * COLS - n; i++) game->brickAlive[i / COLS][i % COLS] = false;
        game->bricksLeft = n;
    }
#endif
    lv_obj_invalidate(game->screen);
}

// The ball waits on the paddle until a tap launches it.
void serve() {
    invalidate(ballArea(game->ballX, game->ballY));
    game->phase = Phase::Serve;
    game->ballX = game->paddleX;
    game->ballY = PADDLE_Y - BALL_R - 1;
    invalidate(ballArea(game->ballX, game->ballY));
}

void newGame() {
    game->level = 1;
    game->score = 0;
    game->lives = START_LIVES;
    game->speed = START_SPEED;
    restoreWall();
    serve();
    updateHud();
    setMessage("Drag to move, tap to launch");
}

void launch() {
    // Straight up with a little sideways drift, alternating sides.
    float angle = (game->score / 10 % 2 ? 0.35f : -0.35f);
    game->velX = game->speed * sinf(angle);
    game->velY = -game->speed * cosf(angle);
    game->phase = Phase::Play;
    setMessage(nullptr);
}

void breakBrick(int r, int c) {
    game->brickAlive[r][c] = false;
    game->bricksLeft--;
    game->score += 10 * game->level;
    if (game->score > bestScore) bestScore = game->score;
    invalidate(brickArea(r, c), 1);
    // A little faster every 12 bricks, up to the cap.
    if (game->bricksLeft % 12 == 0) game->speed = fminf(game->speed * 1.03f, MAX_SPEED);
    updateHud();
}

// Moves the ball up to `step` px, bouncing off walls, bricks and the paddle. Small steps keep the ball from
// passing through a brick between two frames.
void moveBall(float dt) {
    Game &g = *game;
    float mag = sqrtf(g.velX * g.velX + g.velY * g.velY);
    if (mag > 0) {  // keep the speed exact after bounces and speed-ups
        g.velX *= g.speed / mag;
        g.velY *= g.speed / mag;
    }
    float dist = g.speed * dt;
    int steps = max(1, (int)ceilf(dist / 3.0f));
    float sx = g.velX * dt / steps, sy = g.velY * dt / steps;

    for (int i = 0; i < steps && g.phase == Phase::Play; i++) {
        float nx = g.ballX + sx, ny = g.ballY + sy;

        // Walls
        if (nx - BALL_R < 0) {
            nx = BALL_R;
            g.velX = fabsf(g.velX);
            sx = fabsf(sx);
        } else if (nx + BALL_R > SCREEN_W - 1) {
            nx = SCREEN_W - 1 - BALL_R;
            g.velX = -fabsf(g.velX);
            sx = -fabsf(sx);
        }
        if (ny - BALL_R < FIELD_TOP) {
            ny = FIELD_TOP + BALL_R;
            g.velY = fabsf(g.velY);
            sy = fabsf(sy);
        }

        // Bricks: the cells the ball's box touches. The bounce follows the side it came in from.
        int c0 = (int)floorf((nx - BALL_R - FIELD_X) / CELL_W), c1 = (int)floorf((nx + BALL_R - FIELD_X) / CELL_W);
        int r0 = (int)floorf((ny - BALL_R - BRICKS_Y) / CELL_H), r1 = (int)floorf((ny + BALL_R - BRICKS_Y) / CELL_H);
        bool hit = false;
        for (int r = max(0, r0); r <= min(ROWS - 1, r1) && !hit; r++) {
            for (int c = max(0, c0); c <= min(COLS - 1, c1) && !hit; c++) {
                if (!g.brickAlive[r][c]) continue;
                lv_area_t b = brickArea(r, c);
                float cx = constrain(nx, (float)b.x1, (float)b.x2), cy = constrain(ny, (float)b.y1, (float)b.y2);
                float dx = nx - cx, dy = ny - cy;
                if (dx * dx + dy * dy > BALL_R * BALL_R) continue;
                breakBrick(r, c);
                hit = true;
                // Entered through the top/bottom if it was vertically outside the brick before the step.
                bool fromSide = g.ballY >= b.y1 && g.ballY <= b.y2;
                if (fromSide) {
                    g.velX = -g.velX;
                    sx = -sx;
                    nx = g.ballX;
                } else {
                    g.velY = -g.velY;
                    sy = -sy;
                    ny = g.ballY;
                }
            }
        }

        // Paddle: the bounce angle depends on where the ball lands, so the player can aim.
        if (g.velY > 0 && ny + BALL_R >= PADDLE_Y && g.ballY + BALL_R <= PADDLE_Y + 2) {
            float off = (nx - g.paddleX) / (PADDLE_W / 2 + BALL_R);
            if (fabsf(off) <= 1.0f) {
                float angle = off * MAX_BOUNCE;
                g.velX = g.speed * sinf(angle);
                g.velY = -g.speed * cosf(angle);
                sx = g.velX * dt / steps;
                sy = g.velY * dt / steps;
                ny = PADDLE_Y - BALL_R - 1;
            }
        }

        g.ballX = nx;
        g.ballY = ny;

        if (g.bricksLeft == 0) {
            g.phase = Phase::LevelClear;
            g.level++;
#ifdef SIMULATOR
            fprintf(stderr, "Groove Breaker: level %d cleared at %u ms, score %d, lives %d\n", g.level - 1, (unsigned)lv_tick_get(), g.score, g.lives);
#endif
            g.speed = fminf(START_SPEED * powf(LEVEL_SPEEDUP, g.level - 1), MAX_SPEED);
            char buf[48];
            snprintf(buf, sizeof(buf), "Level %d!\nTap to continue", g.level);
            setMessage(buf);
            updateHud();
        } else if (g.ballY - BALL_R > SCREEN_H) {
            g.lives--;
            updateHud();
#ifdef SIMULATOR
            fprintf(stderr, "Groove Breaker: ball lost at %u ms (x %.0f, paddle %.0f)\n", (unsigned)lv_tick_get(), g.ballX, g.paddleX);
#endif
            if (g.lives <= 0) {
                g.phase = Phase::GameOver;
                char buf[80];
                snprintf(buf, sizeof(buf), "Game over\nScore %d  \xC2\xB7  Best %d\nTap to play again", g.score, bestScore);
                setMessage(buf);
            } else {
                serve();
                setMessage("Tap to launch");
            }
        }
    }
}

void movePaddle(lv_coord_t x);
void onPhaseTap();

void tick(lv_timer_t *) {
    if (!game) return;
    uint32_t now = lv_tick_get();
    float dt = min<uint32_t>(lv_tick_elaps(game->lastTick), 50) / 1000.0f;  // no huge jump after a stall
    game->lastTick = now;
#ifdef SIMULATOR
    // SIM_GAME_AUTOPLAY=1: the paddle follows the ball (a little off-centre, to vary the angles) and every
    // pause is tapped through, for checking bounces and levels.
    static const bool autoplay = getenv("SIM_GAME_AUTOPLAY") != nullptr;
    if (autoplay) {
        if (game->phase != Phase::Play) onPhaseTap();
        movePaddle((lv_coord_t)(game->ballX + ((game->score / 10) % 5 - 2) * 12));
    }
#endif
    if (game->phase != Phase::Play) return;
    lv_area_t before = ballArea(game->ballX, game->ballY);
    moveBall(dt);
    invalidate(before);
    invalidate(ballArea(game->ballX, game->ballY));
}

void movePaddle(lv_coord_t x) {
    float target = constrain((float)x, (float)PADDLE_W / 2, (float)(SCREEN_W - PADDLE_W / 2));
    if (target == game->paddleX) return;
    invalidate(paddleArea(game->paddleX));
    game->paddleX = target;
    invalidate(paddleArea(game->paddleX));
    if (game->phase == Phase::Serve) {
        invalidate(ballArea(game->ballX, game->ballY));
        game->ballX = game->paddleX;
        invalidate(ballArea(game->ballX, game->ballY));
    }
}

// Pressing anywhere moves the paddle to the finger; a tap (press and release without much movement)
// launches the ball or moves on after a level or game over.
void onTouch(lv_event_t *e) {
    if (!game) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        movePaddle(p.x);
    } else if (code == LV_EVENT_CLICKED) {
        onPhaseTap();
    }
}

// A tap launches the ball, or moves on after a cleared level or game over.
void onPhaseTap() {
    switch (game->phase) {
        case Phase::Serve: launch(); break;
        case Phase::LevelClear:
            restoreWall();
            serve();
            setMessage("Tap to launch");
            break;
        case Phase::GameOver: newGame(); break;
        case Phase::Play: break;
    }
}

void onDraw(lv_event_t *e) {
    if (!game) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const lv_area_t *clip = ctx->clip_area;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 4;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (!game->brickAlive[r][c]) continue;
            lv_area_t a = brickArea(r, c);
            lv_area_t common;
            if (!_lv_area_intersect(&common, &a, clip)) continue;
            dsc.bg_color = game->brickColor[r][c];
            lv_draw_rect(ctx, &dsc, &a);
        }
    }

    lv_area_t pad = paddleArea(game->paddleX);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = game->accent;
    lv_draw_rect(ctx, &dsc, &pad);

    if (game->phase != Phase::GameOver) {
        lv_area_t ball = ballArea(game->ballX, game->ballY);
        dsc.bg_color = lv_color_white();
        lv_draw_rect(ctx, &dsc, &ball);
    }
}

void onBack(lv_event_t *) {
    if (game && game->onExit) game->onExit();
}

void onUnloaded(lv_event_t *) {
    if (!game) return;
    lv_timer_del(game->timer);
    lv_obj_del_async(game->screen);
    delete game;
    game = nullptr;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

} // namespace

void game_open(const lv_color_t *art, int artW, int artH, lv_color_t accent, void (*onExit)()) {
    if (game) return;
    game = new Game();
    game->onExit = onExit;
    game->accent = accent;
    colourBricks(art, artW, artH);

    lv_obj_t *scr = lv_obj_create(nullptr);
    game->screen = scr;
    lv_obj_set_style_bg_color(scr, BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(scr, onDraw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(scr, onTouch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, onTouch, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(scr, onTouch, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(scr, onUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_pos(back, 12, 8);
    lv_obj_set_size(back, 60, 34);
    lv_obj_set_style_bg_color(back, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLbl = makeLabel(back, &FONT_UI_SMALL, lv_color_white());
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_center(backLbl);

    game->lblScore = makeLabel(scr, &FONT_UI_SMALL, lv_color_white());
    lv_obj_align(game->lblScore, LV_ALIGN_TOP_MID, 0, 14);
    game->lblLives = makeLabel(scr, &FONT_UI_SMALL, accent);
    lv_obj_align(game->lblLives, LV_ALIGN_TOP_RIGHT, -16, 14);

    game->lblMessage = makeLabel(scr, &FONT_UI_LARGE, lv_color_white());
    lv_obj_set_width(game->lblMessage, SCREEN_W - 40);
    lv_obj_set_style_text_align(game->lblMessage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(game->lblMessage, LV_ALIGN_CENTER, 0, 70);
    lv_obj_clear_flag(game->lblMessage, LV_OBJ_FLAG_CLICKABLE);

    game->lastTick = lv_tick_get();
    game->timer = lv_timer_create(tick, TICK_MS, nullptr);
    newGame();
    lv_scr_load(scr);
}

#endif
