#pragma once

#include <cstdint>

namespace meet {

constexpr int kFaceSize = 64;

void FaceDrawEmotion(const char* name, uint16_t* buf, int size);

}  // namespace meet
