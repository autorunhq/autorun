#include "launcher_setup.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "setup_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

struct setup_job
{
    const struct wine_nx_launcher_options *launcher;
    struct setup_boot_options boot;
    SDL_atomic_t done;
    int forwarder;
    unsigned int forwarder_result;
    enum setup_boot_result boot_result;
    char detail[512];
};

static int setup_worker(void *opaque)
{
    struct setup_job *job = opaque;
    if (job->forwarder)
    {
        const char *step = NULL;
        job->forwarder_result = job->launcher->install_forwarder(&step);
        if (job->forwarder_result)
            snprintf(job->detail, sizeof(job->detail), "Could not install while %s (0x%X).",
                     step ? step : "writing the forwarder", job->forwarder_result);
    }
    else
    {
        job->boot_result = setup_boot_install("sdmc:", "romfs:/setup", setup_boot_bundled_manifest(),
                                             &job->boot, job->detail, sizeof(job->detail));
    }
    SDL_AtomicSet(&job->done, 1);
    return 0;
}

static int run_job(struct ui *ui, struct setup_job *job)
{
    SDL_Thread *thread;
    SDL_AtomicSet(&job->done, 0);
    job->detail[0] = 0;
    thread = SDL_CreateThreadWithStackSize(setup_worker, "autorun-setup", 512 * 1024, job);
    if (!thread)
    {
        ui_message(ui, "Setup", "Could not start the installer.");
        return 0;
    }
    ui_progress_begin(ui);
    while (!SDL_AtomicGet(&job->done))
    {
        ui_progress_update(ui, "Autorun setup", job->forwarder ? "Installing the forwarder..." :
                           "Verifying patches and saving the boot entry...", 0, 0);
        SDL_Delay(16);
    }
    SDL_WaitThread(thread, NULL);
    ui_progress_end(ui);
    if (job->forwarder ? job->forwarder_result != 0 :
        job->boot_result != SETUP_BOOT_OK && job->boot_result != SETUP_BOOT_ALREADY)
    {
        ui_message(ui, "Setup not completed", job->detail[0] ? job->detail : setup_boot_error(job->boot_result));
        return 0;
    }
    return 1;
}

static int confirm_install(struct ui *ui, const struct wine_nx_launcher_options *options)
{
    char message[384];
    snprintf(message, sizeof(message), "This installs Autorun on the HOME menu. Installing unofficial titles "
             "can lead to a console ban. Use emuMMC.\n\n%s",
             options->emummc > 0 ? "This console is on emuMMC." : options->emummc == 0 ?
             "This console is NOT on emuMMC." : "The current emuMMC status could not be checked.");
    return ui_confirm(ui, "Install Autorun forwarder?", message, "Install");
}

static int running_atmosphere_matches(const struct setup_boot_manifest *manifest)
{
#ifdef __SWITCH__
    u64 version = 0;
    if (!manifest || R_FAILED(splInitialize())) return -1;
    int known = R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &version));
    splExit();
    return known ? ((version >> 40) & UINT32_C(0xffffff)) == manifest->atmosphere_version : -1;
#else
    (void)manifest;
    return 1;
#endif
}

