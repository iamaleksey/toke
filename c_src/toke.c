/* ------------------------------------------------------------------------- */
/*                                                                           */
/*   The contents of this file are subject to the Mozilla Public License     */
/*   Version 1.1 (the "License"); you may not use this file except in        */
/*   compliance with the License. You may obtain a copy of the License at    */
/*   http://www.mozilla.org/MPL/                                             */
/*                                                                           */
/*   Software distributed under the License is distributed on an "AS IS"     */
/*   basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the */
/*   License for the specific language governing rights and limitations      */
/*   under the License.                                                      */
/*                                                                           */
/*   The Original Code is Toke.                                              */
/*                                                                           */
/*   The Initial Developers of the Original Code are LShift Ltd.             */
/*                                                                           */
/*   Portions created by LShift Ltd are Copyright (C) 2009 LShift Ltd.       */
/*                                                                           */
/*   All Rights Reserved.                                                    */
/*                                                                           */
/*   Contributor(s): ______________________________________.                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

#include <erl_driver.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <tcutil.h>
#include <tchdb.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "toke.h"

#define FALSE                  0
#define TRUE                   1

#define OK                     1
#define TOKYO_ERROR           -1
#define READER_ERROR          -2

#define ATOM_SPEC_LEN          6
#define GET_RESULT_SPEC_LEN    7
#define READER_ERROR_SPEC_LEN  11
#define TOKYO_ERROR_SPEC_LEN   11

typedef struct {
  ErlDrvPort port;
  TCHDB *hdb;
  ErlDrvTermData* get_result_spec;   /* store these in here so that */
  ErlDrvTermData* reader_error_spec; /* they're not shared between  */
  ErlDrvTermData* tokyo_error_spec;  /* threads                     */
} TokeData;

typedef struct {
  ErlIOVec *ev;
  size_t row;
  size_t column;
  ReaderError last_error;
} Reader;

ErlDrvTermData* no_command_atom_spec      = NULL;
ErlDrvTermData* invalid_command_atom_spec = NULL;
ErlDrvTermData* no_such_command_atom_spec = NULL;
ErlDrvTermData* ok_atom_spec              = NULL;
ErlDrvTermData* invalid_state_atom_spec   = NULL;
ErlDrvTermData* not_found_atom_spec       = NULL;

uint8_t toke_invalid_command = TOKE_INVALID_COMMAND;

/* only used in debugging */
void dump_ev(ErlIOVec *ev) {
  printf("total size: %d\r\nvec len: %d\r\n", ev->size, ev->vsize);
  int idx;
  for (idx = 0; idx < ev->vsize; ++idx) {
    printf("iov[%d] = ", idx);
    SysIOVec iov = ev->iov[idx];
    printf("[base = %p, len = %zd]\r\n", iov.iov_base, iov.iov_len);
    printf("binv[%d] = ", idx);
    if (NULL == ev->binv[idx]) {
      printf("NULL\r\n");
    } else {
      ErlDrvBinary* bin = ev->binv[idx];
      printf("[orig_bytes = %p; orig_size = %ld]\r\n",
             bin->orig_bytes, bin->orig_size);
    }
  }
  printf("done\r\n");
}

void make_reader(ErlIOVec *ev, Reader *reader) {
  reader->ev = ev;
  reader->row = 1; /* row 0 is reserved for headers */
  reader->column = 0;
  reader->last_error = READER_NO_ERROR;
}

int read_simple_thing(Reader* reader, char **result, size_t size) {
  size_t row = reader->row;
  size_t column = reader->column;
  long data_left_in_current_row = (reader->ev->binv[row]->orig_size) - column;
  if (data_left_in_current_row == 0) {
    ++row;
    if (row == reader->ev->vsize) {
      reader->last_error = READER_READ_ALL_DATA;
      return FALSE; /* run out of data */
    } else {
      reader->row = row;
      reader->column = 0;
      return read_simple_thing(reader, result, size);
    }
  } else if (data_left_in_current_row < size) {
    reader->last_error = READER_PACKING_ERROR;
    return FALSE; /* packing error! */
  } else {
    *result = (reader->ev->binv[row]->orig_bytes) + column;
    column += size;
    reader->column = column;
    return TRUE;
  }
}

