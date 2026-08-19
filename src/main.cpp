#include <Arduino.h>
#include <NimBLEDevice.h>

static constexpr const char* CYNUS_PREFIX = "CYNUS-";
static NimBLEUUID CYNUS_SERVICE((uint16_t)0xFFF0);
static NimBLEUUID CYNUS_CHAR((uint16_t)0xFFF1);

static constexpr const char* CL_NAME = "MILLENNIUM CHESS";
static constexpr const char* CL_SERVICE = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
static constexpr const char* CL_TX = "49535343-1e4d-4bd9-ba61-23c647249616";
static constexpr const char* CL_RX = "49535343-8841-43f4-a8d4-ecbe34729bb3";

enum GatewayState { SEARCH_CYNUS, WAIT_CHESSLINK, SYNC_BOARD, RUNNING };
static GatewayState state = SEARCH_CYNUS;

enum MoveCycleState { WAIT_HUMAN_MOVE, WAIT_ENGINE_MOVE, WAIT_ROBOT_POSITION };
static MoveCycleState moveCycle = WAIT_HUMAN_MOVE;
static uint32_t moveCycleEnteredAt = 0;
static uint32_t lastMoveWaitWarningAt = 0;

static const char* moveCycleName(MoveCycleState s) {
    switch (s) {
        case WAIT_HUMAN_MOVE: return "WAIT_HUMAN_MOVE";
        case WAIT_ENGINE_MOVE: return "WAIT_ENGINE_MOVE";
        case WAIT_ROBOT_POSITION: return "WAIT_ROBOT_POSITION";
        default: return "?";
    }
}

static void setMoveCycle(MoveCycleState s) {
    if (moveCycle == s) return;
    moveCycle = s;
    moveCycleEnteredAt = millis();
    lastMoveWaitWarningAt = 0;
    Serial.printf("[MOVE] -> %s\n", moveCycleName(moveCycle));
}

static const NimBLEAdvertisedDevice* cynusDev = nullptr;
static NimBLEClient* cynusClient = nullptr;
static NimBLERemoteCharacteristic* cynusChr = nullptr;
static bool cynusConnectPending = false;
static bool cynusReady = false;
static String cynusLine;

static NimBLEServer* clServer = nullptr;
static NimBLECharacteristic* clTx = nullptr;
static NimBLECharacteristic* clRx = nullptr;
static bool clServerStarted = false;
static bool clConnected = false;
static bool clNotify = false;
static uint16_t clConnHandle = BLE_HS_CONN_HANDLE_NONE;
static String clBuf;
static volatile bool clProcessPending = false;
static volatile bool clStatusPending = false;

static char board64[65] = "................................................................";
static String fenNow = "";
static bool boardSynced = false;
static String lastFenSentToChessLink = "";
static String correctionFenCandidate = "";

enum EngineSide { ENGINE_SIDE_UNKNOWN, ENGINE_SIDE_WHITE, ENGINE_SIDE_BLACK };
static EngineSide engineSide = ENGINE_SIDE_UNKNOWN;

static constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* SOUND_OFF_FEN = "rnbq1bnr/pppppppp/8/4k3/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* SOUND_ON_FEN = "rnbq1bnr/pppppppp/4k3/8/8/8/PPPPPPPP/RNBQKBNR";

enum BoardSyncPurpose { BOARD_SYNC_NONE, BOARD_SYNC_STARTUP, BOARD_SYNC_RECOVERY };
static BoardSyncPurpose boardSyncPurpose = BOARD_SYNC_NONE;
static bool initialStartupComplete = false;
static bool chessAdvertisingAllowed = false;
static volatile bool chessConnectEventPending = false;
static volatile bool chessDisconnectEventPending = false;
static volatile bool chessRejectEventPending = false;
static volatile bool cynusDisconnectEventPending = false;
static uint32_t nextCynusScanAt = 0;
static uint32_t boardSyncRequestAt = 0;
static bool boardSyncRequestPending = false;
static bool boardScanPending = false;
static uint32_t boardScanGetFenAt = 0;
static constexpr uint32_t BOARD_SCAN_WAIT_MS = 1000;
static uint32_t lastLinkHealthCheckAt = 0;
static constexpr uint32_t LINK_HEALTH_CHECK_MS = 1000;

static bool cynusWaitingForMove = false;
static bool cynusEngineOffCommandSent = false;
static bool cynusExternalModeConfirmed = false;
static uint32_t engineOffSentAt = 0;
static bool cynusEngineOffSecondSendPending = false;
static bool chessAdvertisingPendingAfterEngineOff = false;
static bool publishNextFenToChessLink = false;
static String bufferedFen = "";

// UI-only state. It never controls the game state machine.
static bool displayPlayPending = false;
static uint32_t displayPlayAt = 0;

static uint8_t ee[256];
static uint8_t led[81];

static void startCynusScan();
static bool connectCynus();
static bool sendCynus(const char* s);
static void cynusDisplay(const char* text);
static void schedulePlayDisplay(uint32_t delayMs = 1800);
static String moveDisplayText(const String& uci);
static String inferMoveDisplayText(const String& oldFen, const String& newFen);
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

static const char* stateName(GatewayState s) {
    switch (s) {
        case SEARCH_CYNUS: return "SEARCH_CYNUS";
        case WAIT_CHESSLINK: return "WAIT_CHESSLINK";
        case SYNC_BOARD: return "SYNC_BOARD";
        case RUNNING: return "RUNNING";
        default: return "?";
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
    int x = hn(a), y = hn(b);
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
    for (size_t i = 0; i < s.length(); ++i) x ^= ((uint8_t)s[i] & 0x7F);
    return x;
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
        case 'S': case 'X': case 'T': case 'V': return 3;
        case 'R': return 5;
        case 'W': return 7;
        case 'L': return 167;
        default: return -1;
    }
}

static bool fen2board(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);
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
        } else return false;
    }
    if (n != 64) return false;
    out[64] = 0;
    memcpy(board64, out, 65);
    return true;
}

static bool fenPlacementTo64(String f, char out[65]) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);
    int n = 0;
    for (size_t i = 0; i < f.length(); ++i) {
        char c = f[i];
        if (c == '/') continue;
        if (c >= '1' && c <= '8') {
            for (int k = 0; k < c - '0'; ++k) {
                if (n >= 64) return false;
                out[n++] = '.';
            }
        } else if (strchr("KQRBNPkqrbnp", c)) {
            if (n >= 64) return false;
            out[n++] = c;
        } else return false;
    }
    if (n != 64) return false;
    out[64] = 0;
    return true;
}

