// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_ERROR_H
#define DFTRACER_UTILS_COMM_ERROR_H

#include <dftracer/utils/dstd/error.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace dftracer::utils::comm {

/// What a channel call fails with, which the conditions of @ref dstd::Condition
/// do not name.
enum class ChannelError : std::int32_t {
    /// A send found the stream of its channel full.
    Full,
    /// A receive was given a buffer shorter than the message it found.
    TooBig,
    /// The sender of the channel closed it.
    Closed,
    /// The two ends opened another channel, or another value size.
    WrongChannel,
    /// A receiver ended a send because its sender asked to cancel it.
    Cancelled,
};

inline constexpr dstd::ErrorDomain COMM_ERROR_DOMAIN =
    dstd::make_error_domain("dftracer.utils.comm");

inline constexpr dstd::ErrorCodeInfo COMM_ERROR_INFOS[] = {
    {&COMM_ERROR_DOMAIN, static_cast<std::int32_t>(ChannelError::Full),
     dstd::Condition::Unknown, "CHANNEL_FULL"},
    {&COMM_ERROR_DOMAIN, static_cast<std::int32_t>(ChannelError::TooBig),
     dstd::Condition::InvalidArgument, "CHANNEL_TOO_BIG"},
    {&COMM_ERROR_DOMAIN, static_cast<std::int32_t>(ChannelError::Closed),
     dstd::Condition::Unknown, "CHANNEL_CLOSED"},
    {&COMM_ERROR_DOMAIN, static_cast<std::int32_t>(ChannelError::WrongChannel),
     dstd::Condition::InvalidArgument, "CHANNEL_WRONG_CHANNEL"},
    {&COMM_ERROR_DOMAIN, static_cast<std::int32_t>(ChannelError::Cancelled),
     dstd::Condition::Cancelled, "CHANNEL_CANCELLED"},
};

constexpr const dstd::ErrorCodeInfo& error_info(ChannelError code) noexcept {
    const auto at = static_cast<std::size_t>(code);
    return at < std::size(COMM_ERROR_INFOS) ? COMM_ERROR_INFOS[at]
                                            : COMM_ERROR_INFOS[0];
}

/// What a channel does with a send that its stream cannot hold. A depth of 0
/// holds every message, which only the plain sends of a collective do.
enum class Overflow : std::uint8_t {
    /// The send waits for a receive to free the stream.
    Block,
    /// The send fails at once with @ref ChannelError::Full.
    Fail,
    /// The oldest message of the stream is dropped and its send completes as
    /// sent, so the new message takes its place.
    DropOldest,
};

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_ERROR_H
