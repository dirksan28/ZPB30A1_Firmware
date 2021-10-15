#include "load.h"
#include "config.h"
#include "adc.h"
#include "settings.h"
#include "inc/stm8s_tim1.h"

/* integrated values */
uint32_t mAmpere_seconds = 0;
uint32_t mWatt_seconds = 0;

bool load_active = 0;
bool load_regulated = 0;
uint16_t current_setpoint;

/* Which condition disabled the load (user, cutoff or error) */
uint8_t load_disable_reason = DISABLE_USER;

error_t error = ERROR_NONE;

/* Calibration mode (unfinished) */
calibration_t calibration_step;
uint16_t calibration_value;

void load_init()
{
    #define PWM_RELOAD (F_CPU / F_PWM)
    // I-SET
    // Hardware low pass is <8 Hz, so we can use full 16 bit resolution (~244Hz).
    TIM1->ARRH = 0xff;
    TIM1->ARRL = 0xff;
    TIM1->PSCRH = 0;
    TIM1->PSCRL = 0;

    TIM1->CCMR1 = TIM1_OCMODE_PWM1 | TIM1_CCMR_OCxPE;
    TIM1->CCER1 = TIM1_CCER1_CC1E;
    TIM1->CCR1H = 0;
    TIM1->CCR1L = 0;
    TIM1->CR1 = TIM1_CR1_CEN;
    TIM1->BKR = TIM1_BKR_MOE;
}

void load_disable(uint8_t reason)
{
    load_disable_reason = reason;
    load_active = 0;
    GPIOE->ODR |= PINE_ENABLE;
}

void load_enable()
{
    load_active = 1;
    //actual activation happens in load_update()
}

static inline void load_update()
{
    uint16_t setpoint = settings.setpoints[settings.mode];
    uint16_t current = 0;
    uint16_t voltage = adc_get_voltage();
    static uint16_t last_current = 0;
    static int16_t step_size = 1;
    #define STEP_SIZE_MAX 200

    if (error) {
        load_disable(DISABLE_ERROR);
        return;
    }

    /* Calibration mode */
    if (calibration_step == CAL_CURRENT) {
        TIM1->CCR1H = calibration_value >> 8;
        TIM1->CCR1L = calibration_value & 0xff;
        return;
    }

    switch (settings.mode) {
        case MODE_CC:
            current = setpoint;
            break;
        case MODE_CV:
            if (!load_active) {
                current = CUR_MIN; // Start with minimum current ...
                step_size = STEP_SIZE_MAX/2; // ... and a large positive step size
            } else {
                current = last_current; // Use last current and make adjustments
            }

            if (voltage < setpoint) {
                //Current too high
                /* Find current which corresponds to set voltage with increasing steps.
                 TODO: Step size should depend on error between measured value and setpoint. */
                if (step_size < 0) {
                    if (step_size > -STEP_SIZE_MAX) {
                        step_size--;
                    }
                } else {
                    //Last step was positive
                    step_size = -1;
                }
                if (step_size > 0) {
                    error = ERROR_INTERNAL;
                }
                if (current > CUR_MIN - step_size) {
                    current += step_size / 16;
                } else {
                    current = CUR_MIN;
                }
            } else {
                //Current too low
                if (step_size > 0) {
                    if (step_size < STEP_SIZE_MAX) {
                        step_size++;
                    }
                } else {
                    //Last step was negative
                    step_size = 1;
                }
                if (step_size < 0) {
                    error = ERROR_INTERNAL;
                }
                if (current < CUR_MAX - step_size) {
                    current += step_size / 16;
                } else {
                    current = CUR_MAX;
                }
            }
            last_current = current;
            break;
        case MODE_CR:
            // Fixed point scaling:
            // U[mV]/R[10mOhm]=I[mA]
            // U * 1000 * c / R * 100 = I * 1000
            // => c = 100
            current = (uint32_t)voltage * 100 / setpoint;
            break;
        case MODE_CW:
            // Fixed point scaling:
            // P[mW]/U[mV] = I[mA]
            // P * 1000 * c / U * 1000 = I * 1000
            // => c = 1000
            current = (uint32_t)setpoint * 1000 / voltage;
            break;
    }
    /* NOTE: Here v_load is used directly instead of adc_get_voltage, because
       for the MOSFET's power dissipation only the voltage that reaches the load's
       terminals is relevant. */
    uint16_t current_power_limited = (uint32_t)(POW_ABS_MAX) * 1000 / v_load;
    if (current < CUR_MIN) current = CUR_MIN;
    if (current > CUR_MAX) current = CUR_MAX;
    /* Stay below the current limit in all modes. */
    if (settings.mode != MODE_CC && current > settings.current_limit) current = settings.current_limit;
    if (load_active && (current > current_power_limited)) {
        if (settings.max_power_action == MAX_P_LIM) {
            current = current_power_limited;
        } else {
            error = ERROR_OVERLOAD;
        }
    }
    current_setpoint = current;

    /* Convert current to PWM value */
    uint32_t tmp = current;
    tmp = tmp * LOAD_CAL_M - LOAD_CAL_T;
    TIM1->CCR1H = tmp >> 24;
    TIM1->CCR1L = tmp >> 16;

    /* Check cutoff voltage */
    if (load_active && settings.cutoff_enabled && voltage < settings.cutoff_voltage) {
        load_disable(DISABLE_CUTOFF);
    }

    load_regulated = GPIOC->IDR & PINC_OL_DETECT;

    // Only turn on load if no error condition is present
    if (load_active && (error == ERROR_NONE)) GPIOE->ODR &= ~PINE_ENABLE;
}

/* Update integrated values (Ah and Wh) */
static inline void load_calc_power()
{
    static uint16_t timer = 0;
    static uint8_t power_remainder = 0; //Remainder from last cycle's division
    static uint8_t current_remainder = 0;
    #if F_SYSTICK % F_POWER_CALC != 0
        #error "F_POWER_CALC must be an integer divider of F_SYSTICK"
    #endif
    if (!load_active || !load_regulated) {
        // Don't update readings when the exact values are unknown
        return;
    }
    timer++;
    if (timer == F_SYSTICK/F_POWER_CALC) {
        timer = 0;

        uint32_t power = current_setpoint;
        power *= adc_get_voltage();
        uint16_t power16 = power / 1000;
        power16 += power_remainder; //Keep track of rounding errors
        power_remainder = power16 % F_POWER_CALC;
        mWatt_seconds += power16 / F_POWER_CALC;

        uint16_t current = current_setpoint + current_remainder;
        current_remainder = current % F_POWER_CALC;
        mAmpere_seconds += current / F_POWER_CALC;
    }
}

void load_timer()
{
    load_calc_power();
    // Load updates always run at maximum frequency
    load_update();
}
