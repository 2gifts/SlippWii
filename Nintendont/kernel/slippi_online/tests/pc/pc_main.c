/* PC test build of the Wii client: runs the self-test (selftest=1 in DIR's
 * config.ini) against the local test tools and exits with its result.
 *
 *   so_pc_selftest DIR [TIMEOUT_S]
 */
#include <stdio.h>
#include <stdlib.h>
#include "so_client.h"
#include "so_diag.h"
#include "so_platform.h"
#include "so_selftest.h"
#include "pc.h"

/* Thread stacks come from kernel.ld on the Wii; the PC ignores them. */
char __slippi_online_stack_addr, __slippi_online_stack_size;
char __slippi_online_rx_stack_addr, __slippi_online_rx_stack_size;
char __slippi_online_done_stack_addr, __slippi_online_done_stack_size;
char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;
char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;

int main(int argc, char **argv)
{
	u32 timeout_ms, start;
	int res;
	if (argc < 2) {
		fprintf(stderr, "usage: so_pc_selftest DIR [TIMEOUT_S]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	timeout_ms = (u32)(argc > 2 ? atoi(argv[2]) : 300) * 1000;
	so_pc_set_data_dir(argv[1]);
	res = so_client_init();
	if (res < 0) {
		printf("so_client_init failed: %d\n", res);
		return 1;
	}
	if (!so_client_config()->selftest) {
		printf("selftest=1 is not set in %s/config.ini\n", argv[1]);
		return 1;
	}
	start = so_time_ms();
	while (so_selftest_result() < 0) {
		if (so_time_ms() - start > timeout_ms) {
			printf("timeout\n");
			return 1;
		}
		so_sleep_us(100000);
	}
	/* The diagnostics beacon the Wii would broadcast. */
	printf("beacon: [%s]\n", so_diag_beacon());
	printf("beacon: [%s]\n", so_diag_beacon());
	return so_selftest_result() ? 0 : 1;
}
