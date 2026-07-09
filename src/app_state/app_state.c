#include "app_state.h"


static AppState APP_STATE_EMPTY;
AppState* current_app_state = &APP_STATE_EMPTY;

void AppStateTransition(AppState* value){
    if (value == NULL) {
        current_app_state = &APP_STATE_EMPTY;
        return;
    }

    if (current_app_state->exit != NULL){
        current_app_state->exit();
    }
    current_app_state = value;
    if (current_app_state->enter != NULL){
        current_app_state->enter();
    }
}

void AppStateEnter() {
    if (current_app_state->enter != NULL){
        current_app_state->enter();
    }
}

void AppStateExit() {
    if (current_app_state->exit != NULL){
        current_app_state->exit();
    }
}

void AppStateUpdate() {
    if (current_app_state->update != NULL){
        current_app_state->update();
    }
}

void AppStateDraw() {
    if (current_app_state->draw != NULL){
        current_app_state->draw();
    }
}

void AppStateGui() {
    if (current_app_state->gui != NULL){
        current_app_state->gui();
    }
}

