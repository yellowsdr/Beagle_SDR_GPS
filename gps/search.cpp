//////////////////////////////////////////////////////////////////////////
// Homemade GPS Receiver
// Copyright (C) 2013 Andrew Holme
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// http://www.holmea.demon.co.uk/GPS/Main.htm
//////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <memory.h>
#include <fftw3.h>
#include <math.h>

#include "types.h"
#include "kiwi.h"
#include "clk.h"
#include "cfg.h"
#include "misc.h"
#include "gps.h"
#include "spi.h"
#include "cacode.h"
#include "e1bcode.h"
#include "debug.h"
#include "simd.h"

///////////////////////////////////////////////////////////////////////////////////////////////

#define NTAPS	31

static fftwf_plan fwd_plan, rev_plan;

// code[sat][...] holds two copies of the FFT: modulo operation on the index is not needed
static fftwf_complex code[MAX_SATS][2*FFT_LEN] __attribute__ ((aligned (16)));

// fwd_buf is also used for decimating the data
static fftwf_complex fwd_buf[NSAMPLES + 2*NTAPS] __attribute__ ((aligned (16)));
static fftwf_complex rev_buf[FFT_LEN]  __attribute__ ((aligned (16)));

///////////////////////////////////////////////////////////////////////////////////////////////

static float inline Bipolar(int bit) {
	// this is a branchless version of
	//	return bit ? -1.0 : +1.0;
    return -1.0f*(bit!=0) + 1.0f*(bit==0);
}

#include <ctype.h>

static int min_sig=MIN_SIG, test_mode;
 
void SearchParams(int argc, char *argv[]) {
	int i;
	
	for (i=1; i<argc; ) {
		char *v = argv[i];
		if (strcmp(v, "?")==0 || strcmp(v, "-?")==0 || strcmp(v, "--?")==0 || strcmp(v, "-h")==0 ||
			strcmp(v, "h")==0 || strcmp(v, "-help")==0 || strcmp(v, "--h")==0 || strcmp(v, "--help")==0) {
			printf("GPS args:\n\t-gsig signal_threshold\n\t-gt test mode\n");
			xit(0);
		}
		if (strcmp(v, "-gsig")==0) {
			i++; min_sig = strtol(argv[i], 0, 0);
			printf("GPS min_sig=%d\n", min_sig);
		} else
		if (strcmp(v, "-gt")==0) {
			test_mode = 1;
			printf("GPS test_mode\n");
		}
		i++;
		while (i<argc && ((argv[i][0] != '+') && (argv[i][0] != '-'))) {
			i++;
		}
	}
}

static char bits[NSAMPLES][2]  __attribute__ ((aligned (16)));
 
// half-band filter
#define FT	0
static float COEF[NTAPS][2] __attribute__ ((aligned (16))) = {
	// remez	    firwin
	-0.010233,   -0.001888,
	 0.000000,    0.000000,
	 0.010668,    0.003862,
	 0.000000,    0.000000,
	-0.016324,   -0.008242,
	 0.000000,    0.000000,
	 0.024377,    0.015947,
	 0.000000,    0.000000,
	-0.036482,   -0.028677,
	 0.000000,    0.000000,
	 0.056990,    0.050719,
	 0.000000,    0.000000,
	-0.101993,   -0.098016,
	 0.000000,    0.000000,

	 0.316926,    0.315942,
	 0.500009,    0.500706,
	 0.316926,    0.315942,

	 0.000000,    0.000000,
	-0.101993,   -0.098016,
	 0.000000,    0.000000,
	 0.056990,    0.050719,
	 0.000000,    0.000000,
	-0.036482,   -0.028677,
	 0.000000,    0.000000,
	 0.024377,    0.015947,
	 0.000000,    0.000000,
	-0.016324,   -0.008242,
	 0.000000,    0.000000,
	 0.010668,    0.003862,
	 0.000000,    0.000000,
	-0.010233,   -0.001888,
 } ;

#define	DECIM_TSLICE	(128-1)

