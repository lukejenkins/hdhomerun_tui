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
#include <time.h>
#include "hdhomerun.h"
#include "hdhomerun_device.h"

#define MAX_DEVICES 10
#define MAX_TUNERS_TOTAL 32 // Max combined tuners from all devices
#define BAR_WIDTH 30
#define MAX_CHANNELS 256
#define LEFT_PANE_WIDTH 15
#define MAX_PLPS 32
#define MAX_MAPS 20 // Maximum number of channel maps supported

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

// A struct to hold a single line of PLP info for sorting
struct plp_line {
    int id;
    char text[256];
};

// Function Prototypes
int discover_and_build_tuner_list(struct unified_tuner tuners[]);
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int db_value, const char* db_unit);
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...);
void draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info);
int show_help_screen(WINDOW *parent_win);
char* save_stream(struct hdhomerun_device_t *hd, WINDOW *win, bool restart_on_error, int tuner_index);
int main_loop(void);
int compare_channels(const void *a, const void *b);
int compare_plps(const void *a, const void *b);
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list);
long parse_db_value(const char *status_str, const char *key);
long parse_status_value(const char *status_str, const char *key);


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
 * Draws a color-coded bar graph for a signal percentage, with optional dB value.
 */
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int db_value, const char* db_unit) {
    char db_str[16] = "";
    if (db_value != -999) {
        sprintf(db_str, "[%3d %s]", db_value, db_unit);
    }
    
    mvwprintw(win, y, x, "%-18s: [%3d%%] ", label, percentage);
    
    int bar_fill_width = (percentage * BAR_WIDTH) / 100;
    if (bar_fill_width > BAR_WIDTH) bar_fill_width = BAR_WIDTH;

    int color_pair = 1; // Default Red
    if (percentage >= 75) color_pair = 3; // Green
    else if (percentage >= 50) color_pair = 2; // Yellow

    wattron(win, COLOR_PAIR(color_pair));
    for (int i = 0; i < bar_fill_width; ++i) waddch(win, '#');
    wattroff(win, COLOR_PAIR(color_pair));

    for (int i = bar_fill_width; i < BAR_WIDTH; ++i) waddch(win, '-');
    
    wprintw(win, " %s ", db_str);
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
 * parse_db_value
 * Helper to find a dB value from a key like "ss=100(-35dBm)".
 */
long parse_db_value(const char *status_str, const char *key) {
    const char *key_found = strstr(status_str, key);
    if (key_found) {
        const char *paren_open = strchr(key_found, '(');
        if (paren_open) {
            return strtol(paren_open + 1, NULL, 10);
        }
    }
    return -999; // Sentinel for not found
}

/*
 * parse_status_value
 * Helper to find a numeric value for a given key in the raw status string.
 * It now uses base 0 for strtol to auto-detect hex (0x) and decimal.
 */
long parse_status_value(const char *status_str, const char *key) {
    const char *found = strstr(status_str, key);
    if (found) {
        // Use base 0 to auto-detect number format (e.g., "0x" for hex)
        return strtol(found + strlen(key), NULL, 0);
    }
    return -999; // Sentinel for not found
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
    char *raw_status_str;
    bool is_atsc3 = false;
    int current_line = 2;

    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
        long bps = parse_status_value(raw_status_str, "bps=");
        long pps = parse_status_value(raw_status_str, "pps=");
        long rssi = parse_db_value(raw_status_str, "ss=");
        long snr = parse_db_value(raw_status_str, "snq=");

        char channel_display[32], lock_display[64];
        strncpy(channel_display, status.channel, sizeof(channel_display) - 1);
        channel_display[sizeof(channel_display)-1] = '\0';
        strncpy(lock_display, status.lock_str, sizeof(lock_display) - 1);
        lock_display[sizeof(lock_display)-1] = '\0';

        if (strncmp(status.channel, "atsc3:", 6) == 0) {
            char *p1 = strchr(status.channel, ':'), *p2 = p1 ? strchr(p1 + 1, ':') : NULL;
            if (p2) {
                int len = p2 - status.channel;
                strncpy(channel_display, status.channel, len);
                channel_display[len] = '\0';
                snprintf(lock_display, sizeof(lock_display), "%s%s", status.lock_str, p2);
            }
        }
        
        print_line_in_box(win, current_line, 2, "Channel: %-15s", channel_display);
        print_line_in_box(win, current_line, 28, "Lock: %s", lock_display);
        current_line++;

        if (strstr(status.lock_str, "atsc3") != NULL) is_atsc3 = true;
        const char *id_label = is_atsc3 ? "BSID" : "TSID";
        long id_val = -999;
        
        char *streaminfo;
        if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
            id_val = parse_status_value(streaminfo, "tsid=");
        }

        char *plpinfo;
        if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
            long bsid = parse_status_value(plpinfo, "bsid=");
            if (bsid != -999) {
                id_val = bsid;
            }
        }

        if (id_val != -999) {
            print_line_in_box(win, current_line, 2, "%s: %ld (0x%lX)", id_label, id_val, id_val);
        }

        current_line++;
        current_line++; // Spacer line
        
        draw_signal_bar(win, current_line++, 2, "Signal Strength", status.signal_strength, rssi, "dBm");
        draw_signal_bar(win, current_line++, 2, "Signal Quality", status.signal_to_noise_quality, snr, "dB ");
        draw_signal_bar(win, current_line++, 2, "Symbol Quality", status.symbol_error_quality, -999, "");
        
        double mbps = 0.0;
        if (pps > 0 && bps != -999) {
            mbps = (double)bps / 1000000.0;
        }
        print_line_in_box(win, current_line++, 2, "%-18s: %.3f Mbps", "Network Rate", mbps);

        char *target_str;
        if (hdhomerun_device_get_tuner_target(hd, &target_str) > 0) {
            print_line_in_box(win, current_line++, 2, "%-18s: %s", "Network Target", target_str);
        }

    }
    
    mvwhline(win, current_line++, 2, ACS_HLINE, getmaxx(win) - 4);

    struct hdhomerun_tuner_vstatus_t vstatus;
    char *vstatus_str;
    bool vstatus_displayed = false;
    if (hdhomerun_device_get_tuner_vstatus(hd, &vstatus_str, &vstatus) > 0 && strlen(vstatus.vchannel) > 0) {
        print_line_in_box(win, current_line++, 2, "Virtual Channel: %s", vstatus.vchannel);
        print_line_in_box(win, current_line++, 2, "Name: %s", vstatus.name);
        vstatus_displayed = true;
    }
    
    char *streaminfo_prog;
    bool streaminfo_displayed = false;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_prog) > 0) {
        streaminfo_displayed = true;
        if (vstatus_displayed) {
             print_line_in_box(win, current_line++, 2, ""); // Spacer
        }
        print_line_in_box(win, current_line++, 2, "Programs:");
        char *streaminfo_copy = strdup(streaminfo_prog);
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
    
    char *plpinfo_str;
    if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str) > 0) {
        if (streaminfo_displayed || vstatus_displayed) mvwhline(win, current_line++, 2, ACS_HLINE, getmaxx(win) - 4);
        print_line_in_box(win, current_line++, 2, "PLP Info:");
        
        struct plp_line plp_lines[MAX_PLPS];
        int plp_count = 0;
        char *plpinfo_copy = strdup(plpinfo_str);
        if(plpinfo_copy) {
            char *line = strtok(plpinfo_copy, "\n");
            while(line != NULL && plp_count < MAX_PLPS) {
                if (strncmp(line, "bsid=", 5) != 0) { // Exclude bsid line
                    sscanf(line, "%d:", &plp_lines[plp_count].id);
                    strncpy(plp_lines[plp_count].text, line, sizeof(plp_lines[0].text) - 1);
                    plp_lines[plp_count].text[sizeof(plp_lines[0].text) - 1] = '\0';
                    plp_count++;
                }
                line = strtok(NULL, "\n");
            }
            free(plpinfo_copy);
        }

        qsort(plp_lines, plp_count, sizeof(struct plp_line), compare_plps);

        for (int i = 0; i < plp_count && current_line < LINES - 4; i++) {
            char* line_to_print = plp_lines[i].text;
            if (strstr(line_to_print, "lock=1")) {
                wattron(win, COLOR_PAIR(3)); print_line_in_box(win, current_line++, 4, line_to_print); wattroff(win, COLOR_PAIR(3));
            } else if (strstr(line_to_print, "lock=0")) {
                wattron(win, COLOR_PAIR(1)); print_line_in_box(win, current_line++, 4, line_to_print); wattroff(win, COLOR_PAIR(1));
            } else {
                print_line_in_box(win, current_line++, 4, line_to_print);
            }
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
 * compare_plps
 * A helper function for qsort to sort the PLP list numerically.
 */
int compare_plps(const void *a, const void *b) {
    struct plp_line *plpA = (struct plp_line *)a;
    struct plp_line *plpB = (struct plp_line *)b;
    return (plpA->id - plpB->id);
}

/*
 * populate_channel_list
 * Gets the tuner's channel map, parses it, and stores a sorted list of channels.
 */
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list) {
    list->count = 0;
    char *map_str;
    if (hdhomerun_device_get_tuner_channelmap(hd, &map_str) <= 0) return;

    char *map_copy = strdup(map_str);
    if (!map_copy) return;

    char *token = strtok(map_copy, " ");
    token = strtok(NULL, " "); 

    while (token != NULL && list->count < MAX_CHANNELS) {
        list->channels[list->count++] = (unsigned int)strtoul(token, NULL, 10);
        token = strtok(NULL, " ");
    }
    free(map_copy);
    if (list->count > 0) qsort(list->channels, list->count, sizeof(unsigned int), compare_channels);
}

/*
 * show_help_screen
 * Displays a scrollable help screen. Returns 1 if user quits, 0 otherwise.
 */
int show_help_screen(WINDOW *parent_win) {
    const char *help_text[] = {
        "HDHomeRun TUI Help",
        "",
        "KEY BINDINGS:",
        "  Up/Dn Arrows : Select tuner to view.",
        "  Lf/Rt Arrows : Change channel.",
        "  +/- Keys     : Seek for next/previous active channel.",
        "  c            : Manually tune to a channel/frequency.",
        "                 Note that directly entering a channel on the ",
        "                 status screen and pressing Enter also works.",
        "  m            : Change the tuner's channel map.",
        "  p            : Set the tuned ATSC 3.0 PLPs.",
        "  s (ATSC 1.0) : Save a 30-second transport stream capture.",
        "  a (ATSC 1.0) : Save a 30-second transport stream capture, but",
        "                 restart if errors occur.",
        "  r            : Refresh the device list.",
        "  h            : Show this help screen.",
        "  q            : Quit the application.",
        NULL
    };

    int num_lines = 0;
    while(help_text[num_lines] != NULL) num_lines++;

    int parent_h, parent_w, parent_y, parent_x;
    getmaxyx(parent_win, parent_h, parent_w);
    getbegyx(parent_win, parent_y, parent_x);

    WINDOW *help_win = newwin(parent_h, parent_w, parent_y, parent_x);
    
    int scroll_pos = 0;
    keypad(help_win, TRUE);
    nodelay(stdscr, FALSE);

    while(1) {
        werase(help_win);
        box(help_win, 0, 0);
        mvwprintw(help_win, 0, 2, " Help ");

        int max_display_lines = getmaxy(help_win) - 3;
        for (int i = 0; i < max_display_lines; i++) {
            if (scroll_pos + i < num_lines) {
                mvwprintw(help_win, i + 1, 2, "%s", help_text[scroll_pos + i]);
            }
        }
        
        mvwprintw(help_win, getmaxy(help_win) - 2, 2, "Scroll: Up/Down | Close: x or Enter | Quit: q");
        wrefresh(help_win);

        int ch = wgetch(help_win);
        switch(ch) {
            case KEY_UP:
                if (scroll_pos > 0) scroll_pos--;
                break;
            case KEY_DOWN:
                if (num_lines > max_display_lines && scroll_pos < num_lines - max_display_lines) {
                    scroll_pos++;
                }
                break;
            case 'q':
                delwin(help_win);
                return 1;
            case 'x':
            case '\n':
            case '\r':
                delwin(help_win);
                nodelay(stdscr, TRUE);
                return 0;
        }
    }
}

/*
 * save_stream
 * Saves a 30-second transport stream capture to a file.
 */
char* save_stream(struct hdhomerun_device_t *hd, WINDOW *win, bool restart_on_error, int tuner_index) {
    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) <= 0) {
        print_line_in_box(win, LINES - 3, 2, "Failed to get tuner status."); wrefresh(win); sleep(2);
        return NULL;
    }

    if (strstr(status.lock_str, "atsc3") != NULL) {
        print_line_in_box(win, LINES - 3, 2, "Save feature not yet implemented for ATSC 3.0."); wrefresh(win); sleep(2);
        return NULL;
    }
    if (strstr(status.lock_str, "none") != NULL) {
        print_line_in_box(win, LINES - 3, 2, "No signal lock. Cannot save stream."); wrefresh(win); sleep(2);
        return NULL;
    }

    unsigned int rf_channel = 0;
    char *p = strchr(status.channel, ':');
    if (!p) p = status.channel; else p++;
    if (isdigit((unsigned char)*p)) rf_channel = strtoul(p, NULL, 10);

    long tsid = 0;
    char *streaminfo;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
        long parsed_tsid = parse_status_value(streaminfo, "tsid=");
        if (parsed_tsid != -999) tsid = parsed_tsid;
    }

    while(1) {
        char filename[128];
        char time_str[20];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
        sprintf(filename, "rf%u-tsid%ld-%s.ts", rf_channel, tsid, time_str);

        wmove(win, LINES - 3, 2); wclrtoeol(win);
        print_line_in_box(win, LINES - 3, 2, "Starting capture..."); wrefresh(win);

        char debug_path[64];
        sprintf(debug_path, "/tuner%d/debug", tuner_index);
        char *debug_str;
        hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
        long start_te = parse_status_value(debug_str, "te=");
        long start_ne = parse_status_value(debug_str, "ne=");
        long start_se = parse_status_value(debug_str, "se=");

        if (hdhomerun_device_stream_start(hd) <= 0) {
            print_line_in_box(win, LINES - 3, 2, "Failed to start stream."); wrefresh(win); sleep(2);
            return NULL;
        }

        FILE *f = fopen(filename, "wb");
        if (!f) {
            hdhomerun_device_stream_stop(hd);
            print_line_in_box(win, LINES - 3, 2, "Failed to open file for writing."); wrefresh(win); sleep(2);
            return NULL;
        }

        time_t start_time = time(NULL);
        bool error_detected = false;
        unsigned long long total_bytes = 0;

        while(time(NULL) - start_time < 30) {
            print_line_in_box(win, LINES - 3, 2, "Saving to %s... %ld s remaining", filename, 30 - (time(NULL) - start_time));
            wrefresh(win);

            size_t actual_size;
            uint8_t *video_data = hdhomerun_device_stream_recv(hd, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);

            if (video_data && actual_size > 0) {
                fwrite(video_data, 1, actual_size, f);
                total_bytes += actual_size;
            }

            if (restart_on_error) {
                hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
                long current_te = parse_status_value(debug_str, "te=");
                long current_ne = parse_status_value(debug_str, "ne=");
                long current_se = parse_status_value(debug_str, "se=");

                if (current_te > start_te || current_ne > start_ne || current_se > start_se) {
                    error_detected = true;
                    break;
                }
            }
            if (getch() != ERR) { // Abort on any key press
                break;
            }
        }

        fclose(f);
        hdhomerun_device_stream_stop(hd);
        
        hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
        long end_te = parse_status_value(debug_str, "te=");
        long end_ne = parse_status_value(debug_str, "ne=");
        long end_se = parse_status_value(debug_str, "se=");

        if (restart_on_error && error_detected) {
            remove(filename);
            wmove(win, LINES - 3, 2); wclrtoeol(win);
            print_line_in_box(win, LINES - 3, 2, "Error detected. Restarting capture in 2s..."); wrefresh(win);
            sleep(2);
            continue;
        }
        
        char *result_str = (char*)malloc(512);
        sprintf(result_str, "Saved %.2f MB to %s\nErrors: %ld transport, %ld network, %ld sequence", 
            (double)total_bytes / (1024*1024), filename,
            end_te - start_te, end_ne - start_ne, end_se - start_se);
        
        return result_str;
    }
    return NULL;
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
    
    static char *persistent_message = NULL;

    WINDOW *tuner_win = newwin(LINES, LEFT_PANE_WIDTH, 0, 0);
    WINDOW *status_win = newwin(LINES, COLS - LEFT_PANE_WIDTH, 0, LEFT_PANE_WIDTH);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    total_tuners = discover_and_build_tuner_list(tuners);
    if (total_tuners == 0) {
        delwin(tuner_win); delwin(status_win);
        return 1;
    }

    while (1) {
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
                if (chan_list.count == 0) {
                    populate_channel_list(hd, &chan_list);
                }
            }
        }

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
        
        if (persistent_message) {
            char *line1 = persistent_message;
            char *line2 = strchr(persistent_message, '\n');
            if (line2) {
                *line2 = '\0'; 
                line2++;     
            }
            wattron(status_win, A_REVERSE);
            print_line_in_box(status_win, LINES - 4, 2, "%s", line1);
            if (line2) {
                print_line_in_box(status_win, LINES - 3, 2, "%s", line2);
                *(line2 - 1) = '\n'; 
            }
            print_line_in_box(status_win, LINES - 2, 2, "Press Enter to dismiss...");
            wattroff(status_win, A_REVERSE);
        } else {
            mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
        }

        wrefresh(tuner_win);
        wrefresh(status_win);

        int ch = getch();

        if (persistent_message && ch != ERR) {
            free(persistent_message);
            persistent_message = NULL;
            wmove(status_win, LINES - 4, 2); wclrtoeol(status_win);
            wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
            if (ch == '\n' || ch == '\r') continue;
        }

        if (isdigit(ch)) { ungetch(ch); ch = 'c'; }

        switch(ch) {
            case 'q':
                if (hd) hdhomerun_device_destroy(hd);
                delwin(tuner_win); delwin(status_win);
                return 0;

            case 'r':
                if (hd) { hdhomerun_device_destroy(hd); hd = NULL; current_device_id = 0; }
                chan_list.count = 0;
                total_tuners = discover_and_build_tuner_list(tuners);
                highlight = 0;
                if (total_tuners == 0) {
                    delwin(tuner_win); delwin(status_win);
                    return 1;
                }
                break;

            case KEY_UP: if (highlight > 0) highlight--; chan_list.count = 0; break;
            case KEY_DOWN: if (highlight < total_tuners - 1) highlight++; chan_list.count = 0; break;
            
            case '+':
            case '=':
            case '-':
            case '_':
                if (!hd) break;
                {
                    int seek_direction = (ch == '+' || ch == '=') ? 1 : -1;
                    unsigned int current_channel = 0;
                    struct hdhomerun_tuner_status_t status;
                    char *s;
                    if (hdhomerun_device_get_tuner_status(hd, &s, &status) > 0) {
                        char *p = strchr(status.channel, ':');
                        if (!p) p = status.channel; else p++;
                        if (isdigit((unsigned char)*p)) current_channel = strtoul(p, NULL, 10);
                    }

                    int current_idx = -1;
                    if (chan_list.count > 0) {
                        for (int i = 0; i < chan_list.count; i++) {
                            if (chan_list.channels[i] == current_channel) {
                                current_idx = i;
                                break;
                            }
                        }
                    }
                    
                    if (current_idx == -1 && chan_list.count > 0) {
                        current_idx = (seek_direction == 1) ? -1 : chan_list.count;
                    }
                    
                    const unsigned int start_channel = current_channel;
                    bool first_iteration = true;

                    while(1) {
                        unsigned int new_channel;
                        if (chan_list.count > 0) {
                            current_idx = (current_idx + seek_direction + chan_list.count) % chan_list.count;
                            new_channel = chan_list.channels[current_idx];
                        } else {
                            if (current_channel == 0) {
                                current_channel = (seek_direction == 1) ? 1 : 70;
                            }
                            current_channel += seek_direction;
                            if (current_channel > 69) current_channel = 2;
                            if (current_channel < 2) current_channel = 69;
                            new_channel = current_channel;
                        }

                        if (!first_iteration && new_channel == start_channel) {
                            break;
                        }
                        first_iteration = false;

                        char tune_str[64];
                        sprintf(tune_str, "auto:%u", new_channel);
                        hdhomerun_device_set_tuner_channel(hd, tune_str);
                        
                        wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
                        box(status_win, 0, 0);
                        print_line_in_box(status_win, LINES - 3, 2, "Seeking %s on ch %u...", (seek_direction == 1) ? "Up" : "Down", new_channel);
                        draw_status_pane(status_win, hd, &tuners[highlight]);
                        mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
                        wrefresh(status_win);

                        bool lock_found = false;
                        for (int i = 0; i < 25; i++) {
                            struct hdhomerun_tuner_status_t seek_status;
                            char *raw_status;
                            if (hdhomerun_device_get_tuner_status(hd, &raw_status, &seek_status) > 0) {
                                if (seek_status.signal_to_noise_quality > 0) {
                                    lock_found = true;
                                    break;
                                }
                            }
                            
                            draw_status_pane(status_win, hd, &tuners[highlight]);
                            print_line_in_box(status_win, LINES - 3, 2, "Seeking %s on ch %u... (%2.1fs)", (seek_direction == 1) ? "Up" : "Down", new_channel, (25-i)/10.0);
                            mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
                            wrefresh(status_win);
                            
                            napms(100);

                            int abort_ch = getch();
                            if (abort_ch != ERR) {
                                ungetch(abort_ch);
                                goto end_seek;
                            }
                        }

                        if (lock_found) {
                            break;
                        }
                    }
                end_seek:
                    wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
                    box(status_win, 0, 0);
                    draw_status_pane(status_win, hd, &tuners[highlight]);
                    mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
                    wrefresh(status_win);
                }
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
            
            case 'a':
            case 's':
                if (!hd) break;
                persistent_message = save_stream(hd, status_win, (ch == 'a'), tuners[highlight].tuner_index);
                break;

            case 'c':
                 if (!hd) break;
                 {
                    char channel_str[20] = {0};
                    nodelay(stdscr, FALSE); echo();
                    wmove(status_win, LINES - 2, 2); wclrtoeol(status_win);
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

            case 'h':
                if (show_help_screen(status_win) == 1) {
                    if (hd) hdhomerun_device_destroy(hd);
                    delwin(tuner_win); delwin(status_win);
                    return 0;
                }
                break;

            case 'm':
                 if (!hd) break;
                 {
                    char *features_str;
                    if (hdhomerun_device_get_var(hd, "/sys/features", &features_str, NULL) <= 0) break;

                    char *features_copy = strdup(features_str);
                    char *map_line_start = strstr(features_copy, "channelmap:");
                    if (!map_line_start) { free(features_copy); break; }

                    char *map_line_end = strchr(map_line_start, '\n');
                    if (map_line_end) *map_line_end = '\0';

                    char *map_names[MAX_MAPS];
                    int map_count = 0;
                    
                    char *token = strtok(map_line_start, " ");
                    while (token != NULL && map_count < MAX_MAPS) {
                        if (strcmp(token, "channelmap:") != 0) {
                            map_names[map_count++] = strdup(token);
                        }
                        token = strtok(NULL, " ");
                    }
                    free(features_copy);

                    if (map_count > 0) {
                        int menu_start_y = 2;
                        wclear(status_win);
                        box(status_win, 0, 0);

                        char *current_map_str;
                        if (hdhomerun_device_get_tuner_channelmap(hd, &current_map_str) > 0) {
                            char *current_map_copy = strdup(current_map_str);
                            if(current_map_copy) {
                                char *current_map_token = strtok(current_map_copy, " ");
                                mvwprintw(status_win, menu_start_y, 2, "Current Map: %s", current_map_token);
                                free(current_map_copy);
                            }
                        }
                        
                        mvwprintw(status_win, menu_start_y + 2, 2, "Select New Map:");
                        for (int i = 0; i < map_count; i++) {
                            mvwprintw(status_win, menu_start_y + i + 4, 4, "%d: %s", i + 1, map_names[i]);
                        }
                        
                        char choice_str[5] = {0};
                        nodelay(stdscr, FALSE); echo();
                        mvwprintw(status_win, menu_start_y + map_count + 6, 2, "Enter number (or any other key to cancel): ");
                        wrefresh(status_win);
                        wgetnstr(status_win, choice_str, sizeof(choice_str) - 1);
                        noecho(); nodelay(stdscr, TRUE);

                        int choice = atoi(choice_str);
                        if (choice > 0 && choice <= map_count) {
                            hdhomerun_device_set_tuner_channelmap(hd, map_names[choice - 1]);
                            chan_list.count = 0;
                        }

                        for (int i = 0; i < map_count; i++) free(map_names[i]);
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
                            wmove(status_win, LINES - 2, 2); wclrtoeol(status_win);
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

