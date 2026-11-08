/*
 * Turbo Term — a pseudo-3D terminal racing game in C.
 * OutRun-style: dodge traffic, carve the curves, chase the distance.
 *
 * Controls:  Up/W accelerate · Down/S brake · Left/Right or A/D steer
 *            R restart after crash · Q quit
 *
 * Public domain — see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

/* ---------- tunables ---------- */
#define SEG_LEN        200.0f
#define SEGMENTS       400
#define DRAW_DIST      180
#define ROAD_WIDTH     2200.0f
#define CAM_HEIGHT     1100.0f
#define PLAYER_Z       2.2f
#define CENTRIFUGAL    0.30f
#define MAX_SPEED      SEG_LEN * 60.0f
#define ACCEL          MAX_SPEED / 5.0f
#define DECEL          -MAX_SPEED / 4.0f
#define OFFROAD_DECEL  -MAX_SPEED / 1.5f
#define ENEMY_CARS     60

#define CAR_W          0.30f
#define ENEMY_W        0.30f

static const char *ANSI_RESET = "\x1b[0m";
static const char *C_GRASS    = "\x1b[42m";
static const char *C_ROAD     = "\x1b[48;5;238m";
static const char *C_RUMBLE_R = "\x1b[41m";
static const char *C_RUMBLE_W = "\x1b[47m";
static const char *C_LANE     = "\x1b[103m";
static const char *C_SKY      = "\x1b[44m";
static const char *C_CAR      = "\x1b[91m";
static const char *C_ENEMY1   = "\x1b[93m";
static const char *C_ENEMY2   = "\x1b[95m";
static const char *C_FOG_TXT  = "\x1b[37m";

typedef struct { float x, y, z; } Pt3;
typedef struct {
    float x, y, z;   /* camera space */
    float sx, sy, w; /* screen space */
} Proj;

typedef struct {
    int   index;
    float curve;
    Proj  p1, p2;
    float clip;
} Segment;

typedef struct {
    float z;        /* absolute z, wrapped to [0,track_length) */
    float offset;   /* -0.9..0.9 lateral */
    float speed;
    float w;
    int   hue;
} Enemy;

static Segment segments[SEGMENTS];
static float   track_length;
static float   position;
static float   player_x;
static float   speed;
static float   steer;
static int     score;
static int     game_over;
static int     started;
static int     gas, brake;
static Enemy   enemies[ENEMY_CARS];
static int     enemy_count;

static char    cell_ch[64][120];
static char    cell_st[64][120][16];
static int     scr_w = 100, scr_h = 30;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lerp(float a, float b, float t)    { return a + (b - a) * t; }
static float ease_in(float a, float b, float t) { return a + (b - a) * t * t; }
static float ease_in_out(float a, float b, float t) {
    return a + (b - a) * ((-cosf(t * (float)M_PI) / 2.0f) + 0.5f);
}
static float increase(float v, float inc, float maxv) {
    float r = fmodf(v + inc, maxv);
    return r < 0 ? r + maxv : r;
}
static float wrapf(float v, float maxv) {
    float r = fmodf(v, maxv);
    return r < 0 ? r + maxv : r;
}
static float pct_rem(float n, float m) { return fmodf(n, m) / m; }

static float camera_depth(void) {
    return 1.0f / tanf(50.0f * (float)M_PI / 180.0f); /* FOV 100 */
}

static Segment *find_segment(float z) {
    int i = (int)(z / SEG_LEN) % SEGMENTS;
    if (i < 0) i += SEGMENTS;
    return &segments[i];
}

/* deterministic curve pattern: blocks of 60 segs
   (15 ease-in, 30 hold, 15 ease-out), 10 curve shapes cycling */
static const float curve_table[10] = {0, 2, -3, 4, -2, 5, -4, 3, -5, 6};
static float curve_for(int n) {
    int block  = (n / 60) % 10;
    int within = n % 60;
    float c = curve_table[block];
    if (within < 15)  return ease_in(0, c, within / 15.0f);
    if (within >= 45) return ease_in_out(c, 0, (within - 45) / 15.0f);
    return c;
}

static void build_track(void) {
    for (int n = 0; n < SEGMENTS; n++) {
        Segment *s = &segments[n];
        s->index = n;
        s->curve = curve_for(n);
        s->clip  = 0;
        s->p1.z = n * SEG_LEN;
        s->p2.z = (n + 1) * SEG_LEN;
        s->p1.sx = s->p1.sy = s->p1.w = 0;
        s->p2.sx = s->p2.sy = s->p2.w = 0;
    }
    track_length = SEG_LEN * SEGMENTS;
}

