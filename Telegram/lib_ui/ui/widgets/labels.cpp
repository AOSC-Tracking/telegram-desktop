// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/labels.h"

#include "base/invoke_queued.h"
#include "ui/text/text_entity.h"
#include "ui/effects/animation_value.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/box_content_divider.h"
#include "ui/basic_click_handlers.h" // UrlClickHandler
#include "ui/inactive_press.h"
#include "ui/painter.h"
#include "ui/integration.h"
#include "ui/ui_utility.h"
#include "base/qt/qt_common_adapters.h"
#include "styles/style_layers.h"
#include "styles/palette.h"

#include <QtWidgets/QApplication>
#include <QtGui/QClipboard>
#include <QtGui/QDrag>
#include <QtGui/QtEvents>
#include <QtCore/QMimeData>

namespace Ui {
namespace {

TextParseOptions _labelOptions = {
	TextParseMultiline, // flags
	0, // maxw
	0, // maxh
	Qt::LayoutDirectionAuto, // dir
};

TextParseOptions _labelMarkedOptions = {
	TextParseMultiline | TextParseLinks | TextParseHashtags | TextParseMentions | TextParseBotCommands | TextParseMarkdown, // flags
	0, // maxw
	0, // maxh
	Qt::LayoutDirectionAuto, // dir
};

} // namespace

CrossFadeAnimation::CrossFadeAnimation(
		style::color bg,
		Data &&was,
		Data &&now)
: _bg(bg) {
	const auto maxLines = qMax(was.lineWidths.size(), now.lineWidths.size());
	auto fillDataTill = [&](Data &data) {
		for (auto i = data.lineWidths.size(); i != maxLines; ++i) {
			data.lineWidths.push_back(-1);
		}
	};
	fillDataTill(was);
	fillDataTill(now);
	auto preparePart = [](const Data &data, int index, const Data &other) {
		auto result = CrossFadeAnimation::Part();
		auto lineWidth = data.lineWidths[index];
		if (lineWidth < 0) {
			lineWidth = other.lineWidths[index];
		}
		const auto pixelRatio = style::DevicePixelRatio();
		auto fullWidth = data.full.width() / pixelRatio;
		auto top = index * data.lineHeight + data.lineAddTop;
		auto left = 0;
		if (data.align & Qt::AlignHCenter) {
			left += (fullWidth - lineWidth) / 2;
		} else if (data.align & Qt::AlignRight) {
			left += (fullWidth - lineWidth);
		}
		auto snapshotRect = data.full.rect().intersected(QRect(left * pixelRatio, top * pixelRatio, lineWidth * pixelRatio, data.font->height * pixelRatio));
		if (!snapshotRect.isEmpty()) {
			result.snapshot = PixmapFromImage(data.full.copy(snapshotRect));
			result.snapshot.setDevicePixelRatio(pixelRatio);
		}
		result.position = data.position + QPoint(data.margin.left() + left, data.margin.top() + top);
		return result;
	};
	for (int i = 0; i != maxLines; ++i) {
		addLine(preparePart(was, i, now), preparePart(now, i, was));
	}
}

void CrossFadeAnimation::addLine(Part was, Part now) {
	_lines.push_back(Line(std::move(was), std::move(now)));
}

void CrossFadeAnimation::paintFrame(QPainter &p, float64 dt) {
	auto progress = anim::linear(1., dt);
	paintFrame(p, progress, 1. - progress, progress);
}

void CrossFadeAnimation::paintFrame(
		QPainter &p,
		float64 positionReady,
		float64 alphaWas,
		float64 alphaNow) {
	if (_lines.isEmpty()) return;

	for (const auto &line : std::as_const(_lines)) {
		paintLine(p, line, positionReady, alphaWas, alphaNow);
	}
}

void CrossFadeAnimation::paintLine(
		QPainter &p,
		const Line &line,
		float64 positionReady,
		float64 alphaWas,
		float64 alphaNow) {
	auto &snapshotWas = line.was.snapshot;
	auto &snapshotNow = line.now.snapshot;
	if (snapshotWas.isNull() && snapshotNow.isNull()) {
		// This can happen if both labels have an empty line or if one
		// label has an empty line where the second one already ended.
		// In this case lineWidth is zero and snapshot is null.
		return;
	}

	const auto pixelRatio = style::DevicePixelRatio();
	auto positionWas = line.was.position;
	auto positionNow = line.now.position;
	auto left = anim::interpolate(positionWas.x(), positionNow.x(), positionReady);
	auto topDelta = (snapshotNow.height() / pixelRatio) - (snapshotWas.height() / pixelRatio);
	auto widthDelta = (snapshotNow.width() / pixelRatio) - (snapshotWas.width() / pixelRatio);
	auto topWas = anim::interpolate(positionWas.y(), positionNow.y() + topDelta, positionReady);
	auto topNow = topWas - topDelta;

	p.setOpacity(alphaWas);
	if (!snapshotWas.isNull()) {
		p.drawPixmap(left, topWas, snapshotWas);
		if (topDelta > 0) {
			p.fillRect(left, topWas - topDelta, snapshotWas.width() / pixelRatio, topDelta, _bg);
		}
		if (widthDelta > 0) {
			p.fillRect(left + (snapshotWas.width() / pixelRatio), topNow, widthDelta, snapshotNow.height() / pixelRatio, _bg);
		}
	}

	p.setOpacity(alphaNow);
	if (!snapshotNow.isNull()) {
		p.drawPixmap(left, topNow, snapshotNow);
		if (topDelta < 0) {
			p.fillRect(left, topNow + topDelta, snapshotNow.width() / pixelRatio, -topDelta, _bg);
		}
		if (widthDelta < 0) {
			p.fillRect(left + (snapshotNow.width() / pixelRatio), topWas, -widthDelta, snapshotWas.height() / pixelRatio, _bg);
		}
	}
}

LabelSimple::LabelSimple(
	QWidget *parent,
	const style::LabelSimple &st,
	const QString &value)
: RpWidget(parent)
, _st(st) {
	setText(value);
}

void LabelSimple::setText(const QString &value, bool *outTextChanged) {
	if (_fullText == value) {
		if (outTextChanged) *outTextChanged = false;
		return;
	}

	_fullText = value;
	_fullTextWidth = _st.font->width(_fullText);
	if (!_st.maxWidth || _fullTextWidth <= _st.maxWidth) {
		_text = _fullText;
		_textWidth = _fullTextWidth;
	} else {
		auto newText = _st.font->elided(_fullText, _st.maxWidth);
		if (newText == _text) {
			if (outTextChanged) *outTextChanged = false;
			return;
		}
		_text = newText;
		_textWidth = _st.font->width(_text);
	}
	resize(_textWidth, _st.font->height);
	update();
	if (outTextChanged) *outTextChanged = true;
}

void LabelSimple::paintEvent(QPaintEvent *e) {
	Painter p(this);

	p.setFont(_st.font);
	p.setPen(_st.textFg);
	p.drawTextLeft(0, 0, width(), _text, _textWidth);
}

FlatLabel::FlatLabel(
	QWidget *parent,
	const style::FlatLabel &st,
	const style::PopupMenu &stMenu)
: RpWidget(parent)
, _text(st.minWidth ? st.minWidth : kQFixedMax)
, _st(st)
, _stMenu(stMenu) {
	init();
}

FlatLabel::FlatLabel(
	QWidget *parent,
	const QString &text,
	const style::FlatLabel &st,
	const style::PopupMenu &stMenu)
: RpWidget(parent)
, _text(st.minWidth ? st.minWidth : kQFixedMax)
, _st(st)
, _stMenu(stMenu) {
	setText(text);
	init();
}

FlatLabel::FlatLabel(
	QWidget *parent,
	rpl::producer<QString> &&text,
	const style::FlatLabel &st,
	const style::PopupMenu &stMenu)
: RpWidget(parent)
, _text(st.minWidth ? st.minWidth : kQFixedMax)
, _st(st)
, _stMenu(stMenu) {
	textUpdated();
	std::move(
		text
	) | rpl::start_with_next([this](const QString &value) {
		setText(value);
	}, lifetime());
	init();
}

FlatLabel::FlatLabel(
	QWidget *parent,
	rpl::producer<TextWithEntities> &&text,
	const style::FlatLabel &st,
	const style::PopupMenu &stMenu,
	const Text::MarkedContext &context)
: RpWidget(parent)
, _text(st.minWidth ? st.minWidth : kQFixedMax)
, _st(st)
, _stMenu(stMenu)
, _touchSelectTimer([=] { touchSelect(); }) {
	textUpdated();

	std::move(
		text
	) | rpl::start_with_next([=](const TextWithEntities &value) {
		setMarkedText(value, context);
	}, lifetime());
	init();
}

void FlatLabel::init() {
	_contextCopyText = Integration::Instance().phraseContextCopyText();
}

void FlatLabel::textUpdated() {
	refreshSize();
	setMouseTracking(_selectable || _text.hasLinks());
	if (_text.hasSpoilers()) {
		_text.setSpoilerLinkFilter([weak = base::make_weak(this)](
				const ClickContext &context) {
			return (context.button == Qt::LeftButton) && weak;
		});
	}
	update();
}

void FlatLabel::setText(const QString &text) {
	_text.setText(_st.style, text, _labelOptions);
	textUpdated();
}

void FlatLabel::setMarkedText(
		const TextWithEntities &textWithEntities,
		Text::MarkedContext context) {
	context.repaint = [=] { update(); };
	_text.setMarkedText(
		_st.style,
		textWithEntities,
		_labelMarkedOptions,
		context);
	textUpdated();
}

void FlatLabel::setSelectable(bool selectable) {
	if (_selectable != selectable) {
		_selection = { 0, 0 };
		_savedSelection = { 0, 0 };
		_selectable = selectable;
		setMouseTracking(_selectable || _text.hasLinks());
	}
}

void FlatLabel::setDoubleClickSelectsParagraph(bool doubleClickSelectsParagraph) {
	_doubleClickSelectsParagraph = doubleClickSelectsParagraph;
}

void FlatLabel::setContextCopyText(const QString &copyText) {
	_contextCopyText = copyText;
}

void FlatLabel::setBreakEverywhere(bool breakEverywhere) {
	_breakEverywhere = breakEverywhere;
}

void FlatLabel::setTryMakeSimilarLines(bool tryMakeSimilarLines) {
	_tryMakeSimilarLines = tryMakeSimilarLines;
}

int FlatLabel::resizeGetHeight(int newWidth) {
	_allowedWidth = newWidth;
	_textWidth = countTextWidth();
	return countTextHeight(_textWidth);
}

int FlatLabel::textMaxWidth() const {
	return _text.maxWidth();
}

int FlatLabel::naturalWidth() const {
	return (_st.align == style::al_top) ? -1 : textMaxWidth();
}

QMargins FlatLabel::getMargins() const {
	return _st.margin;
}

int FlatLabel::countTextWidth() const {
	const auto available = _allowedWidth
		? _allowedWidth
		: (_st.minWidth ? _st.minWidth : _text.maxWidth());
	if (_allowedWidth > 0
		&& _allowedWidth < _text.maxWidth()
		&& _tryMakeSimilarLines) {
		auto large = _allowedWidth;
		auto small = _allowedWidth / 2;
		const auto largeHeight = _text.countHeight(large, _breakEverywhere);
		while (large - small > 1) {
			const auto middle = (large + small) / 2;
			if (largeHeight == _text.countHeight(middle, _breakEverywhere)) {
				large = middle;
			} else {
				small = middle;
			}
		}
		return large;
	}
	return available;
}

int FlatLabel::countTextHeight(int textWidth) {
	_fullTextHeight = _text.countHeight(textWidth, _breakEverywhere);
	return _st.maxHeight
		? qMin(_fullTextHeight, _st.maxHeight)
		: _fullTextHeight;
}

void FlatLabel::refreshSize() {
	int textWidth = countTextWidth();
	int textHeight = countTextHeight(textWidth);
	int fullWidth = _st.margin.left() + textWidth + _st.margin.right();
	int fullHeight = _st.margin.top() + textHeight + _st.margin.bottom();
	resize(fullWidth, fullHeight);
}

void FlatLabel::setLink(uint16 index, const ClickHandlerPtr &lnk) {
	_text.setLink(index, lnk);
}

void FlatLabel::setLinksTrusted() {
	static const auto TrustedLinksFilter = [](
			const ClickHandlerPtr &link,
			Qt::MouseButton button) {
		if (const auto url = dynamic_cast<UrlClickHandler*>(link.get())) {
			url->UrlClickHandler::onClick({ button });
			return false;
		}
		return true;
	};
	setClickHandlerFilter(TrustedLinksFilter);
}

void FlatLabel::setClickHandlerFilter(ClickHandlerFilter &&filter) {
	_clickHandlerFilter = std::move(filter);
}

void FlatLabel::overrideLinkClickHandler(Fn<void()> handler) {
	setClickHandlerFilter([=](
			const ClickHandlerPtr &link,
			Qt::MouseButton button) {
		if (button != Qt::LeftButton) {
			return true;
		}
		handler();
		return false;
	});
}

void FlatLabel::overrideLinkClickHandler(Fn<void(QString url)> handler) {
	setClickHandlerFilter([=](
			const ClickHandlerPtr &link,
			Qt::MouseButton button) {
		if (button != Qt::LeftButton) {
			return true;
		}
		handler(link->url());
		return false;
	});
}

void FlatLabel::setContextMenuHook(Fn<void(ContextMenuRequest)> hook) {
	_contextMenuHook = std::move(hook);
}

void FlatLabel::mouseMoveEvent(QMouseEvent *e) {
	_lastMousePos = e->globalPos();
	dragActionUpdate();
}

void FlatLabel::mousePressEvent(QMouseEvent *e) {
	if (_contextMenu) {
		e->accept();
		return; // ignore mouse press, that was hiding context menu
	}
	dragActionStart(e->globalPos(), e->button());
}

Text::StateResult FlatLabel::dragActionStart(const QPoint &p, Qt::MouseButton button) {
	_lastMousePos = p;
	auto state = dragActionUpdate();

	if (button != Qt::LeftButton) return state;

	ClickHandler::pressed();
	_dragAction = NoDrag;
	_dragWasInactive = WasInactivePress(window());
	if (_dragWasInactive) {
		MarkInactivePress(window(), false);
	}

	if (ClickHandler::getPressed()) {
		_dragStartPosition = mapFromGlobal(_lastMousePos);
		_dragAction = PrepareDrag;
	}
	if (!_selectable || _dragAction != NoDrag) {
		return state;
	}

	if (_trippleClickTimer.isActive() && (_lastMousePos - _trippleClickPoint).manhattanLength() < QApplication::startDragDistance()) {
		if (state.uponSymbol) {
			_selection = { state.symbol, state.symbol };
			_savedSelection = { 0, 0 };
			_dragSymbol = state.symbol;
			_dragAction = Selecting;
			_selectionType = TextSelectType::Paragraphs;
			updateHover(state);
			_trippleClickTimer.callOnce(QApplication::doubleClickInterval());
			update();
		}
	}
	if (_selectionType != TextSelectType::Paragraphs) {
		_dragSymbol = state.symbol;
		bool uponSelected = state.uponSymbol;
		if (uponSelected) {
			if (_dragSymbol < _selection.from || _dragSymbol >= _selection.to) {
				uponSelected = false;
			}
		}
		if (uponSelected) {
			_dragStartPosition = mapFromGlobal(_lastMousePos);
			_dragAction = PrepareDrag; // start text drag
		} else if (!_dragWasInactive) {
			if (state.afterSymbol) ++_dragSymbol;
			_selection = { _dragSymbol, _dragSymbol };
			_savedSelection = { 0, 0 };
			_dragAction = Selecting;
			update();
		}
	}
	return state;
}

Text::StateResult FlatLabel::dragActionFinish(const QPoint &p, Qt::MouseButton button) {
	_lastMousePos = p;
	auto state = dragActionUpdate();

	auto activated = ClickHandler::unpressed();
	if (_dragAction == Dragging) {
		activated = nullptr;
	} else if (_dragAction == PrepareDrag) {
		_selection = { 0, 0 };
		_savedSelection = { 0, 0 };
		update();
	}
	_dragAction = NoDrag;
	_selectionType = TextSelectType::Letters;

	if (activated) {
		// _clickHandlerFilter may delete `this`. In that case we don't want
		// to try to show a context menu or smth like that.
		crl::on_main(this, [=] {
			const auto guard = window();
			if (!_clickHandlerFilter
				|| _clickHandlerFilter(activated, button)) {
				ActivateClickHandler(guard, activated, button);
			}
		});
	}

	if (QGuiApplication::clipboard()->supportsSelection()
		&& !_selection.empty()) {
		TextUtilities::SetClipboardText(
			_text.toTextForMimeData(_selection),
			QClipboard::Selection);
	}

	return state;
}

void FlatLabel::mouseReleaseEvent(QMouseEvent *e) {
	dragActionFinish(e->globalPos(), e->button());
	if (!rect().contains(e->pos())) {
		leaveEvent(e);
	}
}

void FlatLabel::mouseDoubleClickEvent(QMouseEvent *e) {
	auto state = dragActionStart(e->globalPos(), e->button());
	if (((_dragAction == Selecting) || (_dragAction == NoDrag)) && _selectionType == TextSelectType::Letters) {
		if (state.uponSymbol) {
			_dragSymbol = state.symbol;
			_selectionType = _doubleClickSelectsParagraph ? TextSelectType::Paragraphs : TextSelectType::Words;
			if (_dragAction == NoDrag) {
				_dragAction = Selecting;
				_selection = { state.symbol, state.symbol };
				_savedSelection = { 0, 0 };
			}
			mouseMoveEvent(e);

			_trippleClickPoint = e->globalPos();
			_trippleClickTimer.callOnce(QApplication::doubleClickInterval());
		}
	}
}

void FlatLabel::enterEventHook(QEnterEvent *e) {
	_lastMousePos = QCursor::pos();
	dragActionUpdate();
}

void FlatLabel::leaveEventHook(QEvent *e) {
	ClickHandler::clearActive(this);
}

void FlatLabel::focusOutEvent(QFocusEvent *e) {
	if (!_selection.empty()) {
		if (_contextMenu) {
			_savedSelection = _selection;
		}
		_selection = { 0, 0 };
		update();
	}
}

void FlatLabel::focusInEvent(QFocusEvent *e) {
	if (!_savedSelection.empty()) {
		_selection = _savedSelection;
		_savedSelection = { 0, 0 };
		update();
	}
}

void FlatLabel::keyPressEvent(QKeyEvent *e) {
	e->ignore();
	if (e->key() == Qt::Key_Copy || (e->key() == Qt::Key_C && e->modifiers().testFlag(Qt::ControlModifier))) {
		if (!_selection.empty()) {
			copySelectedText();
			e->accept();
		}
#ifdef Q_OS_MAC
	} else if (e->key() == Qt::Key_E && e->modifiers().testFlag(Qt::ControlModifier)) {
		auto selection = _selection.empty() ? (_contextMenu ? _savedSelection : _selection) : _selection;
		if (!selection.empty()) {
			TextUtilities::SetClipboardText(_text.toTextForMimeData(selection), QClipboard::FindBuffer);
		}
#endif // Q_OS_MAC
	}
}

void FlatLabel::contextMenuEvent(QContextMenuEvent *e) {
	if (!_contextMenuHook && !_selectable && !_text.hasLinks()) {
		return;
	}

	showContextMenu(e, ContextMenuReason::FromEvent);
}

bool FlatLabel::eventHook(QEvent *e) {
	if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate || e->type() == QEvent::TouchEnd || e->type() == QEvent::TouchCancel) {
		QTouchEvent *ev = static_cast<QTouchEvent*>(e);
		if (ev->device()->type() == base::TouchDevice::TouchScreen) {
			touchEvent(ev);
			return true;
		}
	}
	return RpWidget::eventHook(e);
}

