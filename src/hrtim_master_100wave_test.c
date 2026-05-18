/**
 * @file hrtim_master_100wave_test.c
 * @brief Isolated register-level test: Master timebase, TD2 = SET@MSTPER / RST@MSTCMP1, stop after 100 periods.
 *
 * Integration: add to build with CMSIS (stm32g4xx.h). Call Init once, Start once.
 * In stm32g4xx_it.c: void HRTIM1_Master_IRQHandler(void) { HRTIM_Master100Wave_MasterIrqHandler(); }
 */
#include "hrtim_master_100wave_test.h"

#include "stm32g4xx.h"

/* ST LL example naming: HRTIM_PRESCALERRATIO_MUL32 maps to CK_PSC = 5 on G4. */
#ifndef HRTIM_M100W_CKPSC
#define HRTIM_M100W_CKPSC (5U)
#endif

/* Timer ticks (16-bit). MCMP1 must be between 3 and PER-3 per RM null-duty rules margin — keep mid-range. */
#ifndef HRTIM_M100W_MPER
#define HRTIM_M100W_MPER (4095U)
#endif
#ifndef HRTIM_M100W_MCMP1
#define HRTIM_M100W_MCMP1 (2048U)
#endif

#define MREP_FOR_N_CYCLES(n) ((uint32_t)((n)-1U))

static volatile uint32_t s_seq_done;

static void rcc_gpio_pb15_hrtim1(void)
{
  RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
  (void)RCC->AHB2ENR;
  RCC->APB2ENR |= RCC_APB2ENR_HRTIM1EN;
  (void)RCC->APB2ENR;

  /* PB15: AF13, very high speed (NUCLEO-G474RE HRTIM1_TD2). */
  GPIOB->MODER &= ~(3U << (15U * 2U));
  GPIOB->MODER |= (2U << (15U * 2U));
  GPIOB->OSPEEDR |= (3U << (15U * 2U));
  GPIOB->PUPDR &= ~(3U << (15U * 2U));
  GPIOB->AFRH &= ~(0xFU << ((15U - 8U) * 4U));
  GPIOB->AFRH |= (13U << ((15U - 8U) * 4U));
}

static void hrtim_wait_dll(void)
{
  /* Continuous calibration, rate 3 (same class as ST LL examples). */
  HRTIM1->sCommonRegs.DLLCR = HRTIM_DLLCR_CALEN | HRTIM_DLLCR_CALRTE_1 | HRTIM_DLLCR_CALRTE_0;
  while ((HRTIM1->sCommonRegs.ISR & HRTIM_ISR_DLLRDY) == 0U) {
  }
}

void HRTIM_Master100Wave_Init(void)
{
  HRTIM_Master_TypeDef *const m = &HRTIM1->sMasterRegs;
  HRTIM_Timerx_TypeDef *const d = &HRTIM1->sTimerxRegs[3]; /* Timer D */

  s_seq_done = 0U;
  rcc_gpio_pb15_hrtim1();
  /* Clear stale fault flags before touching outputs (RM0440 fault stage). */
  HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C | HRTIM_ICR_FLT3C |
                            HRTIM_ICR_FLT4C | HRTIM_ICR_FLT5C | HRTIM_ICR_FLT6C;
  hrtim_wait_dll();

  /* --- Master --- */
  m->MCR = (HRTIM_M100W_CKPSC << HRTIM_MCR_CK_PSC_Pos) | HRTIM_MCR_CONT;
  m->MPER = HRTIM_M100W_MPER;
  m->MCMP1R = HRTIM_M100W_MCMP1;
  m->MREP = MREP_FOR_N_CYCLES(HRTIM_M100W_MASTER_CYCLES);
  m->MDIER = 0U;
  m->MICR = HRTIM_MICR_MREP | HRTIM_MICR_MCMP1 | HRTIM_MICR_MCMP2 | HRTIM_MICR_MCMP3 | HRTIM_MICR_MCMP4;

  /* --- Timer D: same prescaler/continuous; PER matches master for clean alignment (TD2 uses master events only). --- */
  d->TIMxCR = (HRTIM_M100W_CKPSC << HRTIM_TIMCR_CK_PSC_Pos) | HRTIM_TIMCR_CONT;
  d->PERxR = HRTIM_M100W_MPER;
  d->CMP1xR = HRTIM_M100W_MCMP1;
  d->REPxR = 0U;

  d->SETx2R = HRTIM_SET2R_MSTPER;
  d->RSTx2R = HRTIM_RST2R_MSTCMP1;

  d->TIMxDIER = 0U;
  d->TIMxICR = 0xFFFFFFFFU;

  /* Positive polarity, idle = inactive (OUTR defaults ok); explicit idle level bit 19 = 0. */
  d->OUTxR &= ~(HRTIM_OUTR_POL2_Msk | HRTIM_OUTR_IDLES2_Msk | HRTIM_OUTR_IDLM2_Msk);

  /* Load MREP/MPER/MCMP + TIMD shadow into active registers. */
  m->MCNTR = 0U;
  d->CNTxR = 0U;
  HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_MSWU | HRTIM_CR2_TDSWU;

  NVIC_SetPriority(HRTIM1_Master_IRQn, 1U);
  NVIC_EnableIRQ(HRTIM1_Master_IRQn);
}

void HRTIM_Master100Wave_Start(void)
{
  HRTIM_Master_TypeDef *const m = &HRTIM1->sMasterRegs;
  HRTIM_Timerx_TypeDef *const d = &HRTIM1->sTimerxRegs[3];

  s_seq_done = 0U;

  /* Pre-bias TD2 to reset (inactive) before RUN (RM0440 §28.3.14). */
  d->RSTx2R = HRTIM_RST2R_MSTCMP1 | HRTIM_RST2R_SRT;

  m->MDIER = HRTIM_MDIER_MREPIE;

  HRTIM1->sCommonRegs.OENR = HRTIM_OENR_TD2OEN;
  m->MCR |= (HRTIM_MCR_MCEN | HRTIM_MCR_TDCEN);
}

void HRTIM_Master100Wave_MasterIrqHandler(void)
{
  HRTIM_Master_TypeDef *const m = &HRTIM1->sMasterRegs;

  if ((m->MISR & HRTIM_MISR_MREP) != 0U) {
    m->MICR = HRTIM_MICR_MREP;
    m->MDIER &= ~HRTIM_MDIER_MREPIE;

    /* Idle the pin then stop counters (ODIS has priority over OEN). */
    HRTIM1->sCommonRegs.ODISR = HRTIM_ODISR_TD2ODIS;
    m->MCR &= ~(HRTIM_MCR_MCEN | HRTIM_MCR_TDCEN);

    s_seq_done = 1U;
  }
}

uint32_t HRTIM_Master100Wave_IsDone(void)
{
  return s_seq_done;
}
