#include <windows.h>
#include "App.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    return App::Instance().Run(hInstance, nCmdShow);
}
