#pragma once

namespace meet {

enum class AudioCue {
    Connected,
    Weak,
    Lost,
};

void PlayAudioCue(AudioCue cue);

}  // namespace meet
