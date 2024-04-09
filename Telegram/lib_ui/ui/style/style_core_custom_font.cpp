// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/style/style_core_custom_font.h"

#include "ui/style/style_core_font.h"

#include <QGuiApplication>
#include <QFontDatabase>

namespace style {
namespace {

using namespace internal;

auto RegularFont = CustomFont();
auto BoldFont = CustomFont();

} // namespace

void SetCustomFonts(const CustomFont &regular, const CustomFont &bold) {
	RegularFont = regular;
	BoldFont = bold;
}

QFont ResolveFont(const QString &familyOverride, uint32 flags, int size) {
	static auto Database = QFontDatabase();

	const auto bold = ((flags & FontBold) || (flags & FontSemibold));
	const QString FontFamilyName = qEnvironmentVariable("TELEGRAM_DESKTOP_UI_FONT");
	const QString FontFamilyNameMono = qEnvironmentVariable("TELEGRAM_DESKTOP_UI_FONT_MONO");
	const QString FontFamilyBoldStyle = qEnvironmentVariable("TELEGRAM_DESKTOP_UI_FONT_BOLD_STYLE");

	auto result = QFont(FontFamilyName.isEmpty() ? QGuiApplication::font().family() : FontFamilyName);
	if (flags & FontMonospace) {
		result.setFamily(FontFamilyNameMono.isEmpty() ? MonospaceFont() : FontFamilyNameMono);
	}
	if (bold) {
		if (FontFamilyBoldStyle.isEmpty()){
			result.setWeight(QFont::Medium);
		}else{
			result.setStyleName(FontFamilyBoldStyle);
		}
	}

	result.setItalic(flags & FontItalic);
	result.setUnderline(flags & FontUnderline);
	result.setStrikeOut(flags & FontStrikeOut);
	result.setPixelSize(size);

	return result;
}

} // namespace style
