#pragma once
#include <windows.h>

enum class DragState { Idle, Pressed, Dragging };

class DragController {
public:
    void OnButtonDown(int buttonIndex, POINT ptScreen);
    void OnMouseMove(POINT ptScreen);
    int  OnButtonUp();
    void OnCaptureChanged();

    DragState State()        const { return state_; }
    int       DragIndex()    const { return dragIndex_; }
    POINT     CurrentPoint() const { return currentPt_; }

    // Live reordering moves the dragged button within its zone mid-drag; keep the
    // tracked index pointing at the button's new slot so the ghost follows it.
    void      SetDragIndex(int i)  { dragIndex_ = i; }

private:
    static constexpr int kThresholdPx = 6;

    DragState state_     = DragState::Idle;
    int       dragIndex_ = -1;
    POINT     startPt_   = {};
    POINT     currentPt_ = {};
};
