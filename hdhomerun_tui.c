/*
 * hdhomerun_tui.c
 *
 * An ncurses-based command-line tool for interacting with HDHomeRun devices.
 *
 * To compile, you'll need the libhdhomerun source code in a subdirectory
 * named "libhdhomerun" and the ncurses development library installed.
 *
 * On Debian/Ubuntu: sudo apt-get install libncurses5-dev
 * On RedHat/CentOS: sudo yum install ncurses-devel
 * On macOS (using Homebrew): brew install ncurses
 *
 * Then, you can compile with the provided Makefile:
 * make
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include "libhdhomerun/hdhomerun.h"

#define MAX_DEVICES 10
#define MAX_TUNERS_TOTAL 32 // Max combined tuners from all devices
#define BAR_WIDTH 30
#define MAX_CHANNELS 256
#define LEFT_PANE_WIDTH 15

// A struct to hold information about a single, unique tuner
struct unified_tuner {
    uint32_t device_id;
    char ip_str[64];
    int tuner_index;
    int total_tuners_on_device;
};

// A struct to hold the parsed channel list for a tuner
struct channel_list {
    unsigned int channels[MAX_CHANNELS];
    int count;
};

// Function Prototypes
int discover_and_build_tuner_list(struct unified_tuner tuners[]);
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int green_thresh, int yellow_thresh);
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...);
void draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info);
int main_loop(void);
int compare_channels(const void *a, const void *b);
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list);


/*
 * discover_and_build_tuner_list
 * Finds HDHomeRun devices and populates a flat list of all available tuners.
 */
int discover_and_build_tuner_list(struct unified_tuner tuners[]) {
    clear();
    mvprintw(0, 0, "Discovering HDHomeRun devices...");
    refresh();

    struct hdhomerun_discover_t *ds = hdhomerun_discover_create(NULL);
    if (!ds) return 0;

    uint32_t device_types[1] = { HDHOMERUN_DEVICE_TYPE_TUNER };
    if (hdhomerun_discover2_find_devices_broadcast(ds, HDHOMERUN_DISCOVER_FLAGS_IPV4_GENERAL, device_types, 1) < 0) {
        hdhomerun_discover_destroy(ds);
        return 0;
    }

    int total_tuner_count = 0;
    struct hdhomerun_discover2_device_t *device = hdhomerun_discover2_iter_device_first(ds);
    while (device && total_tuner_count < MAX_TUNERS_TOTAL) {
        uint32_t device_id = hdhomerun_discover2_device_get_device_id(device);
        int tuner_count = hdhomerun_discover2_device_get_tuner_count(device);
        char ip_str[64];

        struct hdhomerun_discover2_device_if_t *device_if = hdhomerun_discover2_iter_device_if_first(device);
        struct sockaddr_storage ip_address;
        hdhomerun_discover2_device_if_get_ip_addr(device_if, &ip_address);
        hdhomerun_sock_sockaddr_to_ip_str(ip_str, (struct sockaddr *)&ip_address, true);

        for (int i = 0; i < tuner_count && total_tuner_count < MAX_TUNERS_TOTAL; i++) {
            tuners[total_tuner_count].device_id = device_id;
            strcpy(tuners[total_tuner_count].ip_str, ip_str);
            tuners[total_tuner_count].tuner_index = i;
            tuners[total_tuner_count].total_tuners_on_device = tuner_count;
            total_tuner_count++;
        }

        device = hdhomerun_discover2_iter_device_next(device);
    }

    hdhomerun_discover_destroy(ds);
    clear();
    refresh();
    return total_tuner_count;
}

