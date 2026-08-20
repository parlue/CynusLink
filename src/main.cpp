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

enum MoveCycleState { WAIT_FIRST_MOVE, WAIT_HUMAN_MOVE, WAIT_ENGINE_MOVE, WAIT_ROBOT_POSITION };
static MoveCycleState moveCycle = WAIT_HUMAN_MOVE;
static uint32_t moveCycleEnteredAt = 0;
static uint32_t lastMoveWaitWarningAt = 0;

static const char* moveCycleName(MoveCycleState s) {
    switch (s) {
        case WAIT_FIRST_MOVE: return "WAIT_FIRST_MOVE";
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
static uint32_t lastAutoStatusAt = 0;

static char board64[65] = "................................................................";
static String fenNow = "";
static bool boardSynced = false;
static String lastFenSentToChessLink = "";
static String correctionFenCandidate = "";

enum EngineSide { ENGINE_SIDE_UNKNOWN, ENGINE_SIDE_WHITE, ENGINE_SIDE_BLACK };
static EngineSide engineSide = ENGINE_SIDE_UNKNOWN;
static bool firstMoveOrientationLocked = false;
static bool firstMoveFlipOn = false;
static bool analysisEnabled = false;

static constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* FLIPPED_START_FEN = "RNBKQBNR/PPPPPPPP/8/8/8/8/pppppppp/rnbkqbnr";
static constexpr const char* SOUND_OFF_FEN = "rnbq1bnr/pppppppp/8/4k3/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* SOUND_ON_FEN = "rnbq1bnr/pppppppp/4k3/8/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* FLIP_ON_FEN = "rnbq1bnr/pppppppp/8/7k/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* FLIP_OFF_FEN = "rnbq1bnr/pppppppp/7k/8/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* ANALYSIS_ON_FEN = "rnbq1bnr/pppppppp/8/3k4/8/8/PPPPPPPP/RNBQKBNR";
static constexpr const char* ANALYSIS_OFF_FEN = "rnbq1bnr/pppppppp/3k4/8/8/8/PPPPPPPP/RNBQKBNR";

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
static bool startupFreshFenExpected = false;
static bool startupCorrectionMode = false;
static uint32_t boardScanGetFenAt = 0;
static constexpr uint32_t BOARD_SCAN_WAIT_MS = 1000;
static uint32_t lastLinkHealthCheckAt = 0;
static constexpr uint32_t LINK_HEALTH_CHECK_MS = 1000;
static bool btScanDisplayPending = false;
static uint32_t btScanDisplayAt = 0;

static bool cynusWaitingForMove = false;
static bool cynusEngineOffCommandSent = false;
static bool cynusExternalModeConfirmed = false;
static uint32_t engineOffSentAt = 0;
static bool cynusEngineOffSecondSendPending = false;
static bool chessAdvertisingPendingAfterEngineOff = false;
static bool publishNextFenToChessLink = false;
static String bufferedFen = "";

static bool displayPlayPending = false;
static uint32_t displayPlayAt = 0;

static uint8_t ee[256];
static uint8_t led[81];

// Millennium may update L repeatedly while the engine is still calculating.
// Never drive Cynus from the first decodable L frame.  Keep the candidate
// only while the *same* LED state remains stable; any different L frame,
// including LEDs-off, replaces/cancels it.
static String pendingLedMove = "";
static String pendingLedFrame = "";
static uint32_t pendingLedMoveSince = 0;
static constexpr uint32_t LED_MOVE_STABLE_MS = 2500;

static bool startOrientationLatched = false;
static bool startOrientationFlipped = false;
static bool openingStartPhase = false;
static bool startStatusPending = false;
static String lastStartupErrorDisplay = "";
static String lastStartupFenEvaluated = "";
static String lastGameErrorDisplay = "";

static void startCynusScan();
static bool connectCynus();
static bool sendCynus(const char* s);
static void cynusDisplay(const char* text);
static void schedulePlayDisplay(uint32_t delayMs = 1800);
static String moveDisplayText(const String& uci);
static String inferMoveDisplayText(const String& oldFen, const String& newFen);
static void createChessLinkServer();
static void startChessLinkAdvertising();
static void stopChessLinkAdvertising();
static void sendCL(const String& payload);
static void sendStatus();
static void processCL();
static void handleCL(const String& frame);
static void processPendingLedMove();
static void setState(GatewayState s);
static void requestBoardSync(BoardSyncPurpose purpose, uint32_t delayMs = 0);
static void recoverChessLinkLoss(const char* source);
static void recoverCynusLoss(const char* source);
static void processSupervision();
static bool lockFirstMoveOrientation(bool computerFirst);
static void processStartOrientation();

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

static String squareName(int file, int rankTop) {
    String s;
    s += (char)('a' + file);
    s += (char)('8' - rankTop);
    return s;
}

static String startupSquare(int index) {
    String s;
    s += (char)('A' + (index % 8));
    s += (char)('8' - (index / 8));
    return s;
}

static int orientationOfFen(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);
    if (f == START_FEN) return 0;
    if (f == FLIPPED_START_FEN) return 1;
    return -1;
}

static int mismatchCount(const char actual[65], const char expected[65]) {
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        bool a = actual[i] != '.';
        bool e = expected[i] != '.';
        if (a != e || (a && e && actual[i] != expected[i])) ++n;
    }
    return n;
}

static String differenceDisplay(String actualFen, String expectedFen) {
    char actual[65], expected[65];
    if (!fenPlacementTo64(actualFen, actual) || !fenPlacementTo64(expectedFen, expected)) return "";
    String issues[2];
    int count = 0;
    for (int i = 0; i < 64 && count < 2; ++i) {
        bool a = actual[i] != '.';
        bool e = expected[i] != '.';
        if (!a && e) issues[count++] = "-" + startupSquare(i);
        else if (a && !e) issues[count++] = "+" + startupSquare(i);
        else if (a && e && actual[i] != expected[i]) issues[count++] = "+" + startupSquare(i);
    }
    if (!count) return "";
    if (count == 1) return issues[0];
    return issues[0] + "/" + issues[1];
}

static String startupErrorDisplay(String f) {
    char actual[65], normal[65], flipped[65];
    if (!fenPlacementTo64(f, actual) || !fenPlacementTo64(START_FEN, normal) || !fenPlacementTo64(FLIPPED_START_FEN, flipped)) return "";
    const char* expected = mismatchCount(actual, flipped) < mismatchCount(actual, normal) ? flipped : normal;
    String issues[2];
    int count = 0;
    for (int i = 0; i < 64 && count < 2; ++i) {
        bool a = actual[i] != '.';
        bool e = expected[i] != '.';
        if (!a && e) issues[count++] = "-" + startupSquare(i);
        else if (a && !e) issues[count++] = "+" + startupSquare(i);
        else if (a && e && actual[i] != expected[i]) issues[count++] = "+" + startupSquare(i);
    }
    if (!count) return "";
    if (count == 1) return issues[0];
    return issues[0] + "/" + issues[1];
}

