#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "c2000ware_libraries.h"
#include "stdio.h"
#include "math.h"

#define PWM_PERIOD 5000 //20khz // calculation 100Mhz/required freaquency
#define ADC_OFFSET 2048 // offset is get by measuring the AC sense signal at zero ac voltage ((that point of DC voltage)/3.3)*4096
//for the pwm scheduler
#define ZC_PERIOD       7813U    // 10 ms  calculation (100MHz*10msec)/128 clk divider

volatile uint32_t FIRE_DELAY_R = 7000; // 8 ms  calculation (100MHz*8msec)/128 clk divider
volatile uint32_t FIRE_WIDTH_R = 390; // 1 ms  calculation (100MHz*1msec)/128 clk divider

volatile uint32_t FIRE_DELAY_Y = 7000; 
volatile uint32_t FIRE_WIDTH_Y = 390;

volatile uint32_t FIRE_DELAY_B = 7000; 
volatile uint32_t  FIRE_WIDTH_B = 390;

// volatile is used since it is modified in interrupt 
volatile uint16_t currentDuty = 2500; //give the half of the pwm period to get the 50% duty cycle // calculation : % * period
//flags and sets
volatile bool rms_flag = false; // to check the rms value is desired range

//for tripping logic in side the gpio and pwm interrupt blocks
volatile uint16_t schedState_R = 0;
volatile uint16_t schedState_Y = 0;
volatile uint16_t schedState_B = 0;

//rms calculation for elevated sin
// 2048 counts = 325V. Scale = 325/2048 = 0.15869
const float VOLTS_PER_COUNT = 0.15869f;
volatile uint32_t sum_squares_ac =0;
volatile float final_rms = 0.0f;

//ADC timer_isr variables
volatile uint16_t raw_cal = 0 ;
volatile uint32_t temp_sum_squares = 0;
volatile uint16_t sample_counter = 0;
volatile bool data_ready_flag = false;//inside the cpu timmer isr
volatile uint16_t flatline_counter = 0;



void initEPWM1_R(void);
void initZVC_R(void);
void initepwm_scheduler_R(void); 

void initEPWM2_Y(void);
void initZVC_Y(void);
void initepwm_scheduler_Y(void);

void initEPWM3_B(void);
void initZVC_B(void);
void initepwm_scheduler_B(void);

void initADC(void);
void initSCIA(void);//uart
void initSamplingTimer(void);

void sendSCIText(char *msg);//uart
void readADC(uint16_t *result1);

__interrupt void xint1_ISR_R(void); //R phase gpio ISR for ZVC
__interrupt void epwmSchedulerISR_R(void);  // this help to shedule the PWM accourding to our desired region for a specific period of time

__interrupt void xint2_ISR_Y(void);
__interrupt void epwmSchedulerISR_Y(void);

__interrupt void xint3_ISR_B(void);
__interrupt void epwmSchedulerISR_B(void);

__interrupt void cpuTimer2_ISR(void);

