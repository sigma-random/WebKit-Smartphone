/*
 * Copyright (C) 2006, 2007, 2008 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "Editor.h"

#include "AXObjectCache.h"
#include "ApplyStyleCommand.h"
#include "CSSComputedStyleDeclaration.h"
#include "CSSMutableStyleDeclaration.h"
#include "CSSProperty.h"
#include "CSSPropertyNames.h"
#include "CSSStyleSelector.h"
#include "CSSValueKeywords.h"
#include "CharacterNames.h"
#include "ClipboardEvent.h"
#include "CompositionEvent.h"
#include "CreateLinkCommand.h"
#include "DeleteButtonController.h"
#include "DeleteSelectionCommand.h"
#include "CachedResourceLoader.h"
#include "DocumentFragment.h"
#include "EditingText.h"
#include "EditorClient.h"
#include "EventHandler.h"
#include "EventNames.h"
#include "FocusController.h"
#include "Frame.h"
#include "FrameTree.h"
#include "FrameView.h"
#include "HTMLInputElement.h"
#include "HTMLTextAreaElement.h"
#include "HitTestResult.h"
#include "IndentOutdentCommand.h"
#include "InsertListCommand.h"
#include "KeyboardEvent.h"
#include "KillRing.h"
#include "ModifySelectionListLevel.h"
#include "NodeList.h"
#include "Page.h"
#include "Pasteboard.h"
#include "RemoveFormatCommand.h"
#include "RenderBlock.h"
#include "RenderPart.h"
#include "ReplaceSelectionCommand.h"
#include "Settings.h"
#include "Sound.h"
#include "Text.h"
#include "TextEvent.h"
#include "TextIterator.h"
#include "TypingCommand.h"
#include "UserTypingGestureIndicator.h"
#include "htmlediting.h"
#include "markup.h"
#include "visible_units.h"
#include <wtf/UnusedParam.h>

namespace WebCore {

using namespace std;
using namespace HTMLNames;

// When an event handler has moved the selection outside of a text control
// we should use the target control's selection for this editing operation.
VisibleSelection Editor::selectionForCommand(Event* event)
{
    VisibleSelection selection = m_frame->selection()->selection();
    if (!event)
        return selection;
    // If the target is a text control, and the current selection is outside of its shadow tree,
    // then use the saved selection for that text control.
    Node* target = event->target()->toNode();
    Node* selectionStart = selection.start().node();
    if (target && (!selectionStart || target->shadowAncestorNode() != selectionStart->shadowAncestorNode())) {
        if (target->hasTagName(inputTag) && static_cast<HTMLInputElement*>(target)->isTextField())
            return static_cast<HTMLInputElement*>(target)->selection();
        if (target->hasTagName(textareaTag))
            return static_cast<HTMLTextAreaElement*>(target)->selection();
    }
    return selection;
}

// Function considers Mac editing behavior a fallback when Page or Settings is not available.
EditingBehavior Editor::behavior() const
{
    if (!m_frame || !m_frame->settings())
        return EditingBehavior(EditingMacBehavior);

    return EditingBehavior(m_frame->settings()->editingBehaviorType());
}

EditorClient* Editor::client() const
{
    if (Page* page = m_frame->page())
        return page->editorClient();
    return 0;
}

void Editor::handleKeyboardEvent(KeyboardEvent* event)
{
    if (EditorClient* c = client())
        c->handleKeyboardEvent(event);
}

void Editor::handleInputMethodKeydown(KeyboardEvent* event)
{
    if (EditorClient* c = client())
        c->handleInputMethodKeydown(event);
}

bool Editor::handleTextEvent(TextEvent* event)
{
    // Default event handling for Drag and Drop will be handled by DragController
    // so we leave the event for it.
    if (event->isDrop())
        return false;

    if (event->isPaste()) {
        if (event->pastingFragment())
            replaceSelectionWithFragment(event->pastingFragment(), false, event->shouldSmartReplace(), event->shouldMatchStyle());
        else 
            replaceSelectionWithText(event->data(), false, event->shouldSmartReplace());
        return true;
    }

    String data = event->data();
    if (data == "\n") {
        if (event->isLineBreak())
            return insertLineBreak();
        return insertParagraphSeparator();
    }

    return insertTextWithoutSendingTextEvent(data, false, event);
}

bool Editor::canEdit() const
{
    return m_frame->selection()->isContentEditable();
}

bool Editor::canEditRichly() const
{
    return m_frame->selection()->isContentRichlyEditable();
}

// WinIE uses onbeforecut and onbeforepaste to enables the cut and paste menu items.  They
// also send onbeforecopy, apparently for symmetry, but it doesn't affect the menu items.
// We need to use onbeforecopy as a real menu enabler because we allow elements that are not
// normally selectable to implement copy/paste (like divs, or a document body).

bool Editor::canDHTMLCut()
{
    return !m_frame->selection()->isInPasswordField() && !dispatchCPPEvent(eventNames().beforecutEvent, ClipboardNumb);
}

bool Editor::canDHTMLCopy()
{
    return !m_frame->selection()->isInPasswordField() && !dispatchCPPEvent(eventNames().beforecopyEvent, ClipboardNumb);
}

bool Editor::canDHTMLPaste()
{
    return !dispatchCPPEvent(eventNames().beforepasteEvent, ClipboardNumb);
}

bool Editor::canCut() const
{
    return canCopy() && canDelete();
}

static HTMLImageElement* imageElementFromImageDocument(Document* document)
{
    if (!document)
        return 0;
    if (!document->isImageDocument())
        return 0;
    
    HTMLElement* body = document->body();
    if (!body)
        return 0;
    
    Node* node = body->firstChild();
    if (!node)
        return 0;    
    if (!node->hasTagName(imgTag))
        return 0;
    return static_cast<HTMLImageElement*>(node);
}

bool Editor::canCopy() const
{
    if (imageElementFromImageDocument(m_frame->document()))
        return true;
    SelectionController* selection = m_frame->selection();
    return selection->isRange() && !selection->isInPasswordField();
}

bool Editor::canPaste() const
{
    return canEdit();
}

bool Editor::canDelete() const
{
    SelectionController* selection = m_frame->selection();
    return selection->isRange() && selection->isContentEditable();
}

bool Editor::canDeleteRange(Range* range) const
{
    ExceptionCode ec = 0;
    Node* startContainer = range->startContainer(ec);
    Node* endContainer = range->endContainer(ec);
    if (!startContainer || !endContainer)
        return false;
    
    if (!startContainer->isContentEditable() || !endContainer->isContentEditable())
        return false;
    
    if (range->collapsed(ec)) {
        VisiblePosition start(startContainer, range->startOffset(ec), DOWNSTREAM);
        VisiblePosition previous = start.previous();
        // FIXME: We sometimes allow deletions at the start of editable roots, like when the caret is in an empty list item.
        if (previous.isNull() || previous.deepEquivalent().node()->rootEditableElement() != startContainer->rootEditableElement())
            return false;
    }
    return true;
}

bool Editor::smartInsertDeleteEnabled()
{   
    return client() && client()->smartInsertDeleteEnabled();
}
    
bool Editor::canSmartCopyOrDelete()
{
    return client() && client()->smartInsertDeleteEnabled() && m_frame->selection()->granularity() == WordGranularity;
}

bool Editor::isSelectTrailingWhitespaceEnabled()
{
    return client() && client()->isSelectTrailingWhitespaceEnabled();
}

bool Editor::deleteWithDirection(SelectionController::EDirection direction, TextGranularity granularity, bool killRing, bool isTypingAction)
{
    if (!canEdit())
        return false;

    if (m_frame->selection()->isRange()) {
        if (isTypingAction) {
            TypingCommand::deleteKeyPressed(m_frame->document(), canSmartCopyOrDelete(), granularity);
            revealSelectionAfterEditingOperation();
        } else {
            if (killRing)
                addToKillRing(selectedRange().get(), false);
            deleteSelectionWithSmartDelete(canSmartCopyOrDelete());
            // Implicitly calls revealSelectionAfterEditingOperation().
        }
    } else {
        switch (direction) {
        case SelectionController::DirectionForward:
        case SelectionController::DirectionRight:
            TypingCommand::forwardDeleteKeyPressed(m_frame->document(), canSmartCopyOrDelete(), granularity, killRing);
            break;
        case SelectionController::DirectionBackward:
        case SelectionController::DirectionLeft:
            TypingCommand::deleteKeyPressed(m_frame->document(), canSmartCopyOrDelete(), granularity, killRing);
            break;
        }
        revealSelectionAfterEditingOperation();
    }

    // FIXME: We should to move this down into deleteKeyPressed.
    // clear the "start new kill ring sequence" setting, because it was set to true
    // when the selection was updated by deleting the range
    if (killRing)
        setStartNewKillRingSequence(false);

    return true;
}

void Editor::deleteSelectionWithSmartDelete(bool smartDelete)
{
    if (m_frame->selection()->isNone())
        return;
    
    applyCommand(DeleteSelectionCommand::create(m_frame->document(), smartDelete));
}

void Editor::pasteAsPlainText(const String& pastingText, bool smartReplace)
{
    Node* target = findEventTargetFromSelection();
    if (!target)
        return;
    ExceptionCode ec = 0;
    target->dispatchEvent(TextEvent::createForPlainTextPaste(m_frame->domWindow(), pastingText, smartReplace), ec);
}

void Editor::pasteAsFragment(PassRefPtr<DocumentFragment> pastingFragment, bool smartReplace, bool matchStyle)
{
    Node* target = findEventTargetFromSelection();
    if (!target)
        return;
    ExceptionCode ec = 0;
    target->dispatchEvent(TextEvent::createForFragmentPaste(m_frame->domWindow(), pastingFragment, smartReplace, matchStyle), ec);
}

void Editor::pasteAsPlainTextBypassingDHTML()
{
    pasteAsPlainTextWithPasteboard(Pasteboard::generalPasteboard());
}

void Editor::pasteAsPlainTextWithPasteboard(Pasteboard* pasteboard)
{
    String text = pasteboard->plainText(m_frame);
    if (client() && client()->shouldInsertText(text, selectedRange().get(), EditorInsertActionPasted))
        pasteAsPlainText(text, canSmartReplaceWithPasteboard(pasteboard));
}

#if !PLATFORM(MAC)
void Editor::pasteWithPasteboard(Pasteboard* pasteboard, bool allowPlainText)
{
    RefPtr<Range> range = selectedRange();
    bool chosePlainText;
    RefPtr<DocumentFragment> fragment = pasteboard->documentFragment(m_frame, range, allowPlainText, chosePlainText);
    if (fragment && shouldInsertFragment(fragment, range, EditorInsertActionPasted))
        pasteAsFragment(fragment, canSmartReplaceWithPasteboard(pasteboard), chosePlainText);
}
#endif

bool Editor::canSmartReplaceWithPasteboard(Pasteboard* pasteboard)
{
    return client() && client()->smartInsertDeleteEnabled() && pasteboard->canSmartReplace();
}

bool Editor::shouldInsertFragment(PassRefPtr<DocumentFragment> fragment, PassRefPtr<Range> replacingDOMRange, EditorInsertAction givenAction)
{
    if (!client())
        return false;
    
    if (fragment) {
        Node* child = fragment->firstChild();
        if (child && fragment->lastChild() == child && child->isCharacterDataNode())
            return client()->shouldInsertText(static_cast<CharacterData*>(child)->data(), replacingDOMRange.get(), givenAction);        
    }

    return client()->shouldInsertNode(fragment.get(), replacingDOMRange.get(), givenAction);
}

void Editor::replaceSelectionWithFragment(PassRefPtr<DocumentFragment> fragment, bool selectReplacement, bool smartReplace, bool matchStyle)
{
    if (m_frame->selection()->isNone() || !fragment)
        return;
    
    applyCommand(ReplaceSelectionCommand::create(m_frame->document(), fragment, selectReplacement, smartReplace, matchStyle));
    revealSelectionAfterEditingOperation();
}

void Editor::replaceSelectionWithText(const String& text, bool selectReplacement, bool smartReplace)
{
    replaceSelectionWithFragment(createFragmentFromText(selectedRange().get(), text), selectReplacement, smartReplace, true); 
}

PassRefPtr<Range> Editor::selectedRange()
{
    if (!m_frame)
        return 0;
    return m_frame->selection()->toNormalizedRange();
}

bool Editor::shouldDeleteRange(Range* range) const
{
    ExceptionCode ec;
    if (!range || range->collapsed(ec))
        return false;
    
    if (!canDeleteRange(range))
        return false;

    return client() && client()->shouldDeleteRange(range);
}

bool Editor::tryDHTMLCopy()
{   
    if (m_frame->selection()->isInPasswordField())
        return false;

    if (canCopy())
        // Must be done before oncopy adds types and data to the pboard,
        // also done for security, as it erases data from the last copy/paste.
        Pasteboard::generalPasteboard()->clear();

    return !dispatchCPPEvent(eventNames().copyEvent, ClipboardWritable);
}

bool Editor::tryDHTMLCut()
{
    if (m_frame->selection()->isInPasswordField())
        return false;
    
    if (canCut())
        // Must be done before oncut adds types and data to the pboard,
        // also done for security, as it erases data from the last copy/paste.
        Pasteboard::generalPasteboard()->clear();

    return !dispatchCPPEvent(eventNames().cutEvent, ClipboardWritable);
}

bool Editor::tryDHTMLPaste()
{
    return !dispatchCPPEvent(eventNames().pasteEvent, ClipboardReadable);
}

void Editor::writeSelectionToPasteboard(Pasteboard* pasteboard)
{
    pasteboard->writeSelection(selectedRange().get(), canSmartCopyOrDelete(), m_frame);
}

bool Editor::shouldInsertText(const String& text, Range* range, EditorInsertAction action) const
{
    return client() && client()->shouldInsertText(text, range, action);
}

bool Editor::shouldShowDeleteInterface(HTMLElement* element) const
{
    return client() && client()->shouldShowDeleteInterface(element);
}

void Editor::respondToChangedSelection(const VisibleSelection& oldSelection)
{
    if (client())
        client()->respondToChangedSelection();
    m_deleteButtonController->respondToChangedSelection(oldSelection);
}

void Editor::respondToChangedContents(const VisibleSelection& endingSelection)
{
    if (AXObjectCache::accessibilityEnabled()) {
        Node* node = endingSelection.start().node();
        if (node)
            m_frame->document()->axObjectCache()->postNotification(node->renderer(), AXObjectCache::AXValueChanged, false);
    }
    
    if (client())
        client()->respondToChangedContents();  
}

const SimpleFontData* Editor::fontForSelection(bool& hasMultipleFonts) const
{
#if !PLATFORM(QT)
    hasMultipleFonts = false;

    if (!m_frame->selection()->isRange()) {
        Node* nodeToRemove;
        RenderStyle* style = styleForSelectionStart(nodeToRemove); // sets nodeToRemove

        const SimpleFontData* result = 0;
        if (style)
            result = style->font().primaryFont();
        
        if (nodeToRemove) {
            ExceptionCode ec;
            nodeToRemove->remove(ec);
            ASSERT(!ec);
        }

        return result;
    }

    const SimpleFontData* font = 0;

    RefPtr<Range> range = m_frame->selection()->toNormalizedRange();
    Node* startNode = range->editingStartPosition().node();
    if (startNode) {
        Node* pastEnd = range->pastLastNode();
        // In the loop below, n should eventually match pastEnd and not become nil, but we've seen at least one
        // unreproducible case where this didn't happen, so check for nil also.
        for (Node* n = startNode; n && n != pastEnd; n = n->traverseNextNode()) {
            RenderObject* renderer = n->renderer();
            if (!renderer)
                continue;
            // FIXME: Are there any node types that have renderers, but that we should be skipping?
            const SimpleFontData* f = renderer->style()->font().primaryFont();
            if (!font)
                font = f;
            else if (font != f) {
                hasMultipleFonts = true;
                break;
            }
        }
    }

    return font;
#else
    return 0;
#endif
}

WritingDirection Editor::textDirectionForSelection(bool& hasNestedOrMultipleEmbeddings) const
{
    hasNestedOrMultipleEmbeddings = true;

    if (m_frame->selection()->isNone())
        return NaturalWritingDirection;

    Position position = m_frame->selection()->selection().start().downstream();

    Node* node = position.node();
    if (!node)
        return NaturalWritingDirection;

    Position end;
    if (m_frame->selection()->isRange()) {
        end = m_frame->selection()->selection().end().upstream();

        Node* pastLast = Range::create(m_frame->document(), rangeCompliantEquivalent(position), rangeCompliantEquivalent(end))->pastLastNode();
        for (Node* n = node; n && n != pastLast; n = n->traverseNextNode()) {
            if (!n->isStyledElement())
                continue;

            RefPtr<CSSComputedStyleDeclaration> style = computedStyle(n);
            RefPtr<CSSValue> unicodeBidi = style->getPropertyCSSValue(CSSPropertyUnicodeBidi);
            if (!unicodeBidi)
                continue;

            ASSERT(unicodeBidi->isPrimitiveValue());
            int unicodeBidiValue = static_cast<CSSPrimitiveValue*>(unicodeBidi.get())->getIdent();
            if (unicodeBidiValue == CSSValueEmbed || unicodeBidiValue == CSSValueBidiOverride)
                return NaturalWritingDirection;
        }
    }

    if (m_frame->selection()->isCaret()) {
        if (CSSMutableStyleDeclaration* typingStyle = m_frame->selection()->typingStyle()) {
            RefPtr<CSSValue> unicodeBidi = typingStyle->getPropertyCSSValue(CSSPropertyUnicodeBidi);
            if (unicodeBidi) {
                ASSERT(unicodeBidi->isPrimitiveValue());
                int unicodeBidiValue = static_cast<CSSPrimitiveValue*>(unicodeBidi.get())->getIdent();
                if (unicodeBidiValue == CSSValueEmbed) {
                    RefPtr<CSSValue> direction = typingStyle->getPropertyCSSValue(CSSPropertyDirection);
                    ASSERT(!direction || direction->isPrimitiveValue());
                    if (direction) {
                        hasNestedOrMultipleEmbeddings = false;
                        return static_cast<CSSPrimitiveValue*>(direction.get())->getIdent() == CSSValueLtr ? LeftToRightWritingDirection : RightToLeftWritingDirection;
                    }
                } else if (unicodeBidiValue == CSSValueNormal) {
                    hasNestedOrMultipleEmbeddings = false;
                    return NaturalWritingDirection;
                }
            }
        }
        node = m_frame->selection()->selection().visibleStart().deepEquivalent().node();
    }

    // The selection is either a caret with no typing attributes or a range in which no embedding is added, so just use the start position
    // to decide.
    Node* block = enclosingBlock(node);
    WritingDirection foundDirection = NaturalWritingDirection;

    for (; node != block; node = node->parent()) {
        if (!node->isStyledElement())
            continue;

        RefPtr<CSSComputedStyleDeclaration> style = computedStyle(node);
        RefPtr<CSSValue> unicodeBidi = style->getPropertyCSSValue(CSSPropertyUnicodeBidi);
        if (!unicodeBidi)
            continue;

        ASSERT(unicodeBidi->isPrimitiveValue());
        int unicodeBidiValue = static_cast<CSSPrimitiveValue*>(unicodeBidi.get())->getIdent();
        if (unicodeBidiValue == CSSValueNormal)
            continue;

        if (unicodeBidiValue == CSSValueBidiOverride)
            return NaturalWritingDirection;

        ASSERT(unicodeBidiValue == CSSValueEmbed);
        RefPtr<CSSValue> direction = style->getPropertyCSSValue(CSSPropertyDirection);
        if (!direction)
            continue;

        ASSERT(direction->isPrimitiveValue());
        int directionValue = static_cast<CSSPrimitiveValue*>(direction.get())->getIdent();
        if (directionValue != CSSValueLtr && directionValue != CSSValueRtl)
            continue;

        if (foundDirection != NaturalWritingDirection)
            return NaturalWritingDirection;

        // In the range case, make sure that the embedding element persists until the end of the range.
        if (m_frame->selection()->isRange() && !end.node()->isDescendantOf(node))
            return NaturalWritingDirection;

        foundDirection = directionValue == CSSValueLtr ? LeftToRightWritingDirection : RightToLeftWritingDirection;
    }
    hasNestedOrMultipleEmbeddings = false;
    return foundDirection;
}

bool Editor::hasBidiSelection() const
{
    if (m_frame->selection()->isNone())
        return false;

    Node* startNode;
    if (m_frame->selection()->isRange()) {
        startNode = m_frame->selection()->selection().start().downstream().node();
        Node* endNode = m_frame->selection()->selection().end().upstream().node();
        if (enclosingBlock(startNode) != enclosingBlock(endNode))
            return false;
    } else
        startNode = m_frame->selection()->selection().visibleStart().deepEquivalent().node();

    RenderObject* renderer = startNode->renderer();
    while (renderer && !renderer->isRenderBlock())
        renderer = renderer->parent();

    if (!renderer)
        return false;

    RenderStyle* style = renderer->style();
    if (style->direction() == RTL)
        return true;

    return toRenderBlock(renderer)->containsNonZeroBidiLevel();
}

TriState Editor::selectionUnorderedListState() const
{
    if (m_frame->selection()->isCaret()) {
        if (enclosingNodeWithTag(m_frame->selection()->selection().start(), ulTag))
            return TrueTriState;
    } else if (m_frame->selection()->isRange()) {
        Node* startNode = enclosingNodeWithTag(m_frame->selection()->selection().start(), ulTag);
        Node* endNode = enclosingNodeWithTag(m_frame->selection()->selection().end(), ulTag);
        if (startNode && endNode && startNode == endNode)
            return TrueTriState;
    }

    return FalseTriState;
}

TriState Editor::selectionOrderedListState() const
{
    if (m_frame->selection()->isCaret()) {
        if (enclosingNodeWithTag(m_frame->selection()->selection().start(), olTag))
            return TrueTriState;
    } else if (m_frame->selection()->isRange()) {
        Node* startNode = enclosingNodeWithTag(m_frame->selection()->selection().start(), olTag);
        Node* endNode = enclosingNodeWithTag(m_frame->selection()->selection().end(), olTag);
        if (startNode && endNode && startNode == endNode)
            return TrueTriState;
    }

    return FalseTriState;
}

PassRefPtr<Node> Editor::insertOrderedList()
{
    if (!canEditRichly())
        return 0;
        
    RefPtr<Node> newList = InsertListCommand::insertList(m_frame->document(), InsertListCommand::OrderedList);
    revealSelectionAfterEditingOperation();
    return newList;
}

PassRefPtr<Node> Editor::insertUnorderedList()
{
    if (!canEditRichly())
        return 0;
        
    RefPtr<Node> newList = InsertListCommand::insertList(m_frame->document(), InsertListCommand::UnorderedList);
    revealSelectionAfterEditingOperation();
    return newList;
}

bool Editor::canIncreaseSelectionListLevel()
{
    return canEditRichly() && IncreaseSelectionListLevelCommand::canIncreaseSelectionListLevel(m_frame->document());
}

bool Editor::canDecreaseSelectionListLevel()
{
    return canEditRichly() && DecreaseSelectionListLevelCommand::canDecreaseSelectionListLevel(m_frame->document());
}

PassRefPtr<Node> Editor::increaseSelectionListLevel()
{
    if (!canEditRichly() || m_frame->selection()->isNone())
        return 0;
    
    RefPtr<Node> newList = IncreaseSelectionListLevelCommand::increaseSelectionListLevel(m_frame->document());
    revealSelectionAfterEditingOperation();
    return newList;
}

PassRefPtr<Node> Editor::increaseSelectionListLevelOrdered()
{
    if (!canEditRichly() || m_frame->selection()->isNone())
        return 0;
    
    RefPtr<Node> newList = IncreaseSelectionListLevelCommand::increaseSelectionListLevelOrdered(m_frame->document());
    revealSelectionAfterEditingOperation();
    return newList.release();
}

PassRefPtr<Node> Editor::increaseSelectionListLevelUnordered()
{
    if (!canEditRichly() || m_frame->selection()->isNone())
        return 0;
    
    RefPtr<Node> newList = IncreaseSelectionListLevelCommand::increaseSelectionListLevelUnordered(m_frame->document());
    revealSelectionAfterEditingOperation();
    return newList.release();
}

void Editor::decreaseSelectionListLevel()
{
    if (!canEditRichly() || m_frame->selection()->isNone())
        return;
    
    DecreaseSelectionListLevelCommand::decreaseSelectionListLevel(m_frame->document());
    revealSelectionAfterEditingOperation();
}

void Editor::removeFormattingAndStyle()
{
    applyCommand(RemoveFormatCommand::create(m_frame->document()));
}

void Editor::clearLastEditCommand() 
{
    m_lastEditCommand.clear();
}

// Returns whether caller should continue with "the default processing", which is the same as 
// the event handler NOT setting the return value to false
bool Editor::dispatchCPPEvent(const AtomicString &eventType, ClipboardAccessPolicy policy)
{
    Node* target = findEventTargetFromSelection();
    if (!target)
        return true;
    
    RefPtr<Clipboard> clipboard = newGeneralClipboard(policy, m_frame);

    ExceptionCode ec = 0;
    RefPtr<Event> evt = ClipboardEvent::create(eventType, true, true, clipboard);
    target->dispatchEvent(evt, ec);
    bool noDefaultProcessing = evt->defaultPrevented();

    // invalidate clipboard here for security
    clipboard->setAccessPolicy(ClipboardNumb);
    
    return !noDefaultProcessing;
}

Node* Editor::findEventTargetFrom(const VisibleSelection& selection) const
{
    Node* target = selection.start().element();
    if (!target)
        target = m_frame->document()->body();
    if (!target)
        return 0;
    return target->shadowAncestorNode();

}

Node* Editor::findEventTargetFromSelection() const
{
    return findEventTargetFrom(m_frame->selection()->selection());
}

void Editor::applyStyle(CSSStyleDeclaration* style, EditAction editingAction)
{
    switch (m_frame->selection()->selectionType()) {
    case VisibleSelection::NoSelection:
        // do nothing
        break;
    case VisibleSelection::CaretSelection:
        computeAndSetTypingStyle(style, editingAction);
        break;
    case VisibleSelection::RangeSelection:
        if (style)
            applyCommand(ApplyStyleCommand::create(m_frame->document(), style, editingAction));
        break;
    }
}
    
bool Editor::shouldApplyStyle(CSSStyleDeclaration* style, Range* range)
{   
    return client()->shouldApplyStyle(style, range);
}
    
void Editor::applyParagraphStyle(CSSStyleDeclaration* style, EditAction editingAction)
{
    switch (m_frame->selection()->selectionType()) {
    case VisibleSelection::NoSelection:
        // do nothing
        break;
    case VisibleSelection::CaretSelection:
    case VisibleSelection::RangeSelection:
        if (style)
            applyCommand(ApplyStyleCommand::create(m_frame->document(), style, editingAction, ApplyStyleCommand::ForceBlockProperties));
        break;
    }
}

void Editor::applyStyleToSelection(CSSStyleDeclaration* style, EditAction editingAction)
{
    if (!style || !style->length() || !canEditRichly())
        return;

    if (client() && client()->shouldApplyStyle(style, m_frame->selection()->toNormalizedRange().get()))
        applyStyle(style, editingAction);
}

void Editor::applyParagraphStyleToSelection(CSSStyleDeclaration* style, EditAction editingAction)
{
    if (!style || !style->length() || !canEditRichly())
        return;
    
    if (client() && client()->shouldApplyStyle(style, m_frame->selection()->toNormalizedRange().get()))
        applyParagraphStyle(style, editingAction);
}

bool Editor::clientIsEditable() const
{
    return client() && client()->isEditable();
}

// CSS properties that only has a visual difference when applied to text.
static const int textOnlyProperties[] = {
    CSSPropertyTextDecoration,
    CSSPropertyWebkitTextDecorationsInEffect,
    CSSPropertyFontStyle,
    CSSPropertyFontWeight,
    CSSPropertyColor,
};

static TriState triStateOfStyleInComputedStyle(CSSStyleDeclaration* desiredStyle, CSSComputedStyleDeclaration* computedStyle, bool ignoreTextOnlyProperties = false)
{
    RefPtr<CSSMutableStyleDeclaration> diff = getPropertiesNotInComputedStyle(desiredStyle, computedStyle);

    if (ignoreTextOnlyProperties)
        diff->removePropertiesInSet(textOnlyProperties, sizeof(textOnlyProperties) / sizeof(textOnlyProperties[0]));

    if (!diff->length())
        return TrueTriState;
    if (diff->length() == desiredStyle->length())
        return FalseTriState;
    return MixedTriState;
}

bool Editor::selectionStartHasStyle(CSSStyleDeclaration* style) const
{
    Node* nodeToRemove;
    RefPtr<CSSComputedStyleDeclaration> selectionStyle = selectionComputedStyle(nodeToRemove);
    if (!selectionStyle)
        return false;
    TriState state = triStateOfStyleInComputedStyle(style, selectionStyle.get());
    if (nodeToRemove) {
        ExceptionCode ec = 0;
        nodeToRemove->remove(ec);
        ASSERT(!ec);
    }
    return state == TrueTriState;
}

TriState Editor::selectionHasStyle(CSSStyleDeclaration* style) const
{
    TriState state = FalseTriState;

    if (!m_frame->selection()->isRange()) {
        Node* nodeToRemove;
        RefPtr<CSSComputedStyleDeclaration> selectionStyle = selectionComputedStyle(nodeToRemove);
        if (!selectionStyle)
            return FalseTriState;
        state = triStateOfStyleInComputedStyle(style, selectionStyle.get());
        if (nodeToRemove) {
            ExceptionCode ec = 0;
            nodeToRemove->remove(ec);
            ASSERT(!ec);
        }
    } else {
        for (Node* node = m_frame->selection()->start().node(); node; node = node->traverseNextNode()) {
            RefPtr<CSSComputedStyleDeclaration> nodeStyle = computedStyle(node);
            if (nodeStyle) {
                TriState nodeState = triStateOfStyleInComputedStyle(style, nodeStyle.get(), !node->isTextNode());
                if (node == m_frame->selection()->start().node())
                    state = nodeState;
                else if (state != nodeState && node->isTextNode()) {
                    state = MixedTriState;
                    break;
                }
            }
            if (node == m_frame->selection()->end().node())
                break;
        }
    }

    return state;
}

static bool hasTransparentBackgroundColor(CSSStyleDeclaration* style)
{
    RefPtr<CSSValue> cssValue = style->getPropertyCSSValue(CSSPropertyBackgroundColor);
    if (!cssValue)
        return true;

    if (!cssValue->isPrimitiveValue())
        return false;
    CSSPrimitiveValue* value = static_cast<CSSPrimitiveValue*>(cssValue.get());

    if (value->primitiveType() == CSSPrimitiveValue::CSS_RGBCOLOR)
        return !alphaChannel(value->getRGBA32Value());

    return value->getIdent() == CSSValueTransparent;
}

String Editor::selectionStartCSSPropertyValue(int propertyID)
{
    Node* nodeToRemove;
    RefPtr<CSSComputedStyleDeclaration> selectionStyle = selectionComputedStyle(nodeToRemove);
    if (!selectionStyle)
        return String();

    String value = selectionStyle->getPropertyValue(propertyID);

    // If background color is transparent, traverse parent nodes until we hit a different value or document root
    // Also, if the selection is a range, ignore the background color at the start of selection,
    // and find the background color of the common ancestor.
    if (propertyID == CSSPropertyBackgroundColor && (m_frame->selection()->isRange() || hasTransparentBackgroundColor(selectionStyle.get()))) {
        RefPtr<Range> range(m_frame->selection()->toNormalizedRange());
        ExceptionCode ec = 0;
        for (Node* ancestor = range->commonAncestorContainer(ec); ancestor; ancestor = ancestor->parentNode()) {
            selectionStyle = computedStyle(ancestor);
            if (!hasTransparentBackgroundColor(selectionStyle.get())) {
                value = selectionStyle->getPropertyValue(CSSPropertyBackgroundColor);
                break;
            }
        }
    }

    if (propertyID == CSSPropertyFontSize) {
        RefPtr<CSSValue> cssValue = selectionStyle->getPropertyCSSValue(CSSPropertyFontSize);
        ASSERT(cssValue->isPrimitiveValue());
        int fontPixelSize = static_cast<CSSPrimitiveValue*>(cssValue.get())->getIntValue(CSSPrimitiveValue::CSS_PX);
        int size = CSSStyleSelector::legacyFontSize(m_frame->document(), fontPixelSize, selectionStyle->useFixedFontDefaultSize());
        value = String::number(size);
    }

    if (nodeToRemove) {
        ExceptionCode ec = 0;
        nodeToRemove->remove(ec);
        ASSERT(!ec);
    }

    return value;
}

void Editor::indent()
{
    applyCommand(IndentOutdentCommand::create(m_frame->document(), IndentOutdentCommand::Indent));
}

void Editor::outdent()
{
    applyCommand(IndentOutdentCommand::create(m_frame->document(), IndentOutdentCommand::Outdent));
}

static void dispatchEditableContentChangedEvents(const EditCommand& command)
{
    Element* startRoot = command.startingRootEditableElement();
    Element* endRoot = command.endingRootEditableElement();
    ExceptionCode ec;
    if (startRoot)
        startRoot->dispatchEvent(Event::create(eventNames().webkitEditableContentChangedEvent, false, false), ec);
    if (endRoot && endRoot != startRoot)
        endRoot->dispatchEvent(Event::create(eventNames().webkitEditableContentChangedEvent, false, false), ec);
}

void Editor::appliedEditing(PassRefPtr<EditCommand> cmd)
{
    m_frame->document()->updateLayout();
    
    dispatchEditableContentChangedEvents(*cmd);
    
    VisibleSelection newSelection(cmd->endingSelection());
    // Don't clear the typing style with this selection change.  We do those things elsewhere if necessary.
    changeSelectionAfterCommand(newSelection, false, false);
        
    if (!cmd->preservesTypingStyle())
        m_frame->selection()->clearTypingStyle();
    
    // Command will be equal to last edit command only in the case of typing
    if (m_lastEditCommand.get() == cmd)
        ASSERT(cmd->isTypingCommand());
    else {
        // Only register a new undo command if the command passed in is
        // different from the last command
        m_lastEditCommand = cmd;
        if (client())
            client()->registerCommandForUndo(m_lastEditCommand);
    }
    respondToChangedContents(newSelection);    
}

void Editor::unappliedEditing(PassRefPtr<EditCommand> cmd)
{
    m_frame->document()->updateLayout();
    
    dispatchEditableContentChangedEvents(*cmd);
    
    VisibleSelection newSelection(cmd->startingSelection());
    changeSelectionAfterCommand(newSelection, true, true);
    
    m_lastEditCommand = 0;
    if (client())
        client()->registerCommandForRedo(cmd);
    respondToChangedContents(newSelection);    
}

void Editor::reappliedEditing(PassRefPtr<EditCommand> cmd)
{
    m_frame->document()->updateLayout();
    
    dispatchEditableContentChangedEvents(*cmd);
    
    VisibleSelection newSelection(cmd->endingSelection());
    changeSelectionAfterCommand(newSelection, true, true);
    
    m_lastEditCommand = 0;
    if (client())
        client()->registerCommandForUndo(cmd);
    respondToChangedContents(newSelection);    
}

Editor::Editor(Frame* frame)
    : m_frame(frame)
    , m_deleteButtonController(adoptPtr(new DeleteButtonController(frame)))
    , m_ignoreCompositionSelectionChange(false)
    , m_shouldStartNewKillRingSequence(false)
    // This is off by default, since most editors want this behavior (this matches IE but not FF).
    , m_shouldStyleWithCSS(false)
    , m_killRing(adoptPtr(new KillRing))
    , m_correctionPanelTimer(this, &Editor::correctionPanelTimerFired)
    , m_areMarkedTextMatchesHighlighted(false)
{
}

Editor::~Editor()
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
    if (client())
        client()->dismissCorrectionPanel(true);
#endif
}

void Editor::clear()
{
    m_compositionNode = 0;
    m_customCompositionUnderlines.clear();
    m_shouldStyleWithCSS = false;
}

bool Editor::insertText(const String& text, Event* triggeringEvent)
{
    return m_frame->eventHandler()->handleTextInputEvent(text, triggeringEvent);
}

bool Editor::insertTextWithoutSendingTextEvent(const String& text, bool selectInsertedText, Event* triggeringEvent)
{
    if (text.isEmpty())
        return false;

    VisibleSelection selection = selectionForCommand(triggeringEvent);
    if (!selection.isContentEditable())
        return false;
    RefPtr<Range> range = selection.toNormalizedRange();

    if (!shouldInsertText(text, range.get(), EditorInsertActionTyped))
        return true;

    // Get the selection to use for the event that triggered this insertText.
    // If the event handler changed the selection, we may want to use a different selection
    // that is contained in the event target.
    selection = selectionForCommand(triggeringEvent);
    if (selection.isContentEditable()) {
        if (Node* selectionStart = selection.start().node()) {
            RefPtr<Document> document = selectionStart->document();
            
            // Insert the text
            TypingCommand::insertText(document.get(), text, selection, selectInsertedText);

            // Reveal the current selection 
            if (Frame* editedFrame = document->frame())
                if (Page* page = editedFrame->page())
                    page->focusController()->focusedOrMainFrame()->selection()->revealSelection(ScrollAlignment::alignToEdgeIfNeeded);
        }
    }

    return true;
}

bool Editor::insertLineBreak()
{
    if (!canEdit())
        return false;

    if (!shouldInsertText("\n", m_frame->selection()->toNormalizedRange().get(), EditorInsertActionTyped))
        return true;

    TypingCommand::insertLineBreak(m_frame->document());
    revealSelectionAfterEditingOperation();
    return true;
}

bool Editor::insertParagraphSeparator()
{
    if (!canEdit())
        return false;

    if (!canEditRichly())
        return insertLineBreak();

    if (!shouldInsertText("\n", m_frame->selection()->toNormalizedRange().get(), EditorInsertActionTyped))
        return true;

    TypingCommand::insertParagraphSeparator(m_frame->document());
    revealSelectionAfterEditingOperation();
    return true;
}

void Editor::cut()
{
    if (tryDHTMLCut())
        return; // DHTML did the whole operation
    if (!canCut()) {
        systemBeep();
        return;
    }
    RefPtr<Range> selection = selectedRange();
    if (shouldDeleteRange(selection.get())) {
        if (isNodeInTextFormControl(m_frame->selection()->start().node()))
            Pasteboard::generalPasteboard()->writePlainText(selectedText());
        else
            Pasteboard::generalPasteboard()->writeSelection(selection.get(), canSmartCopyOrDelete(), m_frame);
        didWriteSelectionToPasteboard();
        deleteSelectionWithSmartDelete(canSmartCopyOrDelete());
    }
}

void Editor::copy()
{
    if (tryDHTMLCopy())
        return; // DHTML did the whole operation
    if (!canCopy()) {
        systemBeep();
        return;
    }

    if (isNodeInTextFormControl(m_frame->selection()->start().node()))
        Pasteboard::generalPasteboard()->writePlainText(selectedText());
    else {
        Document* document = m_frame->document();
        if (HTMLImageElement* imageElement = imageElementFromImageDocument(document))
            Pasteboard::generalPasteboard()->writeImage(imageElement, document->url(), document->title());
        else
            Pasteboard::generalPasteboard()->writeSelection(selectedRange().get(), canSmartCopyOrDelete(), m_frame);
    }

    didWriteSelectionToPasteboard();
}

void Editor::paste()
{
    ASSERT(m_frame->document());
    if (tryDHTMLPaste())
        return; // DHTML did the whole operation
    if (!canPaste())
        return;
    CachedResourceLoader* loader = m_frame->document()->cachedResourceLoader();
    loader->setAllowStaleResources(true);
    if (m_frame->selection()->isContentRichlyEditable())
        pasteWithPasteboard(Pasteboard::generalPasteboard(), true);
    else
        pasteAsPlainTextWithPasteboard(Pasteboard::generalPasteboard());
    loader->setAllowStaleResources(false);
}

void Editor::pasteAsPlainText()
{
    if (tryDHTMLPaste())
        return;
    if (!canPaste())
        return;
    pasteAsPlainTextWithPasteboard(Pasteboard::generalPasteboard());
}

void Editor::performDelete()
{
    if (!canDelete()) {
        systemBeep();
        return;
    }

    addToKillRing(selectedRange().get(), false);
    deleteSelectionWithSmartDelete(canSmartCopyOrDelete());

    // clear the "start new kill ring sequence" setting, because it was set to true
    // when the selection was updated by deleting the range
    setStartNewKillRingSequence(false);
}

void Editor::copyURL(const KURL& url, const String& title)
{
    Pasteboard::generalPasteboard()->writeURL(url, title, m_frame);
}

void Editor::copyImage(const HitTestResult& result)
{
    KURL url = result.absoluteLinkURL();
    if (url.isEmpty())
        url = result.absoluteImageURL();

    Pasteboard::generalPasteboard()->writeImage(result.innerNonSharedNode(), url, result.altDisplayString());
}

bool Editor::isContinuousSpellCheckingEnabled()
{
    return client() && client()->isContinuousSpellCheckingEnabled();
}

void Editor::toggleContinuousSpellChecking()
{
    if (client())
        client()->toggleContinuousSpellChecking();
}

bool Editor::isGrammarCheckingEnabled()
{
    return client() && client()->isGrammarCheckingEnabled();
}

void Editor::toggleGrammarChecking()
{
    if (client())
        client()->toggleGrammarChecking();
}

int Editor::spellCheckerDocumentTag()
{
    return client() ? client()->spellCheckerDocumentTag() : 0;
}

#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)

void Editor::uppercaseWord()
{
    if (client())
        client()->uppercaseWord();
}

void Editor::lowercaseWord()
{
    if (client())
        client()->lowercaseWord();
}

void Editor::capitalizeWord()
{
    if (client())
        client()->capitalizeWord();
}

void Editor::showSubstitutionsPanel()
{
    if (!client()) {
        LOG_ERROR("No NSSpellChecker");
        return;
    }

    if (client()->substitutionsPanelIsShowing()) {
        client()->showSubstitutionsPanel(false);
        return;
    }
    client()->showSubstitutionsPanel(true);
}

bool Editor::substitutionsPanelIsShowing()
{
    if (!client())
        return false;
    return client()->substitutionsPanelIsShowing();
}

void Editor::toggleSmartInsertDelete()
{
    if (client())
        client()->toggleSmartInsertDelete();
}

bool Editor::isAutomaticQuoteSubstitutionEnabled()
{
    return client() && client()->isAutomaticQuoteSubstitutionEnabled();
}

void Editor::toggleAutomaticQuoteSubstitution()
{
    if (client())
        client()->toggleAutomaticQuoteSubstitution();
}

bool Editor::isAutomaticLinkDetectionEnabled()
{
    return client() && client()->isAutomaticLinkDetectionEnabled();
}

void Editor::toggleAutomaticLinkDetection()
{
    if (client())
        client()->toggleAutomaticLinkDetection();
}

bool Editor::isAutomaticDashSubstitutionEnabled()
{
    return client() && client()->isAutomaticDashSubstitutionEnabled();
}

void Editor::toggleAutomaticDashSubstitution()
{
    if (client())
        client()->toggleAutomaticDashSubstitution();
}

bool Editor::isAutomaticTextReplacementEnabled()
{
    return client() && client()->isAutomaticTextReplacementEnabled();
}

void Editor::toggleAutomaticTextReplacement()
{
    if (client())
        client()->toggleAutomaticTextReplacement();
}

bool Editor::isAutomaticSpellingCorrectionEnabled()
{
    return client() && client()->isAutomaticSpellingCorrectionEnabled();
}

void Editor::toggleAutomaticSpellingCorrection()
{
    if (client())
        client()->toggleAutomaticSpellingCorrection();
}

#endif

bool Editor::shouldEndEditing(Range* range)
{
    return client() && client()->shouldEndEditing(range);
}

bool Editor::shouldBeginEditing(Range* range)
{
    return client() && client()->shouldBeginEditing(range);
}

void Editor::clearUndoRedoOperations()
{
    if (client())
        client()->clearUndoRedoOperations();
}

bool Editor::canUndo()
{
    return client() && client()->canUndo();
}

void Editor::undo()
{
    if (client())
        client()->undo();
}

bool Editor::canRedo()
{
    return client() && client()->canRedo();
}

void Editor::redo()
{
    if (client())
        client()->redo();
}

void Editor::didBeginEditing()
{
    if (client())
        client()->didBeginEditing();
}

void Editor::didEndEditing()
{
    if (client())
        client()->didEndEditing();
}

void Editor::didWriteSelectionToPasteboard()
{
    if (client())
        client()->didWriteSelectionToPasteboard();
}

void Editor::toggleBold()
{
    command("ToggleBold").execute();
}

void Editor::toggleUnderline()
{
    command("ToggleUnderline").execute();
}

void Editor::setBaseWritingDirection(WritingDirection direction)
{
    Node* focusedNode = frame()->document()->focusedNode();
    if (focusedNode && (focusedNode->hasTagName(textareaTag)
                        || (focusedNode->hasTagName(inputTag) && (static_cast<HTMLInputElement*>(focusedNode)->inputType() == HTMLInputElement::TEXT
                                                                || static_cast<HTMLInputElement*>(focusedNode)->inputType() == HTMLInputElement::SEARCH)))) {
        if (direction == NaturalWritingDirection)
            return;
        static_cast<HTMLElement*>(focusedNode)->setAttribute(dirAttr, direction == LeftToRightWritingDirection ? "ltr" : "rtl");
        frame()->document()->updateStyleIfNeeded();
        return;
    }

    RefPtr<CSSMutableStyleDeclaration> style = CSSMutableStyleDeclaration::create();
    style->setProperty(CSSPropertyDirection, direction == LeftToRightWritingDirection ? "ltr" : direction == RightToLeftWritingDirection ? "rtl" : "inherit", false);
    applyParagraphStyleToSelection(style.get(), EditActionSetWritingDirection);
}

void Editor::selectComposition()
{
    RefPtr<Range> range = compositionRange();
    if (!range)
        return;
    
    // The composition can start inside a composed character sequence, so we have to override checks.
    // See <http://bugs.webkit.org/show_bug.cgi?id=15781>
    VisibleSelection selection;
    selection.setWithoutValidation(range->startPosition(), range->endPosition());
    m_frame->selection()->setSelection(selection, false, false);
}

void Editor::confirmComposition()
{
    if (!m_compositionNode)
        return;
    confirmComposition(m_compositionNode->data().substring(m_compositionStart, m_compositionEnd - m_compositionStart), false);
}

void Editor::confirmCompositionWithoutDisturbingSelection()
{
    if (!m_compositionNode)
        return;
    confirmComposition(m_compositionNode->data().substring(m_compositionStart, m_compositionEnd - m_compositionStart), true);
}

void Editor::confirmComposition(const String& text)
{
    confirmComposition(text, false);
}

void Editor::confirmComposition(const String& text, bool preserveSelection)
{
    UserTypingGestureIndicator typingGestureIndicator(m_frame);

    setIgnoreCompositionSelectionChange(true);

    VisibleSelection oldSelection = m_frame->selection()->selection();

    selectComposition();

    if (m_frame->selection()->isNone()) {
        setIgnoreCompositionSelectionChange(false);
        return;
    }
    
    // Dispatch a compositionend event to the focused node.
    // We should send this event before sending a TextEvent as written in Section 6.2.2 and 6.2.3 of
    // the DOM Event specification.
    Node* target = m_frame->document()->focusedNode();
    if (target) {
        RefPtr<CompositionEvent> event = CompositionEvent::create(eventNames().compositionendEvent, m_frame->domWindow(), text);
        ExceptionCode ec = 0;
        target->dispatchEvent(event, ec);
    }

    // If text is empty, then delete the old composition here.  If text is non-empty, InsertTextCommand::input
    // will delete the old composition with an optimized replace operation.
    if (text.isEmpty())
        TypingCommand::deleteSelection(m_frame->document(), false);

    m_compositionNode = 0;
    m_customCompositionUnderlines.clear();

    insertText(text, 0);

    if (preserveSelection) {
        m_frame->selection()->setSelection(oldSelection, false, false);
        // An open typing command that disagrees about current selection would cause issues with typing later on.
        TypingCommand::closeTyping(m_lastEditCommand.get());
    }

    setIgnoreCompositionSelectionChange(false);
}

void Editor::setComposition(const String& text, const Vector<CompositionUnderline>& underlines, unsigned selectionStart, unsigned selectionEnd)
{
    UserTypingGestureIndicator typingGestureIndicator(m_frame);

    setIgnoreCompositionSelectionChange(true);

    selectComposition();

    if (m_frame->selection()->isNone()) {
        setIgnoreCompositionSelectionChange(false);
        return;
    }

    Node* target = m_frame->document()->focusedNode();
    if (target) {
        // Dispatch an appropriate composition event to the focused node.
        // We check the composition status and choose an appropriate composition event since this
        // function is used for three purposes:
        // 1. Starting a new composition.
        //    Send a compositionstart event when this function creates a new composition node, i.e.
        //    m_compositionNode == 0 && !text.isEmpty().
        // 2. Updating the existing composition node.
        //    Send a compositionupdate event when this function updates the existing composition
        //    node, i.e. m_compositionNode != 0 && !text.isEmpty().
        // 3. Canceling the ongoing composition.
        //    Send a compositionend event when function deletes the existing composition node, i.e.
        //    m_compositionNode != 0 && test.isEmpty().
        RefPtr<CompositionEvent> event;
        if (!m_compositionNode) {
            // We should send a compositionstart event only when the given text is not empty because this
            // function doesn't create a composition node when the text is empty.
            if (!text.isEmpty())
                event = CompositionEvent::create(eventNames().compositionstartEvent, m_frame->domWindow(), text);
        } else {
            if (!text.isEmpty())
                event = CompositionEvent::create(eventNames().compositionupdateEvent, m_frame->domWindow(), text);
            else
              event = CompositionEvent::create(eventNames().compositionendEvent, m_frame->domWindow(), text);
        }
        ExceptionCode ec = 0;
        if (event.get())
            target->dispatchEvent(event, ec);
    }

    // If text is empty, then delete the old composition here.  If text is non-empty, InsertTextCommand::input
    // will delete the old composition with an optimized replace operation.
    if (text.isEmpty())
        TypingCommand::deleteSelection(m_frame->document(), false);

    m_compositionNode = 0;
    m_customCompositionUnderlines.clear();

    if (!text.isEmpty()) {
        TypingCommand::insertText(m_frame->document(), text, true, true);

        // Find out what node has the composition now.
        Position base = m_frame->selection()->base().downstream();
        Position extent = m_frame->selection()->extent();
        Node* baseNode = base.node();
        unsigned baseOffset = base.deprecatedEditingOffset();
        Node* extentNode = extent.node();
        unsigned extentOffset = extent.deprecatedEditingOffset();

        if (baseNode && baseNode == extentNode && baseNode->isTextNode() && baseOffset + text.length() == extentOffset) {
            m_compositionNode = static_cast<Text*>(baseNode);
            m_compositionStart = baseOffset;
            m_compositionEnd = extentOffset;
            m_customCompositionUnderlines = underlines;
            size_t numUnderlines = m_customCompositionUnderlines.size();
            for (size_t i = 0; i < numUnderlines; ++i) {
                m_customCompositionUnderlines[i].startOffset += baseOffset;
                m_customCompositionUnderlines[i].endOffset += baseOffset;
            }
            if (baseNode->renderer())
                baseNode->renderer()->repaint();

            unsigned start = min(baseOffset + selectionStart, extentOffset);
            unsigned end = min(max(start, baseOffset + selectionEnd), extentOffset);
            RefPtr<Range> selectedRange = Range::create(baseNode->document(), baseNode, start, baseNode, end);                
            m_frame->selection()->setSelectedRange(selectedRange.get(), DOWNSTREAM, false);
        }
    }

    setIgnoreCompositionSelectionChange(false);
}

void Editor::ignoreSpelling()
{
    if (!client())
        return;
        
    RefPtr<Range> selectedRange = frame()->selection()->toNormalizedRange();
    if (selectedRange)
        frame()->document()->markers()->removeMarkers(selectedRange.get(), DocumentMarker::Spelling);

    String text = selectedText();
    ASSERT(text.length());
    client()->ignoreWordInSpellDocument(text);
}

void Editor::learnSpelling()
{
    if (!client())
        return;
        
    // FIXME: We don't call this on the Mac, and it should remove misspelling markers around the 
    // learned word, see <rdar://problem/5396072>.

    String text = selectedText();
    ASSERT(text.length());
    client()->learnWord(text);
}

static String findFirstMisspellingInRange(EditorClient* client, Range* searchRange, int& firstMisspellingOffset, bool markAll, RefPtr<Range>& firstMisspellingRange)
{
    ASSERT_ARG(client, client);
    ASSERT_ARG(searchRange, searchRange);
    
    WordAwareIterator it(searchRange);
    firstMisspellingOffset = 0;
    
    String firstMisspelling;
    int currentChunkOffset = 0;

    while (!it.atEnd()) {
        const UChar* chars = it.characters();
        int len = it.length();
        
        // Skip some work for one-space-char hunks
        if (!(len == 1 && chars[0] == ' ')) {
            
            int misspellingLocation = -1;
            int misspellingLength = 0;
            client->checkSpellingOfString(chars, len, &misspellingLocation, &misspellingLength);

            // 5490627 shows that there was some code path here where the String constructor below crashes.
            // We don't know exactly what combination of bad input caused this, so we're making this much
            // more robust against bad input on release builds.
            ASSERT(misspellingLength >= 0);
            ASSERT(misspellingLocation >= -1);
            ASSERT(!misspellingLength || misspellingLocation >= 0);
            ASSERT(misspellingLocation < len);
            ASSERT(misspellingLength <= len);
            ASSERT(misspellingLocation + misspellingLength <= len);
            
            if (misspellingLocation >= 0 && misspellingLength > 0 && misspellingLocation < len && misspellingLength <= len && misspellingLocation + misspellingLength <= len) {
                
                // Compute range of misspelled word
                RefPtr<Range> misspellingRange = TextIterator::subrange(searchRange, currentChunkOffset + misspellingLocation, misspellingLength);

                // Remember first-encountered misspelling and its offset.
                if (!firstMisspelling) {
                    firstMisspellingOffset = currentChunkOffset + misspellingLocation;
                    firstMisspelling = String(chars + misspellingLocation, misspellingLength);
                    firstMisspellingRange = misspellingRange;
                }

                // Store marker for misspelled word.
                ExceptionCode ec = 0;
                misspellingRange->startContainer(ec)->document()->markers()->addMarker(misspellingRange.get(), DocumentMarker::Spelling);
                ASSERT(!ec);

                // Bail out if we're marking only the first misspelling, and not all instances.
                if (!markAll)
                    break;
            }
        }
        
        currentChunkOffset += len;
        it.advance();
    }
    
    return firstMisspelling;
}

#ifndef BUILDING_ON_TIGER

static PassRefPtr<Range> paragraphAlignedRangeForRange(Range* arbitraryRange, int& offsetIntoParagraphAlignedRange, String& paragraphString)
{
    ASSERT_ARG(arbitraryRange, arbitraryRange);
    
    ExceptionCode ec = 0;
    
    // Expand range to paragraph boundaries
    RefPtr<Range> paragraphRange = arbitraryRange->cloneRange(ec);
    setStart(paragraphRange.get(), startOfParagraph(arbitraryRange->startPosition()));
    setEnd(paragraphRange.get(), endOfParagraph(arbitraryRange->endPosition()));
    
    // Compute offset from start of expanded range to start of original range
    RefPtr<Range> offsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), paragraphRange->startPosition(), arbitraryRange->startPosition());
    offsetIntoParagraphAlignedRange = TextIterator::rangeLength(offsetAsRange.get());
    
    // Fill in out parameter with string representing entire paragraph range.
    // Someday we might have a caller that doesn't use this, but for now all callers do.
    paragraphString = plainText(paragraphRange.get());

    return paragraphRange;
}

static int findFirstGrammarDetailInRange(const Vector<GrammarDetail>& grammarDetails, int badGrammarPhraseLocation, int /*badGrammarPhraseLength*/, Range *searchRange, int startOffset, int endOffset, bool markAll)
{
    // Found some bad grammar. Find the earliest detail range that starts in our search range (if any).
    // Optionally add a DocumentMarker for each detail in the range.
    int earliestDetailLocationSoFar = -1;
    int earliestDetailIndex = -1;
    for (unsigned i = 0; i < grammarDetails.size(); i++) {
        const GrammarDetail* detail = &grammarDetails[i];
        ASSERT(detail->length > 0 && detail->location >= 0);
        
        int detailStartOffsetInParagraph = badGrammarPhraseLocation + detail->location;
        
        // Skip this detail if it starts before the original search range
        if (detailStartOffsetInParagraph < startOffset)
            continue;
        
        // Skip this detail if it starts after the original search range
        if (detailStartOffsetInParagraph >= endOffset)
            continue;
        
        if (markAll) {
            RefPtr<Range> badGrammarRange = TextIterator::subrange(searchRange, badGrammarPhraseLocation - startOffset + detail->location, detail->length);
            ExceptionCode ec = 0;
            badGrammarRange->startContainer(ec)->document()->markers()->addMarker(badGrammarRange.get(), DocumentMarker::Grammar, detail->userDescription);
            ASSERT(!ec);
        }
        
        // Remember this detail only if it's earlier than our current candidate (the details aren't in a guaranteed order)
        if (earliestDetailIndex < 0 || earliestDetailLocationSoFar > detail->location) {
            earliestDetailIndex = i;
            earliestDetailLocationSoFar = detail->location;
        }
    }
    
    return earliestDetailIndex;
}
    
