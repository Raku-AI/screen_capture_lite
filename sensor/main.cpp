#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <iostream>
#include <locale>
#include <string>
#include <thread>
#include <vector>

#include "ScreenCapture.h"
#include "HTTPRequest.hpp"

std::shared_ptr<SL::Screen_Capture::IScreenCaptureManager> framegrabber;

std::shared_ptr<std::vector<std::vector<int>>> map;

void processImage(const SL::Screen_Capture::Image &image) {
    auto ptr = SL::Screen_Capture::StartSrc(image);
    int hmax = SL::Screen_Capture::Height(image);
    int wmax = SL::Screen_Capture::Width(image);
    for (auto h = 0; h < hmax; h++) {
        auto ptrRow = ptr;
        for (auto w = 0; w < wmax; w++) {
            int r = ptr->R;
            int g = ptr->G;
            int b = ptr->B;
            ptr++;
        }
        ptr = SL::Screen_Capture::GotoNextRow(image, ptrRow);
    }
}

void createframegrabber() {
    framegrabber = nullptr;
    framegrabber =
        SL::Screen_Capture::CreateCaptureConfiguration([]() {
            auto mons = SL::Screen_Capture::GetMonitors();
            return mons;
        })
        ->onFrameChanged([&](const SL::Screen_Capture::Image &img, const SL::Screen_Capture::Monitor &monitor) {
        })
        ->onNewFrame([&](const SL::Screen_Capture::Image &img, const SL::Screen_Capture::Monitor &monitor) {
            processImage(img);
        })
        ->onMouseChanged([&](const SL::Screen_Capture::Image *img, const SL::Screen_Capture::MousePoint &mousepoint) {
        })
        ->start_capturing();

    framegrabber->setFrameChangeInterval(std::chrono::milliseconds(100));
    framegrabber->setMouseChangeInterval(std::chrono::milliseconds(100));
}

int main() {
    createframegrabber();
    return 0;
}