static void showStartupErrors() {
    if (initialStartupComplete || state != SYNC_BOARD || bufferedFen.length() == 0) return;
    if (bufferedFen == lastStartupFenEvaluated) return;
    lastStartupFenEvaluated = bufferedFen;
    if (orientationOfFen(bufferedFen) >= 0) {
        lastStartupErrorDisplay = "";
        return;
    }
    String msg = startupErrorDisplay(bufferedFen);
    if (!msg.length()) return;
    lastStartupErrorDisplay = msg;
    cynusDisplay(msg.c_str());
    sendCynus("play audio error\n");
    Serial.printf("[STARTUP] position error display: %s (error audio)\n", msg.c_str());
}

static bool handlePieceConfigFen(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);

    bool flipOn = f == FLIP_ON_FEN;
    bool flipOff = f == FLIP_OFF_FEN;
    bool analysisOn = f == ANALYSIS_ON_FEN;
    bool analysisOff = f == ANALYSIS_OFF_FEN;
    if (!flipOn && !flipOff && !analysisOn && !analysisOff) return false;

    publishNextFenToChessLink = false;
    correctionFenCandidate = "";
    boardSynced = fenNow.length() > 0;
    cynusWaitingForMove = true;
    displayPlayPending = false;

    if (flipOn || flipOff) {
        const char* command = flipOn ? "set flip board on\n" : "set flip board off\n";
        if (!sendCynus(command)) return true;
        Serial.printf("[CONFIG] manual flip %s selected with black king; config FEN suppressed from ChessLink\n", flipOn ? "ON" : "OFF");
        return true;
    }

    analysisEnabled = analysisOn;
    cynusDisplay(analysisEnabled ? "Analyse" : "play");
    Serial.printf("[CONFIG] analysis %s selected with black king; config FEN suppressed from ChessLink\n", analysisEnabled ? "ON" : "OFF");
    return true;
}

static bool handleSoundConfigFen(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);
    bool soundOn = f == SOUND_ON_FEN;
    bool soundOff = f == SOUND_OFF_FEN;
    if (!soundOn && !soundOff) return false;
    const char* command = soundOn ? "sound 70\n" : "sound 0\n";
    if (!sendCynus(command)) return true;
    publishNextFenToChessLink = false;
    correctionFenCandidate = "";
    boardSynced = fenNow.length() > 0;
    cynusWaitingForMove = true;
    setMoveCycle(WAIT_HUMAN_MOVE);
    displayPlayPending = false;
    cynusDisplay(soundOn ? "snd on" : "snd off");
    schedulePlayDisplay(3500);
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
    if (bytes.size() <= maxPayload) {
        clTx->setValue(bytes.data(), bytes.size());
        clTx->notify();
        return;
    }
    for (size_t offset = 0; offset < bytes.size(); offset += maxPayload) {
        size_t count = std::min(maxPayload, bytes.size() - offset);
        clTx->setValue(bytes.data() + offset, count);
        clTx->notify();
        delay(8);
    }
}

static void sendStatus() {
    if (!boardSynced) return;
    String p = "s";
    for (int i = 0; i < 64; ++i) p += board64[i];
    sendCL(p);
    lastFenSentToChessLink = fenNow;
}

static uint8_t autoReportMode() { return ee[2] & 7; }

static uint32_t autoReportIntervalMs() {
    uint8_t mode = autoReportMode();
    if (mode == 0) {
        uint32_t scan = ((uint32_t)ee[1] * 2048UL + 999UL) / 1000UL;
        return scan ? scan : 41;
    }
    if (mode == 2) {
        uint32_t timed = ((uint32_t)ee[3] * 4096UL + 999UL) / 1000UL;
        return timed ? timed : 4;
    }
    return 0;
}

static bool autoReport() { return autoReportMode() != 1; }

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        clConnected = true;
        clConnHandle = info.getConnHandle();
        lastAutoStatusAt = 0;
        Serial.printf("[CHESS] connected %s, MTU=%u\n", info.getAddress().toString().c_str(), info.getMTU());
        if (!chessAdvertisingAllowed || !cynusReady || state != WAIT_CHESSLINK) {
            chessRejectEventPending = true;
            return;
        }
        if (clServer) clServer->updateConnParams(info.getConnHandle(), 12, 24, 0, 200);
        chessConnectEventPending = true;
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("[CHESS] disconnected %d\n", reason);
        clConnected = false;
        clNotify = false;
        clConnHandle = BLE_HS_CONN_HANDLE_NONE;
        lastAutoStatusAt = 0;
        chessDisconnectEventPending = true;
    }
};
static ServerCB serverCB;

class TxCB : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t value) override {
        clNotify = value != 0;
        if (clNotify) lastAutoStatusAt = 0;
    }
};
static TxCB txCB;

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        std::string value = characteristic->getValue();
        for (uint8_t b : value) {
            char a = (char)(b & 0x7F);
            if (a >= 0x20 && a <= 0x7E) clBuf += a;
        }
        if (clBuf.length() > 512) clBuf = "";
        clProcessPending = true;
    }
};
static RxCB rxCB;

static uint8_t ledValue(int fileCorner, int rankCornerTop) {
    if (fileCorner < 0 || fileCorner > 8 || rankCornerTop < 0 || rankCornerTop > 8) return 0;
    return led[fileCorner * 9 + rankCornerTop];
}

static uint8_t dominantSquarePattern(int file, int rankCornerTop) {
    const uint8_t corners[4] = { ledValue(file, rankCornerTop), ledValue(file + 1, rankCornerTop), ledValue(file, rankCornerTop + 1), ledValue(file + 1, rankCornerTop + 1) };
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
        String t = "Chg "; t += (char)toupper((unsigned char)uci[4]); return t;
    }
    if (uci.length() >= 4) {
        String t; t += (char)toupper((unsigned char)uci[0]); t += uci[1]; t += '-'; t += (char)toupper((unsigned char)uci[2]); t += uci[3]; return t;
    }
    return "";
}

