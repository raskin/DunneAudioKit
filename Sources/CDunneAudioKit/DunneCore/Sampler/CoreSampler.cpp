// Copyright AudioKit. All Rights Reserved.

#include "CoreSampler.h"
#include "SamplerVoice.h"
#include "FunctionTable.h"
#include "SustainPedalLogic.h"

#include <math.h>
#include <list>

// number of voices
#define MAX_POLYPHONY 64

// MIDI offers 128 distinct note numbers
#define MIDI_NOTENUMBERS 128

// Convert MIDI note to Hz, for 12-tone equal temperament
#define NOTE_HZ(midiNoteNumber) ( 440.0f * pow(2.0f, ((midiNoteNumber) - 69.0f)/12.0f) )

struct CoreSampler::InternalData {
    // list of (pointers to) all loaded samples
    std::list<DunneCore::KeyMappedSampleBuffer*> sampleBufferList;
    
    // maps MIDI note numbers to "closest" samples (all velocity layers)
    std::list<DunneCore::KeyMappedSampleBuffer*> keyMap[MIDI_NOTENUMBERS];
    
    DunneCore::AHDSHREnvelopeParameters ampEnvelopeParameters;
    DunneCore::ADSREnvelopeParameters filterEnvelopeParameters;
    DunneCore::ADSREnvelopeParameters pitchEnvelopeParameters;
    
    // table of voice resources
    DunneCore::SamplerVoice voice[MAX_POLYPHONY];
    
    // one vibrato LFO shared by all voices
    DunneCore::FunctionTableOscillator vibratoLFO;
    
    // one vibrato LFO shared by all voices
    DunneCore::FunctionTableOscillator globalLFO;
    
    DunneCore::SustainPedalLogic pedalLogic;
    
    // tuning table
    float tuningTable[128];
};

CoreSampler::CoreSampler()
: currentSampleRate(44100.0f)    // sensible guess
, isKeyMapValid(false)
, isFilterEnabled(false)
, restartVoiceLFO(false)
, overallGain(0.0f)
, overallPan(0.0f)
, masterVolume(1.0f)
, pitchOffset(0.0f)
, vibratoDepth(0.0f)
, vibratoFrequency(5.0f)
, voiceVibratoDepth(0.0f)
, voiceVibratoFrequency(5.0f)
, glideRate(0.0f)   // 0 sec/octave means "no glide"
, lfoRate(5.0f)
, lfoDepth(0.0f)
, lfoTargetPitchToggle(0.0f)
, lfoTargetGainToggle(0.0f)
, lfoTargetFilterToggle(0.0f)
, isMonophonic(false)
, isLegato(false)
, portamentoRate(1.0f)
, cutoffMultiple(4.0f)
, keyTracking(1.0f)
, cutoffEnvelopeStrength(20.0f)
, filterEnvelopeVelocityScaling(0.0f)
, linearResonance(0.5f)
, pitchADSRSemitones(0.0f)
, loopThruRelease(false)
, stoppingAllVoices(false)
, data(new InternalData)
{
    DunneCore::SamplerVoice *pVoice = data->voice;
    for (int i=0; i < MAX_POLYPHONY; i++, pVoice++)
    {
        pVoice->ampEnvelope.pParameters = &data->ampEnvelopeParameters;
        pVoice->filterEnvelope.pParameters = &data->filterEnvelopeParameters;
        pVoice->pitchEnvelope.pParameters = &data->pitchEnvelopeParameters;
        pVoice->noteFrequency = 0.0f;
        pVoice->glideSecPerOctave = &glideRate;
    }
    
    // Initialize global LFO (sinusoidal)
    data->globalLFO.waveTable.sinusoid();
    data->globalLFO.init(currentSampleRate / CORESAMPLER_CHUNKSIZE, lfoRate);
    
    for (int i=0; i < 128; i++)
        data->tuningTable[i] = NOTE_HZ(i);
}

CoreSampler::~CoreSampler()
{
    unloadAllSamples();
}

