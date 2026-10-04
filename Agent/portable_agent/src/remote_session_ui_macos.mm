#include "remote_session_ui.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <string>
#include <utility>

namespace {

using json = nlohmann::json;

std::string chatBody(const json& message) {
    for (const char* key : {"body", "message", "text"}) {
        const auto it = message.find(key);
        if (it != message.end() && it->is_string()) {
            auto value = it->get<std::string>();
            if (!value.empty()) return value;
        }
    }
    return {};
}

std::int64_t nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

NSString* ns(const std::string& value) {
    return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

@interface Hi5SessionUiTarget : NSObject
@property(copy) void (^openChatHandler)(void);
@property(copy) void (^sendChatHandler)(void);
@end

@implementation Hi5SessionUiTarget
- (void)openChat:(id)sender {
    (void)sender;
    if (self.openChatHandler) self.openChatHandler();
}
- (void)sendChat:(id)sender {
    (void)sender;
    if (self.sendChatHandler) self.sendChatHandler();
}
@end

} // namespace

namespace hi5 {

struct RemoteSessionUi::Impl {
    explicit Impl(SendFn fn) : send(std::move(fn)) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        target = [[Hi5SessionUiTarget alloc] init];
        __unsafe_unretained Impl* weakSelf = this;
        target.openChatHandler = ^{
            if (weakSelf) weakSelf->showChat();
        };
        target.sendChatHandler = ^{
            if (weakSelf) weakSelf->sendCurrentMessage();
        };
    }

    ~Impl() {
        target.openChatHandler = nil;
        target.sendChatHandler = nil;
        [banner orderOut:nil];
        [chat orderOut:nil];
    }

    void dispatchMain(std::function<void()> fn) {
        dispatch_async(dispatch_get_main_queue(), ^{
            fn();
        });
    }

    void handleStart(const json& message) {
        const std::string sessionType = message.value("session_type", "");
        const std::string mode = message.value("mode", "console");
        if (sessionType != "unattended" || mode != "console") return;

        const std::string nextSession =
            message.value("session_id", message.value("sessionId", ""));
        std::string nextTech =
            message.value("technician_name", "Hi5Central technician");
        if (nextTech.empty()) nextTech = "Hi5Central technician";

        send({
            {"type", "agent_presence"},
            {"session_id", nextSession},
            {"technician_name", nextTech},
            {"connected", true},
            {"chat_available", message.value("chat_available", true)}
        });

        dispatchMain([this, nextSession, nextTech]() {
            sessionId = nextSession;
            technician = nextTech;
            showBanner();
        });
    }
    void handleChatMessage(const json& message) {
        const std::string body = chatBody(message);
        if (body.empty()) return;

        std::string sender =
            message.value("display_name", message.value("displayName", ""));
        if (sender.empty()) {
            sender = message.value("sender", "tech") == "user"
                ? "You"
                : (technician.empty() ? "Technician" : technician);
        }

        dispatchMain([this, sender, body]() {
            ensureChat();
            appendLine(sender, body);
            showChat();
        });
    }

    void handleChatClose() {
        dispatchMain([this]() {
            if (chat) [chat orderOut:nil];
        });
    }

    void handleSessionEnd() {
        dispatchMain([this]() {
            if (banner) [banner orderOut:nil];
            if (chat) [chat orderOut:nil];
            sessionId.clear();
            technician.clear();
            if (transcript) [transcript setString:@""];
        });
    }

    void run() {
        [NSApp run];
    }

    void quit() {
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
    }

    void showBanner() {
        ensureBanner();
        [title setStringValue:@"Hi5Central support is connected"];
        [detail setStringValue:
            [NSString stringWithFormat:@"%@ has unattended access",
             ns(technician)]];

        NSScreen* screen = [NSScreen mainScreen];
        if (screen) {
            const NSRect visible = [screen visibleFrame];
            NSRect frame = [banner frame];
            frame.origin.x = NSMidX(visible) - frame.size.width / 2.0;
            frame.origin.y = NSMaxY(visible) - frame.size.height - 14.0;
            [banner setFrame:frame display:YES];
        }
        [banner orderFrontRegardless];
    }

    void ensureBanner() {
        if (banner) return;

        banner = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 440, 72)
            styleMask:NSWindowStyleMaskBorderless
            backing:NSBackingStoreBuffered
            defer:NO];
        [banner setLevel:NSStatusWindowLevel];
        [banner setOpaque:NO];
        [banner setBackgroundColor:
            [NSColor colorWithWhite:0.10 alpha:0.96]];
        [banner setHasShadow:YES];
        [banner setCollectionBehavior:
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorFullScreenAuxiliary];

        NSView* content = [banner contentView];

        title = [[NSTextField alloc]
            initWithFrame:NSMakeRect(18, 39, 310, 20)];
        configureLabel(title, 13, true);
        [title setTextColor:[NSColor whiteColor]];
        [content addSubview:title];

