// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/effects/numbers_animation.h"

#include "ui/painter.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainter>

namespace Ui {

NumbersAnimation::NumbersAnimation(
	const style::font &font,
	Fn<void()> animationCallback)
: _font(font)
, _duration(st::slideWrapDuration)
, _animationCallback(std::move(animationCallback)) {
	for (auto ch = '0'; ch != '9'; ++ch) {
		accumulate_max(_digitWidth, _font->width(ch));
	}
}

void NumbersAnimation::setDuration(int duration) {
	_duration = duration;
}

void NumbersAnimation::setDisabledMonospace(bool value) {
	_disabledMonospace = value;
}

void NumbersAnimation::setText(const QString &text, int value) {
	if (_a_ready.animating()) {
		_delayedText = text;
		_delayedValue = value;
	} else {
		realSetText(text, value);
	}
}

void NumbersAnimation::animationCallback() {
	if (_animationCallback) {
		_animationCallback();
	}
	if (_widthChangedCallback) {
		_widthChangedCallback();
	}
	if (!_a_ready.animating()) {
		if (!_delayedText.isEmpty()) {
			setText(_delayedText, _delayedValue);
		}
	}
}

void NumbersAnimation::realSetText(QString text, int value) {
	_delayedText = QString();
	_delayedValue = 0;

	_growing = (value > _value);
	_value = value;

	auto newSize = text.size();
	while (_digits.size() < newSize) {
		_digits.push_front(Digit());
	}
	while (_digits.size() > newSize && !_digits.front().to.unicode()) {
		_digits.pop_front();
	}
	auto animating = false;
	auto toFullWidth = 0;
	auto bothFullWidth = 0;
	for (auto i = 0, size = int(_digits.size()); i != size; ++i) {
		auto &digit = _digits[i];
		const auto from = digit.from = digit.to;
		digit.fromWidth = digit.toWidth;
		const auto to = digit.to = (newSize + i < size)
			? QChar(0)
			: text[newSize + i - size];
		digit.toWidth = to.unicode() ? _font->width(to) : 0;
		if (from != to) {
			animating = true;
		}
		const auto toCharWidth = (!_disabledMonospace || to.isDigit())
			? _digitWidth
			: digit.toWidth;
		const auto fromCharWidth = (!_disabledMonospace || from.isDigit())
			? _digitWidth
			: digit.fromWidth;
		const auto charWidth = std::max(toCharWidth, fromCharWidth);
		bothFullWidth += charWidth;
		if (to.unicode()) {
			toFullWidth += charWidth;
		}
	}
	_fromWidth = _toWidth;
	_toWidth = toFullWidth;
	_bothWidth = bothFullWidth;
	if (animating) {
		_a_ready.start(
			[this] { animationCallback(); },
			0.,
			1.,
			_duration);
	}
}

int NumbersAnimation::countWidth() const {
	return anim::interpolate(
		_fromWidth,
		_toWidth,
		anim::easeOutCirc(1., _a_ready.value(1.)));
}

int NumbersAnimation::maxWidth() const {
	return std::max(_fromWidth, _toWidth);
}

void NumbersAnimation::finishAnimating() {
	auto width = countWidth();
	_a_ready.stop();
	if (_widthChangedCallback && countWidth() != width) {
		_widthChangedCallback();
	}
	if (!_delayedText.isEmpty()) {
		setText(_delayedText, _delayedValue);
	}
}

void NumbersAnimation::paint(QPainter &p, int x, int y, int outerWidth) {
	auto digitsCount = _digits.size();
	if (!digitsCount) return;

	auto progress = anim::easeOutCirc(1., _a_ready.value(1.));
	auto width = anim::interpolate(_fromWidth, _toWidth, progress);

	const auto initial = p.opacity();
	QString singleChar('0');
	if (style::RightToLeft()) x = outerWidth - x - width;
	x += width - _bothWidth;
	auto fromTop = anim::interpolate(0, _font->height, progress) * (_growing ? 1 : -1);
	auto toTop = anim::interpolate(_font->height, 0, progress) * (_growing ? -1 : 1);
	for (auto i = 0; i != digitsCount; ++i) {
		auto &digit = _digits[i];
		auto from = digit.from;
		auto to = digit.to;
		const auto toCharWidth = (!_disabledMonospace || to.isDigit())
			? _digitWidth
			: digit.toWidth;
		const auto fromCharWidth = (!_disabledMonospace || from.isDigit())
			? _digitWidth
			: digit.fromWidth;
		if (from == to) {
			p.setOpacity(initial);
			singleChar[0] = from;
			p.drawText(x + (toCharWidth - digit.fromWidth) / 2, y + _font->ascent, singleChar);
		} else {
			if (from.unicode()) {
				p.setOpacity(initial * (1. - progress));
				singleChar[0] = from;
				p.drawText(x + (fromCharWidth - digit.fromWidth) / 2, y + fromTop + _font->ascent, singleChar);
			}
			if (to.unicode()) {
				p.setOpacity(initial * progress);
				singleChar[0] = to;
				p.drawText(x + (toCharWidth - digit.toWidth) / 2, y + toTop + _font->ascent, singleChar);
			}
		}
		x += std::max(toCharWidth, fromCharWidth);
	}
	p.setOpacity(initial);
}

LabelWithNumbers::LabelWithNumbers(
	QWidget *parent,
	const style::FlatLabel &st,
	int textTop,
	const StringWithNumbers &value)
: RpWidget(parent)
, _st(st)
, _textTop(textTop)
, _before(GetBefore(value))
, _after(GetAfter(value))
, _numbers(_st.style.font, [=] { update(); })
, _beforeWidth(_st.style.font->width(_before))
, _afterWidth(st.style.font->width(_after)) {
	Expects((value.offset < 0) == (value.length == 0));

	const auto numbers = GetNumbers(value);
	_numbers.setText(numbers, numbers.toInt());
	_numbers.finishAnimating();
}

QString LabelWithNumbers::GetBefore(const StringWithNumbers &value) {
	return value.text.mid(0, value.offset);
}

QString LabelWithNumbers::GetAfter(const StringWithNumbers &value) {
	return (value.offset >= 0)
		? value.text.mid(value.offset + value.length)
		: QString();
}

QString LabelWithNumbers::GetNumbers(const StringWithNumbers &value) {
	return (value.offset >= 0)
		? value.text.mid(value.offset, value.length)
		: QString();
}

void LabelWithNumbers::setValue(const StringWithNumbers &value) {
	_before = GetBefore(value);
	_after = GetAfter(value);
	const auto numbers = GetNumbers(value);
	_numbers.setText(numbers, numbers.toInt());

	const auto oldBeforeWidth = std::exchange(
		_beforeWidth,
		_st.style.font->width(_before));
	_beforeWidthAnimation.start(
		[this] { update(); },
		oldBeforeWidth,
		_beforeWidth,
		st::slideWrapDuration,
		anim::easeOutCirc);

	_afterWidth = _st.style.font->width(_after);
}

void LabelWithNumbers::finishAnimating() {
	_beforeWidthAnimation.stop();
	_numbers.finishAnimating();
	update();
}

void LabelWithNumbers::paintEvent(QPaintEvent *e) {
	Painter p(this);

	const auto beforeWidth = _beforeWidthAnimation.value(_beforeWidth);

	p.setFont(_st.style.font);
	p.setBrush(Qt::NoBrush);
	p.setPen(_st.textFg);
	auto left = 0;
	const auto outerWidth = width();

	p.setClipRect(0, 0, left + beforeWidth, height());
	p.drawTextLeft(left, _textTop, outerWidth, _before, _beforeWidth);
	left += beforeWidth;
	p.setClipping(false);

	_numbers.paint(p, left, _textTop, outerWidth);
	left += _numbers.countWidth();

	const auto availableWidth = outerWidth - left;
	const auto text = (availableWidth < _afterWidth)
		? _st.style.font->elided(_after, availableWidth)
		: _after;
	const auto textWidth = (availableWidth < _afterWidth) ? -1 : _afterWidth;
	p.drawTextLeft(left, _textTop, outerWidth, text, textWidth);
}

} // namespace Ui