static bool handleSoundConfigFen(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);

    bool soundOn = f == SOUND_ON_FEN;
    bool soundOff = f == SOUND_OFF_FEN;
    if (!soundOn && !soundOff) return false;

    const char* command = soundOn ? "sound 70\n" : "sound 0\n";
    if (!sendCynus(command)) {
        Serial.printf("[SOUND] failed to set sound %s\n", soundOn ? "ON" : "OFF");
        return true;
    }

    publishNextFenToChessLink = false;
    correctionFenCandidate = "";
    boardSynced = fenNow.length() > 0;
    cynusWaitingForMove = true;
    setMoveCycle(WAIT_HUMAN_MOVE);
    displayPlayPending = false;
    cynusDisplay(soundOn ? "snd on" : "snd off");
    schedulePlayDisplay(2000);
    Serial.printf("[SOUND] sound %s selected with black king; config FEN suppressed from ChessLink\n", soundOn ? "ON" : "OFF");
    return true;
}

static void sendCL(const String& payload) {
    if (!clTx || !clConnected || !clNotify) {
        Serial.printf("[CHESS TX] not ready: %s\n", payload.c_str());
        return;
    }
    String full = payload + hx(xsum(payload));
    std::vector<uint8_t> bytes;
    bytes.reserve(full.length());
    for (size_t i = 0; i < full.length(); ++i) bytes.push_back((uint8_t)full[i]);
    Serial.printf("[CHESS TX] %s\n", full.c_str());
    Serial.print("[CHESS TX HEX]");
    for (uint8_t b : bytes) Serial.printf(" %02X", b);
    Serial.println();
    uint16_t mtu = 23;
    if (clServer && clConnHandle != BLE_HS_CONN_HANDLE_NONE) {
        uint16_t peerMtu = clServer->getPeerMTU(clConnHandle);
        if (peerMtu >= 23) mtu = peerMtu;
    }
    size_t maxPayload = mtu > 3 ? (size_t)(mtu - 3) : 20;
    Serial.printf("[CHESS TX] MTU=%u, max notification payload=%u, frame=%u\n", mtu, (unsigned)maxPayload, (unsigned)bytes.size());
    if (bytes.size() <= maxPayload) {
        clTx->setValue(bytes.data(), bytes.size());
        bool ok = clTx->notify();
        Serial.printf("[CHESS TX] single notification: %s\n", ok ? "OK" : "FAILED");
        return;
    }
    Serial.println("[CHESS TX] frame larger than MTU, using BLE fragmentation fallback");
    for (size_t offset = 0; offset < bytes.size(); offset += maxPayload) {
        size_t count = std::min(maxPayload, bytes.size() - offset);
        clTx->setValue(bytes.data() + offset, count);
        bool ok = clTx->notify();
        Serial.printf("[CHESS TX] fragment offset=%u size=%u %s\n", (unsigned)offset, (unsigned)count, ok ? "OK" : "FAILED");
        delay(8);
    }
}

static void sendStatus() {
    if (!boardSynced) {
        Serial.println("[CHESS] status requested, but board not synced yet");
        return;
    }
    String p = "s";
    for (int i = 0; i < 64; ++i) p += board64[i];
    Serial.print("[CHESS] TX board A8..H1: ");
    Serial.println(board64);
    sendCL(p);
    lastFenSentToChessLink = fenNow;
}

static bool autoReport() { return (ee[2] & 7) != 1; }

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        clConnected = true;
        clConnHandle = info.getConnHandle();
        Serial.printf("[CHESS] connected %s, MTU=%u\n", info.getAddress().toString().c_str(), info.getMTU());
        if (!chessAdvertisingAllowed || !cynusReady || state != WAIT_CHESSLINK) {
            Serial.println("[CHESS] connection arrived outside allowed window; scheduling reject");
            chessRejectEventPending = true;
            return;
        }
        if (clServer) clServer->updateConnParams(info.getConnHandle(), 12, 24, 0, 200);
        chessConnectEventPending = true;
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        Serial.printf("[CHESS] MTU changed to %u for handle=%u\n", mtu, info.getConnHandle());
    }
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
        Serial.printf("[CHESS] disconnected %d\n", reason);
        clConnected = false;
        clNotify = false;
        clConnHandle = BLE_HS_CONN_HANDLE_NONE;
        chessDisconnectEventPending = true;
    }
};
static ServerCB serverCB;

class TxCB : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t value) override {
        clNotify = value != 0;
        Serial.printf("[CHESS] notify %s\n", clNotify ? "on" : "off");
    }
};
static TxCB txCB;

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        std::string value = characteristic->getValue();
        Serial.printf("[CHESS RX] chunk %u\n", (unsigned)value.size());
        for (uint8_t b : value) {
            char a = (char)(b & 0x7F);
            if (a >= 0x20 && a <= 0x7E) clBuf += a;
        }
        if (clBuf.length() > 512) {
            Serial.println("[CHESS RX] buffer overflow guard - clearing");
            clBuf = "";
        }
        clProcessPending = true;
    }
};
static RxCB rxCB;

static uint8_t ledValue(int fileCorner, int rankCornerTop) {
    if (fileCorner < 0 || fileCorner > 8 || rankCornerTop < 0 || rankCornerTop > 8) return 0;
    return led[fileCorner * 9 + rankCornerTop];
}

static String squareName(int file, int rankTop) {
    String s;
    s += (char)('a' + file);
    s += (char)('8' - rankTop);
    return s;
}

static uint8_t dominantSquarePattern(int file, int rankTop) {
    const uint8_t corners[4] = {
        ledValue(file, rankTop), ledValue(file + 1, rankTop),
        ledValue(file, rankTop + 1), ledValue(file + 1, rankTop + 1)
    };
    int counts[256] = {0};
    for (uint8_t v : corners) if (v != 0) counts[v]++;
    int best = 0;
    for (int v = 1; v < 256; ++v) if (counts[v] > counts[best]) best = v;
    return (uint8_t)best;
}

static String moveDisplayText(const String& uci) {
    if (uci == "e1g1" || uci == "e8g8") return "0-0";
    if (uci == "e1c1" || uci == "e8c8") return "0-0-0";
    if (uci.length() >= 5) {
        char promo = (char)toupper((unsigned char)uci[4]);
        if (promo == 'Q' || promo == 'R' || promo == 'B' || promo == 'N') {
            String t = "Chg ";
            t += promo;
            return t;
        }
    }
    if (uci.length() >= 4) {
        String t;
        t += (char)toupper((unsigned char)uci[0]);
        t += uci[1];
        t += '-';
        t += (char)toupper((unsigned char)uci[2]);
        t += uci[3];
        return t;
    }
    return "";
}

