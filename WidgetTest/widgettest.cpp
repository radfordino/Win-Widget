/*!
#
# Win-Widget. Windows related software for Audio-Widget/SDR-Widget (http://code.google.com/p/sdr-widget/)
# Copyright (C) 2012 Nikolay Kovbasa
#
# Permission to copy, use, modify, sell and distribute this software 
# is granted provided this copyright notice appears in all copies. 
# This software is provided "as is" without express or implied
# warranty, and with no claim as to its suitability for any purpose.
#
#----------------------------------------------------------------------------
# Contact: nikkov@gmail.com, borge.strand@gmail.com
#----------------------------------------------------------------------------
*/

#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#include <conio.h>
#include <math.h>

#include "USBAudioDevice.h"
#include "tlist.h"

#ifdef _ENABLE_TRACE

void debugPrintf(const char *szFormat, ...)
{
    char str[4096];
    va_list argptr;
    va_start(argptr, szFormat);
    vsprintf_s(str, szFormat, argptr);
    va_end(argptr);

    printf(str);
    OutputDebugString(str);
}
#endif


struct AudioSample4
{
	int left;
	int right;
};


struct ThreeByteSample
{
	UCHAR sample[3];
	ThreeByteSample(int val = 0)
	{
		UCHAR *ptrVal = (UCHAR *)&val;
		sample[0] = *(ptrVal);
		sample[1] = *(ptrVal+1);
		sample[2] = *(ptrVal+2);
	}

	operator int()const
	{
		return (sample[0] << 0) + (sample[1] << 8) + (sample[2] << 16);
	}
};


struct AudioSample3
{
	ThreeByteSample left;
	ThreeByteSample right;
};


// Global variables keep this program running.

// A fixed array for signal generator sine table
AudioSample4 globalSineBuffer[48];

// Memory pointers to Wav file data
AudioSample3 * globalWavBuffer3;
AudioSample4 * globalWavBuffer4;
DWORD globalNumParsedSamples = 0;		// The number of samples read in from the wav file, independant of sample format
DWORD globalBufferIndex = 0;
DWORD globalPacketCounter = 0;

// Assume we're running in verbose mode until "-v" can be specified on command line
DWORD globalVerbose = 1; 