void FlatLabel::touchEvent(QTouchEvent *e) {
	if (e->type() == QEvent::TouchCancel) { // cancel
		if (!_touchInProgress) return;
		_touchInProgress = false;
		_touchSelectTimer.cancel();
		_touchSelect = false;
		_dragAction = NoDrag;
		return;
	}

	if (!e->touchPoints().isEmpty()) {
		_touchPrevPos = _touchPos;
		_touchPos = e->touchPoints().cbegin()->screenPos().toPoint();
	}

	switch (e->type()) {
	case QEvent::TouchBegin: {
		if (_contextMenu) {
			e->accept();
			return; // ignore mouse press, that was hiding context menu
		}
		if (_touchInProgress) return;
		if (e->touchPoints().isEmpty()) return;

		_touchInProgress = true;
		_touchSelectTimer.callOnce(QApplication::startDragTime());
		_touchSelect = false;
		_touchStart = _touchPrevPos = _touchPos;
	} break;

	case QEvent::TouchUpdate: {
		if (!_touchInProgress) return;
		if (_touchSelect) {
			_lastMousePos = _touchPos;
			dragActionUpdate();
		}
	} break;

	case QEvent::TouchEnd: {
		if (!_touchInProgress) return;
		_touchInProgress = false;
		auto weak = base::make_weak(this);
		if (_touchSelect) {
			dragActionFinish(_touchPos, Qt::RightButton);
			QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, mapFromGlobal(_touchPos), _touchPos);
			showContextMenu(&contextMenu, ContextMenuReason::FromTouch);
		} else { // one short tap -- like mouse click
			dragActionStart(_touchPos, Qt::LeftButton);
			dragActionFinish(_touchPos, Qt::LeftButton);
		}
		if (weak) {
			_touchSelectTimer.cancel();
			_touchSelect = false;
		}
	} break;
	}
}

