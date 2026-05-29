#include "DragController.h"
#include <cstdlib>

void DragController::OnButtonDown(int buttonIndex, POINT ptScreen)
{
    state_     = DragState::Pressed;
    dragIndex_ = buttonIndex;
    startPt_   = ptScreen;
    currentPt_ = ptScreen;
}

void DragController::OnMouseMove(POINT ptScreen)
{
    currentPt_ = ptScreen;
    if (state_ == DragState::Pressed) {
        int dx = std::abs(ptScreen.x - startPt_.x);
        int dy = std::abs(ptScreen.y - startPt_.y);
        if (dx > kThresholdPx || dy > kThresholdPx)
            state_ = DragState::Dragging;
    }
}

int DragController::OnButtonUp()
{
    int idx = dragIndex_;
    state_     = DragState::Idle;
    dragIndex_ = -1;
    return idx;
}

void DragController::OnCaptureChanged()
{
    state_     = DragState::Idle;
    dragIndex_ = -1;
}
