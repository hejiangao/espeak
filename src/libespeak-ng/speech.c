/*
 * Copyright (C) 2005 to 2013 by Jonathan Duddington
 * email: jonsd@users.sourceforge.net
 * Copyright (C) 2013-2016 Reece H. Dunn
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see: <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "errno.h"
#include "stdio.h"
#include "ctype.h"
#include "string.h"
#include "stdlib.h"
#include "wchar.h"
#include "locale.h"
#include <assert.h>
#include <time.h>

#include "speech.h"

#include <sys/stat.h>
#ifdef PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <winreg.h>
#else  /* PLATFORM_POSIX */
#include <unistd.h>
#endif

#include "espeak_ng.h"
#include "speak_lib.h"

#include "phoneme.h"
#include "synthesize.h"
#include "voice.h"
#include "translate.h"

#include "fifo.h"
#include "event.h"
#include "wave.h"

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

unsigned char *outbuf = NULL;

espeak_EVENT *event_list = NULL;
int event_list_ix = 0;
int n_event_list;
long count_samples;
void *my_audio = NULL;

static const char *option_device = NULL;
static unsigned int my_unique_identifier = 0;
static void *my_user_data = NULL;
static espeak_ng_OUTPUT_MODE my_mode = ENOUTPUT_MODE_SYNCHRONOUS;
static int out_samplerate = 0;
static int voice_samplerate = 22050;
static espeak_ERROR err = EE_OK;

t_espeak_callback *synth_callback = NULL;
int (*uri_callback)(int, const char *, const char *) = NULL;
int (*phoneme_callback)(const char *) = NULL;

char path_home[N_PATH_HOME]; // this is the espeak-data directory
extern int saved_parameters[N_SPEECH_PARAM]; // Parameters saved on synthesis start

void WVoiceChanged(voice_t *wvoice)
{
	// Voice change in wavegen
	voice_samplerate = wvoice->samplerate;
}

static espeak_ERROR status_to_espeak_error(espeak_ng_STATUS status)
{
	switch (status)
	{
	case ENS_OK:               return EE_OK;
	case ENOENT:               return EE_NOT_FOUND;
	case ENS_FIFO_BUFFER_FULL: return EE_BUFFER_FULL;
	default:                   return EE_INTERNAL_ERROR;
	}
}

#ifdef USE_ASYNC

static int dispatch_audio(short *outbuf, int length, espeak_EVENT *event)
{
	int a_wave_can_be_played = fifo_is_command_enabled();

	switch (my_mode)
	{
	case ENOUTPUT_MODE_SPEAK_AUDIO:
	{
		int event_type = 0;
		if (event)
			event_type = event->type;

		if (event_type == espeakEVENT_SAMPLERATE) {
			voice_samplerate = event->id.number;

			if (out_samplerate != voice_samplerate) {
				if (out_samplerate != 0) {
					// sound was previously open with a different sample rate
					wave_close(my_audio);
					sleep(1);
				}
				out_samplerate = voice_samplerate;
				my_audio = wave_open(voice_samplerate, option_device);
				if (!my_audio) {
					err = EE_INTERNAL_ERROR;
					return -1;
				}
				wave_set_callback_is_output_enabled(fifo_is_command_enabled);
				event_init();
			}
		}

		if (outbuf && length && a_wave_can_be_played) {
			wave_write(my_audio, (char *)outbuf, 2*length);
		}

		while (a_wave_can_be_played) {
			// TBD: some event are filtered here but some insight might be given
			// TBD: in synthesise.cpp for avoiding to create WORDs with size=0.
			// TBD: For example sentence "or ALT)." returns three words
			// "or", "ALT" and "".
			// TBD: the last one has its size=0.
			if (event && (event->type == espeakEVENT_WORD) && (event->length == 0))
				break;
			espeak_ERROR a_error = event_declare(event);
			if (a_error != EE_BUFFER_FULL)
				break;
			usleep(10000);
			a_wave_can_be_played = fifo_is_command_enabled();
		}
	}
		break;
	case 0:
		if (synth_callback)
			synth_callback(outbuf, length, event);
		break;
	}

	return a_wave_can_be_played == 0; // 1 = stop synthesis, -1 = error
}