static String inferMoveDisplayText(const String& oldFen, const String& newFen) {
    if (!oldFen.length() || !newFen.length()) return "";
    char oldB[65], newB[65];
    if (!fenPlacementTo64(oldFen, oldB) || !fenPlacementTo64(newFen, newB)) return "";
    int source = -1, destination = -1;
    for (int i = 0; i < 64; ++i) if (oldB[i] != newB[i] && oldB[i] != '.' && newB[i] == '.') { source = i; break; }
    if (source < 0) return "";
    char mover = oldB[source];
    bool white = mover >= 'A' && mover <= 'Z';
    for (int i = 0; i < 64; ++i) {
        if (oldB[i] == newB[i] || newB[i] == '.') continue;
        bool sameSide = white ? (newB[i] >= 'A' && newB[i] <= 'Z') : (newB[i] >= 'a' && newB[i] <= 'z');
        if (sameSide && i != source) { destination = i; break; }
    }
    if (destination < 0) return "";
    String uci = squareName(source % 8, source / 8) + squareName(destination % 8, destination / 8);
    return moveDisplayText(uci);
}

static bool plausibleBoardMove(int source, int destination, EngineSide requiredSide) {
    if (source < 0 || source >= 64 || destination < 0 || destination >= 64 || source == destination) return false;
    char piece = board64[source];
    char target = board64[destination];
    EngineSide side = (piece >= 'A' && piece <= 'Z') ? ENGINE_SIDE_WHITE : ((piece >= 'a' && piece <= 'z') ? ENGINE_SIDE_BLACK : ENGINE_SIDE_UNKNOWN);
    if (side == ENGINE_SIDE_UNKNOWN || (requiredSide != ENGINE_SIDE_UNKNOWN && side != requiredSide)) return false;
    if (target != '.') {
        EngineSide targetSide = (target >= 'A' && target <= 'Z') ? ENGINE_SIDE_WHITE : ((target >= 'a' && target <= 'z') ? ENGINE_SIDE_BLACK : ENGINE_SIDE_UNKNOWN);
        if (targetSide == side) return false;
    }
    int sf = source % 8, sr = source / 8, df = destination % 8, dr = destination / 8;
    int dx = df - sf, dy = dr - sr, ax = abs(dx), ay = abs(dy);
    auto clearPath = [&](int stepX, int stepY) {
        int x = sf + stepX, y = sr + stepY;
        while (x != df || y != dr) {
            if (x < 0 || x > 7 || y < 0 || y > 7 || board64[y * 8 + x] != '.') return false;
            x += stepX; y += stepY;
        }
        return true;
    };
    char p = (char)tolower((unsigned char)piece);
    if (p == 'n') return (ax == 1 && ay == 2) || (ax == 2 && ay == 1);
    if (p == 'k') return (ax <= 1 && ay <= 1) || (ay == 0 && ax == 2);
    if (p == 'b') return ax == ay && ax > 0 && clearPath(dx > 0 ? 1 : -1, dy > 0 ? 1 : -1);
    if (p == 'r') {
        if (dx != 0 && dy != 0) return false;
        return clearPath(dx == 0 ? 0 : (dx > 0 ? 1 : -1), dy == 0 ? 0 : (dy > 0 ? 1 : -1));
    }
    if (p == 'q') {
        if (ax == ay && ax > 0) return clearPath(dx > 0 ? 1 : -1, dy > 0 ? 1 : -1);
        if (dx == 0 || dy == 0) return clearPath(dx == 0 ? 0 : (dx > 0 ? 1 : -1), dy == 0 ? 0 : (dy > 0 ? 1 : -1));
        return false;
    }
    if (p == 'p') {
        int dir = side == ENGINE_SIDE_WHITE ? -1 : 1;
        int startRank = side == ENGINE_SIDE_WHITE ? 6 : 1;
        if (dx == 0 && target == '.') {
            if (dy == dir) return true;
            if (sr == startRank && dy == 2 * dir && board64[(sr + dir) * 8 + sf] == '.') return true;
            return false;
        }
        if (ax == 1 && dy == dir && target != '.') return true;
        return false;
    }
    return false;
}

static bool extractTimeslotDiagnosticMove(String& uci, int& plyOut) {
    if (engineSide == ENGINE_SIDE_UNKNOWN) return false;
    for (int ply = 0; ply < 4; ++ply) {
        const uint8_t sourceMask = (uint8_t)(1U << (7 - ply * 2));
        const uint8_t destinationMask = (uint8_t)(1U << (6 - ply * 2));
        int bestSource = -1, bestDestination = -1, candidates = 0;
        for (int source = 0; source < 64; ++source) {
            char piece = board64[source];
            EngineSide side = (piece >= 'A' && piece <= 'Z') ? ENGINE_SIDE_WHITE : ((piece >= 'a' && piece <= 'z') ? ENGINE_SIDE_BLACK : ENGINE_SIDE_UNKNOWN);
            if (side != engineSide) continue;
            int sf = source % 8, sr = source / 8;
            const uint8_t sc[4] = { ledValue(sf, sr), ledValue(sf + 1, sr), ledValue(sf, sr + 1), ledValue(sf + 1, sr + 1) };
            bool sourceMarked = true;
            for (uint8_t v : sc) if ((v & sourceMask) == 0) { sourceMarked = false; break; }
            if (!sourceMarked) continue;
            for (int destination = 0; destination < 64; ++destination) {
                if (!plausibleBoardMove(source, destination, engineSide)) continue;
                int df = destination % 8, dr = destination / 8;
                const uint8_t dc[4] = { ledValue(df, dr), ledValue(df + 1, dr), ledValue(df, dr + 1), ledValue(df + 1, dr + 1) };
                bool destinationMarked = true;
                for (uint8_t v : dc) if ((v & destinationMask) == 0) { destinationMarked = false; break; }
                if (!destinationMarked) continue;
                bestSource = source;
                bestDestination = destination;
                ++candidates;
            }
        }
        if (candidates == 1) {
            uci = squareName(bestSource % 8, bestSource / 8) + squareName(bestDestination % 8, bestDestination / 8);
            plyOut = ply;
            return true;
        }
        if (candidates > 1) Serial.printf("[CHESS LED DIAG] timeslot ply=%d ambiguous candidates=%d\n", ply, candidates);
    }
    return false;
}

