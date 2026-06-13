#include <fabglconf.h>
#include <dispdrivers/vga16controller.h>
#include <comdrivers/ps2controller.h>
#include <devdrivers/mouse.h>
#include <devdrivers/keyboard.h>
#include <canvas.h>
#include <Bluepad32.h>
#include <SD.h>
#include <SPI.h>

// Pins
#define PIN_BUZZER 25
#define KBD_CLK 33
#define KBD_DAT 32
#define MOUSE_CLK 26
#define MOUSE_DAT 27

fabgl::VGA16Controller DisplayController;
fabgl::PS2Controller PS2Controller;
fabgl::Canvas* canvas = nullptr;

ControllerPtr myControllers[BP32_MAX_GAMEPADS];

// Device Mode
enum DeviceMode {
    MODE_BT_GAMEPAD,
    MODE_PS2_KEYBOARD
};
DeviceMode currentMode = MODE_BT_GAMEPAD;

enum AppPage { PAGE_MENU, PAGE_SCANNING, PAGE_TESTER };
AppPage currentPage = PAGE_MENU;
String connectedMAC = "";
int scanDotCount = 0;
long lastScanDotTime = 0;


// Virtual Buttons
#define VB_COUNT 25
struct BtnDef {
    int x, y, r;
    const char* name;
    uint64_t mapped_bt; // Unified 64-bit input mask
    uint8_t mapped_key; // PS2 Keyboard char/virtual key
    bool isPressed;
};

// Custom Masks for 64-bit Unified Input
#define MASK_DPAD_UP    (1ULL << 16)
#define MASK_DPAD_DOWN  (1ULL << 17)
#define MASK_DPAD_RIGHT (1ULL << 18)
#define MASK_DPAD_LEFT  (1ULL << 19)

#define MASK_MISC_HOME   (1ULL << 20)
#define MASK_MISC_START  (1ULL << 21)
#define MASK_MISC_SELECT (1ULL << 22)

#define MASK_AXIS_L_UP    (1ULL << 32)
#define MASK_AXIS_L_DOWN  (1ULL << 33)
#define MASK_AXIS_L_LEFT  (1ULL << 34)
#define MASK_AXIS_L_RIGHT (1ULL << 35)

#define MASK_AXIS_R_UP    (1ULL << 36)
#define MASK_AXIS_R_DOWN  (1ULL << 37)
#define MASK_AXIS_R_LEFT  (1ULL << 38)
#define MASK_AXIS_R_RIGHT (1ULL << 39)

BtnDef vButtons[VB_COUNT] = {
    // Face Buttons
    {250, 150, 12, "A", BUTTON_A, 'a', false},
    {270, 130, 12, "B", BUTTON_B, 's', false},
    {230, 130, 12, "X", BUTTON_X, 'x', false},
    {250, 110, 12, "Y", BUTTON_Y, 'y', false},
    // D-Pad
    {70, 110, 8, "U", MASK_DPAD_UP, fabgl::VK_UP, false},
    {70, 150, 8, "D", MASK_DPAD_DOWN, fabgl::VK_DOWN, false},
    {50, 130, 8, "L", MASK_DPAD_LEFT, fabgl::VK_LEFT, false},
    {90, 130, 8, "R", MASK_DPAD_RIGHT, fabgl::VK_RIGHT, false},
    // Shoulders & Triggers
    {70, 75, 10, "L1", BUTTON_SHOULDER_L, '1', false},
    {250, 75, 10, "R1", BUTTON_SHOULDER_R, '2', false},
    {70, 55, 10, "L2", BUTTON_TRIGGER_L, '3', false},
    {250, 55, 10, "R2", BUTTON_TRIGGER_R, '4', false},
    // Analogs
    {110, 175, 16, "L3", BUTTON_THUMB_L, 'l', false},
    {210, 175, 16, "R3", BUTTON_THUMB_R, 'r', false},
    // Center
    {130, 130, 6, "SEL", MASK_MISC_SELECT, fabgl::VK_LALT, false},
    {190, 130, 6, "STR", MASK_MISC_START, fabgl::VK_RETURN, false},
    {160, 110, 12, "HOME", MASK_MISC_HOME, fabgl::VK_HOME, false},
    // L-Analog Arrows
    {110, 155, 5, "A_LU", MASK_AXIS_L_UP, 0, false},
    {110, 195, 5, "A_LD", MASK_AXIS_L_DOWN, 0, false},
    {90, 175, 5, "A_LL", MASK_AXIS_L_LEFT, 0, false},
    {130, 175, 5, "A_LR", MASK_AXIS_L_RIGHT, 0, false},
    // R-Analog Arrows
    {210, 155, 5, "A_RU", MASK_AXIS_R_UP, 0, false},
    {210, 195, 5, "A_RD", MASK_AXIS_R_DOWN, 0, false},
    {190, 175, 5, "A_RL", MASK_AXIS_R_LEFT, 0, false},
    {230, 175, 5, "A_RR", MASK_AXIS_R_RIGHT, 0, false}
};

