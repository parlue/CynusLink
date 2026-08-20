from pathlib import Path

p = Path('src/main.cpp')
s = p.read_text(encoding='utf-8')

old = '''                            if (orientation == 1) {
                                if (!sendCynus("set flip board on\\n")) {
                                    Serial.println("[STARTUP] flipped initial position detected, but flip board ON failed; startup remains gated");
                                    startupFreshFenExpected=false;
                                    startupCorrectionMode=true;
                                    continue;
                                }
                                firstMoveFlipOn=true;
                                Serial.println("[STARTUP] flipped initial position detected -> flip board ON sent; first-move gate remains active");
                            }
'''

new = '''                            const bool startupFlipOn = (orientation == 1);
                            if (!sendCynus(startupFlipOn ? "set flip board on\\n" : "set flip board off\\n")) {
                                Serial.printf("[STARTUP] set flip board %s BLE write failed; startup remains gated\\n", startupFlipOn ? "ON" : "OFF");
                                startupFreshFenExpected=false;
                                startupCorrectionMode=true;
                                continue;
                            }
                            firstMoveFlipOn=startupFlipOn;
                            Serial.printf("[STARTUP] initial position -> flip board %s sent; no ACK expected\\n", startupFlipOn ? "ON" : "OFF");
'''

count = s.count(old)
if count != 1:
    raise SystemExit(f'expected exactly one startup flip block, found {count}')

p.write_text(s.replace(old, new, 1), encoding='utf-8')
print('startup flip patch applied')