static String findFirstBadGrammarInRange(EditorClient* client, Range* searchRange, GrammarDetail& outGrammarDetail, int& outGrammarPhraseOffset, bool markAll)
{
    ASSERT_ARG(client, client);
    ASSERT_ARG(searchRange, searchRange);
    
    // Initialize out parameters; these will be updated if we find something to return.
    outGrammarDetail.location = -1;
    outGrammarDetail.length = 0;
    outGrammarDetail.guesses.clear();
    outGrammarDetail.userDescription = "";
    outGrammarPhraseOffset = 0;
    
    String firstBadGrammarPhrase;

    // Expand the search range to encompass entire paragraphs, since grammar checking needs that much context.
    // Determine the character offset from the start of the paragraph to the start of the original search range,
    // since we will want to ignore results in this area.
    int searchRangeStartOffset;
    String paragraphString;
    RefPtr<Range> paragraphRange = paragraphAlignedRangeForRange(searchRange, searchRangeStartOffset, paragraphString);
        
    // Determine the character offset from the start of the paragraph to the end of the original search range, 
    // since we will want to ignore results in this area also.
    int searchRangeEndOffset = searchRangeStartOffset + TextIterator::rangeLength(searchRange);
        
    // Start checking from beginning of paragraph, but skip past results that occur before the start of the original search range.
    int startOffset = 0;
    while (startOffset < searchRangeEndOffset) {
        Vector<GrammarDetail> grammarDetails;
        int badGrammarPhraseLocation = -1;
        int badGrammarPhraseLength = 0;
        client->checkGrammarOfString(paragraphString.characters() + startOffset, paragraphString.length() - startOffset, grammarDetails, &badGrammarPhraseLocation, &badGrammarPhraseLength);
        
        if (!badGrammarPhraseLength) {
            ASSERT(badGrammarPhraseLocation == -1);
            return String();
        }

        ASSERT(badGrammarPhraseLocation >= 0);
        badGrammarPhraseLocation += startOffset;

        
        // Found some bad grammar. Find the earliest detail range that starts in our search range (if any).
        int badGrammarIndex = findFirstGrammarDetailInRange(grammarDetails, badGrammarPhraseLocation, badGrammarPhraseLength, searchRange, searchRangeStartOffset, searchRangeEndOffset, markAll);
        if (badGrammarIndex >= 0) {
            ASSERT(static_cast<unsigned>(badGrammarIndex) < grammarDetails.size());
            outGrammarDetail = grammarDetails[badGrammarIndex];
        }

        // If we found a detail in range, then we have found the first bad phrase (unless we found one earlier but
        // kept going so we could mark all instances).
        if (badGrammarIndex >= 0 && firstBadGrammarPhrase.isEmpty()) {
            outGrammarPhraseOffset = badGrammarPhraseLocation - searchRangeStartOffset;
            firstBadGrammarPhrase = paragraphString.substring(badGrammarPhraseLocation, badGrammarPhraseLength);
            
            // Found one. We're done now, unless we're marking each instance.
            if (!markAll)
                break;
        }

        // These results were all between the start of the paragraph and the start of the search range; look
        // beyond this phrase.
        startOffset = badGrammarPhraseLocation + badGrammarPhraseLength;
    }
    
    return firstBadGrammarPhrase;
}
    
