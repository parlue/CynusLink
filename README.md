Cynus ChessLink BLE Gateway

An ESP32-S3 based Bluetooth Low Energy gateway that aims to make the Manya Cynus chess robot compatible with software that supports the Millennium ChessLink BLE protocol.

Status: Experimental / Work in Progress

Overview

The Manya Cynus uses its own Bluetooth Low Energy protocol. Many chess applications already support Millennium ChessLink compatible hardware.

This project uses an ESP32-S3 as a Bluetooth-to-Bluetooth protocol gateway.

The ESP32-S3 acts as:

a BLE Peripheral / GATT Server toward the chess application

a BLE Central / GATT Client toward the Manya Cynus

a protocol translator between ChessLink and Cynus

Architecture

+---------------------------+
| Chess Application         |
| ChessLink BLE Support     |
+-------------+-------------+
              |
              | Bluetooth LE
              | ChessLink Protocol
              |
              v
+---------------------------+
| ESP32-S3 Gateway          |
|                           |
| ChessLink BLE Peripheral  |
|            |              |
| Protocol Translation      |
|            |              |
| Cynus BLE Client          |
+-------------+-------------+
              |
              | Bluetooth LE
              | Cynus Protocol
              |
              v
+---------------------------+
| Manya Cynus Chess Robot   |
+---------------------------+

The ESP32-S3 only requires USB power. No wired data connection between the computer and the robot is required.

Project Goals

Make the Manya Cynus usable with ChessLink-compatible chess software.

Use an inexpensive ESP32-S3 as a standalone gateway.

Communicate wirelessly on both sides using Bluetooth Low Energy.

Translate board states and chess moves in real time.

Allow moves from the chess application to be physically executed by the Cynus robot.

Keep the solution independent from a specific desktop operating system.

Provide a compact screwless 3D-printed enclosure.

Hardware

Required Components

Component

Purpose

ESP32-S3 DevKitC-1 compatible board

Runs the gateway firmware

Manya Cynus

Physical chess robot

USB-C cable

Programming and power

USB power supply

Standalone power

3D-printed enclosure

Protects the ESP32-S3

The current enclosure is designed around an ESP32-S3-WROOM-1 / ESP32-S3-DevKitC-1 style board.

Why ESP32-S3?

The ESP32-S3 can handle both required BLE roles at the same time.

Chess Application
        |
        | BLE
        v
+----------------------+
| ESP32-S3             |
|                      |
| BLE Peripheral       |
|        +             |
| BLE Central          |
+----------+-----------+
           |
           | BLE
           v
     Manya Cynus

This makes it possible to bridge both Bluetooth connections using a single microcontroller.

How It Works

ChessLink Side

Toward the computer, tablet or phone, the ESP32-S3 presents itself as a ChessLink-compatible BLE device.

The chess application communicates with the gateway as if it were connected to supported ChessLink hardware.

The gateway is intended to handle commands such as:

board status requests

version requests

LED or move indication commands

display clearing commands

additional compatibility commands where required

Cynus Side

At the same time, the ESP32-S3 connects to the Manya Cynus as a BLE client.

The Cynus protocol can provide the current board position and can accept commands that cause the robot to execute chess moves.

Example commands are conceptually similar to:

get fen
scan board
move e2e4

The exact protocol handling is implemented in the Cynus communication layer.

Protocol Translation

Cynus to ChessLink

When the chess application requests the current board state, the gateway obtains or uses the latest Cynus board position and converts it into the format expected by ChessLink.

ChessLink board request
        |
        v
ESP32-S3 Gateway
        |
        v
Read current Cynus position
        |
        v
Parse FEN
        |
        v
Convert board representation
        |
        v
Create ChessLink response
        |
        v
Chess Application

ChessLink to Cynus

When the chess application indicates a move, the gateway determines the source and destination squares and converts the move into UCI notation.

Chess Application
        |
        v
ChessLink move indication
        |
        v
ESP32-S3 Gateway
        |
        v
Determine source square
        |
        v
Determine destination square
        |
        v
Create UCI move
        |
        v
Example: e7e5
        |
        v
Send Cynus move command
        |
        v
Manya Cynus executes move

Internal Board State

The gateway maintains an internal representation of the current chess position.

This is useful for:

detecting moves

translating board states

handling move indications

validating source and destination squares

processing castling

processing en passant

processing promotion

recovering after temporary Bluetooth disconnects

Firmware Architecture

The firmware is planned as a set of independent modules.