void FlatLabel::showContextMenu(QContextMenuEvent *e, ContextMenuReason reason) {
	if (e->reason() == QContextMenuEvent::Mouse) {
		_lastMousePos = e->globalPos();
	} else {
		_lastMousePos = QCursor::pos();
	}
	const auto state = dragActionUpdate();
	const auto hasSelection = _selectable && !_selection.empty();
	const auto uponSelection = _selectable
		&& ((reason == ContextMenuReason::FromTouch && hasSelection)
			|| (state.uponSymbol
				&& (state.symbol >= _selection.from)
				&& (state.symbol < _selection.to)));

	_contextMenu = base::make_unique_q<PopupMenu>(this, _stMenu);
	const auto request = ContextMenuRequest{
		.menu = _contextMenu.get(),
		.link = ClickHandler::getActive(),
		.selection = _selectable ? _selection : TextSelection(),
		.uponSelection = uponSelection,
		.fullSelection = _selectable && _text.isFullSelection(_selection),
	};

	if (_contextMenuHook) {
		_contextMenuHook(request);
	} else {
		fillContextMenu(request);
	}

	if (_contextMenu->empty()) {
		_contextMenu = nullptr;
	} else {
		_contextMenu->popup(e->globalPos());
		e->accept();
	}
}

void FlatLabel::fillContextMenu(ContextMenuRequest request) {
	if (request.fullSelection && !_contextCopyText.isEmpty()) {
		request.menu->addAction(
			_contextCopyText,
			[=] { copyContextText(); });
	} else if (request.uponSelection && !request.fullSelection) {
		request.menu->addAction(
			Integration::Instance().phraseContextCopySelected(),
			[=] { copySelectedText(); });
	} else if (_selectable
		&& request.selection.empty()
		&& !_contextCopyText.isEmpty()) {
		request.menu->addAction(
			_contextCopyText,
			[=] { copyContextText(); });
	}

	if (request.link) {
		const auto label = request.link->copyToClipboardContextItemText();
		if (!label.isEmpty()) {
			request.menu->addAction(
				label,
				[text = request.link->copyToClipboardText()] {
					QGuiApplication::clipboard()->setText(text);
				});
		}
	}
}