#endif /* not BUILDING_ON_TIGER */

#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)

static String findFirstMisspellingOrBadGrammarInRange(EditorClient* client, Range* searchRange, bool checkGrammar, bool& outIsSpelling, int& outFirstFoundOffset, GrammarDetail& outGrammarDetail)
{
    ASSERT_ARG(client, client);
    ASSERT_ARG(searchRange, searchRange);
    
    String firstFoundItem;
    String misspelledWord;
    String badGrammarPhrase;
    ExceptionCode ec = 0;
    
    // Initialize out parameters; these will be updated if we find something to return.
    outIsSpelling = true;
    outFirstFoundOffset = 0;
    outGrammarDetail.location = -1;
    outGrammarDetail.length = 0;
    outGrammarDetail.guesses.clear();
    outGrammarDetail.userDescription = "";
    
    // Expand the search range to encompass entire paragraphs, since text checking needs that much context.
    // Determine the character offset from the start of the paragraph to the start of the original search range,
    // since we will want to ignore results in this area.
    RefPtr<Range> paragraphRange = searchRange->cloneRange(ec);
    setStart(paragraphRange.get(), startOfParagraph(searchRange->startPosition()));
    int totalRangeLength = TextIterator::rangeLength(paragraphRange.get());
    setEnd(paragraphRange.get(), endOfParagraph(searchRange->startPosition()));
    
    RefPtr<Range> offsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), paragraphRange->startPosition(), searchRange->startPosition());
    int searchRangeStartOffset = TextIterator::rangeLength(offsetAsRange.get());
    int totalLengthProcessed = 0;
    
    bool firstIteration = true;
    bool lastIteration = false;
    while (totalLengthProcessed < totalRangeLength) {
        // Iterate through the search range by paragraphs, checking each one for spelling and grammar.
        int currentLength = TextIterator::rangeLength(paragraphRange.get());
        int currentStartOffset = firstIteration ? searchRangeStartOffset : 0;
        int currentEndOffset = currentLength;
        if (inSameParagraph(paragraphRange->startPosition(), searchRange->endPosition())) {
            // Determine the character offset from the end of the original search range to the end of the paragraph,
            // since we will want to ignore results in this area.
            RefPtr<Range> endOffsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), paragraphRange->startPosition(), searchRange->endPosition());
            currentEndOffset = TextIterator::rangeLength(endOffsetAsRange.get());
            lastIteration = true;
        }
        if (currentStartOffset < currentEndOffset) {
            String paragraphString = plainText(paragraphRange.get());
            if (paragraphString.length() > 0) {
                bool foundGrammar = false;
                int spellingLocation = 0;
                int grammarPhraseLocation = 0;
                int grammarDetailLocation = 0;
                unsigned grammarDetailIndex = 0;
                
                Vector<TextCheckingResult> results;
                uint64_t checkingTypes = checkGrammar ? (TextCheckingTypeSpelling | TextCheckingTypeGrammar) : TextCheckingTypeSpelling;
                client->checkTextOfParagraph(paragraphString.characters(), paragraphString.length(), checkingTypes, results);
                
                for (unsigned i = 0; i < results.size(); i++) {
                    const TextCheckingResult* result = &results[i];
                    if (result->type == TextCheckingTypeSpelling && result->location >= currentStartOffset && result->location + result->length <= currentEndOffset) {
                        ASSERT(result->length > 0 && result->location >= 0);
                        spellingLocation = result->location;
                        misspelledWord = paragraphString.substring(result->location, result->length);
                        ASSERT(misspelledWord.length());
                        break;
                    }
                    if (checkGrammar && result->type == TextCheckingTypeGrammar && result->location < currentEndOffset && result->location + result->length > currentStartOffset) {
                        ASSERT(result->length > 0 && result->location >= 0);
                        // We can't stop after the first grammar result, since there might still be a spelling result after
                        // it begins but before the first detail in it, but we can stop if we find a second grammar result.
                        if (foundGrammar)
                            break;
                        for (unsigned j = 0; j < result->details.size(); j++) {
                            const GrammarDetail* detail = &result->details[j];
                            ASSERT(detail->length > 0 && detail->location >= 0);
                            if (result->location + detail->location >= currentStartOffset && result->location + detail->location + detail->length <= currentEndOffset && (!foundGrammar || result->location + detail->location < grammarDetailLocation)) {
                                grammarDetailIndex = j;
                                grammarDetailLocation = result->location + detail->location;
                                foundGrammar = true;
                            }
                        }
                        if (foundGrammar) {
                            grammarPhraseLocation = result->location;
                            outGrammarDetail = result->details[grammarDetailIndex];
                            badGrammarPhrase = paragraphString.substring(result->location, result->length);
                            ASSERT(badGrammarPhrase.length());
                        }
                    }
                }

                if (!misspelledWord.isEmpty() && (!checkGrammar || badGrammarPhrase.isEmpty() || spellingLocation <= grammarDetailLocation)) {
                    int spellingOffset = spellingLocation - currentStartOffset;
                    if (!firstIteration) {
                        RefPtr<Range> paragraphOffsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), searchRange->startPosition(), paragraphRange->startPosition());
                        spellingOffset += TextIterator::rangeLength(paragraphOffsetAsRange.get());
                    }
                    outIsSpelling = true;
                    outFirstFoundOffset = spellingOffset;
                    firstFoundItem = misspelledWord;
                    break;
                }
                if (checkGrammar && !badGrammarPhrase.isEmpty()) {
                    int grammarPhraseOffset = grammarPhraseLocation - currentStartOffset;
                    if (!firstIteration) {
                        RefPtr<Range> paragraphOffsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), searchRange->startPosition(), paragraphRange->startPosition());
                        grammarPhraseOffset += TextIterator::rangeLength(paragraphOffsetAsRange.get());
                    }
                    outIsSpelling = false;
                    outFirstFoundOffset = grammarPhraseOffset;
                    firstFoundItem = badGrammarPhrase;
                    break;
                }
            }
        }
        if (lastIteration || totalLengthProcessed + currentLength >= totalRangeLength)
            break;
        VisiblePosition newParagraphStart = startOfNextParagraph(paragraphRange->endPosition());
        setStart(paragraphRange.get(), newParagraphStart);
        setEnd(paragraphRange.get(), endOfParagraph(newParagraphStart));
        firstIteration = false;
        totalLengthProcessed += currentLength;
    }
    return firstFoundItem;
}