int read_uint8(Reader* reader, uint8_t **result) {
  return read_simple_thing(reader, (char **)result, sizeof(uint8_t));
}

int read_int8(Reader* reader, int8_t **result) {
  return read_simple_thing(reader, (char **)result, sizeof(int8_t));
}

int read_int32(Reader* reader, int32_t **result) {
  return read_simple_thing(reader, (char **)result, sizeof(int32_t));
}

int read_uint64(Reader* reader, uint64_t **result) {
  return read_simple_thing(reader, (char **)result, sizeof(uint64_t));
}

int read_int64(Reader* reader, int64_t **result) {
  return read_simple_thing(reader, (char **)result, sizeof(int64_t));
}

int read_binary(Reader* reader, char **result, uint64_t **binlen) {
  if (read_simple_thing(reader, (char **)binlen, sizeof(uint64_t))) {
    return read_simple_thing(reader, result, **binlen);
  } else {
    return 1;
  }
}

void return_reader_error(TokeData *td, ErlDrvPort port, Reader *reader) {
  const char* error_str;
  if (NULL == reader) {
    error_str = "Null reader";
  } else {
    switch (reader->last_error) {
    case READER_NO_ERROR:
      error_str = "No error";
      break;
    case READER_READ_ALL_DATA:
      error_str = "Exhausted all supplied data";
      break;
    case READER_PACKING_ERROR:
      error_str = "Packing error";
      break;
    default:
      error_str = "Unknown error";
    }
  }
  td->reader_error_spec[5] = (ErlDrvTermData)error_str;
  td->reader_error_spec[6] = (ErlDrvUInt)strlen(error_str);
  driver_output_term(port, td->reader_error_spec, READER_ERROR_SPEC_LEN);
}

void return_tokyo_error(TokeData *td, ErlDrvPort port, TCHDB *hdb) {
  if (NULL == hdb) {
    driver_output_term(port, invalid_state_atom_spec, ATOM_SPEC_LEN);
  } else {
    int ecode = tchdbecode(hdb);
    const char* error_str = tchdberrmsg(ecode);
    td->tokyo_error_spec[5] = (ErlDrvTermData)error_str;
    td->tokyo_error_spec[6] = (ErlDrvUInt)strlen(error_str),
    driver_output_term(port, td->tokyo_error_spec, TOKYO_ERROR_SPEC_LEN);
  }
}

