# HDHomeRun TUI

This repository is a mirror of <https://www.rabbitears.info/kb/hdhomerun_tui>

## Additional Notes

### libhdhomerun Submodule

This repository uses `libhdhomerun` as a git submodule to manage the HDHomeRun library code. To clone this repository along with the submodule, use the following command:

```bash
git clone --recurse-submodules <repository_url>
```

If you have already cloned the repository without the `--recurse-submodules` flag, you can initialize and update the submodule with the following commands:

```bash
git submodule init
git submodule update
```

## Debug Logging

### Verbose Logging

Verbose debug logging has been added to help troubleshoot device connection issues, particularly when connecting via IP address.

#### Enable Verbose Logging

Run the application with the `-v` or `--verbose` flag:

```bash
# Test connecting to a device by IP address with verbose logging
./hdhomerun_tui -d 192.168.1.100 -v

# Test with device ID and verbose logging
./hdhomerun_tui -d 12345678 -v

# Discover all devices with verbose logging
./hdhomerun_tui -v
```

#### Log File Location

When verbose mode is enabled, debug information is written and to:

```plaintext
hdhomerun_tui.log
```

The log file is created in the same directory where you run the application and is appended to if the file already exists.

### What Gets Logged

The verbose logging captures:

1. **Startup Information**
   - TUI version
   - Target device (if specified)

2. **Discovery Process**
   - Discovery initialization
   - Broadcast request status
   - Each discovered device with:
     - Device ID (hex format)
     - IP address
     - Number of tuners
     - Legacy status

3. **Device Filtering** (when using `-d` option)
   - Target device comparison
   - Device ID string comparison (case-insensitive)
   - IP address string comparison (exact match)
   - Match result for each discovered device

4. **Tuner List Building**
   - Each tuner added to the list
   - Total tuner count

5. **Device Connection**
   - When a device connection is created
   - Connection success/failure
   - Device ID and IP used for connection

6. **Channel Operations**
   - Channel map retrieval
   - Channel list population (count of channels found)
   - Channel tuning (left/right arrows, manual entry, seek)
   - Tune command strings and results

7. **Status Retrieval**
   - `get_tuner_status` calls and return codes
   - `get_tuner_vstatus` calls and return codes
   - `get_tuner_streaminfo` calls and return codes
   - Channel, lock status, and bitrate information

8. **User Actions**
   - Key presses (with character display)
   - VLC stream start/stop
   - Device list refresh
   - Save operations initiation

9. **Exit**
   - Application shutdown timestamp

### Direct IP Connection Fallback

When broadcast discovery fails (e.g., across Layer 3 boundaries), the app now attempts to connect directly to the specified IP address and probe for tuners.

**What it does**:

- After broadcast discovery completes with 0 devices
- If a target device was specified (IP or device ID)
- Creates a direct connection using `hdhomerun_device_create_from_str()`
- Queries the device for its ID and IP
- Probes each tuner (0-15) to determine actual tuner count
- Adds all discovered tuners to the list

### Log Examples

#### Successful Local Discovery

```plaintext
[2025-12-10 18:05:31] Starting discovery for specific device: 192.168.1.200
[2025-12-10 18:05:31] Broadcasting discovery request...
[2025-12-10 18:05:31] Discovery broadcast completed
[2025-12-10 18:05:31] Found device: ID=12345678, IP=192.168.1.200, Tuners=4, Legacy=no
[2025-12-10 18:05:31] Device MATCHES target!
[2025-12-10 18:05:31] Added tuner 0: 12345678-0 (192.168.1.200)
```

#### Remote Connection with Direct Fallback

```plaintext
[2025-12-10 18:08:12] Starting discovery for specific device: 192.168.1.200
[2025-12-10 18:08:12] Broadcasting discovery request...
[2025-12-10 18:08:14] Discovery broadcast completed
[2025-12-10 18:08:14] Iterating through discovered devices...
[2025-12-10 18:08:14] Discovery complete. Total tuners found: 0
[2025-12-10 18:08:14] No devices found via broadcast discovery. Attempting direct connection to: 192.168.1.200
[2025-12-10 18:08:14] Direct device connection created successfully
[2025-12-10 18:08:14] Device responded with ID: 12345678
[2025-12-10 18:08:14] Device IP address: 192.168.1.200
[2025-12-10 18:08:14] Tuner 0 responded successfully
[2025-12-10 18:08:14] Tuner 1 responded successfully
[2025-12-10 18:08:14] Tuner 2 responded successfully
[2025-12-10 18:08:14] Tuner 3 responded successfully
[2025-12-10 18:08:14] Tuner 4 query failed, stopping probe
[2025-12-10 18:08:14] Direct connection determined 4 tuners
[2025-12-10 18:08:14] Added tuner 0: 12345678-0 (192.168.1.200) via direct connection
```

#### Main Loop Operations

```plaintext
[2025-12-10 18:10:15] main_loop: Setting tuner to index 0
[2025-12-10 18:10:15] main_loop: set_tuner returned 1
[2025-12-10 18:10:15] populate_channel_list: Getting channel map
[2025-12-10 18:10:15] populate_channel_list: get_tuner_channelmap returned 1
[2025-12-10 18:10:15] populate_channel_list: Channel map: us-bcast 2-69
[2025-12-10 18:10:15] populate_channel_list: Populated 68 channels
[2025-12-10 18:10:15] draw_status_pane: Getting tuner status for 12345678-0
[2025-12-10 18:10:15] draw_status_pane: get_tuner_status returned 1
[2025-12-10 18:10:15] draw_status_pane: Channel=auto:33, Lock=atsc3:33:0+16, bps=18234567, pps=12345
```