void main(void)
{
    // Initialize device clock and peripherals
    Device_init();
    // Disable pin locks and enable internal pull-ups.
    Device_initGPIO();
    // Initialize PIE and clear PIE registers. Disables CPU interrupts.
    Interrupt_initModule();
    // Initialize the PIE vector table with pointers to the shell Interrupt
    // Service Routines (ISR).
    Interrupt_initVectorTable();

    Interrupt_register(INT_XINT1, &xint1_ISR_R); 
    Interrupt_register(INT_EPWM4 , &epwmSchedulerISR_R);

    Interrupt_register(INT_XINT2, &xint2_ISR_Y);
    Interrupt_register(INT_EPWM1 , &epwmSchedulerISR_Y);

    Interrupt_register(INT_XINT3 , &xint3_ISR_B);
    Interrupt_register(INT_EPWM2 , &epwmSchedulerISR_B);

    Interrupt_register(INT_TIMER2, &cpuTimer2_ISR);

    // PinMux and Peripheral Initialization
    Board_init();

    initEPWM1_R();
    initZVC_R();
    initepwm_scheduler_R();

    initEPWM2_Y();
    initZVC_Y();
    initepwm_scheduler_Y();

    initEPWM3_B();
    initZVC_B();
    initepwm_scheduler_B();
 
    initADC();
    initSCIA();
    initSamplingTimer();

    // C2000Ware Library initialization
    C2000Ware_libraries_init();
    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    // Enable Global Interrupt (INTM) and real time interrupt (DBGM)
    EINT;
    ERTM;

    // to force pwms of each phase low initially (no pwm at the beginning)
    EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST); 
    EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST); 
    EPWM_forceTripZoneEvent(EPWM3_BASE, EPWM_TZ_FORCE_EVENT_OST); 
   
    //for UART
    char buffer2[100];
   
    while(1)
    {
        if(data_ready_flag == true)
        {
           float mean_square = (float)sum_squares_ac / 200.0f;
           float rms_counts = sqrtf(mean_square);
           final_rms = rms_counts * VOLTS_PER_COUNT;
            if(final_rms >= 180.0f)
            {
                rms_flag = true;
            }
            else if (final_rms < 170.0f)
            {
                rms_flag = false;
                FIRE_DELAY_R = 7000;
                FIRE_WIDTH_R = 390;
                EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
                Interrupt_enable(INT_XINT1);
                Interrupt_enable(INT_EPWM4);

                FIRE_DELAY_Y = 7000;
                FIRE_WIDTH_Y = 390;
                EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
                Interrupt_enable(INT_XINT2);
                Interrupt_enable(INT_EPWM1);

                FIRE_DELAY_B = 7000;
                FIRE_WIDTH_B = 390;
                EPWM_forceTripZoneEvent(EPWM3_BASE, EPWM_TZ_FORCE_EVENT_OST);
                Interrupt_enable(INT_XINT3);
                Interrupt_enable(INT_EPWM2);
            }
            if(rms_flag == true)
            {  
                //if Total Time: 500ms
                //Loop Duration: ~20ms
                //Total Steps: 500 / 20 = 25 iterations
                //Total Reduction: 800,000 counts
                //New Step Size: 800,000 / 25 = 32,000

                if (FIRE_DELAY_R > 141)
                {
                    FIRE_DELAY_R -= 140; // Decrease the delay
                    FIRE_WIDTH_R += 140; // Increase the width
                        EPWM_setCounterCompareValue(EPWM4_BASE, EPWM_COUNTER_COMPARE_A, FIRE_DELAY_R);
                        EPWM_setCounterCompareValue(EPWM4_BASE, EPWM_COUNTER_COMPARE_B, FIRE_DELAY_R + FIRE_WIDTH_R);
                }
                else
                {
                    Interrupt_disable(INT_XINT1);
                    Interrupt_disable(INT_EPWM4);
                    EPWM_setTimeBaseCounterMode(EPWM4_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
                    EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);     // Cap at zero (full power)
                }

                if (FIRE_DELAY_Y > 141)
                {
                    FIRE_DELAY_Y -= 140; // Decrease the delay
                    FIRE_WIDTH_Y += 140; // Increase the width
                        EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_A, FIRE_DELAY_Y);
                        EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_B, FIRE_DELAY_Y + FIRE_WIDTH_Y);
                }
                else
                {
                    Interrupt_disable(INT_XINT2);
                    Interrupt_disable(INT_EPWM1); 
                    EPWM_setTimeBaseCounterMode(EPWM1_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
                    EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);     // Cap at zero (full power)
                }

                if (FIRE_DELAY_B > 141)
                {
                    FIRE_DELAY_B -= 140; // Decrease the delay
                    FIRE_WIDTH_B += 140; // Increase the width
                        EPWM_setCounterCompareValue(EPWM2_BASE, EPWM_COUNTER_COMPARE_A, FIRE_DELAY_B);
                        EPWM_setCounterCompareValue(EPWM2_BASE, EPWM_COUNTER_COMPARE_B, FIRE_DELAY_B + FIRE_WIDTH_B);
                }
                else
                {
                    Interrupt_disable(INT_XINT3);
                    Interrupt_disable(INT_EPWM2);
                    EPWM_setTimeBaseCounterMode(EPWM2_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
                    EPWM_clearTripZoneFlag(EPWM3_BASE, EPWM_TZ_FLAG_OST);     // Cap at zero (full power)
                }

            }
            sprintf(buffer2, "--Sin RMS: %d \r\n \r\n", (int)final_rms); //  
            sendSCIText(buffer2);
            sprintf(buffer2, "--Sin RMS_adc: %d \r\n \r\n", (int)raw_cal); //  
            sendSCIText(buffer2);
            data_ready_flag = false;
        }
  
    }
}
// ISR
__interrupt void xint1_ISR_R(void)
{
    // trip EPWM when zero crossimg detected
    EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
    if(rms_flag == true)
   { schedState_R = 0;
     EPWM_setInterruptSource(EPWM4_BASE, EPWM_INT_TBCTR_U_CMPA);
     EPWM_setTimeBaseCounter(EPWM4_BASE, 0);
     EPWM_setTimeBaseCounterMode(EPWM4_BASE, EPWM_COUNTER_MODE_UP);
    }
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

__interrupt void xint2_ISR_Y(void)
{
    EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    if(rms_flag == true)
   { schedState_Y = 0;
     EPWM_setInterruptSource(EPWM1_BASE, EPWM_INT_TBCTR_U_CMPA);
     EPWM_setTimeBaseCounter(EPWM1_BASE, 0);
     EPWM_setTimeBaseCounterMode(EPWM1_BASE, EPWM_COUNTER_MODE_UP);
    }
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);

}