static int DecimateBy2float(int size, const fftwf_complex ibuf[], fftwf_complex obuf[], bool yield) {
	const float coef_0 = COEF[0][FT];
	const float coef_m = COEF[(NTAPS-1)/2][FT];
	
	// handle overlap
	memset((void *) ibuf[size], 0, NTAPS * sizeof(fftwf_complex));
	
	for (int i=0, o=0; i<size; i+=2, ++o) {
		float accI = ibuf[i][0]*coef_0;
		float accQ = ibuf[i][1]*coef_0;

		for (int j=2; j<NTAPS; j+=2) {
			const float coef = COEF[j][FT];
			accI += ibuf[i+j][0]*coef;
			accQ += ibuf[i+j][1]*coef;
		}

		accI += ibuf[i+(NTAPS-1)/2][0]*coef_m;
		accQ += ibuf[i+(NTAPS-1)/2][1]*coef_m;

		obuf[o][0] = accI;
		obuf[o][1] = accQ;

		if (yield && ((i>>1)&DECIM_TSLICE) == DECIM_TSLICE) NextTask("DecimateBy2float");
	}
	return size/2;
}

static int DecimateBy2binary(int size, const char ibuf[][2], fftwf_complex obuf[], bool yield) {
	// (1) convert the input to a float array
	simd_bit2float(2*size, (int8_t*)(ibuf), (float*)(obuf));
	// (2) set output to +-1
	for (int i=0; i<size; ++i) {
	 	obuf[i][0] = Bipolar(obuf[i][0] >= 0);
	  	obuf[i][1] = Bipolar(obuf[i][1] >= 0);
	}
	// (3) then use the float version
	size = DecimateBy2float(size, obuf, obuf, yield);
	return size;
}

///////////////////////////////////////////////////////////////////////////////////////////////

