#ifndef PS2_AUDIO_H
#define PS2_AUDIO_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// Runtime "Radio Off": silence the emulated SPU2 output, which carries the game's
// own audio (including its real radio stations). Our own music folder is separate
// and is paused by the music-station module.
void ps2AudioSetOutputMuted(bool muted);
bool ps2AudioOutputMuted();

class PS2AudioBackend
{public:
    PS2AudioBackend();
    ~PS2AudioBackend();

    void onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes);
    void onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr);
    void onSoundCommand(uint32_t sid, uint32_t rpcNum,
                        const uint8_t *sendBuf, uint32_t sendSize,
                        uint8_t *recvBuf, uint32_t recvSize);

    void play(uint32_t sampleAddr, float pitch = 1.0f, float volume = 1.0f,
              uint32_t voiceIndex = 0xFFFFFFFFu);
    void stop(uint32_t voiceId);
    void stopAll();
    void setAudioReady(bool ready);

    // SPU2 output (interleaved stereo 16-bit at 48 kHz) to the host device.
    void submitStream(const int16_t *stereoFrames, uint32_t frameCount);

private:
    struct DecodedSample
    {
        std::vector<int16_t> pcm;
        uint32_t sampleRate = 44100;
    };

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_audioReady = false;
    uint32_t m_mostRecentSampleKey = 0;
    std::vector<DecodedSample> m_loadOrderSamples;
    std::vector<uint32_t> m_loadOrderSampleKeys;
    std::unordered_map<uint32_t, DecodedSample> m_sampleBank;
    std::mutex m_mutex;

    void playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                          bool isBgm = false);
    void pruneFinishedSounds();
};

#endif