static void project(Proj *p, float wx, float wy, float wz,
                    float cam_x, float cam_y, float cam_z) {
    float dz = wz - cam_z;
    if (dz < 0.1f) dz = 0.1f;
    float scale = camera_depth() / dz;
    p->x = wx - cam_x;
    p->y = wy - cam_y;
    p->z = dz;
    p->sx = scr_w / 2 + scale * p->x * scr_w / 2;
    p->sy = scr_h / 2 - scale * p->y * scr_h / 2;
    p->w  = scale * ROAD_WIDTH * scr_w / 2;
}

/* ---------- drawing ---------- */
static void put(int x, int y, char ch, const char *style) {
    if (x < 0 || x >= scr_w || y < 0 || y >= scr_h) return;
    cell_ch[y][x] = ch;
    strncpy(cell_st[y][x], style, 15);
    cell_st[y][x][15] = 0;
}
static void hline(int x0, int x1, int y, char ch, const char *style) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) put(x, y, ch, style);
}
static void text_at(int x, int y, const char *s, const char *style) {
    for (int i = 0; s[i] && x + i < scr_w; i++) put(x + i, y, s[i], style);
}

static int rumble_at(Segment *s) { return (s->index / 3) % 2; }

static void render_segment(Segment *s, int maxy) {
    float y1 = s->p1.sy, y2 = s->p2.sy;
    float x1 = s->p1.sx, x2 = s->p2.sx;
    float w1 = s->p1.w,  w2 = s->p2.w;
    if (y2 <= y1) return;
    int top = (int)y1, bot = (int)y2;
    if (top < scr_h / 2) top = scr_h / 2;      /* keep sky strip */
    if (bot > maxy) bot = maxy;
    if (bot <= top || top >= scr_h) return;
    if (top < 0) top = 0;

    const char *rum = rumble_at(s) ? C_RUMBLE_R : C_RUMBLE_W;

    for (int y = top; y <= bot; y++) {
        float t = (y - y1) / (y2 - y1 + 0.0001f);
        int xl = (int)lerp(x1 - w1, x2 - w2, t);
        int xr = (int)lerp(x1 + w1, x2 + w2, t);
        int xc = (int)lerp(x1, x2, t);
        int wr = (int)(lerp(w1, w2, t) * 0.18f);
        if (wr < 1) wr = 1;

        hline(0, xl - wr - 1, y, ' ', C_GRASS);
        hline(xl - wr, xl - 1, y, '#', rum);
        hline(xl, xr, y, '.', C_ROAD);
        hline(xr + 1, xr + wr, y, '#', rum);
        hline(xr + wr + 1, scr_w - 1, y, ' ', C_GRASS);

        if ((s->index % 6) < 3) {
            put(xc - 1, y, '|', C_LANE);
            put(xc,     y, '|', C_LANE);
        }
    }
}

/* draw an enemy car sprite at absolute z */
static void render_enemy(const Enemy *e) {
    Segment *s1 = find_segment(e->z);
    Segment *s2 = find_segment(e->z + SEG_LEN);
    float pct = pct_rem(e->z, SEG_LEN);
    float sx  = lerp(s1->p1.sx, s2->p1.sx, pct);
    float sy  = lerp(s1->p1.sy, s2->p1.sy, pct);
    float hw  = lerp(s1->p1.w,  s2->p1.w,  pct);   /* road half-width px */
    float scale = camera_depth() / fmaxf(lerp(s1->p1.z, s2->p1.z, pct), 0.1f);

    int cx = (int)(sx + e->offset * hw);
    int cy = (int)sy;
    int wpx = (int)(hw * e->w * 1.6f);
    if (wpx < 2) wpx = 2;
    if (wpx > 14) wpx = 14;
    int hpx = wpx / 2 + 1;
    const char *st = e->hue ? C_ENEMY1 : C_ENEMY2;
    for (int dy = 0; dy < hpx; dy++)
        hline(cx - wpx / 2, cx + wpx / 2, cy + dy, '=', st);
    put(cx, cy, 'o', st);
    (void)scale;
}

static void render_player(void) {
    /* project the player car's ground position */
    float pz = position + PLAYER_Z * SEG_LEN;
    Segment *s1 = find_segment(pz);
    Segment *s2 = find_segment(pz + SEG_LEN);
    float pct = pct_rem(pz, SEG_LEN);
    float sx  = lerp(s1->p1.sx, s2->p1.sx, pct);
    float sy  = lerp(s1->p1.sy, s2->p1.sy, pct);
    float hw  = lerp(s1->p1.w,  s2->p1.w,  pct);
    int cx = (int)(sx + player_x * hw);
    int cy = (int)sy;
    int lean = (int)(steer * 3);

    const char *st = C_CAR;
    text_at(cx - 4 + lean, cy - 4, "  ____  ", st);
    text_at(cx - 4 + lean, cy - 3, " /||==|\\", st);
    text_at(cx - 4 + lean, cy - 2, " | [] []|", st);
    text_at(cx - 4 + lean, cy - 1, " |__||__|", st);
    text_at(cx - 4 + lean, cy,     "   ||   ", st);
}