/*
 * draw_signal_bar
 * Draws a color-coded bar graph for a signal percentage.
 */
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int green_thresh, int yellow_thresh) {
    mvwprintw(win, y, x, "%-24s: [%3d%%] ", label, percentage);
    
    int bar_fill_width = (percentage * BAR_WIDTH) / 100;
    if (bar_fill_width > BAR_WIDTH) bar_fill_width = BAR_WIDTH;

    int color_pair = 1; // Default Red
    if (percentage >= green_thresh) {
        color_pair = 3; // Green
    } else if (percentage >= yellow_thresh) {
        color_pair = 2; // Yellow
    }

    wattron(win, COLOR_PAIR(color_pair));
    for (int i = 0; i < bar_fill_width; ++i) {
        waddch(win, '#');
    }
    wattroff(win, COLOR_PAIR(color_pair));

    for (int i = bar_fill_width; i < BAR_WIDTH; ++i) {
        waddch(win, '-');
    }
}

/*
 * print_line_in_box
 * Formats and prints a line, safely truncating it to fit inside the window box.
 */
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...) {
    char buffer[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    int max_len = getmaxx(win) - x - 1;
    if (max_len < 0) return;

    mvwaddnstr(win, y, x, buffer, max_len);
}


/*
 * draw_status_pane
 * Fetches and displays the status of a tuner in a dedicated sub-window.
 */
void draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info) {
    werase(win);
    box(win, 0, 0);
    
    if (!hd || !tuner_info) {
        mvwprintw(win, 1, 2, "No Tuner Selected");
        return;
    }

    char title[128];
    sprintf(title, " Tuner %08X-%d (%s) Status ", tuner_info->device_id, tuner_info->tuner_index, tuner_info->ip_str);
    mvwprintw(win, 0, 2, "%s", title);

    struct hdhomerun_tuner_status_t status;
    char *status_str;
    bool is_atsc3 = false;
    if (hdhomerun_device_get_tuner_status(hd, &status_str, &status) > 0) {
        char channel_display[32];
        char lock_display[64];
        
        strncpy(channel_display, status.channel, sizeof(channel_display) - 1);
        channel_display[sizeof(channel_display)-1] = '\0';
        strncpy(lock_display, status.lock_str, sizeof(lock_display) - 1);
        lock_display[sizeof(lock_display)-1] = '\0';

        // Smartly split ATSC3 channel strings to move PLP info to the lock field
        if (strncmp(status.channel, "atsc3:", 6) == 0) {
            char *first_colon = strchr(status.channel, ':');
            if (first_colon) {
                char *second_colon = strchr(first_colon + 1, ':');
                if (second_colon) { // This implies PLPs are present
                    int channel_part_len = second_colon - status.channel;
                    strncpy(channel_display, status.channel, channel_part_len);
                    channel_display[channel_part_len] = '\0';
                    snprintf(lock_display, sizeof(lock_display), "%s%s", status.lock_str, second_colon);
                }
            }
        }
        
        print_line_in_box(win, 2, 2, "Channel: %-15s", channel_display);
        print_line_in_box(win, 2, 28, "Lock: %s", lock_display);

        if (strstr(status.lock_str, "atsc3") != NULL) {
            is_atsc3 = true;
        }
        
        draw_signal_bar(win, 4, 2, "Signal Strength", status.signal_strength, 75, 50);
        draw_signal_bar(win, 5, 2, "Signal Quality", status.signal_to_noise_quality, 70, 50);
        draw_signal_bar(win, 6, 2, "Symbol Quality", status.symbol_error_quality, 100, 90);
    }

    int current_line = 7;
    mvwhline(win, current_line++, 2, ACS_HLINE, getmaxx(win) - 4);

    struct hdhomerun_tuner_vstatus_t vstatus;
    char *vstatus_str;
    bool vstatus_displayed = false;
    if (hdhomerun_device_get_tuner_vstatus(hd, &vstatus_str, &vstatus) > 0 && strlen(vstatus.vchannel) > 0) {
        print_line_in_box(win, current_line++, 2, "Virtual Channel: %s", vstatus.vchannel);
        print_line_in_box(win, current_line++, 2, "Name: %s", vstatus.name);
        vstatus_displayed = true;
    }
    
    char *streaminfo;
    bool streaminfo_displayed = false;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
        streaminfo_displayed = true;
        if (vstatus_displayed) { // Add a spacer if vchannel was shown
            current_line++;
        }
        print_line_in_box(win, current_line++, 2, "Programs:");
        char *streaminfo_copy = strdup(streaminfo);
        if(streaminfo_copy) {
            char *line = strtok(streaminfo_copy, "\n");
            while (line != NULL && current_line < LINES - 4) {
                if (strchr(line, ':')) {
                     print_line_in_box(win, current_line++, 4, line);
                }
                line = strtok(NULL, "\n");
            }
            free(streaminfo_copy);
        }
    }
    
    char *plpinfo;
    if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
        if (streaminfo_displayed || vstatus_displayed) {
             mvwhline(win, current_line++, 2, ACS_HLINE, getmaxx(win) - 4);
        }
        print_line_in_box(win, current_line++, 2, "PLP Info:");
        char *plpinfo_copy = strdup(plpinfo);
        if(plpinfo_copy) {
            char *line = strtok(plpinfo_copy, "\n");
            while(line != NULL && current_line < LINES - 4) {
                if (strstr(line, "lock=1")) {
                    wattron(win, COLOR_PAIR(3));
                    print_line_in_box(win, current_line++, 4, line);
                    wattroff(win, COLOR_PAIR(3));
                } else if (strstr(line, "lock=0")) {
                    wattron(win, COLOR_PAIR(1));
                    print_line_in_box(win, current_line++, 4, line);
                    wattroff(win, COLOR_PAIR(1));
                } else {
                    print_line_in_box(win, current_line++, 4, line);
                }
                line = strtok(NULL, "\n");
            }
            free(plpinfo_copy);
        }
    }
}

