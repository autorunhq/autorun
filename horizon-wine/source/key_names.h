#ifndef WINE_NX_KEY_NAMES_H
#define WINE_NX_KEY_NAMES_H

struct wine_nx_key_name
{
    unsigned short code;
    const char *name;
};

static const struct wine_nx_key_name wine_nx_key_names[] =
{
    { 0x00, "Nothing" },
    { 0x0d, "Enter" },       { 0x20, "Space" },        { 0x1b, "Escape" },
    { 0x09, "Tab" },         { 0x08, "Backspace" },
    { 0x10, "Shift" },       { 0x11, "Control" },      { 0x12, "Alt" },
    { 0x26, "Up arrow" },    { 0x28, "Down arrow" },
    { 0x25, "Left arrow" },  { 0x27, "Right arrow" },
    { 0x41, "A" }, { 0x42, "B" }, { 0x43, "C" }, { 0x44, "D" }, { 0x45, "E" },
    { 0x46, "F" }, { 0x47, "G" }, { 0x48, "H" }, { 0x49, "I" }, { 0x4a, "J" },
    { 0x4b, "K" }, { 0x4c, "L" }, { 0x4d, "M" }, { 0x4e, "N" }, { 0x4f, "O" },
    { 0x50, "P" }, { 0x51, "Q" }, { 0x52, "R" }, { 0x53, "S" }, { 0x54, "T" },
    { 0x55, "U" }, { 0x56, "V" }, { 0x57, "W" }, { 0x58, "X" }, { 0x59, "Y" },
    { 0x5a, "Z" },
    { 0x30, "0" }, { 0x31, "1" }, { 0x32, "2" }, { 0x33, "3" }, { 0x34, "4" },
    { 0x35, "5" }, { 0x36, "6" }, { 0x37, "7" }, { 0x38, "8" }, { 0x39, "9" },
    { 0x70, "F1" },  { 0x71, "F2" },  { 0x72, "F3" },  { 0x73, "F4" },
    { 0x74, "F5" },  { 0x75, "F6" },  { 0x76, "F7" },  { 0x77, "F8" },
    { 0x78, "F9" },  { 0x79, "F10" }, { 0x7a, "F11" }, { 0x7b, "F12" },
    { 0x2d, "Insert" },   { 0x2e, "Delete" },    { 0x24, "Home" },
    { 0x23, "End" },      { 0x21, "Page up" },   { 0x22, "Page down" },
    { 0x14, "Caps lock" }, { 0x2c, "Print screen" }, { 0x13, "Pause" },
    { 0x60, "Numpad 0" }, { 0x61, "Numpad 1" }, { 0x62, "Numpad 2" },
    { 0x63, "Numpad 3" }, { 0x64, "Numpad 4" }, { 0x65, "Numpad 5" },
    { 0x66, "Numpad 6" }, { 0x67, "Numpad 7" }, { 0x68, "Numpad 8" },
    { 0x69, "Numpad 9" }, { 0x6a, "Numpad *" }, { 0x6b, "Numpad +" },
    { 0x6d, "Numpad -" }, { 0x6e, "Numpad ." }, { 0x6f, "Numpad /" },
    { 0xba, "Semicolon" }, { 0xbb, "Equals" },    { 0xbc, "Comma" },
    { 0xbd, "Minus" },     { 0xbe, "Period" },    { 0xbf, "Slash" },
    { 0xc0, "Backtick" },  { 0xdb, "Left bracket" }, { 0xdc, "Backslash" },
    { 0xdd, "Right bracket" }, { 0xde, "Apostrophe" },
};

#define WINE_NX_KEY_NAME_COUNT ((int)(sizeof(wine_nx_key_names) / sizeof(wine_nx_key_names[0])))

/* Where a code sits in the list, or -1 for one the list does not name. */
static inline int wine_nx_key_index( unsigned short code )
{
    int i;

    for (i = 0; i < WINE_NX_KEY_NAME_COUNT; i++)
        if (wine_nx_key_names[i].code == code) return i;
    return -1;
}

#endif /* WINE_NX_KEY_NAMES_H */