static bool extractPhasedMoveFromLCommand(String& uci) {
    if (engineSide == ENGINE_SIDE_UNKNOWN) return false;
    int bestSource = -1, bestDestination = -1, bestScore = -999, bestCount = 0;
    for (int source = 0; source < 64; ++source) {
        char piece = board64[source];
        EngineSide sourceSide = (piece >= 'A' && piece <= 'Z') ? ENGINE_SIDE_WHITE : ((piece >= 'a' && piece <= 'z') ? ENGINE_SIDE_BLACK : ENGINE_SIDE_UNKNOWN);
        if (sourceSide != engineSide) continue;
        int sf = source % 8, sr = source / 8;
        const uint8_t sourceCorners[4] = { ledValue(sf, sr), ledValue(sf + 1, sr), ledValue(sf, sr + 1), ledValue(sf + 1, sr + 1) };
        int sourceLow = 0, sourceHigh = 0;
        for (uint8_t v : sourceCorners) {
            if (v & 0x0F) ++sourceLow;
            if (v & 0xF0) ++sourceHigh;
        }
        if (sourceLow != 4 || sourceHigh != 0) continue;

        for (int destination = 0; destination < 64; ++destination) {
            if (!plausibleBoardMove(source, destination, engineSide)) continue;
            int df = destination % 8, dr = destination / 8;
            if ((char)tolower((unsigned char)piece) == 'k' && sr == dr && abs(df - sf) == 2) continue;
            const uint8_t destinationCorners[4] = { ledValue(df, dr), ledValue(df + 1, dr), ledValue(df, dr + 1), ledValue(df + 1, dr + 1) };
            int destinationLow = 0, destinationHigh = 0;
            for (uint8_t v : destinationCorners) {
                if (v & 0x0F) ++destinationLow;
                if (v & 0xF0) ++destinationHigh;
            }
            if (destinationHigh < 2) continue;
            int score = destinationHigh * 4 - destinationLow;
            if (score > bestScore) {
                bestScore = score;
                bestSource = source;
                bestDestination = destination;
                bestCount = 1;
            } else if (score == bestScore) {
                ++bestCount;
            }
        }
    }
    if (bestSource < 0 || bestCount != 1) {
        if (bestSource >= 0) Serial.printf("[CHESS] phased LED pattern ambiguous: best score=%d candidates=%d\n", bestScore, bestCount);
        return false;
    }
    uci = squareName(bestSource % 8, bestSource / 8) + squareName(bestDestination % 8, bestDestination / 8);
    Serial.printf("[CHESS] phased LED fallback selected %s (score=%d)\n", uci.c_str(), bestScore);
    return true;
}

static bool extractMoveFromLCommand(String& uci) {
    bool observed[81]; int observedCount = 0;
    for (int i = 0; i < 81; ++i) { observed[i] = led[i] != 0; if (observed[i]) ++observedCount; }
    if (observedCount < 6) return false;
    if (observedCount > 8) return extractPhasedMoveFromLCommand(uci);
    auto addSquareCorners = [](bool mask[81], int file, int rankTop) {
        mask[file * 9 + rankTop] = true; mask[(file + 1) * 9 + rankTop] = true; mask[file * 9 + rankTop + 1] = true; mask[(file + 1) * 9 + rankTop + 1] = true;
    };
    struct EndpointPair { int a; int b; };
    EndpointPair pairs[16]; int pairCount = 0;
    for (int a = 0; a < 64; ++a) for (int b = a + 1; b < 64; ++b) {
        bool expected[81] = {false}; addSquareCorners(expected, a % 8, a / 8); addSquareCorners(expected, b % 8, b / 8);
        bool same = true; for (int i = 0; i < 81; ++i) if (expected[i] != observed[i]) { same = false; break; }
        if (same && pairCount < 16) pairs[pairCount++] = {a, b};
    }
    if (pairCount != 1) return false;
    int a = pairs[0].a, b = pairs[0].b;
    char pieceA = board64[a], pieceB = board64[b]; bool occA = pieceA != '.', occB = pieceB != '.';
    auto sideOfPiece = [](char p) -> EngineSide { if (p >= 'A' && p <= 'Z') return ENGINE_SIDE_WHITE; if (p >= 'a' && p <= 'z') return ENGINE_SIDE_BLACK; return ENGINE_SIDE_UNKNOWN; };
    int source = -1, destination = -1;
    if (occA != occB) {
        source = occA ? a : b;
        destination = occA ? b : a;
        EngineSide sourceSide = sideOfPiece(board64[source]);
        if (sourceSide == ENGINE_SIDE_UNKNOWN) return false;
        if (engineSide != ENGINE_SIDE_UNKNOWN && sourceSide != engineSide) {
            Serial.printf("[CHESS] ignoring LED pattern %s-%s: source side does not match engine side\n",
                          squareName(source % 8, source / 8).c_str(),
                          squareName(destination % 8, destination / 8).c_str());
            return false;
        }
        if (engineSide == ENGINE_SIDE_UNKNOWN) engineSide = sourceSide;
    }
    else if (occA && occB) {
        if (engineSide == ENGINE_SIDE_UNKNOWN) {
            uint8_t patA = dominantSquarePattern(a % 8, a / 8), patB = dominantSquarePattern(b % 8, b / 8);
            if (patA == 0x33 && patB == 0xCC) { source = a; destination = b; engineSide = sideOfPiece(pieceA); }
            else if (patA == 0xCC && patB == 0x33) { source = b; destination = a; engineSide = sideOfPiece(pieceB); }
            else return false;
        } else {
            bool aIsEngine = sideOfPiece(pieceA) == engineSide, bIsEngine = sideOfPiece(pieceB) == engineSide;
            if (aIsEngine == bIsEngine) return false;
            source = aIsEngine ? a : b; destination = aIsEngine ? b : a;
        }
    } else return false;
    if (!plausibleBoardMove(source, destination, engineSide)) {
        Serial.printf("[CHESS] ignoring LED pattern %s-%s: not a plausible move for current board\n",
                      squareName(source % 8, source / 8).c_str(),
                      squareName(destination % 8, destination / 8).c_str());
        return false;
    }
    uci = squareName(source % 8, source / 8) + squareName(destination % 8, destination / 8);
    return true;
}

static void clearPendingLedMove(const char* reason) {
    if (pendingLedMove.length() && reason) {
        Serial.printf("[CHESS LED] pending %s cancelled: %s\n", pendingLedMove.c_str(), reason);
    }
    pendingLedMove = "";
    pendingLedFrame = "";
    pendingLedMoveSince = 0;
}

static void armPendingLedMove(const String& uci, const String& frame) {
    if (pendingLedMove == uci && pendingLedFrame == frame) return;
    pendingLedMove = uci;
    pendingLedFrame = frame;
    pendingLedMoveSince = millis();
    Serial.printf("[CHESS LED] candidate %s armed; waiting for stable final LED state\n", uci.c_str());
}

static bool lockFirstMoveOrientation(bool computerFirst) {
    if (firstMoveOrientationLocked) return true;
    const bool flipOn = computerFirst;
    const char* cmd = flipOn ? "set flip board on\n" : "set flip board off\n";
    if (!sendCynus(cmd)) {
        Serial.printf("[FIRST MOVE] set flip board %s FAILED; first move remains gated\n", flipOn ? "ON" : "OFF");
        return false;
    }
    firstMoveOrientationLocked = true;
    firstMoveFlipOn = flipOn;
    engineSide = computerFirst ? ENGINE_SIDE_WHITE : ENGINE_SIDE_BLACK;
    Serial.printf("[FIRST MOVE] %s moved first -> flip board %s; orientation locked for game\n",
                  computerFirst ? "Chess computer" : "Cynus board",
                  flipOn ? "ON" : "OFF");
    return true;
}

