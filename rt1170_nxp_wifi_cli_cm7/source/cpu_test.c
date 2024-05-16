/*
 * cpu_test.c
 *
 *  Created on: 2024年2月1日
 *      Author: Matthew
 */
#include "fsl_debug_console.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


unsigned int os_get_timestamp(void);
/* returns time in micro-secs since time began */
#define N   1000

double a[N], b[N], c[N];
//__attribute__((section("BOARD_SDRAM"))) uint8_t cpu_test_data[1024*1000];


int cpu_test(void)
{
    int i, j;

    uint32_t start, end;

    // 初始化数组
    for (i = 0; i < N; i++) {
        a[i] = rand() / (double) RAND_MAX;
        b[i] = rand() / (double) RAND_MAX;
        c[i] = 0.0;
        //cpu_test_data[0]++;
    }

    // 计时开始
    start = os_get_timestamp();

    // 执行计算操作
    for (i = 0; i < N*10; i++) {
        for (j = 0; j < N*10; j++) {
            c[i%N] += a[j%N] * b[(i-j)%N];
        }
    }

    // 计时结束
    end = os_get_timestamp();

    PRINTF("start: %u    end: %u\r\n", start, end);
    PRINTF("Total time: %u  u seconds\r\n", (end - start));

    return 0;
}
