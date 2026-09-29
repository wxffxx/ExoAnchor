#pragma once

#include <stdbool.h>
#include <errno.h>

/* ESP-IDF 5.5.x can return HTTP_EAGAIN after a hard async TLS write error
 * when a previous non-blocking operation left errno at EAGAIN. Keep the
 * classification pure so host tests can lock the fail-fast boundary without
 * depending on the HTTP client's private state machine. */
static inline bool si_agent_cloud_async_error_is_fatal(int socket_errno,
                                                        int tls_error,
                                                        int tls_code)
{
    bool hard_socket_error =
        socket_errno > 0 && socket_errno != EAGAIN &&
        socket_errno != EWOULDBLOCK && socket_errno != EINPROGRESS;
    return tls_error != 0 || tls_code != 0 || hard_socket_error;
}
