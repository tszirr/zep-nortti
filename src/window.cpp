#include <cctype>
#include <cmath>
#include <sstream>

#include "zep/buffer.h"
#include "zep/display.h"
#include "zep/mode.h"
#include "zep/scroller.h"
#include "zep/syntax.h"
#include "zep/tab_window.h"
#include "zep/theme.h"
#include "zep/window.h"

#include "zep/mcommon/logger.h"
#include "zep/mcommon/string/stringutils.h"
#include "zep/mcommon/utf8.h"

namespace
{
struct WindowPass
{
    enum Pass
    {
        Background = 0,
        Text,
        Max
    };
};
} // namespace

// A 'window' is like a vim window; i.e. a region inside a tab
namespace Zep
{

const float ScrollBarSize = 17.0f;
ZepWindow::ZepWindow(ZepTabWindow& window, ZepBuffer* buffer)
    : ZepComponent(window.GetEditor())
    , m_tabWindow(window)
    , m_pBuffer(buffer)
{
    m_bufferRegion = std::make_shared<Region>();
    m_numberRegion = std::make_shared<Region>();
    m_indicatorRegion = std::make_shared<Region>();
    m_textRegion = std::make_shared<Region>();
    m_airlineRegion = std::make_shared<Region>();
    m_vScrollRegion = std::make_shared<Region>();

    m_bufferRegion->flags = RegionFlags::Expanding;
    m_bufferRegion->layoutType = RegionLayoutType::VBox;

    m_numberRegion->flags = RegionFlags::Fixed;
    m_indicatorRegion->flags = RegionFlags::Fixed;
    m_vScrollRegion->flags = RegionFlags::Fixed;
    m_textRegion->flags = RegionFlags::Expanding;
    m_airlineRegion->flags = RegionFlags::Fixed;

    m_editRegion = std::make_shared<Region>();
    m_editRegion->flags = RegionFlags::Expanding;
    m_editRegion->layoutType = RegionLayoutType::HBox;

    // A little daylight between the indicators
    m_textRegion->padding = NVec2f(DPI_X(8), 0);

    // Ensure that the main area with text, numbers, indicators fills the remaining space
    m_expandingEditRegion = std::make_shared<Region>();
    m_expandingEditRegion->flags = RegionFlags::Expanding;
    m_expandingEditRegion->layoutType = RegionLayoutType::HBox;
    m_expandingEditRegion->children.push_back(m_editRegion);

    m_bufferRegion->children.push_back(m_expandingEditRegion);
    m_editRegion->children.push_back(m_numberRegion);
    m_editRegion->children.push_back(m_indicatorRegion);
    m_editRegion->children.push_back(m_textRegion);
    m_editRegion->children.push_back(m_vScrollRegion);

    m_bufferRegion->children.push_back(m_airlineRegion);

    m_vScroller = std::make_shared<Scroller>(GetEditor(), *m_vScrollRegion);
    m_vScroller->vertical = false;

    timer_start(m_toolTipTimer);
}

ZepWindow::~ZepWindow()
{
    std::for_each(m_windowLines.begin(), m_windowLines.end(), [](SpanInfo* pInfo) { delete pInfo; });
}

void ZepWindow::UpdateScrollers()
{
    m_scrollVisibilityChanged = false;

    // For now, scrollers are either on or off; and don't disappear
    auto old_percent = m_vScroller->vScrollVisiblePercent;
    if (m_maxDisplayLines == 0)
    {
        m_vScroller->vScrollVisiblePercent = 1.0f;
        m_scrollVisibilityChanged = (old_percent != m_vScroller->vScrollVisiblePercent);
        return;
    }
    m_vScroller->vScrollVisiblePercent = std::min(m_textRegion->rect.Height() / m_textSizePx.y, 1.0f);
    m_vScroller->vScrollPosition = std::abs(m_textOffsetPx) / m_textSizePx.y;
    m_vScroller->vScrollLinePercent = 1.0f / m_windowLines.size();
    m_vScroller->vScrollPagePercent = m_vScroller->vScrollVisiblePercent;

    if (GetEditor().GetConfig().showScrollBar == 0 || ZTestFlags(GetWindowFlags(), WindowFlags::HideScrollBar))
    {
        m_vScrollRegion->fixed_size = NVec2f(0.0f);
    }
    else
    {
        if (m_vScroller->vScrollVisiblePercent >= 1.0f && GetEditor().GetConfig().showScrollBar != 2)
        {
            m_vScrollRegion->fixed_size = NVec2f(0.0f);
        }
        else
        {
            m_vScrollRegion->fixed_size = NVec2f(ScrollBarSize * GetEditor().GetPixelScale().x, 0.0f);
        }
    }

    if (m_vScrollRegion->rect.Width() != m_vScrollRegion->fixed_size.x)
    {
        m_scrollVisibilityChanged = true;
    }
}

void ZepWindow::UpdateAirline()
{
    m_airline.leftBoxes.clear();
    m_airline.rightBoxes.clear();

    if (IsActiveWindow())
    {
        m_airline.leftBoxes.push_back(AirBox{ GetBuffer().GetMode()->Name(), FilterActiveColor(m_pBuffer->GetTheme().GetColor(ThemeColor::Mode)) });
        switch (GetBuffer().GetMode()->GetEditorMode())
        {
            /*case EditorMode::Hidden:
            m_airline.leftBoxes.push_back(AirBox{ "HIDDEN", m_pBuffer->GetTheme().GetColor(ThemeColor::HiddenText) });
            break;
            */
        default:
            break;
        case EditorMode::Insert:
            m_airline.leftBoxes.push_back(AirBox{ "INSERT", FilterActiveColor(m_pBuffer->GetTheme().GetColor(ThemeColor::CursorInsert)) });
            break;
        case EditorMode::None:
        case EditorMode::Normal:
            m_airline.leftBoxes.push_back(AirBox{ "NORMAL", FilterActiveColor(m_pBuffer->GetTheme().GetColor(ThemeColor::CursorNormal)) });
            break;
        case EditorMode::Visual:
            m_airline.leftBoxes.push_back(AirBox{ "VISUAL", FilterActiveColor(m_pBuffer->GetTheme().GetColor(ThemeColor::VisualSelectBackground)) });
            break;
        };
    }

    auto cursor = BufferToDisplay();
    m_airline.leftBoxes.push_back(AirBox{ m_pBuffer->GetDisplayName(), FilterActiveColor(m_pBuffer->GetTheme().GetColor(ThemeColor::AirlineBackground)) });
    m_airline.leftBoxes.push_back(AirBox{ std::to_string(cursor.x) + ":" + std::to_string(cursor.y), m_pBuffer->GetTheme().GetColor(ThemeColor::TabActive) });

    #ifdef _DEBUG
    m_airline.leftBoxes.push_back(AirBox{ "(" + std::to_string(GetEditor().GetPixelScale().x) + "," + std::to_string(GetEditor().GetPixelScale().y) + ")", m_pBuffer->GetTheme().GetColor(ThemeColor::Error) });
    #endif

    auto extra = GetBuffer().GetMode()->GetAirlines(*this);

    auto lastSize = m_airlineRegion->fixed_size;
    m_airlineRegion->fixed_size = NVec2f(0.0f, GetEditor().GetDisplay().GetFontHeightPixels() * (1 + extra.size()));
    if (m_airlineRegion->fixed_size != lastSize)
    {
        m_layoutDirty = true;
    }
}

void ZepWindow::Notify(std::shared_ptr<ZepMessage> payload)
{
    if (payload->messageId == Msg::Buffer)
    {
        auto pMsg = std::static_pointer_cast<BufferMessage>(payload);

        if (pMsg->pBuffer != m_pBuffer)
        {
            return;
        }

        m_layoutDirty = true;

        if (pMsg->type != BufferMessageType::PreBufferChange)
        {
            // Make sure the cursor is on its 'display' part of the flash cycle after an edit.
            GetEditor().ResetCursorTimer();
        }

        // Remove tooltips that might be present
        DisableToolTipTillMove();
    }
    else if (payload->messageId == Msg::ComponentChanged)
    {
        if (payload->pComponent == m_vScroller.get())
        {
            auto pScroller = static_cast<Scroller*>(payload->pComponent);
            m_textOffsetPx = pScroller->vScrollPosition * m_textSizePx.y;
            UpdateVisibleLineRange();
            EnsureCursorVisible();
            DisableToolTipTillMove();
        }
    }
    else if (payload->messageId == Msg::MouseMove)
    {
        if (!m_toolTips.empty())
        {
            if (ManhattanDistance(m_mouseHoverPos, payload->pos) > 4.0f)
            {
                timer_restart(m_toolTipTimer);
                m_toolTips.clear();
            }
        }
        else
        {
            timer_restart(m_toolTipTimer);
            m_mouseHoverPos = payload->pos;

            // Can now show tooltip again, due to mouse hover
            m_tipDisabledTillMove = false;
        }
    }
    else if (payload->messageId == Msg::ConfigChanged)
    {
        m_layoutDirty = true;
    }
}

void ZepWindow::SetDisplayRegion(const NRectf& region)
{
    if (m_bufferRegion->rect == region)
    {
        return;
    }

    m_displayRect = region;

    m_layoutDirty = true;
    m_bufferRegion->rect = region;

    m_airlineRegion->fixed_size = NVec2f(0.0f, GetEditor().GetDisplay().GetFontHeightPixels());

    m_defaultLineSize = GetEditor().GetDisplay().GetFontHeightPixels();
}

void ZepWindow::EnsureCursorVisible()
{
    UpdateLayout();
    ByteIndex loc = m_bufferCursor;
    for (auto& line : m_windowLines)
    {
        if (line->lineByteRange.first <= loc && line->lineByteRange.second > loc)
        {
            auto cursorLine = line->spanLineIndex;
            if (cursorLine < m_visibleLineIndices.x)
            {
                MoveCursorY(std::abs(m_visibleLineIndices.x - cursorLine));
            }
            else if (cursorLine >= m_visibleLineIndices.y)
            {
                MoveCursorY((long(m_visibleLineIndices.y) - cursorLine) - 1);
            }
            m_cursorMoved = false;
            return;
        }
    }
}

void ZepWindow::ScrollToCursor()
{
    if (!m_cursorMoved)
    {
        return;
    }

    auto lineMargins = DPI_VEC2(GetEditor().GetConfig().lineMargins);
    auto old_offset = m_textOffsetPx;
    auto two_lines = (GetEditor().GetDisplay().GetFontHeightPixels() * 2); // +(lineMargins.x + lineMargins.y) * 2;
    auto& cursorLine = GetCursorLineInfo(BufferToDisplay().y);

    // If the buffer is beyond two lines above the cursor position, move it back by the difference
    if (m_textOffsetPx > (cursorLine.yOffsetPx - two_lines))
    {
        m_textOffsetPx -= (m_textOffsetPx - (cursorLine.yOffsetPx - two_lines));
    }
    else if ((m_textOffsetPx + m_textRegion->rect.Height() - two_lines) < cursorLine.yOffsetPx)
    {
        m_textOffsetPx += cursorLine.yOffsetPx - (m_textOffsetPx + m_textRegion->rect.Height() - two_lines);
    }

    m_textOffsetPx = std::min(m_textOffsetPx, m_textSizePx.y - m_textRegion->rect.Height());
    m_textOffsetPx = std::max(0.f, m_textOffsetPx);

    if (old_offset != m_textOffsetPx)
    {
        UpdateVisibleLineRange();
    }
    m_cursorMoved = false;
}

void ZepWindow::GetCharPointer(ByteIndex loc, const uint8_t*& pBegin, const uint8_t*& pEnd, SpecialChar& special)
{
    static char invalidChar;
    static const char blankSpace = ' ';

    pBegin = &m_pBuffer->GetText()[loc];

    // Shown only one char for end of line
    special = SpecialChar::None;
    if (*pBegin == '\n' || *pBegin == 0)
    {
        invalidChar = '@' + *pBegin;
        if (GetWindowFlags() & WindowFlags::ShowCR)
        {
            pBegin = (const uint8_t*)&invalidChar;
        }
        else
        {
            pBegin = (const uint8_t*)&blankSpace;
        }
        special = SpecialChar::Hidden;
    }
    else if (*pBegin == '\t')
    {
        special = SpecialChar::Tab;
    }
    else if (*pBegin == ' ')
    {
        special = SpecialChar::Space;
    }

    pEnd = pBegin + utf8_codepoint_length(*pBegin);
}

float ZepWindow::GetLineTopPadding(long line)
{
    float height = DPI_Y((float)GetEditor().GetConfig().lineMargins.x);
    auto pLineWidgets = m_pBuffer->GetLineWidgets(line);
    if (pLineWidgets)
    {
        for (auto& widget : *pLineWidgets)
        {
            auto size = DPI_VEC2(widget->GetSize());
            auto margins = DPI_VEC2(GetEditor().GetConfig().widgetMargins);

            // Each widget has a margin then its height then the bottom
            height += margins.x;
            height += size.y;
            height += margins.y;
        }
    }

    return height;
}

// This is the most expensive part of window update; applying line span generation for wrapped text and unicode
// character sizes which may vary in byte count and physical pixel width
// It can take about a millisecond to do during editing on release buildj; but this is fast enough for now.
// There are several ways in which this function can be optimized:
// - Only regenerate spans which have changed, since only the edited line range will change.
// - Generate blocks of text, based on syntax highlighting, instead of single characters.
// - Have a no-wrap text mode and save a lot of the wrapping work.
// - Do some threading
void ZepWindow::UpdateLineSpans()
{
    TIME_SCOPE(UpdateLineSpans);

    //LOG(DEBUG) << "UpdateLineSpans: " << (uint64_t)this << ", " << m_pBuffer->GetName();
    m_maxDisplayLines = (long)std::max(0.0f, std::floor(m_textRegion->rect.Height() / m_defaultLineSize));

    const auto& textBuffer = m_pBuffer->GetText();

    long bufferLine = 0;
    long spanLine = 0;
    float bufferPosYPx = 0.0f;
    float xOffset = m_xPad;

    // Nuke the existing spans
    // In future we can in-place modify for speed
    std::for_each(m_windowLines.begin(), m_windowLines.end(), [](SpanInfo* pInfo) { delete pInfo; });
    m_windowLines.clear();

    auto& display = GetEditor().GetDisplay();

    float textHeight = GetEditor().GetDisplay().GetFontHeightPixels();

    // Process every buffer line
    for (;;)
    {
        // We haven't processed this line yet, so we can't display anything
        // else
        if (m_pBuffer->GetLineCount() <= bufferLine)
            break;

        BufferByteRange lineByteRange;
        if (!m_pBuffer->GetLineOffsets(bufferLine, lineByteRange.first, lineByteRange.second))
            break;

        NVec2f padding = NVec2f(GetLineTopPadding(bufferLine), DPI_Y((float)GetEditor().GetConfig().lineMargins.y));
        float fullLineHeight = textHeight + padding.x + padding.y;

        // Start a new line
        SpanInfo* lineInfo = new SpanInfo();
        lineInfo->bufferLineNumber = bufferLine;
        lineInfo->spanLineIndex = spanLine;
        lineInfo->lineByteRange.first = lineByteRange.first;
        lineInfo->lineByteRange.second = lineByteRange.first;
        lineInfo->yOffsetPx = bufferPosYPx;
        lineInfo->padding = padding;
        lineInfo->textSizePx.x = xOffset;
        lineInfo->textSizePx.y = textHeight;
        lineInfo->isSplitContinuation = false;

        // These offsets are 0 -> n + 1, i.e. the last offset the buffer returns is 1 beyond the current
        // Note: Must not use pointers into the character buffer!
        for (auto ch = lineByteRange.first; ch < lineByteRange.second; ch += utf8_codepoint_length(textBuffer[ch]))
        {
            const uint8_t* pCh = &textBuffer[ch];
            const auto textSize = display.GetCharSize(pCh);

            // Wrap if we have displayed at least one char, and we have to
            if (ZTestFlags(GetWindowFlags(), WindowFlags::WrapText) && ch != lineByteRange.first)
            {
                // At least a single char has wrapped; close the old line, start a new one
                if (((xOffset + textSize.x) + textSize.x) >= (m_textRegion->rect.Width()))
                {
                    // Remember the offset beyond the end of the line
                    lineInfo->lineByteRange.second = ch;
                    lineInfo->textSizePx.x = xOffset;
                    m_windowLines.push_back(lineInfo);

                    // Next line
                    lineInfo = new SpanInfo();
                    spanLine++;
                    bufferPosYPx += fullLineHeight;

                    // Reset the line margin and height, because when we split a line we don't include a
                    // custom widget space above it.  That goes just above the first part of the line
                    padding.x = (float)GetEditor().GetConfig().lineMargins.x;
                    fullLineHeight = textHeight + padding.x + padding.y;

                    // Now jump to the next 'screen line' for the rest of this 'buffer line'
                    lineInfo->lineByteRange = BufferByteRange(ch, utf8_codepoint_length(textBuffer[ch]));
                    lineInfo->spanLineIndex = spanLine;
                    lineInfo->bufferLineNumber = bufferLine;
                    lineInfo->yOffsetPx = bufferPosYPx;
                    lineInfo->padding = padding;
                    lineInfo->textSizePx.y = textHeight;
                    lineInfo->textSizePx.x = xOffset;
                    lineInfo->isSplitContinuation = true;

                    xOffset = m_xPad;
                }
                else
                {
                    xOffset += textSize.x + m_xPad;
                }
            }
            else
            {
                xOffset += textSize.x + m_xPad;
            }

            if (*pCh == '\n' && !ZTestFlags(GetWindowFlags(), WindowFlags::ShowCR))
            {
                xOffset -= (textSize.x + m_xPad);
            }

            if (*pCh == 0)
            {
                xOffset -= (textSize.x + m_xPad);
            }

            lineInfo->yOffsetPx = bufferPosYPx;
            lineInfo->lineByteRange.second = ch + utf8_codepoint_length(textBuffer[ch]);
            lineInfo->textSizePx.x = std::max(lineInfo->textSizePx.x, xOffset);
        }

        // Complete the line
        m_windowLines.push_back(lineInfo);

        // Next time round - down a buffer line, down a span line
        bufferLine++;
        spanLine++;
        xOffset = m_xPad;
        bufferPosYPx += fullLineHeight;
    }

    // Sanity
    if (m_windowLines.empty())
    {
        SpanInfo* lineInfo = new SpanInfo();
        lineInfo->lineByteRange.first = 0;
        lineInfo->lineByteRange.second = 0;
        lineInfo->padding = NVec2f(0.0f);
        lineInfo->textSizePx = NVec2f(0.0f);
        lineInfo->bufferLineNumber = 0;
        m_windowLines.push_back(lineInfo);
    }

    // Now build the codepoint offsets
    for (auto& line : m_windowLines)
    {
        auto ch = line->lineByteRange.first;

        // TODO: Optimize
        line->lineCodePoints.clear();
        uint32_t points = 0;
        while (ch < line->lineByteRange.second)
        {
            LineCharInfo info;

            // Important note: We can't navigate the text buffer by pointers!
            // The gap buffer will get in the way; so need to be careful to use [] or an iterator
            // GetCharSize is cached for speed on debug builds.
            info.byteIndex = ByteIndex(ch);
            info.size = display.GetCharSize(&textBuffer[ch]);
            line->lineCodePoints.push_back(info);
            ch += utf8_codepoint_length(textBuffer[ch]);
            points++;
        }
    }

    UpdateVisibleLineRange();
    m_layoutDirty = true;
}

void ZepWindow::UpdateVisibleLineRange()
{
    TIME_SCOPE(UpdateVisibleLineRange);

    m_visibleLineIndices.x = (long)m_windowLines.size();
    m_visibleLineIndices.y = 0;
    m_textSizePx.x = 0;
    for (long line = 0; line < long(m_windowLines.size()); line++)
    {
        auto& windowLine = *m_windowLines[line];
        m_textSizePx.x = std::max(m_textSizePx.x, windowLine.textSizePx.x);

        if ((windowLine.yOffsetPx + windowLine.FullLineHeightPx()) <= m_textOffsetPx)
        {
            continue;
        }

        if ((windowLine.yOffsetPx - m_textOffsetPx) >= m_textRegion->rect.Height())
        {
            break;
        }

        m_visibleLineIndices.x = std::min(m_visibleLineIndices.x, long(line));
        m_visibleLineIndices.y = long(line);
    }

    m_textSizePx.y = m_windowLines[m_windowLines.size() - 1]->yOffsetPx + GetEditor().GetDisplay().GetFontHeightPixels() + DPI_Y(GetEditor().GetConfig().lineMargins.y) + DPI_Y(GetEditor().GetConfig().lineMargins.x);

    //LOG(DEBUG) << "Text Size: " << m_textSizePx;

    m_visibleLineIndices.y++;
    UpdateScrollers();
}

const SpanInfo& ZepWindow::GetCursorLineInfo(long y)
{
    UpdateLayout();
    y = std::max(0l, y);
    y = std::min(y, long(m_windowLines.size() - 1));
    return *m_windowLines[y];
}

// Convert a normalized y coordinate to the window region
float ZepWindow::ToWindowY(float pos) const
{
    return pos - m_textOffsetPx + m_textRegion->rect.topLeftPx.y;
}

float ZepWindow::TipBoxShadowWidth() const
{
    return DPI_X(4.0f);
}

void ZepWindow::DisplayToolTip(const NVec2f& pos, const RangeMarker& marker) const
{
    auto textSize = GetEditor().GetDisplay().GetTextSize((const uint8_t*)marker.description.c_str(), (const uint8_t*)(marker.description.c_str() + marker.description.size()));

    auto boxShadowWidth = TipBoxShadowWidth();

    // Draw a black area a little wider than the tip box.
    NRectf tipBox(pos.x, pos.y, textSize.x, textSize.y);
    tipBox.Adjust(0, 0, (textBorder + boxShadowWidth) * 2, (textBorder + boxShadowWidth) * 2);

    auto& display = GetEditor().GetDisplay();

    // Don't clip the scroll bar
    auto clip = m_expandingEditRegion->rect;
    clip.SetSize(clip.Size() - NVec2f(m_vScrollRegion->rect.Width(), 0.0f));
    display.SetClipRect(clip);
    display.DrawRectFilled(tipBox, m_pBuffer->GetTheme().GetColor(ThemeColor::Background));

    // Draw a lighter inner and a border the same color as the marker theme
    tipBox.Adjust(boxShadowWidth, boxShadowWidth, -boxShadowWidth, -boxShadowWidth);
    display.DrawRectFilled(tipBox, m_pBuffer->GetTheme().GetColor(marker.backgroundColor));
    display.DrawLine(tipBox.topLeftPx, tipBox.TopRight(), m_pBuffer->GetTheme().GetColor(marker.highlightColor));
    display.DrawLine(tipBox.BottomLeft(), tipBox.bottomRightPx, m_pBuffer->GetTheme().GetColor(marker.highlightColor));
    display.DrawLine(tipBox.topLeftPx, tipBox.BottomLeft(), m_pBuffer->GetTheme().GetColor(marker.highlightColor));
    display.DrawLine(tipBox.TopRight(), tipBox.bottomRightPx, m_pBuffer->GetTheme().GetColor(marker.highlightColor));

    // Draw the text in the box
    display.DrawChars(tipBox.topLeftPx + NVec2f(textBorder, textBorder), m_pBuffer->GetTheme().GetColor(marker.textColor), (const uint8_t*)marker.description.c_str());
}

NVec4f ZepWindow::GetBlendedColor(ThemeColor color) const
{
    auto col = m_pBuffer->GetTheme().GetColor(color);
    if (GetEditor().GetConfig().style == EditorStyle::Minimal)
    {
        float lastEdit = GetEditor().GetLastEditElapsedTime();
        if (lastEdit > GetEditor().GetConfig().backgroundFadeWait)
        {
            lastEdit -= GetEditor().GetConfig().backgroundFadeWait;
            col.w = std::max(0.0f, 1.0f - lastEdit / GetEditor().GetConfig().backgroundFadeTime);
        }
    }
    return col;
}

void ZepWindow::DrawLineWidgets(SpanInfo& lineInfo)
{
    if (lineInfo.isSplitContinuation)
        return;

    auto pLineWidgets = m_pBuffer->GetLineWidgets(lineInfo.bufferLineNumber);
    if (!pLineWidgets)
    {
        return;
    }

    auto lineMargins = DPI_VEC2(GetEditor().GetConfig().lineMargins);
    auto widgetMargins = DPI_VEC2(GetEditor().GetConfig().widgetMargins);

    NVec2f linePx = GetSpanPixelRange(lineInfo);
    float currentY = lineMargins.x;
    for (auto& pWidget : *pLineWidgets)
    {
        auto widgetSize = DPI_VEC2(pWidget->GetSize());

        currentY += widgetMargins.x;
        pWidget->Draw(*m_pBuffer, NVec2f(linePx.x, ToWindowY(currentY + lineInfo.yOffsetPx)));
        currentY += widgetMargins.y;
    }
}

// TODO: This function draws one char at a time.  It could be more optimal at the expense of some
// complexity.  Basically, I don't like the current implementation, but it works for now.
// The text is displayed acorrding to the region bounds and the display lineData
// Additionally (and perhaps that should be a seperate function), this code draws line numbers
bool ZepWindow::DisplayLine(SpanInfo& lineInfo, int displayPass)
{
    static const auto blankSpace = ' ';

    auto cursorCL = BufferToDisplay();
    auto& display = GetEditor().GetDisplay();
    display.SetClipRect(m_bufferRegion->rect);

    auto& buffer = GetBuffer();
    auto pMode = buffer.GetMode();
    if (!pMode)
    {
        return false;
    }

    auto cursorBlink = GetEditor().GetCursorBlinkState();
    auto cursorType = GetBuffer().GetMode()->GetCursorType();

    auto defaultCharSize = GetEditor().GetDisplay().GetDefaultCharSize();
    auto whiteSpaceCol = m_pBuffer->GetTheme().GetColor(ThemeColor::Whitespace);

    // Draw line numbers
    auto displayLineNumber = [&]() {
        if (!IsInsideTextRegion(NVec2i(0, lineInfo.spanLineIndex)))
            return;

        auto cursorBufferLine = GetCursorLineInfo(cursorCL.y).bufferLineNumber;
        std::string strNum;

        // In Vim mode show relative lines, unless in Ex mode (with hidden cursor)
        if (m_displayMode == DisplayMode::Vim && pMode->GetCursorType() != CursorType::None)
        {
            strNum = std::to_string(std::abs(lineInfo.bufferLineNumber - cursorBufferLine));
        }
        else
        {
            strNum = std::to_string(lineInfo.bufferLineNumber);
        }

        auto textSize = display.GetTextSize((const uint8_t*)strNum.c_str(), (const uint8_t*)(strNum.c_str() + strNum.size()));

        auto digitCol = m_pBuffer->GetTheme().GetColor(ThemeColor::LineNumber);
        if (lineInfo.BufferCursorInside(m_bufferCursor))
        {
            digitCol = m_pBuffer->GetTheme().GetColor(ThemeColor::CursorNormal);
        }

        // Numbers
        display.DrawChars(NVec2f(m_numberRegion->rect.bottomRightPx.x - textSize.x, ToWindowY(lineInfo.yOffsetPx + lineInfo.padding.x)), digitCol, (const uint8_t*)strNum.c_str(), (const uint8_t*)(strNum.c_str() + strNum.size()));
    };

    // Drawing commands for the whole line
    if (displayPass == WindowPass::Background)
    {
        display.SetClipRect(m_textRegion->rect);

        NVec2f linePx = GetSpanPixelRange(lineInfo);

        // Fill the background of the line
        // TODO: Adjust pixelRenderRange
        display.DrawRectFilled(
            NRectf(
                NVec2f(linePx.x, ToWindowY(lineInfo.yOffsetPx)),
                NVec2f(linePx.y, ToWindowY(lineInfo.yOffsetPx + lineInfo.FullLineHeightPx()))),
            GetBlendedColor(ThemeColor::Background));

        if (lineInfo.BufferCursorInside(m_bufferCursor))
        {
            if (IsActiveWindow())
            {
                // Don't draw over the visual region
                if (GetBuffer().GetMode()->GetEditorMode() != EditorMode::Visual)
                {
                    auto& cursorLine = GetCursorLineInfo(cursorCL.y);

                    if (IsInsideTextRegion(cursorCL))
                    {
                        float lineSize = 1.0f * GetEditor().GetPixelScale().y;

                        // Normal mode spans the whole buffer, otherwise we just cover the visible text range
                        // This is all about making minimal mode as non-invasive as possible.
                        auto right = GetEditor().GetConfig().style == EditorStyle::Normal ? m_textRegion->rect.bottomRightPx.x : m_textSizePx.y;

                        if (GetEditor().GetConfig().cursorLineSolid)
                        {
                            // Cursor line
                            display.DrawRectFilled(NRectf(NVec2f(m_textRegion->rect.topLeftPx.x, cursorLine.yOffsetPx - m_textOffsetPx + m_textRegion->rect.topLeftPx.y), NVec2f(right, cursorLine.yOffsetPx - m_textOffsetPx + m_textRegion->rect.topLeftPx.y + cursorLine.FullLineHeightPx())), GetBlendedColor(ThemeColor::CursorLineBackground));
                        }
                        else
                        {
                            // Cursor line
                            display.DrawRectFilled(
                                NRectf(
                                    NVec2f(m_textRegion->rect.Left(), ToWindowY(cursorLine.yOffsetPx)),
                                    NVec2f(right, ToWindowY(cursorLine.yOffsetPx + lineSize))),
                                GetBlendedColor(ThemeColor::TabInactive));

                            display.DrawRectFilled(
                                NRectf(
                                    NVec2f(m_textRegion->rect.Left(), ToWindowY(cursorLine.yOffsetPx + cursorLine.FullLineHeightPx() - lineSize)),
                                    NVec2f(right, ToWindowY(cursorLine.yOffsetPx + cursorLine.FullLineHeightPx()))),
                                GetBlendedColor(ThemeColor::TabInactive));

                            display.DrawRectFilled(
                                NRectf(
                                    NVec2f(right, ToWindowY(cursorLine.yOffsetPx)),
                                    NVec2f(right + lineSize, ToWindowY(cursorLine.yOffsetPx + cursorLine.FullLineHeightPx()))),
                                GetBlendedColor(ThemeColor::TabInactive));
                        }
                    }
                }
            }
        }
        display.SetClipRect(m_bufferRegion->rect);

        if (m_indicatorRegion->rect.Width() > 0)
        {
            display.SetClipRect(m_indicatorRegion->rect);

            // Show any markers in the left indicator region
            m_pBuffer->ForEachMarker(RangeMarkerType::Message, SearchDirection::Forward, lineInfo.lineByteRange.first, lineInfo.lineByteRange.second, [&](const std::shared_ptr<RangeMarker>& marker) {
                // >|< Text.  This is the bit between the arrows <-.  A vertical bar in the 'margin'
                if (marker->displayType & RangeMarkerDisplayType::Indicator)
                {
                    if (marker->IntersectsRange(lineInfo.lineByteRange))
                    {
                        display.DrawRectFilled(
                            NRectf(
                                NVec2f(
                                    m_indicatorRegion->rect.Center().x - m_indicatorRegion->rect.Width() / 4,
                                    ToWindowY(lineInfo.yOffsetPx + lineInfo.padding.x)),
                                NVec2f(
                                    m_indicatorRegion->rect.Center().x + m_indicatorRegion->rect.Width() / 4,
                                    ToWindowY(lineInfo.yOffsetPx + lineInfo.padding.x) + display.GetFontHeightPixels())),
                            m_pBuffer->GetTheme().GetColor(marker->highlightColor));
                    }
                }
                return true;
            });

            display.SetClipRect(m_bufferRegion->rect);
        }

        if (m_numberRegion->rect.Width() > 0)
        {
            display.SetClipRect(m_numberRegion->rect);
            displayLineNumber();
            display.SetClipRect(m_bufferRegion->rect);
        }
    }

    auto screenPosX = m_textRegion->rect.Left() + m_xPad;
    auto pSyntax = m_pBuffer->GetSyntax();

    if (pSyntax)
    {
        pSyntax->SetCurrentCursor(GetBufferCursor());
    }

    auto tipTimeSeconds = timer_get_elapsed_seconds(m_toolTipTimer);

    display.SetClipRect(m_textRegion->rect);

    //auto pText = &m_pBuffer->GetText()[0];
    // Walk from the start of the line to the end of the line (in buffer chars)
    for (auto cp : lineInfo.lineCodePoints)
    {
        const uint8_t* pCh;
        const uint8_t* pEnd;
        SpecialChar special;
        GetCharPointer(cp.byteIndex, pCh, pEnd, special);

        // TODO : Cache this for speed - a little sluggish on debug builds.
        if (displayPass == WindowPass::Background)
        {
            NRectf charRect(NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx)), NVec2f(screenPosX + cp.size.x, ToWindowY(lineInfo.yOffsetPx + lineInfo.FullLineHeightPx())));
            if (charRect.Contains(m_mouseHoverPos))
            {
                // Record the mouse-over buffer location
                m_mouseBufferLocation = cp.byteIndex;
            }

            // If the syntax overrides the background, show it first
            if (pSyntax)
            {
                auto syntaxResult = pSyntax->GetSyntaxAt(cp.byteIndex);
                if (syntaxResult.background != ThemeColor::None)
                {
                    auto themeCol = pSyntax->ToBackgroundColor(syntaxResult);
                    display.DrawRectFilled(charRect, themeCol);
                }
            }

            // Show any markers
            m_pBuffer->ForEachMarker(RangeMarkerType::All, SearchDirection::Forward, lineInfo.lineByteRange.first, lineInfo.lineByteRange.second, [&](const std::shared_ptr<RangeMarker>& marker) {
                // Don't show hidden markers
                if (marker->displayType == RangeMarkerDisplayType::Hidden)
                {
                    return true;
                }

                auto sel = marker->range;
                if (marker->ContainsLocation(cp.byteIndex))
                {
                    if (marker->displayType & RangeMarkerDisplayType::Underline)
                    {
                        display.DrawRectFilled(NRectf(NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx + lineInfo.FullLineHeightPx()) - 1), NVec2f(screenPosX + cp.size.x, ToWindowY(lineInfo.yOffsetPx + lineInfo.FullLineHeightPx()))), m_pBuffer->GetTheme().GetColor(marker->highlightColor));
                    }

                    if (marker->displayType & RangeMarkerDisplayType::Background)
                    {
                        display.DrawRectFilled(charRect, m_pBuffer->GetTheme().GetColor(marker->backgroundColor));
                    }

                    // If this marker has an associated tooltip, pop it up after a time delay
                    // TODO: Make tooltip generation seperate to this display loop
                    if (m_toolTips.empty() && !m_tipDisabledTillMove && (tipTimeSeconds > 0.5f))
                    {
                        bool showTip = false;
                        if (marker->displayType & RangeMarkerDisplayType::Tooltip)
                        {
                            if (m_mouseBufferLocation == cp.byteIndex)
                            {
                                showTip = true;
                            }
                        }

                        // If we want the tip showing at anywhere on the line, show it
                        if (marker->displayType & RangeMarkerDisplayType::TooltipAtLine)
                        {
                            // TODO: This should be a helper function
                            // Checks for mouse pos inside a line string
                            if (m_mouseHoverPos.y >= ToWindowY(lineInfo.yOffsetPx) && m_mouseHoverPos.y < (ToWindowY(lineInfo.yOffsetPx) + cp.size.y) && (m_mouseHoverPos.x < m_textRegion->rect.topLeftPx.x + lineInfo.ByteLength() * cp.size.x))
                            {
                                showTip = true;
                            }
                        }

                        if (showTip)
                        {
                            // Register this tooltip
                            m_toolTips[NVec2f(m_mouseHoverPos.x, m_mouseHoverPos.y + textBorder)] = marker;
                        }
                    }
                }
                return true;
            });

            // Draw the visual selection marker second
            if (IsActiveWindow())
            {
                if (GetBuffer().HasSelection())
                {
                    auto sel = m_pBuffer->GetSelection();
                    if (sel.ContainsLocation(cp.byteIndex))
                    {
                        display.DrawRectFilled(NRectf(NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx)), NVec2f(screenPosX + cp.size.x, ToWindowY(lineInfo.yOffsetPx + lineInfo.FullLineHeightPx()))), m_pBuffer->GetTheme().GetColor(ThemeColor::VisualSelectBackground));
                    }
                }
            }
           
            // If active window and this is the cursor char then display the marker as a priority over what we would have shown
            if (IsActiveWindow() &&
                (cp.byteIndex == m_bufferCursor) && (!cursorBlink || cursorType == CursorType::LineMarker))
            {
                auto height = lineInfo.FullLineHeightPx();
                switch (cursorType)
                {
                default:
                case CursorType::None:
                    break;

                case CursorType::LineMarker: {
                    display.SetClipRect(NRectf());
                    auto posX = m_indicatorRegion->rect.Right() - DPI_X(2.0f);
                    GetEditor().GetDisplay().DrawRectFilled(NRectf(
                        NVec2f(posX, ToWindowY(lineInfo.yOffsetPx)),
                        NVec2f(posX + DPI_X(2.0f), ToWindowY(lineInfo.yOffsetPx + height))),
                        m_pBuffer->GetTheme().GetColor(ThemeColor::CursorNormal));
                    display.SetClipRect(m_textRegion->rect);
                }
                break;

                case CursorType::Insert: {
                    GetEditor().GetDisplay().DrawRectFilled(NRectf(
                        NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx)),
                        NVec2f(screenPosX + DPI_X(1.0f), ToWindowY(lineInfo.yOffsetPx + height))),
                        m_pBuffer->GetTheme().GetColor(ThemeColor::CursorInsert));
                }
                break;

                case CursorType::Normal:
                case CursorType::Visual: {
                    GetEditor().GetDisplay().DrawRectFilled(NRectf(
                        NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx)),
                        NVec2f(screenPosX + cp.size.x, ToWindowY(lineInfo.yOffsetPx + height))),
                        m_pBuffer->GetTheme().GetColor(ThemeColor::CursorNormal));
                }
                break;
                }
            }
        }
        // Second pass, characters
        else
        {
            DrawLineWidgets(lineInfo);

            if ((special != SpecialChar::Hidden) || GetWindowFlags() & WindowFlags::ShowCR)
            {
                auto centerY = ToWindowY(lineInfo.yOffsetPx) + cp.size.y / 2;
                auto centerChar = NVec2f(screenPosX + cp.size.x / 2, centerY);
                NVec4f col;
                if (special == SpecialChar::Hidden)
                {
                    col = m_pBuffer->GetTheme().GetColor(ThemeColor::HiddenText);
                }
                else
                {
                    if (pSyntax)
                    {
                        auto syntaxResult = pSyntax->GetSyntaxAt(cp.byteIndex);
                        if (syntaxResult.foreground != ThemeColor::None)
                        {
                            col = pSyntax->ToForegroundColor(syntaxResult);
                        }
                        else
                        {
                            col = m_pBuffer->GetTheme().GetColor(ThemeColor::Text);
                        }
                    }
                    else
                    {
                        col = m_pBuffer->GetTheme().GetColor(ThemeColor::Text);
                    }
                }
          
                // If this is the cursor char we override the colors
                auto ws = whiteSpaceCol;
                if (IsActiveWindow() && 
                    (cp.byteIndex == m_bufferCursor) &&
                    !cursorBlink &&
                    cursorType == CursorType::Normal)
                {
                    col = m_pBuffer->GetTheme().GetComplement(m_pBuffer->GetTheme().GetColor(ThemeColor::CursorNormal));
                    ws = col;
                }

                if (special == SpecialChar::None)
                {
                    display.DrawChars(NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx + lineInfo.padding.x)), col, pCh, pEnd);
                }
                else if (special == SpecialChar::Tab)
                {
                    if (GetWindowFlags() & WindowFlags::ShowWhiteSpace)
                    {
                        // A line and an arrow
                        display.DrawLine(NVec2f(screenPosX + defaultCharSize.x / 2, centerY), NVec2f(screenPosX + cp.size.x - defaultCharSize.x / 4, centerY), ws, 2);
                        display.DrawLine(NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx)), NVec2f(screenPosX + defaultCharSize.x / 2, centerY), ws, 2);
                        display.DrawLine(NVec2f(screenPosX, ToWindowY(lineInfo.yOffsetPx + cp.size.y)), NVec2f(screenPosX + defaultCharSize.x / 2, centerY), ws, 2);
                    }
                }
                else if (special == SpecialChar::Space)
                {
                    if (GetWindowFlags() & WindowFlags::ShowWhiteSpace)
                    {
                        // A dot
                        display.DrawRectFilled(NRectf(centerChar - DPI_VEC2(NVec2f(1.0f, 1.0f)), centerChar + DPI_VEC2(NVec2f(1.0f, 1.0f))), ws);
                    }
                }
            }
        }

        screenPosX += cp.size.x + m_xPad;
    }

    display.SetClipRect(NRectf{});

    return true;
}