// New `lookupSamples()` method: return all matching regions (buffers) for the note and velocity
std::list<DunneCore::KeyMappedSampleBuffer*> CoreSampler::lookupSamples(unsigned noteNumber, unsigned velocity) {
    std::list<DunneCore::KeyMappedSampleBuffer*> matchingSamples;

    // Search for samples mapped to this note
    for (DunneCore::KeyMappedSampleBuffer *pBuf : data->keyMap[noteNumber]) {
        // Check if the velocity matches (if the sample has velocity range)
        if ((int)velocity >= pBuf->minimumVelocity && (int)velocity <= pBuf->maximumVelocity) {
            matchingSamples.push_back(pBuf);  // Add this sample to the list of matches
        }
    }

    return matchingSamples;
}

int CoreSampler::init(double sampleRate)
{
    currentSampleRate = (float)sampleRate;
    data->ampEnvelopeParameters.updateSampleRate((float)(sampleRate/CORESAMPLER_CHUNKSIZE));
    data->filterEnvelopeParameters.updateSampleRate((float)(sampleRate/CORESAMPLER_CHUNKSIZE));
    data->pitchEnvelopeParameters.updateSampleRate((float)(sampleRate/CORESAMPLER_CHUNKSIZE));
    data->vibratoLFO.waveTable.sinusoid();
    data->vibratoLFO.init(sampleRate/CORESAMPLER_CHUNKSIZE, 5.0f);
    data->globalLFO.waveTable.sinusoid();
    data->globalLFO.init(sampleRate / CORESAMPLER_CHUNKSIZE, lfoRate);
    
    for (int i=0; i<MAX_POLYPHONY; i++)
        data->voice[i].init(sampleRate);
    return 0;   // no error
}

void CoreSampler::deinit()
{
}

void CoreSampler::unloadAllSamples()
{
    isKeyMapValid = false;
    for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        delete pBuf;
    data->sampleBufferList.clear();
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
        data->keyMap[i].clear();
}

void CoreSampler::loadSampleData(SampleDataDescriptor& sdd)
{
    DunneCore::KeyMappedSampleBuffer *pBuf = new DunneCore::KeyMappedSampleBuffer();
    pBuf->minimumNoteNumber = sdd.sampleDescriptor.minimumNoteNumber;
    pBuf->maximumNoteNumber = sdd.sampleDescriptor.maximumNoteNumber;
    pBuf->minimumVelocity = sdd.sampleDescriptor.minimumVelocity;
    pBuf->maximumVelocity = sdd.sampleDescriptor.maximumVelocity;
    pBuf->gain = sdd.sampleDescriptor.gain;
    pBuf->pan = sdd.sampleDescriptor.pan;

    data->sampleBufferList.push_back(pBuf);
    
    // Rest of the code
    pBuf->init(sdd.sampleRate, sdd.channelCount, sdd.sampleCount);
    float *pData = sdd.data;
    if (sdd.isInterleaved)
    {
        for (int i = 0; i < sdd.sampleCount; i++)
        {
            pBuf->setData(i, *pData++);
            if (sdd.channelCount > 1) pBuf->setData(sdd.sampleCount + i, *pData++);
        }
    }
    else
    {
        for (int i = 0; i < sdd.channelCount * sdd.sampleCount; i++)
        {
            pBuf->setData(i, *pData++);
        }
    }
    pBuf->noteNumber = sdd.sampleDescriptor.noteNumber;
    pBuf->noteDetune = sdd.sampleDescriptor.noteDetune;
    pBuf->noteFrequency = sdd.sampleDescriptor.noteFrequency;
    
    // Handle rare case where loopEndPoint is 0 (due to being uninitialized)
    if (sdd.sampleDescriptor.loopEndPoint == 0.0f)
        sdd.sampleDescriptor.loopEndPoint = float(sdd.sampleCount - 1);

    if (sdd.sampleDescriptor.startPoint > 0.0f) pBuf->startPoint = sdd.sampleDescriptor.startPoint;
    if (sdd.sampleDescriptor.endPoint > 0.0f)   pBuf->endPoint = sdd.sampleDescriptor.endPoint;
    
    pBuf->isLooping = sdd.sampleDescriptor.isLooping;
    if (pBuf->isLooping)
    {
        // loopStartPoint, loopEndPoint are usually sample indices, but values 0.0-1.0
        // are interpreted as fractions of the total sample length.
        if (sdd.sampleDescriptor.loopStartPoint > 1.0f) pBuf->loopStartPoint = sdd.sampleDescriptor.loopStartPoint;
        else pBuf->loopStartPoint = pBuf->endPoint * sdd.sampleDescriptor.loopStartPoint;
        if (sdd.sampleDescriptor.loopEndPoint > 1.0f) pBuf->loopEndPoint = sdd.sampleDescriptor.loopEndPoint;
        else pBuf->loopEndPoint = pBuf->endPoint * sdd.sampleDescriptor.loopEndPoint;

        // Clamp loop endpoints to valid range
        if (pBuf->loopStartPoint < pBuf->startPoint) pBuf->loopStartPoint = pBuf->startPoint;
        if (pBuf->loopEndPoint > pBuf->endPoint) pBuf->loopEndPoint = pBuf->endPoint;
    }
}

