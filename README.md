# CynusLink

**Bluetooth LE gateway for using the Manya Cynus chess robot with
ChessLink-compatible software.**

CynusLink uses an **ESP32-S3** or **ESP32-C3 SuperMini** as a wireless protocol bridge between the
Manya Cynus and software that supports the Millennium ChessLink
protocol.

## Idea

The Manya Cynus uses its own Bluetooth LE protocol.

CynusLink connects to the Cynus via BLE and translates board states and
move commands between the Cynus and ChessLink protocols.

To the chess application, the ESP32 behaves like a
ChessLink-compatible device.

## How to use

It is important to follow the startup sequence in the correct order.

1. Start the Manya Cynus. Make sure all pieces are in the normal starting position and that the board position is detected correctly by the camera.
2. Power on the CynusLink gateway and wait for it to connect to the Cynus and scan the board. The clock-button indication changes during the scan. If the starting position is not correct, the display shows the detected position errors. Correct the indicated squares. CynusLink scans the board again automatically every five seconds until the starting position is correct. The Cynus Scan/clock button can also be used to scan immediately. When the starting position is correct, the display shows `POS OK` and then `BT Scan` while CynusLink waits for a ChessLink connection.
3. Start or connect the ChessLink-compatible chess computer/software. The Cynus display proceeds from `BT Scan` to `Connect` and finally to `play` when the connection and external-engine handshake are ready.
4. When the display shows `play`, CynusLink is ready to use.

## Options

## Display Options

CynusLink uses the Cynus display to provide simple status information without affecting the normal game logic.

### Connection Status

| Display | Meaning |
| --- | --- |
| `POS OK` | Initial board position is correct |
| `BT Scan` | Cynus connected, waiting for ChessLink |
| `Connect` | ChessLink connected |
| `play` | CynusLink is ready to play |

After ChessLink connects, CynusLink waits until Cynus confirms external-engine readiness. Only then does the display change to `play`.

### Playing as Black (in beta)

CynusLink determines the playing orientation from who makes the first move. If the ChessLink computer makes the first move, CynusLink sets the Cynus board flip before forwarding that move. If the human makes the first move on the board, the normal orientation is used.

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

### Board Options

Several options can be configured directly from the chessboard.

Set up the normal starting position, move only the **black King** to the indicated square, and press the Cynus **Scan** button. Configuration positions are handled internally by CynusLink. The D5/D6 Free Analysis positions are also sent to ChessLink so that compatible analysis software can observe the complete board-state sequence.

| Black King | Function | Display |
| --- | --- | --- |
| `e5` | Sound OFF (`sound 0`) | `snd off` |
| `e6` | Sound ON (`sound 70`) | `snd on` |
| `h5` | Board flip ON (`set flip board on`) | unchanged |
| `h6` | Board flip OFF (`set flip board off`) | unchanged |
| `d5` | Free Analysis ON | `Freemode` |
| `d6` | Free Analysis OFF | `play` |
| `c5` | Set Position | `Set Pos` |
| `c6` | Cancel Set Position | `play` |

For the sound options, the display automatically returns to `play` after a short delay.

### Free Analysis

Move the black King from its normal starting square to **D5** and scan the board to start Free Analysis. The display shows `Freemode`.

In Free Analysis, the Cynus is used as an electronic sensor board. The robot does not move pieces. CynusLink scans the board every five seconds. A changed scan is processed immediately; an unchanged position is not sent again.

ChessLink reports the complete 64-square occupancy whenever the physical board changes. Because Cynus supplies only the completed FEN of a scan, CynusLink recreates the individual sensor events serially:

- A moved piece is lifted and then placed on its destination.
- A removed piece generates only a lift event.
- A newly added piece generates only a placement event.
- A replaced piece is lifted and the new piece is then placed.
- Multiple changes are sent one after another, with at least 500 ms between events.

A lift and its corresponding placement are sent directly one after another before the next piece is processed. This lets the connected software construct its own FEN from normal ChessLink board changes without requiring a legal move sequence.

Free Analysis must also be supported by the connected chess software. Examples are **BearChess** on Windows and **PGN Master** on Android.

For a position with many changes, first build and scan it without the King that will determine the side to move. The missing King keeps the analysis engine paused. When the rest of the position is visible in the software, place that King and wait for the next automatic scan. It is then reported as the final individual placement:

- If the white King is placed last, Black moves first.
- If the black King is placed last, White moves first.

Castling rights are not supplied by the physical board and must be selected in the connected software.

To leave Free Analysis, set up the normal starting position with the black King on **D6** and scan the board. The display returns to `play`, normal play resumes, and the human can make the next move.

### Set Position

Move the black King from its normal starting square to **C5** and scan the board to start Set Position. The display shows `Set Pos`.

Set up the desired position and use the Cynus Scan/clock button when it is ready. There is no automatic scan in this mode. The position is sent to the connected chess computer only after both Kings have been detected.

After the position has been transferred, select the side to move in the connected chess software.

After the position has been accepted and the side to move has been selected in the software, the display returns to `play` and a normal game continues from the new position. To cancel Set Position, set up the normal starting position with the black King on **C6** and scan the board.

## Installation

No development environment is required.

### Web Installer

1. Connect the ESP32-S3 or ESP32-C3 SuperMini to your computer via USB.
2. Open the **CynusLink Web Installer** in Google Chrome or Microsoft Edge.
3. Click **Connect** and select your ESP32.
4. Confirm the installation.
5. Wait until flashing is complete.
6. Disconnect and reconnect USB power.

**[Install CynusLink on ESP32-S3 or ESP32-C3 SuperMini](https://parlue.github.io/CynusLink/)**

> The web installer requires a browser with Web Serial support, such as
> Google Chrome or Microsoft Edge.

The web installer supports **ESP32-S3 DevKitC-1 compatible boards** and the
**ESP32-C3 SuperMini**. It automatically selects the correct firmware for the
connected ESP32.

## Architecture

``` text
Chess Software
     |
     | ChessLink BLE
     |
    ESP32
  CynusLink
     |
     | Cynus BLE
     |
 Manya Cynus
```

The ESP32 operates simultaneously as:

- **BLE Peripheral** for the ChessLink connection
- **BLE Central** for the Cynus connection
- **Protocol gateway** between both devices

## Features

- Wireless BLE-to-BLE gateway
- ChessLink-compatible interface
- Reads the physical Cynus board position
- Transfers physical moves to the chess software
- Sends engine moves to the Cynus robot
- Supports playing as White or Black
- Shows startup position differences on the Cynus display
- New games can be started by setting up the initial position and using the Cynus scanner
- Standalone operation with only USB power
- Screwless 3D-printable ESP32-S3 enclosure

## Hardware

- Manya Cynus chess robot
- ESP32-S3 DevKitC-1 compatible board or ESP32-C3 SuperMini
- USB-C power supply
- Optional 3D-printed enclosure for the ESP32-S3 version

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

Contains the ESP32-S3 and ESP32-C3 source code.

### `Firmware`

Contains ready-to-flash firmware builds.

### `stl`

Contains the STL files for the screwless 3D-printed ESP32-S3 enclosure.

### `docs`

Contains the browser-based ESP32-S3 and ESP32-C3 Web Installer used by GitHub Pages.

## Building from Source

The firmware can also be built with PlatformIO.

Target configurations:

``` text
Boards:    ESP32-S3 DevKitC-1 / ESP32-C3 SuperMini
Framework: Arduino
BLE:       NimBLE-Arduino
```

## Status

**Work in progress.**

Communication between the Cynus, ESP32 and ChessLink-compatible
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
