#include "raylib.h"
#include "raymath.h"
#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
#include <stddef.h>

// #define YSORT_IMPLEMENTATION_INSERT_SORT // temp is single element
#define YSORT_IMPLEMENTATION_MERGE_SORT // temp is same size buffer array. Better at bigger scale.
#include "ysort.h"

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_y_sort_example = {Enter, Exit, Update, Draw, Gui, "Y-Sort example"};

typedef struct {
    Rectangle rect;
    Color color;
    float time;
    bool is_visible;
    unsigned char id;
} Actor;
#define ACTOR_COUNT 100
#define ACTOR_WIDTH 16
#define ACTOR_HEIGHT 32
static Actor actor_list[ACTOR_COUNT];
static Actor temp[ACTOR_COUNT]; // For merge sort implementation
// static Actor temp;   // for insert sort implementation

#define TIME_SCALE 0.2
static Vector2 pivot;
static float radius;


static int ActorCompare(const void *lhs, const void *rhs) {
    const Actor *a = lhs;
    const Actor *b = rhs;

    /* Invisible actors last (example) */
    bool a_hidden = !a->is_visible;
    bool b_hidden = !b->is_visible;
    if (a_hidden && b_hidden) {return 0;}
    if (a_hidden) {return 1;}
    if (b_hidden) {return -1;}

    if (a->rect.y < b->rect.y) return -1;
    if (a->rect.y > b->rect.y) return 1;

    return 0;
}

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;
    
    const Vector2 half_view = Vector2Scale(ScreenStateTargetSize(), 0.5f);
    radius = half_view.y * 2 - ACTOR_HEIGHT;
    pivot = Vector2Add(half_view, (Vector2){-ACTOR_WIDTH * 0.5f, -ACTOR_HEIGHT * 0.5f});

    float time_step = (PI * 2.f) / ACTOR_COUNT;
    float hue_step = (360.f / ACTOR_COUNT) * 10; // Bigger hue jump is easier to visually see
    for (int i = 0; i < ACTOR_COUNT; i += 1) {
        actor_list[i].rect = (Rectangle){0,0, ACTOR_WIDTH, ACTOR_HEIGHT};
        actor_list[i].color = ColorFromHSV(hue_step * i, 1.f, 1.f);
        actor_list[i].id = i;
        actor_list[i].time = time_step * i;
    }
}

static void Update(){
    Vector2 view_size = ScreenStateTargetSize();
    Rectangle view_rect = (Rectangle){0, 0, view_size.x, view_size.y};

    // Move objects with animation
    float delta_time = GetFrameTime();
    for (int i = 0; i < ACTOR_COUNT; i += 1) {
        actor_list[i].time += delta_time * TIME_SCALE;
        Vector2 pos = (Vector2){
            cosf(actor_list[i].time) * radius,
            sinf(actor_list[i].time) * radius,
        };
        actor_list[i].rect.x = pivot.x + pos.x;
        actor_list[i].rect.y = pivot.y + pos.y;
        // View culling
        actor_list[i].is_visible = CheckCollisionRecs(view_rect, actor_list[i].rect);
    }

    
    ysort(actor_list, ACTOR_COUNT, sizeof(Actor), &temp, ActorCompare);
}

static void Draw(){
    for (int i = 0; i < ACTOR_COUNT; i += 1) {
        if (!actor_list[i].is_visible) {
            // all invisible at the end
            continue;
        }
        DrawRectangleRec(actor_list[i].rect, actor_list[i].color);
        DrawText(TextFormat("%d", actor_list[i].id), (int)actor_list[i].rect.x, (int)actor_list[i].rect.y, 10, BLACK);
    }
}

static void Gui() {

}

static void Exit(){

}