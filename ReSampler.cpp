/*
* Copyright (C) 2016 - 2017 Judd Niemann - All Rights Reserved
* You may use, distribute and modify this code under the
* terms of the GNU Lesser General Public License, version 2.1
*
* You should have received a copy of GNU Lesser General Public License v2.1
* with this file. If not, please refer to: https://github.com/jniemann66/ReSampler
*/

// ReSampler.cpp : Audio Sample Rate Converter by Judd Niemann

#if defined(_DEBUG) && defined(_MSC_VER)
// use this to trap floating-point exceptions (MSVC: compile with /fp:strict)
#include <float.h>
unsigned int fp_control_state = _controlfp(_EM_INEXACT, _MCW_EM);
#endif

#define _USE_MATH_DEFINES
#include <math.h>
#include <iostream>
#include <ostream>
#include <cassert>
#include <vector>
#include <algorithm>
#include <memory>
#include <iomanip>
#include <mutex>
#include <cstdio>
#include <cstring>

#include "osspecific.h"
#include "ctpl/ctpl_stl.h"
#include "ReSampler.h"
#include "FIRFilter.h"
#include "ditherer.h"
#include "biquad.h"
#include "dsf.h"
#include "dff.h"
#include "raiitimer.h"
#include "convertstage.h"

////////////////////////////////////////////////////////////////////////////////////////
// This program uses the following libraries:
// 1:
// libsndfile
// available at http://www.mega-nerd.com/libsndfile/
//
// (copy of entire package included in $(ProjectDir)\libsbdfile)
// 
// 2:
// fftw
// http://www.fftw.org/
// 
#include "sndfile.hh"
//                                                                                    //
////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char * argv[])
{
	ConversionInfo ci;
	
	// result of parseParameters() indicates whether to terminate, and 
	// badParams indicates whether there was an error:
	bool badParams = false;
	if (!parseParameters(ci, badParams, argc, argv))
		exit(badParams ? EXIT_FAILURE : EXIT_SUCCESS);

	if (!showBuildVersion())
		exit(EXIT_FAILURE); // can't continue (CPU / build mismatch)

	// echo filenames to user
	std::cout << "Input file: " << ci.inputFilename << std::endl;
	std::cout << "Output file: " << ci.outputFilename << std::endl;

	// Isolate the file extensions
	std::string inFileExt;
	std::string outFileExt;
	if (ci.inputFilename.find_last_of(".") != std::string::npos)
		inFileExt = ci.inputFilename.substr(ci.inputFilename.find_last_of(".") + 1);
	if (ci.outputFilename.find_last_of(".") != std::string::npos)
		outFileExt = ci.outputFilename.substr(ci.outputFilename.find_last_of(".") + 1);

	// detect dsf or dff format
	ci.dsfInput = (inFileExt == "dsf");
	ci.dffInput = (inFileExt == "dff");

	if (!ci.outBitFormat.empty()) {  // new output bit format requested
		ci.outputFormat = determineOutputFormat(outFileExt, ci.outBitFormat);
		if (ci.outputFormat)
			std::cout << "Changing output bit format to " << ci.outBitFormat << std::endl;
		else { // user-supplied bit format not valid; try choosing appropriate format
			determineBestBitFormat(ci.outBitFormat, ci.inputFilename, ci.outputFilename);
			if (ci.outputFormat = determineOutputFormat(outFileExt, ci.outBitFormat))
				std::cout << "Changing output bit format to " << ci.outBitFormat << std::endl;
			else {
				std::cout << "Warning: NOT Changing output file bit format !" << std::endl;
				ci.outputFormat = 0; // back where it started
			}
		}
	}

	if (outFileExt != inFileExt)
	{ // file extensions differ, determine new output format: 

		if (ci.outBitFormat.empty()) { // user changed file extension only. Attempt to choose appropriate output sub format:
			std::cout << "Output Bit Format not specified" << std::endl;
			determineBestBitFormat(ci.outBitFormat, ci.inputFilename, ci.outputFilename);
		}
		ci.outputFormat = determineOutputFormat(outFileExt, ci.outBitFormat);
		if (ci.outputFormat)
			std::cout << "Changing output file format to " << outFileExt << std::endl;
		else { // cannot determine subformat of output file
			std::cout << "Warning: NOT Changing output file format ! (extension different, but format will remain the same)" << std::endl;
		}
	}

	try {
		if (ci.bUseDoublePrecision) {
			std::cout << "Using double precision for calculations." << std::endl;
			if (ci.dsfInput) {
				return convertMT<DsfFile, double>(ci, /* peakDetection = */ false) ? EXIT_SUCCESS : EXIT_FAILURE;
			}
			else if (ci.dffInput) {
				return convertMT<DffFile, double>(ci, /* peakDetection = */ false) ? EXIT_SUCCESS : EXIT_FAILURE;
			}
			else {
				return convertMT<SndfileHandle, double>(ci) ? EXIT_SUCCESS : EXIT_FAILURE;
			}
		}
		else {
			if (ci.dsfInput) {
				return convertMT<DsfFile, float>(ci, /* peakDetection = */ false) ? EXIT_SUCCESS : EXIT_FAILURE;
			}
			else if (ci.dffInput) {
				return convertMT<DffFile, float>(ci, /* peakDetection = */ false) ? EXIT_SUCCESS : EXIT_FAILURE;
			}
			else {
				return convertMT<SndfileHandle, float>(ci) ? EXIT_SUCCESS : EXIT_FAILURE;
			}
		}
	}
		
	catch (const std::exception& e) {
		std::cerr << "fatal error: " << e.what();
		return EXIT_FAILURE;
	}
}

// parseParameters()
// Return value indicates whether caller should continue execution (ie true: continue, false: terminate)
// Some commandline options (eg --version) should result in termination, but not error.
// unacceptable parameters are indicated by setting bBadParams to true