// Parse a wav file header and return file parameters
// See https://ccrma.stanford.edu/courses/422/projects/WaveFormat/
int ParseWavHeader (FILE* wavfile, int* NumChannels, int* SampleRate, int* BytesPerSample, long* NumSamples) {
#define WAVHEADER_L 44				// Length of standard wav file header
#define IG 0xAB						// A byte which doesn't occur elsewhere in header

	unsigned char wavheader[WAVHEADER_L] = {	// Required header contents
		'R', 'I', 'F', 'F',			// Chunk ID
		IG, IG, IG, IG,				// ChunkSize, 36 + bytes of audio data
		'W', 'A', 'V', 'E',			// Format
		'f', 'm', 't', 0x20,		// Subchunk1ID
		0x10, 0x00, 0x00, 0x00,		// Subchunk1Size, 16 for PCM
		0x01, 0x00,					// AudioFormat, 1 for PCM
		IG, IG,						// NumChannels, extract number, only accept 2
		IG, IG, IG, IG,				// SampleRate
		IG, IG, IG, IG,				// ByteRate = SampleRate * NumChannels * BitsPerSample / 8
		IG, IG,						// BlockAlign = NumChannels * BitsPerSample / 8
		IG, IG,						// BitsPerSample
		'd', 'a', 't', 'a',			// Subchunk2ID
		IG, IG, IG, IG};			// SubChunk2Size

	unsigned char readwavheader[WAVHEADER_L];

	rewind (wavfile);				// Make sure to read header from beginning of file!

	// Extract wav file header and do initial error checking
	int n = fread (readwavheader, 1, WAVHEADER_L, wavfile); // Try to read 44 header bytes

	if (n != WAVHEADER_L) {
		printf ("WidgetTest: ERROR: Could not read %d bytes wav file header\n", WAVHEADER_L);
		return 0;
	}

	// Compare wavfile to default header, ignoring all bytes set to IG
	n=0;
	while ( ( (readwavheader[n] == wavheader[n]) || (wavheader[n] == IG) ) && (n < WAVHEADER_L) ) n++;

	if (n != WAVHEADER_L) {
		printf ("WidgetTest: ERROR: wav file header error at position %d\n", n);
		return 0;
	}

	// Extract wav file parameters
	int ChunkSize = (readwavheader[4]) + (readwavheader[5]<<8) + (readwavheader[6]<<16) + (readwavheader[7]<<24);
	*NumChannels = (readwavheader[22]) + (readwavheader[23]<<8);
	*SampleRate = (readwavheader[24]) + (readwavheader[25]<<8) + (readwavheader[26]<<16) + (readwavheader[27]<<24);
	int ByteRate = (readwavheader[28]) + (readwavheader[29]<<8) + (readwavheader[30]<<16) + (readwavheader[31]<<24);
	int BlockAlign = (readwavheader[32]) + (readwavheader[33]<<8);
	int BitsPerSample = (readwavheader[34]) + (readwavheader[35]<<8);
	*BytesPerSample = BitsPerSample >> 3;
	int SubChunk2Size = (readwavheader[40]) + (readwavheader[41]<<8) + (readwavheader[42]<<16) + (readwavheader[43]<<24);
	*NumSamples = SubChunk2Size / *BytesPerSample / *NumChannels;
	double Duration = *NumSamples; Duration /= *SampleRate;

	// Print parameters
	if (globalVerbose == 1) {
		printf ("WidgetTest: ChunkSize = %d\n", ChunkSize);
		printf ("WidgetTest: NumChannels = %d\n", *NumChannels);
		printf ("WidgetTest: SampleRate = %d\n", *SampleRate);
		printf ("WidgetTest: ByteRate = %d\n", ByteRate);
		printf ("WidgetTest: BlockAlign = %d\n", BlockAlign);
		printf ("WidgetTest: BytesPerSample = %d\n", *BytesPerSample); // Bytes per MONO sample, *2 for stereo
		printf ("WidgetTest: SubChunk2Size = %d\n", SubChunk2Size);
		printf ("WidgetTest: NumSamples = %d\n", *NumSamples);
		printf ("WidgetTest: Duration = %4.3fs\n", Duration);
	}

	// Full error checking
	n = 1; // Assuming a clean return. But report all found errors before returning

	if (SubChunk2Size + 36 != ChunkSize) {
		printf ("WidgetTest: ERROR: SubChunk2Size, ChunkSize mismatch %d+36 != %d\n", SubChunk2Size, ChunkSize);
		n = 0;
	}

	if (*NumChannels != 2) {
		printf ("WidgetTest: ERROR: Only 2-channel wav is accepted, not the detected %d-channel.\n", *NumChannels);
		n = 0;
	}

	if ( (*SampleRate != 44100) && (*SampleRate != 48000) && 
		 (*SampleRate != 88200) && (*SampleRate != 96000) &&
		 (*SampleRate != 176400) && (*SampleRate != 192000) ) {
		printf ("WidgetTest: ERROR: Only 44.1/48/88.2/96/176.4/192ksps accepted, not the detected %d.\n", *SampleRate);
		n = 0 ;
	}

	if (ByteRate != *SampleRate * *NumChannels * *BytesPerSample) {
		printf ("WidgetTest: ERROR: Mismatch between ByteRate, SampleRate, NumChannels, BitsPerSample\n");
		n = 0;
	}

	if (BlockAlign != *NumChannels * *BytesPerSample) {
		printf ("WidgetTest: ERROR: Mismatch between BlockAlign, NumChannels, BitsPerSample\n");
		n = 0;
	}

	if ( (*BytesPerSample != 2) && (*BytesPerSample != 3) && (*BytesPerSample != 4) ) {
		printf ("WidgetTest: ERROR: Only 2/3/4 bytes per mono sample accepted, not the detected %d.\n", *BytesPerSample);
		n = 0;
	}

	return n;
}


