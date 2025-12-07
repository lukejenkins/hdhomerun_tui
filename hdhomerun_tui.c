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
 * 
 * *****
 * 
 * This code has been written almost entirely with Google Gemini as a 
 * personal test of how effective LLMs can be with respect to writing code.
 * The answer, as it turns out, is "very," but everything needs to be 
 * validated and it took many iterations to make this work properly.
 * 
 * *****
 * 
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * 
 * *****
 *
 * L1 parsing logic adapted from l1dump.c:
 * https://github.com/drmpeg/dtv-utils/blob/master/l1dump.c
 * 
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdint.h>
#include "hdhomerun.h"
#include "hdhomerun_device.h"

// Headers for native TCP/HTTP download
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_DEVICES 10
#define MAX_TUNERS_TOTAL 32 // Max combined tuners from all devices
#define BAR_WIDTH 30
#define MAX_CHANNELS 256
#define LEFT_PANE_WIDTH 15
#define MAX_PLPS 64
#define MAX_MAPS 20 // Maximum number of channel maps supported
#define MAX_PROGRAMS 128
#define MAX_DISPLAY_LINES (MAX_PLPS * 20 + 300) // Increased buffer for new L1 detail

static const char* TUI_VERSION = "0.8.0";

// A struct to hold information about a single, unique tuner
struct unified_tuner {
    uint32_t device_id;
    char ip_str[64];
    int tuner_index;
    int total_tuners_on_device;
    bool is_legacy;
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

// Enum to define the different types of save operations
enum save_mode {
    SAVE_NORMAL_TS,
    SAVE_AUTORESTART_TS,
    SAVE_NORMAL_DBG,
    SAVE_AUTORESTART_DBG,
    SAVE_NORMAL_PCAP,
    SAVE_AUTORESTART_PCAP
};

// Struct to hold the ATSC 3.0 ModCod to SNR lookup table data
struct modcod_snr {
    char mod[16];
    char cod[8];
    float min_snr;
    float max_snr;
};

// Function Prototypes
int discover_and_build_tuner_list(struct unified_tuner tuners[]);
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int db_value, const char* db_unit);
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...);
int draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, int scroll_offset);
int show_help_screen(WINDOW *parent_win);
char* save_stream(struct hdhomerun_device_t *hd, WINDOW *win, enum save_mode mode, struct unified_tuner *tuner_info, bool debug_enabled);
int main_loop(void);
int compare_channels(const void *a, const void *b);
int compare_plps(const void *a, const void *b);
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list);
long parse_db_value(const char *status_str, const char *key);
long parse_status_value(const char *status_str, const char *key);
char* stream_to_vlc(struct hdhomerun_device_t *hd, WINDOW *win, pid_t *vlc_pid, struct unified_tuner *tuner_info);
int select_program_menu(WINDOW *win, char *streaminfo_str, char *selected_program_str, int *selected_plp);
int get_udp_port();
int show_plp_details_screen(WINDOW *parent_win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info);
const struct modcod_snr* get_snr_for_modcod(const char* mod, const char* cod);
void normalize_mod_str(const char *in, char *out, size_t out_size);
int http_save_stream(const char *ip_addr, const char *url, const char *filename, WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, bool autorestart_enabled, int save_attempts, int max_save_attempts, bool *out_aborted, bool *out_error_detected, bool debug_enabled);
void parse_l1_data(const unsigned char* data, size_t len, char** display_lines, int* line_count, int max_lines);


// --- ATSC 3.0 SNR Lookup Table ---
// Data from atsc3_modcod.csv
static const struct modcod_snr snr_table[] = {
    {"QPSK", "2/15", -6.23, -5.06}, {"QPSK", "3/15", -4.32, -2.97},
    {"QPSK", "4/15", -2.89, -1.36}, {"QPSK", "5/15", -1.7, -0.08},
    {"QPSK", "6/15", -0.54, 1.15}, {"QPSK", "7/15", 0.3, 2.3},
    {"QPSK", "8/15", 1.16, 3.44}, {"QPSK", "9/15", 1.97, 4.7},
    {"QPSK", "10/15", 2.77, 5.97}, {"QPSK", "11/15", 3.6, 7.46},
    {"QPSK", "12/15", 4.49, 9.15}, {"QPSK", "13/15", 5.53, 11.56},
    {"16QAM", "2/15", -2.73, -1.14}, {"16QAM", "3/15", -0.25, 1.45},
    {"16QAM", "4/15", 1.46, 3.41}, {"16QAM", "5/15", 2.82, 4.78},
    {"16QAM", "6/15", 4.21, 6.27}, {"16QAM", "7/15", 5.21, 7.58},
    {"16QAM", "8/15", 6.3, 8.96}, {"16QAM", "9/15", 7.32, 10.28},
    {"16QAM", "10/15", 8.36, 11.73}, {"16QAM", "11/15", 9.5, 13.22},
    {"16QAM", "12/15", 10.57, 14.97}, {"16QAM", "13/15", 11.83, 17.44},
    {"64QAM", "2/15", -0.26, 1.6}, {"64QAM", "3/15", 2.27, 4.3},
    {"64QAM", "4/15", 4.07, 6.22}, {"64QAM", "5/15", 5.5, 7.74},
    {"64QAM", "6/15", 6.96, 9.31}, {"64QAM", "7/15", 8.01, 10.65},
    {"64QAM", "8/15", 9.11, 12.03}, {"64QAM", "9/15", 10.15, 13.34},
    {"64QAM", "10/15", 11.21, 14.77}, {"64QAM", "11/15", 12.38, 16.23},
    {"64QAM", "12/15", 13.48, 17.95}, {"64QAM", "13/15", 14.75, 20.37},
    {"256QAM", "2/15", 2.37, 4.21}, {"256QAM", "3/15", 5.0, 7.0},
    {"256QAM", "4/15", 6.88, 8.99}, {"256QAM", "5/15", 8.35, 10.55},
    {"256QAM", "6/15", 9.85, 12.15}, {"256QAM", "7/15", 10.93, 13.51},
    {"256QAM", "8/15", 12.05, 14.9}, {"256QAM", "9/15", 13.1, 16.2},
    {"256QAM", "10/15", 14.18, 17.61}, {"256QAM", "11/15", 15.35, 19.05},
    {"256QAM", "12/15", 16.45, 20.73}, {"256QAM", "13/15", 17.72, 23.1},
    {"1024QAM", "2/15", 4.97, 6.81}, {"1024QAM", "3/15", 7.69, 9.7},
    {"1024QAM", "4/15", 9.61, 11.75}, {"1024QAM", "5/15", 11.12, 13.34},
    {"1024QAM", "6/15", 12.65, 14.97}, {"1024QAM", "7/15", 13.75, 16.35},
    {"1024QAM", "8/15", 14.89, 17.75}, {"1024QAM", "9/15", 15.95, 19.06},
    {"1024QAM", "10/15", 17.03, 20.46}, {"1024QAM", "11/15", 18.2, 21.9},
    {"1024QAM", "12/15", 19.31, 23.55}, {"1024QAM", "13/15", 20.58, 25.88},
    {"4096QAM", "2/15", 7.58, 9.41}, {"4096QAM", "3/15", 10.38, 12.4},
    {"4096QAM", "4/15", 12.34, 14.45}, {"4096QAM", "5/15", 13.88, 16.07},
    {"4096QAM", "6/15", 15.44, 17.72}, {"4096QAM", "7/15", 16.56, 19.11},
    {"4096QAM", "8/15", 17.72, 20.52}, {"4096QAM", "9/15", 18.79, 21.84},
    {"4096QAM", "10/15", 19.88, 23.25}, {"4096QAM", "11/15", 21.05, 24.69},
    {"4096QAM", "12/15", 22.16, 26.34}, {"4096QAM", "13/15", 23.43, 28.62},
    {{0}} // Sentinel
};

/*
 * normalize_mod_str
 * Converts a device modulation string (e.g., "qam256") to the table format ("256QAM").
 */
void normalize_mod_str(const char *in, char *out, size_t out_size) {
    char digits[8] = {0};
    char alphas[8] = {0};
    int d_idx = 0;
    int a_idx = 0;

    // Separate digits and letters
    for (int i = 0; in[i] != '\0' && i < 15; i++) {
        if (isdigit((unsigned char)in[i])) {
            if (d_idx < 7) digits[d_idx++] = in[i];
        } else {
            if (a_idx < 7) alphas[a_idx++] = toupper((unsigned char)in[i]);
        }
    }
    
    // Reassemble in the correct order (digits then alphas)
    if (d_idx > 0) {
        snprintf(out, out_size, "%s%s", digits, alphas);
    } else {
        snprintf(out, out_size, "%s", alphas);
    }
}

/*
 * get_snr_for_modcod
 * Looks up the min/max SNR for a given modulation and code rate.
 */
const struct modcod_snr* get_snr_for_modcod(const char* mod, const char* cod) {
    for (int i = 0; snr_table[i].mod[0] != 0; i++) {
        if (strcmp(snr_table[i].mod, mod) == 0 && strcmp(snr_table[i].cod, cod) == 0) {
            return &snr_table[i];
        }
    }
    return NULL; // Not found
}

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
        bool is_legacy = hdhomerun_discover2_device_is_legacy(device);
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
            tuners[total_tuner_count].is_legacy = is_legacy;
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
 * Returns the total number of content lines, for scrolling purposes.
 */
int draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, int scroll_offset) {
    werase(win);
    box(win, 0, 0);
    
    if (!hd || !tuner_info) {
        mvwprintw(win, 1, 2, "No Tuner Selected");
        return 0;
    }

    char title[128];
    sprintf(title, " Tuner %08X-%d (%s) Status ", tuner_info->device_id, tuner_info->tuner_index, tuner_info->ip_str);
    mvwprintw(win, 0, 2, "%s", title);

    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    bool is_atsc3 = false;
    int total_content_lines = 0;
    int y = 2; // Start drawing at line 2

    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
        long bps = parse_status_value(raw_status_str, "bps=");
        long pps = parse_status_value(raw_status_str, "pps=");
        long rssi = parse_db_value(raw_status_str, "ss=");
        long snr = parse_db_value(raw_status_str, "snq=");

        total_content_lines = 11; // Base number of lines for the top section

        // --- All drawing is now conditional on scroll position ---
        if (y - scroll_offset > 0) {
            char channel_display[64];
            strncpy(channel_display, status.channel, sizeof(channel_display) - 1);
            channel_display[sizeof(channel_display) - 1] = '\0';

            char lock_display[64];
            strncpy(lock_display, status.lock_str, sizeof(lock_display) - 1);
            lock_display[sizeof(lock_display) - 1] = '\0';

            // If channel is atsc3 and has PLPs defined (e.g. "atsc3:33:0+16"), format display.
            if (strncmp(status.channel, "atsc3:", 6) == 0) {
                char *first_colon = strchr(status.channel, ':');
                if (first_colon) {
                    char *second_colon = strchr(first_colon + 1, ':');
                    if (second_colon) {
                        // Create a temporary copy to modify for channel display
                        char temp_channel[64];
                        strncpy(temp_channel, status.channel, sizeof(temp_channel));
                        temp_channel[second_colon - status.channel] = '\0';
                        snprintf(channel_display, sizeof(channel_display), "%s", temp_channel);

                        // Format the lock display to show the PLPs
                        snprintf(lock_display, sizeof(lock_display), "atsc3:%s", second_colon + 1);
                    }
                }
            }
            print_line_in_box(win, y - scroll_offset, 2, "Channel: %-15s", channel_display);
            print_line_in_box(win, y - scroll_offset, 28, "Lock: %s", lock_display);
        }
        y++;

        if (strstr(status.lock_str, "atsc3") != NULL) is_atsc3 = true;
        const char *id_label = is_atsc3 ? "BSID" : "TSID";
        long id_val = -999;
        
        char *streaminfo;
        if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) id_val = parse_status_value(streaminfo, "tsid=");
        char *plpinfo;
        if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
            long bsid = parse_status_value(plpinfo, "bsid=");
            if (bsid != -999) id_val = bsid;
        }
        if (id_val != -999) {
            if (y - scroll_offset > 0) print_line_in_box(win, y - scroll_offset, 2, "%s: %ld (0x%lX)", id_label, id_val, id_val);
        }
        y += 2; // Spacer

        if (y - scroll_offset > 0) { draw_signal_bar(win, y - scroll_offset, 2, "Signal Strength", status.signal_strength, rssi, "dBm"); } y++;
        if (y - scroll_offset > 0) { draw_signal_bar(win, y - scroll_offset, 2, "Signal Quality", status.signal_to_noise_quality, snr, "dB "); } y++;
        if (y - scroll_offset > 0) { draw_signal_bar(win, y - scroll_offset, 2, "Symbol Quality", status.symbol_error_quality, -999, ""); } y++;
        
        double mbps = (pps > 0 && bps != -999) ? (double)bps / 1000000.0 : 0.0;
        if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "%-18s: %.3f Mbps", "Network Rate", mbps); } y++;

        char *target_str;
        if (hdhomerun_device_get_tuner_target(hd, &target_str) > 0) {
            if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "%-18s: %s", "Network Target", target_str); } y++;
        }
        
        if (y - scroll_offset > 0) { mvwhline(win, y - scroll_offset, 2, ACS_HLINE, getmaxx(win) - 4); } y++;

        struct hdhomerun_tuner_vstatus_t vstatus;
        char *vstatus_str;
        if (hdhomerun_device_get_tuner_vstatus(hd, &vstatus_str, &vstatus) > 0 && strlen(vstatus.vchannel) > 0) {
            total_content_lines += 2;
            if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "Virtual Channel: %s", vstatus.vchannel); } y++;
            if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "Name: %s", vstatus.name); } y++;
        }
        
        char *streaminfo_prog;
        if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_prog) > 0) {
            char *programs[MAX_PROGRAMS];
            int program_count = 0;
            char *streaminfo_copy = strdup(streaminfo_prog);
            if(streaminfo_copy) {
                char *line = strtok(streaminfo_copy, "\n");
                while (line != NULL && program_count < MAX_PROGRAMS) {
                    if (strchr(line, ':') || strstr(line, "program=")) programs[program_count++] = strdup(line);
                    line = strtok(NULL, "\n");
                }
                free(streaminfo_copy);
            }

            // If program count > 7, use two columns, provided the window is wide enough.
            bool two_columns = (program_count > 7) && (getmaxx(win) > 70);

            total_content_lines += 1; // For "Programs:" title
            if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                print_line_in_box(win, y - scroll_offset, 2, "Programs:");
            }
            y++;

            if (two_columns) {
                int midpoint = (program_count + 1) / 2;
                total_content_lines += midpoint;
                for (int i = 0; i < midpoint; i++) {
                    if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                        // Print left column
                        if (strstr(programs[i], "(encrypted)")) {
                            wattron(win, COLOR_PAIR(1));
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                            wattroff(win, COLOR_PAIR(1));
                        } else {
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                        }
                        
                        // Print right column
                        if (i + midpoint < program_count) {
                            if (strstr(programs[i + midpoint], "(encrypted)")) {
                                wattron(win, COLOR_PAIR(1));
                                print_line_in_box(win, y - scroll_offset, getmaxx(win)/2, programs[i + midpoint]);
                                wattroff(win, COLOR_PAIR(1));
                            } else {
                                print_line_in_box(win, y - scroll_offset, getmaxx(win)/2, programs[i + midpoint]);
                            }
                        }
                    }
                    y++;
                }
            } else {
                total_content_lines += program_count;
                for (int i = 0; i < program_count; i++) {
                    if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                        if (strstr(programs[i], "(encrypted)")) {
                            wattron(win, COLOR_PAIR(1));
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                            wattroff(win, COLOR_PAIR(1));
                        } else {
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                        }
                    }
                    y++;
                }
            }

            for (int i = 0; i < program_count; i++) free(programs[i]);
        }
        
        char *plpinfo_str;
        if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str) > 0) {
            struct plp_line plp_lines[MAX_PLPS];
            int plp_count = 0;
            char *plpinfo_copy = strdup(plpinfo_str);
            if(plpinfo_copy) {
                char *line = strtok(plpinfo_copy, "\n");
                while(line != NULL && plp_count < MAX_PLPS) {
                    if (strncmp(line, "bsid=", 5) != 0) {
                        sscanf(line, "%d:", &plp_lines[plp_count].id);
                        strncpy(plp_lines[plp_count].text, line, sizeof(plp_lines[0].text) - 1);
                        plp_lines[plp_count].text[sizeof(plp_lines[0].text) - 1] = '\0';
                        plp_count++;
                    }
                    line = strtok(NULL, "\n");
                }
                free(plpinfo_copy);
            }

            if (plp_count > 0) {
                total_content_lines += 2 + plp_count;
                if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                    mvwhline(win, y - scroll_offset, 2, ACS_HLINE, getmaxx(win) - 4);
                }
                y++;
                if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                    print_line_in_box(win, y - scroll_offset, 2, "PLP Info:");
                }
                y++;
                qsort(plp_lines, plp_count, sizeof(struct plp_line), compare_plps);
                for (int i = 0; i < plp_count; i++) {
                    if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                        char* line_to_print = plp_lines[i].text;
                        if (strstr(line_to_print, "lock=1")) {
                            wattron(win, COLOR_PAIR(3)); print_line_in_box(win, y - scroll_offset, 4, line_to_print); wattroff(win, COLOR_PAIR(3));
                        } else if (strstr(line_to_print, "lock=0")) {
                            wattron(win, COLOR_PAIR(1)); print_line_in_box(win, y - scroll_offset, 4, line_to_print); wattroff(win, COLOR_PAIR(1));
                        } else {
                            print_line_in_box(win, y - scroll_offset, 4, line_to_print);
                        }
                    }
                    y++;
                }
            }
        }
    }
    return total_content_lines;
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
    char version_title[64];
    sprintf(version_title, "HDHomeRun TUI Help (v%s)", TUI_VERSION);

    const char *help_text[] = {
        version_title,
        "",
        "KEY BINDINGS:",
        "  Up/Dn Arrows : Select tuner to view.",
        "  PgUp/PgDn    : Scroll status panel if content overflows.",
        "  Lf/Rt Arrows : Change channel.",
        "  +/- Keys     : Seek for next/previous active channel.",
        "  v            : View stream in VLC (select program for ATSC 1.0).",
        "  d (ATSC 3.0) : Show detailed PLP information and SNR requirements.",
        "  c            : Manually tune to a channel/frequency.",
        "  m            : Change the tuner's channel map.",
        "  p            : Set the tuned ATSC 3.0 PLPs.",
        "  s (ATSC 1.0) : Save a 30-second transport stream capture.",
        "  s (ATSC 3.0) : Save a 30-second debug capture.",
        "  a (ATSC 1.0) : Save a 30-second TS capture with error checking.",
        "  a (ATSC 3.0) : Save a 30-second DBG capture with error checking.",
        "  x (ATSC 3.0) : Save a 30-second PCAP capture, if supported.",
        "  z (ATSC 3.0) : Save a 30-second PCAP capture with error checking.",
        "  Backspace    : During a save, press Backspace to abort.",
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
        
        mvwprintw(help_win, getmaxy(help_win) - 2, 2, "Scroll: Up/Down/PgUp/PgDn | Close: h or Enter | Quit: q");
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
            case KEY_PPAGE:
                scroll_pos -= max_display_lines;
                if (scroll_pos < 0) scroll_pos = 0;
                break;
            case KEY_NPAGE:
                if (num_lines > max_display_lines) {
                    scroll_pos += max_display_lines;
                    if (scroll_pos > num_lines - max_display_lines) {
                        scroll_pos = num_lines - max_display_lines;
                    }
                }
                break;
            case 'q':
                delwin(help_win);
                return 1;
            case 'h':
            case '\n':
            case '\r':
                delwin(help_win);
                nodelay(stdscr, TRUE);
                return 0;
        }
    }
}

