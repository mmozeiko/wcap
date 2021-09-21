#include "wcap.h"

#include <mmdeviceapi.h>
#include <audioclient.h>

typedef struct {
	WAVEFORMATEX* Format;

	IMMDeviceEnumerator* Enumerator;
	IAudioCaptureClient* Capture;
	IAudioClient* Record;
	IAudioClient* Play;

	volatile BOOL Stop;
	HANDLE Event;
	HANDLE Thread;
	DWORD PlayFrameSize;
} Audio;

void Audio_Init(Audio* Audio);

BOOL Audio_Start(Audio* Audio);
void Audio_Flush(Audio* Audio);
void Audio_Stop(Audio* Audio);

BOOL Audio_GetNextBuffer(Audio* Audio, LPVOID* Data, UINT32* FrameCount, UINT64* Time);
void Audio_ReleaseBuffer(Audio* Audio, UINT32 FrameCount);
