/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_debug_console.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "fsl_gpt.h"
#include "task.h"
#include <cli.h>
#include <cli_utils.h>
#include <stdio.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define GPT_IRQ_ID             GPT2_IRQn
#define EXAMPLE_GPT            GPT2
#define EXAMPLE_GPT_IRQHandler GPT2_IRQHandler

/* Get source clock for GPT driver (GPT prescaler = 0) */
#define EXAMPLE_GPT_CLK_FREQ CLOCK_GetFreq(kCLOCK_OscRc48MDiv2)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*******************************************************************************
 * Variables
 ******************************************************************************/

volatile unsigned int g_second = 0;
volatile uint32_t g_count = 0;
/*******************************************************************************
 * Code
 ******************************************************************************/
void EXAMPLE_GPT_IRQHandler(void)
{
    /* Clear interrupt flag.*/
    GPT_ClearStatusFlags(EXAMPLE_GPT, kGPT_OutputCompare1Flag);

    //gptIsrFlag = true;
    g_count++;
    //if (g_count>10000) {
    //	g_count = 0;
    //	g_second ++;
    //}
/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F, Cortex-M7, Cortex-M7F Store immediate overlapping
  exception return operation might vector to incorrect interrupt */
    SDK_ISR_EXIT_BARRIER;
}

/*!
 * @brief Main function
 */
void gsd_timer_init(void)
{
    uint32_t gptFreq;
    gpt_config_t gptConfig;

    /* Board pin, clock, debug console init */
    GPT_GetDefaultConfig(&gptConfig);

    /* Initialize GPT module */
    GPT_Init(EXAMPLE_GPT, &gptConfig);

    /* Divide GPT clock source frequency by 3 inside GPT module */
    GPT_SetClockDivider(EXAMPLE_GPT, 3);

    /* Get GPT clock frequency */
    gptFreq = EXAMPLE_GPT_CLK_FREQ;

    /* GPT frequency is divided by 3 inside module */
    gptFreq /= 30000;

    /* Set both GPT modules to 100 u-second duration */
    GPT_SetOutputCompareValue(EXAMPLE_GPT, kGPT_OutputCompare_Channel1, gptFreq);

    /* Enable GPT Output Compare1 interrupt */
    GPT_EnableInterrupts(EXAMPLE_GPT, kGPT_OutputCompare1InterruptEnable);

    /* Enable at the Interrupt */
    EnableIRQ(GPT_IRQ_ID);

    //PRINTF("\r\nPress any key to start the example");
    //GETCHAR();

    /* Start Timer */
   // PRINTF("\r\nStarting GPT timer ...");
    GPT_StartTimer(EXAMPLE_GPT);
}

unsigned int gsd_get_time(void)
{
	//PRINTF("time: %d\r\n", g_count);
	return g_count;
}
#define MAX_TASK_NUM        20
unsigned int g_have_get_task_state = 0;
TaskStatus_t pxTaskStatusArray[MAX_TASK_NUM];
configRUN_TIME_COUNTER_TYPE pxTaskStatusArray_before[MAX_TASK_NUM];
configRUN_TIME_COUNTER_TYPE g_total_count = 0;




