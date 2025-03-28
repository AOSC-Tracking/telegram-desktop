// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkitgtk_library.h"

#include "base/platform/linux/base_linux_library.h"

namespace Webview::WebKitGTK::Library {

ResolveResult Resolve(bool wayland) {
	const auto lib = (wayland
			? base::Platform::LoadLibrary("libwebkitgtk-6.0.so.4", RTLD_NODELETE)
			: nullptr)
		?: base::Platform::LoadLibrary("libwebkit2gtk-4.1.so.0", RTLD_NODELETE)
		?: base::Platform::LoadLibrary("libwebkit2gtk-4.0.so.37", RTLD_NODELETE);
	const auto result = lib
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_init_check)
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_widget_get_type)
		&& (LOAD_LIBRARY_SYMBOL(lib, gtk_window_set_child)
			|| (LOAD_LIBRARY_SYMBOL(lib, gtk_container_get_type)
				&& LOAD_LIBRARY_SYMBOL(lib, gtk_container_add)))
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_window_new)
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_scrolled_window_new)
		&& (LOAD_LIBRARY_SYMBOL(lib, gtk_window_destroy)
			|| LOAD_LIBRARY_SYMBOL(lib, gtk_widget_destroy))
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_window_fullscreen)
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_widget_set_visible)
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_window_get_type)
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_widget_get_display)
		&& (LOAD_LIBRARY_SYMBOL(lib, gtk_widget_add_css_class)
			|| (LOAD_LIBRARY_SYMBOL(lib, gtk_widget_get_style_context)
				&& LOAD_LIBRARY_SYMBOL(lib, gtk_style_context_add_class)))
		&& (LOAD_LIBRARY_SYMBOL(lib, gtk_style_context_add_provider_for_display)
			|| LOAD_LIBRARY_SYMBOL(lib, gtk_style_context_add_provider_for_screen))
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_style_provider_get_type)
		&& LOAD_LIBRARY_SYMBOL(lib, gtk_css_provider_new)
		&& (LOAD_LIBRARY_SYMBOL(lib, gtk_css_provider_load_from_string)
			|| LOAD_LIBRARY_SYMBOL(lib, gtk_css_provider_load_from_data))
		&& (wayland
			|| (LOAD_LIBRARY_SYMBOL(lib, gtk_plug_new)
				&& LOAD_LIBRARY_SYMBOL(lib, gtk_plug_get_id)
				&& LOAD_LIBRARY_SYMBOL(lib, gtk_plug_get_type)))
		&& LOAD_LIBRARY_SYMBOL(lib, soup_session_new)
		&& LOAD_LIBRARY_SYMBOL(lib, soup_session_send_async)
		&& LOAD_LIBRARY_SYMBOL(lib, soup_session_send_finish)
		&& LOAD_LIBRARY_SYMBOL(lib, soup_message_new)
		&& LOAD_LIBRARY_SYMBOL(lib, soup_message_headers_new)
		&& LOAD_LIBRARY_SYMBOL(lib, soup_message_headers_append)
		&& LOAD_LIBRARY_SYMBOL(lib, soup_message_headers_get_one)
		&& (LOAD_LIBRARY_SYMBOL(lib, soup_message_headers_unref)
			|| LOAD_LIBRARY_SYMBOL(lib, soup_message_headers_free))
		&& LOAD_LIBRARY_SYMBOL(lib, jsc_value_to_string)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_navigation_policy_decision_get_type)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_navigation_policy_decision_get_navigation_action)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_navigation_action_get_request)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_request_get_uri)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_policy_decision_ignore)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_script_dialog_get_dialog_type)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_script_dialog_get_message)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_script_dialog_confirm_set_confirmed)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_script_dialog_prompt_get_default_text)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_script_dialog_prompt_set_text)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_get_type)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_get_is_web_process_responsive)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_get_user_content_manager)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_get_uri)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_get_title)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_can_go_back)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_can_go_forward)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_user_content_manager_register_script_message_handler)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_get_settings)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_settings_set_enable_developer_extras)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_is_loading)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_load_uri)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_reload_bypass_cache)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_user_script_new)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_user_content_manager_add_script)
		&& (LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_evaluate_javascript)
			|| LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_run_javascript))
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_set_background_color)
		&& ((LOAD_LIBRARY_SYMBOL(lib, webkit_network_session_new)
				&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_context_new))
			|| (LOAD_LIBRARY_SYMBOL(lib, webkit_web_view_new_with_context)
				&& LOAD_LIBRARY_SYMBOL(lib, webkit_website_data_manager_new)
				&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_context_new_with_website_data_manager)))
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_web_context_register_uri_scheme)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_request_get_path)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_request_finish_error)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_request_finish_with_response)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_request_get_uri)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_request_get_http_headers)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_response_new)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_response_set_content_type)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_response_set_http_headers)
		&& LOAD_LIBRARY_SYMBOL(lib, webkit_uri_scheme_response_set_status);
	if (!result) {
		return ResolveResult::NoLibrary;
	}
	LOAD_LIBRARY_SYMBOL(lib, gtk_widget_show_all);
	LOAD_LIBRARY_SYMBOL(lib, gtk_widget_get_screen);
	LOAD_LIBRARY_SYMBOL(lib, webkit_javascript_result_get_js_value);
	LOAD_LIBRARY_SYMBOL(lib, webkit_website_data_manager_new);
	LOAD_LIBRARY_SYMBOL(lib, webkit_web_context_new_with_website_data_manager);
	if (LOAD_LIBRARY_SYMBOL(lib, gdk_set_allowed_backends)) {
		gdk_set_allowed_backends(wayland ? "wayland" : "x11");
	}
	return gtk_init_check(0, 0)
		? ResolveResult::Success
		: ResolveResult::CantInit;
}

} // namespace Webview::WebKitGTK::Library
