// CynusLink orientation/startup layer.
// The stable gateway core is kept in main_base.inc; this layer adds
// deterministic normal/flipped start-position handling without changing
// the proven ChessLink move/correction logic.

#define setup cynuslinkCoreSetup
#define loop cynuslinkCoreLoop
#include "main_base.inc"
#undef loop
#undef setup

static constexpr const char* FLIPPED_START_FEN_PATCH =
    "RNBKQBNR/PPPPPPPP/8/8/8/8/pppppppp/rnbkqbnr";

static bool startOrientationLatchedPatch = false;
static bool startOrientationFlippedPatch = false;
static bool openingStartPhasePatch = false;
static bool startStatusPendingPatch = false;
static String lastStartupErrorDisplayPatch = "";
static String lastStartupFenEvaluatedPatch = "";
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
    if (bufferedFen == lastStartupFenEvaluatedPatch) return;
    lastStartupFenEvaluatedPatch = bufferedFen;

    if (startOrientationPatch(bufferedFen) >= 0) {
        lastStartupErrorDisplayPatch = "";
        return;
    }

    String msg = startupErrorDisplayPatch(bufferedFen);
    if (!msg.length()) return;
    lastStartupErrorDisplayPatch = msg;
    cynusDisplay(msg.c_str());
    sendCynus("play audio error\n");
    Serial.printf("[STARTUP] position error display refreshed: %s (error audio)\n", msg.c_str());
}

static void showGameRescanErrorsPatch() {
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
    lastStartupFenEvaluatedPatch = "";
    lastGameErrorDisplayPatch = "";

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
    // Before ChessLink is connected, get move is not a game event.
    // Keep startup validation authoritative and neutralize the core flags.
    if (!initialStartupComplete && state == SYNC_BOARD && !clConnected &&
        (cynusWaitingForMove || cynusExternalModeConfirmed)) {
        cynusWaitingForMove = false;
        cynusExternalModeConfirmed = false;
        Serial.println("[STARTUP] pre-ChessLink get move ignored; staying in board validation");
    }

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
            boardSynced = true;
            setState(WAIT_CHESSLINK);
            setMoveCycle(WAIT_ENGINE_MOVE);
            cynusDisplay("search");
            startChessLinkAdvertising();
            Serial.println("[STARTPOS] flipped boot position accepted; ChessLink enabled for software-first opening");
        }
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

    if (startStatusPendingPatch && boardSynced && clConnected && clNotify) {
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