static int create_events(short *outbuf, int length, espeak_EVENT *event_list, uint32_t the_write_pos)
{
	int finished;
	int i = 0;

	// The audio data are written to the output device.
	// The list of events in event_list (index: event_list_ix) is read:
	// Each event is declared to the "event" object which stores them internally.
	// The event object is responsible of calling the external callback
	// as soon as the relevant audio sample is played.

	do { // for each event
		espeak_EVENT *event;
		if (event_list_ix == 0)
			event = NULL;
		else {
			event = event_list + i;
			event->sample += the_write_pos;
		}
		finished = dispatch_audio((short *)outbuf, length, event);
		length = 0; // the wave data are played once.
		i++;
	} while ((i < event_list_ix) && !finished);
	return finished;
}

int sync_espeak_terminated_msg(uint32_t unique_identifier, void *user_data)
{
	int finished = 0;

	memset(event_list, 0, 2*sizeof(espeak_EVENT));

	event_list[0].type = espeakEVENT_MSG_TERMINATED;
	event_list[0].unique_identifier = unique_identifier;
	event_list[0].user_data = user_data;
	event_list[1].type = espeakEVENT_LIST_TERMINATED;
	event_list[1].unique_identifier = unique_identifier;
	event_list[1].user_data = user_data;

	if (my_mode == ENOUTPUT_MODE_SPEAK_AUDIO) {
		while (1) {
			espeak_ERROR a_error = event_declare(event_list);
			if (a_error != EE_BUFFER_FULL)
				break;
			usleep(10000);
		}
	} else {
		if (synth_callback)
			finished = synth_callback(NULL, 0, event_list);
	}
	return finished;
}

#endif

#pragma GCC visibility push(default)
ESPEAK_NG_API espeak_ng_STATUS espeak_ng_InitializeOutput(espeak_ng_OUTPUT_MODE output_mode, int buffer_length, const char *device)
{
	option_device = device;
	my_mode = output_mode;
	my_audio = NULL;
	option_waveout = 1; // inhibit portaudio callback from wavegen.cpp
	out_samplerate = 0;

	if (output_mode == (ENOUTPUT_MODE_SYNCHRONOUS | ENOUTPUT_MODE_SPEAK_AUDIO)) {
		option_waveout = 0;
		WavegenInitSound();
	}

	// buflength is in mS, allocate 2 bytes per sample
	if ((buffer_length == 0) || (output_mode & ENOUTPUT_MODE_SPEAK_AUDIO))
		buffer_length = 200;

	outbuf_size = (buffer_length * samplerate)/500;
	outbuf = (unsigned char *)realloc(outbuf, outbuf_size);
	if ((out_start = outbuf) == NULL)
		return ENOMEM;

	// allocate space for event list.  Allow 200 events per second.
	// Add a constant to allow for very small buf_length
	n_event_list = (buffer_length*200)/1000 + 20;
	if ((event_list = (espeak_EVENT *)realloc(event_list, sizeof(espeak_EVENT) * n_event_list)) == NULL)
		return ENOMEM;

	return ENS_OK;
}

int GetFileLength(const char *filename)
{
	struct stat statbuf;

	if (stat(filename, &statbuf) != 0)
		return 0;

	if (S_ISDIR(statbuf.st_mode))
		return -2; // a directory

	return statbuf.st_size;
}
#pragma GCC visibility pop

char *Alloc(int size)
{
	char *p;
	if ((p = (char *)malloc(size)) == NULL)
		fprintf(stderr, "Can't allocate memory\n"); // I was told that size+1 fixes a crash on 64-bit systems
	return p;
}

void Free(void *ptr)
{
	if (ptr != NULL)
		free(ptr);
}

