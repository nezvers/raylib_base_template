
#include "enet_example.h"
#include "raylib.h"
#include "raygui.h"
#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
#include <stddef.h>

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_enet_example = {Enter, Exit, Update, Draw, Gui, "Placeholder"};


static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;
}

static void Exit(){

}

static void Update(){

}

static void Draw(){
}

static void Gui() {
    if (GuiButton((Rectangle){20, 20, 100, 25}, "Host")) {
        enet_example_host_create();
    }
    if (GuiButton((Rectangle){130, 20, 100, 25}, "Client")) {
        enet_example_client_create();
    }
}