__interrupt void xint3_ISR_B(void)
{
     EPWM_forceTripZoneEvent(EPWM3_BASE, EPWM_TZ_FORCE_EVENT_OST);
    if(rms_flag == true)
   { schedState_B = 0;
     EPWM_setInterruptSource(EPWM2_BASE, EPWM_INT_TBCTR_U_CMPA);
     EPWM_setTimeBaseCounter(EPWM2_BASE, 0);
     EPWM_setTimeBaseCounterMode(EPWM2_BASE, EPWM_COUNTER_MODE_UP);
    }
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP12);
}

__interrupt void epwmSchedulerISR_R(void)
{
    if (schedState_R == 0)
    {
        // PWM ON
        EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);
        schedState_R = 1;
        EPWM_setInterruptSource(EPWM4_BASE, EPWM_INT_TBCTR_U_CMPB);
    }
    else
    {
        // PWM OFF
        EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
        schedState_R = 0;
        // freeze the scheduler
        EPWM_setTimeBaseCounterMode(EPWM4_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
        EPWM_setInterruptSource(EPWM4_BASE, EPWM_INT_TBCTR_U_CMPA);
    }
    EPWM_clearEventTriggerInterruptFlag(EPWM4_BASE);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP3);
}

__interrupt void epwmSchedulerISR_Y(void)
{
    if (schedState_Y == 0)
    {
        // PWM ON
        EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);
        schedState_Y = 1;
        EPWM_setInterruptSource(EPWM1_BASE, EPWM_INT_TBCTR_U_CMPB);
        
    }
    else
    {
        // PWM OFF
        EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
        schedState_Y = 0;
        // freeze the scheduler
        EPWM_setTimeBaseCounterMode(EPWM1_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
        EPWM_setInterruptSource(EPWM1_BASE, EPWM_INT_TBCTR_U_CMPA);
    }
    EPWM_clearEventTriggerInterruptFlag(EPWM1_BASE);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP3);// nookivechoo
}