int mappingTarget = -1; // -1 if not mapping
bool sdReady = false;
int cursorX = 320, cursorY = 240;
bool cursorVisible = true;

// Helpers
void playBeep() {
    tone(PIN_BUZZER, 1000, 50); // 1000Hz for 50ms
}

void loadConfig() {
    if (!sdReady) return;
    
    // First, reset all mappings to 0
    for (int i = 0; i < VB_COUNT; i++) {
        if (currentMode == MODE_BT_GAMEPAD) vButtons[i].mapped_bt = 0;
        else vButtons[i].mapped_key = 0;
    }

    File file = SD.open("/Button_Config/mappings.cfg", FILE_READ);
    if (!file) return;
    
    String sectionHeader = "";
    if (currentMode == MODE_BT_GAMEPAD) {
        sectionHeader = "[" + connectedMAC + "]";
    } else {
        sectionHeader = "[PS2_KEYBOARD]";
    }

    bool inTargetSection = false;

    while(file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        
        if (line.startsWith("[")) {
            if (line == sectionHeader) {
                inTargetSection = true;
            } else {
                inTargetSection = false;
            }
        } else if (inTargetSection) {
            int eq = line.indexOf('=');
            if (eq > 0) {
                String key = line.substring(0, eq);
                String valStr = line.substring(eq + 1);
                for (int i = 0; i < VB_COUNT; i++) {
                    if (key == vButtons[i].name) {
                        if (currentMode == MODE_BT_GAMEPAD) {
                            vButtons[i].mapped_bt = strtoull(valStr.c_str(), NULL, 10);
                        } else {
                            vButtons[i].mapped_key = valStr.toInt();
                        }
                        break;
                    }
                }
            }
        }
    }
    file.close();
}

void saveConfig() {
    if (!sdReady) return;
    
    String sectionHeader = "";
    if (currentMode == MODE_BT_GAMEPAD) {
        sectionHeader = "[" + connectedMAC + "]";
    } else {
        sectionHeader = "[PS2_KEYBOARD]";
    }

    File inFile = SD.open("/Button_Config/mappings.cfg", FILE_READ);
    File outFile = SD.open("/Button_Config/mappings.tmp", FILE_WRITE);
    if (!outFile) {
        if(inFile) inFile.close();
        return;
    }

    bool inTargetSection = false;
    bool sectionWritten = false;

    if (inFile) {
        while (inFile.available()) {
            String line = inFile.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            
            if (line.startsWith("[")) {
                if (line == sectionHeader) {
                    inTargetSection = true;
                    sectionWritten = true;
                    // Write our new config
                    outFile.println(sectionHeader);
                    for (int i = 0; i < VB_COUNT; i++) {
                        outFile.print(vButtons[i].name);
                        outFile.print("=");
                        if (currentMode == MODE_BT_GAMEPAD) {
                            outFile.println(vButtons[i].mapped_bt);
                        } else {
                            outFile.println(vButtons[i].mapped_key);
                        }
                    }
                } else {
                    inTargetSection = false;
                    outFile.println(line);
                }
            } else if (!inTargetSection) {
                outFile.println(line);
            }
        }
        inFile.close();
    }
    
    if (!sectionWritten) {
        outFile.println(sectionHeader);
        for (int i = 0; i < VB_COUNT; i++) {
            outFile.print(vButtons[i].name);
            outFile.print("=");
            if (currentMode == MODE_BT_GAMEPAD) {
                outFile.println(vButtons[i].mapped_bt);
            } else {
                outFile.println(vButtons[i].mapped_key);
            }
        }
    }
    
    outFile.close();
    
    SD.remove("/Button_Config/mappings.cfg");
    SD.rename("/Button_Config/mappings.tmp", "/Button_Config/mappings.cfg");
}

int last_ax = 0, last_ay = 0, last_arx = 0, last_ary = 0;
int last_l2 = 0, last_r2 = 0;

