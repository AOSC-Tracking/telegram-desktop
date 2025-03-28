// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webrtc/details/webrtc_environment_video_capture.h"

#include "webrtc/webrtc_environment.h"

#include <api/task_queue/default_task_queue_factory.h>
#include <modules/video_capture/video_capture_factory.h>
#include <modules/audio_device/include/audio_device_factory.h>

#ifdef WEBRTC_LINUX // Breaks compilation on MSVC because of ERROR define.
#include <modules/video_capture/video_capture_options.h>
#endif // WEBRTC_LINUX

namespace Webrtc::details {
namespace {

#ifdef WEBRTC_LINUX
[[nodiscard]] webrtc::VideoCaptureOptions GetVideoCaptureOptions() {
	auto result = webrtc::VideoCaptureOptions();
	result.set_allow_v4l2(true);
#ifdef WEBRTC_USE_PIPEWIRE
	//result.set_allow_pipewire(true);
	// This requires a call to result.Init(callback)
	// and waiting for the callback to finish.
#endif
	return result;
}
#endif

[[nodiscard]] std::vector<DeviceInfo> GetDevices() {
#ifdef WEBRTC_LINUX
	auto options = GetVideoCaptureOptions();
	const auto info = std::unique_ptr<
		webrtc::VideoCaptureModule::DeviceInfo
	>(webrtc::VideoCaptureFactory::CreateDeviceInfo(&options));
#else // WEBRTC_LINUX
	const auto info = std::unique_ptr<
		webrtc::VideoCaptureModule::DeviceInfo
	>(webrtc::VideoCaptureFactory::CreateDeviceInfo());
#endif

	auto result = std::vector<DeviceInfo>();
	if (!info) {
		return result;
	}
	const auto count = info->NumberOfDevices();
	for (auto i = uint32_t(); i != count; ++i) {
		constexpr auto kLengthLimit = 256;
		auto id = std::string(kLengthLimit, char(0));
		auto name = std::string(kLengthLimit, char(0));
		info->GetDeviceName(
			i,
			name.data(),
			name.size(),
			id.data(),
			id.size());
		const auto utfName = QString::fromUtf8(name.c_str());
		const auto utfId = id[0] ? QString::fromUtf8(id.c_str()) : utfName;
		result.push_back({
			.id = utfId,
			.name = utfName,
			.type = DeviceType::Camera,
		});
	}
	return result;
}

} // namespace

EnvironmentVideoCapture::EnvironmentVideoCapture(
	not_null<EnvironmentDelegate*> delegate)
: _delegate(delegate) {
}

EnvironmentVideoCapture::~EnvironmentVideoCapture() {
}

QString EnvironmentVideoCapture::defaultId(DeviceType type) {
	Expects(type == DeviceType::Camera);

	return DefaultId();
}

DeviceInfo EnvironmentVideoCapture::device(
		DeviceType type,
		const QString &id) {
	Expects(type == DeviceType::Camera);

	const auto devices = GetDevices();
	const auto i = ranges::find(devices, id, &DeviceInfo::id);
	return (i != end(devices)) ? *i : DeviceInfo();
}

std::vector<DeviceInfo> EnvironmentVideoCapture::devices(DeviceType type) {
	Expects(type == DeviceType::Camera);

	return GetDevices();
}

bool EnvironmentVideoCapture::refreshFullListOnChange(DeviceType type) {
	Expects(type == DeviceType::Camera);

	return true;
}

bool EnvironmentVideoCapture::desktopCaptureAllowed() const {
	Unexpected("EnvironmentVideoCapture::desktopCaptureAllowed.");
}

auto EnvironmentVideoCapture::uniqueDesktopCaptureSource() const
-> std::optional<QString> {
	Unexpected("EnvironmentVideoCapture::uniqueDesktopCaptureSource.");
}

void EnvironmentVideoCapture::defaultIdRequested(DeviceType type) {
	_delegate->devicesForceRefresh(type);
}

void EnvironmentVideoCapture::devicesRequested(DeviceType type) {
	_delegate->devicesForceRefresh(type);
}

QString EnvironmentVideoCapture::DefaultId() {
	const auto devices = GetDevices();
	return devices.empty() ? QString() : devices.front().id;
}

} // namespace Webrtc::details