__interrupt void epwmSchedulerISR_B(void)
{
    if (schedState_B == 0)
    {
        // PWM ON
        EPWM_clearTripZoneFlag(EPWM3_BASE, EPWM_TZ_FLAG_OST);
        schedState_B = 1;
        EPWM_setInterruptSource(EPWM2_BASE, EPWM_INT_TBCTR_U_CMPB);
    }
    else
    {
        // PWM OFF
        EPWM_forceTripZoneEvent(EPWM3_BASE, EPWM_TZ_FORCE_EVENT_OST);
        schedState_B = 0;
        // freeze the scheduler
        EPWM_setTimeBaseCounterMode(EPWM2_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
        EPWM_setInterruptSource(EPWM2_BASE, EPWM_INT_TBCTR_U_CMPA);
    }
    EPWM_clearEventTriggerInterruptFlag(EPWM2_BASE);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP3);
}

//interrupts that run every 100us that timmer 3
__interrupt void cpuTimer2_ISR(void)
{
    
    int16_t adj;

    // if(flatline_counter > 100) 
    // {
    //     sensor_fault = true;
    //     rms_flag = false; // Force safety shutdown
    //     EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
    //     EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    //     EPWM_forceTripZoneEvent(EPWM3_BASE, EPWM_TZ_FORCE_EVENT_OST);
    //     flatline_counter = 0;
    // }
    uint16_t raw = 0 ;
    readADC(&raw);
    raw_cal = raw;
     if(raw_cal <= 5) 
    {
        flatline_counter++;
    }
    adj = (int16_t)raw - ADC_OFFSET; // here is your offset value
    temp_sum_squares += (uint32_t)((int32_t)adj * (int32_t)adj);
    sample_counter++;
        
    if(sample_counter >= 200 && flatline_counter < 100)
    {
        sum_squares_ac = temp_sum_squares; // Transfer to main variable
        sample_counter = 0;
        temp_sum_squares = 0;
        flatline_counter =0;
        data_ready_flag = true; // Tell main loop to calculate RMS
    }
    else 
    {
     sample_counter = 0;
        temp_sum_squares = 0;
        flatline_counter =0;
    }
    
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}


void readADC(uint16_t *result1)
{
    // Force BOTH SOC0 and SOC1 to start
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER0 );
    while (!ADC_getInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1));
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    *result1 = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER0); // Result of SOC0 (ADCIN6)
}    


//adc initializationfor output and input
void initADC(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_ADCA);
    ADC_setVREF(ADCA_BASE, ADC_REFERENCE_INTERNAL, ADC_REFERENCE_3_3V);
    ADC_setPrescaler(ADCA_BASE, ADC_CLK_DIV_4_0);
    ADC_enableConverter(ADCA_BASE);
    DEVICE_DELAY_US(1000);
    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER0, ADC_TRIGGER_SW_ONLY, ADC_CH_ADCIN6, 15);// look here to know the pins
    ADC_setInterruptSource(ADCA_BASE, ADC_INT_NUMBER1, ADC_SOC_NUMBER0);
   
    ADC_enableInterrupt(ADCA_BASE, ADC_INT_NUMBER1);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
}