static void render_hud(void) {
    char buf[80];
    snprintf(buf, sizeof buf, "SCORE %06d", score);
    text_at(1, 0, buf, C_FOG_TXT);
    snprintf(buf, sizeof buf, "SPD %3.0f", speed / MAX_SPEED * 260.0f);
    text_at(scr_w - 11, 0, buf, C_FOG_TXT);
    text_at(1, 1, "TURBO TERM", C_FOG_TXT);

    if (!started) {
        text_at(scr_w / 2 - 12, scr_h / 2 - 2, "TURBO TERM", "\x1b[96m");
        text_at(scr_w / 2 - 17, scr_h / 2,     "UP/W gas  DOWN/S brake", C_FOG_TXT);
        text_at(scr_w / 2 - 13, scr_h / 2 + 1, "A/D or arrows steer", C_FOG_TXT);
        text_at(scr_w / 2 - 12, scr_h / 2 + 3, "press UP or W", C_FOG_TXT);
    }
    if (game_over) {
        text_at(scr_w / 2 - 6, scr_h / 2 - 1, "WRECKED!", "\x1b[91m");
        snprintf(buf, sizeof buf, "score %d", score);
        text_at(scr_w / 2 - 7, scr_h / 2 + 1, buf, C_FOG_TXT);
        text_at(scr_w / 2 - 11, scr_h / 2 + 3, "R restart  Q quit", C_FOG_TXT);
    }
}

