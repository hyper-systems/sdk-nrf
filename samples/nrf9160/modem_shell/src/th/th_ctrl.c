/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <stdio.h>
#include <assert.h>
#include <shell/shell.h>
#include "utils/str_utils.h"

#if defined(CONFIG_MOSH_IPERF3)
#include <sys/select.h>
#include <iperf_api.h>
#endif
extern const struct shell *shell_global;

#define TH_RESPONSE_BUFFER_SIZE 10240

#define TH_1_STACK_SIZE 6144
#define TH_1_PRIORITY 5
K_THREAD_STACK_DEFINE(th_stack_area_1, TH_1_STACK_SIZE);

#define TH_2_STACK_SIZE 6144
#define TH_2_PRIORITY 5
K_THREAD_STACK_DEFINE(th_stack_area_2, TH_2_STACK_SIZE);

struct k_work_q th_work_q_1;
struct k_work_q th_work_q_2;

struct th_ctrl_data {
	const struct shell *shell;
	struct k_work work;
	struct k_poll_signal kill_signal;
	char *results_str;
	char **argv;
	size_t argc;
	uint16_t cmd_len;
	uint8_t th_nbr;
	char *cmd_str;
	bool background;
};

static struct th_ctrl_data th_work_data_1;
static struct th_ctrl_data th_work_data_2;

static char *th_ctrl_get_command_str_from_argv(size_t argc, char **argv,
					       char *out_buf,
					       uint16_t out_buf_len)
{
	int i, total_len = 0, arg_len = 0;

	for (i = 0; i < argc; i++) {
		arg_len = strlen(argv[i]);
		if (total_len + arg_len > out_buf_len) {
			break;
		}
		sprintf(out_buf + total_len, "%s ", argv[i]);
		total_len = strlen(out_buf);
	}
	return out_buf;
}

static char **th_ctrl_util_duplicate_argv(int argc, char **argv)
{
	char **ptr_array;
	int i;

	ptr_array = malloc(sizeof(char *) * argc);
	if (ptr_array == NULL) {
		return NULL;
	}

	for (i = 0; i < argc; i++) {
		ptr_array[i] = mosh_strdup(argv[i]);
		if (ptr_array[i] == NULL) {
			free(ptr_array);
			ptr_array = NULL;
			break;
		}
	}

	return ptr_array;
}

static void th_ctrl_util_duplicate_argv_free(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		free(argv[i]);
	}
	free(argv);
}

static void th_ctrl_work_handler(struct k_work *work_item)
{
	struct th_ctrl_data *data =
		CONTAINER_OF(work_item, struct th_ctrl_data, work);
	int ret;

	shell_print(data->shell, "Starting a thread #%d", data->th_nbr);

	assert(data->argv != NULL);

	if (data->background) {
		assert(data->results_str != NULL);

		ret = iperf_main(data->argc, data->argv, data->results_str,
				 TH_RESPONSE_BUFFER_SIZE, &(data->kill_signal));
	} else {
		assert(data->results_str == NULL);
		ret = iperf_main(data->argc, data->argv, NULL, 0,
				 &(data->kill_signal));
	}

	shell_print(data->shell,
		    "--------------------------------------------------");
	shell_print(data->shell, "iperf_main returned %d from a thread #%d",
		    ret, data->th_nbr);
	if (data->background) {
		shell_print(
			data->shell,
			"Use shell command to print results: \"th results %d\"",
			data->th_nbr);
	}
	shell_print(data->shell,
		    "--------------------------------------------------");
}

static void th_ctrl_data_status_print(const struct shell *shell,
				      struct th_ctrl_data *data)
{
	char *print_buf;

	shell_print(shell, "thread #%d status:", data->th_nbr);
	if (data->results_str != NULL && strlen(data->results_str)) {
		shell_print(shell, "  Results available");
		if (k_work_is_pending(&(data->work))) {
			shell_print(shell, "  thread is running");
		} else {
			shell_print(shell, "  thread is not running");
		}
		print_buf = (char *)calloc(data->cmd_len + 1, sizeof(char));
		shell_print(shell, "  command: %s",
			    th_ctrl_get_command_str_from_argv(
				    data->argc, data->argv, print_buf,
				    data->cmd_len + 1));
		free(print_buf);
	} else {
		shell_print(shell, "  Nothing");
	}
}

void th_ctrl_status_print(const struct shell *shell)
{
	th_ctrl_data_status_print(shell, &th_work_data_1);
	th_ctrl_data_status_print(shell, &th_work_data_2);
}

void th_ctrl_kill_em_all(void)
{
	if (k_work_is_pending(&(th_work_data_1.work))) {
		k_poll_signal_raise(&th_work_data_1.kill_signal, 1);
	}
	if (k_work_is_pending(&(th_work_data_2.work))) {
		k_poll_signal_raise(&th_work_data_2.kill_signal, 2);
	}
}