bool ZepWindow::IsInsideTextRegion(NVec2i pos) const
{
    if (pos.y < m_visibleLineIndices.x || pos.y >= m_visibleLineIndices.y)
    {
        return false;
    }
    return true;
}

ZepTabWindow& ZepWindow::GetTabWindow() const
{
    return m_tabWindow;
}

void ZepWindow::SetWindowFlags(uint32_t windowFlags)
{
    if (windowFlags != m_windowFlags)
    {
        m_windowFlags = windowFlags;

        m_layoutDirty = true;
    }
}

uint32_t ZepWindow::GetWindowFlags() const
{
    auto flags = m_windowFlags;

    if (m_pBuffer && m_pBuffer->GetMode())
    {
        flags = m_pBuffer->GetMode()->ModifyWindowFlags(flags);
    }

    return flags;
}

void ZepWindow::ToggleFlag(uint32_t flag)
{
    if (ZTestFlags(m_windowFlags, flag))
    {
        SetWindowFlags(m_windowFlags & ~flag);
    }
    else
    {
        SetWindowFlags(m_windowFlags | flag);
    }
}

long ZepWindow::GetMaxDisplayLines()
{
    UpdateLayout();
    return m_maxDisplayLines;
}

long ZepWindow::GetNumDisplayedLines()
{
    UpdateLayout();
    return std::min((long)m_windowLines.size(), GetMaxDisplayLines());
}