// Generate a sine table of 48 32-bit entries
void FillSineBuffer(AudioSample4* sinebuffer, bool zoom)
{
	memset(sinebuffer, 0, 48*sizeof(AudioSample4));
	for(int i = 0; i < 48; i++) {
		sinebuffer[i].left  = (int)(0x1FFFFF*sin(2.0*3.14159265358979323846*(double)i/48.));
		sinebuffer[i].right = sinebuffer[i].left;

		if(zoom) {
			sinebuffer[i].left  = sinebuffer[i].left << 8;
			sinebuffer[i].right = sinebuffer[i].right << 8;
		}
	}
}


// Move data from sine table to USB device. This function is passed as a callback
void FillSineData3(void* context, UCHAR *buffer, int& len)
{
	AudioSample3 *sampleBuff = (AudioSample3 *)buffer;
	int sampleLength = len / sizeof(AudioSample3);

	for(int i = 0; i < sampleLength; i++) {
		sampleBuff[i].left  = globalSineBuffer[globalBufferIndex].left;
		sampleBuff[i].right = globalSineBuffer[globalBufferIndex].right;
		globalBufferIndex++;
		if(globalBufferIndex >= 48) 
			globalBufferIndex = 0;
	}
	globalPacketCounter++;
	if(globalPacketCounter > 0xFF)
		globalPacketCounter = 0;
}


// Move data from sine table to USB device. This function is passed as a callback
void FillSineData4(void* context, UCHAR *buffer, int& len)
{
	AudioSample4 *sampleBuff = (AudioSample4 *)buffer;
	int sampleLength = len / sizeof(AudioSample4);

	for(int i = 0; i < sampleLength; i++) {
		sampleBuff[i].left  = globalSineBuffer[globalBufferIndex].left;
		sampleBuff[i].right = globalSineBuffer[globalBufferIndex].right;
		globalBufferIndex++;
		if(globalBufferIndex >= 48) 
			globalBufferIndex = 0;
	}
	globalPacketCounter++;
	if(globalPacketCounter > 0xFF)
		globalPacketCounter = 0;
}


// Fill 3L, 3R bytes from wavfile into RAM (replaces sine table above)
long FillWavBuffer3 (AudioSample3* wavbuffer, FILE* wavfile, int* BytesPerSample, long* NumSamples) {
	unsigned char temp[8];	// Temporary variable for reading a stereo sample from the wavfile
	long readsamples = 0;
	int m = 0;

	if (*BytesPerSample == 2) {
		for (long n=0; n<*NumSamples; n++) {
			m = fread(temp, 1, 4, wavfile);
			if (m == 4) { 		// 16 bits -> 24 bits stereo
				wavbuffer[n].left  = (temp[0]<<8) + (temp[1]<<16);
				wavbuffer[n].right = (temp[2]<<8) + (temp[3]<<16);
				readsamples++;
			}
			else {
				if (globalVerbose == 1)
					printf ("WidgetTest: ERROR: Read error n=%d, ReadSamples=%d, m=%d", n, readsamples, m);
				return readsamples;						// Cut the process short if read fails
			}
		}
	}

	else if (*BytesPerSample == 3) {
		for (long n=0; n<*NumSamples; n++) {
			m = fread(temp, 1, 6, wavfile);
			if (m == 6) { 		// 24 bits -> 24 bits stereo. Can we do simple copy?
				wavbuffer[n].left  = (temp[0]) + (temp[1]<<8) + (temp[2]<<16);
				wavbuffer[n].right = (temp[3]) + (temp[4]<<8) + (temp[5]<<16);
				readsamples++;
			}
			else {
				if (globalVerbose == 1)
					printf ("WidgetTest: ERROR: Read error n=%d, ReadSamples=%d, m=%d", n, readsamples, m);
				return readsamples;						// Cut the process short if read fails
			}
		}
	}

	else if (*BytesPerSample == 4) {
		for (long n=0; n<*NumSamples; n++) {
			m = fread(temp, 1, 8, wavfile);
			if (m == 8) {		// 32 bits -> 24 bits stereo. FIX: add optional dither!
				wavbuffer[n].left  = (temp[1]) + (temp[2]<<8) + (temp[3]<<16);
				wavbuffer[n].right = (temp[5]) + (temp[6]<<8) + (temp[7]<<16);
				readsamples++;
			}
			else {
				if (globalVerbose == 1)
					printf ("WidgetTest: ERROR: Read error n=%d, ReadSamples=%d, m=%d", n, readsamples, m);
				return readsamples;						// Cut the process short if read fails
			}
		}
	}

	return readsamples;
}


