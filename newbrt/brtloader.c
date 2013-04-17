/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
#ident "Copyright (c) 2007-2010 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include <toku_portability.h>

#if !TOKU_WINDOWS
#include <arpa/inet.h>
#endif

#include <stdio.h>
#include <memory.h>
#include <errno.h>
#include <toku_assert.h>
#include <string.h>
#include "zlib.h"
#include <fcntl.h>
#include "x1764.h"
#include "brtloader-internal.h"
#include "brt-internal.h"
#include "sub_block.h"
#include "sub_block_map.h"
#include "pqueue.h"
#include "trace_mem.h"
// to turn on tracing, 
//   cd .../newbrt
//   edit trace_mem.h, set #define BL_DO_TRACE 1
//   make local
//   cd ../src;make local

#if defined(__cilkplusplus)
#include <cilk.h>
#include <cilk_mutex.h>
#include <fake_mutex.h>
#define CILK_BEGIN extern "Cilk++" {
#define CILK_END };
#else
// maybe #include <cilk_stub.h>
#if !defined(CILK_STUB)
#define CILK_STUB
#define cilk_spawn
#define cilk_sync
#define cilk_for for
#endif
#define CILK_BEGIN
#define CILK_END
#endif

// mark everything as C and selectively mark cilk functions
#if defined(__cplusplus) || defined(__cilkplusplus)
extern "C" {
#if 0
} // make emacs happy about matching parens....
#endif
#endif

static size_t (*os_fwrite_fun)(const void *,size_t,size_t,FILE*)=NULL;
void brtloader_set_os_fwrite (size_t (*fwrite_fun)(const void*,size_t,size_t,FILE*)) {
    os_fwrite_fun=fwrite_fun;
}

static size_t do_fwrite (const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (os_fwrite_fun) {
	return os_fwrite_fun(ptr, size, nmemb, stream);
    } else {
	return fwrite(ptr, size, nmemb, stream);
    }
}

static void add_big_buffer(struct file_info *file) {
    if (file->buffer == NULL)
        file->buffer = toku_malloc(file->buffer_size);
    if (file->buffer) {
        int r = setvbuf(file->file, (char *) file->buffer, _IOFBF, file->buffer_size); assert(r == 0);
    }
}

static void cleanup_big_buffer(struct file_info *file) {
    if (file->buffer) {
        toku_free(file->buffer);
        file->buffer = NULL;
    }
}

int brtloader_init_file_infos (struct file_infos *fi) {
    int r = toku_pthread_mutex_init(&fi->lock, NULL);
    assert(r==0);
    fi->n_files = 0;
    fi->n_files_limit = 1;
    fi->n_files_open = 0;
    fi->n_files_extant = 0;
    MALLOC_N(fi->n_files_limit, fi->file_infos);
    if (fi->n_files_limit) return 0;
    else return errno;
}

void brtloader_fi_destroy (struct file_infos *fi, BOOL is_error)
// Effect: Free the resources in the fi.
// If is_error then we close and unlink all the temp files.
// If !is_error then requires that all the temp files have been closed and destroyed
// No error codes are returned.  If anything goes wrong with closing and unlinking then it's only in an is_error case, so we don't care.
{
    toku_pthread_mutex_destroy(&fi->lock);
    if (!is_error) {
	assert(fi->n_files_open==0);
	assert(fi->n_files_extant==0);
    }
    for (int i=0; i<fi->n_files; i++) {
	if (fi->file_infos[i].is_open) {
	    assert(is_error);
	    fclose(fi->file_infos[i].file); // don't check for errors, since we are in an error case.
	}
	if (fi->file_infos[i].is_extant) {
	    assert(is_error);
	    unlink(fi->file_infos[i].fname);
	    toku_free(fi->file_infos[i].fname);
	}
        cleanup_big_buffer(&fi->file_infos[i]);
    }
    toku_free(fi->file_infos);
    fi->n_files=0;
    fi->n_files_limit=0;
    fi->file_infos = NULL;
}

static void open_file_add (struct file_infos *fi,
			   FILE *file,
			   char *fname,
			   /* out */ FIDX *idx)
{
    int r = toku_pthread_mutex_lock(&fi->lock); assert(r==0);
    if (fi->n_files >= fi->n_files_limit) {
	fi->n_files_limit *=2;
	XREALLOC_N(fi->n_files_limit, fi->file_infos);
    }
    assert(fi->n_files < fi->n_files_limit);
    fi->file_infos[fi->n_files].is_open   = TRUE;
    fi->file_infos[fi->n_files].is_extant = TRUE;
    fi->file_infos[fi->n_files].fname     = fname;
    fi->file_infos[fi->n_files].file      = file;
    fi->file_infos[fi->n_files].n_rows    = 0;
    fi->file_infos[fi->n_files].buffer_size = 1<<20;
    fi->file_infos[fi->n_files].buffer    = NULL;
    add_big_buffer(&fi->file_infos[fi->n_files]);
    idx->idx = fi->n_files;
    fi->n_files++;
    fi->n_files_extant++;
    fi->n_files_open++;
    r = toku_pthread_mutex_unlock(&fi->lock); assert(r==0);
}

int brtloader_fi_reopen (struct file_infos *fi, FIDX idx, const char *mode) {
    int result = 0;
    int r = toku_pthread_mutex_lock(&fi->lock); assert(r==0);
    int i = idx.idx;
    assert(i>=0 && i<fi->n_files);
    assert(!fi->file_infos[i].is_open);
    assert(fi->file_infos[i].is_extant);
    fi->file_infos[i].file = fopen(fi->file_infos[i].fname, mode);
    if (fi->file_infos[i].file==NULL) { result = errno; goto error; }
    fi->file_infos[i].is_open = TRUE;
    add_big_buffer(&fi->file_infos[i]);
    fi->n_files_open++;
    r = toku_pthread_mutex_unlock(&fi->lock); assert(r==0);
 error:
    return result;
}

int brtloader_fi_close (struct file_infos *fi, FIDX idx)
{
    { int r2 = toku_pthread_mutex_lock(&fi->lock); assert(r2==0); }
    assert(fi->n_files_open>0);
    fi->n_files_open--;
    assert(idx.idx >=0 && idx.idx < fi->n_files);
    assert(fi->file_infos[idx.idx].is_open);
    fi->file_infos[idx.idx].is_open = FALSE;
    int r = fclose(fi->file_infos[idx.idx].file);
    { int r2 = toku_pthread_mutex_unlock(&fi->lock); assert(r2==0); }
    if (r!=0) return errno;
    else return 0;
}

int brtloader_fi_unlink (struct file_infos *fi, FIDX idx) {
    { int r2 = toku_pthread_mutex_lock(&fi->lock); assert(r2==0); }
    assert(fi->n_files_extant>0);
    fi->n_files_extant--;
    int id = idx.idx;
    assert(id >=0 && id < fi->n_files);
    assert(!fi->file_infos[id].is_open); // must be closed before we unlink
    assert(fi->file_infos[id].is_extant); // must still exist
    fi->file_infos[id].is_extant = FALSE;
    int r = unlink(fi->file_infos[id].fname);  if (r!=0) r=errno;
    toku_free(fi->file_infos[id].fname);
    fi->file_infos[id].fname = NULL;
    { int r2 = toku_pthread_mutex_unlock(&fi->lock); assert(r2==0); }
    return r;
}

int brtloader_open_temp_file (BRTLOADER bl, FIDX *file_idx)
/* Effect: Open a temporary file in read-write mode.  Save enough information to close and delete the file later.
 * Return value: 0 on success, an error number otherwise.
 *  On error, *file_idx and *fnamep will be unmodified.
 *  The open file will be saved in bl->file_infos so that even if errors happen we can free them all.
 */
{
    char *fname = toku_strdup(bl->temp_file_template);
    int fd = mkstemp(fname);
    if (fd<0) { int r = errno; toku_free(fname); return r; }
    FILE *f = fdopen(fd, "r+");
    if (f==NULL) { int r = errno; toku_free(fname); close(fd); return r; }
    open_file_add(&bl->file_infos, f, fname, file_idx);

    static int counter=0;
    //fprintf(stderr, "%s:%d %d: %s\n", __FILE__, __LINE__, counter, fname);
    counter++;
    return 0;
}

static void brtloader_destroy (BRTLOADER bl, BOOL is_error) {
    // These frees rely on the fact that if you free a NULL pointer then nothing bad happens.
    toku_free(bl->dbs);
    toku_free(bl->descriptors);
    for (int i = 0; i < bl->N; i++) {
	if (bl->new_fnames_in_env) 
            toku_free((char*)bl->new_fnames_in_env[i]);
    }
    toku_free(bl->new_fnames_in_env);
    toku_free(bl->bt_compare_funs);
    toku_free((char*)bl->temp_file_template);
    brtloader_fi_destroy(&bl->file_infos, is_error);

    for (int i = 0; i < bl->N; i++) 
        destroy_rowset(&bl->rows[i]);
    toku_free(bl->rows);

    for (int i = 0; i < bl->N; i++)
        destroy_merge_fileset(&bl->fs[i]);
    toku_free(bl->fs);

    // 
    for (int i=0; i<bl->N; i++) {
	assert(bl->fractal_queues[i]==NULL); // !!! If this isn't true, we may have to kill the pthreads and destroy the fractal trees.  For now just barf.
    }
    toku_free(bl->fractal_threads);
    toku_free(bl->fractal_queues);
    toku_free(bl->fractal_threads_live);

    brt_loader_destroy_error_callback(&bl->error_callback);
    brt_loader_destroy_poll_callback(&bl->poll_callback);
}

static void *extractor_thread (void*);

// RFP 2535 error recovery in the loader open needs to be replaced
int toku_brt_loader_open (/* out */ BRTLOADER *blp,
                          CACHETABLE cachetable,
			  generate_row_for_put_func g,
			  DB *src_db,
			  int N, DB*dbs[/*N*/],
			  const struct descriptor *descriptors[/*N*/],
			  const char *new_fnames_in_env[/*N*/],
			  brt_compare_func bt_compare_functions[/*N*/],
			  const char *temp_file_template,
                          LSN load_lsn)
/* Effect: called by DB_ENV->create_loader to create a brt loader.
 * Arguments:
 *   blp                  Return the brt loader here.
 *   g                    The function for generating a row
 *   src_db               The source database.  Needed by g.  May be NULL if that's ok with g.
 *   N                    The number of dbs to create.
 *   dbs                  An array of open databases.  Used by g.  The data will be put in these database.
 *   new_fnames           The file names (these strings are owned by the caller: we make a copy for our own purposes).
 *   temp_file_template   A template suitable for mkstemp()
 * Return value: 0 on success, an error number otherwise.
 */
{
    BRTLOADER CALLOC(bl); // initialized to all zeros (hence CALLOC)
    if (!bl) return errno;

    BL_TRACE("calibrate begin");
#if BL_DO_TRACE
    sleep(1);
#endif
    BL_TRACE("calibrate done");

    bl->panic = 0;
    bl->panic_errno = 0;

    bl->generate_row_for_put = g;
    bl->cachetable = cachetable;

    bl->src_db = src_db;
    bl->N = N;
    bl->load_lsn = load_lsn;

#define MY_CALLOC_N(n,v) CALLOC_N(n,v); if (!v) { int r = errno; brtloader_destroy(bl, TRUE); return r; }
#define SET_TO_MY_STRDUP(lval, s) do { char *v = toku_strdup(s); if (!v) { int r = errno; brtloader_destroy(bl, TRUE); return r; } lval = v; } while (0)

    MY_CALLOC_N(N, bl->dbs);
    for (int i=0; i<N; i++) bl->dbs[i]=dbs[i];
    MY_CALLOC_N(N, bl->descriptors);
    for (int i=0; i<N; i++) bl->descriptors[i]=descriptors[i];
    MY_CALLOC_N(N, bl->new_fnames_in_env);
    for (int i=0; i<N; i++) SET_TO_MY_STRDUP(bl->new_fnames_in_env[i], new_fnames_in_env[i]);
    MY_CALLOC_N(N, bl->bt_compare_funs);
    for (int i=0; i<N; i++) bl->bt_compare_funs[i] = bt_compare_functions[i];

    MY_CALLOC_N(N, bl->fractal_queues);
    for (int i=0; i<N; i++) bl->fractal_queues[i]=NULL;
    MY_CALLOC_N(N, bl->fractal_threads);
    MY_CALLOC_N(N, bl->fractal_threads_live);
    for (int i=0; i<N; i++) bl->fractal_threads_live[i] = FALSE;

    brtloader_init_file_infos(&bl->file_infos);

    SET_TO_MY_STRDUP(bl->temp_file_template, temp_file_template);

    bl->n_rows   = 0; 
    bl->progress = 0;

    bl->rows = (struct rowset *) toku_malloc(N*sizeof(struct rowset));                   if (bl->rows == NULL) return ENOSPC;
    bl->fs   = (struct merge_fileset *) toku_malloc(N*sizeof(struct merge_fileset));     if (bl->rows == NULL) return ENOSPC;
    for(int i=0;i<N;i++) {
        { int r = init_rowset(&bl->rows[i]); if (r!=0) return r; }
        init_merge_fileset(&bl->fs[i]);
    }

    brt_loader_init_error_callback(&bl->error_callback);
    brt_loader_init_poll_callback(&bl->poll_callback);

    { int r = init_rowset(&bl->primary_rowset); if (r!=0) return r; }
    { int r = queue_create(&bl->primary_rowset_queue, 1); if (r!=0) return r; }
    //printf("%s:%d toku_pthread_create\n", __FILE__, __LINE__);
    { int r = toku_pthread_create(&bl->extractor_thread, NULL, extractor_thread, (void*)bl); if (r!=0) return r; }

    *blp = bl;
    BL_TRACE("open");
    return 0;
}

