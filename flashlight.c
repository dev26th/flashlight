// Created by Anatoli Klassen (AKA dev26th) and released to the public domain.
// See file UNLICENSE for details.

#include <stdlib.h>
#include <stdbool.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
#include <util/delay.h>

#define OFFTIME PB4
#define LED1    PB0
#define LED2    PB1
#define BAT     PB2
#define BAT_MUX MUX0

#define DEFAULT_BAT_EMPTY  0x87 // ~3.0V
#define DEFAULT_BAT_LOW    0x9e // ~3.5V

#define EEPROM_MODE_ADDR       ((uint8_t*)12)
#define EEPROM_BAT_EMPTY_ADDR  ((uint8_t*)13)
#define EEPROM_BAT_LOW_ADDR    ((uint8_t*)14)

enum BatLevel {
    BatLevel_Good,
    BatLevel_Low,
    BatLevel_Empty
};
typedef enum BatLevel BatLevel_t;

static uint8_t    leds_mode;
static BatLevel_t bat_level;
static uint8_t    bat_empty_level;
static uint8_t    bat_low_level;

/*
#define UART_BAUD    115200                 // do not use too low rates
#define UART_DELAY   (F_CPU/3/UART_BAUD-1)  // the result delay must be [1..255]
#define UART_TX_DDR  DDRB
#define UART_TX_PORT PORTB
#define UART_TX_PIN  PB3

void uart_init() {
    UART_TX_DDR  |= (1 << UART_TX_PIN);
    UART_TX_PORT |= (1 << UART_TX_PIN);
}

void uart_send_byte(uint8_t b) {
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
}

void uart_send_str(const char* s) {
    for(; *s; ++s)
        uart_send_byte(*s);
}
*/

static inline bool get_offtime() {
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

static inline void battery_init() {
    // ADC for battery level
    ADMUX |= (1 << REFS0) | (1 << BAT_MUX); // use internal ref
    ADMUX |= (1 << ADLAR);

    // prescaler 128
    ADCSRA |= (1 << ADPS1) | (1 << ADPS0) | (1 << ADEN);
}

static uint8_t get_battery_direct() {
    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));

    return ADCH;
}

// filtered
static uint8_t get_battery() {
    static uint16_t battery_reg = 0;

    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));
    uint8_t bat = get_battery_direct();

    if(battery_reg == 0) battery_reg = (bat << 4);
    battery_reg = battery_reg - (battery_reg >> 4) + bat;

    return (battery_reg >> 4);
}

static inline void leds_init() {
    // pins for the LEDs
    DDRB  |=   (1 << LED1) | (1 << LED2);
    PORTB &= ~((1 << LED1) | (1 << LED2));
}

void next_leds_mode() {
    ++leds_mode;
    if(leds_mode > 3) leds_mode = 1;
}

static inline void load_leds_mode() {
    leds_mode = eeprom_read_byte(EEPROM_MODE_ADDR);
    if(leds_mode < 1 || leds_mode > 3) leds_mode = 1;
}

static inline void save_leds_mode() {
    eeprom_write_byte(EEPROM_MODE_ADDR, leds_mode);
}

static void set_leds(uint8_t mode) {
    uint8_t reg = PORTB;
    reg &= ~((1 << LED1) | (1 << LED2));
    switch(mode) {
        case 1: reg |= (1 << LED1); break;
        case 2: reg |= (1 << LED2); break;
        case 3: reg |= (1 << LED1) | (1 << LED2); break;
    }
    PORTB = reg;
}

static inline void leds_off() {
    set_leds(0);
}

void leds_restore() {
    set_leds(leds_mode);
}

static void poweroff() {
    leds_off();
    offtime_disable();
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    while(true) {
        sleep_mode();
    }
}

static inline void battery_calibrate() {
    uint8_t cal = get_battery_direct(); // which level to calibrate?
    uint8_t bat;

    // wait for calibration start
    do {
        set_leds(0);
        _delay_ms(20);
        set_leds(1);
        _delay_ms(20);
        bat = get_battery_direct();
    }
    while(bat == 0 || bat == 0xFF);

    // loop to clear the filter
    for(uint8_t i = 0; i < 16; ++i) {
        _delay_ms(100);
        bat = get_battery(); // LED are now in mode 1
    }

    // save
    if(cal == 0)
        eeprom_write_byte(EEPROM_BAT_EMPTY_ADDR, bat);
    else
        eeprom_write_byte(EEPROM_BAT_LOW_ADDR, bat);

    // wait for switch off
    while(true) {
        set_leds(1);
        _delay_ms(200);
        set_leds(0);
        _delay_ms(200);
    }
}

static inline void check_bat_calibrate() {
    uint8_t cal = get_battery_direct();
    if(cal == 0 || cal == 0xFF)
        battery_calibrate(); // will never return
}

BatLevel_t to_bat_level(uint8_t bat) {
    if(bat <= bat_empty_level)
        return BatLevel_Empty;
    else if(bat <= bat_low_level)
        return BatLevel_Low;
    else
        return BatLevel_Good;
}

static inline bool check_bat_level() { 
    BatLevel_t new_level = to_bat_level(get_battery());
    if(new_level == bat_level) return false; // no changes - no more checks

    // recheck; expect battery level will be only worse
    set_leds(1); // same mode as used for calibration
    _delay_ms(1);
    new_level = to_bat_level(get_battery());
    leds_restore();
    if(new_level <= bat_level) return false; // false alarm

    bat_level = new_level;
    return true;
}

static inline void load_bat_levels() {
    bat_empty_level = eeprom_read_byte(EEPROM_BAT_EMPTY_ADDR);
    if(bat_empty_level == 0xFF) bat_empty_level = DEFAULT_BAT_EMPTY;

    bat_low_level = eeprom_read_byte(EEPROM_BAT_LOW_ADDR);
    if(bat_low_level == 0xFF) bat_low_level = DEFAULT_BAT_LOW;
}

int main(void) {
    // init
    //uart_init();
    bool offtime = get_offtime();
    leds_init();
    battery_init();

    check_bat_calibrate();
    load_bat_levels();

    // initial battery check
    if(BatLevel_Empty == to_bat_level(get_battery())) {
        poweroff();
    }

    // check mode switch
    load_leds_mode();
    if(offtime) { // it was a click
        next_leds_mode();
        save_leds_mode();
    }
    leds_restore();

    uint8_t count = 0;
    uint8_t bat_warn;
    while(1) {
        ++count;

        get_battery(); // force filtering of battery level
        if((count & 0x0F) == 0) { // only every ~1.6 sec
            if(check_bat_level()) {
                if(BatLevel_Empty == bat_level) {
                    poweroff();
                }
                else if(BatLevel_Low == bat_level) {
                    bat_warn = 6; // begin warn cycle with blink
                    leds_mode = 1;  // force low-mode
                }
            }
        }

        if(BatLevel_Low == bat_level) {
            --bat_warn;
            if((bat_warn < 2) || (bat_warn >= 4 && bat_warn < 6))
                leds_off();
            else
                leds_restore();
        }

        _delay_ms(100);
    }

    return 0;
}

