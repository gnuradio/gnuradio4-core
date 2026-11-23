#ifndef GNURADIO_SETTING_NOTIFICATION_HPP
#define GNURADIO_SETTING_NOTIFICATION_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Tag.hpp>

namespace gr::settings {

enum class NotificationLevel : std::uint8_t { trace, debug, info, warning, error, critical, off };
enum class NotificationKind : std::uint8_t { changed, failed };

/**
 * @brief What a block's settings report outside a call's result: a key changed, with the reason, or failed.
 *
 * A notification carries no value; a reader that wants the value reads the block's settings.
 */
struct Notification {
    NotificationLevel level = NotificationLevel::info;
    NotificationKind  what  = NotificationKind::changed;
    std::string       block{}; // the block's unique name
    std::string       key{};   // empty for an event of the whole block or a key the block does not declare
    std::string       reason{};
    std::size_t       nReplaced = 0UZ;   // notifications of the same block, key and level this one replaced
    bool              quiet     = false; // an inherited "off" marked it; only a reader a host installed receives it

    bool operator==(const Notification&) const = default;
};

[[nodiscard]] std::string_view                 notificationLevelName(NotificationLevel level) noexcept;
[[nodiscard]] std::optional<NotificationLevel> parseNotificationLevel(std::string_view name) noexcept;
[[nodiscard]] std::string_view                 notificationKindName(NotificationKind what) noexcept;

namespace detail {

// prints one notification to the standard error stream, one line each, under a lock of its own
void printNotification(const Notification& notification);

// a notification as the event stream carries it: `level` and `what` by name, `block`, `key`, `reason`, and `replaced`,
// the count of the notifications of the same block, key and level it replaced
[[nodiscard]] property_map toMessageData(const Notification& notification);

/**
 * @brief The notifications a scheduler holds for delivery: the latest per block, key and level, oldest first.
 *
 * A push that finds a notification of the same block, key and level replaces it and counts it and those it had
 * replaced, so a reader that stops reading holds memory bounded by the number of keys, not of changes.
 */
class NotificationQueue {
    std::mutex                _lock;
    std::vector<Notification> _held; // oldest first

public:
    void                                    push(Notification notification);
    [[nodiscard]] std::vector<Notification> take();                     // every held notification, oldest first, leaving the queue empty
    [[nodiscard]] std::vector<Notification> take(std::size_t maxCount); // at most `maxCount` of the oldest, oldest first
    [[nodiscard]] bool                      empty();
};

} // namespace detail

} // namespace gr::settings

#endif // GNURADIO_SETTING_NOTIFICATION_HPP