static void brt_loader_set_panic(BRTLOADER bl, int error) {
    bl->panic = 1;
    bl->panic_errno = error;
}

static FILE *bl_fidx2file (BRTLOADER bl, FIDX i) {
    { int r2 = toku_pthread_mutex_lock(&bl->file_infos.lock); assert(r2==0); }
    assert(i.idx >=0 && i.idx < bl->file_infos.n_files);
    assert(bl->file_infos.file_infos[i.idx].is_open);
    FILE *result=bl->file_infos.file_infos[i.idx].file;
    { int r2 = toku_pthread_mutex_unlock(&bl->file_infos.lock); assert(r2==0); }
    return result;
}

static int bl_fwrite(void *ptr, size_t size, size_t nmemb, FIDX streami, BRTLOADER bl)
/* Effect: this is a wrapper for fwrite that returns 0 on success, otherwise returns an error number.
 * Arguments:
 *   ptr    the data to be writen.
 *   size   the amount of data to be written.
 *   nmemb  the number of units of size to be written.
 *   stream write the data here.
 *   bl     passed so we can panic the brtloader if something goes wrong (recording the error number).
 * Return value: 0 on success, an error number otherwise.
 */
{
    FILE *stream = bl_fidx2file(bl, streami);
    size_t r = do_fwrite(ptr, size, nmemb, stream);
    if (r!=nmemb) {
	int e;
	if (os_fwrite_fun)    // if using hook to induce artificial errors (for testing) ...
	    e = errno;        // ... then there is no error in the stream, but there is one in errno
	else
	    e = ferror(stream);
	assert(e!=0);
        brt_loader_set_panic(bl, e);
	return e;
    }
    return 0;
}

static int bl_fread (void *ptr, size_t size, size_t nmemb, FIDX streami, BRTLOADER bl)
/* Effect: this is a wrapper for fread that returns 0 on success, otherwise returns an error number.
 * Arguments:
 *  ptr      read data into here.
 *  size     size of data element to be read.
 *  nmemb    number of data elements to be read.
 *  stream   where to read the data from.
 *  bl       passed so we can panic the brtloader if something goes wrong (recording the error number.
 * Return value: 0 on success, an error number otherwise.
 */
{
    FILE *stream = bl_fidx2file(bl, streami);
    size_t r = fread(ptr, size, nmemb, stream);
    if (r==0) {
	if (feof(stream)) return EOF;
	else {
	do_error: ;
	    int e = ferror(stream);
            brt_loader_set_panic(bl, e);
	    return e;
	}
    } else if (r<nmemb) {
	goto do_error;
    } else {
	return 0;
    }
}

static int bl_write_dbt (DBT *dbt, FIDX datafile, uint64_t *dataoff, BRTLOADER bl)
{
    int r;
    int dlen = dbt->size;
    if ((r=bl_fwrite(&dlen,     sizeof(dlen), 1,    datafile, bl))) return r;
    if ((r=bl_fwrite(dbt->data, 1,            dlen, datafile, bl))) return r;
    if (dataoff)
	*dataoff += dlen + sizeof(dlen);
    return 0;
}

static int bl_read_dbt (/*in*/DBT *dbt, FIDX datafile, BRTLOADER bl)
{
    int len;
    {
	int r;
	if ((r = bl_fread(&len, sizeof(len), 1, datafile, bl))) return r;
	assert(len>=0);
    }
    if ((int)dbt->ulen<len) { dbt->ulen=len; dbt->data=toku_xrealloc(dbt->data, len); }
    {
	int r;
	if ((r = bl_fread(dbt->data, 1, len, datafile, bl)))     return r;
    }
    dbt->size = len;
    return 0;
}

int loader_write_row(DBT *key, DBT *val, FIDX data, u_int64_t *dataoff, BRTLOADER bl)
/* Effect: Given a key and a val (both DBTs), write them to a file.  Increment *dataoff so that it's up to date.
 * Arguments:
 *   key, val   write these.
 *   data       the file to write them to
 *   dataoff    a pointer to a counter that keeps track of the amount of data written so far.
 *   bl         the brtloader (passed so we can panic if needed).
 * Return value: 0 on success, an error number otherwise.
 */
{
    //int klen = key->size;
    //int vlen = val->size;
    int r;
    // we have a chance to handle the errors because when we close we can delete all the files.
    if ((r=bl_write_dbt(key, data, dataoff, bl))) return r;
    if ((r=bl_write_dbt(val, data, dataoff, bl))) return r;
    { int r2 = toku_pthread_mutex_lock(&bl->file_infos.lock); assert(r2==0); }
    bl->file_infos.file_infos[data.idx].n_rows++;
    { int r2 = toku_pthread_mutex_unlock(&bl->file_infos.lock); assert(r2==0); }
    return 0;
}

int loader_read_row (FIDX f, DBT *key, DBT *val, BRTLOADER bl)
/* Effect: Read a key value pair from a file.  The DBTs must have DB_DBT_REALLOC set.
 * Arguments:
 *    f         where to read it from.
 *    key, val  read it into these.
 *    bl        passed so we can panic if needed.
 * Return value: 0 on success, an error number otherwise.
 * Requires:   The DBTs must have DB_DBT_REALLOC
 */
{
    {
	int r = bl_read_dbt(key, f, bl);
	if (r!=0) return r;
    }
    {
	int r = bl_read_dbt(val, f, bl);
	if (r!=0) return r;
    }
    return 0;
}


// 1024 is the right number for production.
//#define SIZE_FACTOR 1
#define SIZE_FACTOR 1024

int init_rowset (struct rowset *rows)
/* Effect: Initialize a collection of rows to be empty. */
{
    rows->rows = NULL;
    rows->data = NULL;

    rows->n_rows = 0;
    rows->n_rows_limit = 100;
    MALLOC_N(rows->n_rows_limit, rows->rows);
    rows->n_bytes = 0;
    rows->n_bytes_limit = 1024*SIZE_FACTOR*16;
    rows->data = (char *) toku_malloc(rows->n_bytes_limit);
    if (rows->rows==NULL || rows->data==NULL) {
	int r = errno;
	toku_free(rows->rows);
	toku_free(rows->data);
	rows->rows = NULL;
	rows->data = NULL;
	return r;
    } else {
	return 0;
    }
}

static void zero_rowset (struct rowset *rows) {
    memset(rows, 0, sizeof(*rows));
}

void destroy_rowset (struct rowset *rows) {
    toku_free(rows->data);
    toku_free(rows->rows);
    zero_rowset(rows);
}

const size_t data_buffer_limit = 1024*SIZE_FACTOR*64;

static int row_wont_fit (struct rowset *rows, size_t size)
/* Effect: Return nonzero if adding a row of size SIZE would be too big (bigger than the buffer limit) */ 
{
    return (data_buffer_limit < rows->n_bytes + size);
}

static void reset_rows (struct rowset *rows)
/* Effect: Reset the rows to an empty collection (but reuse any allocated space) */
{
    rows->n_bytes = 0;
    rows->n_rows = 0;
}

void add_row (struct rowset *rows, DBT *key, DBT *val)
/* Effect: add a row to a collection. */
{
    if (rows->n_rows >= rows->n_rows_limit) {
	rows->n_rows_limit *= 2;
	REALLOC_N(rows->n_rows_limit, rows->rows);
    }
    size_t off      = rows->n_bytes;
    size_t next_off = off + key->size + val->size;

    struct row newrow; 
    memset(&newrow, 0, sizeof newrow); newrow.off = off; newrow.klen = key->size; newrow.vlen = val->size;

    rows->rows[rows->n_rows++] = newrow;
    if (next_off > rows->n_bytes_limit) {
	while (next_off > rows->n_bytes_limit) {
	    rows->n_bytes_limit = rows->n_bytes_limit*2; 
	}
	assert(next_off <= rows->n_bytes_limit);
	REALLOC_N(rows->n_bytes_limit, rows->data);
    }
    memcpy(rows->data+off,           key->data, key->size);
    memcpy(rows->data+off+key->size, val->data, val->size);
    rows->n_bytes = next_off;
}

static int process_primary_rows (BRTLOADER bl, struct rowset *primary_rowset);

CILK_BEGIN
static int finish_primary_rows_internal (BRTLOADER bl) {
    // now we have been asked to finish up.
    int ra[bl->N];

    cilk_for (int i = 0; i < bl->N; i++) {
	struct rowset *rows = &(bl->rows[i]);
	//printf("%s:%d extractor finishing index %d with %ld rows\n", __FILE__, __LINE__, i, rows->n_rows);
	int progress_this_sort = 0; // fix?
	ra[i] = /*cilk_spawn*/sort_and_write_rows(rows, &(bl->fs[i]), bl, i, bl->dbs[i], bl->bt_compare_funs[i], progress_this_sort);
    }
    // cilk_sync;

    int r = 0;
    for (int i = 0; i < bl->N; i++)
        if (ra[i] != 0)
            r = ra[i];

    return r;
}
CILK_END

static int finish_primary_rows (BRTLOADER bl) {
#if defined(__cilkplusplus)
    return cilk::run(finish_primary_rows_internal, bl);
#else
    return           finish_primary_rows_internal (bl);
#endif
}

static void* extractor_thread (void *blv) {
    BL_TRACE("extractor_init");
    BRTLOADER bl = (BRTLOADER)blv;
    while (1) {
	void *item;
	{
	    BL_TRACE("extractor");
	    int r = queue_deq(bl->primary_rowset_queue, &item, NULL, NULL);
	    BL_TRACE("extractor_deq");
	    if (r==EOF) break;
	    assert(r==0); // other errors are arbitrarily bad.
	}
	struct rowset *primary_rowset = (struct rowset *)item;

	//printf("%s:%d extractor got %ld rows\n", __FILE__, __LINE__, primary_rowset.n_rows);

	assert(primary_rowset->n_rows>0);

	// Now we have some rows to output
	{
	    int r = process_primary_rows(bl, primary_rowset);
	    // !!! need to handle this error better.
	    if (r!=0) abort();
	}
    }

    //printf("%s:%d extractor finishing\n", __FILE__, __LINE__);

    int r = finish_primary_rows(bl); 
    r = r; // RFP 2535 assert(r==0); // !!! should deal with this.

    for (int i=0; i<bl->N; i++) {
	destroy_rowset(&bl->rows[i]); // destroy the rowset here so that  if we spawn the sort_and_write, there won't be a race on destroying the rows (thanks to the cilk_sync 2 lines up)
    }
    BL_TRACE("extractor");

    return 0;
}

