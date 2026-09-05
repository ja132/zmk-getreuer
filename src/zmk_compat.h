/*
 * Small compatibility shim between upstream ZMK and forks such as MoErgo's
 * Glove80 ZMK. Upstream renamed zmk_endpoints_send_report() to
 * zmk_endpoint_send_report() in 2026; ZMK_ENDPOINT_NONE_COUNT only exists in
 * the renamed version.
 */
#pragma once
#include <zmk/endpoints.h>

#ifdef ZMK_ENDPOINT_NONE_COUNT
#define ZMK_COMPAT_SEND_REPORT(page) zmk_endpoint_send_report(page)
#else
#define ZMK_COMPAT_SEND_REPORT(page) zmk_endpoints_send_report(page)
#endif
