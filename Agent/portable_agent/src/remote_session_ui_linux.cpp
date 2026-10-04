#include "remote_session_ui.h"

#if defined(__linux__)

#include <gtk/gtk.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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

void queueMain(std::function<void()> fn) {
    auto* work = new std::function<void()>(std::move(fn));
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::function<void()>> callback(
                static_cast<std::function<void()>*>(data));
            (*callback)();
            return G_SOURCE_REMOVE;
        },
        work,
        nullptr);
}

} // namespace

namespace hi5 {

struct RemoteSessionUi::Impl {
    explicit Impl(SendFn fn) : send(std::move(fn)) {
        gtkReady = gtk_init_check(nullptr, nullptr) == TRUE;
    }

    ~Impl() {
        if (gtkReady) {
            if (banner) gtk_widget_destroy(banner);
            if (chat) gtk_widget_destroy(chat);
        }
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

        queueMain([this, nextSession, nextTech]() {
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

        queueMain([this, sender, body]() {
            ensureChat();
            appendLine(sender, body);
            showChat();
        });
    }
    void handleChatClose() {
        queueMain([this]() {
            if (chat) gtk_widget_hide(chat);
        });
    }

    void handleSessionEnd() {
        queueMain([this]() {
            if (banner) gtk_widget_hide(banner);
            if (chat) gtk_widget_hide(chat);
            sessionId.clear();
            technician.clear();
            if (transcriptBuffer) {
                gtk_text_buffer_set_text(transcriptBuffer, "", -1);
            }
        });
    }

    void run() {
        if (gtkReady) {
            gtk_main();
            return;
        }

        while (!quitRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void quit() {
        quitRequested.store(true);
        if (gtkReady) {
            queueMain([]() {
                if (gtk_main_level() > 0) gtk_main_quit();
            });
        }
    }

    void showBanner() {
        if (!gtkReady) return;
        ensureBanner();

        gtk_label_set_text(
            GTK_LABEL(title),
            "Hi5Central support is connected");

        const std::string detailText =
            (technician.empty() ? "A technician" : technician) +
            " has unattended access";
        gtk_label_set_text(GTK_LABEL(detail), detailText.c_str());

        gtk_widget_show_all(banner);
        gtk_window_present(GTK_WINDOW(banner));
    }

    void ensureBanner() {
        if (!gtkReady || banner) return;

        banner = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(banner), "Hi5Central");
        gtk_window_set_default_size(GTK_WINDOW(banner), 460, 76);
        gtk_window_set_decorated(GTK_WINDOW(banner), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(banner), TRUE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(banner), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(banner), TRUE);
        gtk_window_set_position(GTK_WINDOW(banner), GTK_WIN_POS_CENTER_ALWAYS);
        gtk_window_set_type_hint(
            GTK_WINDOW(banner),
            GDK_WINDOW_TYPE_HINT_NOTIFICATION);

        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_container_set_border_width(GTK_CONTAINER(outer), 14);
        gtk_container_add(GTK_CONTAINER(banner), outer);

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_widget_set_hexpand(labels, TRUE);
        gtk_box_pack_start(GTK_BOX(outer), labels, TRUE, TRUE, 0);

        title = gtk_label_new("Hi5Central support is connected");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_label_set_markup(
            GTK_LABEL(title),
            "<b>Hi5Central support is connected</b>");
        gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

        detail = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
        gtk_box_pack_start(GTK_BOX(labels), detail, FALSE, FALSE, 0);

        GtkWidget* chatButton = gtk_button_new_with_label("Chat");
        g_signal_connect(
            chatButton,
            "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->showChat();
            }),
            this);
        gtk_box_pack_end(GTK_BOX(outer), chatButton, FALSE, FALSE, 0);

        g_signal_connect(
            banner,
            "delete-event",
            G_CALLBACK(+[](GtkWidget* widget, GdkEvent*, gpointer) -> gboolean {
                gtk_widget_hide(widget);
                return TRUE;
            }),
            nullptr);
    }
    void ensureChat() {
        if (!gtkReady || chat) return;

        chat = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(
            GTK_WINDOW(chat),
            "Hi5Central Support Chat");
        gtk_window_set_default_size(GTK_WINDOW(chat), 480, 500);
        gtk_window_set_position(GTK_WINDOW(chat), GTK_WIN_POS_CENTER);
        gtk_window_set_keep_above(GTK_WINDOW(chat), TRUE);

        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(outer), 14);
        gtk_container_add(GTK_CONTAINER(chat), outer);

        GtkWidget* heading =
            gtk_label_new("Chat with your connected technician");
        gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
        gtk_label_set_markup(
            GTK_LABEL(heading),
            "<b>Chat with your connected technician</b>");
        gtk_box_pack_start(GTK_BOX(outer), heading, FALSE, FALSE, 0);

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_AUTOMATIC,
            GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

        transcript = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(transcript), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(transcript), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(transcript), GTK_WRAP_WORD_CHAR);
        transcriptBuffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(transcript));
        gtk_container_add(GTK_CONTAINER(scroll), transcript);

