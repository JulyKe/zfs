/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Fletcher Checksums
 * ------------------
 *
 * ZFS's 2nd and 4th order Fletcher checksums are defined by the following
 * recurrence relations:
 *
 *	a  = a    + f
 *	 i    i-1    i-1
 *
 *	b  = b    + a
 *	 i    i-1    i
 *
 *	c  = c    + b		(fletcher-4 only)
 *	 i    i-1    i
 *
 *	d  = d    + c		(fletcher-4 only)
 *	 i    i-1    i
 *
 * Where
 *	a_0 = b_0 = c_0 = d_0 = 0
 * and
 *	f_0 .. f_(n-1) are the input data.
 *
 * Using standard techniques, these translate into the following series:
 *
 *	     __n_			     __n_
 *	     \   |			     \   |
 *	a  =  >     f			b  =  >     i * f
 *	 n   /___|   n - i		 n   /___|	 n - i
 *	     i = 1			     i = 1
 *
 *
 *	     __n_			     __n_
 *	     \   |  i*(i+1)		     \   |  i*(i+1)*(i+2)
 *	c  =  >     ------- f		d  =  >     ------------- f
 *	 n   /___|     2     n - i	 n   /___|	  6	   n - i
 *	     i = 1			     i = 1
 *
 * For fletcher-2, the f_is are 64-bit, and [ab]_i are 64-bit accumulators.
 * Since the additions are done mod (2^64), errors in the high bits may not
 * be noticed.  For this reason, fletcher-2 is deprecated.
 *
 * For fletcher-4, the f_is are 32-bit, and [abcd]_i are 64-bit accumulators.
 * A conservative estimate of how big the buffer can get before we overflow
 * can be estimated using f_i = 0xffffffff for all i:
 *
 * % bc
 *  f=2^32-1;d=0; for (i = 1; d<2^64; i++) { d += f*i*(i+1)*(i+2)/6 }; (i-1)*4
 * 2264
 *  quit
 * %
 *
 * So blocks of up to 2k will not overflow.  Our largest block size is
 * 128k, which has 32k 4-byte words, so we can compute the largest possible
 * accumulators, then divide by 2^64 to figure the max amount of overflow:
 *
 * % bc
 *  a=b=c=d=0; f=2^32-1; for (i=1; i<=32*1024; i++) { a+=f; b+=a; c+=b; d+=c }
 *  a/2^64;b/2^64;c/2^64;d/2^64
 * 0
 * 0
 * 1365
 * 11186858
 *  quit
 * %
 *
 * So a and b cannot overflow.  To make sure each bit of input has some
 * effect on the contents of c and d, we can look at what the factors of
 * the coefficients in the equations for c_n and d_n are.  The number of 2s
 * in the factors determines the lowest set bit in the multiplier.  Running
 * through the cases for n*(n+1)/2 reveals that the highest power of 2 is
 * 2^14, and for n*(n+1)*(n+2)/6 it is 2^15.  So while some data may overflow
 * the 64-bit accumulators, every bit of every f_i effects every accumulator,
 * even for 128k blocks.
 *
 * If we wanted to make a stronger version of fletcher4 (fletcher4c?),
 * we could do our calculations mod (2^32 - 1) by adding in the carries
 * periodically, and store the number of carries in the top 32-bits.
 *
 * --------------------
 * Checksum Performance
 * --------------------
 *
 * There are two interesting components to checksum performance: cached and
 * uncached performance.  With cached data, fletcher-2 is about four times
 * faster than fletcher-4.  With uncached data, the performance difference is
 * negligible, since the cost of a cache fill dominates the processing time.
 * Even though fletcher-4 is slower than fletcher-2, it is still a pretty
 * efficient pass over the data.
 *
 * In normal operation, the data which is being checksummed is in a buffer
 * which has been filled either by:
 *
 *	1. a compression step, which will be mostly cached, or
 *	2. a bcopy() or copyin(), which will be uncached (because the
 *	   copy is cache-bypassing).
 *
 * For both cached and uncached data, both fletcher checksums are much faster
 * than sha-256, and slower than 'off', which doesn't touch the data at all.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/spa.h>
#include <sys/zfs_context.h>
#include <zfs_fletcher.h>

static void fletcher_4_scalar_init(zio_cksum_t *zcp);
static void fletcher_4_scalar(const void *buf, uint64_t size,
    zio_cksum_t *zcp);
static void fletcher_4_scalar_byteswap(const void *buf, uint64_t size,
    zio_cksum_t *zcp);

static const fletcher_4_func_t fletcher_4_scalar_calls = {
	.init = fletcher_4_scalar_init,
	.compute = fletcher_4_scalar,
	.compute_byteswap = fletcher_4_scalar_byteswap,
	.name = "scalar"
};

static const fletcher_4_func_t *fletcher_4_algos[] = {
	&fletcher_4_scalar_calls,
#if defined(HAVE_AVX) && defined(HAVE_AVX2)
	&fletcher_4_avx2_calls,
#endif
};

