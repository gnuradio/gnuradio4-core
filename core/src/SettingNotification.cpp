#include <gnuradio-4.0/SettingNotification.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <iterator>
#include <utility>

namespace gr::settings {

namespace {

constexpr std::array<std::string_view, 7UZ> kLevelNames{"trace", "debug", "info", "warning", "error", "critical", "off"};
constexpr std::array<std::string_view, 2UZ> kKindNames{"changed", "failed"};

} // namespace

std::string_view notificationLevelName(NotificationLevel level) noexcept { return kLevelNames[static_cast<std::size_t>(level)]; }

std::optional<NotificationLevel> parseNotificationLevel(std::string_view name) noexcept {
    const auto it = std::ranges::find(kLevelNames, name);
    return it != kLevelNames.end() ? std::optional<NotificationLevel>(static_cast<NotificationLevel>(it - kLevelNames.begin())) : std::nullopt;
}

std::string_view notificationKindName(NotificationKind what) noexcept { return kKindNames[static_cast<std::size_t>(what)]; }

namespace {
// the line the default reader prints for one notification
std::string notificationLine(const Notification& notification) {
    const std::string key = notification.key.empty() ? std::string{} : std::format(" '{}'", notification.key);
    return std::format("{}: {} {}{}{}{}", notificationLevelName(notification.level), notification.block, notificationKindName(notification.what), key, notification.reason.empty() ? "" : ": ", notification.reason);
}
} // namespace

namespace detail {

void printNotification(const Notification& notification) {
    static std::mutex printLock;
    const std::string line = notificationLine(notification);
    std::lock_guard   lock(printLock);
    std::cerr << line << '\n';
}

property_map toMessageData(const Notification& notification) {
    return property_map{
        {"level", std::string(notificationLevelName(notification.level))},
        {"what", std::string(notificationKindName(notification.what))},
        {"block", notification.block},
        {"key", notification.key},
        {"reason", notification.reason},
        {"replaced", static_cast<std::uint64_t>(notification.nReplaced)},
    };
}

void NotificationQueue::push(Notification notification) {
    std::lock_guard lock(_lock);
    const auto      sameSlot = [&notification](const Notification& held) { return held.block == notification.block && held.key == notification.key && held.level == notification.level; };
    if (const auto it = std::ranges::find_if(_held, sameSlot); it != _held.end()) {
        notification.nReplaced += it->nReplaced + 1UZ;
        _held.erase(it);
    }
    _held.push_back(std::move(notification));
}

std::vector<Notification> NotificationQueue::take() {
    std::lock_guard lock(_lock);
    return std::exchange(_held, {});
}

bool NotificationQueue::empty() {
    std::lock_guard lock(_lock);
    return _held.empty();
}

std::vector<Notification> NotificationQueue::take(std::size_t maxCount) {
    std::lock_guard           lock(_lock);
    const std::size_t         nTaken = std::min(maxCount, _held.size());
    std::vector<Notification> taken(std::make_move_iterator(_held.begin()), std::make_move_iterator(_held.begin() + static_cast<std::ptrdiff_t>(nTaken)));
    _held.erase(_held.begin(), _held.begin() + static_cast<std::ptrdiff_t>(nTaken));
    return taken;
}

} // namespace detail

} // namespace gr::settings
