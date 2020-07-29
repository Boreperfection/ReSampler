﻿/*
* Copyright (C) 2020 Judd Niemann - All Rights Reserved.
* You may use, distribute and modify this code under the
* terms of the GNU Lesser General Public License, version 2.1
*
* You should have received a copy of GNU Lesser General Public License v2.1
* with this file. If not, please refer to: https://github.com/jniemann66/ReSampler
*/

#ifndef MPXDECODE_H
#define MPXDECODE_H

// FM Broadcast Multiplex Decoder (WIP)

#include <vector>
#include <sndfile.h>
#include <sndfile.hh>

#include "FIRFilter.h"
#include "biquad.h"

// #define MPXDECODER_TUNE_PILOT_AGC

// NCO : numerically - controlled oscillator
class NCO
{
public:
	NCO(int sampleRate, double frequency = 19000) : sampleRate(sampleRate),
		filterI(0.0009357513270600214, 0.0018715026541200428, 0.0009357513270600214, -1.8931095931212278, 0.896852598429468),
		filterQ(0.0009357513270600214, 0.0018715026541200428, 0.0009357513270600214, -1.8931095931212278, 0.896852598429468)
	{
		setFrequency(frequency);
		maxJump = 2.0 * angularFreq;
	}

	void sync(double input)
	{
		std::complex<double> theirs{filterI.filter(localI * input), filterQ.filter(localQ * input)};
		double phaseDiff = -std::arg(theirs);

		if(std::abs(phaseDiff > (2 * M_PI * 0.01))) {
			phase += std::max(-maxJump, std::min(phaseDiff * 0.01, maxJump));
			if(phase > M_PI) {
				phase -= 2 * M_PI;
			} else if (phase < - M_PI) {
				phase += 2 * M_PI;
			}
		}

		//	std::cout << 360.0 * phaseDiff / (2* M_PI) << "\n";
	}

	double get() {
		localQ = std::sin(theta + phase);
		localI = std::cos(theta + phase);
		theta += angularFreq;

		if(theta > M_PI) {
			theta -= 2 * M_PI;
		}
		return localI;
	}

	double getFrequency() const
	{
		return sampleRate * angularFreq / (2 * M_PI);
	}

	void setFrequency(double value)
	{
		angularFreq = (2 * M_PI * value) / sampleRate;
	}

	static void saveFilters1(const std::string& filename)
	{
		SndfileHandle sndfile(filename, SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 1, 192000);
		std::vector<double> impulseResponse(10000, 0.0);
		ReSampler::Biquad<double> filt(0.019305318724235306, 0.03861063744847061, 0.019305318724235306, -1.5005428941316463, 0.5777641690285875);

		impulseResponse[100] = filt.filter(1.0);
		for(int i = 100; i < impulseResponse.size(); i++)
		{
			impulseResponse[i] = filt.filter(0.0);
		}

		sndfile.writef(impulseResponse.data(), impulseResponse.size());
	}

	double getPhase() const
	{
		return phase;
	}

	void setPhase(double value)
	{
		phase = value;
	}

private:
	int sampleRate;
	ReSampler::Biquad<double> filterI;
	ReSampler::Biquad<double> filterQ;

	double maxJump;
	double angularFreq;
	double theta{0.0};
	double localI{1.0}; // todo: starting positions ?
	double localQ{0.0};
	double phase{0.0};
};

class MpxDecoder
{
public:
	MpxDecoder(int sampleRate) : nco(sampleRate)
	{
		// test only:  write impulse response of IIR filter to file for evaluation
		// NCO::saveFilters1("e:\\t\\iir.wav");

		// create filters
		auto f0 = make19KhzBandpass<double>(sampleRate);
		auto f1 = make38KhzBandpass<double>(sampleRate);
		auto f2 = make57KhzBandpass<double>(sampleRate);
		auto lpf = make15khzLowpass<double>(sampleRate);

		length = f0.size();
		delayLine.resize(length, 0.0);
		centerTap = (length - 1) / 2;
		currentIndex = length - 1;

		filters.emplace_back(f0.data(), f0.size());
		filters.emplace_back(f1.data(), f1.size());
		filters.emplace_back(f2.data(), f2.size());
		filters.emplace_back(lpf.data(), lpf.size()); // left lowpass
		filters.emplace_back(lpf.data(), lpf.size()); // right lowpass

		// these values determined experimentally:
		// (#define MPXDECODER_TUNE_PILOT_AGC to debug & tweak)
		decreaseRate = std::pow(10.0, /* dB per sec = */ -12.0 / sampleRate / 20.0);
		increaseRate = std::pow(10.0, 64.0 / sampleRate / 20.0);
        peakDecayRate = std::pow(10.0, /* dB per sec = */ -12.0 / sampleRate / 20.0);
		setStereoWidth(0.5);

	}

#ifdef MPXDECODER_TUNE_PILOT_AGC
	~MpxDecoder()
	{
		int64_t totalCount = plusCount + stableCount + minusCount;
		std::cout << 100.0 * plusCount / totalCount << "%, "
				  << 100.0 * stableCount / totalCount << "%, "
				  << 100.0 * minusCount / totalCount << "%" << std::endl;

        std::cout << "Peak Pilot Gain: " << peakPilotGain << std::endl;
	}
#endif

