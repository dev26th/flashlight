// Created by Anatoli Klassen (AKA dev26th) and released to the public domain.
// See file UNLICENSE for details.

#include <stdlib.h>
#include <stdbool.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>

// == config begin ============================================================

#define ENABLE_CALIBRATION 0
#define ENABLE_UART        0
#define ENABLE_SPECIAL     1

#define DEFAULT_BAT_EMPTY  0x7A // ~3.0V
#define DEFAULT_BAT_MIN    0x84 // ~3.2V
#define DEFAULT_BAT_LOW    0x8F // ~3.5V
#define DEFAULT_BAT_DELTA  0x09 // for each .35 A

#define CLICK_TIMEOUT 7   // unit is 50 msec

// == config end ==============================================================

#define OFFTIME PB4
#define LED1    PB0
#define LED2    PB1
#define BAT     PB2
#define BAT_MUX MUX0

#define EEPROM_BAT_EMPTY_ADDR  ((uint8_t*)0)
#define EEPROM_BAT_MIN_ADDR    ((uint8_t*)1)
#define EEPROM_BAT_LOW_ADDR    ((uint8_t*)2)
#define EEPROM_BAT_DELTA_ADDR  ((uint8_t*)3)
#define EEPROM_MODE_ADDR       ((uint8_t*)4)
#define EEPROM_CLICK_ADDR      ((uint8_t*)5)

#define LEDS_MODE_OFF    0
#define LEDS_MODE_MOON   1
#define LEDS_MODE_LOW    2
#define LEDS_MODE_MED    3
#define LEDS_MODE_HIGH   4
#define LEDS_MODE_BEACON 11
#define LEDS_MODE_STROBE 12
#define LEDS_MODE_SOS    13
#define LEDS_MODE_NORMAL_MIN   LEDS_MODE_MOON
#define LEDS_MODE_NORMAL_MAX   LEDS_MODE_HIGH
#define LEDS_MODE_SPECIAL_MIN  LEDS_MODE_BEACON
#define LEDS_MODE_SPECIAL_MAX  LEDS_MODE_SOS

#define CAL_LOW   0x10
#define CAL_HIGH  0xF0

enum BatLevel {
    BatLevel_Good,
    BatLevel_Low,
    BatLevel_Min,
    BatLevel_Empty
};
typedef enum BatLevel BatLevel_t;

static uint8_t    leds_mode;
static uint8_t    leds_now;
static BatLevel_t bat_level;
static uint8_t    bat_empty_level;
static uint8_t    bat_min_level;
static uint8_t    bat_low_level;
static uint8_t    bat_level_delta;

// == UART ====================================================================

#define UART_BAUD    115200                 // do not use too low rates
#define UART_DELAY   (F_CPU/3/UART_BAUD-1)  // the result delay must be [1..255]
#define UART_TX_DDR  DDRB
#define UART_TX_PORT PORTB
#define UART_TX_PIN  PB3

static inline void uart_init() {
#if ENABLE_UART
    UART_TX_DDR  |= (1 << UART_TX_PIN);
    UART_TX_PORT |= (1 << UART_TX_PIN);
#endif
}

void uart_send_byte(uint8_t b) {
#if ENABLE_UART
    __asm__ volatile(
        "cbi %[port], %[pin]  ; start bit"                     "\n\t" // 2
        "adiw r26, 0          ; dummy to balance start"        "\n\t" // 2
        "                     ;   and other bits"              "\n\t"
        "in r18, %[port]      ; save whole port to r18"        "\n\t" // 1
        "ldi r19, 0x03        ; pair r19:%[b] is used"         "\n\t" // 1
        "                     ;   as 16 bit register,"         "\n\t"
        "                     ;   preload with 2 stop bits"    "\n\t"
        "1:"                                                   "\n\t"
        "mov r26, %[d]        ; delay (%[d]*3) cycles"         "\n\t" // 1
        "2:"                                                   "\n\t"
        "dec r26"                                              "\n\t" // 1
        "brne 2b"                                              "\n\t" // 1/2
        "bst %[b], 0          ; move LSB of 'b' to r18 via T"  "\n\t" // 1
        "bld r18, %[pin]"                                      "\n\t" // 1
        "out %[port], r18"                                     "\n\t" // 1
        "lsr r19"                                              "\n\t" // 1
        "ror %[b]"                                             "\n\t" // 1
        "brne 1b              ; next bit"                      "\n\t" // 1/2
        :
        : [d] "r"(UART_DELAY), [b] "r"(b), [port] "I"(_SFR_IO_ADDR(UART_TX_PORT)), [pin] "I"(UART_TX_PIN)
        : "r26", "r18", "r19"
    );
#endif
}