void th_ctrl_kill(const struct shell *shell, int nbr)
{
	if (nbr == 1) {
		if (k_work_is_pending(&(th_work_data_1.work))) {
			k_poll_signal_raise(&th_work_data_1.kill_signal, 1);
		} else {
			shell_print(shell, "Thread #1 not running");
		}
	} else if (nbr == 2) {
		if (k_work_is_pending(&(th_work_data_2.work))) {
			k_poll_signal_raise(&th_work_data_2.kill_signal, 2);
		} else {
			shell_print(shell, "Thread #2 not running");
		}
	}
}

static void th_ctrl_data_result_print(const struct shell *shell,
				      struct th_ctrl_data *data)
{
	if (data->results_str == NULL || !strlen(data->results_str)) {
		shell_print(shell, "No results for thread #%d", data->th_nbr);
	} else {
		shell_print(shell, "thread #%d results:", data->th_nbr);
		shell_print(shell, "-------------------------------------");
		shell_print(shell, "%s", data->results_str);
		shell_print(shell, "-------------------------------------");

		/* Delete data if the work is done */
		if (!k_work_is_pending(&(data->work))) {
			free(data->results_str);
			data->results_str = NULL;
			shell_print(shell, "Note: th results #%d were deleted.",
				    data->th_nbr);

			/* Clean up for cmd argv */
			th_ctrl_util_duplicate_argv_free(data->argc,
							 data->argv);
			data->argc = 0;
			data->argv = NULL;
		}
	}
}

void th_ctrl_result_print(const struct shell *shell, int nbr)
{
	if (nbr == 1) {
		th_ctrl_data_result_print(shell, &th_work_data_1);
	} else if (nbr == 2) {
		th_ctrl_data_result_print(shell, &th_work_data_2);
	}
}

static void th_ctrl_data_start(struct th_ctrl_data *data,
			       const struct shell *shell, size_t argc,
			       char **argv, bool is_background)
{
	assert(!k_work_is_pending(&(data->work)));

	if (!is_background && data->results_str != NULL) {
		/* Flush and free result "file" for foreground */
		free(data->results_str);
		data->results_str = NULL;
	}

	if (is_background && data->results_str == NULL) {
		data->results_str =
			(char *)calloc(TH_RESPONSE_BUFFER_SIZE, sizeof(char));
		if (data->results_str == NULL) {
			shell_error(
				shell,
				"Cannot start a thread: no memory to store a response");
			return;
		}
	} else if (data->results_str != NULL) {
		/* Clears also previous data if not read before starting a new */
		memset(data->results_str, 0, TH_RESPONSE_BUFFER_SIZE);
	}

	assert(argc > 1);

	if (data->argv != NULL) {
		/* Cleanup from possible previous */
		th_ctrl_util_duplicate_argv_free(data->argc, data->argv);
	}
	data->argc = argc - 1; /* Jump to actual command */
	data->argv = th_ctrl_util_duplicate_argv(argc - 1, &argv[1]);
	if (data->argv == NULL) {
		shell_error(
			shell,
			"Cannot start a thread: no memory for duplicated cmd args");
		return;
	}

	data->background = is_background;
	data->shell = shell;
	data->cmd_len = shell->ctx->cmd_buff_len;

	if (data->th_nbr == 1) {
		k_work_submit_to_queue(&th_work_q_1, &(data->work));
	} else {
		assert(data->th_nbr == 2);
		k_work_submit_to_queue(&th_work_q_2, &(data->work));
	}
}

void th_ctrl_start(const struct shell *shell, size_t argc, char **argv,
		   bool is_background)
{
	/* Only iperf3 currently supported */
	if (strcmp(argv[1], "iperf3") != 0) {
		shell_error(shell, "Only iperf3 is supported currently.");
		return;
	}

	shell_print(shell, "Starting ...");

	if (!k_work_is_pending(&(th_work_data_1.work))) {
		th_ctrl_data_start(&th_work_data_1, shell, argc, argv,
				   is_background);
	} else if (!k_work_is_pending(&(th_work_data_2.work))) {
		th_ctrl_data_start(&th_work_data_2, shell, argc, argv,
				   is_background);
	} else {
		shell_error(shell,
			    "Worker threads are all busy. Try again later.");
	}
}

void th_ctrl_init(void)
{
	k_work_queue_start(&th_work_q_1, th_stack_area_1,
			   K_THREAD_STACK_SIZEOF(th_stack_area_1),
			   TH_1_PRIORITY, NULL);
	k_thread_name_set(&(th_work_q_1.thread), "mosh_bg_1");
	k_work_init(&th_work_data_1.work, th_ctrl_work_handler);
	k_poll_signal_init(&th_work_data_1.kill_signal);
	th_work_data_1.th_nbr = 1;

	k_work_queue_start(&th_work_q_2, th_stack_area_2,
			   K_THREAD_STACK_SIZEOF(th_stack_area_2),
			   TH_2_PRIORITY, NULL);
	k_thread_name_set(&(th_work_q_2.thread), "mosh_bg_2");
	k_work_init(&th_work_data_2.work, th_ctrl_work_handler);
	k_poll_signal_init(&th_work_data_2.kill_signal);
	th_work_data_2.th_nbr = 2;
}
