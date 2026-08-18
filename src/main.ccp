#include <Arduino.h>
#include <NimBLEDevice.h>

static constexpr const char* CYNUS_PREFIX = "CYNUS-";
static NimBLEUUID CYNUS_SERVICE((uint16_t)0xFFF0);
static NimBLEUUID CYNUS_CHAR((uint16_t)0xFFF1);

static constexpr const char* CL_NAME = "MILLENNIUM CHESS";
static constexpr const char* CL_SERVICE = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
static constexpr const char* CL_TX = "49535343-1e4d-4bd9-ba61-23c647249616";
static constexpr const char* CL_RX = "49535343-8841-43f4-a8d4-ecbe34729bb3";

enum GatewayState {
    SEARCH_CYNUS,
    WAIT_CHESSLINK,
    SYNC_BOARD,
    RUNNING
};

static GatewayState state = SEARCH_CYNUS;

enum MoveCycleState {
    WAIT_HUMAN_MOVE,
    WAIT_ENGINE_MOVE,
    WAIT_ROBOT_POSITION
};

static MoveCycleState moveCycle = WAIT_HUMAN_MOVE;

// Diagnostic timers used by setMoveCycle(); declared here before first use.
static uint32_t moveCycleEnteredAt = 0;
static uint32_t lastMoveWaitWarningAt = 0;

static const char* moveCycleName(MoveCycleState s) {
    switch (s) {
        case WAIT_HUMAN_MOVE:    return "WAIT_HUMAN_MOVE";
        case WAIT_ENGINE_MOVE:   return "WAIT_ENGINE_MOVE";
        case WAIT_ROBOT_POSITION:return "WAIT_ROBOT_POSITION";
        default:                 return "?";
    }
}

static void setMoveCycle(MoveCycleState s) {
    if (moveCycle == s) return;
    moveCycle = s;
    moveCycleEnteredAt = millis();
    lastMoveWaitWarningAt = 0;
    Serial.printf("[MOVE] -> %s\n", moveCycleName(moveCycle));
}

// --------------------
// Cynus
// --------------------

static const NimBLEAdvertisedDevice* cynusDev = nullptr;
static NimBLEClient* cynusClient = nullptr;
static NimBLERemoteCharacteristic* cynusChr = nullptr;

static bool cynusConnectPending = false;
static bool cynusReady = false;
static String cynusLine;

// --------------------
// ChessLink
// --------------------

static NimBLEServer* clServer = nullptr;
static NimBLECharacteristic* clTx = nullptr;
static NimBLECharacteristic* clRx = nullptr;

static bool clServerStarted = false;
static bool clConnected = false;
static bool clNotify = false;
static uint16_t clConnHandle = BLE_HS_CONN_HANDLE_NONE;
static String clBuf;

// NimBLE callbacks run on the nimble_host task.
// Keep callbacks small and process complete ChessLink frames in loop().
static volatile bool clProcessPending = false;

// --------------------
// Board
// --------------------

static char board64[65] = "................................................................";
static String fenNow = "";
static bool boardSynced = false;

static constexpr const char* START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";

// --------------------
// Startup / recovery supervision
// --------------------

enum BoardSyncPurpose {
    BOARD_SYNC_NONE,
    BOARD_SYNC_STARTUP,
    BOARD_SYNC_RECOVERY
};

static BoardSyncPurpose boardSyncPurpose = BOARD_SYNC_NONE;

// False after every ESP boot. Initial ChessLink advertising is not enabled
// until Cynus has actively returned the normal starting position.
static bool initialStartupComplete = false;

// ChessLink advertising is intentionally blocked during the boot warmup.
static bool chessAdvertisingAllowed = false;

// BLE callbacks only report events. Recovery work is performed in loop().
static volatile bool chessConnectEventPending = false;
static volatile bool chessDisconnectEventPending = false;
static volatile bool chessRejectEventPending = false;
static volatile bool cynusDisconnectEventPending = false;

static uint32_t nextCynusScanAt = 0;
static uint32_t boardSyncRequestAt = 0;
static bool boardSyncRequestPending = false;

static uint32_t lastLinkHealthCheckAt = 0;
static constexpr uint32_t LINK_HEALTH_CHECK_MS = 1000;

// Diagnostic timers only. They never invent or cancel a chess move.

// Set when Cynus explicitly asks the external controller for a move.
static bool cynusWaitingForMove = false;

// External-engine mode control.
// There is no reliable ACK for "set internal engine off", so we do not
// advance state based on arbitrary Cynus text. We send the command and
// use "get move" later as the real proof that external control is active.
static bool cynusEngineOffCommandSent = false;
static bool cynusExternalModeConfirmed = false;
static uint32_t engineOffSentAt = 0;
static bool cynusEngineOffSecondSendPending = false;
static bool chessAdvertisingPendingAfterEngineOff = false;

// Human-move gating:
// During camera recognition, FEN updates are buffered only.
// After "get move", we request one fresh FEN and publish only that one
// to the ChessLink side.
static bool publishNextFenToChessLink = false;
static String bufferedFen = "";

// --------------------
// ChessLink EEPROM emulation
// --------------------

static uint8_t ee[256];
static uint8_t led[81];

// --------------------
// Forward declarations
// --------------------

static void startCynusScan();
static bool connectCynus();
static bool sendCynus(const char* s);

static void createChessLinkServer();
static void warmupChessLinkAdvertising();
static void startChessLinkAdvertising();
static void stopChessLinkAdvertising();

static void sendCL(const String& payload);
static void sendStatus();
static void processCL();
static void handleCL(const String& frame);

static void setState(GatewayState s);

static void requestBoardSync(BoardSyncPurpose purpose, uint32_t delayMs = 0);
static void recoverChessLinkLoss(const char* source);
static void recoverCynusLoss(const char* source);
static void processSupervision();

// ============================================================
// Utility
// ============================================================