static void enqueue_for_extraction (BRTLOADER bl) {
    //printf("%s:%d enqueing %ld items\n", __FILE__, __LINE__, bl->primary_rowset.n_rows);
    struct rowset *MALLOC(enqueue_me);
    *enqueue_me = bl->primary_rowset;
    zero_rowset(&bl->primary_rowset);
    int r = queue_enq(bl->primary_rowset_queue, (void*)enqueue_me, 1, NULL);
    assert(r==0);
}

static int loader_do_put(BRTLOADER bl,
                         DBT *pkey,
                         DBT *pval)
{
    add_row(&bl->primary_rowset, pkey, pval);
    if (row_wont_fit(&bl->primary_rowset, 0)) {
	// queue the rows for further processing by the extractor thread.
	//printf("%s:%d please extract %ld\n", __FILE__, __LINE__, bl->primary_rowset.n_rows);
	BL_TRACE("do_put");
	enqueue_for_extraction(bl);
	BL_TRACE("extract_enq");
	init_rowset(&bl->primary_rowset);
    }
    return 0;
}

static int finish_extractor (BRTLOADER bl) {
    //printf("%s:%d now finishing extraction\n", __FILE__, __LINE__);

    BL_TRACE("do_put");
    if (bl->primary_rowset.n_rows>0) {
	enqueue_for_extraction(bl);
	BL_TRACE("extract_enqeue");
    } else {
	destroy_rowset(&bl->primary_rowset);
    }
    //printf("%s:%d please finish extraction\n", __FILE__, __LINE__);
    {
	int r = queue_eof(bl->primary_rowset_queue);
	assert(r==0);
    }
    //printf("%s:%d joining\n", __FILE__, __LINE__);
    {
	void *toku_pthread_retval;
	int r = toku_pthread_join(bl->extractor_thread, &toku_pthread_retval);
	assert(r==0 && toku_pthread_retval==NULL);
	BL_TRACE("join_on_extractor");
    }
    {
	int r = queue_destroy(bl->primary_rowset_queue);
	assert(r==0);
    }
   //printf("%s:%d joined\n", __FILE__, __LINE__);
    return 0;
}

static const DBT zero_dbt = {0,0,0,0};
static DBT make_dbt (void *data, u_int32_t size) {
    DBT result = zero_dbt;
    result.data = data;
    result.size = size;
    return result;
}

CILK_BEGIN

static int process_primary_rows_internal (BRTLOADER bl, struct rowset *primary_rowset)
// process the rows in primary_rowset, and then destroy the rowset.
// if FLUSH is true then write all the buffered rows out.
// if primary_rowset is NULL then treat it as empty.
{
    BL_TRACE("cilk_call");
    int error_count = 0;
    int *MALLOC_N(bl->N, error_codes);

    // Do parallelize this loop with cilk_grainsize = 1 so that every iteration will run in parallel.
#if defined(__cilkplusplus)
    #pragma cilk_grainsize = 1
#endif
    cilk_for (int i = 0; i < bl->N; i++) {
	error_codes[i] = 0;
	struct rowset *rows = &(bl->rows[i]);
	struct merge_fileset *fs = &(bl->fs[i]);
	brt_compare_func compare = bl->bt_compare_funs[i];

	DBT skey = zero_dbt;
	skey.flags = DB_DBT_REALLOC;
	DBT sval=skey;

	// Don't parallelize this loop, or we have to lock access to add_row() which would be a lot of overehad.
	// Also this way we can reuse the DB_DBT_REALLOC'd value inside skey and sval without a race.
	for (size_t prownum=0; prownum<primary_rowset->n_rows; prownum++) {
	    if (error_count) break;

	    struct row *prow = &primary_rowset->rows[prownum];
	    DBT pkey = zero_dbt;
	    DBT pval = zero_dbt;
	    pkey.data = primary_rowset->data + prow->off;
	    pkey.size = prow->klen;
	    pval.data = primary_rowset->data + prow->off + prow->klen;
	    pval.size = prow->vlen;
	

	    {
		int r = bl->generate_row_for_put(bl->dbs[i], bl->src_db, &skey, &sval, &pkey, &pval, NULL);
		assert(r==0);
	    }

	    if (row_wont_fit(rows, skey.size + sval.size)) {
		//printf("rows.n_rows=%ld\n", rows.n_rows);
		BL_TRACE("puts");
		int progress_this_sort = 0; // fix?
		int r = sort_and_write_rows(rows, fs, bl, i, bl->dbs[i], compare, progress_this_sort); // cannot spawn this because of the race on rows.  If we were to create a new rows, and if sort_and_write_rows were to destroy the rows it is passed, we could spawn it, however.
		BL_TRACE("sort_and_write_rows");
		if (r!=0) {
		    error_codes[i] = r;
#if defined(__cilkplusplus)
		    __sync_fetch_and_add(&error_count, 1);
#else
                    error_count++;
#endif
		    break;
		}
		reset_rows(rows);
	    }
	    add_row(rows, &skey, &sval);

	    //flags==0 means generate_row_for_put callback changed it
	    //(and freed any memory necessary to do so) so that values are now stored
	    //in temporary memory that does not need to be freed.  We need to continue
	    //using DB_DBT_REALLOC however.
	    if (skey.flags == 0) {
		toku_init_dbt(&skey);
		skey.flags = DB_DBT_REALLOC;
	    }
	    if (sval.flags == 0) {
		toku_init_dbt(&sval);
		sval.flags = DB_DBT_REALLOC;
	    }
	}

	{
	    toku_free(skey.data);
	    toku_free(sval.data);
	    skey.data = sval.data = NULL; // set to NULL so that the final cleanup won't free them again.
	}
    }
    
    destroy_rowset(primary_rowset);
    toku_free(primary_rowset);
    int r = 0;
    if (error_count>0) {
	for (int i=0; i<bl->N; i++) {
	    if (error_codes[i]) r = error_codes[i];
	}
	assert(0); // could not find the error code.  This is an error in the program if we get here.
    }
    toku_free(error_codes);
    BL_TRACE("extractor");
    return r;
}
CILK_END

static int process_primary_rows (BRTLOADER bl, struct rowset *primary_rowset) {
    BL_TRACE("extractor");
#if defined(__cilkplusplus)
    int r = cilk::run(process_primary_rows_internal, bl, primary_rowset);
    BL_TRACE("cilk_return");
    return r;
#else
    return           process_primary_rows_internal (bl, primary_rowset);
#endif
}

 
int toku_brt_loader_put (BRTLOADER bl, DBT *key, DBT *val)
/* Effect: Put a key-value pair into the brt loader.  Called by DB_LOADER->put().
 * Return value: 0 on success, an error number otherwise.
 */
{
    if (bl->panic || brt_loader_get_error(&bl->error_callback)) 
        return EINVAL; // previous panic
    bl->n_rows++;
//    return loader_write_row(key, val, bl->fprimary_rows, &bl->fprimary_offset, bl);
    return loader_do_put(bl, key, val);
}

void toku_brt_loader_set_n_rows(BRTLOADER bl, u_int64_t n_rows) {
    bl->n_rows = n_rows;
}

u_int64_t toku_brt_loader_get_n_rows(BRTLOADER bl) {
    return bl->n_rows;
}

int merge_row_arrays_base (struct row dest[/*an+bn*/], struct row a[/*an*/], int an, struct row b[/*bn*/], int bn,
			   int which_db, DB *dest_db, brt_compare_func compare,
			   
			   BRTLOADER bl,
			   struct rowset *rowset)
/* Effect: Given two arrays of rows, a and b, merge them using the comparison function, and write them into dest.
 *   This function is suitable for use in a mergesort.
 *   If a pair of duplicate keys is ever noticed, then call the error_callback function (if it exists), and return DB_KEYEXIST.
 * Arguments:
 *   dest    write the rows here
 *   a,b     the rows being merged
 *   an,bn   the lenth of a and b respectively.
 *   dest_db We need the dest_db to run the comparison function.
 *   compare We need the compare function for the dest_db.
 */
{
    while (an>0 && bn>0) {
	DBT akey; memset(&akey, 0, sizeof akey); akey.data=rowset->data+a->off; akey.size=a->klen;
	DBT bkey; memset(&bkey, 0, sizeof bkey); bkey.data=rowset->data+b->off; bkey.size=b->klen;

	int compare_result = compare(dest_db, &akey, &bkey);
	if (compare_result==0) {
            if (bl->error_callback.error_callback) {
                DBT aval; memset(&aval, 0, sizeof aval); aval.data=rowset->data + a->off + a->klen; aval.size = a->vlen;
                brt_loader_set_error(&bl->error_callback, DB_KEYEXIST, dest_db, which_db, &akey, &aval);
	    }
	    return DB_KEYEXIST;
	} else if (compare_result<0) {
	    // a is smaller
	    *dest = *a;
	    dest++; a++; an--;
	} else {
	    *dest = *b;
	    dest++; b++; bn--;
	}
    }
    while (an>0) {
	*dest = *a;
	dest++; a++; an--;
    }
    while (bn>0) {
	*dest = *b;
	dest++; b++; bn--;
    }
    return 0;
}

static int binary_search (int *location,
			  const DBT *key,
			  struct row a[/*an*/], int an,
			  int abefore,
			  int which_db, DB *dest_db, brt_compare_func compare,
			  BRTLOADER bl,
			  struct rowset *rowset)
// Given a sorted array of rows a, and a dbt key, find the first row in a that is > key.
// If no such row exists, then consider the result to be equal to an.
// On success store abefore+the index into *location
// Return 0 on success.
// Return DB_KEYEXIST if we find a row that is equal to key.
{
    if (an==0) {
	*location = abefore;
	return 0;
    } else {
	int a2 = an/2;
	DBT akey = make_dbt(rowset->data+a[a2].off,  a[a2].klen);
	int compare_result = compare(dest_db, key, &akey);
	if (compare_result==0) {
	    if (bl->error_callback.error_callback) {
                DBT aval = make_dbt(rowset->data + a[a2].off + a[a2].klen,  a[a2].vlen);
                brt_loader_set_error(&bl->error_callback, DB_KEYEXIST, dest_db, which_db, &akey, &aval);
	    }
	    return DB_KEYEXIST;
	} else if (compare_result<0) {
	    // key is before a2
	    if (an==1) {
		*location = abefore;
		return 0;
	    } else {
		return binary_search(location, key,
				     a,    a2,
				     abefore,
				     which_db, dest_db, compare, bl, rowset);
	    }
	} else {
	    // key is after a2
	    if (an==1) {
		*location = abefore + 1;
		return 0;
	    } else {
		return binary_search(location, key,
				     a+a2, an-a2,
				     abefore+a2,
				     which_db, dest_db, compare, bl, rowset);
	    }
	}
    }
}
		   

#define SWAP(typ,x,y) { typ tmp = x; x=y; y=tmp; }

CILK_BEGIN
static int merge_row_arrays (struct row dest[/*an+bn*/], struct row a[/*an*/], int an, struct row b[/*bn*/], int bn,
			     int which_db, DB *dest_db, brt_compare_func compare,
			     BRTLOADER bl,
			     struct rowset *rowset)
/* Effect: Given two sorted arrays of rows, a and b, merge them using the comparison function, and write them into dest.
 *   This function is a cilk function with parallelism, and is suitable for use in a mergesort.
 * Arguments:
 *   dest    write the rows here
 *   a,b     the rows being merged
 *   an,bn   the lenth of a and b respectively.
 *   dest_db We need the dest_db to run the comparison function.
 *   compare We need the compare function for the dest_db.
 */
{
    if (an + bn < 10000) {
	return merge_row_arrays_base(dest, a, an, b, bn, which_db, dest_db, compare, bl, rowset);
    }
    if (an < bn) {
	SWAP(struct row *,a, b)
	SWAP(int         ,an,bn)
    }
    // an >= bn
    int a2 = an/2;
    DBT akey = make_dbt(rowset->data+a[a2].off, a[a2].klen);
    int b2 = 0; // initialize to zero so we can add the answer in.
    {
	int r = binary_search(&b2, &akey, b, bn, 0, which_db, dest_db, compare, bl, rowset);
	if (r!=0) return r; // for example if we found a duplicate, called the error_callback, and now we return an error code.
    }
    int ra, rb;
    ra = cilk_spawn merge_row_arrays(dest,       a,    a2,    b,    b2,    which_db, dest_db, compare, bl, rowset);
    rb =            merge_row_arrays(dest+a2+b2, a+a2, an-a2, b+b2, bn-b2, which_db, dest_db, compare, bl, rowset);
    cilk_sync;
    if (ra!=0) return ra;
    else       return rb;
}
CILK_END

