#!/usr/bin/env python3
"""Generate the reviewable 16:9 EE2028 draft deck.

Install: python3 -m pip install reportlab
Run from any directory: python3 tools/make_slides.py
Generate exact firmware artwork first: python3 tools/preview_display.py
Optional PNG previews: add --render-dir /path/to/previews (requires pymupdf).

The content distinguishes software validation and partial connected-board
checks from pending bench work. It does not assert completed student
contributions. Replace member placeholders and update remaining demo evidence.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

try:
    from reportlab.pdfgen import canvas
    from reportlab.lib.colors import HexColor, Color
    from reportlab.pdfbase.pdfmetrics import stringWidth
except ImportError as error:
    raise SystemExit("Install the generator dependency: python3 -m pip install reportlab") from error


ROOT = Path(__file__).resolve().parents[1]
W, H = 960, 540
NAVY = HexColor("#0C1627")
PANEL = HexColor("#15243A")
PANEL2 = HexColor("#1C2F48")
LINE = HexColor("#2C435E")
WHITE = HexColor("#F4F4EE")
MUTED = HexColor("#B0C2D4")
ORANGE = HexColor("#FFA55C")
GREEN = HexColor("#92D9AB")
RED = HexColor("#F58A8A")
BLUE = HexColor("#8FC6ED")
ASSIGNMENT = "https://canvas.nus.edu.sg/courses/94299/pages/s1ay2627-assignment-2"
BOARD = "https://www.st.com/resource/en/user_manual/um2708-discovery-kit-for-iot-node-multichannel-communication-with-stm32l4-series-stmicroelectronics.pdf"
TCS = "https://look.ams-osram.com/m/7ec5bcc3e40679be/original/TCS3472-DS000390.pdf"
SSD = "https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf"


class Deck:
    def __init__(self, output: Path, duration: int = 20):
        output.parent.mkdir(parents=True, exist_ok=True)
        self.pdf = canvas.Canvas(str(output), pagesize=(W, H), pageCompression=1, invariant=1)
        self.pdf.setTitle("Garden Guard: Animated STM32 Defense Game - DRAFT")
        self.pdf.setAuthor("Group [NUMBER] - member details pending; AI-assisted draft")
        self.pdf.setSubject("STM32 assignment design, validation evidence and proposed demonstration")
        self.page = 0
        self.duration = duration

    def text(self, x, y, value, size=16, color=WHITE, bold=False, font=None):
        self.pdf.setFillColor(color)
        self.pdf.setFont(font or ("Helvetica-Bold" if bold else "Helvetica"), size)
        self.pdf.drawString(x, y, str(value))

    def right(self, x, y, value, size=11, color=MUTED, bold=False):
        self.pdf.setFillColor(color)
        self.pdf.setFont("Helvetica-Bold" if bold else "Helvetica", size)
        self.pdf.drawRightString(x, y, value)

    def wrap(self, x, y, value, width, size=16, leading=None, color=WHITE, bold=False):
        leading = leading or size * 1.35
        font = "Helvetica-Bold" if bold else "Helvetica"
        lines = []
        for paragraph in value.split("\n"):
            current = ""
            for word in paragraph.split():
                proposed = f"{current} {word}".strip()
                if current and stringWidth(proposed, font, size) > width:
                    lines.append(current)
                    current = word
                else:
                    current = proposed
            lines.append(current)
        for line in lines:
            self.text(x, y, line, size, color, bold)
            y -= leading
        return y

    def card(self, x, y, width, height, fill=PANEL, stroke=None, radius=12):
        self.pdf.setFillColor(fill)
        self.pdf.setStrokeColor(stroke or fill)
        self.pdf.roundRect(x, y, width, height, radius, fill=1, stroke=int(stroke is not None))

    def line(self, x1, y1, x2, y2, color=LINE, width=1.2):
        self.pdf.setStrokeColor(color)
        self.pdf.setLineWidth(width)
        self.pdf.line(x1, y1, x2, y2)

    def arrow(self, points, color=ORANGE, width=2.2, head=7):
        for a, b in zip(points, points[1:]):
            self.line(*a, *b, color, width)
        a, b = points[-2:]
        angle = math.atan2(b[1] - a[1], b[0] - a[0])
        path = self.pdf.beginPath()
        path.moveTo(*b)
        path.lineTo(b[0] - head * math.cos(angle - .45), b[1] - head * math.sin(angle - .45))
        path.lineTo(b[0] - head * math.cos(angle + .45), b[1] - head * math.sin(angle + .45))
        path.close()
        self.pdf.setFillColor(color)
        self.pdf.drawPath(path, fill=1, stroke=0)

    def chip(self, x, y, label, color=ORANGE, size=10.5):
        width = stringWidth(label, "Helvetica-Bold", size) + 20
        self.card(x, y, width, 24, fill=Color(color.red, color.green, color.blue, .13), radius=7)
        self.text(x + 10, y + 7, label, size, color, True)
        return width

    def bullet(self, x, y, heading, body, width, color=ORANGE, size=16):
        self.pdf.setFillColor(color)
        self.pdf.circle(x + 3, y + 4, 3, fill=1, stroke=0)
        self.text(x + 19, y, heading, size, WHITE, True)
        return self.wrap(x + 19, y - 24, body, width - 19, size - 1.5, color=MUTED) - 18

    def begin(self, title, subtitle, section, source="", link=None):
        if self.page:
            self.pdf.showPage()
        self.page += 1
        self.pdf.setFillColor(NAVY)
        self.pdf.rect(0, 0, W, H, fill=1, stroke=0)
        self.pdf.setFillColor(ORANGE)
        self.pdf.rect(48, H - 37, 33, 3, fill=1, stroke=0)
        self.text(90, H - 39, section.upper(), 10.5, MUTED, True)
        self.right(912, H - 39, "DRAFT  |  AI-ASSISTED  |  PARTIAL HARDWARE CHECKS", 9, ORANGE, True)
        self.text(48, 463, title, 32, WHITE, True)
        self.text(48, 434, subtitle, 14.5, MUTED)
        self.line(48, 51, 912, 51, LINE)
        self.text(48, 31, "GARDEN GUARD / EE2028 ASSIGNMENT 2", 9, MUTED, True)
        self.right(912, 31, f"{self.page:02d} / 15", 10, MUTED)
        if source:
            self.text(48, 65, source, 8.5, MUTED)
            if link:
                self.pdf.linkURL(link, (48, 62, min(885, 48 + stringWidth(source, "Helvetica", 8.5)), 76), relative=0)
        self.pdf.bookmarkPage(f"slide{self.page}")
        self.pdf.addOutlineEntry(f"{self.page:02d}. {title}", f"slide{self.page}", level=0)

    def state(self, x, y, width, height, name, detail, color):
        self.card(x, y, width, height, PANEL, color)
        self.text(x + 16, y + height - 27, name, 17, color, True)
        self.wrap(x + 16, y + height - 51, detail, width - 32, 12.5, 17, MUTED)

    def save(self):
        self.pdf.save()


def title(d):
    d.begin("Garden Guard", "STM32L4S5 game | Native laptop display + sensing + player interaction", "01 / Project idea")
    d.card(48, 181, 537, 218)
    d.chip(70, 351, "S1 AY2026/27 - ASSIGNMENT 2", GREEN)
    d.wrap(70, 319, "Protect your patch.\nSave the humans.\nRescue the drone.", 470, 28, 37, WHITE, True)
    d.text(70, 201, "Game enhancement built on every compulsory behavior.", 14.5, MUTED)
    d.card(610, 181, 302, 218, PANEL2)
    for y, value, label, color in [(346,"6","zombie hits to win",GREEN),
                                    (279,"3","starting lives",ORANGE),
                                    (212,"4 s","per displayed target",BLUE)]:
        d.text(631, y, value, 34, color, True)
        d.text(707, y + 7, label, 14, WHITE)
    d.text(48, 137, "Group [NUMBER]  |  [Member 1 Name / Matric]  |  [Member 2 Name / Matric]", 14, WHITE)
    d.text(48, 110, "Recorded laptop display: about 29.5 fps. D6/PB1 buzzer confirmed audible on 24 Oct 2026.", 13, ORANGE, True)
    d.text(48, 87, "Original C renderer runs on the MCU. Physical rescue, timeout and a full winning round remain pending.", 12, MUTED)


def baseline(d):
    d.begin("The compulsory baseline stays intact.", "The animated game adds presentation, scoring and feedback to the specified drone behavior.",
            "02 / Requirements", "Source: supplied assignment PDF, pp. 2-5; Core/Src/game.c and telemetry.c", ASSIGNMENT)
    rows = [
        ("Six sensors", "Read all six each second; report physical quantities with units.", "IMPLEMENTED"),
        ("UART monitoring", "Normal telemetry OR all threshold/fault logs; automatically resume.", "IMPLEMENTED"),
        ("Interrupt pushbutton", "Single: +3/10 energy. Double (<500 ms): mode switch or rescue.", "IMPLEMENTED"),
        ("LED and automatic fire", "Standby ON; battle 1 Hz; attacked 2 Hz. Fire at energy >=5/10.", "IMPLEMENTED"),
        ("Attacked / rescue", "10 s rescue window; expiry stops acquisition and application output.", "IMPLEMENTED"),
    ]
    y = 379
    for name, detail, status in rows:
        d.card(48, y - 39, 864, 55)
        d.text(65, y - 6, name, 15, WHITE, True)
        d.text(252, y - 5, detail, 12.1, MUTED)
        d.text(800, y - 6, status, 9.5, GREEN, True)
        y -= 63
    d.text(48, 83, "Physical checks are partial; attack/rescue, calibration and a full winning demo remain pending.", 13, ORANGE, True)


def architecture(d):
    d.begin("A portable game core, bounded board I/O.", "Separate the state machine from HAL peripherals so rules can be tested on the Mac.",
            "03 / Architecture", "Source: Core/Src/main.c, game.c, button.c, board_io.c, external_io.c, game_display.c")
    d.state(48, 287, 232, 112, "ON-BOARD SENSORS", "I2C2: PB10 / PB11\nSix physical measurements", BLUE)
    d.state(48, 174, 232, 89, "USER BUTTON", "PC13 EXTI -> timestamped edges", ORANGE)
    d.state(48, 86, 232, 65, "REFLECTIVE IR", "PC5 / A0 digital presence", GREEN)
    d.card(355, 153, 253, 246, PANEL2, ORANGE)
    d.text(378, 365, "MAIN-LOOP SCHEDULER", 15, ORANGE, True)
    d.text(378, 333, "button.c", 17, WHITE, True)
    d.text(378, 311, "debounce + single/double", 13, MUTED)
    d.text(378, 275, "game.c", 21, WHITE, True)
    d.text(378, 251, "states / energy / scores", 13, MUTED)
    d.text(378, 214, "telemetry.c", 17, WHITE, True)
    d.text(378, 192, "units / faults / thresholds", 13, MUTED)
    d.state(681, 287, 231, 112, "PLAYER FEEDBACK", "C pixels -> laptop / OLED\nBuzzer: D6 PB1 / LED: PB14", GREEN)
    d.state(681, 174, 231, 89, "COMMAND CENTRE", "USART1 PB6 / PB7\n115200 baud, 8N1", BLUE)
    for y, color in [(343,BLUE), (219,ORANGE), (119,GREEN)]:
        target_y = y if y > 153 else 176
        d.arrow([(280,y),(317,y),(317,target_y),(355,target_y)], color)
    for y, color in [(343,GREEN), (219,BLUE)]:
        d.arrow([(608,y),(681,y)], color)
    d.wrap(356, 122, "HAL-free rules + a 32-event queue.\nPure renderer; no dynamic allocation in the core.", 552, 14.5, 21, MUTED)


def states(d):
    d.begin("The rescue deadline is a terminal boundary.", "Round victory/defeat remains a game overlay; it does not replace the compulsory mode controls.",
            "04 / State flowchart", "Source: assignment PDF pp. 2-5; game_button(), game_tick(), game_update_sensors()", ASSIGNMENT)
    d.state(48, 281, 196, 104, "STANDBY", "LED always ON\nSingle press ignored", GREEN)
    d.state(358, 281, 216, 104, "BATTLE", "LED 1 Hz\nSingle: charge / auto-fire", ORANGE)
    d.state(690, 281, 222, 104, "ATTACKED", "LED 2 Hz + 10 s timer\nSingle press ignored", RED)
    d.arrow([(244,351),(358,351)], GREEN)
    d.text(253, 365, "double <500 ms", 10.5, GREEN, True)
    d.arrow([(358,307),(244,307)], BLUE)
    d.text(256, 291, "double: return", 10.5, BLUE)
    d.arrow([(574,351),(690,351)], RED)
    d.text(582, 365, "validated inversion", 10, RED, True)
    d.arrow([(690,307),(574,307)], GREEN)
    d.text(581, 291, "double: rescue", 10.5, GREEN)
    d.state(690, 91, 222, 83, "TERMINATED", "Final output -> UART OFF\nNo restart until reset", RED)
    d.arrow([(801,281),(801,174)], RED)
    d.text(818, 228, "10 s", 18, RED, True)
    d.text(818, 209, "expires", 12, MUTED)
    d.card(48, 172, 526, 79)
    d.text(65, 225, "GAME OVERLAY INSIDE BATTLE", 11, ORANGE, True)
    d.wrap(65, 203, "Six zombie hits: victory. Zero lives: round defeat.\nAutomatic firing / mode switching still operate after the result.", 490, 13, 19, WHITE)
    d.text(48, 120, "Rescue at or after the deadline is rejected.", 17, RED, True)
    d.text(48, 96, "Returning upright re-arms attack detection after a successful rescue.", 12.5, MUTED)


def controls(d):
    d.begin("One button, unambiguous timing.", "Delay a single action until its double-press window has closed; never charge on the first half of a double.",
            "05 / Interaction and game rules", "Source: button.c; game_config.h; game_button(); 30 ms debounce, strictly <500 ms double press")
    d.card(48, 265, 548, 143)
    d.text(67, 382, "FIRST EDGE", 10, MUTED, True)
    d.text(466, 382, "500 ms", 11, ORANGE, True)
    for y in (349,301):
        d.arrow([(92,y),(542,y)], LINE, 1.8)
    d.line(110, 338, 110, 364, GREEN, 3)
    d.text(126, 354, "No second press", 13, WHITE)
    d.line(481,338,481,364,ORANGE,3)
    d.text(364, 327, "commit SINGLE: +3", 12, ORANGE, True)
    d.line(110,290,110,316,GREEN,3)
    d.line(302,290,302,316,GREEN,3)
    d.text(321, 306, "second edge at 300 ms", 12, WHITE)
    d.text(128, 278, "DOUBLE immediately; no charge event", 12, GREEN, True)
    d.card(618, 265, 294, 143, PANEL2)
    d.text(638, 380, "COMMITTED SINGLE PRESSES", 11, ORANGE, True)
    d.text(638, 344, "0 -> 3 -> 6 -> 1", 26, WHITE, True)
    d.wrap(638, 319, "Second single reaches 6/10,\nauto-fires, then consumes 5/10.", 248, 13.5, 20, MUTED)
    score = [
        ("ZOMBIE HIT", "+10 points", "+5 with IR aim lock", GREEN),
        ("HUMAN AVOIDED", "+5 points", "Wait out its 4 s window", BLUE),
        ("HUMAN HIT", "-5 points / -1 life", "Do not charge into a shot", RED),
        ("ZOMBIE MISSED", "-5 points / -1 life", "No shot within 4 s", ORANGE),
    ]
    for i,(name,value,detail,color) in enumerate(score):
        x=48+i*222
        d.card(x,135,198,107)
        d.text(x+14,217,name,10.5,color,True)
        d.text(x+14,185,value,16,WHITE,True)
        d.wrap(x+14,162,detail,174,11.2,15,MUTED)
    d.text(48, 100, "Six zombie hits wins; start with three lives. Exactly 500 ms is not a double press.", 13, MUTED)


def sensors(d):
    d.begin("Six sensors, physical units, explainable limits.", "Starting thresholds are configurable and must be calibrated under the actual demo conditions.",
            "06 / Sensing and monitoring", "Source: app_config.h; board_io.c calibrated conversions; Telemetry_Check() / Telemetry_Format()")
    columns = [(64,"SENSOR"),(285,"OUTPUT"),(496,"NORMAL RANGE"),(725,"INTERPRETATION")]
    for x,label in columns: d.text(x,389,label,10.5,BLUE,True)
    rows = [
        ("Temperature", "degC", "10 to 45", "Operational environment"),
        ("Pressure", "hPa", "850 to 1100", "Atmospheric pressure"),
        ("Humidity", "%RH", "20 to 85", "Relative humidity"),
        ("Accelerometer", "m/s^2, magnitude", "0 to 30", "Motion / impact warning"),
        ("Gyroscope", "deg/s, magnitude", "0 to 250", "Rapid rotation warning"),
        ("Magnetometer", "uT, magnitude", "0 to 200", "Magnetic disturbance"),
    ]
    for i,row in enumerate(rows):
        y=359-i*36
        if i%2==0: d.card(48,y-12,864,33,PANEL,radius=4)
        for (x,_),value in zip(columns,row): d.text(x,y,value,13,WHITE if x<725 else MUTED,x==64)
    d.card(48, 112, 864, 55, PANEL2)
    d.text(64, 144, "UART at 1 Hz", 14, ORANGE, True)
    d.text(223, 144, "Normal: T / P / H / A / G / M with units.", 13.5, WHITE)
    d.text(223, 125, "Any violation or invalid sensor: send all warning/fault lines instead; resume when clear.", 12, MUTED)
    d.text(48, 88, "Bench: all six valid; magnetic warning -> full normal UART recovered. Sensor accuracy is uncalibrated.", 12, MUTED)


def orientation(d):
    d.begin("Use gravity for orientation; gyro for rotation.", "A gyroscope alone reports angular rate, not a stable absolute upside-down direction.",
            "07 / Attacked-state detection", "Source: game_update_sensors(); game_config.h; sampled every 50 ms")
    steps = [
        (48,"1 / CAPTURE UPRIGHT","Hold still at reset.\n6-13 m/s^2 gravity;\ngyro <=20 deg/s.",BLUE),
        (272,"2 / CONFIRM A FLIP","Integrate angular speed.\nRequire >=60 degrees\nof measured rotation.",ORANGE),
        (496,"3 / HOLD INVERSION","Gravity opposes baseline\n(cosine <=-0.7)\nfor at least 350 ms.",RED),
        (720,"4 / ENTER ATTACKED","Start the 10 s timer.\nDouble press rescues;\nupright re-arms detection.",GREEN),
    ]
    for x,heading,body,color in steps:
        d.card(x,203,192,193,PANEL,color)
        d.text(x+14,368,heading,10.5,color,True)
        d.wrap(x+14,270,body,166,12.5,19,MUTED)
        if x<720: d.arrow([(x+192,314),(x+224,314)],color,2,6)
    # Vector boards and gravity arrows: the same physical cue changes sign.
    for cx,angle,color in [(144,0,BLUE),(368,.7,ORANGE),(592,math.pi,RED),(816,0,GREEN)]:
        d.pdf.saveState()
        d.pdf.translate(cx,323)
        d.pdf.rotate(math.degrees(angle))
        d.card(-34,-14,68,28,PANEL2,color,4)
        d.card(-9,-8,18,16,color,radius=2)
        d.pdf.restoreState()
        d.arrow([(cx,344),(cx,295)],color,1.8,6)
    d.card(48,106,864,72,PANEL2)
    d.text(67,151,"REJECT BAD EVIDENCE",11,ORANGE,True)
    d.wrap(67,129,"Invalid motion samples or gaps >250 ms reset continuity. Strong non-gravity acceleration is rejected.\nHysteresis and an upright re-arm prevent repeated attacks while the board stays inverted.",825,13,19,WHITE)


def addons(d):
    d.begin("The MCU renders; the laptop shows its pixels.", "Original 1-bit artwork in a native window. Physical button, sensors and buzzer remain on the board.",
            "08 / Animated game enhancement", "Sources: game_display.c; preview_display.py host-rendered frames; app_config.h; external-hardware.md")
    previews=[(48,"zombie-1800","WALKING ZOMBIE",GREEN),
              (342,"hit-4","FLYING PEA / HIT FEEDBACK",ORANGE),
              (636,"human-1920","FRIENDLY HUMAN - HOLD FIRE",BLUE)]
    for x,name,label,color in previews:
        d.card(x,245,276,163,PANEL2)
        path=ROOT/"output/game-preview/native"/f"{name}.png"
        if not path.is_file():
            raise SystemExit("Generate firmware screen previews first: python3 tools/preview_display.py")
        # The exact native renderer image is used without redrawing the artwork.
        d.pdf.drawImage(str(path),x+18,273,width=240,height=120,mask="auto")
        d.text(x+18,258,label,10,color,True)
    d.card(48,106,423,119)
    d.text(67,202,"PURE SOFTWARE RENDERER",11.5,GREEN,True)
    d.wrap(67,179,"game_display.c draws the 128x64 framebuffer.\nScore, heart lives and energy bar stay visible.\nPea flight, recoil, impact and result effects.\nHost previews show implementation, not a board pass.",382,12.5,19,WHITE)
    d.card(492,106,420,119)
    d.text(511,202,"LIVE DISPLAY AND PHYSICAL I/O",11.5,BLUE,True)
    d.wrap(511,179,"33 ms MCU frames; laptop targets 30 fps.\nAligned SWD reads; coherent sequence checks.\nIR A0/PC5; buzzer D6/PB1 via TIM2 GPIO.\nOLED I2C online; image remains distorted.",378,12.5,19,WHITE)
    d.text(48,84,"Monochrome shapes and FIRE / HOLD FIRE cues identify targets; the reflective module cannot classify colour.",11.4,MUTED)


def scheduling(d):
    d.begin("SysTick drives deadlines and bounded work.", "EXTI records edges. UART uses short HAL_UART_Transmit() calls; the scheduler does not use HAL_Delay().",
            "09 / Bounded scheduling", "Source: main.c due(); SysTick_Handler(); board_io.c UART queue; external_io.c OLED / note sequencer")
    d.card(48,132,472,268)
    d.text(67,371,"TASK",11,BLUE,True)
    d.text(348,371,"CADENCE / BOUND",11,BLUE,True)
    rows=[("Motion + orientation", "50 ms"),("All six sensors / UART", "1000 ms"),
          ("C framebuffer render", "33 ms requested"),("IR stability filter", "30 ms"),
          ("External I2C timeout", "8 ms OLED / 5 ms")]
    for i,(name,value) in enumerate(rows):
        y=335-i*39
        d.text(67,y,name,14,WHITE)
        d.text(348,y,value,14,ORANGE,True)
        if i<4:d.line(67,y-14,500,y-14,LINE,.7)
    nodes=[("Captured EXTI edges",BLUE),("Deadline / state machine",RED),
           ("Due sensor / game tasks",ORANGE),("Bounded OLED + UART work",GREEN)]
    for i,(name,color) in enumerate(nodes):
        y=345-i*61
        d.card(598,y,314,46,PANEL2,color,8)
        d.text(615,y+17,name,14,color,True)
        if i<3:d.arrow([(755,y),(755,y-15)],color,1.6,5)
    d.wrap(48,105,"Laptop: 30 fps target. OLED: immutable 16-byte chunks at 100 kHz; recovery after 1 s if previously online.\nUART: 16-byte foreground chunks, 2 ms HAL timeout; IRQs stay enabled. No HAL_Delay() scheduling.",864,12.5,19,MUTED)


def termination(d):
    d.begin("A failed rescue ends the session predictably.", "The game deadline wins over late input; shutdown finishes only queued output before communication stops.",
            "10 / Termination path", "Source: game_tick() expiry; shutdown_system(); Board_UartIdle(); External_FinishDisplay()")
    d.arrow([(89,337),(876,337)],LINE,3,8)
    for x,color,label,sub in [(108,ORANGE,"ATTACKED","start 10 s countdown"),
                              (580,RED,"10 s DEADLINE","reject rescue / stop acquisition"),
                              (861,GREEN,"SILENT","reset required")]:
        d.line(x,325,x,352,color,4)
        d.text(x-23 if x<800 else x-34,369,label,12,color,True)
        if x<800:d.text(x-23,304,sub,11.5,MUTED)
    for x in (156,202,248,294,340,386,432,478,524):
        d.line(x,332,x,342,MUTED,1)
    d.text(174,275,"Attacked log every second; double press before expiry rescues.",13,WHITE)
    d.card(48,112,423,128)
    d.text(67,209,"AT LOGICAL EXPIRY",12,RED,True)
    d.wrap(67,183,"Discard obsolete game chatter.\nQueue final defeat / termination output.\nNo further sensor acquisition or game actions.",382,14,23,WHITE)
    d.card(492,112,420,128,PANEL2)
    d.text(511,209,"FINAL DRAIN: 600 ms BUDGET",12,ORANGE,True)
    d.wrap(511,183,"Only UART + final OLED frame advance.\nThen stop UART / buzzer / LED / interrupts.\nCPU waits until a hardware reset.",378,14,23,WHITE)
    d.text(48,86,"Final frame stops laptop polling; UART drain then deinitialization establishes application silence.",12.5,MUTED)


def validation(d):
    d.begin("Software checks and selected bench observations.", "D6 sound and native laptop display work; latest OLED communication passed but the image is distorted.",
            "11 / Validation", "Sources: host tests; 24 Oct UART / OLED records; output/laptop-display/verification-30fps.json")
    stats=[(48,"239","game / button checks",GREEN),(342,"8,346","display renderer checks",GREEN),
           (636,"29.5 fps","recorded laptop display",BLUE)]
    for x,value,label,color in stats:
        d.card(x,271,276,130,PANEL2)
        d.text(x+20,341,value,39,color,True)
        d.text(x+20,303,label,15,WHITE)
        if x==636:d.text(x+20,284,"30 fps target; 29.4 fps received in this capture",9,MUTED)
    d.card(48,108,423,140)
    d.text(67,219,"VERIFIED IN SOFTWARE",12,GREEN,True)
    d.wrap(67,192,"Timing / rescue / energy / scores / rollover.\nRenderer checks; visual events / ordering / wrap.\nTelemetry units, faults and recovery: PASS.\nForeground scheduler scenarios: PASS.\nAddress / undefined-behavior sanitizers.",382,13.2,19,WHITE)
    d.card(492,108,420,140)
    d.text(511,219,"OBSERVED ON THE BOARD",12,GREEN,True)
    d.wrap(511,192,"24 Oct: audible D6 buzzer; live laptop frames.\nOLED: 8 s capture, no faults; distorted image.\nInjected button edges: zombie / human hits.\nEarlier: physical double start / LED / charging.\nEarlier: six sensors; IR present and absent.",378,12.5,19,WHITE)
    d.text(48,84,"Pending: full manual win, IR-assisted hit, physical rescue/timeout, measured LED/button timing and calibration.",11.5,MUTED)


def issues(d):
    d.begin("Bench findings guide the next integration step.", "Corrected connectors restore sound and I2C; the laptop makes the original artwork readable.",
            "12 / Engineering decisions", "Source: hardware-test-2026-10-22.md; animation-test-2026-10-22.md; external-hardware.md; I/O review")
    rows=[
        ("Buzzer connector corrected", "Latest D6 is PB1. TIM2 IRQ toggles this GPIO; user confirmed audible feedback on 24 Oct."),
        ("Reflective IR response verified", "Held card: PC5 LOW / aim true. Removed card: PC5 HIGH / aim false. Active-low digital input confirmed."),
        ("OLED moved from D3 to I2C", "Address 0x3c responded; eight seconds had no transfer faults. Pixels remain distorted; panel damage unproven."),
        ("Slow laptop framebuffer reads", "Aligned 32-bit reads cut measured capture time from 92.75 ms to 8.36 ms; the window reached about 29.5 fps."),
        ("Magnetic warning and recovery", "Above 200 uT: warnings. Below 200 uT: full telemetry resumed at 10:17:58. Baseline remains uncalibrated."),
    ]
    for i,(problem,fix) in enumerate(rows):
        y=349-i*54
        d.card(48,y-16,864,47)
        d.text(65,y+4,problem,13.5,ORANGE,True)
        d.wrap(333,y+5,fix,555,12.1,15.5,WHITE)
    d.text(48,89,"Communication success does not prove OLED image quality; the laptop uses the MCU's actual framebuffer.",12,MUTED)


def demo(d):
    d.begin(f"Proposed {d.duration}-minute presentation and demo.", "Rehearse the full 20-minute assignment plan and a compressed 15-minute Lab 5 variant; retain Q&A readiness.",
            "13 / Live demonstration plan", "Sources: Assignment 2 PDF pp. 5-6 (20 min); Lab 5 p. 18 (15 min reference variation)", ASSIGNMENT)
    intervals = (["0-4 min", "4-8 min", "8-12 min", "12-16 min", "16-20 min"]
                 if d.duration == 20 else
                 ["0-3 min", "3-6 min", "6-9 min", "9-12 min", "12-15 min"])
    segments=[(intervals[0],"Design","Idea, baseline, architecture and state flow",BLUE),
              (intervals[1],"Controls + game","Laptop pixels, +3 charge, auto-fire, human avoidance",GREEN),
              (intervals[2],"Sensors + UART","Six units, thresholds, fault log and recovery",ORANGE),
              (intervals[3],"Attack + rescue","Physical flip, 2 Hz LED, rescue double press",RED),
              (intervals[4],"Failure + discussion","10 s timeout, UART silence/reset, evidence and limits",BLUE)]
    for i,(time,name,detail,color) in enumerate(segments):
        y=354-i*52
        d.text(48,y,time,15,color,True)
        d.card(174,y-14,738,42)
        d.text(192,y,name,14,WHITE,True)
        d.text(372,y,detail,12.2,MUTED)
    d.card(48,88,864,44,PANEL2)
    d.text(65,104,"Assignment brief governs the full plan. Confirm the assessed slot; record actual outcomes.",13,ORANGE,True)


def bench(d):
    d.begin("Complete the remaining hardware demonstration.", "Use the native laptop display for readable game pixels; assess OLED image quality separately.",
            "14 / Hardware verification next", "References: ST UM2708; SSD1306 / sensor datasheets; external-hardware.md", BOARD)
    y=385
    points=[("Check display image and extended stability", "I2C now responds. Inspect distorted OLED pixels separately; retain a longer UART/display recording."),
            ("Exercise animation and aim bonus", "Check pea/hit effects and HUD; confirm IR presence adds its bonus to a real zombie hit."),
            ("Play a complete manual winning round", "Injected hits were captured; now use the real button to confirm sounds, scoring and the victory screen."),
            ("Validate every compulsory state", "Measure LED/button timing; physical flip, rescue, timeout silence and reset recovery."),
            ("Calibrate, capture and finalize", "Tune sensor/IR limits; retain UART/photos; fill group details and actual contribution appendix.")]
    for heading,body in points:
        y=d.bullet(48,y,heading,body,864,size=15.5)
    d.text(48,88,"Keep code archive and slides separate; rename both with the required group / matriculation details.",12.5,MUTED)


def contributions(d):
    d.begin("Appendix: planned responsibilities, not a declaration.", "Replace placeholders with actual individual and joint work before submitting or presenting.",
            "15 / Contributions and sources", "Source: assignment PDF pp. 5-6; proposed 50/50 split; hardware report. Actual contribution declarations pending.", ASSIGNMENT)
    d.card(48,236,423,166)
    d.text(67,372,"[MEMBER 1 NAME / MATRIC] - PLANNED 50%",11.5,ORANGE,True)
    d.wrap(67,344,"Player interaction and game rules\nIR presence / optional colour module\nButton interrupts, charge / auto-fire\nBuzzer feedback and LED patterns",382,14,26,WHITE)
    d.card(492,236,420,166)
    d.text(511,372,"[MEMBER 2 NAME / MATRIC] - PLANNED 50%",11.5,GREEN,True)
    d.wrap(511,344,"Display, sensors and monitoring\nOLED integration; six compulsory sensors\nOrientation, UART and thresholds\nPhysical calibration and telemetry evidence",378,14,26,WHITE)
    d.text(48,211,"JOINT - PLANNED",11.5,BLUE,True)
    d.wrap(48,188,"Integration, rescue/termination, laptop viewer, hardware debugging, flowcharts, slides and demonstration.",864,13.5,20,WHITE)
    d.wrap(48,157,"Original assignment requires actual joint/individual contributions and allows PPT/PPTX/PDF/KEY. Lab 5 p. 18 calls the appendix optional and lists PPT only; follow the assignment brief.",864,11.5,16,MUTED)
    d.wrap(48,117,"AI-assisted draft. Planned percentages do not establish completed contributions; replace them with actual work.",864,11.5,16,ORANGE,True)
    links=[("ST board manual / UM2708",BOARD),("SSD1306 datasheet",SSD),("Optional TCS3472 datasheet",TCS)]
    x=48
    for label,url in links:
        d.text(x,93,label,10.5,BLUE)
        width=stringWidth(label,"Helvetica",10.5)
        d.pdf.linkURL(url,(x,90,x+width,104),relative=0)
        x+=width+38


def render(output: Path, directory: Path):
    try:
        import pymupdf
    except ImportError as error:
        raise SystemExit("PDF generated. Install pymupdf to render previews: python3 -m pip install pymupdf") from error
    directory.mkdir(parents=True, exist_ok=True)
    document=pymupdf.open(output)
    for index,page in enumerate(document):
        image=page.get_pixmap(matrix=pymupdf.Matrix(1.5,1.5),alpha=False)
        image.save(directory/f"slide-{index+1:02d}.png")
    print(f"Rendered {len(document)} slides to {directory}")


def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output",type=Path,default=ROOT/"output/pdf/Assignment2_DRAFT_Slides.pdf")
    parser.add_argument("--render-dir",type=Path)
    parser.add_argument("--duration", type=int, choices=(15, 20), default=20,
                        help="Full assignment plan (20) or compressed Lab 5 variant (15)")
    args=parser.parse_args()
    d=Deck(args.output, args.duration)
    slides=[title,baseline,architecture,states,controls,sensors,orientation,addons,
            scheduling,termination,validation,issues,demo,bench,contributions]
    for slide in slides:slide(d)
    if d.page!=15:raise RuntimeError("Update deck page count and footers")
    d.save()
    print(f"Generated {d.page} slides: {args.output}")
    if args.render_dir:render(args.output,args.render_dir)


if __name__=="__main__":
    main()