#pragma GCC visibility push(default)
ESPEAK_NG_API void espeak_ng_InitializePath(const char *path)
{
	if (path != NULL) {
		sprintf(path_home, "%s/espeak-data", path);
		return;
	}

#ifdef PLATFORM_WINDOWS
	HKEY RegKey;
	unsigned long size;
	unsigned long var_type;
	char *env;
	unsigned char buf[sizeof(path_home)-13];

	if ((env = getenv("ESPEAK_DATA_PATH")) != NULL) {
		sprintf(path_home, "%s/espeak-data", env);
		if (GetFileLength(path_home) == -2)
			return; // an espeak-data directory exists
	}

	buf[0] = 0;
	RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Speech\\Voices\\Tokens\\eSpeak", 0, KEY_READ, &RegKey);
	size = sizeof(buf);
	var_type = REG_SZ;
	RegQueryValueExA(RegKey, "path", 0, &var_type, buf, &size);

	sprintf(path_home, "%s\\espeak-data", buf);
#elif defined(PLATFORM_DOS)
	strcpy(path_home, PATH_ESPEAK_DATA);
#else
	char *env;

	// check for environment variable
	if ((env = getenv("ESPEAK_DATA_PATH")) != NULL) {
		snprintf(path_home, sizeof(path_home), "%s/espeak-data", env);
		if (GetFileLength(path_home) == -2)
			return; // an espeak-data directory exists
	}

	snprintf(path_home, sizeof(path_home), "%s/espeak-data", getenv("HOME"));
	if (access(path_home, R_OK) != 0)
		strcpy(path_home, PATH_ESPEAK_DATA);
#endif
}

ESPEAK_NG_API espeak_ng_STATUS espeak_ng_Initialize(void)
{
	int param;
	int srate = 22050; // default sample rate 22050 Hz

	// It seems that the wctype functions don't work until the locale has been set
	// to something other than the default "C".  Then, not only Latin1 but also the
	// other characters give the correct results with iswalpha() etc.
	if (setlocale(LC_CTYPE, "C.UTF-8") == NULL) {
		if (setlocale(LC_CTYPE, "UTF-8") == NULL) {
			if (setlocale(LC_CTYPE, "en_US.UTF-8") == NULL)
				setlocale(LC_CTYPE, "");
		}
	}

	espeak_ng_STATUS result = LoadPhData(&srate);
	if (result != ENS_OK)
		return result;

	WavegenInit(srate, 0);
	LoadConfig();

	memset(&current_voice_selected, 0, sizeof(current_voice_selected));
	SetVoiceStack(NULL, "");
	SynthesizeInit();
	InitNamedata();

	VoiceReset(0);

	for (param = 0; param < N_SPEECH_PARAM; param++)
		param_stack[0].parameter[param] = param_defaults[param];

	SetParameter(espeakRATE, 175, 0);
	SetParameter(espeakVOLUME, 100, 0);
	SetParameter(espeakCAPITALS, option_capitals, 0);
	SetParameter(espeakPUNCTUATION, option_punctuation, 0);
	SetParameter(espeakWORDGAP, 0, 0);

#ifdef USE_ASYNC
	fifo_init();
#endif

	option_phonemes = 0;
	option_phoneme_events = 0;

	return ENS_OK;
}

ESPEAK_NG_API int espeak_ng_GetSampleRate(void)
{
	return samplerate;
}
#pragma GCC visibility pop

