---
title: "feat: Add IPv6 dual-stack support to nic_tester"
type: feat
status: active
date: 2026-07-09
---

## Summary

Add full IPv6 support to the nic_tester PHP extension — dual-family NIC enumeration, dual-stack AF_INET6 socket connections with IPv4-mapped addressing for cross-family reachability, and a `{name, ipv4: [...], ipv6: [...]}` per-interface return format. The single-threaded epoll/select architecture, PHP 5.6.40 Zend API, and strict-unicast-target constraint are preserved.

## Problem Frame

The current nic_tester extension operates exclusively on IPv4: `AF_INET` sockets, `sockaddr_in` structures, `INET_ADDRSTRLEN` buffers, and NIC enumeration that skips `AF_INET6` addresses. Passing an IPv6 target to `nic_tester_check()` fails immediately at `inet_pton(AF_INET, ...)`. On modern dual-stack hosts, this means a significant portion of network interfaces are invisible to the tool, and IPv6-only target hosts are untestable. The user needs to know which interfaces — regardless of address family — can reach a given target.

## Requirements

**Connectivity**
- R1. NIC enumeration collects both IPv4 and IPv6 unicast addresses from all operational interfaces.
- R2. Connection tests use `AF_INET6` dual-stack sockets (`IPV6_V6ONLY=0`) as the unified socket type.
- R3. IPv4 targets are expressed as IPv4-mapped IPv6 addresses (`::ffff:a.b.c.d`) so a single socket type handles both families.
- R4. Cross-family reachability is kernel-decided: the code attempts every NIC-to-target combination without special-casing "this won't work." The kernel's routing and address-selection layers are the authority.
- R5. `epoll` (Linux) / `select` (Windows) I/O multiplexing is unchanged — it operates on file descriptors, not addresses.

**Return value**
- R6. Each successful interface is returned as `{name, ipv4: [...], ipv6: [...]}`, where `ipv4` and `ipv6` are arrays of address strings present on that interface.
- R7. Only interfaces where at least one connection attempt succeeded are included in the return value (no failure entries).

**Input validation**
- R8. The target must be a strict standard unicast address — not a CIDR range, not multicast, not loopback, not the unspecified address (`0.0.0.0` / `::`).
- R9. `inet_pton()` is the primary syntactic validator; semantic checks reject non-unicast address classes.

**Compatibility**
- R10. PHP 5.6.40 Zend API: `zend_parse_parameters(... TSRMLS_CC, "s|ld")`, stack-allocated `zval` with `&` references, no PHP 7+ types.
- R11. Build system (`config.m4`, `config.w32`) requires no new external dependencies — IPv6 support is in the standard platform sockets library.
- R12. The existing function signature `nic_tester_check($target_ip, $port = 80, $timeout = 3.0)` is unchanged.

## Key Technical Decisions

- **Unified AF_INET6 dual-stack socket with IPv4 fallback.** Every connection attempt uses `AF_INET6` with `IPV6_V6ONLY=0`. IPv4 targets are converted to `::ffff:x.x.x.x` mapped form. This eliminates family-specific code paths in the connection layer — one socket type, one connect flow, one epoll/select wait. **Fallback:** If `socket(AF_INET6, ...)` fails with `EAFNOSUPPORT` (kernel compiled without IPv6, `ipv6.disable=1`) and the validated target family is `AF_INET`, fall back to `socket(AF_INET, ...)` with a native `sockaddr_in` destination. This preserves the unified dual-stack path as primary while preventing a regression on IPv6-disabled systems where IPv4 connectivity currently works. (Rationale: Linus-style "one mechanism, not two" for the common case; practical fallback for the edge case. The kernel already implements dual-stack on any modern system; the fallback is a narrow guard, not a parallel architecture.)

- **Per-interface address grouping.** The current flat array (one entry per IPv4 address, interface name repeated) becomes a grouped structure keyed by interface name. Addresses accumulate into per-family arrays as enumeration discovers them. (Rationale: the user asked for `{name, ipv4: [...], ipv6: [...]}` — interface identity is the natural grouping key, and a single interface with both IPv4 and IPv6 should produce one result entry, not two.)