static const char* stateName(GatewayState s) {
    switch (s) {
        case SEARCH_CYNUS:   return "SEARCH_CYNUS";
        case WAIT_CHESSLINK: return "WAIT_CHESSLINK";
        case SYNC_BOARD:     return "SYNC_BOARD";
        case RUNNING:        return "RUNNING";
        default:             return "?";
    }
}

static void setState(GatewayState s) {
    if (state == s) return;

    state = s;
    Serial.printf("[STATE] -> %s\n", stateName(state));
}

static int hn(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool hb(char a, char b, uint8_t& v) {
    int x = hn(a);
    int y = hn(b);

    if (x < 0 || y < 0) return false;

    v = (x << 4) | y;
    return true;
}

static String hx(uint8_t v) {
    const char* H = "0123456789ABCDEF";

    String s;
    s += H[v >> 4];
    s += H[v & 15];
    return s;
}

static uint8_t xsum(const String& s) {
    uint8_t x = 0;

    for (size_t i = 0; i < s.length(); ++i) {
        x ^= ((uint8_t)s[i] & 0x7F);
    }

    return x;
}

static uint8_t odd7(uint8_t c) {
    c &= 0x7F;

    uint8_t n = 0;

    for (int i = 0; i < 7; ++i) {
        if (c & (1 << i)) ++n;
    }

    if (!(n & 1)) c |= 0x80;

    return c;
}

static bool valid(const String& f) {
    if (f.length() < 3) return false;

    size_t n = f.length() - 2;
    uint8_t r;

    if (!hb(f[n], f[n + 1], r)) return false;

    return xsum(f.substring(0, n)) == r;
}

static int flen(char c) {
    switch (c) {
        case 'S':
        case 'X':
        case 'T':
        case 'V':
            return 3;

        case 'R':
            return 5;

        case 'W':
            return 7;

        case 'L':
            return 167;

        default:
            return -1;
    }
}

// ============================================================
// FEN
// ============================================================

static bool fen2board(String f) {
    int sp = f.indexOf(' ');

    if (sp >= 0) {
        f = f.substring(0, sp);
    }

    char out[65];
    int n = 0;

    for (size_t i = 0; i < f.length(); ++i) {
        char c = f[i];

        if (c == '/') continue;

        if (c >= '1' && c <= '8') {
            for (int k = 0; k < c - '0'; ++k) {
                if (n >= 64) return false;
                out[n++] = '.';
            }

            continue;
        }

        if (strchr("KQRBNPkqrbnp", c)) {
            if (n >= 64) return false;
            out[n++] = c;
        } else {
            return false;
        }
    }

    if (n != 64) return false;

    out[64] = 0;
    memcpy(board64, out, 65);

    return true;
}

// ============================================================
// ChessLink TX
// ============================================================

static void sendCL(const String& payload) {
    if (!clTx || !clConnected || !clNotify) {
        Serial.printf(
            "[CHESS TX] not ready: %s\n",
            payload.c_str()
        );
        return;
    }

    String full = payload + hx(xsum(payload));

    std::vector<uint8_t> bytes;
    bytes.reserve(full.length());

    // BLE carries the ChessLink frame as plain 7-bit ASCII bytes.
    // Do NOT set an odd-parity bit on BLE TX.
    for (size_t i = 0; i < full.length(); ++i) {
        bytes.push_back((uint8_t)full[i]);
    }

    Serial.printf(
        "[CHESS TX] %s\n",
        full.c_str()
    );

    Serial.print("[CHESS TX HEX]");
    for (uint8_t b : bytes) {
        Serial.printf(" %02X", b);
    }
    Serial.println();

    uint16_t mtu = 23;

    if (
        clServer &&
        clConnHandle != BLE_HS_CONN_HANDLE_NONE
    ) {
        // IMPORTANT:
        // getPeerInfo(uint8_t) expects a peer INDEX, not a connection handle.
        // For a handle use getPeerMTU() / getPeerInfoByHandle().
        uint16_t peerMtu =
            clServer->getPeerMTU(clConnHandle);

        if (peerMtu >= 23) {
            mtu = peerMtu;
        }
    }

    // ATT notification payload = negotiated MTU - 3 bytes.
    size_t maxPayload =
        mtu > 3 ? (size_t)(mtu - 3) : 20;

    Serial.printf(
        "[CHESS TX] MTU=%u, max notification payload=%u, frame=%u\n",
        mtu,
        (unsigned)maxPayload,
        (unsigned)bytes.size()
    );

    if (bytes.size() <= maxPayload) {
        // Preferred path: one complete ChessLink frame in one BLE notification.
        clTx->setValue(
            bytes.data(),
            bytes.size()
        );

        bool ok = clTx->notify();

        Serial.printf(
            "[CHESS TX] single notification: %s\n",
            ok ? "OK" : "FAILED"
        );

        return;
    }

    // Fallback for clients that negotiate only the default MTU.
    Serial.println(
        "[CHESS TX] frame larger than MTU, using BLE fragmentation fallback"
    );

    for (
        size_t offset = 0;
        offset < bytes.size();
        offset += maxPayload
    ) {
        size_t count = std::min(
            maxPayload,
            bytes.size() - offset
        );

        clTx->setValue(
            bytes.data() + offset,
            count
        );

        bool ok = clTx->notify();

        Serial.printf(
            "[CHESS TX] fragment offset=%u size=%u %s\n",
            (unsigned)offset,
            (unsigned)count,
            ok ? "OK" : "FAILED"
        );

        delay(8);
    }
}

static void sendStatus() {
    if (!boardSynced) {
        Serial.println("[CHESS] status requested, but board not synced yet");
        return;
    }

    String p = "s";

    // ChessLink wire format from the protocol specification:
    // A8..H8, A7..H7, ... A1..H1.
    //
    // board64 already uses exactly this order, so do NOT rotate or mirror
    // it here. Board orientation is a host/application concern.
    for (int i = 0; i < 64; ++i) {
        p += board64[i];
    }

    Serial.print("[CHESS] TX board A8..H1: ");
    Serial.println(board64);

    sendCL(p);
}

static bool autoReport() {
    return (ee[2] & 7) != 1;
}

// ============================================================
// ChessLink BLE callbacks
// ============================================================

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(
        NimBLEServer* server,
        NimBLEConnInfo& info
    ) override {
        clConnected = true;
        clConnHandle = info.getConnHandle();

        Serial.printf(
            "[CHESS] connected %s, MTU=%u\n",
            info.getAddress().toString().c_str(),
            info.getMTU()
        );

        // A client can occasionally catch the short advertising warmup.
        // Reject it unless Cynus is ready and advertising was explicitly opened.
        if (
            !chessAdvertisingAllowed ||
            !cynusReady ||
            state != WAIT_CHESSLINK
        ) {
            Serial.println(
                "[CHESS] connection arrived outside allowed window; scheduling reject"
            );
            chessRejectEventPending = true;
            return;
        }

        // Connection parameter update is BLE housekeeping only.
        if (clServer) {
            clServer->updateConnParams(
                info.getConnHandle(),
                12, 24,
                0,
                200
            );
        }

        chessConnectEventPending = true;
    }

    void onMTUChange(
        uint16_t mtu,
        NimBLEConnInfo& info
    ) override {
        Serial.printf(
            "[CHESS] MTU changed to %u for handle=%u\n",
            mtu,
            info.getConnHandle()
        );
    }

    void onDisconnect(
        NimBLEServer* server,
        NimBLEConnInfo& info,
        int reason
    ) override {
        Serial.printf(
            "[CHESS] disconnected %d\n",
            reason
        );

        clConnected = false;
        clNotify = false;
        clConnHandle = BLE_HS_CONN_HANDLE_NONE;
        chessDisconnectEventPending = true;
    }
};

