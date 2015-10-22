#include "config.h"
#include "modem.h"
#include "Si446x.h"
#include "gps.h"
#include "types.h"
#include "chip.h"

/* The sine_table is the carrier signal. To achieve phase continuity, each tone
 * starts at the index where the previous one left off. By changing the stride of
 * the index (phase_delta) we get 1200 or 2200 Hz. The PHASE_DELTA_XXXX values
 * can be calculated as:
 * 
 * Fg = frequency of the output tone (1200 or 2200)
 * Fm = sampling rate (PLAYBACK_RATE_HZ)
 * Tt = sine table size (TABLE_SIZE)
 * 
 * PHASE_DELTA_Fg = Tt*(Fg/Fm)
 */

#define TX_CPU_CLOCK		12000000
#define TABLE_SIZE			2
#define PLAYBACK_RATE		(TX_CPU_CLOCK / 256) // Tickrate 46.875 kHz
#define BAUD_RATE			1200
#define SAMPLES_PER_BAUD	(PLAYBACK_RATE / BAUD_RATE) // 52.083333333 / 26.041666667
#define PHASE_DELTA_1200	(((TABLE_SIZE * 1200) << 7) / PLAYBACK_RATE) // Fixed point 9.7 // 1258 / 2516
#define PHASE_DELTA_2200	(((TABLE_SIZE * 2200) << 7) / PLAYBACK_RATE) // 2306 / 4613


// Module globals
static uint16_t current_byte;
static uint16_t current_sample_in_baud;		// 1 bit = SAMPLES_PER_BAUD samples
static uint32_t phase_delta;				// 1200/2200 for standard AX.25
static uint32_t phase;						// Fixed point 9.7 (2PI = TABLE_SIZE)
static uint32_t packet_pos;					// Next bit to be sent out
static bool modem_busy = false;				// Is timer running

// Exported globals
uint16_t modem_packet_size = 0;
uint8_t modem_packet[MODEM_MAX_PACKET];

/**
 * Initializes two timer
 * Timer 1: One Tick per 1/playback_rate	CT16B0
 * Timer 2: PWM								CT32B0
 */
void Modem_Init(void)
{
	// Initialize radio
	Si446x_Init();

	// Set radio power and frequency
	radioTune(gps_get_region_frequency(), RADIO_POWER);

	// Setup sampling timer
	Chip_Clock_EnablePeriphClock(SYSCTL_CLOCK_SCT);
	Chip_SYSCTL_PeriphReset(RESET_SCT);
	Chip_SCT_Config(LPC_SCT, SCT_CONFIG_32BIT_COUNTER | SCT_CONFIG_CLKMODE_BUSCLK);
	Chip_SCT_SetMatchCount(LPC_SCT, SCT_MATCH_0, SystemCoreClock / PLAYBACK_RATE);	// Set the match count for match register 0
	Chip_SCT_SetMatchReload(LPC_SCT, SCT_MATCH_0, SystemCoreClock / PLAYBACK_RATE);	// Set the match reload value for match reload register 0
	LPC_SCT->EV[0].CTRL = (1 << 12);												// Event 0 only happens on a match condition
	LPC_SCT->EV[0].STATE = 0x00000001;												// Event 0 only happens in state 0
	LPC_SCT->LIMIT_U = 0x00000001;													// Event 0 is used as the counter limit
	Chip_SCT_EnableEventInt(LPC_SCT, SCT_EVT_0);									// Enable flag to request an interrupt for Event 0
	modem_busy = true;																// Set modem busy flag
	NVIC_EnableIRQ(SCT_IRQn);														// Enable the interrupt for the SCT
	Chip_SCT_ClearControl(LPC_SCT, SCT_CTRL_HALT_L);								// Start the SCT counter by clearing Halt_L in the SCT control register
}

void modem_flush_frame(void) {
	phase_delta = PHASE_DELTA_1200;
	phase = 0;
	packet_pos = 0;
	current_sample_in_baud = 0;

	if(gpsIsOn())
		GPS_hibernate_uart();				// Hibernate UART because it would interrupt the modulation
	Modem_Init();							// Initialize timers and radio

	while(modem_busy)						// Wait for radio getting finished
		__WFI();

	radioShutdown();						// Shutdown radio
	if(gpsIsOn())
		GPS_wake_uart();					// Init UART again to continue GPS decoding
}

/**
 * Interrupt routine which is called <PLAYBACK_RATE> times per second.
 * This method is supposed to load the next sample into the PWM timer.
 */
void SCT_IRQHandler(void) {
	// If done sending packet
	if(packet_pos == modem_packet_size) {
		Chip_SCT_SetControl(LPC_SCT, SCT_CTRL_HALT_L);	// Stop the SCT counter by setting Halt_L in the SCT control register
		Chip_SCT_ClearEventFlag(LPC_SCT, SCT_EVT_0);	// Clear interrupt
		modem_busy = false;								// Set modem busy flag
		return;											// Done
	}

	// If sent SAMPLES_PER_BAUD already, go to the next bit
	if (current_sample_in_baud == 0) {    // Load up next bit
		if ((packet_pos & 7) == 0) {          // Load up next byte
			current_byte = modem_packet[packet_pos >> 3];
		} else {
			current_byte = current_byte / 2;  // ">>1" forces int conversion
		}

		if ((current_byte & 1) == 0) {
			// Toggle tone (1200 <> 2200)
			phase_delta ^= (PHASE_DELTA_1200 ^ PHASE_DELTA_2200);
		}
	}

	phase += phase_delta;

	setGPIO((phase >> 7) & 0x1);

	if(++current_sample_in_baud == SAMPLES_PER_BAUD) {
		current_sample_in_baud = 0;
		packet_pos++;
	}

	Chip_SCT_ClearEventFlag(LPC_SCT, SCT_EVT_0); // Clear interrupt
}