static void processPendingLedMove() {
    if (!pendingLedMove.length()) return;
    const bool firstComputerMove = moveCycle == WAIT_FIRST_MOVE && !firstMoveOrientationLocked;
    if (state != RUNNING || (moveCycle != WAIT_ENGINE_MOVE && moveCycle != WAIT_FIRST_MOVE) ||
        (!cynusWaitingForMove && !firstComputerMove) || !cynusReady || !clConnected) {
        clearPendingLedMove("gateway state changed");
        return;
    }
    if ((uint32_t)(millis() - pendingLedMoveSince) < LED_MOVE_STABLE_MS) return;

    if (firstComputerMove && !lockFirstMoveOrientation(true)) {
        pendingLedMoveSince = millis();
        return;
    }

    String uci = pendingLedMove;
    clearPendingLedMove(nullptr);
    String displayMove = moveDisplayText(uci);
    if (displayMove.length()) cynusDisplay(displayMove.c_str());
    String cmd = "move " + uci + "\n";
    if (sendCynus(cmd.c_str())) {
        Serial.printf("[CHESS LED] stable final candidate accepted: %s\n", uci.c_str());
        cynusWaitingForMove = false;
        setMoveCycle(WAIT_ROBOT_POSITION);
    }
}

static void handleCL(const String& f) {
    if (!valid(f)) return;
    switch (f[0]) {
        case 'S': sendStatus(); break;
        case 'V': sendCL("v0100"); break;
        case 'X': memset(led, 0, sizeof(led)); sendCL("x"); break;
        case 'T':
            memset(led, 0, sizeof(led));
            memset(ee, 0, sizeof(ee)); ee[0]=0x00; ee[1]=0x14; ee[2]=0x00; ee[4]=0x0F;
            lastAutoStatusAt=0;
            Serial.println("[CHESS] Magic Board reset command received; protocol defaults restored");
            break;
        case 'R': { uint8_t a; if (hb(f[1], f[2], a)) sendCL("r" + hx(a) + hx(ee[a])); break; }
        case 'W': {
            uint8_t a,d;
            if (hb(f[1],f[2],a) && hb(f[3],f[4],d)) {
                ee[a]=d;
                if (a==1 || a==2 || a==3) lastAutoStatusAt=0;
                sendCL("w"+hx(a)+hx(d));
            }
            break;
        }
        case 'L': {
            if (state != RUNNING || (moveCycle != WAIT_ENGINE_MOVE && moveCycle != WAIT_FIRST_MOVE)) { clearPendingLedMove("L outside engine wait"); sendCL("l"); break; }
            uint8_t slot; if (!hb(f[1], f[2], slot)) break;
            bool ok = true; for (int i=0;i<81;++i) if (!hb(f[3+i*2],f[4+i*2],led[i])) {ok=false;break;}
            if (!ok) break; sendCL("l");

            String frameKey = f.substring(0, f.length() - 2);
            if (pendingLedMove.length() && pendingLedFrame != frameKey) {
                clearPendingLedMove("LED state changed before stabilization");
            }

            String timeslotUci; int timeslotPly = -1;
            bool timeslotOk = extractTimeslotDiagnosticMove(timeslotUci, timeslotPly);
            String uci; bool legacyOk = extractMoveFromLCommand(uci);
            if (timeslotOk || legacyOk) {
                Serial.printf("[CHESS LED DIAG] legacy=%s timeslot=%s ply=%d\n",
                              legacyOk ? uci.c_str() : "-",
                              timeslotOk ? timeslotUci.c_str() : "-",
                              timeslotOk ? timeslotPly : -1);
            }
            if (legacyOk && (cynusWaitingForMove || moveCycle == WAIT_FIRST_MOVE)) {
                armPendingLedMove(uci, frameKey);
            } else if (pendingLedMove.length() && pendingLedFrame == frameKey) {
                clearPendingLedMove("current LED state no longer yields a move candidate");
            }
            break;
        }
    }
}

static void processCL() {
    while (clBuf.length()) {
        int n = flen(clBuf[0]);
        if (n < 0) { clBuf.remove(0,1); continue; }
        if ((int)clBuf.length() < n) return;
        String f=clBuf.substring(0,n); clBuf.remove(0,n);
        Serial.printf("[CHESS RX] %s\n", f.c_str());
        handleCL(f);
    }
}

static void createChessLinkServer() {
    if (clServerStarted) return;
    clServer=NimBLEDevice::createServer(); clServer->setCallbacks(&serverCB);
    NimBLEService* service=clServer->createService(CL_SERVICE);
    clTx=service->createCharacteristic(CL_TX,NIMBLE_PROPERTY::READ|NIMBLE_PROPERTY::NOTIFY);
    clRx=service->createCharacteristic(CL_RX,NIMBLE_PROPERTY::WRITE|NIMBLE_PROPERTY::WRITE_NR);
    clTx->setCallbacks(&txCB); clRx->setCallbacks(&rxCB);
    if (!clServer->start()) return;
    NimBLEAdvertising* advertising=NimBLEDevice::getAdvertising(); advertising->setName(CL_NAME); advertising->addServiceUUID(CL_SERVICE); advertising->enableScanResponse(true);
    clServerStarted=true; Serial.println("[CHESS] server created, advertising still OFF");
}

static void startChessLinkAdvertising() {
    if (!clServerStarted || !cynusReady || !boardSynced || state != WAIT_CHESSLINK) return;
    chessAdvertisingAllowed=true;
    if (NimBLEDevice::startAdvertising()) Serial.println("[CHESS] advertising as MILLENNIUM CHESS"); else chessAdvertisingAllowed=false;
}

