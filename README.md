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