bool parseParameters(ConversionInfo& ci, bool& bBadParams, int argc, char* argv[]) {
	// initialize defaults:
	ci.inputFilename.clear();
	ci.outputFilename.clear();
	ci.outBitFormat.clear();
	ci.outputFormat = 0;
	ci.outputSampleRate = 44100;
	ci.normalizeAmount = 1.0;
	ci.ditherAmount = 1.0;
	ci.flacCompressionLevel = 5;
	ci.vorbisQuality = 3;
	ci.ditherProfileID = DitherProfileID::standard;

	//////////////////////////////////////////////////////////////
	// terminating switch options: // 

	// help switch:
	if (findCmdlineOption(argv, argv + argc, "--help") || findCmdlineOption(argv, argv + argc, "-h")) {
		std::cout << strUsage << std::endl;
		std::cout << "Additional options:\n\n" << strExtraOptions << std::endl;
		return false;
	}

	// version switch:
	if (findCmdlineOption(argv, argv + argc, "--version")) {
		std::cout << strVersion << std::endl;
		return false;
	}

	if (findCmdlineOption(argv, argv + argc, "--compiler")) {
		showCompiler();
		return false;
	}

	// sndfile-version switch:
	if (findCmdlineOption(argv, argv + argc, "--sndfile-version")) {
		char s[128];
		sf_command(nullptr, SFC_GET_LIB_VERSION, s, sizeof(s));
		std::cout << s << std::endl;
		return false;
	}
	
	// listsubformats
	if (findCmdlineOption(argv, argv + argc, "--listsubformats")) {
		std::string filetype;
		getCmdlineParam(argv, argv + argc, "--listsubformats", filetype);
		listSubFormats(filetype);
		return false;
	}

	// showDitherProfiles
	if (findCmdlineOption(argv, argv + argc, "--showDitherProfiles")) {
		showDitherProfiles();
		return false;
	}

	////////////////////////////////////////////////////////////////////
	// core parameters:
	getCmdlineParam(argv, argv + argc, "-i", ci.inputFilename);
	getCmdlineParam(argv, argv + argc, "-o", ci.outputFilename);
	getCmdlineParam(argv, argv + argc, "-r", ci.outputSampleRate);
	getCmdlineParam(argv, argv + argc, "-b", ci.outBitFormat);

	// double precision switch:
	ci.bUseDoublePrecision = findCmdlineOption(argv, argv + argc, "--doubleprecision");

	// gain
	if (findCmdlineOption(argv, argv + argc, "--gain")) {
		getCmdlineParam(argv, argv + argc, "--gain", ci.gain);
	}
	else {
		ci.gain = 1.0; // default
	}

	// normalize option and parameter:
	ci.bNormalize = findCmdlineOption(argv, argv + argc, "-n");
	if (ci.bNormalize) {
		getCmdlineParam(argv, argv + argc, "-n", ci.normalizeAmount);
		if (ci.normalizeAmount <= 0.0)
			ci.normalizeAmount = 1.0;
		if (ci.normalizeAmount > 1.0)
			std::cout << "\nWarning: Normalization factor greater than 1.0 - THIS WILL CAUSE CLIPPING !!\n" << std::endl;
		ci.limit = ci.normalizeAmount;
	}
	else {
		ci.limit = 1.0; // default
	}

	// dither option and parameter:
	ci.bDither = findCmdlineOption(argv, argv + argc, "--dither");
	if (ci.bDither) {
		getCmdlineParam(argv, argv + argc, "--dither", ci.ditherAmount);
		if (ci.ditherAmount <= 0.0)
			ci.ditherAmount = 1.0;
	}

	// auto-blanking option (for dithering):
	ci.bAutoBlankingEnabled = findCmdlineOption(argv, argv + argc, "--autoblank");

	// ns option to determine dither Profile:
	if (findCmdlineOption(argv, argv + argc, "--ns")) {
		getCmdlineParam(argv, argv + argc, "--ns", ci.ditherProfileID);
		if (ci.ditherProfileID < 0)
			ci.ditherProfileID = 0;
		if (ci.ditherProfileID >= DitherProfileID::end)
			ci.ditherProfileID = getDefaultNoiseShape(ci.outputSampleRate);
	}
	else {
		ci.ditherProfileID = getDefaultNoiseShape(ci.outputSampleRate);
	}

	// --flat-tpdf option (takes precedence over --ns)
	if (findCmdlineOption(argv, argv + argc, "--flat-tpdf")) {
		ci.ditherProfileID = DitherProfileID::flat;
	}

	// seed option and parameter:
	ci.bUseSeed = findCmdlineOption(argv, argv + argc, "--seed");
	ci.seed = 0;
	if (ci.bUseSeed) {
		getCmdlineParam(argv, argv + argc, "--seed", ci.seed);
	}

	// delay trim (group delay compensation)
	ci.bDelayTrim = !findCmdlineOption(argv, argv + argc, "--noDelayTrim");

	// minimum-phase option:
	ci.bMinPhase = findCmdlineOption(argv, argv + argc, "--minphase");

	// flacCompression option and parameter:
	ci.bSetFlacCompression = findCmdlineOption(argv, argv + argc, "--flacCompression");
	if (ci.bSetFlacCompression) {
		getCmdlineParam(argv, argv + argc, "--flacCompression", ci.flacCompressionLevel);
		if (ci.flacCompressionLevel < 0)
			ci.flacCompressionLevel = 0;
		if (ci.flacCompressionLevel > 8)
			ci.flacCompressionLevel = 8;
	}

	// vorbisQuality option and parameter:
	ci.bSetVorbisQuality = findCmdlineOption(argv, argv + argc, "--vorbisQuality");
	if (ci.bSetVorbisQuality) {
		getCmdlineParam(argv, argv + argc, "--vorbisQuality", ci.vorbisQuality);
		if (ci.vorbisQuality < -1)
			ci.vorbisQuality = -1;
		if (ci.vorbisQuality > 10)
			ci.vorbisQuality = 10;
	}

	// noClippingProtection option:
	ci.disableClippingProtection = findCmdlineOption(argv, argv + argc, "--noClippingProtection");

	// default cutoff and transition width:
	ci.lpfMode = normal;
	ci.lpfCutoff = 100.0 * (10.0 / 11.0);
	ci.lpfTransitionWidth = 100.0 - ci.lpfCutoff;

	// relaxedLPF option:
	if (findCmdlineOption(argv, argv + argc, "--relaxedLPF")) {
		ci.lpfMode = relaxed;
		ci.lpfCutoff = 100.0 * (21.0 / 22.0);				// late cutoff
		ci.lpfTransitionWidth = 2 * (100.0 - ci.lpfCutoff); // wide transition (double-width)  
	}
	
	// steepLPF option:
	if (findCmdlineOption(argv, argv + argc, "--steepLPF")) {
		ci.lpfMode = steep;
		ci.lpfCutoff = 100.0 * (21.0 / 22.0);				// late cutoff
		ci.lpfTransitionWidth = 100.0 - ci.lpfCutoff;       // steep transition  
	}

	// custom LPF cutoff frequency:
	if (findCmdlineOption(argv, argv + argc, "--lpf-cutoff")) {
		getCmdlineParam(argv, argv + argc, "--lpf-cutoff", ci.lpfCutoff);
		ci.lpfMode = custom;
		ci.lpfCutoff = std::max(1.0, std::min(ci.lpfCutoff, 99.9));

		// custom LPF transition width:
		if (findCmdlineOption(argv, argv + argc, "--lpf-transition")) {
			getCmdlineParam(argv, argv + argc, "--lpf-transition", ci.lpfTransitionWidth);
		}
		else {
			ci.lpfTransitionWidth = 100 - ci.lpfCutoff; // auto mode
		}
		ci.lpfTransitionWidth = std::max(0.1, std::min(ci.lpfTransitionWidth, 400.0));
	}
	
	// multithreaded option:
	ci.bMultiThreaded = findCmdlineOption(argv, argv + argc, "--mt");

	// rf64 option:
	ci.bRf64 = findCmdlineOption(argv, argv + argc, "--rf64");

	// noPeakChunk option:
	ci.bNoPeakChunk = findCmdlineOption(argv, argv + argc, "--noPeakChunk");

	// noMetadata option:
	ci.bWriteMetaData = !findCmdlineOption(argv, argv + argc, "--noMetadata");

	// test for bad parameters:
	bBadParams = false;
	if (ci.outputFilename.empty()) {
		if (ci.inputFilename.empty()) {
			std::cout << "Error: Input filename not specified" << std::endl;
			bBadParams = true;
		}
		else {
			std::cout << "Output filename not specified" << std::endl;
			ci.outputFilename = ci.inputFilename;
			if (ci.outputFilename.find(".") != std::string::npos) {
				auto dot = ci.outputFilename.find_last_of(".");
				ci.outputFilename.insert(dot, "(converted)");
			}
			else {
				ci.outputFilename.append("(converted)");
			}
			std::cout << "defaulting to: " << ci.outputFilename << "\n" << std::endl;
		}
	}

	else if (ci.outputFilename == ci.inputFilename) {
		std::cout << "\nError: Input and Output filenames cannot be the same" << std::endl;
		bBadParams = true;
	}

	if (ci.outputSampleRate == 0) {
		std::cout << "Error: Target sample rate not specified" << std::endl;
		bBadParams = true;
	}

	if (bBadParams) {
		std::cout << strUsage << std::endl;
		return false;
	}
	return true;
}

