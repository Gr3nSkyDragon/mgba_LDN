# Important!!!

### Note from GSD:

This fork and its primary unique feature(s) were coded using LLM generated material. Consequently, this fork is not associated with nor endorsed by mGBA or its primary contributors. It's provided as-is and is designed to be used in a Windows environment when using a computer and Android when using a mobile device. This was designed for trading between Gen 3 GBA Pokemon games and the Switch ports. 

### Note FOR GSD:

[Useful link](https://github.com/Zapeth/citra/wiki/AES-Keys)

# How to run

### Prerequisites

You will need your Switch prod.keys. You **must** have your Switch prod.keys for this fork to work. Everything else can be done with retail hardware, including trading with a Switch 2.

You'll also need a way to broadcast Wi-Fi, be that a USB Wi-Fi adapter or devboard like the ESP32. If you're getting an ESP32, I recommend an ESP32-S3, as that is also compatible with [Pokemon Automation](https://pokemonautomation.github.io/index.html)

If you're using a USB Wi-Fi adapter, you'll need to set up ldnd.exe from [unlimitedcoder2](https://github.com/unlimitedcoder2/ldnrs/releases), in a version that speaks ldnd protocol 7 (older ldnd builds are not supported anymore). You'll need to follow the steps in that repository to set up your USB Wi-Fi adapter (you will need a compatible USB Wi-Fi adapter). I've been using a cheap/generic AC1300 adapter in my testing. This is a Windows-only program. If you have experience with Linux, you can probably convert it to be Linux-compatible fairly easily. ldnd loads your prod.keys itself (its `--keys` option), so mGBA doesn't need to know where they are anymore, either.

If you're using an ESP32 for GBA games, you'll need to install the GB-Link Switch LND firmware either [manually](https://github.com/GB-Link/GB-Link-Switch-LDN) or via [the GB-Link Switch LDN webpage](https://switch.gblink.io/?from=gblink-launcher). You'll also need to install your prod.keys on the ESP32. The webpage is a little more convenient to use so I'd recommend trying that first.

If you're using an ESP32 for GB games, you'll need to install the Azahar UDS firmware. It's easier to use the firmware flasher, but if you want to install it manually, you can use the [ESP-IDF v5.2.8](https://dl.espressif.com/dl/esp-idf/) installer or use a web-based tool like [esptool](https://espressif.github.io/esptool-js/) to flash the firmware. Flash at address 0x0000. For the aes_keys.txt feature, you can pass the keys in the firmware flasher while setting up your ESP32 or use "uds-esp32-probe COMX --store-key <file>" where X is your COM port, using the uds-esp32-probe script in the mGBA_LDN repository. You can also pass the keys from mGBA using Tools > Settings > BIOS > 3DS UDS key file and then running a Gameboy title.

### ESP32 Screen

We've added screen support! Similar to [pokeldn](https://github.com/Decryptu/pokeldn), we now have an animated screen that plays during communication. Not as fancy, but it's there. It *should* be compatible with most ESP32s, but I've only personally confirmed with the ESP32-S3, and only the SSD1309. In my testing, all three of the aforementioned screens have been interchangeable, but if you encounter any problems, please let me know. 

If you have an SSD1306, SSD1315, or SSD1309 screen, you can connect them to the ESP32 according to this table:

| Board | Screen SDA | Screen SCL | VCC | GND |
|---|---|---|---|---|
| ESP32-S3 | GPIO8 | GPIO9 | 3V3 | GND |
| XIAO ESP32-S3 | D9 (GPIO8) | D10 (GPIO9) | 3V3 | GND |
| XIAO ESP32-C6 | D4 (GPIO22) | D5 (GPIO23) | 3V3 | GND |
| XIAO ESP32-C3 | D4 (GPIO6) | D5 (GPIO7) | 3V3 | GND |
| Other C3 boards (e.g. SuperMini) | GPIO6 | GPIO7 | 3V3 | GND |

### Trading

**DO NOT USE SPEED-UP** under any circumstances. The trade setup or actual trade will likely break down. You probably won't mess up your save file, as the game should just throw a communication error and revert to the last save, but I didn't test this to verify.

Hosting now works for all native Wireless Adapter games (FireRed, LeafGreen, and Emerald). Ruby and Sapphire and the GB titles are join-only at this time. 

For the GBA games, do not use the Wireless Union Room (the left window lady on the upper floor of the Pokemon Center). You can go exploring there if you want, but actual trading is the right window lady.

### Desktop

For trading between instances of mGBA, you can choose to enable the Wireless Adapter (Emulation > Wireless Adapter > Local) for up to two instances. This is more of a novelty thing, as the link cable mode works for up to four players, but I used this for developing the other modes and therefore included it. If you want to familiarize yourself with the Ruby/Sapphire to FRLG process, you can enable the local Cable Wrapper (Emulation > Wireless Adapter > Local, then tick Cable wrapper (Ruby/Sapphire)) in the Ruby/Sapphire instance and the Wireless Adapter (Emulation > Wireless Adapter > Local) for the FRLG instance. **FRLG must host the trade**. Let FRLG host before talking to the Link Cable Trade lady at the middle window in Ruby/Sapphire.

For trading between a computer (FRLG/Emerald) and a Switch, you can choose ldnd (Emulation > Wireless Adapter > ldnd) or ESP32 (Emulation > Wireless Adapter > ESP32). ldnd is designed for a USB Wi-Fi adapter and requires ldnd.exe to be set up correctly and running, and ESP32 is designed for the GB-Link Switch LDN firmware configuration. The ESP32 firmware may have other functionality like battling, berry blending, etc implemented, but I only tested trading. 

For trading between a computer (Ruby/Sapphire) and a Switch, you **must** choose ESP32 and tick Cable wrapper (Emulation > Wireless Adapter > ESP32, then Cable wrapper (Ruby/Sapphire)) for Ruby/Sapphire. ldnd is currently stubbed and does not work. Let FRLG host the trade before interacting with the Link Trade Cable lady at the middle window in Ruby/Sapphire. 

For trading between a computer (Red, Blue, Yellow, Gold Silver, Crystal) and Azahar, run a single instance of mGBA and Azahar. In Azahar, go to Multiplayer and tick the box for "Local mGBA Virtual Console." **Before** starting mGBA, launch the Azahar Virtual Console title and host a trade session. **DO NOT** start the mGBA Gameboy title until Azahar is hosting a trade, and **DO NOT** accept the mGBA trainer until mGBA is ready to enter the trade room. In mGBA, go to Emulation > Wireless Adapter and check both "Local" and "Virtual Console (local only)". Once Azahar is hosting the trade, launch the mGBA Gameboy title and talk to the trade window lady. Once you reach the final "Please wait..." you can accept the mGBA trainer in Azahar. When choosing the Trade Center option, only one game needs to select it. It's recommended that you not mash through this option, as these games are fragile and prone to desync. 

For trading between a computer (Red, Blue, Yellow, Gold Silver, Crystal) and a 3DS, launch the 3DS Virtual Console title and host a trade before opening mGBA. Once the 3DS is ready, launch mGBA and go to Emulation > Wireless Adapter > ESP32. The process is then similar to the computer to Azahar process. Navigate the mGBA game through the trade window setup until you reach the final "Please wait..." before accepting the mGBA trainer in the 3DS. When selecting the Trade Colosseum, only one console needs to choose the option, and please do not mash through the sequence. These games are old and fragile, and prone to desync. 

**DO NOT** select "Virtual Console (local only)" as this is an mGBA to Azahar only feature. Unfortunately, with the latest ldnd updates, ldnd mode is not currently supported at this time, and only ESP32 mode works for the original GB titles. 

### Android

For trading between a smartphone (FRLG/Emerald) and a Switch, you need to install the APK, then click the three bars (☰) menu in the top right, select Wireless Adapter, choose ESP32 (currently supports an ESP32 running the GB-Link Switch LDN firmware; I'm using an ESP32-S3, other models may be added later), and plug the adapter into your smartphone via its USB-C port. You will need a USB-C-to-USB-C cable for this. 

For GB Gen 1 and 2, the steps are the same, but you need to use an ESP32 running the Azahar UDS firmware instead and make sure to have Azahar or the 3DS hosting the trade before starting the game in mGBA.

For trading between a smartphone (Ruby/Sapphire) and a Switch, click the three bars (☰) menu in the top right, select Wireless Adapter, choose Cable Adapter (currently supports an ESP32 running the GB-Link Switch LDN firmware; I'm using an ESP32-S3, other models may be added later), and plug the adapter into your smartphone via its USB-C port. Let FRLG host the trade before interacting with the Link Cable Trade lady at the middle window in Ruby/Sapphire.

### Other Android mGBA Features

Open ROM copies your ROM into the emulator's ROM folder. You can also select your save at the same time to load the save into the game and copy it into the emulator's save folder. If you select multiple ROMs and saves, all will be copied into the correct folders, but only one will be launched.

Import Save lets you use other saves with your ROM. This will update your default save for that ROM until you import another save into the ROM.

Display Settings lets you enable or disable the FPS counter, Frame counter, ESP32 status message, on-screen controls, and pixelation ("scanlines"). Color mode lets you choose between different color modes and filters. You can also change the button colors, either with presets or hexadecimal values for individual buttons. Background Photo lets you set an image as your "shell" image when in vertical mode. Horizontal panels lets you set solid colors, mirrored images, or two distinct images when in horizontal mode. 

# Original ReadMe

mGBA
====

mGBA is an emulator for running Game Boy Advance games. It aims to be faster and more accurate than many existing Game Boy Advance emulators, as well as adding features that other emulators lack. It also supports Game Boy and Game Boy Color games.

Up-to-date news and downloads can be found at [mgba.io](https://mgba.io/).

[![Build status](https://buildbot.mgba.io/badges/build-win32.svg)](https://buildbot.mgba.io)
[![Translation status](https://hosted.weblate.org/widgets/mgba/-/svg-badge.svg)](https://hosted.weblate.org/engage/mgba)

Features
--------

- Highly accurate Game Boy Advance hardware support[<sup>[1]</sup>](#missing).
- Game Boy/Game Boy Color hardware support.
- Fast emulation. Known to run at full speed even on low end hardware, such as netbooks.
- Qt and SDL ports for a heavy-weight and a light-weight frontend.
- Local (same computer) link cable support.
- Save type detection, even for flash memory size[<sup>[2]</sup>](#flashdetect).
- Support for cartridges with motion sensors and rumble (only usable with game controllers).
- Real-time clock support, even without configuration.
- Solar sensor support for Boktai games.
- Game Boy Camera and Game Boy Printer support.
- A built-in BIOS implementation, and ability to load external BIOS files.
- Scripting support using Lua.
- Turbo/fast-forward support by holding Tab.
- Rewind by holding Backquote.
- Frameskip, configurable up to 10.
- Screenshot support.
- Cheat code support.
- 9 savestate slots. Savestates are also viewable as screenshots.
- Video, GIF, WebP, and APNG recording.
- e-Reader support.
- Remappable controls for both keyboards and gamepads.
- Loading from ZIP and 7z files.
- IPS, UPS and BPS patch support.
- Game debugging via a command-line interface and GDB remote support, compatible with Ghidra and IDA Pro.
- Configurable emulation rewinding.
- Support for loading and exporting GameShark and Action Replay snapshots.
- Cores available for RetroArch/Libretro and OpenEmu.
- Community-provided translations for several languages via [Weblate](https://hosted.weblate.org/engage/mgba).
- Many, many smaller things.

#### Game Boy mappers

The following mappers are fully supported:

- MBC1
- MBC1M
- MBC2
- MBC3
- MBC3+RTC
- MBC30
- MBC5
- MBC5+Rumble
- MBC7
- M161
- Wisdom Tree (unlicensed)
- NT "old type" 1 and 2 (unlicensed multicart)
- NT "new type" (unlicensed MBC5-like)
- Pokémon Jade/Diamond (unlicensed)
- Sachen MMC1 (unlicensed)

The following mappers are partially supported:

- MBC6 (missing flash memory write support)
- MMM01
- Pocket Cam
- TAMA5 (incomplete RTC support)
- HuC-1 (missing IR support)
- HuC-3 (missing IR support)
- Sachen MMC2 (missing alternate wiring support)
- BBD (missing logo switching)
- Hitek (missing logo switching)
- GGB-81 (missing logo switching)
- Li Cheng (missing logo switching)
- Sintax (missing logo switching)

### Planned features

- Networked multiplayer link cable support.
- Dolphin/JOY bus link cable support.
- MP2k audio mixing, for higher quality sound than hardware.
- Re-recording support for tool-assist runs.
- A comprehensive debug suite.
- Wireless adapter support.

Supported Platforms
-------------------

- Windows 7 or newer
- OS X 10.9 (Mavericks)[<sup>[3]</sup>](#osxver) or newer
- Linux
- FreeBSD
- Nintendo 3DS
- Nintendo Switch
- Wii
- PlayStation Vita

Other Unix-like platforms, such as OpenBSD, are known to work as well, but are untested and not fully supported.

### System requirements

Requirements are minimal. Any computer that can run Windows Vista or newer should be able to handle emulation. Support for OpenGL 1.1 or newer is also required, with OpenGL 3.2 or newer for shaders and advanced features.

Downloads
---------

Downloads can be found on the official website, in the [Downloads][downloads] section. The source code can be found on [GitHub][source].

Controls
--------

Controls are configurable in the settings menu. Many game controllers should be automatically mapped by default. The default keyboard controls are as follows:

- **A**: X
- **B**: Z
- **L**: A
- **R**: S
- **Start**: Enter
- **Select**: Backspace

Compiling
---------

Compiling requires using CMake 3.1 or newer. GCC, Clang, and Visual Studio 2019 are known to work for compiling mGBA.

#### Docker building

The recommended way to build for most platforms is to use Docker. Several Docker images are provided that contain the requisite toolchain and dependencies for building mGBA across several platforms.

Note: If you are on an older Windows system before Windows 10, you may need to configure your Docker to use VirtualBox shared folders to correctly map your current `mgba` checkout directory to the Docker image's working directory. (See issue [#1985](https://mgba.io/i/1985) for details.)

To use a Docker image to build mGBA, simply run the following command while in the root of an mGBA checkout:

	docker run --rm -it -v ${PWD}:/home/mgba/src mgba/windows:w32

After starting the Docker container, it will produce a `build-win32` directory with the build products. Replace `mgba/windows:w32` with another Docker image for other platforms, which will produce a corresponding other directory. The following Docker images available on Docker Hub:

- mgba/3ds
- mgba/switch
- mgba/ubuntu:xenial
- mgba/ubuntu:bionic
- mgba/ubuntu:focal
- mgba/ubuntu:groovy
- mgba/vita
- mgba/wii
- mgba/windows:w32
- mgba/windows:w64

If you want to speed up the build process, consider adding the flag `-e MAKEFLAGS=-jN` to do a parallel build for mGBA with `N` number of CPU cores.

#### *nix building

To use CMake to build on a Unix-based system, the recommended commands are as follows:

	mkdir build
	cd build
	cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
	make
	sudo make install

This will build and install mGBA into `/usr/bin` and `/usr/lib`. Dependencies that are installed will be automatically detected, and features that are disabled if the dependencies are not found will be shown after running the `cmake` command after warnings about being unable to find them.

If you are on macOS, the steps are a little different. Assuming you are using the homebrew package manager, the recommended commands to obtain the dependencies and build are:

	brew install cmake ffmpeg libzip qt5 sdl2 libedit lua pkg-config
	mkdir build
	cd build
	cmake -DCMAKE_PREFIX_PATH=`brew --prefix qt5` ..
	make

Note that you should not do a `make install` on macOS, as it will not work properly.

#### Windows developer building

##### MSYS2

To build on Windows for development, using MSYS2 is recommended. Follow the installation steps found on their [website](https://msys2.github.io). Make sure you're running the 32-bit version ("MSYS2 MinGW 32-bit") (or the 64-bit version "MSYS2 MinGW 64-bit" if you want to build for x86_64) and run this additional command (including the braces) to install the needed dependencies (please note that this involves downloading over 1100MiB of packages, so it will take a long time):

	pacman -Sy --needed base-devel git ${MINGW_PACKAGE_PREFIX}-{cmake,ffmpeg,gcc,gdb,libelf,libepoxy,libzip,lua,pkgconf,qt5,SDL2,ntldd-git}

Check out the source code by running this command:

	git clone https://github.com/mgba-emu/mgba.git

Then finally build it by running these commands:

	mkdir -p mgba/build
	cd mgba/build
	cmake .. -G "MSYS Makefiles"
	make -j$(nproc --ignore=1)

Please note that this build of mGBA for Windows is not suitable for distribution, due to the scattering of DLLs it needs to run, but is perfect for development. However, if distributing such a build is desired (e.g. for testing on machines that don't have the MSYS2 environment installed), running `cpack -G ZIP` will prepare a zip file with all of the necessary DLLs.

##### Visual Studio

To build using Visual Studio is a similarly complicated setup. To begin you will need to install [vcpkg](https://github.com/Microsoft/vcpkg). After installing vcpkg you will need to install several additional packages:

    vcpkg install ffmpeg[vpx,x264] libepoxy libpng libzip lua sdl2 sqlite3

Note that this installation won't support hardware accelerated video encoding on Nvidia hardware. If you care about this, you'll need to install CUDA beforehand, and then substitute `ffmpeg[vpx,x264,nvcodec]` into the previous command.

You will also need to install Qt. Unfortunately due to Qt being owned and run by an ailing company as opposed to a reasonable organization there is no longer an offline open source edition installer for the latest version, so you'll need to either fall back to an [old version installer](https://download.qt.io/archive/qt/5.12/5.12.9/qt-opensource-windows-x86-5.12.9.exe) (which wants you to create an otherwise-useless account, but you can bypass temporarily setting an invalid proxy or otherwise disabling networking), use the online installer (which requires an account regardless), or use vcpkg to build it (slowly). None of these are great options. For the installer you'll want to install the applicable MSVC versions. Note that the offline installers do not support MSVC 2019. For vcpkg you'll want to install it as such, which will take quite a while, especially on quad core or less computers:

    vcpkg install qt5-base qt5-multimedia

Next, open Visual Studio, select Clone Repository, and enter `https://github.com/mgba-emu/mgba.git`. When Visual Studio is done cloning, go to File > CMake and open the CMakeLists.txt file at the root of the checked out repository. From there, mGBA can be developed in Visual Studio similarly to other Visual Studio CMake projects.

#### Toolchain building

If you have devkitARM (for 3DS), devkitPPC (for Wii), devkitA64 (for Switch), or vitasdk (for PS Vita), you can use the following commands for building:

	mkdir build
	cd build
	cmake -DCMAKE_TOOLCHAIN_FILE=../src/platform/3ds/CMakeToolchain.txt ..
	make

Replace the `-DCMAKE_TOOLCHAIN_FILE` parameter for the following platforms:

- 3DS: `../src/platform/3ds/CMakeToolchain.txt`
- Switch: `../src/platform/switch/CMakeToolchain.txt`
- Vita: `../src/platform/psp2/CMakeToolchain.vitasdk`
- Wii: `../src/platform/wii/CMakeToolchain.txt`

### Dependencies

mGBA has no hard dependencies, however, the following optional dependencies are required for specific features. The features will be disabled if the dependencies can't be found.

- Qt 5: for the GUI frontend. Qt Multimedia or SDL are required for audio.
- SDL: for a more basic frontend and gamepad support in the Qt frontend. SDL 2 is recommended, but 1.2 is supported.
- zlib and libpng: for screenshot support and savestate-in-PNG support.
- libedit: for command-line debugger support.
- ffmpeg or libav: for video, GIF, WebP, and APNG recording.
- libzip or zlib: for loading ROMs stored in zip files.
- SQLite3: for game databases.
- libelf: for ELF loading.
- Lua: for scripting.
- json-c: for the scripting `storage` API.

SQLite3, libpng, and zlib are included with the emulator, so they do not need to be externally compiled first.

Footnotes
---------

<a name="missing">[1]</a> Currently missing features are

- OBJ window for modes 3, 4 and 5 ([Bug #5](http://mgba.io/b/5))

<a name="flashdetect">[2]</a> Flash memory size detection does not work in some cases. These can be configured at runtime, but filing a bug is recommended if such a case is encountered.

<a name="osxver">[3]</a> 10.9 is only needed for the Qt port. It may be possible to build or run the Qt port on 10.7 or older, but this is not officially supported. The SDL port is known to work on 10.5, and may work on older.

[downloads]: http://mgba.io/downloads.html
[source]: https://github.com/mgba-emu/mgba/

Copyright
---------

mGBA is Copyright © 2013 – 2026 Jeffrey Pfau. It is distributed under the [Mozilla Public License version 2.0](https://www.mozilla.org/MPL/2.0/). A copy of the license is available in the distributed LICENSE file.

mGBA contains the following third-party libraries:

- [inih](https://github.com/benhoyt/inih), which is copyright © 2009 – 2020 Ben Hoyt and used under a BSD 3-clause license.
- [LZMA SDK](http://www.7-zip.org/sdk.html), which is public domain.
- [MurmurHash3](https://github.com/aappleby/smhasher) implementation by Austin Appleby, which is public domain.
- [getopt for MSVC](https://github.com/skandhurkat/Getopt-for-Visual-Studio/), which is public domain.
- [SQLite3](https://www.sqlite.org), which is public domain.

If you are a game publisher and wish to license mGBA for commercial usage, please email [licensing@mgba.io](mailto:licensing@mgba.io) for more information.
