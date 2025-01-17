/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FormattingContext.h"

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "DisplayBox.h"
#include "LayoutBox.h"
#include "LayoutContainer.h"
#include "LayoutContext.h"
#include "LayoutDescendantIterator.h"
#include "Logging.h"
#include <wtf/IsoMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebCore {
namespace Layout {

WTF_MAKE_ISO_ALLOCATED_IMPL(FormattingContext);

FormattingContext::FormattingContext(const Box& formattingContextRoot)
    : m_root(makeWeakPtr(formattingContextRoot))
{
}

FormattingContext::~FormattingContext()
{
}

void FormattingContext::computeOutOfFlowHorizontalGeometry(LayoutContext& layoutContext, const Box& layoutBox) const
{
    auto compute = [&](std::optional<LayoutUnit> usedWidth) {
        return Geometry::outOfFlowHorizontalGeometry(layoutContext, *this, layoutBox, usedWidth);
    };

    auto horizontalGeometry = compute({ });
    auto containingBlockWidth = layoutContext.displayBoxForLayoutBox(*layoutBox.containingBlock()).contentBoxWidth();

    if (auto maxWidth = Geometry::computedValueIfNotAuto(layoutBox.style().logicalMaxWidth(), containingBlockWidth)) {
        auto maxHorizontalGeometry = compute(maxWidth);
        if (horizontalGeometry.widthAndMargin.width > maxHorizontalGeometry.widthAndMargin.width)
            horizontalGeometry = maxHorizontalGeometry;
    }

    if (auto minWidth = Geometry::computedValueIfNotAuto(layoutBox.style().logicalMinWidth(), containingBlockWidth)) {
        auto minHorizontalGeometry = compute(minWidth);
        if (horizontalGeometry.widthAndMargin.width < minHorizontalGeometry.widthAndMargin.width)
            horizontalGeometry = minHorizontalGeometry;
    }

    auto& displayBox = layoutContext.displayBoxForLayoutBox(layoutBox);
    displayBox.setLeft(horizontalGeometry.left + horizontalGeometry.widthAndMargin.margin.left);
    displayBox.setContentBoxWidth(horizontalGeometry.widthAndMargin.width);
    displayBox.setHorizontalMargin(horizontalGeometry.widthAndMargin.margin);
    displayBox.setHorizontalNonComputedMargin(horizontalGeometry.widthAndMargin.nonComputedMargin);
}

void FormattingContext::computeOutOfFlowVerticalGeometry(const LayoutContext& layoutContext, const Box& layoutBox) const
{
    auto compute = [&](std::optional<LayoutUnit> usedHeight) {
        return Geometry::outOfFlowVerticalGeometry(layoutContext, layoutBox, usedHeight);
    };

    auto verticalGeometry = compute({ });
    if (auto maxHeight = Geometry::computedMaxHeight(layoutContext, layoutBox)) {
        auto maxVerticalGeometry = compute(maxHeight);
        if (verticalGeometry.heightAndMargin.height > maxVerticalGeometry.heightAndMargin.height)
            verticalGeometry = maxVerticalGeometry;
    }

    if (auto minHeight = Geometry::computedMinHeight(layoutContext, layoutBox)) {
        auto minVerticalGeometry = compute(minHeight);
        if (verticalGeometry.heightAndMargin.height < minVerticalGeometry.heightAndMargin.height)
            verticalGeometry = minVerticalGeometry;
    }

    auto& displayBox = layoutContext.displayBoxForLayoutBox(layoutBox);
    displayBox.setTop(verticalGeometry.top + verticalGeometry.heightAndMargin.margin.top);
    displayBox.setContentBoxHeight(verticalGeometry.heightAndMargin.height);
    ASSERT(!verticalGeometry.heightAndMargin.collapsedMargin);
    displayBox.setVerticalMargin(verticalGeometry.heightAndMargin.margin);
    displayBox.setVerticalNonCollapsedMargin(verticalGeometry.heightAndMargin.margin);
}

void FormattingContext::computeBorderAndPadding(const LayoutContext& layoutContext, const Box& layoutBox) const
{
    auto& displayBox = layoutContext.displayBoxForLayoutBox(layoutBox);
    displayBox.setBorder(Geometry::computedBorder(layoutContext, layoutBox));
    displayBox.setPadding(Geometry::computedPadding(layoutContext, layoutBox));
}

void FormattingContext::placeInFlowPositionedChildren(const LayoutContext& layoutContext, const Container& container) const
{
    // If this container also establishes a formatting context, then positioning already has happend in that the formatting context.
    if (container.establishesFormattingContext() && &container != &root())
        return;

    LOG_WITH_STREAM(FormattingContextLayout, stream << "Start: move in-flow positioned children -> context: " << &layoutContext << " parent: " << &container);
    for (auto& layoutBox : childrenOfType<Box>(container)) {
        if (!layoutBox.isInFlowPositioned())
            continue;
        computeInFlowPositionedPosition(layoutContext, layoutBox);
    }
    LOG_WITH_STREAM(FormattingContextLayout, stream << "End: move in-flow positioned children -> context: " << &layoutContext << " parent: " << &container);
}

void FormattingContext::layoutOutOfFlowDescendants(LayoutContext& layoutContext, const Box& layoutBox) const
{
    // Initial containing block by definition is a containing block.
    if (!layoutBox.isPositioned() && !layoutBox.isInitialContainingBlock())
        return;

    if (!is<Container>(layoutBox))
        return;

    auto& container = downcast<Container>(layoutBox);
    if (!container.hasChild())
        return;

    LOG_WITH_STREAM(FormattingContextLayout, stream << "Start: layout out-of-flow descendants -> context: " << &layoutContext << " root: " << &root());

    for (auto& outOfFlowBox : container.outOfFlowDescendants()) {
        auto& layoutBox = *outOfFlowBox;

        ASSERT(layoutBox.establishesFormattingContext());
        auto formattingContext = layoutContext.formattingContext(layoutBox);

        computeBorderAndPadding(layoutContext, layoutBox);
        computeOutOfFlowHorizontalGeometry(layoutContext, layoutBox);

        auto& formattingState = layoutContext.createFormattingStateForFormattingRootIfNeeded(layoutBox);
        formattingContext->layout(layoutContext, formattingState);

        computeOutOfFlowVerticalGeometry(layoutContext, layoutBox);
        layoutOutOfFlowDescendants(layoutContext, layoutBox);
    }
    LOG_WITH_STREAM(FormattingContextLayout, stream << "End: layout out-of-flow descendants -> context: " << &layoutContext << " root: " << &root());
}

Display::Box FormattingContext::mapBoxToAncestor(const LayoutContext& layoutContext, const Box& layoutBox, const Container& ancestor)
{
    ASSERT(layoutBox.isDescendantOf(ancestor));

    auto& displayBox = layoutContext.displayBoxForLayoutBox(layoutBox);
    auto topLeft = displayBox.topLeft();

    auto* containingBlock = layoutBox.containingBlock();
    for (; containingBlock && containingBlock != &ancestor; containingBlock = containingBlock->containingBlock())
        topLeft.moveBy(layoutContext.displayBoxForLayoutBox(*containingBlock).topLeft());

    if (!containingBlock) {
        ASSERT_NOT_REACHED();
        return Display::Box(displayBox);
    }

    auto mappedDisplayBox = Display::Box(displayBox);
    mappedDisplayBox.setTopLeft(topLeft);
    return mappedDisplayBox;
}

Position FormattingContext::mapTopLeftToAncestor(const LayoutContext& layoutContext, const Box& layoutBox, const Container& ancestor)
{
    ASSERT(layoutBox.isDescendantOf(ancestor));
    return mapCoordinateToAncestor(layoutContext, layoutContext.displayBoxForLayoutBox(layoutBox).topLeft(), *layoutBox.containingBlock(), ancestor);
}

Position FormattingContext::mapCoordinateToAncestor(const LayoutContext& layoutContext, Position position, const Container& containingBlock, const Container& ancestor)
{
    auto mappedPosition = position;
    auto* container = &containingBlock;
    for (; container && container != &ancestor; container = container->containingBlock())
        mappedPosition.moveBy(layoutContext.displayBoxForLayoutBox(*container).topLeft());

    if (!container) {
        ASSERT_NOT_REACHED();
        return position;
    }

    return mappedPosition;
}

#ifndef NDEBUG
void FormattingContext::validateGeometryConstraintsAfterLayout(const LayoutContext& layoutContext) const
{
    if (!is<Container>(root()))
        return;
    auto& formattingContextRoot = downcast<Container>(root());
    // FIXME: add a descendantsOfType<> flavor that stops at nested formatting contexts
    for (auto& layoutBox : descendantsOfType<Box>(formattingContextRoot)) {
        if (&layoutBox.formattingContextRoot() != &formattingContextRoot)
            continue;
        auto& containingBlockDisplayBox = layoutContext.displayBoxForLayoutBox(*layoutBox.containingBlock());
        auto& displayBox = layoutContext.displayBoxForLayoutBox(layoutBox);

        // 10.3.3 Block-level, non-replaced elements in normal flow
        // 10.3.7 Absolutely positioned, non-replaced elements
        if ((layoutBox.isBlockLevelBox() || layoutBox.isOutOfFlowPositioned()) && !layoutBox.replaced()) {
            // margin-left + border-left-width + padding-left + width + padding-right + border-right-width + margin-right = width of containing block
            auto containingBlockWidth = containingBlockDisplayBox.contentBoxWidth();
            ASSERT(displayBox.marginLeft() + displayBox.borderLeft() + displayBox.paddingLeft().value_or(0) + displayBox.contentBoxWidth()
                + displayBox.paddingRight().value_or(0) + displayBox.borderRight() + displayBox.marginRight() == containingBlockWidth);
        }

        // 10.6.4 Absolutely positioned, non-replaced elements
        if (layoutBox.isOutOfFlowPositioned() && !layoutBox.replaced()) {
            // top + margin-top + border-top-width + padding-top + height + padding-bottom + border-bottom-width + margin-bottom + bottom = height of containing block
            auto containingBlockHeight = containingBlockDisplayBox.contentBoxHeight();
            ASSERT(displayBox.top() + displayBox.marginTop() + displayBox.borderTop() + displayBox.paddingTop().value_or(0) + displayBox.contentBoxHeight()
                + displayBox.paddingBottom().value_or(0) + displayBox.borderBottom() + displayBox.marginBottom() == containingBlockHeight);
        }
    }
}
#endif

}
}
#endif
