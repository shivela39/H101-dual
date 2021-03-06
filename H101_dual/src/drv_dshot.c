// CHANGING THE H-BRIDGE CODE CAN RESULT IN CONNECTING THE FETs ACROSS THE
// BATTERY AND AS SUCH BREAKING THE BOARD.

// Dshot driver for H101_dual firmware. Written by Markus Gritsch.
// No throttle jitter, no min/max calibration, just pure digital goodness :)

// Dshot150 would be fast enough for up to 8 kHz main loop frequency. But
// since this implementation does simple bit banging, Dshot150 takes a lot of
// our 1 ms main loop time. Dshot300 takes less time for bit banging, which
// leaves more idle time. Implementing the driver using DMA (like Betaflight
// does) is left as an excercise for the reader ;)

// The ESC signal must be taken before the FET, i.e. non-inverted. The
// signal after the FET with a pull-up resistor is not good enough.
// Bit-bang timing tested only with Keil compiler.

// Dshot capable ESCs required. Consider removing the input filter cap,
// especially if you get drop outs. Tested on "Racerstar MS Series 15A ESC
// BLHeLi_S OPTO 2-4S" ESCs (rebranded ZTW Polaris) with A_H_20_REV16_43.HEX
// and removed filter cap.

// USE AT YOUR OWN RISK. ALWAYS REMOVE PROPS WHEN TESTING.


// Enable this for 3D. The 'Motor Direction' setting in BLHeliSuite must
// be set to 'Bidirectional' (or 'Bidirectional Rev.') accordingly:
#define BIDIRECTIONAL

// Select Dshot150 or Dshot300. Dshot150 consumes quite some main loop time.
// DShot300 may require removing the input filter cap on the ESC:
//#define DSHOT150
#define DSHOT300

// IDLE_OFFSET is added to the throttle. Adjust its value so that the motors
// still spin at minimum throttle.
#define IDLE_OFFSET 40


#include <gd32f1x0.h>

#include "config.h"
#include "defines.h"
#include "drv_pwm.h"
#include "drv_time.h"
#include "hardware.h"

#ifdef USE_DSHOT_DRIVER

#ifdef THREE_D_THROTTLE
#error "Not tested with THREE_D_THROTTLE config option"
#endif

#ifdef __GNUC__
#error "Bit-bang timing not tested with GCC"
#endif

extern int failsafe;
extern int onground;

int pwmdir = 0;
static unsigned long pwm_failsafe_time = 1;
static uint8_t motor_data[ 64 ] = { 0 };

typedef enum { false, true } bool;
static void make_packet( uint8_t number, uint16_t value, bool telemetry );
static void bitbang_data( void );

void pwm_init()
{
	GPIO_InitPara GPIO_InitStructure;

	GPIO_InitStructure.GPIO_Mode = GPIO_MODE_OUT;
	GPIO_InitStructure.GPIO_Speed = GPIO_SPEED_50MHZ;
	GPIO_InitStructure.GPIO_OType = GPIO_OTYPE_PP;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PUPD_NOPULL;

	// A0, A1, A2, A3 TIMER2 ch1, ch2, ch3, ch4
	GPIO_InitStructure.GPIO_Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
	GPIO_Init( GPIOA, &GPIO_InitStructure );

	// A8, A9, A10 TIMER1 ch1, ch2, ch3
	GPIO_InitStructure.GPIO_Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
	GPIO_Init( GPIOA, &GPIO_InitStructure );

	// B1 TIMER3 ch4
	GPIO_InitStructure.GPIO_Pin = GPIO_PIN_1;
	GPIO_Init( GPIOB, &GPIO_InitStructure );

	// top fets off
	GPIO_WriteBit( GPIOF, GPIO_PIN_1, Bit_RESET );
	GPIO_WriteBit( GPIOA, GPIO_PIN_4, Bit_RESET );

	// set failsafetime so signal is off at start
	pwm_failsafe_time = gettime() - 100000;
}

void pwm_set( uint8_t number, float pwm )
{
	if ( pwm < 0.0f ) {
		pwm = 0.0;
	}
	if ( pwm > 0.999f ) {
		pwm = 0.999;
	}

	uint16_t value = 0;

#ifdef BIDIRECTIONAL

	if ( pwmdir == FORWARD ) {
		// maps 0.0 .. 0.999 to 48 + IDLE_OFFSET .. 1047
		value = 48 + IDLE_OFFSET + (uint16_t)( pwm * ( 1000 - IDLE_OFFSET ) );
	} else if ( pwmdir == REVERSE ) {
		// maps 0.0 .. 0.999 to 1048 + IDLE_OFFSET .. 2047
		value = 1048 + IDLE_OFFSET + (uint16_t)( pwm * ( 1000 - IDLE_OFFSET ) );
	}

#else

	// maps 0.0 .. 0.999 to 48 + IDLE_OFFSET * 2 .. 2047
	value = 48 + IDLE_OFFSET * 2 + (uint16_t)( pwm * ( 2001 - IDLE_OFFSET * 2 ) );

#endif

	if ( onground ) {
		value = 0; // stop the motors
	}

	if ( failsafe ) {
		if ( ! pwm_failsafe_time ) {
			pwm_failsafe_time = gettime();
		} else {
			// 100ms after failsafe we turn off the signal (for safety while flashing)
			if ( gettime() - pwm_failsafe_time > 100000 ) {
				value = 0;
			}
		}
	} else {
		pwm_failsafe_time = 0;
	}

	make_packet( number, value, false );

	if ( number == 3 ) {
		bitbang_data();
	}
}