static String inferMoveDisplayText(const String& oldFen, const String& newFen) {
    if (!oldFen.length() || !newFen.length()) return "";
    char oldB[65], newB[65];
    if (!fenPlacementTo64(oldFen, oldB) || !fenPlacementTo64(newFen, newB)) return "";

    const struct { int from; int to; const char* uci; } castles[] = {
        {60, 62, "e1g1"}, {60, 58, "e1c1"}, {4, 6, "e8g8"}, {4, 2, "e8c8"}
    };
    for (const auto& c : castles) {
        char king = c.from == 60 ? 'K' : 'k';
        if (oldB[c.from] == king && newB[c.from] == '.' && newB[c.to] == king) return moveDisplayText(c.uci);
    }

    int source = -1;
    for (int i = 0; i < 64; ++i) {
        if (oldB[i] != newB[i] && oldB[i] != '.' && newB[i] == '.') {
            source = i;
            break;
        }
    }
    if (source < 0) return "";

    char mover = oldB[source];
    bool white = mover >= 'A' && mover <= 'Z';
    int destination = -1;
    for (int i = 0; i < 64; ++i) {
        if (oldB[i] == newB[i] || newB[i] == '.') continue;
        char np = newB[i];
        bool sameSide = white ? (np >= 'A' && np <= 'Z') : (np >= 'a' && np <= 'z');
        if (sameSide && i != source) {
            destination = i;
            break;
        }
    }
    if (destination < 0) return "";

    String uci = squareName(source % 8, source / 8) + squareName(destination % 8, destination / 8);
    if ((mover == 'P' || mover == 'p') && newB[destination] != mover) {
        uci += (char)tolower((unsigned char)newB[destination]);
    }
    return moveDisplayText(uci);
}

static bool extractMoveFromLCommand(String& uci) {
    bool observed[81];
    int observedCount = 0;
    for (int i = 0; i < 81; ++i) {
        observed[i] = led[i] != 0;
        if (observed[i]) ++observedCount;
    }
    Serial.printf("[GATEWAY] raw active corner LEDs=%d\n", observedCount);
    if (observedCount < 6 || observedCount > 8) {
        Serial.println("[GATEWAY] L ignored: raw LED pattern is not a two-square move");
        return false;
    }

    auto addSquareCorners = [](bool mask[81], int file, int rankTop) {
        mask[file * 9 + rankTop] = true;
        mask[(file + 1) * 9 + rankTop] = true;
        mask[file * 9 + rankTop + 1] = true;
        mask[(file + 1) * 9 + rankTop + 1] = true;
    };

    struct EndpointPair { int a; int b; };
    EndpointPair pairs[16];
    int pairCount = 0;
    for (int a = 0; a < 64; ++a) {
        for (int b = a + 1; b < 64; ++b) {
            bool expected[81] = {false};
            addSquareCorners(expected, a % 8, a / 8);
            addSquareCorners(expected, b % 8, b / 8);
            bool same = true;
            for (int i = 0; i < 81; ++i) {
                if (expected[i] != observed[i]) { same = false; break; }
            }
            if (same && pairCount < 16) pairs[pairCount++] = {a, b};
        }
    }

    Serial.printf("[GATEWAY] raw LED endpoint pairs=%d\n", pairCount);
    if (pairCount != 1) {
        Serial.println("[GATEWAY] L ignored: LED endpoints are not unique");
        return false;
    }

    int a = pairs[0].a;
    int b = pairs[0].b;
    char pieceA = board64[a];
    char pieceB = board64[b];
    bool occA = pieceA != '.';
    bool occB = pieceB != '.';

    Serial.printf("[GATEWAY] LED endpoints %s piece=%c, %s piece=%c\n",
        squareName(a % 8, a / 8).c_str(), pieceA,
        squareName(b % 8, b / 8).c_str(), pieceB);

    auto sideOfPiece = [](char p) -> EngineSide {
        if (p >= 'A' && p <= 'Z') return ENGINE_SIDE_WHITE;
        if (p >= 'a' && p <= 'z') return ENGINE_SIDE_BLACK;
        return ENGINE_SIDE_UNKNOWN;
    };
    auto sideName = [](EngineSide side) -> const char* {
        if (side == ENGINE_SIDE_WHITE) return "white";
        if (side == ENGINE_SIDE_BLACK) return "black";
        return "unknown";
    };

    int source = -1;
    int destination = -1;

    if (occA != occB) {
        source = occA ? a : b;
        destination = occA ? b : a;
        EngineSide learned = sideOfPiece(board64[source]);
        if (learned != ENGINE_SIDE_UNKNOWN && learned != engineSide) {
            engineSide = learned;
            Serial.printf("[GATEWAY] engine side learned/refreshed: %s\n", sideName(engineSide));
        }
    } else if (occA && occB) {
        if (engineSide == ENGINE_SIDE_UNKNOWN) {
            uint8_t patA = dominantSquarePattern(a % 8, a / 8);
            uint8_t patB = dominantSquarePattern(b % 8, b / 8);
            if (patA == 0x33 && patB == 0xCC) {
                source = a;
                destination = b;
                engineSide = sideOfPiece(pieceA);
            } else if (patA == 0xCC && patB == 0x33) {
                source = b;
                destination = a;
                engineSide = sideOfPiece(pieceB);
            } else {
                Serial.println("[GATEWAY] L ignored: capture direction unknown before engine side learned");
                return false;
            }
            Serial.printf("[GATEWAY] engine side learned from capture: %s\n", sideName(engineSide));
        } else {
            bool aIsEngine = sideOfPiece(pieceA) == engineSide;
            bool bIsEngine = sideOfPiece(pieceB) == engineSide;
            if (aIsEngine == bIsEngine) {
                Serial.println("[GATEWAY] L ignored: capture endpoints do not identify one engine piece");
                return false;
            }
            source = aIsEngine ? a : b;
            destination = aIsEngine ? b : a;
        }
    } else {
        Serial.println("[GATEWAY] L ignored: both LED endpoints are empty");
        return false;
    }

    uci = squareName(source % 8, source / 8) + squareName(destination % 8, destination / 8);
    Serial.printf("[GATEWAY] PicoChess move direction resolved %s engine=%s\n", uci.c_str(), sideName(engineSide));
    return true;
}