	template<typename FloatType>
	std::pair<FloatType, FloatType> decode(FloatType input)
	{
		delayLine[currentIndex] = input; // place input into history
		if(currentIndex == 0) {
			currentIndex = length - 1;
		} else {
			currentIndex--;
		}
		int d = currentIndex + centerTap;
		FloatType mono = delayLine[d >= length ? d - length : d];

		filters.at(0).put(input);
		filters.at(1).put(input);

		FloatType pilotRaw = filters.at(0).get();
		FloatType sideRaw = filters.at(1).get();
		FloatType pilot = pilotRaw * pilotGain;
		FloatType pilotAbs = std::fabs(pilot);

		if(pilotAbs > pilotPeak) {
			pilotPeak = pilotAbs;
		}

#ifdef MPXDECODER_TUNE_PILOT_AGC
  //      std::cout << pilotGain << ", " << pilotPeak << "\n";
#endif

		if(pilotPeak < pilotStableLow) { // pilot too quiet
			pilotGain *= increaseRate;
			if(pilotGain >= pilotMaxGain) {
				pilotGain = pilotMaxGain;
			}

#ifdef MPXDECODER_TUNE_PILOT_AGC
            std::cout << "+";
			plusCount++;
#endif

		} else if(pilotPeak > pilotStableHigh) { // pilot too loud
			pilotGain *= decreaseRate;
			pilotPeak *= decreaseRate;


#ifdef MPXDECODER_TUNE_PILOT_AGC
            std::cout << "-";
			minusCount++;
#endif

		} else { // stable pilot tone

#ifdef MPXDECODER_TUNE_PILOT_AGC
			stableCount++;
#endif

		}

#ifdef MPXDECODER_TUNE_PILOT_AGC
		peakPilotGain = std::max(pilotGain, peakPilotGain);
#endif
		double p = nco.get();
		nco.sync(pilot);
		FloatType doubledPilot = 2 * p * p - 1.0;

		// do the spectrum shift
		constexpr double scaling = 2.5 * 2; // 10.0
		FloatType side = scaling * doubledPilot * sideRaw;

		// separate L, R stereo channels
		FloatType left = stereoGain * (mono + stereoWidth * side);
		FloatType right = stereoGain * (mono - stereoWidth * side);

		//       std::cout << pilot << ", " << pilotPeak << ", " << pilotGain << "\n";

		if(!lowpassEnabled) {
			return {left, right};
		}

		// filter & return outputs
		//		filters.at(3).put(left);
		//		filters.at(4).put(right);
		filters.at(3).put(mono);
		filters.at(4).put(mono);

		return {filters.at(3).get(), filters.at(4).get()};
	}

	template <typename FloatType>
	static std::vector<FloatType> makeBandpass(int sampleRate, double ft1, double ft2)
	{
		// determine cutoff frequency and steepness
		double nyquist = sampleRate / 2.0;
		double steepness = 0.090909091 / (2000.0 / nyquist);

		// determine filtersize
		int filterSize = static_cast<int>(
					std::min<int>(FILTERSIZE_BASE * steepness, FILTERSIZE_LIMIT)
					| 1 // ensure that filter length is always odd
					);

		// lower transition
		std::vector<FloatType> filterTaps1(filterSize, 0);
		FloatType* pFilterTaps1 = &filterTaps1[0];
		ReSampler::makeLPF<FloatType>(pFilterTaps1, filterSize, ft1, sampleRate);

		// upper transition
		std::vector<FloatType> filterTaps2(filterSize, 0);
		FloatType* pFilterTaps2 = &filterTaps2[0];
		ReSampler::makeLPF<FloatType>(pFilterTaps2, filterSize, ft2, sampleRate);

		// make bandpass
		for(int i = 0; i < filterSize; i++) {
			filterTaps2[i] -= filterTaps1.at(i);
		}

		// determine sidelobe attenuation
		int sidelobeAtten = 60;
		ReSampler::applyKaiserWindow<FloatType>(pFilterTaps2, filterSize, ReSampler::calcKaiserBeta(sidelobeAtten));
		return filterTaps2;
	}