void SearchInit() {
    int sat;
    SATELLITE *sp;
    for (sat = 0, sp = Sats; sp->prn != -1; sat++, sp++) {
        sp->sat = sat;
        switch (sp->type) {
            case Navstar: default: asprintf(&sp->prn_s, "N%02d", sp->prn); break;
            case QZSS: asprintf(&sp->prn_s, "Q%d", sp->prn); break;
            case E1B: asprintf(&sp->prn_s, "E%02d", sp->prn); break;
        }
        //printf("sat %d PRN %d %s\n", sat, sp->prn, sp->prn_s);
    }
    if (sat >= MAX_SATS) {
        printf("MAX_SATS=%d not big enough, ARRAY_LEN(Sats)=%d\n", MAX_SATS, sat);
        assert(sat < MAX_SATS);
    }
    
    GPSstat_init();
    
    //#define TEST_MULT18
    #ifdef TEST_MULT18
        static SPI_MISO sbuf;
        spi_get_noduplex(CmdTestMult18, &sbuf, 8, 1, 1);
        u1_t *b = (u1_t *) sbuf.byte;
        printf("Mult18x18->36->64 %02x%02x %02x%02x %02x%02x %02x%02x\n",
            b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
        xit(0);
    #endif

    #if 0
        int prn = 1;
        E1BCODE e1bt1(prn);
        //spi_set_noduplex(CmdETrst);
        #if 1
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x1248);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x1000);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0100);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0010);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0001);
            
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0000);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0000);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0000);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x0000);
        #else
        for (int i=0; i < I_DIV_CEIL(E1B_CODELEN, 16); i++) {
            //spi_set_noduplex(CmdSetE1Bcode, 0, E1B_code16[prn-1][i]);
            //spi_set_noduplex(CmdSetE1Bcode, 0, 0x1111*i);
        }
        #endif

        #if 1
        spi_set_noduplex(CmdETrst);
        spi_set_noduplex(CmdETw, 0x137f);
        spi_set_noduplex(CmdETw, 0xbabe);
        spi_set_noduplex(CmdETw, 0xfeed);
        #else
        spi_set_noduplex(CmdETwr, 1);
        spi_set_noduplex(CmdETw, 0x137f);
        spi_set_noduplex(CmdETwr, 0);
        spi_set_noduplex(CmdETw, 0xbabe);
        spi_set_noduplex(CmdETwr, 2);
        spi_set_noduplex(CmdETw, 0xfeed);
        #endif

        static SPI_MISO status;
        union {
            u2_t word;
            struct {
                u2_t fpga_id:4, stat_user:4, fpga_ver:4, fw_id:3, ovfl:1;
            };
        } stat;
        for (int i=0; i < E1B_CODELEN+16; i++) {
            //spi_set_noduplex(CmdETrd, i);
            spi_get_noduplex(CmdGetStatus, &status, 2);
            stat.word = status.word[0];
            assert(stat.fpga_id == FPGA_ID);
            int chip = stat.stat_user & 1;
            // typ stat.word = 0x51s3
            printf("E%02d 0x%04x %4d chip %d\n", prn, stat.word, i, chip);
            spi_set_noduplex(CmdETrd);
        }
        xit(0);
    #endif

    const float ca_rate = CPS/FS;
	float ca_phase=0;

    //#define QZSS_PRN_TEST
    #ifdef QZSS_PRN_TEST
        printf("QZSS PRN test:\n");
        for (sp = Sats; sp->prn != -1; sp++) {
            if (sp->type != QZSS) continue;
            CACODE ca(sp->T1, sp->T2);
            int chips = 0;
            for (int i=1; i<=10; i++) {
                chips <<= 1; chips |= ca.Chip(); ca.Clock();
            }
            printf("\t%s first 10 chips: 0%04o\n", PRN(sp->sat), chips);
        }
	#endif

	printf("DECIM %d FFT %d planning..\n", DECIM, FFT_LEN);
    fwd_plan = fftwf_plan_dft_1d(FFT_LEN, fwd_buf, fwd_buf, FFTW_FORWARD,  FFTW_ESTIMATE);
    rev_plan = fftwf_plan_dft_1d(FFT_LEN, rev_buf, rev_buf, FFTW_BACKWARD, FFTW_ESTIMATE);

    for (sp = Sats; sp->prn != -1; sp++) {
        if (sp->type != Navstar && sp->type != QZSS) continue;
        int T1 = sp->T1, T2 = sp->T2;

		//printf("computing CODE FFT for %s T1 %d T2 %d\n", sp->prn_s, T1, T2);
        CACODE ca(T1, T2);

        for (int i=0; i<NSAMPLES; i++) {

            float chip = Bipolar(ca.Chip()); // chip at start of sample period

            ca_phase += ca_rate; // NCO phase at end of period

            if (ca_phase >= 1.0) { // reached or crossed chip boundary?
                ca_phase -= 1.0;
                ca.Clock();

                // These two lines do not make much difference
                chip *= 1.0 - ca_phase;                 // prev chip
                chip += ca_phase * Bipolar(ca.Chip());  // next chip
            }

			fwd_buf[i][0] = chip;
			fwd_buf[i][1] = 0;
		}

        int nsamples = NSAMPLES;

        #if DECIM != 1
            assert(DECIM > 2);
            for (int i=DECIM; i>1; i>>=1) {
                nsamples = DecimateBy2float(nsamples, fwd_buf, fwd_buf);
            }
        #endif

		assert(nsamples == NSAMPLES/DECIM && nsamples == FFT_LEN);

		fftwf_execute(fwd_plan);

		// make two copies of the FFT results in order to avoid modulo operation on the index in Correlate(..)
		memcpy(code[sp->sat],          fwd_buf, nsamples*sizeof(fftwf_complex));
		memcpy(code[sp->sat]+nsamples, fwd_buf, nsamples*sizeof(fftwf_complex));
    }
    
    //#define E1BCODE_TEST
    #ifdef E1BCODE_TEST
        E1BCODE e1bt1(1);
        for (int i=0; i < 20; i++) {
            if ((i&3) == 0) printf(" ");
            printf ("%d", e1bt1.Chip());
            e1bt1.Clock();
        }
        printf(" PRN E1 0xf5d71\n");
        E1BCODE e1bt2(2);
        for (int i=0; i < 20; i++) {
            if ((i&3) == 0) printf(" ");
            printf ("%d", e1bt2.Chip());
            e1bt2.Clock();
        }
        printf(" PRN E2 0x96b85\n");
        xit(0);
    #endif

    float e1b_rate = CPS/FS;
	float e1b_phase=0;

    for (sp = Sats; sp->prn != -1; sp++) {
        if (sp->type != E1B) continue;

		//printf("computing CODE FFT for %s\n", sp->prn_s);
        E1BCODE e1b(sp->prn);

        for (int i=0; i<NSAMPLES; i++) {

            float chip = Bipolar(e1b.Chip()); // chip at start of sample period

            e1b_phase += e1b_rate; // NCO phase at end of period

            if (e1b_phase >= 1.0) { // reached or crossed chip boundary?
                e1b_phase -= 1.0;
                e1b.Clock();

                // These two lines do not make much difference
                chip *= 1.0 - e1b_phase;                 // prev chip
                chip += e1b_phase * Bipolar(e1b.Chip());  // next chip
            }

            fwd_buf[i][0] = chip;
            fwd_buf[i][1] = 0;
		}

        int nsamples = NSAMPLES;

        #if DECIM != 1
            assert(DECIM > 2);
            for (int i=DECIM; i>1; i>>=1) {
                nsamples = DecimateBy2float(nsamples, fwd_buf, fwd_buf);
            }
        #endif

		assert(nsamples == NSAMPLES/DECIM && nsamples == FFT_LEN);

		fftwf_execute(fwd_plan);

		memcpy(code[sp->sat],          fwd_buf, nsamples*sizeof(fftwf_complex));
		memcpy(code[sp->sat]+nsamples, fwd_buf, nsamples*sizeof(fftwf_complex));
    }

    //printf("computing CODE FFTs DONE\n");
    CreateTask(SearchTask, 0, GPS_ACQ_PRIORITY);
}