void FlatLabel::copySelectedText() {
	const auto selection = _selection.empty() ? (_contextMenu ? _savedSelection : _selection) : _selection;
	if (!selection.empty()) {
		TextUtilities::SetClipboardText(_text.toTextForMimeData(selection));
	}
}

void FlatLabel::copyContextText() {
	TextUtilities::SetClipboardText(_text.toTextForMimeData());
}

void FlatLabel::touchSelect() {
	_touchSelect = true;
	dragActionStart(_touchPos, Qt::LeftButton);
}

void FlatLabel::executeDrag() {
	if (_dragAction != Dragging) return;

	auto state = getTextState(_dragStartPosition);
	bool uponSelected = state.uponSymbol && _selection.from <= state.symbol;
	if (uponSelected) {
		if (_dragSymbol < _selection.from || _dragSymbol >= _selection.to) {
			uponSelected = false;
		}
	}

	const auto pressedHandler = ClickHandler::getPressed();
	const auto selectedText = [&] {
		if (uponSelected) {
			return _text.toTextForMimeData(_selection);
		} else if (pressedHandler) {
			return TextForMimeData::Simple(pressedHandler->dragText());
		}
		return TextForMimeData();
	}();
	if (auto mimeData = TextUtilities::MimeDataFromText(selectedText)) {
		auto drag = new QDrag(window());
		drag->setMimeData(mimeData.release());
		drag->exec(Qt::CopyAction);

		// We don't receive mouseReleaseEvent when drag is finished.
		ClickHandler::unpressed();
	}
}

