// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkitgtk.h"

#include "webview/platform/linux/webview_linux_webkitgtk_library.h"
#include "webview/platform/linux/webview_linux_compositor.h"
#include "webview/webview_data_stream.h"
#include "base/platform/base_platform_info.h"
#include "base/debug_log.h"
#include "base/integration.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "base/event_filter.h"
#include "ui/gl/gl_detection.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
#include <QtQuickWidgets/QQuickWidget>
#endif // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

#include <webview/webview.hpp>
#include <crl/crl.h>
#include <rpl/rpl.h>
#include <regex>
#include <sys/mman.h>

namespace Webview::WebKitGTK {
namespace {

using namespace gi::repository;
using namespace gi::repository::Webview;
using namespace Library;
namespace GObject = gi::repository::GObject;

constexpr auto kObjectPath = "/org/desktop_app/GtkIntegration/Webview";
constexpr auto kMasterObjectPath = "/org/desktop_app/GtkIntegration/Webview/Master";
constexpr auto kHelperObjectPath = "/org/desktop_app/GtkIntegration/Webview/Helper";
constexpr auto kDataUrlScheme = "desktop-app-resource";
constexpr auto kFullDomain = "desktop-app-resource://domain/";

std::string SocketPath;

inline auto MethodError() {
	return GLib::Error::new_literal(
		Gio::DBusErrorNS_::quark(),
		int(Gio::DBusError::UNKNOWN_METHOD_),
		"Method does not exist.");
}

inline auto NotFoundError() {
	return GLib::Error::new_literal(
		G_IO_ERROR,
		G_IO_ERROR_NOT_FOUND,
		"Not Found");
}

inline std::string SocketPathToDBusAddress(const std::string &socketPath) {
	return "unix:path=" + socketPath;
}

class Instance final : public Interface, public ::base::has_weak_ptr {
public:
	Instance(bool remoting = true);
	~Instance();

	bool create(Config config);

	ResolveResult resolve();

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void reload() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void focus() override;

	QWidget *widget() override;

	void refreshNavigationHistoryState() override;
	auto navigationHistoryState()
		-> rpl::producer<NavigationHistoryState> override;

	void setOpaqueBg(QColor opaqueBg) override;

	int exec();

private:
	void scriptMessageReceived(void *message);

	bool loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		GLib::Error error);

	void loadChanged(WebKitLoadEvent loadEvent);

	bool decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType);
	GtkWidget *createAnother(WebKitNavigationAction *action);
	bool scriptDialog(WebKitScriptDialog *dialog);

	bool processRedirect(WebKitURISchemeRequest *request);

	void dataRequest(WebKitURISchemeRequest *request);
	void dataResponse(
		WebKitURISchemeRequest *request,
		int fd,
		int64 offset,
		int64 requestedOffset,
		int64 size,
		int64 total,
		std::string mime);

	void startProcess();
	void stopProcess();
	void updateHistoryStates();

	void registerMasterMethodHandlers();
	void registerHelperMethodHandlers();

	void *winId();

	bool _remoting = false;
	bool _connected = false;
	Master _master;
	Helper _helper;
	Gio::DBusServer _dbusServer;
	Gio::DBusObjectManagerServer _dbusObjectManager;
	Gio::Subprocess _serviceProcess;

	bool _wayland = false;
	::base::unique_qptr<QWidget> _widget;
	QPointer<Compositor> _compositor;

	GtkWidget *_window = nullptr;
	GtkWidget *_x11SizeFix = nullptr;
	WebKitWebView *_webview = nullptr;
	GtkCssProvider *_backgroundProvider = nullptr;

	bool _debug = false;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string,bool)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<DialogResult(DialogArgs)> _dialogHandler;
	rpl::variable<NavigationHistoryState> _navigationHistoryState;
	std::function<DataResult(DataRequest)> _dataRequestHandler;
	std::string _dataProtocol;
	std::string _dataDomain;
	bool _loadFailed = false;

};

Instance::Instance(bool remoting)
: _remoting(remoting) {
	if (_remoting) {
		_wayland = !Platform::IsX11();
		startProcess();
	}
}

Instance::~Instance() {
	if (_remoting) {
		stopProcess();
	}
	if (_backgroundProvider) {
		g_object_unref(_backgroundProvider);
	}
	if (_window) {
		if (gtk_window_destroy) {
			gtk_window_destroy(GTK_WINDOW(_window));
		} else {
			gtk_widget_destroy(_window);
		}
	}
}