static espeak_ERROR Synthesize(unsigned int unique_identifier, const void *text, int flags)
{
	// Fill the buffer with output sound
	int length;
	int finished = 0;
	int count_buffers = 0;
#ifdef USE_ASYNC
	uint32_t a_write_pos = 0;
#endif

	if ((outbuf == NULL) || (event_list == NULL))
		return EE_INTERNAL_ERROR; // espeak_Initialize()  has not been called

	option_multibyte = flags & 7;
	option_ssml = flags & espeakSSML;
	option_phoneme_input = flags & espeakPHONEMES;
	option_endpause = flags & espeakENDPAUSE;

	count_samples = 0;

#ifdef USE_ASYNC
	if (my_mode == ENOUTPUT_MODE_SPEAK_AUDIO)
		a_write_pos = wave_get_write_position(my_audio);
#endif

	if (translator == NULL)
		espeak_SetVoiceByName("default");

	SpeakNextClause(NULL, text, 0);

	if (my_mode == (ENOUTPUT_MODE_SYNCHRONOUS | ENOUTPUT_MODE_SPEAK_AUDIO)) {
		for (;;) {
#ifdef PLATFORM_WINDOWS
			Sleep(300); // 0.3s
#else
#ifdef USE_NANOSLEEP
			struct timespec period;
			struct timespec remaining;
			period.tv_sec = 0;
			period.tv_nsec = 300000000; // 0.3 sec
			nanosleep(&period, &remaining);
#else
			sleep(1);
#endif
#endif
			if (SynthOnTimer() != 0)
				break;
		}
		return EE_OK;
	}

	for (;;) {
		out_ptr = outbuf;
		out_end = &outbuf[outbuf_size];
		event_list_ix = 0;
		WavegenFill();

		length = (out_ptr - outbuf)/2;
		count_samples += length;
		event_list[event_list_ix].type = espeakEVENT_LIST_TERMINATED; // indicates end of event list
		event_list[event_list_ix].unique_identifier = unique_identifier;
		event_list[event_list_ix].user_data = my_user_data;

		count_buffers++;
		if (my_mode == ENOUTPUT_MODE_SPEAK_AUDIO) {
#ifdef USE_ASYNC
			finished = create_events((short *)outbuf, length, event_list, a_write_pos);
			if (finished < 0)
				return EE_INTERNAL_ERROR;
			length = 0; // the wave data are played once.
#endif
		} else
			finished = synth_callback((short *)outbuf, length, event_list);
		if (finished) {
			SpeakNextClause(NULL, 0, 2); // stop
			break;
		}

		if (Generate(phoneme_list, &n_phoneme_list, 1) == 0) {
			if (WcmdqUsed() == 0) {
				// don't process the next clause until the previous clause has finished generating speech.
				// This ensures that <audio> tag (which causes end-of-clause) is at a sound buffer boundary

				event_list[0].type = espeakEVENT_LIST_TERMINATED;
				event_list[0].unique_identifier = my_unique_identifier;
				event_list[0].user_data = my_user_data;

				if (SpeakNextClause(NULL, NULL, 1) == 0) {
#ifdef USE_ASYNC
					if (my_mode == ENOUTPUT_MODE_SPEAK_AUDIO) {
						if (dispatch_audio(NULL, 0, NULL) < 0) // TBD: test case
							return err = EE_INTERNAL_ERROR;
					} else
						synth_callback(NULL, 0, event_list); // NULL buffer ptr indicates end of data
#else
					synth_callback(NULL, 0, event_list); // NULL buffer ptr indicates end of data
#endif
					break;
				}
			}
		}
	}
	return EE_OK;
}

void MarkerEvent(int type, unsigned int char_position, int value, int value2, unsigned char *out_ptr)
{
	// type: 1=word, 2=sentence, 3=named mark, 4=play audio, 5=end, 7=phoneme
	espeak_EVENT *ep;
	double time;

	if ((event_list == NULL) || (event_list_ix >= (n_event_list-2)))
		return;

	ep = &event_list[event_list_ix++];
	ep->type = (espeak_EVENT_TYPE)type;
	ep->unique_identifier = my_unique_identifier;
	ep->user_data = my_user_data;
	ep->text_position = char_position & 0xffffff;
	ep->length = char_position >> 24;

	time = ((double)(count_samples + mbrola_delay + (out_ptr - out_start)/2)*1000.0)/samplerate;
	ep->audio_position = (int)time;
	ep->sample = (count_samples + mbrola_delay + (out_ptr - out_start)/2);

	if ((type == espeakEVENT_MARK) || (type == espeakEVENT_PLAY))
		ep->id.name = &namedata[value];
	else if (type == espeakEVENT_PHONEME) {
		int *p;
		p = (int *)(ep->id.string);
		p[0] = value;
		p[1] = value2;
	} else
		ep->id.number = value;
}

