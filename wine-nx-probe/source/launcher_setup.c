#include "launcher_setup.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "setup_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        char payload[768];
        snprintf(payload, sizeof(payload), "%s/setup", job->launcher->runtime_dir);
        job->boot_result = setup_boot_install("sdmc:", payload, setup_boot_bundled_manifest(),
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

int launcher_setup_run(struct ui *ui, const struct wine_nx_launcher_options *options)
{
    static const char *const steps[] = { "Forwarder", "Loader", "Overclocking", "Mesosphere", "Boot entry", "Finish" };
    struct setup_boot_entry *entries = calloc(SETUP_BOOT_MAX_ENTRIES, sizeof(*entries));
    struct setup_job job = { .launcher = options };
    const struct setup_boot_manifest *manifest = setup_boot_bundled_manifest();
    struct ui_input input;
    size_t entry_count = 0;
    int step = 0, selected = 0, top = 0, installed = 0, result = 0;
    int forwarder = options->own_forwarder && options->address_space_bits == 39 && options->four_cores_available;
    char detail[512] = "", description[768], title[160];
    enum setup_boot_result status;

    if (!entries) { ui_message(ui, "Setup", "Not enough memory to open setup."); return 0; }
    status = setup_boot_recover("sdmc:", detail, sizeof(detail));
    if (status != SETUP_BOOT_OK && status != SETUP_BOOT_ALREADY)
    {
        ui_message(ui, "Boot configuration recovery", detail[0] ? detail : setup_boot_error(status));
        free(entries);
        return 0;
    }
    if (status == SETUP_BOOT_ALREADY) { installed = 1; step = 5; }
    status = setup_boot_list("sdmc:", entries, SETUP_BOOT_MAX_ENTRIES, &entry_count, detail, sizeof(detail));
    ui_start_screen(ui);
    while (ui_begin_frame(ui))
    {
        const char *items[SETUP_BOOT_MAX_ENTRIES] = {0};
        int disabled[SETUP_BOOT_MAX_ENTRIES] = {0};
        int count = 1, action = 0, previous = step;
        int items_y = step == 4 ? 369 : 451;
        description[0] = 0;
        snprintf(title, sizeof(title), "%s", steps[step]);
        switch (step)
        {
        case 0:
            snprintf(description, sizeof(description), "%s\n\nThe Autorun forwarder provides a 39-bit address space "
                     "and permission to use all four cores. Low-address Win32 support also needs the loader and kernel patches.",
                     forwarder ? "Your Autorun forwarder is ready." : "Autorun was not started from its current forwarder.");
            items[0] = forwarder ? "Continue" : "Install forwarder";
            disabled[0] = !forwarder && !options->install_forwarder;
            break;
        case 1:
            snprintf(description, sizeof(description), "Choose the loader patch for your boot configuration. "
                     "It is installed separately for the new Autorun boot entry; your existing boot entry stays unchanged.\n\n%s",
                     manifest ? "Matching payloads are verified before installation." :
                     "Patch payloads are not included in this build. You can review setup, but installation is unavailable.");
            items[0] = "Stock Atmosphere loader";
            items[1] = "HOC loader with overclocking";
            count = 2;
            break;
        case 2:
            snprintf(description, sizeof(description), "Keep the overclock settings from your existing HOC loader? "
                     "Only a verified, compatible configuration can be copied. The original loader is not modified.\n\n%s",
                     manifest && manifest->hoc_config_size ? "The installer checks the configuration layout before copying." :
                     "HOC configuration compatibility will be supplied with the patch payload.");
            items[0] = "Keep my overclock settings";
            items[1] = "Use the bundled HOC defaults";
            count = 2;
            break;
        case 3:
            snprintf(description, sizeof(description), "The Mesosphere low-window patch lets a 39-bit Autorun process "
                     "place Win32 game memory below 4 GiB, while native code and JIT caches stay above it.\n\n"
                     "The kernel and loader must match the supported Atmosphere build. "
                     "They are enabled only by the new Hekate boot entry.");
            items[0] = "Choose a Hekate boot entry";
            break;
        case 4:
            snprintf(description, sizeof(description), "Choose the entry to copy from bootloader/hekate_ipl.ini. "
                     "Its boot settings are retained, with Autorun's loader and kernel overrides. The original is kept.%s%s",
                     entry_count ? "" : "\n\n", entry_count ? "" : detail[0] ? detail : setup_boot_error(status));
            count = (int)entry_count;
            for (int i = 0; i < count; i++) { items[i] = entries[i].name; disabled[i] = !entries[i].supported; }
            if (!count) { count = 1; items[0] = "No compatible boot entries found"; disabled[0] = 1; }
            break;
        case 5:
            if (installed)
            {
                snprintf(title, sizeof(title), "Setup complete");
                snprintf(description, sizeof(description), "Reboot into Hekate and select the new Autorun boot entry, "
                         "then open Autorun from the HOME menu.\n\nRebooting uses the console's configured reboot target; "
                         "if Hekate does not appear, enter it using your usual method.");
                items[0] = "Restart console";
                items[1] = "Later";
                count = 2;
            }
            else
            {
                snprintf(description, sizeof(description), "Source: %s\nLoader: %s\nOverclock settings: %s\n\n%s",
                         job.boot.entry.name, job.boot.loader == SETUP_BOOT_HOC ? "HOC" : "Stock Atmosphere",
                         job.boot.loader != SETUP_BOOT_HOC ? "Not changed" : job.boot.preserve_hoc ? "Preserve existing" : "Bundled defaults",
                         manifest ? "The installer verifies every payload, backs up the INI and adds the Autorun entry. "
                         "Keep the SD card connected until it finishes." :
                         "The patch payloads have not been supplied yet. No boot files have been changed. "
                         "Return to Settings > System > Quick setup when they are available.");
                items[0] = "Install boot patches";
                disabled[0] = !manifest;
                items[1] = "Later";
                count = 2;
            }
            break;
        }
        if (selected >= count) selected = count - 1;
        while (ui_poll(ui, &input))
        {
            if (input.button == UI_B)
            {
                if (installed) { result = 1; goto done; }
                if (!step) goto done;
                step -= step == 3 && job.boot.loader == SETUP_BOOT_STOCK ? 2 : 1;
                break;
            }
            if (input.button == UI_X) { result = installed; goto done; }
            if (input.button == UI_UP) { if (selected) selected--; }
            if (input.button == UI_DOWN) { if (selected + 1 < count) selected++; }
            if (input.button == UI_A) action = !disabled[selected];
            if (input.touch == UI_TOUCH_TAP && input.x >= 426 && input.x <= 1054 && input.y >= items_y && input.y < 573)
            {
                int row = (input.y - items_y) / 51 + top;
                if (row < count) { selected = row; action = !disabled[row]; }
            }
        }
        if (step != previous) { selected = top = 0; ui_start_screen(ui); continue; }
        if (action)
        {
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
            case 2: job.boot.preserve_hoc = !selected; step = 3; break;
            case 3: step = 4; break;
            case 4: job.boot.entry = entries[selected]; step = 5; break;
            case 5:
                if (selected == 1) { result = installed; goto done; }
                if (installed)
                {
                    if (ui_confirm(ui, "Restart console?", "Autorun will close and restart the console. Save any other work first.", "Restart"))
                    { result = 2; goto done; }
                }
                else if (ui_confirm(ui, "Install boot patches?", "A separate Autorun boot entry will be added. "
                                     "Your existing boot entry and an INI backup will be kept.", "Install"))
                {
                    job.forwarder = 0;
                    if (run_job(ui, &job)) installed = 1;
                }
                break;
            }
            selected = top = 0;
            ui_start_screen(ui);
            continue;
        }
        if (selected < top) top = selected;
        if (selected >= top + 4) top = selected - 3;
        ui_background(ui);
        ui_gradient(ui, 0, 0, ui->width, ui->height, (SDL_Color){3, 6, 10, 156},
                    (SDL_Color){3, 6, 10, 20}, 1);
        ui_rounded(ui, 184, 90, 924, 560, 22, (SDL_Color){0, 0, 0, 120});
        ui_rounded(ui, 178, 80, 924, 560, 22, (SDL_Color){22, 27, 30, 250});
        ui_rounded_texture(ui, ui_sheen(ui), NULL, (SDL_Rect){178, 80, 924, 186}, 22,
                           (SDL_Color){255, 255, 255, 14});
        ui_outline(ui, 178, 80, 924, 560, 22, 1, (SDL_Color){236, 240, 246, 120});
        ui_text(ui, ui->large, 212, 109, "Autorun setup", ui->text);
        ui_fill(ui, 212, 162, 856, 1, (SDL_Color){236, 240, 246, 24});
        ui_fill(ui, 405, 184, 1, 382, (SDL_Color){236, 240, 246, 24});
        for (int i = 0; i < 6; i++)
        {
            char number[8];
            int y = 188 + i * 46;
            SDL_Color color = i == step ? ui->value : ui->dim;
            SDL_Color badge = i == step ? ui->selection : i < step ?
                              (SDL_Color){39, 66, 51, 255} : (SDL_Color){46, 53, 57, 255};
            if (i == step) ui_rounded(ui, 198, y, 193, 42, 12, ui->focus);
            ui_fill_circle(ui, 221, y + 21, 12, badge);
            snprintf(number, sizeof(number), "%d", i + 1);
            if (i < step)
            {
                for (int j = 0; j < 4; j++) ui_rounded(ui, 215 + j, y + 21 + j, 2, 2, 1, ui->success);
                for (int j = 0; j < 8; j++) ui_rounded(ui, 218 + j, y + 24 - j, 2, 2, 1, ui->success);
            }
            else ui_text_centered(ui, ui->small, 221, y + (42 - TTF_FontHeight(ui->small)) / 2,
                                  number, i == step ? (SDL_Color){22, 27, 30, 255} : ui->dim);
            ui_text_fit(ui, ui->small, 244, y + (42 - TTF_FontHeight(ui->small)) / 2,
                        140, steps[i], color, 0);
        }
        ui_text_fit(ui, ui->large, 426, 180, 628, title, ui->text, 0);
        ui_text_wrapped(ui, ui->small, 426, 228, 628, step == 4 ? 5 : 8, description, ui->dim, 0);
        for (int i = top; i < count && i < top + 4; i++)
        {
            int y = items_y + (i - top) * 51;
            ui_rounded(ui, 420, y - 3, 642, 47, 12, i == selected ? ui->focus : (SDL_Color){30, 36, 39, 192});
            if (i == selected) ui_animated_border(ui, 420, y - 3, 642, 47, 12, 2,
                                                 (SDL_Color){100, 110, 106, 180}, ui->selection);
            ui_text_fit(ui, ui->normal, 436, y + 7, 604, items[i], disabled[i] ? ui->dim : ui->text, i == selected);
        }
        if (step == 4 && entry_count && !entries[selected].supported)
            ui_text_fit(ui, ui->small, 426, 576, 628, entries[selected].reason, ui->danger, 1);
        else if (count > 4)
        {
            char position[32];
            snprintf(position, sizeof(position), "%d / %d", selected + 1, count);
            ui_text_right(ui, ui->small, 1054, 576, position, ui->dim);
        }
        {
            const struct ui_hint hints[] = { {UI_B, step && !installed ? "Back" : "Close"}, {UI_X, "Later"}, {UI_A, "Continue"} };
            ui_hints_right(ui, hints, 3, 1068, 616);
        }
        ui_fade(ui);
        ui_present(ui);
        ui_wait(ui);
    }
    result = installed;
done:
    free(entries);
    ui_start_screen(ui);
    return result;
}