bool Instance::create(Config config) {
	_debug = config.debug;
	_messageHandler = std::move(config.messageHandler);
	_navigationStartHandler = std::move(config.navigationStartHandler);
	_navigationDoneHandler = std::move(config.navigationDoneHandler);
	_dialogHandler = std::move(config.dialogHandler);
	_dataRequestHandler = std::move(config.dataRequestHandler);
	_dataProtocol = kDataUrlScheme;
	_dataDomain = kFullDomain;
	if (!config.dataProtocolOverride.empty()) {
		_dataProtocol = config.dataProtocolOverride;
		_dataDomain = _dataProtocol + "://domain/";
	}

	if (_remoting) {
		const auto resolveResult = resolve();
		if (resolveResult != ResolveResult::Success) {
			LOG(("WebView Error: %1.").arg(
				resolveResult == ResolveResult::NoLibrary
					? "No library"
					: resolveResult == ResolveResult::CantInit
					? "Could not initialize GTK"
					: resolveResult == ResolveResult::IPCFailure
					? "Inter-process communication failure"
					: "Unknown error"));
			return false;
		}

#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		if (_compositor) {
			auto widget = qobject_cast<QQuickWidget*>(_widget);
			if (!widget) {
				if (Ui::GL::ChooseBackendDefault(Ui::GL::CheckCapabilities())
						!= Ui::GL::Backend::OpenGL) {
					LOG(("WebView Error: OpenGL is disabled."));
					return false;
				}
				_widget = ::base::make_unique_q<QQuickWidget>(config.parent);
				widget = static_cast<QQuickWidget*>(_widget.get());
				_compositor->setWidget(widget);
			}
			widget->setClearColor(config.opaqueBg);
			widget->show();
		}
#else // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		if (_compositor) {
			LOG(("WebView Error: No Wayland support."));
			return false;
		}
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

		if (!_helper) {
			return false;
		}

		const ::base::has_weak_ptr guard;
		std::optional<bool> success;
		const auto debug = _debug;
		const auto r = config.opaqueBg.red();
		const auto g = config.opaqueBg.green();
		const auto b = config.opaqueBg.blue();
		const auto a = config.opaqueBg.alpha();
		const auto protocol = config.dataProtocolOverride;
		const auto path = config.userDataPath;
		_helper.call_create(debug, r, g, b, a, protocol, path, crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			success = _helper.call_create_finish(res, nullptr);
			GLib::MainContext::default_().wakeup();
		}));

		while (!success && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		if (success.value_or(false) && !_compositor) {
			const auto window = QPointer(QWindow::fromWinId(WId(winId())));
			::base::install_event_filter(window, [=](
					not_null<QEvent*> e) {
				if (e->type() == QEvent::Show) {
					GLib::timeout_add_seconds_once(1, crl::guard(window, [=] {
						const auto size = window->size();
						window->resize(0, 0);
						window->resize(size);
					}));
				}
				return ::base::EventFilterResult::Continue;
			});
			_widget.reset(
				QWidget::createWindowContainer(
					window,
					config.parent,
					Qt::FramelessWindowHint));
			_widget->show();
		}
		return success.value_or(false);
	}

	_window = _wayland
		? gtk_window_new(GTK_WINDOW_TOPLEVEL)
		: gtk_plug_new(0);
	if (gtk_widget_add_css_class) {
		gtk_widget_add_css_class(_window, "webviewWindow");
	} else {
		gtk_style_context_add_class(
			gtk_widget_get_style_context(_window),
			"webviewWindow");
	}
	_backgroundProvider = gtk_css_provider_new();
	if (gtk_style_context_add_provider_for_display) {
		gtk_style_context_add_provider_for_display(
			gtk_widget_get_display(_window),
			GTK_STYLE_PROVIDER(_backgroundProvider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	} else {
		gtk_style_context_add_provider_for_screen(
			gtk_widget_get_screen(_window),
			GTK_STYLE_PROVIDER(_backgroundProvider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}
	setOpaqueBg(config.opaqueBg);

	if (!_wayland) {
		_x11SizeFix = gtk_scrolled_window_new(nullptr, nullptr);
	}

	const auto base = config.userDataPath;
	const auto baseCache = base + "/cache";
	const auto baseData = base + "/data";

	WebKitWebContext *context = nullptr;
	if (webkit_network_session_new) {
		context = webkit_web_context_new();
		WebKitNetworkSession *session = webkit_network_session_new(
			baseData.c_str(),
			baseCache.c_str());
		_webview = WEBKIT_WEB_VIEW(g_object_new(
			WEBKIT_TYPE_WEB_VIEW,
			"web-context",
			context,
			"network-session",
			session,
			nullptr));
		g_object_unref(session);
		g_object_unref(context);
	} else {
		WebKitWebsiteDataManager *data = webkit_website_data_manager_new(
			"base-cache-directory", baseCache.c_str(),
			"base-data-directory", baseData.c_str(),
			nullptr);
		context = webkit_web_context_new_with_website_data_manager(data);
		g_object_unref(data);

		_webview = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(context));
		g_object_unref(context);
	}

	WebKitUserContentManager *manager =
		webkit_web_view_get_user_content_manager(_webview);
	g_signal_connect_swapped(
		manager,
		"script-message-received::external",
		G_CALLBACK(+[](
			Instance *instance,
			void *message) {
			instance->scriptMessageReceived(message);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"web-process-terminated",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitWebProcessTerminationReason reason) {
			Gio::Application::get_default().quit();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::is-web-process-responsive",
		G_CALLBACK(+[](
			Instance *instance,
			GParamSpec *pspec) {
			Gio::Application::get_default().quit();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"load-failed",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitLoadEvent loadEvent,
			char *failingUri,
			GError *error) -> gboolean {
			return instance->loadFailed(
				loadEvent,
				failingUri,
				GLib::Error(g_error_copy(error)));
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"load-changed",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitLoadEvent loadEvent) {
			instance->loadChanged(loadEvent);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::uri",
		G_CALLBACK(+[](
			Instance *instance,
			GParamSpec *pspec) -> gboolean {
			instance->updateHistoryStates();
			return true;
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::title",
		G_CALLBACK(+[](
			Instance *instance,
			GParamSpec *pspec) -> gboolean {
			instance->updateHistoryStates();
			return true;
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"decide-policy",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitPolicyDecision *decision,
			WebKitPolicyDecisionType decisionType) -> gboolean {
			return instance->decidePolicy(decision, decisionType);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"create",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitNavigationAction *action) -> GtkWidget* {
			return instance->createAnother(action);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"script-dialog",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitScriptDialog *dialog) -> gboolean {
			return instance->scriptDialog(dialog);
		}),
		this);
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external",
		nullptr);
	webkit_web_context_register_uri_scheme(
		context,
		_dataProtocol.c_str(),
		WebKitURISchemeRequestCallback(+[](
			WebKitURISchemeRequest *request,
			Instance *instance) {
			instance->dataRequest(request);
		}),
		this,
		nullptr);
	const GdkRGBA rgba{ 0.f, 0.f, 0.f, 0.f, };
	webkit_web_view_set_background_color(_webview, &rgba);
	if (_debug) {
		WebKitSettings *settings = webkit_web_view_get_settings(_webview);
		webkit_settings_set_enable_developer_extras(settings, true);
	}
	if (gtk_window_set_child) {
		gtk_window_set_child(GTK_WINDOW(_window), GTK_WIDGET(_webview));
	} else if (_x11SizeFix) {
		gtk_container_add(GTK_CONTAINER(_x11SizeFix), GTK_WIDGET(_webview));
		gtk_container_add(GTK_CONTAINER(_window), _x11SizeFix);
	} else {
		gtk_container_add(GTK_CONTAINER(_window), GTK_WIDGET(_webview));
	}
	if (_wayland) {
		gtk_window_fullscreen(GTK_WINDOW(_window));
	}
	if (!gtk_widget_show_all) {
		gtk_widget_set_visible(_window, true);
	} else {
		gtk_widget_show_all(_window);
	}
	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");

	return webkit_web_view_get_is_web_process_responsive(_webview);
}

void Instance::scriptMessageReceived(void *message) {
	if (!_master) {
		return;
	}
	_master.call_message_received([&] {
		const auto s = jsc_value_to_string(
			!webkit_javascript_result_get_js_value
				? reinterpret_cast<JSCValue*>(message)
				: webkit_javascript_result_get_js_value(
					reinterpret_cast<WebKitJavascriptResult*>(message)));
		const auto guard = gsl::finally([&] {
			g_free(s);
		});
		return std::string(s);
	}(), nullptr);
}

bool Instance::loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		GLib::Error error) {
	_loadFailed = true;
	return false;
}

void Instance::loadChanged(WebKitLoadEvent loadEvent) {
	if (loadEvent == WEBKIT_LOAD_STARTED) {
		_loadFailed = false;
	} else if (loadEvent == WEBKIT_LOAD_FINISHED) {
		if (_master) {
			_master.call_navigation_done(!_loadFailed, nullptr);
		}
	}
	updateHistoryStates();
}

bool Instance::decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType) {
	if (decisionType != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
		return false;
	}
	WebKitNavigationPolicyDecision *navigationDecision
		= WEBKIT_NAVIGATION_POLICY_DECISION(decision);
	WebKitNavigationAction *action
		= webkit_navigation_policy_decision_get_navigation_action(
			navigationDecision);
	WebKitURIRequest *request = webkit_navigation_action_get_request(action);
	const gchar *uri = webkit_uri_request_get_uri(request);
	bool result = false;
	if (_master) {
		auto loop = GLib::MainLoop::new_();
		_master.call_navigation_started(uri, false, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			if (const auto ret = _master.call_navigation_started_finish(
					res)) {
				result = std::get<1>(*ret);
			}
			loop.quit();
		});
		loop.run();
	}
	if (!result) {
		webkit_policy_decision_ignore(decision);
	}
	GLib::timeout_add_seconds_once(1, crl::guard(this, [=] {
		if (!webkit_web_view_is_loading(_webview)) {
			if (_master) {
				_master.call_navigation_done(!_loadFailed, nullptr);
			}
		}
	}));
	return !result;
}

GtkWidget *Instance::createAnother(WebKitNavigationAction *action) {
	WebKitURIRequest *request = webkit_navigation_action_get_request(action);
	const gchar *uri = webkit_uri_request_get_uri(request);
	if (_master) {
		_master.call_navigation_started(uri, true, nullptr);
	}
	return nullptr;
}

bool Instance::scriptDialog(WebKitScriptDialog *dialog) {
	const auto type = webkit_script_dialog_get_dialog_type(dialog);
	const auto text = webkit_script_dialog_get_message(dialog);
	const auto value = (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
		? webkit_script_dialog_prompt_get_default_text(dialog)
		: nullptr;
	bool accepted = false;
	std::string result;
	if (_master) {
		auto loop = GLib::MainLoop::new_();
		_master.call_script_dialog(
			type,
			text ? text : "",
			value ? value : "",
			[&](GObject::Object source_object, Gio::AsyncResult res) {
				if (const auto ret = _master.call_script_dialog_finish(res)) {
					std::tie(std::ignore, accepted, result) = *ret;
				}
				loop.quit();
			});
		loop.run();
	}
	if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
		webkit_script_dialog_prompt_set_text(
			dialog,
			accepted ? result.c_str() : nullptr);
	} else if (type != WEBKIT_SCRIPT_DIALOG_ALERT) {
		webkit_script_dialog_confirm_set_confirmed(dialog, false);
	}
	return true;
}

void Instance::dataRequest(WebKitURISchemeRequest *request) {
	if (!_master) {
		webkit_uri_scheme_request_finish_error(
			request,
			NotFoundError().gobj_());
		return;
	}

	if (processRedirect(request)) {
		return;
	}

	g_object_ref(request);
	_master.call_data_request(
		uintptr_t(request),
		webkit_uri_scheme_request_get_path(request) + 1,
		0,
		0,
		[=](GObject::Object source_object, Gio::AsyncResult res) {
			const auto ret = _master.call_data_request_finish(res);
			if (!ret || DataResult(std::get<1>(*ret)) == DataResult::Failed) {
				webkit_uri_scheme_request_finish_error(
					request,
					NotFoundError().gobj_());
				g_object_unref(request);
			}
		});
}

void Instance::dataResponse(
		WebKitURISchemeRequest *request,
		int fd,
		int64 offset,
		int64 requestedOffset,
		int64 size,
		int64 total,
		std::string mime) {
	const auto guard = gsl::finally([&] {
		g_object_unref(request);
		close(fd);
	});

	const auto fail = [&] {
		webkit_uri_scheme_request_finish_error(
			request,
			NotFoundError().gobj_());
	};

	if (fd == -1 || offset < 0 || size <= 0) {
		fail();
		return;
	}

	const auto data = mmap(
		nullptr,
		offset + size,
		PROT_READ,
		MAP_PRIVATE,
		fd,
		0);

	if (data == MAP_FAILED) {
		fail();
		return;
	}

	const auto stream = Gio::MemoryInputStream::new_from_bytes(
		GLib::Bytes::new_with_free_func(
			reinterpret_cast<const uchar*>(data) + offset,
			size,
			[=] { munmap(data, offset + size); }));

	const auto response = webkit_uri_scheme_response_new(
		G_INPUT_STREAM(stream.gobj_()),
		size);

	webkit_uri_scheme_response_set_content_type(response, mime.c_str());
	webkit_uri_scheme_request_finish_with_response(request, response);
	g_object_unref(response);
}

ResolveResult Instance::resolve() {
	if (_remoting) {
		if (!_helper) {
			return ResolveResult::IPCFailure;
		}

		const ::base::has_weak_ptr guard;
		std::optional<ResolveResult> result;
		_helper.call_resolve(crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			const auto reply = _helper.call_resolve_finish(res);
			if (reply) {
				result = ResolveResult(std::get<1>(*reply));
			}
			GLib::MainContext::default_().wakeup();
		}));

		while (!result && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		return result.value_or(ResolveResult::IPCFailure);
	}

	return Resolve(_wayland);
}

void Instance::navigate(std::string url) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_navigate(url, nullptr);
		return;
	}

	webkit_web_view_load_uri(_webview, url.c_str());
}

void Instance::navigateToData(std::string id) {
	navigate(_dataDomain + id);
}

void Instance::reload() {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_reload(nullptr);
		return;
	}

	webkit_web_view_reload_bypass_cache(_webview);
}

