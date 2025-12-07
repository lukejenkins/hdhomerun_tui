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
#define BAR_WIDTH 30
#define MAX_CHANNELS 256

// A struct to hold information about discovered devices
struct discovered_device {
    uint32_t device_id;
    char ip_str[64];
    int tuner_count;
};

// A struct to hold the parsed channel list for a tuner
struct channel_list {
    unsigned int channels[MAX_CHANNELS];
    int count;
};

// Function Prototypes
int discover_devices(struct discovered_device devices[], int max_devices);
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int green_thresh, int yellow_thresh);
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...);
void display_status(struct hdhomerun_device_t *hd, WINDOW *status_win);
int tuning_loop(struct hdhomerun_device_t *hd);
int master_selection_loop(struct discovered_device devices[], int num_devices);
int compare_channels(const void *a, const void *b);
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list);


/*
 * discover_devices
 * Finds HDHomeRun devices on the network and populates an array of structs.
 */
int discover_devices(struct discovered_device devices[], int max_devices) {
    clear();
    mvprintw(0, 0, "Discovering HDHomeRun devices...");
    refresh();

    struct hdhomerun_discover_t *ds = hdhomerun_discover_create(NULL);
    if (!ds) {
        return -1;
    }

    uint32_t device_types[1] = { HDHOMERUN_DEVICE_TYPE_TUNER };
    if (hdhomerun_discover2_find_devices_broadcast(ds, HDHOMERUN_DISCOVER_FLAGS_IPV4_GENERAL, device_types, 1) < 0) {
        hdhomerun_discover_destroy(ds);
        return -1;
    }

    int count = 0;
    struct hdhomerun_discover2_device_t *device = hdhomerun_discover2_iter_device_first(ds);
    while (device && count < max_devices) {
        devices[count].device_id = hdhomerun_discover2_device_get_device_id(device);
        devices[count].tuner_count = hdhomerun_discover2_device_get_tuner_count(device);

        struct hdhomerun_discover2_device_if_t *device_if = hdhomerun_discover2_iter_device_if_first(device);
        struct sockaddr_storage ip_address;
        hdhomerun_discover2_device_if_get_ip_addr(device_if, &ip_address);
        hdhomerun_sock_sockaddr_to_ip_str(devices[count].ip_str, (struct sockaddr *)&ip_address, true);

        count++;
        device = hdhomerun_discover2_iter_device_next(device);
    }

    hdhomerun_discover_destroy(ds);
    clear();
    refresh();
    return count;
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
 * display_status
 * Fetches and displays the status of a tuner in a dedicated window.
 */
void display_status(struct hdhomerun_device_t *hd, WINDOW *status_win) {
    werase(status_win);
    box(status_win, 0, 0);
    
    char title[64];
    sprintf(title, " Tuner %d Status (%08X) ", hdhomerun_device_get_tuner(hd), hdhomerun_device_get_device_id(hd));
    mvwprintw(status_win, 0, 2, "%s", title);

    struct hdhomerun_tuner_status_t status;
    char *status_str;
    bool is_atsc3 = false;
    if (hdhomerun_device_get_tuner_status(hd, &status_str, &status) > 0) {
        mvwprintw(status_win, 2, 2, "Channel: %-10s", status.channel);
        mvwprintw(status_win, 2, 28, "Lock: %s", status.lock_str);
        if (strstr(status.lock_str, "atsc3") != NULL) {
            is_atsc3 = true;
        }
        
        draw_signal_bar(status_win, 4, 2, "Signal Strength", status.signal_strength, 75, 50);
        draw_signal_bar(status_win, 5, 2, "Signal Quality", status.signal_to_noise_quality, 70, 50);
        draw_signal_bar(status_win, 6, 2, "Symbol Quality", status.symbol_error_quality, 100, 90);
    }

    int current_line = 7;
    print_line_in_box(status_win, current_line++, 2, "");

    struct hdhomerun_tuner_vstatus_t vstatus;
    char *vstatus_str;
    if (hdhomerun_device_get_tuner_vstatus(hd, &vstatus_str, &vstatus) > 0 && strlen(vstatus.vchannel) > 0) {
        print_line_in_box(status_win, current_line++, 2, "Virtual Channel: %s", vstatus.vchannel);
        print_line_in_box(status_win, current_line++, 2, "Name: %s", vstatus.name);
        print_line_in_box(status_win, current_line++, 2, ""); // Spacer
    }

    char *streaminfo;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
        print_line_in_box(status_win, current_line++, 2, "Programs (Virtual Channels):");
        char *streaminfo_copy = strdup(streaminfo);
        if(streaminfo_copy) {
            char *line = strtok(streaminfo_copy, "\n");
            while (line != NULL && current_line < LINES - 6) {
                if (strchr(line, ':')) {
                     print_line_in_box(status_win, current_line++, 4, line);
                }
                line = strtok(NULL, "\n");
            }
            free(streaminfo_copy);
        }
        print_line_in_box(status_win, current_line++, 2, ""); // Spacer
    }

    char *plpinfo;
    if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
        print_line_in_box(status_win, current_line++, 2, "PLP Info:");
        char *plpinfo_copy = strdup(plpinfo);
        if(plpinfo_copy) {
            char *line = strtok(plpinfo_copy, "\n");
            while(line != NULL && current_line < LINES - 6) {
                if (strstr(line, "lock=1")) {
                    wattron(status_win, COLOR_PAIR(3)); // Green
                    print_line_in_box(status_win, current_line++, 4, line);
                    wattroff(status_win, COLOR_PAIR(3));
                } else if (strstr(line, "lock=0")) {
                    wattron(status_win, COLOR_PAIR(1)); // Red
                    print_line_in_box(status_win, current_line++, 4, line);
                    wattroff(status_win, COLOR_PAIR(1));
                } else {
                    print_line_in_box(status_win, current_line++, 4, line);
                }
                line = strtok(NULL, "\n");
            }
            free(plpinfo_copy);
        }
    }

    if (is_atsc3) {
        print_line_in_box(status_win, LINES - 5, 2, "Arrows: Ch +/- | c: Tune | p: PLP | Left: Back | q: Quit");
    } else {
        print_line_in_box(status_win, LINES - 5, 2, "Arrows: Ch +/- | c: Tune | Left: Back | q: Quit");
    }
    wrefresh(status_win);
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
        return; // No channel map available
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
 * tuning_loop
 * Handles the user interaction for tuning to a channel and displaying status.
 * Returns 1 to go back, 0 to quit the application.
 */