///////////////////////////////////////////////////////////////////////////////////////////////

void SearchFree() {
    fftwf_destroy_plan(fwd_plan);
    fftwf_destroy_plan(rev_plan);
}

///////////////////////////////////////////////////////////////////////////////////////////////

void GenSamples(char *rbuf, int bytes) {
	int i, j;
	static int rfd;

	if (!rfd) {
		rfd = open("./SiGe_Bands-L1.fs.16368.if.4092.rs81p.dat", O_RDONLY);
		assert(rfd > 0);
	}

    i = read(rfd, rbuf, bytes);
//printf("GenSamples bytes=%d/%d 0x%02x 0x%02x 0x%02x\n", i, bytes, rbuf[0], rbuf[1], rbuf[2]);
    if (i != bytes) {
        printf("end of GPS samples data file\n");
        xit(0);
    }
}

static void Sample() {
    const int lo_sin[] = {1,1,0,0}; // Quadrature local oscillators
    const int lo_cos[] = {1,0,0,1};
	
    const float lo_rate = 4*FC/FS; // NCO rate

    const int US = int(0.5+1000000/BIN_SIZE); // Sample length
    const int PACKET = GPS_SAMPS * 2;

    float lo_phase=0; // NCO phase accumulator
    int i=0;
	
	spi_set(CmdSample); // Trigger sampler and reset code generator in FPGA
	TaskSleepUsec(US);

	while (i < NSAMPLES) {
        static SPI_MISO rx;
		spi_get(CmdGetGPSSamples, &rx, PACKET);
		//GenSamples(rx.byte, PACKET);   // jks

        for (int j=0; j<PACKET; ++j) {
			u1_t byte = rx.byte[j];

            for (int b=0; b<8; ++b, ++i, byte>>=1) {
            	const int bit = (byte&1);
//printf("j%03d byte 0x%02x bit#%d=%d\n", j, byte, b, bit);

                // Down convert to complex (IQ) baseband by mixing (XORing)
                // samples with quadrature local oscillators (mix down by FC)
                if (i >= NSAMPLES)
                	break;

				bits[i][0] = bit ^ lo_sin[int(lo_phase)];
				bits[i][1] = bit ^ lo_cos[int(lo_phase)];

                lo_phase += lo_rate;
				lo_phase -= 4*(lo_phase >= 4);
            }
        }
    }

    NextTask("samp0");

    #if DECIM == 1
        int nsamples = NSAMPLES;
        for (i=0; i < nsamples; i++) {
            fwd_buf[i][0] = Bipolar(bits[i][0]);
            fwd_buf[i][1] = Bipolar(bits[i][1]);
        }
    #else
	    assert(DECIM > 2);
        int nsamples = DecimateBy2binary(NSAMPLES, bits, fwd_buf, true);
        NextTask("samp2");
        for (i=DECIM>>1; i>1; i>>=1) {
            nsamples = DecimateBy2float(nsamples, fwd_buf, fwd_buf, true);
            NextTask("samp3");
        }
    #endif
    
    assert(nsamples == NSAMPLES/DECIM && nsamples == FFT_LEN);
	NextTask("samp4");
	fftwf_execute(fwd_plan); // Transform to frequency domain
    NextTask("samp5");
}