void ZepWindow::SetBufferCursor(ByteIndex location)
{
    // Don't move cursor if not necessary
    // This helps preserve 'lastCursorColumn' from being changed all the time
    // during line clamps, etc.
    if (location != m_bufferCursor)
    {
        m_bufferCursor = m_pBuffer->Clamp(location);
        m_lastCursorColumn = BufferToDisplay(m_bufferCursor).x;
        m_cursorMoved = true;
        DisableToolTipTillMove();
    }
}

void ZepWindow::DisableToolTipTillMove()
{
    m_tipDisabledTillMove = true;
    m_toolTips.clear();
}

void ZepWindow::SetBuffer(ZepBuffer* pBuffer)
{
    assert(pBuffer);

    m_pBuffer = pBuffer;
    m_layoutDirty = true;
    m_textOffsetPx = 0;
    m_bufferCursor = pBuffer->Clamp(pBuffer->GetLastEditLocation());
    m_lastCursorColumn = 0;
    m_cursorMoved = false;
    if (pBuffer->GetMode())
    {
        pBuffer->GetMode()->Begin(this);
    }
    GetEditor().UpdateTabs();
}

ByteIndex ZepWindow::GetBufferCursor()
{
    // Ensure cursor is always valid inside the buffer
    m_bufferCursor = m_pBuffer->Clamp(m_bufferCursor);

    return m_bufferCursor;
}