#endif

void Editor::advanceToNextMisspelling(bool startBeforeSelection)
{
    ExceptionCode ec = 0;

    // The basic approach is to search in two phases - from the selection end to the end of the doc, and
    // then we wrap and search from the doc start to (approximately) where we started.
    
    // Start at the end of the selection, search to edge of document.  Starting at the selection end makes
    // repeated "check spelling" commands work.
    VisibleSelection selection(frame()->selection()->selection());
    RefPtr<Range> spellingSearchRange(rangeOfContents(frame()->document()));
    bool startedWithSelection = false;
    if (selection.start().node()) {
        startedWithSelection = true;
        if (startBeforeSelection) {
            VisiblePosition start(selection.visibleStart());
            // We match AppKit's rule: Start 1 character before the selection.
            VisiblePosition oneBeforeStart = start.previous();
            setStart(spellingSearchRange.get(), oneBeforeStart.isNotNull() ? oneBeforeStart : start);
        } else
            setStart(spellingSearchRange.get(), selection.visibleEnd());
    }

    Position position = spellingSearchRange->startPosition();
    if (!isEditablePosition(position)) {
        // This shouldn't happen in very often because the Spelling menu items aren't enabled unless the
        // selection is editable.
        // This can happen in Mail for a mix of non-editable and editable content (like Stationary), 
        // when spell checking the whole document before sending the message.
        // In that case the document might not be editable, but there are editable pockets that need to be spell checked.

        position = firstEditablePositionAfterPositionInRoot(position, frame()->document()->documentElement()).deepEquivalent();
        if (position.isNull())
            return;
        
        Position rangeCompliantPosition = rangeCompliantEquivalent(position);
        spellingSearchRange->setStart(rangeCompliantPosition.node(), rangeCompliantPosition.deprecatedEditingOffset(), ec);
        startedWithSelection = false; // won't need to wrap
    }
    
    // topNode defines the whole range we want to operate on 
    Node* topNode = highestEditableRoot(position);
    // FIXME: lastOffsetForEditing() is wrong here if editingIgnoresContent(highestEditableRoot()) returns true (e.g. a <table>)
    spellingSearchRange->setEnd(topNode, lastOffsetForEditing(topNode), ec);

    // If spellingSearchRange starts in the middle of a word, advance to the next word so we start checking
    // at a word boundary. Going back by one char and then forward by a word does the trick.
    if (startedWithSelection) {
        VisiblePosition oneBeforeStart = startVisiblePosition(spellingSearchRange.get(), DOWNSTREAM).previous();
        if (oneBeforeStart.isNotNull())
            setStart(spellingSearchRange.get(), endOfWord(oneBeforeStart));
        // else we were already at the start of the editable node
    }
    
    if (spellingSearchRange->collapsed(ec))
        return; // nothing to search in
    
    // Get the spell checker if it is available
    if (!client())
        return;
        
    // We go to the end of our first range instead of the start of it, just to be sure
    // we don't get foiled by any word boundary problems at the start.  It means we might
    // do a tiny bit more searching.
    Node* searchEndNodeAfterWrap = spellingSearchRange->endContainer(ec);
    int searchEndOffsetAfterWrap = spellingSearchRange->endOffset(ec);
    
    int misspellingOffset = 0;
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
    RefPtr<Range> grammarSearchRange = spellingSearchRange->cloneRange(ec);
    String misspelledWord;
    String badGrammarPhrase;
    int grammarPhraseOffset = 0;
    bool isSpelling = true;
    int foundOffset = 0;
    GrammarDetail grammarDetail;
    String foundItem = findFirstMisspellingOrBadGrammarInRange(client(), spellingSearchRange.get(), isGrammarCheckingEnabled(), isSpelling, foundOffset, grammarDetail);
    if (isSpelling) {
        misspelledWord = foundItem;
        misspellingOffset = foundOffset;
    } else {
        badGrammarPhrase = foundItem;
        grammarPhraseOffset = foundOffset;
    }
#else
    RefPtr<Range> firstMisspellingRange;
    String misspelledWord = findFirstMisspellingInRange(client(), spellingSearchRange.get(), misspellingOffset, false, firstMisspellingRange);
    String badGrammarPhrase;

#ifndef BUILDING_ON_TIGER
    int grammarPhraseOffset = 0;
    GrammarDetail grammarDetail;

    // Search for bad grammar that occurs prior to the next misspelled word (if any)
    RefPtr<Range> grammarSearchRange = spellingSearchRange->cloneRange(ec);
    if (!misspelledWord.isEmpty()) {
        // Stop looking at start of next misspelled word
        CharacterIterator chars(grammarSearchRange.get());
        chars.advance(misspellingOffset);
        grammarSearchRange->setEnd(chars.range()->startContainer(ec), chars.range()->startOffset(ec), ec);
    }
    
    if (isGrammarCheckingEnabled())
        badGrammarPhrase = findFirstBadGrammarInRange(client(), grammarSearchRange.get(), grammarDetail, grammarPhraseOffset, false);
#endif
#endif
    
    // If we found neither bad grammar nor a misspelled word, wrap and try again (but don't bother if we started at the beginning of the
    // block rather than at a selection).
    if (startedWithSelection && !misspelledWord && !badGrammarPhrase) {
        spellingSearchRange->setStart(topNode, 0, ec);
        // going until the end of the very first chunk we tested is far enough
        spellingSearchRange->setEnd(searchEndNodeAfterWrap, searchEndOffsetAfterWrap, ec);
        
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
        grammarSearchRange = spellingSearchRange->cloneRange(ec);
        foundItem = findFirstMisspellingOrBadGrammarInRange(client(), spellingSearchRange.get(), isGrammarCheckingEnabled(), isSpelling, foundOffset, grammarDetail);
        if (isSpelling) {
            misspelledWord = foundItem;
            misspellingOffset = foundOffset;
        } else {
            badGrammarPhrase = foundItem;
            grammarPhraseOffset = foundOffset;
        }
#else
        misspelledWord = findFirstMisspellingInRange(client(), spellingSearchRange.get(), misspellingOffset, false, firstMisspellingRange);

#ifndef BUILDING_ON_TIGER
        grammarSearchRange = spellingSearchRange->cloneRange(ec);
        if (!misspelledWord.isEmpty()) {
            // Stop looking at start of next misspelled word
            CharacterIterator chars(grammarSearchRange.get());
            chars.advance(misspellingOffset);
            grammarSearchRange->setEnd(chars.range()->startContainer(ec), chars.range()->startOffset(ec), ec);
        }
        if (isGrammarCheckingEnabled())
            badGrammarPhrase = findFirstBadGrammarInRange(client(), grammarSearchRange.get(), grammarDetail, grammarPhraseOffset, false);
#endif
#endif
    }
    
    if (!badGrammarPhrase.isEmpty()) {
#ifdef BUILDING_ON_TIGER
        ASSERT_NOT_REACHED();
#else
        // We found bad grammar. Since we only searched for bad grammar up to the first misspelled word, the bad grammar
        // takes precedence and we ignore any potential misspelled word. Select the grammar detail, update the spelling
        // panel, and store a marker so we draw the green squiggle later.
        
        ASSERT(badGrammarPhrase.length() > 0);
        ASSERT(grammarDetail.location != -1 && grammarDetail.length > 0);
        
        // FIXME 4859190: This gets confused with doubled punctuation at the end of a paragraph
        RefPtr<Range> badGrammarRange = TextIterator::subrange(grammarSearchRange.get(), grammarPhraseOffset + grammarDetail.location, grammarDetail.length);
        frame()->selection()->setSelection(VisibleSelection(badGrammarRange.get(), SEL_DEFAULT_AFFINITY));
        frame()->selection()->revealSelection();
        
        client()->updateSpellingUIWithGrammarString(badGrammarPhrase, grammarDetail);
        frame()->document()->markers()->addMarker(badGrammarRange.get(), DocumentMarker::Grammar, grammarDetail.userDescription);
#endif        
    } else if (!misspelledWord.isEmpty()) {
        // We found a misspelling, but not any earlier bad grammar. Select the misspelling, update the spelling panel, and store
        // a marker so we draw the red squiggle later.
        
        RefPtr<Range> misspellingRange = TextIterator::subrange(spellingSearchRange.get(), misspellingOffset, misspelledWord.length());
        frame()->selection()->setSelection(VisibleSelection(misspellingRange.get(), DOWNSTREAM));
        frame()->selection()->revealSelection();
        
        client()->updateSpellingUIWithMisspelledWord(misspelledWord);
        frame()->document()->markers()->addMarker(misspellingRange.get(), DocumentMarker::Spelling);
    }
}