///////////////////////////////////////////////////////////////////////////////////////////////

static float Correlate(int sat, const fftwf_complex *data, int *max_snr_dop, int *max_snr_i) {
    fftwf_complex *prod = rev_buf;
    float max_snr=0;
    int code_period_ms = is_E1B(sat)? E1B_CODE_PERIOD : L1_CODE_PERIOD;
    int i;
    int lcl_max_snr_dop, lcl_max_snr_i;
    float lcl_max_pwr, lcl_ave_pwr;
    
    // see paper about baseband FFT symmetry (since input from GPS FE is a real signal)
    // this simulates throwing away the upper 1/2 of the FFT so subsequent FFT
    // output processing can be 1/2 the size (the FFT itself has to be the same size).
    //if (test_mode) for (i=fft_len/2; i<fft_len; i++) data[i][0] = data[i][1] = 0;

    lcl_max_snr_i = 9999;
	// +/- 5 kHz doppler search
    for (int dop = -5000/BIN_SIZE; dop <= 5000/BIN_SIZE; dop++) {
        float max_pwr=0, tot_pwr=0;
        int max_pwr_i=0;

		// prod = conj(data)*code, with doppler shifting applied to C/A or E1B code FFT
		#if 1
		simd_multiply_conjugate(FFT_LEN, data, code[sat]+FFT_LEN-dop, prod);
		#else
        for (i=0; i<FFT_LEN; i++) {
            int j=(i-dop+FFT_LEN)%FFT_LEN;	// doppler shifting applied to C/A or E1B code FFT
            prod[i][0] = data[i][0]*code[sat][j][0] + data[i][1]*code[sat][j][1];
            prod[i][1] = data[i][0]*code[sat][j][1] - data[i][1]*code[sat][j][0];
        }
		#endif
        NextTaskP("corr FFT LONG RUN", NT_LONG_RUN);
		fftwf_execute(rev_plan);
        NextTask("corr FFT end");

        for (i=0; i < SAMPLE_RATE/1000*code_period_ms; i++) {		// 1 msec of samples
            const float pwr = prod[i][0]*prod[i][0] + prod[i][1]*prod[i][1];
            if (pwr>max_pwr) max_pwr=pwr, max_pwr_i=i;
            tot_pwr += pwr;
        }
        NextTask("corr pwr");

        const float ave_pwr = tot_pwr/i;
        const float snr = max_pwr/ave_pwr;
        if (snr > max_snr) lcl_max_snr_dop=dop, lcl_max_snr_i=max_pwr_i;
        if (snr > max_snr) lcl_max_pwr=max_pwr, lcl_ave_pwr=ave_pwr;
        if (snr > max_snr) max_snr=snr, *max_snr_dop=dop, *max_snr_i=max_pwr_i;
    }
    
    //printf("Correlate NORM %s sat%d code_period_ms %d SNR %.0f max %.3g ave %.3g dop %d ca_shift %d\n",
    //    PRN(sat), sat, code_period_ms, max_snr, lcl_max_pwr, lcl_ave_pwr,
    //    (int) (lcl_max_snr_dop*BIN_SIZE), lcl_max_snr_i);

#if 0
    max_snr=0;
    lcl_max_snr_i = 9999;

    for (int dop = -5000/BIN_SIZE; dop <= 5000/BIN_SIZE; dop++) {
        float max_pwr=0, tot_pwr=0;
        int max_pwr_i=0;

		// prod = conj(data)*code, with doppler shifting applied to C/A or E1B code FFT
		#if 0
		simd_multiply_conjugate(FFT_LEN, data, code[sat]+FFT_LEN-dop, prod);
		#else
        for (i=0; i<FFT_LEN; i++) {
            int j=(i-dop+FFT_LEN)%FFT_LEN;	// doppler shifting applied to C/A or E1B code FFT
            prod[i][0] = data[i][0]*code[sat][j][0] + data[i][1]*code[sat][j][1];
            prod[i][1] = data[i][0]*code[sat][j][1] - data[i][1]*code[sat][j][0];
        }
		#endif
        NextTaskP("corr FFT LONG RUN", NT_LONG_RUN);
		fftwf_execute(rev_plan);
        NextTask("corr FFT end");

        for (i=0; i < FFT_LEN; i++) {		// all samples
            const float pwr = prod[i][0]*prod[i][0] + prod[i][1]*prod[i][1];
            if (pwr>max_pwr) max_pwr=pwr, max_pwr_i=i;
            tot_pwr += pwr;
        }
        NextTask("corr pwr");

        const float ave_pwr = tot_pwr/i;
        const float snr = max_pwr/ave_pwr;
        if (snr > max_snr) lcl_max_snr_dop=dop, lcl_max_snr_i=max_pwr_i;
        if (snr > max_snr) max_snr=snr, *max_snr_dop=dop, *max_snr_i=max_pwr_i;
    }

    printf("Correlate FULL %s sat%d code_period_ms %d SNR %.0f dop %d ca_shift %d\n",
        PRN(sat), sat, code_period_ms, max_snr, (int) (lcl_max_snr_dop*BIN_SIZE), lcl_max_snr_i);
#endif

    return max_snr;
}