- **Kernel-decided reachability.** The code does not pre-filter "impossible" combinations (e.g., IPv6-only NIC → IPv4 target). It creates the socket, binds to the NIC address, issues connect, and lets the kernel succeed or fail. The only handling is at the result level: success mark or skip. (Rationale: policy lives in the kernel routing table and `gai.conf`, not in application code. Attempting to predict reachability duplicates kernel logic and gets it wrong when NAT64, 6to4, or other translators are present.)

- **Strict unicast validation at parse time.** Before any socket work, the target is validated with `inet_pton()` + semantic checks (not multicast, not unspecified, not loopback). Invalid targets return `FALSE` immediately with no socket allocation. (Rationale: fail-fast at the PHP boundary rather than diagnosing a bad address after enumerating NICs and creating sockets.)

- **Source: `nic_tester.c.new` as the implementation base.** The `.new` file uses stack-allocated `zval` with `&` reference passing — the correct PHP 5.6 pattern. The current `nic_tester.c` uses heap-allocated `MAKE_STD_ZVAL` from the PHP 5.3/5.4 era. All IPv6 changes go into the `.new` pattern, and the final file replaces `nic_tester.c`. The module version is bumped from `1.0.0` to `2.0.0` to signal the breaking return-format change (`{name, ip}` → `{name, ipv4: [...], ipv6: [...]}`).

- **IPv4-mapped address construction via direct `s6_addr` byte manipulation.** Mapping `a.b.c.d` to `::ffff:a.b.c.d` is done by writing the well-known prefix into `s6_addr[10] = 0xFF, s6_addr[11] = 0xFF` and copying the 4 IPv4 octets into `s6_addr[12..15]`, with `s6_addr[0..9]` zeroed. This avoids a temporary 64-byte string buffer and an extra `inet_pton` call per mapped address. (Rationale: direct, allocation-free, and trivially portable — the IPv4-mapped prefix is defined by RFC 4291 and never changes.)

## Scope Boundaries

### In scope
- IPv4 and IPv6 NIC enumeration on Linux and Windows
- Dual-stack socket creation, bind, and non-blocking connect
- Cross-family connectivity (IPv6 NIC → IPv4 target, IPv4 NIC → IPv6 target) via kernel dual-stack
- Per-interface grouped return format with address arrays
- Target address validation (strict unicast)
- PHP 5.6.40 Zend API compatibility throughout

### Out of scope
- UDP, ICMP, or any protocol other than TCP
- CIDR ranges, subnet scanning, or multi-target batching
- Link-local address filtering as a configurable option (link-local addresses are enumerated and tested; the kernel decides reachability)
- PHP 7.x or PHP 8.x API migration (the PHP 5.6 API is a hard constraint)
- Connection success metrics, timing statistics, or per-address latency reporting

### Deferred to Follow-Up Work
- PHP 7+ compatibility layer (replace `TSRMLS_CC` with `ZEND_ARG_INFO`, migrate to `zend_string`)

## Implementation Units

### U1. Data structures and target address validation

- **Goal:** Replace the IPv4-only `nic_info_t` with a multi-address structure grouped by interface name. Add target address parsing and validation that determines the address family and rejects non-unicast inputs.
- **Requirements:** R8, R9
- **Dependencies:** None (foundational unit)
- **Files:**
  - `nic_tester.c.new` — modify `nic_info_t` struct and add validation helper