void updateAnalogs(bool force) {
    if (currentPage != PAGE_TESTER) return;

    int dxL = 0, dyL = 0, dxR = 0, dyR = 0;
    int l2_val = 0, r2_val = 0;
    
    if (currentMode == MODE_BT_GAMEPAD) {
        for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
            if (myControllers[i] != nullptr && myControllers[i]->isConnected()) {
                dxL = (myControllers[i]->axisX() * 8) / 512;
                dyL = (myControllers[i]->axisY() * 8) / 512;
                dxR = (myControllers[i]->axisRX() * 8) / 512;
                dyR = (myControllers[i]->axisRY() * 8) / 512;
                l2_val = myControllers[i]->brake();
                r2_val = myControllers[i]->throttle();
                break;
            }
        }
    } else {
        l2_val = vButtons[10].isPressed ? 1023 : 0;
        r2_val = vButtons[11].isPressed ? 1023 : 0;
        
        // Simulate analog stick movement from digital PS/2 keys
        if (vButtons[17].isPressed) dyL -= 8;
        if (vButtons[18].isPressed) dyL += 8;
        if (vButtons[19].isPressed) dxL -= 8;
        if (vButtons[20].isPressed) dxL += 8;
        
        if (vButtons[21].isPressed) dyR -= 8;
        if (vButtons[22].isPressed) dyR += 8;
        if (vButtons[23].isPressed) dxR -= 8;
        if (vButtons[24].isPressed) dxR += 8;
    }

    int l2_h = (l2_val * 22) / 1023;
    int r2_h = (r2_val * 22) / 1023;

    if (!force && dxL == last_ax && dyL == last_ay && dxR == last_arx && dyR == last_ary && l2_h == last_l2 && r2_h == last_r2) {
        return; 
    }
    
    canvas->setBrushColor(fabgl::Color::Black);
    if (!force) {
        canvas->setPenColor(fabgl::Color::Black);
        canvas->fillEllipse(110 + last_ax, 175 + last_ay, 6, 6);
        canvas->fillEllipse(210 + last_arx, 175 + last_ary, 6, 6);
    }
    
    fabgl::Color lColor = fabgl::Color::Black;
    if (mappingTarget == 12) lColor = fabgl::Color::BrightYellow;
    else if (vButtons[12].isPressed) lColor = fabgl::Color::BrightGreen;
    
    fabgl::Color rColor = fabgl::Color::Black;
    if (mappingTarget == 13) rColor = fabgl::Color::BrightYellow;
    else if (vButtons[13].isPressed) rColor = fabgl::Color::BrightGreen;

    canvas->setPenColor(fabgl::Color::BrightWhite);
    
    canvas->setBrushColor(lColor);
    canvas->fillEllipse(110 + dxL, 175 + dyL, 6, 6);
    canvas->drawEllipse(110 + dxL, 175 + dyL, 6, 6);
    
    canvas->setBrushColor(rColor);
    canvas->fillEllipse(210 + dxR, 175 + dyR, 6, 6);
    canvas->drawEllipse(210 + dxR, 175 + dyR, 6, 6);
    
    // Draw dynamic fills for L2 and R2
    int l2_x = vButtons[10].x, l2_y = vButtons[10].y;
    int r2_x = vButtons[11].x, r2_y = vButtons[11].y;

    canvas->setBrushColor(fabgl::Color::Black);
    canvas->setPenColor(fabgl::Color::Black);
    canvas->fillRectangle(l2_x - 9, l2_y - 11, l2_x + 9, l2_y + 11);
    canvas->fillRectangle(r2_x - 9, r2_y - 11, r2_x + 9, r2_y + 11);

    if (mappingTarget == 10) {
        canvas->setBrushColor(fabgl::Color::BrightYellow);
        canvas->fillRectangle(l2_x - 9, l2_y - 11, l2_x + 9, l2_y + 11);
    } else if (l2_h > 0) {
        canvas->setBrushColor(fabgl::Color::BrightGreen);
        canvas->fillRectangle(l2_x - 9, l2_y + 11 - l2_h, l2_x + 9, l2_y + 11);
    }

    if (mappingTarget == 11) {
        canvas->setBrushColor(fabgl::Color::BrightYellow);
        canvas->fillRectangle(r2_x - 9, r2_y - 11, r2_x + 9, r2_y + 11);
    } else if (r2_h > 0) {
        canvas->setBrushColor(fabgl::Color::BrightGreen);
        canvas->fillRectangle(r2_x - 9, r2_y + 11 - r2_h, r2_x + 9, r2_y + 11);
    }

    last_ax = dxL; last_ay = dyL; last_arx = dxR; last_ary = dyR;
    last_l2 = l2_h; last_r2 = r2_h;
}