///////////////////////////////////////////////////////////////////////////////////////////////

static int searchRestart, searchResume;

void SearchEnable(int ch, int sat, bool restart) {
    Sats[sat].busy = false;
    if (restart) searchRestart = sat+1;
    //printf("==== %s ch%02d %s\n", restart? "RESTART" : "SIGNAL LOST", ch+1, PRN(sat));
}

///////////////////////////////////////////////////////////////////////////////////////////////

static int searchTaskID = -1;

void SearchTask(void *param) {
    int i, us, ret, ch, last_ch=-1, sat, t_sample, lo_shift=0, ca_shift=0;
    SATELLITE *sp;
    float snr=0;
    
    TaskSleepSec(20);   // jks TEMP due to printf/log shared memory malloc/free crash problem

	searchTaskID = TaskID();

    GPSstat(STAT_PARAMS, 0, DECIM, min_sig);
	GPSstat(STAT_ACQUIRE, 0, 1);

    for(;;) {
        for (sp = Sats; sp->prn != -1; sp++) {
            sat = sp->sat;
            //jks
            if (sp->type != E1B) continue;
            //if (sp->prn != 11 && sp->prn != 12) continue;
            //if (sp->prn != 11) continue;
            //if (sp->prn != 8) continue;
            
            //printf("REMINDER: G2_INIT workaround is DISABLED (2)\n");
            if (searchRestart) {
                //printf("FIXME: this doesn't work for restarting E1Bs!\n");
                searchResume = sat+1;
                sat = searchRestart-1;
                sp = &Sats[sat];
                //printf("==== SEARCH RESTART %s\n", PRN(sat));
                searchRestart = 0;
            } else
            if (searchResume) {
                sat = searchResume-1;
                sp = &Sats[sat];
                //printf("==== SEARCH RESUME %s\n", PRN(sat));
                searchResume = 0;
            }

//jks
TaskSleepMsec(1000);

            if (sp->busy) {     // sat already acquired?
//printf("consider %s: BUSY\n", PRN(sat));
                gps.include_alert_gps = admcfg_bool("include_alert_gps", NULL, CFG_REQUIRED);
            	NextTask("busy1");		// let cpu run
                continue;
            }

            #if GALILEO_CHANS == 0
                while ((ch = ChanReset(sat)) < 0) {		// all channels busy?
//printf("consider %s: ALL CHAN BUSY\n", PRN(sat));
                    TaskSleepMsec(1000);
                    //NextTask("all chans busy");
                }
            #else
                if ((ch = ChanReset(sat)) < 0) {		// all channels busy?
//printf("consider %s: ALL CHAN BUSY\n", PRN(sat));
                    continue;
                }
            #endif
			
			if ((last_ch != ch) && (snr < min_sig)) GPSstat(STAT_SAT, 0, last_ch, -1, 0, 0);
#ifndef	QUIET
			printf("FFT-%s\n", PRN(sat)); fflush(stdout);
#endif
            us = t_sample = timer_us(); // sample time
            Sample();

			snr = Correlate(sat, fwd_buf, &lo_shift, &ca_shift);
			ca_shift *= DECIM;
            
            us = timer_us()-us;
#ifndef	QUIET
//#if 1
            if (sp->type == E1B && snr >= min_sig)
			printf("FFT-%s %.3f secs SNR=%.1f\n", PRN(sat), (float)us/1000000.0, snr);
			fflush(stdout);
#endif

            GPSstat(STAT_SAT, snr, ch, sat, snr < min_sig, us);
            last_ch = ch;

            if (snr < min_sig) {
//printf("consider %s: LOW SNR\n", PRN(sat));
                continue;
            }
            
            //jks
            //continue;

            GPSstat(STAT_DOP, 0, ch, lo_shift*BIN_SIZE, ca_shift);

            sp->busy = true;

            int T1 = sp->T1, T2 = sp->T2;
            int init;
            
            switch (sp->type) {
                case Navstar: default: init = (T1<<4) + T2; break;
                case QZSS: init = G2_INIT | T2; break;
                case E1B: init = E1B_MODE | (sp->prn-1); break;
            }

			//printf("ch%02d %s snr=%.0f init=0x%x lo_shift=%d ca_shift=%d\n",
			//    ch+1, PRN(sat), snr, init, (int) (lo_shift*BIN_SIZE), ca_shift);
            ChanStart(ch, sat, t_sample, init, lo_shift, ca_shift, (int) snr);
    	}
	}
}