static ServerCB serverCB;

class TxCB : public NimBLECharacteristicCallbacks {
    void onSubscribe(
        NimBLECharacteristic* characteristic,
        NimBLEConnInfo& info,
        uint16_t value
    ) override {
        clNotify = value != 0;

        Serial.printf(
            "[CHESS] notify %s\n",
            clNotify ? "on" : "off"
        );

    }
};

static TxCB txCB;

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(
        NimBLECharacteristic* characteristic,
        NimBLEConnInfo& info
    ) override {
        std::string value = characteristic->getValue();

        Serial.printf(
            "[CHESS RX] chunk %u\n",
            (unsigned)value.size()
        );

        for (uint8_t b : value) {
            char a = (char)(b & 0x7F);

            if (a >= 0x20 && a <= 0x7E) {
                clBuf += a;
            }
        }

        if (clBuf.length() > 512) {
            Serial.println("[CHESS RX] buffer overflow guard - clearing");
            clBuf = "";
        }

        // Defer parsing and all notification responses to loop().
        clProcessPending = true;
    }
};

static RxCB rxCB;

// ============================================================
// LED move inference
// ============================================================

static uint8_t ledValue(int fileCorner, int rankCornerTop) {
    // ChessLink protocol numbering (0-based here):
    //
    // LED 1  = A8 corner  -> index 0
    // LED 9  = A1 corner  -> index 8
    // LED 73 = H8 corner  -> index 72
    // LED 81 = H1 corner  -> index 80
    //
    // Therefore the 9x9 corner grid is serialized column-major:
    // index = fileCorner * 9 + rankCornerTop
    //
    // fileCorner:    0..8 from A-side to H-side boundary
    // rankCornerTop: 0..8 from rank-8 edge to rank-1 edge
    if (
        fileCorner < 0 || fileCorner > 8 ||
        rankCornerTop < 0 || rankCornerTop > 8
    ) {
        return 0;
    }

    return led[fileCorner * 9 + rankCornerTop];
}

static String squareName(int file, int rankTop) {
    String s;
    s += (char)('a' + file);
    s += (char)('8' - rankTop);
    return s;
}

struct HighlightedSquare {
    int file;
    int rankTop;
    int score;
    uint8_t dominantPattern;
};

static uint8_t dominantSquarePattern(int file, int rankTop) {
    const uint8_t corners[4] = {
        ledValue(file,     rankTop),
        ledValue(file + 1, rankTop),
        ledValue(file,     rankTop + 1),
        ledValue(file + 1, rankTop + 1)
    };

    int counts[256] = {0};

    for (uint8_t v : corners) {
        if (v != 0) {
            counts[v]++;
        }
    }

    int best = 0;

    for (int v = 1; v < 256; ++v) {
        if (counts[v] > counts[best]) {
            best = v;
        }
    }

    return (uint8_t)best;
}

static int squareHighlightScore(int file, int rankTop) {
    int score = 0;

    if (ledValue(file,     rankTop)     != 0) ++score;
    if (ledValue(file + 1, rankTop)     != 0) ++score;
    if (ledValue(file,     rankTop + 1) != 0) ++score;
    if (ledValue(file + 1, rankTop + 1) != 0) ++score;

    return score;
}

