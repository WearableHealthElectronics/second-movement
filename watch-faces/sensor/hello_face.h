#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "movement.h"

void hello_face_setup(uint8_t watch_face_index, void ** context_ptr);
void hello_face_activate(void *context);
bool hello_face_loop(movement_event_t event, void *context);
void hello_face_resign(void *context);