- **Approach:**
  1. Define `MAX_IPS_PER_NIC` (8) and use `INET6_ADDRSTRLEN` (46) for all address buffers.
  2. New `nic_info_t`: `char name[IFNAMSIZ]`, `char ipv4[MAX_IPS_PER_NIC][INET6_ADDRSTRLEN]`, `int ipv4_count`, `char ipv6[MAX_IPS_PER_NIC][INET6_ADDRSTRLEN]`, `int ipv6_scope_ids[MAX_IPS_PER_NIC]`, `int ipv6_count`, `int sock`, `int success`. The `INET6_ADDRSTRLEN`-sized buffer for IPv4 addresses wastes a few bytes but avoids two buffer-size constants — simplicity over micro-optimization. The `ipv6_scope_ids[]` array stores the `sin6_scope_id` (interface index) for each IPv6 address; this is required for `bind()`/`connect()` with link-local addresses (`fe80::/10`).
  3. Add `static int validate_target(const char *target_ip, int *family)` that:
     - Tries `inet_pton(AF_INET6, target_ip, &in6)`. If success:
       - **IPv4-mapped bypass guard:** Check if `IN6_IS_ADDR_V4MAPPED(&in6)` is true (bytes 0-9 are zero, bytes 10-11 are `0xFF, 0xFF`). If so, extract the embedded IPv4 address from `s6_addr[12..15]` into a `struct in_addr`, convert to string with `inet_ntop(AF_INET, ...)`, then apply the IPv4 validation rules below (not 0.0.0.0, not 127.x, not 224.x/4, not 255.255.255.255). If the embedded IPv4 address is invalid, return 0. If valid, set `*family = AF_INET6` and return 1. This prevents `::ffff:127.0.0.1` and `::ffff:224.0.0.1` from bypassing the IPv4 validation rules.
       - For native (non-mapped) IPv6 addresses: check `!IN6_IS_ADDR_MULTICAST`, `!IN6_IS_ADDR_LOOPBACK`, `!IN6_IS_ADDR_UNSPECIFIED`. Sets `*family = AF_INET6`.
     - Else tries `inet_pton(AF_INET, target_ip, &in4)`. If success, checks not `0.0.0.0`, not `127.0.0.0/8`, not `224.0.0.0/4` (multicast), not `255.255.255.255` (broadcast). Sets `*family = AF_INET`.
     - Else returns 0 (invalid).
  4. On Windows, `IN6_IS_ADDR_*` macros are not available — implement inline with prefix checks against `s6_addr` bytes. For `IN6_IS_ADDR_V4MAPPED`: bytes 0-9 are zero AND bytes 10-11 are `0xFF, 0xFF`.
  5. `MAX_NICS` stays at 64 (reasonable upper bound for interfaces, now grouped).
- **Patterns to follow:** Existing `SAFE_STRNCPY` macro style for any new bounds-checked copies. Existing `#ifdef _WIN32` / `#else` platform fencing pattern.
- **Test scenarios:**
  - Valid IPv4 unicast target (`172.16.84.117`) → `family=AF_INET`, returns 1.
  - Valid IPv6 unicast target (`2001:db8::1`) → `family=AF_INET6`, returns 1.
  - IPv4 multicast (`224.0.0.1`) → returns 0.
  - IPv6 multicast (`ff02::1`) → returns 0.
  - IPv4 loopback (`127.0.0.1`) → returns 0.
  - IPv6 loopback (`::1`) → returns 0.
  - Unspecified (`0.0.0.0`, `::`) → returns 0.
  - Garbage string (`"not-an-address"`) → returns 0.
  - IPv4-mapped IPv6 (`::ffff:172.16.84.117`) → `family=AF_INET6`, returns 1 (valid unicast, embedded IPv4 is clean).
  - IPv4-mapped loopback (`::ffff:127.0.0.1`) → returns 0 (embedded IPv4 is 127.x — loopback).
  - IPv4-mapped multicast (`::ffff:224.0.0.1`) → returns 0 (embedded IPv4 is 224.x — multicast).
  - IPv4-mapped broadcast (`::ffff:255.255.255.255`) → returns 0.
- **Verification:** PHP test calling `nic_tester_check("224.0.0.1", 80)` returns `FALSE`. PHP test calling `nic_tester_check("2001:db8::1", 80)` proceeds to NIC enumeration (returns empty array if no IPv6 route, but does not return FALSE from validation).

---

### U2. Dual-family NIC enumeration

- **Goal:** Collect both IPv4 and IPv6 unicast addresses from all operational interfaces, grouped by interface name, filtering loopback and non-unicast.
- **Requirements:** R1
- **Dependencies:** U1 (needs the new `nic_info_t` struct and `INET6_ADDRSTRLEN`)
- **Files:**
  - `nic_tester.c.new` — rewrite `get_nic_list()`