ZepBuffer& ZepWindow::GetBuffer() const
{
    return *m_pBuffer;
}

bool ZepWindow::IsActiveWindow() const
{
    return m_tabWindow.GetActiveWindow() == this;
}

NVec4f ZepWindow::FilterActiveColor(const NVec4f& col, float atten)
{
    if (!IsActiveWindow())
    {
        return NVec4f(Luminosity(col) * atten);
    }
    return col;
}

void ZepWindow::DisplayScrollers()
{
    if (m_vScrollRegion->rect.Empty())
        return;

    m_vScroller->Display(m_pBuffer->GetTheme());

    GetEditor().GetDisplay().SetClipRect(m_bufferRegion->rect);
}

void ZepWindow::UpdateLayout(bool force)
{
    if (m_layoutDirty || force)
    {
        // Border, and move the text across a bit
        if (ZTestFlags(GetWindowFlags(), WindowFlags::ShowLineNumbers) && GetEditor().GetConfig().showLineNumbers)
        {
            m_numberRegion->fixed_size = NVec2f(float(leftBorderChars) * GetEditor().GetDisplay().GetDefaultCharSize().x, 0.0f);
        }
        else
        {
            m_numberRegion->fixed_size = NVec2f(0.0f);
        }

        if (ZTestFlags(GetWindowFlags(), WindowFlags::ShowIndicators) && GetEditor().GetConfig().showIndicatorRegion)
        {
            m_indicatorRegion->fixed_size = NVec2f(GetEditor().GetDisplay().GetDefaultCharSize().x * 1.5f, 0.0f);
        }
        else
        {
            m_indicatorRegion->fixed_size = NVec2f(0.0f);
        }

        // When wrapping text, we fit the text to the available window space
        if (ZTestFlags(GetWindowFlags(), WindowFlags::WrapText))
        {
            m_editRegion->flags = RegionFlags::Expanding;
            m_editRegion->fixed_size = NVec2f(0.0f);

            // First layout
            LayoutRegion(*m_bufferRegion);

            // Then update the text alignment
            UpdateLineSpans();
        }
        else
        {
            // First update the text, since it is always the same size without wrapping
            UpdateLineSpans();

            // Fix the edit region size at the text size
            m_editRegion->flags = RegionFlags::AlignCenter;

            // Take into account the extra regions to the sides with padding
            m_editRegion->fixed_size = m_textSizePx;
            m_editRegion->fixed_size += m_numberRegion->fixed_size;
            m_editRegion->fixed_size += m_indicatorRegion->fixed_size;

            m_editRegion->fixed_size.x += m_textRegion->padding.x + m_textRegion->padding.y;
            m_editRegion->fixed_size.x += m_numberRegion->padding.x + m_numberRegion->padding.y;
            m_editRegion->fixed_size.x += m_indicatorRegion->padding.x + m_indicatorRegion->padding.y;

            LayoutRegion(*m_bufferRegion);

            // Finally, we have to update the line visibility again because the layout has changed!
            UpdateVisibleLineRange();
        }

        m_layoutDirty = false;
    }
}

