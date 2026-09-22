// ---------------------------------------------------------------------------
// MacStatusItem.mm - NSStatusItem + UNUserNotificationCenter (see MacStatusItem.h).
//
// The icon is an SF Symbol rendered as a template image, so the menu bar
// tints it for light / dark menu bars and the selected state by itself.
// ---------------------------------------------------------------------------
#include "ui/mac/MacStatusItem.h"

#include "core/Logger.h"
#include "platform/Utf.h"

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"StatusItem";

/// Wide -> NSString (never nil).
NSString* ns(std::wstring_view text) {
    const std::string utf8 = hh::platform::toUtf8(text);
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding] ?: @"";
}

/// Notification Center needs a real, identified app bundle (it throws otherwise).
bool notificationsAvailable() {
    NSBundle* bundle = [NSBundle mainBundle];
    return bundle.bundleIdentifier.length > 0 && [bundle.bundlePath.pathExtension isEqualToString:@"app"];
}

} // namespace

/**
 * @brief Menu delegate / action target / notification delegate for one status item.
 */
@interface HHStatusController : NSObject <NSMenuDelegate, UNUserNotificationCenterDelegate>
@property (nonatomic, assign) hh::ui::MacStatusItem* owner;
@property (nonatomic, assign) std::shared_ptr<std::function<std::vector<hh::ui::MacStatusItem::MenuItem>()>>* provider;
@end

@implementation HHStatusController

- (void)menuNeedsUpdate:(NSMenu*)menu {
    [menu removeAllItems];
    if (_provider == nullptr || !*_provider || !**_provider) {
        return;
    }
    // Rebuilt on every open so the check marks and titles are always current.
    for (const hh::ui::MacStatusItem::MenuItem& item : (**_provider)()) {
        if (item.id == 0) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:ns(item.text) action:@selector(choose:) keyEquivalent:@""];
        entry.target = self;
        entry.tag = item.id;
        entry.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
        entry.enabled = item.enabled ? YES : NO;
        [menu addItem:entry];
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    return item.isEnabled;
}

- (void)choose:(NSMenuItem*)sender {
    if (_owner != nullptr && _owner->onCommand) {
        // Copy first: the handler may replace the callback.
        auto handler = _owner->onCommand;
        handler(static_cast<int>(sender.tag));
    }
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    // We only post while the window is hidden: show the banner even though
    // the app itself is technically active.
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_owner != nullptr && self->_owner->onNotificationClicked) {
            auto handler = self->_owner->onNotificationClicked;
            handler();
        }
    });
    completionHandler();
}

@end

namespace hh::ui {

struct MacStatusItem::Objc {
    NSStatusItem* item = nil;
    HHStatusController* controller = nil;
    bool askedPermission = false;
};

MacStatusItem::MacStatusItem() : objc_(std::make_unique<Objc>()) {
    provider_ = std::make_shared<std::function<std::vector<MenuItem>()>>();
    objc_->controller = [[HHStatusController alloc] init];
    objc_->controller.owner = this;
    objc_->controller.provider = &provider_;
}

MacStatusItem::~MacStatusItem() {
    remove();
    objc_->controller.owner = nullptr;
    objc_->controller.provider = nullptr;
    if (notificationsAvailable()) {
        UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
        if (center.delegate == objc_->controller) {
            center.delegate = nil;
        }
    }
}

bool MacStatusItem::add(const std::wstring& tooltip) {
    if (objc_->item != nil) {
        return true;
    }
    @autoreleasepool {
        NSStatusItem* item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
        if (item == nil || item.button == nil) {
            HH_LOG_WARN(kLog, L"status item unavailable");
            return false;
        }
        NSImage* image = [NSImage imageWithSystemSymbolName:@"sun.max" accessibilityDescription:ns(tooltip)];
        if (image != nil) {
            NSImageSymbolConfiguration* config = [NSImageSymbolConfiguration configurationWithPointSize:15
                                                                                                 weight:NSFontWeightMedium];
            image = [image imageWithSymbolConfiguration:config] ?: image;
            [image setTemplate:YES];
            item.button.image = image;
        } else {
            item.button.title = @"HDR";
        }
        item.button.toolTip = ns(tooltip);

        NSMenu* menu = [[NSMenu alloc] initWithTitle:ns(tooltip)];
        menu.delegate = objc_->controller;
        menu.autoenablesItems = NO;
        item.menu = menu;
        objc_->item = item;
    }
    return true;
}

void MacStatusItem::remove() {
    if (objc_->item != nil) {
        [[NSStatusBar systemStatusBar] removeStatusItem:objc_->item];
        objc_->item = nil;
    }
}

bool MacStatusItem::added() const noexcept {
    return objc_->item != nil;
}

void MacStatusItem::setMenuProvider(std::function<std::vector<MenuItem>()> provider) {
    *provider_ = std::move(provider);
}

void MacStatusItem::notify(const std::wstring& title, const std::wstring& text) {
    if (!notificationsAvailable()) {
        HH_LOG_DEBUG(kLog, L"notification skipped (not running from an app bundle): {}", text);
        return;
    }
    @autoreleasepool {
        UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
        center.delegate = objc_->controller;

        UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
        content.title = ns(title);
        content.body = ns(text);
        UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:[NSUUID UUID].UUIDString
                                                                              content:content
                                                                              trigger:nil];
        void (^post)(void) = ^{
            [center addNotificationRequest:request
                     withCompletionHandler:^(NSError* error) {
                         if (error != nil) {
                             HH_LOG_DEBUG(kLog, L"notification not delivered: {}",
                                          hh::platform::toWide(error.localizedDescription.UTF8String ?: ""));
                         }
                     }];
        };

        // Ask once, in context (the first time there is something to say).
        if (!objc_->askedPermission) {
            objc_->askedPermission = true;
            [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                                  completionHandler:^(BOOL granted, NSError* error) {
                                      if (granted) {
                                          post();
                                      } else {
                                          HH_LOG_INFO(kLog, L"notifications not allowed by the user");
                                      }
                                  }];
            return;
        }
        post();
    }
}

} // namespace hh::ui