- **Approach:**
  1. **Interface lookup table.** As addresses are discovered, maintain an in-memory mapping from interface name to `nic_info_t` index. A simple linear scan over `nics[0..count-1]` comparing `name` is sufficient — NIC count is bounded at 64, and this is startup cost, not per-packet.
  2. **Linux path.** Remove the `AF_INET` guard on `ifa->ifa_addr->sa_family`. Accept both `AF_INET` and `AF_INET6`. For each address, look up or create the interface entry, then copy the address string into the appropriate family bucket. Use `IN6_IS_ADDR_LOOPBACK` and `IN6_IS_ADDR_MULTICAST` to skip non-unicast IPv6 addresses. Link-local addresses (`fe80::/10`) are unicast and are included — see the address filtering policy below. **Capture `sin6_scope_id`:** For IPv6 addresses, store `((struct sockaddr_in6*)ifa->ifa_addr)->sin6_scope_id` in `ipv6_scope_ids[]`. This is the interface index required for `bind()`/`connect()` with link-local addresses. Skip `127.0.0.1` and `0.0.0.0` for IPv4.
  3. **Windows path.** Change `GetAdaptersAddresses(AF_INET, ...)` to `GetAdaptersAddresses(AF_UNSPEC, ...)`. The inner unicast-address loop already iterates all families — remove the `AF_INET` guard, handle `AF_INET` and `AF_INET6` branches. For `AF_INET6`, cast to `sockaddr_in6*` and use `inet_ntop(AF_INET6, ...)`. Apply the same non-unicast filtering as Linux.
  4. **Address filtering policy.** Skip: loopback (`127.0.0.1`, `::1`), unspecified (`0.0.0.0`, `::`), multicast (`224.0.0.0/4`, `ff00::/8`). Include: link-local (`169.254.0.0/16`, `fe80::/10`) — these are valid for local-network testing.
  5. **Backward compatibility.** If a system has zero IPv6 addresses (IPv4-only kernel or `ipv6.disable=1`), the function still works — `ipv6_count` remains 0 for all entries, and the return format shows empty `ipv6` arrays.
- **Patterns to follow:** Existing platform `#ifdef` structure, existing `SAFE_STRNCPY` for address string copies, existing `malloc`/`free` pattern in the Windows path.
- **Test scenarios:**
  - Dual-stack Linux host → at least one NIC has both `ipv4_count > 0` and `ipv6_count > 0`.
  - IPv4-only host → all NICs have `ipv6_count == 0`; function still returns valid `ipv4` arrays.
  - Loopback interface (`lo`) → excluded entirely (both `127.0.0.1` and `::1` filtered).
  - Interface with only link-local IPv6 (`fe80::...`) → included, stored in `ipv6[]`.
  - Interface with multicast IPv6 (`ff02::...`) → excluded.
- **Verification:** Compile and load the extension. Call `nic_tester_check("172.16.84.117", 443, 2)` — verify the returned arrays contain `ipv4` and `ipv6` keys (even if `ipv6` is empty). Call with an IPv6 target — verify IPv6 addresses appear in results.

---

### U3. Dual-stack connection layer

- **Goal:** Replace the IPv4-only `test_nics_async()` with a dual-stack implementation that creates `AF_INET6` sockets, binds to NIC addresses, and connects to targets using IPv4-mapped addresses for IPv4 destinations.
- **Requirements:** R2, R3, R4, R5
- **Dependencies:** U1 (data structures), U2 (NIC enumeration)
- **Files:**
  - `nic_tester.c.new` — rewrite `test_nics_async()`