src/
|
+-- main.cpp
|
+-- ble_chesslink_server.cpp
+-- ble_chesslink_server.h
|
+-- ble_cynus_client.cpp
+-- ble_cynus_client.h
|
+-- chesslink_protocol.cpp
+-- chesslink_protocol.h
|
+-- cynus_protocol.cpp
+-- cynus_protocol.h
|
+-- board_state.cpp
+-- board_state.h
|
+-- gateway.cpp
+-- gateway.h

ble_chesslink_server

Implements the BLE peripheral seen by the chess application.

ble_cynus_client

Scans for the Cynus, establishes the BLE connection and handles communication with the robot.

chesslink_protocol

Parses and generates ChessLink protocol messages.

This includes:

command parsing

response generation

checksum handling

board-state encoding

move or LED command decoding

cynus_protocol

Handles communication with the Manya Cynus.

This includes:

commands

responses

notifications

FEN handling

move execution

board_state

Maintains the current chess position and converts between:

FEN

board coordinates

internal square representation

ChessLink board representation

UCI moves

gateway

Connects both protocol implementations and performs the actual protocol translation.

Expected Startup Sequence

1. ESP32-S3 boots
2. BLE stack is initialized
3. Gateway searches for the Manya Cynus
4. ESP32-S3 connects to the Cynus
5. Cynus notifications are enabled
6. External control mode is initialized
7. Current board position is requested
8. ChessLink-compatible BLE service starts
9. Chess application connects to the ESP32-S3
10. Gateway begins translating messages

3D-Printed Enclosure

A custom enclosure is being developed for the ESP32-S3 gateway.

Design goals:

no screws

two-piece snap-fit construction

internal PCB supports

USB connector access

ventilation openings

compact dimensions

suitable for standard FDM printers

minimal or no support material

Enclosure Structure

ESP32-S3 Enclosure
|
+-- Lid
|   |
|   +-- Snap-fit tabs
|   +-- Ventilation openings
|
+-- Base
    |
    +-- PCB supports
    +-- Snap-fit sockets
    +-- USB opening
    +-- Ventilation openings

The enclosure consists of a base and a snap-fit lid.

PETG is recommended because it tolerates repeated flexing of snap-fit features better than many rigid PLA formulations.

Suggested starting print settings:

Layer height: 0.20 mm
Walls:        3
Material:     PETG
Supports:     None where possible

Software Stack

The planned firmware stack is:

ESP32-S3

C/C++

PlatformIO

Arduino framework or ESP-IDF

NimBLE

NimBLE is preferred because the ESP32-S3 must maintain both BLE client and BLE peripheral functionality.

Development Roadmap

Select ESP32-S3 hardware

Design initial screwless enclosure

Establish Cynus BLE connection

Read Cynus board position

Implement Cynus command parser

Emulate ChessLink BLE services

Implement ChessLink checksum handling

Implement ChessLink board-status command

Convert Cynus FEN to ChessLink board state

Decode ChessLink move / LED commands

Translate moves to UCI notation

Send moves to the Cynus robot

Handle castling

Handle en passant

Handle promotion

Implement automatic BLE reconnection

Test with real ChessLink-compatible applications

Finalize enclosure dimensions

Release first usable firmware

Repository Structure

Cynus-ChessLink-Gateway/
|
+-- README.md
+-- LICENSE
+-- platformio.ini
|
+-- src/
|   |
|   +-- main.cpp
|   +-- ble_chesslink_server.cpp
|   +-- ble_cynus_client.cpp
|   +-- chesslink_protocol.cpp
|   +-- cynus_protocol.cpp
|   +-- board_state.cpp
|   +-- gateway.cpp
|
+-- include/
|
+-- docs/
|   |
|   +-- protocol/
|   +-- images/
|
+-- hardware/
    |
    +-- stl/
    +-- openscad/

Project Scope

This is an independent interoperability project.

The goal is not necessarily to reproduce every function of every Millennium ChessLink-compatible device.

The initial target is to implement the subset of the ChessLink protocol required for practical use with the Manya Cynus.

Additional protocol commands can be added as compatibility requirements become known during testing.

Contributing

The project is currently in an early development stage.

Contributions are welcome, especially:

protocol observations

BLE traces

compatibility reports

firmware improvements

testing with different chess applications

enclosure improvements

documentation

Disclaimer

This is an unofficial community project and is not affiliated with, endorsed by, or sponsored by Manya or Millennium.

Product names and protocol names are used only to describe interoperability with the respective hardware.

Use the firmware and hardware designs at your own risk.

License

A final license has not yet been selected.

A possible setup is:

Firmware: MIT or Apache-2.0

3D models: Creative Commons license