static void handleCL(const String& f) {
    Serial.printf("[CHESS RX] %s\n", f.c_str());
    if (!valid(f)) {
        Serial.println("[CHESS] bad checksum");
        return;
    }
    switch (f[0]) {
        case 'S': sendStatus(); break;
        case 'V': sendCL("v0100"); break;
        case 'X': memset(led, 0, sizeof(led)); sendCL("x"); break;
        case 'T':
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
            if (hb(f[1], f[2], a)) sendCL("r" + hx(a) + hx(ee[a]));
            break;
        }
        case 'W': {
            uint8_t a, d;
            if (hb(f[1], f[2], a) && hb(f[3], f[4], d)) {
                ee[a] = d;
                sendCL("w" + hx(a) + hx(d));
            }
            break;
        }
        case 'L': {
            if (state != RUNNING || moveCycle != WAIT_ENGINE_MOVE) {
                Serial.printf("[GATEWAY] ignoring L command: state=%s move=%s\n", stateName(state), moveCycleName(moveCycle));
                sendCL("l");
                break;
            }
            uint8_t slot;
            if (!hb(f[1], f[2], slot)) break;
            bool ok = true;
            for (int i = 0; i < 81; ++i) {
                if (!hb(f[3 + i * 2], f[4 + i * 2], led[i])) { ok = false; break; }
            }
            if (!ok) break;
            sendCL("l");
            String uci;
            if (extractMoveFromLCommand(uci)) {
                Serial.printf("[GATEWAY] decoded move %s\n", uci.c_str());

                String displayMove = moveDisplayText(uci);
                if (displayMove.length()) {
                    displayPlayPending = false;
                    cynusDisplay(displayMove.c_str());
                }

                if (!cynusWaitingForMove) {
                    Serial.println("[GATEWAY] move ignored: Cynus did not request an external move");
                    break;
                }
                String cmd = "move ";
                cmd += uci;
                cmd += "\n";
                if (sendCynus(cmd.c_str())) {
                    cynusWaitingForMove = false;
                    setMoveCycle(WAIT_ROBOT_POSITION);
                    Serial.println("[GATEWAY] move sent to Cynus");
                } else Serial.println("[GATEWAY] failed to send move to Cynus");
            } else Serial.println("[GATEWAY] LED pattern not uniquely a move");
            break;
        }
    }
}

static void processCL() {
    while (clBuf.length()) {
        int n = flen(clBuf[0]);
        if (n < 0) { clBuf.remove(0, 1); continue; }
        if ((int)clBuf.length() < n) return;
        String f = clBuf.substring(0, n);
        clBuf.remove(0, n);
        handleCL(f);
    }
}