- **Approach:**
  1. **Destination address preparation.** If the validated target family is `AF_INET`, build a `sockaddr_in6` destination with the IPv4-mapped form: `::ffff:<target>`. If `AF_INET6`, use the native form. `sin6_port` is `htons(port)` in both cases. `sin6_family` is always `AF_INET6`.
  2. **Per-NIC socket creation.** For each NIC in the enumerated list:
     - Try to create one `AF_INET6` socket with `IPV6_V6ONLY=0` (dual-stack, which is the default on most systems but set explicitly for clarity). If `setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, …)` fails, close the socket and skip this NIC — same error handling pattern as a failed `bind()`.
     - Choose a bind address: prefer an address matching the target's family (IPv4 address for IPv4 target, IPv6 for IPv6 target). If none, try the other family. The first successful `bind()` wins.
     - For IPv4 NIC addresses, express the bind as `::ffff:<nic_ipv4>` in a `sockaddr_in6`.
     - For IPv6 NIC addresses, bind natively, setting `sin6_scope_id` from `ipv6_scope_ids[j]` when the address is link-local (required by the kernel for `bind()`/`connect()` with `fe80::/10` addresses).
     - Set non-blocking via `set_nonblocking()`.
     - Issue `connect()` to the prepared destination.
     - Handle `EINPROGRESS` / `WSAEWOULDBLOCK` as today.
  3. **I/O multiplexing.** The epoll/select wait loop is unchanged except for the socket count and the destination structure type. `EPOLLOUT` / `FD_SET` / `getsockopt(SO_ERROR)` logic is identical.
  4. **Cleanup.** Close all sockets after the wait, same as today.
  5. **Simple fallback.** If no address on a NIC can be successfully bound (all `bind()` calls fail), skip that NIC — `sock = -1`, not added to the fd set.
  6. **IPv6-disabled system fallback.** If `socket(AF_INET6, ...)` fails with `EAFNOSUPPORT` and the validated target family is `AF_INET`, fall back to `socket(AF_INET, SOCK_STREAM, 0)` with a native `sockaddr_in` destination. The NIC bind address for this fallback uses the NIC's first IPv4 address (bypassing the `::ffff:` mapping). All subsequent connect/epoll/select logic is identical — only the socket domain and destination structure differ.
- **Patterns to follow:**
  - Existing non-blocking connect pattern with `EINPROGRESS` / `WSAEWOULDBLOCK`.
  - Existing epoll (`epoll_create1` + `epoll_ctl` + `epoll_wait`) and select (`FD_ZERO` + `FD_SET` + `select` + `FD_ISSET`) platform blocks, transplanted without structural changes.
  - Existing `close_socket()` / `close()` cleanup loop.
- **Test scenarios:**
  - IPv4 target + dual-stack NIC → connection succeeds via dual-stack socket with `::ffff:target`.
  - IPv6 target + IPv6-capable NIC → connection succeeds with native IPv6.
  - IPv4 target + IPv6-only NIC → kernel decides; typically fails with `ENETUNREACH`; NIC not returned.
  - IPv6 target + IPv4-only NIC → kernel decides; typically fails; NIC not returned.
  - NIC with both IPv4 and IPv6 → one socket created, binds to best-match family; success if either path works.
  - Timeout scenario → non-blocking connect never completes; NIC not returned (existing behavior preserved).
- **Verification:** On a dual-stack host with a reachable IPv4 target, call `nic_tester_check("172.16.84.117", 443, 2)`. Interfaces with IPv4 addresses appear in the result with `ipv4` and `ipv6` populated. On an IPv6-capable network, call with an IPv6 target and verify IPv6-capable interfaces are returned.

---

### U4. PHP return value format

- **Goal:** Change the PHP function's return from `[{name, ip}]` to `[{name, ipv4: [...], ipv6: [...]}]`, using PHP 5.6 Zend API stack-allocated `zval`.
- **Requirements:** R6, R7, R10
- **Dependencies:** U1, U2, U3 (needs populated `nic_info_t` arrays)
- **Files:**
  - `nic_tester.c.new` — modify `PHP_FUNCTION(nic_tester_check)`
- **Approach:**
  1. After `test_nics_async()` returns, iterate `nics[]`.
  2. For each NIC with `success == 1`:
     - Stack-allocate `zval result, ipv4_arr, ipv6_arr`.
     - `array_init(&result)`, `array_init(&ipv4_arr)`, `array_init(&ipv6_arr)`.
     - `add_assoc_string(&result, "name", nics[i].name, 1)`.
     - Loop `ipv4_count`: `add_next_index_string(&ipv4_arr, nics[i].ipv4[j], 1)`.
     - `add_assoc_zval(&result, "ipv4", &ipv4_arr)`.
     - Loop `ipv6_count`: `add_next_index_string(&ipv6_arr, nics[i].ipv6[j], 1)`.
     - `add_assoc_zval(&result, "ipv6", &ipv6_arr)`.
     - `add_next_index_zval(return_value, &result)`.
  3. Use `add_assoc_zval` (available in PHP 5.6) to nest the address arrays under their keys.
  4. The `name` field uses `AdapterName` (Windows) or `ifa_name` (Linux) as before.