static bool extractMoveFromLCommand(String& uci) {
    HighlightedSquare squares[64];
    int n = 0;

    for (int rankTop = 0; rankTop < 8; ++rankTop) {
        for (int file = 0; file < 8; ++file) {
            squares[n++] = {
                file,
                rankTop,
                squareHighlightScore(file, rankTop),
                dominantSquarePattern(file, rankTop)
            };
        }
    }

    int hitIndex[64];
    int hitCount = 0;

    for (int i = 0; i < 64; ++i) {
        // A normally highlighted square has all four corner LEDs active.
        // Accept 3 as tolerance for applications with a shared/missing corner.
        if (squares[i].score >= 3) {
            hitIndex[hitCount++] = i;
        }
    }

    Serial.printf(
        "[GATEWAY] L highlights %d squares:",
        hitCount
    );

    for (int i = 0; i < hitCount; ++i) {
        const auto& s = squares[hitIndex[i]];

        Serial.printf(
            " %s(score=%d,pat=%02X)",
            squareName(s.file, s.rankTop).c_str(),
            s.score,
            s.dominantPattern
        );
    }

    Serial.println();

    // ChessLink L is also used for correction displays and arbitrary LED
    // patterns. Never move the robot unless exactly two chess squares are
    // clearly encoded.
    if (hitCount != 2) {
        Serial.println(
            "[GATEWAY] L ignored: not exactly two highlighted chess squares"
        );
        return false;
    }

    const auto& a = squares[hitIndex[0]];
    const auto& b = squares[hitIndex[1]];

    const String sqA = squareName(a.file, a.rankTop);
    const String sqB = squareName(b.file, b.rankTop);

    const int idxA = a.rankTop * 8 + a.file;
    const int idxB = b.rankTop * 8 + b.file;

    const bool occA = board64[idxA] != '.';
    const bool occB = board64[idxB] != '.';

    Serial.printf(
        "[GATEWAY] move candidates %s occ=%d pat=%02X, %s occ=%d pat=%02X\n",
        sqA.c_str(),
        occA,
        a.dominantPattern,
        sqB.c_str(),
        occB,
        b.dominantPattern
    );

    // Normal non-capture: source is the occupied square.
    if (occA && !occB) {
        uci = sqA + sqB;
        return true;
    }

    if (occB && !occA) {
        uci = sqB + sqA;
        return true;
    }

    // Capture: both may be occupied. In observed ChessLink/PicoChess
    // delta frames 0x33 and 0xCC distinguish source and destination.
    // Keep this explicit and conservative; never guess unknown patterns.
    if (
        occA && occB &&
        a.dominantPattern == 0x33 &&
        b.dominantPattern == 0xCC
    ) {
        uci = sqA + sqB;
        return true;
    }

    if (
        occA && occB &&
        a.dominantPattern == 0xCC &&
        b.dominantPattern == 0x33
    ) {
        uci = sqB + sqA;
        return true;
    }

    Serial.println(
        "[GATEWAY] L ignored: source/destination is ambiguous"
    );

    return false;
}

// ============================================================
// ChessLink protocol
// ============================================================

static void handleCL(const String& f) {
    Serial.printf(
        "[CHESS RX] %s\n",
        f.c_str()
    );

    if (!valid(f)) {
        Serial.println("[CHESS] bad checksum");
        return;
    }

    switch (f[0]) {
        case 'S':
            sendStatus();
            break;

        case 'V':
            sendCL("v0100");
            break;

        case 'X':
            memset(led, 0, sizeof(led));
            sendCL("x");
            break;

        case 'T':
            // Re-enter external-control mode before resynchronizing.
            if (cynusReady && clConnected) {
                boardSynced = false;
                if (sendCynus("set internal engine off\n")) {
                    cynusEngineOffCommandSent = true;
                    engineOffSentAt = millis();
                    Serial.println("[CYNUS] internal engine OFF command re-sent");
                }
            }
            break;

        case 'R': {
            uint8_t a;

            if (hb(f[1], f[2], a)) {
                sendCL("r" + hx(a) + hx(ee[a]));
            }

            break;
        }

        case 'W': {
            uint8_t a;
            uint8_t d;

            if (
                hb(f[1], f[2], a) &&
                hb(f[3], f[4], d)
            ) {
                ee[a] = d;
                sendCL("w" + hx(a) + hx(d));
            }

            break;
        }

        case 'L': {
            // Never allow robot movement unless the gateway is fully running
            // and we are specifically waiting for the engine reply.
            if (state != RUNNING || moveCycle != WAIT_ENGINE_MOVE) {
                Serial.printf(
                    "[GATEWAY] ignoring L command: state=%s move=%s\n",
                    stateName(state),
                    moveCycleName(moveCycle)
                );

                sendCL("l");
                break;
            }

            uint8_t slot;

            if (!hb(f[1], f[2], slot)) {
                break;
            }

            bool ok = true;

            for (int i = 0; i < 81; ++i) {
                if (!hb(
                        f[3 + i * 2],
                        f[4 + i * 2],
                        led[i]
                    )) {
                    ok = false;
                    break;
                }
            }

            if (!ok) break;

            sendCL("l");

            String uci;

            if (extractMoveFromLCommand(uci)) {
                Serial.printf(
                    "[GATEWAY] decoded move %s\n",
                    uci.c_str()
                );

                if (!cynusWaitingForMove) {
                    Serial.println(
                        "[GATEWAY] move ignored: Cynus did not request an external move"
                    );
                    break;
                }

                String cmd = "move ";
                cmd += uci;
                cmd += "\n";

                if (sendCynus(cmd.c_str())) {
                    cynusWaitingForMove = false;
                    setMoveCycle(WAIT_ROBOT_POSITION);

                    Serial.println(
                        "[GATEWAY] move sent to Cynus"
                    );
                } else {
                    Serial.println(
                        "[GATEWAY] failed to send move to Cynus"
                    );
                }
            } else {
                Serial.println(
                    "[GATEWAY] LED pattern not uniquely a move"
                );
            }

            break;
        }
    }
}

static void processCL() {
    while (clBuf.length()) {
        int n = flen(clBuf[0]);

        if (n < 0) {
            clBuf.remove(0, 1);
            continue;
        }

        if ((int)clBuf.length() < n) {
            return;
        }

        String f = clBuf.substring(0, n);
        clBuf.remove(0, n);

        handleCL(f);
    }
}

// ============================================================
// ChessLink server
// ============================================================