bool Editor::isSelectionMisspelled()
{
    String selectedString = selectedText();
    int length = selectedString.length();
    if (!length)
        return false;

    if (!client())
        return false;
    
    int misspellingLocation = -1;
    int misspellingLength = 0;
    client()->checkSpellingOfString(selectedString.characters(), length, &misspellingLocation, &misspellingLength);
    
    // The selection only counts as misspelled if the selected text is exactly one misspelled word
    if (misspellingLength != length)
        return false;
    
    // Update the spelling panel to be displaying this error (whether or not the spelling panel is on screen).
    // This is necessary to make a subsequent call to [NSSpellChecker ignoreWord:inSpellDocumentWithTag:] work
    // correctly; that call behaves differently based on whether the spelling panel is displaying a misspelling
    // or a grammar error.
    client()->updateSpellingUIWithMisspelledWord(selectedString);
    
    return true;
}

#ifndef BUILDING_ON_TIGER
static bool isRangeUngrammatical(EditorClient* client, Range *range, Vector<String>& guessesVector)
{
    if (!client)
        return false;

    ExceptionCode ec;
    if (!range || range->collapsed(ec))
        return false;
    
    // Returns true only if the passed range exactly corresponds to a bad grammar detail range. This is analogous
    // to isSelectionMisspelled. It's not good enough for there to be some bad grammar somewhere in the range,
    // or overlapping the range; the ranges must exactly match.
    guessesVector.clear();
    int grammarPhraseOffset;
    
    GrammarDetail grammarDetail;
    String badGrammarPhrase = findFirstBadGrammarInRange(client, range, grammarDetail, grammarPhraseOffset, false);    
    
    // No bad grammar in these parts at all.
    if (badGrammarPhrase.isEmpty())
        return false;
    
    // Bad grammar, but phrase (e.g. sentence) starts beyond start of range.
    if (grammarPhraseOffset > 0)
        return false;
    
    ASSERT(grammarDetail.location >= 0 && grammarDetail.length > 0);
    
    // Bad grammar, but start of detail (e.g. ungrammatical word) doesn't match start of range
    if (grammarDetail.location + grammarPhraseOffset)
        return false;
    
    // Bad grammar at start of range, but end of bad grammar is before or after end of range
    if (grammarDetail.length != TextIterator::rangeLength(range))
        return false;
    
    // Update the spelling panel to be displaying this error (whether or not the spelling panel is on screen).
    // This is necessary to make a subsequent call to [NSSpellChecker ignoreWord:inSpellDocumentWithTag:] work
    // correctly; that call behaves differently based on whether the spelling panel is displaying a misspelling
    // or a grammar error.
    client->updateSpellingUIWithGrammarString(badGrammarPhrase, grammarDetail);
    
    return true;
}
#endif

bool Editor::isSelectionUngrammatical()
{
#ifdef BUILDING_ON_TIGER
    return false;
#else
    Vector<String> ignoredGuesses;
    return isRangeUngrammatical(client(), frame()->selection()->toNormalizedRange().get(), ignoredGuesses);
#endif
}

Vector<String> Editor::guessesForUngrammaticalSelection()
{
#ifdef BUILDING_ON_TIGER
    return Vector<String>();
#else
    Vector<String> guesses;
    // Ignore the result of isRangeUngrammatical; we just want the guesses, whether or not there are any
    isRangeUngrammatical(client(), frame()->selection()->toNormalizedRange().get(), guesses);
    return guesses;
#endif
}

Vector<String> Editor::guessesForMisspelledSelection()
{
    String selectedString = selectedText();
    ASSERT(selectedString.length());

    Vector<String> guesses;
    if (client())
        client()->getGuessesForWord(selectedString, guesses);
    return guesses;
}

#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)

static Vector<String> guessesForMisspelledOrUngrammaticalRange(EditorClient* client, Range *range, bool checkGrammar, bool& misspelled, bool& ungrammatical)
{
    Vector<String> guesses;
    ExceptionCode ec;
    misspelled = false;
    ungrammatical = false;
    
    if (!client || !range || range->collapsed(ec))
        return guesses;

    // Expand the range to encompass entire paragraphs, since text checking needs that much context.
    int rangeStartOffset;
    String paragraphString;
    RefPtr<Range> paragraphRange = paragraphAlignedRangeForRange(range, rangeStartOffset, paragraphString);
    int rangeLength = TextIterator::rangeLength(range);
    if (!rangeLength || !paragraphString.length())
        return guesses;

    Vector<TextCheckingResult> results;
    uint64_t checkingTypes = checkGrammar ? (TextCheckingTypeSpelling | TextCheckingTypeGrammar) : TextCheckingTypeSpelling;
    client->checkTextOfParagraph(paragraphString.characters(), paragraphString.length(), checkingTypes, results);
    
    for (unsigned i = 0; i < results.size(); i++) {
        const TextCheckingResult* result = &results[i];
        if (result->type == TextCheckingTypeSpelling && result->location == rangeStartOffset && result->length == rangeLength) {
            String misspelledWord = paragraphString.substring(rangeStartOffset, rangeLength);
            ASSERT(misspelledWord.length());
            client->getGuessesForWord(misspelledWord, guesses);
            client->updateSpellingUIWithMisspelledWord(misspelledWord);
            misspelled = true;
            return guesses;
        }
    }
    
    if (!checkGrammar)
        return guesses;
        
    for (unsigned i = 0; i < results.size(); i++) {
        const TextCheckingResult* result = &results[i];
        if (result->type == TextCheckingTypeGrammar && result->location <= rangeStartOffset && result->location + result->length >= rangeStartOffset + rangeLength) {
            for (unsigned j = 0; j < result->details.size(); j++) {
                const GrammarDetail* detail = &result->details[j];
                ASSERT(detail->length > 0 && detail->location >= 0);
                if (result->location + detail->location == rangeStartOffset && detail->length == rangeLength) {
                    String badGrammarPhrase = paragraphString.substring(result->location, result->length);
                    ASSERT(badGrammarPhrase.length());
                    for (unsigned k = 0; k < detail->guesses.size(); k++)
                        guesses.append(detail->guesses[k]);
                    client->updateSpellingUIWithGrammarString(badGrammarPhrase, *detail);
                    ungrammatical = true;
                    return guesses;
                }
            }
        }
    }
    return guesses;
}

#endif

Vector<String> Editor::guessesForMisspelledOrUngrammaticalSelection(bool& misspelled, bool& ungrammatical)
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
    return guessesForMisspelledOrUngrammaticalRange(client(), frame()->selection()->toNormalizedRange().get(), isGrammarCheckingEnabled(), misspelled, ungrammatical);
#else
    misspelled = isSelectionMisspelled();
    if (misspelled) {
        ungrammatical = false;
        return guessesForMisspelledSelection();
    }
    if (isGrammarCheckingEnabled() && isSelectionUngrammatical()) {
        ungrammatical = true;
        return guessesForUngrammaticalSelection();
    }
    ungrammatical = false;
    return Vector<String>();
#endif
}

void Editor::showSpellingGuessPanel()
{
    if (!client()) {
        LOG_ERROR("No NSSpellChecker");
        return;
    }

#ifndef BUILDING_ON_TIGER
    // Post-Tiger, this menu item is a show/hide toggle, to match AppKit. Leave Tiger behavior alone
    // to match rest of OS X.
    if (client()->spellingUIIsShowing()) {
        client()->showSpellingUI(false);
        return;
    }
#endif
    
    advanceToNextMisspelling(true);
    client()->showSpellingUI(true);
}

bool Editor::spellingPanelIsShowing()
{
    if (!client())
        return false;
    return client()->spellingUIIsShowing();
}

void Editor::clearMisspellingsAndBadGrammar(const VisibleSelection &movingSelection)
{
    RefPtr<Range> selectedRange = movingSelection.toNormalizedRange();
    if (selectedRange) {
        frame()->document()->markers()->removeMarkers(selectedRange.get(), DocumentMarker::Spelling);
        frame()->document()->markers()->removeMarkers(selectedRange.get(), DocumentMarker::Grammar);
    }
}

void Editor::markMisspellingsAndBadGrammar(const VisibleSelection &movingSelection)
{
    bool markSpelling = isContinuousSpellCheckingEnabled();
    bool markGrammar = markSpelling && isGrammarCheckingEnabled();

    if (markSpelling) {
        RefPtr<Range> unusedFirstMisspellingRange;
        markMisspellings(movingSelection, unusedFirstMisspellingRange);
    }

    if (markGrammar)
        markBadGrammar(movingSelection);
}

