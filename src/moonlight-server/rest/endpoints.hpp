#pragma once

/**
 * Umbrella header for the Moonlight REST endpoints, split by workflow:
 *   - `endpoints/common.hpp`    shared responses (errors, host IP, /serverinfo)
 *   - `endpoints/pairing.hpp`   the HTTP pairing handshake + HTTPS pin confirmation
 *   - `endpoints/streaming.hpp` app list/asset + launch/resume/cancel
 *
 * `rest/servers.cpp` (the route table) and the pairing tests include this single header.
 */
#include <rest/endpoints/common.hpp>
#include <rest/endpoints/pairing.hpp>
#include <rest/endpoints/streaming.hpp>