DunneCore::KeyMappedSampleBuffer *CoreSampler::lookupSample(unsigned noteNumber, unsigned velocity)
{
    // common case: only one sample mapped to this note - return it immediately
    if (data->keyMap[noteNumber].size() == 1)
        return data->keyMap[noteNumber].front();
    
    // search samples mapped to this note for best choice based on velocity
    for (DunneCore::KeyMappedSampleBuffer *pBuf : data->keyMap[noteNumber])
    {
        // if sample does not have velocity range, accept it trivially
        if (pBuf->minimumVelocity < 0 || pBuf->maximumVelocity < 0) return pBuf;
        
        // otherwise (common case), accept based on velocity
        if ((int)velocity >= pBuf->minimumVelocity && (int)velocity <= pBuf->maximumVelocity) return pBuf;
    }
    
    // return nil if no samples mapped to note (or sample velocities are invalid)
    return 0;
}

void CoreSampler::setNoteFrequency(int noteNumber, float noteFrequency)
{
    data->tuningTable[noteNumber] = noteFrequency;
}

// re-compute keyMap[] so every MIDI note number is automatically mapped to the sample buffer
// closest in pitch
void CoreSampler::buildSimpleKeyMap()
{
    // clear out the old mapping entirely
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
        data->keyMap[i].clear();
    
    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        
        // scan loaded samples to find the minimum distance to note nn
        float minDistance = 1000000.0f;
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance < minDistance)
            {
                minDistance = distance;
            }
        }
        
        // scan again to add only samples at this distance to the list for note nn
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance == minDistance)
            {
                data->keyMap[nn].push_back(pBuf);
            }
        }
    }
    isKeyMapValid = true;
}

// rebuild keyMap based on explicit mapping data in samples
void CoreSampler::buildKeyMap(void)
{
    // clear out the old mapping entirely
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++) data->keyMap[i].clear();
    
    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float minFreq = NOTE_HZ(pBuf->minimumNoteNumber);
            float maxFreq = NOTE_HZ(pBuf->maximumNoteNumber);
            if (noteFreq >= minFreq && noteFreq <= maxFreq)
                data->keyMap[nn].push_back(pBuf);
        }
    }
    isKeyMapValid = true;
}

DunneCore::SamplerVoice *CoreSampler::voicePlayingNote(unsigned noteNumber)
{
    for (int i=0; i < MAX_POLYPHONY; i++)
    {
        DunneCore::SamplerVoice *pVoice = &data->voice[i];
        if (pVoice->noteNumber == noteNumber) return pVoice;
    }
    return 0;
}

void CoreSampler::playNote(unsigned noteNumber, unsigned velocity)
{
    bool anotherKeyWasDown = data->pedalLogic.isAnyKeyDown();
    data->pedalLogic.keyDownAction(noteNumber);
    play(noteNumber, velocity, anotherKeyWasDown);
}

void CoreSampler::stopNote(unsigned noteNumber, bool immediate)
{
    if (immediate || data->pedalLogic.keyUpAction(noteNumber))
        stop(noteNumber, immediate);
}

void CoreSampler::sustainPedal(bool down)
{
    if (down) data->pedalLogic.pedalDown();
    else {
        for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
        {
            if (data->pedalLogic.isNoteSustaining(nn))
                stop(nn, false);
        }
        data->pedalLogic.pedalUp();
    }
}