void get_task_state(int argc, char **argv)
{
    const char task_state[]={'r','R','B','S','D'};
    volatile UBaseType_t uxArraySize, x;
    uint32_t ulTotalRunTime,ulStatsAsPercentage;

    /* 获取任务总数目 */
    uxArraySize = uxTaskGetNumberOfTasks();
   if(uxArraySize>MAX_TASK_NUM)
    {
        PRINTF(("当前任务数量过多！\n"));
    }

    /*获取每个任务的状态信息 */
    uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime );
	

    #if (configGENERATE_RUN_TIME_STATS==1)

    PRINTF(("任务名            状态  ID    优先级  堆栈    CPU使用率\n"));

    /* 避免除零错误 */
    if( ulTotalRunTime > 0 )
    {
    	//ulTotalRunTime = ulTotalRunTime - idle_time;
        /* 将获得的每一个任务状态信息部分的转化为程序员容易识别的字符串格式 */
		if (g_have_get_task_state == 0) {
	        for( x = 0; x < uxArraySize; x++ ) {
	            char tmp[128];

	            /* 计算任务运行时间与总运行时间的百分比。*/
	            ulStatsAsPercentage =(uint64_t)(pxTaskStatusArray[ x ].ulRunTimeCounter)*100 / ulTotalRunTime;

	            if( ulStatsAsPercentage > 0UL ){

	               sprintf(tmp,"%-18s%-6c%-6d%-8d%-8d%d \r\n",pxTaskStatusArray[ x].pcTaskName,task_state[pxTaskStatusArray[ x ].eCurrentState],
	                                                                       pxTaskStatusArray[ x ].xTaskNumber,pxTaskStatusArray[ x].uxCurrentPriority,
	                                                                       pxTaskStatusArray[ x ].usStackHighWaterMark,pxTaskStatusArray[ x ].ulRunTimeCounter );
	            } else {
	                /* 任务运行时间不足总运行时间的1%*/
	                sprintf(tmp,"%-18s%-6c%-6d%-8d%-8d\r\n",pxTaskStatusArray[x ].pcTaskName,task_state[pxTaskStatusArray[ x ].eCurrentState],
	                                                                       pxTaskStatusArray[ x ].xTaskNumber,pxTaskStatusArray[ x].uxCurrentPriority,
	                                                                       pxTaskStatusArray[ x ].usStackHighWaterMark);
	            }
				
	            PRINTF(("%s\n",tmp));
	        }
		} else {
			for( x = 0; x < uxArraySize; x++ ) {
	            char tmp[128];
	            if (pxTaskStatusArray[ x ].ulRunTimeCounter < pxTaskStatusArray_before[pxTaskStatusArray[ x ].xTaskNumber]) {
	            	ulStatsAsPercentage = 0;
	            } else {
	            	ulStatsAsPercentage = (uint64_t)(pxTaskStatusArray[ x ].ulRunTimeCounter - pxTaskStatusArray_before[pxTaskStatusArray[ x ].xTaskNumber])*100/(ulTotalRunTime - g_total_count);

	            }
				
	           sprintf(tmp,"%-18s%-6c%-6d%-8d%-8d%d%%\r\n", pxTaskStatusArray[ x].pcTaskName, task_state[pxTaskStatusArray[ x ].eCurrentState],
	                                                                       pxTaskStatusArray[ x ].xTaskNumber, pxTaskStatusArray[ x].uxCurrentPriority,
	                                                                       pxTaskStatusArray[ x ].usStackHighWaterMark, ulStatsAsPercentage);
	        
				
	            PRINTF(("%s\n",tmp));
			}
		}
    }
    PRINTF(("\r\n任务状态:   r-运行  R-就绪  B-阻塞  S-挂起  D-删除 %d \n"), ulTotalRunTime - g_total_count);
	if (g_have_get_task_state == 0) {
		g_have_get_task_state = 1;
	}
	for( x = 0; x < uxArraySize; x++ ) {
		pxTaskStatusArray_before[pxTaskStatusArray[ x ].xTaskNumber] = pxTaskStatusArray[x].ulRunTimeCounter;
		//PRINTF("id:%d %d\r\n", pxTaskStatusArray[ x ].xTaskNumber, pxTaskStatusArray_before[pxTaskStatusArray[ x ].xTaskNumber]);
	}
	g_total_count = ulTotalRunTime;
    #endif //#if (configGENERATE_RUN_TIME_STATS==1)
}

static struct cli_command task_cmd[] = {
    {"task", "", get_task_state},
};

void gsd_task_cmd_init(void)
{
	cli_register_command(&task_cmd[0]);
}
