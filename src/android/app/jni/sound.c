/*
 * sound.c – Android sound wrapper + full Atari800 Sound_* API
 *
 * Merged Android/OpenSL ES playback code with the core Sound_* API implementations.
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <dlfcn.h>
 #include <pthread.h>
 #include <SLES/OpenSLES.h>
 #include <SLES/OpenSLES_Android.h>
 
 #include "util.h"
 #include "cfg.h"
 #include "log.h"
 #include "pokeysnd.h"
 
 /* ------------------------------------------------------------------------
  * Core globals and types
  * --------------------------------------------------------------------- */
 typedef struct {
	 unsigned int freq;
	 unsigned int sample_size;
	 unsigned int channels;
	 unsigned int buffer_ms;
 } Sound_setup_t;
 
 int Sound_enabled = 1;
 Sound_setup_t Sound_desired = { 44100, 2, 1, 0 };
 Sound_setup_t Sound_out;
 unsigned int Sound_latency = 20;
 static int paused = 1;
 #ifndef SOUND_CALLBACK
 static UBYTE *process_buffer = NULL;
 static unsigned int process_buffer_size;
 #endif
 
 /* ------------------------------------------------------------------------
  * Android/OpenSL ES playback state
  * --------------------------------------------------------------------- */
 static int snd_mixrate = 0;
 static int snd_bufsizems = 0;
 static int at_sixteenbit = 1;
 static int osl_disable = 0;
 int Android_osl_sound = 0;
 
 #define BUFSIZE_MS 10
 static void *osl_handle = NULL;
 static SLObjectItf engineObj = NULL;
 static SLEngineItf engine = NULL;
 static SLObjectItf mixerObj = NULL;
 static SLObjectItf playerObj = NULL;
 static SLPlayItf playItf = NULL;
 static SLAndroidSimpleBufferQueueItf bufqItf = NULL;
 static unsigned int bufSize = 0;
 static unsigned int bufCount = 0;
 static void *buffers[2] = { NULL, NULL };
 
 /* ------------------------------------------------------------------------
  * Prototypes for Android/OpenSL ES helpers
  * --------------------------------------------------------------------- */
 static int OSL_load(void);
 static int OSL_init(void);
 static int OSL_buf_alloc(void);
 static int OSL_start_playback(void);
 
 /* Convenience macro */
 #define CHECK_OSL(r, msg) \
	 if ((r) != SL_RESULT_SUCCESS) { \
		 Log_print("OpenSL ES error: %s", msg); \
		 return FALSE; \
	 }
 
 /* ------------------------------------------------------------------------
  * Android/OpenSL ES implementation
  * --------------------------------------------------------------------- */
 static int OSL_load(void) {
	 osl_handle = dlopen("libOpenSLES.so", RTLD_NOW);
	 if (!osl_handle) {
		 Log_print("dlopen OpenSLES failed: %s", dlerror());
		 return FALSE;
	 }
	 return TRUE;
 }
 
 static int OSL_init(void) {
	 SLresult r;
	 /* Create engine */
	 r = slCreateEngine(&engineObj, 0, NULL, 0, NULL, NULL);
	 CHECK_OSL(r, "slCreateEngine");
	 r = (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
	 CHECK_OSL(r, "Engine Realize");
	 r = (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);
	 CHECK_OSL(r, "GetInterface ENGINE");
	 /* Create output mix */
	 r = (*engine)->CreateOutputMix(engine, &mixerObj, 0, NULL, NULL);
	 CHECK_OSL(r, "CreateOutputMix");
	 r = (*mixerObj)->Realize(mixerObj, SL_BOOLEAN_FALSE);
	 CHECK_OSL(r, "Mixer Realize");
	 return TRUE;
 }
 
 static int OSL_buf_alloc(void) {
	 SLresult r;
	 bufCount = snd_bufsizems / BUFSIZE_MS;
	 SLDataLocator_BufferQueue loc_buf = { SL_DATALOCATOR_BUFFERQUEUE, bufCount };
	 SLDataFormat_PCM format = {
		 SL_DATAFORMAT_PCM,
		 Sound_desired.channels,
		 Sound_desired.freq * 1000,
		 Sound_desired.sample_size * 8,
		 Sound_desired.sample_size * 8,
		 (Sound_desired.channels == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER),
		 SL_BYTEORDER_LITTLEENDIAN
	 };
	 SLDataSource src = { &loc_buf, &format };
	 SLDataLocator_OutputMix loc_out = { SL_DATALOCATOR_OUTPUTMIX, mixerObj };
	 SLDataSink sink = { &loc_out, NULL };
	 const SLInterfaceID ids[] = { SL_IID_PLAY, SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
	 const SLboolean req[]  = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
 
	 /* Create audio player */
	 r = (*engine)->CreateAudioPlayer(engine, &playerObj, &src, &sink, 2, ids, req);
	 CHECK_OSL(r, "CreateAudioPlayer");
	 /* Get interfaces */
	 r = (*playerObj)->GetInterface(playerObj, SL_IID_PLAY, &playItf);
	 CHECK_OSL(r, "GetInterface PLAY");
	 r = (*playerObj)->GetInterface(playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bufqItf);
	 CHECK_OSL(r, "GetInterface BUFFERQUEUE");
 
	 /* Allocate buffers */
	 bufSize = snd_mixrate * (at_sixteenbit ? 2 : 1) * BUFSIZE_MS / 1000;
	 for (unsigned i = 0; i < bufCount; i++) {
		 buffers[i] = malloc(bufSize);
		 if (!buffers[i]) return FALSE;
	 }
	 return TRUE;
 }
 
 static int OSL_start_playback(void) {
	 SLresult r = (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_PLAYING);
	 CHECK_OSL(r, "SetPlayState PLAYING");
	 return TRUE;
 }
 
 /* ------------------------------------------------------------------------
  * PLATFORM-level Sound_* wrappers
  * --------------------------------------------------------------------- */
 int PLATFORM_SoundSetup(Sound_setup_t *setup) {
	 if (!Sound_enabled) return TRUE;
	 /* Save desired settings */
	 snd_mixrate = setup->freq;
	 snd_bufsizems = setup->buffer_ms;
	 at_sixteenbit = (setup->sample_size > 1);
	 osl_disable = 0;
	 Android_osl_sound = 0;
	 return OSL_load() && OSL_init() && OSL_buf_alloc() && OSL_start_playback();
 }
 
 void PLATFORM_SoundExit(void) {
	 if (playItf) (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_STOPPED);
	 if (mixerObj) (*mixerObj)->Destroy(mixerObj);
	 if (engineObj) (*engineObj)->Destroy(engineObj);
	 if (osl_handle) dlclose(osl_handle);
 }
 
 void PLATFORM_SoundPause(void) {
	 if (playItf) (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_PAUSED);
 }
 
 void PLATFORM_SoundContinue(void) {
	 if (playItf) (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_PLAYING);
 }
 
 void PLATFORM_SoundWrite(UBYTE *buffer, int size) {
	 (*bufqItf)->Enqueue(bufqItf, buffer, size);
 }
 
 unsigned int PLATFORM_SoundAvailable(void) {
	 return bufCount * bufSize;
 }
 
 void PLATFORM_SoundLock(void) {}
 void PLATFORM_SoundUnlock(void) {}
 
 /* ------------------------------------------------------------------------
  * Android-specific stubs for JNI connectivity
  * --------------------------------------------------------------------- */
 void Android_SoundInit(int rate, int bufsizems, int bit16, int hq, int disableOSL) {
	 Log_print("Android_SoundInit: %d Hz, %d ms, bit16=%d, hq=%d, disableOSL=%d",
			   rate, bufsizems, bit16, hq, disableOSL);
	 /* Configure core POKEY etc */
	 POKEYSND_bienias_fix = 0;
	 POKEYSND_enable_new_pokey = hq;
	 /* Core sound setup */
	 Sound_desired.freq = rate;
	 Sound_desired.sample_size = bit16 ? 2 : 1;
	 Sound_desired.channels = 1;
	 Sound_desired.buffer_ms = bufsizems;
	 /* Track OSL enable */
	 osl_disable = disableOSL;
	 Android_osl_sound = !disableOSL;
 }
 
 void SoundThread_Update(void *buf, int offs, int len) {
	 /* Process POKEYSND audio into buffer */
	 POKEYSND_Process((UBYTE*)buf + offs, len >> (Sound_desired.sample_size>1));
 }
 
 /* Values for NativeOSLSound */
 /* Android_osl_sound is already defined above */
 
 /* ------------------------------------------------------------------------
  * Android glue for core API
  * --------------------------------------------------------------------- */
 int Sound_Initialise(int *argc, char *argv[]) {
	 return TRUE;
 }
 
 void Sound_Exit(void)     { PLATFORM_SoundExit(); }
 void Sound_Pause(void)    { if (Sound_enabled && !paused) PLATFORM_SoundPause(); paused = 1; }
 void Sound_Continue(void) { if (Sound_enabled && paused)  PLATFORM_SoundContinue(); paused = 0; }
 void Sound_Update(void)   { /* no-op; buffer queue callback */ }
 
 /* ------------------------------------------------------------------------
  * Core Sound Configuration & Control
  * --------------------------------------------------------------------- */
 int Sound_ReadConfig(char *option, char *ptr) {
	 if (!strcmp(option, "SOUND_ENABLED"))
		 return (Sound_enabled = Util_sscanbool(ptr)) != -1;
	 if (!strcmp(option, "SOUND_RATE"))
		 return (Sound_desired.freq = Util_sscandec(ptr)) != -1;
	 if (!strcmp(option, "SOUND_BITS"))
		 return (Sound_desired.sample_size = Util_sscandec(ptr)/8) != -1;
	 if (!strcmp(option, "SOUND_BUFFER_MS"))
		 return (Sound_desired.buffer_ms = Util_sscandec(ptr)) != -1;
	 if (!strcmp(option, "SOUND_LATENCY"))
		 return (Sound_latency = Util_sscandec(ptr)) != -1;
	 return FALSE;
 }
 
 int Sound_WriteConfig(FILE *fp) {
	 fprintf(fp, "SOUND_ENABLED=%d\n", Sound_enabled);
	 fprintf(fp, "SOUND_RATE=%u\n", Sound_desired.freq);
	 fprintf(fp, "SOUND_BITS=%u\n", Sound_desired.sample_size*8);
	 fprintf(fp, "SOUND_BUFFER_MS=%u\n", Sound_desired.buffer_ms);
	 fprintf(fp, "SOUND_LATENCY=%u\n", Sound_latency);
	 return TRUE;
 }
 
 int Sound_Setup(void) {
	 if (!Sound_enabled) return TRUE;
	 return PLATFORM_SoundSetup(&Sound_desired);
 }
 
 double Sound_AdjustSpeed(void) { return 1.0; }
 void Sound_SetLatency(unsigned int latency) { Sound_latency = latency; }
 unsigned int Sound_NextPow2(unsigned r) { unsigned v=1; while(v<r) v<<=1; return v; }
 
 /* EOF */
 