void CoreSampler::play(unsigned noteNumber, unsigned velocity, bool anotherKeyWasDown)
{
    if (stoppingAllVoices) return;

    float noteFrequency = data->tuningTable[noteNumber];
    
    // Look up all samples (regions) associated with the note number and velocity
    std::list<DunneCore::KeyMappedSampleBuffer*> samples = lookupSamples(noteNumber, velocity);
    
    if (isMonophonic)
    {
        DunneCore::SamplerVoice *pVoice = &data->voice[0]; // Monophonic uses one voice
        if (!samples.empty())
        {
            DunneCore::KeyMappedSampleBuffer *pBuf = samples.front();  // Use the first sample in monophonic mode
            float detuneFactor = powf(2.0f, pBuf->noteDetune / 1200.0f);
            float detunedFrequency = noteFrequency * detuneFactor;

            if (pVoice->noteNumber >= 0)
            {
                pVoice->restartNewNote(noteNumber, currentSampleRate, detunedFrequency, velocity / 127.0f, pBuf);
            }
            else
            {
                pVoice->start(noteNumber, currentSampleRate, detunedFrequency, velocity / 127.0f, pBuf);
            }

            // Set per-note gain and pan
            pVoice->setGain(pBuf->gain);
            pVoice->setPan(pBuf->pan);

            lastPlayedNoteNumber = noteNumber;
        }
    }
    else // Polyphonic
    {
        // Iterate through all matching samples and start a voice for each
        for (DunneCore::KeyMappedSampleBuffer *pBuf : samples) {
            if (pBuf == nullptr) continue;

            // Apply detune in cents to calculate final frequency
            float detuneFactor = powf(2.0f, pBuf->noteDetune / 1200.0f);
            float detunedFrequency = noteFrequency * detuneFactor;

            for (int i = 0; i < MAX_POLYPHONY; i++) {
                DunneCore::SamplerVoice *pVoice = &data->voice[i];
                if (pVoice->noteNumber < 0) {
                    pVoice->start(noteNumber, currentSampleRate, detunedFrequency, velocity / 127.0f, pBuf);

                    // Set per-note gain and pan
                    pVoice->setGain(pBuf->gain);
                    pVoice->setPan(pBuf->pan);  // Set the pan value

                    break;  // Only one voice per sample
                }
            }
        }
    }
}


void CoreSampler::stop(unsigned noteNumber, bool immediate)
{
    if (isMonophonic) {
        DunneCore::SamplerVoice *pVoice = &data->voice[0];  // Only one voice for monophonic
        if (pVoice->noteNumber == noteNumber) {
            if (immediate) {
                pVoice->stop();  // Immediately stop the voice
            } else {
                pVoice->release(loopThruRelease);  // Release the note
            }
        }
    }
    else {
        // Polyphonic: stop all voices playing the note
        for (int i = 0; i < MAX_POLYPHONY; i++) {
            DunneCore::SamplerVoice *pVoice = &data->voice[i];
            if (pVoice->noteNumber == noteNumber) {
                if (immediate) {
                    pVoice->stop();  // Immediately stop the voice
                } else {
                    pVoice->release(loopThruRelease);  // Release the note and allow it to decay
                }
            }
        }
    }
}


void CoreSampler::stopAllVoices()
{
    // Lock out starting any new notes, and tell Render() to stop all active notes
    stoppingAllVoices = true;
    
    // Wait until Render() has killed all active notes
    bool noteStillSounding = true;
    while (noteStillSounding)
    {
        noteStillSounding = false;
        for (int i=0; i < MAX_POLYPHONY; i++)
            if (data->voice[i].noteNumber >= 0) noteStillSounding = true;
    }
}

void CoreSampler::restartVoices()
{
    // Allow starting new notes again
    stoppingAllVoices = false;
}

