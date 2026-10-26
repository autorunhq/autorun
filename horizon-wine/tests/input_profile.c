#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../source/input_profile.h"

static int held( const uint64_t *keys, unsigned int code )
{
    return !!(keys[code/64] & (1ull << (code%64)));
}

static void mapping( void )
{
    struct input_profile p;
    struct input_pad raw = {0}, out;
    struct input_output keys;
    struct launcher_kv kv = {0};
    struct input_binding b = {0}, old;
    input_profile_defaults(&p, 1);
    for (unsigned int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        assert(input_sources[i].name[0] && input_sources[i].label[0]);
        for (unsigned int j = i + 1; j < INPUT_SOURCE_COUNT; j++)
            assert(strcmp(input_sources[i].name, input_sources[j].name));
        for (unsigned int j = 0; j < p.keys[i].count; j++) assert(input_key_label(p.keys[i].code[j]));
    }
    assert(p.keys[0].code[0] == INPUT_MOUSE_LEFT && p.keys[1].code[0] == INPUT_MOUSE_RIGHT);
    assert(p.keys[2].code[0] == 0x20 && p.keys[3].code[0] == 0x46);
    assert(p.mode == INPUT_CONTROLLER && p.deadzone[0] == 5 && p.deadzone[1] == 5);
    assert(p.mouse_speed == INPUT_MOUSE_SPEED_DEFAULT);
    assert(p.keys[16].code[0] == 'W' && p.keys[17].code[0] == 'S' && p.keys[18].code[0] == 'A' && p.keys[19].code[0] == 'D');
    for (unsigned int i = 0; i < 4; i++)
    {
        assert(p.keys[12 + i].code[0] == '1' + i);
        assert(p.keys[20 + i].code[0] == INPUT_MOUSE_UP + i);
    }
    p.deadzone[0] = p.deadzone[1] = 0;
    for (unsigned int i = 0; i < 16; i++)
    {
        raw.buttons = input_sources[i].button;
        input_map_pad(&p, &raw, &out);
        assert(out.buttons == raw.buttons);
    }
    raw.buttons = 0;
    raw.axis[0] = -32768; raw.axis[1] = 32767; raw.axis[2] = 12000; raw.axis[3] = -17000;
    input_map_pad(&p, &raw, &out);
    assert(!memcmp(raw.axis, out.axis, sizeof(raw.axis)));
    assert(input_binding_parse(&p.pad[0], "2+6+8+9", 1));
    memset(&raw, 0, sizeof(raw)); raw.buttons = input_sources[0].button;
    input_map_pad(&p, &raw, &out);
    assert(out.buttons == (input_sources[1].button | input_sources[5].button | input_sources[7].button | input_sources[8].button));
    assert(input_binding_parse(&p.keys[0], "0xa2+0x41+0x100+0x105", 0));
    input_map_keys(&p, &raw, &keys);
    assert(held(keys.keys, 0xa2) && held(keys.keys, 0x41) && keys.buttons == 1 && keys.wheels == 1);
    p.keys[1] = p.keys[0];
    raw.buttons = input_sources[1].button;
    input_map_keys(&p, &raw, &keys);
    assert(held(keys.keys, 0xa2) && held(keys.keys, 0x41));
    raw.buttons = 0;
    input_map_keys(&p, &raw, &keys);
    assert(!held(keys.keys, 0xa2) && !keys.buttons && !keys.wheels);
    input_profile_defaults(&p, 1);
    launcher_kv_set(&kv, "UP", "0x57");
    launcher_kv_set(&kv, "LUP", "0");
    launcher_kv_set(&kv, "key.A", "none");
    launcher_kv_set(&kv, "mouse-speed", "200");
    input_profile_apply(&p, &kv);
    assert(p.mouse_speed == 200);
    launcher_kv_set(&kv, "mouse-speed", "9999");   /* out of range: the value in hand stays */
    input_profile_apply(&p, &kv);
    assert(p.mouse_speed == 200);
    assert(p.keys[12].code[0] == 0x57 && p.keys[16].code[0] == 0x57 && !p.keys[0].count);
    launcher_kv_set(&kv, "key.LUP", "none");
    input_profile_apply(&p, &kv);
    assert(!p.keys[16].count);
    assert(input_binding_parse(&b,"0xff",0));
    old = b;
    const char *invalid[] = {"", "-1", "1+", "1+1", "1+2+3+4+5", "0x10d", "99999999999999999999", "junk"};
    for (unsigned int i=0; i<sizeof(invalid)/sizeof(*invalid); i++)
    {
        assert(!input_binding_parse(&b, invalid[i], 0));
        assert(!memcmp(&b, &old, sizeof(b)));
    }
    assert(!input_binding_parse(&b, "25", 1));
    assert(input_binding_parse(&b, "none", 1) && !b.count);
    int x=1000,y=-1500;
    input_filter_stick(&x,&y,10); assert(!x && !y);
    x=18022;y=0;
    input_filter_stick(&x,&y,10); assert(x>=16380 && x<=16386 && !y);
    x=32767;y=32767;
    input_filter_stick(&x,&y,10); assert(x==y && x>23165 && x<23175);

    input_profile_defaults(&p, 1);
    memset(&raw, 0, sizeof(raw));
    raw.axis[2] = 6000; raw.axis[3] = -16000;
    input_map_keys(&p, &raw, &keys);
    assert(keys.mouse_x == 6000 && keys.mouse_y == -16000 && !keys.wheels);
    assert(!keys.keys[0] && !keys.keys[1] && !keys.keys[2] && !keys.keys[3]);
    raw.buttons = input_sources[12].button;
    input_map_keys(&p, &raw, &keys);
    assert(held(keys.keys, '1'));
    assert(input_binding_parse(&p.keys[12], "0x10b", 0));
    input_map_keys(&p, &raw, &keys);
    assert(keys.mouse_x == 6000 - 32767 && !held(keys.keys, '1'));
    raw.axis[1] = 32767;
    input_map_keys(&p, &raw, &keys);
    assert(held(keys.keys, 'W'));
}