static void createChessLinkServer() {
    if (clServerStarted) return;
    clServer = NimBLEDevice::createServer();
    clServer->setCallbacks(&serverCB);
    NimBLEService* service = clServer->createService(CL_SERVICE);
    clTx = service->createCharacteristic(CL_TX, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    clRx = service->createCharacteristic(CL_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    clTx->setCallbacks(&txCB);
    clRx->setCallbacks(&rxCB);
    if (!clServer->start()) {
        Serial.println("[CHESS] ERROR: could not start GATT server");
        return;
    }
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setName(CL_NAME);
    advertising->addServiceUUID(CL_SERVICE);
    advertising->enableScanResponse(true);
    clServerStarted = true;
    Serial.println("[CHESS] server created, advertising still OFF");
}

static void warmupChessLinkAdvertising() {
    if (!clServerStarted) return;
    Serial.println("[CHESS] initializing advertising subsystem...");
    chessAdvertisingAllowed = false;
    bool started = NimBLEDevice::startAdvertising();
    if (!started) {
        Serial.println("[CHESS] advertising warmup FAILED");
        return;
    }
    delay(150);
    NimBLEDevice::stopAdvertising();
    delay(100);
    Serial.println("[CHESS] advertising initialized and OFF");
}

static void startChessLinkAdvertising() {
    if (!clServerStarted || !cynusReady || !boardSynced || state != WAIT_CHESSLINK) {
        Serial.println("[CHESS] advertising blocked: robot/board/state not ready");
        return;
    }
    chessAdvertisingAllowed = true;
    bool started = NimBLEDevice::startAdvertising();
    if (started) Serial.println("[CHESS] advertising as MILLENNIUM CHESS");
    else {
        chessAdvertisingAllowed = false;
        Serial.println("[CHESS] ERROR: could not start advertising");
    }
}

static void cynusBytes(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        if (c == '\n') {
            String line = cynusLine;
            cynusLine = "";
            line.trim();
            Serial.printf("[CYNUS LINE] %s\n", line.c_str());

            if (line.equalsIgnoreCase("get move")) {
                bool firstExternalReady = !cynusExternalModeConfirmed;
                cynusWaitingForMove = true;
                cynusExternalModeConfirmed = true;
                Serial.println("[GATEWAY] Cynus is waiting for an external move");
                Serial.println("[CYNUS] external-engine mode CONFIRMED by get move");
                Serial.printf("[LINKS] Cynus=%d ChessLink=%d Notify=%d State=%s Move=%s\n", cynusReady ? 1 : 0, clConnected ? 1 : 0, clNotify ? 1 : 0, stateName(state), moveCycleName(moveCycle));
                if (!(cynusReady && clConnected)) {
                    Serial.println("[GATEWAY] get move received, but both BLE sides are not connected");
                    continue;
                }
                setState(RUNNING);
                if (firstExternalReady) {
                    displayPlayPending = false;
                    cynusDisplay("play");
                    Serial.println("[DISPLAY] PLAY released after Cynus external-engine readiness confirmation");
                }
                if (moveCycle == WAIT_HUMAN_MOVE) {
                    boardSynced = false;
                    publishNextFenToChessLink = true;
                    Serial.println("[MOVE] human move accepted; requesting stable FEN");
                    sendCynus("get fen\n");
                } else if (moveCycle == WAIT_ROBOT_POSITION) {
                    boardSynced = false;
                    publishNextFenToChessLink = false;
                    Serial.println("[MOVE] robot move complete; refreshing internal board only");
                    displayPlayPending = false;
                    cynusDisplay("play");
                    sendCynus("get fen\n");
                } else {
                    if (correctionFenCandidate.length() && correctionFenCandidate != lastFenSentToChessLink) {
                        if (fen2board(correctionFenCandidate)) {
                            fenNow = correctionFenCandidate;
                            boardSynced = true;
                            Serial.printf("[CORRECTION] accepted rescanned FEN %s\n", fenNow.c_str());
                            Serial.printf("[CORRECTION] ChessLink %s\n", board64);
                            if (autoReport()) {
                                clStatusPending = true;
                                Serial.println("[CORRECTION] corrected board queued for PicoChess");
                            } else Serial.println("[CORRECTION] automatic reports disabled; waiting for S");
                        }
                    } else Serial.println("[MOVE] get move while waiting for engine; no changed correction FEN");
                    correctionFenCandidate = "";
                }
                continue;
            }

            if (line.startsWith("fen:")) {
                String f = line.substring(4);
                f.trim();

                if (initialStartupComplete && state == RUNNING && clConnected && moveCycle == WAIT_HUMAN_MOVE && handleSoundConfigFen(f)) {
                    continue;
                }

                if (fen2board(f)) {
                    bufferedFen = f;
                    Serial.printf("[BOARD] buffered FEN %s\n", bufferedFen.c_str());

                    if (state == SYNC_BOARD && cynusReady && boardSyncPurpose != BOARD_SYNC_NONE) {
                        String placement = bufferedFen;
                        int placementSpace = placement.indexOf(' ');
                        if (placementSpace >= 0) placement = placement.substring(0, placementSpace);
                        if (boardSyncPurpose == BOARD_SYNC_STARTUP && placement != START_FEN) {
                            Serial.printf("[STARTUP] board not ready: %s\n", bufferedFen.c_str());
                            Serial.println("[STARTUP] waiting for initial position; correct board and press Cynus scan");
                            continue;
                        }
                        fenNow = bufferedFen;
                        boardSynced = true;
                        Serial.printf("[BOARD] synchronized physical FEN %s\n", fenNow.c_str());
                        Serial.printf("[BOARD] ChessLink %s\n", board64);
                        BoardSyncPurpose completedPurpose = boardSyncPurpose;
                        boardSyncPurpose = BOARD_SYNC_NONE;
                        if (completedPurpose == BOARD_SYNC_STARTUP) {
                            initialStartupComplete = true;
                            engineSide = ENGINE_SIDE_UNKNOWN;
                            Serial.println("[STARTUP] initial position confirmed");
                        } else Serial.println("[RECOVERY] current physical position refreshed");
                        if (!clConnected) {
                            setState(WAIT_CHESSLINK);
                            cynusDisplay("BT Scan");
                            Serial.println("[DISPLAY] BT Scan: valid board confirmed, scanning for ChessLink");
                            startChessLinkAdvertising();
                        } else {
                            setState(RUNNING);
                            setMoveCycle(WAIT_HUMAN_MOVE);
                        }
                        continue;
                    }

                    if (state == RUNNING && clConnected && publishNextFenToChessLink) {
                        String previousAcceptedFen = fenNow;
                        fenNow = bufferedFen;
                        boardSynced = true;
                        publishNextFenToChessLink = false;
                        Serial.printf("[BOARD] accepted HUMAN stable FEN %s\n", fenNow.c_str());
                        Serial.printf("[BOARD] ChessLink %s\n", board64);

                        String humanDisplayMove = inferMoveDisplayText(previousAcceptedFen, fenNow);
                        if (humanDisplayMove.length()) {
                            displayPlayPending = false;
                            cynusDisplay(humanDisplayMove.c_str());
                            schedulePlayDisplay(1400);
                        }

                        if (autoReport()) clStatusPending = true;
                        else Serial.println("[CHESS] automatic reports disabled by E2ROM 02; waiting for S");
                        setMoveCycle(WAIT_ENGINE_MOVE);
                        continue;
                    }

                    if (state == RUNNING && clConnected && moveCycle == WAIT_ENGINE_MOVE) {
                        correctionFenCandidate = bufferedFen;
                        if (correctionFenCandidate != lastFenSentToChessLink) Serial.printf("[CORRECTION] candidate FEN buffered %s\n", correctionFenCandidate.c_str());
                        else Serial.println("[CORRECTION] duplicate FEN buffered; no resend needed");
                        continue;
                    }

                    if (state == RUNNING && moveCycle == WAIT_ROBOT_POSITION) {
                        fenNow = bufferedFen;
                        boardSynced = true;
                        Serial.printf("[BOARD] accepted ROBOT FEN internally %s\n", fenNow.c_str());
                        Serial.println("[MOVE] robot position stored; not echoed to ChessLink");
                        setMoveCycle(WAIT_HUMAN_MOVE);
                        continue;
                    }

                    Serial.println("[BOARD] camera/intermediate FEN buffered only; not sent to ChessLink");
                }
            }
        } else if (c != '\r') cynusLine += c;
    }
}

static void cynusNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
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

class CClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int reason) override {
        Serial.printf("[CYNUS] disconnected %d\n", reason);
        cynusReady = false;
        cynusChr = nullptr;
        cynusDisconnectEventPending = true;
    }
};
static CClientCB cclientCB;

class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveName()) return;
        std::string name = dev->getName();
        if (name.rfind(CYNUS_PREFIX, 0) != 0) return;
        Serial.printf("[CYNUS] found %s\n", name.c_str());
        NimBLEDevice::getScan()->stop();
        cynusDev = dev;
        cynusConnectPending = true;
    }
    void onScanEnd(const NimBLEScanResults&, int) override {
        if (!cynusConnectPending && !cynusReady) {
            delay(500);
            startCynusScan();
        }
    }
};
static ScanCB scanCB;

static void startCynusScan() {
    if (cynusReady || cynusConnectPending) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCB, false);
    scan->setInterval(100);
    scan->setWindow(100);
    scan->setActiveScan(true);
    Serial.println("[CYNUS] scan CYNUS-*");
    scan->start(0);
}

static bool sendCynus(const char* s) {
    if (!cynusReady || !cynusChr) {
        Serial.println("[CYNUS TX] not ready");
        return false;
    }
    Serial.printf("[CYNUS TX] %s", s);
    if (cynusChr->canWriteNoResponse()) return cynusChr->writeValue((const uint8_t*)s, strlen(s), false);
    if (cynusChr->canWrite()) return cynusChr->writeValue((const uint8_t*)s, strlen(s), true);
    return false;
}

static void cynusDisplay(const char* text) {
    if (!cynusReady || !cynusChr || !text) return;
    String msg = text;
    if (msg.length() > 7) msg = msg.substring(0, 7);
    String cmd = "display txt ";
    cmd += msg;
    cmd += "\n";
    if (sendCynus(cmd.c_str())) Serial.printf("[DISPLAY] %s\n", msg.c_str());
    else Serial.printf("[DISPLAY] failed: %s\n", msg.c_str());
}