	// 15khz lowpass for audio output
	template<typename FloatType>
	static std::vector<FloatType> make15khzLowpass(int sampleRate)
	{
		// determine filter steepness
		double nyquist = sampleRate / 2.0;
		double steepness = 0.090909091 / (lpfW / nyquist);

		// determine filtersize
		int filterSize = static_cast<int>(
					std::min<int>(FILTERSIZE_BASE * steepness, FILTERSIZE_LIMIT)
					| 1 // ensure that filter length is always odd
					);

		std::vector<FloatType> filterTaps1(filterSize, 0);
		FloatType* pFilterTaps1 = &filterTaps1[0];
		ReSampler::makeLPF<FloatType>(pFilterTaps1, filterSize, lpfT, sampleRate);
		int sidelobeAtten = 160;
		ReSampler::applyKaiserWindow<FloatType>(pFilterTaps1, filterSize, ReSampler::calcKaiserBeta(sidelobeAtten));
		return filterTaps1;
	}

	// 19khz bandpass filter for the Pilot Tone
	template<typename FloatType>
	static std::vector<FloatType> make19KhzBandpass(int sampleRate)
	{
		return makeBandpass<FloatType>(sampleRate, 18900, 19100);
	}

	// 38khz bandpass filter for the Audio Subcarrier
	template<typename FloatType>
	static std::vector<FloatType> make38KhzBandpass(int sampleRate)
	{
		return makeBandpass<FloatType>(sampleRate, 23000, 53000);
	}

	// 57khz bandpass filter for RDS / RBDS
	template<typename FloatType>
	static std::vector<FloatType> make57KhzBandpass(int sampleRate)
	{
		return makeBandpass<FloatType>(sampleRate, 54000, 60000);
	}

	// function for testing / tweaking the performance of the bandpass filters
	static void saveFilters1(const std::string& filename)
	{
		std::vector<double> filt1 = make19KhzBandpass<double>(192000);
		std::vector<double> filt2 = make38KhzBandpass<double>(192000);
		SndfileHandle sndfile(filename, SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 2, 192000);
		std::cout << "filter size " << filt1.size() << std::endl;
		std::vector<double> interleaved;
		interleaved.reserve(2 * filt1.size());
		for(int i = 0; i < filt1.size(); i++) {
			interleaved.push_back(filt1.at(i));
			interleaved.push_back(filt2.at(i));
		}
		sndfile.writef(interleaved.data(), filt1.size());
	}

	static double getLpfT()
	{
		return lpfT;
	}

	static double getLpfW()
	{
		return lpfW;
	}

	bool getLowpassEnabled() const
	{
		return lowpassEnabled;
	}

	void setLowpassEnabled(bool value)
	{
		lowpassEnabled = value;
	}

	double getStereoWidth() const
	{
		return stereoWidth;
	}

	void setStereoWidth(double value)
	{
		stereoWidth = value;
		stereoGain = 1.0 / (1.0 + value);
	}

private:
	std::vector<ReSampler::FIRFilter<double>> filters;
	NCO nco; // PLL local oscillator

	static constexpr double lpfT = 15500.0;	// LPF transition freq (Hz)
	static constexpr double lpfW = 3500.0;	// LPF transition width (Hz)
	static constexpr double pilotStableLow = 0.95;
	static constexpr double pilotStableHigh = 1.05;

	// if more gain than this is needed, then something is wrong with the Pilot Tone:
    static constexpr double pilotMaxGain = 40.0;

	std::vector<double> delayLine;
	int length;
	int currentIndex;
	int centerTap;

	bool lowpassEnabled{true}; // do final stereo 15khz LPF or not ?
	double pilotPeak{0.0};
	double pilotGain{1.0};
	double increaseRate;
	double decreaseRate;
    double peakDecayRate;
	double stereoWidth;
	double stereoGain;

#ifdef MPXDECODER_TUNE_PILOT_AGC
	int64_t plusCount{0};
	int64_t minusCount{0};
	int64_t stableCount{0};
	double peakPilotGain{0.0};
#endif

};

#endif // MPXDECODE_H
