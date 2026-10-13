/* opj_config_private.h for the SAMRH71 (ARM Cortex-M7, bare metal / RTOS) */

#define OPJ_PACKAGE_VERSION "2.5.3"

/* Not used by openjp2*/
/*#define HAVE_MEMORY_H 1*/
/*#define HAVE_STDLIB_H 1*/
/*#define HAVE_STRINGS_H 1*/
/*#define HAVE_STRING_H 1*/
/*#define HAVE_SYS_STAT_H 1*/
/*#define HAVE_SYS_TYPES_H 1 */
/*#define HAVE_UNISTD_H 1*/
/*#define HAVE_INTTYPES_H 1 */
/*#define HAVE_STDINT_H 1 */

/* No file system: fseeko()/ftello() are not used */
/* #undef _LARGEFILE_SOURCE */
/* #undef _LARGE_FILES */
/* #undef _FILE_OFFSET_BITS */
/* #undef OPJ_HAVE_FSEEKO */

/* No dynamic memory allocation: opj_malloc.c serves every allocation from
 * the static pool given to opj_mem_pool_init(), so none of the system
 * allocators below may be used. */
/* #undef OPJ_HAVE_MALLOC_H */
/* #undef OPJ_HAVE_ALIGNED_ALLOC */
/* #undef OPJ_HAVE__ALIGNED_MALLOC */
/* #undef OPJ_HAVE_MEMALIGN */
/* #undef OPJ_HAVE_POSIX_MEMALIGN */

/* No threads: do not define MUTEX_pthread / MUTEX_win32, so that thread.c
 * builds its single-threaded stub implementation. */

/* Byte order: the Cortex-M7 of the SAMRH71 is little endian */
/* #undef OPJ_BIG_ENDIAN */