/*
 * http_save_stream
 * Performs a download of an HTTP stream using native sockets, replacing wget.
 * Returns 0 on success, -1 on failure.
 */
int http_save_stream(const char *ip_addr, const char *url, const char *filename, WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, bool autorestart_enabled, int save_attempts, int max_save_attempts, bool *out_aborted, bool *out_error_detected, bool debug_enabled) {
    *out_aborted = false;
    *out_error_detected = false;

    // 1. Create and connect socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int rcvbuf_size = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    if (sock < 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Could not create socket."); wrefresh(win); sleep(2);
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(5004);
    if (inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr) <= 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Invalid IP address."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Could not connect to device."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    // 2. Send HTTP GET request
    const char *path_start = strstr(url, "/auto/");
    if (!path_start) {
        print_line_in_box(win, LINES - 3, 2, "Error: Invalid URL for request."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }
    char request[512];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path_start, ip_addr);
    if (send(sock, request, strlen(request), 0) < 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Failed to send request."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    // 3. Open output file
    FILE *f = fopen(filename, "wb");
    if (!f) {
        print_line_in_box(win, LINES - 3, 2, "Error: Failed to open file for writing."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    // 4. Receive data in a non-blocking loop
    fcntl(sock, F_SETFL, O_NONBLOCK);
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    long elapsed_ms = 0;
    bool headers_processed = false;
    char buffer[65536]; // Increased buffer size
    
    while (elapsed_ms < 30000) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 + (current_time.tv_nsec - start_time.tv_nsec) / 1000000;
        long remaining_s = (30000 - elapsed_ms) / 1000;
        if (remaining_s < 0) remaining_s = 0;

        // Update UI
        draw_status_pane(win, hd, tuner_info, 0);
        mvwhline(win, LINES - 5, 1, ' ', getmaxx(win) - 2);
        mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
        mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
        
        if (debug_enabled) {
            print_line_in_box(win, LINES - 5, 2, "URL: %s", url);
        }
        print_line_in_box(win, LINES - 4, 2, "Saving to %s... %lds remaining.", filename, remaining_s);
        if (autorestart_enabled) {
            print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop. (Attempt %d/%d)", save_attempts, max_save_attempts);
        } else {
            print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop.");
        }
        wrefresh(win);

        // Check for user abort
        int ch = getch();
        if (ch == KEY_BACKSPACE) {
            *out_aborted = true;
            break;
        }

        // Check for signal errors if autorestart is on
        if (autorestart_enabled && (elapsed_ms >= 2000)) {
            struct hdhomerun_tuner_status_t current_status;
            char *current_raw_status;
            if (hdhomerun_device_get_tuner_status(hd, &current_raw_status, &current_status) > 0) {
                if (current_status.symbol_error_quality < 100) {
                    *out_error_detected = true;
                    break;
                }
            }
        }

        // Receive data from socket
        int bytes_read = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            char *data_to_write = buffer;
            int len_to_write = bytes_read;
            if (!headers_processed) {
                char *body_start = strstr(buffer, "\r\n\r\n");
                if (body_start) {
                    body_start += 4; // Move pointer past the CRLFCRLF
                    len_to_write = bytes_read - (body_start - buffer);
                    data_to_write = body_start;
                    headers_processed = true;
                } else {
                    // Headers not fully received in this chunk, so write nothing yet
                    len_to_write = 0;
                }
            }
            if (len_to_write > 0) {
                fwrite(data_to_write, 1, len_to_write, f);
            }
        } else if (bytes_read == 0) {
            // Connection closed by server, successful completion
            break;
        } else { // bytes_read < 0
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                // A real error occurred
                break;
            }
            // No data right now, just loop again
        }
    }

    fclose(f);
    close(sock);
    return 0;
}


/*
 * save_stream
 * Saves a 30-second transport stream capture to a file.
 */
char* save_stream(struct hdhomerun_device_t *hd, WINDOW *win, enum save_mode mode, struct unified_tuner *tuner_info, bool debug_enabled) {
    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) <= 0) {
        print_line_in_box(win, LINES - 3, 2, "Failed to get tuner status."); wrefresh(win); sleep(2);
        return NULL;
    }

    char original_channel[128];
    strncpy(original_channel, status.channel, sizeof(original_channel));
    original_channel[sizeof(original_channel) - 1] = '\0';

    if (strstr(status.lock_str, "none") != NULL) {
        print_line_in_box(win, LINES - 3, 2, "No signal lock. Cannot save stream."); wrefresh(win); sleep(2);
        return NULL;
    }

    bool is_pcap = (mode == SAVE_NORMAL_PCAP || mode == SAVE_AUTORESTART_PCAP);
    if (is_pcap) {
        if (parse_db_value(raw_status_str, "ss=") == -999) {
            print_line_in_box(win, LINES - 3, 2, "PCAP capture not available on this device model."); wrefresh(win); sleep(2);
            return NULL;
        }
    }

    unsigned int rf_channel = 0;
    char *p = strchr(status.channel, ':');
    if (!p) p = status.channel; else p++;
    if (isdigit((unsigned char)*p)) rf_channel = strtoul(p, NULL, 10);

    long id_val = 0;
    char *streaminfo;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
        long parsed_id = parse_status_value(streaminfo, "tsid=");
        if (parsed_id != -999) id_val = parsed_id;
    }
    
    bool is_atsc3 = (strstr(status.lock_str, "atsc3") != NULL);
    if (is_atsc3) {
        char *plpinfo;
        if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
            long bsid = parse_status_value(plpinfo, "bsid=");
            if (bsid != -999) id_val = bsid;
        }
    }

    bool autorestart_enabled = (mode == SAVE_AUTORESTART_TS || mode == SAVE_AUTORESTART_DBG || mode == SAVE_AUTORESTART_PCAP);
    char *result_str = NULL;

    // --- ATSC 3.0 Capture Logic ---
    if (is_atsc3) {
        int save_attempts = 0;
        const int max_save_attempts = 5;
        
        while(1) { // Loop to allow for auto-restarting
            const char *format = is_pcap ? "ipv4-pcap" : "dbg";
            const char *ext = is_pcap ? ".pcap" : ".dbg";
            
            char plp_str[128] = {0};
            bool plps_locked = false;
            for (int retry = 0; retry < 4; retry++) { // Initial attempt + 3 retries
                char *plpinfo;
                if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
                    char *plpinfo_copy = strdup(plpinfo);
                    if (plpinfo_copy) {
                        char *line = strtok(plpinfo_copy, "\n");
                        while(line != NULL) {
                            if (strstr(line, "lock=1")) {
                                int plp_id;
                                if (sscanf(line, "%d:", &plp_id) == 1) {
                                    char temp[8];
                                    sprintf(temp, "p%d", plp_id);
                                    strcat(plp_str, temp);
                                }
                            }
                            line = strtok(NULL, "\n");
                        }
                        free(plpinfo_copy);
                    }
                }
                if (strlen(plp_str) > 0) {
                    plps_locked = true;
                    break; // Success
                }
                
                if (retry < 3) {
                    mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
                    mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
                    print_line_in_box(win, LINES - 4, 2, "Could not lock PLPs, retrying... (%d/3)", retry + 1);
                    wrefresh(win);
                    sleep(1);
                }
            }

            if (!plps_locked) {
                mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
                print_line_in_box(win, LINES - 3, 2, "No locked PLPs found for ATSC 3.0 capture."); wrefresh(win); sleep(2);
                goto restore_and_exit;
            }

            save_attempts++;

            char filename[256];
            char time_str[20];
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
            sprintf(filename, "rf%u-bsid%ld-%s-%s%s", rf_channel, id_val, plp_str, time_str, ext);
            
            char url[256];
            sprintf(url, "http://%s:5004/auto/ch%u%s?format=%s", tuner_info->ip_str, rf_channel, plp_str, format);
            
            bool aborted = false;
            bool error_detected = false;

            // Call the native HTTP download function instead of fork/wget
            http_save_stream(tuner_info->ip_str, url, filename, win, hd, tuner_info, autorestart_enabled, save_attempts, max_save_attempts, &aborted, &error_detected, debug_enabled);

            if (autorestart_enabled && error_detected && save_attempts < max_save_attempts) {
                remove(filename);
                mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
                mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
                print_line_in_box(win, LINES - 4, 2, "Symbol Quality error. Restarting capture in 1s... (Attempt %d/%d)", save_attempts, max_save_attempts); wrefresh(win);
                
                napms(500);
                hdhomerun_device_set_tuner_channel(hd, original_channel);
                struct hdhomerun_tuner_status_t lock_status;
                hdhomerun_device_wait_for_lock(hd, &lock_status);
                
                sleep(1);
                continue; // Continue the while loop to retry
            } else if (autorestart_enabled && error_detected && save_attempts >= max_save_attempts) {
                remove(filename);
                result_str = (char*)malloc(512);
                sprintf(result_str, "Signal too unstable. Failed after %d attempts.", max_save_attempts);
                break;
            }
            
            result_str = (char*)malloc(512);
            if (aborted) {
                sprintf(result_str, "Save aborted. Partial file %s may remain.", filename);
            } else {
                sprintf(result_str, "Saved capture to %s", filename);
            }
            break; // Exit the while loop
        }
        goto restore_and_exit; // Jump to restoration code
    }

    // --- ATSC 1.0 Capture Logic ---
    while(1) {
        char filename[128];
        char time_str[20];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
        sprintf(filename, "rf%u-tsid%ld-%s.ts", rf_channel, id_val, time_str);

        print_line_in_box(win, LINES - 4, 2, "Starting capture..."); wrefresh(win);

        char debug_path[64];
        sprintf(debug_path, "/tuner%d/debug", tuner_info->tuner_index);
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

        struct timespec start_time, current_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        long elapsed_ms = 0;
        bool error_detected = false;
        bool aborted = false;
        unsigned long long total_bytes = 0;

        while(elapsed_ms < 30000) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 + (current_time.tv_nsec - start_time.tv_nsec) / 1000000;
            long remaining_s = (30000 - elapsed_ms) / 1000;
            if (remaining_s < 0) remaining_s = 0;

            mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
            mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
            print_line_in_box(win, LINES - 4, 2, "Saving to %s... %lds remaining.", filename, remaining_s);
            print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop.");
            wrefresh(win);

            size_t actual_size;
            uint8_t *video_data = hdhomerun_device_stream_recv(hd, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);

            if (video_data && actual_size > 0) {
                fwrite(video_data, 1, actual_size, f);
                total_bytes += actual_size;
            }

            if (autorestart_enabled) {
                hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
                long current_te = parse_status_value(debug_str, "te=");
                long current_ne = parse_status_value(debug_str, "ne=");
                long current_se = parse_status_value(debug_str, "se=");

                if (current_te > start_te || current_ne > start_ne || current_se > start_se) {
                    error_detected = true;
                    break;
                }
            }
            if (getch() == KEY_BACKSPACE) {
                aborted = true;
                break;
            }
        }

        fclose(f);
        hdhomerun_device_stream_stop(hd);
        
        if (aborted) {
            result_str = (char*)malloc(512);
            sprintf(result_str, "Save aborted. Partial file %s may remain.", filename);
            
            // ATSC 1.0 doesn't need restoration - tuner continues running
            return result_str;
        }

        hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
        long end_te = parse_status_value(debug_str, "te=");
        long end_ne = parse_status_value(debug_str, "ne=");
        long end_se = parse_status_value(debug_str, "se=");

        if (autorestart_enabled && error_detected) {
            remove(filename);
            print_line_in_box(win, LINES - 4, 2, "Error detected. Restarting capture in 1s..."); wrefresh(win);
            sleep(1);
            continue;
        }
        
        result_str = (char*)malloc(512);
        sprintf(result_str, "Saved %.2f MB to %s\nErrors: %ld transport, %ld network, %ld sequence", 
            (double)total_bytes / (1024*1024), filename,
            end_te - start_te, end_ne - start_ne, end_se - start_se);
        
        // ATSC 1.0 doesn't need restoration - tuner continues running
        return result_str;
    }