CILK_BEGIN
int mergesort_row_array (struct row rows[/*n*/], int n, int which_db, DB *dest_db, brt_compare_func compare, BRTLOADER bl, struct rowset *rowset)
/* Sort an array of rows (using mergesort).
 * Arguments:
 *   rows   sort this array of rows.
 *   n      the length of the array.
 *   dest_db  used by the comparison function.
 *   compare  the compare function
 */
{
    if (n<=1) return 0; // base case is sorted
    int mid = n/2;
    int r1, r2;
    r1 = cilk_spawn mergesort_row_array (rows,     mid,   which_db, dest_db, compare, bl, rowset);

    // Don't spawn this one explicitly
    r2 =            mergesort_row_array (rows+mid, n-mid, which_db, dest_db, compare, bl, rowset);

    cilk_sync;
    if (r1!=0) return r1;
    if (r2!=0) return r2;

    struct row *MALLOC_N(n, tmp);
    {
	int r = merge_row_arrays(tmp, rows, mid, rows+mid, n-mid, which_db, dest_db, compare, bl, rowset);
	if (r!=0) {
	    toku_free(tmp);
	    return r;
	}
    }
    memcpy(rows, tmp, sizeof(*tmp)*n);
    toku_free(tmp);
    return 0;
}
CILK_END

// C function for testing mergesort_row_array 
int brt_loader_mergesort_row_array (struct row rows[/*n*/], int n, int which_db, DB *dest_db, brt_compare_func compare, BRTLOADER bl, struct rowset *rowset) {
#if defined(__cilkplusplus)
    return cilk::run(mergesort_row_array, rows, n, which_db, dest_db, compare, bl, rowset);
#else
    return           mergesort_row_array (rows, n, which_db, dest_db, compare, bl, rowset);
#endif
}

CILK_BEGIN
static int sort_rows (struct rowset *rows, int which_db, DB *dest_db, brt_compare_func compare,
		      BRTLOADER bl)
/* Effect: Sort a collection of rows.
 * If any duplicates are found, then call the error_callback function and return non zero.
 * Otherwise return 0.
 * Arguments:
 *   rowset    the */
{
    return mergesort_row_array(rows->rows, rows->n_rows, which_db, dest_db, compare, bl, rows);
}
CILK_END

/* filesets Maintain a collection of files.  Typically these files are each individually sorted, and we will merge them.
 * These files have two parts, one is for the data rows, and the other is a collection of offsets so we an more easily parallelize the manipulation (e.g., by allowing us to find the offset of the ith row quickly). */

void init_merge_fileset (struct merge_fileset *fs)
/* Effect: Initialize a fileset */ 
{
    fs->n_temp_files = 0;
    fs->n_temp_files_limit = 0;
    fs->data_fidxs = NULL;
}

void destroy_merge_fileset (struct merge_fileset *fs)
/* Effect: Destroy a fileset. */
{
    fs->n_temp_files = 0;
    fs->n_temp_files_limit = 0;
    toku_free(fs->data_fidxs);
    fs->data_fidxs = NULL;
}


static int extend_fileset (BRTLOADER bl, struct merge_fileset *fs, FIDX*ffile)
/* Effect: Add two files (one for data and one for idx) to the fileset.
 * Arguments:
 *   bl   the brtloader (needed to panic if anything goes wrong, and also to get the temp_file_template.
 *   fs   the fileset
 *   ffile  the data file (which will be open)
 *   fidx   the index file (which will be open)
 */
{
    FIDX sfile;
    int r;
    r = brtloader_open_temp_file(bl, &sfile); if (r!=0) return r;

    if (fs->n_temp_files+1 > fs->n_temp_files_limit) {
	fs->n_temp_files_limit = (fs->n_temp_files+1)*2;
	REALLOC_N(fs->n_temp_files_limit, fs->data_fidxs);
    }
    fs->data_fidxs[fs->n_temp_files] = sfile;
    fs->n_temp_files++;

    *ffile = sfile;
    return 0;
}

// RFP maybe this should be buried in the brtloader struct
// This was previously a cilk lock, but now we need it to work for pthreads too.
toku_pthread_mutex_t update_progress_lock = TOKU_PTHREAD_MUTEX_INITIALIZER;

static int update_progress (int N,
			    BRTLOADER bl,
			    const char *UU(message))
{
    // Need a lock here because of cilk and also the various pthreads.
    // Must protect the increment and the call to the poll_function.
    toku_pthread_mutex_lock(&update_progress_lock);
    bl->progress+=N;

    //printf(" %20s: %d ", message, bl->progress);
    int r = brt_loader_call_poll_function(&bl->poll_callback, (float)bl->progress/(float)PROGRESS_MAX);
    toku_pthread_mutex_unlock(&update_progress_lock);
    return r;
}

CILK_BEGIN
int sort_and_write_rows (struct rowset *rows, struct merge_fileset *fs, BRTLOADER bl, int which_db, DB *dest_db, brt_compare_func compare,
			 int progress_allocation)
/* Effect: Given a rowset, sort it and write it to a temporary file.
 * Arguments:
 *   rows    the rowset
 *   fs      the fileset into which the sorted data will be added
 *   bl      the brtloader
 *   dest_db the DB, needed for the comparison function.
 *   compare The comparison function.
 * Returns 0 on success, otherwise an error number.
 */
{
    //printf(" sort_and_write use %d progress=%d fin at %d\n", progress_allocation, bl->progress, bl->progress+progress_allocation);
    FIDX sfile;
    u_int64_t soffset=0;
    // TODO: erase the files, and deal with all the cleanup on error paths
    //printf("%s:%d sort_rows n_rows=%ld\n", __FILE__, __LINE__, rows->n_rows);
    int r = sort_rows(rows, which_db, dest_db, compare, bl);
    if (r!=0) {
	return r;
    }
    r = update_progress(progress_allocation/2, bl, "sorted rows");
    progress_allocation -= progress_allocation/2;
    if (r!=0) return r;

    r = extend_fileset(bl, fs, &sfile);
    if (r!=0) return r;
    for (size_t i=0; i<rows->n_rows; i++) {
	DBT skey; memset(&skey, 0, sizeof skey); skey.data = rows->data + rows->rows[i].off;                      skey.size=rows->rows[i].klen;
	DBT sval; memset(&sval, 0, sizeof sval); sval.data = rows->data + rows->rows[i].off + rows->rows[i].klen; sval.size=rows->rows[i].vlen;

	r = loader_write_row(&skey, &sval, sfile, &soffset, bl);
	if (r!=0) return r;
    }
    r = brtloader_fi_close(&bl->file_infos, sfile);  if (r!=0) return r;
    
    return update_progress(progress_allocation, bl, "wrote sorted");
}
CILK_END

// C function for testing sort_and_write_rows
int brt_loader_sort_and_write_rows (struct rowset *rows, struct merge_fileset *fs, BRTLOADER bl, int which_db, DB *dest_db, brt_compare_func compare,
                                    int progress_allocation) {
#if defined(__cilkplusplus)
    return cilk::run(sort_and_write_rows, rows, fs, bl, which_db, dest_db, compare, progress_allocation);
#else
    return           sort_and_write_rows (rows, fs, bl, which_db, dest_db, compare, progress_allocation);
#endif
}

static int merge_some_files (const BOOL to_q, FIDX dest_data, QUEUE q, int n_sources, FIDX srcs_data[/*n_sources*/], BRTLOADER bl, int which_db, DB *dest_db, brt_compare_func compare, int progress_allocation)
/* Effect: Given an array of FILE*'s each containing sorted, merge the data and write it to an output.  All the files remain open after the merge.
 *   This merge is performed in one pass, so don't pass too many files in.  If you need a tree of merges do it elsewhere.
 *   If TO_Q is true then we write rowsets into queue Q.  Otherwise we write into dest_data.
 * Modifies:  May modify the arrays of files (but if modified, it must be a permutation so the caller can use that array to close everything.)
 * Requires: The number of sources is at least one, and each of the input files must have at least one row in it.
 * Implementation note: Currently this code uses a really stupid heap O(n) time per pop instead of O(log n), but we'll fix that soon.
 * Arguments:
 *   to_q         boolean indicating that output is queue (true) or a file (false)
 *   dest_data    where to write the sorted data
 *   q            where to write the sorted data
 *   n_sources    how many source files.
 *   srcs_data    the array of source data files.
 *   bl           the brtloader.
 *   dest_db      the destination DB (used in the comparison function).
 * Return value: 0 on success, otherwise an error number.
 */
{
    //printf(" merge_some_files progress=%d fin at %d\n", bl->progress, bl->progress+progress_allocation);
    DBT keys[n_sources];
    DBT vals[n_sources];
    u_int64_t dataoff[n_sources];
    DBT zero; memset(&zero, 0, sizeof zero);  zero.data=0; zero.flags=DB_DBT_REALLOC; zero.size=0; zero.ulen=0;

    for (int i=0; i<n_sources; i++) {
	keys[i] = vals[i] = zero; // fill these all in with zero so we can delete stuff more reliably.
    }

    pqueue_t      *pq;
    pqueue_node_t *pq_nodes = (pqueue_node_t *)toku_malloc(n_sources * sizeof(pqueue_node_t));

    {
	int r = pqueue_init(&pq, n_sources, which_db, dest_db, compare, &bl->error_callback);
	if (r) return r;
    }

    u_int64_t n_rows = 0;
    // load pqueue with first value from each source
    for (int i=0; i<n_sources; i++) {
	int r = loader_read_row(srcs_data[i], &keys[i], &vals[i], bl);
	if (r==EOF) continue; // if the file is empty, don't initialize the pqueue.
	if (r!=0) return r;

        pq_nodes[i].key = &keys[i];
        pq_nodes[i].val = &vals[i];
        pq_nodes[i].i   = i;
        r = pqueue_insert(pq, &pq_nodes[i]);
        if (r!=0) {
            for (int j=0; j<=i; j++) {
                toku_free(keys[j].data);
                toku_free(vals[j].data);
            }
            pqueue_free(pq);
            toku_free(pq_nodes);
            return r;
        }

	dataoff[i] = 0;
	{ int r2 = toku_pthread_mutex_lock(&bl->file_infos.lock); assert(r2==0); }
	n_rows += bl->file_infos.file_infos[srcs_data[i].idx].n_rows;
	{ int r2 = toku_pthread_mutex_unlock(&bl->file_infos.lock); assert(r2==0); }
    }
    u_int64_t n_rows_done = 0;

    struct rowset *output_rowset = NULL;
    if (to_q) {
	MALLOC(output_rowset);
	assert(output_rowset);
	int r = init_rowset(output_rowset);
	assert(r==0);
    }
    
    //printf(" n_rows=%ld\n", n_rows);
    while (pqueue_size(pq)>0) {
        int r;
        int mini;
        {
            // get the minimum 
            pqueue_node_t *node;
            r = pqueue_pop(pq, &node);
            if (r!=0) {
                for (int i=0; i<n_sources; i++) {
                    toku_free(keys[i].data);
                    toku_free(vals[i].data);
                }
                pqueue_free(pq);
                toku_free(pq_nodes);
                return r;
            }
            mini = node->i;
        }
	if (to_q) {
	    if (row_wont_fit(output_rowset, keys[mini].size + vals[mini].size)) {
		r = queue_enq(q, (void*)output_rowset, 1, NULL);
		assert(r==0);
		MALLOC(output_rowset);
		assert(output_rowset);
		r = init_rowset(output_rowset);
		assert(r==0);
	    }
	    add_row(output_rowset, &keys[mini], &vals[mini]);
	} else {
            // write it to the dest file
	    r = loader_write_row(&keys[mini], &vals[mini], dest_data, &dataoff[mini], bl);
	    if (r!=0) return r;
	}
        
	{
            // read next row from file that just sourced min value 
	    r = loader_read_row(srcs_data[mini], &keys[mini], &vals[mini], bl);
	    if (r!=0) {
		if (feof(bl_fidx2file(bl, srcs_data[mini]))) {
                    // on feof, queue size permanently smaller
		    toku_free(keys[mini].data);
		    toku_free(vals[mini].data);
		} else {
		    r = ferror(bl_fidx2file(bl, srcs_data[mini]));
		    assert(r!=0);
		    return r;
		}
	    }
            else {
                // insert value into queue (re-populate queue)
                pq_nodes[mini].key = &keys[mini];
                r = pqueue_insert(pq, &pq_nodes[mini]);
                if (r!=0) {
                    for (int i=0; i<n_sources; i++) {
                        toku_free(keys[i].data);
                        toku_free(vals[i].data);
                    }
                    pqueue_free(pq);
                    toku_free(pq_nodes);
                    return r;
                }
            }
        }
            
        n_rows_done++;
	const u_int64_t rows_per_report = SIZE_FACTOR*1024;
	if (n_rows_done%rows_per_report==0) {
	    // need to update the progress.
	    double fraction_of_remaining_we_just_did = (double)rows_per_report / (double)(n_rows - n_rows_done + rows_per_report);
	    assert(0<= fraction_of_remaining_we_just_did && fraction_of_remaining_we_just_did<=1);
	    int progress_just_done = fraction_of_remaining_we_just_did * progress_allocation;
	    progress_allocation -= progress_just_done;
	    r = update_progress(progress_just_done, bl, "in file merge");
	    if (r!=0) return r;
	}
    }
    if (to_q) {
		int r = queue_enq(q, (void*)output_rowset, 1, NULL);
		assert(r==0);
    }
    pqueue_free(pq);
    toku_free(pq_nodes);
    return update_progress(progress_allocation, bl, "end of merge_some_files");
}