static void createChessLinkServer() {
    if (clServerStarted) {
        return;
    }

    clServer = NimBLEDevice::createServer();
    clServer->setCallbacks(&serverCB);

    NimBLEService* service =
        clServer->createService(CL_SERVICE);

    clTx = service->createCharacteristic(
        CL_TX,
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::NOTIFY
    );

    clRx = service->createCharacteristic(
        CL_RX,
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR
    );

    clTx->setCallbacks(&txCB);
    clRx->setCallbacks(&rxCB);

    // NimBLE-Arduino 2.x: services are started with the server.
    // NimBLEService::start() is deprecated and has no effect.
    if (!clServer->start()) {
        Serial.println(
            "[CHESS] ERROR: could not start GATT server"
        );
        return;
    }

    NimBLEAdvertising* advertising =
        NimBLEDevice::getAdvertising();

    advertising->setName(CL_NAME);
    advertising->addServiceUUID(CL_SERVICE);
    advertising->enableScanResponse(true);

    clServerStarted = true;

    Serial.println(
        "[CHESS] server created, advertising still OFF"
    );
}

static void warmupChessLinkAdvertising() {
    if (!clServerStarted) {
        return;
    }

    Serial.println(
        "[CHESS] initializing advertising subsystem..."
    );

    chessAdvertisingAllowed = false;

    // Important NimBLE-Arduino dual-role workaround:
    // initialize/start advertising once BEFORE making any outbound
    // client connection. This avoids a late GAP/GATT initialization
    // after the ESP32 is already connected to Cynus.
    bool started = NimBLEDevice::startAdvertising();

    if (!started) {
        Serial.println(
            "[CHESS] advertising warmup FAILED"
        );
        return;
    }

    delay(150);

    NimBLEDevice::stopAdvertising();

    delay(100);

    Serial.println(
        "[CHESS] advertising initialized and OFF"
    );
}

static void startChessLinkAdvertising() {
    if (
        !clServerStarted ||
        !cynusReady ||
        !boardSynced ||
        state != WAIT_CHESSLINK
    ) {
        Serial.println(
            "[CHESS] advertising blocked: robot/board/state not ready"
        );
        return;
    }

    chessAdvertisingAllowed = true;

    bool started = NimBLEDevice::startAdvertising();

    if (started) {
        Serial.println(
            "[CHESS] advertising as MILLENNIUM CHESS"
        );
    } else {
        chessAdvertisingAllowed = false;
        Serial.println(
            "[CHESS] ERROR: could not start advertising"
        );
    }
}

// ============================================================
// Cynus receive
// ============================================================

static void cynusBytes(
    const uint8_t* data,
    size_t len
) {
    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];

        if (c == '\n') {
            String line = cynusLine;
            cynusLine = "";
            line.trim();

            Serial.printf(
                "[CYNUS LINE] %s\n",
                line.c_str()
            );

            if (line.equalsIgnoreCase("get move")) {
                cynusWaitingForMove = true;
                cynusExternalModeConfirmed = true;

                Serial.println(
                    "[GATEWAY] Cynus is waiting for an external move"
                );

                Serial.println(
                    "[CYNUS] external-engine mode CONFIRMED by get move"
                );

                Serial.printf(
                    "[LINKS] Cynus=%d ChessLink=%d Notify=%d State=%s Move=%s\n",
                    cynusReady ? 1 : 0,
                    clConnected ? 1 : 0,
                    clNotify ? 1 : 0,
                    stateName(state),
                    moveCycleName(moveCycle)
                );

                if (!(cynusReady && clConnected)) {
                    Serial.println(
                        "[GATEWAY] get move received, but both BLE sides are not connected"
                    );
                    continue;
                }

                setState(RUNNING);

                if (moveCycle == WAIT_HUMAN_MOVE) {
                    // Human move has been accepted by Cynus.
                    // Fetch exactly one stable FEN and publish it to ChessLink.
                    boardSynced = false;
                    publishNextFenToChessLink = true;

                    Serial.println(
                        "[MOVE] human move accepted; requesting stable FEN"
                    );

                    sendCynus("get fen\n");
                } else if (moveCycle == WAIT_ROBOT_POSITION) {
                    // Cynus finished executing the engine move and is now ready
                    // for the next human move. Refresh the board internally only.
                    boardSynced = false;
                    publishNextFenToChessLink = false;

                    Serial.println(
                        "[MOVE] robot move complete; refreshing internal board only"
                    );

                    sendCynus("get fen\n");
                } else {
                    Serial.println(
                        "[MOVE] get move received while already waiting for engine; ignored"
                    );
                }

                continue;
            }

            if (line.startsWith("fen:")) {
                String f = line.substring(4);
                f.trim();

                if (fen2board(f)) {
                    bufferedFen = f;

                    Serial.printf(
                        "[BOARD] buffered FEN %s\n",
                        bufferedFen.c_str()
                    );

                    // --------------------------------------------------------
                    // STARTUP / RECOVERY BOARD SYNCHRONIZATION
                    // --------------------------------------------------------
                    if (
                        state == SYNC_BOARD &&
                        cynusReady &&
                        boardSyncPurpose != BOARD_SYNC_NONE
                    ) {
                        String placement = bufferedFen;
                        int placementSpace = placement.indexOf(' ');

                        if (placementSpace >= 0) {
                            placement =
                                placement.substring(0, placementSpace);
                        }

                        // On a fresh ESP boot only the normal initial position
                        // unlocks ChessLink. If it is wrong, the user corrects
                        // the physical board and presses the Cynus scan button.
                        if (
                            boardSyncPurpose == BOARD_SYNC_STARTUP &&
                            placement != START_FEN
                        ) {
                            Serial.printf(
                                "[STARTUP] board not ready: %s\n",
                                bufferedFen.c_str()
                            );
                            Serial.println(
                                "[STARTUP] waiting for initial position; correct board and press Cynus scan"
                            );
                            continue;
                        }

                        fenNow = bufferedFen;
                        boardSynced = true;

                        Serial.printf(
                            "[BOARD] synchronized physical FEN %s\n",
                            fenNow.c_str()
                        );

                        Serial.printf(
                            "[BOARD] ChessLink %s\n",
                            board64
                        );

                        BoardSyncPurpose completedPurpose =
                            boardSyncPurpose;
                        boardSyncPurpose = BOARD_SYNC_NONE;

                        if (
                            completedPurpose == BOARD_SYNC_STARTUP
                        ) {
                            initialStartupComplete = true;
                            Serial.println(
                                "[STARTUP] initial position confirmed"
                            );
                        } else {
                            Serial.println(
                                "[RECOVERY] current physical position refreshed"
                            );
                        }

                        // If ChessLink is not connected, only now expose it.
                        if (!clConnected) {
                            setState(WAIT_CHESSLINK);
                            startChessLinkAdvertising();
                        } else {
                            // Defensive recovery path if a client connected
                            // while a resync was pending.
                            setState(RUNNING);
                            setMoveCycle(WAIT_HUMAN_MOVE);
                        }

                        continue;
                    }

                    // During normal play, do NOT publish arbitrary camera
                    // intermediate states. Only publish the first valid FEN
                    // explicitly requested after "get move".
                    if (
                        state == RUNNING &&
                        clConnected &&
                        publishNextFenToChessLink
                    ) {
                        fenNow = bufferedFen;
                        boardSynced = true;
                        publishNextFenToChessLink = false;

                        Serial.printf(
                            "[BOARD] accepted HUMAN stable FEN %s\n",
                            fenNow.c_str()
                        );

                        Serial.printf(
                            "[BOARD] ChessLink %s\n",
                            board64
                        );

                        // This is the human move. ChessLink E2ROM address 02
                        // controls automatic reports. Mode 001 disables them;
                        // every other mode allows an automatic 's...' report.
                        if (autoReport()) {
                            sendStatus();
                        } else {
                            Serial.println(
                                "[CHESS] automatic reports disabled by E2ROM 02; waiting for S"
                            );
                        }

                        setMoveCycle(WAIT_ENGINE_MOVE);

                        continue;
                    }

                    if (
                        state == RUNNING &&
                        moveCycle == WAIT_ROBOT_POSITION
                    ) {
                        // Board updates after an engine move belong to the robot.
                        // Keep them internally, but DO NOT send them back to ChessLink.
                        fenNow = bufferedFen;
                        boardSynced = true;

                        Serial.printf(
                            "[BOARD] accepted ROBOT FEN internally %s\n",
                            fenNow.c_str()
                        );

                        Serial.println(
                            "[MOVE] robot position stored; not echoed to ChessLink"
                        );

                        setMoveCycle(WAIT_HUMAN_MOVE);

                        continue;
                    }

                    Serial.println(
                        "[BOARD] camera/intermediate FEN buffered only; not sent to ChessLink"
                    );
                }
            }
        } else if (c != '\r') {
            cynusLine += c;
        }
    }
}