restore_and_exit:
    // Common restoration point for ATSC 3.0
    // Give the tuner a moment to settle after the capture operation
    napms(500); // Half second delay
    
    hdhomerun_device_set_tuner_channel(hd, original_channel);
    struct hdhomerun_tuner_status_t lock_status;
    hdhomerun_device_wait_for_lock(hd, &lock_status);
    
    return result_str; // May be NULL if there was an early error
}

/*
 * get_udp_port
 * Finds a free ephemeral UDP port for streaming.
 * Returns the port number on success, -1 on failure.
 */
int get_udp_port() {
    struct sockaddr_in sin;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY); 
    sin.sin_port = 0; // Ask OS for a free port
    
    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(sock);
        return -1;
    }

    socklen_t len = sizeof(sin);
    if (getsockname(sock, (struct sockaddr *)&sin, &len) < 0) {
        close(sock);
        return -1;
    }

    int port = ntohs(sin.sin_port);
    close(sock);
    return port;
}


/*
 * select_program_menu
 * Displays a menu of available programs for the user to select.
 * Returns 1 on selection, 0 on cancellation.
 */
int select_program_menu(WINDOW *win, char *streaminfo_str, char *selected_program_str, int *selected_plp) {
    *selected_plp = -1;
    struct program_info {
        char display_str[256];
        char program_num_str[16];
        int plp;
    } programs[MAX_PROGRAMS];
    int program_count = 0;

    char *streaminfo_copy = strdup(streaminfo_str);
    if (!streaminfo_copy) return 0;

    char *line = strtok(streaminfo_copy, "\n");
    while (line != NULL && program_count < MAX_PROGRAMS) {
        while (isspace((unsigned char)*line)) line++;

        // Check for ATSC 3.0 format: "program=..."
        if (strncmp(line, "program=", 8) == 0) {
            strncpy(programs[program_count].display_str, line, sizeof(programs[0].display_str) - 1);
            sscanf(line, "program=%s", programs[program_count].program_num_str);
            char* plp_start = strstr(line, "plp=");
            if (plp_start) {
                sscanf(plp_start, "plp=%d", &programs[program_count].plp);
            } else {
                programs[program_count].plp = -1;
            }
            program_count++;
        } 
        // Check for ATSC 1.0 format: "program [num]: ..." or "[num]: ..."
        else if (strncmp(line, "program ", 8) == 0 || (isdigit((unsigned char)*line) && strchr(line, ':'))) {
            strncpy(programs[program_count].display_str, line, sizeof(programs[0].display_str) - 1);
            if (strncmp(line, "program ", 8) == 0) {
                sscanf(line, "program %s", programs[program_count].program_num_str);
            } else {
                 sscanf(line, "%s", programs[program_count].program_num_str);
            }
            char *colon = strchr(programs[program_count].program_num_str, ':');
            if (colon) *colon = '\0';
            programs[program_count].plp = -1;
            program_count++;
        }
        line = strtok(NULL, "\n");
    }
    free(streaminfo_copy);

    if (program_count == 0) return 0;

    int highlight = 0;
    int choice = -1;
    nodelay(stdscr, FALSE);

    while(choice == -1) {
        wclear(win);
        box(win, 0, 0);
        mvwprintw(win, 0, 2, " Select Program to View ");
        for (int i = 0; i < program_count; i++) {
            if (i + 2 >= getmaxy(win) - 2) break;
            if (i == highlight) wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 4, "%s", programs[i].display_str);
            if (i == highlight) wattroff(win, A_REVERSE);
        }
        mvwprintw(win, getmaxy(win) - 2, 2, "Select: Up/Down/Enter | Cancel: q");
        wrefresh(win);

        int key = getch();
        switch(key) {
            case KEY_UP:
                if (highlight > 0) highlight--;
                break;
            case KEY_DOWN:
                if (highlight < program_count - 1) highlight++;
                break;
            case '\n':
            case '\r':
                choice = highlight;
                break;
            case 'q':
                choice = -2; // Cancel
                break;
        }
    }

    nodelay(stdscr, TRUE);

    if (choice >= 0) {
        strncpy(selected_program_str, programs[choice].program_num_str, 15);
        selected_program_str[15] = '\0';
        *selected_plp = programs[choice].plp;
    }

    return (choice >= 0);
}

/*
 * stream_to_vlc
 * Manages the process of starting and stopping a video stream to VLC.
 */