static void make_packet( uint8_t number, uint16_t value, bool telemetry )
{
	uint16_t packet = ( value << 1 ) | ( telemetry ? 1 : 0 ); // Here goes telemetry bit
	// compute checksum
	uint16_t csum = 0;
	uint16_t csum_data = packet;
	for ( uint8_t i = 0; i < 3; ++i ) {
		csum ^= csum_data; // xor data by nibbles
		csum_data >>= 4;
	}
	csum &= 0xf;
	// append checksum
	packet = ( packet << 4 ) | csum;

	// generate pulses for whole packet
	for ( uint8_t i = 0; i < 16; ++i ) {
		if ( packet & 0x8000 ) { // MSB first
			motor_data[ i * 4 + 0 ] |= 1 << number;
			motor_data[ i * 4 + 1 ] |= 1 << number;
			motor_data[ i * 4 + 2 ] |= 1 << number;
			motor_data[ i * 4 + 3 ] |= 0 << number;
		} else {
			motor_data[ i * 4 + 0 ] |= 1 << number;
			motor_data[ i * 4 + 1 ] |= 0 << number;
			motor_data[ i * 4 + 2 ] |= 0 << number;
			motor_data[ i * 4 + 3 ] |= 0 << number;
		}
		packet <<= 1;
	}
}

// Do not change anything between #pragma push and #pragma pop
// without redoing thorough timing measurements.
#pragma push
#pragma O2

#define gpioset( port , pin) port->BOR = pin
#define gpioreset( port , pin) port->BCR = pin

static void bitbang_data()
{
	for ( uint8_t i = 0; i < 64; ++i ) {
		const uint8_t data = motor_data[ i ];
		motor_data[ i ] = 0;

		if ( data & 0x01 ) {
			__NOP();
			gpioset( GPIOA, GPIO_PIN_1 ); // FL
		} else {
			__NOP(); __NOP();
			gpioreset( GPIOA, GPIO_PIN_1 );
		}

		if ( data & 0x02 ) {
			__NOP();
			gpioset( GPIOA, GPIO_PIN_3 ); // BL
		} else {
			__NOP(); __NOP();
			gpioreset( GPIOA, GPIO_PIN_3 );
		}

		if ( data & 0x04 ) {
			__NOP();
			gpioset( GPIOA, GPIO_PIN_10 ); // FR
		} else {
			__NOP(); __NOP();
			gpioreset( GPIOA, GPIO_PIN_10 );
		}

		if ( data & 0x08 ) {
			__NOP();
			gpioset( GPIOA, GPIO_PIN_8 ); // BR
		} else {
			__NOP(); __NOP();
			gpioreset( GPIOA, GPIO_PIN_8 );
		}

#if defined( DSHOT300 ) && ! defined( DSHOT150 )

		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP();

#elif defined( DSHOT150 ) && ! defined( DSHOT300 )

		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP();

#else
#error "Either define DSHOT150 or DSHOT300"
#endif

	}
}

#pragma pop

#define DSHOT_CMD_BEEP1 1
#define DSHOT_CMD_BEEP2 2
#define DSHOT_CMD_BEEP3 3
#define DSHOT_CMD_BEEP4 4
#define DSHOT_CMD_BEEP5 5 // 5 currently uses the same tone as 4 in BLHeli_S.

#ifndef MOTOR_BEEPS_TIMEOUT
#define MOTOR_BEEPS_TIMEOUT 5e6
#endif

void motorbeep()
{
	static unsigned long motor_beep_time = 0;
	if ( failsafe ) {
		unsigned long time = gettime();
		if ( motor_beep_time == 0 ) {
			motor_beep_time = time;
		}
		const unsigned long delta_time = time - motor_beep_time;
		if ( delta_time > MOTOR_BEEPS_TIMEOUT ) {
			uint8_t beep_command = 0;
			if ( delta_time % 2000000 < 250000 ) {
				beep_command = DSHOT_CMD_BEEP1;
			} else if ( delta_time % 2000000 < 500000 ) {
				beep_command = DSHOT_CMD_BEEP3;
			} else if ( delta_time % 2000000 < 750000 ) {
				beep_command = DSHOT_CMD_BEEP2;
			} else if ( delta_time % 2000000 < 1000000 ) {
				beep_command = DSHOT_CMD_BEEP4;
			}
			if ( beep_command != 0 ) {
				make_packet( 0, beep_command, true );
				make_packet( 1, beep_command, true );
				make_packet( 2, beep_command, true );
				make_packet( 3, beep_command, true );
				bitbang_data();
			}
		}
	} else {
		motor_beep_time = 0;
	}
}

void pwm_dir( int dir )
{
	pwmdir = dir;
}

#endif