// for serial communication UART
void initSCIA(void)
{
    GPIO_setPinConfig(GPIO_28_SCIA_RX);
    GPIO_setPinConfig(GPIO_29_SCIA_TX);
    GPIO_setDirectionMode(28, GPIO_DIR_MODE_IN);
    GPIO_setPadConfig(28, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(28, GPIO_QUAL_ASYNC);
    GPIO_setDirectionMode(29, GPIO_DIR_MODE_OUT);
    GPIO_setPadConfig(29, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(29, GPIO_QUAL_ASYNC);
    SCI_setConfig(SCIA_BASE, DEVICE_LSPCLK_FREQ, 115200, SCI_CONFIG_WLEN_8 | SCI_CONFIG_STOP_ONE | SCI_CONFIG_PAR_NONE);
    SCI_enableFIFO(SCIA_BASE);
    SCI_resetChannels(SCIA_BASE);  
    SCI_resetRxFIFO(SCIA_BASE);
    SCI_resetTxFIFO(SCIA_BASE);
    SCI_enableModule(SCIA_BASE);
    SCI_enableTxModule(SCIA_BASE);
    SCI_enableRxModule(SCIA_BASE);
}
//for serial communication to print
void sendSCIText(char *msg)
{
     while (*msg)
    {
        SCI_writeCharBlockingFIFO(SCIA_BASE, *msg++);
    }
}


void initEPWM1_R(void)
{
    GPIO_setPinConfig(GPIO_10_EPWM6A);
    GPIO_setPadConfig(10, GPIO_PIN_TYPE_STD);

    EPWM_setClockPrescaler(EPWM6_BASE, EPWM_CLOCK_DIVIDER_1, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM6_BASE, PWM_PERIOD);
    EPWM_setTimeBaseCounterMode(EPWM6_BASE, EPWM_COUNTER_MODE_UP);
    EPWM_setPhaseShift(EPWM6_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);

    EPWM_setActionQualifierAction(EPWM6_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_HIGH, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);
    EPWM_setActionQualifierAction(EPWM6_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_UP_CMPA);

    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
    EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);
}


void initEPWM2_Y  (void)
{
    GPIO_setPinConfig(GPIO_8_EPWM5A);
    GPIO_setPadConfig(8, GPIO_PIN_TYPE_STD);

    EPWM_setClockPrescaler(EPWM5_BASE, EPWM_CLOCK_DIVIDER_1, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM5_BASE, PWM_PERIOD);
    EPWM_setTimeBaseCounterMode(EPWM5_BASE, EPWM_COUNTER_MODE_UP);
    EPWM_setPhaseShift(EPWM5_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM5_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);

    EPWM_setActionQualifierAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_HIGH, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);
    EPWM_setActionQualifierAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_UP_CMPA);

    EPWM_setTripZoneAction(EPWM5_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM5_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
    EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);
}