static int toke_init()
{
  no_command_atom_spec =
    (ErlDrvTermData*)driver_alloc(ATOM_SPEC_LEN * sizeof(ErlDrvTermData));
  no_command_atom_spec[0] = ERL_DRV_ATOM;
  no_command_atom_spec[1] = driver_mk_atom("toke_reply");
  no_command_atom_spec[2] = ERL_DRV_ATOM;
  no_command_atom_spec[3] = driver_mk_atom("no_command");
  no_command_atom_spec[4] = ERL_DRV_TUPLE;
  no_command_atom_spec[5] = 2;

  invalid_command_atom_spec =
    (ErlDrvTermData*)driver_alloc(ATOM_SPEC_LEN * sizeof(ErlDrvTermData));
  invalid_command_atom_spec[0] = ERL_DRV_ATOM;
  invalid_command_atom_spec[1] = driver_mk_atom("toke_reply");
  invalid_command_atom_spec[2] = ERL_DRV_ATOM;
  invalid_command_atom_spec[3] = driver_mk_atom("invalid_command");
  invalid_command_atom_spec[4] = ERL_DRV_TUPLE;
  invalid_command_atom_spec[5] = 2;

  no_such_command_atom_spec =
    (ErlDrvTermData*)driver_alloc(ATOM_SPEC_LEN * sizeof(ErlDrvTermData));
  no_such_command_atom_spec[0] = ERL_DRV_ATOM;
  no_such_command_atom_spec[1] = driver_mk_atom("toke_reply");
  no_such_command_atom_spec[2] = ERL_DRV_ATOM;
  no_such_command_atom_spec[3] = driver_mk_atom("no_such_command");
  no_such_command_atom_spec[4] = ERL_DRV_TUPLE;
  no_such_command_atom_spec[5] = 2;

  ok_atom_spec =
    (ErlDrvTermData*)driver_alloc(ATOM_SPEC_LEN * sizeof(ErlDrvTermData));
  ok_atom_spec[0] = ERL_DRV_ATOM;
  ok_atom_spec[1] = driver_mk_atom("toke_reply");
  ok_atom_spec[2] = ERL_DRV_ATOM;
  ok_atom_spec[3] = driver_mk_atom("ok");
  ok_atom_spec[4] = ERL_DRV_TUPLE;
  ok_atom_spec[5] = 2;

  invalid_state_atom_spec =
    (ErlDrvTermData*)driver_alloc(ATOM_SPEC_LEN * sizeof(ErlDrvTermData));
  invalid_state_atom_spec[0] = ERL_DRV_ATOM;
  invalid_state_atom_spec[1] = driver_mk_atom("toke_reply");
  invalid_state_atom_spec[2] = ERL_DRV_ATOM;
  invalid_state_atom_spec[3] = driver_mk_atom("invalid_state");
  invalid_state_atom_spec[4] = ERL_DRV_TUPLE;
  invalid_state_atom_spec[5] = 2;

  not_found_atom_spec =
    (ErlDrvTermData*)driver_alloc(ATOM_SPEC_LEN * sizeof(ErlDrvTermData));
  not_found_atom_spec[0] = ERL_DRV_ATOM;
  not_found_atom_spec[1] = driver_mk_atom("toke_reply");
  not_found_atom_spec[2] = ERL_DRV_ATOM;
  not_found_atom_spec[3] = driver_mk_atom("not_found");
  not_found_atom_spec[4] = ERL_DRV_TUPLE;
  not_found_atom_spec[5] = 2;

  return 0;
}

static ErlDrvData toke_start(ErlDrvPort port, char *buff)
{
  TokeData* td = (TokeData*)driver_alloc(sizeof(TokeData));
  td->port = port;
  td->hdb = NULL;

  td->get_result_spec =
    (ErlDrvTermData*)driver_alloc(GET_RESULT_SPEC_LEN * sizeof(ErlDrvTermData));
  td->get_result_spec[0] = ERL_DRV_ATOM;
  td->get_result_spec[1] = driver_mk_atom("toke_reply");
  td->get_result_spec[2] = ERL_DRV_BUF2BINARY;
  td->get_result_spec[3] = (ErlDrvTermData)NULL;
  td->get_result_spec[4] = 0;
  td->get_result_spec[5] = ERL_DRV_TUPLE;
  td->get_result_spec[6] = 2;

  td->reader_error_spec =
    (ErlDrvTermData*)driver_alloc(READER_ERROR_SPEC_LEN * sizeof(ErlDrvTermData));
  td->reader_error_spec[0] = ERL_DRV_ATOM;
  td->reader_error_spec[1] = driver_mk_atom("toke_reply");
  td->reader_error_spec[2] = ERL_DRV_ATOM;
  td->reader_error_spec[3] = driver_mk_atom("reader_error");
  td->reader_error_spec[4] = ERL_DRV_STRING;
  td->reader_error_spec[5] = (ErlDrvTermData)NULL;
  td->reader_error_spec[6] = 0;
  td->reader_error_spec[7] = ERL_DRV_TUPLE;
  td->reader_error_spec[8] = 2;
  td->reader_error_spec[9] = ERL_DRV_TUPLE;
  td->reader_error_spec[10] = 2;

  td->tokyo_error_spec =
    (ErlDrvTermData*)driver_alloc(TOKYO_ERROR_SPEC_LEN * sizeof(ErlDrvTermData));
  td->tokyo_error_spec[0] = ERL_DRV_ATOM;
  td->tokyo_error_spec[1] = driver_mk_atom("toke_reply");
  td->tokyo_error_spec[2] = ERL_DRV_ATOM;
  td->tokyo_error_spec[3] = driver_mk_atom("error_from_tokyo_cabinet");
  td->tokyo_error_spec[4] = ERL_DRV_STRING;
  td->tokyo_error_spec[5] = (ErlDrvTermData)NULL;
  td->tokyo_error_spec[6] = 0;
  td->tokyo_error_spec[7] = ERL_DRV_TUPLE;
  td->tokyo_error_spec[8] = 2;
  td->tokyo_error_spec[9] = ERL_DRV_TUPLE;
  td->tokyo_error_spec[10] = 2;

  return (ErlDrvData)td;
}

