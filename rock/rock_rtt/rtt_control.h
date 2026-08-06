#ifndef RTT_CONTROL_H
#define RTT_CONTROL_H

#include <rtthread.h>

int rtt_control_handle_command(const char *rx, char *tx, rt_size_t tx_size);

#endif