void FlatLabel::clickHandlerActiveChanged(const ClickHandlerPtr &action, bool active) {
	update();
}

void FlatLabel::clickHandlerPressedChanged(const ClickHandlerPtr &action, bool active) {
	update();
}

CrossFadeAnimation::Data FlatLabel::crossFadeData(
		style::color bg,
		QPoint basePosition) {
	auto result = CrossFadeAnimation::Data();
	result.full = GrabWidgetToImage(this, QRect(), bg->c);
	const auto textWidth = width() - _st.margin.left() - _st.margin.right();
	result.lineWidths = _text.countLineWidths(textWidth, {
		.breakEverywhere = _breakEverywhere,
	});
	result.lineHeight = _st.style.font->height;
	const auto addedHeight = (_st.style.lineHeight - result.lineHeight);
	if (addedHeight > 0) {
		result.lineAddTop = addedHeight / 2;
		result.lineHeight += addedHeight;
	}
	result.position = basePosition + pos();
	result.align = _st.align;
	result.font = _st.style.font;
	result.margin = _st.margin;
	return result;
}

std::unique_ptr<CrossFadeAnimation> FlatLabel::CrossFade(
		not_null<FlatLabel*> from,
		not_null<FlatLabel*> to,
		style::color bg,
		QPoint fromPosition,
		QPoint toPosition) {
	return std::make_unique<CrossFadeAnimation>(
		bg,
		from->crossFadeData(bg, fromPosition),
		to->crossFadeData(bg, toPosition));
}