static void cynusBytes(const uint8_t* data, size_t len) {
    for (size_t i=0;i<len;++i) {
        char c=(char)data[i];
        if (c=='\n') {
            String line=cynusLine; cynusLine=""; line.trim(); Serial.printf("[CYNUS LINE] %s\n",line.c_str());

            if (line.startsWith("promotions:") && state==SYNC_BOARD && cynusReady && boardSyncPurpose==BOARD_SYNC_STARTUP && startupCorrectionMode && !startupFreshFenExpected) {
                startupFreshFenExpected=true;
                boardSyncRequestPending=true;
                boardSyncRequestAt=millis()+250;
                Serial.println("[STARTUP] manual correction scan detected; correction API scan queued");
                continue;
            }

            if (line.equalsIgnoreCase("get move")) {
                if (!(cynusReady && clConnected)) {
                    Serial.println("[GATEWAY] get move ignored until ChessLink is connected");
                    continue;
                }

                bool firstExternalReady=!cynusExternalModeConfirmed;
                cynusWaitingForMove=true;
                cynusExternalModeConfirmed=true;
                setState(RUNNING);
                if (firstExternalReady) { displayPlayPending=false; cynusDisplay("play"); }

                if (moveCycle==WAIT_HUMAN_MOVE || moveCycle==WAIT_FIRST_MOVE) {
                    boardSynced=false;
                    publishNextFenToChessLink=true;
                    sendCynus("get fen\n");
                } else if (moveCycle==WAIT_ROBOT_POSITION) {
                    boardSynced=false;
                    publishNextFenToChessLink=false;
                    cynusDisplay("play");
                    sendCynus("get fen\n");
                } else {
                    if (correctionFenCandidate.length() && correctionFenCandidate != lastFenSentToChessLink && fen2board(correctionFenCandidate)) {
                        fenNow=correctionFenCandidate;
                        boardSynced=true;
                        if (autoReport()) clStatusPending=true;
                    }
                    correctionFenCandidate="";
                }
                continue;
            }

            if (line.startsWith("fen:")) {
                String f=line.substring(4); f.trim();
                if (state==SYNC_BOARD && cynusReady && boardSyncPurpose==BOARD_SYNC_STARTUP && startupCorrectionMode && boardSyncRequestPending) {
                    boardSyncRequestPending=false;
                    Serial.println("[STARTUP] manual correction FEN received; queued fallback scan cancelled");
                }
                if (initialStartupComplete && state==RUNNING && clConnected &&
                    (moveCycle==WAIT_HUMAN_MOVE || moveCycle==WAIT_FIRST_MOVE) && handlePieceConfigFen(f)) continue;
                if (initialStartupComplete && state==RUNNING && clConnected && moveCycle==WAIT_HUMAN_MOVE && handleSoundConfigFen(f)) continue;

                if (state==SYNC_BOARD && cynusReady && boardSyncPurpose==BOARD_SYNC_STARTUP && !startupFreshFenExpected && !startupCorrectionMode) {
                    Serial.printf("[STARTUP] ignoring pre-scan FEN: %s\n", f.c_str());
                    continue;
                }

                if (fen2board(f)) {
                    bufferedFen=f;
                    Serial.printf("[BOARD] buffered FEN %s\n",bufferedFen.c_str());

                    if (state==SYNC_BOARD && cynusReady && boardSyncPurpose!=BOARD_SYNC_NONE) {
                        String placement=bufferedFen;
                        int sp=placement.indexOf(' ');
                        if (sp>=0) placement=placement.substring(0,sp);
                        int orientation=orientationOfFen(placement);

                        if (boardSyncPurpose==BOARD_SYNC_STARTUP) {
                            if (orientation<0) {
                                startupFreshFenExpected=false;
                                startupCorrectionMode=true;
                                Serial.printf("[STARTUP] board not ready: %s\n",bufferedFen.c_str());
                                Serial.println("[STARTUP] waiting for initial position; correct board and press Cynus scan");
                                continue;
                            }

                            boardSyncRequestPending=false;
                            boardScanPending=false;
                            startupFreshFenExpected=false;
                            startupCorrectionMode=false;
                            fenNow=bufferedFen;
                            boardSynced=true;
                            boardSyncPurpose=BOARD_SYNC_NONE;
                            initialStartupComplete=true;
                            startStatusPending=true;
                            lastStartupErrorDisplay="";
                            lastStartupFenEvaluated="";
                            lastGameErrorDisplay="";
                            cynusDisplay("POS OK");
                            Serial.println("[DISPLAY] POS OK");
                            Serial.println("[STARTUP] valid initial position confirmed; flip deferred until first move source is known");

                            if (!clConnected) {
                                setState(WAIT_CHESSLINK);
                                btScanDisplayPending=true;
                                btScanDisplayAt=millis()+2000;
                                Serial.println("[DISPLAY] POS OK: waiting 2 seconds before BT Scan");
                            } else {
                                engineSide = ENGINE_SIDE_UNKNOWN;
                                setState(RUNNING);
                                setMoveCycle(WAIT_FIRST_MOVE);
                            }
                            continue;
                        }

                        fenNow=bufferedFen;
                        boardSynced=true;
                        boardSyncPurpose=BOARD_SYNC_NONE;
                        if (!clConnected) {
                            setState(WAIT_CHESSLINK);
                            cynusDisplay("BT Scan");
                            startChessLinkAdvertising();
                        } else {
                            setState(RUNNING);
                            setMoveCycle(firstMoveOrientationLocked ? WAIT_HUMAN_MOVE : WAIT_FIRST_MOVE);
                        }
                        continue;
                    }

                    if (state==RUNNING && clConnected && publishNextFenToChessLink) {
                        if (moveCycle==WAIT_FIRST_MOVE && !firstMoveOrientationLocked) {
                            if (!lockFirstMoveOrientation(false)) {
                                Serial.println("[FIRST MOVE] human move held; flip OFF must succeed before forwarding to ChessLink");
                                continue;
                            }
                        }
                        String previousAcceptedFen=fenNow;
                        fenNow=bufferedFen;
                        boardSynced=true;
                        publishNextFenToChessLink=false;
                        String humanDisplayMove=inferMoveDisplayText(previousAcceptedFen,fenNow);
                        if (humanDisplayMove.length()) { cynusDisplay(humanDisplayMove.c_str()); schedulePlayDisplay(1400); }
                        if (autoReport()) clStatusPending=true;
                        setMoveCycle(WAIT_ENGINE_MOVE);
                        continue;
                    }

                    if (state==RUNNING && clConnected && moveCycle==WAIT_ENGINE_MOVE) {
                        correctionFenCandidate=bufferedFen;
                        continue;
                    }

                    if (state==RUNNING && moveCycle==WAIT_ROBOT_POSITION) {
                        fenNow=bufferedFen;
                        boardSynced=true;
                        setMoveCycle(WAIT_HUMAN_MOVE);
                        continue;
                    }

                    Serial.println("[BOARD] camera/intermediate FEN buffered only; not sent to ChessLink");
                }
            }
        } else if (c!='\r') cynusLine+=c;
    }
}

static void cynusNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    Serial.print("[CYNUS RX] ");
    for (size_t i=0;i<len;++i) {
        char c=(char)data[i];
        if (c=='\r') Serial.print("\\r");
        else if (c=='\n') Serial.print("\\n");
        else if (c>=32&&c<=126) Serial.print(c);
    }
    Serial.println();
    cynusBytes(data,len);
}

class CClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int reason) override {
        Serial.printf("[CYNUS] disconnected %d\n",reason);
        cynusReady=false;
        cynusChr=nullptr;
        cynusDisconnectEventPending=true;
    }
};
static CClientCB cclientCB;

class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveName()) return;
        std::string name=dev->getName();
        if (name.rfind(CYNUS_PREFIX,0)!=0) return;
        Serial.printf("[CYNUS] found %s\n",name.c_str());
        NimBLEDevice::getScan()->stop();
        cynusDev=dev;
        cynusConnectPending=true;
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
    NimBLEScan* scan=NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCB,false); scan->setInterval(100); scan->setWindow(100); scan->setActiveScan(true);
    Serial.println("[CYNUS] scan CYNUS-*"); scan->start(0);
}

static bool sendCynus(const char* s) {
    if (!cynusReady || !cynusChr) { Serial.println("[CYNUS TX] not ready"); return false; }
    Serial.printf("[CYNUS TX] %s",s);
    if (cynusChr->canWriteNoResponse()) return cynusChr->writeValue((const uint8_t*)s,strlen(s),false);
    if (cynusChr->canWrite()) return cynusChr->writeValue((const uint8_t*)s,strlen(s),true);
    return false;
}

static void cynusDisplay(const char* text) {
    if (!cynusReady || !cynusChr || !text) return;
    String msg=text; if (msg.length()>7) msg=msg.substring(0,7); String cmd="display txt "+msg+"\n";
    if (sendCynus(cmd.c_str())) Serial.printf("[DISPLAY] %s\n",msg.c_str());
}

static void schedulePlayDisplay(uint32_t delayMs) { displayPlayPending=true; displayPlayAt=millis()+delayMs; }

static bool connectCynus() {
    if (!cynusDev) return false;
    if (!cynusClient) { cynusClient=NimBLEDevice::createClient(); cynusClient->setClientCallbacks(&cclientCB,false); cynusClient->setConnectTimeout(10000); }
    if (!cynusClient->connect(cynusDev)) return false;
    NimBLERemoteService* service=cynusClient->getService(CYNUS_SERVICE); if (!service) { cynusClient->disconnect(); return false; }
    cynusChr=service->getCharacteristic(CYNUS_CHAR);
    if (!cynusChr || !cynusChr->canNotify() || !cynusChr->subscribe(true,cynusNotify)) { cynusClient->disconnect(); return false; }
    cynusReady=true; Serial.printf("[CYNUS] connected %s\n",cynusDev->getName().c_str());
    cynusExternalModeConfirmed=false; cynusEngineOffCommandSent=false; cynusEngineOffSecondSendPending=false; chessAdvertisingPendingAfterEngineOff=false;
    boardSyncPurpose=BOARD_SYNC_NONE; boardSyncRequestPending=false; boardScanPending=false; boardSynced=false;
    if (!firstMoveOrientationLocked) engineSide=ENGINE_SIDE_UNKNOWN;
    startupFreshFenExpected=false; startupCorrectionMode=false; lastStartupFenEvaluated=""; btScanDisplayPending=false;
    clearPendingLedMove(nullptr);
    if (sendCynus("set internal engine off\n")) { Serial.println("[CYNUS] internal engine OFF command #1 sent"); engineOffSentAt=millis(); cynusEngineOffSecondSendPending=true; chessAdvertisingPendingAfterEngineOff=true; }
    return true;
}

static void stopChessLinkAdvertising() { if (!clServerStarted) return; chessAdvertisingAllowed=false; NimBLEDevice::getAdvertising()->stop(); Serial.println("[CHESS] advertising stopped"); }

static void requestBoardSync(BoardSyncPurpose purpose, uint32_t delayMs) {
    if (!cynusReady) return;
    clearPendingLedMove(nullptr);
    boardSyncPurpose=purpose; boardSynced=false; publishNextFenToChessLink=false; cynusWaitingForMove=false; if (initialStartupComplete) setMoveCycle(firstMoveOrientationLocked ? WAIT_HUMAN_MOVE : WAIT_FIRST_MOVE); setState(SYNC_BOARD);
    if (purpose==BOARD_SYNC_STARTUP) { startupFreshFenExpected=false; startupCorrectionMode=false; lastStartupFenEvaluated=""; }
    boardSyncRequestPending=true; boardSyncRequestAt=millis()+delayMs; Serial.printf("[SYNC] board request queued purpose=%s\n",purpose==BOARD_SYNC_STARTUP?"STARTUP":"RECOVERY");
}

static void recoverChessLinkLoss(const char*) {
    clearPendingLedMove(nullptr);
    stopChessLinkAdvertising(); clConnected=false; clNotify=false; clConnHandle=BLE_HS_CONN_HANDLE_NONE; clBuf=""; cynusWaitingForMove=false; publishNextFenToChessLink=false; setMoveCycle(firstMoveOrientationLocked ? WAIT_HUMAN_MOVE : WAIT_FIRST_MOVE); lastAutoStatusAt=0;
    if (!cynusReady) { boardSynced=false; setState(SEARCH_CYNUS); return; }
    requestBoardSync(initialStartupComplete?BOARD_SYNC_RECOVERY:BOARD_SYNC_STARTUP,150);
}

static void recoverCynusLoss(const char*) {
    clearPendingLedMove(nullptr);
    cynusReady=false; cynusChr=nullptr; cynusDev=nullptr; boardSynced=false; fenNow=""; bufferedFen=""; lastFenSentToChessLink=""; correctionFenCandidate="";
    displayPlayPending=false; if (!firstMoveOrientationLocked) engineSide=ENGINE_SIDE_UNKNOWN; boardSyncPurpose=BOARD_SYNC_NONE; boardSyncRequestPending=false; boardScanPending=false; cynusWaitingForMove=false;
    startupFreshFenExpected=false; startupCorrectionMode=false; publishNextFenToChessLink=false; cynusEngineOffCommandSent=false; cynusExternalModeConfirmed=false; cynusEngineOffSecondSendPending=false; chessAdvertisingPendingAfterEngineOff=false; btScanDisplayPending=false; lastAutoStatusAt=0;
    stopChessLinkAdvertising(); clConnected=false; clNotify=false; clConnHandle=BLE_HS_CONN_HANDLE_NONE; clBuf=""; setState(SEARCH_CYNUS); nextCynusScanAt=millis()+500;
}