static void toke_stop(ErlDrvData drv_data)
{
  TokeData* td = (TokeData*)drv_data;
  if (NULL != td->hdb) {
    tchdbclose(td->hdb);
    driver_free((char*)td->get_result_spec);
    driver_free((char*)td->reader_error_spec);
    driver_free((char*)td->tokyo_error_spec);
  }
  driver_free((char*)drv_data);

  driver_free((char*)no_command_atom_spec);
  driver_free((char*)invalid_command_atom_spec);
  driver_free((char*)no_such_command_atom_spec);
  driver_free((char*)ok_atom_spec);
  driver_free((char*)invalid_state_atom_spec);
  driver_free((char*)not_found_atom_spec);
}

void toke_new(TokeData *td, ErlDrvTermData **spec, Reader *reader,
              ErlDrvPort port) {
  if (NULL == td->hdb) {
    td->hdb = tchdbnew();
    *spec = ok_atom_spec;
  } else {
    *spec = invalid_state_atom_spec;
  }
}

void toke_del(TokeData *td, ErlDrvTermData **spec, Reader *reader,
              ErlDrvPort port) {
  if (NULL != td->hdb) {
    tchdbdel(td->hdb);
    td->hdb = NULL;
  }
  *spec = ok_atom_spec;
}

void toke_with_hdb(TokeData *td, ErlDrvTermData **spec,
                   Reader *reader, ErlDrvPort port,
                   int (*func)(TokeData *td, Reader *reader, ErlDrvPort port)) {
  if (NULL == td->hdb) {
    *spec = invalid_state_atom_spec;
  } else {
    switch (func(td, reader, port)) {
    case OK:
      *spec = ok_atom_spec;
      break;
    case TOKYO_ERROR:
      return_tokyo_error(td, port, td->hdb);
      break;
    case READER_ERROR:
      return_reader_error(td, port, reader);
      break;
    }
  }
}

int toke_tune(TokeData *td, Reader *reader, ErlDrvPort port) {
  int64_t *bnum = NULL;
  int8_t *apow = NULL;
  int8_t *fpow = NULL;
  uint8_t *opts = NULL;
  if (read_int64(reader, &bnum) && read_int8(reader, &apow) &&
      read_int8(reader, &fpow) && read_uint8(reader, &opts)) {

    int tkopts = 0;
    if (*opts & TOKE_TUNE_LARGE)
      tkopts |= HDBTLARGE;
    if (*opts & TOKE_TUNE_DEFLATE)
      tkopts |= HDBTDEFLATE;
    if (*opts & TOKE_TUNE_BZIP)
      tkopts |= HDBTBZIP;
    if (*opts & TOKE_TUNE_TCBS)
      tkopts |= HDBTTCBS;
    if (*opts & TOKE_TUNE_EXCODEC)
      tkopts |= HDBTEXCODEC;

    return (tchdbtune(td->hdb, *bnum, *apow, *fpow, tkopts)) ? OK : TOKYO_ERROR;
  } else {
    return READER_ERROR;
  }
}

int toke_set_cache(TokeData *td, Reader *reader, ErlDrvPort port) {
  int32_t *rcnum = NULL;
  return (read_int32(reader, &rcnum)) ?
    ((tchdbsetcache(td->hdb, *rcnum)) ? OK : TOKYO_ERROR) : READER_ERROR;
}

int toke_set_xm_size(TokeData *td, Reader *reader, ErlDrvPort port) {
  int64_t *xmsize = NULL;
  return (read_int64(reader, &xmsize)) ?
    ((tchdbsetxmsiz(td->hdb, *xmsize)) ? OK : TOKYO_ERROR) : READER_ERROR;
}

