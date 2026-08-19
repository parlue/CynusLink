# CynusLink

**Bluetooth LE gateway for using the Manya Cynus chess robot with
ChessLink-compatible software.**

CynusLink uses an **ESP32-S3** as a wireless protocol bridge between the
Manya Cynus and software that supports the Millennium ChessLink
protocol.

## Idea

The Manya Cynus uses its own Bluetooth LE protocol.

CynusLink connects to the Cynus via BLE and translates board states and
move commands between the Cynus and ChessLink protocols.

To the chess application, the ESP32-S3 behaves like a
ChessLink-compatible device.

## Options

## Display Options

CynusLink uses the Cynus display to provide simple status information without affecting the normal game logic.

### Connection Status

| Display | Meaning |
| --- | --- |
| `BT Scan` | Cynus connected, waiting for ChessLink |
| `Connect` | ChessLink connected |
| `play` | CynusLink is ready to play |

After ChessLink connects, `ready` is held until Cynus confirms external-engine readiness. Only then does the display change to `play`.

### Playing as Black (Actual not available)

CynusLink supports starting a game with the human player on the **black** side.

Set up the complete initial position with the board rotated by 180 degrees and scan the board. CynusLink recognizes the flipped starting position and sends:

``` text
set flip board on
```

In this mode CynusLink knows that the software plays White. The gateway waits for the first move from the ChessLink software and does not wait for a human move first.

The sequence is:

1. The flipped initial position is detected.
2. Cynus receives `set flip board on`.
3. CynusLink waits until Cynus reports `get move`.
4. The initial board position is reported to the ChessLink software once.
5. If the software is configured to play White, it can immediately send the first move.
6. Cynus executes the software move.
7. After the robot position is confirmed, the game continues with the human player on Black.

For the normal initial position CynusLink sends:

``` text
set flip board off
```

and waits for the human player to make the first White move.

The flip command is set whenever a valid initial position is recognized, including after boot and when a new game is started.

### Initial Position Errors

During startup CynusLink compares the scanned board with the expected initial position. If the position is not correct, the Cynus display shows up to **two differences at the same time**.

Examples:

| Display | Meaning |
| --- | --- |
| `+E4` | A piece is present on E4 where the initial position expects an empty square, or the wrong piece is on E4 |
| `-F8` | The expected piece on F8 is missing |
| `+E4/-F8` | Two position differences are currently detected |

The symbols mean:

- `+` = there is an unexpected or wrong piece on this square
- `-` = the expected piece is missing from this square

Only two differences fit on the seven-character Cynus display. Correct the displayed squares and scan again. If more differences remain, the next scan shows the next current error pattern.

Whenever a new startup error pattern is detected, CynusLink also asks the robot to play its error sound:

``` text
play audio error
```

The same unchanged error pattern is not sounded repeatedly.

### Engine Moves

Moves received from the ChessLink application are shown on the Cynus display while the robot executes them.

| Display | Meaning |
| --- | --- |
| `E2-E4` | Normal move |
| `G1-F3` | Normal move |
| `0-0` | Kingside castling |
| `0-0-0` | Queenside castling |
| `Chg Q` | Promote pawn to Queen |
| `Chg R` | Promote pawn to Rook |
| `Chg B` | Promote pawn to Bishop |
| `Chg N` | Promote pawn to Knight |

After the robot has completed the move, the display returns to `play`.

### Sound Options

Sound can be configured directly from the chessboard.

Set up the normal starting position, move only the **black King**, and press the Cynus **Scan** button.

| Black King | Function | Display |
| --- | --- | --- |
| `e5` | Sound OFF (`sound 0`) | `snd off` |
| `e6` | Sound ON (`sound 70`) | `snd on` |

After two seconds, the display automatically returns to `play`.

These special configuration positions are handled internally by CynusLink and are **not sent to the connected ChessLink software**.

## Installation

No development environment is required.

### Web Installer

1.  Connect the ESP32-S3 to your computer via USB.
2.  Open the **CynusLink Web Installer** in Google Chrome or Microsoft
    Edge.
3.  Click **Connect** and select the ESP32-S3.
4.  Confirm the installation.
5.  Wait until flashing is complete.
6.  Disconnect and reconnect USB power.

**[Install CynusLink on ESP32-S3](https://parlue.github.io/CynusLink/)**

> The web installer requires a browser with Web Serial support, such as
> Google Chrome or Microsoft Edge.

The precompiled firmware is built for an **ESP32-S3 DevKitC-1 compatible
board**.

## Architecture

``` text
Chess Software
     |
     | ChessLink BLE
     |
  ESP32-S3
  CynusLink
     |
     | Cynus BLE
     |
 Manya Cynus
```

The ESP32-S3 operates simultaneously as:

-   **BLE Peripheral** for the ChessLink connection
-   **BLE Central** for the Cynus connection
-   **Protocol gateway** between both devices

## Features

-   Wireless BLE-to-BLE gateway
-   ChessLink-compatible interface
-   Reads the physical Cynus board position
-   Transfers physical moves to the chess software
-   Sends engine moves to the Cynus robot
-   Supports normal and 180-degree-flipped initial positions
-   Supports playing as White or Black
-   Shows startup position differences on the Cynus display
-   New games can be started by setting up the initial position and
    using the Cynus scanner
-   Standalone operation with only USB power
-   Screwless 3D-printable ESP32-S3 enclosure

## Hardware

-   Manya Cynus chess robot
-   ESP32-S3 DevKitC-1 compatible board
-   USB-C power supply
-   Optional 3D-printed enclosure

## Repository

``` text
CynusLink/
|
+-- README.md
|
+-- src/
|   +-- main.cpp
|
+-- stl/
|   +-- 3D print files
|
+-- docs/
    +-- CynusLink Web Installer
    +-- precompiled firmware files
```

### `src`

Contains the ESP32-S3 source code.

### `Firmware`

Contains ready-to-flash firmware builds.

### `stl`

Contains the STL files for the screwless 3D-printed ESP32-S3 enclosure.

### `docs`

Contains the browser-based ESP32-S3 Web Installer used by GitHub Pages.

## Building from Source

The firmware can also be built with PlatformIO.

Target configuration:

``` text
Board:     ESP32-S3 DevKitC-1
Framework: Arduino
BLE:       NimBLE-Arduino
```

## Status

**Work in progress.**

Communication between the Cynus, ESP32-S3 and ChessLink-compatible
software is working (Tested: Bearchess, Picochess and King Element via Diabillo Interface). Physical moves can be transferred to the chess
software and engine moves can be executed by the Cynus.

Further testing and protocol compatibility improvements are ongoing.

## Trademark, Copyright and Protocol Notice

CynusLink is an independent, unofficial interoperability project and is not
affiliated with, endorsed by, or sponsored by MILLENNIUM 2000 GmbH.

MILLENNIUM, ChessLink and related product names, trademarks, documentation
and protocol specifications remain the property of their respective rights
holders.

This project does not claim ownership of the ChessLink protocol.
ChessLink compatibility is implemented solely for the purpose of
interoperability between independently developed hardware and compatible
chess software.

No original MILLENNIUM firmware, software, documentation or other
copyrighted material is distributed with this project.

Please respect the copyrights, trademarks and other intellectual property
rights of MILLENNIUM 2000 GmbH and all other respective rights holders.


Use at your own risk.