// determineBestBitFormat() : determines the most appropriate bit format for the output file, through the following process:
// 1. Try to use infile's format and if that isn't valid for outfile, then:
// 2. use the default subformat for outfile.
// store best bit format as a string in BitFormat

bool determineBestBitFormat(std::string& BitFormat, const std::string& inFilename, const std::string& outFilename)
{
	// get infile's extension from filename:
	std::string inFileExt;
	if (inFilename.find_last_of(".") != std::string::npos)
		inFileExt = inFilename.substr(inFilename.find_last_of(".") + 1);

	bool dsfInput = false;
	bool dffInput = false;

	int inFileFormat;

	if (inFileExt == "dsf") {
		dsfInput = true;
	}
	else if (inFileExt == "dff") {
		dffInput = true;
	}

	else { // libsndfile-openable file

		// Inspect input file for format:
		SndfileHandle infile(inFilename, SFM_READ);
		inFileFormat = infile.format();

		if (int e = infile.error()) {
			std::cout << "Couldn't Open Input File (" << sf_error_number(e) << ")" << std::endl;
			return false;
		}

		// get BitFormat of inFile as a string:
		for (auto& subformat : subFormats) {
			if (subformat.second == (inFileFormat & SF_FORMAT_SUBMASK)) {
				BitFormat = subformat.first;
				break;
			}
		}

		// retrieve infile's TRUE extension (from the file contents), and if retrieval is successful, override extension derived from filename:
		SF_FORMAT_INFO infileFormatInfo;
		infileFormatInfo.format = inFileFormat & SF_FORMAT_TYPEMASK;
		if (sf_command(nullptr, SFC_GET_FORMAT_INFO, &infileFormatInfo, sizeof(infileFormatInfo)) == 0) {
			inFileExt = std::string(infileFormatInfo.extension);
		}
	}

	// get outfile's extension:
	std::string outFileExt;
	if (outFilename.find_last_of(".") != std::string::npos)
		outFileExt = outFilename.substr(outFilename.find_last_of(".") + 1);
	
	// when the input file is dsf/dff, use default output subformat:
	if (dsfInput || dffInput) { // choose default output subformat for chosen output file format
		BitFormat = defaultSubFormats.find(outFileExt)->second;
		std::cout << "defaulting to " << BitFormat << std::endl;
		return true;
	}

	// get total number of major formats:
	SF_FORMAT_INFO formatinfo;
	int format, major_count;
	memset(&formatinfo, 0, sizeof(formatinfo));
	sf_command(nullptr, SFC_GET_FORMAT_MAJOR_COUNT, &major_count, sizeof(int));

	// determine if inFile's subformat is valid for outFile:
	for (int m = 0; m < major_count; m++)
	{
		formatinfo.format = m;
		sf_command(nullptr, SFC_GET_FORMAT_MAJOR, &formatinfo, sizeof(formatinfo));

		if (stricmp(formatinfo.extension, outFileExt.c_str()) == 0) { // match between format number m and outfile's file extension
			format = formatinfo.format | (inFileFormat & SF_FORMAT_SUBMASK); // combine outfile's major format with infile's subformat
			
			// Check if format / subformat combination is valid:
			SF_INFO sfinfo;
			memset(&sfinfo, 0, sizeof(sfinfo));
			sfinfo.channels = 1;
			sfinfo.format = format;

			if (sf_format_check(&sfinfo)) { // Match: infile's subformat is valid for outfile's format
				break;
			} else { // infile's subformat is not valid for outfile's format; use outfile's default subformat
				std::cout << "Output file format " << outFileExt << " and subformat " << BitFormat << " combination not valid ... ";
				BitFormat.clear();
				BitFormat = defaultSubFormats.find(outFileExt)->second;
				std::cout << "defaulting to " << BitFormat << std::endl;
				break;
			}
		}
	} 
	return true;
}

// determineOutputFormat() : returns an integer representing the output format, which libsndfile understands:
int determineOutputFormat(const std::string& outFileExt, const std::string& bitFormat)
{
	SF_FORMAT_INFO info;
	int format = 0;
	int major_count;
	memset(&info, 0, sizeof(info));
	sf_command(nullptr, SFC_GET_FORMAT_MAJOR_COUNT, &major_count, sizeof(int));
	bool bFileExtFound = false;

	// Loop through all major formats to find match for outFileExt:
	for (int m = 0; m < major_count; ++m) {
		info.format = m;
		sf_command(nullptr, SFC_GET_FORMAT_MAJOR, &info, sizeof(info));
		if (stricmp(info.extension, outFileExt.c_str()) == 0) {
			bFileExtFound = true;
			break;
		}
	}

	if (bFileExtFound) {
		// Check if subformat is recognized:
		auto sf = subFormats.find(bitFormat);
		if (sf != subFormats.end())
			format = info.format | sf->second;
		else
			std::cout << "Warning: bit format " << bitFormat << " not recognised !" << std::endl;
	}

	// Special cases:
	if (bitFormat == "8") {
		// user specified 8-bit. Determine whether it must be unsigned or signed, based on major type:
		// These formats always use unsigned: 8-bit when they use 8-bit: mat rf64 voc w64 wav

		if ((outFileExt == "mat") || (outFileExt == "rf64") || (outFileExt == "voc") || (outFileExt == "w64") || (outFileExt == "wav"))
			format = info.format | SF_FORMAT_PCM_U8;
		else
			format = info.format | SF_FORMAT_PCM_S8;
	}

	return format;
}