char* stream_to_vlc(struct hdhomerun_device_t *hd, WINDOW *win, pid_t *vlc_pid, struct unified_tuner *tuner_info) {
    char tuner_target_path[64];
    sprintf(tuner_target_path, "/tuner%d/target", tuner_info->tuner_index);

    // If VLC is already running, stop it.
    if (*vlc_pid > 0) {
        kill(*vlc_pid, SIGTERM);
        waitpid(*vlc_pid, NULL, 0);
        hdhomerun_device_set_var(hd, tuner_target_path, "none", NULL, NULL);
        *vlc_pid = 0;
        return strdup("VLC stream stopped.");
    }

    // Check for signal lock before starting
    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) <= 0 || strstr(status.lock_str, "none") != NULL) {
        return strdup("No signal lock. Cannot start stream.");
    }
    
    // Get the list of available programs
    char *streaminfo_str;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_str) <= 0) {
        return strdup("Failed to get program list.");
    }

    // Show menu for user to select a program
    char selected_program[16] = {0};
    int selected_plp = -1;
    if (!select_program_menu(win, streaminfo_str, selected_program, &selected_plp)) {
        return NULL; // User cancelled the menu
    }
    
    // Set the tuner to the selected program
    char tuner_program_path[64];
    sprintf(tuner_program_path, "/tuner%d/program", tuner_info->tuner_index);
    if (hdhomerun_device_set_var(hd, tuner_program_path, selected_program, NULL, NULL) < 0) {
        char err_msg[128];
        sprintf(err_msg, "Failed to set program to %s.", selected_program);
        return strdup(err_msg);
    }

    // Get a free port for the RTP stream
    int port = get_udp_port();
    if (port < 0) {
        return strdup("Could not find a free UDP port.");
    }

    // Dynamically determine the local IP address that connects to the HDHomeRun
    char local_ip[INET_ADDRSTRLEN];
    int temp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (temp_sock < 0) { return strdup("Socket error determining local IP."); }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(65001); // Connect to a port on the HDHomeRun to find the interface
    if (inet_pton(AF_INET, tuner_info->ip_str, &serv.sin_addr) <= 0) {
        close(temp_sock);
        return strdup("Invalid device IP address.");
    }

    if (connect(temp_sock, (const struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(temp_sock);
        return strdup("Connect error determining local IP.");
    }

    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(temp_sock, (struct sockaddr *)&name, &namelen) < 0) {
        close(temp_sock);
        return strdup("getsockname error determining local IP.");
    }
    close(temp_sock);

    if (inet_ntop(AF_INET, &name.sin_addr, local_ip, sizeof(local_ip)) == NULL) {
        return strdup("inet_ntop error determining local IP.");
    }

    // Set the stream target on the tuner
    char target_str[128];
    sprintf(target_str, "rtp://%s:%d", local_ip, port);
    if (hdhomerun_device_set_var(hd, tuner_target_path, target_str, NULL, NULL) < 0) {
        return strdup("Failed to set stream target.");
    }

    // Fork and execute VLC to listen for the stream
    *vlc_pid = fork();
    if (*vlc_pid == 0) { // Child process for VLC
        // Redirect stdout and stderr to /dev/null to prevent TUI corruption
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null != -1) {
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
        
        char vlc_url[128];
        sprintf(vlc_url, "rtp://@:%d", port);
        execlp("vlc", "vlc", vlc_url, NULL);
        // If execlp fails, print error and exit child process
        perror("execlp for vlc failed");
        _exit(1);
    } else if (*vlc_pid < 0) {
        return strdup("Failed to fork for VLC.");
    }

    return strdup("Streaming to VLC...");
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
    static int status_scroll_offset = 0;
    int total_content_lines = 0;

    // State for VLC piping
    static pid_t vlc_pid = 0;
    // State for mouse control
    static bool mouse_scroll_enabled = false;
    // State for debug mode
    static bool debug_mode_enabled = false;

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
        bool tuner_changed = false;
        struct unified_tuner *selected_tuner = (total_tuners > 0) ? &tuners[highlight] : NULL;

        if (selected_tuner) {
            if (hd == NULL || current_device_id != selected_tuner->device_id) {
                if (hd) hdhomerun_device_destroy(hd);
                char device_id_str[16];
                sprintf(device_id_str, "%08X", selected_tuner->device_id);
                hd = hdhomerun_device_create_from_str(device_id_str, NULL);
                current_device_id = selected_tuner->device_id;
                status_scroll_offset = 0;
                tuner_changed = true;
            }
            if (hd) {
                hdhomerun_device_set_tuner(hd, selected_tuner->tuner_index);
                if (chan_list.count == 0 || tuner_changed) {
                    populate_channel_list(hd, &chan_list);
                    status_scroll_offset = 0;
                }
            }
        }
        
        // If tuner changed while VLC was running, stop it.
        if (tuner_changed && vlc_pid > 0) {
            kill(vlc_pid, SIGTERM); waitpid(vlc_pid, NULL, 0); vlc_pid = 0;
            hdhomerun_device_set_tuner_target(hd, "none");
            if (persistent_message) free(persistent_message);
            persistent_message = strdup("VLC stopped due to tuner change.");
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
        
        total_content_lines = draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
        
        // Check if current tuner is ATSC3 to adjust hint text
        bool is_atsc3 = false;
        if(hd) {
            struct hdhomerun_tuner_status_t current_status;
            char *raw_status;
            if(hdhomerun_device_get_tuner_status(hd, &raw_status, &current_status) > 0) {
                if(strstr(current_status.lock_str, "atsc3")) {
                    is_atsc3 = true;
                }
            }
        }

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
            if (vlc_pid > 0) {
                 mvwprintw(status_win, LINES - 2, 2, "v: Stop VLC | h: Help | q: Quit");
            } else if (total_content_lines > LINES - 4) {
                 mvwprintw(status_win, LINES - 2, 2, "PgUp/PgDn: Scroll | v: View | h: Help | q: Quit");
            } else {
                 if(is_atsc3) {
                    mvwprintw(status_win, LINES - 2, 2, "v: View | <-/->: Ch | h: Help | q: Quit");
                 } else {
                    mvwprintw(status_win, LINES - 2, 2, "v: View | <-/->: Ch | +/-: Seek | h: Help | q: Quit");
                 }
            }
        }

        wrefresh(tuner_win);
        wrefresh(status_win);

        int ch = getch();

        if (ch == KEY_MOUSE) {
            if (mouse_scroll_enabled) {
                MEVENT event;
                if (getmouse(&event) == OK) {
                    if (event.bstate & BUTTON4_PRESSED) { // Scroll up
                        if (status_scroll_offset > 0) status_scroll_offset--;
                    } else if (event.bstate & BUTTON5_PRESSED) { // Scroll down
                        if (status_scroll_offset < total_content_lines - (LINES - 4)) status_scroll_offset++;
                    }
                }
            }
            continue; // Always continue to avoid switch statement processing mouse
        }

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
                if (vlc_pid > 0) { 
                    kill(vlc_pid, SIGTERM); 
                    waitpid(vlc_pid, NULL, 0); 
                    hdhomerun_device_set_tuner_target(hd, "none");
                }
                if (hd) hdhomerun_device_destroy(hd);
                delwin(tuner_win); delwin(status_win);
                return 0;

            case 'r':
                if (vlc_pid > 0) { 
                    kill(vlc_pid, SIGTERM); 
                    waitpid(vlc_pid, NULL, 0); 
                    vlc_pid = 0; 
                    hdhomerun_device_set_tuner_target(hd, "none");
                }
                if (hd) { hdhomerun_device_destroy(hd); hd = NULL; current_device_id = 0; }
                chan_list.count = 0; status_scroll_offset = 0;
                total_tuners = discover_and_build_tuner_list(tuners);
                highlight = 0;
                if (total_tuners == 0) {
                    delwin(tuner_win); delwin(status_win);
                    return 1;
                }
                break;

            case KEY_UP: if (highlight > 0) { highlight--; chan_list.count = 0; status_scroll_offset = 0; tuner_changed=true; } break;
            case KEY_DOWN: if (highlight < total_tuners - 1) { highlight++; chan_list.count = 0; status_scroll_offset = 0; tuner_changed=true; } break;
            
            case KEY_PPAGE: if (status_scroll_offset > 0) status_scroll_offset--; break;
            case KEY_NPAGE: if (status_scroll_offset < total_content_lines - (LINES - 4)) status_scroll_offset++; break;

            case 'd':
                if(hd && is_atsc3) {
                    if (show_plp_details_screen(status_win, hd, selected_tuner) == 1) { // Quit requested
                         if (hd) hdhomerun_device_destroy(hd);
                         delwin(tuner_win); delwin(status_win);
                         return 0;
                    }
                }
                break;

            case 'v':
                if (!hd) break;
                if (persistent_message) free(persistent_message);
                persistent_message = stream_to_vlc(hd, status_win, &vlc_pid, &tuners[highlight]);
                break;

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
                        status_scroll_offset = 0;
                        
                        wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
                        box(status_win, 0, 0);
                        print_line_in_box(status_win, LINES - 3, 2, "Seeking %s on ch %u...", (seek_direction == 1) ? "Up" : "Down", new_channel);
                        draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
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
                            
                            draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
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
                    draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
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
                    status_scroll_offset = 0;
                }
                break;
            
            case 's':
            case 'a':
            case 'x':
            case 'z':
                if (!hd) break;
                {
                    struct hdhomerun_tuner_status_t status;
                    char *raw_status_str;
                    bool is_atsc3_save = false;
                    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
                        if (strstr(status.lock_str, "atsc3") != NULL) is_atsc3_save = true;
                    }

                    enum save_mode mode;
                    bool action_valid = true;
                    switch(ch) {
                        case 's': mode = is_atsc3_save ? SAVE_NORMAL_DBG : SAVE_NORMAL_TS; break;
                        case 'a': mode = is_atsc3_save ? SAVE_AUTORESTART_DBG : SAVE_AUTORESTART_TS; break;
                        case 'x': if (is_atsc3_save) { mode = SAVE_NORMAL_PCAP; } else { action_valid = false; } break;
                        case 'z': if (is_atsc3_save) { mode = SAVE_AUTORESTART_PCAP; } else { action_valid = false; } break;
                    }
                    
                    if (action_valid) {
                        persistent_message = save_stream(hd, status_win, mode, &tuners[highlight], debug_mode_enabled);
                    }
                }
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
                        status_scroll_offset = 0;
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
            
            case 'w': // Hidden toggle for mouse scroll
                mouse_scroll_enabled = !mouse_scroll_enabled;
                break;

            case 'g': // Hidden toggle for debug mode
                debug_mode_enabled = !debug_mode_enabled;
                break;

            case 'm': // Change channel map
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
                            status_scroll_offset = 0;
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
                                status_scroll_offset = 0;
                            }
                        }
                    }
                 }
                 break;
        }
        
        // Check if VLC has exited
        if (vlc_pid > 0) {
            int status;
            if (waitpid(vlc_pid, &status, WNOHANG) == vlc_pid) {
                // VLC has exited. Clean up.
                hdhomerun_device_set_tuner_target(hd, "none");
                vlc_pid = 0;
                if (persistent_message) free(persistent_message);
                persistent_message = strdup("VLC has been closed.");
            }
        }


        // Apply conditional polling rate based on device type
        if (selected_tuner) {
            if (selected_tuner->is_legacy) {
                napms(500); // Slower polling for legacy devices
            } else {
                napms(100); // Faster polling for modern devices
            }
        } else {
            napms(100); // Default if no tuners
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
    
    // Enable keypad for function keys, etc.
    keypad(stdscr, TRUE);

    // Enable mouse event tracking for scroll wheel up/down. This allows us
    // to differentiate between a real KEY_UP/DOWN from the keyboard and a
    // scroll event from the mouse, which we will ignore. Clicks and drags
    // for selection will be handled by the terminal as normal.
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);

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
        if (ch != 'r' && ch != 'R') {
            break;
        }
    }

    endwin();
    return 0;
}

// --- Base64 and L1 Parsing Helpers ---
// This section is a direct adaptation of the code from l1dump.c

int b64invs[] = { 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28,
    29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51 };

size_t b64_decoded_size(const char *in) {
    size_t len;
    size_t ret;
    size_t i;

    if (in == NULL) return 0;

    len = strlen(in);
    ret = len / 4 * 3;

    for (i = len; i-- > 0;) {
        if (in[i] == '=') {
            ret--;
        } else {
            break;
        }
    }
    return ret;
}

int b64_isvalidchar(char c) {
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c == '+' || c == '/' || c == '=') return 1;
    return 0;
}

int b64_decode(const char *in, unsigned char *out, size_t outlen) {
    size_t len;
    size_t i;
    size_t j;
    int v;

    if (in == NULL || out == NULL) return 0;

    len = strlen(in);
    if (outlen < b64_decoded_size(in) || len % 4 != 0) return 0;

    for (i = 0; i < len; i++) {
        if (!b64_isvalidchar(in[i])) {
            return 0;
        }
    }

    for (i = 0, j = 0; i < len; i += 4, j += 3) {
        v = b64invs[in[i] - 43];
        v = (v << 6) | b64invs[in[i + 1] - 43];
        v = in[i + 2] == '=' ? v << 6 : (v << 6) | b64invs[in[i + 2] - 43];
        v = in[i + 3] == '=' ? v << 6 : (v << 6) | b64invs[in[i + 3] - 43];

        out[j] = (v >> 16) & 0xFF;
        if (in[i + 2] != '=') out[j + 1] = (v >> 8) & 0xFF;
        if (in[i + 3] != '=') out[j + 2] = v & 0xFF;
    }

    return 1;
}