NVec2f ZepWindow::GetSpanPixelRange(SpanInfo& span) const
{
    // Need to take account of the text rect offset
    return NVec2f(m_textRegion->rect.Left(),
        span.textSizePx.x + m_textRegion->rect.Left());
}

void ZepWindow::GetCursorInfo(NVec2f& pos, NVec2f& size)
{
    auto cursorCL = BufferToDisplay();
    auto cursorBufferLine = GetCursorLineInfo(cursorCL.y);

    NVec2f cursorSize;
    bool found = false;
    float xPos = m_textRegion->rect.topLeftPx.x + m_xPad;

    int count = 0;
    for (auto ch : cursorBufferLine.lineCodePoints)
    {
        if (count == cursorCL.x)
        {
            found = true;
            cursorSize = ch.size;
            break;
        }
        count++;
        xPos += ch.size.x + m_xPad;
    }

    // If it's a tab, we show a cursor of standard width at the beginning of it
    if (GetBuffer().GetText()[GetBufferCursor()] == '\t')
    {
        cursorSize = GetEditor().GetDisplay().GetDefaultCharSize();
    }
    else if (!found)
    {
        cursorSize = GetEditor().GetDisplay().GetDefaultCharSize();
        xPos += cursorSize.x;
    }

    pos = NVec2f(xPos, cursorBufferLine.yOffsetPx + cursorBufferLine.padding.x - m_textOffsetPx + m_textRegion->rect.topLeftPx.y);
    size = cursorSize;
    size.y = cursorBufferLine.textSizePx.y;
}