// listSubFormats() - lists all valid subformats for a given file extension (without "." or "*."):
void listSubFormats(const std::string& f)
{
	SF_FORMAT_INFO	info;
	int format = 0;
	int major_count;
	memset(&info, 0, sizeof(info));
	sf_command(nullptr, SFC_GET_FORMAT_MAJOR_COUNT, &major_count, sizeof(int));
	bool bFileExtFound = false;

	// Loop through all major formats to find match for outFileExt:
	for (int m = 0; m < major_count; ++m) {
		info.format = m;
		sf_command(nullptr, SFC_GET_FORMAT_MAJOR, &info, sizeof(info));
		if (stricmp(info.extension, f.c_str()) == 0) {
			bFileExtFound = true;
			break;
		}
	}
	if (bFileExtFound) {
		SF_INFO sfinfo;
		memset(&sfinfo, 0, sizeof(sfinfo));
		sfinfo.channels = 1;

		// loop through all subformats and find which ones are valid for file type:
		for (auto& subformat : subFormats) {
			sfinfo.format = (info.format & SF_FORMAT_TYPEMASK) | subformat.second;
			if (sf_format_check(&sfinfo))
				std::cout << subformat.first << std::endl;
		}
	}
	else {
		std::cout << "File extension " << f << " unknown" << std::endl;
	}
}

template<typename FloatType>
std::vector<FloatType> makeFilterCoefficients(unsigned int inputSampleRate, const ConversionInfo& ci, Fraction fraction) {
	// determine base filter size
	int baseFilterSize;
	int overSamplingFactor = 1;
	Fraction f = fraction;
	if ((fraction.numerator != fraction.denominator) && (fraction.numerator <= 4 || fraction.denominator <= 4)) { // simple ratios
		baseFilterSize = FILTERSIZE_MEDIUM * std::max(fraction.denominator, fraction.numerator) / 2;
		if (ci.bMinPhase) { // oversample to improve filter performance
			overSamplingFactor = 8;
			f.numerator *= overSamplingFactor;
			f.denominator *= overSamplingFactor;
		}
	}
	else { // complex ratios
		baseFilterSize = FILTERSIZE_HUGE * std::max(fraction.denominator, fraction.numerator) / 320;
	}

	// determine cutoff frequency and steepness
	double targetNyquist = std::min(inputSampleRate, ci.outputSampleRate) / 2.0;
	double ft = (ci.lpfCutoff / 100.0) * targetNyquist;
	double steepness = steepness = 0.090909091 / (ci.lpfTransitionWidth / 100.0);

	// scale the filter size, according to selected options:
	int filterSize = std::min(static_cast<int>(overSamplingFactor * baseFilterSize * steepness), FILTERSIZE_LIMIT)
		| static_cast<int>(1);	// ensure that filter length is always odd

								// determine sidelobe attenuation
	int sidelobeAtten = ((fraction.numerator == 1) || (fraction.denominator == 1)) ?
		195 :
		160;

	// Make some filter coefficients:
	int overSampFreq = inputSampleRate * f.numerator;
	std::vector<FloatType> filterTaps(filterSize, 0);
	FloatType* pFilterTaps = &filterTaps[0];
	makeLPF<FloatType>(pFilterTaps, filterSize, ft, overSampFreq);
	applyKaiserWindow<FloatType>(pFilterTaps, filterSize, calcKaiserBeta(sidelobeAtten));

	// conditionally convert filter coefficients to minimum-phase:
	if (ci.bMinPhase) {
		makeMinPhase<FloatType>(pFilterTaps, filterSize);
	}

	return filterTaps;
}

// Multi-threaded convert() :

/* Note: type 'FileReader' MUST implement the following methods:
constuctor(const std::string& fileName)
bool error() // or int error()
unsigned int channels()
unsigned int samplerate()
uint64_t frames()
int format()
read(inbuffer, count)
seek(position, whence)
*/