// Global variables for the bit parser, from l1dump.c
#define L1_DUMP_BUFFER_SIZE 512
static char bits[L1_DUMP_BUFFER_SIZE * 8];
static int bits_index = 0;

// get_bits function from l1dump.c
int get_bits(int count) {
    int i;
    long value = 0;
    
    if (bits_index + count > sizeof(bits)) {
        return 0;
    }

    for (i = count; i > 0; i--) {
        value |= bits[bits_index++] << (i - 1);
    }
    return value;
}


/*
 * parse_l1_data
 * Parses the decoded L1 data and adds formatted strings to the display list.
 * This is a direct adaptation of the parsing logic from l1dump.c
 */
void parse_l1_data(const unsigned char* data, size_t len, char** display_lines, int* line_count, int max_lines) {
    long value;
    int i, j, k;
    int l1b_version;
    int l1b_time_info_flag;
    int l1b_num_subframes;
    int l1b_l1_detail_size_bytes;
    int l1b_first_sub_mimo;
    int l1b_first_sub_sbs_first;
    int l1b_first_sub_sbs_last;
    int l1b_first_sub_mimo_mixed = 0;
    int l1d_version;
    int l1d_num_rf;
    int l1d_mimo = 0;
    int l1d_sbs_first = 0;
    int l1d_sbs_last = 0;
    int l1d_num_plp = 0;
    int l1d_plp_layer;
    int l1d_plp_mod = 0;
    int l1d_plp_TI_mode;
    int l1d_plp_num_channel_bonded;
    int l1d_plp_HTI_inter_subframe;
    int l1d_plp_HTI_num_ti_blocks;
    int l1d_mimo_mixed;

    // Helper to add a line to the display buffer safely
    #define add_line(...) \
        if (*line_count < max_lines) { \
            char line_buf[512]; \
            snprintf(line_buf, sizeof(line_buf), __VA_ARGS__); \
            display_lines[(*line_count)++] = strdup(line_buf); \
        }

    // Populate the bit buffer
    bits_index = 0;
    int bit_count = 0;
    for (i = 0; i < len && bit_count < sizeof(bits); i++) {
        for (int n = 7; n >= 0 && bit_count < sizeof(bits); n--) {
            bits[bit_count++] = (data[i] & (1 << n)) ? 1 : 0;
        }
    }
    
    add_line("--- L1-Basic Signaling ---");

    value = get_bits(3); add_line("L1B_version: %ld", value); l1b_version = value;
    value = get_bits(1); add_line("L1B_mimo_scattered_pilot_encoding: %s", value == 0 ? "Walsh-Hadamard" : "Null pilots");
    value = get_bits(1); add_line("L1B_lls_flag: %s", value == 0 ? "No LLS" : "LLS present");
    value = get_bits(2); l1b_time_info_flag = value;
    switch (value) {
        case 0: add_line("L1B_time_info_flag: Not included"); break;
        case 1: add_line("L1B_time_info_flag: ms precision"); break;
        case 2: add_line("L1B_time_info_flag: us precision"); break;
        case 3: add_line("L1B_time_info_flag: ns precision"); break;
    }
    value = get_bits(1); add_line("L1B_return_channel_flag: %ld", value);
    value = get_bits(2);
    switch (value) {
        case 0: add_line("L1B_papr_reduction: None"); break;
        case 1: add_line("L1B_papr_reduction: Tone reservation only"); break;
        case 2: add_line("L1B_papr_reduction: ACE only"); break;
        case 3: add_line("L1B_papr_reduction: Both TR and ACE"); break;
    }
    value = get_bits(1);
    if (value == 0) {
        add_line("L1B_frame_length_mode: Time-aligned");
        value = get_bits(10); add_line("  L1B_frame_length: %ld", value);
        value = get_bits(13); add_line("  L1B_excess_samples_per_symbol: %ld", value);
    } else {
        add_line("L1B_frame_length_mode: Symbol-aligned");
        value = get_bits(16); add_line("  L1B_time_offset: %ld", value);
        value = get_bits(7); add_line("  L1B_additional_samples: %ld", value);
    }
    value = get_bits(8); add_line("L1B_num_subframes: %ld", value + 1); l1b_num_subframes = value;
    value = get_bits(3); add_line("L1B_preamble_num_symbols: %ld", value + 1);
    value = get_bits(3); add_line("L1B_preamble_reduced_carriers: %ld", value);
    value = get_bits(2); add_line("L1B_L1_Detail_content_tag: %ld", value);
    value = get_bits(13); add_line("L1B_L1_Detail_size_bytes: %ld", value); l1b_l1_detail_size_bytes = value;
    value = get_bits(3); add_line("L1B_L1_Detail_fec_type: Mode %ld", value + 1);
    value = get_bits(2); add_line("L1B_L1_additional_parity_mode: K=%ld", value);
    value = get_bits(19); add_line("L1B_L1_Detail_total_cells: %ld", value);
    value = get_bits(1); add_line("L1B_first_sub_mimo: %s", value == 0 ? "No MIMO" : "MIMO"); l1b_first_sub_mimo = value;
    value = get_bits(2); add_line("L1B_first_sub_miso: %ld", value);
    value = get_bits(2); add_line("L1B_first_sub_fft_size: %s", (value < 3) ? (value==0?"8K":value==1?"16K":"32K") : "Reserved");
    value = get_bits(3); add_line("L1B_first_sub_reduced_carriers: %ld", value);
    value = get_bits(4);
    switch(value) {
        case 1: add_line("L1B_first_sub_guard_interval: GI_1_192"); break;
        case 2: add_line("L1B_first_sub_guard_interval: GI_2_384"); break;
        case 3: add_line("L1B_first_sub_guard_interval: GI_3_512"); break;
        case 4: add_line("L1B_first_sub_guard_interval: GI_4_768"); break;
        case 5: add_line("L1B_first_sub_guard_interval: GI_5_1024"); break;
        case 6: add_line("L1B_first_sub_guard_interval: GI_6_1536"); break;
        case 7: add_line("L1B_first_sub_guard_interval: GI_7_2048"); break;
        case 8: add_line("L1B_first_sub_guard_interval: GI_8_2432"); break;
        case 9: add_line("L1B_first_sub_guard_interval: GI_9_3072"); break;
        case 10: add_line("L1B_first_sub_guard_interval: GI_10_3648"); break;
        case 11: add_line("L1B_first_sub_guard_interval: GI_11_4096"); break;
        case 12: add_line("L1B_first_sub_guard_interval: GI_12_4864"); break;
        default: add_line("L1B_first_sub_guard_interval: Reserved (%ld)", value); break;
    }
    value = get_bits(11); add_line("L1B_first_sub_num_ofdm_symbols: %ld", value + 1);
    value = get_bits(5); add_line("L1B_first_sub_scattered_pilot_pattern: %ld", value);
    value = get_bits(3); add_line("L1B_first_sub_scattered_pilot_boost: %ld", value);
    value = get_bits(1); add_line("L1B_first_sub_sbs_first: %ld", value); l1b_first_sub_sbs_first = value;
    value = get_bits(1); add_line("L1B_first_sub_sbs_last: %ld", value); l1b_first_sub_sbs_last = value;
    if (l1b_version >= 1) {
        value = get_bits(1); add_line("L1B_first_sub_mimo_mixed: %ld", value); l1b_first_sub_mimo_mixed = value;
        get_bits(47);
    } else {
        get_bits(48);
    }
    value = get_bits(32); add_line("L1B_crc: 0x%08lx", value);
    
    add_line(" ");
    add_line("--- L1-Detail Signaling ---");
    
    value = get_bits(4); add_line("L1D_version: %ld", value); l1d_version = value;
    value = get_bits(3); add_line("L1D_num_rf: %ld", value); l1d_num_rf = value;
    for (i = 1; i <= l1d_num_rf; i++) {
        value = get_bits(16); add_line("  L1D_bonded_bsid: 0x%04lx", value);
        get_bits(3);
    }
    if (l1b_time_info_flag != 0) {
        value = get_bits(32); add_line("L1D_time_sec: %ld", value);
        value = get_bits(10); add_line("L1D_time_msec: %ld", value);
        if (l1b_time_info_flag > 1) {
            value = get_bits(10); add_line("L1D_time_usec: %ld", value);
            if (l1b_time_info_flag > 2) {
                value = get_bits(10); add_line("L1D_time_nsec: %ld", value);
            }
        }
    }
    for (i = 0; i <= l1b_num_subframes; i++) {
        add_line(" "); add_line("Subframe #%d:", i);
        if (i > 0) {
            value = get_bits(1); add_line("  L1D_mimo: %s", value == 0 ? "No MIMO" : "MIMO"); l1d_mimo = value;
            value = get_bits(2); add_line("  L1D_miso: %ld", value);
            value = get_bits(2); add_line("  L1D_fft_size: %s", (value < 3) ? (value==0?"8K":value==1?"16K":"32K") : "Reserved");
            value = get_bits(3); add_line("  L1D_reduced_carriers: %ld", value);
            value = get_bits(4);
            switch(value) {
                case 1: add_line("  L1D_guard_interval: GI_1_192"); break;
                case 2: add_line("  L1D_guard_interval: GI_2_384"); break;
                case 3: add_line("  L1D_guard_interval: GI_3_512"); break;
                case 4: add_line("  L1D_guard_interval: GI_4_768"); break;
                case 5: add_line("  L1D_guard_interval: GI_5_1024"); break;
                case 6: add_line("  L1D_guard_interval: GI_6_1536"); break;
                case 7: add_line("  L1D_guard_interval: GI_7_2048"); break;
                case 8: add_line("  L1D_guard_interval: GI_8_2432"); break;
                case 9: add_line("  L1D_guard_interval: GI_9_3072"); break;
                case 10: add_line("  L1D_guard_interval: GI_10_3648"); break;
                case 11: add_line("  L1D_guard_interval: GI_11_4096"); break;
                case 12: add_line("  L1D_guard_interval: GI_12_4864"); break;
                default: add_line("  L1D_guard_interval: Reserved (%ld)", value); break;
            }
            value = get_bits(11); add_line("  L1D_num_ofdm_symbols: %ld", value + 1);
            value = get_bits(5); add_line("  L1D_scattered_pilot_pattern: %ld", value);
            value = get_bits(3); add_line("  L1D_scattered_pilot_boost: %ld", value);
            value = get_bits(1); add_line("  L1D_sbs_first: %ld", value); l1d_sbs_first = value;
            value = get_bits(1); add_line("  L1D_sbs_last: %ld", value); l1d_sbs_last = value;
        }
        if (l1b_num_subframes > 0) {
            value = get_bits(1); add_line("  L1D_subframe_multiplex: %ld", value);
        }
        value = get_bits(1); add_line("  L1D_frequency_interleaver: %s", value == 0 ? "Preamble Only" : "All Symbols");
        if ((i == 0 && (l1b_first_sub_sbs_first == 1 || l1b_first_sub_sbs_last == 1)) || (i > 0 && (l1d_sbs_first == 1 || l1d_sbs_last == 1))) {
            value = get_bits(13); add_line("  L1D_sbs_null_cells: %ld", value);
        }
        value = get_bits(6); add_line("  L1D_num_plp: %ld", value + 1); l1d_num_plp = value;
        for (j = 0; j <= l1d_num_plp; j++) {
            add_line("    PLP #%d:", j);
            value = get_bits(6); add_line("      L1D_plp_id: %ld", value);
            value = get_bits(1); add_line("      L1D_plp_lls_flag: %ld", value);
            value = get_bits(2); add_line("      L1D_plp_layer: %s", (value==0) ? "Core" : (value==1 ? "Enhanced" : "Reserved")); l1d_plp_layer = value;
            value = get_bits(24); add_line("      L1D_plp_start: %ld", value);
            value = get_bits(24); add_line("      L1D_plp_size: %ld", value);
            value = get_bits(2); add_line("      L1D_plp_scrambler_type: %s", (value==0) ? "PRBS" : "Reserved");
            value = get_bits(4);
            switch (value) {
                case 0: add_line("      L1D_plp_fec_type: BCH + 16K LDPC"); break;
                case 1: add_line("      L1D_plp_fec_type: BCH + 64K LDPC"); break;
                case 2: add_line("      L1D_plp_fec_type: CRC + 16K LDPC"); break;
                case 3: add_line("      L1D_plp_fec_type: CRC + 64K LDPC"); break;
                case 4: add_line("      L1D_plp_fec_type: 16K LDPC only"); break;
                case 5: add_line("      L1D_plp_fec_type: 64K LDPC only"); break;
                default: add_line("      L1D_plp_fec_type: Reserved"); break;
            }
            if (value <= 5) {
                value = get_bits(4); l1d_plp_mod = value;
                switch (value) {
                    case 0: add_line("      L1D_plp_mod: QPSK"); break;
                    case 1: add_line("      L1D_plp_mod: 16QAM"); break;
                    case 2: add_line("      L1D_plp_mod: 64QAM"); break;
                    case 3: add_line("      L1D_plp_mod: 256QAM"); break;
                    case 4: add_line("      L1D_plp_mod: 1024QAM"); break;
                    case 5: add_line("      L1D_plp_mod: 4096QAM"); break;
                    default: add_line("      L1D_plp_mod: Reserved"); break;
                }
                value = get_bits(4);
                switch (value) {
                    case 0: add_line("      L1D_plp_cod: 2/15"); break;
                    case 1: add_line("      L1D_plp_cod: 3/15"); break;
                    case 2: add_line("      L1D_plp_cod: 4/15"); break;
                    case 3: add_line("      L1D_plp_cod: 5/15"); break;
                    case 4: add_line("      L1D_plp_cod: 6/15"); break;
                    case 5: add_line("      L1D_plp_cod: 7/15"); break;
                    case 6: add_line("      L1D_plp_cod: 8/15"); break;
                    case 7: add_line("      L1D_plp_cod: 9/15"); break;
                    case 8: add_line("      L1D_plp_cod: 10/15"); break;
                    case 9: add_line("      L1D_plp_cod: 11/15"); break;
                    case 10: add_line("      L1D_plp_cod: 12/15"); break;
                    case 11: add_line("      L1D_plp_cod: 13/15"); break;
                    default: add_line("      L1D_plp_cod: Reserved"); break;
                }
            }
            value = get_bits(2); l1d_plp_TI_mode = value;
            switch (value) {
                case 0: add_line("      L1D_plp_TI_mode: No TI"); break;
                case 1: add_line("      L1D_plp_TI_mode: CTI"); break;
                case 2: add_line("      L1D_plp_TI_mode: HTI"); break;
                default: add_line("      L1D_plp_TI_mode: Reserved"); break;
            }
            if (l1d_plp_TI_mode == 0) { value = get_bits(15); add_line("      L1D_plp_fec_block_start: %ld", value); }
            else if (l1d_plp_TI_mode == 1) { value = get_bits(22); add_line("      L1D_plp_CTI_fec_block_start: %ld", value); }
            if (l1d_num_rf > 0) {
                value = get_bits(3); add_line("      L1D_plp_num_channel_bonded: %ld", value); l1d_plp_num_channel_bonded = value;
                if (l1d_plp_num_channel_bonded > 0) {
                    value = get_bits(2); add_line("      L1D_plp_channel_bonding_format: %ld", value);
                    for (k = 0; k < l1d_plp_num_channel_bonded; k++) {
                        value = get_bits(3); add_line("        L1D_plp_bonded_rf_id: %ld", value);
                    }
                }
            }
            if ((i == 0 && l1b_first_sub_mimo == 1) || (i > 0 && l1d_mimo)) {
                value = get_bits(1); add_line("      L1D_plp_mimo_stream_combining: %ld", value);
                value = get_bits(1); add_line("      L1D_plp_mimo_IQ_interleaving: %ld", value);
                value = get_bits(1); add_line("      L1D_plp_mimo_PH: %ld", value);
            }
            if (l1d_plp_layer == 0) {
                value = get_bits(1);
                if (value == 0) { add_line("      L1D_plp_type: non-dispersed"); }
                else {
                    add_line("      L1D_plp_type: dispersed");
                    value = get_bits(14); add_line("      L1D_plp_num_subslices: %ld", value + 1);
                    value = get_bits(24); add_line("      L1D_plp_subslice_interval: %ld", value);
                }
                if ((l1d_plp_TI_mode == 1 || l1d_plp_TI_mode == 2) && l1d_plp_mod == 0) {
                    value = get_bits(1);
                    add_line("      L1D_plp_TI_extended_interleaving: %ld", value);
                }
                if (l1d_plp_TI_mode == 1) {
                    value = get_bits(3); add_line("      L1D_plp_CTI_depth: %ld", value);
                    value = get_bits(11); add_line("      L1D_plp_CTI_start_row: %ld", value);
                } else if (l1d_plp_TI_mode == 2) {
                    value = get_bits(1); add_line("      L1D_plp_HTI_inter_subframe: %ld", value); l1d_plp_HTI_inter_subframe = value;
                    value = get_bits(4); add_line("      L1D_plp_HTI_num_ti_blocks: %ld", value + 1); l1d_plp_HTI_num_ti_blocks = value;
                    value = get_bits(12); add_line("      L1D_plp_HTI_num_fec_blocks_max: %ld", value + 1);
                    if (l1d_plp_HTI_inter_subframe == 0) {
                        value = get_bits(12); add_line("      L1D_plp_HTI_num_fec_blocks: %ld", value + 1);
                    } else {
                        for (k = 0; k <= l1d_plp_HTI_num_ti_blocks; k++) {
                            value = get_bits(12); add_line("        L1D_plp_HTI_num_fec_blocks: %ld", value + 1);
                        }
                    }
                    value = get_bits(1); add_line("      L1D_plp_HTI_cell_interleaver: %ld", value);
                }
            } else {
                value = get_bits(5); add_line("      L1D_plp_ldm_injection_level: %ld", value);
            }
        }
    }
    if (l1d_version >= 1) {
        value = get_bits(16); add_line("L1D_bsid: 0x%04lx", value);
    }
    if (l1d_version >= 2) {
        for (i = 0; i <= l1b_num_subframes; i++) {
            if (i > 0) {
                value = get_bits(1); add_line("  Subframe #%d L1D_mimo_mixed: %ld", i, value); l1d_mimo_mixed = value;
            }
            if ((i == 0 && l1b_first_sub_mimo_mixed == 1) || (i > 0 && l1d_mimo_mixed == 1)) {
                for (j = 0; j <= l1d_num_plp; j++) {
                    value = get_bits(1); add_line("    PLP #%d L1D_plp_mimo: %ld", j, value);
                    if (value == 1) {
                        value = get_bits(1); add_line("      L1D_plp_mimo_stream_combining: %ld", value);
                        value = get_bits(1); add_line("      L1D_plp_mimo_IQ_interleaving: %ld", value);
                        value = get_bits(1); add_line("      L1D_plp_mimo_PH: %ld", value);
                    }
                }
            }
        }
    }
    if ((((l1b_l1_detail_size_bytes * 8) - 32) - (bits_index - 200)) > 0) {
        get_bits(((l1b_l1_detail_size_bytes * 8) - 32) - (bits_index - 200));
    }
    value = get_bits(32); add_line("L1D_crc: 0x%08lx", value);
}