void drawTester() {
    canvas->setBrushColor(fabgl::Color::Black);
    canvas->clear();
    
    canvas->selectFont(&fabgl::FONT_5x8);
    canvas->setPenColor(fabgl::Color::BrightYellow);
    
    String modeText = "Mode: ";
    if (currentMode == MODE_BT_GAMEPAD) {
        modeText += "BT Gamepad (";
        modeText += connectedMAC;
        modeText += ")";
    } else {
        modeText += "PS/2 Keyboard";
    }
    canvas->drawText(5, 5, modeText.c_str());
    canvas->drawText(5, 15, "Click with mouse to map.");
    
    canvas->selectFont(&fabgl::FONT_5x8);
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawRectangle(140, 60, 180, 80);
    canvas->drawText(150, 66, "Back");
    
    canvas->drawRectangle(102, 215, 218, 235);
    canvas->drawText(112, 221, "Save to the SD Card");
    
    // Draw Controller Outline (Realistic Playstation-style)
    canvas->setPenColor(fabgl::Color::BrightWhite);
    fabgl::Point body[] = {
        fabgl::Point(90, 90),  // Top Center Left (Inner L-Shoulder Base)
        fabgl::Point(230, 90), // Top Center Right (Inner R-Shoulder Base)
        fabgl::Point(240, 35), // Inner R-Shoulder Top
        fabgl::Point(260, 35), // Outer R-Shoulder Top
        fabgl::Point(275, 90), // Outer R-Shoulder Base
        fabgl::Point(290, 110),// Right Handle Top Curve
        fabgl::Point(300, 150),// Right Handle Mid Bulge
        fabgl::Point(295, 200),// Right Handle Lower Curve
        fabgl::Point(285, 230),// Right Handle Bottom Outer
        fabgl::Point(260, 235),// Right Handle Bottom Flat
        fabgl::Point(235, 230),// Right Handle Bottom Inner
        fabgl::Point(210, 205),// Under Right Analog
        fabgl::Point(160, 195),// Under Center
        fabgl::Point(110, 205),// Under Left Analog
        fabgl::Point(85, 230), // Left Handle Bottom Inner
        fabgl::Point(60, 235), // Left Handle Bottom Flat
        fabgl::Point(35, 230), // Left Handle Bottom Outer
        fabgl::Point(25, 200), // Left Handle Lower Curve
        fabgl::Point(20, 150), // Left Handle Mid Bulge
        fabgl::Point(30, 110), // Left Handle Top Curve
        fabgl::Point(45, 90),  // Outer L-Shoulder Base
        fabgl::Point(60, 35),  // Outer L-Shoulder Top
        fabgl::Point(80, 35),  // Inner L-Shoulder Top
        fabgl::Point(90, 90)   // Close loop
    };
    canvas->drawPath(body, sizeof(body)/sizeof(body[0]));
    
    // Draw D-pad cross shape
    canvas->drawLine(70, 110, 70, 150);
    canvas->drawLine(50, 130, 90, 130);

    // Draw Virtual Buttons
    canvas->selectFont(&fabgl::FONT_8x8);
    for (int i = 0; i < VB_COUNT; i++) {
        if (mappingTarget == i) {
            canvas->setBrushColor(fabgl::Color::BrightYellow);
        } else if (vButtons[i].isPressed) {
            canvas->setBrushColor(fabgl::Color::BrightGreen);
        } else {
            canvas->setBrushColor(fabgl::Color::Black);
        }
        
        canvas->setPenColor(fabgl::Color::BrightWhite);
        
        const char* name = vButtons[i].name;
        bool isArrow = (strncmp(name, "A_", 2) == 0);
        bool isAnalog = (strcmp(name, "L3") == 0 || strcmp(name, "R3") == 0);
        bool isL1R1 = (strcmp(name, "L1") == 0 || strcmp(name, "R1") == 0);
        bool isL2R2 = (strcmp(name, "L2") == 0 || strcmp(name, "R2") == 0);
        bool isDPad = (strcmp(name, "U") == 0 || strcmp(name, "D") == 0 || strcmp(name, "L") == 0 || strcmp(name, "R") == 0);
        
        if (!isArrow) {
            if (isAnalog) {
                canvas->setBrushColor(fabgl::Color::Black);
                canvas->fillEllipse(vButtons[i].x, vButtons[i].y, vButtons[i].r, vButtons[i].r);
                canvas->drawEllipse(vButtons[i].x, vButtons[i].y, vButtons[i].r, vButtons[i].r);
            } else if (isL1R1) {
                canvas->fillRectangle(vButtons[i].x - 12, vButtons[i].y - 6, vButtons[i].x + 12, vButtons[i].y + 6);
                canvas->drawRectangle(vButtons[i].x - 12, vButtons[i].y - 6, vButtons[i].x + 12, vButtons[i].y + 6);
            } else if (isL2R2) {
                if (mappingTarget != i) canvas->setBrushColor(fabgl::Color::Black);
                canvas->fillRectangle(vButtons[i].x - 10, vButtons[i].y - 12, vButtons[i].x + 10, vButtons[i].y + 12);
                canvas->drawRectangle(vButtons[i].x - 10, vButtons[i].y - 12, vButtons[i].x + 10, vButtons[i].y + 12);
            } else if (isDPad) {
                canvas->fillRectangle(vButtons[i].x - 8, vButtons[i].y - 8, vButtons[i].x + 8, vButtons[i].y + 8);
                canvas->drawRectangle(vButtons[i].x - 8, vButtons[i].y - 8, vButtons[i].x + 8, vButtons[i].y + 8);
            } else {
                canvas->fillEllipse(vButtons[i].x, vButtons[i].y, vButtons[i].r, vButtons[i].r);
                canvas->drawEllipse(vButtons[i].x, vButtons[i].y, vButtons[i].r, vButtons[i].r);
            }
        }
        
        // Draw Label or Vector Arrow
        if (mappingTarget == i) {
            canvas->setPenColor(fabgl::Color::BrightYellow);
        } else if (vButtons[i].isPressed) {
            canvas->setPenColor(fabgl::Color::BrightGreen);
        } else {
            canvas->setPenColor(fabgl::Color::BrightWhite);
        }
        
        if (isArrow) {
            int len = 5;
            if (name[3] == 'U') {
                canvas->drawLine(vButtons[i].x, vButtons[i].y + len, vButtons[i].x, vButtons[i].y - len);
                canvas->drawLine(vButtons[i].x, vButtons[i].y - len, vButtons[i].x - 3, vButtons[i].y - len + 3);
                canvas->drawLine(vButtons[i].x, vButtons[i].y - len, vButtons[i].x + 3, vButtons[i].y - len + 3);
            } else if (name[3] == 'D') {
                canvas->drawLine(vButtons[i].x, vButtons[i].y - len, vButtons[i].x, vButtons[i].y + len);
                canvas->drawLine(vButtons[i].x, vButtons[i].y + len, vButtons[i].x - 3, vButtons[i].y + len - 3);
                canvas->drawLine(vButtons[i].x, vButtons[i].y + len, vButtons[i].x + 3, vButtons[i].y + len - 3);
            } else if (name[3] == 'L') {
                canvas->drawLine(vButtons[i].x + len, vButtons[i].y, vButtons[i].x - len, vButtons[i].y);
                canvas->drawLine(vButtons[i].x - len, vButtons[i].y, vButtons[i].x - len + 3, vButtons[i].y - 3);
                canvas->drawLine(vButtons[i].x - len, vButtons[i].y, vButtons[i].x - len + 3, vButtons[i].y + 3);
            } else if (name[3] == 'R') {
                canvas->drawLine(vButtons[i].x - len, vButtons[i].y, vButtons[i].x + len, vButtons[i].y);
                canvas->drawLine(vButtons[i].x + len, vButtons[i].y, vButtons[i].x + len - 3, vButtons[i].y - 3);
                canvas->drawLine(vButtons[i].x + len, vButtons[i].y, vButtons[i].x + len - 3, vButtons[i].y + 3);
            }
        } else if (mappingTarget != i && !vButtons[i].isPressed) {
            int lenStr = strlen(name);
            int tx = vButtons[i].x - lenStr * 4;
            int ty = vButtons[i].y + vButtons[i].r + 3;
            
            if (strcmp(name, "A") == 0 || strcmp(name, "B") == 0 || strcmp(name, "X") == 0 || strcmp(name, "Y") == 0) {
                tx += 2;
            } else if (strcmp(name, "L1") == 0 || strcmp(name, "L2") == 0) {
                tx = vButtons[i].x + vButtons[i].r + 14; ty = vButtons[i].y - 4;
            } else if (strcmp(name, "R1") == 0 || strcmp(name, "R2") == 0) {
                tx = vButtons[i].x - vButtons[i].r - 10 - lenStr * 8; ty = vButtons[i].y - 4;
            } else if (strcmp(name, "U") == 0) {
                tx = vButtons[i].x - 4; ty = vButtons[i].y - vButtons[i].r - 10;
            } else if (strcmp(name, "D") == 0) {
                tx = vButtons[i].x - 4; ty = vButtons[i].y + vButtons[i].r + 3;
            } else if (strcmp(name, "L") == 0) {
                tx = vButtons[i].x - vButtons[i].r - 10; ty = vButtons[i].y - 4;
            } else if (strcmp(name, "R") == 0) {
                tx = vButtons[i].x + vButtons[i].r + 3; ty = vButtons[i].y - 4;
            } else if (strcmp(name, "L3") == 0) {
                tx = vButtons[i].x + 18; ty = vButtons[i].y + 12;
            } else if (strcmp(name, "R3") == 0) {
                tx = vButtons[i].x - 14 - lenStr * 8; ty = vButtons[i].y + 12;
            } else if (strcmp(name, "HOME") == 0) {
                tx = vButtons[i].x - lenStr * 4; ty = vButtons[i].y - vButtons[i].r - 3;
            }
            canvas->drawText(tx, ty, name);
        }
    }
    
    // Draw Inner Analog stick circles
    updateAnalogs(true);
}