void uart_send_str(const char* s) {
#if ENABLE_UART
    for(; *s; ++s)
        uart_send_byte(*s);
#endif
}

void uart_send_hex(uint8_t n) {
#if ENABLE_UART
    static const char DIGITS[] = "0123456789ABCDEF";
    uart_send_byte(DIGITS[n >> 4]);
    uart_send_byte(DIGITS[n & 0x0F]);
#endif
}

// == utils ===================================================================

void delay(uint8_t ms) {
    static const uint16_t count = F_CPU / 1000 / 4;
    __asm__ volatile(
        "1:"                                                   "\n\t"
        "mov r21, %[d1]"                                       "\n\t"
        "mov r20, %[d2]"                                       "\n\t"
        "2:"                                                   "\n\t"
        "subi r20, 1"                                          "\n\t" // 1
        "sbci r21, 0"                                          "\n\t" // 1
        "brne 2b"                                              "\n\t" // 1/2
        "dec %[ms]"                                            "\n\t"
        "brne 1b"                                              "\n\t"
        :
        : [d1] "a"((uint8_t)(count >> 8)), [d2] "a"((uint8_t)count), [ms] "r"(ms)
        : "r20", "r21"
    );
}

// == mode management =========================================================

static inline bool is_short_click() {
    // check off-time and change the capacitor afterwards
    DDRB &= ~(1 << OFFTIME);
    PORTB &= ~(1 << OFFTIME);

    uint8_t offtime = (PINB & (1 << OFFTIME));

    DDRB |= (1 << OFFTIME);
    PORTB |= (1 << OFFTIME);

    return (offtime != 0);
}

static inline void offtime_disable() {
    DDRB &= ~(1 << OFFTIME);
    PORTB &= ~(1 << OFFTIME);
}

static inline bool is_in_normal_mode() {
    return (leds_mode >= LEDS_MODE_NORMAL_MIN) && (leds_mode <= LEDS_MODE_NORMAL_MAX);
}

static inline bool is_in_special_mode() {
    return (leds_mode >= LEDS_MODE_SPECIAL_MIN) && (leds_mode <= LEDS_MODE_SPECIAL_MAX);
}

static inline void next_leds_mode() {
    bool special = is_in_special_mode();
    ++leds_mode;
    if(special) {
        if(leds_mode > LEDS_MODE_SPECIAL_MAX)
            leds_mode = LEDS_MODE_SPECIAL_MIN;
    }
    else {
        if(leds_mode > LEDS_MODE_NORMAL_MAX)
            leds_mode = LEDS_MODE_NORMAL_MIN;
    }
}

static inline void load_leds_mode() {
    leds_mode = eeprom_read_byte(EEPROM_MODE_ADDR);
    if(!is_in_normal_mode() && !is_in_special_mode()) leds_mode = LEDS_MODE_NORMAL_MIN;
}

static inline void save_leds_mode() {
    eeprom_write_byte(EEPROM_MODE_ADDR, leds_mode);
}

// == LEDs management =========================================================

static inline void leds_init() {
    // pins for the LEDs
    DDRB  |=   (1 << LED1) | (1 << LED2);
    PORTB &= ~((1 << LED1) | (1 << LED2));
}

static inline void start_pwm(uint8_t val) {
    TCNT0  = 0;
    TCCR0B = (1 << CS01); // prescaler 8
    TCCR0A = (1 << COM0A1) | (1 << WGM01) | (1 << WGM00);
    OCR0A  = val;
}

static inline void stop_pwm() {
    TCCR0A = 0;
}

static void set_leds(uint8_t mode) {
    uint8_t reg = PORTB;
    reg &= ~((1 << LED1) | (1 << LED2));
    stop_pwm();
    switch(mode) {
        case LEDS_MODE_MOON: start_pwm(1); break;
        case LEDS_MODE_LOW:  reg |= (1 << LED1); break;
        case LEDS_MODE_MED:  reg |= (1 << LED2); break;
        case LEDS_MODE_HIGH: reg |= (1 << LED1) | (1 << LED2); break;
    }
    PORTB = reg;
    leds_now = mode;
}

void leds_restore() {
    set_leds(leds_mode);
}