void Instance::init(std::string js) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_init(js, nullptr);
		return;
	}

	WebKitUserContentManager *manager
		= webkit_web_view_get_user_content_manager(_webview);
	webkit_user_content_manager_add_script(
		manager,
		webkit_user_script_new(
			js.c_str(),
			WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
			nullptr,
			nullptr));
}

void Instance::eval(std::string js) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_eval(js, nullptr);
		return;
	}

	if (webkit_web_view_evaluate_javascript) {
		webkit_web_view_evaluate_javascript(
			_webview,
			js.c_str(),
			-1,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr);
	} else {
		webkit_web_view_run_javascript(
			_webview,
			js.c_str(),
			nullptr,
			nullptr,
			nullptr);
	}
}

void Instance::focus() {
	if (const auto widget = _widget.get()) {
		widget->activateWindow();
	}
}

QWidget *Instance::widget() {
	return _widget.get();
}

void *Instance::winId() {
	if (_remoting) {
		if (!_helper) {
			return nullptr;
		}

		const ::base::has_weak_ptr guard;
		std::optional<void*> ret;
		_helper.call_get_win_id(crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			const auto reply = _helper.call_get_win_id_finish(res);
			ret = reply
				? reinterpret_cast<void*>(std::get<1>(*reply))
				: nullptr;
			GLib::MainContext::default_().wakeup();
		}));

		while (!ret && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		return ret.value_or(nullptr);
	}

	if (_wayland) {
		return nullptr;
	}

	return reinterpret_cast<void*>(gtk_plug_get_id(GTK_PLUG(_window)));
}