Text::StateResult FlatLabel::dragActionUpdate() {
	auto m = mapFromGlobal(_lastMousePos);
	auto state = getTextState(m);
	updateHover(state);

	if (_dragAction == PrepareDrag && (m - _dragStartPosition).manhattanLength() >= QApplication::startDragDistance()) {
		_dragAction = Dragging;
		InvokeQueued(this, [=] { executeDrag(); });
	}

	return state;
}

void FlatLabel::updateHover(const Text::StateResult &state) {
	bool lnkChanged = ClickHandler::setActive(state.link, this);

	if (!_selectable) {
		refreshCursor(state.uponSymbol);
		return;
	}

	Qt::CursorShape cur = style::cur_default;
	if (_dragAction == NoDrag) {
		if (state.link) {
			cur = style::cur_pointer;
		} else if (state.uponSymbol) {
			cur = style::cur_text;
		}
	} else {
		if (_dragAction == Selecting) {
			uint16 second = state.symbol;
			if (state.afterSymbol && _selectionType == TextSelectType::Letters) {
				++second;
			}
			auto selection = _text.adjustSelection({ qMin(second, _dragSymbol), qMax(second, _dragSymbol) }, _selectionType);
			if (_selection != selection) {
				_selection = selection;
				_savedSelection = { 0, 0 };
				setFocus();
				update();
			}
		} else if (_dragAction == Dragging) {
		}

		if (ClickHandler::getPressed()) {
			cur = style::cur_pointer;
		} else if (_dragAction == Selecting) {
			cur = style::cur_text;
		}
	}
	if (_dragAction == Selecting) {
		//		checkSelectingScroll();
	} else {
		//		noSelectingScroll();
	}

	if (_dragAction == NoDrag && (lnkChanged || cur != _cursor)) {
		setCursor(_cursor = cur);
	}
}