static int int_min (int a, int b)
{
    if (a<b) return a;
    else return b;
}

static int n_passes (int N, int B) {
    int result = 0;
    while (N>1) {
	N = (N+B-1)/B;
	result++;
    }
    return result;
}

int merge_files (struct merge_fileset *fs,
		 BRTLOADER bl,
		 // These are needed for the comparison function and error callback.
		 int which_db, DB *dest_db, brt_compare_func compare,
		 int progress_allocation,
		 // Write rowsets into this queue.
		 QUEUE output_q
		 )
/* Effect:  Given a fileset, merge all the files writing all the answers into a queue.
 *   All the files in fs, and any temporary files will be closed and unlinked (and the fileset will be empty)
 * Return value: 0 on success, otherwise an error number.
 *   On error *fs will contain no open files.  All the files (including any temporary files) will be closed and unlinked.
 *    (however the fs will still need to be deallocated.)
 */
{
    //printf(" merge_files use %d progress=%d fin at %d\n", progress_allocation, bl->progress, bl->progress+progress_allocation);
    const int mergelimit = (SIZE_FACTOR == 1) ? 4 : 256;
//    const int mergelimit = (SIZE_FACTOR == 1) ? 4 : 16;
    int n_passes_left = (fs->n_temp_files==1) ? 1 : n_passes(fs->n_temp_files, mergelimit);
    //printf("%d files, %d per pass, %d passes\n", fs->n_temp_files, mergelimit, n_passes_left);
    int result = 0;
    while (fs->n_temp_files > 0) {
	int progress_allocation_for_this_pass = progress_allocation/n_passes_left;
	progress_allocation -= progress_allocation_for_this_pass;

	assert(fs->n_temp_files>0);
	struct merge_fileset next_file_set;
	BOOL to_queue = (BOOL)(fs->n_temp_files <= mergelimit);
	init_merge_fileset(&next_file_set);
	while (fs->n_temp_files>0) {
	    // grab some files and merge them.
	    int n_to_merge = int_min(mergelimit, fs->n_temp_files);

	    // We are about to do n_to_merge/n_temp_files of the remaining for this pass.
	    int progress_allocation_for_this_subpass = progress_allocation_for_this_pass * (double)n_to_merge / (double)fs->n_temp_files;
	    progress_allocation_for_this_pass -= progress_allocation_for_this_subpass;


	    //printf("%s:%d merging\n", __FILE__, __LINE__);
	    FIDX merged_data = FIDX_NULL;

	    FIDX *MALLOC_N(n_to_merge, datafiles);
	    for (int i=0; i<n_to_merge; i++) datafiles[i] = FIDX_NULL;
	    for (int i=0; i<n_to_merge; i++) {
		int idx = fs->n_temp_files -1 -i;
		datafiles[i] = fs->data_fidxs[idx];
		result = brtloader_fi_reopen(&bl->file_infos, datafiles[i], "r");
		if (result) { printf("%s:%d r=%d\n", __FILE__, __LINE__, result); break; }
	    }
	    if (result==0 && !to_queue) {
		result = extend_fileset(bl, &next_file_set,  &merged_data);
		if (result!=0) { printf("%s:%d r=%d\n", __FILE__, __LINE__, result); break; }
	    }

	    if (result==0) {
		result = merge_some_files(to_queue, merged_data, output_q, n_to_merge, datafiles, bl, which_db, dest_db, compare, progress_allocation_for_this_subpass);
		// if result!=0, fall through
		if (result==0) {
		    /*nothing*/;// this is gratuitous, but we need something to give code coverage tools to help us know that it's important to distinguish between result==0 and result!=0
		}
	    }

	    //printf("%s:%d merged\n", __FILE__, __LINE__);
	    for (int i=0; i<n_to_merge; i++) {
		if (!fidx_is_null(datafiles[i])) {
		    {
			int r = brtloader_fi_close(&bl->file_infos, datafiles[i]);
			if (r!=0 && result==0) result = r;
		    }
		    {
			int r = brtloader_fi_unlink(&bl->file_infos, datafiles[i]);
			if (r!=0 && result==0) result = r;
		    }
		    datafiles[i] = FIDX_NULL;
		}
	    }

	    fs->n_temp_files -= n_to_merge;
	    if (!to_queue && !fidx_is_null(merged_data)) {
		int r = brtloader_fi_close(&bl->file_infos, merged_data);
		if (r!=0 && result==0) result = r;
	    }
	    toku_free(datafiles);

	    if (result!=0) break;
	}


	toku_free(fs->data_fidxs);
	*fs = next_file_set;

	// Update the progress
	n_passes_left--;
	{
	    int r = update_progress(progress_allocation_for_this_pass, bl, "merging files");
	    if (r!=0 && result==0) result = r;
	}

	if (result!=0) break;
    }
    {
	int r = queue_eof(output_q);
	if (r!=0 && result==0) result = r;
    }
    {
	int r = update_progress(progress_allocation, bl, "did merge_files");
	if (r!=0 && result==0) result = r;
    }
    return result;
}

struct subtree_info {
    int64_t block;
    struct subtree_estimates subtree_estimates;
    int32_t fingerprint;
};

struct subtrees_info {
    int64_t next_free_block;
    int64_t n_subtrees;       // was n_blocks
    int64_t n_subtrees_limit;
    struct subtree_info *subtrees;
};

static void allocate_node (struct subtrees_info *sts, int64_t b, const struct subtree_estimates est, const int fingerprint) {
    if (sts->n_subtrees >= sts->n_subtrees_limit) {
	sts->n_subtrees_limit *= 2;
	XREALLOC_N(sts->n_subtrees_limit, sts->subtrees);
    }
    sts->subtrees[sts->n_subtrees].subtree_estimates = est;
    sts->subtrees[sts->n_subtrees].block = b;
    sts->subtrees[sts->n_subtrees].fingerprint = fingerprint;
    sts->n_subtrees++;
}

struct dbuf {
    unsigned char *buf;
    int buflen;
    int off;
};

struct leaf_buf {
    int64_t blocknum;
    struct dbuf dbuf;
    unsigned int rand4fingerprint;
    unsigned int local_fingerprint;
    int local_fingerprint_p;
    int nkeys, ndata, dsize, n_in_buf;
    int nkeys_p, ndata_p, dsize_p, partitions_p, n_in_buf_p;
};

const int nodesize = (SIZE_FACTOR==1) ? (1<<15) : (1<<22);

struct translation {
    int64_t off, size;
};

struct dbout {
    int fd;
    toku_off_t current_off;

    int64_t n_translations;
    int64_t n_translations_limit;
    struct translation *translation;
#ifndef CILK_STUB
    cilk::mutex mutex;
#endif
};

static inline void dbout_lock(struct dbout *out) {
#ifndef CILK_STUB
    out->mutex.lock();
#else
    out = out;
#endif
}

static inline void dbout_unlock(struct dbout *out) {
#ifndef CILK_STUB
    out->mutex.unlock();
#else
    out = out;
#endif
}

static void seek_align_locked(struct dbout *out) {
    toku_off_t old_current_off = out->current_off;
    int alignment = 4096;
    out->current_off += alignment-1;
    out->current_off &= ~(alignment-1);
    toku_off_t r = lseek(out->fd, out->current_off, SEEK_SET);
    if (r!=out->current_off) {
	fprintf(stderr, "Seek failed %s (errno=%d)\n", strerror(errno), errno);
    }
    assert(r==out->current_off);
    assert(out->current_off >= old_current_off);
    assert(out->current_off < old_current_off+alignment);
    assert(out->current_off % alignment == 0);
}

static void seek_align(struct dbout *out) {
    dbout_lock(out);
    seek_align_locked(out);
    dbout_unlock(out);
}

static void dbuf_init (struct dbuf *dbuf) {
    dbuf->buf=0;
    dbuf->buflen=0;
    dbuf->off=0;
}
static void dbuf_destroy (struct dbuf *dbuf) {
    toku_free(dbuf->buf);
}

static int64_t allocate_block (struct dbout *out)
// Return the new block number
{
    dbout_lock(out);
    int64_t result = out->n_translations;
    if (result >= out->n_translations_limit) {
	if (out->n_translations_limit==0) {
	    out->n_translations_limit = 1;
	} else {
	    out->n_translations_limit *= 2;
	}
	REALLOC_N(out->n_translations_limit, out->translation);
    }
    out->n_translations++;
    dbout_unlock(out);
    return result;
}

static void putbuf_bytes (struct dbuf *dbuf, const void *bytes, int nbytes) {
    if (dbuf->off + nbytes > dbuf->buflen) {
	dbuf->buflen += dbuf->off + nbytes;
	dbuf->buflen *= 2;
	REALLOC_N(dbuf->buflen, dbuf->buf);
	assert(dbuf->buf);
    }
    memcpy(dbuf->buf + dbuf->off, bytes, nbytes);
    dbuf->off += nbytes;
}

static void putbuf_int8  (struct dbuf *dbuf, unsigned char v) {
    putbuf_bytes(dbuf, &v, 1);
}

static void putbuf_int32 (struct dbuf *dbuf, int v) {
    putbuf_bytes(dbuf, &v, 4);
}
static void putbuf_int64 (struct dbuf *dbuf, unsigned long long v) {
    putbuf_int32(dbuf, v>>32);
    putbuf_int32(dbuf, v&0xFFFFFFFF);
}

static void putbuf_int32_at(struct dbuf *dbuf, int off, int v) {
    const int nbytes = 4;
    if (off+nbytes > dbuf->buflen) {
	dbuf->buflen += dbuf->off + nbytes;
	dbuf->buflen *= 2;
	REALLOC_N(dbuf->buflen, dbuf->buf);
	assert(dbuf->buf);
    }
    memcpy(dbuf->buf + off, &v, 4);
}
static void putbuf_int64_at(struct dbuf *dbuf, int off, unsigned long long v) {
    unsigned int a = v>>32;
    unsigned int b = v&0xFFFFFFFF;
    putbuf_int32_at(dbuf, off,   a);
    putbuf_int32_at(dbuf, off+4, b);
}

// glibc protects the "random" function with an inlined lock.  this lock is not understood by
// cilkscreen, so we have to tell cilkscreen that "random" is safe.
// RFP check windows random behaviour.
#ifndef CILK_STUB
static cilk::fake_mutex random_mutex;
#endif

static inline long int loader_random(void) {
#ifndef CILK_STUB
    random_mutex.lock();
#endif
    long int r = random();
#ifndef CILK_STUB
    random_mutex.unlock();
#endif
    return r;
}