static void render(void) {
    memset(cell_ch, ' ', sizeof cell_ch);
    memset(cell_st, 0, sizeof cell_st);

    /* sky */
    for (int y = 0; y < scr_h / 2; y++) hline(0, scr_w - 1, y, ' ', C_SKY);

    Segment *base = find_segment(position);
    float base_pct = pct_rem(position, SEG_LEN);
    float pz = position + PLAYER_Z * SEG_LEN;
    Segment *ps = find_segment(pz);
    float p_pct = pct_rem(pz, SEG_LEN);
    float player_y = lerp(ps->p1.z, ps->p2.z, p_pct); /* world y == z (flat) */

    float cam_y = player_y + CAM_HEIGHT;
    float cam_x = player_x * ROAD_WIDTH;
    float x = 0, dx = -(base->curve * base_pct);
    int maxy = scr_h;

    for (int n = 0; n < DRAW_DIST; n++) {
        Segment *s = &segments[(base->index + n) % SEGMENTS];
        int looped = s->index < base->index;
        float cam_z = position - (looped ? track_length : 0);

        project(&s->p1, 0, s->p1.z, s->p1.z, cam_x - x,      cam_y, cam_z);
        project(&s->p2, 0, s->p2.z, s->p2.z, cam_x - x - dx, cam_y, cam_z);

        x += dx;
        dx += s->curve;

        if (s->p1.z - cam_z <= camera_depth() ||
            s->p2.sy >= s->p1.sy ||
            s->p2.sy >= maxy)
            continue;
        render_segment(s, maxy);
        maxy = (int)s->p1.sy;
    }

    /* enemies back-to-front (far first) */
    for (int i = enemy_count - 1; i >= 0; i--) {
        float rel = enemies[i].z - position;
        if (rel < 0) rel += track_length;
        if (rel < PLAYER_Z * SEG_LEN || rel > DRAW_DIST * SEG_LEN * 0.8f) continue;
        render_enemy(&enemies[i]);
    }

    render_player();
    render_hud();

    /* flush */
    printf("\x1b[H");
    const char *last = "";
    for (int y = 0; y < scr_h; y++) {
        for (int x = 0; x < scr_w; x++) {
            const char *st = cell_st[y][x][0] ? cell_st[y][x] : ANSI_RESET;
            if (strcmp(st, last) != 0) { fputs(st, stdout); last = st; }
            putchar(cell_ch[y][x]);
        }
        putchar('\n');
    }
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

/* ---------- game logic ---------- */
static float speed_limit(Segment *s) {
    return MAX_SPEED * (1.0f - fminf(fabsf(s->curve) / 6.0f, 0.55f));
}

static void spawn_enemies(void) {
    enemy_count = 0;
    float gap = SEG_LEN * 12;
    while (enemy_count < ENEMY_CARS && gap < track_length * 0.7f) {
        Enemy *e = &enemies[enemy_count++];
        e->z = wrapf(position + gap, track_length);
        e->offset = ((float)rand() / RAND_MAX) * 1.7f - 0.85f;
        e->speed = MAX_SPEED * (0.30f + 0.45f * ((float)rand() / RAND_MAX));
        e->w = ENEMY_W * (0.8f + 0.5f * ((float)rand() / RAND_MAX));
        e->hue = rand() & 1;
        gap += SEG_LEN * (10 + rand() % 40);
    }
}

static void update_enemies(float dt) {
    for (int i = 0; i < enemy_count; i++) {
        Enemy *e = &enemies[i];
        e->z = wrapf(e->z + e->speed * dt, track_length);
        float rel = e->z - position;
        if (rel < 0) rel += track_length;
        if (rel > track_length * 0.6f) {          /* fell behind: move ahead */
            e->z = wrapf(position + track_length * 0.5f, track_length);
            e->offset = ((float)rand() / RAND_MAX) * 1.7f - 0.85f;
            e->speed = MAX_SPEED * (0.30f + 0.45f * ((float)rand() / RAND_MAX));
        }
    }
}

static int check_collision(void) {
    for (int i = 0; i < enemy_count; i++) {
        const Enemy *e = &enemies[i];
        float rel = e->z - position;
        if (rel < 0) rel += track_length;
        float dz = rel - PLAYER_Z * SEG_LEN;
        if (dz < -SEG_LEN || dz > SEG_LEN) continue;
        if (fabsf(e->offset - player_x) < (e->w + CAR_W) * 0.85f)
            return 1;
    }
    return 0;
}

static void update(float dt) {
    if (!started || game_over) return;
    Segment *ps = find_segment(position + PLAYER_Z * SEG_LEN);

    player_x += steer * dt * 2.4f * (speed / MAX_SPEED);
    player_x -= dt * 2.0f * (speed / MAX_SPEED) * ps->curve * CENTRIFUGAL;
    player_x = clampf(player_x, -2.2f, 2.2f);

    float limit = speed_limit(ps);
    if (speed < limit) speed += ACCEL * dt;
    else               speed += DECEL * dt;
    if (fabsf(player_x) > 1.0f) speed += OFFROAD_DECEL * dt;
    speed = clampf(speed, 0, MAX_SPEED);

    position = increase(position, dt * speed, track_length);
    score = (int)(position / SEG_LEN);

    update_enemies(dt);
    if (check_collision()) { game_over = 1; speed = 0; }
}

/* ---------- input ---------- */
static void handle_keys(void) {
    while (_kbhit()) {
        int k = _getch();
        int ext = (k == 0 || k == 224);
        int k2 = ext ? _getch() : -1;

        int up    = ext ? (k2 == 72) : 0;
        int down  = ext ? (k2 == 80) : 0;
        int left  = ext ? (k2 == 75) : (k == 'a' || k == 'A');
        int right = ext ? (k2 == 77) : (k == 'd' || k == 'D');
        int gaskey  = up || k == 'w' || k == 'W';
        int brakkey = down || k == 's' || k == 'S';

        if (k == 'q' || k == 'Q') exit(0);
        if ((k == 'r' || k == 'R') && game_over) {
            position = 0; player_x = 0; speed = 0; score = 0;
            game_over = 0; started = 0; steer = 0; gas = 0; brake = 0;
        }
        if (left)  { steer = -1; player_x -= 0.05f; }
        if (right) { steer =  1; player_x += 0.05f; }
        if (gaskey) {
            gas = 1;
            if (!started) { started = 1; spawn_enemies(); }
        } else if (!up) gas = 0;
        brake = brakkey || down;
    }
}

static void setup_console(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    printf("\x1b[?25l\x1b[2J");
#endif
}

static double now_sec(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

int main(void) {
    setup_console();
    srand((unsigned)time(NULL) ^ (unsigned)clock());
    build_track();

    double last = 0;
    for (;;) {
        double now = now_sec();
        double dt = last ? now - last : 0.016;
        if (dt > 0.1) dt = 0.1;
        if (dt <= 0) dt = 0.016;
        last = now;
        float fdt = (float)dt;

        handle_keys();
        steer *= 0.82f;
        if (started && !game_over) {
            if (!gas)  speed += DECEL * fdt * 0.5f;
            if (brake) speed += DECEL * 2.5f * fdt;
        }
        update(fdt);
        render();
#ifdef _WIN32
        Sleep(8);
#else
        usleep(16000);
#endif
    }
    return 0;
}
