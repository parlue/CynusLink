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
+-- Firmware/
|   +-- precompiled firmware files
|
+-- stl/
|   +-- 3D print files
|
+-- docs/
    +-- CynusLink Web Installer
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

Basic communication between the Cynus, ESP32-S3 and ChessLink-compatible
software is working. Physical moves can be transferred to the chess
software and engine moves can be executed by the Cynus.

Further testing and protocol compatibility improvements are ongoing.

## Disclaimer

CynusLink is an independent community project and is not affiliated with
or endorsed by Manya or Millennium.

Use at your own risk.