static struct leaf_buf *start_leaf (struct dbout *out, const struct descriptor *desc, int64_t lblocknum) {
    assert(lblocknum < out->n_translations_limit);
    struct leaf_buf *MALLOC(lbuf);
    assert(lbuf);
    lbuf->blocknum = lblocknum;
    dbuf_init(&lbuf->dbuf);
    int height=0;
    int flags=0;
    int layout_version=BRT_LAYOUT_VERSION;
    putbuf_bytes(&lbuf->dbuf, "tokuleaf", 8);
    putbuf_int32(&lbuf->dbuf, layout_version);
    putbuf_int32(&lbuf->dbuf, layout_version); // layout_version original

    putbuf_int32(&lbuf->dbuf, desc->version); // desc version
    putbuf_int32(&lbuf->dbuf, desc->dbt.size); // desc size
    putbuf_bytes(&lbuf->dbuf, desc->dbt.data, desc->dbt.size);

    putbuf_int32(&lbuf->dbuf, nodesize);
    putbuf_int32(&lbuf->dbuf, flags);
    putbuf_int32(&lbuf->dbuf, height);
    lbuf->rand4fingerprint = loader_random();
    putbuf_int32(&lbuf->dbuf, lbuf->rand4fingerprint);
    lbuf->local_fingerprint = 0;
    lbuf->nkeys = lbuf->ndata = lbuf->dsize = 0;
    lbuf->n_in_buf = 0;
    
    // leave these uninitialized for now.
    lbuf->local_fingerprint_p = lbuf->dbuf.off;    lbuf->dbuf.off+=4;
    lbuf->nkeys_p             = lbuf->dbuf.off;    lbuf->dbuf.off+=8;
    lbuf->ndata_p             = lbuf->dbuf.off;    lbuf->dbuf.off+=8;
    lbuf->dsize_p             = lbuf->dbuf.off;    lbuf->dbuf.off+=8;
    lbuf->partitions_p        = lbuf->dbuf.off;    lbuf->dbuf.off+=4; lbuf->dbuf.off += stored_sub_block_map_size; // RFP partition map
    lbuf->n_in_buf_p          = lbuf->dbuf.off;    lbuf->dbuf.off+=4;

    return lbuf;
}

CILK_BEGIN
static void finish_leafnode (struct dbout *out, struct leaf_buf *lbuf, int progress_allocation, BRTLOADER bl);
static int write_nonleaves (BRTLOADER bl, FIDX pivots_fidx, struct dbout *out, struct subtrees_info *sts, const struct descriptor *descriptor);
CILK_END
static void add_pair_to_leafnode (struct leaf_buf *lbuf, unsigned char *key, int keylen, unsigned char *val, int vallen);
static int write_translation_table (struct dbout *out, long long *off_of_translation_p);
static void write_header (struct dbout *out, long long translation_location_on_disk, long long translation_size_on_disk, BLOCKNUM root_blocknum_on_disk, LSN load_lsn);

CILK_BEGIN
static int toku_loader_write_brt_from_q (BRTLOADER bl,
					 const struct descriptor *descriptor,
					 int fd, // write to here
					 int progress_allocation,
					 QUEUE q)
// Effect: Consume a sequence of rowsets work from a queue, creating a fractal tree.  Closes fd.
{

    int r;

    // The pivots file will contain all the pivot strings (in the form <size(32bits)> <data>)
    // The pivots_fname is the name of the pivots file.
    // Note that the pivots file will have one extra pivot in it (the last key in the dictionary) which will not appear in the tree.
    int64_t n_pivots=0; // number of pivots in pivots_file
    FIDX pivots_file;  // the file
    brtloader_open_temp_file (bl, &pivots_file);

    // The blocks_array will contain all the block numbers that correspond to the pivots.  Generally there should be one more block than pivot.
    struct subtrees_info sts; memset(&sts, 0, sizeof sts);
    sts.next_free_block = 3;
    sts.n_subtrees       = 0;
    sts.n_subtrees_limit = 1;

    XMALLOC_N(sts.n_subtrees_limit, sts.subtrees);

    struct dbout out; memset(&out, 0, sizeof out);
    out.fd = fd;
    out.current_off = 8192; // leave 8K reserved at beginning
    out.n_translations = 3; // 3 translations reserved at the beginning
    out.n_translations_limit = 4;

    MALLOC_N(out.n_translations_limit, out.translation);
    out.translation[0].off = -2LL; out.translation[0].size = 0; // block 0 is NULL
    assert(1==RESERVED_BLOCKNUM_TRANSLATION);
    assert(2==RESERVED_BLOCKNUM_DESCRIPTOR);
    out.translation[1].off = -1;                                // block 1 is the block translation, filled in later
    out.translation[2].off = -1;                                // block 2 is the descriptor
    seek_align(&out);
    int64_t lblock = allocate_block(&out);
    struct leaf_buf *lbuf = start_leaf(&out, descriptor, lblock);
    struct subtree_estimates est = zero_estimates;
    est.exact = TRUE;
    u_int64_t n_rows_remaining = bl->n_rows;
    u_int64_t old_n_rows_remaining = bl->n_rows;

    while (1) {
	void *item;
	{
	    int rr = queue_deq(q, &item, NULL, NULL);
	    if (rr==EOF) break;
	    if (rr!=0) { r=rr;  goto error; }
	}
	struct rowset *output_rowset = (struct rowset *)item;

	for (unsigned int i=0; i<output_rowset->n_rows; i++) {
	    DBT key = make_dbt(output_rowset->data+output_rowset->rows[i].off,                               output_rowset->rows[i].klen);
	    DBT val = make_dbt(output_rowset->data+output_rowset->rows[i].off + output_rowset->rows[i].klen, output_rowset->rows[i].vlen);

	    if (lbuf->dbuf.off >= nodesize) {

		int progress_this_node = progress_allocation * (double)(old_n_rows_remaining - n_rows_remaining)/(double)old_n_rows_remaining;
		progress_allocation -= progress_this_node;
		old_n_rows_remaining = n_rows_remaining;

		allocate_node(&sts, lblock, est, lbuf->local_fingerprint);

		n_pivots++;
		if ((r=bl_write_dbt(&key, pivots_file, NULL, bl))) {
		    // how to handle errors in fractal thread?
		    assert(r==0); // this always fails.
		    return r;
		}
	    
		cilk_spawn finish_leafnode(&out, lbuf, progress_this_node, bl);

		lblock = allocate_block(&out);
		lbuf = start_leaf(&out, descriptor, lblock);
	    }
	
	    add_pair_to_leafnode(lbuf, (unsigned char *) key.data, key.size, (unsigned char *) val.data, val.size);
	    est.nkeys++;
	    est.ndata++;
	    est.dsize+=key.size + val.size;
	    n_rows_remaining--;
	}
	destroy_rowset(output_rowset);
	toku_free(output_rowset);
    }

    allocate_node(&sts, lblock, est, lbuf->local_fingerprint);
    {
	int p = progress_allocation/2;
	finish_leafnode(&out, lbuf, p, bl);
	progress_allocation -= p;
    }

    cilk_sync;

    n_pivots++;

    {
	DBT key=make_dbt(0,0); // must write an extra DBT into the pivots file.
	r=bl_write_dbt(&key, pivots_file, NULL, bl);
	assert(r==0); //???!!!
    }

    r = write_nonleaves(bl, pivots_file, &out, &sts, descriptor);
    assert(r==0); //???!!!

    {
	assert(sts.n_subtrees==1);
	BLOCKNUM root_block = make_blocknum(sts.subtrees[0].block);
	toku_free(sts.subtrees);

	// write the descriptor
	{
	    seek_align(&out);
	    assert(out.n_translations >= RESERVED_BLOCKNUM_DESCRIPTOR);
	    assert(out.translation[RESERVED_BLOCKNUM_DESCRIPTOR].off == -1);
	    out.translation[RESERVED_BLOCKNUM_DESCRIPTOR].off = out.current_off;
	    size_t desc_size = 4+toku_serialize_descriptor_size(descriptor);
	    assert(desc_size>0);
	    out.translation[RESERVED_BLOCKNUM_DESCRIPTOR].size = desc_size;
	    struct wbuf wbuf;
	    char *MALLOC_N(desc_size, buf);
	    wbuf_init(&wbuf, buf, desc_size);
	    toku_serialize_descriptor_contents_to_wbuf(&wbuf, descriptor);
	    u_int32_t checksum = x1764_finish(&wbuf.checksum);
	    wbuf_int(&wbuf, checksum);
	    assert(wbuf.ndone==desc_size);
	    r = toku_os_write(out.fd, wbuf.buf, wbuf.ndone);
	    assert(r==0);
	    out.current_off += desc_size;
	    toku_free(buf);
	}
    
	long long off_of_translation;
	r = write_translation_table(&out, &off_of_translation);
	assert(r==0);
	write_header(&out, off_of_translation, (out.n_translations+1)*16+4, root_block, bl->load_lsn);
	if (out.translation) toku_free(out.translation);
	r = update_progress(progress_allocation, bl, "wrote tdb file");
    }

    if (r) goto error;
    r = fsync(fd);
    if (r) { r=errno; goto error; }
    r = close(fd);
    if (r) { r=errno; goto error; }

    // Do we need to pay attention to user_said_stop?  Or should the guy at the other end of the queue pay attention and send in an EOF.

 error:
    return 0;
}
CILK_END

int toku_loader_write_brt_from_q_in_C (BRTLOADER bl,
				       const struct descriptor *descriptor,
				       int fd, // write to here
				       int progress_allocation,
				       QUEUE q)
// This is probably only for testing.
{
#if defined(__cilkplusplus)
    return cilk::run(toku_loader_write_brt_from_q, bl, descriptor, fd, progress_allocation, q);
#else
    return           toku_loader_write_brt_from_q (bl, descriptor, fd, progress_allocation, q);
#endif
}


static void* fractal_thread (void *ftav) {
    BL_TRACE("start_fractal_thread");
    struct fractal_thread_args *fta = (struct fractal_thread_args *)ftav;
#if defined(__cilkplusplus)
    int r = cilk::run(toku_loader_write_brt_from_q, fta->bl, fta->descriptor, fta->fd, fta->progress_allocation, fta->q);
#else
    int r =           toku_loader_write_brt_from_q (fta->bl, fta->descriptor, fta->fd, fta->progress_allocation, fta->q);
#endif
    fta->errno_result = r;
    BL_TRACE("fractal_thread");
    return NULL;
}

static int loader_do_i (BRTLOADER bl,
			int which_db,
                        DB *dest_db,
                        brt_compare_func compare,
                        const struct descriptor *descriptor,
                        const char *new_fname,
                        int progress_allocation // how much progress do I need to add into bl->progress by the end..
                        )
/* Effect: Handle the file creating for one particular DB in the bulk loader. */
{
    //printf("doing i use %d progress=%d fin at %d\n", progress_allocation, bl->progress, bl->progress+progress_allocation);
    struct merge_fileset *fs = &(bl->fs[which_db]);
    struct rowset *rows = &(bl->rows[which_db]);
    assert(rows->data==NULL); // the rows should be all cleaned up alrea

    // a better allocation would be to figure out roughly how many merge passes we'll need.
    int allocation_for_merge = (2*progress_allocation)/3;
    progress_allocation -= allocation_for_merge;

    int r;
    r = queue_create(&bl->fractal_queues[which_db], 3);
    if (r) goto error;

    {
	mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
	int fd = open(new_fname, O_RDWR| O_CREAT | O_BINARY, mode);
	assert(fd>=0);


	struct fractal_thread_args fta = {bl,
					  descriptor,
					  fd,
					  progress_allocation,
					  bl->fractal_queues[which_db],
					  0 // result
	};

	r = toku_pthread_create(bl->fractal_threads+which_db, NULL, fractal_thread, (void*)&fta);
	if (r) {
	    int r2 __attribute__((__unused__)) = queue_destroy(bl->fractal_queues[which_db]);
	    // ignore r2, since we already have an error
	    goto error;
	}
	assert(bl->fractal_threads_live[which_db]==FALSE);
	bl->fractal_threads_live[which_db] = TRUE;
    }

    r = merge_files(fs, bl, which_db, dest_db, compare, allocation_for_merge, bl->fractal_queues[which_db]);

    {
	void *toku_pthread_retval;
	BL_TRACE("do_i");
	int r2 = toku_pthread_join(bl->fractal_threads[which_db], &toku_pthread_retval);
	BL_TRACE("join_on_fractal");
	assert(r2==0 && toku_pthread_retval==NULL);
	assert(bl->fractal_threads_live[which_db]);
	bl->fractal_threads_live[which_db] = FALSE;
    }

    {
	int r2 = queue_destroy(bl->fractal_queues[which_db]);
	assert(r2==0);
	bl->fractal_queues[which_db]=NULL;
    }

 error: // this is the cleanup code.  Even if r==0 (no error) we fall through to here.
    // if we get here we need to free up the merge_fileset and the rowset, as well as the keys
    toku_free(rows->data); rows->data = NULL;
    toku_free(rows->rows); rows->rows = NULL;
    toku_free(fs->data_fidxs); fs->data_fidxs = NULL;
    BL_TRACE("do_i");
    return r;
}