void Editor::markMisspellingsAfterTypingToPosition(const VisiblePosition &p)
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
    TextCheckingOptions textCheckingOptions = 0;
    if (isContinuousSpellCheckingEnabled())
        textCheckingOptions |= MarkSpelling;

    if (isAutomaticQuoteSubstitutionEnabled()
        || isAutomaticLinkDetectionEnabled()
        || isAutomaticDashSubstitutionEnabled()
        || isAutomaticTextReplacementEnabled()
        || ((textCheckingOptions & MarkSpelling) && isAutomaticSpellingCorrectionEnabled()))
        textCheckingOptions |= PerformReplacement;

    if (!textCheckingOptions & (MarkSpelling | PerformReplacement))
        return;

    if (isGrammarCheckingEnabled())
        textCheckingOptions |= MarkGrammar;

    VisibleSelection adjacentWords = VisibleSelection(startOfWord(p, LeftWordIfOnBoundary), endOfWord(p, RightWordIfOnBoundary));
    if (textCheckingOptions & MarkGrammar) {
        VisibleSelection selectedSentence = VisibleSelection(startOfSentence(p), endOfSentence(p));
        markAllMisspellingsAndBadGrammarInRanges(textCheckingOptions, adjacentWords.toNormalizedRange().get(), selectedSentence.toNormalizedRange().get());
    } else {
        markAllMisspellingsAndBadGrammarInRanges(textCheckingOptions, adjacentWords.toNormalizedRange().get(), adjacentWords.toNormalizedRange().get());
    }
#else
    if (!isContinuousSpellCheckingEnabled())
        return;
    
    // Check spelling of one word
    RefPtr<Range> misspellingRange;
    markMisspellings(VisibleSelection(startOfWord(p, LeftWordIfOnBoundary), endOfWord(p, RightWordIfOnBoundary)), misspellingRange);

    // Autocorrect the misspelled word.
    if (!misspellingRange)
        return;
    
    // Get the misspelled word.
    const String misspelledWord = plainText(misspellingRange.get());
    String autocorrectedString = client()->getAutoCorrectSuggestionForMisspelledWord(misspelledWord);

    // If autocorrected word is non empty, replace the misspelled word by this word.
    if (!autocorrectedString.isEmpty()) {
        VisibleSelection newSelection(misspellingRange.get(), DOWNSTREAM);
        if (newSelection != frame()->selection()->selection()) {
            if (!frame()->selection()->shouldChangeSelection(newSelection))
                return;
            frame()->selection()->setSelection(newSelection);
        }

        if (!frame()->editor()->shouldInsertText(autocorrectedString, misspellingRange.get(), EditorInsertActionTyped))
            return;
        frame()->editor()->replaceSelectionWithText(autocorrectedString, false, false);

        // Reset the charet one character further.
        frame()->selection()->moveTo(frame()->selection()->end());
        frame()->selection()->modify(SelectionController::AlterationMove, SelectionController::DirectionForward, CharacterGranularity);
    }

    if (!isGrammarCheckingEnabled())
        return;
    
    // Check grammar of entire sentence
    markBadGrammar(VisibleSelection(startOfSentence(p), endOfSentence(p)));
#endif
}

static void markAllMisspellingsInRange(EditorClient* client, Range* searchRange, RefPtr<Range>& firstMisspellingRange)
{
    // Use the "markAll" feature of findFirstMisspellingInRange. Ignore the return value and the "out parameter";
    // all we need to do is mark every instance.
    int ignoredOffset;
    findFirstMisspellingInRange(client, searchRange, ignoredOffset, true, firstMisspellingRange);
}

#ifndef BUILDING_ON_TIGER
static void markAllBadGrammarInRange(EditorClient* client, Range* searchRange)
{
    // Use the "markAll" feature of findFirstBadGrammarInRange. Ignore the return value and "out parameters"; all we need to
    // do is mark every instance.
    GrammarDetail ignoredGrammarDetail;
    int ignoredOffset;
    findFirstBadGrammarInRange(client, searchRange, ignoredGrammarDetail, ignoredOffset, true);
}
#endif
    
static void markMisspellingsOrBadGrammar(Editor* editor, const VisibleSelection& selection, bool checkSpelling, RefPtr<Range>& firstMisspellingRange)
{
    // This function is called with a selection already expanded to word boundaries.
    // Might be nice to assert that here.
    
    // This function is used only for as-you-type checking, so if that's off we do nothing. Note that
    // grammar checking can only be on if spell checking is also on.
    if (!editor->isContinuousSpellCheckingEnabled())
        return;
    
    RefPtr<Range> searchRange(selection.toNormalizedRange());
    if (!searchRange)
        return;
    
    // If we're not in an editable node, bail.
    Node* editableNode = searchRange->startContainer();
    if (!editableNode || !editableNode->isContentEditable())
        return;

    if (!editor->spellCheckingEnabledInFocusedNode())
        return;

    // Get the spell checker if it is available
    if (!editor->client())
        return;
    
    if (checkSpelling)
        markAllMisspellingsInRange(editor->client(), searchRange.get(), firstMisspellingRange);
    else {
#ifdef BUILDING_ON_TIGER
        ASSERT_NOT_REACHED();
#else
        if (editor->isGrammarCheckingEnabled())
            markAllBadGrammarInRange(editor->client(), searchRange.get());
#endif
    }    
}

bool Editor::spellCheckingEnabledInFocusedNode() const
{
    // Ascend the DOM tree to find a "spellcheck" attribute.
    // When we find a "spellcheck" attribute, retrieve its value and return false if its value is "false".
    const Node* node = frame()->document()->focusedNode();
    while (node) {
        if (node->isElementNode()) {
            const WTF::AtomicString& value = static_cast<const Element*>(node)->getAttribute(spellcheckAttr);
            if (equalIgnoringCase(value, "true"))
                return true;
            if (equalIgnoringCase(value, "false"))
                return false;
        }
        node = node->parent();
    }
    return true;
}

void Editor::markMisspellings(const VisibleSelection& selection, RefPtr<Range>& firstMisspellingRange)
{
    markMisspellingsOrBadGrammar(this, selection, true, firstMisspellingRange);
}
    
void Editor::markBadGrammar(const VisibleSelection& selection)
{
#ifndef BUILDING_ON_TIGER
    RefPtr<Range> firstMisspellingRange;
    markMisspellingsOrBadGrammar(this, selection, false, firstMisspellingRange);
#else
    UNUSED_PARAM(selection);
#endif
}

#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)

static inline bool isAmbiguousBoundaryCharacter(UChar character)
{
    // These are characters that can behave as word boundaries, but can appear within words.
    // If they are just typed, i.e. if they are immediately followed by a caret, we want to delay text checking until the next character has been typed.
    // FIXME: this is required until 6853027 is fixed and text checking can do this for us.
    return character == '\'' || character == rightSingleQuotationMark || character == hebrewPunctuationGershayim;
}

void Editor::markAllMisspellingsAndBadGrammarInRanges(TextCheckingOptions textCheckingOptions, Range* spellingRange, Range* grammarRange)
{
    bool shouldMarkSpelling = textCheckingOptions & MarkSpelling;
    bool shouldMarkGrammar = textCheckingOptions & MarkGrammar;
    bool shouldPerformReplacement = textCheckingOptions & PerformReplacement;
    bool shouldShowCorrectionPanel = textCheckingOptions & ShowCorrectionPanel;

    // This function is called with selections already expanded to word boundaries.
    ExceptionCode ec = 0;
    if (!client() || !spellingRange || (shouldMarkGrammar && !grammarRange))
        return;

    // If we're not in an editable node, bail.
    Node* editableNode = spellingRange->startContainer();
    if (!editableNode || !editableNode->isContentEditable())
        return;

    if (!spellCheckingEnabledInFocusedNode())
        return;

    // Expand the range to encompass entire paragraphs, since text checking needs that much context.
    int spellingRangeStartOffset = 0;
    int spellingRangeEndOffset = 0;
    int grammarRangeStartOffset = 0;
    int grammarRangeEndOffset = 0;
    int offsetDueToReplacement = 0;
    int paragraphLength = 0;
    int selectionOffset = 0;
    int ambiguousBoundaryOffset = -1;
    bool selectionChanged = false;
    bool restoreSelectionAfterChange = false;
    bool adjustSelectionForParagraphBoundaries = false;
    String paragraphString;
    RefPtr<Range> paragraphRange;

    if (shouldMarkGrammar) {
        // The spelling range should be contained in the paragraph-aligned extension of the grammar range.
        paragraphRange = paragraphAlignedRangeForRange(grammarRange, grammarRangeStartOffset, paragraphString);
        RefPtr<Range> offsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), paragraphRange->startPosition(), spellingRange->startPosition());
        spellingRangeStartOffset = TextIterator::rangeLength(offsetAsRange.get());
        grammarRangeEndOffset = grammarRangeStartOffset + TextIterator::rangeLength(grammarRange);
    } else {
        paragraphRange = paragraphAlignedRangeForRange(spellingRange, spellingRangeStartOffset, paragraphString);
    }
    spellingRangeEndOffset = spellingRangeStartOffset + TextIterator::rangeLength(spellingRange);
    paragraphLength = paragraphString.length();
    if (paragraphLength <= 0 || (spellingRangeStartOffset >= spellingRangeEndOffset && (!shouldMarkGrammar || grammarRangeStartOffset >= grammarRangeEndOffset)))
        return;

    if (shouldPerformReplacement) {
        if (m_frame->selection()->selectionType() == VisibleSelection::CaretSelection) {
            // Attempt to save the caret position so we can restore it later if needed
            RefPtr<Range> offsetAsRange = Range::create(paragraphRange->startContainer(ec)->document(), paragraphRange->startPosition(), paragraphRange->startPosition());
            Position caretPosition = m_frame->selection()->end();
            offsetAsRange->setEnd(caretPosition.containerNode(), caretPosition.computeOffsetInContainerNode(), ec);
            if (!ec) {
                selectionOffset = TextIterator::rangeLength(offsetAsRange.get());
                restoreSelectionAfterChange = true;
                if (selectionOffset > 0 && (selectionOffset > paragraphLength || paragraphString[selectionOffset - 1] == newlineCharacter))
                    adjustSelectionForParagraphBoundaries = true;
                if (selectionOffset > 0 && selectionOffset <= paragraphLength && isAmbiguousBoundaryCharacter(paragraphString[selectionOffset - 1]))
                    ambiguousBoundaryOffset = selectionOffset - 1;
            }
        }
    }

    Vector<TextCheckingResult> results;
    uint64_t checkingTypes = 0;
    if (shouldMarkSpelling)
        checkingTypes |= TextCheckingTypeSpelling;
    if (shouldMarkGrammar)
        checkingTypes |= TextCheckingTypeGrammar;
    if (shouldShowCorrectionPanel)
        checkingTypes |= TextCheckingTypeCorrection;
    if (shouldPerformReplacement) {
        if (isAutomaticLinkDetectionEnabled())
            checkingTypes |= TextCheckingTypeLink;
        if (isAutomaticQuoteSubstitutionEnabled())
            checkingTypes |= TextCheckingTypeQuote;
        if (isAutomaticDashSubstitutionEnabled())
            checkingTypes |= TextCheckingTypeDash;
        if (isAutomaticTextReplacementEnabled())
            checkingTypes |= TextCheckingTypeReplacement;
        if (shouldMarkSpelling && isAutomaticSpellingCorrectionEnabled())
            checkingTypes |= TextCheckingTypeCorrection;
    }
    client()->checkTextOfParagraph(paragraphString.characters(), paragraphLength, checkingTypes, results);

#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
    // If this checking is only for showing correction panel, we shouldn't bother to mark misspellings.
    if (shouldShowCorrectionPanel)
        shouldMarkSpelling = false;
#endif

    for (unsigned i = 0; i < results.size(); i++) {
        const TextCheckingResult* result = &results[i];
        int resultLocation = result->location + offsetDueToReplacement;
        int resultLength = result->length;
        if (shouldMarkSpelling && result->type == TextCheckingTypeSpelling && resultLocation >= spellingRangeStartOffset && resultLocation + resultLength <= spellingRangeEndOffset) {
            ASSERT(resultLength > 0 && resultLocation >= 0);
            RefPtr<Range> misspellingRange = TextIterator::subrange(spellingRange, resultLocation - spellingRangeStartOffset, resultLength);
            misspellingRange->startContainer(ec)->document()->markers()->addMarker(misspellingRange.get(), DocumentMarker::Spelling);
        } else if (shouldMarkGrammar && result->type == TextCheckingTypeGrammar && resultLocation < grammarRangeEndOffset && resultLocation + resultLength > grammarRangeStartOffset) {
            ASSERT(resultLength > 0 && resultLocation >= 0);
            for (unsigned j = 0; j < result->details.size(); j++) {
                const GrammarDetail* detail = &result->details[j];
                ASSERT(detail->length > 0 && detail->location >= 0);
                if (resultLocation + detail->location >= grammarRangeStartOffset && resultLocation + detail->location + detail->length <= grammarRangeEndOffset) {
                    RefPtr<Range> badGrammarRange = TextIterator::subrange(grammarRange, resultLocation + detail->location - grammarRangeStartOffset, detail->length);
                    grammarRange->startContainer(ec)->document()->markers()->addMarker(badGrammarRange.get(), DocumentMarker::Grammar, detail->userDescription);
                }
            }
        } else if ((shouldPerformReplacement || shouldShowCorrectionPanel) && resultLocation + resultLength <= spellingRangeEndOffset && resultLocation + resultLength >= spellingRangeStartOffset
                    && (result->type == TextCheckingTypeLink
                    || result->type == TextCheckingTypeQuote
                    || result->type == TextCheckingTypeDash
                    || result->type == TextCheckingTypeReplacement
                    || result->type == TextCheckingTypeCorrection)) {
            // In this case the result range just has to touch the spelling range, so we can handle replacing non-word text such as punctuation.
            ASSERT(resultLength > 0 && resultLocation >= 0);
            int replacementLength = result->replacement.length();
            bool doReplacement = (replacementLength > 0);
            RefPtr<Range> rangeToReplace = TextIterator::subrange(paragraphRange.get(), resultLocation, resultLength);
            VisibleSelection selectionToReplace(rangeToReplace.get(), DOWNSTREAM);
        
            // avoid correcting text after an ambiguous boundary character has been typed
            // FIXME: this is required until 6853027 is fixed and text checking can do this for us
            if (ambiguousBoundaryOffset >= 0 && resultLocation + resultLength == ambiguousBoundaryOffset)
                doReplacement = false;

            // adding links should be done only immediately after they are typed
            if (result->type == TextCheckingTypeLink && selectionOffset > resultLocation + resultLength + 1)
                doReplacement = false;

            // Don't correct spelling in an already-corrected word.
            if (doReplacement && result->type == TextCheckingTypeCorrection) {
                Node* node = rangeToReplace->startContainer();
                int startOffset = rangeToReplace->startOffset();
                int endOffset = startOffset + replacementLength;
                Vector<DocumentMarker> markers = node->document()->markers()->markersForNode(node);
                size_t markerCount = markers.size();
                for (size_t i = 0; i < markerCount; ++i) {
                    const DocumentMarker& marker = markers[i];
                    if ((marker.type == DocumentMarker::Replacement || marker.type == DocumentMarker::RejectedCorrection) && static_cast<int>(marker.startOffset) < endOffset && static_cast<int>(marker.endOffset) > startOffset) {
                        doReplacement = false;
                        break;
                    }
                    if (static_cast<int>(marker.startOffset) >= endOffset)
                        break;
                }
            }
            if (doReplacement && !shouldShowCorrectionPanel && selectionToReplace != m_frame->selection()->selection()) {
                if (m_frame->selection()->shouldChangeSelection(selectionToReplace)) {
                    m_frame->selection()->setSelection(selectionToReplace);
                    selectionChanged = true;
                } else {
                    doReplacement = false;
                }
            }

            String replacedString;
            if (doReplacement) {
                if (result->type == TextCheckingTypeLink) {
                    restoreSelectionAfterChange = false;
                    if (canEditRichly())
                        applyCommand(CreateLinkCommand::create(m_frame->document(), result->replacement));
                } else if (canEdit() && shouldInsertText(result->replacement, rangeToReplace.get(), EditorInsertActionTyped)) {
                    if (result->type == TextCheckingTypeCorrection)
                        replacedString = plainText(rangeToReplace.get());
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
                    if (shouldShowCorrectionPanel && resultLocation + resultLength == spellingRangeEndOffset && result->type == TextCheckingTypeCorrection) {
                        // We only show the correction panel on the last word.
                        Vector<FloatQuad> textQuads;
                        rangeToReplace->textQuads(textQuads);
                        Vector<FloatQuad>::const_iterator end = textQuads.end();
                        FloatRect totalBoundingBox;
                        for (Vector<FloatQuad>::const_iterator it = textQuads.begin(); it < end; ++it)
                            totalBoundingBox.unite(it->boundingBox());
                        m_rangeToBeReplacedByCorrection = rangeToReplace;
                        m_stringToBeReplacedByCorrection = replacedString;
                        client()->showCorrectionPanel(totalBoundingBox, m_stringToBeReplacedByCorrection, result->replacement, this);
                        doReplacement = false;
                    }
#endif
                    if (doReplacement) {
                        replaceSelectionWithText(result->replacement, false, false);
                        spellingRangeEndOffset += replacementLength - resultLength;
                        offsetDueToReplacement += replacementLength - resultLength;
                        if (resultLocation < selectionOffset)
                            selectionOffset += replacementLength - resultLength;
                        if (result->type == TextCheckingTypeCorrection) {
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
                            if (client())
                                client()->dismissCorrectionPanel(true);
#endif
                            // Add a marker so that corrections can easily be undone and won't be re-corrected.
                            RefPtr<Range> replacedRange = TextIterator::subrange(paragraphRange.get(), resultLocation, replacementLength);
                            replacedRange->startContainer()->document()->markers()->addMarker(replacedRange.get(), DocumentMarker::Replacement, replacedString);
                        }
                    }
                }
            }
        }
    }

    if (selectionChanged) {
        // Restore the caret position if we have made any replacements
        setEnd(paragraphRange.get(), endOfParagraph(startOfNextParagraph(paragraphRange->startPosition())));
        int newLength = TextIterator::rangeLength(paragraphRange.get());
        if (restoreSelectionAfterChange && selectionOffset >= 0 && selectionOffset <= newLength) {
            RefPtr<Range> selectionRange = TextIterator::subrange(paragraphRange.get(), 0, selectionOffset);
            m_frame->selection()->moveTo(selectionRange->endPosition(), DOWNSTREAM);
            if (adjustSelectionForParagraphBoundaries)
                m_frame->selection()->modify(SelectionController::AlterationMove, SelectionController::DirectionForward, CharacterGranularity);
        } else {
            // If this fails for any reason, the fallback is to go one position beyond the last replacement
            m_frame->selection()->moveTo(m_frame->selection()->end());
            m_frame->selection()->modify(SelectionController::AlterationMove, SelectionController::DirectionForward, CharacterGranularity);
        }
    }
}

