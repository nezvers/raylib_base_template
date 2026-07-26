#include "raylib.h"
#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
#include <stddef.h>
#include "raymath.h"
#include <stdio.h>

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_fun_rectangles_example = {Enter, Exit, Update, Draw, Gui, "Stretchy Collision"};


static Vector2 player_pos_prev;
static float player_angle;

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;

    player_pos_prev = Screen2Target(GetMousePosition());
}

static void Exit(){

}

static void Update(){
}

static void Draw(){
    const Rectangle player_rect = {-5, -5, 10, 10};
    Vector2 mouse_pos = Screen2Target(GetMousePosition());
    Vector2 origin = {-player_rect.x, -player_rect.y};//(Vector2){mouse_pos.x, mouse_pos.y};
    Vector2 mouse_delta = Vector2Subtract(mouse_pos, player_pos_prev);
    float target_angle = player_angle;
    if (Vector2LengthSqr(mouse_delta) > 3.f) {
        target_angle = Vector2LineAngle(player_pos_prev, mouse_pos);
        if (target_angle < 0.f) {target_angle += 2* PI;}
        float angle_delta = target_angle - player_angle;
        if (angle_delta > PI && player_angle < PI) {
            player_angle += PI * 2;
        }
        if (angle_delta < -PI && player_angle > PI) {
            player_angle -= PI * 2;
        }
        player_pos_prev = mouse_pos;
    }
    player_angle = player_angle + (target_angle - player_angle) * 0.2;
    Rectangle player_rect_cur = {mouse_pos.x, mouse_pos.y, player_rect.width, player_rect.height};
    DrawRectanglePro(player_rect_cur, origin, -player_angle * RAD2DEG, LIME);
}

static void Gui() {

    
}