static void processSupervision() {
    if (chessRejectEventPending) { chessRejectEventPending=false; if (clServer && clConnHandle!=BLE_HS_CONN_HANDLE_NONE) clServer->disconnect(clConnHandle); }
    if (cynusDisconnectEventPending) { cynusDisconnectEventPending=false; recoverCynusLoss("callback"); }
    if (chessDisconnectEventPending) { chessDisconnectEventPending=false; if (cynusReady) recoverChessLinkLoss("callback"); }
    if (chessConnectEventPending) {
        chessConnectEventPending=false;
        if (cynusReady && boardSynced && state==WAIT_CHESSLINK) {
            chessAdvertisingAllowed=false;
            if (!firstMoveOrientationLocked) engineSide=ENGINE_SIDE_UNKNOWN;
            setState(RUNNING);
            setMoveCycle(firstMoveOrientationLocked ? WAIT_HUMAN_MOVE : WAIT_FIRST_MOVE);
            cynusDisplay("Connect");
            Serial.println(firstMoveOrientationLocked ? "[FIRST MOVE] orientation already locked; resuming game" : "[FIRST MOVE] waiting to see whether Cynus or chess computer moves first");
            lastAutoStatusAt=0;
        }
        else chessRejectEventPending=true;
    }
    if (boardSyncRequestPending && (state!=SYNC_BOARD || boardSyncPurpose==BOARD_SYNC_NONE)) {
        boardSyncRequestPending=false;
        Serial.println("[SYNC] stale board scan request cancelled");
    }
    if (boardSyncRequestPending && cynusReady && state==SYNC_BOARD && boardSyncPurpose!=BOARD_SYNC_NONE && (int32_t)(millis()-boardSyncRequestAt)>=0) {
        boardSyncRequestPending=false; Serial.println("[SYNC] starting physical board scan");
        if (sendCynus("scan board\n")) {
            if (boardSyncPurpose==BOARD_SYNC_STARTUP) { boardScanPending=false; startupFreshFenExpected=true; Serial.println("[STARTUP] scan started; waiting for Cynus scan FEN"); }
            else { boardScanPending=true; boardScanGetFenAt=millis()+BOARD_SCAN_WAIT_MS; }
        } else { boardSyncRequestPending=true; boardSyncRequestAt=millis()+500; }
    }
    if (boardScanPending && cynusReady && (int32_t)(millis()-boardScanGetFenAt)>=0) {
        boardScanPending=false; Serial.println("[SYNC] recovery scan complete; requesting fresh FEN");
        if (!sendCynus("get fen\n")) { boardSyncRequestPending=true; boardSyncRequestAt=millis()+500; }
    }
    if (clConnected && clNotify && boardSynced) {
        uint32_t interval = autoReportIntervalMs();
        if (interval && (lastAutoStatusAt==0 || (uint32_t)(millis()-lastAutoStatusAt)>=interval)) {
            lastAutoStatusAt=millis();
            if (fenNow != lastFenSentToChessLink) sendStatus();
        }
    }
    if (millis()-lastLinkHealthCheckAt>=LINK_HEALTH_CHECK_MS) {
        lastLinkHealthCheckAt=millis();
        if (cynusReady && cynusClient && !cynusClient->isConnected()) { recoverCynusLoss("health-check"); return; }
        if (clConnected && clServer && clServer->getConnectedCount()==0) { recoverChessLinkLoss("health-check"); return; }
    }
    if (!cynusReady && !cynusConnectPending && state==SEARCH_CYNUS && nextCynusScanAt && (int32_t)(millis()-nextCynusScanAt)>=0) { nextCynusScanAt=0; startCynusScan(); }
}

static void showGameRescanErrors() {
    if (!initialStartupComplete || state!=RUNNING || moveCycle!=WAIT_ENGINE_MOVE) { lastGameErrorDisplay=""; return; }
    if (!correctionFenCandidate.length() || !lastFenSentToChessLink.length()) return;
    String msg=differenceDisplay(correctionFenCandidate,lastFenSentToChessLink);
    if (!msg.length()) { if (lastGameErrorDisplay.length()) { lastGameErrorDisplay=""; cynusDisplay("play"); } return; }
    if (msg==lastGameErrorDisplay) return;
    lastGameErrorDisplay=msg; displayPlayPending=false; cynusDisplay(msg.c_str()); sendCynus("play audio error\n");
}

static void processStartOrientation() {
    if (startStatusPending && boardSynced && clConnected && clNotify) { clStatusPending=true; startStatusPending=false; }
    showStartupErrors();
    showGameRescanErrors();
}

static void cynuslinkCoreSetup() {
    Serial.begin(115200); delay(1500); Serial.println(); Serial.println("=== CynusLink Robust Core Baseline v2.11 ===");
    memset(ee,0,sizeof(ee)); ee[0]=0x00; ee[1]=0x14; ee[2]=0x00; ee[4]=0x0F;
    NimBLEDevice::init(CL_NAME); NimBLEDevice::setPower(3); NimBLEDevice::setMTU(128);
    createChessLinkServer(); setState(SEARCH_CYNUS); moveCycleEnteredAt=millis(); startCynusScan();
}

static void cynuslinkCoreLoop() {
    if (cynusConnectPending) { cynusConnectPending=false; if (!connectCynus()) { cynusDev=nullptr; delay(1000); startCynusScan(); } }
    if (clProcessPending) { clProcessPending=false; processCL(); }
    if (clStatusPending) { clStatusPending=false; sendStatus(); }
    processPendingLedMove();
    processSupervision();
    if (btScanDisplayPending && cynusReady && !clConnected && state==WAIT_CHESSLINK && (int32_t)(millis()-btScanDisplayAt)>=0) {
        btScanDisplayPending=false;
        cynusDisplay("BT Scan");
        Serial.println("[DISPLAY] BT Scan: POS OK shown for 2 seconds, scanning for ChessLink");
        startChessLinkAdvertising();
    }
    if (displayPlayPending && cynusReady && clConnected && (int32_t)(millis()-displayPlayAt)>=0) { displayPlayPending=false; cynusDisplay("play"); }
    if (cynusEngineOffSecondSendPending && cynusReady && millis()-engineOffSentAt>=300) {
        cynusEngineOffSecondSendPending=false;
        if (sendCynus("set internal engine off\n")) { Serial.println("[CYNUS] internal engine OFF command #2 sent"); engineOffSentAt=millis(); }
    }
    if (chessAdvertisingPendingAfterEngineOff && cynusReady && !cynusEngineOffSecondSendPending && millis()-engineOffSentAt>=300) {
        chessAdvertisingPendingAfterEngineOff=false; Serial.println("[STARTUP] engine-off sequence complete"); requestBoardSync(initialStartupComplete?BOARD_SYNC_RECOVERY:BOARD_SYNC_STARTUP,0);
    }
    delay(10);
}

void setup() { cynuslinkCoreSetup(); }
void loop() { cynuslinkCoreLoop(); processStartOrientation(); }