- **Patterns to follow:** Existing `array_init`, `add_assoc_string`, `add_next_index_zval` usage in `nic_tester.c.new:312-318`. The stack-allocated `zval` with `&` references pattern.
- **Test scenarios:**
  - Successful IPv4 connection → result entries have `name`, `ipv4` (non-empty array), `ipv6` (array, may be empty).
  - Successful IPv6 connection → result entries have `name`, `ipv6` (non-empty array), `ipv4` (array, may be empty).
  - Dual-stack NIC that succeeds → both `ipv4` and `ipv6` arrays are non-empty.
  - No successful connections → empty array returned (existing behavior).
  - All NICs fail → empty array returned.
- **Verification:**
  ```php
  $r = nic_tester_check("172.16.84.117", 443, 2);
  // $r[0]["name"] is string
  // $r[0]["ipv4"] is array of strings
  // $r[0]["ipv6"] is array of strings
  // isset($r[0]["ip"]) is FALSE (old key removed)
  ```

---

### U5. Tests and documentation

- **Goal:** Update the PHP test file with IPv6 cases and refresh the README to document the new return format and IPv6 capability.
- **Requirements:** R11, R12
- **Dependencies:** U1-U4 (implementation must be complete)
- **Files:**
  - `tests/001.phpt` — update expected output and add IPv6 test cases
  - `tests/test.php` — add IPv6 target and format-verification calls
  - `README.md` — update function description, return format, and examples
- **Approach:**
  1. **README.md.** Rewrite the "功能描述" section to mention IPv4/IPv6 dual-stack. Add a "返回值格式" section showing the new structure with example output. Add an IPv6 target example alongside the existing IPv4 example.
  2. **tests/test.php.** Add `nic_tester_check("::1", 80, 1)` call (loopback should return empty — validation rejects `::1`). Add verification that result keys are `name`/`ipv4`/`ipv6`.
  3. **tests/001.phpt.** Keep the extension-presence check. Add a `--FILE--` block that calls `nic_tester_check` with a valid IPv4 target and verifies the return is an array with the new key structure.
- **Patterns to follow:** Existing README style (Chinese descriptions, code blocks with ` ``` `). Existing phpt format.
- **Test scenarios:**
  - README renders correctly with new examples.
  - `test.php` runs without PHP errors and demonstrates new return format.
  - `001.phpt` passes with `make test` (or `run-tests.php`).
- **Verification:** Run `php test.php` and confirm output shows the new `{name, ipv4, ipv6}` structure. Run `php run-tests.php tests/001.phpt` and confirm pass.

## Sources / Research

- **Base file:** `nic_tester.c.new` — uses stack-allocated `zval` with `&` (PHP 5.6-correct), the correct base for IPv6 changes.
- **Existing platform patterns:** `nic_tester.c:60-69` (set_nonblocking), `nic_tester.c:135-280` (test_nics_async epoll/select structure), `nic_tester.c:282-321` (PHP_FUNCTION).
- **Build system:** `config.m4` (Linux, no changes needed), `config.w32:13-14` (already links `ws2_32.lib` + `iphlpapi.lib` — sufficient for IPv6).
- **PHP 5.6 Zend API:** `zend_parse_parameters(… TSRMLS_CC, "s|ld")`, stack `zval` with `array_init(&z)`, `add_assoc_string(&z, …)`, `add_assoc_zval(&z, …)`, `add_next_index_zval(return_value, &z)`.
- **Linux dual-stack:** `AF_INET6` + `IPV6_V6ONLY=0` (default) enables IPv4-mapped address reachability. `IN6_IS_ADDR_*` macros from `<netinet/in.h>`.
- **Windows dual-stack:** `GetAdaptersAddresses(AF_UNSPEC, ...)` for dual-family enumeration. No `IN6_IS_ADDR_*` macros — manual `s6_addr` byte checks required.
- **Address validation reference:** RFC 4291 (IPv6 addressing), RFC 5735 (IPv4 special-use addresses).
