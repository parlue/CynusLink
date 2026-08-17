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
-   Supports starting a new game by scanning the initial board position
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
|   +-- compiled firmware files
|
+-- stl/
    +-- 3D print files
```

### `src`

Contains the ESP32-S3 source code.

### `Firmware`

Contains ready-to-flash firmware builds.

### `stl`

Contains the STL files for the screwless 3D-printed ESP32-S3 enclosure.

## Status

**Work in progress.**

Basic communication between the Cynus, ESP32-S3 and ChessLink-compatible
software is working. Further testing and protocol compatibility
improvements are ongoing.

## Disclaimer

CynusLink is an independent community project and is not affiliated with
or endorsed by Manya or Millennium.

Use at your own risk.