void FlatLabel::refreshCursor(bool uponSymbol) {
	if (_dragAction != NoDrag) {
		return;
	}
	bool needTextCursor = _selectable && uponSymbol;
	style::cursor newCursor = needTextCursor ? style::cur_text : style::cur_default;
	if (ClickHandler::getActive()) {
		newCursor = style::cur_pointer;
	}
	if (newCursor != _cursor) {
		_cursor = newCursor;
		setCursor(_cursor);
	}
}

Text::StateResult FlatLabel::getTextState(const QPoint &m) const {
	Text::StateRequestElided request;
	request.align = _st.align;
	if (_selectable) {
		request.flags |= Text::StateRequest::Flag::LookupSymbol;
	}
	const auto textWidth = _textWidth
		? _textWidth
		: (width() - _st.margin.left() - _st.margin.right());
	const auto useWidth = !(_st.align & Qt::AlignLeft)
		? textWidth
		: std::min(textWidth, _text.maxWidth());
	const auto textLeft = _textWidth
		? ((_st.align & Qt::AlignLeft)
			? _st.margin.left()
			: (_st.align & Qt::AlignHCenter)
			? ((width() - _textWidth) / 2)
			: (width() - _st.margin.right() - _textWidth))
		: _st.margin.left();

	Text::StateResult state;
	bool heightExceeded = _st.maxHeight && (_st.maxHeight < _fullTextHeight || useWidth < _text.maxWidth());
	bool renderElided = _breakEverywhere || heightExceeded;
	if (renderElided) {
		auto lineHeight = qMax(_st.style.lineHeight, _st.style.font->height);
		auto lines = _st.maxHeight ? qMax(_st.maxHeight / lineHeight, 1) : ((height() / lineHeight) + 2);
		request.lines = lines;
		if (_breakEverywhere) {
			request.flags |= Text::StateRequest::Flag::BreakEverywhere;
		}
		state = _text.getStateElided(m - QPoint(textLeft, _st.margin.top()), useWidth, request);
	} else {
		state = _text.getState(m - QPoint(textLeft, _st.margin.top()), useWidth, request);
	}

	return state;
}