void drawMenu() {
    canvas->setBrushColor(fabgl::Color::Black);
    canvas->clear();
    canvas->selectFont(&fabgl::FONT_8x8);
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawText(80, 40, "FABGL GAMEPAD TESTER");
    
    canvas->selectFont(&fabgl::FONT_5x8);
    canvas->setPenColor(fabgl::Color::BrightYellow);
    canvas->drawText(87, 55, "Choose with keyboard or mouse");
    
    canvas->selectFont(&fabgl::FONT_8x8);
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawRectangle(60, 90, 260, 130);
    canvas->drawText(80, 105, "1. Bluetooth Gamepad");
    
    canvas->drawRectangle(60, 150, 260, 190);
    canvas->drawText(80, 165, "2. PS/2 Keyboard");
}

void drawScanning() {
    canvas->setBrushColor(fabgl::Color::Black);
    canvas->clear();
    canvas->selectFont(&fabgl::FONT_8x8);
    canvas->setPenColor(fabgl::Color::BrightYellow);
    
    String text = "Searching for Gamepad";
    for(int i=0; i<scanDotCount; i++) text += ".";
    
    canvas->drawText(70, 110, text.c_str());
    
    canvas->selectFont(&fabgl::FONT_5x8);
    canvas->drawText(70, 135, "(Ensure your pad is in pairing mode)");
    
    canvas->selectFont(&fabgl::FONT_8x8);
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawRectangle(10, 10, 60, 30);
    canvas->drawText(20, 16, "Back");
}