static void cynusNotify(
    NimBLERemoteCharacteristic* characteristic,
    uint8_t* data,
    size_t len,
    bool notify
) {
    Serial.print("[CYNUS RX] ");

    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];

        if (c == '\r') Serial.print("\\r");
        else if (c == '\n') Serial.print("\\n");
        else if (c >= 32 && c <= 126) Serial.print(c);
        else Serial.printf("\\x%02X", data[i]);
    }

    Serial.println();

    cynusBytes(data, len);
}

// ============================================================
// Cynus callbacks
// ============================================================

class CClientCB : public NimBLEClientCallbacks {
    void onDisconnect(
        NimBLEClient* client,
        int reason
    ) override {
        Serial.printf(
            "[CYNUS] disconnected %d\n",
            reason
        );

        cynusReady = false;
        cynusChr = nullptr;
        cynusDisconnectEventPending = true;
    }
};

static CClientCB cclientCB;

class ScanCB : public NimBLEScanCallbacks {
    void onResult(
        const NimBLEAdvertisedDevice* dev
    ) override {
        if (!dev->haveName()) return;

        std::string name = dev->getName();

        if (name.rfind(CYNUS_PREFIX, 0) != 0) {
            return;
        }

        Serial.printf(
            "[CYNUS] found %s\n",
            name.c_str()
        );

        NimBLEDevice::getScan()->stop();

        cynusDev = dev;
        cynusConnectPending = true;
    }

    void onScanEnd(
        const NimBLEScanResults& results,
        int reason
    ) override {
        if (!cynusConnectPending && !cynusReady) {
            delay(500);
            startCynusScan();
        }
    }
};

static ScanCB scanCB;

// ============================================================
// Cynus connect/send
// ============================================================

static void startCynusScan() {
    if (cynusReady || cynusConnectPending) {
        return;
    }

    NimBLEScan* scan =
        NimBLEDevice::getScan();

    scan->setScanCallbacks(
        &scanCB,
        false
    );

    scan->setInterval(100);
    scan->setWindow(100);
    scan->setActiveScan(true);

    Serial.println(
        "[CYNUS] scan CYNUS-*"
    );

    scan->start(0);
}

static bool sendCynus(const char* s) {
    if (!cynusReady || !cynusChr) {
        Serial.println(
            "[CYNUS TX] not ready"
        );

        return false;
    }

    Serial.printf(
        "[CYNUS TX] %s",
        s
    );

    if (cynusChr->canWriteNoResponse()) {
        return cynusChr->writeValue(
            (const uint8_t*)s,
            strlen(s),
            false
        );
    }

    if (cynusChr->canWrite()) {
        return cynusChr->writeValue(
            (const uint8_t*)s,
            strlen(s),
            true
        );
    }

    return false;
}