static inline void poweroff() {
    set_leds(LEDS_MODE_OFF);
    offtime_disable();
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    while(true) {
        sleep_mode();
    }
}

// == battery measurement =====================================================

static inline void battery_init() {
    // ADC for battery level
    ADMUX |= (1 << REFS0) | (1 << BAT_MUX); // use internal ref
    ADMUX |= (1 << ADLAR);

    // prescaler 128
    ADCSRA |= (1 << ADPS1) | (1 << ADPS0) | (1 << ADEN);
}

static inline uint8_t get_battery_direct() {
    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));

    return ADCH;
}

// filtered
static inline uint8_t get_battery() {
    static uint16_t battery_reg = 0;

    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));
    uint8_t bat = get_battery_direct();

    if(battery_reg == 0) battery_reg = (bat << 4);
    battery_reg = battery_reg - (battery_reg >> 4) + bat;

    return (battery_reg >> 4);
}

static void load_bat_levels() {
    bat_empty_level = eeprom_read_byte(EEPROM_BAT_EMPTY_ADDR);
    if(bat_empty_level == 0xFF) bat_empty_level = DEFAULT_BAT_EMPTY;

    bat_min_level = eeprom_read_byte(EEPROM_BAT_MIN_ADDR);
    if(bat_min_level == 0xFF) bat_min_level = DEFAULT_BAT_MIN;

    bat_low_level = eeprom_read_byte(EEPROM_BAT_LOW_ADDR);
    if(bat_low_level == 0xFF) bat_low_level = DEFAULT_BAT_LOW;

    bat_level_delta = eeprom_read_byte(EEPROM_BAT_DELTA_ADDR);
    if(bat_level_delta == 0xFF) bat_level_delta = DEFAULT_BAT_DELTA;

    uart_send_hex(bat_empty_level);
    uart_send_hex(bat_min_level);
    uart_send_hex(bat_low_level);
    uart_send_hex(bat_level_delta);
}

#if ENABLE_CALIBRATION
static inline void battery_calibrate() {
    uint8_t cal = get_battery_direct(); // which level to calibrate?
    uint8_t bat;

    // wait for calibration start
    do {
        set_leds(LEDS_MODE_LOW);
        delay(20);
        set_leds(LEDS_MODE_OFF);
        delay(20);
        bat = get_battery_direct();
    }
    while(bat <= CAL_LOW || bat >= CAL_HIGH);

    // LEDs are now off

    load_bat_levels();
    if(cal) {
        bat_low_level = bat;
        if(bat_low_level < bat_empty_level) bat_empty_level = bat_low_level;

        set_leds(LEDS_MODE_LOW);
        delay(100);
        bat = get_battery_direct();

        if(bat > bat_low_level) bat = bat_low_level;
        bat_level_delta = (bat_low_level - bat);
    }
    else {
        bat_empty_level = bat;
        if(bat_low_level < bat_empty_level) bat_low_level = bat_empty_level;
    }

    // min: mean value of low and empty
    bat_min_level = ((bat_low_level - bat_empty_level) >> 1) + bat_empty_level;

    // save all
    eeprom_write_byte(EEPROM_BAT_EMPTY_ADDR, bat_empty_level);
    eeprom_write_byte(EEPROM_BAT_MIN_ADDR,   bat_min_level);
    eeprom_write_byte(EEPROM_BAT_LOW_ADDR,   bat_low_level);
    eeprom_write_byte(EEPROM_BAT_DELTA_ADDR, bat_level_delta);

    // wait for switch off
    while(true) {
        set_leds(LEDS_MODE_LOW);
        delay(200);
        set_leds(LEDS_MODE_OFF);
        delay(200);
    }
}
#endif

static inline void check_bat_calibrate() {
#if ENABLE_CALIBRATION
    uint8_t cal = get_battery_direct();
    uart_send_str("B");
    uart_send_hex(cal);
    uart_send_str("\r\n");
    if(cal <= CAL_LOW || cal >= CAL_HIGH)
        battery_calibrate(); // will never return
#endif
}

static inline BatLevel_t to_bat_level(uint8_t bat) {
    if(bat <= bat_empty_level)
        return BatLevel_Empty;
    else if(bat <= bat_min_level)
        return BatLevel_Min;
    else if(bat <= bat_low_level)
        return BatLevel_Low;
    else
        return BatLevel_Good;
}

