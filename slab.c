/****************************************************************************
 * SLug Audio Blaster
 *
 * Real-time audio processing
 *
 * Copyright (C) Jean Zundel <jzu@free.fr> 2010 
 *
 * 
 * slab is an evolutive guitar effect designed for the Linksys NSLU2 running 
 * GNU/Linux, but it works on any ALSA-based architecture.
 *
 * COMPILATION
 *  gcc slab.c -Wall -g -lasound -lpthread -o slab
 * You'll need libasound2 and libpthread libraries (+devel), and gcc.
 *
 * TESTING
 * Running it as root allows to use the Slug's LEDs.
 * This program needs an USB joystick - or something similar - to operate.
 * It currently manages two potentiometers and four switches, but there's
 * room for easy expansion.
 * The -d option ouputs debug messages (ALSA errors and joystick events).
 * 
 * RUNNING
 * Once you're all set, you want to edit /etc/inittab to insert this line:
 * sl:23:respawn:/[PATH_TO]/slab
 * (obviously replacing [PATH_TO] with its actual path)
 * which will ensure that init(8) restarts the program when it is stopped.
 * 
 * CAVEAT
 * Enormous hiss! The "noise gate" effect helps only to a point.
 * Any external process/event affects ALSA management and creates an 
 * unwanted delay. You have to stop the program, which will be restarted
 * by init if /etc/inittab is set up accordingly.
 * 
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * $Id: slab.c,v 1.7 2010/07/18 15:13:53 slug Exp $
 ****************************************************************************/

#define ALSA_PCM_NEW_HW_PARAMS_API

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <signal.h>


#define INITIAL_LENGTH 0x100000

#define DEBUG if (debug) printf
#define ERROR if (debug) fprintf

// Switches

#define SW_FLG 2
#define SW_DLY 3
#define SW_HLT 0
#define SW_DIS 1

// Pots & adjustment

#define POTMOD 0
#define POTDIS 2 
#define FACMOD 30
#define FACDIS 3000
#define PADMOD 1200
#define PADDIS 10

// LEDs

#define LED_DISK1  "/sys/class/leds/nslu2:green:disk-1"
#define LED_DISK2  "/sys/class/leds/nslu2:green:disk-2"
#define LED_READY  "/sys/class/leds/nslu2:green:ready"
#define LED_STATUS "/sys/class/leds/nslu2:red:status"


short sinus [SHRT_MAX];              // 1/4 wave for sin() integer simulation 

short procbuf1 [INITIAL_LENGTH],     // Ring buffers
      procbuf2 [INITIAL_LENGTH];
short *procbuf,                      // Pointers to ring buffers
      *procbend,
      *tempbend;
int   asize,                         // ALSA stereo buffer size
      bsize,                         // Mono buffer size (in bytes)
      ssize,                         // Mono buffer size (in samples)
      buflen;

int   joydis,
      joymod = 1000;                         // Values read by joystick()
int   debug = 0;

pthread_t thread;                    // Joystick thread

snd_pcm_uframes_t frames = 44;
snd_pcm_t *handle_rec,
          *handle_play;
short   *recbuf,                     // ALSA I/O buffers
        *playbuf;

int   flange_flag=0;
int   delay_flag=0;
int   dist_flag=0;

void  swapbufs ();
short get_sample (short *p);
void  *joystick ();
void  set_led (char *led, int i);
short push_pull (short sample);
void  debugsig (int signum);

short debugbuf1[100000],
      debugbuf2[100000],
      debugbuf3[100000];

/****************************************************************************
 * Init, main read/process/write loop
 ****************************************************************************/ 