espeak_ERROR sync_espeak_Synth(unsigned int unique_identifier, const void *text, size_t size,
                               unsigned int position, espeak_POSITION_TYPE position_type,
                               unsigned int end_position, unsigned int flags, void *user_data)
{
	(void)size; // unused

	espeak_ERROR aStatus;

	InitText(flags);
	my_unique_identifier = unique_identifier;
	my_user_data = user_data;

	for (int i = 0; i < N_SPEECH_PARAM; i++)
		saved_parameters[i] = param_stack[0].parameter[i];

	switch (position_type)
	{
	case POS_CHARACTER:
		skip_characters = position;
		break;
	case POS_WORD:
		skip_words = position;
		break;
	case POS_SENTENCE:
		skip_sentences = position;
		break;

	}
	if (skip_characters || skip_words || skip_sentences)
		skipping_text = 1;

	end_character_position = end_position;

	aStatus = Synthesize(unique_identifier, text, flags);
	#ifdef USE_ASYNC
	wave_flush(my_audio);
	#endif

	return aStatus;
}

espeak_ERROR sync_espeak_Synth_Mark(unsigned int unique_identifier, const void *text, size_t size,
                                    const char *index_mark, unsigned int end_position,
                                    unsigned int flags, void *user_data)
{
	(void)size; // unused

	espeak_ERROR aStatus;

	InitText(flags);

	my_unique_identifier = unique_identifier;
	my_user_data = user_data;

	if (index_mark != NULL) {
		strncpy0(skip_marker, index_mark, sizeof(skip_marker));
		skipping_text = 1;
	}

	end_character_position = end_position;

	aStatus = Synthesize(unique_identifier, text, flags | espeakSSML);

	return aStatus;
}

void sync_espeak_Key(const char *key)
{
	// symbolic name, symbolicname_character  - is there a system resource of symbolic names per language?
	int letter;
	int ix;

	ix = utf8_in(&letter, key);
	if (key[ix] == 0) {
		// a single character
		sync_espeak_Char(letter);
		return;
	}

	my_unique_identifier = 0;
	my_user_data = NULL;
	Synthesize(0, key, 0); // speak key as a text string
}

void sync_espeak_Char(wchar_t character)
{
	// is there a system resource of character names per language?
	char buf[80];
	my_unique_identifier = 0;
	my_user_data = NULL;

	sprintf(buf, "<say-as interpret-as=\"tts:char\">&#%d;</say-as>", character);
	Synthesize(0, buf, espeakSSML);
}

void sync_espeak_SetPunctuationList(const wchar_t *punctlist)
{
	// Set the list of punctuation which are spoken for "some".
	my_unique_identifier = 0;
	my_user_data = NULL;

	option_punctlist[0] = 0;
	if (punctlist != NULL) {
		wcsncpy(option_punctlist, punctlist, N_PUNCTLIST);
		option_punctlist[N_PUNCTLIST-1] = 0;
	}
}

#pragma GCC visibility push(default)

ESPEAK_API void espeak_SetSynthCallback(t_espeak_callback *SynthCallback)
{
	synth_callback = SynthCallback;
#ifdef USE_ASYNC
	event_set_callback(synth_callback);
#endif
}

ESPEAK_API void espeak_SetUriCallback(int (*UriCallback)(int, const char *, const char *))
{
	uri_callback = UriCallback;
}

ESPEAK_API void espeak_SetPhonemeCallback(int (*PhonemeCallback)(const char *))
{
	phoneme_callback = PhonemeCallback;
}