static void schedulePlayDisplay(uint32_t delayMs) {
    displayPlayPending = true;
    displayPlayAt = millis() + delayMs;
}

static bool connectCynus() {
    if (!cynusDev) return false;
    if (!cynusClient) {
        cynusClient = NimBLEDevice::createClient();
        cynusClient->setClientCallbacks(&cclientCB, false);
        cynusClient->setConnectTimeout(10000);
    }
    if (!cynusClient->connect(cynusDev)) return false;
    NimBLERemoteService* service = cynusClient->getService(CYNUS_SERVICE);
    if (!service) { cynusClient->disconnect(); return false; }
    cynusChr = service->getCharacteristic(CYNUS_CHAR);
    if (!cynusChr || !cynusChr->canNotify() || !cynusChr->subscribe(true, cynusNotify)) {
        cynusClient->disconnect();
        return false;
    }
    cynusReady = true;
    Serial.printf("[CYNUS] connected %s\n", cynusDev->getName().c_str());
    Serial.println("[DISPLAY] waiting for a valid initial board before ChessLink scan");
    cynusExternalModeConfirmed = false;
    cynusEngineOffCommandSent = false;
    cynusEngineOffSecondSendPending = false;
    chessAdvertisingPendingAfterEngineOff = false;
    boardSyncPurpose = BOARD_SYNC_NONE;
    boardSyncRequestPending = false;
    boardScanPending = false;
    boardSynced = false;
    engineSide = ENGINE_SIDE_UNKNOWN;
    if (sendCynus("set internal engine off\n")) {
        Serial.println("[CYNUS] internal engine OFF command #1 sent");
        engineOffSentAt = millis();
        cynusEngineOffSecondSendPending = true;
        chessAdvertisingPendingAfterEngineOff = true;
    } else Serial.println("[CYNUS] ERROR sending internal engine OFF command #1");
    return true;
}

static void stopChessLinkAdvertising() {
    if (!clServerStarted) return;
    chessAdvertisingAllowed = false;
    NimBLEDevice::getAdvertising()->stop();
    Serial.println("[CHESS] advertising stopped");
}

static void requestBoardSync(BoardSyncPurpose purpose, uint32_t delayMs) {
    if (!cynusReady) return;
    boardSyncPurpose = purpose;
    boardSynced = false;
    publishNextFenToChessLink = false;
    cynusWaitingForMove = false;
    setMoveCycle(WAIT_HUMAN_MOVE);
    setState(SYNC_BOARD);
    boardSyncRequestPending = true;
    boardSyncRequestAt = millis() + delayMs;
    Serial.printf("[SYNC] board request queued purpose=%s\n", purpose == BOARD_SYNC_STARTUP ? "STARTUP" : "RECOVERY");
}

static void recoverChessLinkLoss(const char* source) {
    Serial.printf("[RECOVERY] ChessLink loss detected by %s\n", source);
    if (cynusReady) {
        displayPlayPending = false;
        Serial.println("[DISPLAY] ChessLink lost; validating board before restarting BT scan");
    }
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
    requestBoardSync(initialStartupComplete ? BOARD_SYNC_RECOVERY : BOARD_SYNC_STARTUP, 150);
}

static void recoverCynusLoss(const char* source) {
    Serial.printf("[RECOVERY] Cynus loss detected by %s\n", source);
    cynusReady = false;
    cynusChr = nullptr;
    cynusDev = nullptr;
    boardSynced = false;
    fenNow = "";
    bufferedFen = "";
    lastFenSentToChessLink = "";
    correctionFenCandidate = "";
    displayPlayPending = false;
    engineSide = ENGINE_SIDE_UNKNOWN;
    boardSyncPurpose = BOARD_SYNC_NONE;
    boardSyncRequestPending = false;
    boardScanPending = false;
    cynusWaitingForMove = false;
    publishNextFenToChessLink = false;
    cynusEngineOffCommandSent = false;
    cynusExternalModeConfirmed = false;
    cynusEngineOffSecondSendPending = false;
    chessAdvertisingPendingAfterEngineOff = false;
    memset(led, 0, sizeof(led));
    setMoveCycle(WAIT_HUMAN_MOVE);
    stopChessLinkAdvertising();
    if (clServer && clServer->getConnectedCount() > 0 && clConnHandle != BLE_HS_CONN_HANDLE_NONE) {
        Serial.println("[RECOVERY] robot lost; disconnecting ChessLink peer");
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
    if (chessRejectEventPending) {
        chessRejectEventPending = false;
        if (clServer && clConnHandle != BLE_HS_CONN_HANDLE_NONE) {
            uint16_t rejectHandle = clConnHandle;
            Serial.println("[CHESS] rejecting premature client");
            clServer->disconnect(rejectHandle);
        }
    }
    if (cynusDisconnectEventPending) {
        cynusDisconnectEventPending = false;
        recoverCynusLoss("callback");
    }
    if (chessDisconnectEventPending) {
        chessDisconnectEventPending = false;
        if (cynusReady) recoverChessLinkLoss("callback");
    }
    if (chessConnectEventPending) {
        chessConnectEventPending = false;
        if (cynusReady && boardSynced && state == WAIT_CHESSLINK) {
            chessAdvertisingAllowed = false;
            setState(RUNNING);
            setMoveCycle(WAIT_HUMAN_MOVE);
            Serial.println("[CHESS] connected to synchronized board; gateway RUNNING");
            displayPlayPending = false;
            cynusDisplay("Connect");
            Serial.println("[DISPLAY] CONNECT held until Cynus confirms external-engine mode");
        } else {
            Serial.println("[CHESS] connect event without synchronized board; rejecting");
            chessRejectEventPending = true;
        }
    }
    if (boardSyncRequestPending && cynusReady && (int32_t)(millis() - boardSyncRequestAt) >= 0) {
        boardSyncRequestPending = false;
        displayPlayPending = false;
        cynusDisplay("B Scan");
        Serial.println("[DISPLAY] B Scan: scanning physical board before ChessLink");
        Serial.println("[SYNC] starting physical board scan");
        if (sendCynus("scan board\n")) {
            boardScanPending = true;
            boardScanGetFenAt = millis() + BOARD_SCAN_WAIT_MS;
        } else {
            Serial.println("[SYNC] ERROR: could not start board scan");
            boardSyncRequestPending = true;
            boardSyncRequestAt = millis() + 500;
        }
    }
    if (boardScanPending && cynusReady && (int32_t)(millis() - boardScanGetFenAt) >= 0) {
        boardScanPending = false;
        Serial.println("[SYNC] board scan complete; requesting fresh FEN");
        if (!sendCynus("get fen\n")) {
            Serial.println("[SYNC] ERROR: could not request FEN after scan");
            boardSyncRequestPending = true;
            boardSyncRequestAt = millis() + 500;
        }
    }
    if (millis() - lastLinkHealthCheckAt >= LINK_HEALTH_CHECK_MS) {
        lastLinkHealthCheckAt = millis();
        if (cynusReady && cynusClient && !cynusClient->isConnected()) {
            recoverCynusLoss("health-check");
            return;
        }
        if (clConnected && clServer && clServer->getConnectedCount() == 0) {
            recoverChessLinkLoss("health-check");
            return;
        }
    }
    if (state == RUNNING && moveCycle != WAIT_HUMAN_MOVE && moveCycleEnteredAt != 0 && millis() - moveCycleEnteredAt >= 30000 && (lastMoveWaitWarningAt == 0 || millis() - lastMoveWaitWarningAt >= 30000)) {
        lastMoveWaitWarningAt = millis();
        Serial.printf("[WATCHDOG] still waiting in %s for %lu ms; links Cynus=%d ChessLink=%d\n", moveCycleName(moveCycle), (unsigned long)(millis() - moveCycleEnteredAt), cynusReady ? 1 : 0, clConnected ? 1 : 0);
    }
    if (!cynusReady && !cynusConnectPending && state == SEARCH_CYNUS && nextCynusScanAt != 0 && (int32_t)(millis() - nextCynusScanAt) >= 0) {
        nextCynusScanAt = 0;
        startCynusScan();
    }
}

static void cynuslinkCoreSetup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println("=== CynusLink Robust Core Baseline v2.11 ===");
    memset(ee, 0, sizeof(ee));
    ee[0] = 0x00;
    ee[1] = 0x14;
    ee[2] = 0x03;
    ee[4] = 0x0F;
    NimBLEDevice::init(CL_NAME);
    NimBLEDevice::setPower(3);
    NimBLEDevice::setMTU(128);
    createChessLinkServer();
    warmupChessLinkAdvertising();
    setState(SEARCH_CYNUS);
    moveCycleEnteredAt = millis();
    startCynusScan();
}