void Instance::refreshNavigationHistoryState() {
	// Not needed here, there are events.
}

auto Instance::navigationHistoryState()
-> rpl::producer<NavigationHistoryState> {
	return _navigationHistoryState.value();
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (_remoting) {
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		if (const auto widget = qobject_cast<QQuickWidget*>(_widget.get())) {
			widget->setClearColor(opaqueBg);
		}
#endif // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

		if (!_helper) {
			return;
		}

		_helper.call_set_opaque_bg(
			opaqueBg.red(),
			opaqueBg.green(),
			opaqueBg.blue(),
			opaqueBg.alpha(),
			nullptr);

		return;
	}

	const auto background = std::regex_replace(
		".webviewWindow {background: %1;}",
		std::regex("%1"),
		_wayland ? "transparent" : opaqueBg.name().toStdString());

	if (gtk_css_provider_load_from_string) {
		gtk_css_provider_load_from_string(
			_backgroundProvider,
			background.c_str());
	} else {
		gtk_css_provider_load_from_data(
			_backgroundProvider,
			background.c_str(),
			-1,
			nullptr);
	}
}

void Instance::startProcess() {
	auto loop = GLib::MainLoop::new_();

	auto serviceProcess = Gio::Subprocess::new_({
		::base::Integration::Instance().executablePath().toStdString(),
		std::string("-webviewhelper"),
		SocketPath,
	}, Gio::SubprocessFlags::NONE_);

	if (!serviceProcess) {
		LOG(("WebView Error: %1").arg(
			serviceProcess.error().message_().c_str()));
		return;
	}

	_serviceProcess = *serviceProcess;

	const auto socketPath = std::regex_replace(
		SocketPath,
		std::regex("%1"),
		std::string(_serviceProcess.get_identifier()));

	if (socketPath.empty()) {
		LOG(("WebView Error: IPC socket path is not set."));
		return;
	}

	if (_wayland && !_compositor) {
		_compositor = new Compositor(
			QByteArray::fromStdString(
				GLib::path_get_basename(socketPath + "-wayland")));
	}

	auto authObserver = Gio::DBusAuthObserver::new_();
	authObserver.signal_authorize_authenticated_peer().connect([=](
			Gio::DBusAuthObserver,
			Gio::IOStream stream,
			Gio::Credentials credentials) {
		return credentials.get_unix_pid(nullptr)
			== std::stoi(_serviceProcess.get_identifier());
	});

	auto dbusServer = Gio::DBusServer::new_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusServerFlags::NONE_,
		Gio::dbus_generate_guid(),
		authObserver,
		{});

	if (!dbusServer) {
		LOG(("WebView Error: %1.").arg(
			dbusServer.error().message_().c_str()));
		return;
	}

	_dbusServer = *dbusServer;
	_dbusServer.start();
	const ::base::has_weak_ptr guard;
	auto started = ulong();
	const auto newConnection = _dbusServer.signal_new_connection().connect([&](
			Gio::DBusServer,
			Gio::DBusConnection connection) {
		_master = MasterSkeleton::new_();
		auto object = ObjectSkeleton::new_(kMasterObjectPath);
		object.set_master(_master);
		_dbusObjectManager = Gio::DBusObjectManagerServer::new_(kObjectPath);
		_dbusObjectManager.export_(object);
		_dbusObjectManager.set_connection(connection);
		registerMasterMethodHandlers();

		HelperProxy::new_(
			connection,
			Gio::DBusProxyFlags::NONE_,
			kHelperObjectPath,
			crl::guard(&guard, [&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				auto helper = HelperProxy::new_finish(res);
				if (!helper) {
					LOG(("WebView Error: %1").arg(
						helper.error().message_().c_str()));
					loop.quit();
					return;
				}

				_helper = *helper;

				started = _helper.signal_started().connect([&](Helper) {
					_connected = true;
					loop.quit();
				});
			}));

		connection.signal_closed().connect(crl::guard(this, [=](
				Gio::DBusConnection,
				bool remotePeerVanished,
				GLib::Error_Ref error) {
			_connected = false;
			_widget = nullptr;
			GLib::MainContext::default_().wakeup();
		}));

		return true;
	});

	// timeout in case something goes wrong
	bool timeoutHappened = false;
	const auto timeout = GLib::timeout_add_seconds_once(5, [&] {
		timeoutHappened = true;
		loop.quit();
	});

	loop.run();
	if (timeoutHappened) {
		LOG(("WebView Error: Timed out waiting for WebView helper process."));
	} else {
		GLib::Source::remove(timeout);
	}
	if (_helper && started) {
		_helper.disconnect(started);
	}
	_dbusServer.disconnect(newConnection);
}