static int toku_brt_loader_close_internal (BRTLOADER bl)
/* Effect: Close the bulk loader.
 * Return all the file descriptors in the array fds. */
{
    BL_TRACE("puts");
    int result = 0;
    int remaining_progress = PROGRESS_MAX;
    // RFP cilk_for, no breaks in the loop
    for (int i=0; i<bl->N; i++) {
        char * fname_in_cwd = toku_cachetable_get_fname_in_cwd(bl->cachetable, bl->new_fnames_in_env[i]);
	// Take the unallocated progress and divide it among the unfinished jobs.
	// This calculation allocates all of the PROGRESS_MAX bits of progress to some job.
	int allocate_here = remaining_progress/(bl->N - i);
	remaining_progress -= allocate_here;
	//printf("%s:%d do_i(%d)\n", __FILE__, __LINE__, i);
	BL_TRACE("close");
	result = loader_do_i(bl, i, bl->dbs[i], bl->bt_compare_funs[i], bl->descriptors[i], fname_in_cwd,
			     allocate_here
                             );
        toku_free(fname_in_cwd);
	if (result!=0) goto error;
        toku_free((void*)bl->new_fnames_in_env[i]);
	bl->new_fnames_in_env[i] = NULL;
	assert(0<=bl->progress && bl->progress <= PROGRESS_MAX);
	result = update_progress(0, bl, "did index");
	if (result) goto error;
    }
    assert(bl->file_infos.n_files_open   == 0);
    assert(bl->file_infos.n_files_extant == 0);
    assert(bl->progress == PROGRESS_MAX);
 error:
    brtloader_destroy(bl, (BOOL)(result!=0));
    BL_TRACE("close");
    BL_TRACE_END;
    return result;
}

int toku_brt_loader_close (BRTLOADER bl,
                           brt_loader_error_func error_function, void *error_extra,
			   brt_loader_poll_func  poll_function,  void *poll_extra
			   )
{
    int r;

    //printf("Closing\n");

    brt_loader_set_error_function(&bl->error_callback, error_function, error_extra);

    brt_loader_set_poll_function(&bl->poll_callback, poll_function, poll_extra);

    r = finish_extractor(bl);
    assert(r==0); // !!! should check this error code and cleanup if needed.

    // check for an error during extraction
    r = brt_loader_call_error_function(&bl->error_callback);
    if (r) {
        brtloader_destroy(bl, TRUE);
        return r;
    }

    r = toku_brt_loader_close_internal(bl);

    return r;
}

int toku_brt_loader_abort(BRTLOADER bl, BOOL is_error) 
/* Effect : Abort the bulk loader, free brtloader resources */
{
    int result = 0;
    brtloader_destroy(bl, is_error);
    return result;
}

static void add_pair_to_leafnode (struct leaf_buf *lbuf, unsigned char *key, int keylen, unsigned char *val, int vallen) {
    lbuf->n_in_buf++;
    lbuf->nkeys++; // assume NODUP
    lbuf->ndata++;
    lbuf->dsize+= keylen + vallen;
    
    int le_off = lbuf->dbuf.off;
    putbuf_int8(&lbuf->dbuf, 1);
    putbuf_int32(&lbuf->dbuf, keylen);
    putbuf_int32(&lbuf->dbuf, vallen);
    putbuf_bytes(&lbuf->dbuf, key, keylen);
    putbuf_bytes(&lbuf->dbuf, val, vallen);
    int le_len = 1+4+4+keylen+vallen;
    assert(le_off + le_len == lbuf->dbuf.off);
    u_int32_t this_x = x1764_memory(lbuf->dbuf.buf + le_off, le_len);
    u_int32_t this_prod = lbuf->rand4fingerprint * this_x;
    lbuf->local_fingerprint += this_prod;
    if (0) {
	printf("%s:%d x1764(buf+%d, %d)=%8x\n", __FILE__, __LINE__, le_off, le_len, this_x);
	printf("%s:%d rand4fingerprint=%8x\n", __FILE__, __LINE__, lbuf->rand4fingerprint);
	printf("%s:%d this_prod=%8x\n", __FILE__, __LINE__, this_prod);
	printf("%s:%d local_fingerprint=%8x\n", __FILE__, __LINE__, lbuf->local_fingerprint);
    }
}

static void write_literal(struct dbout *out, void*data,  size_t len) {
    assert(out->current_off%4096==0);
    int r = toku_os_write(out->fd, data, len);
    assert(r==0);
    out->current_off+=len;
}

CILK_BEGIN
static void finish_leafnode (struct dbout *out, struct leaf_buf *lbuf, int progress_allocation, BRTLOADER bl) {
    //printf("  finishing leaf node progress=%d fin at %d\n", bl->progress, bl->progress+progress_allocation);
    //printf("local_fingerprint=%8x\n", lbuf->local_fingerprint);
    putbuf_int32_at(&lbuf->dbuf, lbuf->local_fingerprint_p, lbuf->local_fingerprint);
    putbuf_int64_at(&lbuf->dbuf, lbuf->nkeys_p,             lbuf->nkeys);
    putbuf_int64_at(&lbuf->dbuf, lbuf->ndata_p,             lbuf->ndata);
    putbuf_int64_at(&lbuf->dbuf, lbuf->dsize_p,             lbuf->dsize);

    // RFP abstract this
    const int32_t n_partitions = 1;
    struct sub_block_map partition_map;
    sub_block_map_init(&partition_map, 0, 0, 0);
    putbuf_int32_at(&lbuf->dbuf, lbuf->partitions_p,        n_partitions);
    putbuf_int32_at(&lbuf->dbuf, lbuf->partitions_p+4,      partition_map.idx);
    putbuf_int32_at(&lbuf->dbuf, lbuf->partitions_p+8,      partition_map.offset);
    putbuf_int32_at(&lbuf->dbuf, lbuf->partitions_p+12,     partition_map.size);

    putbuf_int32_at(&lbuf->dbuf, lbuf->n_in_buf_p,          lbuf->n_in_buf);

    //print_bytestring(lbuf->dbuf.buf, lbuf->dbuf.off, 200);

    int n_uncompressed_bytes_at_beginning = (8 // tokuleaf
					     +4 // layout version
					     +4 // layout version original
					     );
    int uncompressed_len = lbuf->dbuf.off - n_uncompressed_bytes_at_beginning;

    // choose sub block size and number
    int sub_block_size, n_sub_blocks;
    choose_sub_block_size(uncompressed_len, max_sub_blocks, &sub_block_size, &n_sub_blocks);

    int header_len = n_uncompressed_bytes_at_beginning + sub_block_header_size(n_sub_blocks) + sizeof (uint32_t);

    // initialize the sub blocks
    // struct sub_block sub_block[n_sub_blocks]; RFP cilk++ dynamic array bug, use malloc instead
    struct sub_block *MALLOC_N(n_sub_blocks, sub_block);
    for (int i = 0; i < n_sub_blocks; i++) 
        sub_block_init(&sub_block[i]);
    set_all_sub_block_sizes(uncompressed_len, sub_block_size, n_sub_blocks, sub_block);

    // allocate space for the compressed bufer
    int bound = get_sum_compressed_size_bound(n_sub_blocks, sub_block);
    unsigned char *MALLOC_N(header_len + bound, compressed_buf);

    // compress and checksum the sub blocks
    int compressed_len = compress_all_sub_blocks(n_sub_blocks, sub_block, 
                                                 (char *) (lbuf->dbuf.buf + n_uncompressed_bytes_at_beginning),
                                                 (char *) (compressed_buf + header_len), 1);

    // cppy the uncompressed header to the compressed buffer
    memcpy(compressed_buf, lbuf->dbuf.buf, n_uncompressed_bytes_at_beginning);

    // serialize the sub block header
    memcpy(compressed_buf+16, &n_sub_blocks, 4);
    for (int i = 0; i < n_sub_blocks; i++) {
        memcpy(compressed_buf+20+12*i+0, &sub_block[i].compressed_size, 4);
        memcpy(compressed_buf+20+12*i+4, &sub_block[i].uncompressed_size, 4);
        memcpy(compressed_buf+20+12*i+8, &sub_block[i].xsum, 4);
    }

    // compute the header checksum and serialize it
    u_int32_t header_xsum = x1764_memory(compressed_buf, header_len - sizeof (u_int32_t));
    memcpy(compressed_buf + header_len - sizeof (u_int32_t), &header_xsum, 4);

    dbout_lock(out);
    long long off_of_leaf = out->current_off;
    int size = header_len + compressed_len;
    if (0) {
	fprintf(stderr, "uncompressed buf size=%d (amount of data compressed)\n", uncompressed_len);
	fprintf(stderr, "compressed buf size=%u, off=%lld\n", compressed_len, off_of_leaf);
	fprintf(stderr, "compressed bytes are:");
	//for (int i=0; i<compressed_len; i++) {
	//    unsigned char c = compressed_buf[28+i];
	//    if (isprint(c)) fprintf(stderr, "%c", c);
	//    else fprintf(stderr, "\\%03o", compressed_buf[28+i]);
	//}
	fprintf(stderr, "\ntotal bytes written = %d, last byte is \\%o\n", size, compressed_buf[size-1]);
    }
    write_literal(out, compressed_buf, size);
    //printf("translation[%lld].off = %lld\n", lbuf->blocknum, off_of_leaf);
    out->translation[lbuf->blocknum].off  = off_of_leaf;
    out->translation[lbuf->blocknum].size = size;
    seek_align_locked(out);
    dbout_unlock(out);

    toku_free(sub_block); // RFP cilk++ bug

    toku_free(compressed_buf);
    dbuf_destroy(&lbuf->dbuf);
    toku_free(lbuf);

    //printf("Nodewrite %d (%.1f%%):", progress_allocation, 100.0*progress_allocation/PROGRESS_MAX);
    int r = update_progress(progress_allocation, bl, "wrote node");
    if (r!=0) bl->user_said_stop = r;
}

CILK_END

static int write_translation_table (struct dbout *out, long long *off_of_translation_p) {
    seek_align(out);
    struct dbuf ttable;
    dbuf_init(&ttable);
    long long off_of_translation = out->current_off;
    long long bt_size_on_disk = out->n_translations * 16 + 20;
    putbuf_int64(&ttable, out->n_translations);    // number of records
    putbuf_int64(&ttable, -1LL); // the linked list
    out->translation[1].off = off_of_translation;
    out->translation[1].size = bt_size_on_disk;
    for (int i=0; i<out->n_translations; i++) {
	putbuf_int64(&ttable, out->translation[i].off);
	putbuf_int64(&ttable, out->translation[i].size);
    }
    unsigned int checksum = x1764_memory(ttable.buf, ttable.off);
    putbuf_int32(&ttable, checksum);
    assert(bt_size_on_disk==ttable.off);
    toku_os_full_pwrite(out->fd, ttable.buf, ttable.off, off_of_translation); /* use a bare pwrite and check error codes. ??? */
    dbuf_destroy(&ttable);
    *off_of_translation_p = off_of_translation;
    return 0;
}


