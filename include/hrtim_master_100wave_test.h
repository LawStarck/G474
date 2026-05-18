/**
 * @file hrtim_master_100wave_test.h
 * @brief Isolated test: 100 Master-timed PWM cycles on HRTIM1 TD2 (NUCLEO-G474RE: PB15).
 *
 * Requires: CMSIS device header (stm32g4xx.h), system clock configured, NVIC for HRTIM1 master.
 */
#ifndef HRTIM_MASTER_100WAVE_TEST_H
#define HRTIM_MASTER_100WAVE_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of Master roll-over periods (full PWM cycles on TD2). */
#define HRTIM_M100W_MASTER_CYCLES 100U

/**
 * @brief One-shot init: RCC, GPIO, DLL, Master+TIMD, TD2 crossbar, NVIC.
 *        Does not start counters or enable outputs.
 */
void HRTIM_Master100Wave_Init(void);

/**
 * @brief Start Master+Timer D counters and enable TD2 output (after idle pre-bias).
 */
void HRTIM_Master100Wave_Start(void);

/** Call from @ref HRTIM1_Master_IRQHandler when REP interrupt is used. */
void HRTIM_Master100Wave_MasterIrqHandler(void);

/** Cleared when the burst of @ref HRTIM_M100W_MASTER_CYCLES cycles has finished. */
uint32_t HRTIM_Master100Wave_IsDone(void);

#ifdef __cplusplus
}
#endif

#endif /* HRTIM_MASTER_100WAVE_TEST_H */