int main (int argc, char **argv) {

  int i, d;
  int rc;
  int spl;                           // Sample offset in a mono frame
  int fl_val = 1000;                 // Delay effect needs "integrated"
                                     // joystick variations
  snd_pcm_hw_params_t *params_rec,
                      *params_play;
  int gate;                          // Noise detector
  int dir;                           // Result for ALSA operations
  unsigned int val;                  // Sample frequency

  int s = 0;

  if ((argc > 1) && (!strcmp(argv[1], "-d")))
    debug = 1;
      

  /* Sinus table (*10000) for push_pull algorithm */

  for (i=0; i<32768; i++)
    sinus [i] = (short)(sin ((float)i / 20860.0)*20000);//10000);

  /* LEDs off */

  set_led (LED_DISK1, 0);
  set_led (LED_DISK2, 0);
  set_led (LED_READY, 0);
  set_led (LED_STATUS,0);

  /* Let the joystick live its life */

  pthread_create (&thread, NULL, joystick, NULL);

  signal (SIGINT, debugsig);

  // Processing ring buffer init

  procbuf   = procbuf1;

  // ALSA init

  asize = frames * 4;                // 2 bytes/sample, 2 channels 
  val  = 44100;                      // Should be 48k here, which doesn't work

  recbuf  = (short *)malloc (asize);
  playbuf = (short *)malloc (asize);

  /* PCM capture setup */

  rc = snd_pcm_open (&handle_rec, "plughw:0", SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    ERROR (stderr,
           "rec - unable to open pcm device: %s\n", snd_strerror(rc));
    exit (1);
  }

  /* Allocate a hardware parameters object. 
   * Fill it in with default values.
   * Set the desired hardware parameters: 
   *  Interleaved mode 
   *  Signed 16-bit little-endian format
   *  Two channels (stereo) 
   *  44100 bits/second sampling rate (CD quality) 
   *  Set period size to 44 frames. (?)
   * Write the parameters to the driver. */

  snd_pcm_hw_params_alloca (&params_rec);
  snd_pcm_hw_params_any (handle_rec, 
                         params_rec);
  snd_pcm_hw_params_set_access (handle_rec, params_rec,
                                SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format (handle_rec, params_rec,
                                SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels (handle_rec, params_rec, 2);
  snd_pcm_hw_params_set_rate_near (handle_rec, params_rec, &val, &dir);
  snd_pcm_hw_params_set_period_size_near (handle_rec, params_rec, &frames, 
                                          &dir);
  rc = snd_pcm_hw_params (handle_rec, params_rec);
  if (rc < 0) {
    ERROR (stderr,
           "rec - unable to set hw parameters: %s\n", snd_strerror (rc));
    exit (1);
  }
  snd_pcm_hw_params_get_period_size (params_rec, &frames, &dir);


  /* PCM playback setup */

  rc = snd_pcm_open (&handle_play, "plughw:0", SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    ERROR (stderr,
           "play - unable to open pcm device: %s\n",
             snd_strerror (rc));
    exit (1);
  }

  snd_pcm_hw_params_alloca (&params_play);
  snd_pcm_hw_params_any (handle_play, params_play);
  snd_pcm_hw_params_set_access (handle_play, params_play,
                                SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format (handle_play, params_play,
                                SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels (handle_play, params_play, 2);
  snd_pcm_hw_params_set_rate_near (handle_play, params_play, &val, &dir);
  snd_pcm_hw_params_set_period_size_near (handle_play, params_play, &frames, 
                                          &dir);
  rc = snd_pcm_hw_params (handle_play, params_play);
  if (rc < 0) {
    ERROR (stderr,
           "play - unable to set hw parameters: %s\n", snd_strerror (rc));
    exit(1);
  }
  snd_pcm_hw_params_get_period_size (params_play, &frames, &dir);

  bsize = asize / 2;                 // Stereo (alsa) -> Mono (bytes)
  ssize = bsize / 2;                 // Bytes -> Samples

  buflen = (INITIAL_LENGTH / ssize) * ssize; // Usable part of procbuf
  procbend = procbuf + buflen;

  DEBUG ("procbuf=%d buflen=%d procbend=%d\n", 
         (int)procbuf, buflen, (int)procbend);

  /* Processing loop */

  while (1) {

    /* Read data from device to capture buffer */

    rc = snd_pcm_readi (handle_rec, recbuf, frames);

    if (rc == -EPIPE) {
      ERROR (stderr, 
             "readi - overrun occurred\n");
      snd_pcm_prepare (handle_rec);
      snd_pcm_prepare (handle_play);
      snd_pcm_writei (handle_play, recbuf, frames);

    } else if (rc < 0) {
      ERROR (stderr,
             "error from read: %s\n", snd_strerror (rc));
    } else if (rc != (int)frames) {
      ERROR (stderr, 
             "short read, read %d frames_rec\n", rc);
    }

    /* Store capture buffer content in ring buffer */

    s += ssize;
    if (s > buflen - ssize)
      s = 0;
    
    for (i=0; i<bsize; i++)
      procbuf [s + i/2] = recbuf [i];        // Down by the jetty


    /* Effects */

    // 1) noise gate - optional, depends on soundcard quality

    gate = 1;
    for (spl = 0; spl < ssize; spl++)
      if (abs (procbuf [s + spl]) > 700) 
        gate = 0;           // Don't attenuate the buffer if there's a signal
    if (gate)
      for (spl = 0; spl < ssize; spl++)
        procbuf [s + spl] = procbuf [s + spl] >> 0;

    // 2) distortion - apply a non-linear function to signal

//    if (dist_flag)
      joydis = (joydis < 0) ? 0 : (joydis > 4) ? 4 : joydis;
      for (spl = 0; spl < ssize; spl++)
        for (d = 0; d < joydis ; d++)
          procbuf [s + spl] = push_pull (procbuf [s + spl]);

    // 3) flanger - mix with a few samples behind

    if (flange_flag) {
      if (fl_val != joymod)                  // Aliasing joystick steps
        fl_val > joymod ? fl_val-- : fl_val++ ;
      for (spl = 0; spl < ssize; spl++) 
        procbuf [s + spl] = (short)(
                ((int)get_sample (procbuf+s + spl) +
                 (int)get_sample (procbuf+s + spl - fl_val))>>1);
    }

    // 4) delay (-6 dB) - 50 times more samples behind

    if (delay_flag)
      for (spl = 0; spl < ssize; spl++) 
        procbuf [s + spl] = (short)(
                ((int)get_sample (procbuf+s + spl) + 
                 (int)(get_sample (procbuf+s + spl - joymod*50) >> 1))/1.5);


    /* Back to stereo */

    for (i = 0; i < ssize; i++) {
      playbuf [i*2] = procbuf [s+i];
      playbuf [i*2+1] = procbuf [s+i];
    }

    /* Write playback buffer content to device */

    rc = snd_pcm_writei (handle_play, playbuf, frames);

    if (rc == -EPIPE) {
      ERROR (stderr, 
             "writei - underrun occurred\n");
      snd_pcm_prepare (handle_rec);
      snd_pcm_prepare (handle_play);
      snd_pcm_writei (handle_play, recbuf, frames);
    } else if (rc < 0) {
      ERROR (stderr,
             "error from write: %s\n", snd_strerror (rc));
    }  else if (rc != (int)frames) {
      ERROR (stderr,
             "short write, write %d frames\n", rc);
    }
  }

  return 0;
}


/**************************************************************************** 
 * get_sample()
 *
 * Ring buffer accessor, normalizes offset in the [0-buflen[ range 
 * p - Sample offset
 * returns Sample
 ****************************************************************************/

short get_sample (short *p) {

  while ((int)p < (int)procbuf)
    p += buflen;
  while ((int)p >= (int)(procbuf+buflen))
    p -= buflen;
  return *p;
}


/**************************************************************************** 
 * joystick()
 *
 * Separate thread
 * Detects two pots and four switches 
 ****************************************************************************/

void *joystick ()
{
  int jfd;
  struct js_event ev;
  struct js_event oldev; // FIXME should disappear when new converter arrives

  jfd = open ("/dev/input/js0", O_RDONLY);

  while (1) {
    if (read (jfd, &ev, sizeof (ev)) > 0) {
      if (ev.type == JS_EVENT_AXIS) {
        if (ev.number == POTDIS) {
          joydis = ev.value / FACDIS + PADDIS;  // [-32000:32000] -> [1:4]
          DEBUG ("joydis=%d\n", joydis);
         }
        else if (ev.number == POTMOD) {
          joymod = ev.value / FACMOD + PADMOD; // [-32000:32000] -> [0:1000]
          DEBUG ("joymod=%d\n", joymod);
         }
        else {
          DEBUG ("ev.number=%d ev.value=%d\n", ev.number, ev.value);
        }
      }
      else if ((ev.type == JS_EVENT_BUTTON) &&
               (ev.value == 1)) {
        if (ev.number == SW_FLG) {
          flange_flag ^= 1;
          flange_flag ? set_led (LED_DISK1, 255) : set_led (LED_DISK1, 0);
          DEBUG ("flange=%d\n", flange_flag);
        }
        else if (ev.number == SW_DLY) {
          delay_flag ^= 1;
          delay_flag ? set_led (LED_DISK2, 255) : set_led (LED_DISK2, 0);
          DEBUG ("delay=%d\n", delay_flag);
        }
        else if (ev.number == SW_DIS) {
          dist_flag ^= 1;
          dist_flag ? set_led (LED_READY, 255) : set_led (LED_READY, 0);
          DEBUG ("dist=%d\n", dist_flag);
        }
        else if (ev.number == SW_HLT) {
          set_led (LED_READY, 0);
          set_led (LED_DISK1, 0);
          set_led (LED_DISK2, 0);
          set_led (LED_STATUS,1);
          exit (0);
        }
        else {
          DEBUG ("ev.number=%d\n", ev.number);
        }
        if ((ev.value == 1) && (oldev.value == 1) && (((ev.number == SW_FLG) && (oldev.number == SW_DLY)) || ((ev.number == SW_DLY) && (oldev.number == SW_FLG)))) {
          set_led (LED_READY, 0);
          set_led (LED_DISK1, 0);
          set_led (LED_DISK2, 0);
          set_led (LED_STATUS,1);
          exit (0);                                    // FIXME kludge
        }
      }
    }
    oldev.value = ev.value; oldev.number = ev.number;  // FIXME kludge
  }
}

/**************************************************************************** 
 * push_pull()
 *
 * Push-pull simulator, generates 3rd harmonics
 * sample  Original sample
 * returns Sample processed through a non-linear function
 ****************************************************************************/

short push_pull (short sample)
{
  return (short)(2*(long)sample -
                 ((long)sample * (long)sinus [abs(sample)]) / 15000);
}


/**************************************************************************** 
 * write_to_file()
 *
 * Write a string to a file 
 * Fails silently (in case of insufficient rights)
 * *f  Filename
 * *s  String to write
 ****************************************************************************/

void write_to_file (const char *f, const char *s) {

  int fout;

  fout = open (f, O_WRONLY);
  write (fout, s, strlen (s));
  close (fout);
}

/****************************************************************************
 * set_led()
 *
 * Switch LEDs on/off 
 * Needs to be run as root to work, else nothing happens
 * *led  Dirname (/sys/classes/leds/foo/)
 * i     Value   (0||!0)
 ****************************************************************************/

void set_led (char *led, int i) {

  char led_bright [256];

  strncpy (led_bright, led, 255);
  strncat (led_bright, "/brightness", 255);
  write_to_file (led_bright, i ? "255" : "0");
}   

/****************************************************************************
 * debugsig()
 *
 * Signal handler
 * signum  Signal number
 ****************************************************************************/

void debugsig (int signum) {
/*
  int i;
  char *b;

  b=(char*)debugbuf1;
  for (i=0; i<bsize; i++) DEBUG ("%8d", debugbuf1[i]); DEBUG ("\n\n");
  for (i=0; i<bsize; i++) DEBUG ("%8d", debugbuf2[i]); DEBUG ("\n\n");
  for (i=0; i<bsize; i++) DEBUG ("%8d", debugbuf3[i]); DEBUG ("\n\n");
  DEBUG ("%d %d %d %d %d %d\n\n", b[0], b[1], b[2], b[3], b[4], b[5]);

  snd_pcm_drain (handle_rec);
  snd_pcm_close (handle_rec);
  snd_pcm_drain (handle_play);
  snd_pcm_close (handle_play);
*/
  free (recbuf);
  free (playbuf);

  exit(0);
}