static bool connectCynus() {
    if (!cynusDev) {
        return false;
    }

    if (!cynusClient) {
        cynusClient =
            NimBLEDevice::createClient();

        cynusClient->setClientCallbacks(
            &cclientCB,
            false
        );

        cynusClient->setConnectTimeout(10000);
    }

    if (!cynusClient->connect(cynusDev)) {
        return false;
    }

    NimBLERemoteService* service =
        cynusClient->getService(
            CYNUS_SERVICE
        );

    if (!service) {
        cynusClient->disconnect();
        return false;
    }

    cynusChr =
        service->getCharacteristic(
            CYNUS_CHAR
        );

    if (
        !cynusChr ||
        !cynusChr->canNotify() ||
        !cynusChr->subscribe(
            true,
            cynusNotify
        )
    ) {
        cynusClient->disconnect();
        return false;
    }

    cynusReady = true;

    Serial.printf(
        "[CYNUS] connected %s\n",
        cynusDev->getName().c_str()
    );

    // Before exposing ChessLink, force Cynus into external-engine mode.
    // Send the command once now and a second time after a short delay.
    cynusExternalModeConfirmed = false;
    cynusEngineOffCommandSent = false;
    cynusEngineOffSecondSendPending = false;
    chessAdvertisingPendingAfterEngineOff = false;
    boardSyncPurpose = BOARD_SYNC_NONE;
    boardSyncRequestPending = false;
    boardSynced = false;

    if (sendCynus("set internal engine off\n")) {
        Serial.println(
            "[CYNUS] internal engine OFF command #1 sent"
        );

        engineOffSentAt = millis();
        cynusEngineOffSecondSendPending = true;
        chessAdvertisingPendingAfterEngineOff = true;
    } else {
        Serial.println(
            "[CYNUS] ERROR sending internal engine OFF command #1"
        );
    }

    return true;
}

// ============================================================
// Advertising control
// ============================================================

static void stopChessLinkAdvertising() {
    if (!clServerStarted) {
        return;
    }

    chessAdvertisingAllowed = false;
    NimBLEDevice::getAdvertising()->stop();

    Serial.println(
        "[CHESS] advertising stopped"
    );
}

// ============================================================
// Startup / recovery supervision
// ============================================================

static void requestBoardSync(
    BoardSyncPurpose purpose,
    uint32_t delayMs
) {
    if (!cynusReady) {
        return;
    }

    boardSyncPurpose = purpose;
    boardSynced = false;
    publishNextFenToChessLink = false;
    cynusWaitingForMove = false;
    setMoveCycle(WAIT_HUMAN_MOVE);
    setState(SYNC_BOARD);

    boardSyncRequestPending = true;
    boardSyncRequestAt = millis() + delayMs;

    Serial.printf(
        "[SYNC] board request queued purpose=%s\n",
        purpose == BOARD_SYNC_STARTUP ? "STARTUP" : "RECOVERY"
    );
}

static void recoverChessLinkLoss(const char* source) {
    Serial.printf(
        "[RECOVERY] ChessLink loss detected by %s\n",
        source
    );

    // Keep the client away while the physical position is refreshed.
    stopChessLinkAdvertising();

    clConnected = false;
    clNotify = false;
    clConnHandle = BLE_HS_CONN_HANDLE_NONE;
    clBuf = "";

    memset(led, 0, sizeof(led));
    cynusWaitingForMove = false;
    publishNextFenToChessLink = false;
    setMoveCycle(WAIT_HUMAN_MOVE);

    if (!cynusReady) {
        boardSynced = false;
        setState(SEARCH_CYNUS);
        return;
    }

    // Refresh the real physical position before advertising again.
    // Before the first successful startup we must still require START_FEN.
    requestBoardSync(
        initialStartupComplete
            ? BOARD_SYNC_RECOVERY
            : BOARD_SYNC_STARTUP,
        150
    );
}

static void recoverCynusLoss(const char* source) {
    Serial.printf(
        "[RECOVERY] Cynus loss detected by %s\n",
        source
    );

    cynusReady = false;
    cynusChr = nullptr;
    cynusDev = nullptr;
    boardSynced = false;
    fenNow = "";
    bufferedFen = "";
    boardSyncPurpose = BOARD_SYNC_NONE;
    boardSyncRequestPending = false;

    cynusWaitingForMove = false;
    publishNextFenToChessLink = false;
    cynusEngineOffCommandSent = false;
    cynusExternalModeConfirmed = false;
    cynusEngineOffSecondSendPending = false;
    chessAdvertisingPendingAfterEngineOff = false;

    memset(led, 0, sizeof(led));
    setMoveCycle(WAIT_HUMAN_MOVE);

    stopChessLinkAdvertising();

    if (
        clServer &&
        clServer->getConnectedCount() > 0 &&
        clConnHandle != BLE_HS_CONN_HANDLE_NONE
    ) {
        Serial.println(
            "[RECOVERY] robot lost; disconnecting ChessLink peer"
        );
        clServer->disconnect(clConnHandle);
    }

    clConnected = false;
    clNotify = false;
    clConnHandle = BLE_HS_CONN_HANDLE_NONE;
    clBuf = "";

    setState(SEARCH_CYNUS);

    nextCynusScanAt = millis() + 500;
}