// Fill 4L, 4R bytes from wavfile into RAM (replaces sine table above)
long FillWavBuffer4 (AudioSample4* wavbuffer, FILE* wavfile, int* BytesPerSample, long* NumSamples) {
	unsigned char temp[8];	// Temporary variable for reading a stereo sample from the wavfile
	long readsamples = 0;
	int m = 0;

	if (*BytesPerSample == 2) {
		for (long n=0; n<*NumSamples; n++) {
			m = fread(temp, 1, 4, wavfile);
			if (m == 4) {		// 16 bits -> 32 bits stereo
				wavbuffer[n].left  = (temp[0]<<16) + (temp[1]<<24);
				wavbuffer[n].right = (temp[2]<<16) + (temp[3]<<24);
				readsamples++;
			}
			else {
				if (globalVerbose == 1)
					printf ("WidgetTest: ERROR: Read error n=%d, ReadSamples=%d, m=%d", n, readsamples, m);
				return readsamples;						// Cut the process short if read fails
			}
		}
	}

	else if (*BytesPerSample == 3) {
		for (long n=0; n<*NumSamples; n++) {
			m = fread(temp, 1, 6, wavfile);
			if (m == 6) {		// 24 bits -> 32 bits stereo.
				wavbuffer[n].left  = (temp[0]<<8) + (temp[1]<<16) + (temp[2]<<24);
				wavbuffer[n].right = (temp[3]<<8) + (temp[4]<<16) + (temp[5]<<24);
				readsamples++;
			}
			else {
				if (globalVerbose == 1)
					printf ("WidgetTest: ERROR: Read error n=%d, ReadSamples=%d, m=%d", n, readsamples, m);
				return readsamples;						// Cut the process short if read fails
			}
		}
	}

	else if (*BytesPerSample == 4) {
		for (long n=0; n<*NumSamples; n++) {
			m = fread(temp, 1, 8, wavfile);
			if (m == 8) { 		// 32 bits -> 32 bits stereo.  Can we do simple copy?
				wavbuffer[n].left  = (temp[0]) + (temp[1]<<8) + (temp[2]<<16) + (temp[3]<<24);
				wavbuffer[n].right = (temp[4]) + (temp[5]<<8) + (temp[6]<<16) + (temp[7]<<24);
				readsamples++;
			}
			else {
				if (globalVerbose == 1)
					printf ("WidgetTest: ERROR: Read error n=%d, ReadSamples=%d, m=%d", n, readsamples, m);
				return readsamples;						// Cut the process short if read fails
			}
		}
	}

	return readsamples;
}


// Move data from wav file RAM to USB device. This function is passed as a callback
void FillWavData3(void* context, UCHAR *buffer, int& len)
{
	AudioSample3 *sampleBuff = (AudioSample3 *)buffer;
	int sampleLength = len / sizeof(AudioSample3);

	for(int i = 0; i < sampleLength; i++)
	{
		// Have we read past parsed end of wav file? Determine by counting audio samples
		if (globalBufferIndex < globalNumParsedSamples) { // initially, both are 0
			sampleBuff[i].left  =  globalWavBuffer3[globalBufferIndex].left;
			sampleBuff[i].right =  globalWavBuffer3[globalBufferIndex].right;
			globalBufferIndex++;
		}
		else {
			sampleBuff[i].left  =  0;
			sampleBuff[i].right =  0;
		}

	}
	globalPacketCounter++;
	if(globalPacketCounter > 0xFF)
		globalPacketCounter = 0;
}


