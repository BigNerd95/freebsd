#ifndef	__FFT_HISTOGRAM_H__
#define	__FFT_HISTOGRAM_H__

#define	FFT_HISTOGRAM_START_FREQ	2300
#define	FFT_HISTOGRAM_END_FREQ		6000

#define	FFT_HISTOGRAM_RESOLUTION	4	/* 250Khz increments */

#define	FFT_HISTOGRAM_SIZE	\
	    ((6000-2300)*FFT_HISTOGRAM_RESOLUTION)
#define	FFT_HISTOGRAM_HISTORY_DEPTH	10

struct fft_histogram_data {
	/* XXX should struct-ize these! */
	int16_t	avg_pts[FFT_HISTOGRAM_SIZE][FFT_HISTOGRAM_HISTORY_DEPTH];
	int16_t avg_pts_cur[FFT_HISTOGRAM_SIZE];

	int16_t	max_pts[FFT_HISTOGRAM_SIZE];
};

extern	void fft_histogram_init(void);
extern	void fft_add_sample(struct radar_entry *re,
	    struct radar_fft_entry *fe);
extern int16_t * fft_fetch_freq_avg(int freqKhz);
extern int16_t fft_fetch_freq_max(int freqKhz);

#endif
