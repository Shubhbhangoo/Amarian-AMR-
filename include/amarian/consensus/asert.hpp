#pragma once

/// \file
/// The difficulty-retargeting rule: ASERT, anchored at genesis.
///
/// Amarian retargets on every block, against an absolute schedule measured from
/// genesis, rather than over a sliding window of recent blocks. A window retarget
/// is what creates the timewarp vector, because the rule only inspects timestamps
/// at the edges of the window; retargeting every block against a fixed schedule
/// leaves no boundary to exploit and no accumulated drift. This is the family of
/// algorithms known as ASERT ("Absolutely Scheduled Exponentially Rising
/// Targets"), and the integer arithmetic below is the published aserti3-2d form:
/// a 16-bit fixed-point exponent and a cubic polynomial approximation of `2^x`,
/// chosen by the Bitcoin Cash upgrade specification that fielded it so that a
/// control-system feedback loop gets a monotonic transfer function with no abrupt
/// changes and error below the per-block statistical noise floor.
///
/// ## The schedule
///
/// A child of a tip `P` at height `h` with header timestamp `t` must carry the
/// target
///
///     target = anchor_target * 2^((t - anchor_time - h * target_spacing) / half_life)
///
/// clamped into `[1, pow_limit]`. The anchor is genesis itself: `anchor_bits` is
/// the genesis `target_bits` and `anchor_time` the genesis timestamp, so the
/// "schedule" is the block times genesis would have produced had every block
/// arrived exactly `target_spacing` seconds after the one before. When the chain
/// is exactly on schedule the exponent is zero and difficulty is unchanged; when
/// the chain is *ahead* of schedule (blocks arriving faster than the target) the
/// exponent is negative, the target shrinks and difficulty rises; behind schedule,
/// the opposite. Difficulty doubles for every `half_life` seconds the chain is
/// ahead of its schedule.
///
/// This is the same fixed-anchor construction as the published aserti3-2d. The
/// one difference is the origin of the count: deployed ASERT measures from the
/// parent of its anchor block (which exists for an upgrade but not for a chain
/// that starts on ASERT), while Amarian's schedule counts from genesis itself, so
/// the very first block, mined exactly one interval after genesis, faces exactly
/// the genesis difficulty rather than a difficulty shifted by a constant factor
/// `2^(-spacing/half_life)`.
///
/// ## Why the difficulty is a function of the tip, not of the new block
///
/// The target a header must carry is computed by the node from the *predecessor*
/// (the evaluation block), never from the header's own timestamp. A miner
/// therefore cannot choose the difficulty they mine at by choosing their new
/// block's timestamp: the timestamp field of the block being built is a claim to
/// be checked, not an input to the rule that sets the difficulty it must meet.
/// The tip's timestamp is already constrained when it was accepted — strictly
/// after the median of the preceding eleven blocks and no more than
/// `MAX_FUTURE_BLOCK_SECONDS` ahead of real time — which bounds how far a single
/// miner can move the difficulty of the next block.
///
/// ## Determinism
///
/// Everything here is integer arithmetic on fixed-width types, with the exact
/// fixed-point and clamping rules of the published algorithm:
///
///  1. `exponent = trunc((time_delta - spacing * height_delta) * 2^16 / half_life)`,
///     truncating toward zero, evaluated in 128-bit arithmetic and saturated at
///     `+-(2^20)` — far beyond any cumulative schedule deviation a real chain can
///     accumulate, and the point where the result has long since saturated at the
///     floor or at the hardest valid target anyway.
///  2. The exponent splits into an integer shift and a 16-bit fraction.
///  3. `2^fraction` is approximated by the published cubic, whose error is below
///     0.013% and which is exactly 1 at 0 and exactly 2 at 1.
///  4. The anchor target is multiplied by that factor, shifted by the integer
///     part, and clamped: zero becomes 1 (the hardest valid target), anything
///     that would overflow 256 bits or exceed the floor becomes the floor.

#include <amarian/consensus/params.hpp>

#include <cstdint>

namespace amarian::consensus {

/// The target bits the next block after a tip must carry, computed by the pure
/// ASERT rule over explicit inputs.
///
/// `anchor_bits`/`anchor_time` are the genesis `target_bits` and timestamp;
/// `height_delta` is the tip's height (genesis is height 0, so the tip's own
/// height is the whole distance from the anchor); `tip_time` is the tip's header
/// timestamp — the evaluation block, whose schedule position decides the next
/// block's difficulty. `spacing_seconds`, `half_life_seconds` and
/// `pow_limit_bits` are the rule's constants.
///
/// Preconditions, all checked or unreachable through the parameter table:
/// `height_delta >= 0`, `spacing_seconds > 0`, `half_life_seconds > 0`,
/// `pow_limit_bits` decodes, and the anchor target is no easier than the floor.
/// The result is a canonical compact encoding, never 0, never easier than
/// `pow_limit_bits`, and identical on every node and every platform.
[[nodiscard]] uint32_t AsertNextBits(uint32_t anchor_bits, int64_t anchor_time,
                                     int64_t height_delta, int64_t tip_time,
                                     int64_t spacing_seconds, int64_t half_life_seconds,
                                     uint32_t pow_limit_bits) noexcept;

/// The target bits a child of a tip at `tip_height` with header time `tip_time`
/// must carry, with the anchor taken from the network's genesis block.
[[nodiscard]] uint32_t AsertNextBits(const ChainParams& params, uint32_t tip_height,
                                     int64_t tip_time) noexcept;

}  // namespace amarian::consensus