static const fletcher_4_func_t *fletcher_4_chosen = NULL;
static const fletcher_4_func_t *fletcher_4_fastest = NULL;

static enum fletcher_selector {
	FLETCHER_FASTEST = 0,
	FLETCHER_SCALAR,
#if defined(HAVE_AVX) && defined(HAVE_AVX2)
	FLETCHER_AVX2,
#endif
	FLETCHER_CYCLE
} fletcher_4_impl_selector = FLETCHER_SCALAR;

static const char *selector_names[] = {
	"fastest",
	"scalar",
#if defined(HAVE_AVX) && defined(HAVE_AVX2)
	"avx2",
#endif
#if !defined(_KERNEL)
	"cycle",
#endif
};

static kmutex_t fletcher_4_impl_selector_lock;

static kstat_t *fletcher_4_kstat;

static kstat_named_t fletcher_4_kstat_data[ARRAY_SIZE(fletcher_4_algos)];

void
fletcher_2_native(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const uint64_t *ip = buf;
	const uint64_t *ipend = ip + (size / sizeof (uint64_t));
	uint64_t a0, b0, a1, b1;

	for (a0 = b0 = a1 = b1 = 0; ip < ipend; ip += 2) {
		a0 += ip[0];
		a1 += ip[1];
		b0 += a0;
		b1 += a1;
	}

	ZIO_SET_CHECKSUM(zcp, a0, a1, b0, b1);
}

void
fletcher_2_byteswap(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const uint64_t *ip = buf;
	const uint64_t *ipend = ip + (size / sizeof (uint64_t));
	uint64_t a0, b0, a1, b1;

	for (a0 = b0 = a1 = b1 = 0; ip < ipend; ip += 2) {
		a0 += BSWAP_64(ip[0]);
		a1 += BSWAP_64(ip[1]);
		b0 += a0;
		b1 += a1;
	}

	ZIO_SET_CHECKSUM(zcp, a0, a1, b0, b1);
}

static void fletcher_4_scalar_init(zio_cksum_t *zcp)
{
	ZIO_SET_CHECKSUM(zcp, 0, 0, 0, 0);
}

static void
fletcher_4_scalar(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const uint32_t *ip = buf;
	const uint32_t *ipend = ip + (size / sizeof (uint32_t));
	uint64_t a, b, c, d;

	a = zcp->zc_word[0];
	b = zcp->zc_word[1];
	c = zcp->zc_word[2];
	d = zcp->zc_word[3];

	for (; ip < ipend; ip++) {
		a += ip[0];
		b += a;
		c += b;
		d += c;
	}

	ZIO_SET_CHECKSUM(zcp, a, b, c, d);
}

static void
fletcher_4_scalar_byteswap(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const uint32_t *ip = buf;
	const uint32_t *ipend = ip + (size / sizeof (uint32_t));
	uint64_t a, b, c, d;

	a = zcp->zc_word[0];
	b = zcp->zc_word[1];
	c = zcp->zc_word[2];
	d = zcp->zc_word[3];

	for (; ip < ipend; ip++) {
		a += BSWAP_32(ip[0]);
		b += a;
		c += b;
		d += c;
	}

	ZIO_SET_CHECKSUM(zcp, a, b, c, d);
}

int
fletcher_4_impl_set(const char *selector)
{
	unsigned idx, i;

	for (i = 0; i < ARRAY_SIZE(selector_names); i++) {
		if (strncmp(selector, selector_names[i],
		    strlen(selector_names[i])) == 0) {
			idx = i;
			break;
		}
	}
	if (i >= ARRAY_SIZE(selector_names))
		return (-EINVAL);

	if (fletcher_4_impl_selector == idx)
		return (0);

	mutex_enter(&fletcher_4_impl_selector_lock);
	switch (idx) {
	case FLETCHER_FASTEST:
		fletcher_4_chosen = fletcher_4_fastest;
		break;
#if defined(HAVE_AVX) && defined(HAVE_AVX2)
	case FLETCHER_AVX2:
		fletcher_4_chosen = &fletcher_4_avx2_calls;
		break;
#endif
	case FLETCHER_SCALAR:
	case FLETCHER_CYCLE:
		fletcher_4_chosen = &fletcher_4_scalar_calls;
		break;
	}

	fletcher_4_impl_selector = idx;
	mutex_exit(&fletcher_4_impl_selector_lock);

	return (0);
}

static inline const fletcher_4_func_t *
fletcher_4_impl_get(void)
{
#if !defined(_KERNEL)
	if (fletcher_4_impl_selector == FLETCHER_CYCLE) {
		static volatile unsigned int cycle_count = 0;
		const fletcher_4_func_t *algo = NULL;
		unsigned int index;

		while (1) {
			index = atomic_inc_uint_nv(&cycle_count);
			algo = fletcher_4_algos[
			    index % ARRAY_SIZE(fletcher_4_algos)];
			if (algo->valid == NULL || algo->valid())
				break;
		}
		return (algo);
	}
#endif
	membar_producer();
	return (fletcher_4_chosen);
}