void Editor::changeBackToReplacedString(const String& replacedString)
{
    if (replacedString.isEmpty())
        return;

    RefPtr<Range> selection = selectedRange();
    if (!shouldInsertText(replacedString, selection.get(), EditorInsertActionPasted))
        return;
        
    String paragraphString;
    int selectionOffset;
    RefPtr<Range> paragraphRange = paragraphAlignedRangeForRange(selection.get(), selectionOffset, paragraphString);
    replaceSelectionWithText(replacedString, false, false);
    RefPtr<Range> changedRange = TextIterator::subrange(paragraphRange.get(), selectionOffset, replacedString.length());
    changedRange->startContainer()->document()->markers()->addMarker(changedRange.get(), DocumentMarker::Replacement, String());
}

#endif

void Editor::markMisspellingsAndBadGrammar(const VisibleSelection& spellingSelection, bool markGrammar, const VisibleSelection& grammarSelection)
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
    if (!isContinuousSpellCheckingEnabled())
        return;
    TextCheckingOptions textCheckingOptions = MarkSpelling;
    if (markGrammar && isGrammarCheckingEnabled())
        textCheckingOptions |= MarkGrammar;
    markAllMisspellingsAndBadGrammarInRanges(textCheckingOptions, spellingSelection.toNormalizedRange().get(), grammarSelection.toNormalizedRange().get());
#else
    RefPtr<Range> firstMisspellingRange;
    markMisspellings(spellingSelection, firstMisspellingRange);
    if (markGrammar)
        markBadGrammar(grammarSelection);
#endif
}

void Editor::correctionPanelTimerFired(Timer<Editor>*)
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
    VisibleSelection selection(frame()->selection()->selection());
    VisiblePosition start(selection.start(), selection.affinity());
    VisiblePosition p = startOfWord(start, LeftWordIfOnBoundary);
    VisibleSelection adjacentWords = VisibleSelection(p, start);
    markAllMisspellingsAndBadGrammarInRanges(MarkSpelling | ShowCorrectionPanel, adjacentWords.toNormalizedRange().get(), 0);
#endif
}

void Editor::handleRejectedCorrection()
{
    Range* replacedRange = m_rangeToBeReplacedByCorrection.get();
    if (!replacedRange || m_frame->document() != replacedRange->ownerDocument())
        return;

    replacedRange->startContainer()->document()->markers()->addMarker(replacedRange, DocumentMarker::RejectedCorrection, m_stringToBeReplacedByCorrection);
}

void Editor::startCorrectionPanelTimer()
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
    static const double correctionPanelTimerInterval = 0.3;
    if (client())
        client()->dismissCorrectionPanel(true);
    if (isAutomaticSpellingCorrectionEnabled())
        m_correctionPanelTimer.startOneShot(correctionPanelTimerInterval);
#endif
}

void Editor::handleCancelOperation()
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
    if (client())
        client()->dismissCorrectionPanel(false);
#endif
}

PassRefPtr<Range> Editor::rangeForPoint(const IntPoint& windowPoint)
{
    Document* document = m_frame->documentAtPoint(windowPoint);
    if (!document)
        return 0;
    
    Frame* frame = document->frame();
    ASSERT(frame);
    FrameView* frameView = frame->view();
    if (!frameView)
        return 0;
    IntPoint framePoint = frameView->windowToContents(windowPoint);
    VisibleSelection selection(frame->visiblePositionForPoint(framePoint));
    return avoidIntersectionWithNode(selection.toNormalizedRange().get(), m_deleteButtonController->containerElement());
}

void Editor::revealSelectionAfterEditingOperation()
{
    if (m_ignoreCompositionSelectionChange)
        return;

    m_frame->selection()->revealSelection(ScrollAlignment::alignToEdgeIfNeeded);
}

void Editor::setIgnoreCompositionSelectionChange(bool ignore)
{
    if (m_ignoreCompositionSelectionChange == ignore)
        return;

    m_ignoreCompositionSelectionChange = ignore;
    if (!ignore)
        revealSelectionAfterEditingOperation();
}

PassRefPtr<Range> Editor::compositionRange() const
{
    if (!m_compositionNode)
        return 0;
    unsigned length = m_compositionNode->length();
    unsigned start = min(m_compositionStart, length);
    unsigned end = min(max(start, m_compositionEnd), length);
    if (start >= end)
        return 0;
    return Range::create(m_compositionNode->document(), m_compositionNode.get(), start, m_compositionNode.get(), end);
}

bool Editor::getCompositionSelection(unsigned& selectionStart, unsigned& selectionEnd) const
{
    if (!m_compositionNode)
        return false;
    Position start = m_frame->selection()->start();
    if (start.node() != m_compositionNode)
        return false;
    Position end = m_frame->selection()->end();
    if (end.node() != m_compositionNode)
        return false;

    if (static_cast<unsigned>(start.deprecatedEditingOffset()) < m_compositionStart)
        return false;
    if (static_cast<unsigned>(end.deprecatedEditingOffset()) > m_compositionEnd)
        return false;

    selectionStart = start.deprecatedEditingOffset() - m_compositionStart;
    selectionEnd = start.deprecatedEditingOffset() - m_compositionEnd;
    return true;
}

void Editor::transpose()
{
    if (!canEdit())
        return;

     VisibleSelection selection = m_frame->selection()->selection();
     if (!selection.isCaret())
         return;

    // Make a selection that goes back one character and forward two characters.
    VisiblePosition caret = selection.visibleStart();
    VisiblePosition next = isEndOfParagraph(caret) ? caret : caret.next();
    VisiblePosition previous = next.previous();
    if (next == previous)
        return;
    previous = previous.previous();
    if (!inSameParagraph(next, previous))
        return;
    RefPtr<Range> range = makeRange(previous, next);
    if (!range)
        return;
    VisibleSelection newSelection(range.get(), DOWNSTREAM);

    // Transpose the two characters.
    String text = plainText(range.get());
    if (text.length() != 2)
        return;
    String transposed = text.right(1) + text.left(1);

    // Select the two characters.
    if (newSelection != m_frame->selection()->selection()) {
        if (!m_frame->selection()->shouldChangeSelection(newSelection))
            return;
        m_frame->selection()->setSelection(newSelection);
    }

    // Insert the transposed characters.
    if (!shouldInsertText(transposed, range.get(), EditorInsertActionTyped))
        return;
    replaceSelectionWithText(transposed, false, false);
}

void Editor::addToKillRing(Range* range, bool prepend)
{
    if (m_shouldStartNewKillRingSequence)
        killRing()->startNewSequence();

    String text = plainText(range);
    if (prepend)
        killRing()->prepend(text);
    else
        killRing()->append(text);
    m_shouldStartNewKillRingSequence = false;
}

bool Editor::insideVisibleArea(const IntPoint& point) const
{
    if (m_frame->excludeFromTextSearch())
        return false;
    
    // Right now, we only check the visibility of a point for disconnected frames. For all other
    // frames, we assume visibility.
    Frame* frame = m_frame->isDisconnected() ? m_frame : m_frame->tree()->top(true);
    if (!frame->isDisconnected())
        return true;
    
    RenderPart* renderer = frame->ownerRenderer();
    if (!renderer)
        return false;

    RenderBlock* container = renderer->containingBlock();
    if (!(container->style()->overflowX() == OHIDDEN || container->style()->overflowY() == OHIDDEN))
        return true;

    IntRect rectInPageCoords = container->overflowClipRect(0, 0);
    IntRect rectInFrameCoords = IntRect(renderer->x() * -1, renderer->y() * -1,
                                    rectInPageCoords.width(), rectInPageCoords.height());

    return rectInFrameCoords.contains(point);
}

bool Editor::insideVisibleArea(Range* range) const
{
    if (!range)
        return true;

    if (m_frame->excludeFromTextSearch())
        return false;
    
    // Right now, we only check the visibility of a range for disconnected frames. For all other
    // frames, we assume visibility.
    Frame* frame = m_frame->isDisconnected() ? m_frame : m_frame->tree()->top(true);
    if (!frame->isDisconnected())
        return true;
    
    RenderPart* renderer = frame->ownerRenderer();
    if (!renderer)
        return false;

    RenderBlock* container = renderer->containingBlock();
    if (!(container->style()->overflowX() == OHIDDEN || container->style()->overflowY() == OHIDDEN))
        return true;

    IntRect rectInPageCoords = container->overflowClipRect(0, 0);
    IntRect rectInFrameCoords = IntRect(renderer->x() * -1, renderer->y() * -1,
                                    rectInPageCoords.width(), rectInPageCoords.height());
    IntRect resultRect = range->boundingBox();
    
    return rectInFrameCoords.contains(resultRect);
}

PassRefPtr<Range> Editor::firstVisibleRange(const String& target, bool caseFlag)
{
    RefPtr<Range> searchRange(rangeOfContents(m_frame->document()));
    RefPtr<Range> resultRange = findPlainText(searchRange.get(), target, true, caseFlag);
    ExceptionCode ec = 0;

    while (!insideVisibleArea(resultRange.get())) {
        searchRange->setStartAfter(resultRange->endContainer(), ec);
        if (searchRange->startContainer() == searchRange->endContainer())
            return Range::create(m_frame->document());
        resultRange = findPlainText(searchRange.get(), target, true, caseFlag);
    }
    
    return resultRange;
}

PassRefPtr<Range> Editor::lastVisibleRange(const String& target, bool caseFlag)
{
    RefPtr<Range> searchRange(rangeOfContents(m_frame->document()));
    RefPtr<Range> resultRange = findPlainText(searchRange.get(), target, false, caseFlag);
    ExceptionCode ec = 0;

    while (!insideVisibleArea(resultRange.get())) {
        searchRange->setEndBefore(resultRange->startContainer(), ec);
        if (searchRange->startContainer() == searchRange->endContainer())
            return Range::create(m_frame->document());
        resultRange = findPlainText(searchRange.get(), target, false, caseFlag);
    }
    
    return resultRange;
}

PassRefPtr<Range> Editor::nextVisibleRange(Range* currentRange, const String& target, bool forward, bool caseFlag, bool wrapFlag)
{
    if (m_frame->excludeFromTextSearch())
        return Range::create(m_frame->document());

    RefPtr<Range> resultRange = currentRange;
    RefPtr<Range> searchRange(rangeOfContents(m_frame->document()));
    ExceptionCode ec = 0;
    
    for ( ; !insideVisibleArea(resultRange.get()); resultRange = findPlainText(searchRange.get(), target, forward, caseFlag)) {
        if (resultRange->collapsed(ec)) {
            if (!resultRange->startContainer()->isInShadowTree())
                break;
            searchRange = rangeOfContents(m_frame->document());
            if (forward)
                searchRange->setStartAfter(resultRange->startContainer()->shadowAncestorNode(), ec);
            else
                searchRange->setEndBefore(resultRange->startContainer()->shadowAncestorNode(), ec);
            continue;
        }

        if (forward)
            searchRange->setStartAfter(resultRange->endContainer(), ec);
        else
            searchRange->setEndBefore(resultRange->startContainer(), ec);

        Node* shadowTreeRoot = searchRange->shadowTreeRootNode();
        if (searchRange->collapsed(ec) && shadowTreeRoot) {
            if (forward)
                searchRange->setEnd(shadowTreeRoot, shadowTreeRoot->childNodeCount(), ec);
            else
                searchRange->setStartBefore(shadowTreeRoot, ec);
        }
        
        if (searchRange->startContainer()->isDocumentNode() && searchRange->endContainer()->isDocumentNode())
            break;
    }
    
    if (insideVisibleArea(resultRange.get()))
        return resultRange;
    
    if (!wrapFlag)
        return Range::create(m_frame->document());

    if (forward)
        return firstVisibleRange(target, caseFlag);

    return lastVisibleRange(target, caseFlag);
}

void Editor::changeSelectionAfterCommand(const VisibleSelection& newSelection, bool closeTyping, bool clearTypingStyle)
{
    // If the new selection is orphaned, then don't update the selection.
    if (newSelection.start().isOrphan() || newSelection.end().isOrphan())
        return;

    // If there is no selection change, don't bother sending shouldChangeSelection, but still call setSelection,
    // because there is work that it must do in this situation.
    // The old selection can be invalid here and calling shouldChangeSelection can produce some strange calls.
    // See <rdar://problem/5729315> Some shouldChangeSelectedDOMRange contain Ranges for selections that are no longer valid
    bool selectionDidNotChangeDOMPosition = newSelection == m_frame->selection()->selection();
    if (selectionDidNotChangeDOMPosition || m_frame->selection()->shouldChangeSelection(newSelection))
        m_frame->selection()->setSelection(newSelection, closeTyping, clearTypingStyle);
        
    // Some editing operations change the selection visually without affecting its position within the DOM.
    // For example when you press return in the following (the caret is marked by ^): 
    // <div contentEditable="true"><div>^Hello</div></div>
    // WebCore inserts <div><br></div> *before* the current block, which correctly moves the paragraph down but which doesn't
    // change the caret's DOM position (["hello", 0]).  In these situations the above SelectionController::setSelection call
    // does not call EditorClient::respondToChangedSelection(), which, on the Mac, sends selection change notifications and 
    // starts a new kill ring sequence, but we want to do these things (matches AppKit).
    if (selectionDidNotChangeDOMPosition)
        client()->respondToChangedSelection();
}

String Editor::selectedText() const
{
    return plainText(m_frame->selection()->toNormalizedRange().get());
}

IntRect Editor::firstRectForRange(Range* range) const
{
    int extraWidthToEndOfLine = 0;
    ASSERT(range->startContainer());
    ASSERT(range->endContainer());

    InlineBox* startInlineBox;
    int startCaretOffset;
    Position startPosition = VisiblePosition(range->startPosition()).deepEquivalent();
    if (startPosition.isNull())
        return IntRect();
    startPosition.getInlineBoxAndOffset(DOWNSTREAM, startInlineBox, startCaretOffset);

    RenderObject* startRenderer = startPosition.node()->renderer();
    ASSERT(startRenderer);
    IntRect startCaretRect = startRenderer->localCaretRect(startInlineBox, startCaretOffset, &extraWidthToEndOfLine);
    if (startCaretRect != IntRect())
        startCaretRect = startRenderer->localToAbsoluteQuad(FloatRect(startCaretRect)).enclosingBoundingBox();

    InlineBox* endInlineBox;
    int endCaretOffset;
    Position endPosition = VisiblePosition(range->endPosition()).deepEquivalent();
    if (endPosition.isNull())
        return IntRect();
    endPosition.getInlineBoxAndOffset(UPSTREAM, endInlineBox, endCaretOffset);

    RenderObject* endRenderer = endPosition.node()->renderer();
    ASSERT(endRenderer);
    IntRect endCaretRect = endRenderer->localCaretRect(endInlineBox, endCaretOffset);
    if (endCaretRect != IntRect())
        endCaretRect = endRenderer->localToAbsoluteQuad(FloatRect(endCaretRect)).enclosingBoundingBox();

    if (startCaretRect.y() == endCaretRect.y()) {
        // start and end are on the same line
        return IntRect(min(startCaretRect.x(), endCaretRect.x()),
            startCaretRect.y(),
            abs(endCaretRect.x() - startCaretRect.x()),
            max(startCaretRect.height(), endCaretRect.height()));
    }

    // start and end aren't on the same line, so go from start to the end of its line
    return IntRect(startCaretRect.x(),
        startCaretRect.y(),
        startCaretRect.width() + extraWidthToEndOfLine,
        startCaretRect.height());
}