ESPEAK_API int espeak_Initialize(espeak_AUDIO_OUTPUT output_type, int buf_length, const char *path, int options)
{
	int param;

	espeak_ng_InitializePath(path);
	espeak_ng_STATUS result = espeak_ng_Initialize();
	if (result != ENS_OK) {
		if (result == ENS_VERSION_MISMATCH)
			fprintf(stderr, "Wrong version of espeak-data (expected 0x%x) at %s\n", version_phdata, path_home);
		else {
			fprintf(stderr, "Failed to load espeak-data\n");
			if ((options & espeakINITIALIZE_DONT_EXIT) == 0)
				exit(1);
		}
	}

	switch (output_type)
	{
	case AUDIO_OUTPUT_PLAYBACK:
		espeak_ng_InitializeOutput(ENOUTPUT_MODE_SPEAK_AUDIO, buf_length, NULL);
		break;
	case AUDIO_OUTPUT_RETRIEVAL:
		espeak_ng_InitializeOutput(0, buf_length, NULL);
		break;
	case AUDIO_OUTPUT_SYNCHRONOUS:
		espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, buf_length, NULL);
		break;
	case AUDIO_OUTPUT_SYNCH_PLAYBACK:
		espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS | ENOUTPUT_MODE_SPEAK_AUDIO, buf_length, NULL);
		break;
	}

	option_phoneme_events = (options & (espeakINITIALIZE_PHONEME_EVENTS | espeakINITIALIZE_PHONEME_IPA));

	return samplerate;
}

ESPEAK_API espeak_ERROR espeak_Synth(const void *text, size_t size,
                                     unsigned int position,
                                     espeak_POSITION_TYPE position_type,
                                     unsigned int end_position, unsigned int flags,
                                     unsigned int *unique_identifier, void *user_data)
{
	if (f_logespeak) {
		fprintf(f_logespeak, "\nSYNTH posn %d %d %d flags 0x%x\n%s\n", position, end_position, position_type, flags, (const char *)text);
		fflush(f_logespeak);
	}

	espeak_ERROR a_error = EE_INTERNAL_ERROR;
	static unsigned int temp_identifier;

	if (unique_identifier == NULL)
		unique_identifier = &temp_identifier;
	*unique_identifier = 0;

	if (my_mode & ENOUTPUT_MODE_SYNCHRONOUS)
		return sync_espeak_Synth(0, text, size, position, position_type, end_position, flags, user_data);

#ifdef USE_ASYNC
	// Create the text command
	t_espeak_command *c1 = create_espeak_text(text, size, position, position_type, end_position, flags, user_data);

	// Retrieve the unique identifier
	*unique_identifier = c1->u.my_text.unique_identifier;

	// Create the "terminated msg" command (same uid)
	t_espeak_command *c2 = create_espeak_terminated_msg(*unique_identifier, user_data);

	// Try to add these 2 commands (single transaction)
	if (c1 && c2) {
		a_error = status_to_espeak_error(fifo_add_commands(c1, c2));
		if (a_error != EE_OK) {
			delete_espeak_command(c1);
			delete_espeak_command(c2);
			c1 = c2 = NULL;
		}
	} else {
		delete_espeak_command(c1);
		delete_espeak_command(c2);
	}
#endif
	return a_error;
}