void
fletcher_4_native(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const fletcher_4_func_t *algo = fletcher_4_impl_get();

	algo->init(zcp);
	algo->compute(buf, size, zcp);
	if (algo->fini != NULL)
		algo->fini(zcp);
}

void
fletcher_4_byteswap(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const fletcher_4_func_t *algo = fletcher_4_impl_get();

	algo->init(zcp);
	algo->compute_byteswap(buf, size, zcp);
	if (algo->fini != NULL)
		algo->fini(zcp);
}

void
fletcher_4_incremental_native(const void *buf, uint64_t size,
    zio_cksum_t *zcp)
{
	fletcher_4_scalar(buf, size, zcp);
}

void
fletcher_4_incremental_byteswap(const void *buf, uint64_t size,
    zio_cksum_t *zcp)
{
	fletcher_4_scalar_byteswap(buf, size, zcp);
}

void
fletcher_4_init(void)
{
	const uint64_t const bench_ns = (50 * MICROSEC); /* 50ms */
	unsigned long best_run_count = 0;
	unsigned long best_run_index = 0;
	const unsigned data_size = 4096;
	char *databuf;
	int i;

	databuf = kmem_alloc(data_size, KM_SLEEP);
	for (i = 0; i < ARRAY_SIZE(fletcher_4_algos); i++) {
		const fletcher_4_func_t *algo = fletcher_4_algos[i];
		kstat_named_t *stat = &fletcher_4_kstat_data[i];
		unsigned long run_count = 0;
		hrtime_t start;
		zio_cksum_t zc;

		strncpy(stat->name, algo->name, sizeof (stat->name) - 1);
		stat->data_type = KSTAT_DATA_UINT64;
		stat->value.ui64 = 0;

		if (algo->valid != NULL && !algo->valid())
			continue;

		kpreempt_disable();
		start = gethrtime();
		algo->init(&zc);
		do {
			algo->compute(databuf, data_size, &zc);
			run_count++;
		} while (gethrtime() < start + bench_ns);
		if (algo->fini != NULL)
			algo->fini(&zc);
		kpreempt_enable();

		if (run_count > best_run_count) {
			best_run_count = run_count;
			best_run_index = i;
		}

		/*
		 * Due to high overhead of gethrtime(), the performance data
		 * here is inaccurate and much slower than it could be.
		 * It's fine for our use though because only relative speed
		 * is important.
		 */
		stat->value.ui64 = data_size * run_count *
		    (NANOSEC / bench_ns) >> 20; /* by MB/s */
	}
	kmem_free(databuf, data_size);

	fletcher_4_fastest = fletcher_4_algos[best_run_index];

	mutex_init(&fletcher_4_impl_selector_lock, NULL, MUTEX_DEFAULT, NULL);
	fletcher_4_impl_set("fastest");

	fletcher_4_kstat = kstat_create("zfs", 0, "fletcher_4_bench",
	    "misc", KSTAT_TYPE_NAMED, ARRAY_SIZE(fletcher_4_algos),
	    KSTAT_FLAG_VIRTUAL);
	if (fletcher_4_kstat != NULL) {
		fletcher_4_kstat->ks_data = fletcher_4_kstat_data;
		kstat_install(fletcher_4_kstat);
	}
}

void
fletcher_4_fini(void)
{
	mutex_destroy(&fletcher_4_impl_selector_lock);
	if (fletcher_4_kstat != NULL) {
		kstat_delete(fletcher_4_kstat);
		fletcher_4_kstat = NULL;
	}
}

#if defined(_KERNEL) && defined(HAVE_SPL)

static int
fletcher_4_param_get(char *buffer, struct kernel_param *unused)
{
	int i, cnt = 0;

	for (i = 0; i < ARRAY_SIZE(selector_names); i++) {
		cnt += sprintf(buffer + cnt,
		    fletcher_4_impl_selector == i ? "[%s] " : "%s ",
		    selector_names[i]);
	}

	return (cnt);
}

static int
fletcher_4_param_set(const char *val, struct kernel_param *unused)
{
	return (fletcher_4_impl_set(val));
}

/*
 * Choose a fletcher 4 implementation to use in ZFS.
 * Users can choose the "fastest" algorithm, or the "scalar" one that means
 * to compute fletcher 4 by CPU.
 * Users can also choose "cycle" to exercise all implementions, but this is
 * for testing purpose therefore it can only be set in user space.
 */
module_param_call(fletcher_4_impl_set,
    fletcher_4_param_set, fletcher_4_param_get, NULL, 0644);
MODULE_PARM_DESC(fletcher_4_impl_set, "Select fletcher 4 algorithm to use");

EXPORT_SYMBOL(fletcher_4_init);
EXPORT_SYMBOL(fletcher_4_fini);
EXPORT_SYMBOL(fletcher_2_native);
EXPORT_SYMBOL(fletcher_2_byteswap);
EXPORT_SYMBOL(fletcher_4_native);
EXPORT_SYMBOL(fletcher_4_byteswap);
EXPORT_SYMBOL(fletcher_4_incremental_native);
EXPORT_SYMBOL(fletcher_4_incremental_byteswap);
#endif