void Instance::stopProcess() {
	if (_serviceProcess) {
		_serviceProcess.send_signal(SIGTERM);
	}
	GLib::timeout_add_seconds_once(1, [compositor = _compositor] {
		if (compositor) {
			compositor->deleteLater();
		}
	});
	_compositor = nullptr;
}

void Instance::updateHistoryStates() {
	const auto url = webkit_web_view_get_uri(_webview);
	const auto title = webkit_web_view_get_title(_webview);
	_master.call_navigation_state_update(
		url ? url : "",
		title ? title : "",
		webkit_web_view_can_go_back(_webview),
		webkit_web_view_can_go_forward(_webview),
		nullptr);
}

void Instance::registerMasterMethodHandlers() {
	if (!_master) {
		return;
	}

	_master.signal_handle_get_start_data().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		_master.complete_get_start_data(invocation, [] {
			if (auto app = Gio::Application::get_default()) {
				if (const auto appId = app.get_application_id()) {
					return std::string(appId);
				}
			}

			const auto qtAppId = QGuiApplication::desktopFileName()
				.toStdString();

			if (Gio::Application::id_is_valid(qtAppId)) {
				return qtAppId;
			}

			return std::string();
		}(), _compositor ? _compositor->socketName().toStdString() : "");
		return true;
	});

	_master.signal_handle_message_received().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &message) {
		if (_messageHandler) {
			_messageHandler(message);
			_master.complete_message_received(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
		return true;
	});

	_master.signal_handle_navigation_started().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &uri,
			bool newWindow) {
		if (newWindow) {
			if (_navigationStartHandler && _navigationStartHandler(uri, true)) {
				QDesktopServices::openUrl(QString::fromStdString(uri));
			}
			_master.complete_navigation_started(invocation, false);
		} else if (!std::string(uri).starts_with(_dataDomain)
				&& _navigationStartHandler
				&& !_navigationStartHandler(uri, false)) {
			_master.complete_navigation_started(invocation, false);
		} else {
			_master.complete_navigation_started(invocation, true);
		}

		return true;
	});

	_master.signal_handle_navigation_done().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			bool success) {
		if (_navigationDoneHandler) {
			_navigationDoneHandler(success);
			_master.complete_navigation_done(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
		return true;
	});

	_master.signal_handle_script_dialog().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			int type,
			const std::string &text,
			const std::string &value) {
		if (!_dialogHandler) {
			invocation.return_gerror(MethodError());
			return true;
		}

		const auto dialogType = (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
			? DialogType::Prompt
			: (type == WEBKIT_SCRIPT_DIALOG_ALERT)
			? DialogType::Alert
			: DialogType::Confirm;

		const auto result = _dialogHandler(DialogArgs{
			.type = dialogType,
			.value = value,
			.text = text,
		});

		_master.complete_script_dialog(
			invocation,
			result.accepted,
			result.text);

		return true;
	});

	_master.signal_handle_navigation_state_update().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &url,
			const std::string &title,
			bool canGoBack,
			bool canGoForward) {
		_navigationHistoryState = NavigationHistoryState{
			.url = url,
			.title = title,
			.canGoBack = canGoBack,
			.canGoForward = canGoForward,
		};
		return true;
	});

	_master.signal_handle_data_request().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			uint64 req,
			const std::string &id,
			int64 offset,
			int64 limit) {
		if (!_dataRequestHandler) {
			invocation.return_gerror(MethodError());
			return true;
		}

		_master.complete_data_request(
			invocation,
			int(_dataRequestHandler(DataRequest{
				.id = id,
				.offset = offset,
				.limit = limit,
				.done = crl::guard(this, [=](DataResponse resolved) {
					if (!_helper) {
						return;
					}
					auto &stream = resolved.stream;
					const auto fd = stream ? dup(stream->handle()) : -1;
					const auto size = stream ? stream->size() : 0;
					const auto relatedOffset = offset - resolved.streamOffset;
					const auto relatedSize = size - relatedOffset;
					_helper.call_data_response(
						req,
						GLib::Variant::new_handle(0),
						relatedOffset,
						offset,
						limit > 0
							? std::min(limit, relatedSize)
							: relatedSize,
						resolved.totalSize ?: size,
						stream ? stream->mime() : "",
						Gio::UnixFDList::new_from_array(&fd, 1),
						nullptr,
						nullptr);
				}),
			})));

		return true;
	});
}

