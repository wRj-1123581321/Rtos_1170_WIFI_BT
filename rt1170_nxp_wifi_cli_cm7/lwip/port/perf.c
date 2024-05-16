/*
 * perf.c
 *
 *  Created on: 2024年3月13日
 *      Author: Admin
 */

#include "arch/perf.h"
#include "fsl_debug_console.h"

extern unsigned int gsd_get_time(void);

#define TIME_NUM	102400
struct perf_stat {
	unsigned int start_time[TIME_NUM];
	unsigned int end_time[TIME_NUM];
	unsigned int start_index;
	unsigned int end_index;
} g_perf_stat[5];

extern struct perf_stat __attribute__((section(".gsd.$BOARD_SDRAM"))) g_perf_stat[5];

void perf_start(unsigned int index)
{
	unsigned int start_index = 0;

	start_index = g_perf_stat[index].start_index;
	g_perf_stat[index].start_time[start_index] = gsd_get_time();

	g_perf_stat[index].start_index ++;
	if (g_perf_stat[index].start_index >= TIME_NUM)
		g_perf_stat[index].start_index  = 0;

}


void perf_end(unsigned int index)
{
	unsigned int end_index = g_perf_stat[index].end_index;
	g_perf_stat[index].end_time[end_index] = gsd_get_time();
	g_perf_stat[index].end_index++;
	if (g_perf_stat[index].end_index >= TIME_NUM)
		g_perf_stat[index].end_index  = 0;
}

void printf_perf(void)
{
	uint32_t index = 0;
	for (int i=0; i<5; i++) {
		switch(i){
		case 0:
			PRINTF("udp: \r\n");
			break;
		case 1:
			PRINTF("tcp: \r\n");
			break;
		case 2:
			PRINTF("ip4: \r\n");
			break;
		case 3:
			PRINTF("pbuf: \r\n");
			break;
		case 4:
			PRINTF("test: \r\n");
			break;
		}
		index = g_perf_stat[i].start_index;
		if (index < g_perf_stat[i].end_index)
			index = g_perf_stat[i].end_index;
		/*
		for (int j=0; j<g_perf_stat[i].start_index; j=j+5) {

			PRINTF("%d %d, %d %d, %d %d, %d %d, %d %d\r\n",
					g_perf_stat[i].start_time[j], g_perf_stat[i].end_time[j],
					g_perf_stat[i].start_time[j+1], g_perf_stat[i].end_time[j+1],
					g_perf_stat[i].start_time[j+2], g_perf_stat[i].end_time[j+2],
					g_perf_stat[i].start_time[j+3], g_perf_stat[i].end_time[j+3],
					g_perf_stat[i].start_time[j+4], g_perf_stat[i].end_time[j]+4);

		}
		*/
		for (int j=0; j<index; j++) {
			if ((g_perf_stat[i].end_time[j]-g_perf_stat[i].start_time[j]) > 10) {
				PRINTF("%d %d: %d\r\n",
					g_perf_stat[i].start_time[j], g_perf_stat[i].end_time[j],(g_perf_stat[i].end_time[j]-g_perf_stat[i].start_time[j])
					);
			}
		}
		g_perf_stat[i].start_index = 0;
	}
}