void ZepWindow::PlaceToolTip(const NVec2f& pos, ToolTipPos location, uint32_t lineGap, const std::shared_ptr<RangeMarker> spMarker)
{
    auto textSize = GetEditor().GetDisplay().GetTextSize((const uint8_t*)spMarker->description.c_str(), (const uint8_t*)(spMarker->description.c_str() + spMarker->description.size()));
    float boxShadowWidth = TipBoxShadowWidth();

    NRectf tipBox;
    float currentLineGap = lineGap + .5f;

    for (int i = 0; i < int(ToolTipPos::Count); i++)
    {
        auto genBox = [&]() {
            // Draw a black area a little wider than the tip box.
            tipBox = NRectf(pos.x, pos.y, textSize.x, textSize.y);
            tipBox.Adjust(textBorder + boxShadowWidth, textBorder + boxShadowWidth, textBorder + boxShadowWidth, textBorder + boxShadowWidth);

            float dist = currentLineGap * (GetEditor().GetDisplay().GetFontHeightPixels() + textBorder * 2);
            if (location == ToolTipPos::AboveLine)
            {
                dist += textSize.y;
                tipBox.Adjust(0.0f, -dist);
            }
            else if (location == ToolTipPos::BelowLine)
            {
                tipBox.Adjust(0.0f, dist);
            }
        };

        genBox();
        if (!NRectFits(m_textRegion->rect, tipBox, FitCriteria::X))
        {
            // If it is above or below, slide it to the left to fit
            if (location != ToolTipPos::RightLine)
            {
                // Move in X along to the left
                genBox();
                tipBox.Move(std::max(m_textRegion->rect.Left() + textBorder, (m_textRegion->rect.Right() - (tipBox.Width() + textBorder))), tipBox.Top());
            }
        }

        // Swap above below
        if (!NRectFits(m_textRegion->rect, tipBox, FitCriteria::Y))
        {
            switch (location)
            {
            case ToolTipPos::AboveLine:
                location = ToolTipPos::BelowLine;
                break;
            case ToolTipPos::BelowLine:
                location = ToolTipPos::AboveLine;
                break;
            case ToolTipPos::RightLine:
                location = ToolTipPos::AboveLine;
                break;
            default:
                break;
            }
        }
        else
        {
            break;
        }
    }

    m_toolTips[tipBox.topLeftPx] = spMarker;
}