void CoreSampler::render(unsigned channelCount, unsigned sampleCount, float *outBuffers[])
{
    float *pOutLeft = outBuffers[0];
    float *pOutRight = outBuffers[1];

    // Clear output buffers
    for (unsigned i = 0; i < sampleCount; i++) {
        pOutLeft[i] = 0.0f;
        pOutRight[i] = 0.0f;
    }

    // Set the global LFO frequency
    data->globalLFO.setFrequency(lfoRate);

    // Get the current value from the global LFO
    float globalLFOValue = data->globalLFO.getSample() * lfoDepth;

    // Process each voice (polyphonic voices)
    for (int i = 0; i < MAX_POLYPHONY; i++)
    {
        DunneCore::SamplerVoice *pVoice = &data->voice[i];
        if (pVoice->noteNumber >= 0)
        {
            // Call the existing voice rendering logic
            bool shouldStop = pVoice->prepToGetSamples(sampleCount, masterVolume, pitchOffset, cutoffMultiple,
                                                       keyTracking, cutoffEnvelopeStrength, filterEnvelopeVelocityScaling,
                                                       linearResonance, pitchADSRSemitones, voiceVibratoDepth, voiceVibratoFrequency,
                                                       globalLFOValue, lfoTargetPitchToggle, lfoTargetGainToggle, lfoTargetFilterToggle);

            // If the voice is done, stop it
            if (shouldStop) {
                stopNote(pVoice->noteNumber, true);
            } else {
                pVoice->getSamples(sampleCount, pOutLeft, pOutRight);
            }
        }
    }

    // Apply overall gain and pan (after processing voices)
    float overallGainLinear = powf(10.0f, overallGain / 20.0f);
    float leftPan = (overallPan <= 0.0f) ? 1.0f : (1.0f - overallPan);
    float rightPan = (overallPan >= 0.0f) ? 1.0f : (1.0f + overallPan);
    
    for (unsigned i = 0; i < sampleCount; i++) {
        float leftValue = pOutLeft[i] * overallGainLinear * leftPan;
        float rightValue = pOutRight[i] * overallGainLinear * rightPan;
        pOutLeft[i] = leftValue;
        pOutRight[i] = rightValue;
    }
}

void  CoreSampler::setADSRAttackDurationSeconds(float value) __attribute__((no_sanitize("thread")))
{
    data->ampEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRAttackDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getAttackDurationSeconds();
}

void  CoreSampler::setADSRHoldDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setHoldDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRHoldDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getHoldDurationSeconds();
}

void  CoreSampler::setADSRDecayDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRDecayDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getDecayDurationSeconds();
}

void  CoreSampler::setADSRSustainFraction(float value)
{
    data->ampEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRSustainFraction(void)
{
    return data->ampEnvelopeParameters.sustainFraction;
}

void  CoreSampler::setADSRReleaseHoldDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setReleaseHoldDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRReleaseHoldDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getReleaseHoldDurationSeconds();
}

void  CoreSampler::setADSRReleaseDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRReleaseDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getReleaseDurationSeconds();
}

void  CoreSampler::setFilterAttackDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterAttackDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getAttackDurationSeconds();
}

void  CoreSampler::setFilterDecayDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterDecayDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getDecayDurationSeconds();
}

void  CoreSampler::setFilterSustainFraction(float value)
{
    data->filterEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterSustainFraction(void)
{
    return data->filterEnvelopeParameters.sustainFraction;
}

void  CoreSampler::setFilterReleaseDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterReleaseDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getReleaseDurationSeconds();
}


void  CoreSampler::setPitchAttackDurationSeconds(float value)
{
    data->pitchEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchAttackDurationSeconds(void)
{
    return data->pitchEnvelopeParameters.getAttackDurationSeconds();
}

void  CoreSampler::setPitchDecayDurationSeconds(float value)
{
    data->pitchEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchDecayDurationSeconds(void)
{
    return data->pitchEnvelopeParameters.getDecayDurationSeconds();
}

void  CoreSampler::setPitchSustainFraction(float value)
{
    data->pitchEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchSustainFraction(void)
{
    return data->pitchEnvelopeParameters.sustainFraction;
}

void  CoreSampler::setPitchReleaseDurationSeconds(float value)
{
    data->pitchEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchReleaseDurationSeconds(void)
{
    return data->pitchEnvelopeParameters.getReleaseDurationSeconds();
}