// Move data from wav file RAM to USB device. This function is passed as a callback
void FillWavData4(void* context, UCHAR *buffer, int& len)
{
	AudioSample4 *sampleBuff = (AudioSample4 *)buffer;
	int sampleLength = len / sizeof(AudioSample4);

	for(int i = 0; i < sampleLength; i++)
	{
		// Have we read past parsed end of wav file? Determine by counting audio samples
		if (globalBufferIndex < globalNumParsedSamples) { // initially, both are 0
			sampleBuff[i].left  =  globalWavBuffer4[globalBufferIndex].left;
			sampleBuff[i].right =  globalWavBuffer4[globalBufferIndex].right;
			globalBufferIndex++;
		}
		else {
			sampleBuff[i].left  =  0;
			sampleBuff[i].right =  0;
		}

	}
	globalPacketCounter++;
	if(globalPacketCounter > 0xFF)
		globalPacketCounter = 0;
}


// Move zeros to USB device. This function is passed as a callback
void FillZeroData3(void* context, UCHAR *buffer, int& len)
{
	AudioSample3 *sampleBuff = (AudioSample3 *)buffer;
	int sampleLength = len / sizeof(AudioSample3);

	for(int i = 0; i < sampleLength; i++)
	{
		sampleBuff[i].left  =  0;
		sampleBuff[i].right =  0;
	}
	globalPacketCounter++;
	if(globalPacketCounter > 0xFF)
		globalPacketCounter = 0;
}


// Move zeros to USB device. This function is passed as a callback
void FillZeroData4(void* context, UCHAR *buffer, int& len)
{
	AudioSample4 *sampleBuff = (AudioSample4 *)buffer;
	int sampleLength = len / sizeof(AudioSample4);

	for(int i = 0; i < sampleLength; i++)
	{
		sampleBuff[i].left  =  0;
		sampleBuff[i].right =  0;
	}
	globalPacketCounter++;
	if(globalPacketCounter > 0xFF)
		globalPacketCounter = 0;
}


