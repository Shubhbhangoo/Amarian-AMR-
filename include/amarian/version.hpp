#pragma once

/// \file
/// Version and build identification.
///
/// Two distinct things are reported and must not be confused:
///
///   * The *build* identity — version, commit, compiler, dependency versions
///     as resolved at configure time. Used by `--version` and bug reports.
///   * The *protocol* identity — `PROTOCOL_VERSION` and the P2P user agent.
///     These are consensus- and network-visible and change on their own
///     schedule, independent of the release version.
///
/// Bumping the release version must never silently change protocol behaviour,
/// so the two are deliberately separate constants.

#include <cstdint>
#include <string>

namespace amarian {

/// Human-readable release version, e.g. "0.1.0-dev".
[[nodiscard]] std::string VersionString();

/// Release version plus commit, e.g. "0.1.0-dev (a1b2c3d4e5f6)".
/// A build from a modified working tree is marked, because such a binary
/// does not correspond to any published commit.
[[nodiscard]] std::string VersionStringLong();

/// Multi-line build report: compiler, build type, hardening, sanitizers, and
/// the crypto library versions. Runtime library versions are queried from the
/// loaded library rather than assumed from headers, since a node can be running
/// against a different OpenSSL than it compiled against.
[[nodiscard]] std::string BuildInfoString();

/// P2P protocol version. Independent of the release version: it is bumped only
/// when the wire protocol changes in a way peers must negotiate.
inline constexpr uint32_t PROTOCOL_VERSION = 1;

/// Oldest protocol version this build will accept a connection from.
inline constexpr uint32_t MIN_PEER_PROTOCOL_VERSION = 1;

/// User agent advertised in the P2P handshake, BIP-14 style.
/// Kept short: it is attacker-visible and consumes handshake bandwidth.
[[nodiscard]] std::string UserAgent();

}  // namespace amarian