void ZepWindow::DisplayGridMarkers()
{
    auto& display = GetEditor().GetDisplay();

    auto rc = m_textRegion->rect;
    rc.Adjust(-1, -1, 1, 1);

    // Border around the edge
    display.DrawRect(rc, GetBlendedColor(ThemeColor::TabActive));

    /*
    for (long windowLine = m_visibleLineIndices.x; windowLine < m_visibleLineIndices.y; windowLine++)
    {
        auto& lineInfo = *m_windowLines[windowLine];
        auto pos = m_textRegion->rect.topLeftPx + NVec2f(m_xPad, 0.0f);
        for (int i = 0; i < lineInfo.lineCodePoints.size(); i++)
        {
            auto cp = lineInfo.lineCodePoints[i];

            if (i != 0 && i % 8 == 0)
            {
                display.DrawLine(NVec2f(pos.x - m_xPad / 2, pos.y), NVec2f(pos.x - m_xPad / 2, pos.y + DPI_Y(3.0f)), NVec4f(1.0f));
            }
            pos.x += cp.size.x + m_xPad;
        }
    }
    */
}

void ZepWindow::Display()
{
    TIME_SCOPE(Display);

    auto pMode = GetBuffer().GetMode();
    pMode->PreDisplay(*this);

    // Ensure line spans are valid; updated if the text is changed or the window dimensions change
    UpdateLayout();
    ScrollToCursor();
    UpdateScrollers();

    // Second pass if the scroller visibility changed, since this can change the whole layout!
    if (m_scrollVisibilityChanged)
    {
        m_layoutDirty = true;
        m_cursorMoved = true;
        UpdateLayout();
        ScrollToCursor();
        UpdateScrollers();

        m_scrollVisibilityChanged = false;
    }

    auto& display = GetEditor().GetDisplay();
    auto cursorCL = BufferToDisplay(m_bufferCursor);
    m_mouseBufferLocation = ByteIndex{ -1 };

    // Always update
    UpdateAirline();

    UpdateLayout();

    if (GetEditor().GetConfig().style == EditorStyle::Normal)
    {
        // Fill the background color for the whole area, only in normal mode.
        display.DrawRectFilled(m_textRegion->rect, GetBlendedColor(ThemeColor::Background));
    }

    if (m_numberRegion->rect.Width() > 0)
    {
        display.DrawRectFilled(m_numberRegion->rect, GetBlendedColor(ThemeColor::LineNumberBackground));
    }

    if (m_indicatorRegion->rect.Width() > 0)
    {
        display.DrawRectFilled(m_indicatorRegion->rect, GetBlendedColor(ThemeColor::LineNumberBackground));
    }

    DisplayScrollers();

    // This is a line down the middle of a split
    if (GetEditor().GetConfig().style == EditorStyle::Normal && !ZTestFlags(GetWindowFlags(), WindowFlags::HideSplitMark))
    {
        display.DrawRectFilled(
            NRectf(NVec2f(m_expandingEditRegion->rect.topLeftPx.x, m_expandingEditRegion->rect.topLeftPx.y), NVec2f(m_expandingEditRegion->rect.topLeftPx.x + 1, m_expandingEditRegion->rect.bottomRightPx.y)), GetBlendedColor(ThemeColor::TabInactive));
    }

    {
        TIME_SCOPE(DrawLine);
        for (int displayPass = 0; displayPass < WindowPass::Max; displayPass++)
        {
            for (long windowLine = m_visibleLineIndices.x; windowLine < m_visibleLineIndices.y; windowLine++)
            {
                auto& lineInfo = *m_windowLines[windowLine];
                if (!DisplayLine(lineInfo, displayPass))
                {
                    break;
                }
            }
        }
    }

    if (ZTestFlags(GetWindowFlags(), WindowFlags::GridStyle))
    {
        DisplayGridMarkers();
    }

    // Is the cursor on a tooltip row or mark?
    if (m_toolTips.empty())
    {
        auto cursorLine = GetCursorLineInfo(BufferToDisplay().y);

        // If this marker has an associated tooltip, pop it up after a time delay
        NVec2f pos, size;
        GetCursorInfo(pos, size);

        // Calculate our desired location for the tip.
        auto tipPos = [&](RangeMarker& marker) {
            NVec2f ret;
            NVec2f linePx = GetSpanPixelRange(cursorLine);
            if (marker.tipPos == ToolTipPos::RightLine)
            {
                ret = NVec2f(linePx.y, pos.y);
            }
            else
            {
                ret = NVec2f(linePx.x, pos.y);
            }
            return ret;
        };

        m_pBuffer->ForEachMarker(RangeMarkerType::All, SearchDirection::Forward, cursorLine.lineByteRange.first, cursorLine.lineByteRange.second, [&](const std::shared_ptr<RangeMarker>& marker) {
            if (marker->displayType == RangeMarkerDisplayType::Hidden)
            {
                return true;
            }

            auto sel = marker->range;
            if (marker->displayType & RangeMarkerDisplayType::CursorTip)
            {
                if (m_bufferCursor >= sel.first && m_bufferCursor < sel.second)
                {
                    PlaceToolTip(tipPos(*marker), marker->tipPos, 2, marker);
                }
            }

            if (marker->displayType & RangeMarkerDisplayType::CursorTipAtLine)
            {
                if ((cursorLine.lineByteRange.first <= sel.first && cursorLine.lineByteRange.second > sel.first) || (cursorLine.lineByteRange.first <= sel.second && cursorLine.lineByteRange.second > sel.second))
                {
                    PlaceToolTip(tipPos(*marker), marker->tipPos, 2, marker);
                }
            }
            return true;
        });
    }
    else
    {
        // No hanging tooltips if the markers on the page have gone
        if (m_pBuffer->GetRangeMarkers(RangeMarkerType::Message).empty())
        {
            m_toolTips.clear();
        }
    }

    // No tooltip, and we can show one, then ask for tooltips
    if (!m_tipDisabledTillMove && (timer_get_elapsed_seconds(m_toolTipTimer) > 0.5f) && m_toolTips.empty() && m_lastTipQueryPos != m_mouseHoverPos)
    {
        auto spMsg = std::make_shared<ToolTipMessage>(m_pBuffer, m_mouseHoverPos, m_mouseBufferLocation);
        GetEditor().Broadcast(spMsg);
        if (spMsg->handled && spMsg->spMarker != nullptr)
        {
            PlaceToolTip(NVec2f(m_mouseHoverPos.x, m_mouseHoverPos.y), spMsg->spMarker->tipPos, 1, spMsg->spMarker);
        }
        m_lastTipQueryPos = m_mouseHoverPos;
    }

    for (auto& toolTip : m_toolTips)
    {
        DisplayToolTip(toolTip.first, *toolTip.second);
    }

    display.SetClipRect(NRectf{});

    if (!GetEditor().GetCommandText().empty() || (GetEditor().GetConfig().autoHideCommandRegion == false))
    {
        auto modeAirlines = GetBuffer().GetMode()->GetAirlines(*this);

        // Airline and underline
        display.DrawRectFilled(m_airlineRegion->rect, GetBlendedColor(ThemeColor::AirlineBackground));

        auto airHeight = GetEditor().GetDisplay().GetFontHeightPixels();
        auto border = 12.0f;

        NVec2f screenPosYPx = m_airlineRegion->rect.topLeftPx;

        auto drawAirline = [&](Airline& airline) {
            display.SetClipRect(NRectf{});
            for (int i = 0; i < (int)airline.leftBoxes.size(); i++)
            {
                auto pText = (const uint8_t*)airline.leftBoxes[i].text.c_str();
                auto textSize = display.GetTextSize(pText, pText + airline.leftBoxes[i].text.size());
                textSize.x += border * 2;

                auto col = airline.leftBoxes[i].background;
                display.DrawRectFilled(NRectf(screenPosYPx, NVec2f(textSize.x + screenPosYPx.x, screenPosYPx.y + airHeight)), col);

                NVec4f textCol = m_pBuffer->GetTheme().GetComplement(airline.leftBoxes[i].background, IsActiveWindow() ? NVec4f(0.0f) : NVec4f(.5f, .5f, .5f, 0.0f));
                display.DrawChars(screenPosYPx + NVec2f(border, 0.0f), textCol, (const uint8_t*)(airline.leftBoxes[i].text.c_str()));
                screenPosYPx.x += textSize.x;
            }

            // Clip to the remaining space
            auto clipRect = NRectf(screenPosYPx.x, screenPosYPx.y, m_airlineRegion->rect.Right() - screenPosYPx.x, airHeight);
            if (clipRect.Width() > 0 && clipRect.Height() > 0)
            {
                display.SetClipRect(clipRect);

                float totalRightSize = 0.0f;
                for (int i = 0; i < (int)airline.rightBoxes.size(); i++)
                {
                    auto pText = (const uint8_t*)airline.rightBoxes[i].text.c_str();
                    totalRightSize += display.GetTextSize(pText, pText + airline.rightBoxes[i].text.size()).x + border * 2;
                }

                screenPosYPx.x = m_airlineRegion->rect.Right() - totalRightSize;
                for (int i = 0; i < (int)airline.rightBoxes.size(); i++)
                {
                    auto pText = (const uint8_t*)airline.rightBoxes[i].text.c_str();
                    auto textSize = display.GetTextSize(pText, pText + airline.rightBoxes[i].text.size());
                    textSize.x += border * 2;

                    auto col = airline.rightBoxes[i].background;
                    display.DrawRectFilled(NRectf(screenPosYPx, NVec2f(textSize.x + screenPosYPx.x, screenPosYPx.y + airHeight)), col);

                    NVec4f textCol = m_pBuffer->GetTheme().GetComplement(airline.rightBoxes[i].background, IsActiveWindow() ? NVec4f(0.0f) : NVec4f(.5f, .5f, .5f, 0.0f));
                    display.DrawChars(screenPosYPx + NVec2f(border, 0.0f), textCol, (const uint8_t*)(airline.rightBoxes[i].text.c_str()));
                    screenPosYPx.x += textSize.x;
                }
            }
        };

        for (auto& line : modeAirlines)
        {
            drawAirline(line);
            screenPosYPx.y += airHeight;
            screenPosYPx.x = m_airlineRegion->rect.Left();
        }
        drawAirline(m_airline);
    }

    display.SetClipRect(NRectf{});
}

