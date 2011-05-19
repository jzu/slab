#ifdef SINSTATIC
#define LOG2_N_WAVE  10               /* log2 (N_WAVE) */
#define N_WAVE       1024             /* size of Sinewave [] */
#else
#define LOG2_N_WAVE  16
#define N_WAVE      (1<<LOG2_N_WAVE)
#endif
#define N_LOUD      100               /* size of Loudampl [] */
#ifndef fixed
#define fixed short
#endif

extern fixed Sinewave [N_WAVE]; 
extern fixed Loudampl [N_LOUD];


void initSinewave();

int  fix_fft (short fr [],
              short fi [],
              int m,
              int inverse);