int Instance::exec() {
	auto app = Gio::Application::new_(
		Gio::ApplicationFlags::NON_UNIQUE_);

	app.signal_startup().connect([=](Gio::Application) {
		_helper.emit_started();
	});

	app.signal_activate().connect([](Gio::Application) {});

	app.hold();

	auto loop = GLib::MainLoop::new_();

	const auto socketPath = std::regex_replace(
		SocketPath,
		std::regex("%1"),
		std::to_string(getpid()));

	if (socketPath.empty()) {
		g_critical("IPC socket path is not set.");
		return 1;
	}

	{
		auto socketFile = Gio::File::new_for_path(socketPath);

		auto socketMonitor = socketFile.monitor(Gio::FileMonitorFlags::NONE_);
		if (!socketMonitor) {
			g_critical("%s", socketMonitor.error().message_().c_str());
			return 1;
		}

		socketMonitor->signal_changed().connect([&](
				Gio::FileMonitor,
				Gio::File file,
				Gio::File otherFile,
				Gio::FileMonitorEvent eventType) {
			if (eventType == Gio::FileMonitorEvent::CREATED_) {
				loop.quit();
			}
		});

		if (!socketFile.query_exists()) {
			loop.run();
		}
	}

	auto connection = Gio::DBusConnection::new_for_address_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusConnectionFlags::AUTHENTICATION_CLIENT_);

	if (!connection) {
		g_critical("%s", connection.error().message_().c_str());
		return 1;
	}

	_helper = HelperSkeleton::new_();
	auto object = ObjectSkeleton::new_(kHelperObjectPath);
	object.set_helper(_helper);
	_dbusObjectManager = Gio::DBusObjectManagerServer::new_(kObjectPath);
	_dbusObjectManager.export_(object);
	_dbusObjectManager.set_connection(*connection);
	registerHelperMethodHandlers();

	bool error = false;
	MasterProxy::new_(
		*connection,
		Gio::DBusProxyFlags::NONE_,
		kMasterObjectPath,
		[&](GObject::Object source_object, Gio::AsyncResult res) {
			auto master = MasterProxy::new_finish(res);
			if (!master) {
				error = true;
				g_critical("%s", master.error().message_().c_str());
				loop.quit();
				return;
			}
			_master = *master;
			_master.call_get_start_data([&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				const auto settings = _master.call_get_start_data_finish(
					res);
				if (!settings) {
					error = true;
					g_critical("%s", settings.error().message_().c_str());
					loop.quit();
					return;
				}
				if (const auto appId = std::get<1>(*settings)
						; !appId.empty()) {
					app.set_application_id(appId);
				}
				if (const auto waylandDisplay = std::get<2>(*settings)
						; !waylandDisplay.empty()) {
					GLib::setenv("WAYLAND_DISPLAY", waylandDisplay, true);
					_wayland = true;
				}
				loop.quit();
			});
		});

	connection->signal_closed().connect([&](
			Gio::DBusConnection,
			bool remotePeerVanished,
			GLib::Error_Ref error) {
		app.quit();
	});

	loop.run();

	if (error) {
		return 1;
	}

	return app.run({});
}