bool Editor::shouldChangeSelection(const VisibleSelection& oldSelection, const VisibleSelection& newSelection, EAffinity affinity, bool stillSelecting) const
{
    return client()->shouldChangeSelectedRange(oldSelection.toNormalizedRange().get(), newSelection.toNormalizedRange().get(), affinity, stillSelecting);
}

void Editor::computeAndSetTypingStyle(CSSStyleDeclaration* style, EditAction editingAction)
{
    if (!style || !style->length()) {
        m_frame->selection()->clearTypingStyle();
        return;
    }

    // Calculate the current typing style.
    RefPtr<CSSMutableStyleDeclaration> mutableStyle = style->makeMutable();
    if (m_frame->selection()->typingStyle()) {
        m_frame->selection()->typingStyle()->merge(mutableStyle.get());
        mutableStyle = m_frame->selection()->typingStyle();
    }

    RefPtr<CSSValue> unicodeBidi;
    RefPtr<CSSValue> direction;
    if (editingAction == EditActionSetWritingDirection) {
        unicodeBidi = mutableStyle->getPropertyCSSValue(CSSPropertyUnicodeBidi);
        direction = mutableStyle->getPropertyCSSValue(CSSPropertyDirection);
    }

    Node* node = m_frame->selection()->selection().visibleStart().deepEquivalent().node();
    computedStyle(node)->diff(mutableStyle.get());

    if (editingAction == EditActionSetWritingDirection && unicodeBidi) {
        ASSERT(unicodeBidi->isPrimitiveValue());
        mutableStyle->setProperty(CSSPropertyUnicodeBidi, static_cast<CSSPrimitiveValue*>(unicodeBidi.get())->getIdent());
        if (direction) {
            ASSERT(direction->isPrimitiveValue());
            mutableStyle->setProperty(CSSPropertyDirection, static_cast<CSSPrimitiveValue*>(direction.get())->getIdent());
        }
    }

    // Handle block styles, substracting these from the typing style.
    RefPtr<CSSMutableStyleDeclaration> blockStyle = mutableStyle->copyBlockProperties();
    blockStyle->diff(mutableStyle.get());
    if (blockStyle->length() > 0)
        applyCommand(ApplyStyleCommand::create(m_frame->document(), blockStyle.get(), editingAction));

    // Set the remaining style as the typing style.
    m_frame->selection()->setTypingStyle(mutableStyle.release());
}

PassRefPtr<CSSComputedStyleDeclaration> Editor::selectionComputedStyle(Node*& nodeToRemove) const
{
    nodeToRemove = 0;

    if (m_frame->selection()->isNone())
        return 0;

    RefPtr<Range> range(m_frame->selection()->toNormalizedRange());
    Position position = range->editingStartPosition();

    // If the pos is at the end of a text node, then this node is not fully selected. 
    // Move it to the next deep equivalent position to avoid removing the style from this node. 
    // e.g. if pos was at Position("hello", 5) in <b>hello<div>world</div></b>, we want Position("world", 0) instead. 
    // We only do this for range because caret at Position("hello", 5) in <b>hello</b>world should give you font-weight: bold. 
    Node* positionNode = position.containerNode(); 
    if (m_frame->selection()->isRange() && positionNode && positionNode->isTextNode() && position.computeOffsetInContainerNode() == positionNode->maxCharacterOffset()) 
        position = nextVisuallyDistinctCandidate(position); 

    Element* element = position.element();
    if (!element)
        return 0;

    RefPtr<Element> styleElement = element;
    ExceptionCode ec = 0;

    if (m_frame->selection()->typingStyle()) {
        styleElement = m_frame->document()->createElement(spanTag, false);

        styleElement->setAttribute(styleAttr, m_frame->selection()->typingStyle()->cssText(), ec);
        ASSERT(!ec);

        styleElement->appendChild(m_frame->document()->createEditingTextNode(""), ec);
        ASSERT(!ec);

        if (element->renderer() && element->renderer()->canHaveChildren())
            element->appendChild(styleElement, ec);
        else {
            Node* parent = element->parent();
            Node* next = element->nextSibling();

            if (next)
                parent->insertBefore(styleElement, next, ec);
            else
                parent->appendChild(styleElement, ec);
        }
        ASSERT(!ec);

        nodeToRemove = styleElement.get();
    }

    return computedStyle(styleElement.release());
}

void Editor::textFieldDidBeginEditing(Element* e)
{
    if (client())
        client()->textFieldDidBeginEditing(e);
}

void Editor::textFieldDidEndEditing(Element* e)
{
    if (client())
        client()->textFieldDidEndEditing(e);
}

void Editor::textDidChangeInTextField(Element* e)
{
    if (client())
        client()->textDidChangeInTextField(e);
}

bool Editor::doTextFieldCommandFromEvent(Element* e, KeyboardEvent* ke)
{
    if (client())
        return client()->doTextFieldCommandFromEvent(e, ke);

    return false;
}

void Editor::textWillBeDeletedInTextField(Element* input)
{
    if (client())
        client()->textWillBeDeletedInTextField(input);
}

void Editor::textDidChangeInTextArea(Element* e)
{
    if (client())
        client()->textDidChangeInTextArea(e);
}

void Editor::applyEditingStyleToBodyElement() const
{
    RefPtr<NodeList> list = m_frame->document()->getElementsByTagName("body");
    unsigned len = list->length();
    for (unsigned i = 0; i < len; i++)
        applyEditingStyleToElement(static_cast<Element*>(list->item(i)));
}

void Editor::applyEditingStyleToElement(Element* element) const
{
    if (!element)
        return;

    CSSStyleDeclaration* style = element->style();
    ASSERT(style);

    ExceptionCode ec = 0;
    style->setProperty(CSSPropertyWordWrap, "break-word", false, ec);
    ASSERT(!ec);
    style->setProperty(CSSPropertyWebkitNbspMode, "space", false, ec);
    ASSERT(!ec);
    style->setProperty(CSSPropertyWebkitLineBreak, "after-white-space", false, ec);
    ASSERT(!ec);
}

RenderStyle* Editor::styleForSelectionStart(Node *&nodeToRemove) const
{
    nodeToRemove = 0;

    if (m_frame->selection()->isNone())
        return 0;

    Position position = m_frame->selection()->selection().visibleStart().deepEquivalent();
    if (!position.isCandidate())
        return 0;
    if (!position.node())
        return 0;

    if (!m_frame->selection()->typingStyle())
        return position.node()->renderer()->style();

    RefPtr<Element> styleElement = m_frame->document()->createElement(spanTag, false);

    ExceptionCode ec = 0;
    String styleText = m_frame->selection()->typingStyle()->cssText() + " display: inline";
    styleElement->setAttribute(styleAttr, styleText.impl(), ec);
    ASSERT(!ec);

    styleElement->appendChild(m_frame->document()->createEditingTextNode(""), ec);
    ASSERT(!ec);

    position.node()->parentNode()->appendChild(styleElement, ec);
    ASSERT(!ec);

    nodeToRemove = styleElement.get();
    return styleElement->renderer() ? styleElement->renderer()->style() : 0;
}

// Searches from the beginning of the document if nothing is selected.
bool Editor::findString(const String& target, bool forward, bool caseFlag, bool wrapFlag, bool startInSelection)
{
    if (target.isEmpty())
        return false;

    if (m_frame->excludeFromTextSearch())
        return false;

    // Start from an edge of the selection, if there's a selection that's not in shadow content. Which edge
    // is used depends on whether we're searching forward or backward, and whether startInSelection is set.
    RefPtr<Range> searchRange(rangeOfContents(m_frame->document()));
    VisibleSelection selection = m_frame->selection()->selection();

    if (forward)
        setStart(searchRange.get(), startInSelection ? selection.visibleStart() : selection.visibleEnd());
    else
        setEnd(searchRange.get(), startInSelection ? selection.visibleEnd() : selection.visibleStart());

    RefPtr<Node> shadowTreeRoot = selection.shadowTreeRootNode();
    if (shadowTreeRoot) {
        ExceptionCode ec = 0;
        if (forward)
            searchRange->setEnd(shadowTreeRoot.get(), shadowTreeRoot->childNodeCount(), ec);
        else
            searchRange->setStart(shadowTreeRoot.get(), 0, ec);
    }

    RefPtr<Range> resultRange(findPlainText(searchRange.get(), target, forward, caseFlag));
    // If we started in the selection and the found range exactly matches the existing selection, find again.
    // Build a selection with the found range to remove collapsed whitespace.
    // Compare ranges instead of selection objects to ignore the way that the current selection was made.
    if (startInSelection && areRangesEqual(VisibleSelection(resultRange.get()).toNormalizedRange().get(), selection.toNormalizedRange().get())) {
        searchRange = rangeOfContents(m_frame->document());
        if (forward)
            setStart(searchRange.get(), selection.visibleEnd());
        else
            setEnd(searchRange.get(), selection.visibleStart());

        if (shadowTreeRoot) {
            ExceptionCode ec = 0;
            if (forward)
                searchRange->setEnd(shadowTreeRoot.get(), shadowTreeRoot->childNodeCount(), ec);
            else
                searchRange->setStart(shadowTreeRoot.get(), 0, ec);
        }

        resultRange = findPlainText(searchRange.get(), target, forward, caseFlag);
    }

    ExceptionCode exception = 0;

    // If nothing was found in the shadow tree, search in main content following the shadow tree.
    if (resultRange->collapsed(exception) && shadowTreeRoot) {
        searchRange = rangeOfContents(m_frame->document());
        if (forward)
            searchRange->setStartAfter(shadowTreeRoot->shadowParentNode(), exception);
        else
            searchRange->setEndBefore(shadowTreeRoot->shadowParentNode(), exception);

        resultRange = findPlainText(searchRange.get(), target, forward, caseFlag);
    }

    if (!insideVisibleArea(resultRange.get())) {
        resultRange = nextVisibleRange(resultRange.get(), target, forward, caseFlag, wrapFlag);
        if (!resultRange)
            return false;
    }

    // If we didn't find anything and we're wrapping, search again in the entire document (this will
    // redundantly re-search the area already searched in some cases).
    if (resultRange->collapsed(exception) && wrapFlag) {
        searchRange = rangeOfContents(m_frame->document());
        resultRange = findPlainText(searchRange.get(), target, forward, caseFlag);
        // We used to return false here if we ended up with the same range that we started with
        // (e.g., the selection was already the only instance of this text). But we decided that
        // this should be a success case instead, so we'll just fall through in that case.
    }

    if (resultRange->collapsed(exception))
        return false;

    m_frame->selection()->setSelection(VisibleSelection(resultRange.get(), DOWNSTREAM));
    m_frame->selection()->revealSelection();
    return true;
}

unsigned Editor::countMatchesForText(const String& target, bool caseFlag, unsigned limit, bool markMatches)
{
    if (target.isEmpty())
        return 0;

    RefPtr<Range> searchRange(rangeOfContents(m_frame->document()));

    ExceptionCode exception = 0;
    unsigned matchCount = 0;
    do {
        RefPtr<Range> resultRange(findPlainText(searchRange.get(), target, true, caseFlag));
        if (resultRange->collapsed(exception)) {
            if (!resultRange->startContainer()->isInShadowTree())
                break;

            searchRange = rangeOfContents(m_frame->document());
            searchRange->setStartAfter(resultRange->startContainer()->shadowAncestorNode(), exception);
            continue;
        }

        // Only treat the result as a match if it is visible
        if (insideVisibleArea(resultRange.get())) {
            ++matchCount;
            if (markMatches)
                m_frame->document()->markers()->addMarker(resultRange.get(), DocumentMarker::TextMatch);
        }

        // Stop looking if we hit the specified limit. A limit of 0 means no limit.
        if (limit > 0 && matchCount >= limit)
            break;

        // Set the new start for the search range to be the end of the previous
        // result range. There is no need to use a VisiblePosition here,
        // since findPlainText will use a TextIterator to go over the visible
        // text nodes. 
        searchRange->setStart(resultRange->endContainer(exception), resultRange->endOffset(exception), exception);

        Node* shadowTreeRoot = searchRange->shadowTreeRootNode();
        if (searchRange->collapsed(exception) && shadowTreeRoot)
            searchRange->setEnd(shadowTreeRoot, shadowTreeRoot->childNodeCount(), exception);
    } while (true);

    if (markMatches) {
        // Do a "fake" paint in order to execute the code that computes the rendered rect for each text match.
        if (m_frame->view() && m_frame->contentRenderer()) {
            m_frame->document()->updateLayout(); // Ensure layout is up to date.
            IntRect visibleRect = m_frame->view()->visibleContentRect();
            if (!visibleRect.isEmpty()) {
                GraphicsContext context((PlatformGraphicsContext*)0);
                context.setPaintingDisabled(true);
                m_frame->view()->paintContents(&context, visibleRect);
            }
        }
    }

    return matchCount;
}

void Editor::setMarkedTextMatchesAreHighlighted(bool flag)
{
    if (flag == m_areMarkedTextMatchesHighlighted)
        return;

    m_areMarkedTextMatchesHighlighted = flag;
    m_frame->document()->markers()->repaintMarkers(DocumentMarker::TextMatch);
}

void Editor::respondToChangedSelection(const VisibleSelection& oldSelection, bool closeTyping)
{
    bool isContinuousSpellCheckingEnabled = this->isContinuousSpellCheckingEnabled();
    bool isContinuousGrammarCheckingEnabled = isContinuousSpellCheckingEnabled && isGrammarCheckingEnabled();
    if (isContinuousSpellCheckingEnabled) {
        VisibleSelection newAdjacentWords;
        VisibleSelection newSelectedSentence;
        bool caretBrowsing = m_frame->settings() && m_frame->settings()->caretBrowsingEnabled();
        if (m_frame->selection()->selection().isContentEditable() || caretBrowsing) {
            VisiblePosition newStart(m_frame->selection()->selection().visibleStart());
            newAdjacentWords = VisibleSelection(startOfWord(newStart, LeftWordIfOnBoundary), endOfWord(newStart, RightWordIfOnBoundary));
            if (isContinuousGrammarCheckingEnabled)
                newSelectedSentence = VisibleSelection(startOfSentence(newStart), endOfSentence(newStart));
        }

        // When typing we check spelling elsewhere, so don't redo it here.
        // If this is a change in selection resulting from a delete operation,
        // oldSelection may no longer be in the document.
        if (closeTyping && oldSelection.isContentEditable() && oldSelection.start().node() && oldSelection.start().node()->inDocument()) {
            VisiblePosition oldStart(oldSelection.visibleStart());
            VisibleSelection oldAdjacentWords = VisibleSelection(startOfWord(oldStart, LeftWordIfOnBoundary), endOfWord(oldStart, RightWordIfOnBoundary));
            if (oldAdjacentWords != newAdjacentWords) {
                if (isContinuousGrammarCheckingEnabled) {
                    VisibleSelection oldSelectedSentence = VisibleSelection(startOfSentence(oldStart), endOfSentence(oldStart));
                    markMisspellingsAndBadGrammar(oldAdjacentWords, oldSelectedSentence != newSelectedSentence, oldSelectedSentence);
                } else
                    markMisspellingsAndBadGrammar(oldAdjacentWords, false, oldAdjacentWords);
            }
        }

        // This only erases markers that are in the first unit (word or sentence) of the selection.
        // Perhaps peculiar, but it matches AppKit.
        if (RefPtr<Range> wordRange = newAdjacentWords.toNormalizedRange()) {
            m_frame->document()->markers()->removeMarkers(wordRange.get(), DocumentMarker::Spelling);
            m_frame->document()->markers()->removeMarkers(wordRange.get(), DocumentMarker::Replacement);
        }
        if (RefPtr<Range> sentenceRange = newSelectedSentence.toNormalizedRange())
            m_frame->document()->markers()->removeMarkers(sentenceRange.get(), DocumentMarker::Grammar);
    }

    // When continuous spell checking is off, existing markers disappear after the selection changes.
    if (!isContinuousSpellCheckingEnabled)
        m_frame->document()->markers()->removeMarkers(DocumentMarker::Spelling);
    if (!isContinuousGrammarCheckingEnabled)
        m_frame->document()->markers()->removeMarkers(DocumentMarker::Grammar);

    respondToChangedSelection(oldSelection);
}

bool Editor::selectionStartHasSpellingMarkerFor(int from, int length) const
{
    Node* node = m_frame->selection()->start().node();
    if (!node || !node->renderer())
        return false;
    ASSERT(node->renderer()->isText());

    unsigned int startOffset = static_cast<unsigned int>(from);
    unsigned int endOffset = static_cast<unsigned int>(from + length);
    Vector<DocumentMarker> markers = m_frame->document()->markers()->markersForNode(node);
    for (size_t i = 0; i < markers.size(); ++i) {
        DocumentMarker marker = markers[i];
        if (marker.startOffset <= startOffset && endOffset <= marker.endOffset && marker.type == DocumentMarker::Spelling)
            return true;
    }

    return false;
}


} // namespace WebCore