int toke_set_df_unit(TokeData *td, Reader *reader, ErlDrvPort port) {
  int32_t *dfunit = NULL;
  return (read_int32(reader, &dfunit)) ?
    ((tchdbsetdfunit(td->hdb, *dfunit)) ? OK : TOKYO_ERROR) : READER_ERROR;
}

int toke_open(TokeData *td, Reader *reader, ErlDrvPort port) {
  char *path = NULL;
  uint64_t *path_len = NULL;
  uint8_t *mode = NULL;
  if (read_binary(reader, &path, &path_len) && read_uint8(reader, &mode)) {
    /* strings coming from Erlang are not zero terminated, and the
       length won't include the 0 stop byte, so copy into a new array,
       ensuring we have a stop byte at the end. */
    char path2[(*path_len)+1];
    path2[(*path_len)] = '\0';
    strncpy(path2, path, *path_len);

    int tkmode = 0;
    if (*mode & TOKE_OPEN_READER)
      tkmode |= HDBOREADER;
    if (*mode & TOKE_OPEN_WRITER)
      tkmode |= HDBOWRITER;
    if (*mode & TOKE_OPEN_CREAT)
      tkmode |= HDBOCREAT;
    if (*mode & TOKE_OPEN_TRUNC)
      tkmode |= HDBOTRUNC;
    if (*mode & TOKE_OPEN_NOLCK)
      tkmode |= HDBONOLCK;
    if (*mode & TOKE_OPEN_LCKNB)
      tkmode |= HDBOLCKNB;
    if (*mode & TOKE_OPEN_TSYNC)
      tkmode |= HDBOTSYNC;

    return (tchdbopen(td->hdb, path2, tkmode)) ? OK : TOKYO_ERROR;
  } else {
    return READER_ERROR;
  }
}

int toke_close(TokeData *td, Reader *reader, ErlDrvPort port) {
  return tchdbclose(td->hdb) ? OK : TOKYO_ERROR;
}

int toke_do_insert(TokeData *td, Reader *reader, ErlDrvPort port,
                   bool (*func)(TCHDB *hdb, const void *kbuf, int ksiz,
                                const void *vbuf, int vsiz)) {
  uint64_t *keysize = NULL;
  char *key = NULL;
  uint64_t *valuesize = NULL;
  char *value = NULL;
  return (read_binary(reader, &key, &keysize) &&
          read_binary(reader, &value, &valuesize)) ?
    ((func(td->hdb, key, *keysize, value, *valuesize)) ? OK : TOKYO_ERROR) :
    READER_ERROR;
}

int toke_insert(TokeData *td, Reader *reader, ErlDrvPort port) {
  return toke_do_insert(td, reader, port, tchdbput);
}

int toke_insert_new(TokeData *td, Reader *reader, ErlDrvPort port) {
  return toke_do_insert(td, reader, port, tchdbputkeep);
}

int toke_insert_concat(TokeData *td, Reader *reader, ErlDrvPort port) {
  return toke_do_insert(td, reader, port, tchdbputcat);
}

int toke_insert_async(TokeData *td, Reader *reader, ErlDrvPort port) {
  toke_do_insert(td, reader, port, tchdbputasync);
  return OK; /* throw away any errors, because we're async throughout */
}

int toke_delete(TokeData *td, Reader *reader, ErlDrvPort port) {
  uint64_t *keysize = NULL;
  char *key = NULL;
  return (read_binary(reader, &key, &keysize)) ?
    ((tchdbout(td->hdb, key, *keysize)) ? OK : TOKYO_ERROR) : READER_ERROR;
}

void toke_get(TokeData *td, ErlDrvTermData **spec, Reader *reader,
             ErlDrvPort port) {
  if (NULL == td->hdb) {
    *spec = invalid_state_atom_spec;
  } else {
    uint64_t *keysize = NULL;
    char *key = NULL;
    int valuesize = 0;
    if (read_binary(reader, &key, &keysize)) {
      char *value = tchdbget(td->hdb, key, *keysize, &valuesize);
      if (NULL == value) {
        *spec = not_found_atom_spec;
      } else {
        td->get_result_spec[3] = (ErlDrvTermData)value;
        td->get_result_spec[4] = valuesize;
        driver_output_term(port, td->get_result_spec, GET_RESULT_SPEC_LEN);
        td->get_result_spec[3] = (ErlDrvTermData)NULL;
        td->get_result_spec[4] = 0;
        free(value);
      }
    } else {
      return_reader_error(td, port, reader);
    }
  }
}