void ZepWindow::MoveCursorY(int yDistance, LineLocation clampLocation)
{
    UpdateLayout();

    timer_restart(m_toolTipTimer);
    m_toolTips.clear();

    // Get the cursor
    auto cursorCL = BufferToDisplay();
    if (cursorCL.x == -1)
        return;

    // Find the screen line relative target
    auto target = cursorCL + NVec2i(0, yDistance);
    target.y = std::max(0l, target.y);
    target.y = std::min(target.y, long(m_windowLines.size() - 1));

    auto& line = *m_windowLines[target.y];

    // Snap to the new vertical column if necessary (see comment below)
    if (target.x < m_lastCursorColumn)
        target.x = m_lastCursorColumn;

    assert(!line.lineCodePoints.empty());

    // Move to the same codepoint offset on the line below
    target.x = std::min(target.x, long(line.lineCodePoints.size() - 1));
    target.x = std::max(target.x, long(0));

    GlyphIterator cursorItr(*m_pBuffer, line.lineCodePoints[target.x].byteIndex);

    // We can't call the buffer's LineLocation code, because when moving in span lines,
    // we are technically not moving in buffer lines; we are stepping in wrapped buffer lines.
    switch (clampLocation)
    {
    default:
    case LineLocation::LineBegin:
    case LineLocation::LineFirstGraphChar:
    case LineLocation::BeyondLineEnd:
        assert(!"Not supported Y motion line clamp!");
        break;
    case LineLocation::LineLastNonCR: {
        // Don't skip back if we are right at the start of the line
        // (i.e. an empty line)
        if (target.x != 0 && (cursorItr.Char() == '\n' || cursorItr.Char() == 0))
        {
            cursorItr.MoveClamped(-1, LineLocation::LineLastNonCR);
        }
    }
    break;
    case LineLocation::LineCRBegin:
        // We already clamped to here above by testing for max codepoint
        // Last codepoint is the carriage return
        break;
    }

    m_bufferCursor = cursorItr.ToByteIndex();
    m_cursorMoved = true;

    GetEditor().ResetCursorTimer();

    m_pBuffer->SetLastEditLocation(m_bufferCursor);
}

NVec2i ZepWindow::BufferToDisplay()
{
    return BufferToDisplay(m_bufferCursor);
}

NVec2i ZepWindow::BufferToDisplay(const ByteIndex& loc)
{
    UpdateLayout();

    NVec2i ret(0, 0);
    int line_number = 0;

    // TODO: Performance; quick lookup for line
    for (auto& line : m_windowLines)
    {
        // If inside the line...
        if (line->lineByteRange.first <= loc && line->lineByteRange.second > loc)
        {
            ret.y = line_number;
            ret.x = 0;

            // Scan the code points for where we are
            for (auto& ch : line->lineCodePoints)
            {
                if (ch.byteIndex == loc)
                {
                    return ret;
                }
                ret.x++;
            }
        }
        line_number++;
    }

    assert(!m_windowLines.empty());
    if (m_windowLines.empty())
    {
        return NVec2i(0, 0);
    }

    // Max Last line, last code point offset
    ret.y = long(m_windowLines.size() - 1);
    ret.x = long(m_windowLines[m_windowLines.size() - 1]->lineCodePoints.size() - 1);
    return ret;
}

} // namespace Zep

#if 0
    // Ensure we can see the cursor
    NVec2i cursor(0, 0);
    cursor.x = m_pBuffer->GetBufferColumn(m_bufferCursor);
    cursor.y = m_pBuffer->GetBufferLine(m_bufferCursor) - m_pBuffer->GetBufferLine(m_dvisibleLineRange.x);

    // Handle the case where there is no need to scroll, since the visible lines are inside
    // The current screen rectangle.
    if (cursor.y >= m_nvisibleLineRange.y && m_linesFillScreen)
    {
        m_visibleLineRange.x = cursor.y - (m_nvisibleLineRange.y - m_vnisibleLineRange.x) + 1;
        m_linesChanged = true;
    }
    else if (cursor.y < m_nvisibleLineRange.x)
    {
        m_nvisibleLineRange.x = cursor.y;
        m_linesChanged = true;
    }

    // Clamp
    m_nvisibleLineRange.x = std::max(0l, (long)m_nvisibleLineRange.x);
#endif
