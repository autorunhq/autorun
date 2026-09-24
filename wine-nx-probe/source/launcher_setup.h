#ifndef WINE_NX_LAUNCHER_SETUP_H
#define WINE_NX_LAUNCHER_SETUP_H

struct ui;
struct wine_nx_launcher_options;

/* 0: deferred, 1: installed, 2: installed and reboot requested. */
int launcher_setup_run(struct ui *ui, const struct wine_nx_launcher_options *options);

#endif