/*
 * compare_channels
 * A helper function for qsort to sort the channel list numerically.
 */
int compare_channels(const void *a, const void *b) {
   return (*(int*)a - *(int*)b);
}

/*
 * populate_channel_list
 * Gets the tuner's channel map, parses it, and stores a sorted list of channels.
 */
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list) {
    list->count = 0;
    char *map_str;
    if (hdhomerun_device_get_tuner_channelmap(hd, &map_str) <= 0) {
        return;
    }

    char *map_copy = strdup(map_str);
    if (!map_copy) return;

    char *token = strtok(map_copy, " ");
    token = strtok(NULL, " "); 

    while (token != NULL && list->count < MAX_CHANNELS) {
        list->channels[list->count++] = (unsigned int)strtoul(token, NULL, 10);
        token = strtok(NULL, " ");
    }

    free(map_copy);

    if (list->count > 0) {
        qsort(list->channels, list->count, sizeof(unsigned int), compare_channels);
    }
}


/*
 * main_loop
 * The primary application loop for the unified UI.
 */
int main_loop() {
    struct unified_tuner tuners[MAX_TUNERS_TOTAL];
    int total_tuners = 0;
    int highlight = 0;
    
    struct hdhomerun_device_t *hd = NULL;
    uint32_t current_device_id = 0;

    struct channel_list chan_list;
    chan_list.count = 0;
    
    WINDOW *tuner_win = newwin(LINES, LEFT_PANE_WIDTH, 0, 0);
    WINDOW *status_win = newwin(LINES, COLS - LEFT_PANE_WIDTH, 0, LEFT_PANE_WIDTH);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    total_tuners = discover_and_build_tuner_list(tuners);
    if (total_tuners == 0) {
        delwin(tuner_win);
        delwin(status_win);
        return 1;
    }

    while (1) {
        // --- Handle HDHomeRun Device Connection ---
        if (total_tuners > 0) {
            struct unified_tuner *selected_tuner = &tuners[highlight];
            if (hd == NULL || current_device_id != selected_tuner->device_id) {
                if (hd) hdhomerun_device_destroy(hd);
                char device_id_str[16];
                sprintf(device_id_str, "%08X", selected_tuner->device_id);
                hd = hdhomerun_device_create_from_str(device_id_str, NULL);
                current_device_id = selected_tuner->device_id;
            }
            if (hd) {
                hdhomerun_device_set_tuner(hd, selected_tuner->tuner_index);
                populate_channel_list(hd, &chan_list);
            }
        }

        // --- Drawing ---
        werase(tuner_win);
        box(tuner_win, 0, 0);
        for (int i = 0; i < total_tuners; i++) {
            if (i + 2 >= LINES) break;
            if (i == highlight) wattron(tuner_win, A_REVERSE);
            mvwprintw(tuner_win, i + 1, 2, "%08X-%d", tuners[i].device_id, tuners[i].tuner_index);
            if (i == highlight) wattroff(tuner_win, A_REVERSE);
        }
        mvwprintw(tuner_win, LINES - 2, 2, "r: Refresh");
        
        draw_status_pane(status_win, hd, (hd) ? &tuners[highlight] : NULL);
        mvwprintw(status_win, LINES - 2, 2, "L/R Arrows: Ch | c: Tune | p: PLP | q: Quit");

        wrefresh(tuner_win);
        wrefresh(status_win);

        // --- Input Handling ---
        int ch = getch();

        if (isdigit(ch)) {
            ungetch(ch);
            ch = 'c';
        }

        switch(ch) {
            case 'q':
                if (hd) hdhomerun_device_destroy(hd);
                delwin(tuner_win);
                delwin(status_win);
                return 0;

            case 'r':
                if (hd) { hdhomerun_device_destroy(hd); hd = NULL; current_device_id = 0; }
                total_tuners = discover_and_build_tuner_list(tuners);
                highlight = 0;
                if (total_tuners == 0) {
                    delwin(tuner_win);
                    delwin(status_win);
                    return 1;
                }
                break;

            case KEY_UP:
                if (highlight > 0) highlight--;
                break;
            case KEY_DOWN:
                if (highlight < total_tuners - 1) highlight++;
                break;
            
            case KEY_LEFT:
            case KEY_RIGHT:
                if (!hd) break;
                {
                    unsigned int current_channel = 0, new_channel = 0;
                    struct hdhomerun_tuner_status_t current_status;
                    char *s;
                    if (hdhomerun_device_get_tuner_status(hd, &s, &current_status) > 0) {
                        char *p = strchr(current_status.channel, ':');
                        if (!p) p = current_status.channel; else p++;
                        if (isdigit((unsigned char)*p)) current_channel = strtoul(p, NULL, 10);
                    }

                    if (chan_list.count > 0) {
                        int idx = -1;
                        for (int i = 0; i < chan_list.count; i++) if (chan_list.channels[i] == current_channel) idx = i;
                        if (idx != -1) {
                            if (ch == KEY_RIGHT) idx = (idx + 1) % chan_list.count;
                            else idx = (idx - 1 + chan_list.count) % chan_list.count;
                            new_channel = chan_list.channels[idx];
                        } else {
                            if (ch == KEY_RIGHT) new_channel = chan_list.channels[0];
                            else new_channel = chan_list.channels[chan_list.count - 1];
                        }
                    } else {
                        if (current_channel > 0) {
                            if (ch == KEY_RIGHT) new_channel = (current_channel == 69) ? 2 : current_channel + 1;
                            else new_channel = (current_channel == 2) ? 69 : current_channel - 1;
                        } else {
                            if (ch == KEY_RIGHT) new_channel = 2; else new_channel = 69;
                        }
                    }
                    char tune_str[64];
                    sprintf(tune_str, "auto:%u", new_channel);
                    hdhomerun_device_set_tuner_channel(hd, tune_str);
                }
                break;
            
            case 'c':
                 if (!hd) break;
                 {
                    char channel_str[20] = {0};
                    nodelay(stdscr, FALSE); echo();
                    mvwprintw(status_win, LINES - 2, 2, ""); wclrtoeol(status_win);
                    mvwprintw(status_win, LINES - 2, 2, "Enter Channel/Freq: "); wrefresh(status_win);
                    wgetnstr(status_win, channel_str, sizeof(channel_str) - 1);
                    noecho(); nodelay(stdscr, TRUE);

                    if (strlen(channel_str) > 0) {
                        char full_tune_str[100];
                        sprintf(full_tune_str, "auto:%s", channel_str);
                        hdhomerun_device_set_tuner_channel(hd, full_tune_str);
                        struct hdhomerun_tuner_status_t lock_status;
                        hdhomerun_device_wait_for_lock(hd, &lock_status);
                    }
                 }
                 break;
            
            case 'p':
                 if (!hd) break;
                 {
                    struct hdhomerun_tuner_status_t current_status;
                    char *s;
                    if (hdhomerun_device_get_tuner_status(hd, &s, &current_status) > 0 && strstr(current_status.lock_str, "atsc3")) {
                        char freq_buffer[20] = {0};
                        const char *start = strchr(current_status.channel, ':');
                        if (start) {
                            start++;
                            int i = 0;
                            while (isdigit((unsigned char)*start) && i < (int)sizeof(freq_buffer) - 1) freq_buffer[i++] = *start++;
                            freq_buffer[i] = '\0';
                        }
                        if (strlen(freq_buffer) > 0) {
                            char plp_str_in[20] = {0};
                            nodelay(stdscr, FALSE); echo();
                            mvwprintw(status_win, LINES - 2, 2, ""); wclrtoeol(status_win);
                            mvwprintw(status_win, LINES - 2, 2, "Enter PLPs (e.g. 0,1, Enter for all): "); wrefresh(status_win);
                            wgetnstr(status_win, plp_str_in, sizeof(plp_str_in) - 1);
                            noecho(); nodelay(stdscr, TRUE);

                            char plp_str_out[40] = {0};
                            if (strlen(plp_str_in) > 0) {
                                int j = 0;
                                for (int i = 0; plp_str_in[i] != '\0'; i++) {
                                    if (plp_str_in[i] == ',') plp_str_out[j++] = '+';
                                    else if (isdigit((unsigned char)plp_str_in[i])) plp_str_out[j++] = plp_str_in[i];
                                }
                                plp_str_out[j] = '\0';
                            } else {
                                char *plpinfo;
                                if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
                                    char *plpinfo_copy = strdup(plpinfo);
                                    if(plpinfo_copy) {
                                        char *line = strtok(plpinfo_copy, "\n");
                                        bool first_plp = true;
                                        while(line != NULL) {
                                            int plp_id;
                                            if (sscanf(line, "%d:", &plp_id) == 1) {
                                                if (!first_plp) strcat(plp_str_out, "+");
                                                char plp_id_str[5];
                                                sprintf(plp_id_str, "%d", plp_id);
                                                strcat(plp_str_out, plp_id_str);
                                                first_plp = false;
                                            }
                                            line = strtok(NULL, "\n");
                                        }
                                        free(plpinfo_copy);
                                    }
                                }
                            }
                            if (strlen(plp_str_out) > 0) {
                                char full_tune_str[100];
                                sprintf(full_tune_str, "atsc3:%s:%s", freq_buffer, plp_str_out);
                                hdhomerun_device_set_tuner_channel(hd, full_tune_str);
                                struct hdhomerun_tuner_status_t lock_status;
                                hdhomerun_device_wait_for_lock(hd, &lock_status);
                            }
                        }
                    }
                 }
                 break;
        }
        napms(100);
    }
}


/*
 * main
 * Entry point of the application.
 */
int main() {
    initscr();
    clear();
    noecho();
    cbreak();
    curs_set(0);
    start_color();

    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_YELLOW, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);

    while (1) {
        int result = main_loop();
        if (result == 0) { // Quit
            break;
        }
        // If result is 1 (no devices), show message and wait for input
        clear();
        mvprintw(LINES / 2, (COLS - 28) / 2, "No HDHomeRun devices found.");
        mvprintw(LINES / 2 + 2, (COLS - 40) / 2, "Press 'r' to refresh, or 'q' to quit.");
        refresh();
        nodelay(stdscr, FALSE);
        int ch = getch();
        if (ch != 'r') {
            break;
        }
    }

    endwin();
    return 0;
}