void drawUI() {
    if (currentPage == PAGE_MENU) {
        drawMenu();
    } else if (currentPage == PAGE_SCANNING) {
        drawScanning();
    } else if (currentPage == PAGE_TESTER) {
        drawTester();
    }
    
    // Draw Cursor
    if (cursorVisible) {
        canvas->setPenColor(fabgl::Color::BrightRed);
        canvas->drawLine(cursorX, cursorY, cursorX + 6, cursorY + 6);
        canvas->drawLine(cursorX, cursorY, cursorX, cursorY + 8);
        canvas->drawLine(cursorX, cursorY, cursorX + 8, cursorY);
    }
}

// Bluepad32 Callbacks
void onConnectedController(ControllerPtr ctl) {
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
        if (myControllers[i] == nullptr) {
            myControllers[i] = ctl;
            ControllerProperties props = ctl->getProperties();
            char idStr[32];
            if (props.vendor_id == 0 && props.product_id == 0) {
                sprintf(idStr, "TYP_%04X_MAC_%02X%02X", props.type, props.btaddr[4], props.btaddr[5]);
            } else {
                sprintf(idStr, "VID_%04X_PID_%04X", props.vendor_id, props.product_id);
            }
            connectedMAC = String(idStr);
            break;
        }
    }
}

void onDisconnectedController(ControllerPtr ctl) {
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
        if (myControllers[i] == ctl) {
            myControllers[i] = nullptr;
            break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    // Setup VGA
    // Olimex Pins: Red 21,22; Green 18,19; Blue 4,5; HSync 23; VSync 15
    DisplayController.begin(GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_23, GPIO_NUM_15);
    DisplayController.setResolution(QVGA_320x240_60Hz);
    
    // Setup PS/2
    PS2Controller.begin((gpio_num_t)KBD_CLK, (gpio_num_t)KBD_DAT, (gpio_num_t)MOUSE_CLK, (gpio_num_t)MOUSE_DAT);
    PS2Controller.setKeyboard(new fabgl::Keyboard());
    PS2Controller.keyboard()->begin(true, true, 0); // Port 0
    PS2Controller.setMouse(new fabgl::Mouse());
    PS2Controller.mouse()->begin(1); // Port 1
    PS2Controller.mouse()->setupAbsolutePositioner(320, 240, false);
    
    // Setup Canvas
    canvas = new fabgl::Canvas(&DisplayController);
    
    // Setup SD Card
    SPI.begin(14, 35, 12, 13); // CLK=14, MISO=35, MOSI=12, SS=13
    if (SD.begin(13, SPI)) {
        sdReady = true;
        if (!SD.exists("/Button_Config")) {
            SD.mkdir("/Button_Config");
        }
        // loadConfig();
    }
    
    // Setup Bluepad32
    BP32.setup(&onConnectedController, &onDisconnectedController);
    
    drawUI();
}

void loop() {
    bool uiNeedsRedraw = false;
    
    // Process BP32
    BP32.update();

    if (currentPage == PAGE_SCANNING && currentMode == MODE_BT_GAMEPAD) {
        if (millis() - lastScanDotTime > 500) {
            scanDotCount = (scanDotCount + 1) % 4;
            lastScanDotTime = millis();
            uiNeedsRedraw = true;
        }
        
        for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
            if (myControllers[i] != nullptr && myControllers[i]->isConnected()) {
                currentPage = PAGE_TESTER;
                loadConfig();
                uiNeedsRedraw = true;
                break;
            }
        }
    }
    
    // Process BT Gamepad Input
    if (currentMode == MODE_BT_GAMEPAD) {
        for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
            ControllerPtr myGamepad = myControllers[i];
            if (myGamepad && myGamepad->isConnected()) {
                uint16_t btns = myGamepad->buttons();
                uint8_t dpad = myGamepad->dpad();
                uint16_t miscBtns = myGamepad->miscButtons();
                
                uint64_t all_inputs = (uint64_t)btns;
                if (dpad & 0x01) all_inputs |= MASK_DPAD_UP;
                if (dpad & 0x02) all_inputs |= MASK_DPAD_DOWN;
                if (dpad & 0x04) all_inputs |= MASK_DPAD_RIGHT;
                if (dpad & 0x08) all_inputs |= MASK_DPAD_LEFT;
                
                if (miscBtns & MISC_BUTTON_HOME) all_inputs |= MASK_MISC_HOME;
                if (miscBtns & MISC_BUTTON_START) all_inputs |= MASK_MISC_START;
                if (miscBtns & MISC_BUTTON_SELECT) all_inputs |= MASK_MISC_SELECT;

                if (myGamepad->axisY() < -256) all_inputs |= MASK_AXIS_L_UP;
                if (myGamepad->axisY() > 256) all_inputs |= MASK_AXIS_L_DOWN;
                if (myGamepad->axisX() < -256) all_inputs |= MASK_AXIS_L_LEFT;
                if (myGamepad->axisX() > 256) all_inputs |= MASK_AXIS_L_RIGHT;

                if (myGamepad->axisRY() < -256) all_inputs |= MASK_AXIS_R_UP;
                if (myGamepad->axisRY() > 256) all_inputs |= MASK_AXIS_R_DOWN;
                if (myGamepad->axisRX() < -256) all_inputs |= MASK_AXIS_R_LEFT;
                if (myGamepad->axisRX() > 256) all_inputs |= MASK_AXIS_R_RIGHT;
                
                if (mappingTarget >= 0 && currentMode == MODE_BT_GAMEPAD) {
                    if (all_inputs != 0) {
                        vButtons[mappingTarget].mapped_bt = all_inputs;
                        mappingTarget = -1;
                        // saveConfig();
                        uiNeedsRedraw = true;
                        playBeep();
                        delay(200); // Debounce
                    }
                } else {
                    for (int j = 0; j < VB_COUNT; j++) {
                        bool pressed = (all_inputs & vButtons[j].mapped_bt) != 0;
                        if (pressed && !vButtons[j].isPressed) {
                            playBeep();
                            uiNeedsRedraw = true;
                        } else if (!pressed && vButtons[j].isPressed) {
                            uiNeedsRedraw = true;
                        }
                        vButtons[j].isPressed = pressed;
                    }
                }
            }
        }
    }
    
    // Process PS/2 Keyboard Input
    auto keyboard = PS2Controller.keyboard();
    if (keyboard) {
        // Always drain the virtual key queue to prevent it from filling up
        // and deadlocking the PS/2 interrupt handler (portMAX_DELAY issue).
        while (keyboard->virtualKeyAvailable()) {
            fabgl::VirtualKeyItem item;
            keyboard->getNextVirtualKey(&item);
            
            // If we are mapping a target, assign the first pressed key
            if (mappingTarget >= 0 && currentMode == MODE_PS2_KEYBOARD) {
                if (item.down) {
                    vButtons[mappingTarget].mapped_key = item.vk;
                    mappingTarget = -1;
                    uiNeedsRedraw = true;
                    playBeep();
                }
            }
        }
        
        if (mappingTarget < 0) {
            // Check normal key presses
            bool kbdStateChanged = false;
            for (int i = 0; i < VB_COUNT; i++) {
                bool pressed = keyboard->isVKDown((fabgl::VirtualKey)vButtons[i].mapped_key);
                if (currentMode == MODE_PS2_KEYBOARD) {
                    if (pressed && !vButtons[i].isPressed) {
                        playBeep();
                        kbdStateChanged = true;
                    } else if (!pressed && vButtons[i].isPressed) {
                        kbdStateChanged = true;
                    }
                    vButtons[i].isPressed = pressed;
                }
            }
            if (kbdStateChanged) uiNeedsRedraw = true;
        }
        
        // Main Menu Keyboard Shortcuts
        if (currentPage == PAGE_MENU) {
            if (keyboard->isVKDown(fabgl::VK_1) || keyboard->isVKDown(fabgl::VK_KP_1)) {
                currentMode = MODE_BT_GAMEPAD;
                currentPage = PAGE_SCANNING;
                uiNeedsRedraw = true;
                delay(200); // debounce
            } else if (keyboard->isVKDown(fabgl::VK_2) || keyboard->isVKDown(fabgl::VK_KP_2)) {
                currentMode = MODE_PS2_KEYBOARD;
                currentPage = PAGE_TESTER;
                loadConfig();
                uiNeedsRedraw = true;
                delay(200); // debounce
            }
        }
    }
    
    // Process PS/2 Mouse Input (for UI)
    auto mouse = PS2Controller.mouse();
    if (mouse) {
        while (mouse->deltaAvailable()) {
            fabgl::MouseDelta delta;
            mouse->getNextDelta(&delta);
            mouse->updateAbsolutePosition(&delta);
        }
        
        fabgl::MouseStatus& mstate = mouse->status();
        static bool lastLeft = false;
        
        int nx = mstate.X;
        int ny = mstate.Y;
        nx = max(0, min(319, nx));
        ny = max(0, min(239, ny));
        
        bool leftDown = mstate.buttons.left;
        bool leftClicked = (leftDown && !lastLeft);
        
        if (nx != cursorX || ny != cursorY || leftDown != lastLeft) {
            uiNeedsRedraw = true;
        }
        
        if (leftClicked) {
            if (currentPage == PAGE_MENU) {
                if (nx > 60 && nx < 320 && ny > 100 && ny < 140) {
                    currentMode = MODE_BT_GAMEPAD;
                    currentPage = PAGE_SCANNING;
                    uiNeedsRedraw = true;
                } else if (nx > 60 && nx < 320 && ny > 160 && ny < 200) {
                    currentMode = MODE_PS2_KEYBOARD;
                    currentPage = PAGE_TESTER;
                    loadConfig();
                    uiNeedsRedraw = true;
                }
            } else if (currentPage == PAGE_SCANNING) {
                if (nx < 70 && ny < 40) {
                    currentPage = PAGE_MENU;
                    uiNeedsRedraw = true;
                }
            } else if (currentPage == PAGE_TESTER) {
                if (nx >= 140 && nx <= 180 && ny >= 60 && ny <= 80) {
                    currentPage = PAGE_MENU;
                    mappingTarget = -1;
                    uiNeedsRedraw = true;
                } else if (nx >= 102 && nx <= 218 && ny >= 215 && ny <= 235) {
                    saveConfig();
                    playBeep();
                    delay(100);
                    playBeep();
                } else {
                    for (int i = 0; i < VB_COUNT; i++) {
                        int dx = nx - vButtons[i].x;
                        int dy = ny - vButtons[i].y;
                        if (dx*dx + dy*dy <= vButtons[i].r * vButtons[i].r) {
                            mappingTarget = i;
                            break;
                        }
                    }
                }
            }
        }
        
        cursorX = nx;
        cursorY = ny;
        lastLeft = leftDown;
    }
    
    if (uiNeedsRedraw) {
        drawUI();
    } else {
        updateAnalogs(false);
    }
    
    delay(10);
}
