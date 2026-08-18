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
static bool flippedBootGatePatch = false;

static int startOrientationPatch(String f) {
    int sp = f.indexOf(' ');
    if (sp >= 0) f = f.substring(0, sp);
    if (f == START_FEN) return 0;
    if (f == FLIPPED_START_FEN_PATCH) return 1;
    return -1;
}

static void configureStartOrientationPatch(bool flipped) {
    startOrientationLatchedPatch = true;
    startOrientationFlippedPatch = flipped;
    openingStartPhasePatch = true;
    startStatusPendingPatch = true;

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
    // Special boot case: the original robust core deliberately accepts only
    // the normal start FEN during BOARD_SYNC_STARTUP. A 180-degree start is
    // therefore completed here. ChessLink advertising is held back until
    // Cynus has explicitly said "get move", so PicoChess cannot send its
    // first white move before the robot is ready to receive it.
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
            cynusDisplay("search");
            Serial.println("[STARTPOS] flipped boot position accepted; waiting for Cynus get move before ChessLink");
        }
    }

    // Once Cynus is genuinely waiting for an external move, expose the
    // already-synchronized flipped start board to ChessLink/PicoChess.
    if (flippedBootGatePatch && cynusWaitingForMove) {
        flippedBootGatePatch = false;
        boardSynced = true;
        setState(WAIT_CHESSLINK);
        setMoveCycle(WAIT_ENGINE_MOVE);
        cynusDisplay("search");
        startChessLinkAdvertising();
        Serial.println("[STARTPOS] Cynus ready; ChessLink enabled for software-first opening");
    }

    int acceptedOrientation = startOrientationPatch(fenNow);

    // Any accepted normal or flipped initial position marks a new game.
    // The latch guarantees exactly one flip command while that same start
    // position remains on the board. It is released after the first move.
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

    // While the physical board is still exactly in its initial position,
    // preserve the correct first-turn state even if the legacy core receives
    // startup callbacks that normally default to WAIT_HUMAN_MOVE.
    if (openingStartPhasePatch && state == RUNNING && acceptedOrientation >= 0) {
        if (moveCycle == WAIT_ROBOT_POSITION) {
            // The first software move has been accepted and sent to Cynus.
            // Do not force WAIT_ENGINE_MOVE again while the robot is moving.
            openingStartPhasePatch = false;
        } else {
            setMoveCycle(startOrientationFlippedPatch ? WAIT_ENGINE_MOVE : WAIT_HUMAN_MOVE);
        }
    }

    // Force the initial board status exactly once per recognized start.
    // In the flipped/software-first case this is deliberately gated on the
    // real Cynus "get move" signal, preventing an early PicoChess move from
    // being acknowledged and then discarded.
    if (startStatusPendingPatch && boardSynced && clConnected && clNotify &&
        (!startOrientationFlippedPatch || cynusWaitingForMove)) {
        clStatusPending = true;
        startStatusPendingPatch = false;
        Serial.println("[STARTPOS] initial board status queued for PicoChess");
    }
}

void setup() {
    cynuslinkCoreSetup();
}

void loop() {
    cynuslinkCoreLoop();
    processStartOrientationPatch();
}