static void transitions( void )
{
    struct input_key_edges edges = {0}, taken;
    uint64_t delivered[INPUT_KEY_WORDS] = {0}, keys[INPUT_KEY_WORDS] = {0};
    struct input_key_event events[INPUT_KEY_EVENT_MAX];
    unsigned int n;
    keys[0xa2/64] |= 1ull << (0xa2%64);
    keys[0x41/64] |= 1ull << (0x41%64);
    input_key_edges_update(&edges,keys);
    input_key_edges_take(&edges,&taken);
    n=input_key_events(&taken,delivered,events);
    assert(n==2 && events[0].code==0xa2 && events[0].down && events[1].code==0x41 && events[1].down);
    input_key_edges_update(&edges,keys);
    input_key_edges_take(&edges,&taken);
    assert(!input_key_events(&taken,delivered,events));
    memset(keys,0,sizeof(keys));
    input_key_edges_update(&edges,keys);
    input_key_edges_take(&edges,&taken);
    n=input_key_events(&taken,delivered,events);
    assert(n==2 && events[0].code==0x41 && !events[0].down && events[1].code==0xa2 && !events[1].down);
    keys[1] = 1ull << 1;
    input_key_edges_update(&edges,keys);
    memset(keys,0,sizeof(keys));
    input_key_edges_update(&edges,keys);
    input_key_edges_take(&edges,&taken);
    n=input_key_events(&taken,delivered,events);
    assert(n==2 && events[0].down && !events[1].down && events[0].code==0x41 && events[1].code==0x41);
    for(unsigned int i=0;i<INPUT_KEY_WORDS;i++) delivered[i]=taken.held[i]=taken.pressed[i]=taken.released[i]=UINT64_MAX;
    assert(input_key_events(&taken,delivered,events)==512);
    memset(taken.held,0,sizeof(taken.held));
    assert(input_key_events(&taken,delivered,events)==INPUT_KEY_EVENT_MAX);
}

static void persistence( void )
{
    char root[]="/tmp/autorun-input-XXXXXX", path[768], game[768], saved[768], text[64];
    struct input_profile p;
    struct launcher_kv kv={0};
    assert(mkdtemp(root));
    assert(input_profile_global(root,1,&p,&kv,path,sizeof(path)));
    assert(p.keyboard_auto && p.mode==INPUT_CONTROLLER);
    launcher_kv_set(&kv,"input-mode","2");
    launcher_kv_set(&kv,"left-deadzone","15");
    assert(input_profile_save(&kv,path));
    strcpy(saved,path);
    snprintf(game,sizeof(game),"%s/Game.exe",root);
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mode==2 && p.deadzone[0]==15);
    launcher_kv_set(&kv,"d3d","dxvk");
    launcher_kv_set(&kv,"left-deadzone","25");
    launcher_kv_set(&kv,"keyboard-auto","0");
    assert(input_binding_parse(&p.keys[0],"0xa2+0x41",0));
    assert(input_binding_save(&kv,0,0,&p.keys[0]));
    assert(input_profile_save(&kv,path));
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.deadzone[0]==25 && !p.keyboard_auto && p.keys[0].count==2);
    assert(launcher_kv_get(&kv,"d3d",text,sizeof(text)) && !strcmp(text,"dxvk"));
    assert(launcher_kv_set(&kv,"left-deadzone",NULL));
    assert(launcher_kv_set(&kv,"keyboard-auto",NULL));
    assert(input_binding_save(&kv,0,0,NULL));
    assert(input_profile_save(&kv,path));
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.deadzone[0]==15 && p.keyboard_auto && p.keys[0].code[0]==INPUT_MOUSE_LEFT);
    assert(!unlink(path));
    assert(!unlink(saved));
    snprintf(path,sizeof(path),"%s/config",root);
    assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/program-settings",root);
    rmdir(path);
    assert(!rmdir(root));
}