static void cynuslinkCoreLoop() {
    if (cynusConnectPending) {
        cynusConnectPending = false;
        if (!connectCynus()) {
            cynusDev = nullptr;
            delay(1000);
            startCynusScan();
        }
    }
    if (clProcessPending) {
        clProcessPending = false;
        processCL();
    }
    if (clStatusPending) {
        clStatusPending = false;
        sendStatus();
    }

    processSupervision();

    if (displayPlayPending && cynusReady && clConnected && (int32_t)(millis() - displayPlayAt) >= 0) {
        displayPlayPending = false;
        cynusDisplay("play");
    }

    if (cynusEngineOffSecondSendPending && cynusReady && millis() - engineOffSentAt >= 300) {
        cynusEngineOffSecondSendPending = false;
        if (sendCynus("set internal engine off\n")) {
            Serial.println("[CYNUS] internal engine OFF command #2 sent");
            engineOffSentAt = millis();
        } else Serial.println("[CYNUS] ERROR sending internal engine OFF command #2");
    }
    if (chessAdvertisingPendingAfterEngineOff && cynusReady && !cynusEngineOffSecondSendPending && millis() - engineOffSentAt >= 300) {
        chessAdvertisingPendingAfterEngineOff = false;
        Serial.println("[STARTUP] engine-off sequence complete");
        requestBoardSync(initialStartupComplete ? BOARD_SYNC_RECOVERY : BOARD_SYNC_STARTUP, 0);
    }
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
            } else Serial.println("[DEBUG] both sides must be connected first");
        }
        if (c == 'b') sendStatus();
    }
    delay(10);
}

static constexpr const char* FLIPPED_START_FEN_PATCH =
    "RNBKQBNR/PPPPPPPP/8/8/8/8/pppppppp/rnbkqbnr";

static bool startOrientationLatchedPatch = false;
static bool startOrientationFlippedPatch = false;
static bool openingStartPhasePatch = false;
static bool startStatusPendingPatch = false;
static bool flippedBootGatePatch = false;
static String lastStartupErrorDisplayPatch = "";
static String lastGameErrorDisplayPatch = "";

static int startOrientationPatch(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);
    if (f == START_FEN) return 0;
    if (f == FLIPPED_START_FEN_PATCH) return 1;
    return -1;
}

static String startupSquarePatch(int index) {
    String s;
    s += (char)('A' + (index % 8));
    s += (char)('8' - (index / 8));
    return s;
}

static int startupMismatchCountPatch(const char actual[65], const char expected[65]) {
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        bool a = actual[i] != '.';
        bool e = expected[i] != '.';
        if (a != e || (a && e && actual[i] != expected[i])) ++n;
    }
    return n;
}

static String boardDifferenceDisplayPatch(String actualFen, String expectedFen) {
    char actual[65], expected[65];
    if (!fenPlacementTo64(actualFen, actual)) return "";
    if (!fenPlacementTo64(expectedFen, expected)) return "";

    String issues[2];
    int count = 0;
    for (int i = 0; i < 64 && count < 2; ++i) {
        bool a = actual[i] != '.';
        bool e = expected[i] != '.';
        if (!a && e) {
            issues[count++] = "-" + startupSquarePatch(i);
        } else if (a && !e) {
            issues[count++] = "+" + startupSquarePatch(i);
        } else if (a && e && actual[i] != expected[i]) {
            // Occupied by a different piece than expected.
            issues[count++] = "+" + startupSquarePatch(i);
        }
    }

    if (count == 0) return "";
    if (count == 1) return issues[0];
    return issues[0] + "/" + issues[1];
}

static String startupErrorDisplayPatch(String f) {
    char actual[65], normal[65], flipped[65];
    if (!fenPlacementTo64(f, actual)) return "";
    if (!fenPlacementTo64(START_FEN, normal)) return "";
    if (!fenPlacementTo64(FLIPPED_START_FEN_PATCH, flipped)) return "";

    const char* expected = startupMismatchCountPatch(actual, flipped) < startupMismatchCountPatch(actual, normal)
        ? flipped : normal;

    String issues[2];
    int count = 0;
    for (int i = 0; i < 64 && count < 2; ++i) {
        bool a = actual[i] != '.';
        bool e = expected[i] != '.';
        if (!a && e) {
            issues[count++] = "-" + startupSquarePatch(i);
        } else if (a && !e) {
            issues[count++] = "+" + startupSquarePatch(i);
        } else if (a && e && actual[i] != expected[i]) {
            issues[count++] = "+" + startupSquarePatch(i);
        }
    }

    if (count == 0) return "";
    if (count == 1) return issues[0];
    return issues[0] + "/" + issues[1];
}