static inline bool check_bat_level() { 
    uint8_t b = get_battery();

    switch(leds_now) {
        case LEDS_MODE_LOW:  b += bat_level_delta; break;
        case LEDS_MODE_HIGH: b += bat_level_delta; /* continue */ 
        case LEDS_MODE_MED:  b += bat_level_delta*2; break;
    }

    uart_send_str("b");
    uart_send_hex(b);
    uart_send_str("\r\n");

    BatLevel_t new_level = to_bat_level(b);
    if(new_level <= bat_level) { // expect only battery discharging
        return false;
    }
    else {
        bat_level = new_level;
        return true;
    }
}

// == special modes ===========================================================

#if ENABLE_SPECIAL
static inline void process_beacon(uint8_t count) {
    count &= 0x3F;
    if(count)
        set_leds(LEDS_MODE_OFF);
    else
        set_leds(LEDS_MODE_HIGH);
}

static inline void process_strobe(uint8_t count) {
    count &= 0x01;
    if(count)
        set_leds(LEDS_MODE_OFF);
    else
        set_leds(LEDS_MODE_HIGH);
}

static inline void process_sos(uint8_t count) {
    static const uint8_t SIGNAL[] = { 0b01010101, 0b11011101, 0b11010101, 0b00000000 };
    static uint8_t pos = 0;

    if(count & 0x07) return; //no changes, e.g. one unit ~ 0.4 sec

    uint8_t v = ((SIGNAL[pos >> 3]) >> (7 - (pos & 0x07))) & 0x01;
    pos = (pos + 1) & 0x1F;

    if(v)
        set_leds(LEDS_MODE_HIGH);
    else
        set_leds(LEDS_MODE_OFF);
}
#endif

static inline void process_special_mode(uint8_t count) {
#if ENABLE_SPECIAL
    switch(leds_mode) {
        case LEDS_MODE_BEACON: process_beacon(count); break;
        case LEDS_MODE_STROBE: process_strobe(count); break;
        case LEDS_MODE_SOS:    process_sos(count);    break;
    }
#endif
}

// ============================================================================

int main(void) {
    // init
    uart_init();
    bool short_click = is_short_click();
    leds_init();
    battery_init();

    uart_send_str("S\r\n");

    check_bat_calibrate();
    load_bat_levels();

    // initial battery check
    if(BatLevel_Empty == to_bat_level(get_battery())) {
        poweroff();
    }

    // check mode switch
    uint8_t clicks = 0;
    bool leds_mode_changed = false;
    load_leds_mode();
    if(short_click) {
        // count clicks
        clicks = eeprom_read_byte(EEPROM_CLICK_ADDR);
        ++clicks;
        eeprom_write_byte(EEPROM_CLICK_ADDR, clicks);

        if(clicks == 1) {
            next_leds_mode();
            leds_mode_changed = true;
        }
#if ENABLE_SPECIAL
        else if(clicks == 2) {
            if(is_in_special_mode())
                leds_mode = LEDS_MODE_NORMAL_MIN;
            else
                leds_mode = LEDS_MODE_SPECIAL_MIN;
            leds_mode_changed = true;
        }
#endif
    }
    leds_restore();

    uint8_t count = 0;
    uint8_t time  = 0;
    uint8_t bat_warn = 255;
    while(1) {
        ++count;
        if(time != 255) ++time;

        if(time == CLICK_TIMEOUT) {
            if(clicks)    // reset click counter
                eeprom_write_byte(EEPROM_CLICK_ADDR, 0);
            if(leds_mode_changed)
                save_leds_mode();
        }

        (void)get_battery(); // force filtering of battery level
        if((count & 0x1F) == 0) { // only every ~1.6 sec
            if(check_bat_level()) { // battery can only go down
                if(BatLevel_Empty == bat_level) {
                    poweroff();
                }
                else {
                    if(!is_in_special_mode()) {
                        bat_warn = 6; // begin warn cycle with blink
                        // force low-mode
                        if(BatLevel_Min == bat_level)
                            leds_mode = LEDS_MODE_MOON;
                        else if(leds_mode > LEDS_MODE_LOW)
                            leds_mode = LEDS_MODE_LOW;
                    }
                }
            }
        }

        if(is_in_special_mode()) {
            process_special_mode(count);
        }
        else if(BatLevel_Good != bat_level) {
            --bat_warn;
            if((bat_warn < 2) || (bat_warn >= 4 && bat_warn < 6))
                set_leds(LEDS_MODE_OFF);
            else
                leds_restore();
        }

       delay(50);
    }

    return 0;
}

