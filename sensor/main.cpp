#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <iostream>
#include <locale>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "ScreenCapture.h"
#include "HTTPRequest.hpp"

std::shared_ptr<SL::Screen_Capture::IScreenCaptureManager> framegrabber;

std::shared_ptr<std::vector<std::vector<int>>> worldMapping;

std::shared_ptr<std::map<int, int>> entityIdMapping;

std::shared_ptr<std::vector<int>> screen = nullptr;

void processImage(const SL::Screen_Capture::Image &image) {
    auto ptr = SL::Screen_Capture::StartSrc(image);
    int hmax = SL::Screen_Capture::Height(image);
    int wmax = SL::Screen_Capture::Width(image);
    if (screen == nullptr) {
        screen = std::vector<int>(hmax * wmax * 3, 0);
    }
    int idx = 0;
    for (auto h = 0; h < hmax; h++) {
        auto ptrRow = ptr;
        for (auto w = 0; w < wmax; w++) {
            int r = ptr->R;
            int g = ptr->G;
            int b = ptr->B;
            screen.get()[idx++] = r;
            screen.get()[idx++] = g;
            screen.get()[idx++] = b;
            ptr++;
        }
        ptr = SL::Screen_Capture::GotoNextRow(image, ptrRow);
    }
}

bool isBlack(int r, int g, int b) {
    return r <= 20 && g <= 20 && b <= 20;
}

void processImageForTinyPlatformer(const SL::Screen_Capture::Image &image) {
    int hmax = SL::Screen_Capture::Height(image);
    int wmax = SL::Screen_Capture::Width(image);
    int xStart = -1, xEnd = -1, yStart = -1, yEnd = -1;
    int safearea = 200;
    bool found = false;
    found = false;
    for (auto h = 0; h < hmax; h++) {
        if (h == hmax / 2) {
            for (auto w = safearea; w < wmax; w++) {
                int r = screen.get()[3 * ((h - 1) * wmax + w) + 0];
                int g = screen.get()[3 * ((h - 1) * wmax + w) + 1];
                int b = screen.get()[3 * ((h - 1) * wmax + w) + 2];
                if (!found) {
                    if (isBlack(r, g, b)) {
                        found = true;
                    }
                } else {
                    if (!isBlack(r, g, b)) {
                        xStart = w;
                        break;
                    }
                }
            }
            break;
        }
    }
    found = false;
    for (auto h = 0; h < hmax; h++) {
        if (h == hmax / 2) {
            for (auto w = wmax - safearea; w >= 0; w--) {
                int r = screen.get()[3 * ((h - 1) * wmax + w) + 0];
                int g = screen.get()[3 * ((h - 1) * wmax + w) + 1];
                int b = screen.get()[3 * ((h - 1) * wmax + w) + 2];
                if (!found) {
                    if (isBlack(r, g, b)) {
                        found = true;
                    }
                } else {
                    if (!isBlack(r, g, b)) {
                        xEnd = w;
                        break;
                    }
                }
            }
            break;
        }
    }
    found = false;
    for (auto h = safearea; h < hmax; h++) {
        int r = -1;
        int g = -1;
        int b = -1;
        for (auto w = 0; w < wmax / 2 + 1; w++) {
            r = screen.get()[3 * ((h - 1) * wmax + w) + 0];
            g = screen.get()[3 * ((h - 1) * wmax + w) + 1];
            b = screen.get()[3 * ((h - 1) * wmax + w) + 2];
        }
        if (!found) {
            if (isBlack(r, g, b)) {
                found = true;
            }
        } else {
            if (!isBlack(r, g, b)) {
                yStart = h;
                break;
            }
        }
    }
    found = false;
    for (auto h = hmax - safearea; h >= 0; h--) {
        int r = -1;
        int g = -1;
        int b = -1;
        for (auto w = 0; w < wmax / 2 + 1; w++) {
            r = screen.get()[3 * ((h - 1) * wmax + w) + 0];
            g = screen.get()[3 * ((h - 1) * wmax + w) + 1];
            b = screen.get()[3 * ((h - 1) * wmax + w) + 2];
        }
        if (!found) {
            if (isBlack(r, g, b)) {
                found = true;
            }
        } else {
            if (!isBlack(r, g, b)) {
                yEnd = h;
                break;
            }
        }
    }
    std::cout << wmax << " " << hmax << std::endl;
    std::cout << xStart << " " << xEnd << " ; " << yStart << " " << yEnd << std::endl;
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
            processImageForTinyPlatformer(img);
        })
        ->onMouseChanged([&](const SL::Screen_Capture::Image *img, const SL::Screen_Capture::MousePoint &mousepoint) {
        })
        ->start_capturing();

    framegrabber->setFrameChangeInterval(std::chrono::milliseconds(100));
    framegrabber->setMouseChangeInterval(std::chrono::milliseconds(100));
}

int main() {
    createframegrabber();
    while (true) {
        usleep(100000);
    }
    return 0;
}