void Instance::registerHelperMethodHandlers() {
	if (!_helper) {
		return;
	}

	_helper.signal_handle_create().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			bool debug,
			int r,
			int g,
			int b,
			int a,
			const std::string &protocol,
			const std::string &path) {
		if (create({
			.opaqueBg = QColor(r, g, b, a),
			.dataProtocolOverride = protocol,
			.userDataPath = path,
			.debug = debug,
		})) {
			_helper.complete_create(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
		return true;
	});

	_helper.signal_handle_reload().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		reload();
		_helper.complete_reload(invocation);
		return true;
	});

	_helper.signal_handle_resolve().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		_helper.complete_resolve(invocation, int(resolve()));
		return true;
	});

	_helper.signal_handle_navigate().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &url) {
		navigate(url);
		_helper.complete_navigate(invocation);
		return true;
	});

	_helper.signal_handle_init().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &js) {
		init(js);
		_helper.complete_init(invocation);
		return true;
	});

	_helper.signal_handle_eval().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &js) {
		eval(js);
		_helper.complete_eval(invocation);
		return true;
	});

	_helper.signal_handle_set_opaque_bg().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			int r,
			int g,
			int b,
			int a) {
		setOpaqueBg(QColor(r, g, b, a));
		_helper.complete_set_opaque_bg(invocation);
		return true;
	});

	_helper.signal_handle_get_win_id().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		_helper.complete_get_win_id(
			invocation,
			reinterpret_cast<uint64>(winId()));
		return true;
	});

	_helper.signal_handle_data_response().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			Gio::UnixFDList fds,
			uint64 req,
			GLib::Variant fd,
			int64 offset,
			int64 requestedOffset,
			int64 size,
			int64 total,
			const std::string &mime) {
		dataResponse(
			(WebKitURISchemeRequest*)uintptr_t(req),
			fds.get(fd.get_handle(), nullptr),
			offset,
			requestedOffset,
			size,
			total,
			mime);
		_helper.complete_data_response(invocation);
		return true;
	});
}