static int gps_acquire = 1;

// Decide if the search task should run.
// Conditional because of the large load the acquisition FFT places on the Beagle.
bool SearchTaskRun()
{
	if (searchTaskID == -1) return false;
	
	bool start = false;
	int users = rx_server_users();
	
	// startup: no clock corrections done yet
	if (clk.adc_gps_clk_corrections == 0) start = true;
	
	// no connections: might as well search
	if (users == 0) start = true;
	
	// not too busy (only one user): search if not enough sats to generate new fixes
	//if (users <= 1 && gps.good < 4) start = true;
	
	// search if not enough sats to generate new fixes
	if (gps.good < 5) start = true;
	
	if (admcfg_bool("always_acq_gps", NULL, CFG_REQUIRED) == true) start = true;
	
	if (update_in_progress || sd_copy_in_progress || backup_in_progress) start = false;
	
	bool enable = (admcfg_bool("enable_gps", NULL, CFG_REQUIRED) == true);
	if (!enable) start = false;
	
	//printf("SearchTaskRun: acq %d start %d good %d users %d fixes %d gps_corr %d\n",
	//	gps_acquire, start, gps.good, users, gps.fixes, clk.adc_gps_clk_corrections);
	
	if (gps_acquire && !start) {
		//printf("SearchTaskRun: $sleep\n");
		gps_acquire = 0;
		GPSstat(STAT_ACQUIRE, 0, gps_acquire);
		TaskSleepID(searchTaskID, 0);
	} else
	if (!gps_acquire && start) {
		//printf("SearchTaskRun: $wakeup\n");
		gps_acquire = 1;
		GPSstat(STAT_ACQUIRE, 0, gps_acquire);
		TaskWakeup(searchTaskID, FALSE, 0);
	}
	
	return enable;
}