int toke_get_all1(TokeData *td, ErlDrvPort port) {
  int valuesize = 0;
  char *value = NULL;
  while (NULL != (value = tchdbiternext(td->hdb, &valuesize))) {
    td->get_result_spec[3] = (ErlDrvTermData)value;
    td->get_result_spec[4] = valuesize;
    driver_output_term(port, td->get_result_spec, GET_RESULT_SPEC_LEN);
    td->get_result_spec[3] = (ErlDrvTermData)NULL;
    td->get_result_spec[4] = 0;
    free(value);
  }
  return OK;
}

int toke_get_all(TokeData *td, Reader *reader, ErlDrvPort port) {
  return (tchdbiterinit(td->hdb)) ? toke_get_all1(td, port) : TOKYO_ERROR;
}

static void toke_outputv(ErlDrvData drv_data, ErlIOVec *ev)
{
  Reader reader;
  ErlDrvTermData* spec = NULL;
  uint8_t* command = &toke_invalid_command;
  TokeData* td = (TokeData*)drv_data;
  ErlDrvPort port = td->port;
  /* dump_ev(ev); */
  make_reader(ev, &reader);
  if (read_uint8(&reader, &command)) {
    switch (*command) {

    case TOKE_NEW:
      toke_new(td, &spec, &reader, port);
      break;

    case TOKE_DEL:
      toke_del(td, &spec, &reader, port);
      break;

    case TOKE_TUNE:
      toke_with_hdb(td, &spec, &reader, port, toke_tune);
      break;

    case TOKE_SET_CACHE:
      toke_with_hdb(td, &spec, &reader, port, toke_set_cache);
      break;

    case TOKE_SET_XM_SIZE:
      toke_with_hdb(td, &spec, &reader, port, toke_set_xm_size);
      break;

    case TOKE_SET_DF_UNIT:
      toke_with_hdb(td, &spec, &reader, port, toke_set_df_unit);
      break;

    case TOKE_OPEN:
      toke_with_hdb(td, &spec, &reader, port, toke_open);
      break;

    case TOKE_CLOSE:
      toke_with_hdb(td, &spec, &reader, port, toke_close);
      break;

    case TOKE_INSERT:
      toke_with_hdb(td, &spec, &reader, port, toke_insert);
      break;

    case TOKE_INSERT_NEW:
      toke_with_hdb(td, &spec, &reader, port, toke_insert_new);
      break;

    case TOKE_INSERT_CONCAT:
      toke_with_hdb(td, &spec, &reader, port, toke_insert_concat);
      break;

    case TOKE_INSERT_ASYNC:
      toke_with_hdb(td, &spec, &reader, port, toke_insert_async);
      return; /* return immediately because it's async */

    case TOKE_DELETE:
      toke_with_hdb(td, &spec, &reader, port, toke_delete);
      break;

    case TOKE_GET:
      toke_get(td, &spec, &reader, port);
      break;

    case TOKE_GET_ALL:
      toke_with_hdb(td, &spec, &reader, port, toke_get_all);
      break;

    default:
      spec = no_such_command_atom_spec;
    }
  } else {
    return_reader_error(td, port, &reader);
  }

  if (NULL != spec) {
    driver_output_term(port, spec, ATOM_SPEC_LEN);
  }
}

static ErlDrvEntry toke_driver_entry =
{
  .init = toke_init,
  .start = toke_start,
  .stop = toke_stop,
  .driver_name = (char*) "libtoke",
  .outputv = toke_outputv,
  .extended_marker = ERL_DRV_EXTENDED_MARKER,
  .major_version = ERL_DRV_EXTENDED_MAJOR_VERSION,
  .minor_version = ERL_DRV_EXTENDED_MINOR_VERSION,
  .driver_flags = ERL_DRV_FLAG_USE_PORT_LOCKING
};

DRIVER_INIT (libtoke);

DRIVER_INIT (libtoke) /* must match name in driver_entry */
{
  return &toke_driver_entry;
}