bool Instance::processRedirect(WebKitURISchemeRequest *request) {
	const auto url = webkit_uri_scheme_request_get_uri(request);
	const auto prefix = _dataDomain;
	if (!url || !std::string(url).starts_with(prefix)) {
		return false;
	}
	const auto id = url + prefix.size();
	const auto dot = strchr(id, '.');
	const auto slash = strchr(id, '/');
	if (!dot || !slash || dot > slash) {
		return false;
	}

	auto session = soup_session_new();
	const auto target = std::string("https://") + id;
	auto msg = soup_message_new("GET", target.c_str());
	if (!msg) {
		g_object_unref(session);
		return false;
	}

	// Copy specific headers from the original request
	const auto originalHeaders = webkit_uri_scheme_request_get_http_headers(request);
	if (originalHeaders) {
		SoupMessageHeaders *newHeaders = nullptr;
		g_object_get(msg, "request-headers", &newHeaders, nullptr);
		const auto copyIfPresent = [&](const char *name) {
			const auto value = soup_message_headers_get_one(
				originalHeaders,
				name);
			if (value) {
				soup_message_headers_append(
					newHeaders,
					name,
					value);
			}
		};
		copyIfPresent("Accept");
		copyIfPresent("User-Agent");
		copyIfPresent("Range");
		copyIfPresent("Accept-Language");
		copyIfPresent("Accept-Encoding");
	}

	// Always set our own Referer
	soup_message_headers_append(
		[&] {
			SoupMessageHeaders *headers = nullptr;
			g_object_get(msg, "request-headers", &headers, nullptr);
			return headers;
		}(),
		"Referer",
		"http://desktop-app-resource/page.html");

	g_object_ref(request);
	soup_session_send_async(
		session,
		msg,
		G_PRIORITY_DEFAULT,
		nullptr,
		[](::GObject *source, ::GAsyncResult *result, gpointer data) {
			auto request = (WebKitURISchemeRequest*)data;
			auto session = (SoupSession*)source;
			auto input = soup_session_send_finish(session, result, nullptr);
			if (!input) {
				webkit_uri_scheme_request_finish_error(
					request,
					g_error_new(
						G_IO_ERROR,
						G_IO_ERROR_FAILED,
						"Network request failed"));
			} else {
				auto response = webkit_uri_scheme_response_new(input, -1);
				webkit_uri_scheme_request_finish_with_response(
					request,
					response);
				g_object_unref(response);
				g_object_unref(input);
			}
			g_object_unref(request);
			g_object_unref(session);
		},
		request);

	g_object_unref(msg);
	return true;
}

} // namespace

Available Availability() {
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
	if (!Platform::IsX11()
			&& Ui::GL::ChooseBackendDefault(Ui::GL::CheckCapabilities())
				!= Ui::GL::Backend::OpenGL) {
		return Available{
			.error = Available::Error::NoOpenGL,
			.details = "Please enable OpenGL in application settings.",
		};
	}
#else // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
	if (!Platform::IsX11()) {
		return Available{
			.error = Available::Error::NonX11,
			.details = "Unsupported display server. Please switch to X11.",
		};
	}
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
	const auto resolved = Instance().resolve();
	if (resolved == ResolveResult::NoLibrary) {
		return Available{
			.error = Available::Error::NoWebKitGTK,
			.details = "Please install WebKitGTK "
			"(webkit2gtk-4.1/webkit2gtk-4.0) "
			"from your package manager.",
		};
	}
	const auto success = (resolved == ResolveResult::Success);
	return Available{
		.customSchemeRequests = success,
		.customReferer = success,
	};
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	auto result = std::make_unique<Instance>();
	if (!result->create(std::move(config))) {
		return nullptr;
	}
	return result;
}

int Exec() {
	return Instance(false).exec();
}

void SetSocketPath(const std::string &socketPath) {
	SocketPath = socketPath;
}

} // namespace Webview::WebKitGTK
