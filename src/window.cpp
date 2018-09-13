#include <sstream>
#include <cctype>
#include <cmath>

#include "display.h"
#include "tab_window.h"
#include "window.h"
#include "syntax.h"
#include "buffer.h"
#include "mode.h"
#include "theme.h"

#include "utils/stringutils.h"

// A 'window' is like a Vim Tab
namespace Zep
{

namespace
{
const uint32_t Color_CursorNormal = 0xEEF35FBC;
const uint32_t Color_CursorInsert = 0xFFFFFFFF;
}

ZepWindow::ZepWindow(ZepTabWindow& window, ZepBuffer& buffer, ZepDisplay& display)
    : ZepComponent(display.GetEditor()),
    m_display(display),
    m_window(window),
    m_buffer(buffer)
{
}

ZepWindow::~ZepWindow()
{
}

void ZepWindow::SetCursorMode(CursorMode mode)
{
    cursorMode = mode;
    m_display.ResetCursorTimer();
}

void ZepWindow::Notify(std::shared_ptr<ZepMessage> payload)
{
    if (payload->messageId == Msg_Buffer)
    {
        auto pMsg = std::static_pointer_cast<BufferMessage>(payload);

        if (pMsg->pBuffer != &m_buffer)
        {
            return;
        }

        if (pMsg->type != BufferMessageType::PreBufferChange)
        {
            // Put the cursor where the replaced text was added
            if (pMsg->type == BufferMessageType::TextDeleted ||
                pMsg->type == BufferMessageType::TextAdded ||
                pMsg->type == BufferMessageType::TextChanged)
            {
                PreDisplay(m_bufferRegion);

                if (pMsg->cursorAfter != -1)
                {
                    cursorCL = BufferToDisplay(pMsg->cursorAfter);
                }
                m_display.ResetCursorTimer();
            }
        }
    }
}

long ZepWindow::ClampVisibleLine(long line) const
{
    if (visibleLines.empty())
    {
        return 0;
    }

    line = std::min(line, long(visibleLines.size() - 1));
    line = std::max(line, 0l);
    return line;
}

// Clamp to a column on a single row of the display (will not jump lines, even if the line is the same and wrapped)
NVec2i ZepWindow::ClampVisibleColumn(NVec2i pos, LineLocation location) const
{
    if (visibleLines.size() <= pos.y)
    {
        return pos;
    }

    pos.x = std::max(0l, pos.x);
    switch (location)
    {
    case LineLocation::LineBegin:
        pos.x = std::min(pos.x, 0l);
        break;
    case LineLocation::LineCRBegin:
        pos.x = std::min(pos.x, std::max(0l, visibleLines[pos.y].lastNonCROffset + 1 - visibleLines[pos.y].columnOffsets.x));
        break;
    case LineLocation::LineEnd:
        pos.x = std::min(pos.x, std::max(0l, visibleLines[pos.y].Length() - 1));
        break;
    case LineLocation::LineFirstGraphChar:
        pos.x = std::min(pos.x, std::max(0l, visibleLines[pos.y].firstGraphCharOffset - visibleLines[pos.y].columnOffsets.x));
        break;
    case LineLocation::LineLastGraphChar:
        pos.x = std::min(pos.x, std::max(0l, visibleLines[pos.y].lastGraphCharOffset - visibleLines[pos.y].columnOffsets.x));
        break;
    case LineLocation::LineLastNonCR:
        pos.x = std::min(pos.x, std::max(0l, visibleLines[pos.y].lastNonCROffset - visibleLines[pos.y].columnOffsets.x));
        break;
    default:
        assert(!"Unhandled?");
        break;
    }

    return pos;
}

BufferLocation ZepWindow::DisplayToBuffer() const
{
    return DisplayToBuffer(cursorCL);
}

// Convert a display coordinate to a buffer coordinate
BufferLocation ZepWindow::DisplayToBuffer(const NVec2i& display) const
{
    BufferLocation loc{ 0l };

    if (visibleLines.size() <= display.y)
        return loc;

    //loc.line = visibleLines[display.y].lineNumber;
    loc = visibleLines[display.y].columnOffsets.x + display.x;

    return loc;
}

// TODO: this can be faster.
NVec2i ZepWindow::BufferToDisplay(const BufferLocation& loc) const
{
    NVec2i ret(0, 0);
    if (visibleLines.empty())
    {
        return ret;
    }

    auto location = m_buffer.Clamp(loc);

    auto line = m_buffer.LineFromOffset(location);

    ret.x = -1;
    ret.y = -1;
    for (auto& visLine : visibleLines)
    {
        if (visLine.lineNumber == line)
        {
            if (ret.y == -1)
            {
                // Line match... first found
                ret.y = visLine.screenLineNumber;
            }

            if (location >= visLine.columnOffsets.x && location < visLine.columnOffsets.y)
            {
                // Exact line/number match
                ret.y = visLine.screenLineNumber;
                ret.x = location - visLine.columnOffsets.x;
                break;
            }
        }
    }

    // Clamp it, since it is always minimum 0
    ret.y = std::max(0l, ret.y);
    ret.y = std::min(long(visibleLines[visibleLines.size() - 1].screenLineNumber), ret.y);

    ret.x = std::max(0l, ret.x);
    return ret;
}

void ZepWindow::MoveCursor(LineLocation location)
{
    auto buffer = DisplayToBuffer();
    auto line = cursorCL.y;
    switch (location)
    {
    case LineLocation::LineBegin:
        buffer = m_buffer.GetLinePos(line, location);
        break;
    case LineLocation::LineEnd:
        buffer = m_buffer.GetLinePos(line, location);
        break;
    case LineLocation::LineCRBegin:
        buffer = m_buffer.GetLinePos(line, location);
        break;
    case LineLocation::LineFirstGraphChar:
        buffer = m_buffer.GetLinePos(line, location);
        break;
    case LineLocation::LineLastGraphChar:
        buffer = m_buffer.GetLinePos(line, location);
        break;
    case LineLocation::LineLastNonCR:
        buffer = m_buffer.GetLinePos(line, location);
        break;
    }
    auto cursor = BufferToDisplay(buffer);
    return MoveCursor(cursor - cursorCL, LineLocation::LineCRBegin);
}

// NOTE: This currently moves clamped _inside_ the display region.
// TODO: Make this move within the rest of the whole buffer and ensure it doesn't break anything
void ZepWindow::MoveCursorTo(const BufferLocation& location, LineLocation clampLocation)
{
    MoveCursor(BufferToDisplay(location) - cursorCL, clampLocation);
}

// NOTE: In contrast to above, this will move any distance within the whole buffer, and scroll it appropriately
void ZepWindow::MoveCursor(const NVec2i& distance, LineLocation clampLocation)
{
    auto target = cursorCL + distance;

    // TODO: Add helpers for these conditions
    // TODO: This doesn't account for buffer lines that are multiple window lines: the result
    // is that a scroll through the document will 'jump' multiline buffer lines.  Need to figure out the 'sub-line' of a buffer
    // line and scroll to that.
    // Scroll the whole document up if we are near the top
    if (target.y < 4 && bufferCL.y > 0)
    {
        bufferCL.y += distance.y;
        bufferCL.y = std::max(0l, bufferCL.y);
        target.y = cursorCL.y;
    }
    // Scroll the whole document down if we are near the bottom
    else if ((target.y > long(visibleLines.size()) - 4) &&
        m_buffer.GetLineCount() > bufferCL.y + visibleLines.size())
    {
        bufferCL.y += distance.y;
        bufferCL.y = std::min(long(m_buffer.GetLineCount() - visibleLines.size()), bufferCL.y);
        bufferCL.y = std::max(0l, bufferCL.y);
        target.y = cursorCL.y;

    }
    target.y = ClampVisibleLine(target.y);

    // Snap to the new vertical column if necessary (see comment below)
    if (distance.x == 0)
    {
        if (target.x < lastCursorC)
            target.x = lastCursorC;
    }
    target = ClampVisibleColumn(target, clampLocation);

    // Reset last column, so this is our new vertical center.
    // All buffers let you start on a column, go to a shorter one, go to a longer one and 
    // wind up at the same place.  That's what I'm doing here.
    if (distance.x != 0)
    {
        lastCursorC = target.x;
    }

    cursorCL = target;
    m_display.ResetCursorTimer();
}

void ZepWindow::SetSelectionRange(const NVec2i& start, const NVec2i& end)
{
    selection.startCL = start;
    selection.endCL = end;
    selection.vertical = false;
    if (DisplayToBuffer(selection.startCL) > DisplayToBuffer(selection.endCL))
    {
        std::swap(selection.startCL, selection.endCL);
    }
}

void ZepWindow::SetStatusText(const std::string& strText)
{
    statusLines = StringUtils::SplitLines(strText);
}

void ZepWindow::ClampCursorToDisplay()
{
    // Always ensure the cursor is inside the display
    cursorCL.y = ClampVisibleLine(cursorCL.y);
    cursorCL = ClampVisibleColumn(cursorCL, cursorMode == CursorMode::Insert ? LineLocation::LineCRBegin : LineLocation::LineLastNonCR);
}

void ZepWindow::PreDisplay(const DisplayRegion& region)
{
    m_bufferRegion = region;

    // ** Temporary, status
    std::ostringstream str;
    str << "(" << GetEditor().GetCurrentMode()->Name() << ") NORMAL" << " : " << int(m_buffer.GetLineEnds().size()) << " Lines";
    SetStatusText(str.str());

    auto statusCount = statusLines.size();
    const auto windowSize = m_bufferRegion.bottomRightPx - m_bufferRegion.topLeftPx;
    const float statusSize = m_display.GetFontSize() * statusCount + textBorder * 2.0f; 

    m_statusRegion.bottomRightPx = m_bufferRegion.bottomRightPx;
    m_statusRegion.topLeftPx = m_bufferRegion.bottomRightPx - NVec2f(windowSize.x, statusSize);

    m_textRegion.bottomRightPx = m_statusRegion.topLeftPx + NVec2f(windowSize.x, 0.0f);
    m_textRegion.topLeftPx = m_bufferRegion.topLeftPx;

    // Border, and move the text across a bit
    m_leftRegion.topLeftPx = m_textRegion.topLeftPx;
    m_leftRegion.bottomRightPx = NVec2f(m_leftRegion.topLeftPx.x + leftBorder, m_textRegion.bottomRightPx.y);

    m_textRegion.topLeftPx.x += leftBorder + textBorder;

    auto defaultLineSize = m_display.GetTextSize((Zep::utf8*)"A");
    m_maxDisplayLines = (long)std::max(0.0f, std::floor((m_textRegion.bottomRightPx.y - m_textRegion.topLeftPx.y) / defaultLineSize.y));

    // Start at this displayed buffer line
    LineInfo lineInfo;
    lineInfo.lastNonCROffset = -1;
    lineInfo.firstGraphCharOffset = -1;
    lineInfo.lastGraphCharOffset = -1;
    lineInfo.lineNumber = long(bufferCL.y);
    lineInfo.screenLineNumber = 0;
    lineInfo.screenPosYPx = m_textRegion.topLeftPx.y;

    float screenPosX = m_textRegion.topLeftPx.y;
    visibleLines.clear();

    // Process every buffer line
    for (;;)
    {
        // We haven't processed this line yet, so we can't display anything
        // else
        if (m_buffer.GetLineCount() <= lineInfo.lineNumber)
            break;

        NVec2i columnOffsets;
        if (!m_buffer.GetLineOffsets(lineInfo.lineNumber, columnOffsets.x, columnOffsets.y))
            break;

        // Start this line at the current offset
        lineInfo.columnOffsets.x = columnOffsets.x;
        lineInfo.columnOffsets.y = columnOffsets.x;
        lineInfo.lastGraphCharOffset = 0;

        bool inEndLine = false;
        bool finishedLines = false;

        const auto& textBuffer = m_buffer.GetText();

        // Walk from the start of the line to the end of the line (in buffer chars)
        // Line:
        // [beginoffset]ABCDEF\n[endoffset]
        for (auto ch = columnOffsets.x; ch < columnOffsets.y; ch++)
        {
            const utf8* pCh = &textBuffer[ch];

            // Convenience for later
            if (std::isgraph(*pCh))
            {
                if (lineInfo.firstGraphCharOffset == -1)
                {
                    lineInfo.firstGraphCharOffset = ch;
                }
                lineInfo.lastGraphCharOffset = ch;
            }

            // Shown only one char for end of line
            if (*pCh == '\n' ||
                *pCh == 0)
            {
                inEndLine = true;
            }
            else
            {
                lineInfo.lastNonCROffset = std::max(ch, 0l);
            }

            // TODO: Central UTF-8 helpers
#define UTF8_CHAR_LEN( byte ) (( 0xE5000000 >> (( byte >> 3 ) & 0x1e )) & 3 ) + 1
            const utf8* pEnd = pCh + UTF8_CHAR_LEN(*pCh);

            auto textSize = m_display.GetTextSize(pCh, pEnd);

            lineInfo.columnOffsets.y = ch;

            // We walked off the end
            if ((lineInfo.screenPosYPx + textSize.y) >= m_textRegion.bottomRightPx.y)
            {
                // No more lines
                finishedLines = true;
                break;
            }

            // Wrap
            if (wrap)
            {
                if (((screenPosX + textSize.x) + textSize.x) >= (m_textRegion.bottomRightPx.x))
                {
                    // Remember the offset beyond the end of hte line
                    visibleLines.push_back(lineInfo);

                    // Now jump to the next 'screen line' for the rest of this 'buffer line'
                    lineInfo.columnOffsets = NVec2i(ch, ch);
                    lineInfo.lastNonCROffset = 0;
                    lineInfo.firstGraphCharOffset = -1;
                    lineInfo.lastGraphCharOffset = -1;
                    lineInfo.screenLineNumber++;
                    lineInfo.screenPosYPx += textSize.y;
                    screenPosX = m_textRegion.topLeftPx.x;
                }
                else
                {
                    screenPosX += textSize.x;
                }
            }
        }

        // We walked all the actual chars, and stopped at 1< than column.y
        // So back to the correct line end offset here
        lineInfo.columnOffsets.y++;

        // Ignore this one
        if (finishedLines)
        {
            break;
        }

        visibleLines.push_back(lineInfo);

        // Next Line
        lineInfo.screenPosYPx += defaultLineSize.y;
        lineInfo.screenLineNumber++;
        lineInfo.lineNumber++;
        lineInfo.lastNonCROffset = 0;
        screenPosX = m_textRegion.topLeftPx.x;
    }

    if (visibleLines.empty())
    {
        LineInfo lineInfo;
        lineInfo.columnOffsets.x = 0;
        lineInfo.columnOffsets.y = 0;
        lineInfo.lastNonCROffset = 0;
        lineInfo.firstGraphCharOffset = 0;
        lineInfo.lastGraphCharOffset = 0;
        lineInfo.lineNumber = 0;
        lineInfo.screenLineNumber = 0;
        lineInfo.screenPosYPx = m_textRegion.topLeftPx.y;
        visibleLines.push_back(lineInfo);
    }

    ClampCursorToDisplay();
}

// TODO: This function draws one char at a time.  It could be more optimal at the expense of some 
// complexity.  
// The text is displayed acorrding to the region bounds and the display lineData
// Additionally (and perhaps that should be a seperate function), this code draws line numbers
bool ZepWindow::DisplayLine(const LineInfo& lineInfo, const DisplayRegion& region, int displayPass)
{
    // A middle-dot whitespace character
    static const auto whiteSpace = StringUtils::makeStr(std::wstring(L"\x00b7"));
    static const auto blankSpace = ' ';

    auto activeWindow = (m_display.GetCurrentWindow()->GetCurrentWindow() == this);

    // Draw line numbers
    auto showLineNumber = [&]()
    {
        auto cursorBufferLine = visibleLines[cursorCL.y].lineNumber;
        std::string strNum;
        if (displayMode == DisplayMode::Vim)
        {
            strNum = std::to_string(abs(lineInfo.lineNumber - cursorBufferLine));
        }
        else
        {
            strNum = std::to_string(lineInfo.lineNumber);
        }
        auto textSize = m_display.GetTextSize((const utf8*)strNum.c_str(), (const utf8*)(strNum.c_str() + strNum.size()));

        // Number background
        m_display.DrawRectFilled(NVec2f(m_leftRegion.topLeftPx.x, lineInfo.screenPosYPx),
            NVec2f(m_leftRegion.bottomRightPx.x, lineInfo.screenPosYPx + m_display.GetFontSize()),
            0xFF222222);

        auto digitCol = 0xFF11FF11;
        if (cursorCL.y == lineInfo.screenLineNumber)
        {
            digitCol = Color_CursorNormal;
        }

        // Numbers
        m_display.DrawChars(NVec2f(m_leftRegion.bottomRightPx.x - textSize.x - textBorder, lineInfo.screenPosYPx), digitCol,
            (const utf8*)strNum.c_str(),
            (const utf8*)(strNum.c_str() + strNum.size()));
    };

    if (displayPass == WindowPass::Background)
    {
        showLineNumber();
    }

    bool foundCR = false;
    auto screenPosX = m_textRegion.topLeftPx.x;

    char invalidChar;

    bool foundCursor = false;

    // Walk from the start of the line to the end of the line (in buffer chars)
    for (auto ch = lineInfo.columnOffsets.x; ch < lineInfo.columnOffsets.y; ch++)
    {
        auto pSyntax = m_buffer.GetSyntax();
        auto col = pSyntax != nullptr ? Theme::Instance().GetColor(pSyntax->GetSyntaxAt(ch)) : 0xFFFFFFFF;
        auto* pCh = &m_buffer.GetText()[ch];
        auto bufferLocation = DisplayToBuffer(NVec2i(ch - lineInfo.columnOffsets.x, lineInfo.screenLineNumber));

        // Visible white space
        if (pSyntax && pSyntax->GetSyntaxAt(ch) == SyntaxType::Whitespace)
        {
            pCh = (const utf8*)whiteSpace.c_str();
        }

        // Shown only one char for end of line
        if (*pCh == '\n' ||
            *pCh == 0)
        {
            invalidChar = '@' + *pCh;
            if (m_windowFlags & WindowFlags::ShowCR)
            {
                pCh = (const utf8*)&invalidChar;
            }
            else
            {
                pCh = (const utf8*)&blankSpace;
            }
            col = 0x771111FF;
        }

        // TODO: Central UTF-8 helpers
        // TODO: Test UTF/fix it.  I'm keeping UTF in mind but not testing it yet.
#define UTF8_CHAR_LEN( byte ) (( 0xE5000000 >> (( byte >> 3 ) & 0x1e )) & 3 ) + 1
        auto pEnd = pCh + UTF8_CHAR_LEN(*pCh);

        auto textSize = m_display.GetTextSize(pCh, pEnd);

        if (displayPass == 0)
        {
            if (activeWindow)
            {
                if (cursorMode == CursorMode::Visual)
                {
                    auto selectionBegin = DisplayToBuffer(selection.startCL);
                    auto selectionEnd = DisplayToBuffer(selection.endCL);
                    if (bufferLocation >= selectionBegin&&
                        bufferLocation <= selectionEnd)
                    {
                        m_display.DrawRectFilled(NVec2f(screenPosX, lineInfo.screenPosYPx), NVec2f(screenPosX + textSize.x, lineInfo.screenPosYPx + textSize.y), 0xFF784F26);
                    }
                }

                if (cursorCL.y == lineInfo.screenLineNumber)
                {
                    // Cursor
                    cursorPosPx = NVec2f(m_textRegion.topLeftPx.x + textSize.x * cursorCL.x, lineInfo.screenPosYPx);
                    auto cursorBlink = m_display.GetCursorBlinkState();
                    if (!cursorBlink)
                    {
                        switch (cursorMode)
                        {
                        default:
                        case CursorMode::Hidden:
                            break;

                        case CursorMode::Insert:
                        {
                            m_display.DrawRectFilled(NVec2f(cursorPosPx.x - 1, cursorPosPx.y), NVec2f(cursorPosPx.x, cursorPosPx.y + textSize.y), 0xEEFFFFFF);
                        }
                        break;

                        case CursorMode::Normal:
                        case CursorMode::Visual:
                        {
                            m_display.DrawRectFilled(cursorPosPx, NVec2f(cursorPosPx.x + textSize.x, cursorPosPx.y + textSize.y), Color_CursorNormal);
                        }
                        break;
                        }
                    }
                }
            }
        }
        else
        {
            if (pSyntax && pSyntax->GetSyntaxAt(ch) == SyntaxType::Whitespace)
            {
                auto centerChar = NVec2f(screenPosX + textSize.x / 2, lineInfo.screenPosYPx + textSize.y / 2);
                m_display.DrawRectFilled(centerChar - NVec2f(1.0f, 1.0f), centerChar + NVec2f(1.0f, 1.0f), 0xFF524814);
            }
            else
            {
                m_display.DrawChars(NVec2f(screenPosX, lineInfo.screenPosYPx), col,
                    pCh,
                    pEnd);
            }
        }

        screenPosX += textSize.x;
    }

    return true;
}

void ZepWindow::Display()
{
    PreDisplay(m_bufferRegion);

    auto activeWindow = (m_display.GetCurrentWindow()->GetCurrentWindow() == this);
    cursorPosPx = m_bufferRegion.topLeftPx;

    if (activeWindow)
    {
        if (cursorMode == CursorMode::Normal ||
            cursorMode == CursorMode::Insert)
        {
            auto& cursorLine = visibleLines[cursorCL.y];

            // Cursor line 
            m_display.DrawRectFilled(NVec2f(m_textRegion.topLeftPx.x, cursorLine.screenPosYPx),
                NVec2f(m_textRegion.bottomRightPx.x, cursorLine.screenPosYPx + m_display.GetFontSize()),
                0xFF222222);
        }
    }

    for (int displayPass = 0; displayPass < WindowPass::Max; displayPass++)
    {
        for (const auto& lineInfo : visibleLines)
        {
            if (!DisplayLine(lineInfo, m_textRegion, displayPass))
            {
                break;
            }
        }
    }

    if (statusLines.empty())
        statusLines.push_back(" ");
    long statusCount = long(statusLines.size());
    const float statusSize = m_display.GetFontSize() * statusCount + textBorder * 2.0f;
    auto statusSpace = statusCount;
    statusSpace = std::max(statusCount, 0l);

    // Background rect for status (airline)
    m_display.DrawRectFilled(m_statusRegion.topLeftPx, m_statusRegion.bottomRightPx, 0xAA111111);

    // Draw status text
    NVec2f screenPosYPx = m_statusRegion.topLeftPx + NVec2f(0.0f, textBorder);
    for (int i = 0; i < statusSpace; i++)
    {
        auto textSize = m_display.GetTextSize((const utf8*)statusLines[i].c_str(),
            (const utf8*)(statusLines[i].c_str() + statusLines[i].size()));

        m_display.DrawRectFilled(screenPosYPx, screenPosYPx + NVec2f(textSize.x, m_display.GetFontSize() + textBorder), 0xFF111111 );
        m_display.DrawChars(screenPosYPx,
            0xFFFFFFFF,
            (const utf8*)(statusLines[i].c_str()));

        screenPosYPx.y += m_display.GetFontSize();
        screenPosYPx.x = m_statusRegion.topLeftPx.x;
    }

}
} // Zep