int launcher_setup_run(struct ui *ui, const struct wine_nx_launcher_options *options, SDL_Texture *logo)
{
    static const char *const steps[] = { "Forwarder", "Loader", "Overclocking", "Mesosphere", "Boot entry", "Finish" };
    struct setup_boot_entry *entries = calloc(SETUP_BOOT_MAX_ENTRIES, sizeof(*entries));
    struct setup_job job = { .launcher = options };
    const struct setup_boot_manifest *manifest = setup_boot_bundled_manifest();
    struct setup_boot_hoc_info hoc = {0};
    struct ui_input input;
    size_t entry_count = 0;
    int step = 0, selected = 0, top = 0, installed = 0, updating = 0, result = 0;
    enum setup_boot_loader installed_loader = SETUP_BOOT_STOCK;
    int forwarder = options->own_forwarder && options->address_space_bits == 39 && options->four_cores_available;
    char detail[512] = "", description[768], title[160], notice_text[256];
    enum setup_boot_result status;
    int romfs_mounted = 0;

    if (!entries) { ui_message(ui, "Setup", "Not enough memory to open setup."); return 0; }
#ifdef __SWITCH__
    if (manifest) romfs_mounted = R_SUCCEEDED(romfsInit());
#endif
    if (manifest && !romfs_mounted)
    {
        ui_message(ui, "Setup", "Could not open bundled boot patches from Autorun.");
        free(entries);
        return 0;
    }
    status = setup_boot_recover("sdmc:", detail, sizeof(detail));
    if (status != SETUP_BOOT_OK && status != SETUP_BOOT_ALREADY)
    {
        ui_message(ui, "Boot configuration recovery", detail[0] ? detail : setup_boot_error(status));
        free(entries);
#ifdef __SWITCH__
        if (romfs_mounted) romfsExit();
#endif
        return 0;
    }
    if (status == SETUP_BOOT_ALREADY) { installed = 1; step = 5; }
    status = setup_boot_list("sdmc:", entries, SETUP_BOOT_MAX_ENTRIES, &entry_count, detail, sizeof(detail));
    if (status == SETUP_BOOT_OK)
        for (size_t i = 0; i < entry_count; i++)
            if (!strcasecmp(entries[i].name, SETUP_BOOT_ENTRY_NAME)) updating = 1;
    if (manifest && updating) setup_boot_needs_update("sdmc:", manifest, &installed_loader);
    if (manifest) setup_boot_hoc_probe("sdmc:", manifest, &hoc);
    ui_start_screen(ui);
    while (ui_begin_frame(ui))
    {
        const char *items[SETUP_BOOT_MAX_ENTRIES] = {0}, *notes[SETUP_BOOT_MAX_ENTRIES] = {0};
        const char *notice = NULL;
        int disabled[SETUP_BOOT_MAX_ENTRIES] = {0};
        int count = 1, action = 0, previous = step;
        int hoc_mismatch = manifest && !strcmp(hoc.path, "atmosphere/kips/hoc.kip") &&
                           (!hoc.compatible || hoc.kip_version != manifest->hoc_kip_version);
        int items_y = step == 4 ? 378 : 438, row_h = step == 4 ? 50 : 70;
        description[0] = 0;
        notice_text[0] = 0;
        snprintf(title, sizeof(title), "%s", steps[step]);
        switch (step)
        {
        case 0:
            snprintf(description, sizeof(description), "Start Autorun directly from the HOME Menu with a 39-bit address space "
                     "and access to all four CPU cores. The next steps enable low-address memory for Win32 games.");
            notice = forwarder ? "Your Autorun forwarder is ready." : "This session was not started from the current Autorun forwarder.";
            items[0] = forwarder ? "Continue" : "Install forwarder";
            notes[0] = forwarder ? "Set up the loader and kernel patches" : "Add Autorun to the HOME Menu";
            disabled[0] = !forwarder && !options->install_forwarder;
            break;
        case 1:
            snprintf(description, sizeof(description), "Choose the loader for the Autorun boot entry. "
                     "Stock patches use separate files. The HOC option updates atmosphere/kips/hoc.kip and keeps its settings when selected.");
            notice = manifest ? "Patches are bundled and verified against the selected Atmosphere package." :
                                "Boot patches are not bundled in this build.";
            items[0] = "Stock Atmosphere loader";
            items[1] = "HOC loader with overclocking";
            notes[0] = "For a standard Atmosphere setup";
            notes[1] = "Keep HOC overclocking support";
            count = 2;
            break;
        case 2:
            snprintf(description, sizeof(description), "Choose which overclock settings the patched HOC loader should use. "
                     "It replaces atmosphere/kips/hoc.kip so the HOC overlay can keep editing the active loader.");
            if (hoc_mismatch) notice = "Installed HOC does not match the bundled version. Update HOC first, or choose the stock loader.";
            else if (!*hoc.path) notice = "No existing HOC loader found. Bundled defaults will be used.";
            else if (!hoc.compatible) notice = "Existing HOC settings use a different CUST layout and cannot be copied safely.";
            else if (hoc.kip_version != manifest->hoc_kip_version)
            {
                snprintf(notice_text, sizeof(notice_text),
                         "Existing HOC %u.%u.%u differs from bundled %u.%u.%u. Compatible settings can still be copied.",
                         hoc.kip_version / 100, hoc.kip_version / 10 % 10, hoc.kip_version % 10,
                         manifest->hoc_kip_version / 100, manifest->hoc_kip_version / 10 % 10,
                         manifest->hoc_kip_version % 10);
                notice = notice_text;
            }
            else notice = "Your existing HOC configuration can be copied into the patched loader.";
            items[0] = "Keep my overclock settings";
            items[1] = "Use the bundled HOC defaults";
            notes[0] = "Copy your compatible HOC configuration";
            notes[1] = "Start with the supplied configuration";
            disabled[0] = !hoc.compatible || hoc_mismatch;
            disabled[1] = hoc_mismatch;
            count = 2;
            break;
        case 3:
            snprintf(description, sizeof(description), "The Mesosphere low-window patch lets a 39-bit Autorun process "
                     "place Win32 game memory below 4 GiB, while native code and JIT caches stay above it.");
            notice = "The kernel and loader must match your supported Atmosphere build. The low-window change only applies to Autorun.";
            items[0] = "Choose a Hekate boot entry";
            notes[0] = "Select the configuration to copy";
            break;
        case 4:
            snprintf(description, sizeof(description), "Choose the entry to copy from bootloader/hekate_ipl.ini. "
                     "Autorun keeps its other boot settings and uses the bundled loader, kernel and security monitor. The original is kept.%s%s",
                     entry_count ? "" : "\n\n", entry_count ? "" : detail[0] ? detail : setup_boot_error(status));
            count = (int)entry_count;
            for (int i = 0; i < count; i++) { items[i] = entries[i].name; disabled[i] = !entries[i].supported; }
            if (!count) { count = 1; items[0] = "No compatible boot entries found"; disabled[0] = 1; }
            break;
        case 5:
            if (installed)
            {
                snprintf(title, sizeof(title), "Setup complete");
                snprintf(description, sizeof(description), "Reboot into Hekate and select the Autorun boot entry, "
                         "then open Autorun from the HOME Menu.");
                notice = "Restart uses the console's configured reboot target. If Hekate does not appear, enter it using your usual method.";
                items[0] = "Restart console";
                notes[0] = "Select the Autorun boot entry in Hekate";
            }
            else
            {
                snprintf(description, sizeof(description), "Boot entry: %s\nLoader: %s\nOverclock settings: %s",
                         job.boot.entry.name, job.boot.loader == SETUP_BOOT_HOC ? "HOC" : "Stock Atmosphere",
                         job.boot.loader != SETUP_BOOT_HOC ? "Not changed" : job.boot.preserve_hoc ? "Preserve existing" : "Bundled defaults");
                notice = manifest ? "Leave the SD card connected until installation finishes." :
                         "Patch files are not bundled yet. No boot files have been changed. Return to Quick setup when they are available.";
                items[0] = updating ? "Update boot patches" : "Install boot patches";
                disabled[0] = !manifest;
                notes[0] = updating ? "Replace the existing Autorun boot entry" : "Verify files and create the Autorun boot entry";
            }
            break;
        }
        if (selected >= count) selected = count - 1;
        while (ui_poll(ui, &input))
        {
            if (input.button == UI_B)
            {
                ui_sound(ui, LAUNCHER_SOUND_BACK);
                if (installed) { result = 1; goto done; }
                if (!step) goto done;
                step -= step == 3 && job.boot.loader == SETUP_BOOT_STOCK ? 2 : 1;
                break;
            }
            if (input.button == UI_UP && selected) { selected--; ui_sound(ui, LAUNCHER_SOUND_MOVE); }
            if (input.button == UI_DOWN && selected + 1 < count) { selected++; ui_sound(ui, LAUNCHER_SOUND_MOVE); }
            if (input.button == UI_A) action = !disabled[selected];
            if (input.touch == UI_TOUCH_TAP && input.x >= 420 && input.x <= 1088 && input.y >= items_y && input.y < 578)
            {
                int row = (input.y - items_y) / row_h + top;
                if (row < count) { selected = row; action = !disabled[row]; }
            }
        }
        if (step != previous)
        {
            selected = step == 1 && updating && installed_loader == SETUP_BOOT_HOC ? 1 : 0;
            top = 0;
            ui_start_screen(ui);
            continue;
        }
        if (action)
        {
            ui_sound(ui, LAUNCHER_SOUND_ACCEPT);
            switch (step)
            {
            case 0:
                if (!forwarder)
                {
                    job.forwarder = 1;
                    if (!confirm_install(ui, options) || !run_job(ui, &job)) break;
                    forwarder = 1;
                }
                step = 1;
                break;
            case 1:
                job.boot.loader = selected ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
                if (!selected) job.boot.preserve_hoc = 0;
                step = selected ? 2 : 3;
                break;
            case 2:
                job.boot.preserve_hoc = !selected;
                if (job.boot.preserve_hoc) snprintf(job.boot.hoc_source, sizeof(job.boot.hoc_source), "%s", hoc.path);
                else job.boot.hoc_source[0] = 0;
                step = 3;
                break;
            case 3: step = 4; break;
            case 4:
            {
                int atmosphere_match = running_atmosphere_matches(manifest);
                if (manifest && atmosphere_match < 0)
                    ui_message(ui, "Atmosphere version unavailable", "Could not verify the running Atmosphere version. Boot an Atmosphere 1.11.2 entry before installing patches.");
                else if (manifest && !atmosphere_match)
                    ui_message(ui, "Atmosphere version mismatch", "The running Atmosphere version is not 1.11.2. Update Atmosphere and reboot first.");
                else { job.boot.entry = entries[selected]; step = 5; }
                break;
            }
            case 5:
                if (installed)
                {
                    if (ui_confirm(ui, "Restart console?", "Autorun will close and restart the console.", "Restart"))
                    { result = 2; goto done; }
                }
                else if (ui_confirm(ui, updating ? "Update boot patches?" : "Install boot patches?",
                                    updating ? "The Autorun boot entry and its patches will be updated." :
                                               "A separate Autorun boot entry will be added. Your existing boot entry will be kept.",
                                    updating ? "Update" : "Install"))
                {
                    job.forwarder = 0;
                    if (run_job(ui, &job)) installed = 1;
                }
                break;
            }
            selected = step == 1 && updating && installed_loader == SETUP_BOOT_HOC ? 1 : 0;
            top = 0;
            ui_start_screen(ui);
            continue;
        }
        if (selected < top) top = selected;
        if (selected >= top + 4) top = selected - 3;
        ui_background(ui);
        ui_gradient(ui, 0, 0, ui->width, ui->height, (SDL_Color){3, 6, 10, 156},
                    (SDL_Color){3, 6, 10, 20}, 1);
        ui_rounded(ui, 166, 78, 960, 584, 26, (SDL_Color){0, 0, 0, 120});
        ui_rounded(ui, 160, 68, 960, 584, 26, (SDL_Color){22, 27, 30, 250});
        ui_outline(ui, 160, 68, 960, 584, 26, 1, (SDL_Color){236, 240, 246, 85});
        if (logo)
        {
            int w, h;
            if (!SDL_QueryTexture(logo, NULL, NULL, &w, &h) && w > 0 && h > 0)
            {
                float scale = 62.0f / (w > h ? w : h);
                SDL_Rect mark = {196 + (62 - (int)(w * scale)) / 2, 91 + (62 - (int)(h * scale)) / 2,
                                 (int)(w * scale), (int)(h * scale)};
                SDL_SetTextureColorMod(logo, 255, 255, 255);
                SDL_SetTextureAlphaMod(logo, 255);
                SDL_RenderCopy(ui->renderer, logo, NULL, &mark);
            }
        }
        ui_text(ui, ui->large, 276, 91, "Quick setup", ui->value);
        ui_text(ui, ui->small, 276, 134, "Prepare Autorun for your console", ui->dim);
        {
            char position[24];
            snprintf(position, sizeof(position), "%d / 6", step + 1);
            ui_text_right(ui, ui->small, 1088, 115, position, ui->dim);
        }
        ui_fill(ui, 192, 176, 896, 1, (SDL_Color){236, 240, 246, 24});
        ui_fill(ui, 396, 204, 1, 374, (SDL_Color){236, 240, 246, 24});
        for (int i = 0; i < 6; i++)
        {
            char number[8];
            int y = 208 + i * 52;
            SDL_Color color = i == step ? ui->value : ui->dim;
            SDL_Color badge = i == step ? ui->selection : i < step ?
                              (SDL_Color){39, 66, 51, 255} : (SDL_Color){46, 53, 57, 255};
            if (i == step) ui_rounded(ui, 180, y, 202, 44, 12, ui->focus);
            ui_fill_circle(ui, 205, y + 22, 12, badge);
            snprintf(number, sizeof(number), "%d", i + 1);
            if (i < step)
            {
                for (int j = 0; j < 4; j++) ui_rounded(ui, 199 + j, y + 22 + j, 2, 2, 1, ui->success);
                for (int j = 0; j < 8; j++) ui_rounded(ui, 202 + j, y + 25 - j, 2, 2, 1, ui->success);
            }
            else ui_text_centered(ui, ui->small, 205, y + (44 - TTF_FontHeight(ui->small)) / 2,
                                  number, i == step ? (SDL_Color){22, 27, 30, 255} : ui->dim);
            ui_text_fit(ui, ui->small, 230, y + (44 - TTF_FontHeight(ui->small)) / 2,
                        138, steps[i], color, 0);
        }
        ui_text_fit(ui, ui->large, 424, 202, 664, title, ui->value, 0);
        ui_text_wrapped(ui, ui->small, 424, 252, 652, step == 4 ? 5 : 4, description, ui->text, 0);
        if (notice)
        {
            ui_rounded(ui, 420, 352, 668, 68, 12, (SDL_Color){30, 40, 42, 255});
            ui_fill(ui, 434, 366, 3, 40, ui->selection);
            ui_text_wrapped(ui, ui->small, 450, 363, 620, 2, notice, ui->dim, 0);
        }
        for (int i = top; i < count && i < top + 4; i++)
        {
            int y = items_y + (i - top) * row_h;
            ui_rounded(ui, 420, y, 668, row_h - 6, 12, i == selected ? ui->focus : (SDL_Color){30, 36, 39, 192});
            if (i == selected) ui_animated_border(ui, 420, y, 668, row_h - 6, 12, 2,
                                                 (SDL_Color){100, 110, 106, 180}, ui->selection);
            ui_text_fit(ui, ui->normal, 438, y + (notes[i] ? 6 : 9), 628, items[i],
                        disabled[i] ? ui->dim : ui->value, i == selected);
            if (notes[i]) ui_text_fit(ui, ui->small, 438, y + 36, 628, notes[i], ui->dim, 0);
        }
        if (step == 4 && entry_count && !entries[selected].supported)
            ui_text_fit(ui, ui->small, 424, 581, 664, entries[selected].reason, ui->danger, 1);
        else if (count > 4)
        {
            char position[32];
            snprintf(position, sizeof(position), "%d / %d", selected + 1, count);
            ui_text_right(ui, ui->small, 1088, 581, position, ui->dim);
        }
        {
            const struct ui_hint hints[] = { {UI_B, step && !installed ? "Back" : "Close"}, {UI_A, "Continue"} };
            ui_fill(ui, 192, 608, 896, 1, (SDL_Color){236, 240, 246, 24});
            ui_hints_right(ui, hints, 2, 1088, 630);
        }
        ui_fade(ui);
        ui_present(ui);
        ui_wait(ui);
    }
    result = installed;
done:
    free(entries);
#ifdef __SWITCH__
    if (romfs_mounted) romfsExit();
#endif
    ui_start_screen(ui);
    return result;
}