void FlatLabel::setOpacity(float64 o) {
	_opacity = o;
	update();
}

void FlatLabel::setTextColorOverride(std::optional<QColor> color) {
	_textColorOverride = color;
	update();
}

void FlatLabel::paintEvent(QPaintEvent *e) {
	Painter p(this);
	p.setOpacity(_opacity);
	if (_textColorOverride) {
		p.setPen(*_textColorOverride);
	} else {
		p.setPen(_st.textFg);
	}
	p.setTextPalette(_st.palette);
	const auto textWidth = _textWidth
		? _textWidth
		: (width() - _st.margin.left() - _st.margin.right());
	const auto textLeft = _textWidth
		? ((_st.align & Qt::AlignLeft)
			? _st.margin.left()
			: (_st.align & Qt::AlignHCenter)
			? ((width() - _textWidth) / 2)
			: (width() - _st.margin.right() - _textWidth))
		: _st.margin.left();
	const auto selection = !_selection.empty()
		? _selection
		: _contextMenu
		? _savedSelection
		: _selection;
	const auto heightExceeded = _st.maxHeight
		&& (_st.maxHeight < _fullTextHeight || textWidth < _text.maxWidth());
	const auto renderElided = _breakEverywhere || heightExceeded;
	const auto lineHeight = qMax(_st.style.lineHeight, _st.style.font->height);
	const auto elisionHeight = !renderElided
		? 0
		: _st.maxHeight
		? qMax(_st.maxHeight, lineHeight)
		: height();
	const auto paused = _animationsPausedCallback
		? _animationsPausedCallback()
		: WhichAnimationsPaused::None;
	_text.draw(p, {
		.position = { textLeft, _st.margin.top() },
		.availableWidth = textWidth,
		.align = _st.align,
		.clip = e->rect(),
		.palette = &_st.palette,
		.spoiler = Text::DefaultSpoilerCache(),
		.now = crl::now(),
		.pausedEmoji = (paused == WhichAnimationsPaused::CustomEmoji
			|| paused == WhichAnimationsPaused::All),
		.pausedSpoiler = (paused == WhichAnimationsPaused::Spoiler
			|| paused == WhichAnimationsPaused::All),
		.selection = selection,
		.elisionHeight = elisionHeight,
		.elisionBreakEverywhere = renderElided && _breakEverywhere,
	});
}

DividerLabel::DividerLabel(
	QWidget *parent,
	object_ptr<RpWidget> &&child,
	const style::margins &padding,
	RectParts parts)
: PaddingWrap(parent, std::move(child), padding)
, _background(this, st::boxDividerHeight, st::boxDividerBg, parts) {
}

int DividerLabel::naturalWidth() const {
	return -1;
}

void DividerLabel::resizeEvent(QResizeEvent *e) {
	_background->lower();
	_background->setGeometry(rect());
	return PaddingWrap::resizeEvent(e);
}

} // namespace Ui