static void showStartupErrorsPatch() {
    if (initialStartupComplete || state != SYNC_BOARD || bufferedFen.length() == 0) return;
    if (startOrientationPatch(bufferedFen) >= 0) {
        lastStartupErrorDisplayPatch = "";
        return;
    }

    String msg = startupErrorDisplayPatch(bufferedFen);
    if (!msg.length() || msg == lastStartupErrorDisplayPatch) return;
    lastStartupErrorDisplayPatch = msg;
    cynusDisplay(msg.c_str());
    sendCynus("play audio error\n");
    Serial.printf("[STARTUP] position error display: %s (error audio)\n", msg.c_str());
}

static void showGameRescanErrorsPatch() {
    // The robust core already uses correctionFenCandidate while waiting for
    // the engine. This is exactly the safe place to compare a repeated scan:
    // a genuine human move has already been accepted and sent to ChessLink,
    // so lastFenSentToChessLink is the expected physical position.
    if (!initialStartupComplete || state != RUNNING || moveCycle != WAIT_ENGINE_MOVE) {
        lastGameErrorDisplayPatch = "";
        return;
    }
    if (!correctionFenCandidate.length() || !lastFenSentToChessLink.length()) return;

    String msg = boardDifferenceDisplayPatch(correctionFenCandidate, lastFenSentToChessLink);
    if (!msg.length()) {
        if (lastGameErrorDisplayPatch.length()) {
            lastGameErrorDisplayPatch = "";
            cynusDisplay("play");
            Serial.println("[RESCAN] board matches expected position again");
        }
        return;
    }
    if (msg == lastGameErrorDisplayPatch) return;

    lastGameErrorDisplayPatch = msg;
    displayPlayPending = false;
    cynusDisplay(msg.c_str());
    sendCynus("play audio error\n");
    Serial.printf("[RESCAN] board difference: %s (error audio)\n", msg.c_str());
}

static void configureStartOrientationPatch(bool flipped) {
    startOrientationLatchedPatch = true;
    startOrientationFlippedPatch = flipped;
    openingStartPhasePatch = true;
    startStatusPendingPatch = true;
    lastStartupErrorDisplayPatch = "";
    lastGameErrorDisplayPatch = "";

    // The physical orientation also tells us the software side immediately.
    // Normal board: human is White, software is Black.
    // Flipped board: human is Black, software is White.
    engineSide = flipped ? ENGINE_SIDE_WHITE : ENGINE_SIDE_BLACK;
    setMoveCycle(flipped ? WAIT_ENGINE_MOVE : WAIT_HUMAN_MOVE);

    const char* cmd = flipped ? "set flip board on\n" : "set flip board off\n";
    if (sendCynus(cmd)) {
        Serial.printf("[STARTPOS] set flip board %s; first turn=%s\n",
            flipped ? "ON" : "OFF", flipped ? "SOFTWARE" : "HUMAN");
    } else {
        Serial.printf("[STARTPOS] ERROR sending set flip board %s\n",
            flipped ? "ON" : "OFF");
    }
}

static void processStartOrientationPatch() {
    if (!initialStartupComplete && state == SYNC_BOARD &&
        startOrientationPatch(bufferedFen) == 1) {
        if (!startOrientationLatchedPatch) {
            if (!fen2board(bufferedFen)) return;
            fenNow = bufferedFen;
            initialStartupComplete = true;
            boardSyncPurpose = BOARD_SYNC_NONE;
            boardSyncRequestPending = false;
            boardScanPending = false;
            publishNextFenToChessLink = false;
            correctionFenCandidate = "";
            lastFenSentToChessLink = "";
            configureStartOrientationPatch(true);
            boardSynced = false;
            flippedBootGatePatch = true;
            stopChessLinkAdvertising();
            Serial.println("[STARTPOS] flipped boot position accepted; waiting for Cynus get move before ChessLink");
        }
    }

    if (flippedBootGatePatch && cynusWaitingForMove) {
        flippedBootGatePatch = false;
        boardSynced = true;
        setState(WAIT_CHESSLINK);
        setMoveCycle(WAIT_ENGINE_MOVE);
        cynusDisplay("BT Scan");
        Serial.println("[DISPLAY] BT Scan: flipped board valid and Cynus ready");
        startChessLinkAdvertising();
        Serial.println("[STARTPOS] Cynus ready; ChessLink enabled for software-first opening");
    }

    int acceptedOrientation = startOrientationPatch(fenNow);

    if (acceptedOrientation >= 0) {
        bool flipped = acceptedOrientation == 1;
        if (!startOrientationLatchedPatch || startOrientationFlippedPatch != flipped) {
            configureStartOrientationPatch(flipped);
            lastFenSentToChessLink = "";
            Serial.printf("[NEW GAME] %s initial position recognized\n",
                flipped ? "flipped" : "normal");
        }
    } else if (fenNow.length() && startOrientationLatchedPatch) {
        startOrientationLatchedPatch = false;
        openingStartPhasePatch = false;
        startStatusPendingPatch = false;
        Serial.println("[STARTPOS] initial position left; orientation latch released for next game");
    }

    if (openingStartPhasePatch && state == RUNNING && acceptedOrientation >= 0) {
        if (moveCycle == WAIT_ROBOT_POSITION) {
            openingStartPhasePatch = false;
        } else {
            setMoveCycle(startOrientationFlippedPatch ? WAIT_ENGINE_MOVE : WAIT_HUMAN_MOVE);
        }
    }

    if (startStatusPendingPatch && boardSynced && clConnected && clNotify &&
        (!startOrientationFlippedPatch || cynusWaitingForMove)) {
        clStatusPending = true;
        startStatusPendingPatch = false;
        Serial.println("[STARTPOS] initial board status queued for PicoChess");
    }

    showStartupErrorsPatch();
    showGameRescanErrorsPatch();
}

void setup() {
    cynuslinkCoreSetup();
}

void loop() {
    cynuslinkCoreLoop();
    processStartOrientationPatch();
}
