#ifndef AUTORUN_LAUNCHER_INPUT_H
#define AUTORUN_LAUNCHER_INPUT_H

#include "input_profile.h"
#include "launcher_ui.h"

enum launcher_input_row
{
    INPUT_ROW_MODE, INPUT_ROW_PAD, INPUT_ROW_KEYS, INPUT_ROW_KEYBOARD,
    INPUT_ROW_LEFT_ZONE, INPUT_ROW_RIGHT_ZONE, INPUT_ROW_COUNT
};
int launcher_input_rows( struct ui_row *rows, const char *root, const char *program, int keyboard_auto );
void launcher_input_edit( struct ui *ui, struct ui_list *list, int row, enum ui_action action,
                          const char *root, const char *program, const char *title, int keyboard_auto );
void launcher_input_remap( struct ui *ui, const char *root, const char *program, const char *title,
                           int keyboard_auto, int controller );

#endif