template<typename FileReader, typename FloatType>
bool convertMT(const ConversionInfo& ci, bool peakDetection)
{
	bool multiThreaded = ci.bMultiThreaded;

	// Open input file:
	FileReader infile(ci.inputFilename);

	if (int e = infile.error()) {
		std::cout << "Error: Couldn't Open Input File (" << sf_error_number(e) << ")" << std::endl; // to-do: make this more specific (?)
		return false;
	}

	// read metadata:
	MetaData m;
	getMetaData(m, infile);
	
	// read file properties:
	int nChannels = infile.channels();
	unsigned int inputSampleRate = infile.samplerate();
	sf_count_t inputSampleCount = infile.frames() * nChannels;

	// determine conversion ratio:
	Fraction fOriginal = getSimplifiedFraction(inputSampleRate, ci.outputSampleRate);
	Fraction f = fOriginal;

	// set buffer sizes:
	size_t inputChannelBufferSize = BUFFERSIZE;
	size_t inputBlockSize = BUFFERSIZE * nChannels;
	size_t outputChannelBufferSize = std::ceil(BUFFERSIZE * static_cast<double>(f.numerator) / static_cast<double>(f.denominator));
	size_t outputBlockSize = nChannels * outputChannelBufferSize;
	
	// allocate buffers:
	std::vector<FloatType> inputBlock(inputBlockSize, 0);		// input buffer for storing interleaved samples from input file
	std::vector<FloatType> outputBlock(outputBlockSize, 0);		// output buffer for storing interleaved samples to be saved to output file
	std::vector<std::vector<FloatType>> inputChannelBuffers;	// input buffer for each channel to store deinterleaved samples 
	std::vector<std::vector<FloatType>> outputChannelBuffers;	// output buffer for each channel to store converted deinterleaved samples
	for (int n = 0; n < nChannels; n++) {
		inputChannelBuffers.emplace_back(std::vector<FloatType>(inputChannelBufferSize, 0));
		outputChannelBuffers.emplace_back(std::vector<FloatType>(outputChannelBufferSize, 0));
	}
			
	int inputFileFormat = infile.format();
	if (inputFileFormat != DFF_FORMAT && inputFileFormat != DSF_FORMAT) { // this block only relevant to libsndfile ...
		// detect if input format is a floating-point format:
		bool bFloat = false;
		bool bDouble = false;
		switch (inputFileFormat & SF_FORMAT_SUBMASK) {
		case SF_FORMAT_FLOAT:
			bFloat = true;
			break;
		case SF_FORMAT_DOUBLE:
			bDouble = true;
			break;
		}

		for (auto& subformat : subFormats) { // scan subformats for a match:
			if (subformat.second == (inputFileFormat & SF_FORMAT_SUBMASK)) {
				std::cout << "input bit format: " << subformat.first;
				break;
			}
		}

		if (bFloat)
			std::cout << " (float)";
		if (bDouble)
			std::cout << " (double precision)";

		std::cout << std::endl;
	}

	std::cout << "source file channels: " << nChannels << std::endl;
	std::cout << "input sample rate: " << inputSampleRate << "\noutput sample rate: " << ci.outputSampleRate << std::endl;

	sf_count_t samplesRead;
	sf_count_t totalSamplesRead = 0;
	FloatType peakInputSample;
	if (peakDetection) {
		peakInputSample = 0.0;
		std::cout << "Scanning input file for peaks ...";

		do {
			samplesRead = infile.read(inputBlock.data(), inputBlockSize);
			totalSamplesRead += samplesRead;
			for (unsigned int s = 0; s < samplesRead; ++s) { // read all samples, without caring which channel they belong to
				peakInputSample = std::max(peakInputSample, std::abs(inputBlock[s]));
			}
		} while (samplesRead > 0);

		std::cout << "Done\n";
		std::cout << "Peak input sample: " << std::fixed << peakInputSample << " (" << 20 * log10(peakInputSample) << " dBFS)" << std::endl;
		infile.seek(0, SEEK_SET); // rewind back to start of file
	}

	else { // no peak detection
		peakInputSample = ci.bNormalize ?
			0.5 /* ... a guess, since we haven't actually measured the peak (in the case of DSD, it is a good guess.) */ :
			1.0;
	}

	if (ci.bNormalize) { // echo Normalization settings to user
		auto prec = std::cout.precision();
		std::cout << "Normalizing to " << std::setprecision(2) << ci.limit << std::endl;
		std::cout.precision(prec);
	}

	// echo filter settings to user:
	double targetNyquist = std::min(inputSampleRate, ci.outputSampleRate) / 2.0;
	double ft = (ci.lpfCutoff / 100.0) * targetNyquist;
	auto prec = std::cout.precision();
	std::cout << "LPF transition frequency: " << std::fixed << std::setprecision(2) << ft << " Hz (" << 100 * ft / targetNyquist << " %)" << std::endl;
	std::cout.precision(prec);
	if (ci.bMinPhase) {
		std::cout << "Using Minimum-Phase LPF" << std::endl;
	}

	// calculate filter coefficients
	std::vector<FloatType> FilterTaps = std::move(makeFilterCoefficients<FloatType>(inputSampleRate, ci, fOriginal));

	// echo conversion ratio to user:
	FloatType resamplingFactor = static_cast<FloatType>(ci.outputSampleRate) / inputSampleRate;
	std::cout << "\nConversion ratio: " << resamplingFactor
		<< " (" << fOriginal.numerator << ":" << fOriginal.denominator << ")" << std::endl;

	// make a vector of filters (one filter for each channel):
	std::vector<FIRFilter<FloatType>> filters;
	for (int n = 0; n < nChannels; n++) {
		filters.emplace_back(FilterTaps.data(), FilterTaps.size());
	}

	//to-do: handle oversampled ratios (eg 8/8) - f vs fOriginal etc

	// calculate group Delay
	int groupDelay = (ci.bMinPhase || !ci.bDelayTrim) ? 0 : (FilterTaps.size() - 1) / 2 / fOriginal.denominator;
	if (fOriginal.numerator == 1 && fOriginal.denominator == 1) {
		groupDelay = 0;
	}

	// std::cout << "expected group delay " << groupDelay << std::endl;

	// if the outputFormat is zero, it means "No change to file format"
	// if output file format has changed, use outputFormat. Otherwise, use same format as infile: 
	int outputFileFormat = ci.outputFormat ? ci.outputFormat : inputFileFormat;

	// if the minor (sub) format of outputFileFormat is not set, attempt to use minor format of input file (as a last resort)
	if ((outputFileFormat & SF_FORMAT_SUBMASK) == 0) {
		outputFileFormat |= (inputFileFormat & SF_FORMAT_SUBMASK); // may not be valid subformat for new file format. 
	}

	// for wav files, determine whether to switch to rf64 mode:
	if (((outputFileFormat & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV) ||
		((outputFileFormat & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAVEX)) {
		if (ci.bRf64 ||
			checkWarnOutputSize(inputSampleCount, getSfBytesPerSample(outputFileFormat), fOriginal.numerator, fOriginal.denominator)) {
			std::cout << "Switching to rf64 format !" << std::endl;
			outputFileFormat &= ~SF_FORMAT_TYPEMASK; // clear file type
			outputFileFormat |= SF_FORMAT_RF64;
		}
	}

	// determine number of bits in output format (used for Dithering purposes):
	int outputSignalBits;
	switch (outputFileFormat & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_24:
		outputSignalBits = 24;
		break;
	case SF_FORMAT_PCM_S8:
	case SF_FORMAT_PCM_U8:
		outputSignalBits = 8;
		break;
	default:
		outputSignalBits = 16;
	}

	// confirm dithering options for user:
	if (ci.bDither) {
		auto prec = std::cout.precision();
		std::cout << "Generating " << std::setprecision(2) << ci.ditherAmount << " bits of " << ditherProfileList[ci.ditherProfileID].name << " dither for " << outputSignalBits << "-bit output format";
		std::cout.precision(prec);
		if (ci.bAutoBlankingEnabled)
			std::cout << ", with auto-blanking";
		std::cout << std::endl;
	}

	// make a vector of ditherers (one ditherer for each channel):
	std::vector<Ditherer<FloatType>> ditherers;
	int seed = ci.bUseSeed ? ci.seed : time(0);

	for (int n = 0; n < nChannels; n++) {
		// to-do: explore other seed-generation options (remote possibility of overlap)
		// maybe use a single global RNG ? 
		// or use discard/jump-ahead ... to ensure parallel streams are sufficiently "far away" from each other ?
		ditherers.emplace_back(outputSignalBits, ci.ditherAmount, ci.bAutoBlankingEnabled, n + seed, static_cast<DitherProfileID>(ci.ditherProfileID));
	}

	// Calculate initial gain:
	FloatType gain = ci.gain *
		(ci.bNormalize ? f.numerator * (ci.limit / peakInputSample) : f.numerator * ci.limit);

	if (ci.bDither) { // allow headroom for dithering:
		FloatType ditherCompensation =
			(pow(2, outputSignalBits - 1) - pow(2, ci.ditherAmount - 1)) / pow(2, outputSignalBits - 1); // eg 32767/32768 = 0.999969 (-0.00027 dB)
		gain *= ditherCompensation;
	}

	FloatType peakOutputSample;
	bool bClippingDetected;
	RaiiTimer timer;

	do { // clipping detection loop (repeat if clipping detected)

		bClippingDetected = false;
		std::unique_ptr<SndfileHandle> outFile;

		// make a vector of converter stages:
		bool bypassMode = (f.numerator == 1 && f.denominator == 1);
		std::vector<ConvertStage<FloatType>> convertStages;
		for (int n = 0; n < nChannels; n++) {
			convertStages.emplace_back(f.numerator, f.denominator, filters[n], bypassMode);
		}

		try { // Open output file:

			// output file may need to be overwriten on subsequent passes,
			// and the only way to close the file is to destroy the SndfileHandle.  

			outFile.reset(new SndfileHandle(ci.outputFilename, SFM_WRITE, outputFileFormat, nChannels, ci.outputSampleRate));

			if (int e = outFile->error()) {
				std::cout << "Error: Couldn't Open Output File (" << sf_error_number(e) << ")" << std::endl;
				return false;
			}

			if (ci.bNoPeakChunk) {
				outFile->command(SFC_SET_ADD_PEAK_CHUNK, nullptr, SF_FALSE);
			}

			if (ci.bWriteMetaData) {
				if (!setMetaData(m, *outFile)) {
					std::cout << "Warning: problem writing metadata to output file ( " << outFile->strError() << " )" << std::endl;
				}
			}

			// if the minor (sub) format of outputFileFormat is flac, and user has requested a specific compression level, set compression level:
			if (((outputFileFormat & SF_FORMAT_FLAC) == SF_FORMAT_FLAC) && ci.bSetFlacCompression) {
				std::cout << "setting flac compression level to " << ci.flacCompressionLevel << std::endl;
				double cl = static_cast<double>(ci.flacCompressionLevel / 8.0); // there are 9 flac compression levels from 0-8. Normalize to 0-1.0
				outFile->command(SFC_SET_COMPRESSION_LEVEL, &cl, sizeof(cl));
			}

			// if the minor (sub) format of outputFileFormat is vorbis, and user has requested a specific quality level, set quality level:
			if (((outputFileFormat & SF_FORMAT_VORBIS) == SF_FORMAT_VORBIS) && ci.bSetVorbisQuality) {

				auto prec = std::cout.precision();
				std::cout.precision(1);
				std::cout << "setting vorbis quality level to " << ci.vorbisQuality << std::endl;
				std::cout.precision(prec);

				double cl = static_cast<double>((1.0 - ci.vorbisQuality) / 11.0); // Normalize from (-1 to 10), to (1.0 to 0) ... why is it backwards ?
				outFile->command(SFC_SET_COMPRESSION_LEVEL, &cl, sizeof(cl));
			}
		}

		catch (std::exception& e) {
			std::cout << "Error: Couldn't Open Output File " << e.what() << std::endl;
			return false;
		}

		if (multiThreaded) {
			std::cout << "Converting (multi-threaded) ...";
		}
		else {
			std::cout << "Converting ...";
		}

		peakOutputSample = 0.0;
		totalSamplesRead = 0;
		sf_count_t incrementalProgressThreshold = inputSampleCount / 10;
		sf_count_t nextProgressThreshold = incrementalProgressThreshold;

		int outStartOffset = std::min(groupDelay * nChannels, static_cast<int>(outputBlockSize) - nChannels);
//		size_t count; // to-do: remove ?
		do {
			// Grab a block of interleaved samples from file:
			samplesRead = infile.read(inputBlock.data(), inputBlockSize);
			totalSamplesRead += samplesRead;

			// de-interleave into channel buffers
			size_t i = 0;
			for (size_t s = 0 ; s < samplesRead; s += nChannels) {
				for (int ch = 0 ; ch < nChannels; ++ch) {
					inputChannelBuffers[ch][i] = inputBlock[s+ch];
				}
				++i;
			}

			typedef struct {
				size_t outBlockindex;
				FloatType peak;
			} Result;

			std::vector<std::future<Result>> results(nChannels);
			ctpl::thread_pool threadPool(nChannels);
			size_t outputBlockIndex;
			
			for (int ch = 0; ch < nChannels; ++ch) { // run convert stage for each channel (concurrently)

				auto kernel = [&, ch](int x = 0) {
					FloatType* iBuf = inputChannelBuffers[ch].data();
					FloatType* oBuf = outputChannelBuffers[ch].data();
					size_t o = 0;
					FloatType localPeak = 0.0;
					size_t localOutputBlockIndex = 0;
					convertStages[ch].convert(oBuf, o, iBuf, i);
					for (size_t f = 0; f < o; ++f) {
						FloatType outputSample = ci.bDither ? ditherers[ch].dither(gain * oBuf[f]) : gain * oBuf[f]; // gain, dither
						localPeak = std::max(localPeak, std::abs(outputSample)); // peak
						outputBlock[localOutputBlockIndex + ch] = outputSample; // interleave
						localOutputBlockIndex += nChannels;
					}
					Result res;
					res.outBlockindex = localOutputBlockIndex;
					res.peak = localPeak;
					return res;
				};

				if (multiThreaded) {
					results[ch] = threadPool.push(kernel);
				}
				else {
					Result res = kernel();
					peakOutputSample = std::max(peakOutputSample, res.peak);
					outputBlockIndex = res.outBlockindex;
				}
			}

			if (multiThreaded) { // collect results:
				for (int ch = 0; ch < nChannels; ++ch) {
					Result res = results[ch].get();
					peakOutputSample = std::max(peakOutputSample, res.peak);
					outputBlockIndex = res.outBlockindex;
				}
			}

			outFile->write(outputBlock.data() + outStartOffset, outputBlockIndex - outStartOffset); // group delay compensation
			outStartOffset = 0; // reset after first use

			// conditionally send progress update:
			if (totalSamplesRead > nextProgressThreshold) {
				int progressPercentage = std::min(static_cast<int>(99), static_cast<int>(100 * totalSamplesRead / inputSampleCount));
				std::cout << progressPercentage << "%\b\b\b" << std::flush;
				nextProgressThreshold += incrementalProgressThreshold;
			}

		} while (samplesRead > 0);

		// notify user:
		std::cout << "Done" << std::endl;
		auto prec = std::cout.precision();
		std::cout << "Peak output sample: " << std::setprecision(6) << peakOutputSample << " (" << 20 * log10(peakOutputSample) << " dBFS)" << std::endl;
		std::cout.precision(prec);

		// Test for clipping:	
		if (peakOutputSample > ci.limit) {
			bClippingDetected = true;
			FloatType gainAdjustment = static_cast<FloatType>(clippingTrim) * ci.limit / peakOutputSample;

			gain *= gainAdjustment;
			std::cout << "\nClipping detected !" << std::endl;
			if (!ci.disableClippingProtection) {
				std::cout << "Re-doing with " << 20 * log10(gainAdjustment) << " dB gain adjustment" << std::endl;
				infile.seek(0, SEEK_SET);
			}

			if (ci.bDither) {
				for (auto& ditherer : ditherers) {
					ditherer.adjustGain(gainAdjustment);
					ditherer.reset();
				}
			}

			convertStages.clear();
		}

	} while (!ci.disableClippingProtection && bClippingDetected);
	return true;
} // ends convertMT()

// retrieve metadata using libsndfile API :
bool getMetaData(MetaData& metadata, SndfileHandle& infile) {
	const char* empty = "";
	const char* str;

	metadata.title.assign((str = infile.getString(SF_STR_TITLE)) ? str : empty);
	metadata.copyright.assign((str = infile.getString(SF_STR_COPYRIGHT)) ? str : empty);
	metadata.software.assign((str = infile.getString(SF_STR_SOFTWARE)) ? str : empty);
	metadata.artist.assign((str = infile.getString(SF_STR_ARTIST)) ? str : empty);
	metadata.comment.assign((str = infile.getString(SF_STR_COMMENT)) ? str : empty);
	metadata.date.assign((str = infile.getString(SF_STR_DATE)) ? str : empty);
	metadata.album.assign((str = infile.getString(SF_STR_ALBUM)) ? str : empty);
	metadata.license.assign((str = infile.getString(SF_STR_LICENSE)) ? str : empty);
	metadata.trackNumber.assign((str = infile.getString(SF_STR_TRACKNUMBER)) ? str : empty);
	metadata.genre.assign((str = infile.getString(SF_STR_GENRE)) ? str : empty);

	// retrieve Broadcast Extension (bext) chunk, if it exists:
	metadata.has_bext_fields = (infile.command(SFC_GET_BROADCAST_INFO, (void*)&metadata.broadcastInfo, sizeof(SF_BROADCAST_INFO)) == SF_TRUE);

	if (metadata.has_bext_fields) {
		std::cout << "Input file contains a Broadcast Extension (bext) chunk" << std::endl;
	}

	// retrieve cart chunk, if it exists:
	metadata.has_cart_chunk = (infile.command(SFC_GET_CART_INFO, (void*)&metadata.cartInfo, sizeof(LargeSFCartInfo)) == SF_TRUE);

	if (metadata.has_cart_chunk) {
		// Note: size of CART chunk is variable, depending on size of last field (tag_text[]) 
		if (metadata.cartInfo.tag_text_size > MAX_CART_TAG_TEXT_SIZE) {
			metadata.cartInfo.tag_text_size = MAX_CART_TAG_TEXT_SIZE; // apply hard limit on number of characters (spec says unlimited ...)
		}
		std::cout << "Input file contains a cart chunk" << std::endl;
	}
	return true;
}

// set metadata using libsndfile API :
bool setMetaData(const MetaData& metadata, SndfileHandle& outfile) {

	std::cout << "Writing Metadata" << std::endl;
	if (!metadata.title.empty()) outfile.setString(SF_STR_TITLE, metadata.title.c_str());
	if (!metadata.copyright.empty()) outfile.setString(SF_STR_COPYRIGHT, metadata.copyright.c_str());
	if (!metadata.software.empty()) outfile.setString(SF_STR_SOFTWARE, metadata.software.c_str());
	if (!metadata.artist.empty()) outfile.setString(SF_STR_ARTIST, metadata.artist.c_str());
	if (!metadata.comment.empty()) outfile.setString(SF_STR_COMMENT, metadata.comment.c_str());
	if (!metadata.date.empty()) outfile.setString(SF_STR_DATE, metadata.date.c_str());
	if (!metadata.album.empty()) outfile.setString(SF_STR_ALBUM, metadata.album.c_str());
	if (!metadata.license.empty()) outfile.setString(SF_STR_LICENSE, metadata.license.c_str());
	if (!metadata.trackNumber.empty()) outfile.setString(SF_STR_TRACKNUMBER, metadata.trackNumber.c_str());
	if (!metadata.genre.empty()) outfile.setString(SF_STR_GENRE, metadata.genre.c_str());

	if (((outfile.format() &  SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV) ||
		((outfile.format() &  SF_FORMAT_TYPEMASK) == SF_FORMAT_WAVEX) ||
		((outfile.format() &  SF_FORMAT_TYPEMASK) == SF_FORMAT_RF64)) { /* some sort of wav file */

		// attempt to write bext / cart chunks:
		if (metadata.has_bext_fields) {
			outfile.command(SFC_SET_BROADCAST_INFO, (void*)&metadata.broadcastInfo, sizeof(SF_BROADCAST_INFO));
		}

		if (metadata.has_cart_chunk) {
			outfile.command(SFC_SET_CART_INFO,
				(void*)&metadata.cartInfo,
				sizeof(metadata.cartInfo) - MAX_CART_TAG_TEXT_SIZE + metadata.cartInfo.tag_text_size // (size of cartInfo WITHOUT tag text) + (actual size of tag text) 
			);
		}
	}

	return (outfile.error() == 0);
}

bool testSetMetaData(SndfileHandle& outfile) {
	MetaData m;
	m.title.assign("test title");
	m.copyright.assign("test copyright");
	m.software.assign("test software");
	m.artist.assign("test artist");
	m.comment.assign("test comment");
	m.date.assign("test date");
	m.album.assign("test album");
	m.license.assign("test license");
	m.trackNumber.assign("test track number");
	m.genre.assign("test genre");
	return setMetaData(m, outfile);
}

int getDefaultNoiseShape(int sampleRate) {
	if (sampleRate <= 44100) {
		return DitherProfileID::standard;
	}
	else if (sampleRate <= 48000) {
		return DitherProfileID::standard;
	}
	else if (sampleRate <= 88200) {
		return DitherProfileID::flat_f;
	}
	else if (sampleRate <= 96000) {
		return DitherProfileID::flat_f;
	}
	else if (sampleRate <= 176400) {
		return DitherProfileID::flat_f;
	}
	else {
		return DitherProfileID::flat_f;
	}
}

void showDitherProfiles() {
	for (int d = DitherProfileID::flat; d != DitherProfileID::end; ++d) {
		std::cout << ditherProfileList[d].id << " : " << ditherProfileList[d].name << std::endl;
	}
}

int getSfBytesPerSample(int format) {
	int subformat = format & SF_FORMAT_SUBMASK;
	switch (subformat) {
	case SF_FORMAT_PCM_S8:
		return 1;
	case SF_FORMAT_PCM_16:
		return 2;
	case SF_FORMAT_PCM_24:
		return 3;
	case SF_FORMAT_PCM_32:
		return 4;
	case SF_FORMAT_PCM_U8:
		return 1;
	case SF_FORMAT_FLOAT:
		return 4;
	case SF_FORMAT_DOUBLE:
		return 8;
	default:
		return 2; // for safety
	}
}

bool checkWarnOutputSize(uint64_t inputSamples, int bytesPerSample, int numerator, int denominator)
{
	uint64_t outputDataSize = inputSamples * bytesPerSample * numerator / denominator;

	const uint64_t limit4G = 1ULL << 32;
	if (outputDataSize >= limit4G) {
		std::cout << "Warning: output file ( " << fmtNumberWithCommas(outputDataSize) << " bytes of data ) will exceed 4GB limit"  << std::endl;
		return true;
	}
	return false;
}

std::string fmtNumberWithCommas(uint64_t n) {
	std::string s = std::to_string(n);
	int insertPosition = s.length() - 3;
	while (insertPosition > 0) {
		s.insert(insertPosition, ",");
		insertPosition -= 3;
	}
	return s;
}

bool testSetMetaData(DsfFile& outfile) {
	// stub - to-do
	return true;
}

bool testSetMetaData(DffFile& outfile) {
	// stub - to-do
	return true;
}

bool getMetaData(MetaData& metadata, const DffFile& f) {
	// stub - to-do
	return true;
}

bool getMetaData(MetaData& metadata, const DsfFile& f) {
	// stub - to-do
	return true;
}

// gcd() - greatest common divisor:
int gcd(int a, int b) {
	if (a<0) a = -a;
	if (b<0) b = -b;
	while (b != 0) {
		a %= b;
		if (a == 0) return b;
		b %= a;
	}
	return a;
}

//  getSimplifiedFraction() - turns a sample-rate ratio into a fraction:
Fraction getSimplifiedFraction(int inputSampleRate, int outputSampleRate)			// eg 44100, 48000
{
	Fraction f;
	f.numerator = (outputSampleRate / gcd(inputSampleRate, outputSampleRate));		// L (eg 160)
	f.denominator = (inputSampleRate / gcd(inputSampleRate, outputSampleRate));		// M (eg 147)
	return f;
}

// The following functions are used for parsing commandline parameters:

void getCmdlineParam(char** begin, char** end, const std::string& optionName, std::string& Parameter)
{
	Parameter.clear();
	char** it = std::find(begin, end, optionName);
	if (it != end)	// found option
		if (++it != end) // found parameter after option
			Parameter = *it;
}

void getCmdlineParam(char** begin, char** end, const std::string& optionName, unsigned int& nParameter)
{
	nParameter = 0;
	char** it = std::find(begin, end, optionName);
	if (it != end)	// found option
		if (++it != end) // found parameter after option
			nParameter = atoi(*it);
}

void getCmdlineParam(char** begin, char** end, const std::string& optionName, int& nParameter)
{
	nParameter = 0;
	char** it = std::find(begin, end, optionName);
	if (it != end)	// found option
		if (++it != end) // found parameter after option
			nParameter = atoi(*it);
}


void getCmdlineParam(char** begin, char** end, const std::string& optionName, double& Parameter)
{
	Parameter = 0.0;
	char** it = std::find(begin, end, optionName);
	if (it != end)	// found option
		if (++it != end) // found parameter after option
			Parameter = atof(*it);
}

bool findCmdlineOption(char** begin, char** end, const std::string& option) {
	return (std::find(begin, end, option) != end);
}

bool checkSSE2() {
#if defined (_MSC_VER) || defined (__INTEL_COMPILER)
	bool bSSE2ok = false;
	int CPUInfo[4] = { 0,0,0,0 };
	__cpuid(CPUInfo, 0);
	if (CPUInfo[0] != 0) {
		__cpuid(CPUInfo, 1);
		if (CPUInfo[3] & (1 << 26))
			bSSE2ok = true;
	}
	if (bSSE2ok) {
		std::cout << "CPU supports SSE2 (ok)";
		return true;
	}
	else {
		std::cout << "Your CPU doesn't support SSE2 - please try a non-SSE2 build on this machine" << std::endl;
		return false;
	}
#endif // defined (_MSC_VER) || defined (__INTEL_COMPILER)
return true; // todo: fix the check on gcc
}

bool checkAVX() {
#if defined (_MSC_VER) || defined (__INTEL_COMPILER)
	// Verify CPU capabilities:
	bool bAVXok = false;
	int cpuInfo[4] = { 0,0,0,0 };
	__cpuid(cpuInfo, 0);
	if (cpuInfo[0] != 0) {
		__cpuid(cpuInfo, 1);
		if (cpuInfo[2] & (1 << 28)) {
			bAVXok = true; // Note: this test only confirms CPU AVX capability, and does not check OS capability.
						   // to-do: check for AVX2 ...
		}
	}
	if (bAVXok) {
		std::cout << "CPU supports AVX (ok)";
		return true;
	}
	else {
		std::cout << "Your CPU doesn't support AVX - please try a non-AVX build on this machine" << std::endl;
		return false;
	}
#endif // defined (_MSC_VER) || defined (__INTEL_COMPILER)
return true; // todo: gcc detection
}

bool showBuildVersion() {
	std::cout << strVersion << " ";
#if defined(_M_X64) || defined(__x86_64__)
	std::cout << "64-bit version";
#ifdef USE_AVX
	std::cout << " AVX build ... ";
	if (!checkAVX())
		return false;
#ifdef USE_FMA
	std::cout << "\nusing FMA (Fused Multiply-Add) instruction ... ";
#endif
#endif // USE_AVX	
	std::cout << std::endl;
#else
	std::cout << "32-bit version";
#if defined(USE_SSE2)
	std::cout << ", SSE2 build ... ";
	// Verify processor capabilities:
	if (!checkSSE2())
		return false;
#endif // defined(USE_SSE2)
	std::cout << "\n" << std::endl;
#endif
	return true;
}

void showCompiler() {
	// https://sourceforge.net/p/predef/wiki/Compilers/
#if defined (__clang__)
	std::cout << "Clang " << __clang_major__ << "." 
	<< __clang_minor__ << "."
	<< __clang_patchlevel__ << std::endl;
#elif defined (__MINGW64__)
	std::cout << "minGW-w64" << std::endl;
#elif defined (__MINGW32__)
	std::cout << "minGW" << std::endl;
#elif defined (__GNUC__)
	std::cout << "gcc " << __GNUC__ << "." 
	<< __GNUC_MINOR__ << "."
	<< __GNUC_PATCHLEVEL__ << std::endl;
#elif defined (_MSC_VER)
	std::cout << "Visual C++ " << _MSC_FULL_VER << std::endl;
#elif defined (__INTEL_COMPILER)
	std::cout << "Intel Compiler " << __INTEL_COMPILER << std::endl;
#else
	std::cout << "unknown" << std::endl;
#endif 

}