        GtkWidget* composer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_end(GTK_BOX(outer), composer, FALSE, FALSE, 0);

        input = gtk_entry_new();
        gtk_entry_set_placeholder_text(
            GTK_ENTRY(input),
            "Type a message…");
        gtk_widget_set_hexpand(input, TRUE);
        g_signal_connect(
            input,
            "activate",
            G_CALLBACK(+[](GtkEntry*, gpointer data) {
                static_cast<Impl*>(data)->sendCurrentMessage();
            }),
            this);
        gtk_box_pack_start(GTK_BOX(composer), input, TRUE, TRUE, 0);

        GtkWidget* sendButton = gtk_button_new_with_label("Send");
        g_signal_connect(
            sendButton,
            "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->sendCurrentMessage();
            }),
            this);
        gtk_box_pack_end(GTK_BOX(composer), sendButton, FALSE, FALSE, 0);

        g_signal_connect(
            chat,
            "delete-event",
            G_CALLBACK(+[](GtkWidget* widget, GdkEvent*, gpointer) -> gboolean {
                gtk_widget_hide(widget);
                return TRUE;
            }),
            nullptr);
    }

    void showChat() {
        if (!gtkReady) return;
        ensureChat();
        gtk_widget_show_all(chat);
        gtk_window_present(GTK_WINDOW(chat));
        gtk_widget_grab_focus(input);
    }
    void appendLine(
        const std::string& displayName,
        const std::string& body) {

        if (!gtkReady) return;
        ensureChat();

        GtkTextIter end;
        gtk_text_buffer_get_end_iter(transcriptBuffer, &end);
        const std::string line =
            displayName + ": " + body + "\n\n";
        gtk_text_buffer_insert(
            transcriptBuffer,
            &end,
            line.c_str(),
            static_cast<gint>(line.size()));

        GtkTextMark* mark =
            gtk_text_buffer_get_insert(transcriptBuffer);
        gtk_text_view_scroll_mark_onscreen(
            GTK_TEXT_VIEW(transcript),
            mark);
    }

    void sendCurrentMessage() {
        if (!gtkReady || !input || sessionId.empty()) return;

        const char* value = gtk_entry_get_text(GTK_ENTRY(input));
        const std::string body = value ? value : "";
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
            gtk_entry_set_text(GTK_ENTRY(input), "");
        }
    }

    SendFn send;
    bool gtkReady = false;
    std::atomic<bool> quitRequested{false};
    std::string sessionId;
    std::string technician;

    GtkWidget* banner = nullptr;
    GtkWidget* chat = nullptr;
    GtkWidget* title = nullptr;
    GtkWidget* detail = nullptr;
    GtkWidget* transcript = nullptr;
    GtkTextBuffer* transcriptBuffer = nullptr;
    GtkWidget* input = nullptr;
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