ESPEAK_API espeak_ERROR espeak_Synth_Mark(const void *text, size_t size,
                                          const char *index_mark,
                                          unsigned int end_position,
                                          unsigned int flags,
                                          unsigned int *unique_identifier,
                                          void *user_data)
{
	espeak_ERROR a_error = EE_OK;
	static unsigned int temp_identifier;

	if (f_logespeak)
		fprintf(f_logespeak, "\nSYNTH MARK %s posn %d flags 0x%x\n%s\n", index_mark, end_position, flags, (const char *)text);

	if (unique_identifier == NULL)
		unique_identifier = &temp_identifier;
	*unique_identifier = 0;

	if (my_mode & ENOUTPUT_MODE_SYNCHRONOUS)
		return sync_espeak_Synth_Mark(0, text, size, index_mark, end_position, flags, user_data);

#ifdef USE_ASYNC
	// Create the mark command
	t_espeak_command *c1 = create_espeak_mark(text, size, index_mark, end_position,
	                                          flags, user_data);

	// Retrieve the unique identifier
	*unique_identifier = c1->u.my_mark.unique_identifier;

	// Create the "terminated msg" command (same uid)
	t_espeak_command *c2 = create_espeak_terminated_msg(*unique_identifier, user_data);

	// Try to add these 2 commands (single transaction)
	if (c1 && c2) {
		a_error = status_to_espeak_error(fifo_add_commands(c1, c2));
		if (a_error != EE_OK) {
			delete_espeak_command(c1);
			delete_espeak_command(c2);
			c1 = c2 = NULL;
		}
	} else {
		delete_espeak_command(c1);
		delete_espeak_command(c2);
	}
#endif
	return a_error;
}

ESPEAK_API espeak_ERROR espeak_Key(const char *key)
{
	// symbolic name, symbolicname_character  - is there a system resource of symbolicnames per language

	if (f_logespeak)
		fprintf(f_logespeak, "\nKEY %s\n", key);

	espeak_ERROR a_error = EE_OK;

	if (my_mode & ENOUTPUT_MODE_SYNCHRONOUS) {
		sync_espeak_Key(key);
		return EE_OK;
	}

#ifdef USE_ASYNC
	t_espeak_command *c = create_espeak_key(key, NULL);
	a_error = status_to_espeak_error(fifo_add_command(c));
	if (a_error != EE_OK)
		delete_espeak_command(c);
#endif
	return a_error;
}

ESPEAK_API espeak_ERROR espeak_Char(wchar_t character)
{
	// is there a system resource of character names per language?

	if (f_logespeak)
		fprintf(f_logespeak, "\nCHAR U+%x\n", character);

#ifdef USE_ASYNC
	espeak_ERROR a_error;

	if (my_mode & ENOUTPUT_MODE_SYNCHRONOUS) {
		sync_espeak_Char(character);
		return EE_OK;
	}

	t_espeak_command *c = create_espeak_char(character, NULL);
	a_error = status_to_espeak_error(fifo_add_command(c));
	if (a_error != EE_OK)
		delete_espeak_command(c);
	return a_error;
#else
	sync_espeak_Char(character);
	return EE_OK;
#endif
}

ESPEAK_API int espeak_GetParameter(espeak_PARAMETER parameter, int current)
{
	// current: 0=default value, 1=current value
	if (current)
		return param_stack[0].parameter[parameter];
	return param_defaults[parameter];
}

ESPEAK_API espeak_ERROR espeak_SetParameter(espeak_PARAMETER parameter, int value, int relative)
{
	if (f_logespeak)
		fprintf(f_logespeak, "SETPARAM %d %d %d\n", parameter, value, relative);
#ifdef USE_ASYNC
	espeak_ERROR a_error;

	if (my_mode & ENOUTPUT_MODE_SYNCHRONOUS) {
		SetParameter(parameter, value, relative);
		return EE_OK;
	}

	t_espeak_command *c = create_espeak_parameter(parameter, value, relative);

	a_error = status_to_espeak_error(fifo_add_command(c));
	if (a_error != EE_OK)
		delete_espeak_command(c);
	return a_error;
#else
	SetParameter(parameter, value, relative);
	return EE_OK;
#endif
}

ESPEAK_API espeak_ERROR espeak_SetPunctuationList(const wchar_t *punctlist)
{
	// Set the list of punctuation which are spoken for "some".

#ifdef USE_ASYNC
	espeak_ERROR a_error;

	if (my_mode & ENOUTPUT_MODE_SYNCHRONOUS) {
		sync_espeak_SetPunctuationList(punctlist);
		return EE_OK;
	}

	t_espeak_command *c = create_espeak_punctuation_list(punctlist);
	a_error = status_to_espeak_error(fifo_add_command(c));
	if (a_error != EE_OK)
		delete_espeak_command(c);
	return a_error;
#else
	sync_espeak_SetPunctuationList(punctlist);
	return EE_OK;
#endif
}