#### Channel Tuning

```plaintext
[2025-12-10 18:11:20] main_loop: Key pressed: 67 (char: C)
[2025-12-10 18:11:25] Manual tune: channel_str=520000000, full_tune_str=auto:520000000
[2025-12-10 18:11:25] Manual tune: set_tuner_channel returned 1
```

### Log Analysis

Check the log for:

- **Device Discovery**: Did the device get discovered?

  ```plaintext
  Found device: ID=12345678, IP=192.168.1.100, Tuners=2, Legacy=no
  ```

- **Filtering Match**: Did the IP match correctly?

  ```plaintext
  Checking if device matches target '192.168.1.100': DeviceID=12345678, IP=192.168.1.100
  Device MATCHES target!
  ```

- **Connection Creation**: Was the connection created successfully?

  ```plaintext
  Creating device connection: ID=12345678, IP=192.168.1.100
  Device connection created successfully
  ```

If the IP doesn't match, you'll see:

```plaintext
Device does NOT match target (ID comparison: false, IP comparison: false)
```

This indicates the IP string comparison is failing, which could be due to:

- Incorrect IP format
- Whitespace differences
- IP address mismatch

### Troubleshooting with Logs

If functionality isn't working on remote connections, check:

1. **Connection Success**: Does the device respond after direct connection?
2. **API Return Codes**: Are `get_tuner_*` functions returning > 0?
3. **Channel Map**: Is the channel map retrieved successfully?
4. **Tuner Status**: Is status information being retrieved?
5. **Command Results**: Are `set_tuner_*` commands returning success (1)?

Common issues to diagnose:

- Return code of 0 or negative = API call failed
- Empty channel map = channel map not configured or not accessible
- Status returns but shows "none" for lock = tuning failed
- Commands return 0 = device not responding to commands

### Tips

- The log file appends each run, so you can see multiple sessions
- Timestamps are included for each log entry
- Use `tail -f hdhomerun_tui.log` to watch logs in real-time
- Clear the log file with `rm hdhomerun_tui.log` if it gets too large

## Mirror of Original Page

The HDHomeRun Terminal User Interface is software written with the goal of providing an updated interface for Mac and Linux platforms that supports the latest ATSC 3.0 features the way the Windows HDHomeRun Config GUI does, and also include some additional functions that may be useful.

To download the latest version of the HDHomeRun TUI, see here: [https://www.rabbitears.info/dl/hdhomerun_tui/](https://www.rabbitears.info/dl/hdhomerun_tui/)

The HDHomeRun TUI is being written using Google Gemini and Anthropic Claude. It has been written for the Linux command line, but can be compiled on the Mac with the proper prerequisites and should be possible to get working on Windows. Many thanks go to to drmpeg; his l1detail parsing code has been adapted for use in the HDHomeRun TUI. It, like the TUI, is licensed in GPLv3.

[https://github.com/drmpeg/dtv-utils/tree/master](https://github.com/drmpeg/dtv-utils/tree/master)

## How to Use

Upon opening, the pane at the left shows detected HDHomeRun tuners, which can be accessed using the **Up** and **Down** arrows. To refresh the list, press **R**.

To tune a specific channel, you can either press **C** or directly enter its number and press **Enter**, or you can use the **Left** or **Right** arrows. To seek to the next channel with a detected signal, use the **-** or **+** keys. To change the tuner's channel map, press **M**.

If you have VLC installed and are running the TUI inside a GUI command line, you can use the **V** key to select a program to stream to view in VLC.

### ATSC 1.0 Features

If you are tuned to an ATSC 1.0 signal, you can use the **S** key to save a 30-second transport stream capture. Alternatively, you can use the **A** key to save a 30-second transport stream capture, but it will reset until it gets 30 seconds without any signal errors. To abort an on-going save, press the **Backspace** key.

### ATSC 3.0 Features

If you are tuned to an ATSC 3.0 signal, you can use the **D** key to view detailed PLP information and SNR requirements. If you have the Dev upgrade to your HDHomeRun 4K tuner, it will also show the L1 Basic and L1 Detail information. From this screen, you can press **S** to save a text copy of the information.

If you are tuned to an ATSC 3.0 signal, you can use the **P** key to select specific PLPs to tune, as the HDHomeRun 4K tunes only the first PLP by default. Separate multiple PLPs with a comma, or leave blank to attempt to tune all PLPs.

If you are tuned to an ATSC 3.0 signal, you can use the **S** key to save a 30-second debug capture. Alternatively, you can use the **A** key to save a 30-second debug capture, but it will reset until it gets 30 seconds without any detected signal errors. If you have the Dev upgrade to your HDHomeRun 4K tuner, you can use the **X** key to save a 30-second ALP-PCAP file, or **Z** to save a 30-second ALP-PCAP file, but it will reset up to 5 times until it gets 30 seconds without any detected signal errors. For any of these options, it will also save a text file under the same name with the PLP and/or L1 information noted above. To abort an on-going save, press the **Backspace** key.

## Additional Information

Pressing **H** will show a help screen with this information, while pressing **Q** will quit the program.

Please reach out with any questions or comments by [e-mail](https://www.rabbitears.info/static.php?name=contact), on [this HDHomeRun Forum thread](https://forum.silicondust.com/forum/viewtopic.php?t=79229), or on the [RabbitEars Discord](https://discord.gg/tnamT4eccd). Enjoy this tool!