static void game_layouts( void )
{
    char root[]="/tmp/autorun-input-game-XXXXXX", path[768], game[768];
    struct input_profile p;
    struct launcher_kv kv={0};
    assert(mkdtemp(root));

    /* Septerra Core: the mouse layout over the global controls. */
    snprintf(game,sizeof(game),"%s/septerra.exe",root);
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mode==INPUT_KEYBOARD_MOUSE && p.mouse_speed==150);
    assert(p.keys[6].code[0]==INPUT_MOUSE_LEFT);            /* ZL: left click */
    assert(p.keys[11].code[0]==INPUT_MOUSE_RIGHT);          /* right stick click: right click */
    assert(p.keys[20].code[0]==INPUT_MOUSE_UP && p.keys[23].code[0]==INPUT_MOUSE_MOVE_RIGHT);
    assert(p.keys[7].count==2 && p.keys[7].code[0]==0x10 && p.keys[7].code[1]==INPUT_MOUSE_LEFT);
    assert(p.keys[9].code[0]==0x09 && p.keys[12].code[0]=='Q');  /* Tab and the party keys */

    /* The player's global controls stay under the game's layout... */
    snprintf(path,sizeof(path),"%s/config/keys.txt",root);
    assert(input_profile_save(&(struct launcher_kv){0},path));
    {
        struct launcher_kv global={0};
        assert(launcher_kv_set(&global,"key.A","0x20"));
        assert(launcher_kv_set(&global,"input-mode","1"));
        assert(input_profile_save(&global,path));
    }
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.keys[0].code[0]==INPUT_MOUSE_LEFT && p.mode==INPUT_KEYBOARD_MOUSE);

    /* ...and their own per-game changes win over it, and clearing them brings
     * the game's layout back. */
    assert(launcher_kv_set(&kv,"key.A","0x1b"));
    assert(input_profile_save(&kv,path));
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.keys[0].code[0]==0x1b);
    assert(input_binding_save(&kv,0,0,NULL));
    assert(input_profile_save(&kv,path));
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.keys[0].code[0]==INPUT_MOUSE_LEFT);

    /* The recompiled executable goes by its own name. */
    snprintf(game,sizeof(game),"%s/SR-Septerra.exe",root);
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.keys[11].code[0]==INPUT_MOUSE_RIGHT && p.mode==INPUT_KEYBOARD_MOUSE);

    /* Harbinger: attack, walk, heal, and the stand-and-shoot shift. */
    snprintf(game,sizeof(game),"%s/Harbinger.exe",root);
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mode==INPUT_KEYBOARD_MOUSE && p.mouse_speed==150);
    assert(p.keys[6].code[0]==INPUT_MOUSE_LEFT);
    assert(p.keys[11].code[0]==INPUT_MOUSE_RIGHT);
    assert(p.keys[3].code[0]==INPUT_MOUSE_MIDDLE);
    assert(p.keys[4].code[0]==0x10);
    assert(p.keys[7].count==2 && p.keys[7].code[0]==0x10);

    /* The player's own mouse speed is theirs: the game's suggested speed
     * stands over the global defaults, not over a choice they made. */
    snprintf(game,sizeof(game),"%s/septerra.exe",root);
    snprintf(path,sizeof(path),"%s/config/keys.txt",root);
    {
        struct launcher_kv global={0};
        assert(launcher_kv_set(&global,"key.A","0x20"));
        assert(launcher_kv_set(&global,"input-mode","1"));
        assert(launcher_kv_set(&global,"mouse-speed","25"));
        assert(input_profile_save(&global,path));
    }
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mouse_speed==25 && p.mouse_speed_explicit);
    /* Their per-game choice wins over even that, and the new low stops
     * count; clearing it falls back to the global one. */
    assert(launcher_kv_set(&kv,"mouse-speed","5"));
    assert(input_profile_save(&kv,path));
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mouse_speed==5);
    assert(launcher_kv_set(&kv,"mouse-speed",NULL));
    assert(input_profile_save(&kv,path));
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mouse_speed==25);
    /* With no speed named anywhere, the game's own comes back. */
    snprintf(path,sizeof(path),"%s/config/keys.txt",root);
    {
        struct launcher_kv global={0};
        assert(launcher_kv_set(&global,"key.A","0x20"));
        assert(launcher_kv_set(&global,"input-mode","1"));
        assert(input_profile_save(&global,path));
    }
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mouse_speed==150 && !p.mouse_speed_explicit);

    /* A game without a layout keeps the player's own global controls. */
    snprintf(game,sizeof(game),"%s/Game.exe",root);
    assert(input_profile_program(root,game,1,&p,&kv,path,sizeof(path)));
    assert(p.mode==INPUT_CONTROLLER && p.keys[0].code[0]==0x20 && p.keys[3].code[0]==0x46);

    /* The last reset left the program's settings empty, and an empty settings
     * file is removed rather than kept. */
    snprintf(path,sizeof(path),"%s/septerra.wine-nx.txt",root);
    assert(access(path,F_OK));
    snprintf(path,sizeof(path),"%s/config/keys.txt",root);
    assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/config",root);
    assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/program-settings",root);
    rmdir(path);
    assert(!rmdir(root));
}

int main(void)
{
    mapping(); transitions(); persistence(); game_layouts();
    puts("Input profiles: mapping, deadzones, chords, transitions, inheritance and game layouts passed");
    return 0;
}