static void write_header (struct dbout *out, long long translation_location_on_disk, long long translation_size_on_disk, BLOCKNUM root_blocknum_on_disk, LSN load_lsn) {
    struct brt_header h; memset(&h, 0, sizeof h);
    h.layout_version   = BRT_LAYOUT_VERSION;
    h.checkpoint_count = 1;
    h.checkpoint_lsn   = load_lsn; // (max_uint_long means that this doesn't need any kind of recovery
    h.nodesize         = nodesize;
    h.root             = root_blocknum_on_disk;
    h.flags            = 0;
    h.layout_version_original = BRT_LAYOUT_VERSION;

    unsigned int size = toku_serialize_brt_header_size (&h);
    struct wbuf wbuf;
    char *MALLOC_N(size, buf);
    wbuf_init(&wbuf, buf, size);
    toku_serialize_brt_header_to_wbuf(&wbuf, &h, translation_location_on_disk, translation_size_on_disk);
    assert(wbuf.ndone==size);
    toku_os_full_pwrite(out->fd, wbuf.buf, wbuf.ndone, 0); // ??? use the version that returns error codes?
    toku_free(buf);
}

static int read_some_pivots (FIDX pivots_file, int n_to_read, BRTLOADER bl,
		      /*out*/ DBT pivots[/*n_to_read*/])
// pivots is an array to be filled in.  The pivots array is uninitialized.
{
    for (int i=0; i<n_to_read; i++) {
        memset(&pivots[i], 0, sizeof pivots[i]);
	int r = bl_read_dbt(&pivots[i], pivots_file, bl);
	if (r!=0) return r;
    };
    return 0;
}

static int setup_nonleaf_block (int n_children,
				struct subtrees_info *subtrees,         FIDX pivots_file,        int64_t first_child_offset_in_subtrees,
				struct subtrees_info *next_subtrees,    FIDX next_pivots_file,
				struct dbout *out, BRTLOADER bl,
				/*out*/int64_t *blocknum,
				/*out*/struct subtree_info **subtrees_info_p,
				/*out*/DBT **pivots_p)
// Do the serial part of setting up a non leaf block.
//   Read the pivots out of the file, and store them in a newly allocated array of DBTs (returned in *pivots_p)  There are (n_blocks_to_use-1) of these.
//   Copy the final pivot into the next_pivots file instead of returning it.
//   Copy the subtree_info from the subtrees structure, and store them in a newly allocated array of subtree_infos (return in *subtrees_info_p).  There are n_blocks_to_use of these.
//   Allocate a block number and return it in *blocknum.
//   Store the blocknum in the next_blocks structure, so it can be combined with the pivots at the next level of the tree.
//   Update n_blocks_used and n_translations.
// This code cannot be called in parallel because of all the race conditions.
// The actual creation of the node can be called in parallel after this work is done.
{
    //printf("Nonleaf has children :"); for(int i=0; i<n_children; i++) printf(" %ld", subtrees->subtrees[i].block); printf("\n");

    DBT *MALLOC_N(n_children, pivots);
    int r = read_some_pivots(pivots_file, n_children, bl, pivots);
    assert(r==0);

    if ((r=bl_write_dbt(&pivots[n_children-1], next_pivots_file, NULL, bl))) return r;
    // The last pivot was written to the next_pivots file, so we free it now instead of returning it.
    toku_free(pivots[n_children-1].data);
    pivots[n_children-1] = zero_dbt;

    struct subtree_estimates new_subtree_estimates; 
    memset(&new_subtree_estimates, 0, sizeof new_subtree_estimates);
    new_subtree_estimates.exact = TRUE;

    struct subtree_info *MALLOC_N(n_children, subtrees_array);
    int32_t fingerprint = 0;
    for (int i=0; i<n_children; i++) {
	int64_t from_blocknum = first_child_offset_in_subtrees + i;
	subtrees_array[i] = subtrees->subtrees[from_blocknum];
	add_estimates(&new_subtree_estimates, &subtrees->subtrees[from_blocknum].subtree_estimates);
	fingerprint += subtrees->subtrees[from_blocknum].fingerprint;
    }

    *blocknum = allocate_block(out);
    allocate_node(next_subtrees, *blocknum, new_subtree_estimates, fingerprint);

    *pivots_p = pivots;
    *subtrees_info_p = subtrees_array;
    return 0;
}

CILK_BEGIN

static void write_nonleaf_node (struct dbout *out, int64_t blocknum_of_new_node, int n_children,
                                DBT *pivots, /* must free this array, as well as the things it points t */
                                struct subtree_info *subtree_info, int height, const struct descriptor *desc)
{
    assert(height>0);
    BRTNODE XMALLOC(node);
    node->desc  =(struct descriptor *)desc;
    node->nodesize = nodesize;
    node->thisnodename = make_blocknum(blocknum_of_new_node);
    node->layout_version = BRT_LAYOUT_VERSION;
    node->layout_version_original = BRT_LAYOUT_VERSION;
    node->height=height;
    node->u.n.n_children = n_children;
    node->flags = 0;
    node->local_fingerprint = 0;
    node->rand4fingerprint = loader_random();
    XMALLOC_N(n_children-1, node->u.n.childkeys);
    unsigned int totalchildkeylens = 0;
    for (int i=0; i<n_children-1; i++) {
	struct kv_pair *childkey = kv_pair_malloc(pivots[i].data, pivots[i].size, NULL, 0);
	assert(childkey);
	node->u.n.childkeys[i] = childkey;
	totalchildkeylens += kv_pair_keylen(childkey);
    }
    node->u.n.n_bytes_in_buffers = 0;
    node->u.n.totalchildkeylens = totalchildkeylens;
    XMALLOC_N(n_children, node->u.n.childinfos);
    for (int i=0; i<n_children; i++) {
	struct brtnode_nonleaf_childinfo *ci = &node->u.n.childinfos[i];
	ci->subtree_fingerprint = subtree_info[i].fingerprint;
	ci->subtree_estimates   = subtree_info[i].subtree_estimates;
	ci->blocknum            = make_blocknum(subtree_info[i].block);
	ci->have_fullhash       = FALSE;
	ci->fullhash            = 0;
	int r = toku_fifo_create(&ci->buffer);
	assert(r==0);
	ci->n_bytes_in_buffer = 0;
    }

    size_t n_bytes;
    char *bytes;
    int r = toku_serialize_brtnode_to_memory(node, 1, 1, &n_bytes, &bytes);
    assert(r==0);

    dbout_lock(out);
    out->translation[blocknum_of_new_node].off = out->current_off;
    out->translation[blocknum_of_new_node].size = n_bytes;
    //fprintf(stderr, "Wrote internal node at %ld (%ld bytes)\n", out->current_off, n_bytes);
    //for (uint32_t i=0; i<n_bytes; i++) { unsigned char b = bytes[i]; printf("%d:%02x (%d) ('%c')\n", i, b, b, (b>=' ' && b<128) ? b : '*'); }
    write_literal(out, bytes, n_bytes);
    seek_align_locked(out);
    dbout_unlock(out);

    toku_free(bytes);
    for (int i=0; i<n_children-1; i++) {
	toku_free(pivots[i].data);
	toku_free(node->u.n.childkeys[i]);
    }
    for (int i=0; i<n_children; i++) {
	toku_fifo_free(&node->u.n.childinfos[i].buffer);
    }
    toku_free(pivots);
    toku_free(node->u.n.childinfos);
    toku_free(node->u.n.childkeys);
    toku_free(node);
    toku_free(subtree_info);

    blocknum_of_new_node = blocknum_of_new_node;
}

static int write_nonleaves (BRTLOADER bl, FIDX pivots_fidx, struct dbout *out, struct subtrees_info *sts, const struct descriptor *descriptor) {

    int height=1;
    // Watch out for the case where we saved the last pivot but didn't write any more nodes out.
    // The trick is not to look at n_pivots, but to look at blocks.n_blocks
    while (sts->n_subtrees > 1) {
	// If there is more than one block in blocks, then we must build another level of the tree.

	// we need to create a pivots file for the pivots of the next level.
	// and a blocks_array
	// So for example.
	//  1) we grab 16 pivots and 16 blocks.
	//  2) We put the 15 pivots and 16 blocks into an non-leaf node.
	//  3) We put the 16th pivot into the next pivots file.
	{
	    int r = fseek(bl_fidx2file(bl, pivots_fidx), 0, SEEK_SET);
	    if (r!=0) { assert(errno!=0); return errno; }
	}

	FIDX next_pivots_file;
	brtloader_open_temp_file (bl, &next_pivots_file);

	struct subtrees_info next_sts; memset(&next_sts, 0, sizeof next_sts);
        next_sts.n_subtrees = 0;
        next_sts.n_subtrees_limit = 1;

	XMALLOC_N(next_sts.n_subtrees_limit, next_sts.subtrees);

	const int n_per_block = 16;
	int64_t n_subtrees_used = 0;
	while (sts->n_subtrees - n_subtrees_used >= n_per_block*2) {
	    // grab the first N_PER_BLOCK and build a node.
	    DBT *pivots;
	    int64_t blocknum_of_new_node;
	    struct subtree_info *subtree_info;
	    int r = setup_nonleaf_block (n_per_block,
					 sts, pivots_fidx, n_subtrees_used,
					 &next_sts, next_pivots_file,
					 out, bl,
					 &blocknum_of_new_node, &subtree_info, &pivots);
	    assert(r==0);
	    cilk_spawn write_nonleaf_node(out, blocknum_of_new_node, n_per_block, pivots, subtree_info, height, descriptor); // frees all the data structures that go into making the node.
	    n_subtrees_used += n_per_block;
	}
	// Now we have a one or two blocks at the end to handle.
	int64_t n_blocks_left = sts->n_subtrees - n_subtrees_used;
	assert(n_blocks_left>=2);
	if (n_blocks_left > n_per_block) {
	    // Write half the remaining blocks
	    int64_t n_first = n_blocks_left/2;
	    DBT *pivots;
	    int64_t blocknum_of_new_node;
	    struct subtree_info *subtree_info;
	    int r = setup_nonleaf_block(n_first,
					sts, pivots_fidx, n_subtrees_used,
					&next_sts, next_pivots_file,
					out, bl,
					&blocknum_of_new_node, &subtree_info, &pivots);
	    assert(r==0);
	    cilk_spawn write_nonleaf_node(out, blocknum_of_new_node, n_first, pivots, subtree_info, height, descriptor);
	    n_blocks_left -= n_first;
	    n_subtrees_used += n_first;
	}
	{
	    // Write the last block. 
	    DBT *pivots;
	    int64_t blocknum_of_new_node;
	    struct subtree_info *subtree_info;
	    int r = setup_nonleaf_block(n_blocks_left,
					sts, pivots_fidx, n_subtrees_used,
					&next_sts, next_pivots_file,
					out, bl,
					&blocknum_of_new_node, &subtree_info, &pivots);
	    assert(r==0);
	    cilk_spawn write_nonleaf_node(out, blocknum_of_new_node, n_blocks_left, pivots, subtree_info, height, descriptor);
	    n_subtrees_used += n_blocks_left;
	}
	assert(n_subtrees_used == sts->n_subtrees);

        cilk_sync;

	// Now set things up for the next iteration.
	int r = brtloader_fi_close(&bl->file_infos, pivots_fidx); assert(r==0);
	r = brtloader_fi_unlink(&bl->file_infos, pivots_fidx);    assert(r==0);
	pivots_fidx = next_pivots_file;
	toku_free(sts->subtrees);
	*sts = next_sts;
	height++;
    }
    { int r = brtloader_fi_close (&bl->file_infos, pivots_fidx); assert(r==0); }
    { int r = brtloader_fi_unlink(&bl->file_infos, pivots_fidx); assert(r==0); }
    return 0;
}

CILK_END

#if 0
// C function for testing write_file_to_dbfile
int brt_loader_write_file_to_dbfile (int outfile, FIDX infile, BRTLOADER bl, const struct descriptor *descriptor, int progress_allocation) {
#if defined(__cilkplusplus)
    return cilk::run(write_file_to_dbfile, outfile, infile, bl, descriptor, progress_allocation);
#else
    return           write_file_to_dbfile (outfile, infile, bl, descriptor, progress_allocation);
#endif
}
#endif

#if defined(__cplusplus) || defined(__cilkplusplus)
}
#endif
