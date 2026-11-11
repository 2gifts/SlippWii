// SlippWii input latency test: a GameCube controller that presses a button by itself and says
// when (tools/latency_probe measures the rest on the Wii; tools/latency/analyze.py adds them up).
//
// For a Rana Tadpole (RP2040; the GameCube data line on GPIO 28, as HayBox's `pico` pinout), on
// a console through its USB-C to GameCube cable. It answers the console as a controller with
// nothing held, and about once a second holds the d-pad's left for 150 ms: a press at an
// instant chosen without regard to the console's polls, as a finger's is. Every answer while it
// is held carries the press's age then (how long before this poll it came), in the six analog
// values, small enough to sit inside Melee's dead zones:
//
//   stick x, y, C-stick x, y   128 - 8 + a nibble of the age, high first, in units of 16 us
//   L analog                   16 + the press's number (mod 16): 16..31 marks a test press
//   R analog                   1 + (the nibbles + the number) mod 16: a check
//
// The LED is on while the button is held (for a camera: tools/latency/README.md).
//
// Not a controller to play with: the buttons do nothing, but Start, A, C-Down or C-Right held
// while plugging in go to the flashing mode (BOOTSEL), as the stock firmwares' do. Plugged into
// a PC it is a serial port that prints its counters once a second; opening that port at 1200
// baud also goes to the flashing mode (arduino-pico's reset).
//
// Build: tools/latency/tadpole/build.py (arduino-cli, earlephilhower's arduino-pico 3.6.3, the
// versions HayBox uses). src/joybus is joybus-pio 1.2.3 (LGPL-3.0, JonnyHaystack).

#include "src/joybus/GamecubeConsole.hpp"

#include <hardware/gpio.h>
#include <hardware/timer.h>
#include <pico/bootrom.h>

static const uint DATA_PIN = 28;
static const uint LED_PIN = 25;
static const uint BOOTSEL_PINS[] = { 0, 14, 15, 16 }; // Start, A, C-Down, C-Right (to ground)

static const uint32_t HOLD_US = 150000;
static const uint32_t GAP_MIN_US = 500000;
static const uint32_t GAP_SPREAD_US = 400000;
static const uint32_t FIRST_PRESS_US = 3000000;

static GamecubeConsole *console;
static volatile uint32_t polls, presses, bad_polls, last_age_us, last_gap_us;

static uint32_t rng = 0x2545F491;

static uint32_t next_random() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void setup() {
    for (uint pin : BOOTSEL_PINS) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
    busy_wait_ms(5);
    for (uint pin : BOOTSEL_PINS) {
        if (!gpio_get(pin)) {
            reset_usb_boot(0, 0);
        }
    }
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    console = new GamecubeConsole(DATA_PIN, pio0);
}

// The console's polls, one answer each. (WaitForPollStart answers its other commands itself.)
void loop() {
    static uint64_t press_us = FIRST_PRESS_US, last_poll_us;
    static bool held;
    static uint32_t number;
    gc_report_t report = default_gc_report;

    console->WaitForPollStart();
    uint64_t now = time_us_64();
    last_gap_us = (uint32_t)(now - last_poll_us);
    last_poll_us = now;

    if (held && now >= press_us + HOLD_US) {
        held = false;
        press_us += HOLD_US + GAP_MIN_US + next_random() % GAP_SPREAD_US;
        if (press_us < now) { // (the console was not asking for a while: not a burst of presses)
            press_us = now + GAP_MIN_US + next_random() % GAP_SPREAD_US;
        }
        gpio_put(LED_PIN, 0);
    }
    if (!held && now >= press_us) {
        if (now - press_us > 100000) { // (no poll near the instant: choose another)
            press_us = now + GAP_MIN_US + next_random() % GAP_SPREAD_US;
        } else {
            held = true;
            number++;
            presses = number;
            gpio_put(LED_PIN, 1);
        }
    }
    if (held) {
        uint32_t age = (uint32_t)(now - press_us) / 16;
        if (age > 0xFFFF) {
            age = 0xFFFF;
        }
        uint32_t n0 = age >> 12 & 15, n1 = age >> 8 & 15, n2 = age >> 4 & 15, n3 = age & 15;
        report.dpad_left = 1;
        report.stick_x = 120 + n0;
        report.stick_y = 120 + n1;
        report.cstick_x = 120 + n2;
        report.cstick_y = 120 + n3;
        report.l_analog = 16 + (number & 15);
        report.r_analog = 1 + ((n0 + n1 + n2 + n3 + number) & 15);
        last_age_us = age * 16;
    }

    if (console->WaitForPollEnd() != PollStatus::ERROR) {
        console->SendReport(&report);
        polls = polls + 1;
    } else {
        bad_polls = bad_polls + 1;
    }
}

// The second core: the counters, for a PC to read (nothing listens on a console).
void setup1() {
    Serial.begin(115200);
}

void loop1() {
    delay(1000);
    Serial.printf("tadpole-latency 1: polls %lu (bad %lu), presses %lu, last age %lu us, last poll gap %lu us\r\n",
                  (unsigned long)polls, (unsigned long)bad_polls, (unsigned long)presses,
                  (unsigned long)last_age_us, (unsigned long)last_gap_us);
}