/*
 * show_plp_details_screen
 * Displays a detailed, scrollable view of ATSC 3.0 PLP info.
 */
int show_plp_details_screen(WINDOW *parent_win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info) {
    char *plpinfo_str_orig;
    char *streaminfo_str_orig;
    
    // Make copies of the strings immediately to avoid buffer overwrites
    if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str_orig) <= 0) {
        return 0; // No PLP info available
    }
    char *plpinfo_copy = strdup(plpinfo_str_orig);

    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_str_orig) <= 0) {
        streaminfo_str_orig = "";
    }
    char *streaminfo_copy = strdup(streaminfo_str_orig);


    char *display_lines[MAX_DISPLAY_LINES]; 
    int line_count = 0;

    // Add an initial blank line for spacing
    if (line_count < MAX_DISPLAY_LINES) {
        display_lines[line_count++] = strdup(" ");
    }

    long bsid = -999;
    long tsid = -999;

    if (plpinfo_copy) {
        bsid = parse_status_value(plpinfo_copy, "bsid=");
    }
    if (streaminfo_copy) {
        tsid = parse_status_value(streaminfo_copy, "tsid=");
    }

    if (bsid != -999) {
        char bsid_line[64];
        sprintf(bsid_line, "L1D BSID: %ld (0x%lX)", bsid, bsid);
        display_lines[line_count++] = strdup(bsid_line);
    } else {
        display_lines[line_count++] = strdup("L1D BSID: Not set");
    }

    if (tsid != -999) {
        char tsid_line[64];
        sprintf(tsid_line, "SLT TSID: %ld (0x%lX)", tsid, tsid);
        display_lines[line_count++] = strdup(tsid_line);
    } else {
        display_lines[line_count++] = strdup("SLT TSID: Not set");
    }

    if (line_count < MAX_DISPLAY_LINES) {
        display_lines[line_count++] = strdup(" "); // Add a blank line after the ID block
    }

    if(plpinfo_copy) {
        char *line = strtok(plpinfo_copy, "\n");
        while(line != NULL && line_count < MAX_DISPLAY_LINES) {
            if (strncmp(line, "bsid=", 5) != 0) {
                display_lines[line_count++] = strdup(line);
                
                char *mod_ptr = strstr(line, "mod=");
                char *cod_ptr = strstr(line, "cod=");

                if (mod_ptr && cod_ptr && line_count < MAX_DISPLAY_LINES) {
                    char raw_mod_str[16] = {0}, normalized_mod_str[16] = {0}, cod_str[8] = {0};
                    
                    const char *mod_val_start = mod_ptr + 4;
                    const char *mod_val_end = strchr(mod_val_start, ' ');
                    size_t mod_len = mod_val_end ? (size_t)(mod_val_end - mod_val_start) : strlen(mod_val_start);
                    if (mod_len < sizeof(raw_mod_str)) {
                        strncpy(raw_mod_str, mod_val_start, mod_len);
                        raw_mod_str[mod_len] = '\0';
                        normalize_mod_str(raw_mod_str, normalized_mod_str, sizeof(normalized_mod_str));
                    }

                    const char *cod_val_start = cod_ptr + 4;
                    const char *cod_val_end = strchr(cod_val_start, ' ');
                    size_t cod_len = cod_val_end ? (size_t)(cod_val_end - cod_val_start) : strlen(cod_val_start);
                    if (cod_len < sizeof(cod_str)) {
                        strncpy(cod_str, cod_val_start, cod_len);
                        cod_str[cod_len] = '\0';
                    }
                    
                    const struct modcod_snr *snr_data = get_snr_for_modcod(normalized_mod_str, cod_str);
                    if (snr_data) {
                        char snr_line[256] = {0};
                        sprintf(snr_line, "  -> Required SNR: Min %.2f dB, Max %.2f dB", snr_data->min_snr, snr_data->max_snr);
                        display_lines[line_count++] = strdup(snr_line);
                    }
                }
                if (line_count < MAX_DISPLAY_LINES) {
                    display_lines[line_count++] = strdup(" ");
                }
            }
            line = strtok(NULL, "\n");
        }
    }

    // --- Add L1 Detail if available ---
    char *raw_status_str;
    struct hdhomerun_tuner_status_t status;
    bool has_db_values = false;
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
        if (parse_db_value(raw_status_str, "ss=") != -999) has_db_values = true;
    }

    char *version_str;
    long version_num = 0;
    if (hdhomerun_device_get_var(hd, "/sys/version", &version_str, NULL) > 0) {
        char numeric_version_str[16] = {0};
        int i = 0;
        while(version_str[i] && isdigit((unsigned char)version_str[i]) && i < 15) {
            numeric_version_str[i] = version_str[i];
            i++;
        }
        version_num = atol(numeric_version_str);
    }

    if (has_db_values && version_num > 20250623) {
        char l1_path[64];
        sprintf(l1_path, "/tuner%d/l1detail", tuner_info->tuner_index);

        char *l1_detail_str;
        if (hdhomerun_device_get_var(hd, l1_path, &l1_detail_str, NULL) > 0) {
            // Add a separator before L1 detail info
            if (line_count < MAX_DISPLAY_LINES - 3) {
                display_lines[line_count++] = strdup(" ");
                display_lines[line_count++] = strdup("__HLINE__");
                display_lines[line_count++] = strdup(" ");
            }

            size_t decoded_len = b64_decoded_size(l1_detail_str);
            unsigned char *decoded_data = malloc(decoded_len);
            if (decoded_data) {
                if (b64_decode(l1_detail_str, decoded_data, decoded_len)) {
                    parse_l1_data(decoded_data, decoded_len, display_lines, &line_count, MAX_DISPLAY_LINES);
                }
                free(decoded_data);
            }
        }
    }

    int parent_h, parent_w, parent_y, parent_x;
    getmaxyx(parent_win, parent_h, parent_w);
    getbegyx(parent_win, parent_y, parent_x);

    WINDOW *detail_win = newwin(parent_h, parent_w, parent_y, parent_x);
    
    int scroll_pos = 0;
    keypad(detail_win, TRUE);
    nodelay(stdscr, FALSE);
    char message[512] = {0};

    while(1) {
        werase(detail_win);
        box(detail_win, 0, 0);
        mvwprintw(detail_win, 0, 2, " ATSC 3.0 PLP & L1 Details ");

        int max_display_lines = getmaxy(detail_win) - 4; // Reserve line for spacing
        for (int i = 0; i < max_display_lines; i++) {
            if (scroll_pos + i < line_count) {
                if (strcmp(display_lines[scroll_pos + i], "__HLINE__") == 0) {
                    mvwhline(detail_win, i + 1, 2, ACS_HLINE, getmaxx(detail_win) - 4);
                } else {
                    mvwprintw(detail_win, i + 1, 2, "%s", display_lines[scroll_pos + i]);
                }
            }
        }
        
        if (message[0] != '\0') {
            mvwprintw(detail_win, getmaxy(detail_win) - 2, 2, "%s", message);
        } else {
            mvwprintw(detail_win, getmaxy(detail_win) - 2, 2, "Scroll: Up/Dn | s: Save | d: Close | q: Quit");
        }
        wrefresh(detail_win);

        int ch = wgetch(detail_win);
        message[0] = '\0'; // Clear message on any new keypress

        switch(ch) {
            case KEY_UP: if (scroll_pos > 0) scroll_pos--; break;
            case KEY_DOWN: if (line_count > max_display_lines && scroll_pos < line_count - max_display_lines) scroll_pos++; break;
            case KEY_PPAGE: scroll_pos -= max_display_lines; if (scroll_pos < 0) scroll_pos = 0; break;
            case KEY_NPAGE:
                if (line_count > max_display_lines) {
                    scroll_pos += max_display_lines;
                    if (scroll_pos > line_count - max_display_lines) scroll_pos = line_count - max_display_lines;
                }
                break;
            case 's':
                {
                    unsigned int rf_channel = 0;
                    long id_val = -999;

                    char *status_str_s, *plpinfo_str_s, *streaminfo_str_s;
                    struct hdhomerun_tuner_status_t status_s;
                    
                    if (hdhomerun_device_get_tuner_status(hd, &status_str_s, &status_s) > 0) {
                        char *p = strchr(status_s.channel, ':');
                        if (!p) p = status_s.channel; else p++;
                        if (isdigit((unsigned char)*p)) rf_channel = strtoul(p, NULL, 10);
                    }
                    
                    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_str_s) > 0) {
                        char *si_copy = strdup(streaminfo_str_s);
                        if (si_copy) {
                            id_val = parse_status_value(si_copy, "tsid=");
                            free(si_copy);
                        }
                    }
                    if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str_s) > 0) {
                        char *pi_copy = strdup(plpinfo_str_s);
                        if(pi_copy) {
                            long bsid_s = parse_status_value(pi_copy, "bsid=");
                            if (bsid_s != -999) id_val = bsid_s;
                            free(pi_copy);
                        }
                    }
                    if (id_val == -999) id_val = 0;

                    char filename[256];
                    char time_str[20];
                    time_t now = time(NULL);
                    struct tm *t = localtime(&now);
                    strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
                    sprintf(filename, "rf%u-bsid%ld-details-%s.txt", rf_channel, id_val, time_str);
                    
                    FILE *f = fopen(filename, "w");
                    if (f) {
                        for(int i = 0; i < line_count; i++) {
                            fprintf(f, "%s\n", display_lines[i]);
                        }
                        fclose(f);
                        snprintf(message, sizeof(message), "Saved details to %s", filename);
                    } else {
                        snprintf(message, sizeof(message), "Error: Could not open file for writing.");
                    }
                }
                break;
            case 'q':
                delwin(detail_win);
                for(int i = 0; i < line_count; i++) free(display_lines[i]);
                free(streaminfo_copy);
                free(plpinfo_copy);
                return 1; // Quit requested
            case 'd':
            case '\n':
            case '\r':
                delwin(detail_win);
                for(int i = 0; i < line_count; i++) free(display_lines[i]);
                free(streaminfo_copy);
                free(plpinfo_copy);
                nodelay(stdscr, TRUE);
                return 0;
        }
    }
}