ESPEAK_API void espeak_SetPhonemeTrace(int phonememode, FILE *stream)
{
	/* phonememode:  Controls the output of phoneme symbols for the text
	      bits 0-2:
	         value=0  No phoneme output (default)
	         value=1  Output the translated phoneme symbols for the text
	         value=2  as (1), but produces IPA phoneme names rather than ascii
	      bit 3:   output a trace of how the translation was done (showing the matching rules and list entries)
	      bit 4:   produce pho data for mbrola
	      bit 7:   use (bits 8-23) as a tie within multi-letter phonemes names
	      bits 8-23:  separator character, between phoneme names

	   stream   output stream for the phoneme symbols (and trace).  If stream=NULL then it uses stdout.
	*/

	option_phonemes = phonememode;
	f_trans = stream;
	if (stream == NULL)
		f_trans = stderr;
}

ESPEAK_API const char *espeak_TextToPhonemes(const void **textptr, int textmode, int phonememode)
{
	/* phoneme_mode
	    bit 1:   0=eSpeak's ascii phoneme names, 1= International Phonetic Alphabet (as UTF-8 characters).
	    bit 7:   use (bits 8-23) as a tie within multi-letter phonemes names
	    bits 8-23:  separator character, between phoneme names
	 */

	option_multibyte = textmode & 7;
	*textptr = TranslateClause(translator, NULL, *textptr, NULL, NULL);
	return GetTranslatedPhonemeString(phonememode);
}

ESPEAK_API void espeak_CompileDictionary(const char *path, FILE *log, int flags)
{
	espeak_ng_CompileDictionary(path, dictionary_name, log, flags);
}

ESPEAK_API espeak_ERROR espeak_Cancel(void)
{
#ifdef USE_ASYNC
	fifo_stop();
	event_clear_all();

	if (my_mode == ENOUTPUT_MODE_SPEAK_AUDIO)
		wave_close(my_audio);
#endif
	embedded_value[EMBED_T] = 0; // reset echo for pronunciation announcements

	for (int i = 0; i < N_SPEECH_PARAM; i++)
		SetParameter(i, saved_parameters[i], 0);

	return EE_OK;
}

ESPEAK_API int espeak_IsPlaying(void)
{
#ifdef USE_ASYNC
	if ((my_mode == ENOUTPUT_MODE_SPEAK_AUDIO) && wave_is_busy(my_audio))
		return 1;

	return fifo_is_busy();
#else
	return 0;
#endif
}

ESPEAK_API espeak_ERROR espeak_Synchronize(void)
{
	espeak_ERROR berr = err;
#ifdef USE_ASYNC
	while (espeak_IsPlaying())
		usleep(20000);
#endif
	err = EE_OK;
	return berr;
}

extern void FreePhData(void);
extern void FreeVoiceList(void);

ESPEAK_API espeak_ERROR espeak_Terminate(void)
{
#ifdef USE_ASYNC
	fifo_stop();
	fifo_terminate();
	event_terminate();

	if (my_mode == ENOUTPUT_MODE_SPEAK_AUDIO) {
		wave_close(my_audio);
		wave_terminate();
		out_samplerate = 0;
	}
#endif
	Free(event_list);
	event_list = NULL;
	Free(outbuf);
	outbuf = NULL;
	FreePhData();
	FreeVoiceList();

	if (f_logespeak) {
		fclose(f_logespeak);
		f_logespeak = NULL;
	}

	return EE_OK;
}

ESPEAK_API const char *espeak_Info(const char **ptr)
{
	if (ptr != NULL)
		*ptr = path_home;
	return version_string;
}

#pragma GCC visibility pop