        detail = [[NSTextField alloc]
            initWithFrame:NSMakeRect(18, 15, 310, 18)];
        configureLabel(detail, 11, false);
        [detail setTextColor:
            [NSColor colorWithWhite:0.80 alpha:1.0]];
        [content addSubview:detail];

        NSButton* button = [[NSButton alloc]
            initWithFrame:NSMakeRect(342, 20, 80, 32)];
        [button setTitle:@"Chat"];
        [button setBezelStyle:NSBezelStyleRounded];
        [button setTarget:target];
        [button setAction:@selector(openChat:)];
        [content addSubview:button];
    }
    void ensureChat() {
        if (chat) return;

        chat = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 470, 480)
            styleMask:(NSWindowStyleMaskTitled |
                       NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable)
            backing:NSBackingStoreBuffered
            defer:NO];
        [chat setTitle:@"Hi5Central Support Chat"];
        [chat setLevel:NSFloatingWindowLevel];
        [chat center];

        NSView* content = [chat contentView];

        NSTextField* heading = [[NSTextField alloc]
            initWithFrame:NSMakeRect(18, 438, 430, 22)];
        configureLabel(heading, 14, true);
        [heading setStringValue:@"Chat with your connected technician"];
        [content addSubview:heading];

        NSScrollView* scroll = [[NSScrollView alloc]
            initWithFrame:NSMakeRect(18, 76, 434, 354)];
        [scroll setHasVerticalScroller:YES];
        [scroll setBorderType:NSBezelBorder];

        transcript = [[NSTextView alloc]
            initWithFrame:NSMakeRect(0, 0, 430, 350)];
        [transcript setEditable:NO];
        [transcript setSelectable:YES];
        [transcript setFont:[NSFont systemFontOfSize:12]];
        [scroll setDocumentView:transcript];
        [content addSubview:scroll];

        input = [[NSTextField alloc]
            initWithFrame:NSMakeRect(18, 24, 334, 32)];
        [input setPlaceholderString:@"Type a message…"];
        [input setTarget:target];
        [input setAction:@selector(sendChat:)];
        [content addSubview:input];

        NSButton* button = [[NSButton alloc]
            initWithFrame:NSMakeRect(362, 24, 90, 32)];
        [button setTitle:@"Send"];
        [button setBezelStyle:NSBezelStyleRounded];
        [button setTarget:target];
        [button setAction:@selector(sendChat:)];
        [content addSubview:button];
    }

    void showChat() {
        ensureChat();
        [chat makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [chat makeFirstResponder:input];
    }

    void appendLine(
        const std::string& displayName,
        const std::string& body) {

        ensureChat();
        NSString* line = [NSString stringWithFormat:@"%@: %@\n\n",
            ns(displayName), ns(body)];
        [[transcript textStorage] appendAttributedString:
            [[NSAttributedString alloc] initWithString:line]];
        [transcript scrollRangeToVisible:
            NSMakeRange([[transcript string] length], 0)];
    }

    void sendCurrentMessage() {
        if (!input || sessionId.empty()) return;

        NSString* value = [input stringValue];
        if (!value || value.length == 0) return;
        std::string body([value UTF8String] ?: "");
        if (body.empty()) return;

        json message = {
            {"type", "chat_message"},
            {"session_id", sessionId},
            {"sender", "user"},
            {"display_name", "Device user"},
            {"body", body},
            {"message", body},
            {"text", body},
            {"unix_ms", nowUnixMs()}
        };

        if (send(message)) {
            appendLine("You", body);
            [input setStringValue:@""];
        }
    }

    static void configureLabel(
        NSTextField* label,
        CGFloat size,
        bool bold) {

        [label setBezeled:NO];
        [label setDrawsBackground:NO];
        [label setEditable:NO];
        [label setSelectable:NO];
        [label setFont:bold
            ? [NSFont boldSystemFontOfSize:size]
            : [NSFont systemFontOfSize:size]];
    }
    SendFn send;
    std::string sessionId;
    std::string technician;

    __strong Hi5SessionUiTarget* target = nil;
    __strong NSPanel* banner = nil;
    __strong NSWindow* chat = nil;
    __strong NSTextField* title = nil;
    __strong NSTextField* detail = nil;
    __strong NSTextView* transcript = nil;
    __strong NSTextField* input = nil;
};

RemoteSessionUi::RemoteSessionUi(SendFn send)
    : impl_(std::make_unique<Impl>(std::move(send))) {}

RemoteSessionUi::~RemoteSessionUi() = default;

void RemoteSessionUi::handleStart(const nlohmann::json& message) {
    impl_->handleStart(message);
}

void RemoteSessionUi::handleChatMessage(
    const nlohmann::json& message) {
    impl_->handleChatMessage(message);
}

void RemoteSessionUi::handleChatClose() {
    impl_->handleChatClose();
}

void RemoteSessionUi::handleSessionEnd() {
    impl_->handleSessionEnd();
}

void RemoteSessionUi::run() {
    impl_->run();
}

void RemoteSessionUi::quit() {
    impl_->quit();
}

} // namespace hi5

#endif