static void processSupervision() {
    // Reject a connection that caught the short boot advertising warmup.
    if (chessRejectEventPending) {
        chessRejectEventPending = false;

        if (
            clServer &&
            clConnHandle != BLE_HS_CONN_HANDLE_NONE
        ) {
            uint16_t rejectHandle = clConnHandle;
            Serial.println(
                "[CHESS] rejecting premature client"
            );
            clServer->disconnect(rejectHandle);
        }
    }

    if (cynusDisconnectEventPending) {
        cynusDisconnectEventPending = false;
        recoverCynusLoss("callback");
    }

    if (chessDisconnectEventPending) {
        chessDisconnectEventPending = false;

        // If Cynus recovery already owns the teardown, do not start
        // a competing ChessLink recovery.
        if (cynusReady) {
            recoverChessLinkLoss("callback");
        }
    }

    if (chessConnectEventPending) {
        chessConnectEventPending = false;

        if (
            cynusReady &&
            boardSynced &&
            state == WAIT_CHESSLINK
        ) {
            chessAdvertisingAllowed = false;
            setState(RUNNING);
            setMoveCycle(WAIT_HUMAN_MOVE);

            Serial.println(
                "[CHESS] connected to synchronized board; gateway RUNNING"
            );
        } else {
            Serial.println(
                "[CHESS] connect event without synchronized board; rejecting"
            );
            chessRejectEventPending = true;
        }
    }

    if (
        boardSyncRequestPending &&
        cynusReady &&
        (int32_t)(millis() - boardSyncRequestAt) >= 0
    ) {
        boardSyncRequestPending = false;
        Serial.println("[SYNC] actively requesting physical FEN");
        sendCynus("get fen\n");
    }

    // Once per second verify the actual NimBLE link state as a fallback
    // for a missed/stale disconnect callback.
    if (
        millis() - lastLinkHealthCheckAt >=
        LINK_HEALTH_CHECK_MS
    ) {
        lastLinkHealthCheckAt = millis();

        if (
            cynusReady &&
            cynusClient &&
            !cynusClient->isConnected()
        ) {
            recoverCynusLoss("health-check");
            return;
        }

        if (
            clConnected &&
            clServer &&
            clServer->getConnectedCount() == 0
        ) {
            recoverChessLinkLoss("health-check");
            return;
        }
    }

    // Diagnostic only: long protocol waits are logged but never force
    // a move-state transition.
    if (
        state == RUNNING &&
        moveCycle != WAIT_HUMAN_MOVE &&
        moveCycleEnteredAt != 0 &&
        millis() - moveCycleEnteredAt >= 30000 &&
        (
            lastMoveWaitWarningAt == 0 ||
            millis() - lastMoveWaitWarningAt >= 30000
        )
    ) {
        lastMoveWaitWarningAt = millis();

        Serial.printf(
            "[WATCHDOG] still waiting in %s for %lu ms; links Cynus=%d ChessLink=%d\n",
            moveCycleName(moveCycle),
            (unsigned long)(millis() - moveCycleEnteredAt),
            cynusReady ? 1 : 0,
            clConnected ? 1 : 0
        );
    }

    if (
        !cynusReady &&
        !cynusConnectPending &&
        state == SEARCH_CYNUS &&
        nextCynusScanAt != 0 &&
        (int32_t)(millis() - nextCynusScanAt) >= 0
    ) {
        nextCynusScanAt = 0;
        startCynusScan();
    }
}

// ============================================================
// Setup / loop
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println(
        "=== CynusLink Robust Core Baseline v2.2 ==="
    );

    memset(ee, 0, sizeof(ee));

    ee[0] = 0x00;
    ee[1] = 0x14;
    ee[2] = 0x03;
    ee[4] = 0x0F;

    // One NimBLE stack for both roles.
    NimBLEDevice::init(CL_NAME);
    NimBLEDevice::setPower(3);

    // Prefer a larger ATT MTU so a complete ChessLink status frame
    // (67 bytes including checksum) can fit into a single notification.
    NimBLEDevice::setMTU(128);

    // Create the ChessLink GATT server exactly once at boot.
    createChessLinkServer();

    // Initialize the peripheral/advertising path BEFORE the first
    // outbound client connection, then switch advertising back off.
    // The chess software should not have enough time to establish a
    // useful session here; normal advertising starts only after Cynus.
    warmupChessLinkAdvertising();

    setState(SEARCH_CYNUS);
    moveCycleEnteredAt = millis();

    // Stage 1: only Cynus scan. ChessLink advertising is OFF.
    startCynusScan();
}

void loop() {
    if (cynusConnectPending) {
        cynusConnectPending = false;

        if (!connectCynus()) {
            cynusDev = nullptr;

            delay(1000);
            startCynusScan();
        }
    }

    // Process ChessLink commands outside the NimBLE host callback.
    // This avoids stack pressure/re-entrancy when an S/L/X command causes
    // us to send a notification response.
    if (clProcessPending) {
        clProcessPending = false;
        processCL();
    }

    processSupervision();

    // Send engine-off a second time before exposing ChessLink.
    if (
        cynusEngineOffSecondSendPending &&
        cynusReady &&
        millis() - engineOffSentAt >= 300
    ) {
        cynusEngineOffSecondSendPending = false;

        if (sendCynus("set internal engine off\n")) {
            Serial.println(
                "[CYNUS] internal engine OFF command #2 sent"
            );

            engineOffSentAt = millis();
        } else {
            Serial.println(
                "[CYNUS] ERROR sending internal engine OFF command #2"
            );
        }
    }

    // After both engine-off sends, actively fetch the physical board.
    // On a fresh boot ChessLink stays hidden until START_FEN is confirmed.
    if (
        chessAdvertisingPendingAfterEngineOff &&
        cynusReady &&
        !cynusEngineOffSecondSendPending &&
        millis() - engineOffSentAt >= 300
    ) {
        chessAdvertisingPendingAfterEngineOff = false;

        Serial.println(
            "[STARTUP] engine-off sequence complete"
        );

        requestBoardSync(
            initialStartupComplete
                ? BOARD_SYNC_RECOVERY
                : BOARD_SYNC_STARTUP,
            0
        );
    }

    // Debug only:
    // f = request FEN, but only if both sides are connected
    // b = manually send board status
    if (Serial.available()) {
        char c = Serial.read();

        if (c == 'f') {
            if (cynusReady && clConnected) {
                boardSynced = false;
                if (sendCynus("set internal engine off\n")) {
                    cynusEngineOffCommandSent = true;
                    engineOffSentAt = millis();
                    Serial.println("[CYNUS] internal engine OFF command re-sent");
                }
            } else {
                Serial.println(
                    "[DEBUG] both sides must be connected first"
                );
            }
        }

        if (c == 'b') {
            sendStatus();
        }
    }

    delay(10);
}