int main(int argc, char* argv[]) {

	int freq;
	int mode; // BSB 20121007 0:signal generator 1:wav file player
	int SubSlotSize = 0;

	if (argc == 1) {	// No arguments
		freq = 48000;	// Default sampling frequency
		mode = 0;		// Run as signal generator
	}

	else if (argc == 2) { // One argument, may it be a valid audio frequency?
		freq = atoi(argv[1]);
		if ( (freq == 44100) || (freq == 48000) || (freq == 88200) || (freq == 96000) || (freq == 176400) || (freq == 192000) )
			mode = 0;	// Run as signal generator
		else
			mode = 1;	// Run as wav file player
	}

	else				// >1 argument, run as wav file player
		mode = 1;		// Run as wav file player


	if (mode == 0) {	// signal generator
		if (globalVerbose == 1)
			printf("WidgetTest: Running as signal generator.\n");

		// First check for valid playback device
		USBAudioDevice device(true);
		if (!device.InitDevice()) {
			printf ("WidgetTest: ERROR: UAC2 Audio device not found\n");
			return -1;
		}
		SubSlotSize = device.GetDACSubslotSize();
		
		if(SubSlotSize == 3) {
			FillSineBuffer(globalSineBuffer, false);
			device.SetDACCallback(FillSineData3, NULL);
		}
		else if(SubSlotSize == 4) {
			FillSineBuffer(globalSineBuffer, true);
			device.SetDACCallback(FillSineData4, NULL);
		}
		device.SetSampleRate(freq);
		device.Start();
		printf("WidgetTest: Press any key to stop...\n");
		_getch();
		device.Stop();
	}

	else if (mode == 1) { // wav file player
		int NumChannels = 0;
		int SampleRate = 0;
		int prevSampleRate = 0;
		int BytesPerSample = 0;
		long NumSamples = 0;		// The number of samples in the wav file, independant of sample format
		FILE * wavfile;

		if (globalVerbose == 1)
			printf("WidgetTest: Running as wav file player.\n");

		// First check for valid playback device
		USBAudioDevice device(true);
		if (!device.InitDevice()) {
			printf ("WidgetTest: ERROR: UAC2 Audio device not found\n");
			return -1;
		}
		SubSlotSize = device.GetDACSubslotSize();

		// Starting out with default sample rate and mute
		if(SubSlotSize == 3)
			device.SetDACCallback(FillZeroData3, NULL);
		else if(SubSlotSize == 4)
			device.SetDACCallback(FillZeroData4, NULL);

		// Open wav files
		for (int n=1; n<argc; n++) {
			// Then do various checks on each wav file(s) to play
			globalNumParsedSamples = 0;	// We haven't yet read anything from the wav file
			globalBufferIndex = 0;

			wavfile = fopen(argv[n],"rb");	// fopen_s is recommended..
			if (wavfile==NULL) {
				printf ("\nWidgetTest: WARNING: File not found: %s\n",argv[n]);
				continue;
			}

			if (globalVerbose == 1)
				printf ("\nWidgetTest: Found file: %s\n",argv[n]);

			if (!ParseWavHeader (wavfile, &NumChannels, &SampleRate, &BytesPerSample, &NumSamples))
				continue;		// Function does its own error reporting

			if(SubSlotSize == 3) {
				globalWavBuffer3 = new AudioSample3[NumSamples];

				globalNumParsedSamples = FillWavBuffer3 (globalWavBuffer3, wavfile, &BytesPerSample, &NumSamples);
				if (globalVerbose == 1)
					printf ("WidgetTest: globalNumParsedSamples=%d NumSamples=%d\n", globalNumParsedSamples, NumSamples);
			}
			else if(SubSlotSize == 4) {
				globalWavBuffer4 = new AudioSample4[NumSamples];
				globalNumParsedSamples = FillWavBuffer4 (globalWavBuffer4, wavfile, &BytesPerSample, &NumSamples);
				if (globalVerbose == 1)
					printf ("WidgetTest: globalNumParsedSamples=%d NumSamples=%d\n", globalNumParsedSamples, NumSamples);
			}

			fclose (wavfile);

			// Now play the darn thing :-) 
			if (globalNumParsedSamples == NumSamples) {
				if (globalVerbose == 1)
					printf ("WidgetTest: Wav file read into memory\n");

				if ( (prevSampleRate != SampleRate) && (prevSampleRate != 0) ) {
					device.Stop();						// When on >1st wav file, and with new s-rate, first stop device
					printf ("STOP \n");
				}

				// Reconnect to globalWavBuffer3/4 now that one of them is filled up
				if(SubSlotSize == 3)
					device.SetDACCallback(FillWavData3, NULL);
				else if(SubSlotSize == 4)
					device.SetDACCallback(FillWavData4, NULL);

				if (prevSampleRate != SampleRate) {		// With 1st wav file, prevSampleRate==0. For 1st wav file and
					device.SetSampleRate(SampleRate);	// sample rate changes, change the rate and restart device
					device.Start();
					printf ("START \n");
				}

				prevSampleRate = SampleRate;

				// Wait loop while music hopefully plays
				printf("WidgetTest: Press any key to continue...\n"); // FIX: also continue end after end of file
				while	(				// Wait for key press or end of file
						( !_kbhit() ) &&
						( globalBufferIndex < globalNumParsedSamples )
						) {
					Sleep(50);			// Parameter is ms
				}
				if (_kbhit())
					_getch();



				// globalWavBuffer will be deleted below, mute in the meantime
				if(SubSlotSize == 3)
					device.SetDACCallback(FillZeroData3, NULL);
				else if(SubSlotSize == 4)
					device.SetDACCallback(FillZeroData4, NULL);

				printf("WidgetTest: globalBufferIndex=%d globalNumParsedSamples=%d\n", globalBufferIndex, globalNumParsedSamples);
			}
			else {
				printf ("WidgetTest: ERROR: Wav file not read into memory\n");
				return -1;
			}

			if(SubSlotSize == 3)
				delete [] globalWavBuffer3;
			else if(SubSlotSize == 4)
				delete [] globalWavBuffer4;
		} // for n wav files

		device.Stop();

	} // wav player

	_CrtDumpMemoryLeaks();
	return 0;
} // main()

/*
Todo:

- Don't start and stop device for each song
- Test above with changing sample rates, start/stop with different sample rate only?
- Smaller memory footprint
- Determine how callback is called (thread?)
- Measure and optimize lateny
  - Differential CPU time measurement in callback
  - Preventing swapping of memory
  - Make callback thread(?) more real-time-y

*/