void initEPWM3_B (void)
{
    GPIO_setPinConfig(GPIO_4_EPWM3A);
    GPIO_setPadConfig(4, GPIO_PIN_TYPE_STD);

    EPWM_setClockPrescaler(EPWM3_BASE, EPWM_CLOCK_DIVIDER_1, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM3_BASE, PWM_PERIOD);
    EPWM_setTimeBaseCounterMode(EPWM3_BASE, EPWM_COUNTER_MODE_UP);
    EPWM_setPhaseShift(EPWM3_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM3_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);

    EPWM_setActionQualifierAction(EPWM3_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_HIGH, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);
    EPWM_setActionQualifierAction(EPWM3_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_UP_CMPA);

    EPWM_setTripZoneAction(EPWM3_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM3_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
    EPWM_clearTripZoneFlag(EPWM3_BASE, EPWM_TZ_FLAG_OST);
}


//sheduler pwm
void initepwm_scheduler_R(void)
{   
    EPWM_setClockPrescaler(EPWM4_BASE, EPWM_CLOCK_DIVIDER_128, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM4_BASE, ZC_PERIOD);
    EPWM_setTimeBaseCounter(EPWM4_BASE, 0);
    EPWM_setTimeBaseCounterMode(EPWM4_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
    EPWM_setPhaseShift(EPWM4_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM4_BASE, EPWM_COUNTER_COMPARE_A, FIRE_DELAY_R);

    EPWM_setCounterCompareValue(EPWM4_BASE, EPWM_COUNTER_COMPARE_B, FIRE_DELAY_R + FIRE_WIDTH_R);
    EPWM_setInterruptSource(EPWM4_BASE, EPWM_INT_TBCTR_U_CMPA);

    EPWM_enableInterrupt(EPWM4_BASE);
    EPWM_setInterruptEventCount(EPWM4_BASE, 1);
    Interrupt_enable(INT_EPWM4);
}

void initepwm_scheduler_Y(void)
{   
    EPWM_setClockPrescaler(EPWM1_BASE, EPWM_CLOCK_DIVIDER_128, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM1_BASE, ZC_PERIOD);
    EPWM_setTimeBaseCounter(EPWM1_BASE, 0);
    EPWM_setTimeBaseCounterMode(EPWM1_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
    EPWM_setPhaseShift(EPWM1_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_A, FIRE_DELAY_Y);

    EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_B, FIRE_DELAY_Y + FIRE_WIDTH_Y);
    EPWM_setInterruptSource(EPWM1_BASE, EPWM_INT_TBCTR_U_CMPA);

    EPWM_enableInterrupt(EPWM1_BASE);
    EPWM_setInterruptEventCount(EPWM1_BASE, 1);
    Interrupt_enable(INT_EPWM1);
}

void initepwm_scheduler_B(void)
{   
    EPWM_setClockPrescaler(EPWM2_BASE, EPWM_CLOCK_DIVIDER_128, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM2_BASE, ZC_PERIOD);
    EPWM_setTimeBaseCounter(EPWM2_BASE, 0);
    EPWM_setTimeBaseCounterMode(EPWM2_BASE, EPWM_COUNTER_MODE_STOP_FREEZE);
    EPWM_setPhaseShift(EPWM2_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM2_BASE, EPWM_COUNTER_COMPARE_A, FIRE_DELAY_B);

    EPWM_setCounterCompareValue(EPWM2_BASE, EPWM_COUNTER_COMPARE_B, FIRE_DELAY_B + FIRE_WIDTH_B);
    EPWM_setInterruptSource(EPWM2_BASE, EPWM_INT_TBCTR_U_CMPA);

    EPWM_enableInterrupt(EPWM2_BASE);
    EPWM_setInterruptEventCount(EPWM2_BASE, 1);
    Interrupt_enable(INT_EPWM2);
}

void initZVC_R(void)
{
    GPIO_setPinConfig(GPIO_12_GPIO12);
    GPIO_setDirectionMode(12, GPIO_DIR_MODE_IN);
    GPIO_setQualificationMode(12, GPIO_QUAL_ASYNC);
    GPIO_setInterruptPin(12, GPIO_INT_XINT1);
    Interrupt_enable(INT_XINT1);
    GPIO_setInterruptType(GPIO_INT_XINT1, GPIO_INT_TYPE_BOTH_EDGES);
    GPIO_enableInterrupt(GPIO_INT_XINT1);
}

void initZVC_Y(void)
{
    GPIO_setPinConfig(GPIO_34_GPIO34);
    GPIO_setDirectionMode(34, GPIO_DIR_MODE_IN);
    GPIO_setQualificationMode(34, GPIO_QUAL_ASYNC);
    GPIO_setInterruptPin(34, GPIO_INT_XINT2);
    Interrupt_enable(INT_XINT2);
    GPIO_setInterruptType(GPIO_INT_XINT2, GPIO_INT_TYPE_BOTH_EDGES);
    GPIO_enableInterrupt(GPIO_INT_XINT2);
}

void initZVC_B(void)
{
    GPIO_setPinConfig(GPIO_33_GPIO33);
    GPIO_setDirectionMode(33, GPIO_DIR_MODE_IN);
    GPIO_setQualificationMode(33, GPIO_QUAL_ASYNC);
    GPIO_setInterruptPin(33, GPIO_INT_XINT3);
    Interrupt_enable(INT_XINT3);
    GPIO_setInterruptType(GPIO_INT_XINT3, GPIO_INT_TYPE_BOTH_EDGES);
    GPIO_enableInterrupt(GPIO_INT_XINT3);
}

void initSamplingTimer(void) {
    // 100MHz / 10,000 = 10kHz (100us)
    CPUTimer_setPeriod(CPUTIMER2_BASE, 0xFFFFFFFF);
    CPUTimer_setPreScaler(CPUTIMER2_BASE, 0);
    CPUTimer_setPeriod(CPUTIMER2_BASE, 10000);
    CPUTimer_stopTimer(CPUTIMER2_BASE);
    CPUTimer_reloadTimerCounter(CPUTIMER2_BASE);
    CPUTimer_enableInterrupt(CPUTIMER2_BASE);
    Interrupt_enable(INT_TIMER2);
    CPUTimer_startTimer(CPUTIMER2_BASE);

}