int tuning_loop(struct hdhomerun_device_t *hd) {
    WINDOW *status_win = newwin(LINES - 2, COLS - 2, 1, 1);
    keypad(status_win, TRUE);
    nodelay(status_win, TRUE);

    struct channel_list chan_list;
    populate_channel_list(hd, &chan_list);

    while (1) {
        display_status(hd, status_win);

        int ch = wgetch(status_win);

        if (ch == 'q') {
            delwin(status_win);
            return 0; // Signal to quit application
        }
        if (ch == KEY_LEFT) {
            break; // Break loop to go back
        }

        if (ch == KEY_UP || ch == KEY_DOWN) {
            struct hdhomerun_tuner_status_t current_status;
            char *current_status_str;
            unsigned int current_channel = 0;
            unsigned int new_channel = 0;

            if (hdhomerun_device_get_tuner_status(hd, &current_status_str, &current_status) > 0) {
                char *channel_part = strchr(current_status.channel, ':');
                if (!channel_part) channel_part = current_status.channel;
                else channel_part++;
                
                if (isdigit((unsigned char)*channel_part)) {
                    current_channel = (unsigned int)strtoul(channel_part, NULL, 10);
                }
            }

            if (chan_list.count > 0) {
                // Smart surfing with channel map
                int current_index = -1;
                for (int i = 0; i < chan_list.count; i++) {
                    if (chan_list.channels[i] == current_channel) {
                        current_index = i;
                        break;
                    }
                }

                if (current_index != -1) {
                    if (ch == KEY_UP) {
                        current_index = (current_index + 1) % chan_list.count;
                    } else { // KEY_DOWN
                        current_index = (current_index - 1 + chan_list.count) % chan_list.count;
                    }
                    new_channel = chan_list.channels[current_index];
                } else {
                    if (ch == KEY_UP) new_channel = chan_list.channels[0]; // Lowest channel
                    else new_channel = chan_list.channels[chan_list.count - 1]; // Highest channel
                }
            } else {
                // Fallback: simple increment/decrement with wrap-around
                if (current_channel > 0 && current_channel < 1000) {
                    if (ch == KEY_UP) {
                        new_channel = (current_channel == 69) ? 2 : current_channel + 1;
                    } else { // KEY_DOWN
                        new_channel = (current_channel == 2) ? 69 : current_channel - 1;
                    }
                } else {
                    if (ch == KEY_UP) new_channel = 2;
                    else new_channel = 69; // Changed from 36 to 69 for consistency
                }
            }

            char full_tune_str[100];
            sprintf(full_tune_str, "auto:%u", new_channel);
            hdhomerun_device_set_tuner_channel(hd, full_tune_str);
        }

        if (ch == 'c') {
            nodelay(status_win, FALSE);
            char channel_str[20] = {0};
            echo();
            
            const char *prompt = "Enter RF Freq (e.g. 533000000) or Enter to cancel: ";
            int prompt_len = strlen(prompt);
            int max_input_len = getmaxx(status_win) - 1 - (2 + prompt_len);
            if (max_input_len > sizeof(channel_str) - 1) max_input_len = sizeof(channel_str) - 1;
            if (max_input_len < 0) max_input_len = 0;

            int line_width = getmaxx(status_win) - 3;
            char line_buffer[256];
            snprintf(line_buffer, sizeof(line_buffer), "%-*s", line_width, prompt);

            wattron(status_win, A_REVERSE);
            mvwprintw(status_win, LINES - 5, 2, "%s", line_buffer);
            wattroff(status_win, A_REVERSE);
            wmove(status_win, LINES - 5, 2 + prompt_len);
            wrefresh(status_win);
            
            wgetnstr(status_win, channel_str, max_input_len);

            if (strlen(channel_str) > 0) {
                char full_tune_str[100];
                sprintf(full_tune_str, "auto:%s", channel_str);
                hdhomerun_device_set_tuner_channel(hd, full_tune_str);
                struct hdhomerun_tuner_status_t lock_status;
                hdhomerun_device_wait_for_lock(hd, &lock_status);
            }
            
            noecho();
            nodelay(status_win, TRUE);
        } else if (ch == 'p') {
            struct hdhomerun_tuner_status_t current_status;
            char *current_status_str;
            if (hdhomerun_device_get_tuner_status(hd, &current_status_str, &current_status) > 0 && strstr(current_status.lock_str, "atsc3")) {
                
                char freq_buffer[20] = {0};
                const char *start = strchr(current_status.channel, ':');
                if (start) {
                    start++; // Move past the colon
                    int i = 0;
                    while (isdigit((unsigned char)*start) && i < sizeof(freq_buffer) - 1) {
                        freq_buffer[i++] = *start++;
                    }
                    freq_buffer[i] = '\0';
                }

                if (strlen(freq_buffer) > 0) {
                    nodelay(status_win, FALSE);
                    echo();
                    char plp_str_in[20] = {0};
                    
                    const char *prompt = "Enter PLPs (e.g. 0,1) or Enter for all: ";
                    int prompt_len = strlen(prompt);
                    int max_input_len = getmaxx(status_win) - 1 - (2 + prompt_len);
                    if (max_input_len > sizeof(plp_str_in) - 1) max_input_len = sizeof(plp_str_in) - 1;
                    if (max_input_len < 0) max_input_len = 0;

                    int line_width = getmaxx(status_win) - 3;
                    char line_buffer[256];
                    snprintf(line_buffer, sizeof(line_buffer), "%-*s", line_width, prompt);

                    wattron(status_win, A_REVERSE);
                    mvwprintw(status_win, LINES - 5, 2, "%s", line_buffer);
                    wattroff(status_win, A_REVERSE);
                    wmove(status_win, LINES - 5, 2 + prompt_len);
                    wrefresh(status_win);
                    
                    wgetnstr(status_win, plp_str_in, max_input_len);

                    char plp_str_out[40] = {0};
                    if (strlen(plp_str_in) > 0) {
                        int j = 0;
                        for (int i = 0; plp_str_in[i] != '\0'; i++) {
                            if (plp_str_in[i] == ',') {
                                plp_str_out[j++] = '+';
                            } else if (isdigit((unsigned char)plp_str_in[i])) {
                                plp_str_out[j++] = plp_str_in[i];
                            }
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

                    noecho();
                    nodelay(status_win, TRUE);
                }
            }
        }
        
        napms(200);
    }
    delwin(status_win);
    return 1; // Signal to go back
}

/*
 * master_selection_loop
 * Main UI loop for selecting a device and tuner from a unified screen.
 * Returns: 1 to continue to next selection, 2 to refresh, 0 to quit.
 */
int master_selection_loop(struct discovered_device devices[], int num_devices) {
    int height = 15;
    int width = 70;
    int start_y = (LINES - height) / 2;
    int start_x = (COLS - width) / 2;
    WINDOW *win = newwin(height, width, start_y, start_x);
    keypad(win, TRUE);

    int device_pane_width = 40;
    int device_highlight = 1;
    int tuner_highlight = 1;
    int active_pane = 0; // 0 for devices, 1 for tuners

    while (1) {
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 0, 2, " Select Device and Tuner (r: refresh, q: quit) ");

        // Draw device pane
        mvwprintw(win, 1, 2, "Devices");
        for (int i = 0; i < num_devices; i++) {
            if (i + 3 >= height -1) break; // Don't draw outside window
            if (i + 1 == device_highlight) {
                wattron(win, (active_pane == 0) ? A_REVERSE : A_BOLD);
            }
            mvwprintw(win, i + 3, 3, "%08X at %s", devices[i].device_id, devices[i].ip_str);
            if (i + 1 == device_highlight) {
                wattroff(win, (active_pane == 0) ? A_REVERSE : A_BOLD);
            }
        }

        // Draw tuner pane
        for(int i = 1; i < height - 1; i++) {
            mvwaddch(win, i, device_pane_width, ACS_VLINE);
        }
        mvwprintw(win, 1, device_pane_width + 2, "Tuners");
        struct discovered_device *current_device = &devices[device_highlight - 1];
        for (int i = 0; i < current_device->tuner_count; i++) {
            if (i + 3 >= height -1) break; // Don't draw outside window
            if (active_pane == 1 && i + 1 == tuner_highlight) {
                wattron(win, A_REVERSE);
            }
            mvwprintw(win, i + 3, device_pane_width + 3, "Tuner %d", i);
            if (active_pane == 1 && i + 1 == tuner_highlight) {
                wattroff(win, A_REVERSE);
            }
        }
        wrefresh(win);

        int c = wgetch(win);
        switch (c) {
            case KEY_UP:
                if (active_pane == 0) {
                    if (device_highlight == 1) device_highlight = num_devices;
                    else --device_highlight;
                    tuner_highlight = 1;
                } else {
                    if (tuner_highlight == 1) tuner_highlight = current_device->tuner_count;
                    else --tuner_highlight;
                }
                break;
            case KEY_DOWN:
                if (active_pane == 0) {
                    if (device_highlight == num_devices) device_highlight = 1;
                    else ++device_highlight;
                    tuner_highlight = 1;
                } else {
                    if (tuner_highlight == current_device->tuner_count) tuner_highlight = 1;
                    else ++tuner_highlight;
                }
                break;
            case KEY_LEFT:
                active_pane = 0;
                break;
            case KEY_RIGHT:
                if (active_pane == 0) {
                    active_pane = 1;
                } else { // If in tuner pane, right arrow is same as enter
                    goto select_tuner;
                }
                break;
            case 10: // Enter
                if (active_pane == 0) {
                    active_pane = 1;
                } else {
                select_tuner:; // Label for goto
                    struct discovered_device *selected_device = &devices[device_highlight - 1];
                    int selected_tuner = tuner_highlight - 1;
                    
                    char device_id_str[9];
                    sprintf(device_id_str, "%08X", selected_device->device_id);
                    struct hdhomerun_device_t *hd = hdhomerun_device_create_from_str(device_id_str, NULL);
                    
                    delwin(win);
                    clear();
                    refresh();

                    if (hd) {
                        hdhomerun_device_set_tuner(hd, selected_tuner);
                        if (tuning_loop(hd) == 0) {
                            hdhomerun_device_destroy(hd);
                            return 0; // Propagate quit signal
                        }
                        hdhomerun_device_destroy(hd);
                    }
                    return 1; // Re-show selection screen
                }
                break;
            case 'r':
                delwin(win);
                return 2; // Refresh
            case 'q':
                delwin(win);
                return 0; // Quit
        }
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
        struct discovered_device devices[MAX_DEVICES];
        int num_devices = discover_devices(devices, MAX_DEVICES);

        if (num_devices <= 0) {
            mvprintw(LINES / 2, (COLS - 28) / 2, "No HDHomeRun devices found.");
            refresh();
            mvprintw(LINES / 2 + 2, (COLS - 40) / 2, "Press 'r' to refresh, or 'q' to quit.");
            int ch = getch();
            if (ch == 'r') {
                clear();
                refresh();
                continue;
            }
            break; // Quit on any other key
        }
        
        int result = master_selection_loop(devices, num_devices);
        if (result == 0) { // Quit
            break;
        }
        if (result == 2) { // Refresh
            continue;
        }
    }

    endwin();
    return 0;
}
